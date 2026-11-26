/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025 by Sho Tanimoto
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "audio.h"
#include "buffering.h"
#include "core_alloc.h"
#include "dir.h"
#include "file.h"
#include "metadata.h"
#include "misc.h"
#include "pathfuncs.h"
#include "pcm_mixer.h"
#include "pcm_sink.h"
#include "playback.h"
#include "playlist.h"
#include "powermgmt.h"
#include "settings.h"
#include "sound.h"
#include "string-extra.h"
#ifdef HAVE_TAGCACHE
#include "tagcache.h"
#endif
#include "usb.h"
#include "usb_drv.h"
#include <stdio.h>
#include <string.h>

#include "../usb_iap.h"
#include "debug.h"
#include "libiap/iap.h"
#include "macros.h"
#include "platform.h"

/* iAP callbacks are serialized under iap_ctx_mutex on the USB thread; keeping
 * large scratch structs/path buffers out of automatic storage avoids
 * overflowing usb_stack during FAT32 / tagcache operations. */
static struct mp3entry shared_id3;
static bool            has_active_tag_filters(const struct Platform* plt);
static IAPBool         play_current_db_selection(struct IAPContext* iap_ctx, uint32_t track_index);

/* Head units (e.g. Volvo) probe the database right after connecting with an
 * unfiltered SelectDBRecord(Track, 0) without ever listing tracks. Only treat
 * a leaf selection as "user picked this item, play it" when the accessory has
 * actually retrieved records of that type (a count is not enough: the select
 * path itself counts) since the last reset, or narrowed the
 * selection with category filters; otherwise just remember it for a later
 * PlayCurrentSelection, like a real iPod. */
static bool leaf_select_is_user_pick(const struct Platform* plt, uint8_t type) {
    return (type < 16 && (plt->db_listed_mask & (1u << type))) || has_active_tag_filters(plt);
}

#ifdef HAVE_TAGCACHE
static struct tagcache_search        shared_cat_tcs;
static struct tagcache_search_clause shared_cat_kind_clause;
static uint32_t                      shared_cat_uniqbuf[2048];
static char                          shared_cat_buf[256];
static bool                          shared_cat_tcs_open = false;
static int                           shared_cat_tag      = -1;
static int                           shared_cat_kind     = -1;
static int32_t                       shared_cat_artist   = -1;
static int32_t                       shared_cat_album    = -1;
static int32_t                       shared_cat_genre    = -1;
static int32_t                       shared_cat_composer = -1;
static uint32_t                      shared_cat_next_idx = 0;

static void close_shared_cat_tcs(void) {
    if(shared_cat_tcs_open) {
        tagcache_search_finish(&shared_cat_tcs);
        shared_cat_tcs_open = false;
    }
}
#endif

void* iap_platform_malloc(struct IAPContext* iap_ctx, size_t size, int flags) {
    struct Platform* plt = iap_ctx->platform;
    for(size_t i = 0; i < ARRAYLEN(plt->malloc_results); i += 1) {
        if(plt->malloc_results[i].ptr != NULL) {
            continue;
        }
        struct IAPAllocResult result;
        if(flags & IAPPlatformMallocFlags_Uncached) {
            check_act(iap_alloc_usb_send_buffer(size, &result), return NULL);
        } else {
            check_act(iap_alloc_buffer(size, &result), return NULL);
        }
        plt->malloc_results[i] = result;
        return result.ptr;
    }
    ERROR("no free malloc slot");
    return NULL;
}

void iap_platform_free(struct IAPContext* iap_ctx, void* ptr) {
    struct Platform* plt = iap_ctx->platform;
    for(size_t i = 0; i < ARRAYLEN(plt->malloc_results); i += 1) {
        if(plt->malloc_results[i].ptr == ptr) {
            core_free(plt->malloc_results[i].handle);
            plt->malloc_results[i].ptr = NULL;
            return;
        }
    }
    ERROR("no matching malloc slot");
}

int iap_platform_send_hid_report(struct IAPContext* iap_ctx, const void* ptr, size_t size) {
    (void)iap_ctx;
#if DEBUG_DUMP_TX == 1
    logf("==== dev ==== %p %u > %d", ptr, size, HID_EP_IN);
    iap_platform_dump_hex(ptr, MIN(size, 48));
#endif
    const int ret = usb_drv_send_nonblocking(HID_EP_IN, (void*)ptr, size);
    return ret == 0 ? (int)size : ret;
}

IAPBool iap_platform_get_ipod_serial_num(struct IAPContext* iap_ctx, struct IAPSpan* serial) {
    (void)iap_ctx;
#if defined(IPOD_COLOR) && defined(USB_IPOD_IDENTITY) && !defined(BOOTLOADER)
    char serial_buf[17];
    if(usb_storage_ipod_serial(serial_buf)) {
        return iap_span_append(serial, serial_buf, strlen(serial_buf) + 1);
    }
#elif defined(IPOD_VIDEO) && !defined(SIMULATOR)
    const uint32_t* hw_serial = (const uint32_t*)0x20004034;
    if(hw_serial[0] != 0 || hw_serial[1] != 0) {
        char serial_buf[17];
        snprintf(serial_buf, sizeof(serial_buf), "%08lX%08lX", (unsigned long)hw_serial[1], (unsigned long)hw_serial[0]);
        return iap_span_append(serial, serial_buf, strlen(serial_buf) + 1);
    }
#endif
    static const char* serial_num = "000000000000";
    return iap_span_append(serial, serial_num, strlen(serial_num) + 1);
}

IAPBool iap_platform_get_play_status(struct IAPContext* iap_ctx, struct IAPPlatformPlayStatus* status) {
    struct Platform* plt = iap_ctx->platform;

    /* never leak uninitialized stack values (seen as tracks=1046596 when
     * stopped); the playback list size is valid even while stopped */
    memset(status, 0, sizeof(*status));
    const int amount    = playlist_amount();
    status->track_count = amount > 0 ? (uint32_t)amount : 0;
    status->track_index = (uint32_t)-1;
    status->state       = _iap_convert_play_status(audio_status());
    if(status->state == IAPIPodStatePlayStatus_PlaybackStopped) {
        return iap_true;
    }

    const int disp_idx   = playlist_get_display_index() - 1;
    status->track_index  = disp_idx >= 0 ? (uint32_t)disp_idx : 0;
    status->track_caps   = IAPIPodStateTrackCapBits_HasReleaseDate;
    struct mp3entry* id3 = audio_current_track();
    if(id3 != NULL) {
        status->track_total_ms = id3->length;
        status->track_pos_ms   = id3->elapsed;
    }
    if(plt->aa_slot >= 0 && playback_current_aa_hid(plt->aa_slot) >= 0) {
        status->track_caps |= IAPIPodStateTrackCapBits_HasAlbumArts;
    }

    return iap_true;
}

static bool has_playlist_ext(const char* name, size_t* stem_len) {
    const char* dot = strrchr(name, '.');
    if(dot == NULL || dot == name) {
        return false;
    }
    if(strcasecmp(dot, ".m3u8") != 0 && strcasecmp(dot, ".m3u") != 0) {
        return false;
    }
    if(stem_len != NULL) {
        *stem_len = (size_t)(dot - name);
    }
    return true;
}

static const char* get_playlist_catalog_dir(void) {
    return global_settings.playlist_catalog_dir[0] != '\0' ? (const char*)global_settings.playlist_catalog_dir : PLAYLIST_CATALOG_DEFAULT_DIR;
}

struct playlist_step_state {
    const char* prev_name;
    char*       best_name;
    size_t      best_name_size;
    char*       best_dir;
    size_t      best_dir_size;
    char*       best_file;
    size_t      best_file_size;
    uint32_t    count;
    bool        found;
};

static void consider_playlist_entry(struct playlist_step_state* st, const char* disp_name, const char* dir, const char* file) {
    st->count += 1;
    if(st->prev_name != NULL) {
        int cmp = strcasecmp(disp_name, st->prev_name);
        if(cmp < 0 || (cmp == 0 && strcmp(disp_name, st->prev_name) <= 0)) {
            return;
        }
    }
    if(!st->found) {
        strlcpy(st->best_name, disp_name, st->best_name_size);
        strlcpy(st->best_dir, dir, st->best_dir_size);
        strlcpy(st->best_file, file, st->best_file_size);
        st->found = true;
        return;
    }
    int cmp = strcasecmp(disp_name, st->best_name);
    if(cmp < 0 || (cmp == 0 && strcmp(disp_name, st->best_name) < 0)) {
        strlcpy(st->best_name, disp_name, st->best_name_size);
        strlcpy(st->best_dir, dir, st->best_dir_size);
        strlcpy(st->best_file, file, st->best_file_size);
    }
}

static void scan_playlist_dir_entries(struct playlist_step_state* st, const char* dir_path, const char* prefix) {
    DIR* dir = opendir(dir_path);
    if(dir == NULL) {
        return;
    }
    struct dirent* entry;
    while((entry = readdir(dir)) != NULL) {
        if(is_dotdir_name(entry->d_name) || is_macos_name(entry->d_name)) {
            continue;
        }
        struct dirinfo info = dir_get_info(dir, entry);
        if(info.attribute & ATTR_DIRECTORY) {
            continue;
        }
        size_t stem_len = 0;
        if(!has_playlist_ext(entry->d_name, &stem_len)) {
            continue;
        }
        char disp_name[128];
        if(prefix != NULL && prefix[0] != '\0') {
            snprintf(disp_name, sizeof(disp_name), "%s/%.*s", prefix, (int)stem_len, entry->d_name);
        } else {
            snprintf(disp_name, sizeof(disp_name), "%.*s", (int)stem_len, entry->d_name);
        }
        consider_playlist_entry(st, disp_name, dir_path, entry->d_name);
    }
    closedir(dir);
}

static bool find_playlist_step(const char* root_dir, struct playlist_step_state* st) {
    st->count = 0;
    st->found = false;

    /* Scan top-level playlists first while keeping at most one DIR* open. */
    scan_playlist_dir_entries(st, root_dir, NULL);

    /* Then scan 1-level subfolders (e.g. Finder-synced playlist folders). */
    for(int target_sub = 0;; target_sub++) {
        DIR* root = opendir(root_dir);
        if(root == NULL) {
            break;
        }
        int            sub_idx = 0;
        static char    sub_name[128];
        static char    sub_path[MAX_PATH];
        bool           has_sub = false;
        struct dirent* entry;
        while((entry = readdir(root)) != NULL) {
            if(is_dotdir_name(entry->d_name) || is_macos_name(entry->d_name)) {
                continue;
            }
            struct dirinfo info = dir_get_info(root, entry);
            if(!(info.attribute & ATTR_DIRECTORY)) {
                continue;
            }
            if(sub_idx == target_sub) {
                strlcpy(sub_name, entry->d_name, sizeof(sub_name));
                snprintf(sub_path, sizeof(sub_path), "%s/%s", root_dir, entry->d_name);
                has_sub = true;
                break;
            }
            sub_idx++;
        }
        closedir(root);
        if(!has_sub) {
            break;
        }
        scan_playlist_dir_entries(st, sub_path, sub_name);
    }

    return st->found;
}

static uint32_t count_real_playlists(void) {
    const char*              root = get_playlist_catalog_dir();
    static char              best_name[128];
    static char              best_dir[MAX_PATH];
    static char              best_file[MAX_PATH];
    struct playlist_step_state st = {
        .prev_name      = NULL,
        .best_name      = best_name,
        .best_name_size = sizeof(best_name),
        .best_dir       = best_dir,
        .best_dir_size  = sizeof(best_dir),
        .best_file      = best_file,
        .best_file_size = sizeof(best_file),
    };
    find_playlist_step(root, &st);
    return st.count;
}

static bool find_playlist_by_index(uint32_t index, char* out_name, size_t out_name_size, char* out_dir, size_t out_dir_size, char* out_file,
                                   size_t out_file_size) {
    const char* root = get_playlist_catalog_dir();
    static char prev_name[128];
    static char best_name[128];
    static char best_dir[MAX_PATH];
    static char best_file[MAX_PATH];
    prev_name[0] = '\0';

    for(uint32_t step = 0; step <= index; step++) {
        struct playlist_step_state st = {
            .prev_name      = step == 0 ? NULL : prev_name,
            .best_name      = best_name,
            .best_name_size = sizeof(best_name),
            .best_dir       = best_dir,
            .best_dir_size  = sizeof(best_dir),
            .best_file      = best_file,
            .best_file_size = sizeof(best_file),
        };
        if(!find_playlist_step(root, &st)) {
            return false;
        }
        strlcpy(prev_name, best_name, sizeof(prev_name));
    }

    if(out_name != NULL) {
        strlcpy(out_name, best_name, out_name_size);
    }
    if(out_dir != NULL) {
        strlcpy(out_dir, best_dir, out_dir_size);
    }
    if(out_file != NULL) {
        strlcpy(out_file, best_file, out_file_size);
    }
    return true;
}

static void title_from_path(const char* path, char* title, size_t title_size) {
    if(title_size == 0) {
        return;
    }
#if defined(HAVE_TAGCACHE) && defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
    if(tagcache_is_usable()) {
        static struct mp3entry shared_title_id3;
        memset(&shared_title_id3, 0, sizeof(shared_title_id3));
        if(tagcache_fill_tags(&shared_title_id3, path) && shared_title_id3.title != NULL && shared_title_id3.title[0] != '\0') {
            strlcpy(title, shared_title_id3.title, title_size);
            return;
        }
    }
#endif
    const char* base = strrchr(path, '/');
    base             = base != NULL ? base + 1 : path;
    const char* dot  = strrchr(base, '.');
    if(dot != NULL && dot > base) {
        snprintf(title, title_size, "%.*s", (int)(dot - base), base);
    } else {
        strlcpy(title, base, title_size);
    }
}

static void extract_album_or_dir(const struct mp3entry* id3, const char* filename, char* out, size_t size) {
    if(id3 != NULL && id3->album != NULL && id3->album[0] != '\0') {
        strlcpy(out, id3->album, size);
        return;
    }
    if(filename != NULL) {
        const char* slash = strrchr(filename, '/');
        if(slash != NULL && slash > filename) {
            const char* prev = slash - 1;
            while(prev > filename && *prev != '/') {
                prev--;
            }
            if(*prev == '/') {
                prev++;
            }
            snprintf(out, size, "%.*s", (int)(slash - prev), prev);
            return;
        }
    }
    if(size > 0) {
        out[0] = '\0';
    }
}

static void get_trackinfo_ex(const unsigned int track, struct mp3entry* id3, char* out_album, size_t out_album_size) {
    const int amount = playlist_amount();
    memset(id3, 0, sizeof(*id3));
    if(amount <= 0) {
        if(out_album != NULL && out_album_size > 0) {
            out_album[0] = '\0';
        }
        return;
    }

    const int        tracknum = ((int)track + playlist_get_first_index(NULL)) % amount;
    struct mp3entry* cur      = (audio_status() != 0) ? audio_current_track() : NULL;
    if(cur != NULL && playlist_next(0) == tracknum) {
        memcpy(id3, cur, sizeof(*id3));
        if(out_album != NULL) {
            extract_album_or_dir(id3, cur->path, out_album, out_album_size);
        }
        return;
    }

    struct playlist_track_info info;
    if(playlist_get_track_info(NULL, tracknum, &info) != 0) {
        if(out_album != NULL && out_album_size > 0) {
            out_album[0] = '\0';
        }
        return;
    }
#if defined(HAVE_TAGCACHE) && defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
    if(!tagcache_is_usable() || !tagcache_fill_tags(id3, info.filename))
#endif
    {
        get_metadata(id3, -1, info.filename);
    }
    if(out_album != NULL) {
        extract_album_or_dir(id3, info.filename, out_album, out_album_size);
    }
}

static bool start_playlist_file(const char* dir, const char* file, int start_index) {
    if(playlist_create(dir, file) == -1) {
        return false;
    }
    if(global_settings.playlist_shuffle) {
        playlist_shuffle(0, -1);
    }
    const int amount = playlist_amount();
    if(amount <= 0) {
        return false;
    }
    if(start_index < 0 || start_index >= amount) {
        start_index = 0;
    }
    playlist_start(start_index, 0, 0);
    return true;
}

#ifdef HAVE_TAGCACHE
static void apply_tagcache_filters(struct Platform* plt, struct tagcache_search* tcs, int target_tag, struct tagcache_search_clause* kind_clause) {
    if(target_tag != tag_artist && plt->db_artist_seek >= 0) {
        tagcache_search_add_filter(tcs, tag_artist, plt->db_artist_seek);
    }
    if(target_tag != tag_album && plt->db_album_seek >= 0) {
        tagcache_search_add_filter(tcs, tag_album, plt->db_album_seek);
    }
    if(target_tag != tag_genre && plt->db_genre_seek >= 0) {
        tagcache_search_add_filter(tcs, tag_genre, plt->db_genre_seek);
    }
    if(target_tag != tag_composer && plt->db_composer_seek >= 0) {
        tagcache_search_add_filter(tcs, tag_composer, plt->db_composer_seek);
    }
    if(plt->db_kind >= 0) {
        kind_clause->tag          = tag_virt_kind;
        kind_clause->type         = clause_is;
        kind_clause->numeric      = true;
        kind_clause->source       = source_constant;
        kind_clause->numeric_data = plt->db_kind;
        kind_clause->str          = NULL;
        tagcache_search_add_clause(tcs, kind_clause);
    }
}

static bool play_tagcache_album_seek(int32_t album_seek) {
    close_shared_cat_tcs();
    if(!tagcache_is_usable() || playlist_create(NULL, NULL) == -1) {
        return false;
    }
    struct playlist_insert_context pl_ctx;
    if(playlist_insert_context_create(NULL, &pl_ctx, PLAYLIST_INSERT_LAST, false, false) < 0) {
        playlist_insert_context_release(&pl_ctx);
        return false;
    }
    static struct tagcache_search tcs;
    static char                   path[MAX_PATH];
    uint32_t                      inserted = 0;
    if(tagcache_search(&tcs, tag_filename)) {
        tagcache_search_add_filter(&tcs, tag_album, album_seek);
        while(tagcache_get_next(&tcs, path, sizeof(path))) {
            playlist_insert_context_add(&pl_ctx, path);
            inserted++;
        }
        tagcache_search_finish(&tcs);
    }
    playlist_insert_context_release(&pl_ctx);
    if(inserted == 0) {
        return false;
    }
    playlist_sync(NULL);
    playlist_start(0, 0, 0);
    return true;
}

static bool skip_album_via_tagcache(const char* cur_album, int dir) {
    close_shared_cat_tcs();
    if(!tagcache_is_usable()) {
        return false;
    }
    static struct tagcache_search tcs;
    static uint32_t               uniqbuf[2048];
    static char                   name[128];
    int32_t                       first_seek = -1;
    int32_t                       last_seek  = -1;
    int32_t                       prev_seek  = -1;
    int32_t                       pick_seek  = -1;
    bool                          take_next  = false;

    if(!tagcache_search(&tcs, tag_album)) {
        return false;
    }
    tagcache_search_set_uniqbuf(&tcs, uniqbuf, sizeof(uniqbuf));
    while(tagcache_get_next(&tcs, name, sizeof(name))) {
        if(strcmp(name, UNTAGGED) == 0) {
            continue;
        }
        if(first_seek < 0) {
            first_seek = tcs.result_seek;
        }
        if(take_next) {
            pick_seek = tcs.result_seek;
            break;
        }
        if(cur_album[0] != '\0' && strcasecmp(name, cur_album) == 0) {
            if(dir > 0) {
                take_next = true;
            } else if(prev_seek >= 0) {
                pick_seek = prev_seek;
                break;
            } else {
                take_next = true; /* Wait for end so we wrap to last_seek cleanly. */
            }
        }
        prev_seek = tcs.result_seek;
        last_seek = tcs.result_seek;
    }
    tagcache_search_finish(&tcs);

    if(pick_seek < 0) {
        pick_seek = (dir > 0) ? first_seek : last_seek;
    }
    return pick_seek >= 0 && play_tagcache_album_seek(pick_seek);
}
#endif

static bool skip_album(int dir) {
    const int amount = playlist_amount();
    char      cur_album[128];
    cur_album[0] = '\0';

    if(amount > 0) {
        int cur_disp = (audio_status() != 0) ? (playlist_get_display_index() - 1) : 0;
        if(cur_disp < 0 || cur_disp >= amount) {
            cur_disp = 0;
        }
        get_trackinfo_ex((unsigned int)cur_disp, &shared_id3, cur_album, sizeof(cur_album));

        int  step    = dir;
        int  visited = 0;
        char cand_album[128];
        while(visited < amount && playlist_check(step)) {
            int cand_disp = ((cur_disp + step) % amount + amount) % amount;
            get_trackinfo_ex((unsigned int)cand_disp, &shared_id3, cand_album, sizeof(cand_album));
            if(cand_album[0] != '\0' && strcasecmp(cand_album, cur_album) != 0) {
                if(dir < 0) {
                    char prev_album[128];
                    while((visited + 1) < amount && playlist_check(step - 1)) {
                        int prev_disp = ((cur_disp + step - 1) % amount + amount) % amount;
                        if(prev_disp == cur_disp) {
                            break;
                        }
                        get_trackinfo_ex((unsigned int)prev_disp, &shared_id3, prev_album, sizeof(prev_album));
                        if(strcasecmp(prev_album, cand_album) != 0) {
                            break;
                        }
                        step -= 1;
                        visited += 1;
                    }
                }
                if(audio_status() != 0) {
                    audio_skip(step);
                } else {
                    int target_disp = ((cur_disp + step) % amount + amount) % amount;
                    playlist_start((target_disp + playlist_get_first_index(NULL)) % amount, 0, 0);
                }
                return true;
            }
            step += dir;
            visited += 1;
        }
    }

#ifdef HAVE_TAGCACHE
    if(skip_album_via_tagcache(cur_album, dir)) {
        return true;
    }
#endif
    if(playlist_allow_dirplay(NULL)) {
        if(dir > 0) {
            audio_next_dir();
        } else {
            audio_prev_dir();
        }
        return true;
    }
    return false;
}

static bool skip_playlist_catalog(int dir) {
    const uint32_t total = count_real_playlists();
    if(total == 0) {
        return false;
    }

    struct playlist_info* cur_pl  = playlist_get_current();
    int32_t               cur_idx = -1;
    if(cur_pl != NULL && cur_pl->filename[0] != '\0') {
        char p_dir[MAX_PATH];
        char p_file[MAX_PATH];
        char full[MAX_PATH];
        for(uint32_t i = 0; i < total; i++) {
            if(find_playlist_by_index(i, NULL, 0, p_dir, sizeof(p_dir), p_file, sizeof(p_file))) {
                snprintf(full, sizeof(full), "%s/%s", p_dir, p_file);
                if(strcasecmp(cur_pl->filename, full) == 0) {
                    cur_idx = (int32_t)i;
                    break;
                }
            }
        }
    }

    uint32_t next_idx;
    if(cur_idx < 0) {
        next_idx = (dir > 0) ? 0 : (total - 1);
    } else {
        next_idx = (uint32_t)(((cur_idx + dir) % (int32_t)total + (int32_t)total) % (int32_t)total);
    }

    char sel_dir[MAX_PATH];
    char sel_file[MAX_PATH];
    if(!find_playlist_by_index(next_idx, NULL, 0, sel_dir, sizeof(sel_dir), sel_file, sizeof(sel_file))) {
        return false;
    }
    return start_playlist_file(sel_dir, sel_file, 0);
}

void iap_platform_autoplay(struct IAPContext* iap_ctx) {
    (void)iap_ctx;
    const int st = audio_status();
    if(st & AUDIO_STATUS_PLAY) {
        if(st & AUDIO_STATUS_PAUSE) {
            audio_resume();
        }
        return;
    }
    if(playlist_amount() <= 0) {
        playlist_resume();
    }
    if(playlist_amount() > 0) {
        int           idx     = global_status.resume_index;
        unsigned long elapsed = global_status.resume_elapsed;
        unsigned long offset  = global_status.resume_offset;
        if(idx < 0 || idx >= playlist_amount()) {
            idx     = 0;
            elapsed = 0;
            offset  = 0;
        }
        playlist_start(idx, elapsed, offset);
    }
}

void iap_platform_control(struct IAPContext* iap_ctx, enum IAPPlatformControl control, struct IAPPlatformPendingControl pending) {
    struct Platform* plt = iap_ctx->platform;

    if((control == IAPPlatformControl_Play || control == IAPPlatformControl_TogglePlayPause) && plt->db_selection_pending &&
       (has_active_tag_filters(plt) || plt->db_has_playlist || plt->db_has_track)) {
        iap_platform_db_play_selection(iap_ctx, plt->db_has_track ? plt->db_track_idx : 0, pending);
        return;
    }
    if((control == IAPPlatformControl_Play || control == IAPPlatformControl_TogglePlayPause) && audio_status() == 0) {
        iap_platform_autoplay(iap_ctx);
        check_act(iap_control_response(iap_ctx, pending, iap_true), );
        return;
    }

    long    button            = BUTTON_NONE;
    IAPBool ret               = iap_true;
    bool    started_from_stop = false;
    switch(control) {
    case IAPPlatformControl_TogglePlayPause:
        button = BUTTON_MULTIMEDIA_PLAYPAUSE;
        break;
    case IAPPlatformControl_Play:
        if(audio_status() != AUDIO_STATUS_PLAY) {
            button = BUTTON_MULTIMEDIA_PLAYPAUSE;
        }
        break;
    case IAPPlatformControl_Pause:
        if(audio_status() == AUDIO_STATUS_PLAY) {
            button = BUTTON_MULTIMEDIA_PLAYPAUSE;
        }
        break;
    case IAPPlatformControl_Stop:
        button = BUTTON_MULTIMEDIA_STOP;
        break;
    case IAPPlatformControl_Next:
        button = BUTTON_MULTIMEDIA_NEXT;
        break;
    case IAPPlatformControl_Prev:
        button = BUTTON_MULTIMEDIA_PREV;
        break;
    case IAPPlatformControl_NextAlbum: {
        const bool was_stopped = (audio_status() == 0);
        ret                    = skip_album(1);
        started_from_stop      = ret && was_stopped;
    } break;
    case IAPPlatformControl_PrevAlbum: {
        const bool was_stopped = (audio_status() == 0);
        ret                    = skip_album(-1);
        started_from_stop      = ret && was_stopped;
    } break;
    case IAPPlatformControl_NextPlaylist: {
        const bool was_stopped = (audio_status() == 0);
        ret                    = skip_playlist_catalog(1);
        started_from_stop      = ret && was_stopped;
    } break;
    case IAPPlatformControl_PrevPlaylist: {
        const bool was_stopped = (audio_status() == 0);
        ret                    = skip_playlist_catalog(-1);
        started_from_stop      = ret && was_stopped;
    } break;
    case IAPPlatformControl_VolumeUp:
        adjust_volume(1);
        break;
    case IAPPlatformControl_VolumeDown:
        adjust_volume(-1);
        break;
    case IAPPlatformControl_ToggleMute:
        /* rockbox has no mute/unmute */
        break;
    }
    logf("control=%d button=0x%04lX", control, button);

    if(button != BUTTON_NONE) {
        const bool was_stopped = (audio_status() == 0);
        check_act(button_queue_try_post(button, 0), ret = false);
        started_from_stop = ret && button == BUTTON_MULTIMEDIA_PLAYPAUSE && was_stopped;
    }
    if(started_from_stop) {
        /* we are transitioning from stopped to playing, which may take time.
         * to maintain synchronization with accessories, do not send an ack until
         * playback actually begins. */
        plt->control_pending = true;
        plt->pending_control = pending;
        return;
    }
    check_act(iap_control_response(iap_ctx, pending, ret), );
}

static uint8_t normalize_8(int val, int min, int max) {
    return 0xFF * (val - min) / (max - min);
}

IAPBool iap_platform_get_volume(struct IAPContext* iap_ctx, struct IAPPlatformVolumeStatus* status) {
    (void)iap_ctx;

    status->volume = _iap_convert_volume(global_status.volume);
    status->muted  = iap_false;
    return iap_true;
}

IAPBool iap_platform_get_power_status(struct IAPContext* iap_ctx, struct IAPPlatformPowerStatus* status) {
    (void)iap_ctx;
    status->battery_level = _iap_convert_battery_level(battery_level());
    status->state         = _iap_convert_charge_status(charge_state);
    return iap_true;
}

IAPBool iap_platform_get_shuffle_setting(struct IAPContext* iap_ctx, uint8_t* status) {
    (void)iap_ctx;
    *status = _iap_convert_shuffle_state(global_settings.playlist_shuffle);
    return iap_true;
}

static void apply_shuffle(int mode, bool save) {
    global_settings.playlist_shuffle = mode;
    if(save) {
        settings_save();
    }
    if(audio_status() & AUDIO_STATUS_PLAY) {
        playlist_reorder(mode);
    }
}

IAPBool iap_platform_set_shuffle_setting(struct IAPContext* iap_ctx, uint8_t status, IAPBool restore_on_exit) {
    struct Platform* plt = iap_ctx->platform;

    const int mode = status == IAPIPodStateShuffleSettingState_Albums   ? SHUFFLE_ALBUMS
                     : status == IAPIPodStateShuffleSettingState_Tracks ? SHUFFLE_SONGS
                                                                        : SHUFFLE_OFF;
    if(status <= IAPIPodStateShuffleSettingState_Albums && mode != global_settings.playlist_shuffle) {
        if(!restore_on_exit) {
            plt->saved_shuffle = -1; /* a lasting change replaces the user's */
        } else if(plt->saved_shuffle < 0) {
            plt->saved_shuffle = global_settings.playlist_shuffle;
        }
        apply_shuffle(mode, !restore_on_exit);
    }
    return iap_true;
}

IAPBool iap_platform_get_repeat_setting(struct IAPContext* iap_ctx, uint8_t* status) {
    (void)iap_ctx;
    *status = _iap_convert_repeat_state(global_settings.repeat_mode);
    return iap_true;
}

static void apply_repeat(int mode, bool save) {
    global_settings.repeat_mode = mode;
    if(save) {
        settings_save();
    }
    if(audio_status() & AUDIO_STATUS_PLAY) {
        audio_flush_and_reload_tracks();
    }
}

IAPBool iap_platform_set_repeat_setting(struct IAPContext* iap_ctx, uint8_t status, IAPBool restore_on_exit) {
    struct Platform* plt = iap_ctx->platform;

    /* there are more repeat options in Rockbox rather than iAP.
     * e.g. RB Repeat One and AB are both mapped to iAP Repeat One.
     * so we should not accept iAP Repeat One as RB Repeat One, if we have
     * RB Repeat AB set. */
    if(_iap_convert_repeat_state(global_settings.repeat_mode) == status) {
        /* already the same (or equievalent) mode set */
        return iap_true;
    }

    static const uint8_t table[] = {
        [IAPIPodStateRepeatSettingState_Off] = REPEAT_OFF,
        [IAPIPodStateRepeatSettingState_One] = REPEAT_ONE,
        [IAPIPodStateRepeatSettingState_All] = REPEAT_ALL,
    };
    check_act(status < ARRAY_SIZE(table), return iap_false);
    /* e.g. a Volvo XC40 sets repeat all, to be restored on exit, whenever it
     * starts playback: keep that to the session instead of saving it */
    if(!restore_on_exit) {
        plt->saved_repeat_mode = -1; /* a lasting change replaces the user's */
    } else if(plt->saved_repeat_mode < 0) {
        plt->saved_repeat_mode = global_settings.repeat_mode;
    }
    apply_repeat(table[status], !restore_on_exit);
    return iap_true;
}

void iap_platform_restore_on_exit(struct Platform* plt) {
    bool restored = false;
    if(plt->saved_repeat_mode >= 0) {
        if(global_settings.repeat_mode != plt->saved_repeat_mode) {
            apply_repeat(plt->saved_repeat_mode, false);
            restored = true;
        }
        plt->saved_repeat_mode = -1;
    }
    if(plt->saved_shuffle >= 0) {
        if(global_settings.playlist_shuffle != plt->saved_shuffle) {
            apply_shuffle(plt->saved_shuffle, false);
            restored = true;
        }
        plt->saved_shuffle = -1;
    }
    /* anything else saved meanwhile may have stored the session's values */
    if(restored) {
        settings_save();
    }
}

IAPBool iap_platform_get_date_time(struct IAPContext* iap_ctx, struct IAPDateTime* time) {
    (void)iap_ctx;
    _iap_convert_datetime(get_time(), time);
    return iap_true;
}

IAPBool iap_platform_get_backlight_level(struct IAPContext* iap_ctx, uint8_t* level) {
    (void)iap_ctx;

#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    *level = normalize_8(global_settings.brightness, MIN_BRIGHTNESS_SETTING, MAX_BRIGHTNESS_SETTING);
#else
    *level = 0xFF;
#endif
    return iap_true;
}

IAPBool iap_platform_get_hold_switch_state(struct IAPContext* iap_ctx, IAPBool* state) {
    (void)iap_ctx;

#ifdef HAS_BUTTON_HOLD
    *state = button_hold();
#else
    *state = iap_false;
#endif
    return iap_true;
}

/* taken from iap-core.c */
static void get_trackinfo(const unsigned int track, struct mp3entry* id3) {
    get_trackinfo_ex(track, id3, NULL, 0);
}

IAPBool iap_platform_get_indexed_track_info(struct IAPContext* iap_ctx, uint32_t index, struct IAPPlatformTrackInfo* info) {
    struct Platform* plt    = iap_ctx->platform;
    const int        amount = playlist_amount();
    check_act(amount > 0 && (int)index < amount, return iap_false);

    get_trackinfo(index, &shared_id3);

    if(info->total_ms != NULL) {
        *info->total_ms = shared_id3.length;
    }
    if(info->caps != NULL) {
        *info->caps = IAPIPodStateTrackCapBits_HasReleaseDate;
        if(audio_status() != 0 && (int)index == playlist_get_display_index() - 1 && plt->aa_slot >= 0 && playback_current_aa_hid(plt->aa_slot) >= 0) {
            *info->caps |= IAPIPodStateTrackCapBits_HasAlbumArts;
        }
    }
    if(info->release_date != NULL) {
        info->release_date->year    = shared_id3.year;
        info->release_date->month   = 0;
        info->release_date->day     = 0;
        info->release_date->hour    = 0;
        info->release_date->minute  = 0;
        info->release_date->seconds = 0;
    }
    if(info->artist != NULL) {
        check_act(iap_span_append(info->artist, shared_id3.artist, strlen(shared_id3.artist) + 1), return iap_false);
    }
    if(info->composer != NULL) {
        check_act(iap_span_append(info->composer, shared_id3.composer, strlen(shared_id3.composer) + 1), return iap_false);
    }
    if(info->album != NULL) {
        check_act(iap_span_append(info->album, shared_id3.album, strlen(shared_id3.album) + 1), return iap_false);
    }
    if(info->title != NULL) {
        check_act(iap_span_append(info->title, shared_id3.title, strlen(shared_id3.title) + 1), return iap_false);
    }
    return iap_true;
}

IAPBool iap_platform_set_playing_track(struct IAPContext* iap_ctx, uint32_t index) {
    (void)iap_ctx;
    const int amount = playlist_amount();
    iap_trace_logf("set_playing_track: idx=%lu amount=%d", (unsigned long)index, amount);
    if(amount <= 0 || (int)index >= amount) {
        return iap_false;
    }
    if(audio_status() == 0) {
        const int raw = ((int)index + playlist_get_first_index(NULL)) % amount;
        playlist_start(raw, 0, 0);
        return iap_true;
    }
    int cur_disp = playlist_get_display_index() - 1;
    if(cur_disp < 0) {
        cur_disp = 0;
    }
    audio_skip((int)index - cur_disp);
    return iap_true;
}

IAPBool iap_platform_open_artwork(struct IAPContext* iap_ctx, uint32_t index, struct IAPPlatformArtwork* artwork) {
    struct Platform* plt = iap_ctx->platform;
    /* only aa for currently playing track is available */
    check_act((int)index == playlist_get_display_index() - 1, return iap_false);
    const int hid = playback_current_aa_hid(plt->aa_slot);
    check_act(hid >= 0, return iap_false, "%d %d", plt->aa_slot, hid);
    struct bitmap* bmp;
    check_act(bufgetdata(hid, 0, (void*)&bmp) > 0, return iap_false);
    artwork->color  = iap_true;
    artwork->width  = bmp->width;
    artwork->height = bmp->height;
    artwork->opaque = hid;
    return iap_true;
}

IAPBool iap_platform_get_artwork_ptr(struct IAPContext* iap_ctx, struct IAPPlatformArtwork* artwork, struct IAPSpan* span) {
    struct Platform* plt = iap_ctx->platform;

    /* check the albumart has not reloaded */
    /* FIXME: not a correct check due to possibility of hid confliction */
    const int hid = playback_current_aa_hid(plt->aa_slot);
    check_act(hid == (int)artwork->opaque, return iap_false);

    struct bitmap* bmp;
    /* more checks */
    check_act(bufgetdata(hid, 0, (void*)&bmp) > 0, return iap_false);
    check_act(bmp->width == artwork->width && bmp->height == artwork->height, return iap_false);

    span->ptr  = bmp->data;
    span->size = bmp->width * bmp->height * 2;
    return iap_true;
}

IAPBool iap_platform_close_artwork(struct IAPContext* iap_ctx, struct IAPPlatformArtwork* artwork) {
    (void)iap_ctx;
    (void)artwork;
    return iap_true;
}

void iap_platform_dump_hex(const void* ptr, size_t size) {
    if(ptr == NULL) {
        logf("(null)");
        return;
    }

#if DEBUG_HEXDUMP_NOLIMIT != 1
    size = MIN(size, 32);
#endif

    static const char chars[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

    char line[4 * (8 + 1) + 1];
    for(size_t l = 0; l * 16 < size; l += 1) {
        char* c = line;
        for(size_t b = 0; b < 4; b += 1) {
            for(size_t i = 0; i < 4; i += 1) {
                size_t index = l * 16 + b * 4 + i;
                if(index >= size) {
                    break;
                }
                *c++ = chars[((uint8_t*)ptr)[index] >> 4];
                *c++ = chars[((uint8_t*)ptr)[index] & 0xF];
            }
            *c++ = ' ';
        }
        *c++ = '\0';
        logf("%04X: %s", l * 16, line);
    }
}

IAPBool iap_platform_on_acc_samprs_received(struct IAPContext* iap_ctx, struct IAPSpan* samprs) {
    struct Platform* plt  = iap_ctx->platform;
    uint8_t          mask = 0;

    while(samprs->size > 0) {
        uint32_t sample_rate;
        check_act(iap_span_read_32(samprs, &sample_rate), return iap_false);
        if(sample_rate == SAMPR_48) {
            mask |= IAP_SAMPR_BIT_48;
        } else if(sample_rate == SAMPR_44) {
            mask |= IAP_SAMPR_BIT_44;
        } else if(sample_rate == SAMPR_32) {
            mask |= IAP_SAMPR_BIT_32;
        }
    }
    check_act(mask != 0, return iap_false, "accessory reported no supported sample rates");
    plt->acc_sampr_mask = mask;
    iap_audio_configure_samprs(mask);
    check_act(mixer_switch_sink(PCM_SINK_IAP), return false);
    return iap_true;
}

static void ensure_db_init(struct Platform* plt) {
    if(plt->db_initialized) {
        return;
    }
    plt->db_initialized       = true;
    plt->db_hierarchy         = 0;
    plt->db_has_playlist      = false;
    plt->db_playlist_idx      = 0;
    plt->db_artist_seek       = -1;
    plt->db_album_seek        = -1;
    plt->db_genre_seek        = -1;
    plt->db_composer_seek     = -1;
    plt->db_kind              = -1;
    plt->db_has_track         = false;
    plt->db_track_idx         = 0;
    plt->db_browsed_mask      = 0;
    plt->db_listed_mask       = 0;
    plt->db_selection_pending = false;
}

static bool has_active_tag_filters(const struct Platform* plt) {
    return plt->db_artist_seek >= 0 || plt->db_album_seek >= 0 || plt->db_genre_seek >= 0 || plt->db_composer_seek >= 0 || plt->db_kind >= 0;
}

static bool playlist_file_query(const char* dir, const char* file, int32_t target_idx, uint32_t* out_count, char* out_title, size_t out_title_size) {
    static char full_path[MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir, file);
    const int fd = open(full_path, O_RDONLY);
    if(fd < 0) {
        if(out_count != NULL) {
            *out_count = 0;
        }
        return false;
    }

    static char line[MAX_PATH];
    static char resolved[MAX_PATH];
    uint32_t    count = 0;
    bool        found = false;
    while(read_line(fd, line, sizeof(line)) > 0) {
        char* s = line;
        if((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
            s += 3;
        }
        s = skip_whitespace(s);
        if(s[0] == '\0' || s[0] == '#') {
            continue;
        }
        if(target_idx >= 0 && (uint32_t)target_idx == count) {
            if(playlist_resolve_path(resolved, sizeof(resolved), s, dir) >= 0) {
                title_from_path(resolved, out_title, out_title_size);
            } else {
                title_from_path(s, out_title, out_title_size);
            }
            found = true;
            if(out_count == NULL) {
                break;
            }
        }
        count++;
    }
    close(fd);
    if(out_count != NULL) {
        *out_count = count;
    }
    return target_idx < 0 ? true : found;
}

#ifdef HAVE_TAGCACHE
static bool query_tagcache_category(struct Platform* plt, int tag, int kind_override, int32_t target_idx, uint32_t* out_count, char* out_name,
                                    size_t out_name_size, int32_t* out_seek) {
    if(!tagcache_is_usable()) {
        close_shared_cat_tcs();
        if(out_count != NULL) {
            *out_count = 0;
        }
        return false;
    }

    const int eff_kind   = (kind_override >= 0) ? kind_override : plt->db_kind;
    const int saved_kind = plt->db_kind;

    if(out_count == NULL && target_idx >= 0) {
        const bool can_reuse = shared_cat_tcs_open && shared_cat_tag == tag && shared_cat_kind == eff_kind &&
                               shared_cat_artist == plt->db_artist_seek && shared_cat_album == plt->db_album_seek &&
                               shared_cat_genre == plt->db_genre_seek && shared_cat_composer == plt->db_composer_seek &&
                               (uint32_t)target_idx >= shared_cat_next_idx;
        if(!can_reuse) {
            close_shared_cat_tcs();
            if(!tagcache_search(&shared_cat_tcs, tag)) {
                return false;
            }
            if(tag != tag_title && tag != tag_filename) {
                tagcache_search_set_uniqbuf(&shared_cat_tcs, shared_cat_uniqbuf, sizeof(shared_cat_uniqbuf));
            }
            plt->db_kind = eff_kind;
            apply_tagcache_filters(plt, &shared_cat_tcs, tag, &shared_cat_kind_clause);
            plt->db_kind = saved_kind;

            shared_cat_tcs_open = true;
            shared_cat_tag      = tag;
            shared_cat_kind     = eff_kind;
            shared_cat_artist   = plt->db_artist_seek;
            shared_cat_album    = plt->db_album_seek;
            shared_cat_genre    = plt->db_genre_seek;
            shared_cat_composer = plt->db_composer_seek;
            shared_cat_next_idx = 0;
        }

        while(tagcache_get_next(&shared_cat_tcs, shared_cat_buf, sizeof(shared_cat_buf))) {
            if(tag != tag_title && strcmp(shared_cat_buf, UNTAGGED) == 0) {
                continue;
            }
            const uint32_t cur = shared_cat_next_idx++;
            if(cur == (uint32_t)target_idx) {
                if(out_name != NULL && out_name_size > 0) {
                    strlcpy(out_name, shared_cat_buf, out_name_size);
                }
                if(out_seek != NULL) {
                    *out_seek = shared_cat_tcs.result_seek;
                }
                return true;
            }
        }
        close_shared_cat_tcs();
        return false;
    }

    close_shared_cat_tcs();
    if(!tagcache_search(&shared_cat_tcs, tag)) {
        if(out_count != NULL) {
            *out_count = 0;
        }
        return false;
    }
    if(tag != tag_title && tag != tag_filename) {
        tagcache_search_set_uniqbuf(&shared_cat_tcs, shared_cat_uniqbuf, sizeof(shared_cat_uniqbuf));
    }
    plt->db_kind = eff_kind;
    apply_tagcache_filters(plt, &shared_cat_tcs, tag, &shared_cat_kind_clause);
    plt->db_kind = saved_kind;

    uint32_t count = 0;
    bool     found = false;
    while(tagcache_get_next(&shared_cat_tcs, shared_cat_buf, sizeof(shared_cat_buf))) {
        if(tag != tag_title && strcmp(shared_cat_buf, UNTAGGED) == 0) {
            continue;
        }
        if(target_idx >= 0 && (uint32_t)target_idx == count) {
            if(out_name != NULL && out_name_size > 0) {
                strlcpy(out_name, shared_cat_buf, out_name_size);
            }
            if(out_seek != NULL) {
                *out_seek = shared_cat_tcs.result_seek;
            }
            found = true;
            if(out_count == NULL) {
                break;
            }
        }
        count++;
    }
    tagcache_search_finish(&shared_cat_tcs);

    if(out_count != NULL) {
        *out_count = count;
    }
    return target_idx < 0 ? true : found;
}
#endif

#ifdef HAVE_TAGCACHE
static bool db_tag_for_type(uint8_t type, int* out_tag, int* out_kind) {
    *out_kind = -1;
    switch(type) {
    case IAPDatabaseType_Artist:
        *out_tag = tag_artist;
        return true;
    case IAPDatabaseType_Album:
        *out_tag = tag_album;
        return true;
    case IAPDatabaseType_Genre:
        *out_tag = tag_genre;
        return true;
    case IAPDatabaseType_Composer:
        *out_tag = tag_composer;
        return true;
    case IAPDatabaseType_Audiobook:
        *out_tag  = tag_title;
        *out_kind = 2;
        return true;
    case IAPDatabaseType_Podcast:
        *out_tag  = tag_album;
        *out_kind = 1;
        return true;
    default:
        return false;
    }
}
#endif

IAPBool iap_platform_db_reset(struct IAPContext* iap_ctx, uint8_t selection_hierarchy) {
    struct Platform* plt = iap_ctx->platform;
    if(selection_hierarchy != 0 && selection_hierarchy != IAPResetDBSelectionHierarchySelection_Audio &&
       selection_hierarchy != IAPResetDBSelectionHierarchySelection_Video) {
        return iap_false;
    }
    plt->db_initialized       = true;
    plt->db_hierarchy         = selection_hierarchy;
    plt->db_has_playlist      = false;
    plt->db_playlist_idx      = 0;
    plt->db_artist_seek       = -1;
    plt->db_album_seek        = -1;
    plt->db_genre_seek        = -1;
    plt->db_composer_seek     = -1;
    plt->db_kind              = -1;
    plt->db_has_track         = false;
    plt->db_track_idx         = 0;
    plt->db_browsed_mask      = 0;
    plt->db_listed_mask       = 0;
    plt->db_selection_pending = false;
#ifdef HAVE_TAGCACHE
    close_shared_cat_tcs();
#endif
    return iap_true;
}

IAPBool iap_platform_db_get_count(struct IAPContext* iap_ctx, uint8_t type, uint32_t* count) {
    struct Platform* plt = iap_ctx->platform;
    ensure_db_init(plt);

    if(plt->db_hierarchy == IAPResetDBSelectionHierarchySelection_Video) {
        *count = 0;
        return iap_true;
    }
    if(type < 16) {
        plt->db_browsed_mask |= (uint16_t)(1u << type);
    }

    switch(type) {
    case IAPDatabaseType_Playlist:
    case IAPDatabaseType_NestedPlaylist: {
        const uint32_t real = count_real_playlists();
        if(real > 0) {
            *count = real;
        } else if(playlist_amount() > 0
#ifdef HAVE_TAGCACHE
                  || tagcache_is_usable()
#endif
        ) {
            *count = 1; /* virtual "All Tracks" fallback */
        } else {
            *count = 0;
        }
        return iap_true;
    }
    case IAPDatabaseType_Artist:
    case IAPDatabaseType_Album:
    case IAPDatabaseType_Genre:
    case IAPDatabaseType_Composer:
    case IAPDatabaseType_Audiobook:
    case IAPDatabaseType_Podcast: {
#ifdef HAVE_TAGCACHE
        int tag  = 0;
        int kind = -1;
        db_tag_for_type(type, &tag, &kind);
        query_tagcache_category(plt, tag, kind, -1, count, NULL, 0, NULL);
#else
        *count = 0;
#endif
        return iap_true;
    }
    case IAPDatabaseType_Track: {
        if(plt->db_has_playlist) {
            const uint32_t real = count_real_playlists();
            if(real > 0) {
                static char dir[MAX_PATH];
                static char file[MAX_PATH];
                if(!find_playlist_by_index(plt->db_playlist_idx, NULL, 0, dir, sizeof(dir), file, sizeof(file))) {
                    *count = 0;
                    return iap_false;
                }
                return playlist_file_query(dir, file, -1, count, NULL, 0) ? iap_true : iap_false;
            }
        }
#ifdef HAVE_TAGCACHE
        if(has_active_tag_filters(plt)) {
            query_tagcache_category(plt, tag_title, -1, -1, count, NULL, 0, NULL);
            return iap_true;
        }
        /* Unfiltered Track category = whole library (not the playback queue),
         * so the list matches what PlayCurrentSelection will enqueue. */
        if(tagcache_is_usable()) {
            const int sc = tagcache_get_song_count();
            if(sc >= 0) {
                *count = (uint32_t)sc;
                return iap_true;
            }
            query_tagcache_category(plt, tag_title, -1, -1, count, NULL, 0, NULL);
            return iap_true;
        }
#endif
        const int amount = playlist_amount();
        if(amount > 0) {
            *count = (uint32_t)amount;
            return iap_true;
        }
        *count = 0;
        return iap_true;
    }
    case IAPDatabaseType_TopLevel:
    case IAPDatabaseType_GeniusMixes:
    case IAPDatabaseType_ITunesU:
        *count = 0;
        return iap_true;
    default:
        return iap_false;
    }
}

IAPBool iap_platform_db_select(struct IAPContext* iap_ctx, uint8_t type, uint32_t index) {
    struct Platform* plt = iap_ctx->platform;
    ensure_db_init(plt);

    if(plt->db_hierarchy == IAPResetDBSelectionHierarchySelection_Video) {
        return iap_false;
    }

    iap_trace_logf("db_select: type=0x%02X idx=%ld mask=0x%04X", type, (long)(int32_t)index, plt->db_browsed_mask);

    switch(type) {
    case IAPDatabaseType_TopLevel:
        iap_platform_db_reset(iap_ctx, plt->db_hierarchy);
        return iap_true;
    case IAPDatabaseType_Playlist:
    case IAPDatabaseType_NestedPlaylist: {
        if(index == 0xFFFFFFFFu) {
            plt->db_has_playlist = false;
            plt->db_has_track    = false;
            plt->db_browsed_mask = 0;
            plt->db_listed_mask  = 0;
            return iap_true;
        }
        uint32_t total = 0;
        check_act(iap_platform_db_get_count(iap_ctx, IAPDatabaseType_Playlist, &total), return iap_false);
        if(index >= total) {
            return iap_false;
        }
        plt->db_has_playlist      = true;
        plt->db_playlist_idx      = index;
        plt->db_artist_seek       = -1;
        plt->db_album_seek        = -1;
        plt->db_genre_seek        = -1;
        plt->db_composer_seek     = -1;
        plt->db_kind              = -1;
        plt->db_has_track         = false;
        plt->db_browsed_mask      = 0;
        plt->db_listed_mask       = 0;
        plt->db_selection_pending = true;
        return iap_true;
    }
    case IAPDatabaseType_Genre:
    case IAPDatabaseType_Artist:
    case IAPDatabaseType_Album:
    case IAPDatabaseType_Composer:
    case IAPDatabaseType_Podcast:
    case IAPDatabaseType_Audiobook: {
#ifdef HAVE_TAGCACHE
        if(index == 0xFFFFFFFFu) {
            plt->db_has_playlist = false;
            plt->db_has_track    = false;
            plt->db_browsed_mask = 0;
            plt->db_listed_mask  = 0;
            if(type == IAPDatabaseType_Genre) {
                plt->db_genre_seek    = -1;
                plt->db_artist_seek   = -1;
                plt->db_album_seek    = -1;
                plt->db_composer_seek = -1;
                plt->db_kind          = -1;
            } else if(type == IAPDatabaseType_Artist) {
                plt->db_artist_seek = -1;
                plt->db_album_seek  = -1;
                plt->db_kind        = -1;
            } else if(type == IAPDatabaseType_Album) {
                plt->db_album_seek = -1;
                plt->db_kind       = -1;
            } else if(type == IAPDatabaseType_Composer) {
                plt->db_composer_seek = -1;
                plt->db_album_seek    = -1;
                plt->db_kind          = -1;
            } else {
                plt->db_kind       = -1;
                plt->db_album_seek = -1;
            }
            return iap_true;
        }
        if(type < 16 && !(plt->db_browsed_mask & (1u << type))) {
            if(type == IAPDatabaseType_Artist) {
                plt->db_genre_seek = -1;
                plt->db_kind       = -1;
            } else if(type == IAPDatabaseType_Album) {
                plt->db_artist_seek   = -1;
                plt->db_genre_seek    = -1;
                plt->db_composer_seek = -1;
                plt->db_kind          = -1;
            } else if(type == IAPDatabaseType_Composer) {
                plt->db_genre_seek  = -1;
                plt->db_artist_seek = -1;
                plt->db_kind        = -1;
            }
        }
        int     tag  = 0;
        int     kind = -1;
        int32_t seek = -1;
        db_tag_for_type(type, &tag, &kind);
        if(!query_tagcache_category(plt, tag, kind, (int32_t)index, NULL, NULL, 0, &seek)) {
            if(has_active_tag_filters(plt)) {
                plt->db_artist_seek   = -1;
                plt->db_album_seek    = -1;
                plt->db_genre_seek    = -1;
                plt->db_composer_seek = -1;
                plt->db_kind          = -1;
                if(!query_tagcache_category(plt, tag, kind, (int32_t)index, NULL, NULL, 0, &seek)) {
                    return iap_false;
                }
            } else {
                return iap_false;
            }
        }
        plt->db_has_playlist = false;
        plt->db_has_track    = false;
        if(type == IAPDatabaseType_Genre) {
            plt->db_genre_seek    = seek;
            plt->db_artist_seek   = -1;
            plt->db_album_seek    = -1;
            plt->db_composer_seek = -1;
            plt->db_kind          = -1;
        } else if(type == IAPDatabaseType_Artist) {
            plt->db_artist_seek = seek;
            plt->db_album_seek  = -1;
            plt->db_kind        = -1;
        } else if(type == IAPDatabaseType_Album) {
            plt->db_album_seek = seek;
            plt->db_kind       = -1;
        } else if(type == IAPDatabaseType_Composer) {
            plt->db_composer_seek = seek;
            plt->db_album_seek    = -1;
            plt->db_kind          = -1;
        } else if(type == IAPDatabaseType_Podcast) {
            plt->db_kind          = 1;
            plt->db_album_seek    = seek;
            plt->db_artist_seek   = -1;
            plt->db_genre_seek    = -1;
            plt->db_composer_seek = -1;
        } else {
            const bool user_pick  = leaf_select_is_user_pick(plt, IAPDatabaseType_Audiobook);
            plt->db_kind          = 2;
            plt->db_has_track     = true;
            plt->db_track_idx     = index;
            plt->db_artist_seek   = -1;
            plt->db_album_seek    = -1;
            plt->db_genre_seek    = -1;
            plt->db_composer_seek = -1;
            plt->db_browsed_mask  = 0;
            plt->db_listed_mask   = 0;
            if(user_pick) {
                return play_current_db_selection(iap_ctx, index);
            }
            plt->db_selection_pending = true;
            return iap_true;
        }
        plt->db_browsed_mask      = 0;
        plt->db_listed_mask       = 0;
        plt->db_selection_pending = true;
        return iap_true;
#else
        return iap_false;
#endif
    }
    case IAPDatabaseType_Track: {
        if(index == 0xFFFFFFFFu) {
            plt->db_has_track = false;
            return iap_true;
        }
        uint32_t total = 0;
        check_act(iap_platform_db_get_count(iap_ctx, IAPDatabaseType_Track, &total), return iap_false);
        if(index >= total) {
            return iap_false;
        }
        plt->db_has_track = true;
        plt->db_track_idx = index;
        if(leaf_select_is_user_pick(plt, IAPDatabaseType_Track)) {
            return play_current_db_selection(iap_ctx, index);
        }
        iap_trace_logf("db_select: track %lu kept pending (probe)", (unsigned long)index);
        plt->db_selection_pending = true;
        return iap_true;
    }
    default:
        return iap_false;
    }
}

IAPBool iap_platform_db_get_record(struct IAPContext* iap_ctx, uint8_t type, uint32_t index, char* buf, size_t buf_size) {
    struct Platform* plt = iap_ctx->platform;
    ensure_db_init(plt);

    if(buf == NULL || buf_size == 0 || plt->db_hierarchy == IAPResetDBSelectionHierarchySelection_Video) {
        return iap_false;
    }
    if(type < 16) {
        plt->db_browsed_mask |= (uint16_t)(1u << type);
        plt->db_listed_mask |= (uint16_t)(1u << type);
    }

    switch(type) {
    case IAPDatabaseType_Playlist:
    case IAPDatabaseType_NestedPlaylist: {
        const uint32_t real = count_real_playlists();
        if(real > 0) {
            return find_playlist_by_index(index, buf, buf_size, NULL, 0, NULL, 0) ? iap_true : iap_false;
        }
        if(index == 0) {
            strlcpy(buf, "All Tracks", buf_size);
            return iap_true;
        }
        return iap_false;
    }
    case IAPDatabaseType_Artist:
    case IAPDatabaseType_Album:
    case IAPDatabaseType_Genre:
    case IAPDatabaseType_Composer:
    case IAPDatabaseType_Audiobook:
    case IAPDatabaseType_Podcast: {
#ifdef HAVE_TAGCACHE
        int tag  = 0;
        int kind = -1;
        db_tag_for_type(type, &tag, &kind);
        return query_tagcache_category(plt, tag, kind, (int32_t)index, NULL, buf, buf_size, NULL) ? iap_true : iap_false;
#else
        return iap_false;
#endif
    }
    case IAPDatabaseType_Track: {
        if(plt->db_has_playlist) {
            const uint32_t real = count_real_playlists();
            if(real > 0) {
                static char dir[MAX_PATH];
                static char file[MAX_PATH];
                if(!find_playlist_by_index(plt->db_playlist_idx, NULL, 0, dir, sizeof(dir), file, sizeof(file))) {
                    return iap_false;
                }
                return playlist_file_query(dir, file, (int32_t)index, NULL, buf, buf_size) ? iap_true : iap_false;
            }
        }
#ifdef HAVE_TAGCACHE
        if(has_active_tag_filters(plt) || tagcache_is_usable()) {
            return query_tagcache_category(plt, tag_title, -1, (int32_t)index, NULL, buf, buf_size, NULL) ? iap_true : iap_false;
        }
#endif
        const int amount = playlist_amount();
        if(amount > 0 && (int)index < amount) {
            get_trackinfo(index, &shared_id3);
            if(shared_id3.title != NULL && shared_id3.title[0] != '\0') {
                strlcpy(buf, shared_id3.title, buf_size);
            } else {
                struct playlist_track_info info;
                const int                  raw = ((int)index + playlist_get_first_index(NULL)) % amount;
                if(playlist_get_track_info(NULL, raw, &info) == 0) {
                    title_from_path(info.filename, buf, buf_size);
                } else {
                    buf[0] = '\0';
                }
            }
            return iap_true;
        }
#ifdef HAVE_TAGCACHE
        return query_tagcache_category(plt, tag_title, -1, (int32_t)index, NULL, buf, buf_size, NULL) ? iap_true : iap_false;
#else
        return iap_false;
#endif
    }
    default:
        return iap_false;
    }
}

static IAPBool play_current_db_selection(struct IAPContext* iap_ctx, uint32_t track_index) {
    struct Platform* plt = iap_ctx->platform;
    ensure_db_init(plt);
    plt->db_selection_pending = false;

    if(plt->db_hierarchy == IAPResetDBSelectionHierarchySelection_Video) {
        return iap_false;
    }

    if(plt->db_has_playlist) {
        const uint32_t real = count_real_playlists();
        if(real > 0) {
            static char dir[MAX_PATH];
            static char file[MAX_PATH];
            int         start_idx = (track_index == 0xFFFFFFFFu) ? 0 : (int)track_index;
            if(!find_playlist_by_index(plt->db_playlist_idx, NULL, 0, dir, sizeof(dir), file, sizeof(file)) ||
               !start_playlist_file(dir, file, start_idx)) {
                return iap_false;
            }
            return iap_true;
        }
    }

#ifdef HAVE_TAGCACHE
    if(tagcache_is_usable() && (has_active_tag_filters(plt) || track_index != 0xFFFFFFFFu || plt->db_has_track || playlist_amount() == 0)) {
        close_shared_cat_tcs();
        if(playlist_create(NULL, NULL) == -1) {
            return iap_false;
        }
        struct playlist_insert_context pl_ctx;
        if(playlist_insert_context_create(NULL, &pl_ctx, PLAYLIST_INSERT_LAST, false, false) < 0) {
            playlist_insert_context_release(&pl_ctx);
            return iap_false;
        }
        uint32_t start_idx = track_index;
        if((start_idx == 0xFFFFFFFFu || start_idx == 0) && plt->db_has_track) {
            start_idx = plt->db_track_idx;
        }
        if(start_idx == 0xFFFFFFFFu) {
            start_idx = 0;
        }
        const bool     filtered    = has_active_tag_filters(plt);
        const uint32_t skip_before = filtered ? 0 : start_idx;
        const uint32_t max_insert  = filtered ? 1024 : 128;

        /* Iterate the same tag/filter set used to list Track records
         * (query_tagcache_category(tag_title)) so track_index refers to the very
         * same entry the head unit displayed, then map it to its filename. */
        static struct tagcache_search        tcs;
        static struct tagcache_search_clause kind_clause;
        static char                          title[256];
        static char                          path[MAX_PATH];
        uint32_t                             seen     = 0;
        uint32_t                             inserted = 0;
        if(tagcache_search(&tcs, tag_title)) {
            apply_tagcache_filters(plt, &tcs, tag_title, &kind_clause);
            while(tagcache_get_next(&tcs, title, sizeof(title))) {
                if(seen++ < skip_before) {
                    continue;
                }
                if(tagcache_retrieve(&tcs, tcs.idx_id, tag_filename, path, sizeof(path)) && path[0] != '\0') {
                    playlist_insert_context_add(&pl_ctx, path);
                    inserted++;
                    if(inserted >= max_insert) {
                        break;
                    }
                }
            }
            tagcache_search_finish(&tcs);
        }
        playlist_insert_context_release(&pl_ctx);
        if(!filtered) {
            start_idx = 0;
        } else if(start_idx >= inserted) {
            start_idx = 0;
        }
        iap_trace_logf("play_selection: track=%ld start=%lu inserted=%lu filtered=%d "
                       "album=%ld artist=%ld",
                       (long)(int32_t)track_index, (unsigned long)start_idx, (unsigned long)inserted, filtered ? 1 : 0, (long)plt->db_album_seek,
                       (long)plt->db_artist_seek);
        if(inserted == 0) {
            return iap_false;
        }
        playlist_sync(NULL);
        playlist_start((int)start_idx, 0, 0);
        return iap_true;
    }
#endif

    if(playlist_amount() == 0 && playlist_resume() == -1) {
        return iap_false;
    }
    if(track_index != 0xFFFFFFFFu) {
        return iap_platform_set_playing_track(iap_ctx, track_index);
    } else if(audio_status() != AUDIO_STATUS_PLAY) {
        iap_platform_autoplay(iap_ctx);
    }
    return iap_true;
}

void iap_platform_db_play_selection(struct IAPContext* iap_ctx, uint32_t track_index, struct IAPPlatformPendingControl pending) {
    struct Platform* plt = iap_ctx->platform;
    ensure_db_init(plt);
    if(!plt->db_selection_pending && plt->db_has_track && (track_index == 0xFFFFFFFFu || track_index == 0 || track_index == plt->db_track_idx) &&
       (audio_status() & AUDIO_STATUS_PLAY)) {
        check_act(iap_control_response(iap_ctx, pending, iap_true), );
        return;
    }
    const IAPBool ok = play_current_db_selection(iap_ctx, track_index);
    check_act(iap_control_response(iap_ctx, pending, ok), );
}

uint8_t _iap_convert_play_status(int rb_audio_status) {
    if(rb_audio_status & AUDIO_STATUS_PAUSE) {
        return IAPIPodStatePlayStatus_PlaybackPaused;
    } else if(rb_audio_status & AUDIO_STATUS_PLAY) {
        return IAPIPodStatePlayStatus_Playing;
    } else {
        return IAPIPodStatePlayStatus_PlaybackStopped;
    }
}

uint8_t _iap_convert_volume(int rb_volume) {
    return normalize_8(rb_volume, sound_min(SOUND_VOLUME), sound_max(SOUND_VOLUME));
}

uint8_t _iap_convert_shuffle_state(int rb_state) {
    return rb_state == SHUFFLE_ALBUMS ? IAPIPodStateShuffleSettingState_Albums
           : rb_state ? IAPIPodStateShuffleSettingState_Tracks
           : IAPIPodStateShuffleSettingState_Off;
}

uint8_t _iap_convert_repeat_state(int rb_state) {
    static const uint8_t table[] = {
        [REPEAT_OFF]     = IAPIPodStateRepeatSettingState_Off,
        [REPEAT_ALL]     = IAPIPodStateRepeatSettingState_All,
        [REPEAT_ONE]     = IAPIPodStateRepeatSettingState_One,
        [REPEAT_SHUFFLE] = IAPIPodStateRepeatSettingState_All,
#ifdef AB_REPEAT_ENABLE
        [REPEAT_AB] = IAPIPodStateRepeatSettingState_One,
#endif
    };

    if(rb_state == REPEAT_OFF && global_settings.next_folder) {
        /* when repeat is off and next folder is enabled, report repeat all.
         * without this hack, the accessory will likely set repeat to all,
         * resulting in trapping us in the current directory. */
        return IAPIPodStateRepeatSettingState_All;
    } else {
        check_act(rb_state < ARRAY_SIZE(table), return iap_false);
        return table[rb_state];
    }
}

uint8_t _iap_convert_battery_level(int rb_battery_level) {
    return 0xFF * rb_battery_level / 100;
}

uint8_t _iap_convert_charge_status(enum charge_state_type rb_charge_state) {
    switch(rb_charge_state) {
    case CHARGING:
        return IAPIPodStatePowerState_ExternalCharging;
    case TOPOFF:
    case TRICKLE:
        return IAPIPodStatePowerState_ExternalCharged;
        break;
    default:
        return IAPIPodStatePowerState_Internal;
    }
}

void _iap_convert_datetime(struct tm* rb_time, struct IAPDateTime* time) {
    time->year    = rb_time->tm_year + 1900;
    time->month   = rb_time->tm_mon + 1;
    time->day     = rb_time->tm_mday;
    time->hour    = rb_time->tm_hour;
    time->minute  = rb_time->tm_min;
    time->seconds = rb_time->tm_sec;
}
