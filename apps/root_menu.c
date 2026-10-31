/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2007 Jonathan Gordon
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
#include "config.h"
#include "appevents.h"
#include "menu.h"
#include "root_menu.h"
#include "itunesdb.h"
#include "lang.h"
#include "settings.h"
#include "screens.h"
#include "kernel.h"
#include "debug.h"
#include "misc.h"
#include "open_plugin.h"
#include "rolo.h"
#include "powermgmt.h"
#include "power.h"
#include "talk.h"
#include "audio.h"
#include "shortcuts.h"

#ifdef HAVE_HOTSWAP
#include "storage.h"
#include "mv.h"
#endif
/* gui api */
#include "list.h"
#include "splash.h"
#include "action.h"
#include "yesno.h"
#include "viewport.h"

#include "tree.h"
#if CONFIG_TUNER
#include "radio.h"
#endif
#ifdef HAVE_RECORDING
#include "recording.h"
#endif
#include "wps.h"
#include "bookmark.h"
#include "playlist.h"
#include "playlist_viewer.h"
#include "playlist_catalog.h"
#include "menus/exported_menus.h"
#ifdef HAVE_RTC_ALARM
#include "rtc.h"
#endif
#ifdef HAVE_TAGCACHE
#include "tagcache.h"
#include "tagtree.h"
#endif
#include "language.h"
#include "plugin.h"
#include "disk.h"
#include "backlight.h"
#include "icons.h"

struct root_items {
    int (*function)(void* param);
    void* param;
    const struct menu_item_ex *context_menu;
};
static int next_screen = GO_TO_ROOT; /* holding info about the upcoming screen
                                        * which is the current screen for the
                                        * rest of the code after load_screen
                                        * is called */
static int last_screen = GO_TO_ROOT; /* unfortunatly needed so we can resume
                                        or goto current track based on previous
                                        screen */

static int previous_music = GO_TO_WPS; /* Toggles behavior of the return-to
                                        * playback-button depending
                                        * on FM radio */
static int last_db_dirlevel = 0, last_db_selection = 0;

/* Where the music playing was started. Holding Play opens Now Playing from anywhere,
   and Menu there then goes back to that and not to where Play was held. Not known
   after a boot, when it goes back as usual. */
static enum { ORIGIN_NONE, ORIGIN_SCREEN, ORIGIN_COVER_FLOW } music_origin = ORIGIN_NONE;
static int music_origin_screen;
static unsigned int music_starts_seen;
static bool wps_after_hold = false;

static void note_music_origin(int screen)
{
    const unsigned int starts = playlist_start_count();
    if (starts == music_starts_seen)
        return;
    music_starts_seen = starts;
    switch (screen)
    {
        case GO_TO_WPS:
        case GO_TO_PLAYLIST_VIEWER: /* within what's playing */
            return;
        case GO_TO_SHUFFLE_SONGS: /* from the main menu */
            music_origin_screen = GO_TO_ROOT;
            break;
#ifdef HAVE_TAGCACHE
        case GO_TO_DBBROWSER:
            tagtree_save_place(last_db_dirlevel, last_db_selection);
            /* fall through */
#endif
        default:
            music_origin_screen = screen;
            break;
    }
    music_origin = ORIGIN_SCREEN;
}

int root_menu_cover_flow(void)
{
    const int ret = plugin_load(PLUGIN_DEMOS_DIR "/pictureflow.rock", NULL);
    const unsigned int starts = playlist_start_count();
    if (starts != music_starts_seen)
    {
        music_starts_seen = starts;
        music_origin = ORIGIN_COVER_FLOW;
    }
    return ret;
}

/* The screen to go to from Now Playing, if it was opened by holding Play */
static int back_to_music_origin(int otherwise)
{
    switch (music_origin)
    {
        case ORIGIN_NONE:
            return otherwise;
        case ORIGIN_COVER_FLOW:
            switch (root_menu_cover_flow())
            {
                case PLUGIN_GOTO_WPS:
                    wps_after_hold = true;
                    return GO_TO_WPS;
                case PLUGIN_GOTO_ROOT:
                    return GO_TO_ROOT;
                case PLUGIN_GOTO_PLUGIN:
                    return GO_TO_PLUGIN;
                default:
                    return GO_TO_DBBROWSER;
            }
        case ORIGIN_SCREEN:
        default:
#ifdef HAVE_TAGCACHE
            if (music_origin_screen == GO_TO_DBBROWSER)
                tagtree_restore_place(&last_db_dirlevel, &last_db_selection);
#endif
            return music_origin_screen;
    }
}

#if (CONFIG_TUNER)
static void rootmenu_start_playback_callback(unsigned short id, void *param)
{
    (void) id; (void) param;
    /* Cancel FM radio selection as previous music. For cases where we start
       playback without going to the WPS, such as playlist insert or
       playlist catalog. */
    previous_music = GO_TO_WPS;
}
#endif

static char current_track_path[MAX_PATH];
static void rootmenu_track_changed_callback(unsigned short id, void* param)
{
    (void)id;
    struct mp3entry *id3 = ((struct track_event *)param)->id3;
    strmemccpy(current_track_path, id3->path, MAX_PATH);
}
static int browser(void* param)
{
    int ret_val;
#ifdef HAVE_TAGCACHE
    struct tree_context* tc = tree_get_context();
#endif
    int filter = SHOW_SUPPORTED;
    char folder[MAX_PATH] = "/";
    /* stuff needed to remember position in file browser */
    /*static char last_folder[MAX_PATH] = "/"*/
    char *last_folder = global_status.browse_last_folder;

    /* and stuff for the database browser */
#ifdef HAVE_TAGCACHE
    static int last_ft_dirlevel = 0;
#endif

    switch ((intptr_t)param)
    {
        case GO_TO_FILEBROWSER:
            filter = global_settings.dirfilter;
            if (global_settings.browse_current &&
                    last_screen == GO_TO_WPS &&
                    current_track_path[0])
            {
                strcpy(folder, current_track_path);
            }
            else if (!strcmp(last_folder, "/"))
            {
                strcpy(folder, global_settings.start_directory);
            }
            else
            {
#ifdef HAVE_HOTSWAP
                bool in_hotswap = false;
                /* handle entering an ejected drive */
                int i;
                for (i = 0; i < NUM_VOLUMES; i++)
                {
                    char vol_string[VOL_MAX_LEN + 1];
                    if (!volume_removable(i))
                        continue;
                    get_volume_name(i, vol_string);
                    /* test whether we would browse the external card */
                    if (!volume_present(i) &&
                            (strstr(last_folder, vol_string)
#ifdef HAVE_HOTSWAP_STORAGE_AS_MAIN
                                                                || (i == 0)
#endif
                                                                ))
                    {   /* leave folder as "/" to avoid crash when trying
                         * to access an ejected drive */
                        strcpy(folder, "/");
                        in_hotswap = true;
                        break;
                    }
                }
                if (!in_hotswap)
#endif /*HAVE_HOTSWAP*/
                    strcpy(folder, last_folder);
            }
            push_current_activity(ACTIVITY_FILEBROWSER);
        break;
#ifdef HAVE_TAGCACHE
        case GO_TO_DBBROWSER:
            if (!tagcache_is_usable())
            {
                bool reinit_attempted = false;

                /* Now display progress until it's ready or the user exits */
                while(!tagcache_is_usable())
                {
                    struct tagcache_stat *stat = tagcache_get_stat();

                    /* Allow user to exit */
                    if (action_userabort(HZ/2))
                        break;

                    /* Maybe just needs to reboot due to delayed commit */
                    if (stat->commit_delayed)
                    {
                        splash(HZ*2, ID2P(LANG_PLEASE_REBOOT));
                        break;
                    }

                    /* Check if ready status is known */
                    if (!stat->readyvalid)
                    {
                        splash(0, ID2P(LANG_TAGCACHE_BUSY));
                        continue;
                    }

                    /* Re-init if required */
                    if (!reinit_attempted && !stat->ready &&
                        stat->processed_entries == 0 && stat->commit_step == 0)
                    {
                        /* Prompt the user */
                        reinit_attempted = true;
                        static const char *lines[]={
                            ID2P(LANG_TAGCACHE_BUSY), ID2P(LANG_TAGCACHE_FORCE_UPDATE)};
                        static const struct text_message message={lines, 2};
                        if(gui_syncyesno_run(&message, NULL, NULL) == YESNO_NO)
                            break;
                        clear_screen_buffer(false);

                        /* Start initialisation */
                        tagcache_rebuild();
                    }

                    /* Display building progress */
                    static long talked_tick = 0;
                    if(global_settings.talk_menu &&
                       (talked_tick == 0
                        || TIME_AFTER(current_tick, talked_tick+7*HZ)))
                    {
                        talked_tick = current_tick;
                        if (stat->commit_step > 0)
                        {
                            talk_id(LANG_TAGCACHE_INIT, false);
                            talk_number(stat->commit_step, true);
                            talk_id(VOICE_OF, true);
                            talk_number(tagcache_get_max_commit_step(), true);
                        } else if(stat->processed_entries)
                        {
                            talk_number(stat->processed_entries, false);
                            talk_id(LANG_BUILDING_DATABASE, true);
                        }
                    }
                    if (stat->commit_step > 0)
                    {
                        /* (prevent redundant voicing by splash_progress */
                        bool tmp = global_settings.talk_menu;
                        global_settings.talk_menu = false;

                        if (lang_is_rtl())
                        {
                            splash_progress(stat->commit_step,
                                            tagcache_get_max_commit_step(),
                                            "[%d/%d] %s", stat->commit_step,
                                            tagcache_get_max_commit_step(),
                                            str(LANG_TAGCACHE_INIT));
                        }
                        else
                        {
                            splash_progress(stat->commit_step,
                                            tagcache_get_max_commit_step(),
                                            "%s [%d/%d]", str(LANG_TAGCACHE_INIT),
                                            stat->commit_step,
                                            tagcache_get_max_commit_step());
                        }
                        global_settings.talk_menu = tmp;
                    }
                    else
                    {
                        splashf(0, str(LANG_BUILDING_DATABASE),
                                   stat->processed_entries); /* (voiced above) */
                    }
                }
            }
            if (!tagcache_is_usable())
                return GO_TO_PREVIOUS;
            if (!tagtree_ready())
            {
                splashf(HZ*3, "%s\n%s", str(LANG_FILE_NOT_FOUND),
                        ROCKBOX_DIR "/tagnavi.config");
                return GO_TO_PREVIOUS;
            }
            filter = SHOW_ID3DB;
            last_ft_dirlevel = tc->dirlevel;
            tc->dirlevel = last_db_dirlevel;
            tc->selected_item = last_db_selection;
            push_current_activity(ACTIVITY_DATABASEBROWSER);
        break;
#endif /*HAVE_TAGCACHE*/
    }

    struct browse_context browse = {
        .dirfilter = filter,
        .icon = Icon_NOICON,
        .root = folder,
    };

    ret_val = rockbox_browse(&browse);

    if (ret_val == GO_TO_WPS
        || ret_val == GO_TO_PREVIOUS_MUSIC
        || ret_val == GO_TO_PLUGIN)
        pop_current_activity_without_refresh();
    else
        pop_current_activity();

    switch ((intptr_t)param)
    {
        case GO_TO_FILEBROWSER:
            if (!get_current_file(last_folder, MAX_PATH) ||
                (!strchr(&last_folder[1], '/') &&
                 global_settings.start_directory[1] != '\0'))
            {
                last_folder[0] = '/';
                last_folder[1] = '\0';
            }
        break;
#ifdef HAVE_TAGCACHE
        case GO_TO_DBBROWSER:
            last_db_dirlevel = tc->dirlevel;
            last_db_selection = tc->selected_item;
            tc->dirlevel = last_ft_dirlevel;
        break;
#endif
    }
    return ret_val;
}

#ifdef HAVE_RECORDING
static int recscrn(void* param)
{
    (void)param;
    recording_screen(false);
    return GO_TO_ROOT;
}
#endif
#ifdef HAVE_TAGCACHE
static int shuffle_songs(void* param)
{
    (void)param;
    return tagtree_shuffle_songs() ? GO_TO_WPS : GO_TO_PREVIOUS;
}
#endif
static int wpsscrn(void* param)
{
    int ret_val = GO_TO_PREVIOUS;
    int audstatus = audio_status();
    (void)param;
    push_current_activity(ACTIVITY_WPS);

#ifdef HAVE_PITCHCONTROL
    if (!audstatus)
    {
        sound_set_pitch(global_status.resume_pitch);
        dsp_set_timestretch(global_status.resume_speed);
    }
#endif

    if (audstatus)
    {
        talk_shutup();
        ret_val = gui_wps_show();
    }
    else if (global_status.resume_index != -1)
    {
        DEBUGF("Resume index %d crc32 %lX offset %lX\n",
               global_status.resume_index,
               (unsigned long)global_status.resume_crc32,
               (unsigned long)global_status.resume_offset);
        if (playlist_resume() != -1)
        {
            playlist_resume_track(global_status.resume_index,
                global_status.resume_crc32,
                global_status.resume_elapsed,
                global_status.resume_offset);
            ret_val = gui_wps_show();
        }
    }
    else if (!file_exists(PLAYLIST_CONTROL_FILE))
        splash(HZ*2, ID2P(LANG_NOTHING_TO_RESUME));
    else if (yesno_pop(ID2P(LANG_REPLAY_FINISHED_PLAYLIST)) &&
             playlist_resume() != -1)
    {
        playlist_start(0, 0, 0);
        ret_val = gui_wps_show();
    }

    if (ret_val == GO_TO_PLAYLIST_VIEWER
        || ret_val == GO_TO_PLUGIN
        || ret_val == GO_TO_WPS
        || ret_val == GO_TO_PREVIOUS_MUSIC
        || ret_val == GO_TO_PREVIOUS_BROWSER
        || (ret_val == GO_TO_PREVIOUS
               && (last_screen == GO_TO_MAINMENU /* Settings */
                || last_screen == GO_TO_BROWSEPLUGINS
                || last_screen == GO_TO_SYSTEM_SCREEN
                || last_screen == GO_TO_PLAYLISTS_SCREEN)))
    {
        pop_current_activity_without_refresh();
    }
    else
        pop_current_activity();

    return ret_val;
}
#if CONFIG_TUNER
static int radio(void* param)
{
    (void)param;
    radio_screen();
    return GO_TO_ROOT;
}
#endif

static int miscscrn(void * param)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex*)param;
    /* room for the settings' values, and for what's opened from them */
    menu_set_wide(true);
    int result = do_menu(menu, NULL, NULL, false);
    menu_set_wide(false);
    switch (result)
    {
        case GO_TO_PLUGIN:
        case GO_TO_PLAYLIST_VIEWER:
        case GO_TO_WPS:
        case GO_TO_PREVIOUS_MUSIC:
            return result;
        default:
            return GO_TO_ROOT;
    }
}


static int playlist_view_catalog(void * param)
{
    (void)param;
    push_current_activity(ACTIVITY_PLAYLISTBROWSER);
    bool item_was_selected = catalog_view_playlists();

    if (item_was_selected)
    {
        pop_current_activity_without_refresh();
        return GO_TO_WPS;
    }
    pop_current_activity();
    /* back to the Music menu it is in, or the main menu */
    return GO_TO_PREVIOUS;
}

static int playlist_view(void * param)
{
    (void)param;
    int val;

    val = playlist_viewer();
    switch (val)
    {
        case PLAYLIST_VIEWER_MAINMENU:
        case PLAYLIST_VIEWER_USB:
            return GO_TO_ROOT;
        case PLAYLIST_VIEWER_OK:
            return GO_TO_PREVIOUS;
    }
    return GO_TO_PREVIOUS;
}

/* the Photos plugin is made for the iPod Video screen and keys */
#if (CONFIG_KEYPAD == IPOD_4G_PAD) && (LCD_WIDTH == 320) && (LCD_HEIGHT == 240)
#define HAVE_PHOTOS_MENU
static int photos(void *param)
{
    (void)param;
    switch (plugin_load(PLUGIN_APPS_DIR "/photos.rock", NULL))
    {
        case PLUGIN_GOTO_WPS:
            return GO_TO_WPS;
        case PLUGIN_GOTO_PLUGIN:
            return GO_TO_PLUGIN;
        default:
            return GO_TO_ROOT;
    }
}
#endif

static int load_bmarks(void* param)
{
    (void)param;
    if(bookmark_mrb_load())
        return GO_TO_WPS;
    return GO_TO_PREVIOUS;
}

/* These are all static const'd from apps/menus/ *.c
   so little hack so we can use them */
extern struct menu_item_ex
        file_menu,
#ifdef HAVE_TAGCACHE
        tagcache_menu,
#endif
        main_menu_,
        manage_settings,
        plugin_menu,
        playlist_options,
        info_menu,
        system_menu;
static const struct root_items items[] = {
    [GO_TO_FILEBROWSER] =   { browser, (void*)GO_TO_FILEBROWSER, &file_menu},
#ifdef HAVE_TAGCACHE
    [GO_TO_DBBROWSER] =     { browser, (void*)GO_TO_DBBROWSER, NULL },
#endif
    [GO_TO_WPS] =           { wpsscrn, NULL, NULL },
    [GO_TO_MAINMENU] =      { miscscrn, (struct menu_item_ex*)&main_menu_,
                                                            NULL },

#ifdef HAVE_RECORDING
    [GO_TO_RECSCREEN] =     {  recscrn, NULL, &recording_settings_menu },
#endif

#if CONFIG_TUNER
    [GO_TO_FM] =            { radio, NULL, &radio_settings_menu },
#endif

    [GO_TO_RECENTBMARKS] =  { load_bmarks, NULL, &bookmark_settings_menu },
    [GO_TO_BROWSEPLUGINS] = { miscscrn, &plugin_menu, NULL },
    [GO_TO_PLAYLISTS_SCREEN] = { playlist_view_catalog, NULL,
                                                        &playlist_options },
    [GO_TO_PLAYLIST_VIEWER] = { playlist_view, NULL, &playlist_options },
    [GO_TO_SYSTEM_SCREEN] = { miscscrn, &info_menu, &system_menu },
    [GO_TO_SHORTCUTMENU] = { do_shortcut_menu, NULL, NULL },
    [GO_TO_EXTRAS] = { miscscrn, (struct menu_item_ex*)&extras_menu, NULL },
#ifdef HAVE_TAGCACHE
    [GO_TO_SHUFFLE_SONGS] = { shuffle_songs, NULL, NULL },
#endif
#ifdef HAVE_PHOTOS_MENU
    [GO_TO_PHOTOS] = { photos, NULL, NULL },
#endif

};
#define NUM_ITEMS (int)(sizeof(items)/sizeof(*items))

static int item_callback(int action,
                         const struct menu_item_ex *this_item,
                         struct gui_synclist *this_list);

MENUITEM_RETURNVALUE(shortcut_menu, ID2P(LANG_SHORTCUTS), GO_TO_SHORTCUTMENU,
                        NULL, Icon_Bookmark);

MENUITEM_RETURNVALUE(file_browser, ID2P(LANG_DIR_BROWSER), GO_TO_FILEBROWSER,
                        NULL, Icon_file_view_menu);
#ifdef HAVE_TAGCACHE
MENUITEM_RETURNVALUE(db_browser, ID2P(LANG_MUSIC), GO_TO_DBBROWSER,
                        NULL, Icon_Audio);
MENUITEM_RETURNVALUE(shuffle_songs_item, ID2P(LANG_SHUFFLE_SONGS),
                        GO_TO_SHUFFLE_SONGS, NULL, Icon_Audio);
#endif
MENUITEM_RETURNVALUE(rocks_browser, ID2P(LANG_PLUGINS), GO_TO_BROWSEPLUGINS,
                        NULL, Icon_Plugin);
MENUITEM_RETURNVALUE(extras_item, ID2P(LANG_EXTRAS), GO_TO_EXTRAS,
                        NULL, Icon_Plugin);
#ifdef HAVE_PHOTOS_MENU
MENUITEM_RETURNVALUE(photos_item, ID2P(LANG_PHOTOS), GO_TO_PHOTOS,
                        NULL, Icon_Plugin);
#endif

static char *get_wps_item_name(int selected_item, void * data,
                               char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    if (audio_status())
        return ID2P(LANG_NOW_PLAYING);
    return ID2P(LANG_RESUME_PLAYBACK);
}
MENUITEM_RETURNVALUE_DYNTEXT(wps_item, GO_TO_WPS, NULL, get_wps_item_name,
                                NULL, NULL, Icon_Playback_menu);
#ifdef HAVE_RECORDING
MENUITEM_RETURNVALUE(rec, ID2P(LANG_RECORDING), GO_TO_RECSCREEN,
                        NULL, Icon_Recording);
#endif
#if CONFIG_TUNER
MENUITEM_RETURNVALUE(fm, ID2P(LANG_FM_RADIO), GO_TO_FM,
                        item_callback, Icon_Radio_screen);
#endif
MENUITEM_RETURNVALUE(menu_, ID2P(LANG_SETTINGS), GO_TO_MAINMENU,
                        NULL, Icon_Submenu_Entered);
MENUITEM_RETURNVALUE(bookmarks, ID2P(LANG_BOOKMARK_MENU_RECENT_BOOKMARKS),
                        GO_TO_RECENTBMARKS,  item_callback,
                        Icon_Bookmark);
MENUITEM_RETURNVALUE(playlists, ID2P(LANG_PLAYLISTS), GO_TO_PLAYLISTS_SCREEN,
                     NULL, Icon_Playlist);
MENUITEM_RETURNVALUE(system_menu_, ID2P(LANG_SYSTEM), GO_TO_SYSTEM_SCREEN,
                     NULL, Icon_System_menu);

struct menu_item_ex root_menu_;
static struct menu_callback_with_desc root_menu_desc = {
        item_callback, ID2P(LANG_ROCKBOX_TITLE), Icon_Rockbox };

static struct menu_table menu_table[] = {
    /* Order here represents the default ordering */
    { "bookmarks", &bookmarks },
    { "files", &file_browser },
#ifdef HAVE_TAGCACHE
    { "database", &db_browser },
    { "shuffle_songs", &shuffle_songs_item },
#endif
    { "wps", &wps_item },
    { "settings", &menu_ },
#ifdef HAVE_RECORDING
    { "recording", &rec },
#endif
#if CONFIG_TUNER
    { "radio", &fm },
#endif
    { "playlists", &playlists },
    { "plugins", &rocks_browser },
    { "extras", &extras_item },
#ifdef HAVE_PHOTOS_MENU
    { "photos", &photos_item },
#endif
    { "system_menu", &system_menu_ },
    { "shortcuts", &shortcut_menu },
};
#define MAX_MENU_ITEMS (sizeof(menu_table) / sizeof(struct menu_table))
static struct menu_item_ex *root_menu__[MAX_MENU_ITEMS];

struct menu_table *root_menu_get_options(int *nb_options)
{
    *nb_options = MAX_MENU_ITEMS;

    return menu_table;
}

void root_menu_load_from_cfg(void* setting, char *value)
{
    char *next = value, *start, *end;
    unsigned int menu_item_count = 0, i;
    bool main_menu_added = false;

    if (*value == '-')
    {
        root_menu_set_default(setting, NULL);
        return;
    }
    root_menu_.flags = MENU_HAS_DESC | MT_MENU;
    root_menu_.submenus = (const struct menu_item_ex **)&root_menu__;
    root_menu_.callback_and_desc = &root_menu_desc;

    while (next && menu_item_count < MAX_MENU_ITEMS)
    {
        start = next;
        next = strchr(next, ',');
        if (next)
        {
            *next = '\0';
            next++;
        }
        start = skip_whitespace(start);
        if ((end = strchr(start, ' ')))
            *end = '\0';
        for (i=0; i<MAX_MENU_ITEMS; i++)
        {
            if (*start && !strcmp(start, menu_table[i].string))
            {
                root_menu__[menu_item_count++] = (struct menu_item_ex *)menu_table[i].item;
                if (menu_table[i].item == &menu_)
                    main_menu_added = true;
                break;
            }
        }
    }
    if (!main_menu_added)
        root_menu__[menu_item_count++] = (struct menu_item_ex *)&menu_;
    root_menu_.flags |= MENU_ITEM_COUNT(menu_item_count);
    *(bool*)setting = true;
}

char* root_menu_write_to_cfg(void* setting, char*buf, int buf_len)
{
    (void)setting;
    unsigned i, written, j;
    for (i = 0; i < MENU_GET_COUNT(root_menu_.flags); i++)
    {
        for (j=0; j<MAX_MENU_ITEMS; j++)
        {
            if (menu_table[j].item == root_menu__[i])
            {
                written = snprintf(buf, buf_len, "%s, ", menu_table[j].string);
                buf_len -= written;
                buf += written;
                break;
            }
        }
    }
    return buf;
}

void root_menu_set_default(void* setting, void* defaultval)
{
    /* Only the items a stock iPod has are in the default menu and in its
       order, the rest can still be added back through the main menu
       configuration; playlists are in the Music (database) menu */
    char order[] = "bookmarks, database, photos, extras, settings, shuffle_songs, wps";
    (void)defaultval;

    root_menu_load_from_cfg(setting, order);
    *(bool*)setting = false;
}

bool root_menu_is_changed(void* setting, void* defaultval)
{
    (void)defaultval;
    return *(bool*)setting;
}

static int item_callback(int action,
                         const struct menu_item_ex *this_item,
                         struct gui_synclist *this_list)
{
    (void)this_list;
    switch (action)
    {
        case ACTION_TREE_STOP:
            return ACTION_REDRAW;
        case ACTION_REQUEST_MENUITEM:
#if CONFIG_TUNER
            if (this_item == &fm)
            {
                if (radio_hardware_present() == 0)
                    return ACTION_EXIT_MENUITEM;
            }
            else
#endif
                if (this_item == &bookmarks)
            {
                if (global_settings.usemrb == 0)
                    return ACTION_EXIT_MENUITEM;
            }
        break;
    }
    return action;
}

static int get_selection(int last_screen)
{
    int i;
    /* only the items in use, the screen may not be in the menu at all */
    int len = MENU_GET_COUNT(root_menu_.flags);
    for(i=0; i < len; i++)
    {
        if (((root_menu__[i]->flags&MENU_TYPE_MASK) == MT_RETURN_VALUE) &&
            (root_menu__[i]->value == last_screen))
        {
            return i;
        }
    }
    return 0;
}

static inline int load_screen(int screen)
{
    /* set the global_status.last_screen before entering,
        if we dont we will always return to the wrong screen on boot */
    int old_previous = last_screen;
    int ret_val;
    enum current_activity activity = ACTIVITY_UNKNOWN;
    if (screen <= GO_TO_ROOT)
        return screen;
    if (screen == old_previous)
        old_previous = GO_TO_ROOT;
#ifdef HAVE_TAGCACHE
    /* an action rather than a screen, never to be gone back or booted to */
    bool is_action = (screen == GO_TO_SHUFFLE_SONGS);
#else
    bool is_action = false;
#endif
    if (!is_action)
    {
        global_status.last_screen = (char)screen;
        status_save(false);
    }

    if (screen == GO_TO_BROWSEPLUGINS)
        activity = ACTIVITY_PLUGINBROWSER;
    else if (screen == GO_TO_MAINMENU)
        activity = ACTIVITY_SETTINGS;
    else if (screen == GO_TO_SYSTEM_SCREEN)
        activity =  ACTIVITY_SYSTEMSCREEN;

    if (activity != ACTIVITY_UNKNOWN)
        push_current_activity(activity);

    ret_val = items[screen].function(items[screen].param);

    if (activity != ACTIVITY_UNKNOWN)
    {
        if (ret_val == GO_TO_PLUGIN
            || ret_val == GO_TO_WPS
            || ret_val == GO_TO_PREVIOUS_MUSIC
            || ret_val == GO_TO_PREVIOUS_BROWSER
            || ret_val == GO_TO_FILEBROWSER)
        {
            pop_current_activity_without_refresh();
        }
        else
            pop_current_activity();
    }

    last_screen = screen;
    if (ret_val == GO_TO_PREVIOUS || is_action)
        last_screen = old_previous;
    return ret_val;
}

static int load_context_screen(int selection)
{
    const struct menu_item_ex *context_menu = NULL;
    int retval = GO_TO_PREVIOUS;
    push_current_activity(ACTIVITY_CONTEXTMENU);
    if ((root_menu__[selection]->flags&MENU_TYPE_MASK) == MT_RETURN_VALUE)
    {
        int item = root_menu__[selection]->value;
        context_menu = items[item].context_menu;
    }
    /* special cases */
    else if (root_menu__[selection] == &info_menu)
    {
        context_menu = &system_menu;
    }

    if (context_menu)
        retval = do_menu(context_menu, NULL, NULL, false);
    pop_current_activity();
    return retval;
}

static int load_plugin_screen(char *key)
{
    int ret_val = PLUGIN_ERROR;
    int loops = 100;
    int old_previous = last_screen;
    int old_global = global_status.last_screen;
    last_screen = next_screen;
    global_status.last_screen = (char)next_screen;

    while(loops-- > 0) /* just to keep things from getting out of hand */
    {
        int opret = open_plugin_load_entry(key);
        struct open_plugin_entry_t *op_entry = open_plugin_get_entry();
        char *path = op_entry->path;
        char *param = op_entry->param;
        if (param[0] == '\0')
            param = NULL;
        if (path[0] == '\0' && key)
            path = P2STR((unsigned char *)key);
        int ret = plugin_load(path, param);

        if (ret == PLUGIN_USB_CONNECTED || ret == PLUGIN_ERROR)
            ret_val = GO_TO_ROOT;
        else if (ret == PLUGIN_GOTO_WPS) /* prevent infinite loop */
            ret_val = old_global == GO_TO_WPS ? GO_TO_BROWSEPLUGINS : GO_TO_WPS;
        else if (ret == PLUGIN_GOTO_PLUGIN)
        {
            if(op_entry->lang_id == LANG_OPEN_PLUGIN)
            {
                if (key == (char*)ID2P(LANG_SHORTCUTS))
                {
                    op_entry->lang_id = LANG_SHORTCUTS;
                }
                else /* Bugfix ensure proper key */
                {
                    key = ID2P(LANG_OPEN_PLUGIN);
                }
            }
            continue;
        }
        else
        {
            if (ret == PLUGIN_GOTO_ROOT)
                ret_val = GO_TO_ROOT;
            else
                ret_val = GO_TO_PREVIOUS;
            /* Prevents infinite loop with WPS, Plugins, Previous Screen*/
            if (ret == PLUGIN_OK && old_global == GO_TO_WPS && !audio_status())
                ret_val = GO_TO_ROOT;
            last_screen = (old_previous == next_screen || old_global == GO_TO_ROOT)
                ? GO_TO_ROOT : old_previous;
            if (last_screen == GO_TO_ROOT)
                global_status.last_screen = GO_TO_ROOT;
        }
        /* ret_val != GO_TO_PLUGIN */

        if (opret != OPEN_PLUGIN_NEEDS_FLUSHED || last_screen != GO_TO_WPS)
        {
            /* Keep the entry in case of GO_TO_PREVIOUS */
            op_entry->hash = 0; /*remove hash -- prevents flush to disk */
            op_entry->lang_id = LANG_PREVIOUS_SCREEN;
            /*open_plugin_add_path(NULL, NULL, NULL);// clear entry */
        }
        break;
    } /*while */
    return ret_val;
}

static void ignore_back_button_stub(bool ignore)
{
#if (CONFIG_PLATFORM&PLATFORM_ANDROID)
    /* BACK button to be handled by Android instead of rockbox */
    android_ignore_back_button(ignore);
#else
    (void) ignore;
#endif
}

static int root_menu_setup_screens(void)
{
    int new_screen = next_screen;
    if (global_settings.start_in_screen == 0)
        new_screen = global_status.last_screen;
    else
        new_screen = global_settings.start_in_screen - 2;

    if (new_screen >= NUM_ITEMS || new_screen == GO_TO_PHOTOS ||
        (new_screen == GO_TO_WPS && global_status.resume_index == -1))
        new_screen = GO_TO_ROOT;
    else if (new_screen == GO_TO_PLUGIN)
    {
        if (global_status.last_screen == GO_TO_SHORTCUTMENU)
        {
            /* Can make this any value other than GO_TO_SHORTCUTMENU
               otherwise it takes over on startup when the user wanted
               the plugin at key - LANG_START_SCREEN */
            global_status.last_screen = GO_TO_PLUGIN;
        }
        if(global_status.last_screen == GO_TO_SHORTCUTMENU ||
           global_status.last_screen == GO_TO_PLUGIN)
        {
            if (global_settings.start_in_screen == 0)
            {  /* Start in: Previous Screen */
                last_screen = GO_TO_PREVIOUS;
                global_status.last_screen = GO_TO_ROOT;
                /* since the plugin has GO_TO_PLUGIN as origin it
                   will just return GO_TO_PREVIOUS <=> GO_TO_PLUGIN in a loop
                   To allow exit after restart we check for GO_TO_ROOT
                   if so exit to ROOT after the plugin exits */
            }
        }
    }
#if CONFIG_TUNER
    add_event(PLAYBACK_EVENT_START_PLAYBACK, rootmenu_start_playback_callback);
#endif
    add_event(PLAYBACK_EVENT_TRACK_CHANGE, rootmenu_track_changed_callback);
#ifdef HAVE_RTC_ALARM
    int alarm_wake_up_screen = 0;
    if ( rtc_check_alarm_started(true) )
    {
        rtc_enable_alarm(false);

#if (defined(HAVE_RECORDING) || CONFIG_TUNER)
        alarm_wake_up_screen = global_settings.alarm_wake_up_screen;
#endif
        switch (alarm_wake_up_screen)
        {
#if CONFIG_TUNER
            case ALARM_START_FM:
                new_screen = GO_TO_FM;
                break;
#endif
#ifdef HAVE_RECORDING
            case ALARM_START_REC:
                recording_start_automatic = true;
                new_screen = GO_TO_RECSCREEN;
                break;
#endif
            default:
                new_screen = GO_TO_WPS;
                break;
        } /* switch() */
    }
#endif /* HAVE_RTC_ALARM */

#if defined(HAVE_HEADPHONE_DETECTION) || defined(HAVE_LINEOUT_DETECTION)
    if (new_screen == GO_TO_WPS && global_settings.unplug_autoresume)
    {
       new_screen = GO_TO_ROOT;
#ifdef HAVE_HEADPHONE_DETECTION
        if (headphones_inserted())
            new_screen = GO_TO_WPS;
#endif
#ifdef HAVE_LINEOUT_DETECTION
        if (lineout_inserted())
            new_screen = GO_TO_WPS;
#endif
    }
#endif /*(HAVE_HEADPHONE_DETECTION) || (HAVE_LINEOUT_DETECTION)*/
    return new_screen;
}


#ifdef HAVE_TAGCACHE
/* the step of the import after a sync, each with a bar of its own */
static const char *library_update_step(int phase)
{
    switch (phase)
    {
        case TAGCACHE_IMPORT_READING:
            return str(LANG_READING_LIBRARY);
        case TAGCACHE_IMPORT_SCANNING:
            return str(LANG_CHECKING_SONGS);
        case TAGCACHE_IMPORT_COUNTS:
            return str(LANG_UPDATING_PLAY_COUNTS);
        case TAGCACHE_IMPORT_COVERS:
        case TAGCACHE_IMPORT_SLIDES:
            return str(LANG_PREPARING_COVERS);
        default: /* the commit, and loading it into memory */
            return str(LANG_UPDATING_LIBRARY);
    }
}

static int library_update_percent(int phase, int done, int total)
{
    if (phase == TAGCACHE_IMPORT_LOADING)
        return 100; /* of the commit before it: it has no numbers itself */
    return total > 0 ? 100 * MIN(done, total) / total : 0;
}

/* what the screen shows now */
static const char *shown_text;
static int shown_percent;
static long shown_at;

/* the screen with this step and how far it is: drawn for a change, and every
   second as the theme's backdrop can come back meanwhile */
static void library_update_show(const char *text, int percent)
{
    if (text == shown_text && percent == shown_percent
        && TIME_BEFORE(current_tick, shown_at + HZ))
        return;
    shown_text = text;
    shown_percent = percent;
    shown_at = current_tick;

    const int bar_w = 140, bar_h = 6;
    lcd_set_viewport(NULL);
#if LCD_DEPTH > 1
    lcd_set_backdrop(NULL);
#endif
#ifdef STOCK_BOOT_LOGO
    int y = STOCK_BOOT_LOGO_Y;
#else
    int y = (LCD_HEIGHT - 96) / 2;
#endif
    lcd_set_background(LCD_BLACK);
    lcd_clear_display();
#ifdef STOCK_BOOT_LOGO
    lcd_bmp(&bm_bootlogo, (LCD_WIDTH - BMPWIDTH_bootlogo) / 2, y);
    y += BMPHEIGHT_bootlogo;
#else
    y += 96;
#endif
    y += 22;
    int x = (LCD_WIDTH - bar_w) / 2;
    lcd_set_foreground(LCD_RGBPACK(70, 70, 70));
    lcd_fillrect(x, y, bar_w, bar_h);
    lcd_set_foreground(LCD_WHITE);
    lcd_fillrect(x, y, bar_w * percent / 100, bar_h);

    int w, h;
    lcd_setfont(FONT_UI);
    lcd_getstringsize(text, &w, &h);
    lcd_set_foreground(LCD_RGBPACK(160, 160, 160));
    lcd_set_drawmode(DRMODE_FG);
    lcd_putsxy((LCD_WIDTH - w) / 2, y + bar_h + 10, text);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_update();
    backlight_on();
}

/* A step of the import that isn't the tagcache's. Its parts may each count
   anew, so its one bar only goes forward. */
static void library_update_forward(const char *text, int done, int total)
{
    static int percent;
    if (shown_text != text)
        percent = 0;
    if (total > 0)
        percent = MAX(percent, 100 * MIN(done, total) / total);
    library_update_show(text, percent);
}

#ifdef ITUNESDB_HAVE_ART
void library_update_progress(int done, int total)
{
    library_update_forward((const char *)str(LANG_PREPARING_ALBUMS), done, total);
}
#endif

void library_update_songs_progress(int done, int total)
{
    library_update_forward((const char *)str(LANG_PREPARING_SONGS), done, total);
}

/* After a sync the database takes in the new songs before the menu shows,
 * as a stock iPod has them as soon as it starts: the boot logo stays, with
 * the step and how far that is under it. Menu leaves it to finish in the
 * background. */
void show_library_update(void)
{
    int done, total, phase;
    bool shown = false;
    while ((phase = tagcache_get_import(&done, &total))
           != TAGCACHE_IMPORT_NONE)
    {
        int percent = library_update_percent(phase, done, total);
        if (!shown)
        {
            /* the whole screen, without the theme */
            FOR_NB_SCREENS(i)
                viewportmanager_theme_enable(i, false, NULL);
            shown_text = NULL;
            shown = true;
        }
        library_update_show(library_update_step(phase), percent);
        int action = get_action(CONTEXT_STD, HZ / 4);
        if (action == ACTION_STD_CANCEL
            || default_event_handler(action) == SYS_USB_CONNECTED)
            break;
    }
    if (shown)
    {
#ifdef ITUNESDB_HAVE_ART
        /* the album list of the new database, which Cover Flow itself makes:
           it isn't left for when Cover Flow opens */
        if (phase == TAGCACHE_IMPORT_NONE
            && file_exists(PLUGIN_DEMOS_DIR "/pictureflow.rock"))
        {
            library_update_show(str(LANG_PREPARING_ALBUMS), 0);
            plugin_load(PLUGIN_DEMOS_DIR "/pictureflow.rock",
                        ITUNESDB_ALBUM_LIST);
        }
#endif
        if (phase == TAGCACHE_IMPORT_NONE)
            tagtree_prepare_songs();
        lcd_set_background(LCD_DEFAULT_BG);
        lcd_set_foreground(LCD_DEFAULT_FG);
        lcd_clear_display();
        FOR_NB_SCREENS(i)
            viewportmanager_theme_undo(i, true);
    }
}
#endif /* HAVE_TAGCACHE */

void root_menu(void)
{
    int previous_browser = global_status.last_browser;
    int selected = 0;
    int shortcut_origin = GO_TO_ROOT;

    push_current_activity(ACTIVITY_MAINMENU);
#ifdef HAVE_TAGCACHE
    show_library_update();
#endif
#ifdef STOCK_BOOT_LOGO
    /* the theme the boot turned off, now that the menu shows */
    FOR_NB_SCREENS(i)
        viewportmanager_theme_undo(i, false);
#endif
    next_screen = root_menu_setup_screens();

    while (true)
    {
        switch (next_screen)
        {
            case MENU_ATTACHED_USB:
            case MENU_SELECTED_EXIT:
                /* fall through */
            case GO_TO_ROOT:
                if (last_screen != GO_TO_ROOT)
                    selected = get_selection(last_screen);
                global_status.last_screen = GO_TO_ROOT; /* We've returned to ROOT */
                /* When we are in the main menu we want the hardware BACK
                 * button to be handled by HOST instead of rockbox */
                ignore_back_button_stub(true);

                next_screen = do_menu(&root_menu_, &selected, NULL, false);

                ignore_back_button_stub(false);

                if (next_screen != GO_TO_PREVIOUS)
                    last_screen = GO_TO_ROOT;
                break;
#ifdef HAVE_TAGCACHE
            case GO_TO_DBBROWSER:
#endif
            case GO_TO_FILEBROWSER:
            case GO_TO_PLAYLISTS_SCREEN:
                global_status.last_browser = previous_browser = next_screen;
                goto load_next_screen;
                break;
#if CONFIG_TUNER
            case GO_TO_WPS:
            case GO_TO_FM:
                previous_music = next_screen;
                goto load_next_screen;
                break;
#endif /* With !CONFIG_TUNER previous_music is always GO_TO_WPS */

            case GO_TO_PREVIOUS:
            {
                next_screen = last_screen;
                if (last_screen == GO_TO_PLUGIN)/* for WPS */
                    last_screen = GO_TO_PREVIOUS;
                else if (last_screen == GO_TO_PREVIOUS)
                    next_screen = GO_TO_ROOT;
                break;
            }

            case GO_TO_PREVIOUS_BROWSER:
                next_screen = previous_browser;
                break;

            case GO_TO_PREVIOUS_MUSIC:
                next_screen = previous_music;
                wps_after_hold = true;
                break;
            case GO_TO_ROOTITEM_CONTEXT:
                next_screen = load_context_screen(selected);
                break;
            case GO_TO_PLUGIN:
            {

                char *key;
                if (global_status.last_screen == GO_TO_SHORTCUTMENU)
                {
                    struct open_plugin_entry_t *op_entry = open_plugin_get_entry();
                    if (op_entry->lang_id == LANG_OPEN_PLUGIN)
                        op_entry->lang_id = LANG_SHORTCUTS;
                    shortcut_origin = last_screen;
                    key = ID2P(LANG_SHORTCUTS);
                }
                else
                {
                    switch (last_screen)
                    {
                        case GO_TO_ROOT:
                            key = ID2P(LANG_START_SCREEN);
                            break;
                        case GO_TO_WPS:
                            key = ID2P(LANG_ONPLAY_MENU_TITLE);
                            break;
                        case GO_TO_SHORTCUTMENU:
                            key = ID2P(LANG_SHORTCUTS);
                            break;
                        case GO_TO_PREVIOUS:
                            key = ID2P(LANG_PREVIOUS_SCREEN);
                            break;
                        default:
                            key = ID2P(LANG_OPEN_PLUGIN);
                            break;
                    }
                }

                next_screen = load_plugin_screen(key);

                if (next_screen == GO_TO_PREVIOUS)
                {
                    /* shortcuts may take several trips through the GO_TO_PLUGIN
                       case make sure we preserve and restore the origin */
                    if(tree_get_context()->out_of_tree > 0) /* a shortcut has been selected */
                    {
                        next_screen = GO_TO_FILEBROWSER;
                        shortcut_origin = GO_TO_ROOT;
                        /* note in some cases there is a screen to return to
                        but the history is rewritten as if you browsed here
                        from the root so return there when finished */
                    }
                    else if (shortcut_origin != GO_TO_ROOT)
                    {
                        if (shortcut_origin != GO_TO_WPS)
                            next_screen = shortcut_origin;
                        shortcut_origin = GO_TO_ROOT;
                    }
                    /* skip GO_TO_PREVIOUS */
                    if (last_screen == GO_TO_BROWSEPLUGINS)
                    {
                        next_screen = last_screen;
                        last_screen = GO_TO_PLUGIN;
                    }
                }
                previous_browser = (next_screen == GO_TO_WPS) ?
                                   GO_TO_PLUGIN : global_status.last_browser;
                break;
            }
            default:
                goto load_next_screen;
                break;
        } /* switch() */
        continue;
load_next_screen: /* load_screen is inlined */
        {
            const int screen = next_screen;
            next_screen = load_screen(screen);
            note_music_origin(screen);
            if (screen == GO_TO_WPS)
            {
                const bool after_hold = wps_after_hold;
                wps_after_hold = false;
                if (after_hold && next_screen == GO_TO_PREVIOUS)
                    next_screen = back_to_music_origin(next_screen);
            }
        }
    }

}
