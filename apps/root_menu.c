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
#include "smart_playlists.h"
#include "library_playlists.h"
#include "ipod_playlists.h"
#include "font.h"
#include "root_menu.h"
#include "gui/skin_engine/skin_engine.h"
#include "gui/skin_engine/menu_cover.h"
#include "photos_panel.h"
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
#ifdef HAVE_DIRCACHE
#include "dircache.h"
#endif
#include "language.h"
#include "plugin.h"
#include "disk.h"
#include "backlight.h"
#include "icons.h"
#ifdef HAVE_TAGCACHE
#include "music_search.h"
#include "album_years.h"
#include "list_covers.h"
#endif

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
#ifdef HAVE_TAGCACHE
/* Music chosen in the main menu, which opens it at its own menu */
static bool music_from_main_menu = false;
/* A section of Music in the main menu (music_section) whose lists were left
   for Now Playing or a plugin, and not by Menu: the lists are still where it was
   left, to go back to. 0 for none, or else its phrase. */
static int section_left_in = 0;
static bool in_section = false;
/* where Search and Cover Flow go back to: Music, or the main menu they were
   opened from */
static int search_back = GO_TO_DBBROWSER;
static int cover_flow_back = GO_TO_DBBROWSER;
static bool cover_flow_from_main_menu = false;

static bool is_section_screen(int screen)
{
    return screen >= GO_TO_ARTISTS && screen <= GO_TO_AUDIOBOOKS;
}
static int items_section_lang(int screen);
#endif

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
        case GO_TO_SHUFFLE_ALBUMS:
            music_origin_screen = GO_TO_ROOT;
            break;
#ifdef HAVE_TAGCACHE
        case GO_TO_DBBROWSER:
        case GO_TO_ARTISTS: case GO_TO_ALBUMS: case GO_TO_SONGS:
        case GO_TO_PODCASTS: case GO_TO_GENRES: case GO_TO_COMPOSERS:
        case GO_TO_AUDIOBOOKS:
            tagtree_save_place(last_db_dirlevel, last_db_selection);
            /* fall through */
#endif
        default:
            music_origin_screen = screen;
            break;
    }
    music_origin = ORIGIN_SCREEN;
}

/* the song of Cover Flow's Go to Artist, "" for none; offered only while
   root_menu_cover_flow() has it open, which goes there, and not as a plugin
   of a file's menu or a hotkey opens it */
static char cover_flow_go_to[MAX_PATH];
static bool cover_flow_go_to_offered;

bool root_menu_go_to_later(const char *path)
{
    if (cover_flow_go_to_offered && path)
        strmemccpy(cover_flow_go_to, path, sizeof(cover_flow_go_to));
    return cover_flow_go_to_offered;
}

int root_menu_cover_flow(const char *at, char *go_to)
{
#ifdef HAVE_TAGCACHE
    char back[MAX_PATH];
#endif
    while (true)
    {
        cover_flow_go_to[0] = '\0';
#ifdef HAVE_TAGCACHE
        cover_flow_go_to_offered = true;
#endif
        const int ret = plugin_load(PLUGIN_DEMOS_DIR "/pictureflow.rock", at);
        cover_flow_go_to_offered = false;
        const unsigned int starts = playlist_start_count();
        if (starts != music_starts_seen)
        {
            music_starts_seen = starts;
            music_origin = ORIGIN_COVER_FLOW;
#ifdef HAVE_TAGCACHE
            cover_flow_back = cover_flow_from_main_menu ? GO_TO_ROOT : GO_TO_DBBROWSER;
#endif
        }
        if (go_to)
            strmemccpy(go_to, ret == PLUGIN_OK ? cover_flow_go_to : "", MAX_PATH);
#ifdef HAVE_TAGCACHE
        if (ret != PLUGIN_OK || go_to || !cover_flow_go_to[0])
            return ret;
        strmemccpy(back, cover_flow_go_to, sizeof(back));
        switch (root_menu_go_to_track(back, true))
        {
            case -1: /* no artist found */
            case 0: /* Menu there: Cover Flow at it again */
                at = back;
                break;
            case GO_TO_WPS:
            case GO_TO_PREVIOUS_MUSIC:
                return PLUGIN_GOTO_WPS;
            case GO_TO_PLUGIN:
                return PLUGIN_GOTO_PLUGIN;
            default:
                return PLUGIN_GOTO_ROOT;
        }
#else
        return ret;
#endif
    }
}

/* The screen to go to from Now Playing, if it was opened by holding Play */
static int back_to_music_origin(int otherwise)
{
    switch (music_origin)
    {
        case ORIGIN_NONE:
            return otherwise;
        case ORIGIN_COVER_FLOW:
        {
#ifdef HAVE_TAGCACHE
            const int back = cover_flow_back;
            cover_flow_from_main_menu = back == GO_TO_ROOT;
#else
            const int back = GO_TO_DBBROWSER;
#endif
            const int ret = root_menu_cover_flow(NULL, NULL);
#ifdef HAVE_TAGCACHE
            cover_flow_from_main_menu = false;
#endif
            switch (ret)
            {
                case PLUGIN_GOTO_WPS:
                    wps_after_hold = true;
                    return GO_TO_WPS;
                case PLUGIN_GOTO_ROOT:
                    return GO_TO_ROOT;
                case PLUGIN_GOTO_PLUGIN:
                    return GO_TO_PLUGIN;
                default:
                    return back;
            }
        }
        case ORIGIN_SCREEN:
        default:
#ifdef HAVE_TAGCACHE
            if (music_origin_screen == GO_TO_DBBROWSER
                || is_section_screen(music_origin_screen))
                tagtree_restore_place(&last_db_dirlevel, &last_db_selection);
            /* a section goes on from there, not from its list */
            section_left_in = is_section_screen(music_origin_screen)
                ? items_section_lang(music_origin_screen) : 0;
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
            if (!in_section)
                section_left_in = 0;
            last_ft_dirlevel = tc->dirlevel;
            tc->dirlevel = last_db_dirlevel;
            tc->selected_item = last_db_selection;
            if (music_from_main_menu)
                tagtree_to_top(tc);
            music_from_main_menu = false;
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
            /* a list opened in place by Go to stays as it is, Menu going up
               from it as by hand */
            tagtree_forget_browse();
        break;
#endif
    }
    return ret_val;
}

#ifdef HAVE_TAGCACHE
/* The Music lists at the track playing, from the menu over Now Playing. Menu in the
   list opened goes back to that menu, and the place in the lists stays as it was,
   unless music was started from them. 0 for going back, or else the screen to go to. */
/* the lists opened by a go-to, if it found its place, and left; -1 if it
   didn't */
static int go_to_browse(bool found, unsigned int starts)
{
    if (!found)
        return -1;
    const int screen = browser((void*)GO_TO_DBBROWSER);
    const bool started = playlist_start_count() != starts;
    if (started)
        note_music_origin(GO_TO_DBBROWSER);
    tagtree_end_go_to(!started, &last_db_dirlevel, &last_db_selection);
    return screen == GO_TO_PREVIOUS ? 0 : screen;
}

int root_menu_go_to_playing(bool artist)
{
    const unsigned int starts = playlist_start_count();
    /* once: MAX() would open the lists a second time for a screen to go to */
    const int screen = go_to_browse(tagtree_go_to_playing(artist, &last_db_dirlevel,
                                                          &last_db_selection), starts);
    return MAX(screen, 0);
}

int root_menu_go_to_track(const char *path, bool artist)
{
    const unsigned int starts = playlist_start_count();
    return go_to_browse(tagtree_go_to_track(path, artist, &last_db_dirlevel,
                                            &last_db_selection), starts);
}

int root_menu_playing_from(void)
{
    const unsigned int starts = playlist_start_count();
    const int screen = go_to_browse(tagtree_go_to_from(&last_db_dirlevel,
                                                       &last_db_selection), starts);
    return MAX(screen, 0);
}


int root_menu_go_to_name(int tag, const char *name)
{
    const unsigned int starts = playlist_start_count();
    if (!tagtree_go_to_name(tag, name, &last_db_dirlevel, &last_db_selection))
        return -1;
    const int screen = browser((void*)GO_TO_DBBROWSER);
    const bool started = playlist_start_count() != starts;
    if (started)
        note_music_origin(GO_TO_DBBROWSER);
    tagtree_end_go_to(!started, &last_db_dirlevel, &last_db_selection);
    return screen == GO_TO_PREVIOUS ? 0 : screen;
}

int root_menu_go_to_album(const char *name, int32_t artist)
{
    const unsigned int starts = playlist_start_count();
    return go_to_browse(tagtree_go_to_album(name, artist, &last_db_dirlevel,
                                            &last_db_selection), starts);
}

void root_menu_artists_grouping_changed(bool on)
{
    (void)on;
    tagtree_artists_grouping_changed(&last_db_dirlevel, &last_db_selection);
}

/* A section of Music in the main menu, as on a stock iPod: its list, where Menu
   goes back to the main menu. Left for Now Playing, it opens again where it was,
   as Music does. */
static int music_section(void *param)
{
    const int lang = (intptr_t)param;
    if (section_left_in == lang)
        tagtree_resume_section(lang, &last_db_dirlevel, &last_db_selection);
    else if (!tagtree_go_to_section(lang, &last_db_dirlevel, &last_db_selection))
        return GO_TO_ROOT;
    in_section = true;
    const int screen = browser((void*)GO_TO_DBBROWSER);
    in_section = false;
    const bool left_by_menu = screen == GO_TO_PREVIOUS || screen == GO_TO_ROOT;
    tagtree_end_go_to(left_by_menu, &last_db_dirlevel, &last_db_selection);
    section_left_in = left_by_menu ? 0 : lang;
    return screen;
}

/* Cover Flow from the main menu, back to it */
static int cover_flow_screen(void *param)
{
    (void)param;
    cover_flow_from_main_menu = true;
    const int ret = root_menu_cover_flow(NULL, NULL);
    cover_flow_from_main_menu = false;
    switch (ret)
    {
        case PLUGIN_GOTO_WPS:
            return GO_TO_WPS;
        case PLUGIN_GOTO_ROOT:
            return GO_TO_ROOT;
        case PLUGIN_GOTO_PLUGIN:
            return GO_TO_PLUGIN;
        default:
            return GO_TO_PREVIOUS;
    }
}

/* Search, whose Menu goes up to Music, or to the main menu it was opened from */
static int search_screen(void *param)
{
    int screen = music_search_screen(param);
    if (screen == GO_TO_DBBROWSER)
        screen = search_back;
    if (screen == GO_TO_DBBROWSER || screen == GO_TO_ROOT)
        search_back = GO_TO_DBBROWSER;
    return screen;
}
#endif

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
    return tagtree_shuffle_songs(false) ? GO_TO_WPS : GO_TO_PREVIOUS;
}
static int shuffle_albums(void* param)
{
    (void)param;
    return tagtree_shuffle_songs(true) ? GO_TO_WPS : GO_TO_PREVIOUS;
}
#endif
/* The list to resume is in memory as its files have it: read under the boot
   logo, on Updating Library or by the USB screen, with the main menu alone
   shown since, where nothing writes a playlist's file. Any other screen
   left for and Resume Playback loads it again, as it always did. */
static bool resume_fresh;

/* the list for Resume Playback: playlist_resume()'s answer, without the
   load and its wait when it's there already */
static int resume_list(void)
{
    const bool fresh = resume_fresh;
    resume_fresh = false; /* it plays, or isn't to be played */
    return fresh && playlist_resumed() ? 0 : playlist_resume();
}

static int wpsscrn(void* param)
{
    int ret_val = GO_TO_PREVIOUS;
    int audstatus = audio_status();
    int loaded; /* playlist_resume()'s answer */
    (void)param;
    /* the theme isn't drawn first: Now Playing clears the screen and draws
       it then, if it shows it at all. Drawn whole after a list it took
       140 ms, behind which the slide into Now Playing waited */
    push_activity_without_refresh(ACTIVITY_WPS);

#ifdef HAVE_PITCHCONTROL
    if (!audstatus)
    {
        sound_set_pitch(global_status.resume_pitch);
        dsp_set_timestretch(global_status.resume_speed);
    }
#endif

    if (audstatus)
    {
        resume_fresh = false; /* nothing to resume: it plays */
        talk_shutup();
        ret_val = gui_wps_show();
    }
    else if (global_status.resume_index != -1)
    {
        DEBUGF("Resume index %d crc32 %lX offset %lX\n",
               global_status.resume_index,
               (unsigned long)global_status.resume_crc32,
               (unsigned long)global_status.resume_offset);
        loaded = resume_list();
        if (loaded != -1)
        {
            /* its songs may be gone since the boot before, as through
               another disk mode; not for a list loaded in part */
            if (loaded >= 0)
                playlist_check_resume();
            if (global_status.resume_index == -1 || playlist_amount() <= 0)
                splash(HZ*2, ID2P(LANG_NOTHING_TO_RESUME));
            else
            {
                playlist_resume_track(global_status.resume_index,
                    global_status.resume_crc32,
                    global_status.resume_elapsed,
                    global_status.resume_offset);
                ret_val = gui_wps_show();
            }
        }
    }
    else if (!file_exists(PLAYLIST_CONTROL_FILE))
        splash(HZ*2, ID2P(LANG_NOTHING_TO_RESUME));
    /* a list played to its end starts again from the top, with no question */
    else if ((loaded = resume_list()) != -1)
    {
        if (loaded >= 0)
            playlist_check_resume();
        if (playlist_amount() <= 0)
            splash(HZ*2, ID2P(LANG_NOTHING_TO_RESUME));
        else
        {
            playlist_start(0, 0, 0);
            ret_val = gui_wps_show();
        }
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
    [GO_TO_SHUFFLE_ALBUMS] = { shuffle_albums, NULL, NULL },
#endif
#ifdef HAVE_PHOTOS_MENU
    [GO_TO_PHOTOS] = { photos, NULL, NULL },
#endif
#ifdef HAVE_TAGCACHE
    [GO_TO_SEARCH] = { search_screen, NULL, NULL },
    [GO_TO_COVER_FLOW] = { cover_flow_screen, NULL, NULL },
    [GO_TO_ARTISTS] = { music_section, (void*)LANG_ARTISTS, NULL },
    [GO_TO_ALBUMS] = { music_section, (void*)LANG_ALBUMS, NULL },
    [GO_TO_SONGS] = { music_section, (void*)LANG_SONGS, NULL },
    [GO_TO_PODCASTS] = { music_section, (void*)LANG_PODCASTS, NULL },
    [GO_TO_GENRES] = { music_section, (void*)LANG_GENRES, NULL },
    [GO_TO_COMPOSERS] = { music_section, (void*)LANG_COMPOSERS, NULL },
    [GO_TO_AUDIOBOOKS] = { music_section, (void*)LANG_AUDIOBOOKS, NULL },
#endif

};
#define NUM_ITEMS (int)(sizeof(items)/sizeof(*items))

#ifdef HAVE_TAGCACHE
static int items_section_lang(int screen)
{
    return (intptr_t)items[screen].param;
}
#endif

static int item_callback(int action,
                         const struct menu_item_ex *this_item,
                         struct gui_synclist *this_list);

MENUITEM_RETURNVALUE(shortcut_menu, ID2P(LANG_SHORTCUTS), GO_TO_SHORTCUTMENU,
                        NULL, Icon_Bookmark);

MENUITEM_RETURNVALUE(file_browser, ID2P(LANG_DIR_BROWSER), GO_TO_FILEBROWSER,
                        NULL, Icon_file_view_menu);
#ifdef HAVE_TAGCACHE
MENUITEM_RETURNVALUE(db_browser, ID2P(LANG_MUSIC), GO_TO_DBBROWSER,
                        item_callback, Icon_Audio);
MENUITEM_RETURNVALUE(shuffle_songs_item, ID2P(LANG_SHUFFLE_SONGS),
                        GO_TO_SHUFFLE_SONGS, item_callback, Icon_Audio);
MENUITEM_RETURNVALUE(shuffle_albums_item, ID2P(LANG_SHUFFLE_ALBUMS),
                        GO_TO_SHUFFLE_ALBUMS, item_callback, Icon_Audio);
/* Music's sections, in the main menu if Settings > Main Menu has them; Playlists
   is the item below */
MENUITEM_RETURNVALUE(cover_flow_item, ID2P(LANG_COVER_FLOW), GO_TO_COVER_FLOW,
                        item_callback, Icon_Audio);
MENUITEM_RETURNVALUE(search_item, ID2P(LANG_SEARCH), GO_TO_SEARCH,
                        item_callback, Icon_Audio);
MENUITEM_RETURNVALUE(artists_item, ID2P(LANG_ARTISTS), GO_TO_ARTISTS,
                        item_callback, Icon_Audio);
MENUITEM_RETURNVALUE(albums_item, ID2P(LANG_ALBUMS), GO_TO_ALBUMS,
                        item_callback, Icon_Audio);
MENUITEM_RETURNVALUE(songs_item, ID2P(LANG_SONGS), GO_TO_SONGS,
                        item_callback, Icon_Audio);
MENUITEM_RETURNVALUE(podcasts_item, ID2P(LANG_PODCASTS), GO_TO_PODCASTS,
                        item_callback, Icon_Audio);
MENUITEM_RETURNVALUE(genres_item, ID2P(LANG_GENRES), GO_TO_GENRES,
                        item_callback, Icon_Audio);
MENUITEM_RETURNVALUE(composers_item, ID2P(LANG_COMPOSERS), GO_TO_COMPOSERS,
                        item_callback, Icon_Audio);
MENUITEM_RETURNVALUE(audiobooks_item, ID2P(LANG_AUDIOBOOKS), GO_TO_AUDIOBOOKS,
                        item_callback, Icon_Audio);
#endif
MENUITEM_RETURNVALUE(rocks_browser, ID2P(LANG_PLUGINS), GO_TO_BROWSEPLUGINS,
                        NULL, Icon_Plugin);
MENUITEM_RETURNVALUE(extras_item, ID2P(LANG_EXTRAS), GO_TO_EXTRAS,
                        item_callback, Icon_Plugin);
#ifdef HAVE_PHOTOS_MENU
MENUITEM_RETURNVALUE(photos_item, ID2P(LANG_PHOTOS), GO_TO_PHOTOS,
                        item_callback, Icon_Plugin);
#endif

static char *get_wps_item_name(int selected_item, void * data,
                               char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    if (audio_status())
        return ID2P(LANG_NOW_PLAYING);
    return ID2P(LANG_RESUME_PLAYBACK);
}
MENUITEM_RETURNVALUE_DYNTEXT(wps_item, GO_TO_WPS, item_callback, get_wps_item_name,
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
                     item_callback, Icon_Playlist);
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
    { "shuffle_albums", &shuffle_albums_item },
    { "cover_flow", &cover_flow_item },
    { "search", &search_item },
    { "artists", &artists_item },
    { "albums", &albums_item },
    { "songs", &songs_item },
    { "podcasts", &podcasts_item },
    { "genres", &genres_item },
    { "composers", &composers_item },
    { "audiobooks", &audiobooks_item },
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

static void root_menu_parse(void* setting, char *value)
{
    char *next = value, *start, *end;
    unsigned int menu_item_count = 0, i;
    bool main_menu_added = false;

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
    /* Only the items a stock iPod has are in the menu, and in its order:
       Music's sections after it, each there only if Settings > Main Menu has
       it (item_callback) */
#ifdef HAVE_TAGCACHE
    char order[] = "bookmarks, database, cover_flow, search, playlists, artists, "
                   "albums, songs, podcasts, genres, composers, audiobooks, "
                   "photos, extras, settings, shuffle_songs, shuffle_albums, wps";
#else
    char order[] = "bookmarks, database, photos, extras, settings, shuffle_songs, shuffle_albums, wps";
#endif
    (void)defaultval;

    root_menu_parse(setting, order);
    *(bool*)setting = false;
}

/* The menu is always a stock iPod's, with nothing here to change it: an order
   Rockbox saved, coming from it with its settings, had Files, Plugins and
   System, and no Photos, Extras or Shuffle Songs */
void root_menu_load_from_cfg(void* setting, char *value)
{
    (void)value;
    root_menu_set_default(setting, NULL);
}

bool root_menu_is_changed(void* setting, void* defaultval)
{
    (void)defaultval;
    return *(bool*)setting;
}

#ifdef HAVE_TAGCACHE
/* The items Settings > Main Menu turns on or off, by its rows */
static const struct {
    const struct menu_item_ex *item;
    int row;
} main_menu_rows[] = {
    { &db_browser, MAIN_MENU_MUSIC },
    { &cover_flow_item, MAIN_MENU_SECTION + 0 },
    { &search_item, MAIN_MENU_SECTION + 1 },
    { &playlists, MAIN_MENU_SECTION + 2 },
    { &artists_item, MAIN_MENU_SECTION + 3 },
    { &albums_item, MAIN_MENU_SECTION + 4 },
    { &songs_item, MAIN_MENU_SECTION + 5 },
    { &podcasts_item, MAIN_MENU_SECTION + 6 },
    { &genres_item, MAIN_MENU_SECTION + 7 },
    { &composers_item, MAIN_MENU_SECTION + 8 },
    { &audiobooks_item, MAIN_MENU_SECTION + 9 },
#ifdef HAVE_PHOTOS_MENU
    { &photos_item, MAIN_MENU_PHOTOS },
#endif
    { &extras_item, MAIN_MENU_EXTRAS },
    { &shuffle_songs_item, MAIN_MENU_SHUFFLE_SONGS },
};

/* Whether the main menu has it: as Main Menu says, and a section only if
   tagnavi.config has it, Music only with a section of its own to show */
static bool main_menu_has(const struct menu_item_ex *item)
{
    if (item == &shuffle_albums_item)
        return global_settings.main_menu_shuffle_albums;
    for (unsigned i = 0; i < ARRAYLEN(main_menu_rows); i++)
    {
        if (main_menu_rows[i].item != item)
            continue;
        const int row = main_menu_rows[i].row;
        if (!global_settings.main_menu_show[row])
            return false;
        if (row == MAIN_MENU_MUSIC)
            return !tagtree_music_empty();
        if (row >= MAIN_MENU_SECTION && row < MAIN_MENU_SECTION + MUSIC_SECTIONS)
            return tagtree_has_section(tagtree_section_lang[row - MAIN_MENU_SECTION]);
        return true;
    }
    return true;
}

/* the screen in the main menu, or the main menu itself if it isn't */
static int main_menu_screen(int screen)
{
    for (unsigned i = 0; i < ARRAYLEN(main_menu_rows); i++)
    {
        if (main_menu_rows[i].item->value == screen)
            return main_menu_has(main_menu_rows[i].item) ? screen : GO_TO_ROOT;
    }
    return screen;
}
#endif

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
            else if (this_item == &wps_item)
            {
                /* as Now Playing on a stock iPod: none with nothing to play,
                   rather than Resume Playback that plays nothing */
                if (!audio_status() && global_status.resume_index == -1
                    && !file_exists(PLAYLIST_CONTROL_FILE))
                    return ACTION_EXIT_MENUITEM;
            }
#ifdef HAVE_TAGCACHE
            else if (!main_menu_has(this_item))
                return ACTION_EXIT_MENUITEM;
#endif
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
    bool is_action = (screen == GO_TO_SHUFFLE_SONGS
                      || screen == GO_TO_SHUFFLE_ALBUMS);
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
#ifdef HAVE_TAGCACHE
    else if (new_screen == GO_TO_SEARCH) /* what was typed is gone */
        new_screen = GO_TO_DBBROWSER;
#endif
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
#ifdef HAVE_TAGCACHE
    /* Music or a section of it only if the main menu has it now; Cover Flow,
       a plugin, not at all, as Photos */
    if (new_screen == GO_TO_COVER_FLOW)
        new_screen = GO_TO_ROOT;
    else if (new_screen == GO_TO_DBBROWSER || is_section_screen(new_screen))
        new_screen = main_menu_screen(new_screen);
#endif
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
        case TAGCACHE_IMPORT_CHECKING:
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
    const bool same_screen = text == shown_text
        && TIME_BEFORE(current_tick, shown_at + HZ);
    if (same_screen && percent == shown_percent)
        return;
    shown_text = text;
    shown_percent = percent;
    if (!same_screen)
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
    if (!same_screen)
    {
        lcd_set_background(LCD_BLACK);
        lcd_clear_display();
#ifdef STOCK_BOOT_LOGO
        lcd_bmp(&bm_bootlogo, (LCD_WIDTH - BMPWIDTH_bootlogo) / 2, y);
#endif
    }
#ifdef STOCK_BOOT_LOGO
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
    if (same_screen)
    {
        /* its many small steps were a good part of the update's time */
        lcd_update_rect(x, y, bar_w, bar_h);
        return;
    }

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
    /* a step of its own, also when shown from 0 under the same words: Search's
       letters and the library's playlists are both Updating Library */
    if (shown_text != text || shown_percent < percent)
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

/* the covers of songs outside Finder's folder, of albums done of total */
void library_update_covers_progress(int done, int total)
{
    library_update_forward((const char *)str(LANG_PREPARING_COVERS), done, total);
}
#endif

void library_update_songs_progress(int done, int total)
{
    library_update_forward((const char *)str(LANG_PREPARING_SONGS), done, total);
}

/* Search's letters, as Updating Library */
static void library_letters_progress(int done, int total)
{
    library_update_forward((const char *)str(LANG_UPDATING_LIBRARY), done, total);
}

/* USB or power off, come as Cover Flow's plugin made the covers: it stops for
   it, its files closed, and it's answered then. Waited for, the computer saw
   no disk for as long as the covers took. */
static long held_event;

void library_update_hold(long event)
{
    held_event = event;
}

/* the same for one come as the library's playlists were indexed: only the
   events, as get_action() would have the theme draw its bar over the screen */
static bool library_playlists_go_on(void)
{
    const long event = button_get(false);
    if (event == SYS_USB_CONNECTED || event == SYS_POWEROFF
        || event == SYS_REBOOT)
        held_event = event;
    else if (IS_SYSEVENT(event))
        default_event_handler(event);
    return !held_event;
}

/* the bytes of text's first line in width: up to a space, or the characters
   that fit in a line with none, as in Chinese */
static int line_bytes(const unsigned char *text, int width)
{
    int w, h;
    int fit = font_measurestring(text, -1, width, &w, &h, FONT_UI);
    if (text[fit] == '\0')
        return fit;
    for (int i = fit; i > 0; i--)
        if (text[i] == ' ')
            return i;
    if (fit > 0)
        return fit;
    do /* one character at least */
        fit++;
    while ((text[fit] & 0xc0) == 0x80);
    return fit;
}

/* Not Enough Space under the boot logo until a button is pressed, the theme
   off */
static void library_no_space_show(void)
{
    const unsigned char *title = str(LANG_NOT_ENOUGH_SPACE);
    const unsigned char *text = str(LANG_FREE_UP_SPACE);
    const int width = LCD_WIDTH - 2 * 20;
    const unsigned char *lines[4];
    int lens[4], n = 0;
    while (*text && n < 4)
    {
        lines[n] = text;
        lens[n] = line_bytes(text, width);
        text += lens[n++];
        while (*text == ' ')
            text++;
    }

    lcd_set_viewport(NULL);
#if LCD_DEPTH > 1
    lcd_set_backdrop(NULL);
#endif
    lcd_set_background(LCD_BLACK);
    lcd_clear_display();
    int w, h;
    lcd_setfont(FONT_UI);
    lcd_getstringsize(title, &w, &h);
    /* where Updating Library's bar is, the logo higher for more lines */
#ifdef STOCK_BOOT_LOGO
    int y = MIN(STOCK_BOOT_LOGO_Y + BMPHEIGHT_bootlogo + 22,
                LCD_HEIGHT - 8 - (n + 1) * h - 4);
    lcd_bmp(&bm_bootlogo, (LCD_WIDTH - BMPWIDTH_bootlogo) / 2,
            MIN(STOCK_BOOT_LOGO_Y, y - 10 - BMPHEIGHT_bootlogo));
#else
    int y = MIN((LCD_HEIGHT - 96) / 2 + 96 + 22,
                LCD_HEIGHT - 8 - (n + 1) * h - 4);
#endif
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(LCD_WHITE);
    lcd_putsxy((LCD_WIDTH - w) / 2, y, title);
    y += h + 4;
    lcd_set_foreground(LCD_RGBPACK(160, 160, 160));
    for (int i = 0; i < n; i++, y += h)
    {
        char line[128];
        strmemccpy(line, lines[i], MIN(lens[i] + 1, (int)sizeof(line)));
        lcd_putsxy((LCD_WIDTH - font_getstringsize(line, NULL, NULL, FONT_UI))
                   / 2, y, line);
    }
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_update();
    backlight_on();
    shown_text = NULL; /* Updating Library's screen drawn whole again */

    while (true)
    {
        const long button = button_get(true);
        if (button == SYS_USB_CONNECTED || button == SYS_POWEROFF
            || button == SYS_REBOOT)
        {
            held_event = button;
            return;
        }
        if (IS_SYSEVENT(button))
            default_event_handler(button);
        /* a press, not the release of one before nor a turn of the wheel */
        else if (button != BUTTON_NONE
                 && !(button & (BUTTON_REL | BUTTON_REPEAT))
#ifdef BUTTON_SCROLL_FWD
                 && !(button & (BUTTON_SCROLL_FWD | BUTTON_SCROLL_BACK))
#endif
                )
            break;
    }
    /* its release and repeats aren't the menu's; the system's events are
       answered, as Play held to switch off */
    long button;
    do
    {
        button = button_get_w_tmo(HZ / 50);
        if (button == SYS_USB_CONNECTED || button == SYS_POWEROFF
            || button == SYS_REBOOT)
        {
            held_event = button;
            return;
        }
        if (IS_SYSEVENT(button))
            default_event_handler(button);
    }
    while (button != BUTTON_NONE || button_status() != BUTTON_NONE);
}

#ifdef ITUNESDB_HAVE_ART
/* whether Cover Flow's album list is of the database: its commit, entries
   and albums' file size, as pictureflow.c's load_album_index() checks them */
static bool album_list_current(void)
{
    struct tagcache_stat *stat = tagcache_get_stat();
    int32_t head[4];
    int fd = open(ITUNESDB_ALBUM_INDEX, O_RDONLY);
    bool current = fd >= 0 && read(fd, head, sizeof(head)) == sizeof(head)
        && !memcmp(head, "PFIV", 4) && head[1] == tagcache_get_commitid()
        && head[2] == stat->total_entries;
    if (fd >= 0)
        close(fd);
    if (current)
    {
        char file[MAX_PATH];
        snprintf(file, sizeof(file), "%s/database_%d.tcd", stat->db_path,
                 tag_album);
        fd = open(file, O_RDONLY);
        current = fd >= 0 && ffilesize(fd) == head[3];
        if (fd >= 0)
            close(fd);
    }
    return current;
}
#else
#define album_list_current() true
#endif

/* After a sync the database takes in the new songs before the menu shows,
 * as a stock iPod has them as soon as it starts: the boot logo stays, with
 * the step and how far that is under it, until it's done. Menu left it to
 * finish behind the menu, where it took the memory of music started then,
 * which stopped, and the caches made here weren't. */
void show_library_update(void)
{
    int done, total, phase;
    bool shown = false;
    /* The check after USB of the songs outside Finder's folder shows only once
       it takes long or finds a change: a connection that copied none is over
       before the screen would come */
    const int commitid = tagcache_get_commitid();
    /* the database Cover Flow's albums and the songs were made for: a check
       cut short by USB after it had changed the database left them, and the
       one after found nothing more */
    static int prepared_commitid = -1;
    if (prepared_commitid < 0)
        prepared_commitid = commitid; /* as the boot has them */
    const long quiet_until = current_tick + HZ / 2;
    bool checked_only = true;
    while ((phase = tagcache_get_import(&done, &total))
           != TAGCACHE_IMPORT_NONE)
    {
        if (phase != TAGCACHE_IMPORT_CHECKING)
            checked_only = false;
        if (!shown && phase == TAGCACHE_IMPORT_CHECKING
            && tagcache_get_commitid() == commitid
            && TIME_BEFORE(current_tick, quiet_until))
        {
            /* only the events, as USB: get_action() had the theme draw its
               status bar over the screen left */
            const long event = button_get_w_tmo(HZ / 20);
            if (IS_SYSEVENT(event)
                && default_event_handler(event) == SYS_USB_CONNECTED)
                break;
            continue;
        }
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
        if (default_event_handler(action) == SYS_USB_CONNECTED)
            break;
    }
    /* left for want of room: nothing made of it either */
    bool no_space = phase == TAGCACHE_IMPORT_NONE && tagcache_take_no_space();
    /* a check that took long but found nothing has nothing to make again, nor
       an import that changed neither the database nor the covers, with what
       they make made for this database */
    const bool imported = phase == TAGCACHE_IMPORT_NONE && !no_space
        && ((shown && !checked_only
             && !(itunesdb_import_kept_art() && album_list_current()
                  && tagtree_songs_prepared()))
            || tagcache_get_commitid() != prepared_commitid);
    /* Finder may sync photos only, so this goes by their own database */
    const bool photos_changed = phase == TAGCACHE_IMPORT_NONE && photos_panel_stale();
    /* the letters of the tags' alphabets, which Search types with */
    bool letters_stale = phase == TAGCACHE_IMPORT_NONE
                         && music_search_letters_stale();
#ifdef ITUNESDB_HAVE_ART
    /* the covers of songs outside Finder's folder, not made yet, or cut short
       by Menu or USB: by Cover Flow's plugin, without which nothing's made,
       from the database in memory, without which it finds no album */
    const struct tagcache_stat *tc = tagcache_get_stat();
    const bool covers_stale = phase == TAGCACHE_IMPORT_NONE
                              && global_settings.album_art != AA_OFF
                              && tc->ready && tc->ramcache
                              && file_exists(PLUGIN_DEMOS_DIR "/pictureflow.rock")
                              && (itunesdb_covers_stale()
                                  || itunesdb_covers_images_changed());
    /* and the covers of the lists' rows, made of Cover Flow's slides */
    const bool row_covers_stale = phase == TAGCACHE_IMPORT_NONE
                                  && list_covers_stale();
#else
    const bool covers_stale = false;
    const bool row_covers_stale = false;
#endif
    if ((imported || covers_stale || row_covers_stale || photos_changed
         || letters_stale) && !shown)
    {
        FOR_NB_SCREENS(i)
            viewportmanager_theme_enable(i, false, NULL);
        shown_text = NULL;
        shown = true;
    }
#ifdef ITUNESDB_HAVE_ART
    /* the album list of the new database, which Cover Flow itself makes:
       it isn't left for when Cover Flow opens; and the covers after it */
    if ((imported || covers_stale) && phase == TAGCACHE_IMPORT_NONE
        && !no_space && file_exists(PLUGIN_DEMOS_DIR "/pictureflow.rock")
        && itunesdb_covers_room())
    {
        library_update_show(str(LANG_PREPARING_ALBUMS), 0);
        plugin_load(PLUGIN_DEMOS_DIR "/pictureflow.rock", ITUNESDB_ALBUM_LIST);
    }
#endif
    /* the rest after USB or switching off, come meanwhile: what they'd go
       by is of before it */
    /* the albums' years, which the lists of an artist's albums go by: before
       Songs is prepared, which is kept as they're made */
    if (phase == TAGCACHE_IMPORT_NONE && !held_event && !album_years_ready()
        && album_years_make())
    {
        tagtree_albums_reordered();
        /* the lists' covers, by the albums' numbers in it */
        list_covers_load();
        /* and Search's names, its albums by those numbers, if they're of
           another table, which wasn't made as they were written */
        if (!letters_stale && music_search_letters_stale())
        {
            letters_stale = true;
            if (!shown)
            {
                FOR_NB_SCREENS(i)
                    viewportmanager_theme_enable(i, false, NULL);
                shown_text = NULL;
                shown = true;
            }
        }
    }
#ifdef ITUNESDB_HAVE_ART
    /* those covers made, after the albums' years they're in the order of, and
       Cover Flow's plugin above, which may have made some again */
    if (phase == TAGCACHE_IMPORT_NONE && !held_event && !no_space
        && album_years_ready() && (row_covers_stale || list_covers_stale()))
    {
        library_update_show(str(LANG_PREPARING_COVERS), 0);
        list_covers_make(library_update_covers_progress, library_playlists_go_on);
    }
#endif
    /* Songs' playlist in the order chosen, if no import nor a song played
       wrote it, and its orders' file: after the albums' years they go by */
    const bool songs_stale = phase == TAGCACHE_IMPORT_NONE && !held_event
                             && !imported && !no_space && tagtree_songs_stale();
    if (songs_stale && !shown)
    {
        FOR_NB_SCREENS(i)
            viewportmanager_theme_enable(i, false, NULL);
        shown_text = NULL;
        shown = true;
    }
    if ((imported || songs_stale) && phase == TAGCACHE_IMPORT_NONE && !held_event)
        tagtree_prepare_songs();
    /* not by a database the caches couldn't be made for */
    no_space |= library_take_no_room();
    if (phase == TAGCACHE_IMPORT_NONE && !held_event && !no_space)
        prepared_commitid = tagcache_get_commitid();
    if (letters_stale && !held_event)
    {
        library_update_show(str(LANG_UPDATING_LIBRARY), 0);
        music_search_letters_make(library_letters_progress);
    }
    /* the library's playlists, as Songs is: their lists of songs, and those of
       the live smart ones by ratings and plays as they are, ready before they
       open, which would wait for them. Under what shows, a second at most:
       the files of the smart ones are written after a sync, by the import,
       or as one starts to play. */
    if (phase == TAGCACHE_IMPORT_NONE && !held_event)
    {
        if (shown && !letters_stale)
            library_update_show(str(LANG_UPDATING_LIBRARY), 0);
        smart_update_at_boot();
        /* the marks of the synced ones, as this import left them */
        ip_synced_load();
        lp_index_all(library_playlists_go_on);
    }
    /* The list to resume, read again after all that may have written a
       playlist's file, while this or the boot's logo shows and the disk
       spins: Resume Playback then starts at once. For the main menu, which
       knows nothing wrote one since; not with USB or a switch off waiting */
    playlist_resume_forget();
    if (!held_event && get_current_activity() == ACTIVITY_MAINMENU)
        playlist_preload();
    no_space |= library_take_no_room();
    if (no_space && !held_event && !shown)
    {
        FOR_NB_SCREENS(i)
            viewportmanager_theme_enable(i, false, NULL);
        shown = true;
    }
    if (shown)
    {
        if (photos_changed && !held_event) /* else made the next time */
        {
            library_update_show(str(LANG_PREPARING_PHOTOS), 0);
            photos_panel_update();
            skin_reload_while_playing();
        }
        no_space |= library_take_no_room();
        if (no_space && !held_event)
            library_no_space_show();
        lcd_set_background(global_settings.bg_color);
        lcd_set_foreground(global_settings.fg_color);
        lcd_clear_display();
        FOR_NB_SCREENS(i)
            viewportmanager_theme_undo(i, true);
    }
    if (held_event)
    {
        const long event = held_event;
        held_event = 0;
        default_event_handler(event);
    }
}
#endif /* HAVE_TAGCACHE */

#ifdef HAVE_TC_RAMCACHE
/* The database in memory before the menu shows, as the dircache, under the
   boot logo: loaded behind the menu, once its thread had been idle a second,
   a list opened then waited for it on "Wait". Before Updating Library, which
   loads it itself after an import; the screen stays on its last frame
   meanwhile. True when
   USB came meanwhile, and the check of the songs after it with it. */
static bool database_ram_wait(void)
{
    bool usb = false;
    if (!tagcache_ram_pending())
        return false;
    tagcache_ram_load_now();
    long start = current_tick;
    while (tagcache_ram_pending())
    {
        if (TIME_AFTER(current_tick, start + 20 * HZ))
        {
            tagtree_ram_waited_out(); /* nor the lists after, 20 s more */
            break;
        }
        /* only the events, as USB: get_action() had the theme draw its status
           bar and panel ("Calculating...") over the boot logo */
        const long event = button_get_w_tmo(HZ / 10);
        if (IS_SYSEVENT(event)
            && default_event_handler(event) == SYS_USB_CONNECTED)
        {
            /* the load USB stopped, again, and waited for: the menu came
               without it */
            tagcache_ram_load_now();
            start = current_tick;
            usb = true;
        }
    }
    return usb;
}
#endif

static bool library_update_pending = false;

void library_update_after_usb(void)
{
    library_update_pending = true;
}

void root_menu(void)
{
    int previous_browser = global_status.last_browser;
    int selected = 0;
    int shortcut_origin = GO_TO_ROOT;
    bool root_usb = false; /* the main menu itself was left for USB */

    push_current_activity(ACTIVITY_MAINMENU);
#ifdef HAVE_DIRCACHE
    /* The tree of the files is in memory before the menu shows, as every cache
       is, under the boot logo: in a second, as nothing else wants the CPU. Built
       behind Updating Library, the songs opened from the menu waited on
       "Loading..." for it. */
    dircache_wait();
#endif
    bool usb_checked = false;
#ifdef HAVE_TC_RAMCACHE
    usb_checked = database_ram_wait();
#endif
#ifdef HAVE_TAGCACHE
    /* the songs outside Finder's folder, as after USB: the check there was
       lost to a reset or a switch off right after it, and the songs copied
       waited for the next connection. Not again after one that USB, come
       meanwhile, was followed by. */
    if (!usb_checked)
        tagcache_check_sync();
    show_library_update();
#endif
#ifdef ITUNESDB_HAVE_ART
    /* the cover of the song to resume, by the library as it is now */
    menu_cover_resume_look();
#endif
#ifndef HAVE_TAGCACHE
    playlist_preload();
#endif
    /* until a screen other than the main menu is the first */
    resume_fresh = playlist_resumed();
#ifdef STOCK_BOOT_LOGO
    /* the theme the boot turned off, now that the menu shows */
    FOR_NB_SCREENS(i)
        viewportmanager_theme_undo(i, false);
#endif
#ifdef SIMULATOR
    /* RBSIM_SCRIPT's keys from now on, not to the boot's waits for USB, which
       take them and drop them (button-sdl.c) */
    extern volatile bool sim_boot_done;
    sim_boot_done = true;
#endif
    /* with no settings yet, the language is asked for first, as on a stock iPod */
    if (settings_first_start())
        language_screen();
    next_screen = root_menu_setup_screens();

    while (true)
    {
#ifdef HAVE_TAGCACHE
        /* the import begins as it shows, not behind whatever was open */
        /* not under music, started meanwhile: an import's memory stops it,
           and the check waits for it */
        if (library_update_pending && !(audio_status() & AUDIO_STATUS_PLAY))
        {
            library_update_pending = false;
            if (tagcache_check_sync())
            {
                show_library_update();
                /* read there just now */
                resume_fresh = playlist_resumed();
            }
#ifdef ITUNESDB_HAVE_ART
            menu_cover_resume_look();
#endif
        }
        /* the albums' years, if USB during Updating Library left them
           unmade, or the database changed since: as the menu shows, in a
           moment, not while music plays */
        if (!album_years_ready() && tagcache_finds_entries()
            && !(audio_status() & AUDIO_STATUS_PLAY) && album_years_make())
        {
            tagtree_albums_reordered();
            list_covers_load();
        }
#endif
        /* another screen may write a playlist's file; not the Play button's
           way to Now Playing, which is Resume Playback's */
        if (next_screen != GO_TO_ROOT && next_screen != GO_TO_WPS
            && !(next_screen == GO_TO_PREVIOUS_MUSIC
                 && previous_music == GO_TO_WPS)
            && next_screen != MENU_ATTACHED_USB
            && next_screen != MENU_SELECTED_EXIT)
            resume_fresh = false;
        switch (next_screen)
        {
            case MENU_ATTACHED_USB:
            case MENU_SELECTED_EXIT:
                /* fall through */
            case GO_TO_ROOT:
                if (last_screen != GO_TO_ROOT)
                    selected = get_selection(last_screen);
                /* USB came to the main menu itself: the list is as the USB
                   screen, or Updating Library after it, has just read it */
                if (root_usb)
                    resume_fresh = !audio_status() && playlist_resumed();
                root_usb = false;
                /* the song to resume gone over USB: its list, loaded again
                   as Resume Playback does unless it's there as just read,
                   for the next song still there, or for none and no Resume
                   Playback */
                if (!audio_status() && playlist_resume_gone())
                {
                    if ((resume_fresh && playlist_resumed())
                        || playlist_resume() >= 0)
                        playlist_check_resume();
                    resume_fresh = playlist_resumed(); /* or read just now */
                }
                global_status.last_screen = GO_TO_ROOT; /* We've returned to ROOT */
                /* When we are in the main menu we want the hardware BACK
                 * button to be handled by HOST instead of rockbox */
                ignore_back_button_stub(true);

                next_screen = do_menu(&root_menu_, &selected, NULL, false);
                root_usb = next_screen == MENU_ATTACHED_USB;
#ifdef HAVE_TAGCACHE
                music_from_main_menu = next_screen == GO_TO_DBBROWSER;
                if (next_screen == GO_TO_SEARCH)
                {
                    /* Search added to the main menu, which Menu goes back to */
                    music_search_new();
                    search_back = GO_TO_ROOT;
                }
#endif

                ignore_back_button_stub(false);

                if (next_screen != GO_TO_PREVIOUS)
                    last_screen = GO_TO_ROOT;
                break;
#ifdef HAVE_TAGCACHE
            case GO_TO_DBBROWSER:
#endif
            case GO_TO_FILEBROWSER:
            case GO_TO_PLAYLISTS_SCREEN:
#ifdef HAVE_TAGCACHE
            case GO_TO_SEARCH: /* Menu in Now Playing comes back to it */
            case GO_TO_ARTISTS: case GO_TO_ALBUMS: case GO_TO_SONGS:
            case GO_TO_PODCASTS: case GO_TO_GENRES: case GO_TO_COMPOSERS:
            case GO_TO_AUDIOBOOKS:
#endif
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
                /* gone there as the screen went dark: Menu back here */
                wps_after_hold = !action_dark_return_taken();
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
