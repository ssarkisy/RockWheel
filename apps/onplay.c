/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 Björn Stenberg
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
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "debug.h"
#include "lcd.h"
#include "audio.h"
#include "menu.h"
#include "lang.h"
#include "playlist.h"
#include "button.h"
#include "kernel.h"
#include "keyboard.h"
#include "mp3data.h"
#include "metadata.h"
#include "screens.h"
#include "tree.h"
#include "settings.h"
#include "playlist_viewer.h"
#include "talk.h"
#include "onplay.h"
#include "transition.h"
#include "root_menu.h"
#include "filetypes.h"
#include "fileop.h"
#include "open_plugin.h"
#include "plugin.h"
#include "bookmark.h"
#include "action.h"
#include "splash.h"
#include "yesno.h"
#include "menus/exported_menus.h"
#include "icons.h"
#include "sound_menu.h"
#include "playlist_menu.h"
#include "playlist_catalog.h"
#ifdef HAVE_TAGCACHE
#include "tagtree.h"
#include "library_playlists.h"
#include "ipod_playlists.h"
#endif
#include "cuesheet.h"
#include "chapters.h"
#include "statusbar-skinned.h"
#include "pitchscreen.h"
#include "viewport.h"
#include "pathfuncs.h"
#include "shortcuts.h"
#include "misc.h"
#ifdef HAVE_DISK_STORAGE
#include "storage.h"
#endif
#include "language.h"
#include "night_mode.h"
#include "on_the_go.h"
#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
#include "skin_engine/cover_palette.h"
#endif

static int onplay_result = ONPLAY_OK;
static bool in_queue_submenu = false;

static bool (*ctx_current_playlist_insert)(int position, bool queue, bool create_new);
static int (*ctx_add_to_playlist)(const char* playlist, bool new_playlist);
static char *onplay_get_plugin_name(bool reload);
extern struct menu_item_ex file_menu; /* settings_menu.c  */

/* redefine MAKE_MENU so the MENU_EXITAFTERTHISMENU flag can be added easily */
#define MAKE_ONPLAYMENU( name, str, callback, icon, ... )               \
    static const struct menu_item_ex *name##_[]  = {__VA_ARGS__};       \
    static const struct menu_callback_with_desc name##__ = {callback,str,icon};\
    static const struct menu_item_ex name =                             \
        {MT_MENU|MENU_HAS_DESC|MENU_EXITAFTERTHISMENU|                  \
         MENU_ITEM_COUNT(sizeof( name##_)/sizeof(*name##_)),            \
            { (void*)name##_},{.callback_and_desc = & name##__}};

static struct selected_file
{
    char buf[MAX_PATH];
    const char *path;
    int attr;
    int context;
} selected_file;

static struct clipboard
{
    char path[MAX_PATH];    /* Clipped file's path */
    unsigned int attr;      /* Clipped file's attributes */
    unsigned int flags;     /* Operation type flags */
} clipboard;

/* set selected file (doesn't touch buffer) */
static void selected_file_set(int context, const char *path, int attr)
{
    selected_file.path     = path;
    selected_file.attr     = attr;
    selected_file.context  = context;
}

/* Empty the clipboard */
static void clipboard_clear_selection(struct clipboard *clip)
{
    clip->path[0] = '\0';
    clip->attr    = 0;
    clip->flags   = 0;
}

/* Store the selection in the clipboard */
static bool clipboard_clip(struct clipboard *clip, const char *path,
                           unsigned int attr, unsigned int flags)
{
    /* if it fits it clips */
    if (strmemccpy(clip->path, path, sizeof (clip->path)) != NULL)
    {
        clip->attr = attr;
        clip->flags = flags;
        return true;
    }
    else {
        clipboard_clear_selection(clip);
        return false;
    }
}

/* CONTEXT_WPS playlist options */
/* A screen of songs over this menu, the queue's or a playlist's: Menu there
   comes back to this menu; a song chosen, or Play, goes to it */
static int wps_view_songs(int (*view)(void))
{
    /* a song's menu opened in it chose that song: this menu's is the track
       playing again, as Add to On-The-Go here goes by it */
    static struct selected_file saved_file;
    saved_file = selected_file;
    const int saved_result = onplay_result;
    const int ret = view();
    selected_file = saved_file;
    onplay_result = saved_result;
    switch (ret)
    {
        case PLAYLIST_VIEWER_CANCEL:
            return 0;
        case PLAYLIST_VIEWER_MAINMENU:
            return GO_TO_ROOT;
        default:
            return GO_TO_WPS;
    }
}

static int view_queue(void)
{
    return playlist_viewer_ex(NULL, NULL);
}

/* Up Next: the queue, the songs to come, as Apple's Music has it, always
   (the owner's decision, 2026-11-09); where they're from is Playing From */
static int wps_view_cur_playlist(void)
{
    return wps_view_songs(view_queue);
}

#ifdef HAVE_ALBUMART
static void view_album_art(void)
{
    plugin_load(VIEWERS_DIR"/imageviewer.rock", NULL);
}
#endif

/* the songs to come, opened at once: saving them needed the keyboard, and the
   rest of Rockbox's menu here was for its own playlists */
MENUITEM_FUNCTION(wps_view_cur_playlist_item,
                  MENU_EXITAFTERTHISMENU|MENU_FUNC_OPENS_SCREEN|MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_UP_NEXT), wps_view_cur_playlist, NULL, Icon_NOICON);

/* argument for add_to_playlist (for use by menu callbacks) */
#define PL_NONE    0x00
#define PL_QUEUE   0x01
#define PL_REPLACE 0x02
struct add_to_pl_param
{
    int8_t position;
    uint8_t flags;
};

static struct add_to_pl_param addtopl_insert           = {PLAYLIST_INSERT, PL_NONE};
static struct add_to_pl_param addtopl_insert_first     = {PLAYLIST_INSERT_FIRST, PL_NONE};
static struct add_to_pl_param addtopl_insert_last      = {PLAYLIST_INSERT_LAST, PL_NONE};
static struct add_to_pl_param addtopl_insert_shuf      = {PLAYLIST_INSERT_SHUFFLED, PL_NONE};
static struct add_to_pl_param addtopl_insert_last_shuf = {PLAYLIST_INSERT_LAST_SHUFFLED, PL_NONE};

static struct add_to_pl_param addtopl_queue            = {PLAYLIST_INSERT, PL_QUEUE};
static struct add_to_pl_param addtopl_queue_first      = {PLAYLIST_INSERT_FIRST, PL_QUEUE};
static struct add_to_pl_param addtopl_queue_last       = {PLAYLIST_INSERT_LAST, PL_QUEUE};
static struct add_to_pl_param addtopl_queue_shuf       = {PLAYLIST_INSERT_SHUFFLED, PL_QUEUE};
static struct add_to_pl_param addtopl_queue_last_shuf  = {PLAYLIST_INSERT_LAST_SHUFFLED, PL_QUEUE};

static struct add_to_pl_param addtopl_replace          = {PLAYLIST_INSERT, PL_REPLACE};
static struct add_to_pl_param addtopl_replace_shuffled = {PLAYLIST_INSERT_LAST_SHUFFLED, PL_REPLACE};

static void op_playlist_insert_selected(int position, bool queue)
{
#ifdef HAVE_TAGCACHE
    if (selected_file.context == CONTEXT_STD && ctx_current_playlist_insert != NULL)
    {
        ctx_current_playlist_insert(position, queue, false);
        return;
    }
    else if (selected_file.context == CONTEXT_ID3DB)
    {
        tagtree_current_playlist_insert(position, queue);
        return;
    }
#endif
    if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO)
        playlist_insert_track(NULL, selected_file.path, position, queue, true);
    else if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_M3U)
    {
#ifdef HAVE_TAGCACHE
        /* a smart one as its screen has it */
        lp_sync(selected_file.path);
#endif
        playlist_insert_playlist(NULL, selected_file.path, position, queue);
    }
    else if (selected_file.attr & ATTR_DIRECTORY)
    {
        bool recurse = (global_settings.recursive_dir_insert == RECURSE_ON);
        if (global_settings.recursive_dir_insert == RECURSE_ASK)
        {

            const char *lines[] = {
                ID2P(LANG_RECURSE_DIRECTORY_QUESTION),
                selected_file.path
            };
            const struct text_message message={lines, 2};
            /* Ask if user wants to recurse directory */
            recurse = (gui_syncyesno_run(&message, NULL, NULL)==YESNO_YES);
        }

        playlist_insert_directory(NULL, selected_file.path, position, queue,
                                  recurse == RECURSE_ON, NULL);
    }
}

/* CONTEXT_[TREE|ID3DB|STD] playlist options */
static int add_to_playlist(void* arg)
{
    struct add_to_pl_param* param = arg;
    int position = param->position;
    bool new_playlist = (param->flags & PL_REPLACE) == PL_REPLACE;
    bool queue = (param->flags & PL_QUEUE) == PL_QUEUE;

    /* warn if replacing the playlist */
    if (new_playlist && !warn_on_pl_erase())
        return 1;

    splash(0, ID2P(LANG_WAIT));

    if (new_playlist && global_settings.keep_current_track_on_replace_playlist)
    {
        if (audio_status() & AUDIO_STATUS_PLAY)
        {
            playlist_remove_all_tracks(NULL);
            new_playlist = false;
        }
    }

    if (new_playlist)
        playlist_create(NULL, NULL);

    /* always set seed before inserting shuffled */
    if (position == PLAYLIST_INSERT_SHUFFLED ||
        position == PLAYLIST_INSERT_LAST_SHUFFLED)
    {
        if (position == PLAYLIST_INSERT_LAST_SHUFFLED)
            playlist_set_last_shuffled_start();
    }

    op_playlist_insert_selected(position, queue);

    if (new_playlist && (playlist_amount() > 0))
    {
        /* nothing is currently playing so begin playing what we just
           inserted */
        if (global_settings.playlist_shuffle)
            playlist_shuffle(0, -1);
        playlist_start(0, 0, 0);
        onplay_result = ONPLAY_START_PLAY;
    }

    playlist_set_modified(NULL, true);
    return 0;
}

static bool view_playlist(void)
{
    bool result;

    result = playlist_viewer_ex(selected_file.path, NULL);

    if (result == PLAYLIST_VIEWER_OK &&
        onplay_result == ONPLAY_OK)
        /* playlist was started from viewer */
        onplay_result = ONPLAY_START_PLAY;

    return result;
}

static int treeplaylist_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list);

/* insert items */
MENUITEM_FUNCTION_W_PARAM(i_pl_item, 0, ID2P(LANG_ADD),
                  add_to_playlist, &addtopl_insert,
                  treeplaylist_callback, Icon_Playlist);
MENUITEM_FUNCTION_W_PARAM(i_first_pl_item, 0, ID2P(LANG_PLAY_NEXT),
                  add_to_playlist, &addtopl_insert_first,
                  treeplaylist_callback, Icon_Playlist);
MENUITEM_FUNCTION_W_PARAM(i_last_pl_item, 0, ID2P(LANG_PLAY_LAST),
                  add_to_playlist, &addtopl_insert_last,
                  treeplaylist_callback, Icon_Playlist);
MENUITEM_FUNCTION_W_PARAM(i_shuf_pl_item, 0, ID2P(LANG_ADD_SHUFFLED),
                  add_to_playlist, &addtopl_insert_shuf,
                  treeplaylist_callback, Icon_Playlist);
MENUITEM_FUNCTION_W_PARAM(i_last_shuf_pl_item, 0, ID2P(LANG_PLAY_LAST_SHUFFLED),
                  add_to_playlist, &addtopl_insert_last_shuf,
                  treeplaylist_callback, Icon_Playlist);
/* queue items */
MENUITEM_FUNCTION_W_PARAM(q_pl_item, 0, ID2P(LANG_QUEUE),
                  add_to_playlist, &addtopl_queue,
                  treeplaylist_callback, Icon_Playlist);
MENUITEM_FUNCTION_W_PARAM(q_first_pl_item, 0, ID2P(LANG_QUEUE_FIRST),
                  add_to_playlist, &addtopl_queue_first,
                  treeplaylist_callback, Icon_Playlist);
MENUITEM_FUNCTION_W_PARAM(q_last_pl_item, 0, ID2P(LANG_QUEUE_LAST),
                  add_to_playlist, &addtopl_queue_last,
                  treeplaylist_callback, Icon_Playlist);
MENUITEM_FUNCTION_W_PARAM(q_shuf_pl_item, 0, ID2P(LANG_QUEUE_SHUFFLED),
                  add_to_playlist, &addtopl_queue_shuf,
                  treeplaylist_callback, Icon_Playlist);
MENUITEM_FUNCTION_W_PARAM(q_last_shuf_pl_item, 0, ID2P(LANG_QUEUE_LAST_SHUFFLED),
                  add_to_playlist, &addtopl_queue_last_shuf,
                  treeplaylist_callback, Icon_Playlist);

/* queue submenu */
MAKE_ONPLAYMENU(queue_menu, ID2P(LANG_QUEUE_MENU),
                treeplaylist_callback, Icon_Playlist,
                &q_first_pl_item,
                &q_pl_item,
                &q_shuf_pl_item,
                &q_last_pl_item,
                &q_last_shuf_pl_item);

/* replace playlist */
MENUITEM_FUNCTION_W_PARAM(replace_pl_item, 0, ID2P(LANG_PLAY),
                  add_to_playlist, &addtopl_replace,
                  treeplaylist_callback, Icon_Playlist);

MENUITEM_FUNCTION_W_PARAM(replace_shuf_pl_item, 0, ID2P(LANG_PLAY_SHUFFLED),
                  add_to_playlist, &addtopl_replace_shuffled,
                  treeplaylist_callback, Icon_Playlist);

MAKE_ONPLAYMENU(tree_playlist_menu, ID2P(LANG_PLAYING_NEXT),
                treeplaylist_callback, Icon_Playlist,

                /* insert */
                &i_first_pl_item,
                &i_pl_item,
                &i_last_pl_item,
                &i_shuf_pl_item,
                &i_last_shuf_pl_item,

                /* queue */
                &q_first_pl_item,
                &q_pl_item,
                &q_last_pl_item,
                &q_shuf_pl_item,
                &q_last_shuf_pl_item,

                /* Queue submenu */
                &queue_menu,

                /* replace */
                &replace_pl_item,
                &replace_shuf_pl_item
               );

static int treeplaylist_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list)
{
    (void)this_list;
    int sel_file_attr = (selected_file.attr & FILE_ATTR_MASK);

    switch (action)
    {
    case ACTION_REQUEST_MENUITEM:
        if (this_item == &tree_playlist_menu)
        {
            if (sel_file_attr != FILE_ATTR_AUDIO &&
                sel_file_attr != FILE_ATTR_M3U &&
                (selected_file.attr & ATTR_DIRECTORY) == 0)
                return ACTION_EXIT_MENUITEM;
        }
        else if (this_item == &queue_menu)
        {
            if (global_settings.show_queue_options != QUEUE_SHOW_IN_SUBMENU)
                return ACTION_EXIT_MENUITEM;

            /* queueing options only work during playback */
            if (!(audio_status() & AUDIO_STATUS_PLAY))
                return ACTION_EXIT_MENUITEM;
        }
        else if ((this_item->flags & MENU_TYPE_MASK) == MT_FUNCTION_CALL_W_PARAM &&
                 this_item->function_param->function_w_param == add_to_playlist)
        {
            struct add_to_pl_param *param = this_item->function_param->param;

            /* what plays, now that these are in the first menu too */
            if (sel_file_attr != FILE_ATTR_AUDIO &&
                sel_file_attr != FILE_ATTR_M3U &&
                (selected_file.attr & ATTR_DIRECTORY) == 0)
                return ACTION_EXIT_MENUITEM;
#ifdef HAVE_TAGCACHE
            /* nor Music's own rows, as Songs, the whole library, or Albums,
               as On-The-Go hasn't them either: a stock iPod's open nothing */
            if (selected_file.context == CONTEXT_ID3DB && tagtree_at_top())
                return ACTION_EXIT_MENUITEM;
#endif

            if ((param->flags & PL_QUEUE) == PL_QUEUE)
            {
                if (global_settings.show_queue_options != QUEUE_SHOW_AT_TOPLEVEL &&
                    !in_queue_submenu)
                    return ACTION_EXIT_MENUITEM;
            }

            if (param->position == PLAYLIST_INSERT_SHUFFLED ||
                param->position == PLAYLIST_INSERT_LAST_SHUFFLED)
            {
                if (!global_settings.show_shuffled_adding_options)
                    return ACTION_EXIT_MENUITEM;

                if (sel_file_attr != FILE_ATTR_M3U &&
                    (selected_file.attr & ATTR_DIRECTORY) == 0)
                    return ACTION_EXIT_MENUITEM;
            }

            if ((param->flags & PL_REPLACE) != PL_REPLACE)
            {
                if (!(audio_status() & AUDIO_STATUS_PLAY))
                    return ACTION_EXIT_MENUITEM;
            }
        }

        break;

    case ACTION_ENTER_MENUITEM:
        in_queue_submenu = this_item == &queue_menu;
        break;
    }

    return action;
}

void onplay_show_playlist_menu(const char* path, int attr, void (*playlist_insert_cb))
{
    ctx_current_playlist_insert = playlist_insert_cb;
    selected_file_set(CONTEXT_STD, path, attr);
    in_queue_submenu = false;
    do_menu(&tree_playlist_menu, NULL, NULL, false);
}

/* On-The-Go, as on a stock iPod: the song, album, artist or the like, or the
   playlist, at its end; over Now Playing, the song playing */
static int add_to_on_the_go(void)
{
#ifdef HAVE_TAGCACHE
    /* as into any playlist made on the iPod: its index and, if it plays,
       the queue with it */
    switch (selected_file.context)
    {
        case CONTEXT_ID3DB:
            ip_add_selection(OTG_FILE);
            break;
        case CONTEXT_WPS:
        {
            const struct mp3entry *id3 = audio_current_track();
            if (id3 && id3->path[0])
                ip_add_file(OTG_FILE, id3->path);
            break;
        }
        default:
            if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_M3U)
                ip_add_playlist(OTG_FILE, selected_file.path);
            else if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO)
                ip_add_file(OTG_FILE, selected_file.path);
            break;
    }
#else
    switch (selected_file.context)
    {
        case CONTEXT_WPS:
        {
            const struct mp3entry *id3 = audio_current_track();
            if (id3 && id3->path[0])
                otg_add_file(id3->path);
            break;
        }
        default:
            if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_M3U)
                otg_add_playlist(selected_file.path);
            else if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO)
                otg_add_file(selected_file.path);
            break;
    }
#endif
    return 0;
}

static int on_the_go_callback(int action, const struct menu_item_ex *this_item,
                              struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action != ACTION_REQUEST_MENUITEM)
        return action;
    switch (selected_file.context)
    {
#ifdef HAVE_TAGCACHE
        case CONTEXT_ID3DB:
        {
            /* not Music's own rows, as Songs, which would add all of them, nor
               a list empty, which has no row */
            const int attr = selected_file.attr & FILE_ATTR_MASK;
            return tagtree_at_top()
                || (attr != FILE_ATTR_AUDIO && !(selected_file.attr & ATTR_DIRECTORY)) ?
                   ACTION_EXIT_MENUITEM : action;
        }
#endif
        case CONTEXT_WPS:
            return audio_current_track() ? action : ACTION_EXIT_MENUITEM;
        default:
        {
            const int attr = selected_file.attr & FILE_ATTR_MASK;
            return selected_file.path
                && (attr == FILE_ATTR_AUDIO || attr == FILE_ATTR_M3U) ?
                   action : ACTION_EXIT_MENUITEM;
        }
    }
}

MENUITEM_FUNCTION(on_the_go_item, 0, ID2P(LANG_ADD_TO_ON_THE_GO),
                  add_to_on_the_go, on_the_go_callback, Icon_Playlist);

#ifdef HAVE_TAGCACHE
/* The same, at the end of a playlist made on the iPod, picked from the
   catalogue or made there: as Add to Playlist on a stock iPod, but not into
   one synced from Finder, which the next sync writes again */
static int add_to_picked_playlist(void)
{
    char target[MAX_PATH], song[MAX_PATH];
    song[0] = '\0';
    /* the song playing as it is now, before the catalogue is gone through */
    if (selected_file.context == CONTEXT_WPS)
    {
        const struct mp3entry *id3 = audio_current_track();
        if (!id3 || !id3->path[0])
            return 0;
        strmemccpy(song, id3->path, sizeof(song));
    }
    const int r = ip_pick_playlist(target, sizeof(target));
    if (r < 0)
    {
        onplay_result = ONPLAY_MAINMENU;
        return 0;
    }
    if (r == 0)
        return 0;
    switch (selected_file.context)
    {
        case CONTEXT_ID3DB:
            tagtree_reload_after_browse();
            ip_add_selection(target);
            break;
        case CONTEXT_WPS:
            ip_add_file(target, song);
            break;
        default:
            if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_M3U)
                ip_add_playlist(target, selected_file.path);
            else if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO)
                ip_add_file(target, selected_file.path);
            break;
    }
    return 0;
}

MENUITEM_FUNCTION(add_to_playlist_item, 0, ID2P(LANG_ADD_TO_PL),
                  add_to_picked_playlist, on_the_go_callback, Icon_Playlist);
#endif

/* playlist catalog options */
static bool cat_add_to_a_playlist(void)
{
    return catalog_add_to_a_playlist(selected_file.path, selected_file.attr,
                                     false, NULL, ctx_add_to_playlist);
}

static bool cat_add_to_a_new_playlist(void)
{
    return catalog_add_to_a_playlist(selected_file.path, selected_file.attr,
                                     true, NULL, ctx_add_to_playlist);
}

static int cat_playlist_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list);

MENUITEM_FUNCTION(cat_add_to_list, 0, ID2P(LANG_ADD_TO_EXISTING_PL),
                  cat_add_to_a_playlist, NULL, Icon_Playlist);
MENUITEM_FUNCTION(cat_add_to_new, 0, ID2P(LANG_CATALOG_ADD_TO_NEW),
                  cat_add_to_a_new_playlist, NULL, Icon_Playlist);
MAKE_ONPLAYMENU(cat_playlist_menu, ID2P(LANG_ADD_TO_PL),
                cat_playlist_callback, Icon_Playlist,
                &cat_add_to_list, &cat_add_to_new);

void onplay_show_playlist_cat_menu(const char* track_name, int attr, void (*add_to_pl_cb))
{
    ctx_add_to_playlist = add_to_pl_cb;
    selected_file_set(CONTEXT_STD, track_name, attr);
    do_menu(&cat_playlist_menu, NULL, NULL, false);
}

static int cat_playlist_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (!selected_file.path ||
        (((selected_file.attr & FILE_ATTR_MASK) != FILE_ATTR_AUDIO) &&
         ((selected_file.attr & FILE_ATTR_MASK) != FILE_ATTR_M3U) &&
         ((selected_file.attr & ATTR_DIRECTORY) == 0)))
    {
        return ACTION_EXIT_MENUITEM;
    }

    if (action == ACTION_REQUEST_MENUITEM)
    {
        if ((audio_status() & AUDIO_STATUS_PLAY)
           || selected_file.context != CONTEXT_WPS)
        {
            return action;
        }
        return ACTION_EXIT_MENUITEM;
    }
    return action;
}

static void splash_cancelled(void)
{
    clear_screen_buffer(true);
    splash(HZ, ID2P(LANG_CANCEL));
}

static void splash_failed(int lang_what, int err)
{
    cond_talk_ids_fq(lang_what, LANG_FAILED);
    clear_screen_buffer(true);
    splashf(HZ*2, "%s %s (%d)", str(lang_what), str(LANG_FAILED), err);
}

static bool clipboard_cut(void)
{
    return clipboard_clip(&clipboard, selected_file.path, selected_file.attr,
                          PASTE_CUT);
}

static bool clipboard_copy(void)
{
    return clipboard_clip(&clipboard, selected_file.path, selected_file.attr,
                          PASTE_COPY);
}

/* Paste the clipboard to the current directory */
static int clipboard_paste(void)
{
    if (!clipboard.path[0])
        return 1;

    int rc = copy_move_fileobject(clipboard.path, getcwd(NULL, 0), clipboard.flags);

    switch (rc)
    {
    case FORC_CANCELLED:
        splash_cancelled();
        /* Fallthrough */
    case FORC_SUCCESS:
        onplay_result = ONPLAY_RELOAD_DIR;
        /* Fallthrough */
    case FORC_NOOP:
        clipboard_clear_selection(&clipboard);
        /* Fallthrough */
    case FORC_NOOVERWRT:
        break;
    default:
        if (rc < FORC_SUCCESS) {
            splash_failed(LANG_PASTE, rc);
            onplay_result = ONPLAY_RELOAD_DIR;
        }
    }

    return 1;
}

static char *wps_context_get_item_name(int selected_item, void * data,
                               char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    int item = (intptr_t)data;
    const struct hotkey_assignment *hkey =
      get_hotkey(HK_CTX_GET(item, global_settings.context_wps));
    if (hkey->action == HOTKEY_PLUGIN)
    {
        return onplay_get_plugin_name(false);
    }
#ifdef HAVE_PITCHCONTROL
    else if (hkey->action == HOTKEY_PITCHSCREEN)
    {
        int32_t ts = dsp_get_timestretch();
        int32_t pitch = sound_get_pitch();
        if (ts != PITCH_SPEED_100 || pitch != PITCH_SPEED_100)
        {
            int32_t speed = GET_SPEED(pitch, ts);
            snprintf(buffer, buffer_len,"%s %ld.%ld%% / %ld.%ld%%", str(hkey->lang_id),
                 pitch / PITCH_SPEED_PRECISION,
                (pitch % PITCH_SPEED_PRECISION) / (PITCH_SPEED_PRECISION / 10),
                 speed / PITCH_SPEED_PRECISION,
                (speed % PITCH_SPEED_PRECISION) / (PITCH_SPEED_PRECISION / 10));
            return buffer;
        }
    }
#endif
    return ID2P(hkey->lang_id);
}

static int wps_context_item_speak_item(int selected_item, void * data)
{
    int item = (intptr_t)data;
    const struct hotkey_assignment *hkey =
      get_hotkey(HK_CTX_GET(item, global_settings.context_wps));

    if (hkey->action == HOTKEY_PLUGIN)
    {
        talk_spell(onplay_get_plugin_name(false), false);
        return 0;
    }
#ifdef HAVE_PITCHCONTROL
    else if (hkey->action == HOTKEY_PITCHSCREEN)
    {
        int32_t ts = dsp_get_timestretch();
        int32_t pitch = sound_get_pitch();
        if (ts != PITCH_SPEED_100 || pitch != PITCH_SPEED_100)
        {
            talk_id(hkey->lang_id, false);

            if (pitch != PITCH_SPEED_100)
                talk_value_decimal(pitch, UNIT_PERCENT, 2, true);
            if (ts != PITCH_SPEED_100)
            {
                int32_t speed = GET_SPEED(pitch, ts);
                talk_id(LANG_SPEED, true);
                talk_value_decimal(speed, UNIT_PERCENT, 2, true);
            }
            return 0;
        }
    }
#endif
    talk_id(hkey->lang_id, false);
    return 0;
    (void)selected_item;
}

static int execute_hotkey(int action);
static int wps_context_item_cb(int,const struct menu_item_ex *,struct gui_synclist *);

/* need special handling so we can change the icon */
#define WPSCTX_RETURNVALUE_DYNTEXT(name, val, cb, text_callback,            \
                                     voice_callback, text_cb_data, icon)    \
     struct menu_get_name_and_icon name##_                                  \
         = {cb,text_callback,voice_callback,text_cb_data,icon};             \
     static const struct menu_item_ex name   =                              \
        { MT_RETURN_VALUE|MENU_DYNAMIC_DESC, { .value = val},               \
        {.menu_get_name_and_icon = & name##_}};

WPSCTX_RETURNVALUE_DYNTEXT(context_item_0, GO_TO_PREVIOUS, wps_context_item_cb,
  wps_context_get_item_name, wps_context_item_speak_item, (void*)0, Icon_NOICON);

WPSCTX_RETURNVALUE_DYNTEXT(context_item_1, GO_TO_PREVIOUS, wps_context_item_cb,
  wps_context_get_item_name, wps_context_item_speak_item, (void*)1, Icon_NOICON);

WPSCTX_RETURNVALUE_DYNTEXT(context_item_2, GO_TO_PREVIOUS, wps_context_item_cb,
  wps_context_get_item_name, wps_context_item_speak_item, (void*)2, Icon_NOICON);

WPSCTX_RETURNVALUE_DYNTEXT(context_item_3, GO_TO_PREVIOUS, wps_context_item_cb,
  wps_context_get_item_name, wps_context_item_speak_item, (void*)3, Icon_NOICON);

WPSCTX_RETURNVALUE_DYNTEXT(context_item_4, GO_TO_PREVIOUS, wps_context_item_cb,
  wps_context_get_item_name, wps_context_item_speak_item, (void*)4, Icon_NOICON);

/* map item number to menu_get_name_and_icon structs so we can change icons */
static struct menu_get_name_and_icon * const ctx_item_map[HK_CTX_ITEMS]=
  {&context_item_0_, &context_item_1_, &context_item_2_, &context_item_3_, &context_item_4_};

#ifdef HAVE_PITCHCONTROL
MENUITEM_FUNCTION(pitch_screen_item, 0, ID2P(LANG_PITCH),
                  gui_syncpitchscreen_run, NULL, Icon_Audio);
MENUITEM_FUNCTION(pitch_reset_item, 0, ID2P(LANG_RESET_SETTING),
                  reset_pitch, NULL, Icon_Submenu_Entered);

MAKE_ONPLAYMENU(pitch_menu, ID2P(LANG_PITCH),
                NULL, Icon_Audio,
                &pitch_screen_item,
                &pitch_reset_item);
#endif

static int wps_context_item_cb(int action,
                                  const struct menu_item_ex *this_item,
                                  struct gui_synclist *this_list)
{
    if (action == ACTION_ENTER_MENUITEM || action == ACTION_REQUEST_MENUITEM)
    {
        int item = (intptr_t) this_item->menu_get_name_and_icon->list_get_name_data;
        int act =  HK_CTX_GET(item, global_settings.context_wps);
        if (act == HOTKEY_OFF)
        {
            return ACTION_EXIT_MENUITEM;
        }
        ctx_item_map[item]->icon_id = get_hotkey(act)->icon;
#ifdef HAVE_PITCHCONTROL
        if (act == HOTKEY_PITCHSCREEN)
        {
            int32_t ts = dsp_get_timestretch();
            if (sound_get_pitch() != PITCH_SPEED_100 || ts != PITCH_SPEED_100)
            {
                ctx_item_map[item]->icon_id = Icon_Submenu; /* if setting changed show + */
            }
        }
#endif
    }
    else if (action == ACTION_EXIT_MENUITEM) /* selected */
    {
        int item = (intptr_t) this_item->menu_get_name_and_icon->list_get_name_data;
        int act =  HK_CTX_GET(item, global_settings.context_wps);
        if (act == HOTKEY_CONTEXT_MENU)
        {
            onplay_result = hotkey_run_menu(HOTKEY_FLAG_WPS, true, 0);
        }
#ifdef HAVE_PITCHCONTROL
        else if (act == HOTKEY_PITCHSCREEN && ctx_item_map[item]->icon_id == Icon_Submenu)
        {
            do_menu(&pitch_menu, NULL, NULL, false);
        }
#endif
        else
        {
            onplay_result = execute_hotkey(act);
        }
        return ACTION_EXIT_AFTER_THIS_MENUITEM;
    }
    return action;
    (void)this_list;
}


static int browse_id3_wrapper(void)
{
    if (browse_id3(audio_current_track(),
            playlist_get_display_index(),
            playlist_amount(), NULL, 1, NULL))
        return GO_TO_ROOT;
    return GO_TO_PREVIOUS;
}

/* CONTEXT_WPS items */
MENUITEM_FUNCTION(browse_id3_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_MENU_SHOW_ID3_INFO),
                  browse_id3_wrapper, NULL, Icon_NOICON);

static int clipboard_delete_selected_fileobject(void)
{
    int rc = delete_fileobject(selected_file.path);
    if (rc < FORC_SUCCESS) {
        splash_failed(LANG_DELETE, rc);
    } else if (rc == FORC_CANCELLED) {
        splash_cancelled();
    }
    if (rc != FORC_NOOP) {
        /* Could have failed after some but not all needed changes; reload */
        onplay_result = ONPLAY_RELOAD_DIR;
    }
    return 1;
}

static void show_result(int rc, int lang_what)
{
    if (rc < FORC_SUCCESS) {
        splash_failed(lang_what, rc);
    } else if (rc == FORC_CANCELLED) {
        /* splash_cancelled(); kbd_input() splashes it */
    } else if (rc == FORC_SUCCESS) {
        onplay_result = ONPLAY_RELOAD_DIR;
    }
}

static int clipboard_create_dir(void)
{
    int rc = create_dir();

    show_result(rc, LANG_CREATE_DIR);

    return 1;
}

static int clipboard_rename_selected_file(void)
{
    int rc = rename_file(selected_file.path);

    show_result(rc, LANG_RENAME);

    return 1;
}

/* CONTEXT_[TREE|ID3DB] items */
static int clipboard_callback(int action,
                              const struct menu_item_ex *this_item,
                              struct gui_synclist *this_list);

MENUITEM_FUNCTION(rename_file_item, 0, ID2P(LANG_RENAME),
                  clipboard_rename_selected_file, clipboard_callback, Icon_NOICON);
MENUITEM_FUNCTION(clipboard_cut_item, 0, ID2P(LANG_CUT),
                  clipboard_cut, clipboard_callback, Icon_NOICON);
MENUITEM_FUNCTION(clipboard_copy_item, 0, ID2P(LANG_COPY),
                  clipboard_copy, clipboard_callback, Icon_NOICON);
MENUITEM_FUNCTION(clipboard_paste_item, 0, ID2P(LANG_PASTE),
                  clipboard_paste, clipboard_callback, Icon_NOICON);
MENUITEM_FUNCTION(delete_file_item, 0, ID2P(LANG_DELETE),
                  clipboard_delete_selected_fileobject, clipboard_callback, Icon_NOICON);
MENUITEM_FUNCTION(delete_dir_item, 0, ID2P(LANG_DELETE_DIR),
                 clipboard_delete_selected_fileobject, clipboard_callback, Icon_NOICON);
MENUITEM_FUNCTION(create_dir_item, 0, ID2P(LANG_CREATE_DIR),
                  clipboard_create_dir, clipboard_callback, Icon_NOICON);

/* other items */
static bool list_viewers(void)
{
    int ret = filetype_list_viewers(selected_file.path);
    if (ret == PLUGIN_USB_CONNECTED)
        onplay_result = ONPLAY_RELOAD_DIR;
    return false;
}

#ifdef HAVE_TAGCACHE
static bool prepare_database_sel(void *param)
{
    if (selected_file.context == CONTEXT_ID3DB)
    {
        if (param && !strcmp(param, "properties")
            && (selected_file.attr & FILE_ATTR_MASK) != FILE_ATTR_AUDIO)
        {
            strmemccpy(selected_file.buf, MAKE_ACT_STR(ACTIVITY_DATABASEBROWSER),
                       sizeof(selected_file.buf));
        }
        else
        {
            /* If database is not loaded into RAM, or tagcache_ram is
               set to "quick", filename needs to be retrieved from disk! */
#ifdef HAVE_DISK_STORAGE
            if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO
                && !storage_disk_is_active()
#ifdef HAVE_TC_RAMCACHE
                && (global_settings.tagcache_ram != TAGCACHE_RAM_ON
                    || !tagcache_is_in_ram())
#endif
            )
                splash(0, ID2P(LANG_WAIT));
#endif
             if (!tagtree_get_subentry_filename(selected_file.buf, MAX_PATH))
            {
                onplay_result = ONPLAY_RELOAD_DIR;
                return false;
            }
        }

        selected_file.path = selected_file.buf;
    }
    return true;
}
#endif

static bool onplay_load_plugin(void *param)
{
#ifdef HAVE_TAGCACHE
    if (!prepare_database_sel(param))
        return false;
#endif
    int ret = filetype_load_plugin((const char*)param, selected_file.path);
    if (ret == PLUGIN_USB_CONNECTED)
        onplay_result = ONPLAY_RELOAD_DIR;
    else if (ret == PLUGIN_GOTO_PLUGIN)
        onplay_result = ONPLAY_PLUGIN;
    else if (ret == PLUGIN_GOTO_WPS)
        onplay_result = ONPLAY_START_PLAY;
    else if (ret == PLUGIN_GOTO_ROOT)
        onplay_result = ONPLAY_MAINMENU;
    return false;
}

static char *onplay_get_plugin_name(bool reload)
{
    static char ctx_plugin_namebuf[OPEN_PLUGIN_NAMESZ] = "";
    if (reload || ctx_plugin_namebuf[0] == '\0')
    {
        int res = open_plugin_load_entry(ID2P(LANG_ONPLAY_MENU_TITLE));
        if (res >= 0 || res ==  OPEN_PLUGIN_NEEDS_FLUSHED)
        {
            struct open_plugin_entry_t *op = open_plugin_get_entry();
            strmemccpy(ctx_plugin_namebuf, op->name, sizeof(ctx_plugin_namebuf));
        }
        else
        {
            strmemccpy(ctx_plugin_namebuf, str(LANG_OPEN_PLUGIN),
                       sizeof(ctx_plugin_namebuf));
        }
    }

    if (get_current_activity() == ACTIVITY_SETTINGS)
        return str(LANG_OPEN_PLUGIN);
    return ctx_plugin_namebuf;
}

static int reveal(void)
{
#ifdef HAVE_TAGCACHE
    if (!prepare_database_sel(NULL))
        return 0;
#endif
    if (!file_exists(selected_file.path))
    {
        splash(HZ*2, ID2P(LANG_FILE_NOT_FOUND));
        return 0;
    }

    strmemccpy(global_status.browse_last_folder, selected_file.path,
               sizeof global_status.browse_last_folder);
    onplay_result = ONPLAY_REVEAL_FILE;
    return GO_TO_FILEBROWSER;
}

MENUITEM_FUNCTION(reveal_item, 0, ID2P(LANG_SHOW_IN_FILES),
                  reveal, clipboard_callback, Icon_file_view_menu);
MENUITEM_FUNCTION(list_viewers_item, 0, ID2P(LANG_ONPLAY_OPEN_WITH),
                  list_viewers, clipboard_callback, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(properties_item, 0, ID2P(LANG_PROPERTIES),
                  onplay_load_plugin, (void *)"properties",
                  clipboard_callback, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(track_info_item, 0, ID2P(LANG_MENU_SHOW_ID3_INFO),
                  onplay_load_plugin, (void *)"properties",
                  clipboard_callback, Icon_NOICON);
#ifdef HAVE_TAGCACHE
MENUITEM_FUNCTION_W_PARAM(pictureflow_item, 0, ID2P(LANG_ONPLAY_PICTUREFLOW),
                  onplay_load_plugin, (void *)"pictureflow",
                  clipboard_callback, Icon_NOICON);
#endif
static bool onplay_add_to_shortcuts(void)
{
    shortcuts_add(SHORTCUT_BROWSER, selected_file.path);
    return false;
}
MENUITEM_FUNCTION(add_to_faves_item, 0, ID2P(LANG_ADD_TO_FAVES),
                  onplay_add_to_shortcuts,
                  clipboard_callback, Icon_NOICON);

static void set_dir_helper(char* dirnamebuf, size_t bufsz)
{
    path_append(dirnamebuf, selected_file.path, PA_SEP_HARD, bufsz);
    settings_save();
}

#if LCD_DEPTH > 1

static void show_updated_backdrop(void)
{
    skin_backdrop_load_setting();
    viewportmanager_theme_changed(THEME_STATUSBAR);
    skin_backdrop_show(sb_get_backdrop(SCREEN_MAIN));
}

static bool set_backdrop(void)
{
    char previous_backdrop[sizeof global_settings.backdrop_file];
    strcpy(previous_backdrop, global_settings.backdrop_file);

    path_append(global_settings.backdrop_file, selected_file.path,
                PA_SEP_HARD, sizeof(global_settings.backdrop_file));

    show_updated_backdrop();

    if (!yesno_pop(ID2P(LANG_SET_AS_BACKDROP))) {
        strcpy(global_settings.backdrop_file, previous_backdrop);
        show_updated_backdrop();
    }
    else
        settings_save();

    return true;
}
MENUITEM_FUNCTION(set_backdrop_item, 0, ID2P(LANG_SET_AS_BACKDROP),
                  set_backdrop, clipboard_callback, Icon_NOICON);
#endif
#ifdef HAVE_RECORDING
static bool set_recdir(void)
{
    set_dir_helper(global_settings.rec_directory,
                   sizeof(global_settings.rec_directory));
    return false;
}
MENUITEM_FUNCTION(set_recdir_item, 0, ID2P(LANG_RECORDING_DIR),
                  set_recdir, clipboard_callback, Icon_Recording);
#endif
static bool set_startdir(void)
{
    set_dir_helper(global_settings.start_directory,
                   sizeof(global_settings.start_directory));
    return false;
}
MENUITEM_FUNCTION(set_startdir_item, 0, ID2P(LANG_START_DIR),
                  set_startdir, clipboard_callback, Icon_file_view_menu);

static bool set_catalogdir(void)
{
    catalog_set_directory(selected_file.path);
    settings_save();
    return false;
}
MENUITEM_FUNCTION(set_catalogdir_item, 0, ID2P(LANG_PLAYLIST_DIR),
                  set_catalogdir, clipboard_callback, Icon_Playlist);

#ifdef HAVE_TAGCACHE
static bool set_databasedir(void)
{
    struct tagcache_stat *tc_stat = tagcache_get_stat();
    if (strcasecmp(selected_file.path, tc_stat->db_path))
    {
        splash(HZ, ID2P(LANG_PLEASE_REBOOT));
    }

    set_dir_helper(global_settings.tagcache_db_path,
                   sizeof(global_settings.tagcache_db_path));
    return false;
}
MENUITEM_FUNCTION(set_databasedir_item, 0, ID2P(LANG_DATABASE_DIR),
                  set_databasedir, clipboard_callback, Icon_Audio);
#endif

MAKE_ONPLAYMENU(set_as_dir_menu, ID2P(LANG_SET_AS),
                clipboard_callback, Icon_NOICON,
                &set_catalogdir_item,
#ifdef HAVE_TAGCACHE
                &set_databasedir_item,
#endif
#ifdef HAVE_RECORDING
                &set_recdir_item,
#endif
                &set_startdir_item);

static int clipboard_callback(int action,
                              const struct menu_item_ex *this_item,
                              struct gui_synclist *this_list)
{
    (void)this_list;
    switch (action)
    {
        case ACTION_REQUEST_MENUITEM:
            /* Rockbox's actions on files, its plugins and its info pages: there's
               no file browser here, and the files are Finder's */
            return ACTION_EXIT_MENUITEM;
#ifdef HAVE_MULTIVOLUME
            /* no rename+delete for volumes */
            if ((selected_file.attr & ATTR_VOLUME) &&
                (this_item == &rename_file_item ||
                 this_item == &delete_dir_item ||
                 this_item == &clipboard_cut_item ||
                 this_item == &list_viewers_item))
                return ACTION_EXIT_MENUITEM;
#endif
#ifdef HAVE_TAGCACHE
            if (selected_file.context == CONTEXT_ID3DB)
            {
                /* no PictureFlow, which is Cover Flow in the Music menu,
                   and no files, which RockWheel's menus don't show */
                if (this_item == &track_info_item)
                    return action;
                return ACTION_EXIT_MENUITEM;
            }
#endif
            if (this_item == &clipboard_paste_item)
            {  /* visible if there is something to paste */
                return (clipboard.path[0] != 0) ?
                                    action : ACTION_EXIT_MENUITEM;
            }
            else if (this_item == &create_dir_item &&
                     *tree_get_context()->dirfilter <= NUM_FILTER_MODES)
            {
                return action;
            }
            else if (selected_file.path)
            {
                /* requires an actual file */
                if (this_item == &clipboard_cut_item ||
                    this_item == &clipboard_copy_item)
                {
                    if (*tree_get_context()->dirfilter != SHOW_M3U)
                        return action;
                }
                else if (this_item == &rename_file_item ||
                    (this_item == &track_info_item &&
                        (selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO) ||
                    (this_item == &properties_item &&
                        (selected_file.attr & FILE_ATTR_MASK) != FILE_ATTR_AUDIO) ||
                    this_item == &add_to_faves_item)
                {
                    return action;
                }
                else if ((selected_file.attr & ATTR_DIRECTORY))
                {
                    /* only for directories */
                    if (this_item == &delete_dir_item ||
                        this_item == &set_startdir_item ||
                        this_item == &set_catalogdir_item ||
#ifdef HAVE_TAGCACHE
                        this_item == &set_databasedir_item ||
#endif
#ifdef HAVE_RECORDING
                        this_item == &set_recdir_item ||
#endif
                        this_item == &set_as_dir_menu
                        )
                        return action;
                }
                    /* only for files */
                else if (this_item == &list_viewers_item)
                {
                    if (*tree_get_context()->dirfilter != SHOW_M3U)
                        return action;
                }
                else if (this_item == &delete_file_item ||
                         (this_item == &reveal_item &&
                          selected_file.context == CONTEXT_WPS))
                    return action;
#if LCD_DEPTH > 1
                else if (this_item == &set_backdrop_item)
                {
                    char *suffix = strrchr(selected_file.path, '.');
                    if (suffix)
                    {
                        if (strcasecmp(suffix, ".bmp") == 0)
                        {
                            return action;
                        }
                    }
                }
#endif
            }
            return ACTION_EXIT_MENUITEM;
            break;
    }
    return action;
}

static int onplaymenu_callback(int action,
                               const struct menu_item_ex *this_item,
                               struct gui_synclist *this_list);

#ifdef HAVE_TAGCACHE
/* Cover Flow at the album playing: leaving it goes back to the WPS, not to
   this menu */
static int open_cover_flow(void)
{
    switch (root_menu_cover_flow(NULL, NULL))
    {
        case PLUGIN_GOTO_ROOT:
            return GO_TO_ROOT;
        case PLUGIN_GOTO_PLUGIN:
            return GO_TO_PLUGIN;
        default:
            return GO_TO_WPS;
    }
}
MENUITEM_FUNCTION(cover_flow_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_COVER_FLOW),
                  open_cover_flow, NULL, Icon_NOICON);

/* where the lists open from a menu over another screen */
enum {
    LISTS_AT_PLAYING,   /* the track playing */
    LISTS_AT_FROM,      /* where it was started */
    LISTS_AT_SONG,      /* the song of the menu, of Search or a playlist */
};

/* The Music lists opened from the menu over Now Playing, Search or a playlist.
   Menu there comes back to that menu or screen, which goes on as it was,
   whatever a menu opened in the lists has chosen. */
static int go_to_lists(int at, bool artist)
{
    static struct selected_file saved_file;
    bool (*saved_insert)(int, bool, bool) = ctx_current_playlist_insert;
    int (*saved_add)(const char*, bool) = ctx_add_to_playlist;
    char path[MAX_PATH];

    saved_file = selected_file;
    if (at == LISTS_AT_SONG)
        strmemccpy(path, selected_file.path, sizeof(path));
    /* the lists as they always are, beside the panel */
    const bool wide = menu_wide();
    menu_set_wide(false);
    int screen;
    switch (at)
    {
        case LISTS_AT_PLAYING:
            screen = root_menu_go_to_playing(artist);
            break;
        case LISTS_AT_FROM:
            screen = root_menu_playing_from();
            break;
        default:
            transition_begin(TRANSITION_FORWARD);
            screen = root_menu_go_to_track(path, artist);
            /* not found: nothing slides */
            if (screen < 0)
            {
                transition_cancel();
                screen = 0;
            }
            break;
    }
    menu_set_wide(wide);
    if (screen == 0)
    {
        selected_file = saved_file;
        ctx_current_playlist_insert = saved_insert;
        ctx_add_to_playlist = saved_add;
        onplay_result = ONPLAY_OK;
    }
    return screen;
}
static int go_to_artist(void)
{
    return go_to_lists(LISTS_AT_PLAYING, true);
}
static int go_to_album(void)
{
    return go_to_lists(LISTS_AT_PLAYING, false);
}
/* none for a song the library hasn't, as of a playlist from Finder whose
   file was added since: they did nothing */
static int go_to_playing_callback(int action, const struct menu_item_ex *this_item,
                                  struct gui_synclist *this_list)
{
    (void)this_item; (void)this_list;
    const struct mp3entry *id3 = audio_current_track();
    if (action == ACTION_REQUEST_MENUITEM
        && (!id3 || !id3->tagcache_idx || !tagtree_ready() || !tagcache_is_usable()))
        return ACTION_EXIT_MENUITEM;
    return action;
}
MENUITEM_FUNCTION(go_to_artist_item,
                  MENU_EXITAFTERTHISMENU|MENU_FUNC_OPENS_SCREEN|MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_GO_TO_ARTIST), go_to_artist, go_to_playing_callback,
                  Icon_NOICON);
MENUITEM_FUNCTION(go_to_album_item,
                  MENU_EXITAFTERTHISMENU|MENU_FUNC_OPENS_SCREEN|MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_GO_TO_ALBUM), go_to_album, go_to_playing_callback,
                  Icon_NOICON);

/* Playing From: where the music playing was started, at the song playing: a
   list of Music's, or a playlist, of Playlists, On-The-Go or another. None if
   that isn't known, as for Search, Cover Flow or Shuffle Songs; Up Next is
   there anyway (the owner's decision, 2026-11-09) */
static int playing_from(void)
{
    if (tagtree_playing_from_known())
        return go_to_lists(LISTS_AT_FROM, false);
    return wps_view_songs(lp_view_playing);
}
static int playing_from_callback(int action, const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list)
{
    (void)this_item; (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM && !tagtree_playing_from_known()
        && !lp_playing_from_known())
        return ACTION_EXIT_MENUITEM;
    return action;
}
MENUITEM_FUNCTION(playing_from_item,
                  MENU_EXITAFTERTHISMENU|MENU_FUNC_OPENS_SCREEN|MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_PLAYING_FROM), playing_from, playing_from_callback,
                  Icon_NOICON);

/* Go to Artist and Go to Album on a song of a list, as over Now Playing for
   the song playing, and Go to Artist on an album: in Music's lists in place,
   Menu there back to the row; from Search, a playlist and Current Playlist in
   lists of their own, Menu back to that screen. After Play Last, before a
   playlist's Remove (the owner's decision, 2026-11-09). */
static int song_go_to(bool artist)
{
    if (selected_file.context == CONTEXT_ID3DB)
    {
        /* loaded again even if it wasn't found: others were searched */
        if (tagtree_browse_from_row(artist))
            transition_begin(TRANSITION_FORWARD);
        onplay_result = ONPLAY_RELOAD_DIR;
        return 0;
    }
    return go_to_lists(LISTS_AT_SONG, artist);
}
static int song_go_to_artist(void)
{
    return song_go_to(true);
}
static int song_go_to_album(void)
{
    return song_go_to(false);
}
static int song_go_to_callback(int action, const struct menu_item_ex *this_item,
                               struct gui_synclist *this_list);
MENUITEM_FUNCTION(song_go_to_artist_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_GO_TO_ARTIST), song_go_to_artist, song_go_to_callback,
                  Icon_NOICON);
MENUITEM_FUNCTION(song_go_to_album_item, MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_GO_TO_ALBUM), song_go_to_album, song_go_to_callback,
                  Icon_NOICON);
/* a song of the library, of Music's lists or of Search and the playlists,
   which list those alone; Go to Artist on an album too, and no Go to Album in
   its own list (tagtree_row_goes_to()) */
static int song_go_to_callback(int action, const struct menu_item_ex *this_item,
                               struct gui_synclist *this_list)
{
    (void)this_list;
    if (action != ACTION_REQUEST_MENUITEM)
        return action;
    const bool album = this_item == &song_go_to_album_item;
    switch (selected_file.context)
    {
        case CONTEXT_ID3DB:
            return tagtree_row_goes_to(album) ? action : ACTION_EXIT_MENUITEM;
        case CONTEXT_STD:
            return selected_file.path
                   && (selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO
                   && tagtree_ready() && tagcache_is_usable() ?
                   action : ACTION_EXIT_MENUITEM;
        default:
            return ACTION_EXIT_MENUITEM;
    }
}
#endif

/* Shuffle, repeat and Sound Check, changed right here by selecting them, with the same effect
   on what's playing as changing the settings */
static int cycle_shuffle(void)
{
    const struct settings_list *setting =
        find_setting(&global_settings.playlist_shuffle);

    switch (global_settings.playlist_shuffle)
    {
        case SHUFFLE_OFF:   global_settings.playlist_shuffle = SHUFFLE_SONGS; break;
        case SHUFFLE_SONGS: global_settings.playlist_shuffle = SHUFFLE_ALBUMS; break;
        default:            global_settings.playlist_shuffle = SHUFFLE_OFF; break;
    }
    if (setting)
        setting->choice_setting->option_callback(global_settings.playlist_shuffle);
    settings_save();
    return 0;
}

static char *shuffle_item_name(int selected_item, void *data,
                               char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    int value;
    switch (global_settings.playlist_shuffle)
    {
        case SHUFFLE_SONGS:  value = LANG_SONGS; break;
        case SHUFFLE_ALBUMS: value = LANG_ALBUMS; break;
        default:             value = LANG_OFF; break;
    }
    snprintf(buffer, buffer_len, "%s\t%s", str(LANG_SHUFFLE), str(value));
    return buffer;
}

static int cycle_repeat(void)
{
    const struct settings_list *setting =
        find_setting(&global_settings.repeat_mode);

    switch (global_settings.repeat_mode)
    {
        case REPEAT_OFF: global_settings.repeat_mode = REPEAT_ALL; break;
        case REPEAT_ALL: global_settings.repeat_mode = REPEAT_ONE; break;
        default:         global_settings.repeat_mode = REPEAT_OFF; break;
    }
    if (setting)
        setting->choice_setting->option_callback(global_settings.repeat_mode);
    settings_save();
    return 0;
}

static char *repeat_item_name(int selected_item, void *data,
                              char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    int value;
    switch (global_settings.repeat_mode)
    {
        case REPEAT_OFF:     value = LANG_OFF; break;
        case REPEAT_ALL:     value = LANG_ALL; break;
        case REPEAT_ONE:     value = LANG_REPEAT_ONE; break;
        case REPEAT_SHUFFLE: value = LANG_SHUFFLE; break;
#ifdef AB_REPEAT_ENABLE
        case REPEAT_AB:      value = LANG_REPEAT_AB; break;
#endif
        default:             value = LANG_OFF; break;
    }
    snprintf(buffer, buffer_len, "%s\t%s", str(LANG_REPEAT), str(value));
    return buffer;
}

/* with this flag on an item, the menu with it stays open */
static int toggle_sound_check(void)
{
    const struct settings_list *setting =
        find_setting(&global_settings.replaygain_settings.type);
    int *type = &global_settings.replaygain_settings.type;

    *type = *type == REPLAYGAIN_OFF ? REPLAYGAIN_TRACK : REPLAYGAIN_OFF;
    if (setting)
        setting->choice_setting->option_callback(*type);
    settings_save();
    return 0;
}

static char *sound_check_item_name(int selected_item, void *data,
                                   char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    snprintf(buffer, buffer_len, "%s\t%s", str(LANG_REPLAYGAIN_MODE),
             str(global_settings.replaygain_settings.type == REPLAYGAIN_OFF ?
                 LANG_OFF : LANG_ON));
    return buffer;
}

static int wps_set_volume_limit(void)
{
    volume_limit_screen();
    setvol();
    return 0;
}

/* by a shorter name for the narrow menu: its screen has the whole one */
static char *volume_limit_item_name(int selected_item, void *data,
                                    char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    char value[16];
    snprintf(buffer, buffer_len, "%s\t%s", str(LANG_VOLUME_LIMIT),
             format_volume_step(value, sizeof(value), global_settings.volume_limit));
    return buffer;
}

MENUITEM_FUNCTION_DYNTEXT(wps_volume_limit_item,
                          MENU_EXITAFTERTHISMENU|MENU_FUNC_OPENS_SCREEN,
                          wps_set_volume_limit, volume_limit_item_name,
                          NULL, NULL, NULL, Icon_NOICON);

static int toggle_dark_theme(void)
{
    dark_theme_toggle();
    return 0;
}

static char *dark_theme_item_name(int selected_item, void *data,
                                  char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    snprintf(buffer, buffer_len, "%s\t%s", str(LANG_DARK_THEME),
             str(global_settings.dark_theme ? LANG_ON : LANG_OFF));
    return buffer;
}

MENUITEM_FUNCTION_DYNTEXT(wps_dark_theme_item, MENU_EXITAFTERTHISMENU,
                          toggle_dark_theme, dark_theme_item_name,
                          NULL, NULL, NULL, Icon_NOICON);

static int toggle_night_mode(void)
{
    night_mode_toggle();
    return 0;
}

static char *night_mode_item_name(int selected_item, void *data,
                                  char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    snprintf(buffer, buffer_len, "%s\t%s", str(LANG_NIGHT_MODE),
             str(global_settings.night_mode ? LANG_ON : LANG_OFF));
    return buffer;
}

MENUITEM_FUNCTION_DYNTEXT(wps_night_mode_item, MENU_EXITAFTERTHISMENU,
                          toggle_night_mode, night_mode_item_name,
                          NULL, NULL, NULL, Icon_NOICON);

#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
static int cycle_cover_colours(void)
{
    const struct settings_list *setting =
        find_setting(&global_settings.cover_colours);
    int *mode = &global_settings.cover_colours;

    *mode = *mode == COVER_COLOURS_FILL ? COVER_COLOURS_OFF : *mode + 1;
    if (setting)
        setting->choice_setting->option_callback(*mode);
    settings_save();
    return 0;
}

static char *cover_colours_item_name(int selected_item, void *data,
                                     char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    int value = LANG_OFF;
    if (global_settings.cover_colours == COVER_COLOURS_TEXT)
        value = LANG_COVER_TEXT;
    else if (global_settings.cover_colours == COVER_COLOURS_SCREEN)
        value = LANG_COVER_SCREEN;
    else if (global_settings.cover_colours == COVER_COLOURS_FILL)
        value = LANG_COVER_FILL;
    snprintf(buffer, buffer_len, "%s\t%s", str(LANG_COVER_COLOURS), str(value));
    return buffer;
}

MENUITEM_FUNCTION_DYNTEXT(wps_cover_colours_item, MENU_EXITAFTERTHISMENU,
                          cycle_cover_colours, cover_colours_item_name,
                          NULL, NULL, NULL, Icon_NOICON);
#endif

#ifdef HAVE_BACKLIGHT_BRIGHTNESS
static int wps_set_brightness(void)
{
    do_setting_from_menu(&brightness_item, NULL);
    return 0;
}

static char *brightness_item_name(int selected_item, void *data,
                                  char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    snprintf(buffer, buffer_len, "%s\t%d", str(LANG_BRIGHTNESS),
             global_settings.brightness);
    return buffer;
}

MENUITEM_FUNCTION_DYNTEXT(wps_brightness_item,
                          MENU_EXITAFTERTHISMENU|MENU_FUNC_OPENS_SCREEN,
                          wps_set_brightness, brightness_item_name,
                          NULL, NULL, NULL, Icon_NOICON);
#endif

MENUITEM_FUNCTION_DYNTEXT(wps_shuffle_item, MENU_EXITAFTERTHISMENU, cycle_shuffle,
                          shuffle_item_name, NULL, NULL, NULL, Icon_NOICON);
MENUITEM_FUNCTION_DYNTEXT(wps_repeat_item, MENU_EXITAFTERTHISMENU, cycle_repeat,
                          repeat_item_name, NULL, NULL, NULL, Icon_NOICON);
MENUITEM_FUNCTION_DYNTEXT(wps_sound_check_item, MENU_EXITAFTERTHISMENU,
                          toggle_sound_check, sound_check_item_name,
                          NULL, NULL, NULL, Icon_NOICON);

MENUITEM_GROUP(wps_group_playback, ID2P(LANG_PLAYBACK));
MENUITEM_GROUP(wps_group_settings, ID2P(LANG_SETTINGS));

/* Chapters, of a track playing that has them: their list */
static int chapters_callback(int action, const struct menu_item_ex *this_item,
                             struct gui_synclist *this_list)
{
    (void)this_item; (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM
        && !chapters_count(audio_current_track()))
        return ACTION_EXIT_MENUITEM;
    return action;
}
MENUITEM_FUNCTION(chapters_item,
                  MENU_EXITAFTERTHISMENU|MENU_FUNC_OPENS_SCREEN|MENU_FUNC_CHECK_RETVAL,
                  ID2P(LANG_CHAPTERS), chapters_screen, chapters_callback,
                  Icon_NOICON);

/* used when onplay() is called in the CONTEXT_WPS context */
MAKE_ONPLAYMENU( wps_onplay_menu, ID2P(LANG_NOW_PLAYING),
           onplaymenu_callback, Icon_Audio,
           /* no rating, bookmarks or cuesheet: the rating is a view of the
              WPS, as on a stock iPod; the configurable items are off by
              default */
#ifdef HAVE_TAGCACHE
           &cover_flow_item, &go_to_artist_item, &go_to_album_item,
#endif
           &chapters_item,
           &wps_group_playback,
           &wps_shuffle_item, &wps_repeat_item,
           /* On-The-Go, as on a stock iPod, rather than a playlist picked or
              named with the keyboard */
#ifdef HAVE_TAGCACHE
           &playing_from_item,
#endif
           &wps_view_cur_playlist_item,
           &on_the_go_item,
#ifdef HAVE_TAGCACHE
           &add_to_playlist_item,
#endif
           &wps_group_settings,
           /* the two that change with the headphones first */
           &wps_volume_limit_item, &ipod_eq_menu, &wps_sound_check_item,
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
           &wps_brightness_item,
#endif
#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
           &wps_cover_colours_item,
#endif
           &wps_dark_theme_item,
           &wps_night_mode_item,
#ifndef HAVE_HOTKEY
           &context_item_0,
#endif
           &context_item_1,
           &context_item_2,
           &context_item_3,
           &context_item_4,
         );

/* Settings > Now Playing Menu: each row of that menu shown or not, by its
   setting; Rockbox's configurable items after them go by their own */
static const struct {
    const struct menu_item_ex *item;
    int row;
} wps_rows[] = {
#ifdef HAVE_TAGCACHE
    { &cover_flow_item, WPS_MENU_COVER_FLOW },
    { &go_to_artist_item, WPS_MENU_GO_TO_ARTIST },
    { &go_to_album_item, WPS_MENU_GO_TO_ALBUM },
#endif
    { &wps_shuffle_item, WPS_MENU_SHUFFLE },
    { &wps_repeat_item, WPS_MENU_REPEAT },
    /* Up Next; Playing From, when it's known, has no row of its own */
    { &wps_view_cur_playlist_item, WPS_MENU_CURRENT_PLAYLIST },
    { &on_the_go_item, WPS_MENU_ON_THE_GO },
#ifdef HAVE_TAGCACHE
    /* with Add to On-The-Go, which the setting is of */
    { &add_to_playlist_item, WPS_MENU_ON_THE_GO },
#endif
    { &wps_volume_limit_item, WPS_MENU_VOLUME_LIMIT },
    { &ipod_eq_menu, WPS_MENU_EQ },
    { &wps_sound_check_item, WPS_MENU_SOUND_CHECK },
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    { &wps_brightness_item, WPS_MENU_BRIGHTNESS },
#endif
#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
    { &wps_cover_colours_item, WPS_MENU_COVER_COLOURS },
#endif
    { &wps_dark_theme_item, WPS_MENU_DARK_THEME },
    { &wps_night_mode_item, WPS_MENU_NIGHT_MODE },
};

/* -1 for one that isn't of them */
static int wps_row(const struct menu_item_ex *item)
{
    for (unsigned i = 0; i < ARRAYLEN(wps_rows); i++)
    {
        if (wps_rows[i].item == item)
            return global_settings.wps_menu_show[wps_rows[i].row];
    }
    return -1;
}

/* a group's bar with none of its rows left goes in init_menu_lists */
bool onplay_row_hidden(const struct menu_item_ex *menu, int i)
{
    return menu == &wps_onplay_menu && wps_row(menu->submenus[i]) == 0;
}

/* none of its rows on: held Select does nothing there, rather than open an
   empty menu */
bool onplay_wps_menu_empty(void)
{
    /* Rockbox's configurable items, set in config.cfg, after them: the
       menu's, as the first is the hotkey's with one */
#ifdef HAVE_HOTKEY
    for (int item = 1; item < 5; item++)
#else
    for (int item = 0; item < 5; item++)
#endif
    {
        if (HK_CTX_GET(item, global_settings.context_wps) != HOTKEY_OFF)
            return false;
    }
    /* Chapters has no setting: there with a track that has them */
    if (chapters_count(audio_current_track()))
        return false;
    for (unsigned i = 0; i < ARRAYLEN(wps_rows); i++)
    {
        if (global_settings.wps_menu_show[wps_rows[i].row])
            return false;
    }
    return true;
}


MENUITEM_FUNCTION(view_playlist_item, 0, ID2P(LANG_VIEW),
                  view_playlist,
                  onplaymenu_callback, Icon_Playlist);

#ifdef HAVE_TAGCACHE
/* A playlist made on the iPod, held in the catalogue: Rename, Move to Folder
   and Delete, which the import's haven't, as the next sync makes them again */
static int ipod_playlist_callback(int action,
                                  const struct menu_item_ex *this_item,
                                  struct gui_synclist *this_list)
{
    (void)this_item; (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM
        && !(selected_file.context == CONTEXT_TREE
             && (selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_M3U
             && ip_editable(selected_file.path)))
        return ACTION_EXIT_MENUITEM;
    return action;
}

static void ipod_playlist_done(int r, const char *path)
{
    if (r < 0)
        onplay_result = ONPLAY_MAINMENU;
    else
    {
        if (r > 0 && path)
            tree_select_after_reload(path);
        onplay_result = ONPLAY_RELOAD_DIR;
    }
}

static int ipod_playlist_rename(void)
{
    char done[MAX_PATH];
    const int r = ip_rename(selected_file.path, done, sizeof(done));
    ipod_playlist_done(r, done);
    return 0;
}

static bool folders_only(char *name, int attr, struct tree_context *tc)
{
    (void)name; (void)tc;
    return attr & ATTR_DIRECTORY;
}

static int ipod_playlist_move(void)
{
    char root[MAX_PATH], folder[MAX_PATH], done[MAX_PATH];
    if (!catalog_directory_name(root, sizeof(root)))
        return 0;
    folder[0] = '\0';
    struct browse_context browse = {
        .dirfilter = SHOW_M3U,
        .flags = BROWSE_SELECTONLY | BROWSE_NO_CONTEXT_MENU | BROWSE_PICK_FOLDER,
        .callback_show_item = folders_only,
        .title = str(LANG_PLAYLISTS),
        .icon = Icon_Playlist,
        .root = root,
        .buf = folder,
        .bufsize = sizeof(folder),
    };
    if (rockbox_browse(&browse) == GO_TO_ROOT)
    {
        onplay_result = ONPLAY_MAINMENU;
        return 0;
    }
    int r = 0;
    if (browse.flags & BROWSE_SELECTED)
    {
        /* the folder without the separator at its end the browser gives */
        size_t len = strlen(folder);
        if (len > 1 && folder[len - 1] == '/')
            folder[len - 1] = '\0';
        r = ip_move(selected_file.path, folder, done, sizeof(done));
    }
    ipod_playlist_done(r, NULL);
    return 0;
}

static int ipod_playlist_delete(void)
{
    char title[MAX_PATH];
    const char *base = strrchr(selected_file.path, '/');
    strmemccpy(title, base ? base + 1 : selected_file.path, sizeof(title));
    char *ext = strrchr(title, '.');
    if (ext && ext != title)
        *ext = '\0';
    const int r = ip_delete(selected_file.path, title);
    ipod_playlist_done(r, NULL);
    return 0;
}

/* a song of a playlist made on the iPod, as its screen asks */
static bool song_removable;
void onplay_set_removable(bool removable)
{
    song_removable = removable;
}

static int remove_song_callback(int action, const struct menu_item_ex *this_item,
                                struct gui_synclist *this_list)
{
    (void)this_item; (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM && !song_removable)
        return ACTION_EXIT_MENUITEM;
    return action;
}

static int remove_song(void)
{
    onplay_result = ONPLAY_REMOVE;
    return 0;
}

MENUITEM_FUNCTION(remove_song_item, 0, ID2P(LANG_REMOVE),
                  remove_song, remove_song_callback, Icon_NOICON);

MENUITEM_FUNCTION(ipod_rename_item, 0, ID2P(LANG_RENAME),
                  ipod_playlist_rename, ipod_playlist_callback, Icon_NOICON);
MENUITEM_FUNCTION(ipod_move_item, 0, ID2P(LANG_MOVE_TO_FOLDER),
                  ipod_playlist_move, ipod_playlist_callback, Icon_NOICON);
MENUITEM_FUNCTION(ipod_delete_item, 0, ID2P(LANG_DELETE),
                  ipod_playlist_delete, ipod_playlist_callback, Icon_NOICON);
#endif

#ifdef HAVE_TAGCACHE
/* an episode's own rows: Mark as Played, Mark as Unplayed (Finder's), Start
   from Beginning on one begun; the one playing has the last alone */
static int episode_mark(bool played)
{
    if (tagtree_row_mark(played))
        onplay_result = ONPLAY_RELOAD_DIR;
    return 0;
}
static int episode_mark_played(void)
{
    return episode_mark(true);
}
static int episode_mark_unplayed(void)
{
    return episode_mark(false);
}
static int episode_start_over(void)
{
    onplay_result = tagtree_row_start_over() ? ONPLAY_START_PLAY
                                             : ONPLAY_RELOAD_DIR;
    return 0;
}
/* A book's part in Audiobooks: Start Over for a book begun, Mark as Finished
   for one not heard to its end, unless it plays */
static int book_start_over(void)
{
    onplay_result = tagtree_book_start_over() ? ONPLAY_START_PLAY
                                              : ONPLAY_RELOAD_DIR;
    return 0;
}
static int book_finish(void)
{
    if (tagtree_book_finish())
        onplay_result = ONPLAY_RELOAD_DIR;
    return 0;
}
static int book_callback(int action, const struct menu_item_ex *this_item,
                         struct gui_synclist *this_list);
MENUITEM_FUNCTION(book_start_over_item, 0, ID2P(LANG_START_OVER),
                  book_start_over, book_callback, Icon_NOICON);
MENUITEM_FUNCTION(book_finish_item, 0, ID2P(LANG_MARK_AS_FINISHED),
                  book_finish, book_callback, Icon_NOICON);
static int book_callback(int action, const struct menu_item_ex *this_item,
                         struct gui_synclist *this_list)
{
    (void)this_list;
    if (action != ACTION_REQUEST_MENUITEM)
        return action;
    bool playing = false;
    const int state = selected_file.context == CONTEXT_ID3DB ?
                      tagtree_row_book(&playing) : -1;
    const bool shown = this_item == &book_start_over_item ? state == 1
                       : state >= 0 && state != 2 && !playing;
    return shown ? action : ACTION_EXIT_MENUITEM;
}
static int episode_callback(int action, const struct menu_item_ex *this_item,
                            struct gui_synclist *this_list);
MENUITEM_FUNCTION(episode_start_over_item, 0, ID2P(LANG_START_FROM_BEGINNING),
                  episode_start_over, episode_callback, Icon_NOICON);
MENUITEM_FUNCTION(episode_played_item, 0, ID2P(LANG_MARK_AS_PLAYED),
                  episode_mark_played, episode_callback, Icon_NOICON);
MENUITEM_FUNCTION(episode_unplayed_item, 0, ID2P(LANG_MARK_AS_UNPLAYED),
                  episode_mark_unplayed, episode_callback, Icon_NOICON);
static int episode_callback(int action, const struct menu_item_ex *this_item,
                            struct gui_synclist *this_list)
{
    (void)this_list;
    if (action != ACTION_REQUEST_MENUITEM)
        return action;
    bool marked = false, playing = false;
    const int state = selected_file.context == CONTEXT_ID3DB ?
                      tagtree_row_episode(&marked, &playing) : -1;
    bool shown;
    if (this_item == &episode_start_over_item)
        shown = state == EPISODE_STARTED;
    else if (this_item == &episode_played_item)
        shown = state >= 0 && state != EPISODE_PLAYED && !playing;
    else
        shown = state >= 0 && state != EPISODE_NEW && marked && !playing;
    return shown ? action : ACTION_EXIT_MENUITEM;
}
#endif

/* used when onplay() is not called in the CONTEXT_WPS context: for a song, an album
   or the like, or a playlist, what plays it, at once rather than in a submenu. Its
   title is the name the list shows for it. */
static char tree_menu_title[MAX_PATH];
void onplay_set_title(const char *title)
{
    strmemccpy(tree_menu_title, title, sizeof(tree_menu_title));
}

MAKE_ONPLAYMENU( tree_onplay_menu, tree_menu_title,
           onplaymenu_callback, Icon_file_view_menu,
           /* no Add to Playlist, as on a stock iPod: it picked any playlist,
              those of the library too, which the next sync writes again, and
              named a new one by its path with the keyboard; On-The-Go goes to
              the Mac instead */
#ifdef HAVE_TAGCACHE
           &episode_start_over_item, &book_start_over_item,
#endif
           &on_the_go_item,
#ifdef HAVE_TAGCACHE
           &add_to_playlist_item,
#endif
           &i_first_pl_item, &i_last_pl_item,
#ifdef HAVE_TAGCACHE
           &episode_played_item, &episode_unplayed_item, &book_finish_item,
           &song_go_to_artist_item, &song_go_to_album_item,
           &remove_song_item,
           &ipod_rename_item, &ipod_move_item, &ipod_delete_item,
#endif
           &rename_file_item, &clipboard_cut_item, &clipboard_copy_item,
           &clipboard_paste_item, &delete_file_item, &delete_dir_item,
           &list_viewers_item, &create_dir_item, &properties_item, &track_info_item,
           &reveal_item,
#ifdef HAVE_TAGCACHE
           &pictureflow_item,
#endif
#if LCD_DEPTH > 1
           &set_backdrop_item,
#endif
           &add_to_faves_item, &set_as_dir_menu,
         );

static int onplaymenu_callback(int action,
                               const struct menu_item_ex *this_item,
                               struct gui_synclist *this_list)
{
    (void)this_list;

    switch (action)
    {
        case ACTION_TREE_STOP:
            /* back to the track playing, as holding Play goes there elsewhere */
            if (this_item == &wps_onplay_menu)
                return ACTION_STD_CANCEL;
            break;
        case ACTION_REQUEST_MENUITEM:
            if (this_item == &view_playlist_item)
            {
                if ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_M3U &&
                        selected_file.context == CONTEXT_TREE)
                    return action;
            }
            return ACTION_EXIT_MENUITEM;
            break;
        case ACTION_EXIT_MENUITEM:
            return ACTION_EXIT_AFTER_THIS_MENUITEM;
            break;
        default:
            break;
    }
    return action;
}

/* direct function calls, no need for menu callbacks */
static bool hotkey_delete_item(void)
{
#ifdef HAVE_MULTIVOLUME
    /* no delete for volumes */
    if (selected_file.attr & ATTR_VOLUME)
        return false;
#endif

#ifdef HAVE_TAGCACHE
    if (selected_file.context == CONTEXT_ID3DB &&
        (selected_file.attr & FILE_ATTR_MASK) != FILE_ATTR_AUDIO)
        return false;

     if (!prepare_database_sel(NULL))
        return false;
#endif

    return clipboard_delete_selected_fileobject();
}

static bool hotkey_open_with(void)
{
    /* only open files */
    if (selected_file.attr & ATTR_DIRECTORY)
        return false;
#ifdef HAVE_MULTIVOLUME
    if (selected_file.attr & ATTR_VOLUME)
        return false;
#endif
#ifdef HAVE_TAGCACHE
    if (!prepare_database_sel(NULL))
        return false;
#endif
    return list_viewers();
}

static int hotkey_tree_pl_insert_shuffled(void)
{
    if ((audio_status() & AUDIO_STATUS_PLAY) ||
        (selected_file.attr & ATTR_DIRECTORY) ||
        ((selected_file.attr & FILE_ATTR_MASK) == FILE_ATTR_M3U))
    {
        add_to_playlist(&addtopl_insert_shuf);
    }
    return ONPLAY_RELOAD_DIR;
}

static int hotkey_tree_run_plugin(void *param)
{
#ifdef HAVE_TAGCACHE
    if (!prepare_database_sel(param))
        return ONPLAY_RELOAD_DIR;
#endif
    if (filetype_load_plugin((const char*)param, selected_file.path) == PLUGIN_GOTO_WPS)
        return ONPLAY_START_PLAY;

    return ONPLAY_RELOAD_DIR;
}

static int hotkey_wps_run_plugin(void)
{
    if (get_current_activity() == ACTIVITY_CONTEXTMENU)
        open_plugin_run(ID2P(LANG_ONPLAY_MENU_TITLE));
#ifdef HAVE_HOTKEY
    else
        open_plugin_run(ID2P(LANG_HOTKEY_WPS));
#endif
    return ONPLAY_OK;
}

static int hotkey_execute_menu(void)
{
    intptr_t flag = HOTKEY_FLAG_WPS;
    if (selected_file.context != CONTEXT_WPS)
        flag = HOTKEY_FLAG_TREE;

    return hotkey_run_menu(flag, true, 0);
}

#define HOTKEY_FUNC(func, param) {{(void *)func}, param}

/* Any desired hotkey functions go here, in the enum in onplay.h,
   and in the settings menu in settings_list.c.  The order here
   is not important. */

static const struct hotkey_assignment hotkey_items[] = {
 [0]{ .action = HOTKEY_OFF,
      .lang_id = LANG_OFF,
      .func = HOTKEY_FUNC(NULL,NULL),
      .return_code = ONPLAY_RELOAD_DIR,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_TREE,
      .icon=Icon_NOICON},
   { .action = HOTKEY_VIEW_PLAYLIST,
      .lang_id = LANG_VIEW_DYNAMIC_PLAYLIST,
      .func = HOTKEY_FUNC(NULL, NULL),
      .return_code = ONPLAY_PLAYLIST,
      .flags = HOTKEY_FLAG_WPS,
      .icon = Icon_Playlist},
   { .action = HOTKEY_SHOW_TRACK_INFO,
      .lang_id = LANG_MENU_SHOW_ID3_INFO,
      .func = HOTKEY_FUNC(browse_id3_wrapper, NULL),
      .return_code = ONPLAY_RELOAD_DIR,
      .flags = HOTKEY_FLAG_WPS,
      .icon = Icon_Questionmark},
#ifdef HAVE_PITCHCONTROL
    { .action = HOTKEY_PITCHSCREEN,
      .lang_id = LANG_PITCH,
      .func = HOTKEY_FUNC(gui_syncpitchscreen_run, NULL),
      .return_code = ONPLAY_RELOAD_DIR,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_NOSBS,
      .icon = Icon_Audio},
#endif
    { .action = HOTKEY_OPEN_WITH,
      .lang_id = LANG_ONPLAY_OPEN_WITH,
      .func = HOTKEY_FUNC(hotkey_open_with, NULL),
      .return_code = ONPLAY_RELOAD_DIR,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_TREE,
      .icon = Icon_Plugin},
   { .action = HOTKEY_DELETE,
      .lang_id = LANG_DELETE,
      .func = HOTKEY_FUNC(hotkey_delete_item, NULL),
      .return_code = ONPLAY_RELOAD_DIR,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_TREE,
      .icon = Icon_NOICON},
   { .action = HOTKEY_INSERT,
      .lang_id = LANG_ADD,
      .func = HOTKEY_FUNC(add_to_playlist, (intptr_t*)&addtopl_insert),
      .return_code = ONPLAY_RELOAD_DIR,
      .flags = HOTKEY_FLAG_TREE,
      .icon = Icon_Queued},
   { .action = HOTKEY_INSERT_SHUFFLED,
      .lang_id = LANG_ADD_SHUFFLED,
      .func = HOTKEY_FUNC(hotkey_tree_pl_insert_shuffled, NULL),
      .return_code = ONPLAY_FUNC_RETURN,
      .flags = HOTKEY_FLAG_TREE,
      .icon = Icon_Queued},
   { .action = HOTKEY_PLUGIN,
      .lang_id = LANG_OPEN_PLUGIN,
      .func = HOTKEY_FUNC(hotkey_wps_run_plugin, NULL),
      .return_code = ONPLAY_FUNC_RETURN,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_NOSBS,
      .icon = Icon_Plugin},
   { .action = HOTKEY_BOOKMARK,
      .lang_id = LANG_BOOKMARK_MENU_CREATE,
      .func = HOTKEY_FUNC(bookmark_create_menu, NULL),
      .return_code = ONPLAY_OK,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_NOSBS,
      .icon = Icon_Bookmark},
   { .action = HOTKEY_BOOKMARK_LIST,
      .lang_id = LANG_BOOKMARK_MENU_LIST,
      .func = HOTKEY_FUNC(bookmark_load_menu, NULL),
      .return_code = ONPLAY_START_PLAY,
      .flags = HOTKEY_FLAG_WPS,
      .icon = Icon_Bookmark},
   { .action = HOTKEY_PROPERTIES,
      .lang_id = LANG_PROPERTIES,
      .func = HOTKEY_FUNC(hotkey_tree_run_plugin, (void *)"properties"),
      .return_code = ONPLAY_FUNC_RETURN,
      .flags = HOTKEY_FLAG_TREE,
      .icon = Icon_NOICON},
   { .action = HOTKEY_SHOW_IN_FILES,
      .lang_id = LANG_SHOW_IN_FILES,
      .func = HOTKEY_FUNC(reveal, NULL),
      .return_code = ONPLAY_REVEAL_FILE,
      .flags = HOTKEY_FLAG_WPS,
      .icon = Icon_file_view_menu},
   { .action = HOTKEY_CONTEXT_MENU,
      .lang_id = LANG_ONPLAY_MENU_TITLE,
      .func = HOTKEY_FUNC(hotkey_execute_menu, NULL),
      .return_code = ONPLAY_FUNC_RETURN,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_TREE,
      .icon = Icon_Submenu},
#ifdef HAVE_ALBUMART
    { .action = HOTKEY_ALBUMART,
      .lang_id = LANG_VIEW_ALBUMART,
      .func = HOTKEY_FUNC(view_album_art, NULL),
      .return_code = ONPLAY_OK,
      .flags = HOTKEY_FLAG_WPS | HOTKEY_FLAG_NOSBS,
      .icon = Icon_Wps},
#endif
#ifdef HAVE_TAGCACHE
    { .action = HOTKEY_PICTUREFLOW,
      .lang_id = LANG_ONPLAY_PICTUREFLOW,
      .func = HOTKEY_FUNC(hotkey_tree_run_plugin, (void *)"pictureflow"),
      .return_code = ONPLAY_FUNC_RETURN,
      .flags = HOTKEY_FLAG_TREE,
      .icon = Icon_Plugin},
#endif
};

const struct hotkey_assignment *get_hotkey(int action)
{
    for (size_t i = ARRAYLEN(hotkey_items) - 1; i < ARRAYLEN(hotkey_items); i--)
    {
        if (hotkey_items[i].action == action)
            return &hotkey_items[i];
    }
    return &hotkey_items[0]; /* no valid hotkey set, return HOTKEY_OFF*/
}

/* Execute the hotkey function, if listed */
static int execute_hotkey(int action)
{
    /* search assignment struct for a match for the hotkey setting */
    const struct hotkey_assignment *this_item = get_hotkey(action);

    /* run the associated function (with optional param), if any */
    const struct menu_func_param func = this_item->func;

    int func_return = ONPLAY_RELOAD_DIR;
    if (func.function != NULL)
    {
        if (func.param != NULL)
        {
            func_return = (*func.function_w_param)(func.param);
        }
        else
        {
            func_return = (*func.function)();
        }
    }
    const int return_code = this_item->return_code;

    if (return_code == ONPLAY_FUNC_RETURN)
        return func_return;  /* Use value returned by function */
    return return_code;      /* or return the associated value */
}

static const char* hotkey_get_name(int selected_item, void * data,
                                     char * buffer, size_t buffer_len)
{
    (void)buffer; (void)buffer_len;
    const struct hotkey_assignment **hk_menu =
                (const struct hotkey_assignment**)data;

    if (hk_menu[selected_item]->action == HOTKEY_PLUGIN)
    {
        return onplay_get_plugin_name(false);
    }

    return ID2P(hk_menu[selected_item]->lang_id);
}

static int hotkey_get_talk(int selected_item, void * data)
{
    const struct hotkey_assignment **hk_menu =
                (const struct hotkey_assignment**)data;
    if (hk_menu[selected_item]->action == HOTKEY_PLUGIN)
        talk_spell(onplay_get_plugin_name(false), false);
    else
        talk_id(hk_menu[selected_item]->lang_id, false);
    return 0;
}

static enum themable_icons hotkey_get_icon(int selected_item, void * data)
{
    const struct hotkey_assignment **hk_menu =
                (const struct hotkey_assignment**)data;
    return hk_menu[selected_item]->icon;
}

int hotkey_run_menu(intptr_t flag, bool execute, int current_action)
{
    const struct hotkey_assignment *hk_menu[ARRAYLEN(hotkey_items)];

    char *title = str(LANG_ONPLAY_MENU_TITLE);
#ifdef HAVE_HOTKEY
    if (flag & HOTKEY_FLAG_TREE)
            title = str(LANG_HOTKEY_FILE_BROWSER);
#endif
    struct simplelist_info info;
    int selected = 0;
    int count = 0;
    for (size_t i = 0; i < ARRAYLEN(hotkey_items); i++)
    {
        hk_menu[i] = NULL; /*clear all the hk_menu entries prior to setting them */
        if (hotkey_items[i].action == HOTKEY_OFF && execute)
            continue; /* Don't display HOTKEY_OFF item */
        if ((hotkey_items[i].flags & flag) == flag)
        {
            if (!execute || hotkey_items[i].action != HOTKEY_CONTEXT_MENU)
            {
                if (hotkey_items[i].action == current_action)
                    selected = count;
                hk_menu[count++] = &hotkey_items[i];
            }
        }
    }

    simplelist_info_init(&info, title, count, (void*)&hk_menu);
    info.get_name = hotkey_get_name;
    info.get_icon = hotkey_get_icon;
    info.get_talk = hotkey_get_talk;
    info.selection = selected;
    simplelist_show_list(&info);

    if (execute)
    {
        if (info.selection < 0) /* canceled */
            return ONPLAY_RELOAD_DIR;
        return execute_hotkey(hk_menu[info.selection]->action);
    }
    else
    {
        if (info.selection < 0) /* canceled */
            return -1;
        return hk_menu[info.selection]->action;
    }
}

int onplay(char* file, int attr, int from_context, bool hotkey, int customaction)
{
    const struct menu_item_ex *menu;
    onplay_result = ONPLAY_OK;
    ctx_current_playlist_insert = NULL;
    selected_file_set(from_context, NULL, attr);

#ifdef HAVE_TAGCACHE
    if (from_context == CONTEXT_ID3DB)
    {
        ctx_add_to_playlist = tagtree_add_to_playlist;
        if (file != NULL)
        {
            /* add a leading slash so that catalog_add_to_a_playlist
               later prefills the name when creating a new playlist */
            snprintf(selected_file.buf, MAX_PATH, "/%s", file);
            selected_file.path = selected_file.buf;
        }
    }
   else
#endif
    {
        ctx_add_to_playlist = NULL;
        if (file != NULL)
        {
            strmemccpy(selected_file.buf, file, MAX_PATH);
            selected_file.path = selected_file.buf;
        }

    }
    int menu_selection;

#ifdef HAVE_HOTKEY
    if (hotkey)
    {
        if (from_context == CONTEXT_WPS)
        {
            wps_context_item_cb(ACTION_EXIT_MENUITEM, &context_item_0, NULL);
            return onplay_result;
        }
        else
        {
            return execute_hotkey(global_settings.hotkey_tree & HK_CTX_MASK);
        }
    }
#else
    (void)hotkey;
#endif
    if (customaction == ONPLAY_CUSTOMACTION_SHUFFLE_SONGS)
    {
        int returnCode = add_to_playlist(&addtopl_replace_shuffled);
        if (returnCode == 1)
            // User did not want to erase his current playlist, so let's show again the database main menu
            return ONPLAY_RELOAD_DIR;
        return ONPLAY_START_PLAY;
    }
    if (from_context == CONTEXT_WPS && onplay_wps_menu_empty())
        return onplay_result;

    push_current_activity(ACTIVITY_CONTEXTMENU);
    /* over Now Playing, the whole width, with room for the rows' values */
    const bool was_wide = menu_wide();
    if (from_context == CONTEXT_WPS)
    {
        menu = &wps_onplay_menu;
        menu_set_wide(true);
    }
    else
    {
        menu = &tree_onplay_menu;
        if (!tree_menu_title[0])
            onplay_set_title(str(LANG_ONPLAY_MENU_TITLE));
    }
    /* from Now Playing it began there, before Now Playing left its screen,
       so that the menu slides over it: held longer, for the theme drawn twice
       since, for the menu's activity and its width */
    if (from_context != CONTEXT_WPS)
        transition_begin(TRANSITION_FORWARD);
    else
        transition_wait(HZ/2);
    menu_selection = do_menu(menu, NULL, NULL, false);
    tree_menu_title[0] = '\0';
#ifdef HAVE_TAGCACHE
    song_removable = false;
#endif
    if (from_context == CONTEXT_WPS)
        menu_set_wide(was_wide);
    pop_current_activity();

    /* the latter: Play held, over a list; Now Playing stays where it is anyway */
    if (menu_selection == GO_TO_WPS || menu_selection == GO_TO_PREVIOUS_MUSIC)
        return ONPLAY_START_PLAY;
    if (menu_selection == GO_TO_ROOT)
        return ONPLAY_MAINMENU;
    if (menu_selection == GO_TO_MAINMENU)
        return ONPLAY_MAINMENU;
    if (menu_selection == GO_TO_PLAYLIST_VIEWER)
        return ONPLAY_PLAYLIST;
    if (menu_selection == GO_TO_PLUGIN)
        return ONPLAY_PLUGIN;

    return onplay_result;
}

int get_onplay_context(void)
{
    return selected_file.context;
}

static int hotkey_menu_do_setting(void *param, int *setting, int flag)
{
    int current = *setting;
    int item = (intptr_t)param;

    int temp =  HK_CTX_GET(item, current);
    int sel = hotkey_run_menu(flag, false, temp);
    if (sel >=0)
    {
        if (sel == HOTKEY_PLUGIN)
        {
            if (get_current_activity() != ACTIVITY_QUICKSCREEN)
            {
#ifdef HAVE_HOTKEY
                if (item == 0)
                    open_plugin_browse(ID2P(LANG_HOTKEY_WPS));
                else
#endif
                {
                    open_plugin_browse(ID2P(LANG_ONPLAY_MENU_TITLE));
                    onplay_get_plugin_name(true);
                }
            }
        }

        current &= ~HK_CTX_SET(item, HK_CTX_MASK);/*clear*/
        current |= HK_CTX_SET(item, sel);

        /* check for duplicates */
#ifdef HAVE_HOTKEY
        if (item > 0)
            for (int i = 1; i < HK_CTX_ITEMS; i++)
#else
        for (int i = 0; i < HK_CTX_ITEMS; i++)
#endif
        {
            if (i != item)
            {
                if (HK_CTX_GET(i, *setting) == sel)
                {
                    current &= ~HK_CTX_SET(i, HK_CTX_MASK);/*clear*/
                }
            }
        }
        *setting = current;
    }
    return sel;
}

int wps_context_menu_do_setting(void *param)
{
    return hotkey_menu_do_setting(param, &global_settings.context_wps, HOTKEY_FLAG_WPS);

}
#ifdef HAVE_HOTKEY
int tree_context_menu_do_setting(void *param)
{
    return hotkey_menu_do_setting(param, &global_settings.hotkey_tree, HOTKEY_FLAG_TREE);
}
#endif

void wps_context_menu_load_from_cfg(void* setting, char *value)
{
    int item = 0;
    int var = 0;
    char *st = value;
    char *end = value;
    while (*end != '\0' && item < HK_CTX_ITEMS)
    {
        end++;
        if (*end == ',' || *end == '\0')
        {
            st = skip_whitespace(st);
            if (end - st  > 1)
            {
                for (size_t i = ARRAYLEN(hotkey_items) - 1; i < ARRAYLEN(hotkey_items); i--)
                {
                    const char *this = lang_id_to_english(hotkey_items[i].lang_id);
                    if (strncasecmp(st, this, end-st) == 0)
                    {
                        var |= HK_CTX_SET(item, hotkey_items[i].action);
                    }
                }
            }
            st = end + 1;
            item++;
        }
    }
    *(int*)setting = var;
}

char* wps_context_menu_write_to_cfg(void* setting, char*buf, int buf_len)
{
    int var = *(int*)setting;
    int items = HK_CTX_ITEMS;

#ifdef HAVE_HOTKEY
    if (setting == &global_settings.hotkey_tree)
        items = 1;
#endif

    unsigned int written;
    char *buffer = buf;
    for (int i = 0; i < items && buf_len > 0; i++)
    {
        written = snprintf(buffer, buf_len, "%s, ",
                    lang_id_to_english(get_hotkey(HK_CTX_GET(i, var))->lang_id));
        buf_len -= written;
        buffer += written;
    }
    return buf;
}

void wps_context_menu_set_default(void* setting, void* defaultval)
{
    *(int*)setting = *(int*)defaultval;
}

bool wps_context_menu_is_changed(void* setting, void* defaultval)
{
    return *(int*)setting != *(int*)defaultval;
}
