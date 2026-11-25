/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 Daniel Stenberg
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
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "string-extra.h"
#include "panic.h"

#include "applimits.h"
#include "dir.h"
#include "file.h"
#include "lcd.h"
#include "font.h"
#include "button.h"
#include "kernel.h"
#include "usb.h"
#include "tree.h"
#include "audio.h"
#include "playlist.h"
#include "menu.h"
#include "transition.h"
#include "skin_engine/skin_engine.h"
#include "settings.h"
#include "debug.h"
#include "storage.h"
#include "rolo.h"
#include "icons.h"
#include "lang.h"
#include "screens.h"
#include "keyboard.h"
#include "bookmark.h"
#include "onplay.h"
#include "core_alloc.h"
#include "power.h"
#include "action.h"
#include "talk.h"
#include "filetypes.h"
#include "plugin.h"
#include "misc.h"
#include "pathfuncs.h"
#include "filetree.h"
#include "tagtree.h"
#ifdef HAVE_RECORDING
#include "recorder/recording.h"
#endif
#include "rtc.h"
#include "dircache.h"
#ifdef HAVE_TAGCACHE
#include "tagcache.h"
#endif
#include "yesno.h"
#include "eeprom_settings.h"
#include "playlist_catalog.h"

/* gui api */
#include "list.h"
#include "splash.h"
#include "quickscreen.h"
#include "shortcuts.h"
#include "appevents.h"

#include "root_menu.h"
#include "on_the_go.h"
#ifdef HAVE_TAGCACHE
#include "ipod_playlists.h"
#endif
#ifdef HAVE_TAGCACHE
#include "music_search.h"
#include "album_header.h"
#include "list_covers.h"
#include "library_playlists.h"
#include "itunesdb.h"
#endif

static struct gui_synclist tree_lists;

/* I put it here because other files doesn't use it yet,
 * but should be elsewhere since it will be used mostly everywhere */
static struct tree_context tc;

char lastfile[MAX_PATH];
static char lastdir[MAX_PATH];
#ifdef HAVE_TAGCACHE
static int lasttable, lastextra;
#endif

static bool reload_dir = false;

static bool start_wps = false;
static int curr_context = false;/* id3db or tree*/

static int dirbrowse(void);
static int ft_play_dirname(char* name);
static int ft_play_filename(char *dir, char *file, int attr);
static void say_filetype(int attr);

struct entry* tree_get_entries(struct tree_context *t)
{
    return core_get_data(t->cache.entries_handle);
}

struct entry* tree_get_entry_at(struct tree_context *t, int index)
{
    if(index < 0 || index >= t->cache.max_entries)
        return NULL; /* no entry */
    struct entry* entries = tree_get_entries(t);
    return &entries[index];
}

static struct entry *get_valid_entry(const char* funcname,
                                     struct tree_context *t, int index)
{
    struct entry *entry = tree_get_entry_at(t, index);
    if (!entry)
        panicf("Invalid tree entry %s", funcname);
    /*DEBUGF("%s tc: %x idx: %d\n", funcname, t, index);*/
    return entry;
}

/* On-The-Go, as on a stock iPod: a row of its own, the last of the catalogue's
   top, when it's viewed; none of the catalogue's entries */
static bool catalog_otg, catalog_otg_empty;
#ifdef HAVE_TAGCACHE
/* Music's lists opened next are those Now Playing was left for by Menu
   (root_menu.c): on the song playing, in the list the music is from */
static bool select_playing;
#endif
static bool catalog_on_otg; /* the cursor was on it, and goes back there */
/* The rows of the catalogue before its entries, a kind each: as it's viewed,
   Unsynced at its top, and New Playlist and New Folder in Unsynced and its
   folders; as a playlist is picked, New Playlist and New Folder; as a folder
   is, Move Here and New Folder. Its Sort By is in the menu of a held Select */
enum {
    LEAD_UNSYNCED, LEAD_NEW_PLAYLIST, LEAD_NEW_FOLDER, LEAD_MOVE_HERE,
};
static unsigned char catalog_leads[3];
static int catalog_lead; /* how many */
/* Unsynced, viewed: the catalogue's top as its playlists and folders
   that aren't synced from the computer, shown where they are on the disk
   (a virtual folder: viewing it moves nothing); below the top, a folder in
   Unsynced, not one from Music */
static bool catalog_mine;
/* the lead row of this kind selected as the list is loaded next, -1 for
   the entry's */
static int catalog_select = -1;
/* Playlists' rows in two lines with their covers, as Playlist Covers has
   them: each playlist's collage and its songs and length, a folder's tile
   and its playlists, from the table in memory (library_playlists.h) */
static bool catalog_covers;
#ifdef HAVE_TAGCACHE
/* In Music, an album's songs under its header (album_header.c): the rows it
   takes at the top, which aren't entries, and none selects; 0 for another
   list. Its title the list's above while it shows, the album's once it
   has scrolled away */
static int album_rows;
static bool album_title_shown;
#else
#define album_rows 0
#endif

/* the catalogue as Playlists shows it, not as one of it is picked */
static bool catalog_viewed(void)
{
    return *tc.dirfilter == SHOW_M3U && tc.browse
           && (tc.browse->flags & BROWSE_ON_THE_GO);
}

/* Unsynced shown, in place of the catalogue's top */
static bool catalog_in_mine(void)
{
    return catalog_mine && tc.dirlevel == 0 && catalog_viewed();
}

static void catalog_count(void)
{
    catalog_lead = 0;
    catalog_otg = false;
    if (*tc.dirfilter != SHOW_M3U || !tc.browse)
        return;
    const unsigned flags = tc.browse->flags;
    bool makes = false;
    if (flags & BROWSE_ON_THE_GO)
    {
        if (tc.dirlevel == 0 && !catalog_mine)
        {
            catalog_leads[catalog_lead++] = LEAD_UNSYNCED;
            catalog_otg = true;
            catalog_otg_empty = otg_empty();
        }
        else
            /* only in Unsynced and its folders, where all that isn't
               the computer's is: none in a folder from Music, at any
               depth */
            makes = catalog_mine;
    }
    else if (flags & BROWSE_PICK_FOLDER)
    {
        catalog_leads[catalog_lead++] = LEAD_MOVE_HERE;
        catalog_leads[catalog_lead++] = LEAD_NEW_FOLDER;
    }
    else if (flags & BROWSE_PICK_PLAYLIST)
        makes = true;
    if (makes)
    {
        catalog_leads[catalog_lead++] = LEAD_NEW_PLAYLIST;
        catalog_leads[catalog_lead++] = LEAD_NEW_FOLDER;
    }
}

/* The bars of groups among a list's rows, each over the entry it starts with:
   in Music an album's discs and All Songs' albums, as the list was made. The
   rows past catalog_lead are then as many more than the entries. */
static int group_count(void)
{
#ifdef HAVE_TAGCACHE
    if (*tc.dirfilter == SHOW_ID3DB)
        return tagtree_group_count(&tc);
#endif
    return 0;
}

static int group_at(int group)
{
#ifdef HAVE_TAGCACHE
    if (*tc.dirfilter == SHOW_ID3DB)
        return tagtree_group_at(&tc, group);
#endif
    (void)group;
    return 0;
}

/* Of the rows past catalog_lead, the groups' before row, by halves: each
   group's is at its entry's and those of the groups before it */
static int groups_before(int row)
{
    int lo = 0, hi = group_count();
    while (lo < hi)
    {
        const int mid = (lo + hi) / 2;
        if (group_at(mid) + mid < row)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* the group whose name a row is, or -1 */
static int group_of(int row)
{
    row -= album_rows + catalog_lead;
    if (row < 0)
        return -1;
    const int group = groups_before(row);
    return group < group_count() && group_at(group) + group == row ? group : -1;
}

static bool catalog_lead_row(int row)
{
    return row < catalog_lead;
}

/* the row of the lead of this kind, -1 if the list has none */
static int catalog_lead_of(int kind)
{
    for (int row = 0; row < catalog_lead; row++)
    {
        if (catalog_leads[row] == kind)
            return row;
    }
    return -1;
}

/* the rows of an album's header, the entries and their groups' names, and
   On-The-Go's after them */
static int catalog_rows(void)
{
    return album_rows + catalog_lead + tc.filesindir + group_count();
}

static bool catalog_otg_row(int row)
{
    return catalog_otg && row == catalog_rows();
}

/* a group's name, or a row of an album's header, which none selects either */
static bool tree_is_group(int row, void *data)
{
    (void)data;
    return row < album_rows || group_of(row) >= 0;
}

/* the row of an entry, after an album's header, the groups' names over it
   and those before; and the entry of a row that isn't a group's name */
static int row_of(int index)
{
    int lo = 0, hi = group_count();
    while (lo < hi)
    {
        const int mid = (lo + hi) / 2;
        if (group_at(mid) <= index)
            lo = mid + 1;
        else
            hi = mid;
    }
    return album_rows + catalog_lead + index + lo;
}

static int index_of(int row)
{
    row = MAX(row - album_rows - catalog_lead, 0);
    return MAX(row - groups_before(row), 0);
}

static bool ext_stripit(bool isdir, int attr, int dirfilter)
{
    if((dirfilter != SHOW_ID3DB) && !isdir)
    {
        switch(global_settings.show_filename_ext)
        {
            case 0:
                /* show file extension: off */
                return true;
                break;
            case 1:
                /* show file extension: on */
                break;
            case 2:
                /* show file extension: only unknown types */
                return filetype_supported(attr);
            case 3:
            default:
                /* show file extension: only when viewing all */
                return (dirfilter != SHOW_ALL);
        }
    }
    return false;
}

static const char* tree_get_filename(int selected_item, void *data,
                                     char *buffer, size_t buffer_len)
{
    struct tree_context * local_tc=(struct tree_context *)data;
    char *name;
    int attr=0;
#ifdef HAVE_TAGCACHE
    bool id3db = *(local_tc->dirfilter) == SHOW_ID3DB;

    if (id3db)
    {
        const int group = group_of(selected_item);
        /* an album's header, which is drawn whole rather than by rows */
        if (selected_item < album_rows)
            return "";
        /* a group's name, as "Disc 2" */
        if (group >= 0)
            return tagtree_group_name(&tc, group, buffer, buffer_len);
        return tagtree_get_entry_name(&tc, index_of(selected_item), buffer,
                                      buffer_len);
    }
    else
#endif
    {
        if (catalog_otg_row(selected_item))
            return str(LANG_ON_THE_GO);
        if (catalog_lead_row(selected_item))
        {
            switch (catalog_leads[selected_item])
            {
                case LEAD_UNSYNCED:
                    return str(LANG_UNSYNCED);
                case LEAD_NEW_FOLDER:
                    return str(LANG_NEW_FOLDER);
                case LEAD_MOVE_HERE:
                    return str(LANG_MOVE_HERE);
                default:
                    return str(LANG_NEW_PLAYLIST);
            }
        }
        struct entry *entry = get_valid_entry(__func__, local_tc,
                                              index_of(selected_item));
        name = entry->name;
        attr = entry->attr;
    }

    if(ext_stripit((attr & ATTR_DIRECTORY), attr, *(local_tc->dirfilter)))
    {
        return(strip_extension(buffer, buffer_len, name));
    }
    return(name);
}

#ifdef HAVE_LCD_COLOR
static int tree_get_filecolor(int selected_item, void * data)
{
    if (*tc.dirfilter == SHOW_ID3DB || tree_is_group(selected_item, NULL)
        || catalog_otg_row(selected_item) || catalog_lead_row(selected_item))
        return -1;
    struct tree_context * local_tc=(struct tree_context *)data;
    struct entry *entry = get_valid_entry(__func__, local_tc, index_of(selected_item));

    return filetype_get_color(entry->name, entry->attr);
}
#endif

#ifdef HAVE_TAGCACHE
/* the song playing, in the Music list it's from; not a group's name over it */
static bool tree_is_playing(int selected_item, void *data)
{
    struct tree_context *local_tc = (struct tree_context *)data;
    return *local_tc->dirfilter == SHOW_ID3DB && !tree_is_group(selected_item, NULL)
        && tagtree_row_is_playing(local_tc, index_of(selected_item),
                                  gui_synclist_playing());
}

/* an album's year before its name, in an artist's albums, and a song's
   number before its title, in an album's songs */
static const char *tree_get_lead(int selected_item, void *data, char *buf,
                                 size_t size)
{
    struct tree_context *local_tc = (struct tree_context *)data;
    return *local_tc->dirfilter == SHOW_ID3DB && !tree_is_group(selected_item, NULL) ?
           tagtree_entry_lead(local_tc, index_of(selected_item), buf, size) : NULL;
}

/* Quick Scroll's letter of a row of Music's, as the list goes by them */
static const char *tree_get_initial(int selected_item, void *data, char *buf,
                                    size_t size)
{
    struct tree_context *local_tc = (struct tree_context *)data;
    return *local_tc->dirfilter == SHOW_ID3DB && !tree_is_group(selected_item, NULL) ?
           tagtree_entry_initial(local_tc, index_of(selected_item), buf, size) : NULL;
}

/* At the right end of a song's row on an album's screen: its length. Where
   Star Ratings, when it's on, is to put the song's stars instead */
static const char *tree_get_value(int selected_item, void *data, char *buf,
                                  size_t size)
{
    struct tree_context *local_tc = (struct tree_context *)data;
    return album_rows && !tree_is_group(selected_item, NULL) ?
           tagtree_entry_length(local_tc, index_of(selected_item), buf, size) : NULL;
}

/* an album's year and album artist under its name, in a list of albums in
   two lines */
static const char *tree_get_subline(int selected_item, void *data, char *buf,
                                    size_t size)
{
    struct tree_context *local_tc = (struct tree_context *)data;
    return tree_is_group(selected_item, NULL) ? NULL
           : tagtree_entry_subline(local_tc, index_of(selected_item), buf, size);
}

/* and its cover at its left, or the note in its place; a person's All Songs
   a collage of the albums under it, as a playlist's row; none for a row that
   isn't an album's */
#define ALL_SONGS_LOOKS 16 /* the albums looked at for its collage's four */
static int tree_get_cover(int selected_item, void *data, const fb_data **pixels)
{
    struct tree_context *local_tc = (struct tree_context *)data;
    if (tree_is_group(selected_item, NULL))
        return LIST_COVER_NONE;
    const int row = index_of(selected_item);
    int albums[ALL_SONGS_LOOKS];
    const int n = tagtree_all_songs_albums(local_tc, row, albums, ALL_SONGS_LOOKS);
    if (n >= 0)
        return list_covers_collage(albums, n, pixels);
    const int album = tagtree_entry_album(local_tc, row);
    return album < -1 ? LIST_COVER_NONE : list_covers_get(album, pixels);
}

/* Before each frame of a list with covers: what it lacks counted anew, its
   rows' covers read now if they can be at once, and the covers of the rows
   next to those shown asked for after theirs, the way the list went first,
   as far again as it went, at least a screen */
static void tree_covers_before_draw(struct gui_synclist *list)
{
    static int last_start;
    list_covers_frame();
    list_covers_draw_soon(list);
    const int start = list->start_item[SCREEN_MAIN];
    const int shown = list_get_nb_lines(list, SCREEN_MAIN);
    const bool down = start >= last_start;
    const int far = MIN(shown + 2 * abs(start - last_start), 32);
    last_start = start;
    for (int k = 0; k < far + shown; k++)
    {
        /* that way first, then the other, a screen */
        const bool forward = k < far ? down : !down;
        const int d = k < far ? k : k - far;
        const int row = forward ? start + shown + d : start - 1 - d;
        if (row < 0 || row >= list->nb_items || tree_is_group(row, NULL))
            continue;
        const int album = tagtree_entry_album(&tc, index_of(row));
        if (album >= 0)
            list_covers_ahead(album);
    }
}

/* An album's title as its header has scrolled away, the list's above until
   then, which the header's name would repeat: as iOS's bar has it */
static void tree_before_draw(struct gui_synclist *list)
{
    const bool shown = list->start_item[SCREEN_MAIN] >= album_rows;
    if (shown == album_title_shown)
        return;
    album_title_shown = shown;
    gui_synclist_set_title(list, shown ? tagtree_get_title(&tc)
                                       : tagtree_get_title_above(&tc),
                           list->title_icon);
}
#endif

#ifdef HAVE_TAGCACHE
/* The mark before a row's name in Playlists: a playlist's, On-The-Go's too,
   whatever made it, but a smart one's of its own; and a folder's, On This
   iPod's too. New Playlist, New Folder and Move Here the theme's own, if
   any */
static int tree_get_mark(int selected_item, void *data)
{
    struct tree_context *local_tc = (struct tree_context *)data;
    if (catalog_lead_row(selected_item))
    {
        switch (catalog_leads[selected_item])
        {
            case LEAD_UNSYNCED:
                /* with covers its tile has the folder's mark */
                return catalog_covers ? LINE_MARK_NONE : LINE_MARK_FOLDER;
            case LEAD_MOVE_HERE:
                /* it makes nothing new: its name in line with the others' */
                return ui_metric(UI_METRIC_MENU_MARKS) ? LINE_MARK_BLANK
                                                       : LINE_MARK_NONE;
            default:
                /* a plus before New Playlist and New Folder */
                return ui_metric(UI_METRIC_MENU_MARKS) ? LINE_MARK_PLUS
                                                       : LINE_MARK_NONE;
        }
    }
    if (catalog_covers)
        return LINE_MARK_NONE;
    if (catalog_otg_row(selected_item))
        return LINE_MARK_PLAYLIST;
    const struct entry *entry = get_valid_entry(__func__, local_tc,
                                                index_of(selected_item));
    return entry->attr & ATTR_DIRECTORY ? LINE_MARK_FOLDER
           : entry->attr & ENTRY_SMART_PLAYLIST ? LINE_MARK_SMART : LINE_MARK_PLAYLIST;
}
#endif

#ifdef HAVE_TAGCACHE
/* What a row of Playlists with covers has, from the table in memory, by its
   path: an entry's, or On-The-Go's. False without it; *folder for a folder
   of the catalogue, which may have none */
static bool catalog_row(int selected_item, struct lp_row_shown *row,
                        bool *folder)
{
    *folder = false;
    if (catalog_otg_row(selected_item))
        return lp_rows_find(itunesdb_path_hash(OTG_FILE), row);
    if (catalog_lead_row(selected_item) || tree_is_group(selected_item, NULL))
        return false;
    const struct entry *entry = get_valid_entry(__func__, &tc,
                                                index_of(selected_item));
    *folder = entry->attr & ATTR_DIRECTORY;
    /* its path's hash without its path, on the stack the list draws on */
    return lp_rows_find(itunesdb_path_hash_in(tc.currdir, entry->name), row);
}

/* its second line: a playlist's songs and length, a folder's playlists,
   Unsynced's; none for New Playlist and the like */
static const char *tree_catalog_subline(int selected_item, void *data,
                                        char *buf, size_t size)
{
    (void)data;
    struct lp_row_shown row;
    if (catalog_lead_row(selected_item))
    {
        if (catalog_leads[selected_item] != LEAD_UNSYNCED)
            return NULL;
        row = (struct lp_row_shown){ .folder = true, .known = true,
                                     .count = MAX(lp_rows_unsynced(), 0) };
        return lp_rows_text(&row, buf, size);
    }
    bool folder;
    if (!catalog_row(selected_item, &row, &folder))
    {
        /* a folder without a row has no playlists, as one just made; an
           empty On-The-Go none either */
        if (!folder && !(catalog_otg_row(selected_item) && catalog_otg_empty))
            return NULL;
        row = (struct lp_row_shown){ .folder = folder, .known = true };
    }
    return lp_rows_text(&row, buf, size);
}

/* and its tile: a playlist's collage, or the note; a folder's mark */
static int tree_catalog_cover(int selected_item, void *data,
                              const fb_data **pixels)
{
    (void)data;
    if (catalog_lead_row(selected_item))
        return catalog_leads[selected_item] == LEAD_UNSYNCED ? LIST_COVER_FOLDER
                                                            : LIST_COVER_NONE;
    struct lp_row_shown row;
    bool folder;
    const bool found = catalog_row(selected_item, &row, &folder);
    if (folder || (found && row.folder))
        return LIST_COVER_FOLDER;
    return found ? list_covers_collage(row.album, row.albums, pixels)
                 : LIST_COVER_NOTE;
}

/* Before each frame, as Music's lists with covers: what it lacks counted
   anew, read now if it can be at once, and the covers of the rows next to
   those shown asked for after theirs */
static void tree_catalog_before_draw(struct gui_synclist *list)
{
    static int last_start;
    list_covers_frame();
    list_covers_draw_soon(list);
    const int start = list->start_item[SCREEN_MAIN];
    const int shown = list_get_nb_lines(list, SCREEN_MAIN);
    const bool down = start >= last_start;
    const int far = MIN(shown + 2 * abs(start - last_start), 16);
    last_start = start;
    for (int k = 0; k < far + shown; k++)
    {
        const bool forward = k < far ? down : !down;
        const int d = k < far ? k : k - far;
        const int row = forward ? start + shown + d : start - 1 - d;
        struct lp_row_shown r;
        bool folder;
        if (row < 0 || row >= list->nb_items || !catalog_row(row, &r, &folder))
            continue;
        for (int i = 0; i < r.albums; i++)
            list_covers_ahead(r.album[i]);
    }
}
#endif

#ifdef HAVE_TAGCACHE
/* the theme's mark of a row of Music's own menu, each name after its column */
static int tree_get_menu_mark(int selected_item, void *data)
{
    struct tree_context *local_tc = (struct tree_context *)data;
    const int kind = tagtree_music_row_kind(local_tc, index_of(selected_item));
    return kind < 0 ? LINE_MARK_BLANK : LINE_MARK_MENU + kind;
}
#endif

#ifdef HAVE_TAGCACHE
/* the mark of a podcast's episode, new or begun, before its name */
static int tree_get_state(int selected_item, void *data)
{
    struct tree_context *local_tc = (struct tree_context *)data;
    return tree_is_group(selected_item, NULL) ? LINE_MARK_NONE
           : tagtree_entry_mark(local_tc, index_of(selected_item));
}
#endif

/* Folders, database levels and playlists lead to more, tracks don't */
static bool tree_has_children(int selected_item, void * data)
{
    struct tree_context * local_tc=(struct tree_context *)data;
#ifdef HAVE_TAGCACHE
    /* nor a group's name; All Songs does lead */
    if (*(local_tc->dirfilter) == SHOW_ID3DB)
        return !tree_is_group(selected_item, NULL)
               && tagtree_row_has_children(local_tc, index_of(selected_item));
#endif
    if (catalog_otg_row(selected_item))
        return !catalog_otg_empty;
    /* Unsynced does, even empty: New Playlist is in it */
    if (catalog_lead_row(selected_item))
        return catalog_leads[selected_item] == LEAD_UNSYNCED;
    if (tree_is_group(selected_item, NULL))
        return false;
    struct entry *entry = get_valid_entry(__func__, local_tc, index_of(selected_item));
    return (entry->attr & ATTR_DIRECTORY)
        || ((entry->attr & FILE_ATTR_MASK) == FILE_ATTR_M3U
            && !(*local_tc->dirfilter == SHOW_M3U
                 && (entry->attr & ENTRY_EMPTY_PLAYLIST)));
}

static enum themable_icons tree_get_fileicon(int selected_item, void * data)
{
    struct tree_context * local_tc=(struct tree_context *)data;
#ifdef HAVE_TAGCACHE
    bool id3db = *(local_tc->dirfilter) == SHOW_ID3DB;
    if (id3db) {
        return tree_is_group(selected_item, NULL) ? Icon_NOICON
                                                  : tagtree_get_icon(&tc);
    }
    else
#endif
    {
        if (catalog_otg_row(selected_item))
            return Icon_Playlist;
        if (tree_is_group(selected_item, NULL) || catalog_lead_row(selected_item))
            return Icon_NOICON;
        struct entry *entry = get_valid_entry(__func__, local_tc,
                                              index_of(selected_item));

        return filetype_get_icon(entry->attr);
    }
}

static int tree_voice_cb(int selected_item, void * data)
{
    struct tree_context * local_tc=(struct tree_context *)data;
    unsigned char *name;
    int attr=0;
    int customaction = ONPLAY_NO_CUSTOMACTION;
#ifdef HAVE_TAGCACHE
    bool id3db = *(local_tc->dirfilter) == SHOW_ID3DB;
    char buf[AVERAGE_FILENAME_LENGTH*2];

    if (id3db)
    {
        /* a group's name, which isn't selected */
        if (tree_is_group(selected_item, NULL))
            return 0;
        attr = tagtree_get_attr(local_tc);
        name = tagtree_get_entry_name(local_tc, index_of(selected_item), buf,
                                      sizeof(buf));
        customaction = tagtree_get_custom_action(local_tc);

        /* See if name is an encoded ID, if it is, then speak it normally */
        int lang_id = P2ID(name);
        /*debugf("%s Found name %s id %d\n", __func__, P2STR(name), lang_id);*/
        if (lang_id >= 0) {
            if (global_settings.talk_menu)
                talk_id(lang_id, true);
            return 0;
        }

        /* Otherwise, it is either a custom "header" or a database entry,
           so try to look up a talk clip for it. */

        // XXX this needs further work, so disable it for now
        // -- need to distinguish between "headers" and entries
        // each entry type ("artist", "album", etc) should be delineated
        // so we can split the clips into subdirs.
#if 0
        if (global_settings.talk_file_clip) {
            if (talk_file(LANG_DIR"/database/", NULL,
                          P2STR(name), file_thumbnail_ext, NULL, true) > 0)
                return 0;

            // XXX fall back to spelling it out?
            return 0;
        }
#endif
    }
    else
#endif
    {
        if (catalog_otg_row(selected_item))
        {
            talk_id(LANG_ON_THE_GO, false);
            return 0;
        }
        if (tree_is_group(selected_item, NULL) || catalog_lead_row(selected_item))
            return 0;
        struct entry *entry = get_valid_entry(__func__, local_tc,
                                              index_of(selected_item));
        name = entry->name;
        attr = entry->attr;
    }
    bool is_dir = (attr & ATTR_DIRECTORY);
    bool did_clip = false;
    /* First the .talk clip case */
    if(is_dir)
    {
        if(global_settings.talk_dir_clip)
        {
            did_clip = true;
            if (ft_play_dirname(name) <= 0)
                /* failed, not existing */
                did_clip = false;
        }
    } else { /* it's a file */
        if (global_settings.talk_file_clip && (attr & FILE_ATTR_THUMBNAIL))
        {
            did_clip = true;
            if (ft_play_filename(local_tc->currdir, name, attr) <= 0)
                /* failed, not existing */
                did_clip = false;
        }
    }
    bool spell_name = (customaction == ONPLAY_CUSTOMACTION_FIRSTLETTER);
    if(!did_clip)
    {
        /* say the number or spell if required or as a fallback */
        switch (is_dir ? global_settings.talk_dir : global_settings.talk_file)
        {
        case 1: /* as numbers */
            talk_id(is_dir ? VOICE_DIR : VOICE_FILE, false);
            talk_number(selected_item+1        - (is_dir ? 0 : local_tc->dirsindir),
                        true);
            break;
        case 2: /* spelled */
            talk_shutup();
            if(global_settings.talk_filetype)
            {
                if(is_dir)
                    talk_id(VOICE_DIR, true);
            }
            spell_name = true;
            break;
        }
    }

    if(global_settings.talk_filetype && !is_dir
       && *local_tc->dirfilter < NUM_FILTER_MODES)
    {
        say_filetype(attr);
    }

    /* spell name AFTER voicing filetype */
    if (spell_name) {
            bool stripit = ext_stripit(is_dir, attr, *(local_tc->dirfilter));
            char *ext = NULL;

            /* Don't spell the extension if it's not displayed */

            if (stripit) {
                ext = strrchr(name, '.');
                if (ext)
                    *ext = 0;
            }

        talk_spell(name, true);

        if (stripit && ext)
            *ext = '.';
    }

    return 0;
}

bool check_rockboxdir(void)
{
    if(!dir_exists(ROCKBOX_DIR))
    {   /* No need to localise this message.
           If .rockbox is missing, it wouldn't work anyway */
        FOR_NB_SCREENS(i)
            screens[i].clear_display();
        splash(HZ*2, "No .rockbox directory");
        FOR_NB_SCREENS(i)
            screens[i].clear_display();
        splash(HZ*2, "Installation incomplete");
        return false;
    }
    return true;
}

/* do this really late in the init sequence */
void tree_init(void)
{
    check_rockboxdir();
    strcpy(tc.currdir, "/");
}

struct tree_context* tree_get_context(void)
{
    return &tc;
}

void tree_lock_cache(struct tree_context *t)
{
    core_pin(t->cache.name_buffer_handle);
    core_pin(t->cache.entries_handle);
}

void tree_unlock_cache(struct tree_context *t)
{
    core_unpin(t->cache.name_buffer_handle);
    core_unpin(t->cache.entries_handle);
}

/*
 * Returns the position of a given file in the current directory
 * returns -1 if not found
 */
static int tree_get_file_position(char * filename)
{
    int i, ret = -1;/* no file match, return undefined */

    tree_lock_cache(&tc);
    struct entry *entries = tree_get_entries(&tc);

    /* use lastfile to determine the selected item (default=0) */
    for (i=0; i < tc.filesindir; i++)
    {
#if ((CONFIG_PLATFORM & PLATFORM_NATIVE) || defined(__APPLE__) || defined(_WIN32) || defined(__CYGWIN__))
        if (!strcasecmp(entries[i].name, filename))
#else
        if (!strcmp(entries[i].name, filename))
#endif
        {
            ret = i;
            break;
        }
    }
    tree_unlock_cache(&tc);
    return(ret);
}

/* As on an iPod classic, the theme's panel is beside the menus, the main
   menu's and Music's, and the lists from there on have the whole width, for
   long names: Artists and deeper, and the playlists. Other browsers, such as
   the fonts' in Settings, keep the width they were opened with. On the iPod
   photo the pane is the main menu's alone: Music's menu has the width too */
static void tree_set_width(void)
{
    bool wide;
#ifdef HAVE_TAGCACHE
    if (*tc.dirfilter == SHOW_ID3DB)
#ifdef UI_MAIN_MENU_PANE
        wide = true;
#else
        wide = tc.dirlevel > 0;
#endif
    else
#endif
    if (*tc.dirfilter == SHOW_M3U)
        wide = true;
    else
        return;
    if (wide != menu_wide())
        menu_set_wide(wide);
}

/* A hint shown over the list, till a key, ticks at most. The keys down as it
   shows, as Select+Play that asked for it, let go first, not ending it; a
   key pressed then ends it, pressed and let go here, doing nothing else, a
   step of the wheel, which nothing holds, the one alone. A system event, as
   USB, ends it and is left for dirbrowse() */
static void hint_wait(long ticks)
{
    const long until = current_tick + ticks;
    bool fresh = button_status() == BUTTON_NONE;
    while (TIME_BEFORE(current_tick, until))
    {
        fresh = fresh || button_status() == BUTTON_NONE;
        const long button = button_get_w_tmo(HZ / 20);
        if (button == BUTTON_NONE)
            continue;
        if (IS_SYSEVENT(button))
        {
            button_queue_post(button, 0);
            break;
        }
        /* nor a key's release or repeat, as of the one that opened the list */
        if (!fresh || (button & (BUTTON_REL | BUTTON_REPEAT)))
            continue;
        while (button_status() != BUTTON_NONE)
            button_get_w_tmo(HZ / 20);
        button_clear_queue();
        break;
    }
}

/* The hint of where Sort By is, "Sort By is in the hold-Select menu", over a
   list with an order to choose as it's entered, not as it's gone back to
   nor loaded again: SORT_BY_HINTS times at most, none once a menu has shown
   Sort By (onplay.c). Wide, in lines to be read (splash_wide()), till a key,
   6 s at most (hint_wait()) */
static void sort_hint(struct gui_synclist *list, bool id3db)
{
    static int level = -1, table, extra;
    static bool mine;
    static char dir[MAX_PATH];
    const bool same = tc.dirlevel == level
        && (id3db ? tc.currtable == table && tc.currextra == extra
                  : catalog_mine == mine && !strcmp(tc.currdir, dir));
    const bool back = tc.dirlevel < level;
    level = tc.dirlevel;
    table = tc.currtable;
    extra = tc.currextra;
    mine = catalog_mine;
    strmemccpy(dir, tc.currdir, sizeof(dir));
    if (same || back || global_settings.sort_by_hints >= SORT_BY_HINTS)
        return;
#ifdef HAVE_TAGCACHE
    if (id3db ? !tagtree_sort_setting(&tc) : !catalog_viewed())
        return;
#else
    if (!catalog_viewed())
        return;
#endif
    global_settings.sort_by_hints++;
    settings_save();
    splash_wide(ID2P(LANG_SORT_BY_HINT));
    hint_wait(6 * HZ);
    gui_synclist_draw(list);
}

/*
 * Called when a new dir is loaded (for example when returning from other apps ...)
 * also completely redraws the tree
 */
static int update_dir(void)
{
    struct gui_synclist * const list = &tree_lists;
    int show_path_in_browser = global_settings.show_path_in_browser;
    bool changed = false;

    const char* title = NULL;/* Must clear the title as the list is reused */
    int icon = NOICON;

#ifdef HAVE_TAGCACHE
    bool id3db = *tc.dirfilter == SHOW_ID3DB;
#else
    const bool id3db = false;
#endif

    tree_set_width();
#ifdef HAVE_TAGCACHE
    /* Multicolor's colours of the section of Music the list is in (tagtree_enter()
       and the go-tos set it), before the list's viewports are */
    if (id3db)
        ui_theme_set_section(tagtree_list_kind(&tc), true);
#endif
    /* Ensure that list is initialized before update_dir returns */
    gui_synclist_init(list, &tree_get_filename, &tc, false, 1, NULL);

#ifdef HAVE_TAGCACHE
    /* the memory of the lists' covers, if the setting was turned on while
       music played, which has stopped since; Playlists' too */
    if (id3db || catalog_viewed())
        list_covers_apply();
    /* Checks for changes */
    if (id3db) {
        if (tc.currtable != lasttable ||
            tc.currextra != lastextra ||
            reload_dir)
        {
            if (tagtree_load(&tc) < 0)
                return -1;

            lasttable = tc.currtable;
            lastextra = tc.currextra;
            changed = true;
        }
    }
    else
#endif
    {
        tc.sort_dir = global_settings.sort_dir;
        /* if the tc.currdir has been changed, reload it ...*/
        if (reload_dir || strncmp(tc.currdir, lastdir, sizeof(lastdir)))
        {
            if (ft_load(&tc, NULL) < 0)
                return -1;
            strmemccpy(lastdir, tc.currdir, MAX_PATH);
            changed = true;
        }
    }
    /* if selected item is undefined */
    bool forgotten = false;
    if (tc.selected_item == -1)
    {
        if (!id3db)
            /* use lastfile to determine the selected item */
            tc.selected_item = tree_get_file_position(lastfile);

        /* If the file doesn't exists, select the first one (default); at
           the catalogue's top, Unsynced */
        if(tc.selected_item < 0)
        {
            tc.selected_item = 0;
            forgotten = true;
        }
        changed = true;
    }
    if (changed)
    {
        if( !id3db && tc.dirfull )
        {
            splash(HZ, ID2P(LANG_SHOWDIR_BUFFER_FULL));
        }
    }

#ifdef HAVE_TAGCACHE
    /* rows of two lines, as high as they are before a header counts those it
       takes: a show's episodes are under one */
    if (id3db && tagtree_rows_two_lines(&tc))
        gui_synclist_set_subline_callback(list, tree_get_subline);
    /* an album's songs, under its header */
    album_rows = id3db ? album_header_open(&tc, list,
                                           tc.filesindir + group_count()) : 0;
    album_title_shown = false;
    if (id3db)
    {
        if (show_path_in_browser == SHOW_PATH_FULL
            || show_path_in_browser == SHOW_PATH_CURRENT)
        {
            title = album_rows ? tagtree_get_title_above(&tc) : tagtree_get_title(&tc);
            icon = filetype_get_icon(ATTR_DIRECTORY);
        }
    }
    else
#endif
    {
        if (tc.browse && tc.browse->title)
        {
            title = tc.browse->title;
            icon = tc.browse->icon;
            if (icon == NOICON)
                icon = filetype_get_icon(ATTR_DIRECTORY);
            /* display sub directories in the title of plugin browser, and the
               folders of playlists */
            if (tc.dirlevel > 0
                && (*tc.dirfilter == SHOW_PLUGINS || *tc.dirfilter == SHOW_M3U))
            {
                char *subdir = strrchr(tc.currdir, '/');
                if (subdir != NULL)
                    title = subdir + 1; /* step past the separator */
            }
            if (catalog_in_mine())
                title = str(LANG_UNSYNCED);
        }
        else
        {
            if (show_path_in_browser == SHOW_PATH_FULL)
            {
                title = tc.currdir;
                icon = filetype_get_icon(ATTR_DIRECTORY);
            }
            else if (show_path_in_browser == SHOW_PATH_CURRENT)
            {
                title = strrchr(tc.currdir, '/');
                if (title != NULL)
                {
                    title++; /* step past the separator */
                    if (*title == '\0')
                    {
                        /* Display "Files" for the root dir */
                        title = ID2P(LANG_DIR_BROWSER);
                    }
                    icon = filetype_get_icon(ATTR_DIRECTORY);
                }
            }
        }
    }

    /* set title and icon, if nothing is set, clear the title
     * with NULL and icon as NOICON as the list is reused */
    gui_synclist_set_title(list, P2STR((unsigned char*)title), icon);

    catalog_count();
    gui_synclist_set_nb_items(list, catalog_rows() + (catalog_otg ? 1 : 0));
    /* the groups' names on bars of their own, as Settings has them; with an
       album's header, rows none selects too, whatever the album's discs */
    if (group_count() || album_rows)
        list->callback_is_group = tree_is_group;
    if (group_count())
        list->callback_draw_item = menu_draw_value_line;
#ifdef HAVE_TAGCACHE
    /* an album's header, its songs' lengths in a column as wide as the
       longest's, if any has one, and its title as the header scrolls away */
    struct tagtree_album album;
    if (album_rows && tagtree_album(&tc, &album))
    {
        gui_synclist_set_block(list, album_rows, album_header_draw);
        if (album.longest > 0)
        {
            char longest[sizeof(list->value_widest)];
            if (!tagtree_value_widest(&tc, longest, sizeof(longest)))
                format_time(longest, sizeof(longest), album.longest);
            gui_synclist_set_value_callback(list, tree_get_value, longest);
        }
        if (title)
            gui_synclist_set_before_draw(list, tree_before_draw);
    }
#endif
    gui_synclist_set_icon_callback(list,
                            global_settings.show_icons?tree_get_fileicon:NULL);
    gui_synclist_set_has_children_callback(list, tree_has_children);
#ifdef HAVE_TAGCACHE
    gui_synclist_set_playing_callback(list, id3db ? tree_is_playing : NULL);
    gui_synclist_set_empty_text(list, id3db ? tagtree_empty_text(&tc) : NULL);
    /* as wide as the list's years or numbers need, if it has them */
    const int lead_digits = id3db ? tagtree_lead_digits(&tc) : 0;
    gui_synclist_set_lead_callback(list, lead_digits ? tree_get_lead : NULL,
                                   lead_digits);
    /* albums in two lines (above), each one's year and album artist under
       its name, and their covers, as Artists in Albums and Covers in Lists
       have them. A list without covers has none read for the one before, as
       an album's header's waits for them */
    if (id3db && tagtree_rows_covers(&tc))
    {
        gui_synclist_set_cover_callback(list, tree_get_cover);
        gui_synclist_set_before_draw(list, tree_covers_before_draw);
        list_covers_opened();
    }
    /* Playlists' with Playlist Covers, as the catalogue is viewed, not as a
       playlist or a folder is picked: nothing written as it opens, which
       could wake a sleeping disk (library_playlists.c writes the table's
       changes at the disk's idle) */
    catalog_covers = !id3db && catalog_viewed() && global_settings.playlist_covers
                     && list_covers_on() && lp_rows_ready();
    if (catalog_covers)
    {
        gui_synclist_set_subline_callback(list, tree_catalog_subline);
        gui_synclist_set_cover_callback(list, tree_catalog_cover);
        gui_synclist_set_before_draw(list, tree_catalog_before_draw);
        list_covers_opened();
    }
    if (!list->callback_get_cover)
        list_covers_cancel();
    /* in Music's lists, not the catalogue's of playlists, new first */
    gui_synclist_set_initial_callback(list, id3db ? tree_get_initial : NULL);
    list->initial_steps = !tc.dirfull;
    /* in the catalogue, not in the file browser showing playlists */
    gui_synclist_set_mark_callback(list, catalog_lead ? tree_get_mark
                                   : id3db && tagtree_rows_states(&tc) ?
                                     tree_get_state
                                   : id3db && ui_metric(UI_METRIC_MENU_MARKS)
                                     && tagtree_is_music_menu(&tc) ?
                                     tree_get_menu_mark : NULL);
    if(!id3db || global_settings.talk_menu)
#endif
    {
        gui_synclist_set_voice_callback(list, &tree_voice_cb);
    }
#ifdef HAVE_LCD_COLOR
    gui_synclist_set_color_callback(list, &tree_get_filecolor);
#endif
#ifdef HAVE_TAGCACHE
    /* back from Now Playing to the list the music plays from: on its song
       playing, not the row chosen before the music went on (the owner's
       rule, 2026-11-24) */
    if (id3db && select_playing)
    {
        const int playing = tagtree_playing_row(&tc, tc.selected_item);
        if (playing >= 0)
            tc.selected_item = playing;
    }
    select_playing = false;
#endif
    if( tc.selected_item >= tc.filesindir)
        tc.selected_item=tc.filesindir-1;

    /* on a row before the entries if one is to be; back on On-The-Go,
       unless it was saved as a playlist, which the cursor goes to; on On
       This iPod if the entry is forgotten; or on the entry, or with none,
       the first row */
    int row = catalog_select >= 0 ? catalog_lead_of(catalog_select) : -1;
    catalog_select = -1;
    if (row < 0 && catalog_otg && catalog_on_otg && !catalog_otg_empty)
        row = catalog_rows();
    if (row < 0 && forgotten)
        row = catalog_lead_of(LEAD_UNSYNCED);
    if (row < 0 && tc.filesindir > 0)
        row = row_of(tc.selected_item);
    if (row < 0)
        row = 0;
    gui_synclist_select_item(list, row);
    gui_synclist_draw(list);
    gui_synclist_speak_item(list);
    sort_hint(list, id3db);
    return tc.filesindir;
}

/* load tracks from specified directory to resume play */
void resume_directory(const char *dir)
{
    int dirfilter = *tc.dirfilter;
    int ret;
#ifdef HAVE_TAGCACHE
    bool id3db = *tc.dirfilter == SHOW_ID3DB;
#else
    const bool id3db = false;
#endif
    /* make sure the dirfilter is sane. The only time it should be possible
     * thats its not is when resume playlist is called from a plugin
     */
    if (!id3db)
        *tc.dirfilter = global_settings.dirfilter;
    ret = ft_load(&tc, dir);
    *tc.dirfilter = dirfilter;
    if (ret < 0)
        return;
    lastdir[0] = 0;

    ft_build_playlist(&tc, 0);

#ifdef HAVE_TAGCACHE
    if (id3db)
        tagtree_load(&tc);
#endif
}

/* Returns the current working directory and also writes cwd to buf if
   non-NULL.  In case of error, returns NULL. */
#if defined(N3DS) || defined(SONY_PSP)
char *__wrap_getcwd(char *buf, getcwd_size_t size)
#else
char *getcwd(char *buf, getcwd_size_t size)
#endif
{
    if (!buf)
        return tc.currdir;
    else if (size)
    {
        if (strmemccpy(buf, tc.currdir, size) != NULL)
            return buf;
    }
    /* size == 0, or truncation in strmemccpy */
    return NULL;
}

/* Force a reload of the directory next time directory browser is called */
void reload_directory(void)
{
    reload_dir = true;
}

char* get_current_file(char* buffer, size_t buffer_len)
{
#ifdef HAVE_TAGCACHE
    /* in ID3DB mode it is a bad idea to call this function */
    /* (only happens with `follow playlist') */
    if( *tc.dirfilter == SHOW_ID3DB )
        return NULL;
#endif

    struct entry *entry = tree_get_entry_at(&tc, tc.selected_item);
    if (entry && getcwd(buffer, buffer_len))
    {
        if (!tc.dirlength)
            return buffer;

        size_t usedlen = strlen(buffer);

        if (usedlen + 2 < buffer_len) /* ensure enough room for '/' + '\0' */
        {
            if (buffer[usedlen-1] != '/')
            {
                buffer[usedlen] = '/';
                /* strmemccpy will zero terminate if we run out of space after */
                usedlen++;
            }
            buffer_len -= usedlen;
            if (strmemccpy(buffer + usedlen, entry->name, buffer_len) != NULL)
                return buffer;
        }
    }
    return NULL;
}

/* Allow apps to change our dirfilter directly (required for sub browsers)
   if they're suddenly going to become a file browser for example */
void set_dirfilter(int l_dirfilter)
{
    *tc.dirfilter = l_dirfilter;
}

/* Selects a path + file and update tree context properly */
static void set_current_file_ex(const char *path, const char *filename)
{
    int i;

#ifdef HAVE_TAGCACHE
    /* in ID3DB mode it is a bad idea to call this function */
    /* (only happens with `follow playlist') */
    if( *tc.dirfilter == SHOW_ID3DB )
        return;
#endif

    if (!filename) /* path and filename supplied combined */
    {
        /* separate directory from filename */
        /* gets the directory's name and put it into tc.currdir */
        filename = strrchr(path+1,'/');
        size_t endpos = filename - path;
        if (filename && endpos < MAX_PATH - 1)
        {
            strmemccpy(tc.currdir, path, endpos + 1);
            filename++;
        }
        else
        {
            strcpy(tc.currdir, "/");
            filename = path+1;
        }
    }
    else /* path and filename came in separate ensure an ending '/' */
    {
        char *end_p = strmemccpy(tc.currdir, path, MAX_PATH);
        size_t endpos = end_p - tc.currdir;
        if (endpos < MAX_PATH)
        {
            if (tc.currdir[endpos - 2] != '/')
            {
                tc.currdir[endpos - 1] = '/';
                tc.currdir[endpos] = '\0';
            }
        }
    }
    strmemccpy(lastfile, filename, MAX_PATH);


    /* If we changed dir we must recalculate the dirlevel
       and adjust the selected history properly */
    if (strncmp(tc.currdir,lastdir,sizeof(lastdir)))
    {
        tc.dirlevel =  0;
        tc.selected_item_history[tc.dirlevel] = -1;

        /* use '/' to calculate dirlevel, as deep as the history goes */
        for (i = 1; path[i] != '\0'; i++)
        {
            if (path[i] == '/' && tc.dirlevel < MAX_DIR_LEVELS - 1)
            {
                tc.dirlevel++;
                tc.selected_item_history[tc.dirlevel] = -1;
            }
        }
    }
    if (ft_load(&tc, NULL) >= 0)
    {
        tc.selected_item = tree_get_file_position(lastfile);
        if (!tc.is_browsing && tc.out_of_tree == 0)
        {
            /* the browser is closed */
            /* don't allow the previous items to overwrite what we just loaded */
            tc.out_of_tree = tc.selected_item + 1;
        }
    }
}

/* Selects a file and update tree context properly */
void set_current_file(const char *path)
{
    set_current_file_ex(path, NULL);
}


static int exit_to_new_screen(int screen)
{
    gui_synclist_scroll_stop(&tree_lists);
    return screen;
}

#ifdef HAVE_TAGCACHE
/* the cursor on this one, of the folder viewed, as it's loaded again */
void tree_select_after_reload(const char *path)
{
    const char *base = strrchr(path, '/');
    strmemccpy(lastfile, base ? base + 1 : path, MAX_PATH);
    tc.selected_item = -1;
    reload_dir = true;
}

/* one of the menus a held Select opens on a row of the list open */
static bool sort_menu_open;

/* A held Select on a folder of the catalogue the iPod may change: Rename
   and Delete. False for USB. */
static bool catalog_folder_menu(const char *path, const char *name)
{
    /* and the catalogue's Sort By, then Cancel last, as the other menus a
       held Select opens. Select on Sort By switches it, the menu open again
       on it */
    static char sort_row[64];
    MENUITEM_STRINGLIST(folder_menu, ID2P(LANG_ONPLAY_MENU_TITLE),
                        onplay_cancel_callback,
                        ID2P(LANG_RENAME), ID2P(LANG_DELETE), sort_row,
                        ID2P(LANG_CANCEL_0));
    char done[MAX_PATH];
    int r = 0, choice, selected = 0;
    sort_menu_open = true;
    do
    {
        snprintf(sort_row, sizeof(sort_row), "%s\t%s", str(LANG_SORT_BY),
                 tree_sort_value());
        choice = do_menu(&folder_menu, &selected, NULL, false);
    }
    while (choice == 2 && onplay_sort_next());
    sort_menu_open = false;
    switch (choice)
    {
        case 0:
            r = ip_rename(path, done, sizeof(done));
            if (r > 0)
                tree_select_after_reload(done);
            break;
        case 1:
            r = ip_delete(path, name);
            break;
        case MENU_ATTACHED_USB:
            r = -1;
            break;
    }
    reload_dir = true;
    return r >= 0;
}
#endif

#ifdef HAVE_TAGCACHE
/* Cover Flow from Music's own row: the history's entry over the lists, which
   leave for it as for Now Playing (root_menu_leave_asked()), Menu there back
   to them */
static int tree_cover_flow(void)
{
    root_menu_ask_cover_flow();
    return GO_TO_WPS;
}
#endif

#ifdef HAVE_TAGCACHE
bool tree_catalog_show_item(char *name, int attr, struct tree_context *c)
{
    (void)attr;
    if (c->dirlevel != 0)
        return true;
    char path[MAX_PATH];
    ft_assemble_path(path, sizeof(path), c->currdir, name);
    return ip_is_synced(path) != catalog_mine;
}

/* Of the catalogue opened at a file, as it gives it below its root: whether
   it's in Unsynced, as it or its folder at the top is there and not
   synced */
static bool catalog_opens_mine(const char *root, const char *rel)
{
    if (!rel || !*rel)
        return false;
    const char *end = strchr(rel, '/');
    char top[MAX_PATH], path[MAX_PATH];
    strmemccpy(top, rel, MIN(end ? (size_t)(end - rel) + 1 : sizeof(top),
                             sizeof(top)));
    if (path_append(path, root, top, sizeof(path)) >= sizeof(path))
        return false;
    return (file_exists(path) || dir_exists(path)) && !ip_is_synced(path);
}

/* Playlists' Sort By chosen: that order, of each level; the cursor back on
   each level above by its row's name, and on the row it was on: a lead by
   its kind, On-The-Go, or an entry by its name */
static void catalog_sort_chosen(int value)
{
    global_settings.playlists_sort = value;
    settings_save();
    for (int level = 0; level < tc.dirlevel && level < MAX_DIR_LEVELS; level++)
        tc.selected_item_history[level] = -1;
    const int row = gui_synclist_get_sel_pos(&tree_lists);
    if (catalog_lead_row(row))
        catalog_select = catalog_leads[row];
    else if (!catalog_otg_row(row) && tc.filesindir > 0)
    {
        strmemccpy(lastfile, get_valid_entry(__func__, &tc, index_of(row))->name,
                   MAX_PATH);
        tc.selected_item = -1;
    }
    reload_dir = true;
}

/* Sort By, in the menu a held Select opens on a row of a list with an order
   to choose (onplay.c's, and the catalogue's own), while that's open
   (sort_menu_open): the setting of Music's list's order
   (tagtree_sort_setting()) or of Playlists', as the catalogue is viewed;
   NULL elsewhere */
int *tree_sort_setting(void)
{
    if (!sort_menu_open)
        return NULL;
#ifdef HAVE_TAGCACHE
    if (*tc.dirfilter == SHOW_ID3DB)
        return tagtree_sort_setting(&tc);
#endif
    return catalog_viewed() ? &global_settings.playlists_sort : NULL;
}

/* its value's name */
const char *tree_sort_value(void)
{
#ifdef HAVE_TAGCACHE
    if (*tc.dirfilter == SHOW_ID3DB)
        return tagtree_sort_value(&tc);
#endif
    return str(global_settings.playlists_sort ? LANG_SORT_Z_A : LANG_SORT_A_Z);
}

/* an order chosen: the list loaded again in it, the row selected kept. Music's
   at once, under a menu still open too, so that what goes by the row
   selected there next, as Go to Artist's place to come back to, has the list
   as it is now; its screen with the next load (reload_dir) */
void tree_sort_chosen(int value)
{
#ifdef HAVE_TAGCACHE
    if (*tc.dirfilter == SHOW_ID3DB)
    {
        tagtree_sort_by_chosen(&tc, value);
        tagtree_load(&tc);
        reload_dir = true;
        return;
    }
#endif
    catalog_sort_chosen(value);
}

#ifdef HAVE_HOTKEY
/* Select+Play in a list with an order to choose, on any of its rows: its
   next order at once, as Sort By in the menu of a held Select, the list in
   it on the same row; then the order's name in a box as the hint's, about a
   second, any key ending it and taken. False for a list without one */
static bool sort_hotkey(void)
{
    sort_menu_open = true;
    const bool sorted = onplay_sort_next();
    sort_menu_open = false;
    if (!sorted)
        return false;
    update_dir();
    reload_dir = false;
    char text[64];
    snprintf(text, sizeof(text), "%s: %s", str(LANG_SORT_BY), tree_sort_value());
    splash_wide(text);
    hint_wait(HZ);
    gui_synclist_draw(&tree_lists);
    return true;
}
#endif

/* The catalogue's row with no menu of its own, Unsynced, On-The-Go or a
   folder from the computer: Sort By and Cancel, by the row's name */
static void catalog_sort_menu(const char *name)
{
    sort_menu_open = true;
    onplay_sort_menu(name);
    sort_menu_open = false;
}

/* Play on the selected row plays it, as on a stock iPod (Apple's guide:
   "select it and press Play/Pause"), where it has songs: in Music's lists
   (tagtree_row_plays()), and a playlist with songs or On-The-Go of the
   catalogue as Playlists shows it, given back to play at once
   (tree_picked_to_play()). Elsewhere Play pauses and goes on, as in any
   list */
static bool tree_row_plays(void)
{
    const int row = gui_synclist_get_sel_pos(&tree_lists);
    if (*tc.dirfilter == SHOW_ID3DB)
        return tc.filesindir > 0 && !tree_is_group(row, NULL)
               && tagtree_row_plays(&tc, index_of(row));
    if (!catalog_viewed() || catalog_lead_row(row))
        return false;
    if (catalog_otg_row(row))
        return !catalog_otg_empty;
    if (tc.filesindir <= 0)
        return false;
    const struct entry *entry = get_valid_entry(__func__, &tc, index_of(row));
    return (entry->attr & FILE_ATTR_MASK) == FILE_ATTR_M3U
           && !(entry->attr & (ATTR_DIRECTORY | ENTRY_EMPTY_PLAYLIST));
}

void tree_select_playing(bool select)
{
    select_playing = select;
}

/* the playlist the catalogue gave back last was picked by Play */
static bool picked_to_play;

bool tree_picked_to_play(void)
{
    return picked_to_play;
}
#endif

/* main loop, handles key events */
static int dirbrowse(void)
{
    int numentries=0;
    char buf[MAX_PATH];
    int button;
    int oldbutton;
    bool reload_root = false;
    int lastfilter = *tc.dirfilter;
    bool lastsortcase = global_settings.sort_case;
    bool exit_func = false;

    char* currdir = tc.currdir; /* just a shortcut */
#ifdef HAVE_TAGCACHE
    bool id3db = *tc.dirfilter == SHOW_ID3DB;

    if (id3db)
        curr_context=CONTEXT_ID3DB;
    else
#endif
        curr_context=CONTEXT_TREE;
    /* in Music, a row forgotten opens the list at its first, as it's loaded;
       the catalogue's looked for once it's read as it's shown (update_dir) */
    if (tc.selected_item < 0 && !id3db && *tc.dirfilter != SHOW_M3U)
        tc.selected_item = 0;
#ifdef HAVE_TAGCACHE
    lasttable = -1;
    lastextra = -1;
#endif

    start_wps = false;
    numentries = update_dir();
    reload_dir = false;
    if (numentries == -1)
        return exit_to_new_screen(GO_TO_PREVIOUS);  /* currdir is not a directory */

    if (*tc.dirfilter > NUM_FILTER_MODES && numentries==0 && !catalog_otg
        && !catalog_lead)
    {
        splash(HZ*2, *tc.dirfilter == SHOW_M3U ?
                     ID2P(LANG_CATALOG_NO_PLAYLISTS) : ID2P(LANG_NO_FILES));
        return exit_to_new_screen(GO_TO_PREVIOUS);  /* No files found for rockbox_browse() */
    }

    while(tc.browse && tc.is_browsing) {
        bool restore = false;
        if (tc.dirlevel < 0)
            tc.dirlevel = 0; /* shouldnt be needed.. this code needs work! */

        keyclick_set_callback(gui_synclist_keyclick_callback, &tree_lists);
#ifdef HAVE_TAGCACHE
        /* an album's cover, read meanwhile as the disk spins up: looked for
           each tenth of a second, and drawn as it comes */
        const bool cover = album_rows && album_header_cover_reading();
        /* and the covers of albums' rows the reader reads as the wheel rests,
           each list drawn again as they come */
        const bool row_covers = tree_lists.callback_get_cover != NULL;
        int timeout = list_do_action_timeout(&tree_lists, cover ? HZ/10 : HZ/2);
        if (row_covers)
            timeout = list_covers_timeout(timeout);
        button = get_action(CONTEXT_TREE|ALLOW_SOFTLOCK, timeout);
        if (row_covers && button != ACTION_NONE)
            list_covers_input();
        if ((album_rows && album_header_cover_came())
            || (row_covers && list_covers_poll()))
            gui_synclist_draw(&tree_lists);
#else
        button = get_action(CONTEXT_TREE|ALLOW_SOFTLOCK,
                            list_do_action_timeout(&tree_lists, HZ/2));
#endif
        oldbutton = button;
#ifdef HAVE_TAGCACHE
        /* Play on a row that plays: as Select there, a row of songs played
           rather than opened. Through the list first, which takes Quick
           Scroll's letter off as for Select */
        const bool play_row = button == ACTION_LIST_PLAYPAUSE && tree_row_plays();
        if (play_row)
            button = ACTION_STD_OK;
#endif
        gui_synclist_do_button(&tree_lists, &button);
        const bool on_otg = catalog_otg_row(gui_synclist_get_sel_pos(&tree_lists));
        const int lead_row = gui_synclist_get_sel_pos(&tree_lists);
        const bool on_lead = catalog_lead_row(lead_row);
        if (catalog_otg)
            catalog_on_otg = on_otg;
        /* an entry's still, for what goes by it */
        tc.selected_item = on_otg ? MAX(tc.filesindir - 1, 0)
                                  : index_of(gui_synclist_get_sel_pos(&tree_lists));
#ifdef HAVE_HOTKEY
        /* Select+Play: the list's next order, if it has one */
        if (button == ACTION_TREE_HOTKEY && sort_hotkey())
            button = ACTION_NONE;
#endif
        int customaction = ONPLAY_NO_CUSTOMACTION;
        bool do_restore_display = true;
        #ifdef HAVE_TAGCACHE
            /* held Select on Music's own rows, its sections, or in a list with
               none, as Composers without composers: nothing to add, so no
               menu, as on a stock iPod, rather than one left empty */
            if (id3db && button == ACTION_STD_CONTEXT
                && (tagtree_at_top() || tc.filesindir <= 0))
                button = ACTION_NONE;
            if (id3db && (button == ACTION_STD_OK || button == ACTION_STD_CONTEXT))
            {
                customaction = tagtree_get_custom_action(&tc);
                if (customaction == ONPLAY_CUSTOMACTION_SHUFFLE_SONGS)
                {
                    /* The code to insert shuffled is on the context branch of the switch so we always go here */
                    button = ACTION_STD_CONTEXT;
                    do_restore_display = false;
                }
                else if (customaction == ONPLAY_CUSTOMACTION_SEARCH)
                {
                    /* a screen of its own too */
                    if (button == ACTION_STD_OK)
                    {
                        music_search_new();
                        transition_begin(TRANSITION_FORWARD);
                        return exit_to_new_screen(GO_TO_SEARCH);
                    }
                    button = ACTION_NONE;
                }
                else if (customaction == ONPLAY_CUSTOMACTION_PLAYLISTS)
                {
                    /* the playlist catalogue is a screen of its own */
                    if (button == ACTION_STD_OK)
                        return exit_to_new_screen(GO_TO_PLAYLISTS_SCREEN);
                    button = ACTION_NONE;
                }
                else if (customaction == ONPLAY_CUSTOMACTION_BOOK_RESUME)
                {
                    /* Resume or Listen Again: as Select on the part it goes
                       on with, which plays from its place; no menu */
                    const int row = tagtree_book_resume_row(&tc);
                    if (button == ACTION_STD_OK && row >= 0)
                        tc.selected_item = row;
                    else
                        button = ACTION_NONE;
                }
                else if (customaction == ONPLAY_CUSTOMACTION_COVERFLOW)
                {
                    /* PictureFlow, back to this list when it is left */
                    if (button == ACTION_STD_OK)
                        return exit_to_new_screen(tree_cover_flow());
                    button = ACTION_NONE;
                }
            }
        #endif
#ifdef HAVE_TAGCACHE
        if (on_lead)
        {
            const int lead = catalog_leads[lead_row];
            if (button == ACTION_STD_OK)
            {
                char made[MAX_PATH];
                int r;
                switch (lead)
                {
                    case LEAD_UNSYNCED:
                        catalog_mine = true;
                        tc.selected_item = 0;
                        reload_dir = true;
                        transition_begin(TRANSITION_FORWARD);
                        break;
                    case LEAD_MOVE_HERE:
                        tc.browse->flags |= BROWSE_SELECTED;
                        strmemccpy(tc.browse->buf, currdir, tc.browse->bufsize);
                        return exit_to_new_screen(GO_TO_PREVIOUS);
                    default:
                        r = lead == LEAD_NEW_FOLDER ?
                            ip_new_folder(currdir, made, sizeof(made))
                            : ip_new_playlist(currdir, made, sizeof(made));
                        if (r < 0)
                            return exit_to_new_screen(GO_TO_ROOT);
                        /* a new one picked at once, to add to */
                        if (r > 0 && lead == LEAD_NEW_PLAYLIST
                            && (tc.browse->flags & BROWSE_PICK_PLAYLIST))
                        {
                            tc.browse->flags |= BROWSE_SELECTED;
                            strmemccpy(tc.browse->buf, made, tc.browse->bufsize);
                            return exit_to_new_screen(GO_TO_PREVIOUS);
                        }
                        if (r > 0)
                            tree_select_after_reload(made);
                        else
                        {
                            /* the screen of the name was over it */
                            reload_dir = true;
                            catalog_select = lead;
                        }
                        break;
                }
            }
            /* a held Select on one of the catalogue viewed: its Sort By */
            if (button == ACTION_STD_CONTEXT && catalog_viewed())
                catalog_sort_menu(tree_get_filename(lead_row, &tc, buf,
                                                    sizeof(buf)));
            /* Menu, Play, USB and the rest as on any row */
            if (button == ACTION_STD_OK || button == ACTION_STD_CONTEXT
#ifdef HAVE_HOTKEY
                || button == ACTION_TREE_HOTKEY
#endif
               )
                button = ACTION_NONE;
        }
#endif
        if (on_otg)
        {
            /* On-The-Go: its file, as a playlist's, or nothing while empty */
            if (button == ACTION_STD_OK && !catalog_otg_empty
                && (tc.browse->flags & BROWSE_SELECTONLY))
            {
                tc.browse->flags |= BROWSE_SELECTED;
                strmemccpy(tc.browse->buf, OTG_FILE, tc.browse->bufsize);
#ifdef HAVE_TAGCACHE
                picked_to_play = play_row;
#endif
                return exit_to_new_screen(GO_TO_PREVIOUS);
            }
#ifdef HAVE_TAGCACHE
            if (button == ACTION_STD_CONTEXT && catalog_viewed())
                catalog_sort_menu(str(LANG_ON_THE_GO));
#endif
            if (button == ACTION_STD_OK || button == ACTION_STD_CONTEXT
#ifdef HAVE_HOTKEY
                || button == ACTION_TREE_HOTKEY
#endif
               )
                button = ACTION_NONE;
        }
        switch ( button ) {
            case ACTION_STD_OK:
                /* nothing to do if no files to display */
                if ( numentries == 0 )
                    break;
                /* a playlist without songs, as On-The-Go empty: nothing to
                   view, though one may be picked to add songs to */
                if ((tc.browse->flags & BROWSE_ON_THE_GO)
                    && (get_valid_entry(__func__, &tc, tc.selected_item)->attr
                        & ENTRY_EMPTY_PLAYLIST))
                    break;
                if (tc.browse->flags & BROWSE_SELECTONLY)
                {
                    struct entry *entry =
                                get_valid_entry(__func__, &tc, tc.selected_item);
                    short attr = entry->attr;
                    if(!(attr & ATTR_DIRECTORY))
                    {
                        tc.browse->flags |= BROWSE_SELECTED;
                        get_current_file(tc.browse->buf, tc.browse->bufsize);
#ifdef HAVE_TAGCACHE
                        picked_to_play = play_row;
#endif
                        return exit_to_new_screen(GO_TO_PREVIOUS);
                    }
                }
                /* into a list, or to Now Playing, or a plugin, which has none */
                transition_begin(TRANSITION_FORWARD);
#ifdef HAVE_TAGCACHE
                /* the song playing, of this list: to it, paused or not, as on
                   a stock iPod, rather than from its start */
                if (id3db && tagtree_row_is_playing(&tc, tc.selected_item,
                                                   gui_synclist_song_playing()))
                    return exit_to_new_screen(GO_TO_WPS);
#endif
#ifdef HAVE_TAGCACHE
                int entered = id3db ? play_row ? tagtree_play_row(&tc)
                                               : tagtree_enter(&tc, true)
                                    : ft_enter(&tc);
                /* nothing played: no slide, the list as it was */
                if (play_row && entered != GO_TO_WPS)
                    transition_cancel();
                switch (entered)
#else
                switch (ft_enter(&tc))
#endif
                {
                    case GO_TO_FILEBROWSER: reload_dir = true; break;
                    case GO_TO_PLUGIN:
                        return exit_to_new_screen(GO_TO_PLUGIN);
                    case GO_TO_WPS:
                        return exit_to_new_screen(GO_TO_WPS);
#if CONFIG_TUNER
                    case GO_TO_FM:
                        return exit_to_new_screen(GO_TO_FM);
#endif
                    case GO_TO_ROOT: exit_func = true; break;
                    default:
                        break;
                }
                restore = do_restore_display;
                break;

            case ACTION_STD_CANCEL:
                exit_to_new_screen(0);
#ifdef HAVE_TAGCACHE
                /* the go-to's level, of lists a Go to opened: back to the
                   screen under them in the history */
                if (*tc.dirfilter == SHOW_ID3DB
                    && tc.dirlevel == tagtree_go_to_level())
                {
                    transition_begin(TRANSITION_BACK);
                    return exit_to_new_screen(GO_TO_PREVIOUS);
                }
#endif
                /* out of Unsynced: on its row at the top */
                if (catalog_in_mine())
                {
                    catalog_mine = false;
                    catalog_on_otg = false;
                    catalog_select = LEAD_UNSYNCED;
                    reload_dir = true;
                    transition_begin(TRANSITION_BACK);
                    break;
                }
                if (*tc.dirfilter > NUM_FILTER_MODES && tc.dirlevel < 1) {
                    transition_begin(TRANSITION_BACK);
                    exit_func = true;
                    break;
                }
                if ((*tc.dirfilter == SHOW_ID3DB && tc.dirlevel == 0) ||
                    ((*tc.dirfilter != SHOW_ID3DB && !strcmp(currdir,"/"))))
                {
                    if (oldbutton == ACTION_TREE_PGLEFT)
                        break;
                    transition_begin(TRANSITION_BACK);
                    return exit_to_new_screen(GO_TO_ROOT);
                }

                transition_begin(TRANSITION_BACK);
#ifdef HAVE_TAGCACHE
                if (id3db)
                    tagtree_exit(&tc, true);
                else
#endif
                    if (ft_exit(&tc) == 3)
                        exit_func = true;

                restore = do_restore_display;
                break;

            case ACTION_TREE_STOP:
                /* holding Play goes to Now Playing, if it has a song: stopped,
                   it shows it paused and starts nothing (wpsscrn()) */
                if (now_playing_available())
                    return exit_to_new_screen(GO_TO_PREVIOUS_MUSIC);
                break;

            case ACTION_STD_MENU:
                return exit_to_new_screen(GO_TO_ROOT);
                break;

#ifdef HAVE_RECORDING
            case ACTION_STD_REC:
                return exit_to_new_screen(GO_TO_RECSCREEN);
#endif

            case ACTION_TREE_WPS:
                return exit_to_new_screen(GO_TO_PREVIOUS_MUSIC);
                break;
#ifdef HAVE_QUICKSCREEN
            case ACTION_STD_QUICKSCREEN:
            {
                /* Shortcuts Menu currently disabled in browse filter modes */
                bool enter_shortcuts_menu = global_settings.shortcuts_replaces_qs;
                if (enter_shortcuts_menu && *tc.dirfilter >= NUM_FILTER_MODES)
                    break;
                else if (!enter_shortcuts_menu)
                {
                    int ret = quickscreen_show(button);
                    if (ret == QUICKSCREEN_IN_USB)
                        reload_dir = true;
                    else if (ret == QUICKSCREEN_GOTO_SHORTCUTS_MENU)
                        enter_shortcuts_menu = true;
                }

                if (enter_shortcuts_menu && *tc.dirfilter < NUM_FILTER_MODES)
                {
                    int last_screen = global_status.last_screen;
                    global_status.last_screen = GO_TO_SHORTCUTMENU;
                    int shortcut_ret = do_shortcut_menu(NULL);
                    if (shortcut_ret == GO_TO_PREVIOUS)
                        global_status.last_screen = last_screen;
                    else
                        return exit_to_new_screen(shortcut_ret);
                }

                restore = do_restore_display;
                break;
            }
#endif

#ifdef HAVE_HOTKEY
            case ACTION_TREE_HOTKEY:
                if (!global_settings.hotkey_tree)
                    break;
                /* fall through */
#endif
            case ACTION_STD_CONTEXT:
            {
                bool hotkey = button == ACTION_TREE_HOTKEY;
                int onplay_result;
                int attr = 0;

                if (tc.browse->flags & BROWSE_NO_CONTEXT_MENU)
                    break;
                /* a folder of playlists has no songs of its own for Playing Next
                   and Add to Playlist: its menu, of one made on the iPod, has
                   Rename and Delete; of one from the computer, Sort By alone,
                   as the catalogue is viewed */
                if (numentries && *tc.dirfilter == SHOW_M3U
                    && (get_valid_entry(__func__, &tc, tc.selected_item)->attr
                        & ATTR_DIRECTORY))
                {
#ifdef HAVE_TAGCACHE
                    struct entry *entry = get_valid_entry(__func__, &tc,
                                                          tc.selected_item);
                    char name[MAX_PATH];
                    strmemccpy(name, entry->name, sizeof(name));
                    ft_assemble_path(buf, sizeof(buf), currdir, name);
                    if ((tc.browse->flags & BROWSE_ON_THE_GO)
                        && ip_folder_editable(buf))
                    {
                        if (!catalog_folder_menu(buf, name))
                            return exit_to_new_screen(GO_TO_ROOT);
                    }
                    else if (catalog_viewed())
                        catalog_sort_menu(name);
#endif
                    break;
                }

                /* the menu's Sort By of the list's order, while it's open */
                sort_menu_open = !hotkey;
                if(!numentries)
                    onplay_result = onplay(NULL, 0, curr_context, hotkey, customaction);
                else {
#ifdef HAVE_TAGCACHE
                    if (id3db)
                    {
                        char title[MAX_PATH];
                        /* the menu of a row at the top, All Songs, by the
                           list's title, its artist, as an album's by its name */
                        if (tc.selected_item < tc.special_entry_count)
                            onplay_set_title(tagtree_get_title(&tc));
                        else if (tagtree_get_entry_name(&tc, tc.selected_item,
                                                        title, sizeof(title)))
                            onplay_set_title(title);
                        if (tagtree_get_attr(&tc) == FILE_ATTR_AUDIO)
                        {
                            attr = FILE_ATTR_AUDIO;

                            /* Look up the filename only once it is needed, so we
                               don't have to wait for the disk to wake up here. */
                            buf[0] = '\0';
                        }
                        else
                        {
                            attr = ATTR_DIRECTORY;
                            int title_len = 0;

                            /* In case of "special entries", add table title as
                               prefix, e.g. "The Beatles [All Tracks]", instead
                               of just "[All Tracks]", to improve the suggested
                               playlist filename.
                            */
                            if (tc.selected_item < tc.special_entry_count)
                            {
                                title_len = snprintf(buf, sizeof(buf), "%s ",
                                                     tagtree_get_title(&tc));
                                if (title_len < 0)
                                    title_len = 0;
                            }

                            if (title_len < (int) sizeof(buf))
                                tagtree_get_entry_name(&tc, tc.selected_item,
                                                       buf + title_len,
                                                       sizeof(buf) - title_len);

                            fix_path_part(buf, 0, sizeof(buf) - 1);
                        }
                    }
                    else
#endif
                    {
                        struct entry *entry =
                               get_valid_entry(__func__, &tc, tc.selected_item);

                        attr = entry->attr;

                        ft_assemble_path(buf, sizeof(buf), currdir, entry->name);
                        /* a playlist by its name, as the list shows it */
                        char title[MAX_PATH];
                        strmemccpy(title, entry->name, sizeof(title));
                        if ((attr & FILE_ATTR_MASK) == FILE_ATTR_M3U)
                            strrsplt(title, '.');
                        onplay_set_title(title);

                    }
                    onplay_result = onplay(buf, attr, curr_context, hotkey, customaction);
                }
                sort_menu_open = false;
                switch (onplay_result)
                {
                    case ONPLAY_MAINMENU:
                        return exit_to_new_screen(GO_TO_ROOT);
                        break;

                    case ONPLAY_REVEAL_FILE:
                        return exit_to_new_screen(GO_TO_FILEBROWSER);
                        break;

                    case ONPLAY_OK:
                        restore = do_restore_display;
                        break;

                    case ONPLAY_RELOAD_DIR:
                        reload_dir = true;
                        break;

                    case ONPLAY_START_PLAY:
                        return exit_to_new_screen(GO_TO_WPS);
                        break;

                    case ONPLAY_PLUGIN:
                        return exit_to_new_screen(GO_TO_PLUGIN);
                        break;
                }
                break;
            }

#ifdef HAVE_HOTSWAP
            case SYS_FS_CHANGED:
#ifdef HAVE_TAGCACHE
                if (!id3db)
#endif
                    reload_dir = true;
                /* The 'dir no longer valid' situation will be caught later
                 * by checking the showdir() result. */
                break;
#endif

            default:
                if (default_event_handler(button) == SYS_USB_CONNECTED)
                {
                    if(*tc.dirfilter > NUM_FILTER_MODES)
                        /* leave sub-browsers after usb, doing otherwise
                           might be confusing to the user */
                        exit_func = true;
                    else
                        reload_dir = true;
                }
                break;
        }
        if (start_wps)
            return exit_to_new_screen(GO_TO_WPS);
        if (button && !IS_SYSEVENT(button))
        {
            storage_spin();
        }


    check_rescan:
        /* do we need to rescan dir? */
        if (reload_dir || reload_root ||
            lastfilter != *tc.dirfilter ||
            lastsortcase != global_settings.sort_case)
        {
            if (reload_root) {
                strcpy(currdir, "/");
                tc.dirlevel = 0;
#ifdef HAVE_TAGCACHE
                tc.currtable = 0;
                tc.currextra = 0;
                lasttable = -1;
                lastextra = -1;
#endif
                reload_root = false;
            }

            if (!reload_dir)
            {
                gui_synclist_select_item(&tree_lists, row_of(0));
                gui_synclist_draw(&tree_lists);
                tc.selected_item = 0;
                lastdir[0] = 0;
            }

            lastfilter = *tc.dirfilter;
            lastsortcase = global_settings.sort_case;
            restore = do_restore_display;
        }

        if (exit_func)
            return exit_to_new_screen(GO_TO_PREVIOUS);

        if (restore || reload_dir) {
            FOR_NB_SCREENS(i)
                screens[i].scroll_stop();
            /* restore display */
            numentries = update_dir();
            reload_dir = false;
            if (currdir[1] && (numentries < 0))
            {   /* not in root and reload failed */
                reload_root = true; /* try root */
                goto check_rescan;
            }
        }
    }
    return exit_to_new_screen(GO_TO_ROOT);
}

int create_playlist(void)
{
    bool ret;
    trigger_cpu_boost();
    ret = catalog_add_to_a_playlist(PATH_ROOTSTR, ATTR_DIRECTORY, true, NULL, NULL);
    cancel_cpu_boost();

    return (ret) ? 1 : 0;
}

#define NUM_TC_BACKUP   3
static struct tree_context backups[NUM_TC_BACKUP];
/* do not make backup if it is not recursive call */
static int backup_count = -1;
int rockbox_browse(struct browse_context *browse)
{
    tc.is_browsing = (browse != NULL);
    int ret_val = 0;
    int dirfilter = SHOW_ALL;
    if (tc.is_browsing)
        dirfilter = browse->dirfilter;
    else
    {
        DEBUGF("%s browse is [NULL] \n", __func__);
        browse = tc.browse;
    }
    if (backup_count >= NUM_TC_BACKUP)
        return GO_TO_PREVIOUS;
    if (backup_count >= 0)
        backups[backup_count] = tc;
    backup_count++;
    /* the lists' width set by tree_set_width(), back as it was on leaving,
       also straight from deep in Music to the main menu or Now Playing */
    const bool was_wide = menu_wide();
    int *prev_dirfilter = tc.dirfilter;
    tc.dirfilter = &dirfilter;
    tc.sort_dir = global_settings.sort_dir;

    reload_dir = true;

    if (tc.out_of_tree > 0)
    {
        /* an item has already been loaded out_of_tree holds the selected index
         * what happens with the item is dependent on the browse context */
        tc.selected_item = tc.out_of_tree - 1;
        tc.out_of_tree = 0;
        ret_val = ft_enter(&tc);
    }
    else
    {
        if (*tc.dirfilter >= NUM_FILTER_MODES)
        {
            int last_context;
            /* don't reset if its the same browse already loaded */
            if (tc.browse != browse ||
                !(tc.currdir[1] && strstr(tc.currdir, browse->root) != NULL))
            {
                tc.browse = browse;
                tc.selected_item = 0;
                tc.dirlevel = 0;

                strmemccpy(tc.currdir, browse->root, sizeof(tc.currdir));
            }

            start_wps = false;
            last_context = curr_context;

            /* the file, as the catalogue gives it, below the root */
            const char *rel = browse->selected;
            while (rel && *rel == '/')
                rel++;
            const char *sub = rel ? strrchr(rel, '/') : NULL;
#ifdef HAVE_TAGCACHE
            if (*tc.dirfilter == SHOW_M3U && (browse->flags & BROWSE_ON_THE_GO))
            {
                /* back on On-The-Go, at the top, if the cursor was on it
                   and it's still there (update_dir), rather than at the
                   playlist chosen before it */
                if (catalog_on_otg && !otg_empty())
                    rel = sub = NULL;
                /* in Unsynced, if it is there or in one of its folders */
                catalog_mine = catalog_opens_mine(browse->root, rel);
            }
#endif
            if (sub && *tc.dirfilter == SHOW_M3U)
            {
                /* a playlist in a folder of the catalogue's: back in that
                   folder, as many levels down as it is */
                char path[MAX_PATH];
                const size_t len = path_append(path, browse->root, rel, sizeof(path));
                int depth = 0;
                for (const char *c = rel; c < sub; c++)
                    depth += *c == '/';
                /* not if the folder has gone, as when Finder removed it: the
                   catalogue's top then, or it would close at once */
                bool folder = false;
                if (len < sizeof(path) && depth + 1 < MAX_DIR_LEVELS)
                {
                    char *name = strrchr(path, '/');
                    if (name)
                    {
                        *name = '\0';
                        folder = dir_exists(path);
                        *name = '/';
                    }
                }
                if (folder)
                {
                    set_current_file_ex(path, NULL);
                    tc.dirlevel = depth + 1;
                }
                else
                {
                    strmemccpy(tc.currdir, browse->root, sizeof(tc.currdir));
                    tc.selected_item = 0;
                    tc.dirlevel = 0;
                }
            }
            else if (browse->selected)
            {
                set_current_file_ex(browse->root, browse->selected);
                /* set_current_file changes dirlevel, change it back */
                tc.dirlevel = 0;
                /* the catalogue's top read again as it's shown, On This
                   iPod's or what's synced, and the file looked for there */
                if (*tc.dirfilter == SHOW_M3U)
                    tc.selected_item = -1;
            }

            ret_val = dirbrowse();
            curr_context = last_context;
        }
        else
        {
            if (dirfilter != SHOW_ID3DB && (browse->flags & BROWSE_DIRFILTER) == 0)
                tc.dirfilter = &global_settings.dirfilter;
            tc.browse = browse;
            set_current_file(browse->root);
            if (browse->flags&BROWSE_RUNFILE)
                ret_val = ft_enter(&tc);
            else
                ret_val = dirbrowse();
        }
    }

    tc.is_browsing = false;
    tc.dirfilter = prev_dirfilter; /* Bugfix restore dirfilter*/
    if (menu_wide() != was_wide)
    {
        if (ret_val == GO_TO_WPS || ret_val == GO_TO_PREVIOUS_MUSIC)
            menu_set_wide_later(was_wide);
        else
            menu_set_wide(was_wide);
    }

    backup_count--;
    if (backup_count >= 0)
    {
        tc = backups[backup_count];
        /* the cache has the listing of the browser that's gone, as a picker
           of playlists filtered it: this one's read again */
        lastdir[0] = '\0';
    }

    return ret_val;
}

static int move_callback(int handle, void* current, void* new)
{
    struct tree_cache* cache = &tc.cache;
    ptrdiff_t diff = new - current;
    /* FIX_PTR makes sure to not accidentally update static allocations */
#define FIX_PTR(x) \
    { if ((void*)x >= current && (void*)x < (current+cache->name_buffer_size)) x+= diff; }

    if (handle == cache->name_buffer_handle)
    {   /* update entry structs, *even if they are struct tagentry */
        struct entry *this = core_get_data(cache->entries_handle);
        struct entry *last = this + cache->max_entries;
        for(; this < last; this++)
            FIX_PTR(this->name);
    }
    /* nothing to do if entries moved */
    return BUFLIB_CB_OK;
}

static struct buflib_callbacks ops = {
    .move_callback = move_callback,
    .shrink_callback = NULL,
};

void tree_mem_init(void)
{
    /* initialize tree context struct */
    struct tree_cache* cache = &tc.cache;
    memset(&tc, 0, sizeof(tc));
    tc.dirfilter = &global_settings.dirfilter;
    tc.sort_dir = global_settings.sort_dir;

#ifdef HAVE_TAGCACHE
    /* the library's songs, if more: Songs whole */
    tagcache_lists_room(global_settings.max_files_in_dir, sizeof(struct entry),
                        &cache->max_entries, &cache->name_buffer_size);
#else
    cache->name_buffer_size = AVERAGE_FILENAME_LENGTH *
        global_settings.max_files_in_dir;
    cache->max_entries = global_settings.max_files_in_dir;
#endif
    cache->name_buffer_handle = core_alloc_ex(cache->name_buffer_size, &ops);
    cache->entries_handle =
            core_alloc_ex(cache->max_entries*(sizeof(struct entry)), &ops);
}

bool bookmark_play(char *resume_file, int index, unsigned long elapsed,
                   unsigned long offset, int seed, char *filename)
{
    int i;
    char* suffix = strrchr(resume_file, '.');
    bool started = false;

    if (suffix != NULL && !strncasecmp(suffix, ".m3u", sizeof(".m3u") - 1)) /* gets m3u8 too */
    {
        /* Playlist playback */
        char* slash;
        /* check that the file exists */
        if (!file_exists(resume_file))
            return false;

        slash = strrchr(resume_file,'/');
        if (slash)
        {
            char* cp;
            *slash=0;

            cp=resume_file;
            if (!cp[0])
                cp="/";

            if (playlist_create(cp, slash+1) != -1)
            {
                if (global_settings.playlist_shuffle)
                    playlist_shuffle(seed, -1);
                started = true;
            }
            *slash='/';
        }
    }
    else
    {
        /* Directory playback */
        lastdir[0]='\0';
        if (playlist_create(resume_file, NULL) != -1)
        {
            char filename_buf[MAX_PATH + 1];
            const char* peek_filename;
            resume_directory(resume_file);
            if (global_settings.playlist_shuffle)
                playlist_shuffle(seed, -1);

            /* Check if the file is at the same spot in the directory,
               else search for it */
            int amt = playlist_amount();
            for ( i=0; i < amt; i++ )
            {
                int modidx = (i + index) % amt;
                peek_filename = playlist_peek(modidx, filename_buf,
                    sizeof(filename_buf));

                if (peek_filename == NULL)
                {
                    if (index == 0) /* searched every entry didn't find a match */
                        return false;
                    /* playlist has shrunk, search from the top */
                    i = 0;
                    amt = index;
                    index = 0;
                }
                else if (!strcmp(strrchr(peek_filename, '/') + 1, filename))
                {
                    started = true;
                    index = modidx;
                    break;
                }
            }
        }
    }

    if (started)
    {
        playlist_start(index, elapsed, offset);
        start_wps = true;
    }
    return started;
}

static void say_filetype(int attr)
{
    talk_id(tree_get_filetype_voiceclip(attr), true);
}

static int ft_play_dirname(char* name)
{
#ifdef HAVE_MULTIVOLUME
    int vol = path_get_volume_id(name);
    if (talk_volume_id(vol))
        return 1;
#endif

    return talk_file(tc.currdir, name, dir_thumbnail_name, NULL,
                     global_settings.talk_filetype ?
                     TALK_IDARRAY(VOICE_DIR) : NULL,
                     false);
}

static int ft_play_filename(char *dir, char *file, int attr)
{
    if (strlen(file) >= strlen(file_thumbnail_ext)
        && strcasecmp(&file[strlen(file) - strlen(file_thumbnail_ext)],
                      file_thumbnail_ext))
        /* file has no .talk extension */
        return talk_file(dir, NULL, file, file_thumbnail_ext,
                         TALK_IDARRAY(tree_get_filetype_voiceclip(attr)), false);

    /* it already is a .talk file, play this directly, but prefix it. */
    return talk_file(dir, NULL, file, NULL,
                     TALK_IDARRAY(LANG_VOICE_DIR_HOVER), false);
}

/* These two functions are called by the USB and shutdown handlers */
void tree_flush(void)
{
     tc.is_browsing = false;/* clear browse to prevent reentry to a possibly missing file */
#ifdef HAVE_TAGCACHE
    /* where the music playing was started in the lists, for Playing From
       after the boot */
    tagtree_keep_place();
    tagcache_shutdown();
#endif
    /* The database stays in memory over USB: Finder doesn't touch its
     * files, and the play counts written for Finder take half a second
     * from memory, where read from the disk they took three and a half. */

#ifdef HAVE_DIRCACHE
    int old_val = global_status.dircache_size;
#ifdef HAVE_EEPROM_SETTINGS
    bool savecache = false;
#endif

    if (global_settings.dircache)
    {
        dircache_suspend();

        struct dircache_info info;
        dircache_get_info(&info);

        global_status.dircache_size = info.last_size;
    #ifdef HAVE_EEPROM_SETTINGS
        savecache = firmware_settings.initialized;
    #endif
    }
    else
    {
        global_status.dircache_size = 0;
    }

    if (old_val != global_status.dircache_size)
        status_save(true);

    #ifdef HAVE_EEPROM_SETTINGS
        if (savecache)
            dircache_save();
    #endif
#endif /* HAVE_DIRCACHE */
}

void tree_restore(void)
{
#ifdef HAVE_EEPROM_SETTINGS
    firmware_settings.disk_clean = false;
#endif

#ifdef HAVE_TC_RAMCACHE
    tagcache_remove_statefile();
#endif

#ifdef HAVE_DIRCACHE
    if (global_settings.dircache)
    {
        if (dircache_resume() > 0)
        {
            /* Print "Scanning disk..." to the display. */
            splash(0, str(LANG_SCANNING_DISK));
        }
        /* built again before what was open shows, as at boot (root_menu.c) */
        dircache_wait();
    }
#endif
    /* the database's pass after it, tagcache_start_scan(), is the caller's:
       after USB it follows the import or check there is (misc.c) */
}
