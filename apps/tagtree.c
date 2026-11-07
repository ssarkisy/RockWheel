/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 by Miika Pekkarinen
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

/**
 * Basic structure on this file was copied from dbtree.c and modified to
 * support the tag cache interface.
 */

//#define LOGF_ENABLE

#include <stdio.h>
#include <stdlib.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "splash.h"
#include "icons.h"
#include "tree.h"
#include "action.h"
#include "settings.h"
#include "tagcache.h"
#include "itunesdb.h"
#include "tagtree.h"
#include "root_menu.h"
#include "timefuncs.h"
#include "lang.h"
#include "logf.h"
#include "talk.h"
#include "playlist.h"
#include "keyboard.h"
#include "gui/list.h"
#include "gui/transition.h"
#include "core_alloc.h"
#include "yesno.h"
#include "misc.h"
#include "filetypes.h"
#include "audio.h"
#include "appevents.h"
#include "storage.h"
#include "dir.h"
#include "playback.h"
#include "strnatcmp.h"
#include "panic.h"
#include "onplay.h"
#include "plugin.h"
#include "language.h"
#include "playlist_catalog.h"

#define str_or_empty(x) (x ? x : "(NULL)")

#define TAGNAVI_DEFAULT_CONFIG  ROCKBOX_DIR "/tagnavi.config"
#define TAGNAVI_USER_CONFIG     ROCKBOX_DIR "/tagnavi_user.config"
/* Songs to play are written into a file and the playlist is loaded from
   it, as from any playlist file: adding them one at a time writes the
   control file for each, which takes seconds for a whole library. There
   are two, so that the playlist playing goes on from its file while the
   other is written, and is replaced only once. */
#define TAGTREE_PLAYLIST_DIR    ROCKBOX_DIR
#define TAGTREE_SLOTS       4
#define TAGTREE_SLOT_SONGS  2 /* a pair for the list of all songs alone: others
                                 played meanwhile leave it ready */
static const char * const tagtree_playlist_names[TAGTREE_SLOTS] = {
    "tagtree.m3u8", "tagtree2.m3u8", "tagtree_songs.m3u8", "tagtree_songs2.m3u8"
};
/* Beside each, what spares writing it again: which list of which database it
   holds, and where each of its songs starts */
static const char * const tagtree_list_index_names[TAGTREE_SLOTS] = {
    ROCKBOX_DIR "/tagtree.idx", ROCKBOX_DIR "/tagtree2.idx",
    ROCKBOX_DIR "/tagtree_songs.idx", ROCKBOX_DIR "/tagtree_songs2.idx"
};
#define TAGTREE_LIST_MAGIC 0x4c545452 /* "RTTL" */
struct tagtree_list_header {
    uint32_t magic;
    int32_t commitid;   /* of the database */
    uint32_t entries;   /* of the list */
    uint32_t id;        /* which list, from how it is searched for */
    uint32_t count;     /* places after this */
};

/* Whether a playlist playing or to be resumed is from this file */
static bool tagtree_slot_in_use(int slot)
{
    char path[MAX_PATH], current[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", TAGTREE_PLAYLIST_DIR,
             tagtree_playlist_names[slot]);
    if (playlist_get_name(NULL, current, sizeof(current))
        && !strcasecmp(current, path))
        return true;
    /* at boot it is only named in the control file, on its first line */
    char head[MAX_PATH + 32];
    int fd = open(playlist_get_current()->control_filename, O_RDONLY);
    ssize_t len = fd >= 0 ? read(fd, head, sizeof(head) - 1) : -1;
    if (fd >= 0)
        close(fd);
    if (len <= 0)
        return false;
    head[len] = '\0';
    char *line_end = strchr(head, '\n');
    if (line_end)
        *line_end = '\0';
    return strstr(head, tagtree_playlist_names[slot]) != NULL;
}

/* The list of Music the music playing is from, as noted where it was started
   (tagtree_note_queue), with the start's count, its playlist's file and the
   database's commit */
static uint32_t queue_id;
static unsigned int queue_starts = ~0u;
static char queue_file[MAX_PATH];
static int32_t queue_commit;

static void tagtree_note_queue(const char *file, uint32_t id)
{
    strmemccpy(queue_file, file, sizeof(queue_file));
    queue_id = id;
    queue_starts = playlist_start_count();
    queue_commit = tagcache_get_commitid();
}

/* That list's id, or 0 if the music playing is of none. Started elsewhere, as
   resumed at a start, by the playlist's file: one of the slots', whose index
   says which list, of this database. That is read only while the disk is up:
   the first draw of a list after a sleep waited for it seconds. */
static uint32_t tagtree_queue_list(void)
{
    char current[MAX_PATH];
    if (!playlist_get_name(NULL, current, sizeof(current)))
        current[0] = '\0';
    if (queue_starts == playlist_start_count()
        && queue_commit == tagcache_get_commitid()
        && !strcasecmp(queue_file, current))
        return queue_id;
#ifdef HAVE_DISK_STORAGE
    if (!storage_disk_is_active())
        return 0;
#endif
    uint32_t id = 0;
    for (int i = 0; i < TAGTREE_SLOTS && current[0]; i++)
    {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", TAGTREE_PLAYLIST_DIR,
                 tagtree_playlist_names[i]);
        if (strcasecmp(current, path))
            continue;
        struct tagtree_list_header h;
        int fd = open(tagtree_list_index_names[i], O_RDONLY);
        if (fd >= 0)
        {
            if (read(fd, &h, sizeof(h)) == sizeof(h) && h.magic == TAGTREE_LIST_MAGIC
                && h.commitid == tagcache_get_commitid())
                id = h.id;
            close(fd);
        }
        break;
    }
    tagtree_note_queue(current, id);
    return id;
}

/* The one of a pair, from first, which is free to be written, as a full path
   into buf. Its index goes first, so that it never goes with another file. */
static int tagtree_free_slot(int first, char *buf, size_t size)
{
    int slot = tagtree_slot_in_use(first) ? first + 1 : first;
    snprintf(buf, size, "%s/%s", TAGTREE_PLAYLIST_DIR,
             tagtree_playlist_names[slot]);
    remove(tagtree_list_index_names[slot]);
    return slot;
}

static int tagtree_play_folder(struct tree_context* c);

/* on the screen of an import, which shows how far it is */
static bool list_on_boot_screen;

/* The list last made, for going back to it, from Now Playing or from the menu
   over it, which has rows of its own: kept as long as the lists' buffers, its
   search and the database are as they were. All songs took a second to make
   again, each time. */
static uint32_t kept_hash = 0;
static int kept_count, kept_offset, kept_entry_count, kept_special;
static bool kept_dirfull;
/* the list made had all its rows, as it wasn't left before the end */
static bool entries_whole;
/* and goes by play statistics, so isn't kept: they change without a new
   database */
static bool entries_of_stats;


/* this needs to be same size as struct entry (tree.h) and name needs to be
 * the first; so that they're compatible enough to walk arrays of both
 * derefencing the name member*/
struct tagentry {
    char* name;
    int newtable;
    int extraseek;
    int customaction;
    char* album_name;
};

/* the rows of the menu shown, out of the lists' buffer, which keeps the list */
static struct tagentry root_entries[TAGMENU_MAX_ITEMS];
static int root_rows;

static struct tagentry* tagtree_get_entry(struct tree_context *c, int id);

#define SEARCHSTR_SIZE 256

enum table {
    TABLE_ROOT = 1,
    TABLE_NAVIBROWSE,
    TABLE_ALLSUBENTRIES,
    TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS,
    TABLE_PLAYTRACK,
};

static const struct id3_to_search_mapping {
    char   *string;
    size_t id3_offset;
} id3_to_search_mapping[] = {
    { "",  0 },              /* offset n/a */
    { "#directory#",  0 },   /* offset n/a */
    { "#title#",  offsetof(struct mp3entry, title) },
    { "#artist#", offsetof(struct mp3entry, artist) },
    { "#album#",  offsetof(struct mp3entry, album) },
    { "#genre#",  offsetof(struct mp3entry, genre_string) },
    { "#composer#",  offsetof(struct mp3entry, composer) },
    { "#albumartist#",   offsetof(struct mp3entry, albumartist) },
};
enum variables {
    var_sorttype = 100,
    var_limit,
    var_strip,
    var_menu_start,
    var_include,
    var_rootmenu,
    var_format,
    menu_byfirstletter,
    menu_next,
    menu_load,
    menu_reload,
    menu_shuffle_songs,
    menu_playlists,
    menu_coverflow,
    menu_search,
};

/* Capacity 10 000 entries (for example 10k different artists) */
#define UNIQBUF_SIZE (64*1024)
#define TAGTREE_SORT_MAX 5000
static uint32_t uniqbuf[UNIQBUF_SIZE / sizeof(uint32_t)];

#define MAX_TAGS 5
#define MAX_MENU_ID_SIZE 32

#define RELOAD_TAGTREE (-1024)

static int(*qsort_fn)(const char*, const char*, size_t);
/* dummmy functions to allow compatibility strncasecmp */
static int strnatcasecmp_n(const char *a, const char *b, size_t n)
{
    (void)n;
     return strnatcasecmp(a, b);
}
static int strnatcasecmp_n_inv(const char *a, const char *b, size_t n)
{
    (void)n;
     return strnatcasecmp(b, a);
}
static int strncasecmp_inv(const char *a, const char *b, size_t n)
{
    return strncasecmp(b, a, n);
}

/*
 * "%3d. %s" autoscore title %sort = "inverse" %limit = "100"
 *
 * valid = true
 * formatstr = "%-3d. %s"
 * tags[0] = tag_autoscore
 * tags[1] = tag_title
 * tag_count = 2
 *
 * limit = 100
 * sort_inverse = true
 */
struct display_format {
    char name[32];
    struct tagcache_search_clause *clause[TAGCACHE_MAX_CLAUSES];
    int clause_count;
    char *formatstr;
    int group_id;
    int tags[MAX_TAGS];
    int tag_count;

    int limit;
    int strip;
    bool sort_inverse;
};

static struct display_format *formats[TAGMENU_MAX_FMTS];
static int format_count;

#define MENUENTRY_MAX_NAME 64
struct menu_entry {
    char _name[MENUENTRY_MAX_NAME];
    const unsigned char *name;
    int type;
    struct search_instruction {
        char name[MENUENTRY_MAX_NAME];
        int tagorder[MAX_TAGS];
        int tagorder_count;
        struct tagcache_search_clause *clause[MAX_TAGS][TAGCACHE_MAX_CLAUSES];
        int format_id[MAX_TAGS];
        int clause_count[MAX_TAGS];
        int result_seek[MAX_TAGS];
    } si;
    int link;
};

struct menu_root {
    char _title[MENUENTRY_MAX_NAME];
    const unsigned char *title;
    char id[MAX_MENU_ID_SIZE];
    int itemcount;
    struct menu_entry *items[TAGMENU_MAX_ITEMS];
};

/* Statusbar text of the current view. */
static char current_title[MAX_TAGS][128];

static struct menu_root * menus[TAGMENU_MAX_MENUS];
static struct menu_root * menu;
static struct search_instruction *csi;
static const char *strp;
static int menu_count;
static int rootmenu;

static int current_offset;
static int current_entry_count;

static struct tree_context *tc;

static int max_history_level; /* depth of menu levels with applicable history */
static int selected_item_history[MAX_DIR_LEVELS];
static int table_history[MAX_DIR_LEVELS];
static int extra_history[MAX_DIR_LEVELS];

/* The sections of Music that Settings > Music Menu shows or not, by their
   names in tagnavi.config, in the order of global_settings.music_menu_show.
   One the file has that isn't here is always shown. */
const int tagtree_section_lang[MUSIC_SECTIONS] = {
    LANG_COVER_FLOW, LANG_SEARCH, LANG_PLAYLISTS, LANG_ARTISTS, LANG_ALBUMS,
    LANG_SONGS, LANG_PODCASTS, LANG_GENRES, LANG_COMPOSERS, LANG_AUDIOBOOKS,
};
/* every item of Music's own menu, as when it's gone into past its list */
static bool root_all_items;
/* since the shown ones changed, the rows to go back to are of others */
static bool root_rows_changed;

static bool root_item_shown(const struct menu_entry *item)
{
    const int lang = P2ID((const unsigned char *)item->name);
    for (int i = 0; i < MUSIC_SECTIONS; i++)
    {
        if (tagtree_section_lang[i] == lang)
            return global_settings.music_menu_show[i];
    }
    return true;
}

/* The row of Music's own menu that the item is in, 0 if it isn't shown */
static int root_row_of(int item)
{
    const struct menu_root *root = menus[rootmenu];
    int row = 0;
    if (!root || item >= root->itemcount || !root_item_shown(root->items[item]))
        return 0;
    for (int i = 0; i < item; i++)
        row += root_item_shown(root->items[i]);
    return row;
}

bool tagtree_music_empty(void)
{
    const struct menu_root *root = menus[rootmenu];
    if (!root)
        return false;
    for (int i = 0; i < root->itemcount; i++)
    {
        if (root_item_shown(root->items[i]))
            return false;
    }
    return true;
}

/* a few memory alloc helper */
static int tagtree_handle;
static size_t tagtree_bufsize, tagtree_buf_used;

#define UPDATE(x, y) { x = (typeof(x))((char*)(x) + (y)); }
static int move_callback(int handle, void* current, void* new)
{
    (void)handle; (void)current; (void)new;
    ptrdiff_t diff = new - current;

    if (menu)
    {
        if ((char *) menu->title == (char *) menu->_title)
            UPDATE(menu->title, diff);
        UPDATE(menu, diff);
    }

    if (csi)
        UPDATE(csi, diff);

    /* the menu's rows, by their names here */
    for (int i = 0; i < root_rows; i++)
    {
        char *name = root_entries[i].name;
        if (name >= (char *)current && name < (char *)current + tagtree_bufsize)
            UPDATE(root_entries[i].name, diff);
    }

    /* loop over menus */
    for(int i = 0; i < menu_count; i++)
    {
        struct menu_root* menuroot = menus[i];
        /* then over the menu_entries of a menu */
        for(int j = 0; j < menuroot->itemcount; j++)
        {
            struct menu_entry* mentry = menuroot->items[j];
            /* then over the search_instructions of each menu_entry */
            for(int k = 0; k < mentry->si.tagorder_count; k++)
            {
                for(int l = 0; l < mentry->si.clause_count[k]; l++)
                {
                    if(mentry->si.clause[k][l]->str)
                        UPDATE(mentry->si.clause[k][l]->str, diff);
                    UPDATE(mentry->si.clause[k][l], diff);
                }
            }
            if ((char *) menuroot->items[j]->name == (char *) menuroot->items[j]->_name)
                UPDATE(menuroot->items[j]->name, diff);
            UPDATE(menuroot->items[j], diff);
        }
        if ((char *) menus[i]->title == (char *) menus[i]->_title)
            UPDATE(menus[i]->title, diff);
        UPDATE(menus[i], diff);
    }

    /* now the same game for formats */
    for(int i = 0; i < format_count; i++)
    {
        for(int j = 0; j < formats[i]->clause_count; j++)
        {
            UPDATE(formats[i]->clause[j]->str, diff);
            UPDATE(formats[i]->clause[j], diff);
        }

        if (formats[i]->formatstr)
            UPDATE(formats[i]->formatstr, diff);

        UPDATE(formats[i], diff);
    }
    return BUFLIB_CB_OK;
}
#undef UPDATE

static struct buflib_callbacks ops = {
    .move_callback = move_callback,
    .shrink_callback = NULL,
};

/* by words, many times quicker than crc_32() by nibbles */
static uint32_t words_hash(const void *data, size_t size, uint32_t hash)
{
    const uint32_t *word = data;
    for (size_t n = size / sizeof(*word); n > 0; n--)
        hash = (hash ^ *word++) * 16777619; /* FNV's prime */
    return hash;
}

static uint32_t text_hash(const char *text, uint32_t hash)
{
    for (; text && *text; text++)
        hash = (hash ^ (unsigned char)*text) * 16777619;
    return hash;
}

/* The list in the lists' buffer, which list it is and of which database */
static uint32_t tagtree_data_crc(struct tree_context* c)
{
    const uint32_t list[] = {
        c->currtable, c->currextra, (uintptr_t)csi, tagcache_get_commitid(),
        global_settings.interpret_numbers,
        (uintptr_t)str(LANG_TAGNAVI_UNTAGGED), /* a row's name, of the language */
    };
    uint32_t hash = words_hash(list, sizeof(list), 2166136261u);
    hash = text_hash(str(LANG_TAGNAVI_UNTAGGED), hash);
    /* The search, which can be out of tagtree's buffer (album artists), and its
       clauses down to the list, which get what they look for as it is entered.
       The rest of tagtree's buffer changes only with the menus: with it, this
       took 11 ms on the iPod */
    if (csi)
    {
        hash = words_hash(csi, sizeof(*csi), hash);
        for (int i = 0; i <= c->currextra && i < MAX_TAGS; i++)
        {
            for (int j = 0; j < csi->clause_count[i]; j++)
            {
                const struct tagcache_search_clause *clause = csi->clause[i][j];
                hash = words_hash(clause, sizeof(*clause), hash);
                hash = text_hash(clause->str, hash);
            }
        }
    }
    /* the names and the tagentries, of their start, which anything else using
       them writes first */
    hash = words_hash(core_get_data(c->cache.name_buffer_handle),
                      MIN(c->cache.name_buffer_size, 8*1024), hash);
    hash = words_hash(core_get_data(c->cache.entries_handle),
                      MIN(c->cache.max_entries * sizeof(struct tagentry),
                          8*1024), hash);
    logf("%s 0x%x", __func__, hash);
    return hash ? hash : 1; /* 0 for none kept */
}

/* the list just made kept */
static void keep_list(struct tree_context *c, int count)
{
    if (entries_of_stats)
    {
        kept_hash = 0;
        return;
    }
    kept_hash = tagtree_data_crc(c);
    kept_count = count;
    kept_offset = current_offset;
    kept_entry_count = current_entry_count;
    kept_special = c->special_entry_count;
    kept_dirfull = c->dirfull;
}

/* the list kept, if it is the one asked for: its count, or else -1 */
static int kept_list(struct tree_context *c)
{
    if (!kept_hash || kept_hash != tagtree_data_crc(c))
        return -1;
    current_offset = kept_offset;
    current_entry_count = kept_entry_count;
    c->special_entry_count = kept_special;
    c->dirfull = kept_dirfull;
    return kept_count;
}

static void* tagtree_alloc(size_t size)
{
    size = ALIGN_UP(size, sizeof(void*));
    if (size > (tagtree_bufsize - tagtree_buf_used))
        return NULL;

    char* buf = core_get_data(tagtree_handle) + tagtree_buf_used;

    tagtree_buf_used += size;
    return buf;
}

static void* tagtree_alloc0(size_t size)
{
    void* ret = tagtree_alloc(size);
    if (ret)
        memset(ret, 0, size);
    return ret;
}

static char* tagtree_strdup(const char* buf)
{
    size_t len = strlen(buf) + 1;
    char* dest = tagtree_alloc(len);
    if (dest)
        strcpy(dest, buf);
    return dest;
}

/* save to call without locking */
static int get_token_str(char *buf, int size)
{
    /* Find the start. */
    while (*strp != '"' && *strp != '\0')
        strp++;

    if (*strp == '\0' || *(++strp) == '\0')
        return -1;

    /* Read the data. */
    while (*strp != '"' && *strp != '\0' && --size > 0)
        *(buf++) = *(strp++);

    *buf = '\0';
    if (*strp != '"')
        return -2;

    strp++;

    return 0;
}

static int get_tag(int *tag)
{
    /* case insensitive matching ahead - these should all be lower case */
#define TAG_TABLE \
        TAG_MATCH("lm", tag_virt_length_min) \
        TAG_MATCH("ls", tag_virt_length_sec) \
        TAG_MATCH("pm", tag_virt_playtime_min) \
        TAG_MATCH("ps", tag_virt_playtime_sec) \
        TAG_MATCH("->", menu_next) \
        TAG_MATCH("~>", menu_shuffle_songs) \
        TAG_MATCH("==>", menu_load) \
        TAG_MATCH("year", tag_year) \
        TAG_MATCH("kind", tag_virt_kind) \
        TAG_MATCH("album", tag_album) \
        TAG_MATCH("genre", tag_genre) \
        TAG_MATCH("title", tag_title) \
        TAG_MATCH("%sort", var_sorttype) \
        TAG_MATCH("artist", tag_artist) \
        TAG_MATCH("length", tag_length) \
        TAG_MATCH("rating", tag_rating) \
        TAG_MATCH("%limit", var_limit) \
        TAG_MATCH("%strip", var_strip) \
        TAG_MATCH("%search", menu_search) \
        TAG_MATCH("bitrate", tag_bitrate) \
        TAG_MATCH("comment", tag_comment) \
        TAG_MATCH("discnum", tag_discnumber) \
        TAG_MATCH("%format", var_format) \
        TAG_MATCH("%reload", menu_reload) \
        TAG_MATCH("filename", tag_filename) \
        TAG_MATCH("basename", tag_virt_basename) \
        TAG_MATCH("tracknum", tag_tracknumber) \
        TAG_MATCH("composer", tag_composer) \
        TAG_MATCH("ensemble", tag_albumartist) \
        TAG_MATCH("grouping", tag_grouping) \
        TAG_MATCH("entryage", tag_virt_entryage) \
        TAG_MATCH("commitid", tag_commitid) \
        TAG_MATCH("%include", var_include) \
        TAG_MATCH("playcount", tag_playcount) \
        TAG_MATCH("autoscore", tag_virt_autoscore) \
        TAG_MATCH("lastplayed", tag_lastplayed) \
        TAG_MATCH("lastoffset", tag_lastoffset) \
        TAG_MATCH("%root_menu", var_rootmenu) \
        TAG_MATCH("%playlists", menu_playlists) \
        TAG_MATCH("%coverflow", menu_coverflow) \
        TAG_MATCH("albumartist", tag_albumartist) \
        TAG_MATCH("lastelapsed", tag_lastelapsed) \
        TAG_MATCH("%menu_start", var_menu_start) \
        TAG_MATCH("%byfirstletter", menu_byfirstletter) \
        TAG_MATCH("canonicalartist", tag_virt_canonicalartist) \
    /* END OF TAG_TABLE MACRO */

    /* build two separate arrays tag strings and symbol map*/
    #define TAG_MATCH(str, tag) str,
    static const char * const get_tag_match[] =
    {
        TAG_TABLE
    };
    #undef TAG_MATCH
    #define TAG_MATCH(str, tag) tag,
    static const uint8_t get_tag_symbol[]=
    {
        TAG_TABLE
    };
    #undef TAG_MATCH

    const char *tagstr;
    ptrdiff_t tagstr_len;

    /* Find the start. */
    while (*strp == ' ' || *strp == '>')
        strp++;

    if (*strp == '\0' || *strp == '?')
        return 0;

    tagstr = strp;
    while(*strp != '\0' && *strp != ' ') /* walk to the end of the tag */
    {
        strp++;
    }
    tagstr_len = strp - tagstr;

    char first = tolower(*tagstr++); /* get the first letter elide cmp fn call */

    for (size_t i = 0; i < ARRAYLEN(get_tag_match); i++)
    {
        const char *match = get_tag_match[i];

        if (first == match[0] && strncasecmp(tagstr, match + 1, tagstr_len - 1) == 0)
        {
            /* check for full match */
            if (match[tagstr_len] == '\0')
            {
                *tag = get_tag_symbol[i];
                return 1;
            }
        }
    }
    logf("NO MATCH: %.*s\n", (int)tagstr_len, tagstr);

    return -1;
}

static int get_clause(int *condition)
{
    /* one or two operator conditionals */
    #define OPS2VAL(op1, op2) ((uint16_t)op1 << 8 | (uint16_t)op2)
    #define CLAUSE(op1, op2, symbol) {OPS2VAL(op1, op2), symbol }

    struct clause_symbol {uint16_t value;uint16_t symbol;};
    const struct clause_symbol *match;
    static const struct clause_symbol get_clause_match[] =
    {
        CLAUSE('=', ' ', clause_is),
        CLAUSE('=', '=', clause_is),
        CLAUSE('!', '=', clause_is_not),
        CLAUSE('>', ' ', clause_gt),
        CLAUSE('>', '=', clause_gteq),
        CLAUSE('<', ' ', clause_lt),
        CLAUSE('<', '=', clause_lteq),
        CLAUSE('~', ' ', clause_contains),
        CLAUSE('!', '~', clause_not_contains),
        CLAUSE('^', ' ', clause_begins_with),
        CLAUSE('!', '^', clause_not_begins_with),
        CLAUSE('$', ' ', clause_ends_with),
        CLAUSE('!', '$', clause_not_ends_with),
        CLAUSE('@', '~', clause_contains_oneof),
        CLAUSE('@', '^', clause_begins_oneof),
        CLAUSE('@', '$', clause_ends_oneof),
        CLAUSE('@', ' ', clause_oneof),
        CLAUSE('*', '~', clause_not_contains_oneof),
        CLAUSE('*', '^', clause_not_begins_oneof),
        CLAUSE('*', '$', clause_not_ends_oneof),
        CLAUSE('!', '@', clause_not_oneof),
        CLAUSE(0, 0, 0) /* sentinel */
    };

    /* Find the start. */
    while (*strp == ' ' && *strp != '\0')
        strp++;

    if (*strp == '\0')
        return 0;

    char op1 = strp[0];
    char op2 = strp[1];
    if (op2 == '"') /*allow " to end a single op conditional */
        op2 = ' ';

    uint16_t value = OPS2VAL(op1, op2);

    for (match = get_clause_match; match->value != 0; match++)
    {
        if (value == match->value)
        {
            *condition = match->symbol;
            return 1;
        }
    }

    return 0;
#undef OPS2VAL
#undef CLAUSE
}

static bool read_clause(struct tagcache_search_clause *clause)
{
    char buf[SEARCHSTR_SIZE];
    unsigned int i;

    if (get_tag(&clause->tag) <= 0)
        return false;

    if (get_clause(&clause->type) <= 0)
        return false;

    if (get_token_str(buf, sizeof buf) < 0)
        return false;

    for (i=0; i<ARRAYLEN(id3_to_search_mapping); i++)
    {
        if (!strcasecmp(buf, id3_to_search_mapping[i].string))
            break;
    }

    if (i<ARRAYLEN(id3_to_search_mapping)) /* runtime search operand found */
    {
        clause->source = source_runtime+i;
        clause->str = tagtree_alloc(SEARCHSTR_SIZE);
    }
    else
    {
        clause->source = source_constant;
        clause->str = tagtree_strdup(buf);
    }

    if (!clause->str)
    {
        logf("tagtree failed to allocate %s", "clause string");
        return false;
    }
    else if (TAGCACHE_IS_NUMERIC(clause->tag))
    {
        clause->numeric = true;
        clause->numeric_data = atoi(clause->str);
    }
    else
        clause->numeric = false;

    logf("got clause: %d/%d [%s]", clause->tag, clause->type, clause->str);

    return true;
}

static bool read_variable(char *buf, int size)
{
    int condition;

    if (!get_clause(&condition))
        return false;

    if (condition != clause_is)
        return false;

    if (get_token_str(buf, size) < 0)
        return false;

    return true;
}

/* "%3d. %s" autoscore title %sort = "inverse" %limit = "100" */
static int get_format_str(struct display_format *fmt)
{
    int ret;
    char buf[128];
    int i;

    memset(fmt, 0, sizeof(struct display_format));

    if (get_token_str(fmt->name, sizeof fmt->name) < 0)
        return -10;

    /* Determine the group id */
    fmt->group_id = 0;
    for (i = 0; i < format_count; i++)
    {
        if (!strcasecmp(formats[i]->name, fmt->name))
        {
            fmt->group_id = formats[i]->group_id;
            break;
        }

        if (formats[i]->group_id > fmt->group_id)
            fmt->group_id = formats[i]->group_id;
    }

    if (i == format_count)
        fmt->group_id++;

    logf("format: (%d) %s", fmt->group_id, fmt->name);

    if (get_token_str(buf, sizeof buf) < 0)
        return -10;

    fmt->formatstr = tagtree_strdup(buf);

    while (fmt->tag_count < MAX_TAGS)
    {
        ret = get_tag(&fmt->tags[fmt->tag_count]);
        if (ret < 0)
            return -11;

        if (ret == 0)
            break;

        switch (fmt->tags[fmt->tag_count]) {
        case var_sorttype:
            if (!read_variable(buf, sizeof buf))
                return -12;
            if (!strcasecmp("inverse", buf))
                fmt->sort_inverse = true;
            break;

        case var_limit:
            if (!read_variable(buf, sizeof buf))
                return -13;
            fmt->limit = atoi(buf);
            break;

        case var_strip:
            if (!read_variable(buf, sizeof buf))
                return -14;
            fmt->strip = atoi(buf);
            break;

        default:
            fmt->tag_count++;
        }
    }

    return 1;
}

static int add_format(const char *buf)
{
    if (format_count >= TAGMENU_MAX_FMTS)
    {
        logf("too many formats");
        return -1;
    }

    strp = buf;

    if (formats[format_count] == NULL)
        formats[format_count] = tagtree_alloc0(sizeof(struct display_format));
    if (!formats[format_count])
    {
        logf("tagtree failed to allocate %s", "format string");
        return -2;
    }
    if (get_format_str(formats[format_count]) < 0)
    {
        logf("get_format_str() parser failed!");
        if (formats[format_count])
            memset(formats[format_count], 0, sizeof(struct display_format));
        return -4;
    }

    while (*strp != '\0' && *strp != '?')
        strp++;

    if (*strp == '?')
    {
        int clause_count = 0;
        strp++;

        core_pin(tagtree_handle);
        while (1)
        {
            struct tagcache_search_clause *new_clause;

            if (clause_count >= TAGCACHE_MAX_CLAUSES)
            {
                logf("too many clauses");
                break;
            }

            new_clause = tagtree_alloc(sizeof(struct tagcache_search_clause));
            if (!new_clause)
            {
                logf("tagtree failed to allocate %s", "search clause");
                return -3;
            }
            formats[format_count]->clause[clause_count] = new_clause;
            if (!read_clause(new_clause))
                break;

            clause_count++;
        }
        core_unpin(tagtree_handle);

        formats[format_count]->clause_count = clause_count;
    }

    format_count++;

    return 1;
}

static int get_condition(struct search_instruction *inst)
{
    struct tagcache_search_clause *new_clause;
    int clause_count;
    char buf[128];

    switch (*strp)
    {
        case '=':
        {
            int i;

            if (get_token_str(buf, sizeof buf) < 0)
                return -1;

            for (i = 0; i < format_count; i++)
            {
                if (!strcasecmp(formats[i]->name, buf))
                    break;
            }

            if (i == format_count)
            {
                logf("format not found: %s", buf);
                return -2;
            }

            inst->format_id[inst->tagorder_count] = formats[i]->group_id;
            return 1;
        }
        case '?':
        case ' ':
        case '&':
            strp++;
            return 1;
        case '-':
        case '\0':
            return 0;
    }

    clause_count = inst->clause_count[inst->tagorder_count];
    if (clause_count >= TAGCACHE_MAX_CLAUSES)
    {
        logf("Too many clauses");
        return -2;
    }

    new_clause = tagtree_alloc0(sizeof(struct tagcache_search_clause));
    if (!new_clause)
    {
        logf("tagtree failed to allocate %s", "search clause");
        return -3;
    }

    inst->clause[inst->tagorder_count][clause_count] = new_clause;

    if (*strp == '|')
    {
        strp++;
        new_clause->type = clause_logical_or;
    }
    else
    {
        core_pin(tagtree_handle);
        bool ret = read_clause(new_clause);
        core_unpin(tagtree_handle);
        if (!ret)
            return -1;
    }
    inst->clause_count[inst->tagorder_count]++;

    return 1;
}

/* example search:
 * "Best" artist ? year >= "2000" & title !^ "crap" & genre = "good genre" \
 *      : album  ? year >= "2000" : songs
 * ^  begins with
 * *  contains
 * $  ends with
 */

static bool parse_search(struct menu_entry *entry, const char *str)
{
    int ret;
    int type;
    struct search_instruction *inst = &entry->si;
    char buf[MAX_PATH];
    int i;

    strp = str;

    /* Parse entry name */
    if (get_token_str(entry->_name, sizeof entry->_name) < 0)
    {
        logf("No name found.");
        return false;
    }

    /* Attempt to entry name to lang_id for voicing/translation
    (excepted for single character entries like those in the 'First Letter' menus)
    */
    if (entry->_name[0] != '\0' && entry->_name[1] != '\0')
    {
        int lang_id = lang_english_to_id(entry->_name);
        if (lang_id >= 0)
            entry->name = ID2P(lang_id);
        else
            entry->name = entry->_name;
    }
    else
        entry->name = entry->_name;

    /* Parse entry type */
    if (get_tag(&entry->type) <= 0)
        return false;

    if (entry->type == menu_load)
    {
        if (get_token_str(buf, sizeof buf) < 0)
            return false;

        /* Find the matching root menu or "create" it */
        for (i = 0; i < menu_count; i++)
        {
            if (!strcasecmp(menus[i]->id, buf))
            {
                entry->link = i;
                return true;
            }
        }

        if (menu_count >= TAGMENU_MAX_MENUS)
        {
            logf("max menucount reached");
            return false;
        }

        /* Allocate a new menu unless link is found. */
        menus[menu_count] = tagtree_alloc0(sizeof(struct menu_root));
        if (!menus[menu_count])
        {
            logf("tagtree failed to allocate %s", "menu");
            return false;
        }
        strmemccpy(menus[menu_count]->id, buf, MAX_MENU_ID_SIZE);
        entry->link = menu_count;
        ++menu_count;

        return true;
    }

    /* the playlist catalogue, as Playlists in the Music menu of a stock iPod,
       Cover Flow, as the first item of an iPod Classic's, and Search */
    if (entry->type == menu_playlists || entry->type == menu_coverflow
        || entry->type == menu_search)
        return true;

    if (entry->type != menu_next && entry->type != menu_shuffle_songs)
        return false;

    while (inst->tagorder_count < MAX_TAGS)
    {
        ret = get_tag(&inst->tagorder[inst->tagorder_count]);
        if (ret < 0)
        {
            logf("Parse error #1");
            logf("%s", strp);
            return false;
        }

        if (ret == 0)
            break ;

        logf("tag: %d", inst->tagorder[inst->tagorder_count]);

        core_pin(tagtree_handle);
        while ( (ret = get_condition(inst)) > 0 ) ;
        core_unpin(tagtree_handle);

        if (ret < 0)
            return false;

        inst->tagorder_count++;

        if (get_tag(&type) <= 0 || (type != menu_next && type != menu_shuffle_songs))
            break;
    }

    return true;
}

static int compare(const void *p1, const void *p2)
{
    struct tagentry *e1 = (struct tagentry *)p1;
    struct tagentry *e2 = (struct tagentry *)p2;
    return qsort_fn(e1->name, e2->name, MAX_PATH);
}

static int compare_with_albums(const void *p1, const void *p2)
{
    struct tagentry *e1 = (struct tagentry *)p1;
    struct tagentry *e2 = (struct tagentry *)p2;
    int sort_album_res = qsort_fn(
        e1->album_name == NULL ? "" : e1->album_name,
        e2->album_name == NULL ? "" : e2->album_name, MAX_PATH);
    if (sort_album_res != 0)
    {
        /* If album name is different */
        return sort_album_res;
    }
    return qsort_fn(e1->name, e2->name, MAX_PATH);
}

static void tagtree_buffer_event(unsigned short id, void *ev_data)
{
    (void)id;
    struct tagcache_search tcs;
    struct mp3entry *id3 = ((struct track_event *)ev_data)->id3;

    bool runtimedb = global_settings.runtimedb;
    bool autoresume = itunesdb_resumes(id3->path);

    /* Its number in the database, which the speaker in the lists and the
       rating go by, all the same; the data only if it's gathered */

    logf("be:%s", id3->path);

    while (! tagcache_is_fully_initialized())
        yield();

    if (!tagcache_find_index(&tcs, id3->path))
    {
        logf("tc stat: not found: %s", id3->path);
        return;
    }

    if (runtimedb)
    {
        id3->playcount  = tagcache_get_numeric(&tcs, tag_playcount);
        if (!id3->rating)
            id3->rating = tagcache_get_numeric(&tcs, tag_rating);
        id3->lastplayed = tagcache_get_numeric(&tcs, tag_lastplayed);
        id3->score      = tagcache_get_numeric(&tcs, tag_virt_autoscore) / 10;
        id3->playtime   = tagcache_get_numeric(&tcs, tag_playtime);

        logf("-> %ld/%ld", id3->playcount, id3->playtime);
    }

    if (autoresume)
    {
        /* Load current file resume info if not already defined (by
           another resume mechanism) */
        if (id3->elapsed == 0)
        {
            id3->elapsed = tagcache_get_numeric(&tcs, tag_lastelapsed);

            logf("tagtree_buffer_event: Set elapsed for %s to %lX\n",
                 str_or_empty(id3->title), id3->elapsed);
        }

        if (id3->offset == 0)
        {
            id3->offset = tagcache_get_numeric(&tcs, tag_lastoffset);

            logf("tagtree_buffer_event: Set offset for %s to %lX\n",
                 str_or_empty(id3->title), id3->offset);
        }
    }

    /* Store our tagcache index pointer. */
    id3->tagcache_idx = tcs.idx_id+1;

    tagcache_search_finish(&tcs);
}

/* Its numbers again as it plays: a song come back to while it was still in
   the buffer had them as when it was buffered, the stars just given it gone
   and its next play counted on from the plays before */
static void tagtree_track_change_event(unsigned short id, void *ev_data)
{
    (void)id;
    struct mp3entry *id3 = ((struct track_event *)ev_data)->id3;
    struct tagcache_numbers n;
    if (!id3->tagcache_idx || !global_settings.runtimedb
        || !tagcache_get_numbers(id3->tagcache_idx - 1, &n))
        return;
    id3->playcount = n.playcount;
    id3->playtime = n.playtime;
    id3->lastplayed = n.lastplayed;
    /* the database's alone: no file's metadata gives one */
    id3->rating = n.rating;
}

static void tagtree_track_finish_event(unsigned short id, void *ev_data)
{
    (void)id;
    struct track_event *te = (struct track_event *)ev_data;
    struct mp3entry *id3 = te->id3;

    long tagcache_idx = id3->tagcache_idx;
    if (!tagcache_idx)
    {
        logf("No tagcache index pointer found");
        return;
    }
    tagcache_idx--;

    bool auto_skip = te->flags & TEF_AUTO_SKIP;
    bool runtimedb = global_settings.runtimedb;
    bool autoresume = itunesdb_resumes(id3->path);

    /* A play is a minute of a track, or all of a shorter one: neither
       skipping on after a few seconds nor only a full play to the end.
       What plays from where it was left is played when it ends, as the
       place says nothing of how long it was listened to. The autoresume
       point is always processed. */
    if (runtimedb && (id3->elapsed == 0
        || (id3->elapsed < 60 * 1000 && !auto_skip)
        || (autoresume && !auto_skip)
        ))
    {
        logf("not db logging unplayed or skipped track");
        runtimedb = false;
    }

    /* 3s because that is the threshold the WPS uses to rewind instead
       of skip backwards */
    if (autoresume && (id3->elapsed == 0
        || (id3->elapsed < 3 * 1000 && !auto_skip)))
    {
        logf("not logging autoresume");
        autoresume = false;
    }

    /* Do not gather data unless proper setting has been enabled and at least
       one is still slated to be recorded */
    if (!(runtimedb || autoresume))
    {
        logf("runtimedb gathering and autoresume not enabled/ignored");
        return;
    }

    long lastplayed = tagcache_increase_serial();
    if (lastplayed < 0)
    {
        logf("incorrect tc serial:%ld", lastplayed);
        return;
    }
#if CONFIG_RTC
    /* The time rather than the serial number, for Play Counts to tell
       iTunes when; later plays still have the larger numbers */
    long now = mktime(get_time());
    if (now > lastplayed)
        lastplayed = now;
#endif

    if (runtimedb)
    {
        long playcount;
        long playtime;

        /* on from the database's, as the track's may be of when it was
           buffered, played again from the buffer */
        struct tagcache_numbers n;
        const bool fresh = tagcache_get_numbers(tagcache_idx, &n);
        playcount = (fresh ? n.playcount : id3->playcount) + 1;

        /* Ignore the last 15s (crossfade etc.) */
        playtime = (fresh ? n.playtime : id3->playtime)
                   + MIN(id3->length, id3->elapsed + 15 * 1000);

        logf("ube:%s", id3->path);
        logf("-> %ld/%ld", playcount, playtime);
        logf("-> %ld/%ld/%ld", id3->elapsed, id3->length,
             MIN(id3->length, id3->elapsed + 15 * 1000));

        /* Queue the updates to the tagcache system. */
        tagcache_update_numeric(tagcache_idx, tag_playcount, playcount);
        tagcache_update_numeric(tagcache_idx, tag_playtime, playtime);
        tagcache_update_numeric(tagcache_idx, tag_lastplayed, lastplayed);
    }

    if (autoresume)
    {
        unsigned long elapsed = auto_skip ? 0 : id3->elapsed;
        unsigned long offset = auto_skip ? 0 : id3->offset;
        tagcache_update_numeric(tagcache_idx, tag_lastelapsed, elapsed);
        tagcache_update_numeric(tagcache_idx, tag_lastoffset, offset);

        logf("tagtree_track_finish_event: Save resume for %s: %lX %lX",
             str_or_empty(id3->title), elapsed, offset);
    }
}

int tagtree_export(void)
{
    struct tagcache_search tcs;

    splash(0, ID2P(LANG_CREATING));
    if (!tagcache_create_changelog(&tcs))
    {
        splash(HZ*2, ID2P(LANG_FAILED));
    }

    return 0;
}

int tagtree_import(void)
{
    splash(0, ID2P(LANG_WAIT));
    if (!tagcache_import_changelog())
    {
        splash(HZ*2, ID2P(LANG_FAILED));
    }

    return 0;
}

static bool alloc_menu_parse_buf(char *buf, int type)
{
    /* allocate a new menu item (if needed) initialize it with data parsed
       from buf Note: allows setting menu type, type ignored when < 0
    */
    /* Allocate */
    if (menu->items[menu->itemcount] == NULL)
        menu->items[menu->itemcount] = tagtree_alloc0(sizeof(struct menu_entry));
    if (!menu->items[menu->itemcount])
    {
        logf("tagtree failed to allocate %s", "menu items");
        return false;
    }

    /* Initialize */
    core_pin(tagtree_handle);
    if (parse_search(menu->items[menu->itemcount], buf))
    {
        if (type >= 0)
            menu->items[menu->itemcount]->type = type;
        menu->itemcount++;
    }
    core_unpin(tagtree_handle);
    return true;
}

static void build_firstletter_menu(char *buf, size_t bufsz)
{
#if 0 /* GCC complains about this I can't find a definitive answer */
    const char *subitem = buf;
    size_t l = strlen(buf) + 1;
    buf+=l;
    bufsz-=l;
#else
    char subitem[32]; /* canonicalartist longest subitem we expect add a bit extra..*/
    strmemccpy(subitem, buf, sizeof(subitem));
#endif

    const char * const fmt ="\"%s\"-> %s ? %s %c\"%c\"-> %s =\"fmt_title\"";
    const char * const showsub = /* album subitem for canonicalartist */
        ((strcasestr(subitem, "artist") == NULL) ? "title" : "album -> title");

    /* Using >= "0" & <= "9" does not work as intended in all cases
    we need to use the previous & the next character in the ASCII table
    which are "/" and ":" to get the intended effect */
    const char * fmt_numeric ="\"%s\"-> %s ? %s > \"/\" & %s < \":\" -> %s =\"fmt_title\"";
    snprintf(buf, bufsz, fmt_numeric,
            str(LANG_DISPLAY_NUMERIC), subitem, subitem, subitem, showsub);

    if (!alloc_menu_parse_buf(buf, menu_byfirstletter))
    {
        return;
    }

    for (int i = 0; i < 26; i++)
    {
        snprintf(buf, bufsz, fmt, "#", subitem, subitem,'^', 'A' + i, showsub);
        buf[1] = 'A' + i; /* overwrite the placeholder # with the current letter */
        /* ex: "A" -> title ? title ^ "A" -> title = "fmt_title" */
        if (!alloc_menu_parse_buf(buf, menu_byfirstletter))
        {
            return;
        }
    }

    /* use lower character there because strncasecmp works will convert them anyway */
    const char * fmt_special ="\"%s\"-> %s ? %s *^ "\
    "\"a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q|r|s|t|u|v|w|x|y|z|0|1|2|3|4|5|6|7|8|9\" -> %s =\"fmt_title\"";
    snprintf(buf, bufsz, fmt_special,
        str(LANG_DISPLAY_SPECIAL_CHARACTER), subitem, subitem, showsub);

    if (!alloc_menu_parse_buf(buf, menu_byfirstletter))
    {
        return;
    }
}

static bool parse_menu(const char *filename);
static int parse_line(int n, char *buf, void *parameters)
{
    char data[256];
    int variable;
    static bool read_menu;
    int i;
    char *p;

    (void)parameters;

    /* Strip possible <CR> at end of line. */
    p = strchr(buf, '\r');
    if (p != NULL)
        *p = '\0';

    logf("parse:%d/%s", n, buf);

    /* First line, do initialisation. */
    if (n == 0)
    {
        if (strcasecmp(TAGNAVI_VERSION, buf))
        {
            logf("Version mismatch");
            return -1;
        }

        read_menu = false;
    }

    if (buf[0] == '#')
        return 0;

    if (buf[0] == '\0')
    {
        if (read_menu)
        {
            /* End the menu */
            read_menu = false;
        }
        return 0;
    }

    if (!read_menu)
    {
        strp = buf;
        if (get_tag(&variable) <= 0)
            return 0;

        switch (variable)
        {
            case var_format:
                if (add_format(strp) < 0)
                {
                    logf("Format add fail: %s", data);
                }
                break;

            case var_include:
                if (get_token_str(data, sizeof(data)) < 0)
                {
                    logf("%%include empty");
                    return 0;
                }

                if (!parse_menu(data))
                {
                    logf("Load menu fail: %s", data);
                }
                break;
            case menu_byfirstletter: /* Fallthrough */
            case var_menu_start:
                if (menu_count >= TAGMENU_MAX_MENUS)
                {
                    logf("max menucount reached");
                    return 0;
                }

                if (get_token_str(data, sizeof data) < 0)
                {
                    logf("%%menu_start id empty");
                    return 0;
                }

                menu = NULL;
                for (i = 0; i < menu_count; i++)
                {
                    if (!strcasecmp(menus[i]->id, data))
                    {
                        menu = menus[i];
                    }
                }

                if (menu == NULL)
                {
                    menus[menu_count] = tagtree_alloc0(sizeof(struct menu_root));
                    if (!menus[menu_count])
                    {
                        logf("tagtree failed to allocate %s", "menu");
                        return -2;
                    }
                    menu = menus[menu_count];
                    ++menu_count;
                    strmemccpy(menu->id, data, MAX_MENU_ID_SIZE);
                }

                if (get_token_str(menu->_title, sizeof(menu->_title)) < 0)
                {
                    logf("%%menu_start title empty");
                    return 0;
                }

                /* Attempt to match title to lang_id for voicing/translation */
                int lang_id = lang_english_to_id(menu->_title);
                if (lang_id >= 0)
                    menu->title = ID2P(lang_id);
                else
                    menu->title = menu->_title;

                logf("menu: %s id: %ld", P2STR(menu->title), P2ID(menu->title));

                if (variable == menu_byfirstletter)
                {
                    if (get_token_str(data, sizeof(data)) < 0)
                    {
                        logf("%%firstletter_menu has no subitem"); /*artist,album*/
                        return 0;
                    }
                    logf("A-Z Menu subitem: %s", data);
                    read_menu = false;
                    build_firstletter_menu(data, sizeof(data));
                    break;
                }
                read_menu = true;
                break;

            case var_rootmenu:
                /* Only set root menu once. */
                if (rootmenu >= 0)
                    break;

                if (get_token_str(data, sizeof(data)) < 0)
                {
                    logf("%%rootmenu empty");
                    return 0;
                }

                for (i = 0; i < menu_count; i++)
                {
                    if (!strcasecmp(menus[i]->id, data))
                    {
                        rootmenu = i;
                    }
                }
                break;
        }

        return 0;
    }

    if (menu->itemcount >= TAGMENU_MAX_ITEMS)
    {
        logf("max itemcount reached");
        return 0;
    }

    if (!alloc_menu_parse_buf(buf, -1))
    {
        return -2;
    }

    return 0;
}

static bool parse_menu(const char *filename)
{
    int fd;
    char buf[1024];
    int rc;

    if (menu_count >= TAGMENU_MAX_MENUS)
    {
        logf("max menucount reached");
        return false;
    }

    fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        logf("Search instruction file not found.");
        return false;
    }

    /* Now read file for real, parsing into si */
    rc = fast_readline(fd, buf, sizeof buf, NULL, parse_line);
    close(fd);

    return (rc >= 0);
}

static void tagtree_unload(struct tree_context *c)
{
    /* may be spurious... */
    core_pin(tagtree_handle);
    /* the menu's rows and the list kept go with the menus */
    root_rows = 0;
    kept_hash = 0;

    remove_event(PLAYBACK_EVENT_TRACK_BUFFER, tagtree_buffer_event);
    remove_event(PLAYBACK_EVENT_TRACK_CHANGE, tagtree_track_change_event);
    remove_event(PLAYBACK_EVENT_TRACK_FINISH, tagtree_track_finish_event);

    if (c)
    {
        tree_lock_cache(c);
        struct tagentry *dptr = root_entries;
        menu = menus[c->currextra];
        if (!menu)
        {
            logf("tagtree menu doesn't exist");
            return;
        }

        for (int i = 0; i < menu->itemcount; i++)
        {
            dptr->name = NULL;
            dptr->newtable = 0;
            dptr->extraseek = 0;
            dptr->customaction = ONPLAY_NO_CUSTOMACTION;
            dptr++;
        }
    }

    for (int i = 0; i < menu_count; i++)
        menus[i] = NULL;
    menu_count = 0;

    for (int i = 0; i < format_count; i++)
        formats[i] = NULL;
    format_count = 0;

    core_free(tagtree_handle);
    tagtree_handle   = 0;
    tagtree_buf_used = 0;
    tagtree_bufsize  = 0;

    if (c)
        tree_unlock_cache(c);
}

static bool initialize_tagtree(void) /* also used when user selects 'Reload' in 'custom menu'*/
{
    max_history_level = 0;
    format_count = 0;
    menu_count = 0;
    menu = NULL;
    rootmenu = -1;
    tagtree_handle = core_alloc_maximum(&tagtree_bufsize, &ops);
    if (tagtree_handle < 0)
        panicf("tagtree OOM");

    /* Use the user tagnavi config if present, otherwise use the default. */
    const char* tagnavi_file;
    if(file_exists(TAGNAVI_USER_CONFIG))
        tagnavi_file = TAGNAVI_USER_CONFIG;
    else
        tagnavi_file = TAGNAVI_DEFAULT_CONFIG;

    if (!parse_menu(tagnavi_file))
    {
        tagtree_unload(NULL);
        return false;
    }

    /* safety check since tree.c needs to cast tagentry to entry */
    if (sizeof(struct tagentry) != sizeof(struct entry))
        panicf("tagentry(%zu) and entry mismatch(%zu)",
                sizeof(struct tagentry), sizeof(struct entry));

    /* If no root menu is set, assume it's the first single menu
     * we have. That shouldn't normally happen. */
    if (rootmenu < 0)
        rootmenu = 0;

    add_event(PLAYBACK_EVENT_TRACK_BUFFER, tagtree_buffer_event);
    add_event(PLAYBACK_EVENT_TRACK_CHANGE, tagtree_track_change_event);
    add_event(PLAYBACK_EVENT_TRACK_FINISH, tagtree_track_finish_event);

    core_shrink(tagtree_handle, NULL, tagtree_buf_used);
    return true;
}

static bool tagnavi_loaded;

void tagtree_init(void)
{
    tagnavi_loaded = initialize_tagtree();
}

bool tagtree_ready(void)
{
    return tagnavi_loaded;
}

static int format_str(struct tagcache_search *tcs, struct display_format *fmt,
                      char *buf, int buf_size)
{
    static char fmtbuf[20];
    bool read_format = false;
    unsigned fmtbuf_pos = 0;
    int parpos = 0;
    int buf_pos = 0;
    int i;

    /* memset(buf, 0, buf_size); probably uneeded */
    for (i = 0; fmt->formatstr[i] != '\0'; i++)
    {
        if (fmt->formatstr[i] == '%')
        {
            read_format = true;
            fmtbuf_pos = 0;
            if (parpos >= fmt->tag_count)
            {
                logf("too many format tags");
                return -1;
            }
        }

        char formatchar = fmt->formatstr[i];

        if (read_format)
        {
            fmtbuf[fmtbuf_pos++] = formatchar;
            if (fmtbuf_pos >= sizeof fmtbuf)
            {
                logf("format parse error");
                return -2;
            }

            if (formatchar == 's' || formatchar == 'd')
            {
                unsigned space_left = buf_size - buf_pos;
                char tmpbuf[MAX_PATH];
                char *result;

                fmtbuf[fmtbuf_pos] = '\0';
                read_format = false;

                switch (formatchar)
                {
                case 's':
                    if (fmt->tags[parpos] == tcs->type)
                    {
                        result = tcs->result;
                    }
                    else
                    {
                        /* Need to fetch the tag data. */
                        int tag = fmt->tags[parpos];

                        if (!tagcache_retrieve(tcs, tcs->idx_id,
                                               tag, tmpbuf, sizeof tmpbuf))
                        {
                            logf("retrieve failed");
                            return -3;
                        }

                        result = tmpbuf;
                    }
                    buf_pos +=
                        snprintf(&buf[buf_pos], space_left, fmtbuf, result);
                    break;

                case 'd':
                    buf_pos +=
                        snprintf(&buf[buf_pos], space_left, fmtbuf,
                                 tagcache_get_numeric(tcs, fmt->tags[parpos]));
                }

                parpos++;
            }
        }
        else
            buf[buf_pos++] = formatchar;

        if (buf_pos >= buf_size - 1)    /* need at least one more byte for \0 */
        {
            logf("buffer overflow");
            return -4;
        }
    }

    buf[buf_pos++] = '\0';

    return 0;
}

static struct tagentry* get_entries(struct tree_context *tc)
{
    return core_get_data(tc->cache.entries_handle);
}

static void tcs_get_basename(struct tagcache_search *tcs, bool is_basename)
{
    if (is_basename)
    {
        char* basename = strrchr(tcs->result, '/');
        if (basename != NULL)
        {
            tcs->result = basename + 1;
            tcs->result_len = strlen(tcs->result) + 1;
        }
    }
}

/* false when left by the user */
static bool tagtree_search_progress(int count)
{
    if (list_on_boot_screen)
    {
        library_update_songs_progress(0, 0);
        return !action_userabort(TIMEOUT_NOBLOCK);
    }
    return show_search_progress(false, count, 0, 0);
}

/* The group of formats of the list's rows */
static int format_group(struct tree_context *c, int level)
{
    /* sorted by albums, with the view that has the disc number etc */
    return c->currtable == TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS
        ? csi->format_id[level + 1] : csi->format_id[level];
}

/* The format of rows that show the name searched for as it is, "%s" of it,
   with only names before it that show otherwise: a format for each, by "is"
   that name. NULL if the list's formats do more. Songs have one for
   "[Untagged]", and formatting them took more than the search. */
static const struct display_format *plain_format(int group, int tag)
{
    if (TAGCACHE_IS_NUMERIC(tag))
        return NULL;
    for (int i = 0; i < format_count; i++)
    {
        const struct display_format *f = formats[i];
        if (f->group_id != group)
            continue;
        if (f->tag_count != 1 || strcmp(f->formatstr, "%s"))
            return NULL;
        if (f->clause_count == 0)
            return f->tags[0] == tag ? f : NULL;
        const struct tagcache_search_clause *cl = f->clause[0];
        if (f->clause_count != 1 || cl->tag != tag || cl->type != clause_is
            || cl->numeric || !cl->str)
            return NULL;
    }
    return NULL;
}

/* whether a row of that name shows by the plain format */
static bool shows_plain(const struct display_format *plain, const char *name)
{
    for (int i = 0; i < format_count && formats[i] != plain; i++)
    {
        if (formats[i]->group_id == plain->group_id
            && !strcasecmp(formats[i]->clause[0]->str, name))
            return false;
    }
    return true;
}

/* tags that change as music plays, without a new database */
static bool tag_of_stats(int tag)
{
    switch (tag)
    {
        case tag_playcount: case tag_rating: case tag_playtime:
        case tag_lastplayed: case tag_lastelapsed: case tag_lastoffset:
        case tag_virt_playtime_min: case tag_virt_playtime_sec:
        case tag_virt_autoscore:
            return true;
    }
    return false;
}

/* whether a list is searched or shown by them: Most Played, Recently Played */
static bool list_of_stats(int group, int tag, int level)
{
    if (tag_of_stats(tag))
        return true;
    for (int i = 0; i <= level; i++)
    {
        if (tag_of_stats(csi->tagorder[i]))
            return true;
        for (int j = 0; j < csi->clause_count[i]; j++)
        {
            if (tag_of_stats(csi->clause[i][j]->tag))
                return true;
        }
    }
    for (int i = 0; i < format_count; i++)
    {
        if (formats[i]->group_id != group)
            continue;
        for (int j = 0; j < formats[i]->tag_count; j++)
        {
            if (tag_of_stats(formats[i]->tags[j]))
                return true;
        }
        for (int j = 0; j < formats[i]->clause_count; j++)
        {
            if (tag_of_stats(formats[i]->clause[j]->tag))
                return true;
        }
    }
    return false;
}

static int make_entries(struct tree_context *c, int offset, bool init)
{
    logf( "%s", __func__);
    char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    struct tagcache_search tcs;
    struct display_format *fmt;
    int i;
    int namebufused = 0;
    int total_count = 0;
    c->special_entry_count = 0;
    int level = c->currextra;
    int tag;
    bool sort = false;
    bool sort_inverse;
    bool is_basename = false;
    int sort_limit;
    int strip;
    entries_whole = false;

    /* Show search progress straight away if the disk needs to spin up,
       otherwise show it after the normal 1/2 second delay */
    show_search_progress(
#ifdef HAVE_DISK_STORAGE
#ifdef HAVE_TC_RAMCACHE
        tagcache_is_in_ram() ? true :
#endif
        storage_disk_is_active()
#else
        true
#endif
        , 0, 0, 0);

    if (c->currtable == TABLE_ALLSUBENTRIES || c->currtable == TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS)
    {
        tag = tag_title;
        level--;
    }
    else
        tag = csi->tagorder[level];

    if (tag == menu_reload)
        return RELOAD_TAGTREE;

    if (tag == tag_virt_basename) /* basename shortcut */
    {
        is_basename = true;
        tag = tag_filename;
    }

    if (!tagcache_search(&tcs, tag))
        return -1;

    /* Prevent duplicate entries in the search list. */
    tagcache_search_set_uniqbuf(&tcs, uniqbuf, UNIQBUF_SIZE);

    if (level || is_basename|| csi->clause_count[0] || TAGCACHE_IS_NUMERIC(tag))
        sort = true;

    for (i = 0; i < level; i++)
    {
        if (TAGCACHE_IS_NUMERIC(csi->tagorder[i]))
        {
            static struct tagcache_search_clause cc;

            memset(&cc, 0, sizeof(struct tagcache_search_clause));
            cc.tag = csi->tagorder[i];
            cc.type = clause_is;
            cc.numeric = true;
            cc.numeric_data = csi->result_seek[i];
            tagcache_search_add_clause(&tcs, &cc);
        }
        else
        {
            tagcache_search_add_filter(&tcs, csi->tagorder[i],
                                       csi->result_seek[i]);
        }
    }

    /* because tagcache saves the clauses, we need to lock the buffer
     * for the entire duration of the search */
    core_pin(tagtree_handle);
    for (i = 0; i <= level; i++)
    {
        int j;

        for (j = 0; j < csi->clause_count[i]; j++)
            tagcache_search_add_clause(&tcs, csi->clause[i][j]);
    }

    current_offset = offset;
    current_entry_count = 0;
    c->dirfull = false;

    const int group = format_group(c, level);
    fmt = NULL;
    for (i = 0; i < format_count; i++)
    {
        if (formats[i]->group_id == group)
            fmt = formats[i];
    }
    const struct display_format *plain = is_basename ? NULL
                                                     : plain_format(group, tag);
    entries_of_stats = list_of_stats(group, tag, level);
    /* the album only for sorting by it */
    const bool with_album = c->currtable == TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS;

    if (fmt)
    {
        sort_inverse = fmt->sort_inverse;
        sort_limit = fmt->limit;
        strip = fmt->strip;
        sort = true;
    }
    else
    {
        sort_inverse = false;
        sort_limit = 0;
        strip = 0;
    }

    /* lock buflib out due to possible yields */
    tree_lock_cache(c);
    struct tagentry *dptr = core_get_data(c->cache.entries_handle);

    /* no <All tracks>, <By album> or <Random> entries in the lists: as on a
       stock iPod, the Music menu has Songs instead */

    while (tagcache_get_next(&tcs, tcs_buf, tcs_bufsz))
    {
        if (total_count++ < offset)
            continue;

        dptr->newtable = TABLE_NAVIBROWSE;
        if (tag == tag_title || tag == tag_filename)
        {
            dptr->newtable = TABLE_PLAYTRACK;
            dptr->extraseek = tcs.idx_id;
        }
        else
            dptr->extraseek = tcs.result_seek;
        dptr->customaction = ONPLAY_NO_CUSTOMACTION;

        fmt = NULL;
        /* Check the format, but for a name that shows as it is */
        const bool as_is = plain && shows_plain(plain, tcs.result);
        for (i = 0; i < format_count && !as_is; i++)
        {
            if (formats[i]->group_id != group)
                continue;

            if (tagcache_check_clauses(&tcs, formats[i]->clause,
                                       formats[i]->clause_count))
            {
                fmt = formats[i];
                break;
            }
        }

        if (strcmp(tcs.result, UNTAGGED) == 0)
        {
            if (tag == tag_title && tcs.type == tag_title && tcs.filter_count <= 1)
            { /* Fallback to basename */
                char *lastname = dptr->name;
                dptr->name = core_get_data(c->cache.name_buffer_handle)+namebufused;
                if ((c->cache.name_buffer_size - namebufused) > 0 &&
                    tagcache_retrieve(&tcs, tcs.idx_id, tag_virt_basename, dptr->name,
                                      c->cache.name_buffer_size - namebufused))
                {
                    namebufused += strlen(dptr->name)+1;
                    dptr->album_name = core_get_data(c->cache.name_buffer_handle)+namebufused;
                    if (with_album && (c->cache.name_buffer_size - namebufused) > 0 &&
                        tagcache_retrieve(&tcs, tcs.idx_id, tag_album, dptr->album_name,
                                    c->cache.name_buffer_size - namebufused))
                        namebufused += strlen(dptr->album_name)+1;
                    else
                        dptr->album_name = NULL;
                    goto entry_skip_formatter;
                }
                dptr->name = lastname; /* restore last entry if filename failed */
                dptr->album_name = NULL;
            }

            tcs.result = str(LANG_TAGNAVI_UNTAGGED);
            tcs.result_len = strlen(tcs.result);
            tcs.ramresult = true;
        }

        if (!tcs.ramresult || fmt)
        {

            dptr->name = core_get_data(c->cache.name_buffer_handle)+namebufused;

            if (fmt)
            {
                int ret = format_str(&tcs, fmt, dptr->name,
                                     c->cache.name_buffer_size - namebufused);
                bool error_on_str_format = ret < 0;
                if (!error_on_str_format)
                {
                    namebufused += strlen(dptr->name)+1; /* include NULL */
                    dptr->album_name = core_get_data(c->cache.name_buffer_handle)+namebufused;
                    if (with_album && (c->cache.name_buffer_size - namebufused) > 0 &&
                            tagcache_retrieve(&tcs, tcs.idx_id, tag_album, dptr->album_name,
                                      c->cache.name_buffer_size - namebufused))
                        namebufused += strlen(dptr->album_name)+1;
                    else
                        dptr->album_name = NULL;
                }
                else
                {
                    if (ret == -4)          /* buffer full */
                    {
                        logf("chunk mode #2: %d", current_entry_count);
                        c->dirfull = true;
                        sort = false;
                        break;
                    }

                    logf("format_str() failed");
                    tagcache_search_finish(&tcs);
                    tree_unlock_cache(c);
                    core_unpin(tagtree_handle);
                    return 0;
                }
            }
            else
            {
                tcs_get_basename(&tcs, is_basename);
                namebufused += tcs.result_len;
                bool buffer_full = (namebufused >= c->cache.name_buffer_size);
                if (!buffer_full)
                {
                    dptr->album_name = core_get_data(c->cache.name_buffer_handle)+namebufused;
                    if (with_album && tagcache_retrieve(&tcs, tcs.idx_id, tag_album, dptr->album_name,
                                      c->cache.name_buffer_size - namebufused))
                        namebufused += strlen(dptr->album_name)+1;
                    else
                        dptr->album_name = NULL;
                    strcpy(dptr->name, tcs.result);
                }
                else
                {
                    logf("chunk mode #2a: %d", current_entry_count);
                    c->dirfull = true;
                    sort = false;
                    break ;
                }
            }
        }
        else
        {
            tcs_get_basename(&tcs, is_basename);
            dptr->name = tcs.result;
            dptr->album_name = NULL;
        }
entry_skip_formatter:
        dptr++;
        current_entry_count++;

        if (current_entry_count >= c->cache.max_entries)
        {
            logf("chunk mode #3: %d", current_entry_count);
            c->dirfull = true;
            sort = false;
            break ;
        }

        if (init)
        {
            if (!tagtree_search_progress(total_count))
            {   /* user aborted */
                tagcache_search_finish(&tcs);
                tree_unlock_cache(c);
                core_unpin(tagtree_handle);
                return current_entry_count;
            }
        }
    }

    /* A long list stays in the database's order, as sorted already: 13000 names
       took seconds, and this is the order it always had in chunks */
    if (sort && current_entry_count <= TAGTREE_SORT_MAX)
    {
        if (global_settings.interpret_numbers)
            qsort_fn = sort_inverse ? strnatcasecmp_n_inv : strnatcasecmp_n;
        else
            qsort_fn = sort_inverse ? strncasecmp_inv : strncasecmp;

        struct tagentry *entries = get_entries(c);
        qsort(&entries[c->special_entry_count],
              current_entry_count - c->special_entry_count,
              sizeof(struct tagentry),
              c->currtable == TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS ? compare_with_albums : compare
        );
    }

    if (!init)
    {
        tagcache_search_finish(&tcs);
        tree_unlock_cache(c);
        core_unpin(tagtree_handle);
        return current_entry_count;
    }

    bool counted = true;
    while (tagcache_get_next(&tcs, tcs_buf, tcs_bufsz))
    {
        if (!tagtree_search_progress(total_count))
        {
            counted = false;
            break;
        }
        total_count++;
    }

    tagcache_search_finish(&tcs);
    tree_unlock_cache(c);
    core_unpin(tagtree_handle);

    if (!sort && (sort_inverse || sort_limit))
    {
        if (global_settings.talk_menu) {
            talk_id(LANG_SHOWDIR_BUFFER_FULL, true);
            talk_value(total_count, UNIT_INT, true);
        }

        /* (voiced above) */
        splashf(HZ*4, str(LANG_SHOWDIR_BUFFER_FULL), total_count);
        logf("Too small dir buffer");
        return 0;
    }

    if (sort_limit)
        total_count = MIN(total_count, sort_limit);

    if (strip)
    {
        dptr = get_entries(c);
        for (i = c->special_entry_count; i < current_entry_count; i++, dptr++)
        {
            int len = strlen(dptr->name);

            if (len < strip)
                continue;

            dptr->name = &dptr->name[strip];
        }
    }

    entries_whole = counted;
    return total_count;

}

#ifdef HAVE_TC_RAMCACHE
static bool waited_out = false;

/* waited for before the menu as long as a list would */
void tagtree_ram_waited_out(void)
{
    waited_out = true;
}
#endif

/* The list, once the database is in memory if it's about to be: made from the
   disk meanwhile, all songs took minutes, and held back its loading as long.
   It is before the menu shows (root_menu.c), so this is for one that hasn't
   come: waited for 20 s once, not by every list after, as a step into a list
   loads two or three */
static int retrieve_entries(struct tree_context *c, int offset, bool init)
{
#ifdef HAVE_TC_RAMCACHE
    if (!tagcache_ram_pending())
        waited_out = false;
    const long start = current_tick;
    bool told = false;
    while (!waited_out && tagcache_ram_pending())
    {
        if (TIME_AFTER(current_tick, start + 20 * HZ))
        {
            waited_out = true;
            break;
        }
        if (!told && !list_on_boot_screen && TIME_AFTER(current_tick, start + HZ / 2))
        {
            splash(0, ID2P(LANG_WAIT));
            told = true;
        }
        sleep(HZ / 10);
    }
#endif
    return make_entries(c, offset, init);
}

static int load_root(struct tree_context *c)
{
    struct tagentry *dptr = root_entries;
    int i;

    tc = c;
    c->currtable = TABLE_ROOT;
    if (c->dirlevel == 0)
        c->currextra = rootmenu;

    menu = menus[c->currextra];
    if (menu == NULL)
        return 0;

    root_rows = 0;

    /* Music's own menu without the sections turned off in Music Menu: a row
       goes into the item of its extraseek, not of its number */
    const bool all = root_all_items || c->currextra != rootmenu;
    int rows = 0;
    for (i = 0; i < menu->itemcount; i++)
    {
        if (!all && !root_item_shown(menu->items[i]))
            continue;
        rows++;

        dptr->name = (char*)menu->items[i]->name;

        logf( "%s loading menu %d name: %s, lang_id %ld", __func__, i,
             P2STR((unsigned char*)dptr->name),P2ID((unsigned char*)dptr->name));

        switch (menu->items[i]->type)
        {
            case menu_next:
                dptr->newtable = TABLE_NAVIBROWSE;
                dptr->extraseek = i;
                dptr->customaction = ONPLAY_NO_CUSTOMACTION;
                break;

            case menu_load:
                dptr->newtable = TABLE_ROOT;
                dptr->extraseek = menu->items[i]->link;
                dptr->customaction = ONPLAY_NO_CUSTOMACTION;
                break;

            case menu_shuffle_songs:
                dptr->newtable = TABLE_NAVIBROWSE;
                dptr->extraseek = i;
                dptr->customaction = ONPLAY_CUSTOMACTION_SHUFFLE_SONGS;
                break;

            case menu_byfirstletter:
                dptr->newtable = TABLE_NAVIBROWSE;
                dptr->extraseek = i;
                dptr->customaction = ONPLAY_CUSTOMACTION_FIRSTLETTER;
                break;

            case menu_playlists:
                dptr->newtable = TABLE_NAVIBROWSE;
                dptr->extraseek = i;
                dptr->customaction = ONPLAY_CUSTOMACTION_PLAYLISTS;
                break;

            case menu_coverflow:
                dptr->newtable = TABLE_NAVIBROWSE;
                dptr->extraseek = i;
                dptr->customaction = ONPLAY_CUSTOMACTION_COVERFLOW;
                break;

            case menu_search:
                dptr->newtable = TABLE_NAVIBROWSE;
                dptr->extraseek = i;
                dptr->customaction = ONPLAY_CUSTOMACTION_SEARCH;
                break;
        }

        dptr++;
    }

    root_rows = rows;
    return rows;
}

int tagtree_load(struct tree_context* c)
{
    logf( "%s", __func__);

    int count;
    int table = c->currtable;

    c->dirsindir = 0;

    if (!table)
    {
        c->dirfull = false;
        table = TABLE_ROOT;
        c->currtable = table;
        c->currextra = rootmenu;
    }

    switch (table)
    {
        case TABLE_ROOT:
            logf( "root...");
            count = load_root(c);
            break;

        case TABLE_ALLSUBENTRIES:
        case TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS:
        case TABLE_NAVIBROWSE:
            logf("navibrowse...");

            count = kept_list(c);
            if (count >= 0)
            {
                logf("Reusing %d entries", count);
                break;
            }

            cpu_boost(true);
            count = retrieve_entries(c, 0, true);
            cpu_boost(false);
            if (count >= 0 && entries_whole)
                keep_list(c, count);
            else
                kept_hash = 0;
            break;

        default:
            logf("Unsupported table %d\n", table);
            return -1;
    }

    if (count < 0)
    {
        if (count != RELOAD_TAGTREE)
            splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
        else /* unload and re-init tagtree */
        {
            splash(HZ, ID2P(LANG_WAIT));
            tagtree_unload(c);
            tagnavi_loaded = initialize_tagtree();
            if (!tagnavi_loaded)
                return 0;
        }
        c->dirlevel = 0;
        count = load_root(c);
    }

    /* The _total_ numer of entries available. */
    c->dirlength = c->filesindir = count;

    return count;
}

/* Enters menu or table for selected item in the database.
 *
 * Call this with the is_visible parameter set to false to
 * prevent selected_item_history from being updated or applied, in
 * case the menus aren't displayed to the user.
 * Before calling tagtree_enter again with the parameter set to
 * true, make sure that you are back at the previous dirlevel, by
 * calling tagtree_exit as needed, with is_visible set to false.
 */
/* A place in the lists, to go back to */
struct tagtree_place
{
    struct menu_root *menu;
    struct search_instruction *csi;
    int result_seek[MAX_TAGS];
    int currtable, currextra, dirlevel, selected_item, max_history_level;
    int selected_item_history[MAX_DIR_LEVELS];
    int table_history[MAX_DIR_LEVELS];
    int extra_history[MAX_DIR_LEVELS];
    char current_title[MAX_TAGS][sizeof(current_title[0])];
};

/* where the music playing was started, and where the lists were before they were
   opened at the track playing */
static struct tagtree_place saved_place, go_to_place;
/* the level that was opened at the track playing, where Menu leaves the lists */
static int go_to_level = -1;

static void place_save(struct tagtree_place *place, int dirlevel, int selected_item)
{
    const struct tree_context *c = tree_get_context();
    place->menu = menu;
    place->csi = csi;
    if (csi)
        memcpy(place->result_seek, csi->result_seek, sizeof(csi->result_seek));
    place->currtable = c->currtable;
    place->currextra = c->currextra;
    place->dirlevel = dirlevel;
    place->selected_item = selected_item;
    place->max_history_level = max_history_level;
    memcpy(place->selected_item_history, selected_item_history,
           sizeof(selected_item_history));
    memcpy(place->table_history, table_history, sizeof(table_history));
    memcpy(place->extra_history, extra_history, sizeof(extra_history));
    memcpy(place->current_title, current_title, sizeof(current_title));
}

static void place_restore(const struct tagtree_place *place, int *dirlevel,
                          int *selected_item)
{
    struct tree_context *c = tree_get_context();
    menu = place->menu;
    csi = place->csi;
    if (csi)
        memcpy(csi->result_seek, place->result_seek, sizeof(csi->result_seek));
    c->currtable = place->currtable;
    c->currextra = place->currextra;
    *dirlevel = place->dirlevel;
    *selected_item = place->selected_item;
    max_history_level = place->max_history_level;
    memcpy(selected_item_history, place->selected_item_history,
           sizeof(selected_item_history));
    memcpy(table_history, place->table_history, sizeof(table_history));
    memcpy(extra_history, place->extra_history, sizeof(extra_history));
    memcpy(current_title, place->current_title, sizeof(current_title));
}

void tagtree_save_place(int dirlevel, int selected_item)
{
    place_save(&saved_place, dirlevel, selected_item);
}

void tagtree_restore_place(int *dirlevel, int *selected_item)
{
    place_restore(&saved_place, dirlevel, selected_item);
}

/* The row of the list with this name, or -1 */
static int tagtree_find_row(struct tree_context *c, const char *name)
{
    if (!strcmp(name, UNTAGGED))
        name = str(LANG_TAGNAVI_UNTAGGED);
    for (int i = 0; i < c->dirlength; i++)
    {
        if (!strcmp(tagtree_get_entry(c, i)->name, name))
            return i;
    }
    return -1;
}

/* The list of them at the top of Music entered, and the row of name in it, as
   by hand: true with c at the list that row opens, or else the lists back as
   they were. By the album artist, the list of artists is of those instead. */
static bool go_to_row(int tag, bool by_album_artist, const char *name,
                      int *dirlevel, int *selected_item)
{
    static struct search_instruction album_artists;
    struct tree_context *c = tree_get_context();
    int row = -1;

    place_save(&go_to_place, *dirlevel, *selected_item);
    c->dirlevel = 0;
    c->currtable = 0;
    max_history_level = 0;
    /* its section gone into even if Music Menu doesn't show it: a row an item */
    root_all_items = true;
    if (tagtree_load(c) > 0)
    {
        for (int i = 0; i < menu->itemcount && row < 0; i++)
        {
            const struct search_instruction *si = &menu->items[i]->si;
            if (menu->items[i]->type == menu_next && si->tagorder[0] == tag
                && si->clause_count[0] == 0)
                row = i;
        }
    }
    root_all_items = false;
    if (row >= 0)
    {
        /* the list of them; Menu up from it to the row of Music's own menu */
        c->selected_item = row;
        tagtree_enter(c, true);
        selected_item_history[0] = root_row_of(row);
        if (by_album_artist)
        {
            album_artists = *csi;
            album_artists.tagorder[0] = tag_albumartist;
            csi = &album_artists;
        }
        tagtree_load(c);
        row = tagtree_find_row(c, name);
    }
    if (row < 0)
    {
        place_restore(&go_to_place, dirlevel, selected_item);
        return false;
    }
    c->selected_item = row;
    tagtree_enter(c, true);
    tagtree_load(c);
    return true;
}

/* the lists there, with that row selected, until tagtree_end_go_to() */
static void go_to_end(int row, int *dirlevel, int *selected_item)
{
    struct tree_context *c = tree_get_context();
    c->selected_item = row;
    *dirlevel = c->dirlevel;
    *selected_item = row;
    go_to_level = c->dirlevel;
}

/* Puts the lists at the track playing, as one gets there by hand: its album's artist,
   or else its own, with its album selected, or its album with the track selected. The
   Artists list is of the tracks' artists, where a guest on one song has a row of their
   own, so the albums here are those with that album artist. tagtree_end_go_to() ends
   this, and until then Menu in that list leaves the lists. False if it isn't in the
   database. */
bool tagtree_go_to_playing(bool artist, int *dirlevel, int *selected_item)
{
    static char name[TAGCACHE_BUFSZ], album[TAGCACHE_BUFSZ];
    struct tree_context *c = tree_get_context();
    const struct mp3entry *id3 = audio_current_track();
    const int tag = artist ? tag_virt_canonicalartist : tag_album;
    struct tagcache_search tcs;
    int row;

    if (!id3 || !tagtree_ready() || !tagcache_is_usable()
        || !tagcache_find_index(&tcs, id3->path))
        return false;
    const int track = tcs.idx_id;
    const bool by_album_artist = artist
        && tagcache_retrieve(&tcs, track, tag_albumartist, name, sizeof(name))
        && name[0] && strcmp(name, UNTAGGED);
    const bool found = (by_album_artist
                        || tagcache_retrieve(&tcs, track, tag, name, sizeof(name)))
        && tagcache_retrieve(&tcs, track, tag_album, album, sizeof(album));
    tagcache_search_finish(&tcs);
    if (!found || !go_to_row(tag, by_album_artist, name, dirlevel, selected_item))
        return false;

    /* the one playing */
    row = 0;
    if (artist)
        row = MAX(tagtree_find_row(c, album), 0);
    else
    {
        for (int i = 0; i < c->dirlength; i++)
        {
            if (tagtree_get_entry(c, i)->extraseek == track)
            {
                row = i;
                break;
            }
        }
    }
    go_to_end(row, dirlevel, selected_item);
    return true;
}

bool tagtree_go_to_name(int tag, const char *name, int *dirlevel, int *selected_item)
{
    if (!tagtree_ready() || !tagcache_is_usable()
        || !go_to_row(tag, false, name, dirlevel, selected_item))
        return false;
    go_to_end(0, dirlevel, selected_item);
    return true;
}

/* The item of Music's own menu by its phrase, -1 if tagnavi.config hasn't it */
static int root_item_of(int lang)
{
    const struct menu_root *root = menus[rootmenu];
    for (int i = 0; root && i < root->itemcount; i++)
    {
        if (P2ID((const unsigned char *)root->items[i]->name) == lang)
            return i;
    }
    return -1;
}

bool tagtree_has_section(int lang)
{
    return root_item_of(lang) >= 0;
}

/* the section of Music opened from the main menu, -1 for none; and the row
   each was left at, to open there again */
static int section_open = -1;
static int section_rows[MUSIC_SECTIONS];

static int section_of(int lang)
{
    for (int i = 0; i < MUSIC_SECTIONS; i++)
    {
        if (tagtree_section_lang[i] == lang)
            return i;
    }
    return -1;
}

bool tagtree_go_to_section(int lang, int *dirlevel, int *selected_item)
{
    struct tree_context *c = tree_get_context();
    const int item = root_item_of(lang);
    const int section = section_of(lang);
    if (item < 0 || section < 0 || !tagtree_ready() || !tagcache_is_usable())
        return false;

    place_save(&go_to_place, *dirlevel, *selected_item);
    c->dirlevel = 0;
    c->currtable = 0;
    max_history_level = 0;
    root_all_items = true;
    const bool loaded = tagtree_load(c) > item;
    root_all_items = false;
    if (!loaded)
    {
        place_restore(&go_to_place, dirlevel, selected_item);
        return false;
    }
    c->selected_item = item;
    tagtree_enter(c, true);
    selected_item_history[0] = root_row_of(item);
    const int count = tagtree_load(c);
    section_open = section;
    go_to_end(MIN(section_rows[section], MAX(count - 1, 0)), dirlevel, selected_item);
    return true;
}

void tagtree_resume_section(int lang, int *dirlevel, int *selected_item)
{
    place_save(&go_to_place, *dirlevel, *selected_item);
    section_open = section_of(lang);
    go_to_level = 1;
}

int tagtree_go_to_level(void)
{
    return go_to_level;
}

void tagtree_end_go_to(bool go_back, int *dirlevel, int *selected_item)
{
    if (section_open >= 0)
    {
        /* its list's row, as left by Menu there or from deeper in it */
        section_rows[section_open] = *dirlevel == 1 ? *selected_item
                                                    : selected_item_history[1];
        section_open = -1;
    }
    go_to_level = -1;
    if (go_back)
        place_restore(&go_to_place, dirlevel, selected_item);
}

int tagtree_enter(struct tree_context* c, bool is_visible)
{
    logf( "%s", __func__);

    int rc = 0;
    struct tagentry *dptr;
    struct mp3entry *id3;
    int newextra;
    int seek;
    int source;
    bool is_random_item = false;
    bool adjust_selection = true;

    dptr = tagtree_get_entry(c, c->selected_item);

    c->dirfull = false;
    seek = dptr->extraseek;
    if (seek == -1) /* <Random> menu item was selected */
    {
        is_random_item = true;
        if(c->filesindir<=c->special_entry_count) /* Menu contains only special entries */
            return 0;

        dptr = (tagtree_get_entry(c, c->special_entry_count+(rand() % (c->filesindir-c->special_entry_count))));
        seek = dptr->extraseek;
    }
    newextra = dptr->newtable;

    if (c->dirlevel >= MAX_DIR_LEVELS)
        return 0;

    if (is_visible) /* update selection history only for user-selected items */
    {
        /* We need to discard selected item history for levels
        descending from current one if selection has changed */
        if (max_history_level < c->dirlevel + 1
            || (max_history_level > c->dirlevel
                && selected_item_history[c->dirlevel] != c->selected_item)
            || is_random_item)
        {
            max_history_level = c->dirlevel + 1;
            if (max_history_level < MAX_DIR_LEVELS)
                selected_item_history[max_history_level] = 0;
        }

        selected_item_history[c->dirlevel]=c->selected_item;
    }
    table_history[c->dirlevel] = c->currtable;
    extra_history[c->dirlevel] = c->currextra;

    if (c->dirlevel + 1 < MAX_DIR_LEVELS)
    {
        c->dirlevel++;
        /*DEBUGF("Tagtree depth %d\n", c->dirlevel);*/
    }
    else
    {
        DEBUGF("Tagtree depth exceeded\n");
    }

    /* lock buflib for possible I/O to protect dptr */
    tree_lock_cache(c);
    core_pin(tagtree_handle);

    switch (c->currtable) {
        case TABLE_ROOT:
            c->currextra = newextra;

            if (newextra == TABLE_ROOT)
            {
                menu = menus[seek];
                c->currextra = seek;
            }

            else if (newextra == TABLE_NAVIBROWSE)
            {
                int i, j;

                csi = &menu->items[seek]->si;
                c->currextra = 0;

                unsigned char *name = dptr->name;

                strmemccpy(current_title[c->currextra], P2STR(name),
                           sizeof(current_title[0]));

                logf("%s (ROOT) current title %s", __func__, P2STR(name));

                /* Read input as necessary. */
                for (i = 0; i < csi->tagorder_count; i++)
                {
                    for (j = 0; j < csi->clause_count[i]; j++)
                    {
                        char* searchstring;

                        if (csi->clause[i][j]->type == clause_logical_or)
                            continue;

                        source = csi->clause[i][j]->source;

                        if (source == source_constant)
                            continue;

                        /* discard history for lower levels when doing runtime searches */
                        if (is_visible)
                            max_history_level = c->dirlevel - 1;

                        searchstring=csi->clause[i][j]->str;
                        *searchstring = '\0';

                        id3 = audio_current_track();

                        if (source == source_current_path && id3)
                        {
                            char *e;
                            strmemccpy(searchstring, id3->path, SEARCHSTR_SIZE);
                            e = strrchr(searchstring, '/');
                            if (e)
                                *e = '\0';
                        }
                        else if (source > source_runtime && id3)
                        {

                            int k = source-source_runtime;
                            int offset = id3_to_search_mapping[k].id3_offset;
                            char **src = (char**)((char*)id3 + offset);
                            if (*src)
                            {
                                strmemccpy(searchstring, *src, SEARCHSTR_SIZE);
                            }
                        }
                        else
                        {
                            rc = kbd_input(searchstring, SEARCHSTR_SIZE, NULL);
                            if (rc < 0 || !searchstring[0])
                            {
                                tagtree_exit(c, is_visible);
                                tree_unlock_cache(c);
                                core_unpin(tagtree_handle);
                                return 0;
                            }
                            if (csi->clause[i][j]->numeric)
                                csi->clause[i][j]->numeric_data = atoi(searchstring);
                        }


                    }
                }
            }
            c->currtable = newextra;

            break;

        case TABLE_NAVIBROWSE:
        case TABLE_ALLSUBENTRIES:
        case TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS:
            if (newextra == TABLE_PLAYTRACK)
            {
                adjust_selection = false;

                if (global_settings.party_mode && audio_status()) {
                    splash(HZ, ID2P(LANG_PARTY_MODE));
                    break;
                }
                c->dirlevel--;
                /* about to create a new current playlist...
                 allow user to cancel the operation */
                if (!warn_on_pl_erase())
                    break;
                cpu_boost(true);
                if (tagtree_play_folder(c) >= 0)
                    rc = 2;
                cpu_boost(false);
                break;
            }

            c->currtable = newextra;
            csi->result_seek[c->currextra] = seek;
            if (c->currextra < csi->tagorder_count-1)
                c->currextra++;
            else
                c->dirlevel--;

            unsigned char *name = dptr->name;
            name = P2STR(name);
            logf("%s (NAVI/ALLSUB) current title %s", __func__, name);
            /* Update the statusbar title */
            strmemccpy(current_title[c->currextra], name, sizeof(current_title[0]));
            break;

        default:
            c->dirlevel--;
            break;
    }

    if (adjust_selection)
    {
        if (is_visible && c->dirlevel <= max_history_level)
            c->selected_item = selected_item_history[c->dirlevel];
        else
            c->selected_item = 0;
    }

    tree_unlock_cache(c);
    core_unpin(tagtree_handle);

    return rc;
}

/* Exits current database menu or table */
void tagtree_exit(struct tree_context* c, bool is_visible)
{
    logf( "%s", __func__);
    if (is_visible) /* update selection history only for user-selected items */
    {
        if (c->selected_item != selected_item_history[c->dirlevel])
            max_history_level = c->dirlevel; /* discard descending item history */
        selected_item_history[c->dirlevel] = c->selected_item;
    }
    c->dirfull = false;
    if (c->dirlevel > 0)
    {
        c->dirlevel--;
        /*DEBUGF("Tagtree depth %d\n", c->dirlevel);*/
    }
    else
    {
        DEBUGF("Tagtree nothing to exit\n");
    }

    if (is_visible)
        c->selected_item = selected_item_history[c->dirlevel];
    c->currtable = table_history[c->dirlevel];
    c->currextra = extra_history[c->dirlevel];
}

/* as Music is opened from the main menu, as on a stock iPod: not the list it
   was left in, as Songs, which opened in place of Music's menu */
void tagtree_to_top(struct tree_context* c)
{
    while (c->dirlevel > 0)
        tagtree_exit(c, true);
    if (root_rows_changed)
        c->selected_item = 0;
    root_rows_changed = false;
}

/* A section turned on or off in Music Menu: the rows of Music's own menu, as
   gone back to, were of the sections it showed before. Music opens at its
   first row then. */
void tagtree_music_menu_changed(bool shown)
{
    (void)shown;
    selected_item_history[0] = 0;
    saved_place.selected_item_history[0] = 0;
    go_to_place.selected_item_history[0] = 0;
    if (saved_place.dirlevel == 0)
        saved_place.selected_item = 0;
    if (go_to_place.dirlevel == 0)
        go_to_place.selected_item = 0;
    root_rows_changed = true;
}

int tagtree_get_filename(struct tree_context* c, char *buf, int buflen)
{
    struct tagcache_search tcs;
    int extraseek = tagtree_get_entry(c, c->selected_item)->extraseek;


    if (!tagcache_search(&tcs, tag_filename))
        return -1;

    if (!tagcache_retrieve(&tcs, extraseek, tcs.type, buf, buflen))
    {
        tagcache_search_finish(&tcs);
        return -2;
    }

    tagcache_search_finish(&tcs);

    return 0;
}

int tagtree_get_custom_action(struct tree_context* c)
{
    if (c->dirlength == 0)
        return 0;
    return tagtree_get_entry(c, c->selected_item)->customaction;
}

static void swap_array_bool(bool *a, bool *b)
{
    bool temp = *a;
    *a = *b;
    *b = temp;
}

/**
 * Randomly shuffle an array using the Fisher-Yates algorithm :
 *  https://en.wikipedia.org/wiki/Random_permutation
 *  This algorithm has a linear complexity.
 *  Don't forget to srand before call to use it with a relevant seed.
 */
static bool* fill_random_playlist_indexes(bool *bool_array, size_t arr_sz,
                                         size_t track_count, size_t max_slots)
{
    size_t i;
    if (track_count * sizeof(bool) > arr_sz || max_slots > track_count)
        return NULL;

    for (i = 0; i < arr_sz; i++) /* fill max_slots with TRUE */
        bool_array[i] = i < max_slots;

    /* shuffle bool array */
    for (i = track_count - 1; i > 0; i--)
    {
        int j = rand() % (i + 1);
        swap_array_bool(&bool_array[i], &bool_array[j]);
    }
    return bool_array;
}

/* Writes a playlist file a large block at a time, through the plugin
   buffer; one write() a song takes long on the iPod */
struct playlist_writer {
    int fd;
    char *buf;
    size_t size, used;
    bool ok;
    /* of a new file: where each song starts, from the end of the buffer */
    uint32_t *offsets;
    int count, max;
    uint32_t next;
};

/* those of the last new file, until the plugin buffer is used again; none
   if it wasn't finished */
static uint32_t *written_offsets;
static int written_count;

static bool playlist_writer_open(struct playlist_writer *w,
                                 const char *playlist, bool new_playlist)
{
    w->buf = plugin_get_buffer(&w->size);
    w->used = 0;
    w->ok = true;
    w->offsets = NULL;
    w->count = 0;
    written_offsets = NULL;
    written_count = 0;
    if (new_playlist)
        w->fd = open_utf8(playlist, O_CREAT|O_WRONLY|O_TRUNC);
    else
        w->fd = open(playlist, O_CREAT|O_WRONLY|O_APPEND, 0666);
    if (new_playlist && w->fd >= 0 && w->buf)
    {
        /* after the BOM */
        w->next = lseek(w->fd, 0, SEEK_CUR);
        w->max = playlist_get_current()->max_playlist_size;
        size_t room = w->max * sizeof(uint32_t) + sizeof(uint32_t);
        if (w->size > room + MAX_PATH)
        {
            w->size -= room;
            w->offsets = (uint32_t *)ALIGN_UP((uintptr_t)(w->buf + w->size),
                                              sizeof(uint32_t));
        }
    }
    return w->fd >= 0 && w->buf && w->size >= MAX_PATH;
}

static bool playlist_writer_flush(struct playlist_writer *w)
{
    if (w->ok && w->used > 0)
        w->ok = write(w->fd, w->buf, w->used) == (ssize_t)w->used;
    w->used = 0;
    return w->ok;
}

static bool playlist_writer_add(struct playlist_writer *w, const char *path)
{
    size_t len = strlen(path);
    if (w->used + len + 1 > w->size && !playlist_writer_flush(w))
        return false;
    if (w->offsets && w->count < w->max)
    {
        w->offsets[w->count++] = w->next;
        w->next += len + 1;
    }
    memcpy(w->buf + w->used, path, len);
    w->buf[w->used + len] = '\n';
    w->used += len + 1;
    return true;
}

static bool playlist_writer_close(struct playlist_writer *w)
{
    bool ok = w->fd >= 0 && playlist_writer_flush(w);
    if (w->fd >= 0)
        close(w->fd);
    w->fd = -1;
    if (ok && w->offsets)
    {
        written_offsets = w->offsets;
        written_count = w->count;
    }
    return ok;
}

static bool insert_all_playlist(struct tree_context *c,
                                const char* playlist, bool new_playlist,
                                int position, bool queue)
{
    struct tagcache_search tcs;
    int n;
    unsigned long last_tick;
    int slots_remaining = 0;
    bool fill_randomly = false;
    bool *rand_bool_array = NULL;
    char buf[MAX_PATH];
    struct playlist_insert_context context;
    struct playlist_writer writer = { .fd = -1 };

    cpu_boost(true);

    if (!tagcache_search(&tcs, tag_filename))
    {
        splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
        cpu_boost(false);
        return false;
    } /* NOTE: you need to close this search before returning */

    if (playlist == NULL)
    {
        if (playlist_insert_context_create(NULL, &context, position, queue, false) < 0)
        {
            tagcache_search_finish(&tcs);
            cpu_boost(false);
            return false;
        }
    }
    else if (!playlist_writer_open(&writer, playlist, new_playlist))
    {
        playlist_writer_close(&writer);
        tagcache_search_finish(&tcs);
        cpu_boost(false);
        return false;
    }

    last_tick = current_tick + HZ/2; /* Show splash after 0.5 seconds have passed */
    splash_progress_set_delay(HZ / 2); /* wait 1/2 sec before progress */
    n = c->filesindir;

    if (playlist == NULL)
    {
        int max_playlist_size = playlist_get_current()->max_playlist_size;
        slots_remaining = max_playlist_size - playlist_get_current()->amount;
        if (slots_remaining <= 0)
        {
            logf("Playlist has no space remaining");
            tagcache_search_finish(&tcs);
            cpu_boost(false);
            return false;
        }

        fill_randomly = n > slots_remaining;

        if (fill_randomly)
        {
            size_t bufsize = 0;
            bool *buffer = (bool *) plugin_get_buffer(&bufsize);
            rand_bool_array = fill_random_playlist_indexes(buffer, bufsize,
                                                           n, slots_remaining);

            splashf(HZ * 2, ID2P(LANG_RANDOM_SHUFFLE_RANDOM_SELECTIVE_SONGS_SUMMARY),
                    slots_remaining); /* voiced above */
        }
    }

    bool exit_loop_now = false;
    for (int i = 0; i < n; i++)
    {
        if (TIME_AFTER(current_tick, last_tick + HZ/4))
        {
            if (list_on_boot_screen)
                library_update_songs_progress(i, n);
            else /* (voiced) */
                splash_progress(i, n, "%s", str(LANG_WAIT)); /* stopped as before, unsaid */
            if (action_userabort(TIMEOUT_NOBLOCK))
            {
                exit_loop_now = true;
                break;
            }
            last_tick = current_tick;
        }

        if (playlist == NULL)
        {
            if (fill_randomly)
            {
                int remaining_tracks = n - i;
                if (remaining_tracks > slots_remaining)
                {
                    if (rand_bool_array)
                    {
                        /* Skip the track if rand_bool_array[i] is FALSE */
                        if (!rand_bool_array[i])
                            continue;
                    }
                    else
                    {
                        /* Generate random value between 0 and remaining_tracks - 1 */
                        int selrange = RAND_MAX / remaining_tracks; /* Improve distribution */
                        int random;

                        for (int r = 0; r < 0x0FFF; r++) /* limit loops */
                        {
                            random = rand() / selrange;
                            if (random < remaining_tracks)
                                break;
                            else
                                random = 0;
                        }
                        /* Skip the track if random >= slots_remaining */
                        if (random >= slots_remaining)
                            continue;
                    }
                }
            }
        }

        if (!tagcache_retrieve(&tcs, tagtree_get_entry(c, i)->extraseek, tcs.type, buf, sizeof buf))
            continue;

        if (playlist == NULL)
        {
            if (fill_randomly)
            {
                if (--slots_remaining <= 0)
                {
                    exit_loop_now = true;
                    break;
                }
            }

            if (playlist_insert_context_add(&context, buf) < 0) {
                logf("playlist_insert_track failed");
                exit_loop_now = true;
                break;
            }
        }
        else if (!playlist_writer_add(&writer, buf))
        {
            exit_loop_now = true;
            break;
        }
        /* now and then: music may still play, and a yield for every song
           gave the codec a turn each time */
        if ((i & 63) == 0)
            yield();

        if (exit_loop_now)
            break;
    }

    bool ok = true;
    if (playlist == NULL)
        playlist_insert_context_release(&context);
    else
    {
        ok = playlist_writer_close(&writer);
        if (exit_loop_now)
            written_offsets = NULL;
    }

    tagcache_search_finish(&tcs);
    cpu_boost(false);

    return ok;
}

static bool goto_allsubentries(int newtable)
{
    int i = 0;
    while (i < 2 && (newtable == TABLE_NAVIBROWSE || newtable == TABLE_ALLSUBENTRIES
        || newtable == TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS))
    {
        tagtree_enter(tc, false);
        tagtree_load(tc);
        newtable = tagtree_get_entry(tc, tc->selected_item)->newtable;
        i++;
    }
    return (newtable == TABLE_PLAYTRACK);
}

static void reset_tc_to_prev(int dirlevel, int selected_item)
{
    while (tc->dirlevel > dirlevel)
        tagtree_exit(tc, false);
    tc->selected_item = selected_item;
    tagtree_load(tc);
}

static bool tagtree_insert_selection(int position, bool queue,
                                     const char* playlist, bool new_playlist)
{
    char buf[MAX_PATH];
    int dirlevel = tc->dirlevel;
    int selected_item = tc->selected_item;
    int newtable;
    int ret;

    show_search_progress(
#ifdef HAVE_DISK_STORAGE
        storage_disk_is_active()
#else
        true
#endif
        , 0, 0, 0);

    newtable = tagtree_get_entry(tc, tc->selected_item)->newtable;

    if (newtable == TABLE_PLAYTRACK) /* Insert a single track? */
    {
        if (tagtree_get_filename(tc, buf, sizeof buf) < 0)
            return false;

        if (!playlist)
            playlist_insert_track(NULL, buf, position, queue, true);
        else
            catalog_insert_into(playlist, new_playlist, buf, FILE_ATTR_AUDIO);

        return true;
    }

    ret = goto_allsubentries(newtable);
    if (ret)
    {
        if (tc->filesindir <= 0)
            splash(HZ, ID2P(LANG_END_PLAYLIST));
        else if (!insert_all_playlist(tc, playlist, new_playlist, position, queue))
            splash(HZ*2, ID2P(LANG_FAILED));
    }

    reset_tc_to_prev(dirlevel, selected_item);
    return ret;
}

/* Execute action_cb for all entries of the current table's
 * selected item, handing over each entry's filename in the
 * callback function parameter. Parameter will be NULL for
 * entries whose filename couldn't be retrieved.
 */
bool tagtree_entries_iterate(struct tagcache_search *tcs,
                             bool (*action_cb)(const char *file_name),
                             char* buf, size_t buf_sz)
{
    int i, n;
    unsigned long last_tick;
    int ret = true;
    int dirlevel = tc->dirlevel;
    int selected_item = tc->selected_item;
    int newtable = tagtree_get_entry(tc, tc->selected_item)->newtable;

    cpu_boost(true);
    if (!goto_allsubentries(newtable))
        ret = false;
    else if (tagcache_search(tcs, tag_filename))
    {
        last_tick = current_tick + HZ/2;
        splash_progress_set_delay(HZ / 2); /* wait 1/2 sec before progress */
        n = tc->filesindir;
        for (i = 0; i < n; i++)
        {
            /* (voiced) */
            splash_progress(i, n, "%s", str(LANG_WAIT)); /* stopped as before, unsaid */
            if (TIME_AFTER(current_tick, last_tick + HZ/4))
            {
                if (action_userabort(TIMEOUT_NOBLOCK))
                    break;
                last_tick = current_tick;
            }

            if (!action_cb(tagcache_retrieve(tcs, tagtree_get_entry(tc, i)->extraseek,
                                             tcs->type, buf, buf_sz) ? buf : NULL))
            {
                ret = false;
                break;
            }
            yield();
        }

        tagcache_search_finish(tcs);
    }
    else
    {
        splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
        ret = false;
    }
    reset_tc_to_prev(dirlevel, selected_item);
    cpu_boost(false);
    return ret;
}

/* Try to return first subentry's filename for current selection
 */
bool tagtree_get_subentry_filename(char *buf, size_t bufsize)
{
    int ret = true;
    int dirlevel = tc->dirlevel;
    int selected_item = tc->selected_item;
    int newtable = tagtree_get_entry(tc, tc->selected_item)->newtable;

    if (!goto_allsubentries(newtable) || tagtree_get_filename(tc, buf, bufsize) < 0)
        ret = false;

    reset_tc_to_prev(dirlevel, selected_item);
    return ret;
}

/* Play all songs in random order, as Shuffle Songs in the main menu of a
 * stock iPod, but for podcasts and audiobooks. When there are more than the
 * playlist can hold, a random selection fills it. False if nothing could
 * be played. */
bool tagtree_shuffle_songs(void)
{
    struct tagcache_search tcs;
    struct playlist_writer writer;
    char buf[MAX_PATH];
    int max = playlist_get_current()->max_playlist_size;
    int count = tagcache_get_song_count(); /* -1 until counted */
    int added = 0;
    long last_tick;

    if (!tagcache_is_usable() || !tagcache_search(&tcs, tag_filename))
    {
        splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
        return false;
    }

    cpu_boost(true);
    splash(0, ID2P(LANG_WAIT));
    /* as when playing a list */
    if (audio_status() & AUDIO_STATUS_PLAY)
        audio_stop();
    char file[MAX_PATH];
    const char *name = tagtree_playlist_names[
        tagtree_free_slot(0, file, sizeof(file))];
    bool ok = playlist_writer_open(&writer, file, true);
    last_tick = current_tick + HZ/2; /* progress after half a second */
    int seen = 0;
    while (ok && added < max && tagcache_get_next(&tcs, buf, sizeof buf))
    {
        if (!buf[0])
            continue; /* the entry of a removed file */
        /* selection sampling: each of the remaining songs gets picked
           with the chance there are slots left for it */
        int i = seen++;
        if (count > max && i < count && rand() % (count - i) >= max - added)
            continue;
        if (itunesdb_track_flags(buf) & ITUNESDB_SKIP_SHUFFLE)
            continue;
        ok = playlist_writer_add(&writer, buf);
        added++;
        if ((added & 63) == 0)
            yield();

        if (TIME_AFTER(current_tick, last_tick + HZ/4))
        {
            splash_progress(seen, count > 0 ? count : seen, "%s (%s)",
                            str(LANG_WAIT), str(LANG_OFF_ABORT));
            if (action_userabort(TIMEOUT_NOBLOCK))
                break;
            last_tick = current_tick;
        }
    }
    tagcache_search_finish(&tcs);
    ok = playlist_writer_close(&writer) && ok;

    if (ok && added > 0
        && playlist_create(TAGTREE_PLAYLIST_DIR, name) == 0
        && playlist_amount() > 0)
    {
        playlist_shuffle(current_tick, -1);
        playlist_start(0, 0, 0);
    }
    else
        ok = false;
    cpu_boost(false);
    return ok;
}

bool tagtree_current_playlist_insert(int position, bool queue)
{
    return tagtree_insert_selection(position, queue, NULL, false);
}


int tagtree_add_to_playlist(const char* playlist, bool new_playlist)
{
    if (!new_playlist)
        tagtree_load(tc); /* because display_playlists was called */
    return tagtree_insert_selection(0, false, playlist, new_playlist) ? 0 : -1;
}

/* the selection's songs at the end of a playlist file, from the list it's in */
bool tagtree_add_to_file(const char *file)
{
    return tagtree_insert_selection(0, false, file, false);
}

/* in Music's own list, whose rows are its sections: none adds a song */
bool tagtree_at_top(void)
{
    return tc && tc->dirlevel == 0;
}

/* Which list this is, from how it is searched for. Going through its songs
   instead would load them all, a chunk at a time: over a second for 13000. */
static uint32_t tagtree_list_id(int table, int extra,
                                const struct search_instruction *si)
{
    uint32_t id = 2166136261u;
#define LIST_ID_ADD(value) id = (id ^ (uint32_t)(value)) * 16777619u
    LIST_ID_ADD(table);
    LIST_ID_ADD(extra);
    LIST_ID_ADD(si->tagorder_count);
    for (int i = 0; i < si->tagorder_count; i++)
    {
        LIST_ID_ADD(si->tagorder[i]);
        LIST_ID_ADD(si->format_id[i]);
        /* what was chosen on the way here */
        if (i < extra)
            LIST_ID_ADD(si->result_seek[i]);
        for (int j = 0; j < si->clause_count[i]; j++)
        {
            const struct tagcache_search_clause *clause = si->clause[i][j];
            LIST_ID_ADD(clause->tag);
            LIST_ID_ADD(clause->type);
            LIST_ID_ADD(clause->numeric);
            LIST_ID_ADD(clause->numeric_data);
            for (const char *p = clause->str; p && *p; p++)
                LIST_ID_ADD(*p);
        }
    }
#undef LIST_ID_ADD
    return id;
}

static bool tagtree_list_is_all_songs(void)
{
    return csi->tagorder_count == 1 && csi->tagorder[0] == tag_title
           && csi->clause_count[0] == 0;
}

/* what spares writing the file just written the next time */
static void tagtree_save_list_index(const struct tree_context *c, int slot,
                                    uint32_t id)
{
    if (!written_offsets)
        return;
    struct tagtree_list_header h = {
        TAGTREE_LIST_MAGIC, tagcache_get_commitid(), c->filesindir, id,
        written_count
    };
    int fd = open(tagtree_list_index_names[slot], O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if (fd >= 0)
    {
        write(fd, &h, sizeof(h));
        write(fd, written_offsets, written_count * sizeof(uint32_t));
        close(fd);
    }
}

/* The list's playlist from a file written for it before, or from a new one.
   A list of the whole library took over four seconds each time. */
/* The place of the row's track in the current playlist, if it is the list's
   written to that slot, as it was loaded, and not to be shuffled: -1 if not */
static int tagtree_place_playing(int slot, int fd,
                                 const struct tagtree_list_header *h, int row)
{
    uint32_t seek;
    if (global_settings.playlist_shuffle
        || h->count != h->entries || row < 0 || row >= (int)h->count
        || lseek(fd, sizeof(*h) + row * sizeof(seek), SEEK_SET) < 0
        || read(fd, &seek, sizeof(seek)) != sizeof(seek))
        return -1;
    return playlist_place_of(TAGTREE_PLAYLIST_DIR, tagtree_playlist_names[slot],
                             h->count, seek);
}

/* The list as the current playlist, its row at *start_index: the playlist
   written for it before, or written now. If it's that one playing already,
   unshuffled, it goes on to the row's track there, and *start_index is that
   one's place. */
/* the playlist's file of the list last loaded as the current one, or empty */
static char loaded_file[MAX_PATH];

static int tagtree_load_list(struct tree_context *c, int *start_index)
{
    struct tagtree_list_header h;
    loaded_file[0] = '\0';
    const uint32_t id = tagtree_list_id(c->currtable, c->currextra, csi);
    const int32_t commitid = tagcache_get_commitid();

    for (int i = 0; i < TAGTREE_SLOTS; i++)
    {
        int fd = open(tagtree_list_index_names[i], O_RDONLY);
        if (fd < 0)
            continue;
        bool same = read(fd, &h, sizeof(h)) == sizeof(h)
            && h.magic == TAGTREE_LIST_MAGIC && h.commitid == commitid
            && h.entries == (uint32_t)c->filesindir && h.id == id;
        if (same)
        {
            const int place = tagtree_place_playing(i, fd, &h, *start_index);
            if (place >= 0)
            {
                close(fd);
                *start_index = place;
                snprintf(loaded_file, sizeof(loaded_file), "%s/%s",
                         TAGTREE_PLAYLIST_DIR, tagtree_playlist_names[i]);
                return 0;
            }
            same = lseek(fd, sizeof(h), SEEK_SET) == sizeof(h)
                && playlist_create_from_index(TAGTREE_PLAYLIST_DIR,
                                              tagtree_playlist_names[i], fd,
                                              h.count) == 0;
        }
        close(fd);
        if (same)
        {
            snprintf(loaded_file, sizeof(loaded_file), "%s/%s",
                     TAGTREE_PLAYLIST_DIR, tagtree_playlist_names[i]);
            return 0;
        }
    }

    char file[MAX_PATH];
    int slot = tagtree_free_slot(tagtree_list_is_all_songs()
                                 ? TAGTREE_SLOT_SONGS : 0, file, sizeof(file));
    const char *name = tagtree_playlist_names[slot];
    if (!insert_all_playlist(c, file, true, PLAYLIST_INSERT_LAST, false))
        return -2;

    tagtree_save_list_index(c, slot, id);
    if ((!written_offsets
         || playlist_create_indexed(TAGTREE_PLAYLIST_DIR, name, written_offsets,
                                    written_count) < 0)
        && playlist_create(TAGTREE_PLAYLIST_DIR, name) < 0)
    {
        logf("Failed creating playlist\n");
        return -1;
    }
    strmemccpy(loaded_file, file, sizeof(loaded_file));
    return 0;
}

/* the list of all songs in the main menu, if there is one */
static struct search_instruction *tagtree_all_songs(void)
{
    if (!tagnavi_loaded || rootmenu < 0 || !menus[rootmenu]
        || !tagcache_is_usable())
        return NULL;

    const struct menu_root *root = menus[rootmenu];
    for (int i = 0; i < root->itemcount; i++)
    {
        struct search_instruction *si = &root->items[i]->si;
        if (root->items[i]->type == menu_next && si->tagorder_count == 1
            && si->tagorder[0] == tag_title && si->clause_count[0] == 0)
            return si;
    }
    return NULL;
}

/* The slot of the playlist of that list, written for this database, and its
   index open after its header: -1 without one. */
static int tagtree_songs_slot(const struct search_instruction *songs,
                              struct tagtree_list_header *h, int *fd)
{
    const uint32_t id = tagtree_list_id(TABLE_NAVIBROWSE, 0, songs);
    for (int i = TAGTREE_SLOT_SONGS; i < TAGTREE_SLOTS; i++)
    {
        *fd = open(tagtree_list_index_names[i], O_RDONLY);
        if (*fd < 0)
            continue;
        if (read(*fd, h, sizeof(*h)) == sizeof(*h)
            && h->magic == TAGTREE_LIST_MAGIC && h->id == id
            && h->commitid == tagcache_get_commitid())
            return i;
        close(*fd);
    }
    return -1;
}

/* the place of a line in a playlist file, -1 if it isn't there */
static int tagtree_find_in_playlist(const char *file, const char *line)
{
    static char block[4096];
    const size_t len = strlen(line);
    size_t have = 0;
    int number = 0;
    int result = -1;
    int fd = open_utf8(file, O_RDONLY);
    if (fd < 0)
        return -1;

    while (result < 0)
    {
        ssize_t got = read(fd, block + have, sizeof(block) - have);
        if (got <= 0)
            break;
        have += got;
        char *start = block, *end = block + have, *nl;
        while ((nl = memchr(start, '\n', end - start)) != NULL)
        {
            if ((size_t)(nl - start) == len && !memcmp(start, line, len))
            {
                result = number;
                break;
            }
            number++;
            start = nl + 1;
        }
        have = end - start;
        memmove(block, start, have);
        /* longer than any line should be */
        if (have == sizeof(block))
            break;
    }
    close(fd);
    return result;
}

/* Plays this song within the list of all songs, from the playlist made for it
   at the import. False if there isn't one, or it isn't in there. Needs no buffer
   but its own, so a plugin may call it. */
bool tagtree_play_song_in_library(const char *path)
{
    struct tagtree_list_header h;
    char file[MAX_PATH];
    int fd;
    const struct search_instruction *songs = tagtree_all_songs();
    if (!songs)
        return false;
    int slot = tagtree_songs_slot(songs, &h, &fd);
    if (slot < 0)
        return false;

    cpu_boost(true);
    snprintf(file, sizeof(file), "%s/%s", TAGTREE_PLAYLIST_DIR,
             tagtree_playlist_names[slot]);
    int index = tagtree_find_in_playlist(file, path);
    bool ok = index >= 0 && index < (int)h.count;
    if (ok)
    {
        /* as when playing a list: at once, without the fade */
        if (audio_status() & AUDIO_STATUS_PLAY)
            audio_stop_at_once();
        ok = playlist_create_from_index(TAGTREE_PLAYLIST_DIR,
                                        tagtree_playlist_names[slot], fd,
                                        h.count) == 0;
    }
    close(fd);
    if (ok)
    {
        if (global_settings.playlist_shuffle)
        {
            index = playlist_shuffle(0, index);
            if (!global_settings.play_selected)
                index = 0;
        }
        playlist_start(index, 0, 0);
        tagtree_note_queue(file, h.id);
    }
    cpu_boost(false);
    return ok;
}

/* At the end of an import: the playlist of the list of all songs, which takes
   seconds to write, so that playing a song of it starts at once. */
void tagtree_prepare_songs(void)
{
    struct tree_context *c = tree_get_context();
    struct search_instruction *songs = tagtree_all_songs();
    if (!songs)
        return;

    /* as the browser has it for that list, and back after */
    struct search_instruction *old_csi = csi;
    const int table = c->currtable, extra = c->currextra;
    const int files = c->filesindir, length = c->dirlength;
    const int dirs = c->dirsindir, special = c->special_entry_count;
    csi = songs;
    c->currtable = TABLE_NAVIBROWSE;
    c->currextra = 0;
    tc = c;

    const uint32_t id = tagtree_list_id(c->currtable, c->currextra, csi);
    struct tagtree_list_header h;
    int fd;
    bool ready = tagtree_songs_slot(songs, &h, &fd) >= 0;
    if (ready)
        close(fd);

    if (!ready)
    {
        tree_lock_cache(c);
        core_pin(tagtree_handle);
        cpu_boost(true);
        list_on_boot_screen = true;
        library_update_songs_progress(0, 0);
        int count = retrieve_entries(c, 0, true);
        if (count > 0)
        {
            char file[MAX_PATH];
            int slot = tagtree_free_slot(TAGTREE_SLOT_SONGS, file, sizeof(file));
            c->filesindir = c->dirlength = count;
            if (insert_all_playlist(c, file, true, PLAYLIST_INSERT_LAST, false))
                tagtree_save_list_index(c, slot, id);
        }
        /* and Songs opens with it at once */
        if (count > 0 && entries_whole)
            keep_list(c, count);
        else
            kept_hash = 0;
        list_on_boot_screen = false;
        cpu_boost(false);
        core_unpin(tagtree_handle);
        tree_unlock_cache(c);
    }

    csi = old_csi;
    c->currtable = table;
    c->currextra = extra;
    c->filesindir = files;
    c->dirlength = length;
    c->dirsindir = dirs;
    c->special_entry_count = special;
}

static int tagtree_play_folder(struct tree_context* c)
{
    logf( "%s", __func__);
    int start_index = c->selected_item;
    /* Now Playing takes half a second to a second to come, and the slide
       into it waited for half */
    transition_wait(HZ);

    /* The music playing would change anyway; stopped first, it doesn't
       take turns with building the playlist */
    if (audio_status() & AUDIO_STATUS_PLAY)
        audio_stop_at_once();

    int n = c->filesindir;
    loaded_file[0] = '\0';
    if (n <= playlist_get_current()->max_playlist_size)
    {
        int result = tagtree_load_list(c, &start_index);
        if (result < 0)
            return result;
    }
    else if (playlist_create(NULL, NULL) < 0)
    {
        logf("Failed creating playlist\n");
        return -1;
    }
    else if (!insert_all_playlist(c, NULL, false, PLAYLIST_INSERT_LAST, false))
        return -2;

    bool has_playlist_been_randomized = n > playlist_get_current()->max_playlist_size;
    if (has_playlist_been_randomized)
    {
        /* We need to recalculate the start index based on a percentage to put the user
        around its desired start position and avoid out of bounds */

        int percentage_start_index = 100 * start_index / n;
        start_index = percentage_start_index * playlist_get_current()->amount / 100;
    }

    if (global_settings.playlist_shuffle)
    {
        start_index = playlist_shuffle(0, start_index);
        if (!global_settings.play_selected)
            start_index = 0;
    }

    playlist_start(start_index, 0, 0);
    /* the list the music is now from, for its speaker */
    tagtree_note_queue(loaded_file, loaded_file[0]
                       ? tagtree_list_id(c->currtable, c->currextra, csi) : 0);
    if (kept_hash) /* as it is now, the playlist made from it */
        keep_list(c, kept_count);
    return 0;
}

static struct tagentry* tagtree_get_entry(struct tree_context *c, int id)
{
    struct tagentry *entry;
    int realid = id - current_offset;

    if (c->currtable == TABLE_ROOT)
        return &root_entries[MAX(0, MIN(id, TAGMENU_MAX_ITEMS - 1))];

    /* Load the next chunk if necessary. */
    if (realid >= current_entry_count || realid < 0)
    {
        cpu_boost(true);
        if (retrieve_entries(c, MAX(0, id - (current_entry_count / 2)),
                             false) < 0)
        {
            logf("retrieve failed");
            cpu_boost(false);
            return NULL;
        }
        realid = id - current_offset;
        cpu_boost(false);
    }

    entry = get_entries(c);
    return &entry[realid];
}

char* tagtree_get_entry_name(struct tree_context *c, int id,
                                    char* buf, size_t bufsize)
{
    struct tagentry *entry = tagtree_get_entry(c, id);
    if (!entry)
        return NULL;

    unsigned char *name = entry->name;

    int lang_id = P2ID(name);
    logf("%s: '%s' id: %d\n", __func__,
         P2STR(name), lang_id);
    if (lang_id >= 0)
    {
        strmemccpy(buf,P2STR(name), bufsize);
        return entry->name;
    }

    strmemccpy(buf, entry->name, bufsize);
    return buf;
}


char *tagtree_get_title(struct tree_context* c)
{
    switch (c->currtable)
    {
        case TABLE_ROOT:
            logf("%s (ROOT) %s", __func__,  P2STR(menu->title));
            return P2STR(menu->title);

        case TABLE_NAVIBROWSE:
        case TABLE_ALLSUBENTRIES:
        case TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS:
            logf("%s (NAVI/ALLSUB) idx: %d %s", __func__,
                 c->currextra, current_title[c->currextra]);
            return current_title[c->currextra];
    }

    return "?";
}

/* The row is the song playing, paused too, of the list the music playing is
   from: in another list it plays as any other, its own list then. playing is
   its id3's tagcache_idx. */
bool tagtree_row_is_playing(struct tree_context *c, int row, long playing)
{
    if (playing <= 0 || c->currtable == TABLE_ROOT || row < 0 || row >= c->filesindir
        || !csi)
        return false;
    const struct tagentry *entry = tagtree_get_entry(c, row);
    if (!entry || entry->newtable != TABLE_PLAYTRACK || entry->extraseek + 1 != playing)
        return false;
    const uint32_t queue = tagtree_queue_list();
    return queue && queue == tagtree_list_id(c->currtable, c->currextra, csi);
}

int tagtree_get_attr(struct tree_context* c)
{
    int attr = -1;
    switch (c->currtable)
    {
        case TABLE_NAVIBROWSE:
            if (csi->tagorder[c->currextra] == tag_title
                || csi->tagorder[c->currextra] == tag_virt_basename)
                attr = FILE_ATTR_AUDIO;
            else
                attr = ATTR_DIRECTORY;
            break;

        case TABLE_ALLSUBENTRIES:
        case TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS:
            attr = FILE_ATTR_AUDIO;
            break;

        default:
            attr = ATTR_DIRECTORY;
            break;
    }

    return attr;
}

int tagtree_get_icon(struct tree_context* c)
{
    int icon = Icon_Folder;

    if (tagtree_get_attr(c) == FILE_ATTR_AUDIO)
        icon = Icon_Audio;

    return icon;
}
