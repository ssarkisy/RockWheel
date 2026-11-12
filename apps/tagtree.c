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
#include <errno.h>
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
#include "sortkey.h"
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
#include "album_years.h"
#include "list_covers.h"

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
/* another when the lists' songs are put in another order: an index of the
   order before played other songs than the rows chosen */
#define TAGTREE_LIST_MAGIC 0x4c545453 /* "STTL" */
struct tagtree_list_header {
    uint32_t magic;
    int32_t commitid;   /* of the database */
    uint32_t entries;   /* the list's songs, past its rows at the top */
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

/* Which list each slot's file holds, of which database, as its index says:
   read at the start, while the disk is up, and kept as each index is written
   or removed. So the list the music is from is known without the disk, after
   a boot too: read as a list was drawn, with the disk asleep it waited for it
   seconds, and was left unknown, the speaker missing from its list. */
static struct {
    uint32_t id;        /* 0 for none */
    int32_t commitid;
} slot_lists[TAGTREE_SLOTS];

static void slot_lists_read(void)
{
    for (int i = 0; i < TAGTREE_SLOTS; i++)
    {
        struct tagtree_list_header h;
        slot_lists[i].id = 0;
        int fd = open(tagtree_list_index_names[i], O_RDONLY);
        if (fd < 0)
            continue;
        if (read(fd, &h, sizeof(h)) == sizeof(h) && h.magic == TAGTREE_LIST_MAGIC)
        {
            slot_lists[i].id = h.id;
            slot_lists[i].commitid = h.commitid;
        }
        close(fd);
    }
}

bool tagtree_is_list_file(const char *path)
{
    for (int i = 0; i < TAGTREE_SLOTS; i++)
    {
        char slot[MAX_PATH];
        snprintf(slot, sizeof(slot), "%s/%s", TAGTREE_PLAYLIST_DIR,
                 tagtree_playlist_names[i]);
        if (!strcasecmp(path, slot))
            return true;
    }
    return false;
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
   says which list, of this database (slot_lists). */
static uint32_t tagtree_queue_list(void)
{
    char current[MAX_PATH];
    if (!playlist_get_name(NULL, current, sizeof(current)))
        current[0] = '\0';
    if (queue_starts == playlist_start_count()
        && queue_commit == tagcache_get_commitid()
        && !strcasecmp(queue_file, current))
        return queue_id;
    uint32_t id = 0;
    for (int i = 0; i < TAGTREE_SLOTS && current[0]; i++)
    {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", TAGTREE_PLAYLIST_DIR,
                 tagtree_playlist_names[i]);
        if (strcasecmp(current, path))
            continue;
        if (slot_lists[i].commitid == tagcache_get_commitid())
            id = slot_lists[i].id;
        break;
    }
    tagtree_note_queue(current, id);
    return id;
}

/* a slot's file is of a list of this database, to load again */
static bool slot_holds_list(int slot)
{
    return slot_lists[slot].id
           && slot_lists[slot].commitid == tagcache_get_commitid();
}

/* The one of a pair, from first, which is free to be written, as a full path
   into buf: not the one in use, and of the two the one that holds no list,
   so that Songs in two orders keeps both. Its index goes first, so that it
   never goes with another file. */
static int tagtree_free_slot(int first, char *buf, size_t size)
{
    int slot = first;
    if (tagtree_slot_in_use(first)
        || (slot_holds_list(first) && !slot_holds_list(first + 1)
            && !tagtree_slot_in_use(first + 1)))
        slot = first + 1;
    snprintf(buf, size, "%s/%s", TAGTREE_PLAYLIST_DIR,
             tagtree_playlist_names[slot]);
    remove(tagtree_list_index_names[slot]);
    slot_lists[slot].id = 0;
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
static bool kept_dirfull, kept_years, kept_two_lines, kept_split;
static int kept_tracks, kept_groups;
static int32_t kept_untagged;
static int kept_initials, kept_initials_tag;
static int kept_shows;
static bool kept_episodes;
/* the rows of the list made are albums with their years */
static bool entries_years;
/* each in two lines, its name over its year and its album artist, with its
   cover (struct tagentry's album): as Artists in Albums and Covers in Lists
   have them */
static bool entries_two_lines;
/* and the albums of a name of several album artists are each a row of its
   own, as Artists in Albums has them in Albums */
static bool entries_split;
/* the rows go by Quick Scroll's letters (sortkey_initial()), as they're in
   that order: of each row's place in the file of the list's tag, a song's of its
   title, from what the commit sorted by; or of its name, as songs sorted
   without their places. None for a list in another order. A list by years
   shows the years. */
enum { INITIALS_NONE, INITIALS_BY_PLACE, INITIALS_BY_NAME };
static int entries_initials;
static int entries_initials_tag;
/* or songs album by album, each album's by disc and track, with their
   numbers: the digits of the largest, 0 for a list without them */
static int entries_tracks;
/* the grey bars among its songs, as Settings' groups: an album's discs, All
   Songs' albums (struct song_group); 0 for a list without them */
static int entries_groups;
/* the list's row of the songs without its tag, by its seek, as a tag may have
   the text that row shows: -1 if none, and in a list of songs, whose seeks are
   of another kind */
static int32_t entries_untagged = -1;
/* Podcasts' shows in two lines: how many struct show_sums are kept; 0
   otherwise */
static int entries_shows;
/* a show's episodes: newest first, two lines, no covers */
static bool entries_episodes;
/* The shows are Audiobooks' books, with a book's sums. And a book's own list,
   its parts in their order: how many, the row of the one begun or -1 (past
   the rows at the top), whether all are heard, and their length in ms */
static bool entries_books;
static struct book_parts {
    int parts, current;
    bool finished;
    long length;
} entries_parts;
static bool kept_books;
static struct book_parts kept_parts;
/* the book whose screen was made last, and the rows over its parts then */
static int32_t parts_album = -1;
static int parts_special;
/* the list made had all its rows, as it wasn't left before the end */
static bool entries_whole;
/* and goes by play statistics, so isn't kept: they change without a new
   database */
static bool entries_of_stats;
/* its songs are one album's, as its screen shows them under a header
   (tagtree_album()): songs 0 for a list that isn't. Made from its rows as it
   loads, as one kept is shown again too */
static struct tagtree_album album_view;


/* this needs to be same size as struct entry (tree.h) and name needs to be
 * the first; so that they're compatible enough to walk arrays of both
 * derefencing the name member*/
struct tagentry {
    char* name;
    int newtable;
    int extraseek;
    int customaction;
    union {
        char* album_name;   /* a song's, sorted by its album */
        /* an album's, in a list with years: its year, and in a list of two
           lines its number in the table of years (album_years.h), whose
           cover and album artist the row shows; -1 for none. A show's has
           its sums' number in the year's place (struct show_sums) */
        struct {
            int16_t year, no;
        } album;
        /* and then, in a list of one line, or a song's number, as the text
           before the row's name, which the list doesn't make again at each
           frame: its digits, without an end if they're four; empty if it
           hasn't one */
        char lead[4];
    };
};
/* the year of a row that isn't an album, the songs without one: last */
#define YEAR_LAST (-1)

/* A bar over a list's songs, from the one it's over: "Disc 2" over an album's
   second disc, "2009 · The Fame" or "2019 · Singles" in All Songs. Not one of
   the list's entries: tree.c puts it among their rows. Made with the list, at
   the end of the names' room, the first last, so that nothing is read for it
   as it's drawn */
struct song_group {
    int32_t row;    /* the entry it is over */
    int32_t album;  /* its name's place in the names' room, or one of these */
    int32_t year;   /* 0 for none */
    int32_t disc;   /* 0 for none */
};
#define GROUP_NO_ALBUM (-1) /* a disc of the album the list is of */
#define GROUP_SINGLES  (-2) /* the albums of a year with one song each */
#define GROUP_UNKNOWN  (-3) /* the songs without an album */

/* the rows of the menu shown, out of the lists' buffer, which keeps the list */
static struct tagentry root_entries[TAGMENU_MAX_ITEMS];
static int root_rows;

static struct tagentry* tagtree_get_entry(struct tree_context *c, int id);
static int tagtree_find_row(struct tree_context *c, const char *name);
static bool shuffle_row_wanted(struct tree_context *c);
struct search_instruction;
static bool section_is(const struct search_instruction *si, int lang);
static int32_t playing_entry(unsigned long *elapsed);

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
/* and Music's rules (sortkey.h) */
static int sortkey_compare_n(const char *a, const char *b, size_t n)
{
    (void)n;
    return sortkey_compare(a, b);
}
static int sortkey_compare_n_inv(const char *a, const char *b, size_t n)
{
    (void)n;
    return sortkey_compare(b, a);
}

/* Rows go by their places in the tag files, which the commit sorted by
   Music's rules and the Sort fields (sortkey.h, tagcache.h): a row's seek
   there, or a song's from the database in memory, a number to compare rather
   than two names. Not rows of a format's names, which sort by what they
   show, as "fmt_title" by disc and track, as qsort_fn has it. */
static bool sort_by_place;
static bool sort_places_inverse;
static int sort_places_tag; /* a song's title, or the tag of the rows' seeks */

/* the songs' places are known, from the database in memory */
static bool song_places_known(void)
{
#ifdef HAVE_TC_RAMCACHE
    return tagcache_is_in_ram();
#else
    return false;
#endif
}

/* Music > Songs' Sort By, global_settings.songs_sort */
enum { SONGS_BY_NAME, SONGS_BY_ARTIST, SONGS_BY_ALBUM, SONGS_BY_YEAR };

static bool album_numbers_fit(void);

/* the songs' keys are their albums' numbers in the table of years and their
   tags' places in the database's memory */
static bool songs_sortable(void)
{
    return song_places_known() && album_years_ready() && album_numbers_fit();
}

static int songs_setting(void)
{
    const int order = global_settings.songs_sort;
    return order >= SONGS_BY_NAME && order <= SONGS_BY_YEAR ? order : SONGS_BY_NAME;
}

static int songs_order(void)
{
    return songs_sortable() ? songs_setting() : SONGS_BY_NAME;
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
        /* and of the row chosen at each level, an album of a name several
           album artists have, its album artist's: the songs are of it too.
           -1 for another row */
        int result_artist[MAX_TAGS];
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

/* Music > Songs: all songs, by their titles */
static bool is_songs(const struct search_instruction *si)
{
    return si && si->tagorder_count == 1 && si->tagorder[0] == tag_title
           && si->clause_count[0] == 0;
}

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
   One the file has that isn't here is always shown, but Recently Added, by
   its own setting. */
const int tagtree_section_lang[MUSIC_SECTIONS] = {
    LANG_COVER_FLOW, LANG_SEARCH, LANG_PLAYLISTS, LANG_ARTISTS, LANG_ALBUMS,
    LANG_SONGS, LANG_PODCASTS, LANG_GENRES, LANG_COMPOSERS, LANG_AUDIOBOOKS,
};
/* every item of Music's own menu, as when it's gone into past its list */
static bool root_all_items;
/* since the shown ones changed, the rows to go back to are of others */
static bool root_rows_changed;

/* Recently Added's row, by a setting of its own: only with albums to list,
   the albums' table made. And as Music's menu last had it, -1 before that:
   the rows below it move as it comes or goes (tagtree_albums_reordered()) */
static bool recent_row(void)
{
    return global_settings.music_menu_recently_added
           && album_years_recent_count() > 0;
}
static int recent_row_seen = -1;

static bool root_item_shown(const struct menu_entry *item)
{
    const int lang = P2ID((const unsigned char *)item->name);
    if (lang == LANG_RECENTLY_ADDED)
        return recent_row();
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

/* A place in the lists, to go back to */
struct tagtree_place
{
    struct menu_root *menu;
    struct search_instruction *csi;
    int result_seek[MAX_TAGS], result_artist[MAX_TAGS];
    int currtable, currextra, dirlevel, selected_item, max_history_level;
    int selected_item_history[MAX_DIR_LEVELS];
    int table_history[MAX_DIR_LEVELS];
    int extra_history[MAX_DIR_LEVELS];
    char current_title[MAX_TAGS][sizeof(current_title[0])];
};

/* where the music playing was started, and where the lists were before they were
   opened at the track playing */
static struct tagtree_place saved_place, go_to_place;
/* the song a list was opened at in place from, by Go to Artist or Go to Album
   in its menu, to go back to (tagtree_browse_from_row()); and a place held for
   a moment */
static struct tagtree_place browse_place, scratch_place;

/* Artists' search with the other artists at its top, out of tagtree's buffer, as
   go_to_row() makes it: [0] of the albums' artists, [1] of the songs'. Each is
   made again only as it was, so that a place kept in one stays of its tag. With
   the item of Music's own menu each is of, and the name of the row Menu left
   one's top by, which that item's own list is opened at instead. */
static struct search_instruction artists_copy[2];
static int artists_copy_item[2];
static char artists_copy_left[sizeof(current_title[0])];

/* a few memory alloc helper */
static int tagtree_handle;
static size_t tagtree_bufsize, tagtree_buf_used;

/* whether p points into the buffer, as it was at start */
static bool in_buffer(const void *p, const void *start)
{
    return (const char *)p >= (const char *)start
           && (const char *)p < (const char *)start + tagtree_bufsize;
}

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

    /* the search, unless it's a copy, and the places, which point into it too */
    if (csi && in_buffer(csi, current))
        UPDATE(csi, diff);
    struct tagtree_place *const places[] = {
        &saved_place, &go_to_place, &browse_place, &scratch_place
    };
    for (size_t i = 0; i < ARRAYLEN(places); i++)
    {
        if (places[i]->menu && in_buffer(places[i]->menu, current))
            UPDATE(places[i]->menu, diff);
        if (places[i]->csi && in_buffer(places[i]->csi, current))
            UPDATE(places[i]->csi, diff);
    }
    /* the copies' clauses, which are the menus' */
    for (size_t i = 0; i < ARRAYLEN(artists_copy); i++)
    {
        struct search_instruction *si = &artists_copy[i];
        for (int k = 0; k < si->tagorder_count; k++)
        {
            for (int l = 0; l < si->clause_count[k]; l++)
            {
                if (in_buffer(si->clause[k][l], current))
                    UPDATE(si->clause[k][l], diff);
            }
        }
    }

    /* the menu's rows, by their names here */
    for (int i = 0; i < root_rows; i++)
    {
        if (in_buffer(root_entries[i].name, current))
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

/* The names of the rows of the songs without a tag, untagged_name()'s: a
   list's rows point at them, in the language's buffer */
static const int untagged_phrases[] = {
    LANG_TAGNAVI_UNTAGGED, LANG_UNKNOWN_ARTIST, LANG_UNKNOWN_ALBUM,
    LANG_UNKNOWN_GENRE,
};

/* The row of the songs without the tag a list is of, by the name the Music
   app gives it, as "Unknown Artist"; "[Untagged]" for a tag of one's own
   tagnavi */
static const char *untagged_name(int tag)
{
    switch (tag)
    {
        case tag_virt_canonicalartist:
        case tag_artist:
        case tag_albumartist:
            return str(LANG_UNKNOWN_ARTIST);
        case tag_album:
            return str(LANG_UNKNOWN_ALBUM);
        case tag_genre:
            return str(LANG_UNKNOWN_GENRE);
    }
    return str(LANG_TAGNAVI_UNTAGGED);
}

/* The list in the lists' buffer, which list it is and of which database */
static uint32_t tagtree_data_crc(struct tree_context* c)
{
    /* the years and their order in lists of albums, All Songs in its albums'
       order, and Songs, whose Sort By goes by them */
    const bool of_albums = c->currtable == TABLE_NAVIBROWSE && csi
                           && c->currextra < MAX_TAGS
                           && csi->tagorder[c->currextra] == tag_album;
    const bool all_songs = c->currtable == TABLE_ALLSUBENTRIES;
    const bool songs = c->currtable == TABLE_NAVIBROWSE && c->currextra == 0
                       && is_songs(csi);
    const bool podcasts = csi && c->currtable != TABLE_ROOT
                          && (section_is(csi, LANG_PODCASTS)
                              || section_is(csi, LANG_AUDIOBOOKS));
    const uint32_t list[] = {
        c->currtable, c->currextra, (uintptr_t)csi, tagcache_get_commitid(),
        global_settings.interpret_numbers,
        of_albums || all_songs || songs ? album_years_generation() : 0,
        songs ? songs_order() : 0,
        of_albums ? global_settings.albums_sort : 0,
        /* their rows in one line or two, split by album artists or not */
        of_albums ? global_settings.album_artists : 0,
        of_albums ? list_covers_on() : 0,
        all_songs ? global_settings.all_songs_sort : 0,
        /* and Shuffle Songs at its top, as Shuffle in Lists has it */
        csi && c->currtable != TABLE_ROOT ? shuffle_row_wanted(c) : 0,
        /* the episodes' states, which a show's row sums: their places,
           plays and marks, and the one playing, started as it plays */
        podcasts ? tagcache_states_generation() : 0,
        podcasts ? playing_entry(NULL) + 1 : 0,
#ifdef HAVE_TC_RAMCACHE
        /* the songs' numbers, which come from the database's memory */
        tagcache_is_in_ram(),
#endif
    };
    uint32_t hash = words_hash(list, sizeof(list), 2166136261u);
    /* the rows' names of the language, where they are and what they say */
    for (size_t i = 0; i < ARRAYLEN(untagged_phrases); i++)
    {
        const char *name = str(untagged_phrases[i]);
        const uint32_t where = (uintptr_t)name;
        hash = words_hash(&where, sizeof(where), hash);
        hash = text_hash(name, hash);
    }
    /* The search, which can be out of tagtree's buffer (artists_copy), and its
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
    kept_years = entries_years;
    kept_two_lines = entries_two_lines;
    kept_split = entries_split;
    kept_tracks = entries_tracks;
    kept_groups = entries_groups;
    kept_untagged = entries_untagged;
    kept_initials = entries_initials;
    kept_initials_tag = entries_initials_tag;
    kept_shows = entries_shows;
    kept_episodes = entries_episodes;
    kept_books = entries_books;
    kept_parts = entries_parts;
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
    entries_years = kept_years;
    entries_two_lines = kept_two_lines;
    entries_split = kept_split;
    entries_tracks = kept_tracks;
    entries_groups = kept_groups; /* with their bars, kept in the names' room */
    entries_untagged = kept_untagged;
    entries_initials = kept_initials;
    entries_initials_tag = kept_initials_tag;
    entries_shows = kept_shows; /* with their sums, kept in the names' room */
    entries_episodes = kept_episodes;
    entries_books = kept_books;
    entries_parts = kept_parts;
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
        TAG_MATCH("date", tag_date) \
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
        TAG_MATCH("dateadded", tag_dateadded) \
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

static int compare_rows(const struct tagentry *e1, const struct tagentry *e2)
{
    if (!sort_by_place)
        return qsort_fn(e1->name, e2->name, MAX_PATH);
    long a = e1->extraseek, b = e2->extraseek;
    if (sort_places_tag == tag_title)
    {
        a = tagcache_sort_key(a, tag_title);
        b = tagcache_sort_key(b, tag_title);
    }
    if (sort_places_inverse)
        return a < b ? 1 : a > b ? -1 : 0;
    return a < b ? -1 : a > b;
}

static int compare(const void *p1, const void *p2)
{
    return compare_rows(p1, p2);
}

/* an artist's albums by their years, the oldest first, those of a year by
   their dates (album_date_order()), then those without one, by name: the
   songs without an album last */
static int compare_years(const void *p1, const void *p2)
{
    const struct tagentry *e1 = p1, *e2 = p2;
    const int y1 = e1->album.year, y2 = e2->album.year;
    if (y1 != y2)
    {
        if (y1 <= 0 || y2 <= 0)
            return y1 <= 0 && (y2 > 0 || y1 < y2) ? 1 : -1;
        return y1 < y2 ? -1 : 1;
    }
    if (y1 > 0)
    {
        const int32_t d1 = album_date_order(album_years_row_date(e1->extraseek));
        const int32_t d2 = album_date_order(album_years_row_date(e2->extraseek));
        if (d1 != d2)
            return d1 < d2 ? -1 : 1;
    }
    return compare_rows(e1, e2);
}

/* songs by their albums' names, which the rows don't have the places of */
static int compare_with_albums(const void *p1, const void *p2)
{
    struct tagentry *e1 = (struct tagentry *)p1;
    struct tagentry *e2 = (struct tagentry *)p2;
    int sort_album_res = sortkey_compare(
        e1->album_name == NULL ? "" : e1->album_name,
        e2->album_name == NULL ? "" : e2->album_name);
    if (sort_album_res != 0)
    {
        /* If album name is different */
        return sort_album_res;
    }
    return compare_rows(e1, e2);
}

/* the entry of the book's part playing now, -1 for none: from its track
   change to its finish, which a rewind to its start isn't */
static int book_playing = -1;

static void note_book_playing(const struct mp3entry *id3)
{
    /* by its path where it has no entry yet: the first track's change comes
       before its load, which gives it one */
    const long playing = id3->tagcache_idx > 0 ? id3->tagcache_idx - 1
                         : tagcache_find_entry(id3->path);
    book_playing = tagcache_track_kind(playing) == 2 ? playing : -1;
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

    /* A book has one place: as a part is loaded to play, the others lose
       theirs, but for the one playing now, whose stored place is the book's
       until it stops. A part loaded behind the playing one lost its own at
       that one's load, and starts at 0:00. */
    if (tagcache_track_kind(tcs.idx_id) == 2)
    {
        /* resumed before the database was in memory, its start noted none */
        if (book_playing < 0)
            note_book_playing(audio_current_track());
        tagcache_book_one_place(tcs.idx_id, book_playing);
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
    note_book_playing(id3);
    /* A part that starts with no place has a nominal one, the book's: one
       reached by playing out, or chosen, had none until it stopped */
    if (book_playing >= 0 && itunesdb_resumes(id3->path)
        && tagcache_track_place(book_playing) <= 0)
    {
        tagcache_update_numeric(book_playing, tag_lastelapsed, 1);
        tagcache_book_one_place(book_playing, -1);
    }
    if (!id3->tagcache_idx || !global_settings.runtimedb
        || !tagcache_get_numbers(id3->tagcache_idx - 1, &n))
        return;
    id3->playcount = n.playcount;
    id3->playtime = n.playtime;
    id3->lastplayed = n.lastplayed;
    /* the database's alone: no file's metadata gives one */
    id3->rating = n.rating;
}

/* an episode left this close to its end counts as played */
#define EPISODE_END_MS 60000

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
    const bool part = tagcache_idx == book_playing;
    if (part && !(te->flags & TEF_REWIND))
        book_playing = -1;

    bool auto_skip = te->flags & TEF_AUTO_SKIP;
    bool runtimedb = global_settings.runtimedb;
    bool autoresume = itunesdb_resumes(id3->path);

    /* an episode left in its last minute, or the last half of a shorter
       one, ends as one that plays out; either way its mark comes off */
    if ((itunesdb_track_flags(id3->path) & ITUNESDB_PODCAST) && id3->elapsed > 0)
    {
        const unsigned long left = id3->length > id3->elapsed ?
                                   id3->length - id3->elapsed : 0;
        if (!auto_skip && id3->length > 0
            && left <= MIN(EPISODE_END_MS, id3->length / 2))
            auto_skip = true;
        if (auto_skip)
            tagcache_set_unplayed(tagcache_idx, id3->path, false);
    }

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
        /* a part stopped at a place: the book's one place now */
        if (part && elapsed > 0)
            tagcache_book_one_place(tagcache_idx, -1);

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

/* The levels of Music's Artists and Genres that list the songs' artists, as
   tagnavi.config has them, a bit each, by the item's number in Music's own
   menu: the buffer they're in moves. Group by Album Artist lists the albums'
   artists there instead, the same list Go to Artist opens. */
static uint8_t artist_levels[TAGMENU_MAX_ITEMS];

static void artist_levels_mark(void)
{
    const struct menu_root *root = menus[rootmenu];
    memset(artist_levels, 0, sizeof(artist_levels));
    for (int i = 0; root && i < root->itemcount; i++)
    {
        const int lang = P2ID((const unsigned char *)root->items[i]->name);
        const struct search_instruction *si = &root->items[i]->si;
        if (root->items[i]->type != menu_next
            || (lang != LANG_ARTISTS && lang != LANG_GENRES))
            continue;
        for (int level = 0; level < si->tagorder_count; level++)
        {
            if (si->tagorder[level] == tag_virt_canonicalartist)
                artist_levels[i] |= 1 << level;
        }
    }
}

/* the tag Group by Album Artist has them list */
static int artist_levels_wanted(void)
{
    return global_settings.artists_by_album_artist ? tag_albumartist
                                                   : tag_virt_canonicalartist;
}

static void artist_levels_apply(void)
{
    const int tag = artist_levels_wanted();
    const struct menu_root *root = rootmenu >= 0 ? menus[rootmenu] : NULL;
    for (int i = 0; root && i < root->itemcount; i++)
    {
        for (int level = 0; level < MAX_TAGS; level++)
        {
            if (artist_levels[i] & (1 << level))
                root->items[i]->si.tagorder[level] = tag;
        }
    }
}

/* the first of an item's levels, -1 if it has none */
static int artist_levels_first(int item)
{
    for (int level = 0; level < MAX_TAGS; level++)
    {
        if (artist_levels[item] & (1 << level))
            return level;
    }
    return -1;
}

/* the first of those levels, if the search is Artists' or Genres': -1 if not */
static int artists_level(const struct search_instruction *si)
{
    const struct menu_root *root = rootmenu >= 0 ? menus[rootmenu] : NULL;
    for (int i = 0; root && si && i < root->itemcount; i++)
    {
        if (&root->items[i]->si == si)
            return artist_levels_first(i);
    }
    return -1;
}

/* the tag those levels have, -1 if there are none */
static int artist_levels_tag(void)
{
    const struct menu_root *root = rootmenu >= 0 ? menus[rootmenu] : NULL;
    for (int i = 0; root && i < root->itemcount; i++)
    {
        const int level = artist_levels_first(i);
        if (level >= 0)
            return root->items[i]->si.tagorder[level];
    }
    return -1;
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
    artist_levels_mark();
    artist_levels_apply();

    add_event(PLAYBACK_EVENT_TRACK_BUFFER, tagtree_buffer_event);
    add_event(PLAYBACK_EVENT_TRACK_CHANGE, tagtree_track_change_event);
    add_event(PLAYBACK_EVENT_TRACK_FINISH, tagtree_track_finish_event);

    core_shrink(tagtree_handle, NULL, tagtree_buf_used);
    return true;
}

static bool tagnavi_loaded;
static void kept_place_read(void);

void tagtree_init(void)
{
    tagnavi_loaded = initialize_tagtree();
    slot_lists_read();
    if (tagnavi_loaded)
        kept_place_read();
}

bool tagtree_ready(void)
{
    return tagnavi_loaded;
}

/* The name of the song idx without a title, in buf: its file's, without the
   extension, as the Music app shows it. False if it isn't known */
static bool song_file_name(struct tagcache_search *tcs, int idx, char *buf,
                           long size)
{
    if (!tagcache_retrieve(tcs, idx, tag_virt_basename, buf, size))
        return false;
    char *dot = strrchr(buf, '.');
    if (dot && dot != buf)
        *dot = '\0';
    return true;
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
                const char *result;

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
                        /* an empty one as a list of that tag names it */
                        if (!strcmp(tmpbuf, UNTAGGED)
                            && !(tag == tag_title
                                 && song_file_name(tcs, tcs->idx_id, tmpbuf,
                                                   sizeof tmpbuf)))
                            result = untagged_name(tag);
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
   that name. NULL if the list's formats do more. Formatting the rows of
   Songs took more than the search. */
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

/* Music > Recently Added, in Albums' form in tagnavi.config: its top is the
   albums added last, not a search (make_recent()) */
static bool is_recent(const struct search_instruction *si)
{
    return section_is(si, LANG_RECENTLY_ADDED);
}

/* Music > Albums: the album tag alone, at the top, as go_to_row() finds it */
static bool is_albums(const struct search_instruction *si)
{
    return si && si->tagorder[0] == tag_album && si->clause_count[0] == 0
           && !is_recent(si);
}

/* the albums' numbers fit in their rows (struct tagentry's album): a table of
   more of them leaves the lists of albums in one line */
static bool album_numbers_fit(void)
{
    return album_years_count() <= INT16_MAX;
}

/* The album artist a row of the list made last is of, as the songs it opens
   are: of an album in Albums whose name several album artists have, each a
   row of its own; -1 for another row, whose songs are of its name alone, as
   they always were */
static int32_t row_artist(const struct tagentry *row)
{
    int n = 0;
    if (!entries_split || row->newtable != TABLE_NAVIBROWSE || row->album.no < 0
        || row->customaction != ONPLAY_NO_CUSTOMACTION)
        return -1;
    album_years_find(row->extraseek, &n);
    return n > 1 ? album_years_artist(row->album.no) : -1;
}

/* the order of the albums' dates (album_date_order()), those without one
   after, the songs without an album last */
#define KEY_UNDATED (INT32_MAX - 1)
#define KEY_LAST    INT32_MAX
static int32_t date_key(int32_t date, bool unknown)
{
    if (unknown)
        return KEY_LAST;
    const int32_t order = album_date_order(date);
    return order ? order : KEY_UNDATED;
}

/* the year of one, 0 for none, YEAR_LAST for the songs without an album */
static int key_year(int32_t key)
{
    return key == KEY_LAST ? YEAR_LAST : key == KEY_UNDATED ? 0 : key / 10000;
}

/* A row of Albums, by its name's latest date, or its own album's when the
   albums of a name are apart: or its year alone, of a row whose album the
   table hasn't numbered in it */
static int32_t albums_key(const struct tagentry *e)
{
    if (e->album.year == YEAR_LAST)
        return KEY_LAST;
    int32_t date = 0;
    if (e->album.no >= 0)
        date = entries_split ? album_years_date(e->album.no)
                             : album_years_name_date_of(e->album.no);
    if (album_date_year(date) != e->album.year)
        date = e->album.year;
    return date_key(date, false);
}

/* By their dates, those of a date in the order they were: each row's key
   once, sorted with its place through the room for as many rows after them,
   then each row moved to its place along the cycles of the order */
struct year_key {
    int32_t key, row;
};
_Static_assert(2 * sizeof(struct year_key) <= sizeof(struct tagentry),
               "the keys and their merge in the room of as many rows");
static void sort_by_year(struct tagentry *e, int n, struct tagentry *room)
{
    struct year_key *keys = (struct year_key *)room, *tmp = keys + n;
    for (int i = 0; i < n; i++)
        keys[i] = (struct year_key){ albums_key(&e[i]), i };
    for (int width = 1; width < n; width *= 2)
    {
        for (int lo = 0; lo < n; lo += 2 * width)
        {
            const int mid = MIN(lo + width, n), hi = MIN(lo + 2 * width, n);
            int a = lo, b = mid, k = lo;
            while (a < mid && b < hi)
                tmp[k++] = keys[b].key < keys[a].key ? keys[b++] : keys[a++];
            while (a < mid)
                tmp[k++] = keys[a++];
            while (b < hi)
                tmp[k++] = keys[b++];
        }
        memcpy(keys, tmp, n * sizeof(*keys));
    }
    for (int i = 0; i < n; i++)
    {
        if (keys[i].row < 0 || keys[i].row == i)
            continue;
        const struct tagentry first = e[i];
        int to = i;
        while (keys[to].row != i)
        {
            const int from = keys[to].row;
            e[to] = e[from];
            keys[to].row = -1;
            to = from;
        }
        e[to] = first;
        keys[to].row = -1;
    }
}

/* the name of a tag at seek is one, not UNTAGGED, in the database's memory */
static bool tag_named(int tag, long seek)
{
#ifdef HAVE_TC_RAMCACHE
    char probe[2];
    return tagcache_tag_string(tag, seek, probe, sizeof(probe));
#else
    (void)tag; (void)seek;
    return false;
#endif
}

/* Songs' keys, compared in place of names: a song's album's number in the
   table of years or its album's place by date, then disc and track; by
   artist, its artist's place in the tag's file first. Songs without an album
   go last in their order by name; an artist without a name is the last. */
struct song_key {
    uint32_t hi, lo;
};
struct album_place {
    int32_t key, no;
};
#define SONG_ALBUM_NONE 0xffffu
#define SONGS_SORT_MAX  0x7fff /* bit 15 marks a row moved */

static int cmp_album_place(const void *p1, const void *p2)
{
    const struct album_place *a = p1, *b = p2;
    if (a->key != b->key)
        return a->key < b->key ? -1 : 1;
    return a->no < b->no ? -1 : a->no > b->no;
}

static bool key_before(const struct song_key *a, const struct song_key *b)
{
    return a->hi != b->hi ? a->hi < b->hi : a->lo < b->lo;
}

/* for songs_orders_write(): Songs made by name whatever its Sort By, and
   then the sort's room in it, NULL for none; of use while the lists' buffers
   are held still */
static bool songs_for_orders;
static struct song_key *songs_orders_keys;

/* The sort's room for n songs, in the names' room past used, nothing
   allocated, so also while music plays: the keys, the albums' keys, then the
   rows' numbers twice, or before them the albums' places by date. NULL
   without it, or for more songs than bit 15 leaves, or none: Songs then has
   no Sort By, in any order */
static struct song_key *songs_sort_room(struct tree_context *c, int n, long used)
{
    char *names = core_get_data(c->cache.name_buffer_handle);
    struct song_key *keys = (struct song_key *)ALIGN_UP((uintptr_t)(names + used),
                                                         sizeof(uint32_t));
    const int albums = album_years_count();
    if (n <= 0 || n > SONGS_SORT_MAX)
        return NULL;
    const size_t need = n * sizeof(*keys) + ALIGN_UP(albums, 2) * sizeof(uint16_t)
                        + MAX(2 * n * sizeof(uint16_t),
                              albums * sizeof(struct album_place));
    return (char *)keys + need <= names + c->cache.name_buffer_size ? keys : NULL;
}

/* A row of Songs in an order: the row of the list by name that goes there,
   and what it keeps, its album's number, for its cover, or by year its
   album's year. As the sort gives them and the orders' file has them */
struct song_place {
    uint16_t from;  /* bit 15 marks a row moved */
    int16_t value;
};
_Static_assert(sizeof(struct song_place) <= sizeof(struct song_key),
               "the places in the keys' room");

/* The list by name sorted into an order, its rows where they were, each
   given its album's number or year: stable, by merges of the rows' numbers,
   in songs_sort_room()'s keys, which the places then take */
static struct song_place *songs_order_sort(struct tree_context *c, int order,
                                           struct song_key *keys)
{
    struct tagentry *e = get_entries(c) + c->special_entry_count;
    const int n = current_entry_count - c->special_entry_count;
    const int albums = album_years_count();
    uint16_t *album_key = (uint16_t *)(keys + n);
    uint16_t *a = album_key + ALIGN_UP(albums, 2);
    uint16_t *b = a + n;
    struct album_place *places = (struct album_place *)a;

    if (order == SONGS_BY_ALBUM)
    {
        for (int no = 0; no < albums; no++)
            album_key[no] = no;
    }
    else
    {
        for (int no = 0; no < albums; no++)
            places[no] = (struct album_place){
                date_key(album_years_date(no), false), no
            };
        qsort(places, albums, sizeof(*places), cmp_album_place);
        for (int i = 0; i < albums; i++)
            album_key[places[i].no] = i;
    }
    for (int no = 0; no < albums; no++)
    {
        if (!tag_named(tag_album, album_years_album(no)))
            album_key[no] = SONG_ALBUM_NONE;
    }

    for (int i = 0; i < n; i++)
    {
        const int32_t idx = e[i].extraseek;
        const int no = album_years_number(tagcache_sort_key(idx, tag_album),
                                          tagcache_sort_key(idx, tag_albumartist));
        const uint32_t k = no >= 0 ? album_key[no] : SONG_ALBUM_NONE;
        const bool album = k != SONG_ALBUM_NONE;
        const long disc = MIN(MAX(tagcache_sort_key(idx, tag_discnumber), 0), 64);
        const long track = MIN(MAX(tagcache_sort_key(idx, tag_tracknumber), 0), 999);
        keys[i].lo = k << 16 | (album ? disc * 1000 + track : 0);
        keys[i].hi = 0;
        if (order == SONGS_BY_ARTIST)
        {
            const long artist = tagcache_sort_key(idx, tag_virt_canonicalartist);
            keys[i].hi = tag_named(tag_virt_canonicalartist, artist) ?
                         (uint32_t)artist : UINT32_MAX;
        }
        if (order == SONGS_BY_YEAR)
            e[i].album = (typeof(e[i].album)){
                .year = album ? album_years_year(no) : YEAR_LAST, .no = -1
            };
        else
            e[i].album = (typeof(e[i].album)){ .year = 0, .no = album ? no : -1 };
    }

    for (int i = 0; i < n; i++)
        a[i] = i;
    for (int width = 1; width < n; width *= 2)
    {
        for (int lo = 0; lo < n; lo += 2 * width)
        {
            const int mid = MIN(lo + width, n), hi = MIN(lo + 2 * width, n);
            int x = lo, y = mid, k = lo;
            while (x < mid && y < hi)
                b[k++] = key_before(&keys[a[y]], &keys[a[x]]) ? a[y++] : a[x++];
            while (x < mid)
                b[k++] = a[x++];
            while (y < hi)
                b[k++] = a[y++];
        }
        uint16_t *t = a;
        a = b;
        b = t;
        /* for the codec: nothing but this room is used across it */
        yield();
    }

    struct song_place *rows = (struct song_place *)keys;
    for (int k = 0; k < n; k++)
        rows[k] = (struct song_place){
            a[k], order == SONGS_BY_YEAR ? e[a[k]].album.year : e[a[k]].album.no
        };
    return rows;
}

/* The list by name into the order of those places: each row moved along the
   cycles of the order, then given what it keeps */
static void songs_order_apply(struct tagentry *e, int n, struct song_place *rows,
                              int order)
{
    for (int i = 0; i < n; i++)
    {
        if ((rows[i].from & 0x8000) || rows[i].from == i)
            continue;
        const struct tagentry first = e[i];
        int to = i;
        while (rows[to].from != i)
        {
            const int from = rows[to].from;
            e[to] = e[from];
            rows[to].from |= 0x8000;
            to = from;
        }
        e[to] = first;
        rows[to].from |= 0x8000;
    }
    for (int i = 0; i < n; i++)
    {
        if (order == SONGS_BY_YEAR)
            e[i].album = (typeof(e[i].album)){ .year = rows[i].value, .no = -1 };
        else
            e[i].album = (typeof(e[i].album)){ .year = 0, .no = rows[i].value };
    }
}

/* Songs' orders kept in a file, so that the list in one opens without its
   sort, 0.5 s of its 0.7 for 13 000 songs on an iPod. Updating Library
   writes it (songs_orders_write()), each order's places one after another;
   a list reads its order's into the sort's room, where the sort puts them */
#define SONGS_ORDERS_FILE    ROCKBOX_DIR "/songs_orders.dat"
#define SONGS_ORDERS_MAGIC   0x4f535752 /* "RWSO" */
/* raised with the keys, their order or the file's layout, and with what is
   made of the albums' dates: album_date_order(), album_date_year()
   (album_years.c) and date_key() */
#define SONGS_ORDERS_VERSION 1
#define SONGS_ORDERS         3 /* by artist, album and year, in SONGS_BY_'s order */
struct songs_orders_header {
    uint32_t magic, version;
    int32_t commitid;
    uint32_t room;          /* the lists' size, which decides whether the sort has room */
    uint32_t songs;         /* places in each order; 0 for none, the sort without room */
    uint32_t songs_hash, albums_hash;   /* songs_inputs() */
    uint32_t rows_hash;     /* the list by name the places are of */
    uint32_t sum[SONGS_ORDERS]; /* of each order's places */
};
#define HASH_START 2166136261u

/* All that the sort reads, hashed, so that the file is used only for the
   same: each song's places in its tags' files, by which the list by name and
   the keys go, its disc and track, and the artist without a name; the albums
   of the table of years in their order, each one's seeks, date and whether it
   has a name */
struct songs_inputs {
    uint32_t songs, albums;
};

#ifdef HAVE_TC_RAMCACHE
struct songs_hashing {
    uint32_t hash;
    int32_t artist, unnamed;
};

/* with the database's memory held still: nothing waits here */
static void songs_hash_track(int idx_id, const int32_t *seek, void *data)
{
    struct songs_hashing *h = data;
    const int32_t artist = seek[tag_virt_canonicalartist];
    const int32_t of[] = {
        idx_id, seek[tag_title], seek[tag_album], seek[tag_albumartist], artist,
        seek[tag_discnumber], seek[tag_tracknumber]
    };
    h->hash = words_hash(of, sizeof(of), h->hash);
    /* names are once in a tag's file: one seek at most is of none */
    if (artist != h->artist)
    {
        h->artist = artist;
        if (!tag_named(tag_virt_canonicalartist, artist))
            h->unnamed = artist;
    }
}
#endif

static bool songs_inputs(struct songs_inputs *in)
{
#ifdef HAVE_TC_RAMCACHE
    struct songs_hashing h = { HASH_START, -1, -1 };
    if (!tagcache_for_each_track(songs_hash_track, &h))
        return false;
    in->songs = words_hash(&h.unnamed, sizeof(h.unnamed), h.hash);
    const int32_t albums = album_years_count();
    uint32_t hash = words_hash(&albums, sizeof(albums), HASH_START);
    for (int no = 0; no < albums; no++)
    {
        const int32_t album = album_years_album(no);
        const int32_t of[] = {
            album, album_years_artist(no), album_years_date(no),
            tag_named(tag_album, album)
        };
        hash = words_hash(of, sizeof(of), hash);
    }
    in->albums = hash;
    return true;
#else
    (void)in;
    return false;
#endif
}

static uint32_t songs_rows_hash(const struct tagentry *e, int n)
{
    uint32_t hash = HASH_START;
    for (int i = 0; i < n; i++)
        hash = (hash ^ (uint32_t)e[i].extraseek) * 16777619;
    return hash;
}

/* The file open past its header, if it is of the library, the table of
   years, the rules and the lists' size as they are now: -1 if not, or
   without it */
static int songs_orders_open(struct songs_orders_header *h)
{
    struct songs_inputs in;
    const int fd = open(SONGS_ORDERS_FILE, O_RDONLY);
    if (fd < 0)
        return -1;
    if (read(fd, h, sizeof(*h)) == sizeof(*h)
        && h->magic == SONGS_ORDERS_MAGIC && h->version == SONGS_ORDERS_VERSION
        && h->commitid == tagcache_get_commitid()
        && h->room == (uint32_t)tree_get_context()->cache.max_entries
        && h->songs <= SONGS_SORT_MAX
        && ffilesize(fd) == (off_t)(sizeof(*h) + SONGS_ORDERS * h->songs
                                                 * sizeof(struct song_place))
        && songs_inputs(&in)
        && h->songs_hash == in.songs && h->albums_hash == in.albums)
        return fd;
    close(fd);
    return -1;
}

/* each row of the list is in the places once: else the move wouldn't end */
static bool songs_places_whole(struct song_place *rows, int n)
{
    bool whole = true;
    for (int k = 0; k < n && whole; k++)
    {
        const int from = rows[k].from & 0x7fff;
        whole = from < n && !(rows[from].from & 0x8000);
        if (whole)
            rows[from].from |= 0x8000;
    }
    for (int k = 0; k < n; k++)
        rows[k].from &= 0x7fff;
    return whole;
}

/* a hard disk asleep isn't woken for the file: flash, or a disk that spins */
static bool songs_orders_disk_on(void)
{
#if defined(SIMULATOR) || !defined(HAVE_DISK_STORAGE)
    return true;
#elif (CONFIG_STORAGE & STORAGE_ATA)
    return ata_disk_isssd() || storage_disk_is_active();
#else
    return storage_disk_is_active();
#endif
}

/* An order's places for the list by name just made, from the file into the
   sort's room: NULL if it isn't to be read, or isn't of this list, and then
   it is sorted. One that says it is, but isn't whole, is removed, for the
   next Updating Library to write it again */
static struct song_place *songs_order_read(struct tree_context *c, int order,
                                           struct song_key *keys)
{
    const struct tagentry *e = get_entries(c) + c->special_entry_count;
    const int n = current_entry_count - c->special_entry_count;
    struct song_place *rows = (struct song_place *)keys;
    const ssize_t size = n * sizeof(*rows);
    struct songs_orders_header h;
    if (!songs_orders_disk_on())
        return NULL;
    const int fd = songs_orders_open(&h);
    if (fd < 0)
        return NULL;
    const bool ok = h.songs == (uint32_t)n && h.rows_hash == songs_rows_hash(e, n)
        && lseek(fd, sizeof(h) + (order - SONGS_BY_ARTIST) * size, SEEK_SET) >= 0
        && read(fd, rows, size) == size
        && words_hash(rows, size, HASH_START) == h.sum[order - SONGS_BY_ARTIST]
        && songs_places_whole(rows, n);
    close(fd);
    if (!ok)
        remove(SONGS_ORDERS_FILE);
    return ok ? rows : NULL;
}

/* A person's albums: an artist's, album artist's or composer's, which go by
   their years once the table of the years is made, and have All Songs */
static bool persons_albums(struct tree_context *c, int level, int tag)
{
    if (tag != tag_album || c->currtable != TABLE_NAVIBROWSE || level <= 0)
        return false;
    switch (csi->tagorder[level - 1])
    {
        case tag_virt_canonicalartist:
        case tag_artist:
        case tag_albumartist:
        case tag_composer:
            return true;
    }
    return false;
}

/* each song of a person's albums, counted for All Songs, as the search goes
   through the database's memory held still: nothing waits here */
static void persons_match(const int32_t *tag_seek, void *data)
{
    (*(int *)data)++;
    album_years_match(tag_seek, NULL);
}

/* the search is that of Music's section of this name in tagnavi.config */
static bool section_is(const struct search_instruction *si, int lang)
{
    const int item = root_item_of(lang);
    return item >= 0 && si == &menus[rootmenu]->items[item]->si;
}

static bool in_section(int lang)
{
    return section_is(csi, lang);
}

/* A show's episodes in Podcasts, by date when the list is whole;
   list_hash() marks its id */
static bool show_episodes(const struct search_instruction *si, int table,
                          int level)
{
    return table == TABLE_NAVIBROWSE && si && level > 0
           && level < si->tagorder_count && si->tagorder[level] == tag_title
           && si->tagorder[level - 1] == tag_album
           && section_is(si, LANG_PODCASTS) && song_places_known();
}

/* a book's parts in Audiobooks, in their discs' and tracks' order */
static bool book_parts(const struct search_instruction *si, int table,
                       int level)
{
    return table == TABLE_NAVIBROWSE && si && level > 0
           && level < si->tagorder_count && si->tagorder[level] == tag_title
           && si->tagorder[level - 1] == tag_album
           && section_is(si, LANG_AUDIOBOOKS) && song_places_known();
}

/* Per show, at the end of the names' room, first one last (as
   song_groups()): filled by the search's match hook. A book's: its parts,
   those not heard, those begun; its length in ms; the part begun, the first
   in the book's order, by its key (part_key()) and entry, and the length of
   the parts before it, summed in a second pass. */
struct show_sums {
    int32_t album;       /* its name's seek, which its row has */
    int32_t albumartist; /* of the first episode found: its cover's album */
    int32_t episodes;
    int32_t unplayed;    /* those of them that are new */
    int32_t started;     /* and those begun */
    union {
        int32_t newest;  /* the newest one's date, 0 for none */
        int32_t length;
    };
    /* the newest of those begun, whose time left the row shows: its date
       and its entry, -1 for none */
    union {
        int32_t started_date;
        int32_t before;
    };
    int32_t started_idx;
    int32_t key;
};

struct shows_counting {
    struct show_sums *end; /* past the first */
    long room;             /* the names' room */
    const int *used;       /* and what the rows' names have taken of it */
    int count, last;
    bool full;             /* no room: the names have come to the sums */
    bool books;            /* Audiobooks: a book's sums */
    int32_t playing;       /* the entry playing, begun as it plays; -1 */
};

/* a part's place in its book: its disc, then its track; 0 without either */
static int32_t part_key(const int32_t *tag_seek)
{
    return (MAX(tag_seek[tag_discnumber], 0) << 16)
           | (MAX(tag_seek[tag_tracknumber], 0) & 0xffff);
}

/* An episode's date as its row has it and its list goes by: tag_date, or
   its year alone, as the albums' dates take a song's; 0 for none */
static long seek_date(const int32_t *tag_seek)
{
    const long date = tag_seek[tag_date], year = tag_seek[tag_year];
    return date > 0 ? date : year > 0 && year < 10000 ? year * 10000 : 0;
}

/* the entry of the track playing, if it remembers its place and has one, -1
   for none, and where it is: an episode is begun as it plays. UI thread */
static int32_t playing_entry(unsigned long *elapsed)
{
    const struct mp3entry *id3 = audio_current_track();
    if (!(audio_status() & AUDIO_STATUS_PLAY) || !id3 || id3->tagcache_idx <= 0
        || id3->elapsed == 0 || !itunesdb_resumes(id3->path))
        return -1;
    if (elapsed)
        *elapsed = id3->elapsed;
    return id3->tagcache_idx - 1;
}

static void shows_match(const int32_t *tag_seek, void *data)
{
    struct shows_counting *s = data;
    const int32_t album = tag_seek[tag_album];
    if (!s->full)
        s->full = *s->used + (long)(s->count * sizeof(*s->end)) > s->room;
    if (s->full)
        return;
    /* the show of the episode before, most often */
    int i = s->last;
    if (i >= s->count || s->end[-1 - i].album != album)
    {
        for (i = 0; i < s->count && s->end[-1 - i].album != album; i++)
            ;
    }
    if (i == s->count)
    {
        if (*s->used + (long)((s->count + 1) * sizeof(*s->end)) > s->room)
        {
            s->full = true;
            return;
        }
        s->end[-1 - i] = (struct show_sums){
            .album = album, .albumartist = tag_seek[tag_albumartist],
            .started_idx = -1
        };
        s->count++;
    }
    struct show_sums *sums = &s->end[-1 - i];
    const int32_t idx = tagcache_match_entry(tag_seek);
    const long date = seek_date(tag_seek);
    const int state = idx >= 0 && idx == s->playing ? EPISODE_STARTED
                                                    : tagcache_episode_state(idx);
    sums->episodes++;
    s->last = i;
    if (s->books)
    {
        const int32_t key = part_key(tag_seek);
        sums->length += MAX(tag_seek[tag_length], 0);
        if (state == EPISODE_NEW)
            sums->unplayed++;
        else if (state == EPISODE_STARTED && (!sums->started++ || key < sums->key))
        {
            sums->key = key;
            sums->started_idx = idx;
        }
        return;
    }
    sums->newest = MAX(sums->newest, date);
    if (state == EPISODE_NEW)
        sums->unplayed++;
    else if (state == EPISODE_STARTED)
    {
        if (!sums->started++ || date > sums->started_date)
        {
            sums->started_date = date;
            sums->started_idx = idx;
        }
    }
    s->last = i;
}

/* The second pass over Audiobooks: of each book begun, the length of the
   parts before the one begun */
static void books_before_match(const int32_t *tag_seek, void *data)
{
    struct shows_counting *s = data;
    const int32_t album = tag_seek[tag_album];
    int i = s->last;
    if (i >= s->count || s->end[-1 - i].album != album)
    {
        for (i = 0; i < s->count && s->end[-1 - i].album != album; i++)
            ;
    }
    if (i == s->count)
        return;
    struct show_sums *sums = &s->end[-1 - i];
    s->last = i;
    if (sums->started_idx >= 0 && part_key(tag_seek) < sums->key)
        sums->before += MAX(tag_seek[tag_length], 0);
}

static long episode_date(int32_t idx)
{
    const long date = tagcache_sort_key(idx, tag_date);
    const long year = tagcache_sort_key(idx, tag_year);
    return date > 0 ? date : year > 0 && year < 10000 ? year * 10000 : 0;
}

/* The newest first: by release date, a month alone after its days and a
   year alone after its months, those without a date last; of a day, the one
   added later first, then the larger number, then by title */
static int compare_episodes(const void *p1, const void *p2)
{
    const int32_t a = ((const struct tagentry *)p1)->extraseek;
    const int32_t b = ((const struct tagentry *)p2)->extraseek;
    long ka = episode_date(a), kb = episode_date(b);
    if (ka != kb)
        return ka > kb ? -1 : 1;
    ka = tagcache_sort_key(a, tag_dateadded);
    kb = tagcache_sort_key(b, tag_dateadded);
    if (ka != kb)
        return ka > kb ? -1 : 1;
    ka = MAX(tagcache_sort_key(a, tag_tracknumber), 0);
    kb = MAX(tagcache_sort_key(b, tag_tracknumber), 0);
    if (ka != kb)
        return ka > kb ? -1 : 1;
    ka = tagcache_sort_key(a, tag_title);
    kb = tagcache_sort_key(b, tag_title);
    if (ka != kb)
        return ka < kb ? -1 : 1;
    return a < b ? -1 : a > b;
}

/* An album's songs, in a section of albums: not a show's episodes nor a
   book's parts, which Podcasts and Audiobooks list in their own way */
static bool album_songs(struct tree_context *c, int level, int tag)
{
    return tag == tag_title && c->currtable == TABLE_NAVIBROWSE && level > 0
           && csi->tagorder[level - 1] == tag_album
           && !in_section(LANG_PODCASTS) && !in_section(LANG_AUDIOBOOKS);
}

/* The kind of list whose Shuffle Songs row Settings > Shuffle in Lists turns
   on or off (SHUFFLE_ROW_...), by what the list shows: a genre's artists go
   by Artists, a composer's albums by An Artist's Albums, as an artist's All
   Songs does. -1 for a list without one: Podcasts' and Audiobooks', whose
   episodes and parts are listed in their order, and those of one's own
   tagnavi. The whole library under Artists, Albums, Songs, Genres and
   Composers, which have no condition at their top. */
static int shuffle_row_kind(struct tree_context *c)
{
    if (!csi || in_section(LANG_PODCASTS) || in_section(LANG_AUDIOBOOKS))
        return -1;
    if (c->currtable == TABLE_ALLSUBENTRIES)
        return SHUFFLE_ROW_ARTIST;
    const int level = c->currextra;
    if (c->currtable != TABLE_NAVIBROWSE || level >= csi->tagorder_count)
        return -1;
    /* Recently Added's top isn't the whole library */
    if (level == 0 && is_recent(csi))
        return -1;
    const int tag = csi->tagorder[level];
    const bool whole = level == 0 && csi->clause_count[0] == 0;
    const int above = level > 0 ? csi->tagorder[level - 1] : -1;
    switch (tag)
    {
        case tag_title:
            return whole ? SHUFFLE_ROW_SONGS
                   : above == tag_album ? SHUFFLE_ROW_ALBUM : -1;
        case tag_album:
            return whole ? SHUFFLE_ROW_ALBUMS
                   : persons_albums(c, level, tag) ? SHUFFLE_ROW_ARTIST : -1;
        case tag_virt_canonicalartist:
        case tag_artist:
        case tag_albumartist:
            return whole || above == tag_genre ? SHUFFLE_ROW_ARTISTS : -1;
        case tag_genre:
            return whole ? SHUFFLE_ROW_GENRES : -1;
        case tag_composer:
            return whole ? SHUFFLE_ROW_COMPOSERS : -1;
    }
    return -1;
}

/* whether the list has Shuffle Songs at its top, as it has songs enough */
static bool shuffle_row_wanted(struct tree_context *c)
{
    const int kind = shuffle_row_kind(c);
    return kind >= 0 && global_settings.shuffle_rows_show[kind];
}

/* whether the row Shuffle Songs would play the whole library: Shuffle Songs of
   the main menu's */
static bool shuffle_row_whole(struct tree_context *c)
{
    return c->currtable == TABLE_NAVIBROWSE && c->currextra == 0
           && csi->clause_count[0] == 0;
}

/* Shuffle Songs as a list's first row, playing its songs in a new order */
static const struct tagentry shuffle_row = {
    .name = ID2P(LANG_SHUFFLE_SONGS), .newtable = TABLE_NAVIBROWSE,
    .extraseek = -4, /* not <Random>'s, Sort By's nor All Songs' */
    .customaction = ONPLAY_CUSTOMACTION_SHUFFLE_LIST,
};

/* each song the list's search finds, for the songs Shuffle Songs would play */
static void count_match(const int32_t *tag_seek, void *data)
{
    (void)tag_seek;
    (*(int *)data)++;
}

/* A row's year or number kept as the text before its name: its digits, as
   many as there are, none for none or one of more than four */
static int keep_lead(struct tagentry *entry, long n)
{
    char text[12];
    const int len = n > 0 && n < 10000 ? strlen(itoa_buf(text, sizeof(text), n))
                                       : 0;
    memset(entry->lead, 0, sizeof(entry->lead));
    memcpy(entry->lead, text, len);
    return len;
}

/* The names of the list's format show the songs' numbers themselves, as
   tagnavi.config's "fmt_best_tracks" does: not cut off again with what sorts
   them, as "fmt_title" cuts its */
static bool format_shows_track(int group, int strip)
{
    if (strip > 0)
        return false;
    for (int i = 0; i < format_count; i++)
    {
        if (formats[i]->group_id != group)
            continue;
        for (int j = 0; j < formats[i]->tag_count; j++)
        {
            if (formats[i]->tags[j] == tag_tracknumber)
                return true;
        }
    }
    return false;
}

/* The next song of an album, idx, after one of that disc and track: whether
   the album still goes by them, each song with a number in a place of its
   own. Its number in *track, with its disc in *disc, for the next one. Two
   albums of one name, in one list, went 1, 1, 2, 2. */
static bool album_goes_on(int32_t idx, long *disc, long *track)
{
    /* none, -1 in the database, sorts as the format's 0 */
    const long d = MAX(tagcache_sort_key(idx, tag_discnumber), 0);
    const long t = MAX(tagcache_sort_key(idx, tag_tracknumber), 0);
    if (d < *disc || (d == *disc && (t < *track || (t == *track && t > 0))))
        return false;
    *disc = d;
    *track = t;
    return true;
}

/* The list's bars, at the end of the names' room: the first the last there,
   the next before it */
static struct song_group *song_groups(struct tree_context *c)
{
    char *names = core_get_data(c->cache.name_buffer_handle);
    return (struct song_group *)(names + c->cache.name_buffer_size);
}

/* One more bar, before those made, *groups of them: false if it doesn't fit
   past the names' room used */
static bool add_group(struct tree_context *c, long used, int *groups,
                      const struct song_group *group)
{
    if (used + (long)((*groups + 1) * sizeof(*group)) > c->cache.name_buffer_size)
        return false;
    song_groups(c)[-1 - *groups] = *group;
    (*groups)++;
    return true;
}

/* The songs of the list, past its rows at the top, each with its number as
   the database's memory has it, kept: the digits of the largest, 0 if none
   has one. 0 too for songs in another order than their discs' and tracks', as
   by a format of one's own tagnavi, or of two albums, and for the songs of
   the row without an album, which come from any. 0 for a single song: a
   number says nothing there. An album on two discs or more, each song on
   one, has a bar over each disc's first song, "Disc 1", "Disc 4" by the tag,
   as entries_groups: past the names' room used, or none if they don't fit. */
static int number_songs(struct tree_context *c, struct tagcache_search *tcs,
                        long used)
{
    struct tagentry *entries = get_entries(c);
    const int first = c->special_entry_count;
    char album[sizeof(UNTAGGED) + 1]; /* a longer name, cut, isn't it */
    if (current_entry_count - first < 2
        || (tagcache_retrieve(tcs, entries[first].extraseek, tag_album,
                              album, sizeof(album))
            && !strcmp(album, UNTAGGED)))
        return 0;
    long disc = 0, track = 0;
    int digits = 0, groups = 0;
    bool discs = true;
    for (int i = first; i < current_entry_count; i++)
    {
        const long last_disc = disc;
        if (!album_goes_on(entries[i].extraseek, &disc, &track))
            return 0;
        digits = MAX(digits, keep_lead(&entries[i], track));
        const struct song_group bar = {
            .row = i, .album = GROUP_NO_ALBUM, .year = 0, .disc = disc
        };
        discs = discs && disc > 0
                && (disc == last_disc || add_group(c, used, &groups, &bar));
    }
    entries_groups = discs && groups >= 2 ? groups : 0;
    return digits;
}

/* The list as an album's screen, if it's one album's songs (tagtree_album()):
   each song's album and album artist the first's, as the database has the
   song's artist for none, and their discs and tracks in order, as by
   tagnavi's fmt_title, though two may share a number. Of the rows past those
   at the top, from the database's memory, as the list is loaded.
   A show's episodes too: year and longest 0, albumartist -1 if authors
   differ */
static void album_view_make(struct tree_context *c)
{
    album_view.songs = 0;
    const int level = c->currextra;
    const int first = c->special_entry_count;
    const bool book = entries_parts.parts > 0;
    const bool show = entries_episodes || book;
    struct tagcache_numbers numbers;
    if (c->currtable != TABLE_NAVIBROWSE || !csi || c->dirfull
        || current_offset != 0 || current_entry_count != c->filesindir
        || first >= current_entry_count
        || !(book ? book_parts(csi, c->currtable, level)
             : show ? show_episodes(csi, c->currtable, level)
                    : album_songs(c, level, csi->tagorder[level]))
        || !strcmp(current_title[level], UNTAGGED))
        return;
    const struct tagentry *entries = get_entries(c);
    const int32_t idx = entries[first].extraseek;
    if (!tagcache_get_numbers(idx, &numbers) || (numbers.kind != 0 && !show))
        return;
    const long album = tagcache_sort_key(idx, tag_album);
    long albumartist = tagcache_sort_key(idx, tag_albumartist);
    long disc = 0, track = 0, longest = 0;
    for (int i = first; i < current_entry_count; i++)
    {
        const int32_t song = entries[i].extraseek;
        const long d = MAX(tagcache_sort_key(song, tag_discnumber), 0);
        const long t = MAX(tagcache_sort_key(song, tag_tracknumber), 0);
        if (entries[i].newtable != TABLE_PLAYTRACK
            || tagcache_sort_key(song, tag_album) != album)
            return;
        if (show)
        {
            if (tagcache_sort_key(song, tag_albumartist) != albumartist)
                albumartist = -1;
            continue;
        }
        if (tagcache_sort_key(song, tag_albumartist) != albumartist
            || d < disc || (d == disc && t < track))
            return;
        disc = d;
        track = t;
        longest = MAX(longest, tagcache_sort_key(song, tag_length));
    }
    album_view = (struct tagtree_album){
        .songs = current_entry_count - first,
        .year = show ? 0 : album_year(album, albumartist),
        /* a book's rows have values too, its parts' starts */
        .longest = book ? MAX(entries_parts.length, 1) : longest,
        .first = idx,
        .album = album,
        .albumartist = albumartist,
    };
}

static int make_all_songs(struct tree_context *c, int extra, bool init);

/* Audiobooks' rows, made: a second pass for what is heard of the books begun,
   with the search's conditions again, and those books first, in the order
   they had. buf is the search's room. */
static void books_finish(struct tree_context *c, struct shows_counting *s,
                         char *buf, size_t size)
{
    struct tagentry *entries = get_entries(c);
    struct tagcache_search tcs;
    bool begun = false;
    for (int k = 0; k < s->count && !begun; k++)
        begun = s->end[-1 - k].started_idx >= 0;
    if (!begun || !tagcache_search(&tcs, tag_album))
        return;
    tagcache_search_set_uniqbuf(&tcs, uniqbuf, UNIQBUF_SIZE);
    for (int j = 0; j < csi->clause_count[0]; j++)
        tagcache_search_add_clause(&tcs, csi->clause[0][j]);
    s->last = 0;
    tagcache_search_set_match(&tcs, books_before_match, s);
    while (tagcache_get_next(&tcs, buf, size))
        ;
    tagcache_search_finish(&tcs);

    int front = c->special_entry_count;
    for (int i = front; i < current_entry_count; i++)
    {
        const int k = entries[i].album.year;
        if (k < 0 || k >= s->count || s->end[-1 - k].started_idx < 0)
            continue;
        const struct tagentry row = entries[i];
        memmove(&entries[front + 1], &entries[front],
                (i - front) * sizeof(*entries));
        entries[front++] = row;
    }
    if (front > c->special_entry_count)
        entries_initials = INITIALS_NONE; /* not by name from the top */
}

/* A book's parts, made, in their order: what is heard of it, and the row at
   their top, Resume for one begun and Listen Again for one heard to its end;
   a book in one file has its own row for that. */
static void parts_finish(struct tree_context *c)
{
    struct tagentry *entries = get_entries(c);
    const int first = c->special_entry_count;
    const int n = current_entry_count - first;
    if (c->dirfull || n <= 0 || current_entry_count >= c->cache.max_entries)
        return;
    const long album = tagcache_sort_key(entries[first].extraseek, tag_album);
    const int32_t playing = playing_entry(NULL);
    struct book_parts book = { .parts = n, .current = -1 };
    int unplayed = 0;
    for (int i = first; i < current_entry_count; i++)
    {
        const int32_t idx = entries[i].extraseek;
        if (entries[i].newtable != TABLE_PLAYTRACK
            || tagcache_sort_key(idx, tag_album) != album)
            return;
        const int state = idx == playing ? EPISODE_STARTED
                                         : tagcache_episode_state(idx);
        book.length += MAX(tagcache_sort_key(idx, tag_length), 0);
        if (state == EPISODE_STARTED && book.current < 0)
            book.current = i - first;
        unplayed += state == EPISODE_NEW;
    }
    book.finished = book.current < 0 && !unplayed;
    entries_parts = book;
    const unsigned char *name = book.current >= 0 ? ID2P(LANG_BOOKMARK_CONTEXT_RESUME)
                                : book.finished ? ID2P(LANG_LISTEN_AGAIN) : NULL;
    /* a book in one file: its own row says it (tagtree_get_entry_name()) */
    if (n == 1 || !name)
        return;
    memmove(&entries[first + 1], &entries[first], n * sizeof(*entries));
    entries[first] = (struct tagentry){
        .name = (char *)name, .newtable = TABLE_NAVIBROWSE, .extraseek = -2,
        .customaction = ONPLAY_CUSTOMACTION_BOOK_RESUME,
        .album = { .year = 0, .no = -1 }
    };
    current_entry_count++;
    c->special_entry_count++;
}

/* The rows chosen on the way to the list at level as the search's filters: by
   their tags' seeks, a number's as a condition */
static void add_level_filters(struct tagcache_search *tcs, int level)
{
    static struct tagcache_search_clause numbers[MAX_TAGS];
    for (int i = 0; i < level; i++)
    {
        if (TAGCACHE_IS_NUMERIC(csi->tagorder[i]))
        {
            struct tagcache_search_clause *cc = &numbers[i];
            memset(cc, 0, sizeof(*cc));
            cc->tag = csi->tagorder[i];
            cc->type = clause_is;
            cc->numeric = true;
            cc->numeric_data = csi->result_seek[i];
            tagcache_search_add_clause(tcs, cc);
        }
        else
            tagcache_search_add_filter(tcs, csi->tagorder[i], csi->result_seek[i]);
        /* an album of a name several album artists have: of its own */
        if (csi->result_artist[i] >= 0)
            tagcache_search_add_filter(tcs, tag_albumartist, csi->result_artist[i]);
    }
}

/* Music > Recently Added, as Albums' rows, from the table without a search;
   a name's albums one row without Artists in Albums; in one line the year
   before the name */
static int make_recent(struct tree_context *c)
{
    const bool numbers = album_numbers_fit();
    const bool two_lines = numbers && (global_settings.album_artists
                                       || list_covers_on());
    const bool split = two_lines && global_settings.album_artists;
    long used = 0;
    int n = 0;
    tree_lock_cache(c);
    struct tagentry *entries = get_entries(c);
    char *names = core_get_data(c->cache.name_buffer_handle);
    for (int i = 0; i < album_years_recent_count() && n < c->cache.max_entries;
         i++)
    {
        const int no = album_years_recent(i);
        const int32_t seek = album_years_album(no);
        /* a name's albums in one row: had already, by a newer one */
        int k = split ? n : 0;
        while (k < n && entries[k].extraseek != seek)
            k++;
        char *name = names + used;
        const long room = c->cache.name_buffer_size - used;
        if (no < 0 || k < n || room < TAG_MAXLEN
            || !tagcache_tag_string(tag_album, seek, name, room))
            continue;
        used += strlen(name) + 1;
        entries[n] = (struct tagentry){
            .name = name, .newtable = TABLE_NAVIBROWSE, .extraseek = seek,
            .customaction = ONPLAY_NO_CUSTOMACTION,
            .album = { .year = split ? album_years_year(no)
                                     : album_name_year(seek),
                       .no = numbers ? no : -1 }
        };
        if (!two_lines)
            keep_lead(&entries[n], entries[n].album.year);
        n++;
    }
    tree_unlock_cache(c);

    current_offset = 0;
    current_entry_count = n;
    c->special_entry_count = 0;
    c->dirfull = false;
    entries_years = !two_lines;
    entries_two_lines = two_lines;
    entries_split = split;
    entries_untagged = -1;
    entries_initials_tag = tag_album;
    entries_of_stats = false;
    entries_whole = true;
    return n;
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
    /* a part of a list in chunks after its first: the rows at its top, which
       aren't songs, as its first part found them */
    const int special_before = c->special_entry_count;
    c->special_entry_count = 0;
    int level = c->currextra;
    int tag;
    bool sort = false;
    bool sort_inverse;
    bool is_basename = false;
    int sort_limit;
    int strip;
    entries_whole = false;
    entries_years = false;
    entries_two_lines = false;
    entries_split = false;
    entries_tracks = 0;
    entries_groups = 0;
    entries_initials = INITIALS_NONE;
    entries_shows = 0;
    entries_episodes = false;
    entries_books = false;
    entries_parts.parts = 0;
    album_view.songs = 0;

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

    if (c->currtable == TABLE_ALLSUBENTRIES)
        return make_all_songs(c, c->currextra - 2, init);
    if (c->currtable == TABLE_NAVIBROWSE && level == 0 && is_recent(csi))
        return make_recent(c);

    if (c->currtable == TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS)
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

    /* Music > Albums, all of them in memory with their years: Sort By its
       first row, and by their years if so */
    bool albums = offset == 0 && level == 0 && tag == tag_album
        && c->currtable == TABLE_NAVIBROWSE && is_albums(csi)
        && album_years_ready() && tcs.ramsearch
        && tcs.entry_count + 1 < c->cache.max_entries / 2;
    /* an artist's albums: their years, as the songs found are on them, and
       those songs counted */
    const bool persons = offset == 0 && persons_albums(c, level, tag);
    const bool years = persons && album_years_ready();
    int persons_songs = 0;
    /* an album's songs: their numbers */
    const bool numbered = offset == 0 && album_songs(c, level, tag);
    entries_years = false;
    /* a list's first part, or all of it: its row of <Untagged> not found yet */
    if (offset == 0)
        entries_untagged = -1;
    if (persons)
    {
        album_years_rows_begin();
        tagcache_search_set_match(&tcs, persons_match, &persons_songs);
    }
    /* Shuffle Songs at its top, and the songs it would play counted as the
       search finds them, by a person's albums' own count if it has one: the
       row goes if they are too few. Those of the whole library are the
       database's: the search of a list of it goes through its tag's names,
       not its songs */
    const bool shuffle = offset == 0 && shuffle_row_wanted(c);
    const bool whole = shuffle && shuffle_row_whole(c);
    int list_songs = 0;
    if (whole)
        list_songs = tagcache_get_song_count() >= 0 ? tagcache_get_song_count()
                                                     : tagcache_entry_count();
    else if (shuffle && !persons)
        tagcache_search_set_match(&tcs, count_match, &list_songs);
    /* Podcasts' shows counted by the hook; a show's episodes by date. Whole
       lists from memory only */
    const bool books = in_section(LANG_AUDIOBOOKS);
    const bool shows = offset == 0 && level == 0 && tag == tag_album
        && c->currtable == TABLE_NAVIBROWSE
        && (books || in_section(LANG_PODCASTS)) && tcs.ramsearch && !shuffle;
    const bool episodes = offset == 0 && tag == tag_title && tcs.ramsearch
        && show_episodes(csi, c->currtable, level);
    const bool parts = offset == 0 && tag == tag_title && tcs.ramsearch
        && book_parts(csi, c->currtable, level);
    struct shows_counting counting = {
        .room = c->cache.name_buffer_size, .used = &namebufused,
        .books = books, .playing = shows ? playing_entry(NULL) : -1
    };
    if (shows)
        tagcache_search_set_match(&tcs, shows_match, &counting);

    if (level || is_basename|| csi->clause_count[0] || TAGCACHE_IS_NUMERIC(tag))
        sort = true;

    add_level_filters(&tcs, level);

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
        /* names of a format of tagnavi's, which take the names' room and
           are stripped: as they were, without Sort By */
        albums = false;
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
    counting.end = (struct show_sums *)song_groups(c);

    /* no <All tracks>, <By album> or <Random> entries in the lists: as on a
       stock iPod, the Music menu has Songs instead. Shuffle Songs first, then
       Albums' Sort By */
    if (shuffle)
    {
        *dptr++ = shuffle_row;
        current_entry_count++;
        c->special_entry_count++;
    }
    if (albums)
    {
        dptr->name = ID2P(LANG_SORT_BY);
        dptr->newtable = TABLE_NAVIBROWSE;
        dptr->extraseek = -2; /* not <Random>'s */
        dptr->customaction = ONPLAY_CUSTOMACTION_SORT_BY;
        dptr->album.year = 0;
        dptr->album.no = -1;
        dptr++;
        current_entry_count++;
        c->special_entry_count++;
    }
    /* and Songs', its rows its titles alone, not in a format's own order,
       which another has no list for (below) */
    const bool songs = offset == 0 && level == 0 && tag == tag_title
        && c->currtable == TABLE_NAVIBROWSE && is_songs(csi) && plain
        && !sort_inverse && !sort_limit && tcs.ramsearch && songs_sortable();
    if (songs)
    {
        *dptr++ = (struct tagentry){
            .name = ID2P(LANG_SORT_BY), .newtable = TABLE_NAVIBROWSE,
            .extraseek = -2, .customaction = ONPLAY_CUSTOMACTION_SORT_BY,
            .album = { .year = 0, .no = -1 }
        };
        current_entry_count++;
        c->special_entry_count++;
    }
    /* past the songs of the parts before, in their rows after those at the
       top */
    const int skip = offset > 0 ? offset - special_before : 0;

    /* Composers lists the composers alone, without a row of the songs that
       have none: most of a library */
    const bool composers = level == 0 && tag == tag_composer
                           && c->currtable == TABLE_NAVIBROWSE;
    /* a row's name is a format's, which it sorts by */
    bool formatted = false;

    while (tagcache_get_next(&tcs, tcs_buf, tcs_bufsz))
    {
        if (composers && !strcmp(tcs.result, UNTAGGED))
            continue;
        if (total_count++ < skip)
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
                formatted = true;
                break;
            }
        }

        if (strcmp(tcs.result, UNTAGGED) == 0)
        {
            if (dptr->newtable != TABLE_PLAYTRACK)
                entries_untagged = tcs.result_seek;
            /* a song without a title by its file's name, in the title's
               place: in an album, its track's */
            if (tag == tag_title
                && song_file_name(&tcs, tcs.idx_id, tcs_buf, tcs_bufsz))
            {
                tcs.result = tcs_buf;
                tcs.ramresult = false;
            }
            else
            {
                tcs.result = (char *)untagged_name(tag);
                tcs.ramresult = true;
            }
            tcs.result_len = strlen(tcs.result) + 1;
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

    /* Shuffle Songs over four songs or more, and other rows: all of them
       found, unless the list is in chunks, which has more anyway */
    if (shuffle && !c->dirfull
        && ((persons ? persons_songs : list_songs) < SHUFFLE_ROW_MIN_SONGS
            || current_entry_count == c->special_entry_count))
    {
        struct tagentry *entries = get_entries(c);
        memmove(&entries[0], &entries[1],
                (current_entry_count - 1) * sizeof(*entries));
        current_entry_count--;
        c->special_entry_count--;
    }

    /* Songs by its Sort By; by name without the row, whatever the order
       chosen, in chunks or without room for the sort, which the lists' size
       decides: list_hash() has it */
    int songs_by = SONGS_BY_NAME;
    if (songs)
    {
        struct song_key *keys = c->dirfull ? NULL
            : songs_sort_room(c, current_entry_count - c->special_entry_count,
                              namebufused);
        if (!keys)
        {
            struct tagentry *entries = get_entries(c);
            const int row = c->special_entry_count - 1;
            memmove(&entries[row], &entries[row + 1],
                    (current_entry_count - row - 1) * sizeof(*entries));
            current_entry_count--;
            c->special_entry_count--;
        }
        else if (!songs_for_orders
                 && (songs_by = songs_order()) != SONGS_BY_NAME)
        {
            struct song_place *rows = songs_order_read(c, songs_by, keys);
            if (!rows)
                rows = songs_order_sort(c, songs_by, keys);
            songs_order_apply(get_entries(c) + c->special_entry_count,
                              current_entry_count - c->special_entry_count,
                              rows, songs_by);
            sort = false;
        }
        /* not sorted again below, by a format's names: the file's places
           are of the list as those in an order have it */
        if (songs_for_orders)
        {
            songs_orders_keys = keys;
            sort = false;
        }
    }

    /* each album's year, now that all its songs are found: the songs without
       an album have none, and go last. With covers, in two lines, and each
       row's album, the first of its name among the songs, whose cover it has */
    if (years && !c->dirfull)
    {
        struct tagentry *entries = get_entries(c);
        entries_two_lines = list_covers_on() && album_numbers_fit();
        for (i = c->special_entry_count; i < current_entry_count; i++)
        {
            const int32_t seek = entries[i].extraseek;
            const bool unknown = seek == entries_untagged;
            entries[i].album.year = unknown ? YEAR_LAST : album_years_row(seek);
            entries[i].album.no = unknown || !entries_two_lines ? -1
                                  : album_years_row_album(seek);
        }
        entries_years = true;
    }
    /* Shows in two lines if all were counted and the sums still fit: sums'
       number in album.year, cover's album in album.no */
    if (shows && !c->dirfull && !counting.full && counting.count <= INT16_MAX
        && current_entry_count > c->special_entry_count
        && namebufused + (long)(counting.count * sizeof(struct show_sums))
           <= c->cache.name_buffer_size)
    {
        struct tagentry *entries = get_entries(c);
        const bool numbers = album_years_ready() && album_numbers_fit();
        for (i = c->special_entry_count; i < current_entry_count; i++)
        {
            const int32_t seek = entries[i].extraseek;
            int k = 0, n;
            while (k < counting.count && counting.end[-1 - k].album != seek)
                k++;
            if (k == counting.count)
                break;
            int no = -1;
            if (numbers && seek != entries_untagged)
            {
                no = album_years_number(seek, counting.end[-1 - k].albumartist);
                if (no < 0)
                    no = album_years_find(seek, &n);
            }
            entries[i].album.year = k;
            entries[i].album.no = no;
        }
        if (i == current_entry_count)
        {
            entries_two_lines = true;
            entries_shows = counting.count;
            entries_books = books;
        }
    }
    /* Albums in two lines, as Artists in Albums or Covers in Lists has them:
       each row's album, the first of its name, and the name's year; with
       Artists in Albums, each album of a name several album artists have a
       row of its own, at its own year, in their order, as Cover Flow has
       them apart. Made from the last row back, with room for the years' sort
       after them, which would split none if there wasn't */
    if (albums && !c->dirfull && album_numbers_fit()
        && (global_settings.album_artists || list_covers_on()))
    {
        struct tagentry *entries = get_entries(c);
        const int first = c->special_entry_count;
        bool split = global_settings.album_artists;
        int more = 0;
        for (i = first; i < current_entry_count && split; i++)
        {
            int n = 0;
            if (entries[i].extraseek != entries_untagged)
                album_years_find(entries[i].extraseek, &n);
            more += MAX(n - 1, 0);
        }
        if (current_entry_count + more + 1 >= c->cache.max_entries / 2)
        {
            split = false;
            more = 0;
        }
        int to = current_entry_count + more;
        for (i = current_entry_count - 1; i >= first; i--)
        {
            const struct tagentry row = entries[i];
            const bool unknown = row.extraseek == entries_untagged;
            int n = 0;
            const int no = unknown ? -1 : album_years_find(row.extraseek, &n);
            const int rows = split && no >= 0 ? n : 1;
            for (int k = rows - 1; k >= 0; k--)
            {
                struct tagentry *e = &entries[--to];
                *e = row;
                e->album.no = no < 0 ? -1 : no + k;
                e->album.year = unknown ? YEAR_LAST
                    : rows > 1 ? album_years_year(no + k)
                    : album_name_year(row.extraseek);
            }
        }
        current_entry_count += more;
        total_count += more;
        entries_two_lines = true;
        entries_split = split;
    }
    /* Albums by their dates, each name's, or each album's own when split;
       those of a date by name, as the database has them already. Through the
       room after them, without a comparison of names: 1666 names were
       seconds' work by qsort. In one line, album.no only for the sort: the
       year's text, keep_lead(), takes its place after it */
    if (albums && global_settings.albums_sort == 0 && !c->dirfull)
    {
        struct tagentry *entries = get_entries(c);
        const int n = current_entry_count - c->special_entry_count;
        for (i = c->special_entry_count; i < current_entry_count && !entries_two_lines;
             i++)
        {
            const bool unknown = entries[i].extraseek == entries_untagged;
            int of_name;
            const int no = unknown ? -1 : album_years_find(entries[i].extraseek,
                                                           &of_name);
            entries[i].album.year = unknown ? YEAR_LAST
                                            : album_years_name_year_of(no);
            entries[i].album.no = album_numbers_fit() ? no : -1;
        }
        sort_by_year(&entries[c->special_entry_count], n,
                     &entries[current_entry_count]);
        entries_years = true;
    }

    /* a show's episodes by date, not by their format's names */
    const bool by_date = episodes && !c->dirfull && !sort_inverse && !sort_limit;
    if (by_date)
    {
        struct tagentry *entries = get_entries(c);
        qsort(&entries[c->special_entry_count],
              current_entry_count - c->special_entry_count,
              sizeof(struct tagentry), compare_episodes);
        entries_two_lines = true;
        entries_episodes = true;
    }

    /* A long list stays in the database's order, as sorted already: 13000 names
       took seconds, and this is the order it always had in chunks */
    if (sort && !by_date && current_entry_count <= TAGTREE_SORT_MAX)
    {
        /* by place, as the database has them; a format's names by what they
           show, as they did, files' by their names; songs without their
           places by Music's rules */
        sort_by_place = !formatted && !is_basename && TAGCACHE_IS_SORTED(tag)
                        && (tag != tag_title || song_places_known());
        sort_places_inverse = sort_inverse;
        sort_places_tag = tag;
        if (!formatted && !is_basename && TAGCACHE_IS_SORTED(tag))
            qsort_fn = sort_inverse ? sortkey_compare_n_inv : sortkey_compare_n;
        else if (global_settings.interpret_numbers)
            qsort_fn = sort_inverse ? strnatcasecmp_n_inv : strnatcasecmp_n;
        else
            qsort_fn = sort_inverse ? strncasecmp_inv : strncasecmp;

        struct tagentry *entries = get_entries(c);
        qsort(&entries[c->special_entry_count],
              current_entry_count - c->special_entry_count,
              sizeof(struct tagentry),
              entries_years ? compare_years :
              c->currtable == TABLE_ALLSUBENTRIES_SORTED_BY_ALBUMS ? compare_with_albums : compare
        );
    }

    /* Quick Scroll's letters, for rows of names in their order: sorted here,
       by their places or by their names, or as the database has them, which
       a list of the tag alone at the top is in, in chunks too */
    entries_initials_tag = tag;
    if (!formatted && !is_basename && !sort_inverse && !entries_of_stats
        && !with_album && !by_date && TAGCACHE_IS_SORTED(tag))
    {
        if (sort && current_entry_count <= TAGTREE_SORT_MAX)
            entries_initials = sort_by_place ? INITIALS_BY_PLACE : INITIALS_BY_NAME;
        else if (level == 0 && !csi->clause_count[0])
            entries_initials = tag != tag_title || song_places_known() ?
                               INITIALS_BY_PLACE : INITIALS_BY_NAME;
    }
    /* by year, the album's year before the title, as Albums by year; by
       artist or album, that name under the title, and Quick Scroll by it */
    if (songs_by != SONGS_BY_NAME)
    {
        entries_years = songs_by == SONGS_BY_YEAR;
        entries_two_lines = !entries_years;
        entries_initials = entries_years ? INITIALS_NONE : INITIALS_BY_PLACE;
        entries_initials_tag = songs_by == SONGS_BY_ARTIST ?
                               tag_virt_canonicalartist : tag_album;
    }

    /* each song's number and its disc's bar, now that they're in their
       order, as all of them are found; unless their names have the number */
    if (numbered && !c->dirfull && !format_shows_track(group, strip))
        entries_tracks = number_songs(c, &tcs, namebufused);

    /* The row of the songs without the tag, "Unknown Artist", the last: after
       the names, and the albums without a year. Found by its seek, wherever the
       database's order or a sort put it. Not among songs, nor in a list in
       chunks, whose rows are places in the database */
    if (entries_untagged >= 0 && offset == 0 && !c->dirfull)
    {
        struct tagentry *entries = get_entries(c);
        for (i = c->special_entry_count; i < current_entry_count - 1; i++)
        {
            if (entries[i].extraseek == entries_untagged)
            {
                const struct tagentry row = entries[i];
                memmove(&entries[i], &entries[i + 1],
                        (current_entry_count - 1 - i) * sizeof(*entries));
                entries[current_entry_count - 1] = row;
                break;
            }
        }
    }

    /* the years, sorted by, kept as the texts before the albums' names; in
       two lines, under them */
    if (entries_years && !entries_two_lines)
    {
        struct tagentry *entries = get_entries(c);
        for (i = c->special_entry_count; i < current_entry_count; i++)
            keep_lead(&entries[i], entries[i].album.year);
    }

    /* All Songs over an artist's albums, two or more of them, as on a stock
       iPod: one row of the artist's songs, below any rows already at the top,
       so that Shuffle Songs would stay first. Not if they are more than its
       list can gather in half the lists' room, with a row to spare at its
       top; nor over albums with no list below them */
    if (persons && !c->dirfull
        && current_entry_count - c->special_entry_count >= 2
        && persons_songs < c->cache.max_entries / 2
        && level + 1 < csi->tagorder_count)
    {
        struct tagentry *entries = get_entries(c);
        const int top = c->special_entry_count;
        memmove(&entries[top + 1], &entries[top],
                (current_entry_count - top) * sizeof(*entries));
        entries[top] = (struct tagentry){
            .name = ID2P(LANG_ALL_SONGS), .newtable = TABLE_ALLSUBENTRIES,
            .extraseek = -3, /* not <Random>'s, nor Sort By's */
            .customaction = ONPLAY_NO_CUSTOMACTION,
            .album = { .year = YEAR_LAST, .no = -1 }
        };
        c->special_entry_count++;
        current_entry_count++;
    }

    if (!init)
    {
        tagcache_search_finish(&tcs);
        tree_unlock_cache(c);
        core_unpin(tagtree_handle);
        if (offset > 0)
            c->special_entry_count = special_before;
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
        if (!(composers && !strcmp(tcs.result, UNTAGGED)))
            total_count++;
    }

    tagcache_search_finish(&tcs);
    if (entries_books)
        books_finish(c, &counting, tcs_buf, tcs_bufsz);
    if (parts && !sort_inverse && !sort_limit)
        parts_finish(c);
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
    total_count += c->special_entry_count;

    if (strip)
    {
        /* the names, past the rows at the top, whose names are phrases */
        dptr = get_entries(c) + c->special_entry_count;
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
    c->special_entry_count = 0; /* Albums' Sort By, if it was there */
    if (c->dirlevel == 0)
        c->currextra = rootmenu;

    menu = menus[c->currextra];
    if (menu == NULL)
        return 0;

    root_rows = 0;

    /* Music's own menu without the sections turned off in Music Menu: a row
       goes into the item of its extraseek, not of its number */
    const bool all = root_all_items || c->currextra != rootmenu;
    if (c->currextra == rootmenu)
        recent_row_seen = recent_row();
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

/* the list entered last has no row left in it to go back to: it opens at
   first_row() as it's loaded */
static bool open_at_first_row;
static void shuffle_rows_check(struct tree_context *c);

/* The row a list opens at, with none left in it to go back to: its first
   song, album or artist, past the rows at its top, Shuffle Songs, Sort By or
   All Songs, a step up from it and in sight (the owner's decision,
   2026-11-08): an artist's first album, All Songs' first song, and Albums'
   first album rather than its Sort By */
static int first_row(struct tree_context *c)
{
    /* a book's screen opens on its Resume or Listen Again, the row over its
       parts */
    if (c->special_entry_count > 0 && entries_parts.parts > 1
        && (entries_parts.current >= 0 || entries_parts.finished))
        return c->special_entry_count - 1;
    return c->special_entry_count;
}

int tagtree_load(struct tree_context* c)
{
    logf( "%s", __func__);

    int count;
    int table = c->currtable;
    const bool at_first_row = open_at_first_row;
    open_at_first_row = false;
    shuffle_rows_check(c);

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
    album_view_make(c);

    /* and a list whose row to go back to was forgotten, as its rows moved */
    if (at_first_row || c->selected_item < 0)
        c->selected_item = first_row(c);
    /* a book's screen made again with Resume now over its parts, or no
       longer: the row to go back to is the part it was, as many rows on */
    else if (entries_parts.parts > 0 && album_view.songs > 0
             && album_view.album == parts_album
             && c->selected_item >= parts_special)
        c->selected_item += c->special_entry_count - parts_special;
    if (entries_parts.parts > 0 && album_view.songs > 0)
    {
        parts_album = album_view.album;
        parts_special = c->special_entry_count;
    }

    /* the list opened in place of a copy's top, at the row it was left by */
    if (artists_copy_left[0])
    {
        if (c->currtable == TABLE_NAVIBROWSE && c->currextra == 0)
            c->selected_item = MAX(tagtree_find_row(c, artists_copy_left), 0);
        artists_copy_left[0] = '\0';
    }

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
/* the level that was opened at the track playing, where Menu leaves the lists */
static int go_to_level = -1;

static void place_save(struct tagtree_place *place, int dirlevel, int selected_item)
{
    const struct tree_context *c = tree_get_context();
    place->menu = menu;
    place->csi = csi;
    if (csi)
    {
        memcpy(place->result_seek, csi->result_seek, sizeof(csi->result_seek));
        memcpy(place->result_artist, csi->result_artist,
               sizeof(csi->result_artist));
    }
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
    {
        memcpy(csi->result_seek, place->result_seek, sizeof(csi->result_seek));
        memcpy(csi->result_artist, place->result_artist,
               sizeof(csi->result_artist));
    }
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

/* Of saved_place, where the music playing was started: the start's count and
   its database, and the list it is, the queue's id then, which a list of songs
   has. Kept over a boot by its list, as the start's count isn't (~0u). */
static unsigned int saved_starts = ~0u;
static int32_t saved_commit;
static uint32_t saved_list, saved_key;
/* the start of the whole library's Shuffle Songs atop a list of it, which
   isn't that list's music: no place, as for Shuffle Songs of the main menu */
static unsigned int unplaced_starts = ~0u;

static void go_to_end(int row, int *dirlevel, int *selected_item);

static uint32_t list_hash(int table, int extra, const struct search_instruction *si,
                          const int *seeks, const int *artists, bool by_order);

/* Which list a place is at, by the rows chosen on the way there as the place
   has them, but not by the order of its songs, which All Songs' Sort By
   changes, and the albums' years made since the boot */
static uint32_t place_key(const struct tagtree_place *place)
{
    if (!place->csi)
        return 0;
    return list_hash(place->currtable, place->currextra, place->csi,
                     place->result_seek, place->result_artist, false);
}

static void kept_place_write(void);

void tagtree_save_place(int dirlevel, int selected_item)
{
    /* the place, for Menu over Now Playing to go back to, but none to play
       from for the whole library's Shuffle Songs */
    place_save(&saved_place, dirlevel, selected_item);
    const bool placed = playlist_start_count() != unplaced_starts;
    saved_starts = placed ? playlist_start_count() : ~0u;
    saved_commit = tagcache_get_commitid();
    saved_list = placed ? tagtree_queue_list() : 0;
    saved_key = place_key(&saved_place);
    /* kept over a reset, which writes nothing, while the disk is up for the
       playlist started */
    if (saved_list && tagnavi_loaded)
        kept_place_write();
}

/* Whether the place where the music playing was started is known, a list of
   Music's: the music started there since, or the list it was playing still,
   as after a boot. That one is the list the place is at still, which a
   tagnavi.config of another version would make another: or else unknown. */
bool tagtree_playing_from_known(void)
{
    if (!saved_place.csi || saved_place.currtable == TABLE_ROOT
        || saved_commit != tagcache_get_commitid())
        return false;
    return saved_starts == playlist_start_count()
           || (saved_list && saved_list == tagtree_queue_list()
               && place_key(&saved_place) == saved_key);
}

/* The lists there, from Now Playing's Playing From, with the song playing
   selected if the list has it, or else the row it was started from. Until
   tagtree_end_go_to(), Menu in that list leaves the lists. False if it isn't
   known. */
bool tagtree_go_to_from(int *dirlevel, int *selected_item)
{
    struct tree_context *c = tree_get_context();
    const struct mp3entry *id3 = audio_current_track();
    if (!tagtree_ready() || !tagcache_is_usable() || !tagtree_playing_from_known())
        return false;
    place_save(&go_to_place, *dirlevel, *selected_item);
    place_restore(&saved_place, dirlevel, selected_item);
    c->dirlevel = *dirlevel;
    c->selected_item = *selected_item;
    if (tagtree_load(c) <= 0)
    {
        place_restore(&go_to_place, dirlevel, selected_item);
        return false;
    }
    int row = MIN(c->selected_item, c->dirlength - 1);
    const long idx = id3 ? id3->tagcache_idx - 1 : -1;
    for (int i = c->special_entry_count; i < c->dirlength && idx >= 0; i++)
    {
        const struct tagentry *entry = tagtree_get_entry(c, i);
        if (entry->newtable == TABLE_PLAYTRACK && entry->extraseek == idx)
        {
            row = i;
            break;
        }
    }
    go_to_end(row, dirlevel, selected_item);
    return true;
}

/* That place kept over a boot, in a file written as the iPod shuts down or
   USB is connected, not as music starts, which it would make wait: its
   pointers as the menus' and searches' numbers */
#define KEPT_PLACE_FILE  ROCKBOX_DIR "/tagtree_from.dat"
#define KEPT_PLACE_MAGIC 0x50465752 /* "RWFP" */
struct kept_place {
    uint32_t magic;
    uint32_t size;          /* of the place, as this build has it */
    int32_t commitid;
    uint32_t list;
    uint32_t key;           /* place_key(), of the menus as they were */
    int32_t menu;           /* in menus[] */
    int32_t csi_menu, csi_item; /* in menus[], -1 for a copy of Artists */
    int32_t copy, copy_tag; /* the copy's, artists_copy[] and its top's tag */
    struct tagtree_place place;
};
/* one for writing it and for reading it: never both at once */
static struct kept_place kept;

static int menu_index(const struct menu_root *m)
{
    for (int i = 0; i < menu_count; i++)
    {
        if (menus[i] == m)
            return i;
    }
    return -1;
}

/* the hash of the file as it is on the disk, 0 for none: it's written again
   only as the place changes */
static uint32_t kept_on_disk;

static void kept_place_remove(void)
{
    remove(KEPT_PLACE_FILE);
    kept_on_disk = 0;
}

/* saved_place into the file, unless it's there already: its pointers as the
   numbers of the menus and searches they point into */
static void kept_place_write(void)
{
    kept = (struct kept_place){
        .magic = KEPT_PLACE_MAGIC, .size = sizeof(kept.place),
        .commitid = saved_commit, .list = saved_list, .key = saved_key,
        .menu = menu_index(saved_place.menu),
        .csi_menu = -1, .csi_item = -1, .copy = -1, .copy_tag = -1,
        .place = saved_place,
    };
    for (int i = 0; i < 2; i++)
    {
        if (saved_place.csi == &artists_copy[i])
        {
            kept.copy = i;
            kept.csi_item = artists_copy_item[i];
            kept.copy_tag = artists_copy[i].tagorder[0];
        }
    }
    for (int m = 0; m < menu_count && kept.copy < 0 && kept.csi_menu < 0; m++)
    {
        for (int i = 0; i < menus[m]->itemcount; i++)
        {
            if (saved_place.csi == &menus[m]->items[i]->si)
            {
                kept.csi_menu = m;
                kept.csi_item = i;
                break;
            }
        }
    }
    if (kept.menu < 0 || (kept.copy < 0 && kept.csi_menu < 0))
    {
        kept_place_remove();
        return;
    }
    /* of no use in a file, and another each boot */
    kept.place.menu = NULL;
    kept.place.csi = NULL;
    const uint32_t hash = words_hash(&kept, sizeof(kept), 2166136261u) | 1;
    if (hash == kept_on_disk)
        return;
    if (whole_file_write(KEPT_PLACE_FILE, &kept, sizeof(kept)))
        kept_on_disk = hash;
    else
        kept_place_remove();
}

/* As USB is connected or the iPod shuts down: the place as it is then, or none.
   Written as music starts from a list too (tagtree_save_place()), as a reset by
   Menu and Select shuts nothing down; then the disk is up for the playlist's
   control file anyway, and here it's written only if it has changed since. */
void tagtree_keep_place(void)
{
    /* nothing played since the boot, nor resumed: the file kept is still of
       the music to resume, which isn't named before that */
    if (playlist_start_count() == 0)
        return;
    if (!tagnavi_loaded || !tagtree_playing_from_known() || !saved_list)
        kept_place_remove();
    else
        kept_place_write();
}

/* at the start, the menus read */
static void kept_place_read(void)
{
    int fd = open(KEPT_PLACE_FILE, O_RDONLY);
    if (fd < 0)
        return;
    const bool got = read(fd, &kept, sizeof(kept)) == sizeof(kept);
    close(fd);
    const struct tagtree_place *place = &kept.place;
    if (!got || kept.magic != KEPT_PLACE_MAGIC || kept.size != sizeof(kept.place)
        || kept.menu < 0 || kept.menu >= menu_count || !menus[rootmenu]
        || place->currextra < 0 || place->currextra >= MAX_TAGS
        || place->dirlevel < 0 || place->dirlevel >= MAX_DIR_LEVELS
        || place->max_history_level < 0
        || place->max_history_level >= MAX_DIR_LEVELS)
        return;
    struct search_instruction *si = NULL;
    if (kept.copy >= 0 && kept.copy < 2 && kept.csi_item >= 0
        && kept.csi_item < menus[rootmenu]->itemcount)
    {
        /* made again as go_to_row() makes it, the same each time */
        si = &artists_copy[kept.copy];
        *si = menus[rootmenu]->items[kept.csi_item]->si;
        si->tagorder[0] = kept.copy_tag;
        artists_copy_item[kept.copy] = kept.csi_item;
    }
    else if (kept.copy < 0 && kept.csi_menu >= 0 && kept.csi_menu < menu_count
             && kept.csi_item >= 0 && kept.csi_item < menus[kept.csi_menu]->itemcount)
        si = &menus[kept.csi_menu]->items[kept.csi_item]->si;
    if (!si)
        return;
    saved_place = kept.place;
    saved_place.menu = menus[kept.menu];
    saved_place.csi = si;
    saved_commit = kept.commitid;
    saved_list = kept.list;
    saved_key = kept.key;
    kept_on_disk = words_hash(&kept, sizeof(kept), 2166136261u) | 1;
}

void tagtree_restore_place(int *dirlevel, int *selected_item)
{
    place_restore(&saved_place, dirlevel, selected_item);
}

/* The row of the list with this name, or -1: for the database's <Untagged>,
   the row of the songs without the list's tag, by its seek, as a tag may have
   the text that row shows */
static int tagtree_find_row(struct tree_context *c, const char *name)
{
    const bool untagged = !strcmp(name, UNTAGGED);
    /* past Sort By and All Songs, whose names are phrases' numbers */
    for (int i = c->special_entry_count; i < c->dirlength; i++)
    {
        /* got first: a part of a list in chunks finds its seek. A tag with
           the name that row shows isn't it, where the row isn't moved last */
        const struct tagentry *row = tagtree_get_entry(c, i);
        const bool unknown = entries_untagged >= 0
                             && row->extraseek == entries_untagged;
        if (untagged ? unknown : !unknown && !strcmp(row->name, name))
            return i;
    }
    return -1;
}

/* The row of the album of this name and album artist's seek, as Albums has
   the albums of a name apart; or else the first row of the name, as a list
   of one row for all of them has it, or -1 */
static int find_album_row(struct tree_context *c, const char *name,
                          int32_t artist)
{
    for (int i = c->special_entry_count; i < c->dirlength && entries_two_lines
                                         && artist >= 0; i++)
    {
        const struct tagentry *row = tagtree_get_entry(c, i);
        if (row->album.no >= 0 && album_years_artist(row->album.no) == artist
            && !strcmp(row->name, name))
            return i;
    }
    return tagtree_find_row(c, name);
}

/* The list of them at the top of Music entered, and the row of name in it, as
   by hand: true with c at the list that row opens, or else the lists back as
   they were, from back, where they are saved first. The list is the item's
   whose top is of item_tag, made of list_tag instead if that's another: the
   albums' artists in place of the songs' or the other way round
   (artists_copy). An album's row by its album artist's seek too, if artist
   isn't -1. */
static bool go_to_row(int item_tag, int list_tag, const char *name,
                      int32_t artist, struct tagtree_place *back, int *dirlevel,
                      int *selected_item)
{
    struct tree_context *c = tree_get_context();
    int row = -1;

    place_save(back, *dirlevel, *selected_item);
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
            if (menu->items[i]->type == menu_next && si->tagorder[0] == item_tag
                && si->clause_count[0] == 0 && !is_recent(si))
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
        if (list_tag != item_tag)
        {
            const int k = list_tag == tag_albumartist ? 0 : 1;
            artists_copy[k] = *csi;
            artists_copy[k].tagorder[0] = list_tag;
            artists_copy_item[k] = row;
            csi = &artists_copy[k];
        }
        tagtree_load(c);
        row = find_album_row(c, name, artist);
    }
    if (row < 0)
    {
        place_restore(back, dirlevel, selected_item);
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

/* what Music's Artists lists now: the songs' artists, as tagnavi.config has
   them, or the albums' (Group by Album Artist) */
static int artists_tag(void)
{
    const int item = root_item_of(LANG_ARTISTS);
    if (item < 0 || menus[rootmenu]->items[item]->type != menu_next)
        return tag_virt_canonicalartist;
    return menus[rootmenu]->items[item]->si.tagorder[0];
}

/* Puts the lists at the song idx, as one gets there by hand: its album's artist, or
   else its own, with its album selected, or its album with the song selected. If
   Artists lists the songs' artists, where a guest on one song has a row of their own,
   the list is of the albums' artists in its place, so that the albums here are those
   with that album artist. The row selected, or -1, the lists back as they were from
   back, where they are saved first. */
static int go_to_song(int idx, bool artist, struct tagtree_place *back,
                      int *dirlevel, int *selected_item)
{
    static char name[TAGCACHE_BUFSZ], album[TAGCACHE_BUFSZ];
    struct tree_context *c = tree_get_context();
    struct tagcache_search tcs;

    if (idx < 0 || !tagcache_search(&tcs, tag_title))
        return -1;
    const int tag = artist ? artists_tag() : tag_album;
    const bool by_album_artist = artist
        && tagcache_retrieve(&tcs, idx, tag_albumartist, name, sizeof(name))
        && name[0] && strcmp(name, UNTAGGED);
    const bool found = (by_album_artist
                        || tagcache_retrieve(&tcs, idx, tag, name, sizeof(name)))
        && tagcache_retrieve(&tcs, idx, tag_album, album, sizeof(album));
    tagcache_search_finish(&tcs);
    /* the album's row of its album artist, of those of its name */
    const int32_t album_artist = !artist && song_places_known() ?
                                 tagcache_sort_key(idx, tag_albumartist) : -1;
    if (!found || !go_to_row(tag, by_album_artist ? tag_albumartist : tag, name,
                             album_artist, back, dirlevel, selected_item))
        return -1;

    /* the one gone to */
    if (artist)
    {
        const int row = tagtree_find_row(c, album);
        return row >= 0 ? row : first_row(c);
    }
    for (int i = 0; i < c->dirlength; i++)
    {
        if (tagtree_get_entry(c, i)->extraseek == idx)
            return i;
    }
    return first_row(c);
}

/* The lists at a song by its file, as Go to Artist and Go to Album over Now Playing,
   in Search, a playlist and Cover Flow put them. tagtree_end_go_to() ends this, and
   until then Menu in that list leaves the lists. False if it isn't in the
   database. */
bool tagtree_go_to_track(const char *path, bool artist, int *dirlevel,
                         int *selected_item)
{
    struct tagcache_search tcs;
    if (!tagtree_ready() || !tagcache_is_usable()
        || !tagcache_find_index(&tcs, path))
        return false;
    const int idx = tcs.idx_id;
    tagcache_search_finish(&tcs);
    const int row = go_to_song(idx, artist, &go_to_place, dirlevel, selected_item);
    if (row < 0)
        return false;
    go_to_end(row, dirlevel, selected_item);
    return true;
}

bool tagtree_go_to_playing(bool artist, int *dirlevel, int *selected_item)
{
    const struct mp3entry *id3 = audio_current_track();
    return id3 && tagtree_go_to_track(id3->path, artist, dirlevel, selected_item);
}

/* the level of the list opened in place by Go to in a list's menu, where Menu
   goes back to the song it was chosen on; -1 for none. And whether that was in
   Cover Flow instead, gone back to at the album of that song */
static int browse_level = -1;
static int browse_cover_flow = -1;

/* The first song of a row that isn't one, of the list: as the list's search
   finds it, by the rows chosen on the way and the row's own tag. -1 if none */
static int first_song_of(struct tree_context *c, const struct tagentry *entry)
{
    struct tagcache_search tcs;
    const int level = c->currextra;
    if (c->currtable != TABLE_NAVIBROWSE || level >= csi->tagorder_count
        || TAGCACHE_IS_NUMERIC(csi->tagorder[level])
        || !tagcache_search(&tcs, tag_title))
        return -1;
    add_level_filters(&tcs, level);
    tagcache_search_add_filter(&tcs, csi->tagorder[level], entry->extraseek);
    /* of an album whose name several album artists have, its own song */
    const int32_t artist = row_artist(entry);
    if (artist >= 0)
        tagcache_search_add_filter(&tcs, tag_albumartist, artist);
    core_pin(tagtree_handle);
    for (int i = 0; i <= level; i++)
    {
        for (int j = 0; j < csi->clause_count[i]; j++)
            tagcache_search_add_clause(&tcs, csi->clause[i][j]);
    }
    char buf[TAGCACHE_BUFSZ];
    const int idx = tagcache_get_next(&tcs, buf, sizeof(buf)) ? tcs.idx_id : -1;
    tagcache_search_finish(&tcs);
    core_unpin(tagtree_handle);
    return idx;
}

/* the songs of an album's own list: under its row of Albums, of an artist's
   albums, of a show or a book */
static bool album_list(struct tree_context *c)
{
    return c->currtable == TABLE_NAVIBROWSE && c->currextra > 0
           && csi->tagorder[c->currextra - 1] == tag_album;
}

/* Go to Album on a song of a list but its album's own, and Go to Artist on a
   song or an album: false for other rows, and before the database is ready */
bool tagtree_row_goes_to(bool album)
{
    struct tree_context *c = tc;
    if (!c || c->dirlevel == 0 || c->currtable == TABLE_ROOT || !csi
        || !tagtree_ready() || !tagcache_is_usable()
        || c->selected_item < c->special_entry_count
        || c->selected_item >= c->filesindir)
        return false;
    const struct tagentry *entry = tagtree_get_entry(c, c->selected_item);
    if (!entry)
        return false;
    if (entry->newtable == TABLE_PLAYTRACK)
        return !album || !album_list(c);
    return !album && c->currtable == TABLE_NAVIBROWSE
           && c->currextra < csi->tagorder_count
           && csi->tagorder[c->currextra] == tag_album;
}

/* The lists at the song, in place: Menu there back to where they were. A
   second time from there keeps the first song to go back to, so that none
   goes back to a list that goes back to it (the owner's decision,
   2026-11-09). */
static bool browse_to(struct tree_context *c, int idx, bool artist)
{
    const bool first = browse_level < 0;
    int dirlevel = c->dirlevel, selected_item = c->selected_item;
    const int row = go_to_song(idx, artist, first ? &browse_place : &scratch_place,
                               &dirlevel, &selected_item);
    if (row < 0)
    {
        /* the lists as they were, from the place go_to_row() saved, its
           level and row too; the list itself is to be loaded again */
        c->dirlevel = dirlevel;
        c->selected_item = selected_item;
        return false;
    }
    c->selected_item = row;
    browse_level = c->dirlevel;
    if (first)
        browse_cover_flow = -1;
    return true;
}

/* Go to Artist or Go to Album on the row selected (tagtree_row_goes_to()), in
   place: an album by its first song. False, the lists as they were, if it
   isn't found. Either way the list is to be loaded again: the search went
   through others */
bool tagtree_browse_from_row(bool artist)
{
    struct tree_context *c = tc;
    const struct tagentry *entry = tagtree_get_entry(c, c->selected_item);
    if (!entry)
        return false;
    return browse_to(c, entry->newtable == TABLE_PLAYTRACK ? entry->extraseek
                                                           : first_song_of(c, entry),
                     artist);
}

/* Cover Flow's Go to Artist, from Music's own row: the same, Menu there back
   to Cover Flow at the album of that song */
bool tagtree_browse_from_cover_flow(const char *path)
{
    struct tagcache_search tcs;
    if (!tagtree_ready() || !tagcache_is_usable()
        || !tagcache_find_index(&tcs, path))
        return false;
    const int idx = tcs.idx_id;
    tagcache_search_finish(&tcs);
    if (!browse_to(tc, idx, true))
        return false;
    browse_cover_flow = idx;
    return true;
}

int tagtree_browse_level(void)
{
    return browse_level;
}

/* Menu in the list opened in place: back to the song it was opened from, the
   cursor on it. True if that was Cover Flow, the file of the song to open it
   at in buf. */
bool tagtree_end_browse(int *dirlevel, int *selected_item, char *buf, size_t size)
{
    struct tagcache_search tcs;
    place_restore(&browse_place, dirlevel, selected_item);
    browse_level = -1;
    const int idx = browse_cover_flow;
    browse_cover_flow = -1;
    if (idx < 0 || !tagcache_search(&tcs, tag_filename))
        return false;
    const bool found = tagcache_retrieve(&tcs, idx, tag_filename, buf, size);
    tagcache_search_finish(&tcs);
    return found;
}

/* the lists left, for Now Playing or another screen: they stay where they are,
   as after Go to over Now Playing, Menu there going up as by hand */
void tagtree_forget_browse(void)
{
    browse_level = -1;
    browse_cover_flow = -1;
}
bool tagtree_go_to_name(int tag, const char *name, int *dirlevel, int *selected_item)
{
    if (!tagtree_ready() || !tagcache_is_usable())
        return false;
    /* An artist: the row of Artists, or else of the other artists' list in its
       place, as a guest on a song has none among the albums' artists: their
       albums, each with their songs, as in Artists by the songs' artists */
    const bool artist = tag == tag_virt_canonicalartist;
    const int item_tag = artist ? artists_tag() : tag;
    const int other = item_tag == tag_albumartist ? tag_virt_canonicalartist
                                                  : tag_albumartist;
    if (!go_to_row(item_tag, item_tag, name, -1, &go_to_place, dirlevel,
                   selected_item)
        && (!artist || !go_to_row(item_tag, other, name, -1, &go_to_place,
                                  dirlevel, selected_item)))
        return false;
    go_to_end(first_row(tree_get_context()), dirlevel, selected_item);
    return true;
}

bool tagtree_go_to_album(const char *name, int32_t artist, int *dirlevel,
                         int *selected_item)
{
    if (!tagtree_ready() || !tagcache_is_usable()
        || !go_to_row(tag_album, tag_album, name, artist, &go_to_place, dirlevel,
                      selected_item))
        return false;
    go_to_end(first_row(tree_get_context()), dirlevel, selected_item);
    return true;
}

bool tagtree_has_section(int lang)
{
    return root_item_of(lang) >= 0;
}

/* the section of Music opened from the main menu, -1 for none; and the row
   each was left at, to open there again */
static int section_open = -1;
static int section_rows[MUSIC_SECTIONS] = { [0 ... MUSIC_SECTIONS - 1] = -1 };

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
    /* at its first row if none is remembered, as its rows moved */
    go_to_end(section_rows[section] < 0 ? first_row(c)
                                        : MIN(section_rows[section], MAX(count - 1, 0)),
              dirlevel, selected_item);
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
    /* a Sort By, which switches the order, and Shuffle Songs, which plays
       (tagtree_shuffle_list()): into nothing */
    if (!dptr || dptr->customaction == ONPLAY_CUSTOMACTION_SORT_BY
        || dptr->customaction == ONPLAY_CUSTOMACTION_SHUFFLE_LIST)
        return 0;

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

    /* the list entered has no row left in it to go back to */
    bool unvisited = false;
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
            unvisited = true;
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
                for (i = 0; i < MAX_TAGS; i++)
                    csi->result_artist[i] = -1;

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
            csi->result_artist[c->currextra] = row_artist(dptr);
            if (c->currextra < csi->tagorder_count-1)
                c->currextra++;
            else
                c->dirlevel--;

            unsigned char *name = dptr->name;
            name = P2STR(name);
            logf("%s (NAVI/ALLSUB) current title %s", __func__, name);
            /* Update the statusbar title: of the row of the songs without the
               tag, the database's name, which tagtree_get_title() shows as
               the row was, and by which the row is found again. All Songs
               has the artist's, as the list of albums it is over */
            if (newextra == TABLE_ALLSUBENTRIES)
                name = (unsigned char *)current_title[c->currextra - 1];
            else if (entries_untagged >= 0 && seek == entries_untagged)
                name = (unsigned char *)UNTAGGED;
            strmemccpy(current_title[c->currextra], (char *)name,
                       sizeof(current_title[0]));
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
        open_at_first_row = unvisited;
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

    /* Up to the top of a copy, as the go-to that made it ended in music
       played: Menu leaves a go-to's lists before that. The list of the other
       artists isn't Music's, so the item's own list is opened in its place,
       as by hand, at the row of the name left if it has one. */
    for (int k = 0; is_visible && k < 2; k++)
    {
        if (csi != &artists_copy[k] || c->currtable != TABLE_NAVIBROWSE
            || c->currextra != 0
            || artists_copy_item[k] >= menus[rootmenu]->itemcount)
            continue;
        csi = &menus[rootmenu]->items[artists_copy_item[k]]->si;
        strmemccpy(artists_copy_left, current_title[1],
                   sizeof(artists_copy_left));
        max_history_level = MIN(max_history_level, c->dirlevel);
    }
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
/* the rows of Albums in the other order: a place in it, at its first row (-1,
   as tagtree_load() finds it); or one in Recently Added */
static void albums_place_reset(struct tagtree_place *place, bool recent)
{
    if (!(recent ? is_recent(place->csi) : is_albums(place->csi))
        || place->dirlevel < 1)
        return;
    if (place->dirlevel == 1)
        place->selected_item = -1;
    else
        place->selected_item_history[1] = -1;
    place->max_history_level = MIN(place->max_history_level, 1);
}

/* the years made again: Albums in another order, as of the library now, or
   by its years, which weren't there */
void tagtree_albums_reordered(void)
{
    if (recent_row_seen >= 0 && recent_row_seen != recent_row())
        tagtree_music_menu_changed(true);
    const int section = section_of(LANG_ALBUMS);
    if (section >= 0)
        section_rows[section] = -1;
    albums_place_reset(&saved_place, false);
    albums_place_reset(&go_to_place, false);
    /* and Recently Added, whose rows are the table's too */
    albums_place_reset(&saved_place, true);
    albums_place_reset(&go_to_place, true);
    if ((is_albums(csi) || is_recent(csi)) && max_history_level >= 1)
    {
        selected_item_history[1] = -1;
        max_history_level = 1;
    }
}

/* Group by Album Artist switched: a row chosen in the artists of Artists or
   Genres, which the lists below it were made of, isn't one of the new ones. A
   place there or below goes back to that list, at its first row, and the rows
   to go back to from there down are forgotten. False for another place. */
static bool artists_place_cut(struct tagtree_place *place)
{
    const int level = artists_level(place->csi);
    if (level < 0 || (place->currtable == TABLE_ROOT
                      && place->menu != menus[rootmenu]))
        return false;
    /* the level of the lists that list is at, from Music's own menu or
       from one of the lists of that item */
    const int list = place->currtable == TABLE_ROOT
                     ? place->dirlevel + 1 + level
                     : place->dirlevel - (place->currextra - level);
    if (list <= place->dirlevel)
    {
        place->dirlevel = list;
        place->currtable = TABLE_NAVIBROWSE;
        place->currextra = level;
        place->selected_item = -1;
    }
    place->max_history_level = MIN(place->max_history_level, list);
    for (int i = MAX(list, 0); i < MAX_DIR_LEVELS; i++)
        place->selected_item_history[i] = -1;
    return true;
}

void tagtree_artists_grouping_changed(int *dirlevel, int *selected_item)
{
    /* as they are already, as after a .cfg that didn't change it, or none */
    const int tag = artist_levels_tag();
    if (tag < 0 || tag == artist_levels_wanted())
        return;
    artist_levels_apply();
    artists_place_cut(&saved_place);
    artists_place_cut(&go_to_place);
    artists_place_cut(&browse_place);
    /* where the lists were left */
    place_save(&scratch_place, *dirlevel, *selected_item);
    if (artists_place_cut(&scratch_place))
        place_restore(&scratch_place, dirlevel, selected_item);
    const int section = section_of(LANG_ARTISTS);
    if (section >= 0)
        section_rows[section] = -1;
}

/* a place in All Songs, whose songs are in the other order: at its first */
static void all_songs_place_reset(struct tagtree_place *place)
{
    if (place->currtable == TABLE_ALLSUBENTRIES)
        place->selected_item = -1;
}

/* a place in Songs, whose songs are in another order: at its first */
static void songs_place_reset(struct tagtree_place *place)
{
    if (is_songs(place->csi) && place->currtable == TABLE_NAVIBROWSE)
        place->selected_item = -1;
}

void tagtree_sort_by_switched(struct tree_context *c)
{
    if (c->currtable == TABLE_ALLSUBENTRIES)
    {
        global_settings.all_songs_sort = !global_settings.all_songs_sort;
        all_songs_place_reset(&saved_place);
        all_songs_place_reset(&go_to_place);
        all_songs_place_reset(&browse_place);
    }
    else if (is_songs(csi))
    {
        global_settings.songs_sort = (songs_setting() + 1) % (SONGS_BY_YEAR + 1);
        songs_place_reset(&saved_place);
        songs_place_reset(&go_to_place);
        songs_place_reset(&browse_place);
        const int section = section_of(LANG_SONGS);
        if (section >= 0)
            section_rows[section] = -1;
    }
    else
    {
        global_settings.albums_sort = !global_settings.albums_sort;
        /* below the list, the rows left there are of other albums now */
        max_history_level = MIN(max_history_level, c->dirlevel);
        const int section = section_of(LANG_ALBUMS);
        if (section >= 0)
            section_rows[section] = -1;
        albums_place_reset(&saved_place, false);
        albums_place_reset(&go_to_place, false);
        albums_place_reset(&browse_place, false);
    }
    settings_save();
}

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

/* the rows remembered in the lists below Music's own menu, at their first
   rows instead */
static void place_rows_forget(struct tagtree_place *place)
{
    for (int i = 1; i < MAX_DIR_LEVELS; i++)
        place->selected_item_history[i] = -1;
    if (place->dirlevel > 0)
        place->selected_item = -1;
    place->max_history_level = MIN(place->max_history_level, place->dirlevel);
}

/* Shuffle in Lists as the lists' rows were last made by: Reset Settings, a
   .cfg loaded and Settings from the Shortcuts menu change it without its
   callback, so each load looks */
static bool shuffle_rows_seen[SHUFFLE_ROWS];
static bool shuffle_rows_known;

/* Shuffle in Lists changed since the lists were loaded last: a list's rows
   move by one where its Shuffle Songs comes or goes, so the rows remembered
   to go back to are forgotten, the lists opened at their first rows. Seldom
   done; the list kept is made again, its hash having the setting. */
static void shuffle_rows_check(struct tree_context *c)
{
    const bool changed = shuffle_rows_known
        && memcmp(shuffle_rows_seen, global_settings.shuffle_rows_show,
                  sizeof(shuffle_rows_seen));
    memcpy(shuffle_rows_seen, global_settings.shuffle_rows_show,
           sizeof(shuffle_rows_seen));
    shuffle_rows_known = true;
    if (!changed)
        return;
    for (int i = 1; i < MAX_DIR_LEVELS; i++)
        selected_item_history[i] = -1;
    max_history_level = 0;
    place_rows_forget(&saved_place);
    place_rows_forget(&go_to_place);
    place_rows_forget(&browse_place);
    if (c->dirlevel > 0)
        c->selected_item = -1;
    for (int i = 0; i < MUSIC_SECTIONS; i++)
        section_rows[i] = -1;
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
    errno = 0;
    if (new_playlist)
        w->fd = open_utf8(playlist, O_CREAT|O_WRONLY|O_TRUNC);
    else
        w->fd = open(playlist, O_CREAT|O_WRONLY|O_APPEND, 0666);
    if (w->fd < 0)
        library_write_failed();
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
    if (w->ok && w->used > 0 && write(w->fd, w->buf, w->used) != (ssize_t)w->used)
    {
        w->ok = false;
        library_write_failed();
    }
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
    /* the songs, past the rows at the top, as All Songs' Sort By */
    const int first = c->special_entry_count;
    n = c->filesindir - first;

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

        if (!tagcache_retrieve(&tcs, tagtree_get_entry(c, first + i)->extraseek,
                               tcs.type, buf, sizeof buf))
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
        /* its first row past those at the top, as a Sort By */
        tc->selected_item = tc->special_entry_count;
        if (tc->selected_item >= tc->filesindir)
            return false;
        newtable = tagtree_get_entry(tc, tc->selected_item)->newtable;
        i++;
    }
    return (newtable == TABLE_PLAYTRACK);
}

/* The songs under the row selected, all of them, in the order of the lists
   it opens: an artist's albums by their dates, then each album's songs by
   disc and track; a genre's by artist first. With one search through the
   database, as the lists would make them, rather than each list opened in
   turn, which for a row of 1,400 albums took minutes. Their
   keys are gathered past half the lists' room, then sorted, and the songs
   made the list there. Without <All tracks> in the lists, an artist added
   only its first album. */
struct gather_key {
    int32_t group;  /* the artist, under a genre */
    int32_t date;   /* its album's row's, in date_key()'s order */
    int32_t album;  /* its seek, which goes as their names (sortkey.h) */
    int32_t order;  /* disc and track, then its number as shown */
    int32_t idx;    /* the song */
};

struct gathering {
    struct gather_key *keys;
    int found, room;
    int group_tag;  /* -1 for none */
};

/* each song found, as the search goes through the database's memory held
   still: nothing waits here */
static void gather_match(const int32_t *tag_seek, void *data)
{
    struct gathering *g = data;
    if (g->found >= g->room)
        return;
    struct gather_key *k = &g->keys[g->found++];
    k->group = g->group_tag >= 0 ? tag_seek[g->group_tag] : 0;
    k->album = tag_seek[tag_album];
    k->order = tag_seek[tag_discnumber] * 1000 + tag_seek[tag_tracknumber];
    album_years_match(tag_seek, NULL);
}

static int cmp_gather_key(const void *p1, const void *p2)
{
    const struct gather_key *a = p1, *b = p2;
    if (a->group != b->group)
        return a->group < b->group ? -1 : 1;
    if (a->date != b->date)
        return a->date < b->date ? -1 : 1;
    if (a->album != b->album)
        return a->album < b->album ? -1 : 1;
    if (a->order != b->order)
        return a->order < b->order ? -1 : 1;
    return a->idx < b->idx ? -1 : a->idx > b->idx;
}

/* The songs of the row of seek row_seek at level extra of the search, under
   the rows chosen above it, gathered as described above: of row_artist's
   album too, if it's not -1 (row_artist()). Their keys at *keys, sorted, and
   their count; -1 if the database is busy. With their progress shown, as a
   list's, if left is given: if the user leaves before the end, true in it,
   and only the songs found till then. The lists' buffers are held still by
   the caller, as long as it reads the keys. */
static int gather_songs(struct tree_context *c, int extra, int32_t row_seek,
                        int32_t row_artist, bool *left, struct gather_key **keys)
{
    struct tagcache_search tcs;
    if (!tagcache_search(&tcs, tag_title))
        return -1;
    /* the row's level and those above it, as the list it opens has them */
    static struct tagcache_search_clause numeric[MAX_TAGS];
    for (int i = 0; i <= extra; i++)
    {
        const int32_t seek = i < extra ? csi->result_seek[i] : row_seek;
        const int32_t artist = i < extra ? csi->result_artist[i] : row_artist;
        if (TAGCACHE_IS_NUMERIC(csi->tagorder[i]))
        {
            memset(&numeric[i], 0, sizeof(numeric[i]));
            numeric[i].tag = csi->tagorder[i];
            numeric[i].type = clause_is;
            numeric[i].numeric = true;
            numeric[i].numeric_data = seek;
            tagcache_search_add_clause(&tcs, &numeric[i]);
        }
        else
            tagcache_search_add_filter(&tcs, csi->tagorder[i], seek);
        if (artist >= 0)
            tagcache_search_add_filter(&tcs, tag_albumartist, artist);
    }
    core_pin(tagtree_handle);
    for (int i = 0; i < csi->tagorder_count; i++)
    {
        for (int j = 0; j < csi->clause_count[i]; j++)
            tagcache_search_add_clause(&tcs, csi->clause[i][j]);
    }

    /* past half the lists' room: the songs' rows, written over its start
       one by one, reach a key only after it's read */
    struct tagentry *entries = get_entries(c);
    const int half = c->cache.max_entries / 2;
    /* a genre's: by artist, as its list of them, of the albums' artists if
       it's of those; an album's by disc and track alone */
    const int next = extra + 1 < csi->tagorder_count ? csi->tagorder[extra + 1]
                                                     : tag_album;
    struct gathering g = {
        .keys = (struct gather_key *)&entries[half],
        .room = (c->cache.max_entries - half) * sizeof(*entries)
                / sizeof(struct gather_key),
        .group_tag = next == tag_artist || next == tag_albumartist
                     || next == tag_virt_canonicalartist ? next : -1,
    };
    album_years_rows_begin();
    tagcache_search_set_match(&tcs, gather_match, &g);
    char buf[TAGCACHE_BUFSZ];
    int n = 0;
    while (n < g.room && tagcache_get_next(&tcs, buf, sizeof(buf)))
    {
        g.keys[n++].idx = tcs.idx_id;
        if (left && !tagtree_search_progress(n))
        {
            *left = true;
            break;
        }
    }
    n = MIN(n, g.found);
    /* the album and the artist without a name, which go last as their rows:
       one look at each, as its songs mostly come together */
    int32_t untagged = -1, untagged_group = -1;
    int32_t checked = -1, group_checked = -1;
    for (int i = 0; i < n && (untagged < 0
                              || (g.group_tag >= 0 && untagged_group < 0)); i++)
    {
        const struct gather_key *k = &g.keys[i];
        if (untagged < 0 && k->album != checked)
        {
            checked = k->album;
            if (tagcache_retrieve(&tcs, k->idx, tag_album, buf, sizeof(buf))
                && !strcmp(buf, UNTAGGED))
                untagged = checked;
        }
        if (g.group_tag >= 0 && untagged_group < 0 && k->group != group_checked)
        {
            group_checked = k->group;
            if (tagcache_retrieve(&tcs, k->idx, g.group_tag, buf, sizeof(buf))
                && !strcmp(buf, UNTAGGED))
                untagged_group = group_checked;
        }
    }

    for (int i = 0; i < n; i++)
    {
        struct gather_key *k = &g.keys[i];
        k->date = date_key(album_years_row_date(k->album), k->album == untagged);
        if (untagged_group >= 0 && k->group == untagged_group)
            k->group = INT32_MAX;
    }
    qsort(g.keys, n, sizeof(*g.keys), cmp_gather_key);
    tagcache_search_finish(&tcs);
    core_unpin(tagtree_handle);
    *keys = g.keys;
    return n;
}

/* Those songs the list, until reset_tc_to_prev(): the list the row opens if
   it's of songs, as Songs in Music's menu, as it is; or else, below Music's
   menu, all of them found, the list at the level it was. All Songs, the
   artist's row a level up, as its own list has them, rows at its top too.
   False for none. */
static bool goto_all_songs(void)
{
    const int level = tc->dirlevel;
    if (tc->currtable == TABLE_ROOT)
    {
        /* a section of Music's menu: its list, if of songs */
        tagtree_enter(tc, false);
        if (tc->dirlevel != level + 1 || tagtree_load(tc) < 0
            || tc->dirlevel != level + 1 || tc->filesindir <= 0)
            return false;
        return tagtree_get_entry(tc, tc->special_entry_count)->newtable
               == TABLE_PLAYTRACK;
    }
    if (tc->currtable != TABLE_NAVIBROWSE || !csi)
        return false;
    const struct tagentry *row = tagtree_get_entry(tc, tc->selected_item);
    if (!row || row->customaction != ONPLAY_NO_CUSTOMACTION)
        return false;
    const int extra = tc->currextra;
    /* All Songs: as its list has them, in the order of its Sort By */
    if (row->newtable == TABLE_ALLSUBENTRIES)
    {
        const int count = make_all_songs(tc, extra - 1, false);
        kept_hash = 0;
        if (count <= tc->special_entry_count)
            return false;
        tc->filesindir = tc->dirlength = count;
        return true;
    }
    if (row->newtable != TABLE_NAVIBROWSE)
        return false;

    tree_lock_cache(tc);
    struct gather_key *keys;
    const int n = gather_songs(tc, extra, row->extraseek, row_artist(row), NULL,
                               &keys);
    struct tagentry *entries = get_entries(tc);
    for (int i = 0; i < n; i++)
    {
        const int32_t idx = keys[i].idx;
        entries[i] = (struct tagentry){
            .name = "", .newtable = TABLE_PLAYTRACK, .extraseek = idx,
            .customaction = ONPLAY_NO_CUSTOMACTION, .album_name = NULL
        };
    }
    tree_unlock_cache(tc);
    if (n <= 0)
        return false;
    current_offset = 0;
    current_entry_count = n;
    tc->filesindir = tc->dirlength = n;
    tc->special_entry_count = 0;
    tc->dirfull = false;
    entries_years = false;
    entries_tracks = 0;
    entries_groups = 0;
    entries_untagged = -1;
    entries_initials = INITIALS_NONE;
    album_view.songs = 0;
    kept_hash = 0;
    return true;
}

/* two songs' keys of one album, as gathered */
static bool same_album(const struct gather_key *a, const struct gather_key *b)
{
    return a->group == b->group && a->date == b->date && a->album == b->album;
}

/* Each year's albums with one song here, its singles, after its other albums,
   in their order: through tmp, with room for as many keys as there are. The
   songs without an album aren't singles, as one or many */
static void singles_last(struct gather_key *keys, int n, struct gather_key *tmp)
{
    for (int start = 0, end; start < n; start = end)
    {
        for (end = start + 1; end < n && keys[end].group == keys[start].group
                              && key_year(keys[end].date)
                                 == key_year(keys[start].date); end++)
            ;
        int kept = start, singles = 0;
        for (int a = start, b; a < end; a = b)
        {
            for (b = a + 1; b < end && keys[b].album == keys[a].album; b++)
                ;
            if (b - a == 1 && keys[a].date != KEY_LAST)
                tmp[singles++] = keys[a];
            else
            {
                memmove(&keys[kept], &keys[a], (b - a) * sizeof(*keys));
                kept += b - a;
            }
        }
        memcpy(&keys[kept], tmp, singles * sizeof(*keys));
    }
}

/* The disc of each song of an album, if each has one and they're two or
   more, as its songs go by them: their count, or 0 */
static int album_discs(const struct gather_key *keys, int n)
{
    long disc = 0;
    int discs = 0;
    for (int i = 0; i < n; i++)
    {
        const long d = tagcache_sort_key(keys[i].idx, tag_discnumber);
        if (d <= 0 || d < disc)
            return 0;
        if (d != disc)
            discs++;
        disc = d;
    }
    return discs >= 2 ? discs : 0;
}

/* The bars of All Songs by album, its songs' rows from first on: one over
   each album's songs, "2009 · The Fame", or one over each disc's if they're of
   two discs or more; one over a year's singles, after its albums; and one
   over the songs without an album, the last. Their albums' names in the
   names' room from *used on, which is moved past them. -1 if they don't all
   fit in the room, and then there are none; none without the database's
   memory, as without the songs' numbers. */
static int album_groups(struct tree_context *c, struct tagcache_search *tcs,
                        const struct gather_key *keys, int n, int first,
                        long *used)
{
#ifdef HAVE_TC_RAMCACHE
    if (!tagcache_is_in_ram())
#endif
        return 0;
    char *names = core_get_data(c->cache.name_buffer_handle);
    int groups = 0;
    bool singles = false; /* the year's singles' bar is made */
    for (int start = 0, end; start < n; start = end)
    {
        for (end = start + 1; end < n && same_album(&keys[end], &keys[start]);
             end++)
            ;
        const bool unknown = keys[start].date == KEY_LAST;
        const bool single = !unknown && end - start == 1;
        const int year = key_year(keys[start].date);
        const bool same_year = start > 0
                               && keys[start - 1].group == keys[start].group
                               && key_year(keys[start - 1].date) == year;
        if (single && singles && same_year)
            continue;
        singles = single;
        struct song_group bar = {
            .row = first + start, .album = unknown ? GROUP_UNKNOWN : GROUP_SINGLES,
            .year = MAX(year, 0),
            .disc = 0
        };
        int discs = 0;
        if (!unknown && !single)
        {
            char *name = &names[*used];
            const long room = c->cache.name_buffer_size - *used
                              - (groups + 1) * (long)sizeof(bar);
            if (room <= 1
                || !tagcache_retrieve(tcs, keys[start].idx, tag_album, name, room))
                return -1;
            bar.album = *used;
            *used += strlen(name) + 1;
            discs = album_discs(&keys[start], end - start);
        }
        for (int i = start; i < end; i++)
        {
            /* a disc's first song */
            const long disc = discs ? tagcache_sort_key(keys[i].idx,
                                                        tag_discnumber) : 0;
            if (i > start && disc == bar.disc)
                continue;
            bar.row = first + i;
            bar.disc = disc;
            if (!add_group(c, *used, &groups, &bar))
                return -1;
        }
    }
    return groups;
}

/* All Songs: the songs of the artist's row at level extra of the search, as
   gather_songs() has them, under a row of their own, Sort By. Each by its
   title alone, as in an album, and a song without one by its file's name. By
   album, with their numbers as the songs go album by album, and the bars of
   album_groups(), as the database's memory has their discs; by name, A to Z
   as in Songs, without either. Entered past the artist's albums, two levels
   down, or played from its row there, as it's shown. */
static int make_all_songs(struct tree_context *c, int extra, bool init)
{
    if (extra < 0)
        return -1;
    tree_lock_cache(c);
    struct gather_key *keys;
    bool left = false;
    const int n = gather_songs(c, extra, csi->result_seek[extra],
                               csi->result_artist[extra], init ? &left : NULL,
                               &keys);
    struct tagcache_search tcs;
    if (n < 0 || !tagcache_search(&tcs, tag_title))
    {
        tree_unlock_cache(c);
        return -1;
    }
    const bool by_name = global_settings.all_songs_sort != 0;
    /* past Shuffle Songs, over four songs or more, and Sort By */
    const bool shuffle = n >= SHUFFLE_ROW_MIN_SONGS && shuffle_row_wanted(c);
    const int first = shuffle ? 2 : 1;
    struct tagentry *entries = get_entries(c);
    char *names = core_get_data(c->cache.name_buffer_handle);
    long used = 0;
    int groups = 0;
    if (by_name)
    {
        /* by the places of their titles, as Songs: from the database in
           memory, or else by name, but for too many to sort, which stay in
           the database's order */
        if (song_places_known() || n > TAGTREE_SORT_MAX)
        {
            for (int i = 0; i < n; i++)
            {
                keys[i].group = keys[i].date = keys[i].album = 0;
                keys[i].order = tagcache_sort_key(keys[i].idx, tag_title);
            }
            qsort(keys, n, sizeof(*keys), cmp_gather_key);
        }
        for (int i = 0; i < n; i++)
            keys[i].order = 0;
    }
    else
    {
        /* the singles, through the names' room: twice the keys' size for
           each of the most songs there are */
        singles_last(keys, n, (struct gather_key *)names);
        /* each album's numbers, as its own list has them, in its songs' keys
           in place of their order, which is sorted by: none for an album
           whose songs don't each have a place of their own, nor for the songs
           without an album, last as their row is, which come from any, nor
           for an album of one song here */
        for (int start = 0, end; start < n; start = end)
        {
            long disc = 0, track = 0;
            bool numbered = keys[start].date != KEY_LAST;
            for (end = start; end < n && same_album(&keys[end], &keys[start]);
                 end++)
            {
                numbered = numbered && album_goes_on(keys[end].idx, &disc, &track);
                keys[end].order = track;
            }
            numbered = numbered && end - start > 1;
            for (int i = start; !numbered && i < end; i++)
                keys[i].order = 0;
        }
        groups = album_groups(c, &tcs, keys, n, first, &used);
        /* none rather than some: the names' room for the songs' names */
        if (groups < 0)
        {
            groups = 0;
            used = 0;
        }
    }
    if (shuffle)
        entries[0] = shuffle_row;
    entries[first - 1] = (struct tagentry){
        .name = ID2P(LANG_SORT_BY), .newtable = TABLE_NAVIBROWSE,
        .extraseek = -2, /* not <Random>'s */
        .customaction = ONPLAY_CUSTOMACTION_SORT_BY
    };
    /* in the names' room, up to the bars, 80 bytes for each of the most songs
       there are: a row past it, which titles of twice the usual length would
       take, has no name rather than the list fewer rows */
    const long room = c->cache.name_buffer_size
                      - groups * (long)sizeof(struct song_group);
    char buf[TAGCACHE_BUFSZ];
    int digits = 0;
    for (int i = 0; i < n; i++)
    {
        const int32_t idx = keys[i].idx;
        const long track = keys[i].order;
        const char *name = buf;
        if (!tagcache_retrieve(&tcs, idx, tag_title, buf, sizeof(buf))
            || (!strcmp(buf, UNTAGGED)
                && !song_file_name(&tcs, idx, buf, sizeof(buf))))
            name = untagged_name(tag_title);
        const long len = strlen(name) + 1;
        char *row_name = "";
        if (used + len <= room)
        {
            row_name = memcpy(&names[used], name, len);
            used += len;
        }
        struct tagentry *entry = &entries[first + i];
        *entry = (struct tagentry){
            .name = row_name, .newtable = TABLE_PLAYTRACK, .extraseek = idx,
            .customaction = ONPLAY_NO_CUSTOMACTION
        };
        digits = MAX(digits, keep_lead(entry, track));
    }
    tagcache_search_finish(&tcs);
    if (by_name && !song_places_known() && n <= TAGTREE_SORT_MAX)
    {
        sort_by_place = false;
        qsort_fn = sortkey_compare_n;
        qsort(&entries[first], n, sizeof(*entries), compare);
    }
    tree_unlock_cache(c);

    current_offset = 0;
    current_entry_count = first + n;
    c->special_entry_count = first;
    c->dirfull = false;
    entries_tracks = digits;
    entries_groups = groups;
    entries_untagged = -1;
    /* by name, by their titles' places or else by their names, as they could
       be sorted */
    entries_initials_tag = tag_title;
    entries_initials = !by_name ? INITIALS_NONE
                       : song_places_known() ? INITIALS_BY_PLACE
                       : n <= TAGTREE_SORT_MAX ? INITIALS_BY_NAME : INITIALS_NONE;
    album_view.songs = 0;
    /* searched by the clauses of all its levels, and its titles shown
       without a format (group 0) */
    entries_of_stats = list_of_stats(0, tag_title, csi->tagorder_count - 1);
    entries_whole = !left;
    return first + n;
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

    ret = goto_all_songs();
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

    cpu_boost(true);
    if (!goto_all_songs())
        ret = false;
    else if (tagcache_search(tcs, tag_filename))
    {
        last_tick = current_tick + HZ/2;
        splash_progress_set_delay(HZ / 2); /* wait 1/2 sec before progress */
        /* the songs, past the rows at the top, as All Songs' Sort By */
        const int first = tc->special_entry_count;
        n = tc->filesindir - first;
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

            if (!action_cb(tagcache_retrieve(tcs,
                                             tagtree_get_entry(tc, first + i)->extraseek,
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
        /* nor a podcast or a book copied by hand */
        if ((itunesdb_track_flags(buf) & ITUNESDB_SKIP_SHUFFLE)
            || tagcache_track_kind(tcs.idx_id) != 0)
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

/* the list the selection is in, as it was before the catalogue was gone
   through to pick a playlist, which took the list's buffer */
void tagtree_reload_after_browse(void)
{
    tagtree_load(tc);
}

/* in Music's own list, whose rows are its sections: none adds a song */
bool tagtree_at_top(void)
{
    return tc && tc->dirlevel == 0;
}

/* Which list this is, from how it is searched for, by the rows chosen on the
   way there (seeks, and the album artists of albums apart, artists); with
   by_order, of the order of its songs as well. Going through its songs
   instead would load them all, a chunk at a time: over a second for 13000. */
static uint32_t list_hash(int table, int extra, const struct search_instruction *si,
                          const int *seeks, const int *artists, bool by_order)
{
    uint32_t id = 2166136261u;
#define LIST_ID_ADD(value) id = (id ^ (uint32_t)(value)) * 16777619u
    LIST_ID_ADD(table);
    LIST_ID_ADD(extra);
    /* All Songs in its albums' order: by their years if the table of them
       was made at the start, else by name, as numbers in names go. A saved
       playlist of the other order played other songs than the rows chosen */
    if (table == TABLE_ALLSUBENTRIES && by_order)
    {
        LIST_ID_ADD(album_years_ready());
        LIST_ID_ADD(global_settings.interpret_numbers);
        LIST_ID_ADD(global_settings.all_songs_sort); /* its Sort By */
    }
    /* and Songs in another order than by name, as the lists' size let it be
       sorted: by name, the id it always had */
    if (table == TABLE_NAVIBROWSE && by_order && extra == 0 && is_songs(si)
        && songs_order() != SONGS_BY_NAME)
    {
        LIST_ID_ADD(songs_order());
        LIST_ID_ADD(tree_get_context()->cache.max_entries);
    }
    /* a show's episodes by date: not the id of a playlist written by disc
       and track */
    if (by_order && show_episodes(si, table, extra))
    {
        LIST_ID_ADD(0x45504953u); /* "EPIS" */
        LIST_ID_ADD(tree_get_context()->cache.max_entries);
    }
    LIST_ID_ADD(si->tagorder_count);
    for (int i = 0; i < si->tagorder_count; i++)
    {
        LIST_ID_ADD(si->tagorder[i]);
        LIST_ID_ADD(si->format_id[i]);
        /* what was chosen on the way here */
        if (i < extra)
            LIST_ID_ADD(seeks[i]);
        if (i < extra && artists[i] >= 0)
            LIST_ID_ADD(artists[i]);
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

static uint32_t tagtree_list_id(int table, int extra,
                                const struct search_instruction *si)
{
    return list_hash(table, extra, si, si->result_seek, si->result_artist, true);
}

/* An artist's, a genre's or a composer's songs shuffled, by its Shuffle
   Songs: an id of their own, which no list has, as there's no list of them */
static uint32_t shuffled_list_id(struct tree_context *c)
{
    return (tagtree_list_id(c->currtable, c->currextra, csi) ^ 0x53485546u)
           * 16777619u; /* "SHUF" */
}

static bool tagtree_list_is_all_songs(void)
{
    return is_songs(csi);
}

/* what spares writing the file just written the next time */
static void tagtree_save_list_index(const struct tree_context *c, int slot,
                                    uint32_t id)
{
    if (!written_offsets)
        return;
    struct tagtree_list_header h = {
        TAGTREE_LIST_MAGIC, tagcache_get_commitid(),
        c->filesindir - c->special_entry_count, id, written_count
    };
    int fd = open(tagtree_list_index_names[slot], O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if (fd >= 0)
    {
        const ssize_t size = written_count * sizeof(uint32_t);
        if (write(fd, &h, sizeof(h)) == sizeof(h)
            && write(fd, written_offsets, size) == size)
        {
            slot_lists[slot].id = id;
            slot_lists[slot].commitid = h.commitid;
        }
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
            && h.entries == (uint32_t)(c->filesindir - c->special_entry_count)
            && h.id == id;
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

/* room for a file of the library's over what its next update keeps */
static bool library_file_room(unsigned long bytes)
{
    sector_t free_kb = 0;
    volume_size(IF_MV(0,) NULL, &free_kb);
    return free_kb >= bytes / 1024 + 1 + tagcache_caches_floor_kb();
}

/* Songs' orders aren't in their file as the library is, and have their room:
   without it they wait, and nothing says so, as the playlist */
static bool songs_orders_stale(void)
{
    struct songs_orders_header h;
    if (!songs_sortable() || tagcache_get_song_count() == 0)
        return false;
    const int fd = songs_orders_open(&h);
    if (fd >= 0)
    {
        close(fd);
        return false;
    }
    return library_file_room(sizeof(h) + SONGS_ORDERS * tagcache_entry_count()
                                         * sizeof(struct song_place));
}

/* At Updating Library: Songs made by name, sorted into each order in turn,
   the places written. Where the sort has no room, the header alone, which
   says so for this library and lists' size: else it was tried at each boot.
   Whole or not at all (whole_file_open()); the list isn't kept */
static void songs_orders_write(struct tree_context *c)
{
    struct songs_inputs in;
    songs_orders_keys = NULL;
    songs_for_orders = true;
    const int count = retrieve_entries(c, 0, true);
    songs_for_orders = false;
    kept_hash = 0;
    if (count <= 0 || !entries_whole || !songs_inputs(&in))
        return;
    struct song_key *keys = songs_orders_keys;
    const int n = keys ? current_entry_count - c->special_entry_count : 0;
    struct songs_orders_header h = {
        .magic = SONGS_ORDERS_MAGIC, .version = SONGS_ORDERS_VERSION,
        .commitid = tagcache_get_commitid(), .room = c->cache.max_entries,
        .songs = n, .songs_hash = in.songs, .albums_hash = in.albums,
        .rows_hash = songs_rows_hash(get_entries(c) + c->special_entry_count, n),
    };
    const ssize_t size = n * sizeof(struct song_place);
    const int fd = whole_file_open(SONGS_ORDERS_FILE);
    if (fd < 0)
        return;
    /* the header again at the end, with the places' sums */
    bool ok = write(fd, &h, sizeof(h)) == sizeof(h);
    for (int i = 0; i < SONGS_ORDERS && n && ok; i++)
    {
        const struct song_place *rows = songs_order_sort(c, SONGS_BY_ARTIST + i,
                                                         keys);
        h.sum[i] = words_hash(rows, size, HASH_START);
        ok = write(fd, rows, size) == size;
    }
    ok = ok && lseek(fd, 0, SEEK_SET) == 0
         && write(fd, &h, sizeof(h)) == sizeof(h);
    whole_file_close(fd, SONGS_ORDERS_FILE, ok);
}

/* At Updating Library: Songs' orders into their file, so that the list opens
   without its sort, and the playlist of the list of all songs, which takes
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
    /* as Music's menu enters it: else the list kept here wasn't found */
    for (int i = 0; i < MAX_TAGS; i++)
        songs->result_artist[i] = -1;

    const uint32_t id = tagtree_list_id(c->currtable, c->currextra, csi);
    struct tagtree_list_header h;
    int fd;
    bool ready = tagtree_songs_slot(songs, &h, &fd) >= 0;
    if (ready)
        close(fd);
    const bool orders = songs_orders_stale();

    if (!ready || orders)
    {
        tree_lock_cache(c);
        core_pin(tagtree_handle);
        cpu_boost(true);
        list_on_boot_screen = true;
        library_update_songs_progress(0, 0);
        /* first, for the list made next to be read from them */
        if (orders)
            songs_orders_write(c);
        int count = ready ? 0 : retrieve_entries(c, 0, true);
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

bool tagtree_songs_prepared(void)
{
    struct search_instruction *songs = tagtree_all_songs();
    struct tagtree_list_header h;
    int fd;
    if (!songs || tagtree_songs_slot(songs, &h, &fd) < 0)
        return false;
    close(fd);
    return true;
}

/* Room for Songs' playlist and its index over what the next update of the
   library keeps: its paths are no longer than their tag's file */
static bool songs_playlist_room(void)
{
    char file[MAX_PATH];
    snprintf(file, sizeof(file), "%s/database_%d.tcd",
             tagcache_get_stat()->db_path, tag_filename);
    const int fd = open(file, O_RDONLY);
    if (fd < 0)
        return false;
    const off_t paths = ffilesize(fd);
    close(fd);
    return paths >= 0
           && library_file_room(paths + tagcache_entry_count() * sizeof(uint32_t));
}

/* Without room the playlist waits for a song played, and nothing says so:
   asked at every boot, a write that failed would raise Not Enough Space at
   each. Not without the database in memory: all songs from the disk took
   minutes */
bool tagtree_songs_stale(void)
{
    return song_places_known() && tagcache_get_song_count() != 0
           && tagtree_all_songs()
           && ((!tagtree_songs_prepared() && songs_playlist_room())
               || songs_orders_stale());
}

static int tagtree_play_folder(struct tree_context* c)
{
    logf( "%s", __func__);
    /* of the songs, past the rows at the top, as insert_all_playlist() */
    int start_index = c->selected_item - c->special_entry_count;
    /* Now Playing takes half a second to a second to come, and the slide
       into it waited for half */
    transition_wait(HZ);

    /* The music playing would change anyway; stopped first, it doesn't
       take turns with building the playlist */
    if (audio_status() & AUDIO_STATUS_PLAY)
        audio_stop_at_once();

    int n = c->filesindir - c->special_entry_count;
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

    /* a show's episodes and a book's parts in their order, whatever Shuffle
       is */
    if (global_settings.playlist_shuffle
        && !in_section(LANG_PODCASTS) && !in_section(LANG_AUDIOBOOKS))
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

/* A list of songs' Shuffle Songs: the playlist of the list, as playing a song
   of it loads it, written first if it hasn't been, made anew each time rather
   than gone on with if it plays, which Resume wouldn't give again. */
static bool shuffle_list_songs(struct tree_context *c)
{
    int start = -1; /* no row of it: not the one playing gone on with */
    /* The music playing would change anyway; stopped first, it doesn't take
       turns with building the playlist */
    if (audio_status() & AUDIO_STATUS_PLAY)
        audio_stop_at_once();
    loaded_file[0] = '\0';
    if (c->filesindir - c->special_entry_count
            > playlist_get_current()->max_playlist_size
        || tagtree_load_list(c, &start) < 0)
        return false;
    playlist_shuffle(current_tick, -1);
    playlist_start(0, 0, 0);
    /* the list the music is now from, for its speaker */
    tagtree_note_queue(loaded_file, tagtree_list_id(c->currtable, c->currextra, csi));
    if (kept_hash) /* as it is now, the playlist made from it */
        keep_list(c, kept_count);
    return true;
}

/* An artist's, a genre's or a composer's Shuffle Songs: their songs, found as
   the list's rows were, by the rows chosen on the way and the conditions of all
   levels, into a playlist file of the first pair, in the database's order:
   the list of them isn't made. "Skip when shuffling" isn't heeded, as in a
   playlist: the songs are those of the list (the owner's decision,
   2026-11-08). The music playing goes on as it's written, the other file of
   the pair, so that Menu to abort leaves it playing. */
static bool shuffle_list_found(struct tree_context *c)
{
    struct tagcache_search tcs;
    struct playlist_writer writer;
    char buf[MAX_PATH], file[MAX_PATH];
    const int max = playlist_get_current()->max_playlist_size;
    int added = 0;

    if (!tagcache_search(&tcs, tag_filename))
        return false;
    add_level_filters(&tcs, c->currextra);
    core_pin(tagtree_handle);
    for (int i = 0; i < csi->tagorder_count; i++)
    {
        for (int j = 0; j < csi->clause_count[i]; j++)
            tagcache_search_add_clause(&tcs, csi->clause[i][j]);
    }
    const int slot = tagtree_free_slot(0, file, sizeof(file));
    const char *name = tagtree_playlist_names[slot];
    bool ok = playlist_writer_open(&writer, file, true);
    long last_tick = current_tick + HZ/2; /* progress after half a second */
    while (ok && added < max && tagcache_get_next(&tcs, buf, sizeof(buf)))
    {
        if (!buf[0])
            continue; /* the entry of a removed file */
        ok = playlist_writer_add(&writer, buf);
        added++;
        if ((added & 63) == 0)
            yield();
        if (TIME_AFTER(current_tick, last_tick + HZ/4))
        {
            splash_progress(added, added, "%s (%s)", str(LANG_WAIT),
                            str(LANG_OFF_ABORT)); /* unsaid */
            if (action_userabort(TIMEOUT_NOBLOCK))
            {
                ok = false;
                break;
            }
            last_tick = current_tick;
        }
    }
    tagcache_search_finish(&tcs);
    core_unpin(tagtree_handle);
    ok = playlist_writer_close(&writer) && ok && added > 0;
    /* an id of its own, kept with the file, for Playing From after a boot */
    const uint32_t id = shuffled_list_id(c);
    if (ok)
        tagtree_save_list_index(c, slot, id);
    if (ok && (audio_status() & AUDIO_STATUS_PLAY))
        audio_stop_at_once();
    if (ok && written_offsets)
        ok = playlist_create_indexed(TAGTREE_PLAYLIST_DIR, name, written_offsets,
                                     written_count) == 0;
    else if (ok)
        ok = playlist_create(TAGTREE_PLAYLIST_DIR, name) == 0;
    ok = ok && playlist_amount() > 0;
    if (ok)
    {
        playlist_shuffle(current_tick, -1);
        playlist_start(0, 0, 0);
        tagtree_note_queue(file, id);
    }
    return ok;
}

/* A list's Shuffle Songs: all of its songs in a new order, from a playlist of
   them, as a playlist's Shuffle Songs; the setting Shuffle stays as it is, for
   playing a song (the owner's decision, 2026-11-08). The whole library's as
   Shuffle Songs in the main menu. False if nothing was started, as when the
   playlist playing wasn't to be replaced. */
bool tagtree_shuffle_list(struct tree_context *c)
{
    if (global_settings.party_mode && audio_status())
    {
        splash(HZ, ID2P(LANG_PARTY_MODE));
        return false;
    }
    if (!warn_on_pl_erase())
        return false;
    if (shuffle_row_whole(c))
    {
        const bool ok = tagtree_shuffle_songs();
        if (ok)
            unplaced_starts = playlist_start_count();
        return ok;
    }

    /* as when playing a song of a list (tagtree_play_folder()) */
    transition_wait(HZ);
    cpu_boost(true);
    const bool ok = tagtree_get_attr(c) == FILE_ATTR_AUDIO ? shuffle_list_songs(c)
                                                           : shuffle_list_found(c);
    cpu_boost(false);
    return ok;
}

bool tagtree_row_is_shuffle(struct tree_context *c, int row)
{
    return c->currtable != TABLE_ROOT && row >= 0 && row < c->special_entry_count
           && tagtree_get_entry(c, row)->customaction
              == ONPLAY_CUSTOMACTION_SHUFFLE_LIST;
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

/* A name in a list drawn as settings are, with a Sort By or bars among its
   rows: a tab in it would cut it there */
static void tabs_to_spaces(struct tree_context *c, char *name)
{
    if (!tagtree_has_sort_by(c) && !tagtree_group_count(c))
        return;
    for (char *tab = strchr(name, '\t'); tab; tab = strchr(tab, '\t'))
        *tab = ' ';
}

char* tagtree_get_entry_name(struct tree_context *c, int id,
                                    char* buf, size_t bufsize)
{
    struct tagentry *entry = tagtree_get_entry(c, id);
    if (!entry)
        return NULL;

    /* Sort By, as a setting with its value at the right end: of Albums,
       Year or Name, of All Songs, Album or Name, of Songs, one of four */
    if (entry->customaction == ONPLAY_CUSTOMACTION_SORT_BY)
    {
        static const int songs_values[] = {
            [SONGS_BY_NAME] = LANG_SORT_NAME, [SONGS_BY_ARTIST] = LANG_SORT_ARTIST,
            [SONGS_BY_ALBUM] = LANG_SORT_ALBUM, [SONGS_BY_YEAR] = LANG_SORT_YEAR,
        };
        const bool all_songs = c->currtable == TABLE_ALLSUBENTRIES;
        int value = all_songs ? (global_settings.all_songs_sort ?
                                 LANG_SORT_NAME : LANG_SORT_ALBUM)
                              : (global_settings.albums_sort ?
                                 LANG_SORT_NAME : LANG_SORT_YEAR);
        if (!all_songs && is_songs(csi))
            value = songs_values[songs_setting()];
        snprintf(buf, bufsize, "%s\t%s", str(LANG_SORT_BY), str(value));
        return buf;
    }

    /* a book in one file: its row is Play, Resume or Listen Again */
    if (c->currtable != TABLE_ROOT && entries_parts.parts == 1
        && id == c->special_entry_count)
    {
        strmemccpy(buf, str(entries_parts.current >= 0 ? LANG_BOOKMARK_CONTEXT_RESUME
                            : entries_parts.finished ? LANG_LISTEN_AGAIN
                            : LANG_PLAY), bufsize);
        return buf;
    }

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
    tabs_to_spaces(c, buf);
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
        {
            logf("%s (NAVI/ALLSUB) idx: %d %s", __func__,
                 c->currextra, current_title[c->currextra]);
            /* the level of the row the title names: All Songs is titled by
               the artist's, past its albums' */
            const int named = c->currextra
                              - (c->currtable == TABLE_ALLSUBENTRIES ? 2 : 1);
            if (named >= 0 && !strcmp(current_title[c->currextra], UNTAGGED))
                return (char *)untagged_name(csi->tagorder[named]);
            return current_title[c->currextra];
        }
    }

    return "?";
}

bool tagtree_has_sort_by(struct tree_context *c)
{
    for (int row = 0; row < c->special_entry_count; row++)
    {
        if (tagtree_get_entry(c, row)->customaction
            == ONPLAY_CUSTOMACTION_SORT_BY)
            return true;
    }
    return false;
}

bool tagtree_row_has_children(struct tree_context *c, int row)
{
    /* of the rows at the top, All Songs, a list of its own */
    if (row < c->special_entry_count)
        return tagtree_get_entry(c, row)->newtable == TABLE_ALLSUBENTRIES;
    return tagtree_get_attr(c) == ATTR_DIRECTORY;
}

int tagtree_lead_digits(struct tree_context *c)
{
    /* of the list made last, which Music's own menu isn't; a year's four,
       unless it's under the name */
    if (c->currtable == TABLE_ROOT || entries_two_lines)
        return 0;
    return entries_years ? 4 : entries_tracks;
}

bool tagtree_rows_two_lines(struct tree_context *c)
{
    /* of the list made last, which Music's own menu isn't */
    return c->currtable != TABLE_ROOT && entries_two_lines;
}

bool tagtree_rows_covers(struct tree_context *c)
{
    return tagtree_rows_two_lines(c) && !entries_episodes && list_covers_on();
}

/* An episode's place and state as its row has them: the database's, or
   begun as it plays. The track is taken, under its mutex, for the row
   playing alone: the list has which that is, once a draw */
static int episode_state(int32_t idx, long *place)
{
    unsigned long elapsed = 0;
    if (idx >= 0 && gui_synclist_playing() == idx + 1
        && playing_entry(&elapsed) == idx)
    {
        *place = elapsed;
        return EPISODE_STARTED;
    }
    *place = tagcache_track_place(idx);
    return tagcache_episode_state(idx);
}

/* A podcast's or a book's time in minutes, as every list and Now Playing
   have it: "45 min" under an hour, "6 h 12 min" from one on, "7 h" for whole
   hours. Its length. */
static size_t minutes_text(long minutes, char *buf, size_t size)
{
    minutes = MAX(minutes, 1);
    if (minutes < 60)
        return snprintf(buf, size, str(LANG_TIME_MIN), (int)minutes);
    if (minutes % 60 == 0)
        return snprintf(buf, size, str(LANG_TIME_H), (int)(minutes / 60));
    return snprintf(buf, size, str(LANG_TIME_H_MIN), (int)(minutes / 60),
                    (int)(minutes % 60));
}

const char *tagtree_time_left(unsigned long elapsed, unsigned long length,
                              char *buf, size_t size)
{
    char mins[48];
    if (!length)
        return NULL;
    const unsigned long left = length > elapsed ? length - elapsed : 0;
    minutes_text((left + 59999) / 60000, mins, sizeof(mins));
    snprintf(buf, size, str(LANG_TIME_LEFT), mins);
    return buf;
}

/* "28 min left" of an episode begun, by the database's length: NULL without
   it */
static const char *left_text(int32_t idx, long place, char *buf, size_t size)
{
    const long length = tagcache_sort_key(idx, tag_length);
    return place > 0 && length > 0 ? tagtree_time_left(place, length, buf, size)
                                   : NULL;
}

/* A date as the episodes' rows have it, "20 May", with its year if that
   isn't the clock's, "20 May 2023"; a month and its year, or a year alone:
   its length, 0 for none */
static size_t date_text(long date, char *buf, size_t size)
{
    const int year = date / 10000, month = date / 100 % 100, day = date % 100;
    buf[0] = '\0';
    if (month >= 1 && month <= 12)
    {
        const char *name = str(LANG_MONTH_JANUARY + month - 1);
        const struct tm *tm = get_time();
        if (!day)
            return snprintf(buf, size, "%s %d", name, year);
        if (valid_time(tm) && tm->tm_year + 1900 == year)
            return snprintf(buf, size, str(LANG_DATE_DAY_MONTH), day, name);
        return snprintf(buf, size, str(LANG_DATE_DAY_MONTH_YEAR), day, name, year);
    }
    return year > 0 ? snprintf(buf, size, "%d", year) : 0;
}

/* A show's second line, from the sums kept as the list was made: what is
   left of the episode begun, the newest of them; or how many are new and
   the newest episode's date, "2 new · 9 Nov"; or how many it has */
static const char *show_subline(struct tree_context *c,
                                const struct tagentry *entry, char *buf,
                                size_t size)
{
    const int k = entry->album.year;
    if (k < 0 || k >= entries_shows)
        return NULL;
    const struct show_sums *sums =
        &((const struct show_sums *)song_groups(c))[-1 - k];
    if (sums->started_idx >= 0)
    {
        long place;
        if (episode_state(sums->started_idx, &place) == EPISODE_STARTED
            && left_text(sums->started_idx, place, buf, size))
            return buf;
    }
    if (sums->unplayed > 0)
    {
        char date[64];
        const size_t len = snprintf(buf, size, str(LANG_EPISODES_NEW),
                                    (int)sums->unplayed);
        if (date_text(sums->newest, date, sizeof(date)) && len < size)
            snprintf(buf + len, size - len, " \xc2\xb7 %s", date); /* U+00B7 */
        return buf;
    }
    snprintf(buf, size, plural_str(sums->episodes, LANG_EPISODES_ONE,
                                   LANG_EPISODES_FEW, LANG_EPISODES_MANY),
             (int)sums->episodes);
    return buf;
}

/* A book's second line, from its sums: its author, then "6 h 12 min left"
   of one begun whose parts have their numbers, "Finished" for one heard to
   its end, or else its length */
static const char *book_subline(struct tree_context *c,
                                const struct tagentry *entry, char *buf,
                                size_t size)
{
    const int k = entry->album.year;
    if (k < 0 || k >= entries_shows)
        return NULL;
    const struct show_sums *sums =
        &((const struct show_sums *)song_groups(c))[-1 - k];
    char text[64];
    long place;
    text[0] = '\0';
    if (sums->started_idx >= 0)
    {
        if (episode_state(sums->started_idx, &place) == EPISODE_STARTED
            && (sums->key > 0 || sums->episodes == 1))
            tagtree_time_left(sums->before + place, sums->length, text,
                              sizeof(text));
    }
    else if (!sums->unplayed)
        strlcpy(text, str(LANG_BOOK_FINISHED), sizeof(text));
    else
        minutes_text((sums->length + 30000) / 60000, text, sizeof(text));
#ifdef HAVE_TC_RAMCACHE
    if (!tagcache_tag_string(tag_albumartist, sums->albumartist, buf, size))
#endif
        strlcpy(buf, str(LANG_UNKNOWN_AUTHOR), size);
    const size_t len = strlen(buf);
    if (text[0] && len < size)
        snprintf(buf + len, size - len, " \xc2\xb7 %s", text); /* U+00B7 */
    return buf;
}

/* An episode's: "28 min left" once it's begun; else its date and its length
   to the nearest minute, "20 May 2023 · 22 min", those of them it has.
   From the database's memory */
const char *tagtree_track_date(int32_t idx, bool with_length, char *buf,
                               size_t size)
{
    const long length = with_length ? tagcache_sort_key(idx, tag_length) : 0;
    const size_t len = date_text(episode_date(idx), buf, size);
    if (length > 0 && len < size)
    {
        char mins[48];
        minutes_text((length + 30000) / 60000, mins, sizeof(mins));
        snprintf(buf + len, size - len, "%s%s", len ? " \xc2\xb7 " : "",
                 mins); /* U+00B7 */
    }
    return buf[0] ? buf : NULL;
}

static const char *episode_subline(int32_t idx, char *buf, size_t size)
{
    long place;
    if (episode_state(idx, &place) == EPISODE_STARTED
        && left_text(idx, place, buf, size))
        return buf;
    return tagtree_track_date(idx, true, buf, size);
}

/* The music playing is the saved list of the album of the entry idx under
   Music's section lang, Podcasts or Audiobooks, as it was started there or
   resumed after a boot: by the list's id, which the queue has */
static bool queue_is_list_of(int lang, int32_t idx)
{
    const int item = tagnavi_loaded && rootmenu >= 0 ? root_item_of(lang) : -1;
    if (item < 0 || idx < 0 || !song_places_known())
        return false;
    const struct search_instruction *si = &menus[rootmenu]->items[item]->si;
    if (si->tagorder_count < 2 || si->tagorder[0] != tag_album
        || si->tagorder[1] != tag_title)
        return false;
    int seeks[MAX_TAGS] = { tagcache_sort_key(idx, tag_album) };
    int artists[MAX_TAGS];
    for (int i = 0; i < MAX_TAGS; i++)
        artists[i] = -1;
    const uint32_t queue = tagtree_queue_list();
    return queue && queue == list_hash(TABLE_NAVIBROWSE, 1, si, seeks, artists,
                                       true);
}

bool tagtree_passes_over(const char *playing, const char *next)
{
    if (global_settings.repeat_mode != REPEAT_OFF
        || !(itunesdb_track_flags(next) & ITUNESDB_PODCAST)
        || !(itunesdb_track_flags(playing) & ITUNESDB_PODCAST))
        return false;
    const long a = tagcache_find_entry(playing), b = tagcache_find_entry(next);
    return a >= 0 && b >= 0 && a != b
           && tagcache_sort_key(a, tag_album) == tagcache_sort_key(b, tag_album)
           && tagcache_episode_state(b) == EPISODE_PLAYED
           && queue_is_list_of(LANG_PODCASTS, a);
}

bool tagtree_queue_in_order(void)
{
    const struct mp3entry *id3 = audio_current_track();
    const int32_t idx = id3 ? id3->tagcache_idx - 1 : -1;
    return queue_is_list_of(LANG_PODCASTS, idx)
           || queue_is_list_of(LANG_AUDIOBOOKS, idx);
}

/* the row selected, if it's an episode of the show listed: NULL if not */
static const struct tagentry *selected_episode(void)
{
    struct tree_context *c = tc;
    if (!c || c->currtable == TABLE_ROOT || !entries_episodes || !csi
        || !tagtree_ready() || !tagcache_is_usable()
        || c->selected_item < c->special_entry_count
        || c->selected_item >= c->filesindir)
        return NULL;
    const struct tagentry *entry = tagtree_get_entry(c, c->selected_item);
    return entry && entry->newtable == TABLE_PLAYTRACK ? entry : NULL;
}

int tagtree_row_episode(bool *marked, bool *playing)
{
    const struct tagentry *entry = selected_episode();
    struct tagcache_numbers n;
    long place;
    if (!entry)
        return -1;
    *marked = tagcache_get_numbers(entry->extraseek, &n)
              && tagcache_track_marked(entry->extraseek);
    *playing = playing_entry(NULL) == entry->extraseek;
    return episode_state(entry->extraseek, &place);
}

/* the time of a play, as the end of a track notes it; -1 for none */
static long play_stamp(void)
{
    long lastplayed = tagcache_increase_serial();
#if CONFIG_RTC
    const long now = mktime(get_time());
    if (lastplayed >= 0 && now > lastplayed)
        lastplayed = now;
#endif
    return lastplayed;
}

bool tagtree_row_mark(bool played)
{
    const struct tagentry *entry = selected_episode();
    struct tagcache_numbers n;
    struct tagcache_search tcs;
    char path[MAX_PATH];
    if (!entry || !tagcache_get_numbers(entry->extraseek, &n)
        || !tagcache_search(&tcs, tag_filename))
        return false;
    const int32_t idx = entry->extraseek;
    const bool found = tagcache_retrieve(&tcs, idx, tcs.type, path, sizeof(path));
    tagcache_search_finish(&tcs);
    if (!found)
        return false;
    /* Finder's episode by its mark alone: a play it didn't have would wait
       for a Mac that may never take it. Another row has no mark and is
       played by a play, a Finder song's sent to the Mac as any is */
    const bool marked = tagcache_track_marked(idx);
    if (marked ? !tagcache_set_unplayed(idx, path, !played) : !played)
        return false;
    tagcache_update_numeric(idx, tag_lastelapsed, 0);
    tagcache_update_numeric(idx, tag_lastoffset, 0);
    const long stamp = !marked && n.playcount <= 0 ? play_stamp() : -1;
    if (stamp >= 0)
    {
        tagcache_update_numeric(idx, tag_playcount, 1);
        tagcache_update_numeric(idx, tag_lastplayed, stamp);
    }
    return true;
}

bool tagtree_row_start_over(void)
{
    struct tree_context *c = tc;
    const struct tagentry *entry = selected_episode();
    if (!entry)
        return false;
    const int32_t idx = entry->extraseek;
    if (!warn_on_pl_erase())
        return false;
    /* the one playing stopped first: its stop notes its place */
    if (audio_status() & AUDIO_STATUS_PLAY)
        audio_stop_at_once();
    tagcache_update_numeric(idx, tag_lastelapsed, 0);
    tagcache_update_numeric(idx, tag_lastoffset, 0);
    cpu_boost(true);
    tree_lock_cache(c);
    core_pin(tagtree_handle);
    const bool ok = tagtree_play_folder(c) >= 0;
    core_unpin(tagtree_handle);
    tree_unlock_cache(c);
    cpu_boost(false);
    return ok;
}

/* the list is a book's parts and the row selected one of them */
static bool selected_part(void)
{
    struct tree_context *c = tc;
    return c && c->currtable != TABLE_ROOT && entries_parts.parts > 0 && csi
           && tagtree_ready() && tagcache_is_usable()
           && c->selected_item >= c->special_entry_count
           && c->selected_item < c->filesindir;
}

int tagtree_row_book(bool *playing)
{
    struct tree_context *c = tc;
    if (!selected_part())
        return -1;
    const struct mp3entry *id3 = audio_current_track();
    const int32_t now = (audio_status() & AUDIO_STATUS_PLAY) && id3
                        ? id3->tagcache_idx - 1 : -2;
    *playing = false;
    for (int i = c->special_entry_count; i < c->filesindir && !*playing; i++)
    {
        const struct tagentry *entry = tagtree_get_entry(c, i);
        *playing = entry && entry->extraseek == now;
    }
    return entries_parts.current >= 0 ? 1 : entries_parts.finished ? 2 : 0;
}

/* every part's place forgotten; with finished, each counted as heard: Finder's
   by its mark alone, as its episode is, another by a play if it has none */
static void book_reset(struct tree_context *c, bool finished)
{
    struct tagcache_search tcs;
    char path[MAX_PATH];
    const bool paths = finished && tagcache_search(&tcs, tag_filename);
    for (int i = c->special_entry_count; i < c->filesindir; i++)
    {
        const struct tagentry *entry = tagtree_get_entry(c, i);
        struct tagcache_numbers n;
        if (!entry || !tagcache_get_numbers(entry->extraseek, &n))
            continue;
        const int32_t idx = entry->extraseek;
        tagcache_update_numeric(idx, tag_lastelapsed, 0);
        tagcache_update_numeric(idx, tag_lastoffset, 0);
        if (!finished)
            continue;
        if (tagcache_track_marked(idx))
        {
            if (paths && tagcache_retrieve(&tcs, idx, tcs.type, path, sizeof(path)))
                tagcache_set_unplayed(idx, path, false);
        }
        else if (n.playcount <= 0)
        {
            const long stamp = play_stamp();
            if (stamp >= 0)
            {
                tagcache_update_numeric(idx, tag_playcount, 1);
                tagcache_update_numeric(idx, tag_lastplayed, stamp);
            }
        }
    }
    if (paths)
        tagcache_search_finish(&tcs);
}

bool tagtree_book_start_over(void)
{
    struct tree_context *c = tc;
    if (!selected_part() || !yesno_pop(ID2P(LANG_START_OVER))
        || !warn_on_pl_erase())
        return false;
    /* the one playing stopped first: its stop notes its place */
    if (audio_status() & AUDIO_STATUS_PLAY)
        audio_stop_at_once();
    book_reset(c, false);
    c->selected_item = c->special_entry_count;
    cpu_boost(true);
    tree_lock_cache(c);
    core_pin(tagtree_handle);
    const bool ok = tagtree_play_folder(c) >= 0;
    core_unpin(tagtree_handle);
    tree_unlock_cache(c);
    cpu_boost(false);
    return ok;
}

bool tagtree_book_finish(void)
{
    if (!selected_part() || !yesno_pop(ID2P(LANG_MARK_AS_FINISHED)))
        return false;
    book_reset(tc, true);
    return true;
}

bool tagtree_rows_states(struct tree_context *c)
{
    /* books have no dots */
    return c->currtable != TABLE_ROOT
           && ((entries_shows && !entries_books) || entries_episodes);
}

int tagtree_entry_mark(struct tree_context *c, int row)
{
    if (!tagtree_rows_states(c) || row < c->special_entry_count
        || row >= c->filesindir)
        return LINE_MARK_NONE;
    const struct tagentry *entry = tagtree_get_entry(c, row);
    if (!entry)
        return LINE_MARK_NONE;
    long place;
    int state = EPISODE_PLAYED;
    if (entries_episodes)
    {
        if (entry->newtable == TABLE_PLAYTRACK)
            state = episode_state(entry->extraseek, &place);
    }
    else if (entry->album.year >= 0 && entry->album.year < entries_shows)
    {
        /* a new one before one begun, which its second line has */
        const struct show_sums *sums = &((const struct show_sums *)
                                         song_groups(c))[-1 - entry->album.year];
        if (sums->unplayed > 0)
            state = EPISODE_NEW;
        else if (sums->started_idx >= 0
                 && episode_state(sums->started_idx, &place) == EPISODE_STARTED)
            state = EPISODE_STARTED;
    }
    return state == EPISODE_NEW ? LINE_MARK_NEW
           : state == EPISODE_STARTED ? LINE_MARK_HALF : LINE_MARK_EMPTY;
}

const char *tagtree_album_subline(int no, int year, bool artist,
                                  const char *except, char *buf, size_t size)
{
    size_t len = 0;
    buf[0] = '\0';
    if (year > 0)
        len = snprintf(buf, size, "%d", year);
#ifdef HAVE_TC_RAMCACHE
    static const char dot[] = " \xc2\xb7 "; /* U+00B7 */
    const size_t gap = len ? sizeof(dot) - 1 : 0;
    if (no >= 0 && artist && len + gap < size)
    {
        char *name = buf + len + gap;
        if (tagcache_tag_string(tag_albumartist, album_years_artist(no), name,
                                size - len - gap)
            && (!except || strcasecmp(name, except)))
            memcpy(buf + len, dot, gap);
        else
            buf[len] = '\0';
    }
#else
    (void)no; (void)artist; (void)except;
#endif
    return buf[0] ? buf : NULL;
}

/* under a song's title in Songs by artist or by album */
static const char *song_tag_name(int32_t idx, int tag, char *buf, size_t size)
{
#ifdef HAVE_TC_RAMCACHE
    if (!tagcache_is_in_ram())
        return NULL;
    if (tagcache_tag_string(tag, tagcache_sort_key(idx, tag), buf, size))
        return buf;
    return untagged_name(tag);
#else
    (void)idx; (void)tag; (void)buf; (void)size;
    return NULL;
#endif
}

/* Albums has the album artist as Artists in Albums has it, the albums of a
   name of several apart; an artist's albums only one other than the
   artist's, as a compilation's, which the songs found there are on too */
const char *tagtree_entry_subline(struct tree_context *c, int row, char *buf,
                                  size_t size)
{
    if (!tagtree_rows_two_lines(c) || row < c->special_entry_count
        || row >= c->filesindir)
        return NULL;
    const struct tagentry *entry = tagtree_get_entry(c, row);
    if (!entry)
        return NULL;
    if (entries_episodes)
        return entry->newtable == TABLE_PLAYTRACK ?
               episode_subline(entry->extraseek, buf, size) : NULL;
    if (entries_shows)
        return entries_books ? book_subline(c, entry, buf, size)
                             : show_subline(c, entry, buf, size);
    if (entry->newtable == TABLE_PLAYTRACK)
        return song_tag_name(entry->extraseek, entries_initials_tag, buf, size);
    const bool albums = (is_albums(csi) || is_recent(csi)) && c->currextra == 0;
    return tagtree_album_subline(entry->album.no, entry->album.year,
                                 entries_split || !albums,
                                 albums ? NULL : current_title[c->currextra],
                                 buf, size);
}

int tagtree_entry_album(struct tree_context *c, int row)
{
    if (!tagtree_rows_covers(c) || row < c->special_entry_count
        || row >= c->filesindir)
        return -2;
    const struct tagentry *entry = tagtree_get_entry(c, row);
    return !entry ? -2 : entry->album.no >= 0 ? entry->album.no : -1;
}

/* Albums' rows, as Artists in Albums has them apart or not: when it's
   another than as the last check found it, by its setting or by settings
   applied, as from a .cfg, places in Albums are of other rows */
void tagtree_album_rows_check(void)
{
    static bool known, apart;
    if (known && apart != global_settings.album_artists)
        tagtree_albums_reordered();
    known = true;
    apart = global_settings.album_artists;
}

void tagtree_album_rows_changed(bool on)
{
    (void)on;
    tagtree_album_rows_check();
}

/* An album's year, in a list with years, written to buf for before its
   name, or NULL for a row without one; or a song's number, in a list with
   them, and "" for a song without one, its title in line with the others.
   From the text kept with the row, as each row is drawn */
const char *tagtree_entry_lead(struct tree_context *c, int row, char *buf,
                               size_t size)
{
    if (!tagtree_lead_digits(c) || row < c->special_entry_count
        || row >= c->filesindir)
        return NULL;
    const struct tagentry *entry = tagtree_get_entry(c, row);
    /* an album without a year at the others' edge in Recently Added, whose
       dated and undated albums are mixed */
    if (!entry || !entry->lead[0])
        return !entry || (entries_years && !(c->currextra == 0 && is_recent(csi)))
               ? NULL : "";
    const size_t len = entry->lead[sizeof(entry->lead) - 1] ? sizeof(entry->lead)
                                                             : strlen(entry->lead);
    if (len >= size)
        return NULL;
    memcpy(buf, entry->lead, len);
    buf[len] = '\0';
    return buf;
}

const char *tagtree_entry_initial(struct tree_context *c, int row, char *buf,
                                  size_t size)
{
    if (c->currtable == TABLE_ROOT || row < c->special_entry_count
        || row >= c->filesindir)
        return NULL;
    /* an album's year, in a list by them, kept as its lead or, in two lines,
       with its album; one without, by its name after */
    if (entries_years && entries_two_lines)
    {
        const struct tagentry *entry = tagtree_get_entry(c, row);
        if (entry && entry->album.year > 0)
            return itoa_buf(buf, size, entry->album.year);
    }
    else if (entries_years)
    {
        const char *year = tagtree_entry_lead(c, row, buf, size);
        if (year && *year)
            return year;
    }
    const struct tagentry *entry = entries_initials != INITIALS_NONE ?
                                   tagtree_get_entry(c, row) : NULL;
    if (!entry || size < 5)
        return NULL;
    ucschar_t letter = 0;
    if (entries_initials == INITIALS_BY_NAME)
    {
        if (P2ID((const unsigned char *)entry->name) < 0
            && strcmp(entry->name, UNTAGGED))
            letter = sortkey_initial(sortkey_start(entry->name, true));
    }
    else if (entry->newtable == TABLE_PLAYTRACK)
    {
        /* of its title, or of the name its list goes by: none without one */
        const long seek = tagcache_sort_key(entry->extraseek, entries_initials_tag);
        if (entries_initials_tag == tag_title
            || tag_named(entries_initials_tag, seek))
            letter = tagcache_initial(entries_initials_tag, seek);
    }
    /* not the row of those without the tag, wherever its text would go */
    else if (entry->extraseek != entries_untagged)
        letter = tagcache_initial(entries_initials_tag, entry->extraseek);
    if (!letter)
        return NULL;
    *utf8encode(letter, (unsigned char *)buf) = '\0';
    return buf;
}

int tagtree_group_count(struct tree_context *c)
{
    /* of the list made last, which Music's own menu isn't */
    return c->currtable == TABLE_ROOT ? 0 : entries_groups;
}

int tagtree_group_at(struct tree_context *c, int group)
{
    return song_groups(c)[-1 - group].row;
}

/* A bar's text, from what was kept for it as the list was made: its year,
   its album or what it's of instead, and its disc, those it has, as
   "2017 · Tides & Tungsten · Disc 1" */
const char *tagtree_group_name(struct tree_context *c, int group, char *buf,
                               size_t size)
{
    const struct song_group *bar = &song_groups(c)[-1 - group];
    const char *names = core_get_data(c->cache.name_buffer_handle);
    char year[12] = "", disc[32] = "";
    if (bar->year > 0)
        itoa_buf(year, sizeof(year), bar->year);
    if (bar->disc > 0)
        snprintf(disc, sizeof(disc), str(LANG_DISC_NUMBER), (int)bar->disc);
    const char *album = bar->album >= 0 ? &names[bar->album] : "";
    if (bar->album == GROUP_SINGLES)
        album = str(LANG_SINGLES);
    else if (bar->album == GROUP_UNKNOWN)
        album = untagged_name(tag_album);
    const char *parts[] = { year, album, disc };
    size_t len = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < ARRAYLEN(parts) && len < size; i++)
    {
        if (parts[i][0])
            len += snprintf(&buf[len], size - len, "%s%s",
                            len ? " \xc2\xb7 " : "", parts[i]); /* U+00B7 */
    }
    return buf;
}

bool tagtree_album(struct tree_context *c, struct tagtree_album *album)
{
    /* of the list made last, which Music's own menu isn't */
    if (c->currtable == TABLE_ROOT || album_view.songs <= 0)
        return false;
    *album = album_view;
    return true;
}

bool tagtree_album_is_show(struct tree_context *c)
{
    return c->currtable != TABLE_ROOT && album_view.songs > 0 && entries_episodes;
}

bool tagtree_album_is_book(struct tree_context *c)
{
    return c->currtable != TABLE_ROOT && album_view.songs > 0
           && entries_parts.parts > 0;
}

const char *tagtree_album_counts(struct tree_context *c, char *buf, size_t size)
{
    if (tagtree_album_is_show(c))
    {
        snprintf(buf, size, plural_str(album_view.songs, LANG_EPISODES_ONE,
                                       LANG_EPISODES_FEW, LANG_EPISODES_MANY),
                 album_view.songs);
        return buf;
    }
    if (!tagtree_album_is_book(c))
        return NULL;
    /* "8 parts \xc2\xb7 9 h 5 min", or the length alone of a book in one file */
    size_t len = 0;
    if (entries_parts.parts > 1)
    {
        len = snprintf(buf, size, plural_str(entries_parts.parts, LANG_PARTS_ONE,
                                             LANG_PARTS_FEW, LANG_PARTS_MANY),
                       entries_parts.parts);
        if (len + 4 < size)
            len += snprintf(buf + len, size - len, " \xc2\xb7 "); /* U+00B7 */
    }
    if (len < size)
        minutes_text((entries_parts.length + 30000) / 60000, buf + len,
                     size - len);
    return buf;
}

/* a book's rows: at Resume's right the part it goes on with, "Part 3"; at a
   part's where it starts in the book; at a one-file book's row its length */
static const char *book_value(struct tree_context *c, int row, char *buf,
                              size_t size)
{
    const int first = c->special_entry_count;
    if (entries_parts.parts == 1)
    {
        minutes_text((entries_parts.length + 30000) / 60000, buf, size);
        return row == first ? buf : NULL;
    }
    if (row < first)
    {
        if (entries_parts.current < 0)
            return NULL;
        snprintf(buf, size, str(LANG_PART_NUMBER), entries_parts.current + 1);
        return buf;
    }
    long start = 0;
    for (int i = first; i < row; i++)
    {
        const struct tagentry *entry = tagtree_get_entry(c, i);
        start += entry ? MAX(tagcache_sort_key(entry->extraseek, tag_length), 0)
                       : 0;
    }
    format_time(buf, size, start);
    return buf;
}

const char *tagtree_value_widest(struct tree_context *c, char *buf, size_t size)
{
    char other[32];
    if (!tagtree_album_is_book(c))
        return NULL;
    if (entries_parts.parts == 1)
        return book_value(c, c->special_entry_count, buf, size);
    format_time(buf, size, entries_parts.length);
    snprintf(other, sizeof(other), str(LANG_PART_NUMBER), entries_parts.parts);
    if (strlen(other) > strlen(buf))
        strlcpy(buf, other, size);
    return buf;
}

int tagtree_book_resume_row(struct tree_context *c)
{
    if (c->currtable == TABLE_ROOT || entries_parts.parts <= 0)
        return -1;
    return c->special_entry_count + MAX(entries_parts.current, 0);
}

bool tagtree_album_text(struct tree_context *c, int tag, char *buf, size_t size)
{
    struct tagcache_search tcs;
    if (c->currtable == TABLE_ROOT || album_view.songs <= 0
        || !tagcache_search(&tcs, tag_title))
        return false;
    /* not from the disk, as between a commit and the database's load */
    const bool found = tcs.ramsearch
                       && tagcache_retrieve(&tcs, album_view.first, tag, buf, size);
    tagcache_search_finish(&tcs);
    return found;
}

/* As Now Playing has it, 3:41 or 1:00:01 */
const char *tagtree_entry_length(struct tree_context *c, int row, char *buf,
                                 size_t size)
{
    if (c->currtable != TABLE_ROOT && album_view.songs > 0
        && entries_parts.parts > 0 && row >= 0 && row < c->filesindir)
        return book_value(c, row, buf, size);
    if (c->currtable == TABLE_ROOT || album_view.songs <= 0
        || row < c->special_entry_count || row >= c->filesindir)
        return NULL;
    const struct tagentry *entry = tagtree_get_entry(c, row);
    const long length = entry ? tagcache_sort_key(entry->extraseek, tag_length) : 0;
    if (length <= 0)
        return NULL;
    format_time(buf, size, length);
    return buf;
}

const char *tagtree_get_title_above(struct tree_context *c)
{
    const int level = c->currextra - 1;
    if (c->currtable != TABLE_NAVIBROWSE || level < 0)
        return tagtree_get_title(c);
    /* the row of the songs without its tag, by the name that row shows */
    if (level > 0 && !strcmp(current_title[level], UNTAGGED))
        return untagged_name(csi->tagorder[level - 1]);
    return current_title[level];
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
