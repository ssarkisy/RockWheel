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

/* reuse of tagtree data after tagtree_play_folder() */
static uint32_t loaded_entries_crc = 0;


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

static uint32_t tagtree_data_crc(struct tree_context* c)
{
    char* buf;
    uint32_t crc;
    buf = core_get_data(tagtree_handle); /* data for the search clauses etc */
    crc = crc_32(buf, tagtree_buf_used, c->dirlength);
    buf = core_get_data(c->cache.name_buffer_handle); /* names */
    /* of their start, which anything else using them writes first: the whole
       of them took a tenth of a second and more as each song started */
    crc = crc_32(buf, MIN(c->cache.name_buffer_size, 32*1024), crc);
    buf = core_get_data(c->cache.entries_handle); /* tagentries */
    crc = crc_32(buf, MIN(c->cache.max_entries * sizeof(struct tagentry),
                          32*1024), crc);
    logf("%s 0x%x", __func__, crc);
    return crc;
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
       and Cover Flow, as the first item of an iPod Classic's */
    if (entry->type == menu_playlists || entry->type == menu_coverflow)
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

    /* Do not gather data unless proper setting has been enabled. */
    if (!runtimedb && !autoresume)
        return;

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

        playcount = id3->playcount + 1;

        /* Ignore the last 15s (crossfade etc.) */
        playtime = id3->playtime + MIN(id3->length, id3->elapsed + 15 * 1000);

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

    remove_event(PLAYBACK_EVENT_TRACK_BUFFER, tagtree_buffer_event);
    remove_event(PLAYBACK_EVENT_TRACK_FINISH, tagtree_track_finish_event);

    if (c)
    {
        tree_lock_cache(c);
        struct tagentry *dptr = core_get_data(c->cache.entries_handle);
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

static int retrieve_entries(struct tree_context *c, int offset, bool init)
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

    fmt = NULL;
    for (i = 0; i < format_count; i++)
    {
        if (c->currtable == TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS)
        {
            /* If it is sorted by albums, we need to use the proper view
            that includes the disc number etc so sorting will be correct for each albums.
            Otherwise, tracks will be sorted by title names */
            if (formats[i]->group_id != csi->format_id[level + 1])
                continue;
        }
        else if (formats[i]->group_id != csi->format_id[level])
            continue;
        fmt = formats[i];
    }

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
        /* Check the format */
        for (i = 0; i < format_count; i++)
        {
            if (c->currtable == TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS)
            {
                /* If it is sorted by albums, we need to use the proper view
                that includes the disc number etc so sorting will be correct for each albums.
                Otherwise, tracks will be sorted by title names */
                if (formats[i]->group_id != csi->format_id[level + 1])
                    continue;
            }
            else if (formats[i]->group_id != csi->format_id[level])
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
                    if ((c->cache.name_buffer_size - namebufused) > 0 &&
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
                    if ((c->cache.name_buffer_size - namebufused) > 0 &&
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
                    if (tagcache_retrieve(&tcs, tcs.idx_id, tag_album, dptr->album_name,
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

    while (tagcache_get_next(&tcs, tcs_buf, tcs_bufsz))
    {
        if (!tagtree_search_progress(total_count))
            break;
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

    return total_count;

}

static int load_root(struct tree_context *c)
{
    struct tagentry *dptr = core_get_data(c->cache.entries_handle);
    int i;

    tc = c;
    c->currtable = TABLE_ROOT;
    if (c->dirlevel == 0)
        c->currextra = rootmenu;

    menu = menus[c->currextra];
    if (menu == NULL)
        return 0;

    if (menu->itemcount > c->cache.max_entries)
            panicf("%s tree_cache too small", __func__);

    for (i = 0; i < menu->itemcount; i++)
    {

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
        }

        dptr++;
    }

    current_offset = 0;
    current_entry_count = i;

    return i;
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

            if (loaded_entries_crc != 0)
            {
                if (loaded_entries_crc == tagtree_data_crc(c))
                {
                    count = c->dirlength;
                    logf("Reusing %d entries", count);
                    break;
                }
            }

            cpu_boost(true);
            count = retrieve_entries(c, 0, true);
            cpu_boost(false);
            break;

        default:
            logf("Unsupported table %d\n", table);
            return -1;
    }

    loaded_entries_crc = 0;

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
/* The place in the lists where the music playing was started, to go back to it */
static struct
{
    struct menu_root *menu;
    struct search_instruction *csi;
    int result_seek[MAX_TAGS];
    int currtable, currextra, dirlevel, selected_item, max_history_level;
    int selected_item_history[MAX_DIR_LEVELS];
    int table_history[MAX_DIR_LEVELS];
    int extra_history[MAX_DIR_LEVELS];
} saved_place;

void tagtree_save_place(int dirlevel, int selected_item)
{
    saved_place.menu = menu;
    saved_place.csi = csi;
    if (csi)
        memcpy(saved_place.result_seek, csi->result_seek, sizeof(csi->result_seek));
    saved_place.currtable = tc->currtable;
    saved_place.currextra = tc->currextra;
    saved_place.dirlevel = dirlevel;
    saved_place.selected_item = selected_item;
    saved_place.max_history_level = max_history_level;
    memcpy(saved_place.selected_item_history, selected_item_history,
           sizeof(selected_item_history));
    memcpy(saved_place.table_history, table_history, sizeof(table_history));
    memcpy(saved_place.extra_history, extra_history, sizeof(extra_history));
}

void tagtree_restore_place(int *dirlevel, int *selected_item)
{
    menu = saved_place.menu;
    csi = saved_place.csi;
    if (csi)
        memcpy(csi->result_seek, saved_place.result_seek, sizeof(csi->result_seek));
    tc->currtable = saved_place.currtable;
    tc->currextra = saved_place.currextra;
    *dirlevel = saved_place.dirlevel;
    *selected_item = saved_place.selected_item;
    max_history_level = saved_place.max_history_level;
    memcpy(selected_item_history, saved_place.selected_item_history,
           sizeof(selected_item_history));
    memcpy(table_history, saved_place.table_history, sizeof(table_history));
    memcpy(extra_history, saved_place.extra_history, sizeof(extra_history));
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
                splash_progress(i, n, "%s (%s)", str(LANG_WAIT), str(LANG_OFF_ABORT));
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
            splash_progress(i, n, "%s (%s)", str(LANG_WAIT), str(LANG_OFF_ABORT));
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
static int tagtree_load_list(struct tree_context *c)
{
    struct tagtree_list_header h;
    const uint32_t id = tagtree_list_id(c->currtable, c->currextra, csi);
    const int32_t commitid = tagcache_get_commitid();

    for (int i = 0; i < TAGTREE_SLOTS; i++)
    {
        int fd = open(tagtree_list_index_names[i], O_RDONLY);
        if (fd < 0)
            continue;
        bool same = read(fd, &h, sizeof(h)) == sizeof(h)
            && h.magic == TAGTREE_LIST_MAGIC && h.commitid == commitid
            && h.entries == (uint32_t)c->filesindir && h.id == id
            && playlist_create_from_index(TAGTREE_PLAYLIST_DIR,
                                          tagtree_playlist_names[i], fd,
                                          h.count) == 0;
        close(fd);
        if (same)
            return 0;
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
        /* as when playing a list */
        if (audio_status() & AUDIO_STATUS_PLAY)
            audio_stop();
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
    loaded_entries_crc = 0;
}

static int tagtree_play_folder(struct tree_context* c)
{
    logf( "%s", __func__);
    int start_index = c->selected_item;

    /* The music playing would change anyway; stopped first, it doesn't
       take turns with building the playlist */
    if (audio_status() & AUDIO_STATUS_PLAY)
        audio_stop();

    int n = c->filesindir;
    if (n <= playlist_get_current()->max_playlist_size)
    {
        int result = tagtree_load_list(c);
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
    loaded_entries_crc = tagtree_data_crc(c); /* save crc in case we return */
    return 0;
}

static struct tagentry* tagtree_get_entry(struct tree_context *c, int id)
{
    struct tagentry *entry;
    int realid = id - current_offset;

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
