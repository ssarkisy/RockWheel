/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Sergei Sarkisian
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

/* The iTunesDB that Finder writes when it syncs the iPod, read after each
   sync like the stock firmware does: its playlists become .m3u8 files, and
   its ratings and play counts go into the database. The file is too big to
   load, so it is read a record at a time. The layout is libgpod's. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "core_alloc.h"
#include "rbunicode.h"
#include "string-extra.h"
#include "pathfuncs.h"
#include "settings.h"
#include "misc.h"
#include "rbpaths.h"
#include "tagcache.h"
#include "logf.h"
#include "lcd.h"
#include "itunesdb.h"

/* what the last import was made from, and the tracks in iTunesDB order with
   the counts they had then, which Play Counts entries are made against */
#define STATE_FILE      ROCKBOX_DIR "/itunesdb.dat"
#define STATE_MAGIC     0x54494252 /* "RBIT" */
#define STATE_VERSION   1
/* the playlists made from iTunesDB, removed again on the next import */
#define PLAYLISTS_FILE  ROCKBOX_DIR "/itunesdb_playlists.txt"

/* the folder where Finder keeps the music */
#define MUSIC_DIR       "/iPod_Control/Music"

#define CACHE_SIZE      32768
#define POOL_PER_TRACK  64 /* bytes of path; Finder's take about 35 */
#define WRITE_BUF_SIZE  4096

struct state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t db_size;
    uint32_t db_mtime;
    uint32_t count;
};

struct state_entry {
    uint64_t hash;
    uint32_t playcount;
    uint8_t rating;  /* 0..100, 20 a star */
    uint8_t media;   /* the media type, 0xff when it doesn't fit */
    uint16_t unused;
};

struct track {
    uint32_t id;     /* what playlists refer to */
    uint32_t path;   /* offset in the path pool, 0 for none */
    uint32_t art;    /* the ArtworkDB image id of its cover, 0 for none */
    struct state_entry st;
};

static struct {
    int fd;
    unsigned char *cache;
    off_t cache_pos;
    int cache_len;
    struct track *tracks;
    int count;
    uint32_t *by_id;   /* indexes of tracks[], by id */
    uint32_t *by_hash; /* and by path hash */
    char *pool;
    size_t pool_size, pool_used;
    bool complete;     /* every track listed was read */
} db;

static uint32_t le32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static const unsigned char *db_get(off_t pos, size_t len)
{
    if (len > CACHE_SIZE || pos < 0)
        return NULL;
    if (pos < db.cache_pos || pos + (off_t)len > db.cache_pos + db.cache_len)
    {
        if (lseek(db.fd, pos, SEEK_SET) != pos)
            return NULL;
        db.cache_pos = pos;
        db.cache_len = read(db.fd, db.cache, CACHE_SIZE);
        if (db.cache_len < (int)len)
        {
            db.cache_len = 0;
            return NULL;
        }
    }
    return db.cache + (pos - db.cache_pos);
}

/* the record with this tag at pos, at least len bytes of it */
static const unsigned char *db_record(off_t pos, const char *tag, size_t len)
{
    const unsigned char *p = db_get(pos, len);
    return (p && !memcmp(p, tag, 4)) ? p : NULL;
}

/* the string of the mhod at pos as UTF-8. iTunesDB strings are UTF-16 */
static bool mhod_string(off_t pos, char *buf, size_t size)
{
    const unsigned char *p = db_record(pos, "mhod", 40);
    if (!p)
        return false;
    size_t units = MIN(le32(p + 28) / 2, (size - 1) / 3);
    p = db_get(pos + 40, units * 2);
    if (!p)
        return false;
    *utf16LEdecode(p, (unsigned char *)buf, units) = '\0';
    return true;
}

/* the path without its volume: Rockbox has "/<HDD0>/iPod_Control/..." where
   iTunesDB has "/iPod_Control/..." */
static const char *strip_volume(const char *path)
{
#ifdef HAVE_MULTIVOLUME
    path_strip_volume(path, &path, false);
#endif
    return path;
}

uint64_t itunesdb_path_hash(const char *path)
{
    /* FNV-1a, ignoring ASCII case like FAT does */
    uint64_t hash = 0xcbf29ce484222325ull;
    path = strip_volume(path);
    for (const unsigned char *p = (const unsigned char *)path; *p; p++)
    {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        hash = (hash ^ c) * 0x100000001b3ull;
    }
    return hash;
}

/* the size and time of iTunesDB, which change with every sync */
static bool db_stamp(uint32_t *size, uint32_t *mtime)
{
    DIR *dir = opendir(ITUNESDB_DIR);
    if (!dir)
        return false;
    bool found = false;
    struct dirent *entry;
    while (!found && (entry = readdir(dir)))
    {
        if (strcasecmp(entry->d_name, "iTunesDB"))
            continue;
        struct dirinfo info = dir_get_info(dir, entry);
        *size = info.size;
        *mtime = info.mtime;
        found = true;
    }
    closedir(dir);
    return found;
}

static bool read_state_header(struct state_header *hdr)
{
    int fd = open(STATE_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    bool ok = read(fd, hdr, sizeof(*hdr)) == sizeof(*hdr)
        && hdr->magic == STATE_MAGIC && hdr->version == STATE_VERSION;
    close(fd);
    return ok;
}

bool itunesdb_changed(void)
{
    uint32_t size, mtime;
    struct state_header hdr;
    if (!db_stamp(&size, &mtime))
        return false;
    return !read_state_header(&hdr) || hdr.db_size != size
        || hdr.db_mtime != mtime;
}

/* tracks */

static void read_track(off_t pos, struct track *track)
{
    char path[MAX_PATH];
    const unsigned char *p = db_record(pos, "mhit", 12);
    memset(track, 0, sizeof(*track));
    if (!p)
        return;
    uint32_t header_len = le32(p + 4);
    p = db_get(pos, MIN(header_len, 356));
    if (!p || header_len < 0x9c)
        return;
    int mhods = le32(p + 12);
    track->id = le32(p + 16);
    track->st.rating = p[31];
    track->st.playcount = le32(p + 80);
    if (header_len >= 212)
    {
        uint32_t media = le32(p + 208);
        track->st.media = media > 0xfe ? 0xff : media;
    }
    if (header_len >= 356)
        track->art = le32(p + 352);

    off_t child = pos + header_len;
    for (int i = 0; i < mhods; i++)
    {
        p = db_record(child, "mhod", 16);
        if (!p)
            break;
        uint32_t total = le32(p + 8);
        if (le32(p + 12) == 2 && mhod_string(child, path, sizeof(path)))
        {
            /* ":iPod_Control:Music:F07:CWZV.m4a" */
            for (char *c = path; *c; c++)
                if (*c == ':')
                    *c = '/';
            size_t len = strlen(path) + 1;
            if (db.pool_used + len <= db.pool_size)
            {
                track->path = db.pool_used;
                memcpy(db.pool + db.pool_used, path, len);
                db.pool_used += len;
                track->st.hash = itunesdb_path_hash(path);
            }
            break;
        }
        if (total == 0)
            break;
        child += total;
    }
}

static bool read_tracks(off_t list)
{
    const unsigned char *p = db_record(list, "mhlt", 12);
    if (!p)
        return false;
    off_t pos = list + le32(p + 4);
    int listed = le32(p + 8);
    int count = MIN(listed, db.count);
    db.count = 0;
    for (int i = 0; i < count; i++)
    {
        p = db_record(pos, "mhit", 12);
        if (!p)
            break;
        uint32_t total = le32(p + 8); /* before read_track moves the cache */
        read_track(pos, &db.tracks[db.count++]);
        if (total == 0)
            break;
        pos += total;
        if ((i & 255) == 0)
            yield();
    }
    db.complete = db.count == listed;
    return db.count > 0;
}

static int cmp_id(const void *a, const void *b)
{
    uint32_t x = db.tracks[*(const uint32_t *)a].id;
    uint32_t y = db.tracks[*(const uint32_t *)b].id;
    return (x > y) - (x < y);
}

static int cmp_hash(const void *a, const void *b)
{
    uint64_t x = db.tracks[*(const uint32_t *)a].st.hash;
    uint64_t y = db.tracks[*(const uint32_t *)b].st.hash;
    return (x > y) - (x < y);
}

static struct track *find_by_id(uint32_t id)
{
    int lo = 0, hi = db.count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        struct track *t = &db.tracks[db.by_id[mid]];
        if (t->id == id)
            return t;
        if (t->id < id)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

static struct track *find_by_hash(uint64_t hash)
{
    int lo = 0, hi = db.count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        struct track *t = &db.tracks[db.by_hash[mid]];
        if (t->st.hash == hash)
            return t;
        if (t->st.hash < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* playlists */

static void playlist_dir(char *buf, size_t size)
{
    const char *dir = PLAYLIST_CATALOG_DEFAULT_DIR;
    if (global_settings.playlist_catalog_dir[0] != '\0')
        dir = global_settings.playlist_catalog_dir;
    strmemccpy(buf, dir, size);
}

/* the playlists of the last import go, as Finder may have removed some */
static void remove_old_playlists(void)
{
    char line[MAX_PATH];
    int fd = open(PLAYLISTS_FILE, O_RDONLY);
    if (fd < 0)
        return;
    while (read_line(fd, line, sizeof(line)) > 0)
        if (line[0] == '/')
            remove(line);
    close(fd);
    remove(PLAYLISTS_FILE);
}

/* a playlist name as a file name that FAT takes; false if it is too long */
static bool playlist_file(char *buf, size_t size, const char *dir,
                          const char *name)
{
    char clean[MAX_PATH / 2];
    size_t n = 0;
    for (const char *c = name; *c && n < sizeof(clean) - 1; c++)
        clean[n++] = strchr("/\\:*?\"<>|", *c) ? '_' : *c;
    clean[n] = '\0';
    if (n == 0)
        strcpy(clean, "Playlist");

    if ((size_t)snprintf(buf, size, "%s/%s.m3u8", dir, clean) >= size)
        return false;
    for (int i = 2; file_exists(buf) && i < 100; i++)
        if ((size_t)snprintf(buf, size, "%s/%s %d.m3u8", dir, clean, i) >= size)
            return false;
    return true;
}

struct writer {
    int fd;
    size_t used;
    char buf[WRITE_BUF_SIZE];
};

static void writer_flush(struct writer *w)
{
    if (w->used)
        write(w->fd, w->buf, w->used);
    w->used = 0;
}

static void writer_line(struct writer *w, const char *line)
{
    size_t len = strlen(line);
    if (w->used + len + 1 > sizeof(w->buf))
        writer_flush(w);
    if (len + 1 > sizeof(w->buf))
        return;
    memcpy(w->buf + w->used, line, len);
    w->buf[w->used + len] = '\n';
    w->used += len + 1;
}

/* the mhyp at pos as a playlist file, listed in the playlists file */
static void write_playlist(off_t pos, const char *dir, int list_fd,
                           struct writer *w)
{
    char name[MAX_PATH / 2], file[MAX_PATH];
    const unsigned char *p = db_record(pos, "mhyp", 44);
    if (!p)
        return;
    uint32_t header_len = le32(p + 4);
    int mhods = le32(p + 12);
    int items = le32(p + 16);
    bool hidden = le32(p + 20) != 0;   /* the library itself */
    bool podcasts = (p[42] | p[43] << 8) != 0;
    if (hidden || podcasts)
        return;

    name[0] = '\0';
    off_t child = pos + header_len;
    for (int i = 0; i < mhods; i++)
    {
        p = db_record(child, "mhod", 16);
        if (!p)
            return;
        uint32_t total = le32(p + 8);
        if (le32(p + 12) == 1)
            mhod_string(child, name, sizeof(name));
        if (total == 0)
            return;
        child += total;
    }

    if (!playlist_file(file, sizeof(file), dir, name))
        return;
    w->fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (w->fd < 0)
        return;
    w->used = 0;
    writer_line(w, "#EXTM3U");
    for (int i = 0; i < items; i++)
    {
        p = db_record(child, "mhip", 28);
        if (!p)
            break;
        uint32_t total = le32(p + 8);
        struct track *t = find_by_id(le32(p + 24));
        if (t && t->path)
            writer_line(w, db.pool + t->path);
        if (total == 0)
            break;
        child += total;
    }
    writer_flush(w);
    close(w->fd);
    fdprintf(list_fd, "%s\n", file);
}

static void write_playlists(off_t list)
{
    char dir[MAX_PATH];
    const unsigned char *p = db_record(list, "mhlp", 12);
    if (!p)
        return;
    off_t pos = list + le32(p + 4);
    int count = le32(p + 8);

    playlist_dir(dir, sizeof(dir));
    mkdir(dir);
    int list_fd = open(PLAYLISTS_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (list_fd < 0)
        return;
    static struct writer w;
    for (int i = 0; i < count; i++)
    {
        p = db_record(pos, "mhyp", 12);
        if (!p)
            break;
        uint32_t total = le32(p + 8);
        write_playlist(pos, dir, list_fd, &w);
        if (total == 0)
            break;
        pos += total;
        yield();
    }
    close(list_fd);
}

/* ratings and play counts */

/* 1 to set the counts, 0 to leave the entry, -1 to remove it */
static int update_entry(const char *path, long *playcount, long *rating,
                        void *data)
{
    (void)data;
    struct track *t = find_by_hash(itunesdb_path_hash(path));
    if (!t)
    {
        /* Finder renames and removes the files in its folder as it syncs,
           so one that iTunesDB doesn't list is left from an older sync.
           Only trusted when all of iTunesDB could be read. */
        return (db.complete && !strncasecmp(strip_volume(path), MUSIC_DIR "/",
                                            sizeof(MUSIC_DIR))) ? -1 : 0;
    }
    *playcount = t->st.playcount;
    *rating = t->st.rating / 10; /* the database counts half stars */
    return 1;
}

static bool write_state(uint32_t size, uint32_t mtime)
{
    struct state_header hdr = {
        STATE_MAGIC, STATE_VERSION, size, mtime, db.count
    };
    int fd = open(STATE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    bool ok = write(fd, &hdr, sizeof(hdr)) == sizeof(hdr);
    for (int i = 0; ok && i < db.count; i++)
        ok = write(fd, &db.tracks[i].st, sizeof(struct state_entry))
             == sizeof(struct state_entry);
    close(fd);
    return ok;
}

/* iTunesDB and its tracks, for the import and for the covers */

static int cache_handle, table_handle;
static off_t playlists_list;
static uint32_t db_size, db_mtime;

static void db_unload(void)
{
    if (db.fd > 0)
        close(db.fd);
    if (table_handle > 0)
    {
        core_unpin(table_handle);
        core_free(table_handle);
    }
    if (cache_handle > 0)
    {
        core_unpin(cache_handle);
        core_free(cache_handle);
    }
    cache_handle = table_handle = 0;
    memset(&db, 0, sizeof(db));
    cpu_boost(false);
}

static bool db_load(void)
{
    off_t tracks_list = -1, pos;
    const unsigned char *p;
    int sections;

    memset(&db, 0, sizeof(db));
    playlists_list = -1;
    if (!db_stamp(&db_size, &db_mtime))
        return false;
    db.fd = open(ITUNESDB_FILE, O_RDONLY);
    if (db.fd < 0)
        return false;
    cpu_boost(true);

    /* 4 more bytes for the second half of a surrogate pair at the end */
    cache_handle = core_alloc(CACHE_SIZE + 4);
    if (cache_handle <= 0)
        return false;
    core_pin(cache_handle);
    db.cache = core_get_data(cache_handle);

    /* the sections: tracks are index 1, playlists index 2 */
    p = db_record(0, "mhbd", 24);
    if (!p)
        return false;
    pos = le32(p + 4);
    sections = le32(p + 20);
    for (int i = 0; i < sections; i++)
    {
        p = db_record(pos, "mhsd", 16);
        if (!p)
            break;
        uint32_t header_len = le32(p + 4), total = le32(p + 8);
        uint32_t index = le32(p + 12);
        if (index == 1)
            tracks_list = pos + header_len;
        else if (index == 2)
            playlists_list = pos + header_len;
        if (total == 0)
            break;
        pos += total;
    }
    if (tracks_list < 0 || !(p = db_record(tracks_list, "mhlt", 12)))
        return false;

    db.count = le32(p + 8);
    size_t table_size =
        db.count * (sizeof(struct track) + 2 * sizeof(uint32_t));
    db.pool_size = db.count * POOL_PER_TRACK;
    table_handle = core_alloc(table_size + db.pool_size);
    if (table_handle <= 0)
        return false;
    core_pin(table_handle);
    db.tracks = core_get_data(table_handle);
    db.by_id = (uint32_t *)(db.tracks + db.count);
    db.by_hash = db.by_id + db.count;
    db.pool = (char *)(db.by_hash + db.count);
    db.pool_used = 1; /* offset 0 means no path */

    if (!read_tracks(tracks_list))
        return false;
    for (int i = 0; i < db.count; i++)
        db.by_id[i] = db.by_hash[i] = i;
    qsort(db.by_id, db.count, sizeof(uint32_t), cmp_id);
    qsort(db.by_hash, db.count, sizeof(uint32_t), cmp_hash);
    return true;
}

void itunesdb_import(void)
{
    if (db_load())
    {
        remove_old_playlists();
        if (playlists_list >= 0)
            write_playlists(playlists_list);

        if (tagcache_update_runtime(update_entry, NULL))
            write_state(db_size, db_mtime);
        logf("itunesdb: %d tracks imported", db.count);
    }
    db_unload();
}

#ifdef ITUNESDB_HAVE_ART
/* Covers. Finder scales each one into the ithmb files beside ArtworkDB for
   the stock firmware, as RGB565 frames ready for the screen; the tracks
   point at them by image id. */

#define ARTWORK_DIR     "/iPod_Control/Artwork"
#define ARTWORKDB_FILE  ARTWORK_DIR "/ArtworkDB"
/* where each track's cover is, by path hash */
#define ART_FILE        ROCKBOX_DIR "/itunesdb_art.dat"
#define ART_MAGIC       0x41494252 /* "RBIA" */
#define ART_VERSION     1

struct art_header {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t format;    /* of the ithmb files, the largest there is */
    uint32_t width;     /* of its frames */
    uint32_t height;
};

struct art_entry {
    uint64_t hash;
    uint32_t offset;    /* in F<format>_<file>.ithmb */
    uint32_t file;
};

/* an ArtworkDB image */
struct art_image {
    uint32_t id;
    uint32_t offset;
    uint32_t file;
};

static struct art_header art_hdr;

/* the name of an ithmb file in an ArtworkDB mhod, which unlike iTunesDB
   says how it is encoded */
static void read_artwork_name(off_t pos, char *buf, size_t size)
{
    const unsigned char *p = db_record(pos, "mhod", 36);
    buf[0] = '\0';
    if (!p)
        return;
    size_t len = le32(p + 24);
    if (p[28] == 2)
    {
        size_t units = MIN(len / 2, (size - 1) / 3);
        p = db_get(pos + 36, units * 2);
        if (p)
            *utf16LEdecode(p, (unsigned char *)buf, units) = '\0';
    }
    else
    {
        len = MIN(len, size - 1);
        p = db_get(pos + 36, len);
        if (p)
        {
            memcpy(buf, p, len);
            buf[len] = '\0';
        }
    }
}

/* the frame of the largest format in the mhii at pos; the first image
   decides which format that is for all of them */
static bool read_image(off_t pos, struct art_image *image)
{
    struct { uint32_t format, offset, size, width, height, file; } frames[8];
    int nframes = 0;
    char name[48];
    const unsigned char *p = db_record(pos, "mhii", 20);
    if (!p)
        return false;
    uint32_t header_len = le32(p + 4);
    int children = le32(p + 12);
    image->id = le32(p + 16);

    off_t child = pos + header_len;
    for (int i = 0; i < children && nframes < (int)ARRAYLEN(frames); i++)
    {
        p = db_record(child, "mhod", 14);
        if (!p)
            break;
        uint32_t len = le32(p + 4), total = le32(p + 8);
        if ((p[12] | p[13] << 8) == 2
            && (p = db_record(child + len, "mhni", 36)))
        {
            uint32_t mhni_len = le32(p + 4);
            frames[nframes].format = le32(p + 16);
            frames[nframes].offset = le32(p + 20);
            frames[nframes].size = le32(p + 24);
            frames[nframes].height = p[32] | p[33] << 8;
            frames[nframes].width = p[34] | p[35] << 8;
            /* ":F1029_1.ithmb" or "F1029_1.ithmb" */
            read_artwork_name(child + len + mhni_len, name, sizeof(name));
            const char *n = strrchr(name, '_');
            frames[nframes].file = n ? atoi(n + 1) : 0;
            if (frames[nframes].file > 0)
                nframes++;
        }
        if (total == 0)
            break;
        child += total;
    }

    if (art_hdr.format == 0)
    {
        for (int i = 0; i < nframes; i++)
        {
            uint32_t pixels = frames[i].width * frames[i].height;
            if (frames[i].size == pixels * 2
                && pixels > art_hdr.width * art_hdr.height)
            {
                art_hdr.format = frames[i].format;
                art_hdr.width = frames[i].width;
                art_hdr.height = frames[i].height;
            }
        }
    }
    for (int i = 0; i < nframes; i++)
    {
        if (frames[i].format == art_hdr.format
            && frames[i].size == art_hdr.width * art_hdr.height * 2)
        {
            image->offset = frames[i].offset;
            image->file = frames[i].file;
            return true;
        }
    }
    return false;
}

static int cmp_image(const void *a, const void *b)
{
    uint32_t x = ((const struct art_image *)a)->id;
    uint32_t y = ((const struct art_image *)b)->id;
    return (x > y) - (x < y);
}

static struct art_image *find_image(struct art_image *images, int count,
                                    uint32_t id)
{
    int lo = 0, hi = count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (images[mid].id == id)
            return &images[mid];
        if (images[mid].id < id)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* ArtworkDB's images, sorted by id; returns the handle of their memory */
static int read_images(struct art_image **images, int *count)
{
    int handle;
    const unsigned char *p;

    close(db.fd);
    db.fd = open(ARTWORKDB_FILE, O_RDONLY);
    db.cache_pos = db.cache_len = 0;
    if (db.fd < 0 || !(p = db_record(0, "mhfd", 24)))
        return -1;
    off_t pos = le32(p + 4);
    int sections = le32(p + 20);
    for (int i = 0; i < sections; i++)
    {
        p = db_record(pos, "mhsd", 14);
        if (!p)
            break;
        uint32_t header_len = le32(p + 4), total = le32(p + 8);
        if ((p[12] | p[13] << 8) == 1 && (p = db_record(pos + header_len, "mhli", 12)))
        {
            off_t item = pos + header_len + le32(p + 4);
            int n = le32(p + 8);
            handle = core_alloc(n * sizeof(struct art_image) + 1);
            if (handle <= 0)
                return -1;
            core_pin(handle);
            *images = core_get_data(handle);
            for (int k = 0; k < n; k++)
            {
                p = db_record(item, "mhii", 12);
                if (!p)
                    break;
                uint32_t item_total = le32(p + 8);
                if (read_image(item, &(*images)[*count]))
                    (*count)++;
                if (item_total == 0)
                    break;
                item += item_total;
                if ((k & 255) == 0)
                    yield();
            }
            qsort(*images, *count, sizeof(struct art_image), cmp_image);
            return handle;
        }
        if (total == 0)
            break;
        pos += total;
    }
    return -1;
}

static void write_art_index(void)
{
    struct art_image *images = NULL;
    int art_count = 0;
    memset(&art_hdr, 0, sizeof(art_hdr));
    int handle = read_images(&images, &art_count);
    if (handle <= 0 || art_hdr.format == 0)
        goto done;

    int fd = open(ART_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        goto done;
    art_hdr.magic = ART_MAGIC;
    art_hdr.version = ART_VERSION;
    write(fd, &art_hdr, sizeof(art_hdr));
    /* by path hash, which db.by_hash already has them in */
    uint32_t count = 0;
    for (int i = 0; i < db.count; i++)
    {
        struct track *t = &db.tracks[db.by_hash[i]];
        struct art_image *image = t->path && t->art ?
            find_image(images, art_count, t->art) : NULL;
        if (!image)
            continue;
        struct art_entry entry = { t->st.hash, image->offset, image->file };
        write(fd, &entry, sizeof(entry));
        count++;
    }
    art_hdr.count = count;
    lseek(fd, 0, SEEK_SET);
    write(fd, &art_hdr, sizeof(art_hdr));
    close(fd);
    rename(ART_FILE ".tmp", ART_FILE);
    logf("itunesdb: %lu covers", (unsigned long)count);

done:
    if (handle > 0)
    {
        core_unpin(handle);
        core_free(handle);
    }
}

void itunesdb_import_art(void)
{
    /* the old covers point into ithmb files Finder has rewritten */
    remove(ART_FILE);
    if (db_load())
        write_art_index();
    db_unload();
}

bool itunesdb_find_art(const char *path, struct itunesdb_art *art)
{
    struct art_header hdr;
    struct art_entry entry;
    bool found = false;
    int fd = open(ART_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    if (read(fd, &hdr, sizeof(hdr)) == sizeof(hdr)
        && hdr.magic == ART_MAGIC && hdr.version == ART_VERSION)
    {
        uint64_t hash = itunesdb_path_hash(path);
        int lo = 0, hi = (int)hdr.count - 1;
        while (lo <= hi && !found)
        {
            int mid = (lo + hi) / 2;
            lseek(fd, sizeof(hdr) + mid * sizeof(entry), SEEK_SET);
            if (read(fd, &entry, sizeof(entry)) != sizeof(entry))
                break;
            if (entry.hash == hash)
                found = true;
            else if (entry.hash < hash)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
    }
    close(fd);
    if (!found)
        return false;
    if ((size_t)snprintf(art->path, sizeof(art->path),
                         ARTWORK_DIR "/F%lu_%lu.ithmb",
                         (unsigned long)hdr.format,
                         (unsigned long)entry.file) >= sizeof(art->path))
        return false;
    art->offset = entry.offset;
    art->width = hdr.width;
    art->height = hdr.height;
    return true;
}

static inline unsigned lerp(unsigned a, unsigned b, unsigned f)
{
    return (a * (256 - f) + b * f) >> 8;
}

/* the pixel between four, f in 256ths */
static fb_data bilerp(fb_data a, fb_data b, fb_data c, fb_data d,
                      unsigned fx, unsigned fy)
{
    unsigned r = lerp(lerp(a >> 11, b >> 11, fx), lerp(c >> 11, d >> 11, fx), fy);
    unsigned g = lerp(lerp((a >> 5) & 63, (b >> 5) & 63, fx),
                      lerp((c >> 5) & 63, (d >> 5) & 63, fx), fy);
    unsigned bl = lerp(lerp(a & 31, b & 31, fx), lerp(c & 31, d & 31, fx), fy);
    return r << 11 | g << 5 | bl;
}

int itunesdb_load_art(int fd, const struct itunesdb_art *art,
                      struct bitmap *bm, int max_size)
{
    int fw = art->width, fh = art->height;
    int w = bm->width, h = bm->height;
    if (fw <= 0 || fh <= 0 || w <= 0 || h <= 0)
        return -1;

    /* the largest that fits, up or down, keeping the shape */
    if (w * fh > h * fw)
        w = (h * fw + fh / 2) / fh;
    else
        h = (w * fh + fw / 2) / fw;
    int out_size = w * h * sizeof(fb_data);
    fb_data *out = (fb_data *)bm->data;

    /* the frame's rows are read into the room after the bitmap, which
       is there for decoding JPEG */
    fb_data *rows = out + w * h;
    int room = max_size - (int)sizeof(struct bitmap) - out_size;
    int max_rows = MIN(room / (int)(fw * sizeof(fb_data)), fh);
    if (max_rows < 2)
        return -1;
    int first = -1, nrows = 0;

    /* sample at pixel centres in 16.16 steps; this CPU has no divide, so
       nothing in the loops may divide */
    long xstep = ((long)fw << 16) / w, ystep = ((long)fh << 16) / h;
    long sy = ystep / 2 - 0x8000;
    for (int y = 0; y < h; y++, sy += ystep)
    {
        long cy = sy < 0 ? 0 : sy;
        int y0 = cy >> 16, y1 = MIN(y0 + 1, fh - 1);
        unsigned fy = (cy >> 8) & 255;
        if (first < 0 || y0 < first || y1 >= first + nrows)
        {
            first = y0;
            nrows = MIN(max_rows, fh - first);
            size_t bytes = nrows * fw * sizeof(fb_data);
            if (lseek(fd, art->offset + first * fw * sizeof(fb_data), SEEK_SET) < 0
                || read(fd, rows, bytes) != (ssize_t)bytes)
                return -1;
        }
        const fb_data *r0 = rows + (y0 - first) * fw;
        const fb_data *r1 = rows + (y1 - first) * fw;
        long sx = xstep / 2 - 0x8000;
        for (int x = 0; x < w; x++, sx += xstep)
        {
            long cx = sx < 0 ? 0 : sx;
            int x0 = cx >> 16, x1 = MIN(x0 + 1, fw - 1);
            *out++ = bilerp(r0[x0], r0[x1], r1[x0], r1[x1], (cx >> 8) & 255, fy);
        }
    }

    bm->width = w;
    bm->height = h;
#if LCD_DEPTH > 1
    bm->format = FORMAT_NATIVE;
    bm->maskdata = NULL;
#endif
#ifdef HAVE_LCD_COLOR
    bm->alpha_offset = 0;
#endif
    return out_size;
}
#endif /* ITUNESDB_HAVE_ART */
