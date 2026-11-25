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
#include "health_log.h"
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
#include "gui/ui_theme.h"

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
#include "free_space.h"
#ifdef HAVE_TAGCACHE
#include "music_search.h"
#include "album_years.h"
#include "list_covers.h"
#include "artist_rows.h"
#endif
#include "hang.h"

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
#endif

/* The history (the owner's rule, 2026-11-25): the screens Menu goes back
   through, one at a time, to the main menu under them all, the newest last,
   each with what it opens again as it was left: a list its place
   (tagtree_nav_save(), by the entry's index), Cover Flow what it said as it
   quit (root_menu_cover_flow_left()), the playlist playing's screen its file.
   Every one is opened by the root loop, none inside another, so the stack
   doesn't grow with it. At most NAV_MAX: past that the oldest is forgotten.
   As one is added, a tail of two blocks of the same screens, two or more in
   each, is one: the older block goes, as Music, Now Playing, Cover Flow, Now
   Playing, Cover Flow, Now Playing ends as Music, Now Playing, Cover Flow,
   Now Playing, the newest of each kept. The same screen by its kind: Cover
   Flow at any album, a list of the same kind (tagtree_list_type()) */
#ifdef HAVE_TAGCACHE
#define NAV_MAX TAGTREE_NAV_SLOTS
#else
#define NAV_MAX 16
#endif
enum { NAV_SCREEN, NAV_LISTS, NAV_COVER_FLOW, NAV_PLAYLIST };
/* the playlist playing's screen, from Now Playing's Playing From: a screen of
   the root loop's own (load_next_screen), not one of the main menu's */
#define GO_TO_PLAYLIST_SONGS (-101)
static struct nav_entry {
    int16_t screen;
    uint8_t kind;
    uint32_t type;
    char state[MAX_PATH];
} nav[NAV_MAX];
static int nav_count;
/* the root loop opens the top one again, as it was left; and it comes from
   Now Playing, gone back from or left */
static bool nav_again, nav_from_wps;
/* the state of the entry added next */
static char nav_fresh[MAX_PATH];

static struct nav_entry *nav_top(void)
{
    return nav_count > 0 ? &nav[nav_count - 1] : NULL;
}

static uint32_t nav_type_of(int screen, int kind)
{
#ifdef HAVE_TAGCACHE
    if (kind == NAV_LISTS)
        return tagtree_list_type();
#endif
    return (uint32_t)(screen & 0xff) << 24 | kind;
}

/* entry i gone, those after it one down */
static void nav_drop(int i)
{
    for (int k = i; k + 1 < nav_count; k++)
    {
        nav[k] = nav[k + 1];
#ifdef HAVE_TAGCACHE
        tagtree_nav_copy(k, k + 1);
#endif
    }
    nav_count--;
#ifdef HAVE_TAGCACHE
    tagtree_nav_clear(nav_count);
#endif
}

/* a tail of two blocks of the same screens: the older goes, again until none */
static void nav_collapse(void)
{
    bool again = true;
    while (again)
    {
        again = false;
        for (int len = 2; 2 * len <= nav_count && !again; len++)
        {
            const int a = nav_count - 2 * len, b = nav_count - len;
            int k = 0;
            while (k < len && nav[a + k].type == nav[b + k].type)
                k++;
            if (k < len)
                continue;
            for (k = 0; k < len; k++)
                nav_drop(a);
            again = true;
        }
    }
}

static void nav_push(int screen, int kind)
{
    if (nav_count == NAV_MAX)
        nav_drop(0);
    struct nav_entry *e = &nav[nav_count++];
    e->screen = screen;
    e->kind = kind;
    e->type = nav_type_of(screen, kind);
    strmemccpy(e->state, nav_fresh, sizeof(e->state));
    nav_fresh[0] = '\0';
#ifdef HAVE_TAGCACHE
    tagtree_nav_clear(nav_count - 1);
#endif
    nav_collapse();
}

static void nav_clear(void)
{
    while (nav_count > 0)
        nav_drop(nav_count - 1);
}

/* The top one closed by Menu: the screen to open again under it, the main
   menu under them all */
static int nav_back(void)
{
    if (nav_count > 0)
        nav_drop(nav_count - 1);
    const struct nav_entry *e = nav_top();
    if (!e)
        return GO_TO_ROOT;
    nav_again = true;
    return e->screen;
}

static int nav_kind_of(int screen)
{
#ifdef HAVE_TAGCACHE
    if (screen == GO_TO_DBBROWSER || (screen >= GO_TO_ARTISTS
                                      && screen <= GO_TO_COMPILATIONS))
        return NAV_LISTS;
    if (screen == GO_TO_COVER_FLOW)
        return NAV_COVER_FLOW;
#endif
    if (screen == GO_TO_PLAYLIST_SONGS)
        return NAV_PLAYLIST;
    return NAV_SCREEN;
}

/* A screen asked for from within another, the root loop's to open over it,
   once that one has left as for Now Playing (root_menu_leave_asked()) */
enum {
    ASK_NONE, ASK_COVER_FLOW, ASK_UP_NEXT, ASK_PLAYLIST, ASK_PLAYING, ASK_FROM,
    ASK_TRACK, ASK_SONG, ASK_NAME, ASK_ALBUM,
};
static struct {
    int what;
    bool artist;
    int idx, tag;
    int32_t key;
    char text[MAX_PATH];
} asked;

static void ask(int what, bool artist, const char *text)
{
    asked.what = what;
    asked.artist = artist;
    strmemccpy(asked.text, text ? text : "", sizeof(asked.text));
}

bool root_menu_leave_asked(void)
{
    return asked.what != ASK_NONE;
}

void root_menu_ask_cover_flow(void)
{
    ask(ASK_COVER_FLOW, false, NULL);
}

void root_menu_ask_up_next(void)
{
    ask(ASK_UP_NEXT, false, NULL);
}

void root_menu_ask_playlist(const char *file)
{
    ask(ASK_PLAYLIST, false, file);
}

void root_menu_ask_go_to_playing(bool artist)
{
    ask(ASK_PLAYING, artist, NULL);
}

void root_menu_ask_playing_from(void)
{
    ask(ASK_FROM, false, NULL);
}

void root_menu_ask_go_to_track(const char *path, bool artist)
{
    ask(ASK_TRACK, artist, path);
}

void root_menu_ask_go_to_song(int idx, bool artist)
{
    ask(ASK_SONG, artist, NULL);
    asked.idx = idx;
}

void root_menu_ask_go_to_name(int tag, const char *name)
{
    ask(ASK_NAME, false, name);
    asked.tag = tag;
}

void root_menu_ask_go_to_album(const char *name, int32_t artist)
{
    ask(ASK_ALBUM, false, name);
    asked.key = artist;
}

#ifdef HAVE_TAGCACHE
static bool is_section_screen(int screen)
{
    return screen >= GO_TO_ARTISTS && screen <= GO_TO_COMPILATIONS;
}
#endif

/* The music's origin, which Playing From goes to: started from Music's lists,
   the place there (tagtree_save_place()). Each start once, by the screen it
   was made in; Cover Flow takes its own (cover_flow()), so that the lists
   under it aren't taken for it */
static unsigned int music_starts_seen;

static void note_music_origin(int screen)
{
    const unsigned int starts = playlist_start_count();
    if (starts == music_starts_seen)
        return;
    music_starts_seen = starts;
#ifdef HAVE_TAGCACHE
    if (screen == GO_TO_DBBROWSER || is_section_screen(screen))
        tagtree_save_place(last_db_dirlevel, last_db_selection);
#else
    (void)screen;
#endif
}

/* the song of Cover Flow's Go to Artist, "" for none; offered only while
   cover_flow() has it open, which goes there, and not as a plugin of a file's
   menu or a hotkey opens it */
static char cover_flow_go_to[MAX_PATH];
static bool cover_flow_go_to_offered;

bool root_menu_go_to_later(const char *path)
{
    if (cover_flow_go_to_offered && path)
        strmemccpy(cover_flow_go_to, path, sizeof(cover_flow_go_to));
    return cover_flow_go_to_offered;
}

void root_menu_cover_flow_left(const char *state)
{
    struct nav_entry *e = nav_top();
    if (cover_flow_go_to_offered && e && e->kind == NAV_COVER_FLOW && state)
        strmemccpy(e->state, state, sizeof(e->state));
}

#ifdef HAVE_TAGCACHE
/* Cover Flow, its entry the top one: at the album playing, or with its state
   as it was left (root_menu_cover_flow_left()). Its Go to Artist is the
   lists' entry over it */
static int cover_flow(const char *at)
{
    /* Music's, from wherever it opens, without the status line, which
       the screen it goes back to draws in its own again */
    ui_theme_set_section(UI_SECTION_MUSIC, false);
    cover_flow_go_to[0] = '\0';
    cover_flow_go_to_offered = true;
    const int ret = plugin_load(PLUGIN_DEMOS_DIR "/pictureflow.rock", at);
    cover_flow_go_to_offered = false;
    /* music started there is Cover Flow's, not the lists' it was
       opened over (note_music_origin()) */
    music_starts_seen = playlist_start_count();
    if (ret == PLUGIN_OK && cover_flow_go_to[0])
    {
        root_menu_ask_go_to_track(cover_flow_go_to, true);
        return PLUGIN_GOTO_WPS;
    }
    return ret;
}

/* The lists asked for, at the root, their place set: the entry added for them
   next; false if they weren't found */
static bool go_to_asked(void)
{
    if (!tagtree_ready() || !tagcache_is_usable())
        return false;
    int *const d = &last_db_dirlevel, *const s = &last_db_selection;
    switch (asked.what)
    {
        case ASK_PLAYING:
            return tagtree_go_to_playing(asked.artist, d, s);
        case ASK_FROM:
            return tagtree_go_to_from(d, s);
        case ASK_TRACK:
            return tagtree_go_to_track(asked.text, asked.artist, d, s);
        case ASK_SONG:
            return tagtree_go_to_song(asked.idx, asked.artist, d, s);
        case ASK_NAME:
            return tagtree_go_to_name(asked.tag, asked.text, d, s);
        case ASK_ALBUM:
            return tagtree_go_to_album(asked.text, asked.key, d, s);
    }
    return false;
}
#endif

/* The screen asked for, the one left under it: the screen to open next, a new
   entry; or that one again, if what was asked for isn't there */
static int nav_open_asked(void)
{
    int screen = GO_TO_PREVIOUS;
    switch (asked.what)
    {
#ifdef HAVE_TAGCACHE
        case ASK_COVER_FLOW:
            screen = GO_TO_COVER_FLOW;
            break;
#endif
        case ASK_UP_NEXT:
            screen = GO_TO_PLAYLIST_VIEWER;
            break;
        case ASK_PLAYLIST:
            strmemccpy(nav_fresh, asked.text, sizeof(nav_fresh));
            screen = GO_TO_PLAYLIST_SONGS;
            break;
        default:
#ifdef HAVE_TAGCACHE
            if (go_to_asked())
                screen = GO_TO_DBBROWSER;
#endif
            break;
    }
    asked.what = ASK_NONE;
    if (screen != GO_TO_PREVIOUS)
        return screen;
    const struct nav_entry *e = nav_top();
    if (!e)
        return GO_TO_ROOT;
    nav_again = true;
    return e->screen;
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

                    /* Maybe just needs to reboot due to delayed commit; not
                       one put off for want of memory, which a reboot doesn't
                       give: what can be done instead */
                    if (stat->commit_delayed)
                    {
                        if (tagcache_commit_no_memory())
                            splash(HZ*5, ID2P(LANG_LIBRARY_NO_MEMORY));
                        else
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
            if (!tagcache_is_usable() || !tagtree_ready())
            {
                if (tagcache_is_usable())
                    splashf(HZ*3, "%s\n%s", str(LANG_FILE_NOT_FOUND),
                            ROCKBOX_DIR "/tagnavi.config");
                /* the lists not opened: what was to show in them as they
                   opened goes too */
                tree_select_playing(false);
                return GO_TO_PREVIOUS;
            }
            filter = SHOW_ID3DB;
            last_ft_dirlevel = tc->dirlevel;
            /* their entry's, as they were left; back from Now Playing at the
               song playing, if they show it (tree_select_playing()) */
            if (nav_again)
            {
                tagtree_nav_restore(nav_count - 1, &last_db_dirlevel,
                                    &last_db_selection);
                tree_select_playing(nav_from_wps);
            }
            tc->dirlevel = last_db_dirlevel;
            tc->selected_item = last_db_selection;
            if (music_from_main_menu)
                tagtree_to_top(tc);
            music_from_main_menu = false;
            /* before the status line is drawn as it opens; a level gone to
               sets its own (update_dir()) */
            ui_theme_set_section(tagtree_list_kind(tc), true);
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
            /* as they were left, for their entry to open again so; and no
               go-to's level or section for the next lists */
            if (nav_count > 0 && nav_top()->kind == NAV_LISTS)
            {
                tagtree_nav_save(nav_count - 1, last_db_dirlevel,
                                 last_db_selection);
                nav_top()->type = tagtree_nav_type(nav_count - 1);
            }
            tagtree_end_go_to(false, &last_db_dirlevel, &last_db_selection);
        break;
#endif
    }
    return ret_val;
}

#ifdef HAVE_TAGCACHE
bool root_menu_play_found(int tag, const char *name, int32_t artist)
{
    /* the lists' place as it was, and the file browser's level, which the
       lists have while they're not shown (browser()) */
    struct tree_context *tc = tree_get_context();
    const int dirlevel = tc->dirlevel, selected_item = tc->selected_item;
    const bool played = tagtree_play_found(tag, name, artist, &last_db_dirlevel,
                                           &last_db_selection);
    tc->dirlevel = dirlevel;
    tc->selected_item = selected_item;
    return played;
}

void root_menu_artists_grouping_changed(bool on)
{
    (void)on;
    tagtree_artists_grouping_changed(&last_db_dirlevel, &last_db_selection);
}

void root_menu_compilations_changed(bool on)
{
    (void)on;
    tagtree_compilations_changed(&last_db_dirlevel, &last_db_selection);
}

void root_menu_compilations_check(void)
{
    tagtree_compilations_check(&last_db_dirlevel, &last_db_selection);
}

/* A section of Music in the main menu, as on a stock iPod: its list, where Menu
   goes back to the main menu. Left for Now Playing, it opens again where it was,
   as Music does. */
static int music_section(void *param)
{
    const int lang = (intptr_t)param;
    if (!nav_again
        && !tagtree_go_to_section(lang, &last_db_dirlevel, &last_db_selection))
        return GO_TO_ROOT;
    return browser((void*)GO_TO_DBBROWSER);
}

/* Cover Flow, from the main menu, Music's row or Now Playing: at the album
   playing, or as it was left; Menu there back to the screen under it. From
   Now Playing, Now Playing on the screen until Cover Flow draws, as when it
   opened over Now Playing */
static int cover_flow_screen(void *param)
{
    (void)param;
    const struct nav_entry *e = nav_top();
    const char *at = NULL;
#ifdef ITUNESDB_HAVE_ART
    if (nav_again && e)
        at = e->state[0] ? e->state : ITUNESDB_COVER_FLOW_BACK;
#else
    (void)e;
#endif
    if (nav_from_wps)
        plugin_load_keep_screen();
    const int ret = cover_flow(at);
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

/* Search, whose Menu goes back to the screen under it: Music, or the main
   menu */
static int search_screen(void *param)
{
    const int screen = music_search_screen(param);
    return screen == GO_TO_DBBROWSER ? GO_TO_PREVIOUS : screen;
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
/* the list for Resume Playback: playlist_resume()'s answer, without the
   load and its wait when it's in memory as a load would make it now: read
   under the boot logo, on Updating Library or by the USB screen, whatever
   screens were shown since (playlist_resumed()) */
static int resume_list(void)
{
    return playlist_resumed() ? 0 : playlist_resume();
}

bool now_playing_available(void)
{
    return audio_status() || global_status.resume_index != -1
           || file_exists(PLAYLIST_CONTROL_FILE);
}

static int wpsscrn(void* param)
{
    int ret_val = GO_TO_PREVIOUS;
    int audstatus = audio_status();
    int loaded; /* playlist_resume()'s answer */
    /* opened by Play held in a list, a menu, Cover Flow or Photos, which go
       here then if now_playing_available(), with the music stopped: the song
       paused at its place, which Play plays from. Nothing sounds and the place
       stays, also at the power-off that a hold held on comes to. No button is
       taken on the way here, so the hold is still the last action. */
    const bool paused = !audstatus && action_last_action() == ACTION_TREE_STOP;
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
                if (paused)
                    playlist_resume_track_paused(global_status.resume_index,
                        global_status.resume_crc32,
                        global_status.resume_elapsed,
                        global_status.resume_offset);
                else
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
            if (paused)
                playlist_start_paused(0, 0, 0);
            else
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
    /* back to the Music menu it is in, or the main menu; to the main menu
       anyway as Menu held left it */
    return catalog_left_for_root() ? GO_TO_ROOT : GO_TO_PREVIOUS;
}

/* a playlist viewer's result as the root loop's: a song chosen, or the
   screen gone dark, to Now Playing over it; Menu back to the screen under it */
static int viewer_screen(int val)
{
    switch (val)
    {
        case PLAYLIST_VIEWER_MAINMENU:
        case PLAYLIST_VIEWER_USB:
            return GO_TO_ROOT;
        case PLAYLIST_VIEWER_OK:
            return GO_TO_WPS;
    }
    return GO_TO_PREVIOUS;
}

/* Up Next, the music's songs to come */
static int playlist_view(void * param)
{
    (void)param;
    return viewer_screen(playlist_viewer());
}

#ifdef HAVE_TAGCACHE
/* the playlist playing's screen, by its file, from Playing From: back at the
   song playing, if the music is from it still */
static int playlist_songs(void)
{
    const struct nav_entry *e = nav_top();
    if (!e || !e->state[0])
        return GO_TO_PREVIOUS;
    push_current_activity(ACTIVITY_PLAYLISTBROWSER);
    const int ret = viewer_screen(lp_view_back(e->state, NULL));
    if (ret == GO_TO_WPS)
        pop_current_activity_without_refresh();
    else
        pop_current_activity();
    return ret;
}
#endif

/* Photos, where its plugin is made for the screen (root_menu.h) */
#ifdef HAVE_PHOTOS_MENU
static int photos(void *param)
{
    (void)param;
#ifdef UI_MAIN_MENU_PANE
    /* The iPod photo's menu takes the screen's whole width. From the main
       menu's Select the plugin sets it, once it has photos to show: the
       theme drawn wide over the main menu leaves part of its pane, and with
       none "No Photos" goes over the main menu as it is. From Now Playing's
       Menu, its theme left in the main menu's look, the pane with the song's
       cover or note: the wide look at once, its lists' area cleared, which
       plugin_load() puts on the screen under "Loading...". */
    if (last_screen != GO_TO_ROOT)
    {
        menu_set_wide(true);
        FOR_NB_SCREENS(i)
        {
            struct viewport vp;
            viewport_set_defaults(&vp, i);
            struct viewport *last = screens[i].set_viewport(&vp);
            screens[i].clear_viewport();
            screens[i].set_viewport(last);
        }
    }
#endif
    /* on the Video its menu keeps the collage beside it, not the song's
       cover, which the theme shows by a screen that has no panel */
    menu_set_selected_panel(MENU_PANEL_PHOTOS);
    const int status = plugin_load(PLUGIN_APPS_DIR "/photos.rock", NULL);
    menu_set_selected_panel(MENU_PANEL_NONE);
    photos_panel_show_end();
#ifdef UI_MAIN_MENU_PANE
    /* the main menu's width back; drawn now only for the main menu itself,
       not under Now Playing or a plugin coming next (as tree.c) */
    if (status == PLUGIN_GOTO_WPS || status == PLUGIN_GOTO_PLUGIN)
        menu_set_wide_later(false);
    else
        menu_set_wide(false);
#endif
    switch (status)
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
    [GO_TO_COMPILATIONS] = { music_section, (void*)LANG_COMPILATIONS, NULL },
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
MENUITEM_RETURNVALUE(compilations_item, ID2P(LANG_COMPILATIONS),
                        GO_TO_COMPILATIONS, item_callback, Icon_Audio);
#endif
MENUITEM_RETURNVALUE(rocks_browser, ID2P(LANG_PLUGINS), GO_TO_BROWSEPLUGINS,
                        NULL, Icon_Plugin);
MENUITEM_RETURNVALUE(extras_item, ID2P(LANG_EXTRAS), GO_TO_EXTRAS,
                        item_callback, Icon_Plugin);
#ifdef HAVE_PHOTOS_MENU
MENUITEM_RETURNVALUE(photos_item, ID2P(LANG_PHOTOS), GO_TO_PHOTOS,
                        item_callback, Icon_Plugin);
#endif

/* Now Playing, with the music stopped too, as on a stock iPod: there it goes
   on with the song to resume (wpsscrn()) */
#ifdef IPOD_VIDEO
/* Beside the pane, in every theme: the short name of a language whose Now
   Playing is cut there in one, which its ipodvideo text of
   LANG_NOW_PLAYING_SHORT has, or else Now Playing itself; the photo's is
   menu.c's pane_name(). Spoken in full */
static char *get_wps_item_name(int selected_item, void *data,
                               char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    if (*str(LANG_NOW_PLAYING_SHORT))
        return ID2P(LANG_NOW_PLAYING_SHORT);
    return ID2P(LANG_NOW_PLAYING);
}

static int talk_wps_item(int selected_item, void *data)
{
    (void)selected_item; (void)data;
    talk_id(LANG_NOW_PLAYING, false);
    return 0;
}
MENUITEM_RETURNVALUE_DYNTEXT(wps_item, GO_TO_WPS, item_callback,
                             get_wps_item_name, talk_wps_item, NULL,
                             Icon_Playback_menu);
#else
MENUITEM_RETURNVALUE(wps_item, ID2P(LANG_NOW_PLAYING), GO_TO_WPS, item_callback,
                     Icon_Playback_menu);
#endif
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
    { "compilations", &compilations_item },
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
                   "albums, compilations, songs, podcasts, genres, composers, "
                   "audiobooks, photos, extras, settings, shuffle_songs, "
                   "shuffle_albums, wps";
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
    { &compilations_item, MAIN_MENU_SECTION + MUSIC_SECTION_COMPILATIONS },
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
                if (!now_playing_available())
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

/* Multicolor's section of a screen of the main menu's, set as it opens
   (ui_theme_set_section()); -1 for one that sets its own: Music's lists by
   the level they open at, Now Playing by its track, which Shuffle goes to,
   Cover Flow (cover_flow()); and the current playlist, which keeps
   the one of the screen it's opened over */
static int screen_section(int screen)
{
    switch (screen)
    {
#ifdef HAVE_TAGCACHE
        case GO_TO_DBBROWSER:
        case GO_TO_ARTISTS: case GO_TO_ALBUMS: case GO_TO_SONGS:
        case GO_TO_PODCASTS: case GO_TO_GENRES: case GO_TO_COMPOSERS:
        case GO_TO_AUDIOBOOKS: case GO_TO_COMPILATIONS:
        case GO_TO_SHUFFLE_SONGS:
        case GO_TO_SHUFFLE_ALBUMS:
        case GO_TO_COVER_FLOW:
#endif
        case GO_TO_WPS:
        case GO_TO_PLAYLIST_VIEWER:
            return -1;
#ifdef HAVE_TAGCACHE
        case GO_TO_SEARCH:
#endif
        case GO_TO_PLAYLISTS_SCREEN:
            return UI_SECTION_MUSIC;
        default:
            return UI_SECTION_SYSTEM;
    }
}

/* where Now Playing was opened from, for root_menu_power_off() */
static int screen_before_wps = GO_TO_ROOT;

void root_menu_power_off(void)
{
    /* A song a held Play showed paused and that never sounded: Start Screen's
       Previous Screen boots where the hold was, as Now Playing would play it */
    if (audio_paused_unheard() && global_status.last_screen == GO_TO_WPS)
        global_status.last_screen = (char)screen_before_wps;
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
        if (screen == GO_TO_WPS && global_status.last_screen != GO_TO_WPS)
            screen_before_wps = global_status.last_screen;
        global_status.last_screen = (char)screen;
        status_save(false);
    }

    if (screen == GO_TO_BROWSEPLUGINS)
        activity = ACTIVITY_PLUGINBROWSER;
    else if (screen == GO_TO_MAINMENU)
        activity = ACTIVITY_SETTINGS;
    else if (screen == GO_TO_SYSTEM_SCREEN)
        activity =  ACTIVITY_SYSTEMSCREEN;

    ui_theme_set_section(screen_section(screen), true);
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
        default: /* the commit, loading it into memory, and Finder's
                    playlists made again */
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
/* while set, the words of every step: the library sorted again for the menu
   language chosen is one thing to the user, whatever it makes again */
static const char *update_words;
static int shown_percent;
static long shown_at;

/* The bar under the boot logo, which stays where the iPod draws it, and the
   step's words under the bar: close enough for the covers' count to fit
   under them on the iPod photo's screen too, the same for every step */
#if LCD_HEIGHT >= 240
#define UPDATE_BAR_GAP  18
#define UPDATE_TEXT_GAP 8
#else
#define UPDATE_BAR_GAP  12
#define UPDATE_TEXT_GAP 7
#endif

/* The count under the words of a step that makes covers: by album for the
   import's slides, under Preparing Covers, and for the lists' covers; by
   song, then by album, for the covers of songs without the computer's.
   Each step's own, from its start, as the bar is. Shown while it is set,
   whatever the words (the steps after the import are all Updating
   Library), so it's cleared as each step ends: by the step, and by the
   screen after the import's loop and after Cover Flow's plugin. */
enum { UPDATE_COUNT_NONE, UPDATE_COUNT_ALBUMS, UPDATE_COUNT_SONGS };
static int count_kind, count_done, count_total;
static char shown_count[64];
static long count_at;

static void library_update_count(int kind, int done, int total)
{
    count_kind = total > 0 ? kind : UPDATE_COUNT_NONE;
    count_done = MAX(0, MIN(done, total));
    count_total = total;
}

/* the screen with this step and how far it is: drawn for a change, and every
   second as the theme's backdrop can come back meanwhile */
static void library_update_show(const char *text, int percent)
{
    if (update_words)
        text = update_words;
    char count[sizeof(shown_count)] = "";
    if (count_kind != UPDATE_COUNT_NONE && !update_words)
    {
        const char *form = count_kind == UPDATE_COUNT_SONGS
            ? plural_str(count_total, LANG_COVERS_SONGS_OF_ONE,
                         LANG_COVERS_SONGS_OF_FEW, LANG_COVERS_SONGS_OF_MANY)
            : plural_str(count_total, LANG_COVERS_ALBUMS_OF_ONE,
                         LANG_COVERS_ALBUMS_OF_FEW,
                         LANG_COVERS_ALBUMS_OF_MANY);
        snprintf(count, sizeof(count), form, count_done, count_total);
    }
    const bool same_screen = text == shown_text
        && TIME_BEFORE(current_tick, shown_at + HZ);
    /* the count drawn as the bar moves, else four times a second: a draw for
       each of thousands of songs or slides held up the step */
    const bool count_due = strcmp(count, shown_count)
        && (percent != shown_percent || !*count || !*shown_count
            || TIME_AFTER(current_tick, count_at + HZ / 4));
    if (same_screen && percent == shown_percent && !count_due)
        return;
    shown_text = text;
    shown_percent = percent;
    if (!same_screen)
        shown_at = current_tick;
    if (!same_screen || count_due)
    {
        strmemccpy(shown_count, count, sizeof(shown_count));
        count_at = current_tick;
    }

    /* 140 px on the iPod Video's screen, 96 on the iPod photo's */
    const int bar_w = LCD_WIDTH * 7 / 16, bar_h = 6;
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
    y += UPDATE_BAR_GAP;
    int x = (LCD_WIDTH - bar_w) / 2;
#ifdef HAVE_LCD_COLOR
    if (global_settings.ui_theme == UI_THEME_DIETER)
    {
        /* Dieter's: a groove, its top wall in shadow and its lower lip lit,
           filled with the light of its pane's floor */
        lcd_set_foreground(LCD_RGBPACK(0x29, 0x28, 0x29));
        lcd_fillrect(x, y, bar_w, bar_h);
        lcd_set_foreground(LCD_RGBPACK(0x10, 0x10, 0x10));
        lcd_hline(x, x + bar_w - 1, y);
        lcd_set_foreground(LCD_RGBPACK(0x39, 0x38, 0x39));
        lcd_hline(x, x + bar_w - 1, y + bar_h);
        lcd_set_foreground(LCD_RGBPACK(0xe7, 0xe3, 0xde));
        lcd_fillrect(x, y + 1, bar_w * percent / 100, bar_h - 1);
    }
    else
#endif
    {
        lcd_set_foreground(LCD_RGBPACK(70, 70, 70));
        lcd_fillrect(x, y, bar_w, bar_h);
        lcd_set_foreground(LCD_WHITE);
        lcd_fillrect(x, y, bar_w * percent / 100, bar_h);
    }
    /* the count's line under the words, in the theme's smaller font */
    const int text_y = y + bar_h + UPDATE_TEXT_GAP;
    const int count_y = text_y + font_get(FONT_UI)->height + 1;
    int count_font = font_find(ui_font_file(UI_FONT_SUBLINE));
    if (count_font < 0)
        count_font = FONT_UI;
    const int count_h = font_get(count_font)->height;
    if (same_screen && count_due)
    {
        lcd_set_foreground(LCD_BLACK);
        lcd_fillrect(0, count_y, LCD_WIDTH, count_h);
    }
    lcd_set_foreground(LCD_RGBPACK(160, 160, 160));
    lcd_set_drawmode(DRMODE_FG);
    int w, h;
    if ((!same_screen || count_due) && *count)
    {
        lcd_setfont(count_font);
        lcd_getstringsize(count, &w, &h);
        lcd_putsxy((LCD_WIDTH - w) / 2, count_y, count);
    }
    if (same_screen)
    {
        lcd_set_drawmode(DRMODE_SOLID);
        lcd_setfont(FONT_UI);
        /* its many small steps were a good part of the update's time */
        lcd_update_rect(x, y, bar_w, bar_h + 1);
        if (count_due)
            lcd_update_rect(0, count_y, LCD_WIDTH, count_h);
        return;
    }

    lcd_setfont(FONT_UI);
    lcd_getstringsize(text, &w, &h);
    lcd_putsxy((LCD_WIDTH - w) / 2, text_y, text);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_update();
    backlight_on();
}

/* The steps after the import that aren't the tagcache's, all under Updating
   Library's words, each with a bar of its own: Cover Flow's album list; the
   covers of songs outside Finder's folder, then the lists' and the artists'
   covers; Songs; Search's letters */
enum { UPDATE_STEP_ALBUMS, UPDATE_STEP_COVERS, UPDATE_STEP_SONGS,
       UPDATE_STEP_LETTERS };
/* the step library_update_forward() drew last, none at each screen's start:
   the same step of a screen before, as Songs after USB, had its bar go on
   from that screen's */
static int forward_step = -1;

/* A step of the update that isn't the tagcache's. Its parts may each count
   anew, so its one bar only goes forward. */
static void library_update_forward(int step, int done, int total)
{
    static int percent;
    const char *text = (const char *)str(LANG_UPDATING_LIBRARY);
    /* a step of its own from 0 under the same words: by its own number, by
       the screen shown from 0 just before it (the lists' covers, Search's
       letters), or by another screen; under update_words' one line too */
    if (shown_text != (update_words ? update_words : text)
        || step != forward_step || shown_percent < percent)
        percent = 0;
    forward_step = step;
    if (total > 0)
        percent = MAX(percent, 100 * MIN(done, total) / total);
    library_update_show(text, percent);
}

#ifdef ITUNESDB_HAVE_ART
void library_update_progress(int done, int total)
{
    library_update_forward(UPDATE_STEP_ALBUMS, done, total);
}

/* the covers of songs outside Finder's folder, of albums done of total */
void library_update_covers_progress(int done, int total)
{
    library_update_forward(UPDATE_STEP_COVERS, done, total);
}

/* and its count, of songs or of albums, from Cover Flow's plugin */
void library_update_covers_count(int done, int total, bool songs)
{
    library_update_count(songs ? UPDATE_COUNT_SONGS : UPDATE_COUNT_ALBUMS,
                         done, total);
}

/* the lists' covers: the first half of the bar their albums looked up, not
   counted; then the albums with a cover gone through as they're made */
static void library_row_covers_progress(int done, int total)
{
    int made, slots;
    list_covers_making(&made, &slots);
    library_update_count(UPDATE_COUNT_ALBUMS, made, slots);
    library_update_covers_progress(done, total);
}
#endif

void library_update_songs_progress(int done, int total)
{
    library_update_forward(UPDATE_STEP_SONGS, done, total);
}

/* Search's letters */
static void library_letters_progress(int done, int total)
{
    library_update_forward(UPDATE_STEP_LETTERS, done, total);
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

/* The disk's free clusters counted again, if that's owed (free_space.c), as
   Updating Library's steps go: under the boot logo or what USB's screen
   left, nothing new shown. USB or a switch off come meanwhile stop it and are
   answered after it: a count stopped for USB begins again after that session,
   the volume mounted again; one stopped as music started goes on from where
   it stopped. Before the library's check, whose room goes by that count, and
   not under an import, whose writes would wait for each of its reads. True
   if it answered USB, whose session checked the library after it, or left
   it pending. */
bool library_free_recount(void)
{
    free_space_recount(library_playlists_go_on);
    if (!held_event)
        return false;
    const long event = held_event;
    held_event = 0;
    return default_event_handler(event) == SYS_USB_CONNECTED;
}

/* the bytes of text's first line in width: up to a space, or the characters
   that fit in a line with none, as in Chinese */
int text_line_bytes(const unsigned char *text, int width)
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
        lens[n] = text_line_bytes(text, width);
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
    int y = MIN(STOCK_BOOT_LOGO_Y + BMPHEIGHT_bootlogo + UPDATE_BAR_GAP,
                LCD_HEIGHT - 8 - (n + 1) * h - 4);
    lcd_bmp(&bm_bootlogo, (LCD_WIDTH - BMPWIDTH_bootlogo) / 2,
            MIN(STOCK_BOOT_LOGO_Y, y - 10 - BMPHEIGHT_bootlogo));
#else
    int y = MIN((LCD_HEIGHT - 96) / 2 + 96 + UPDATE_BAR_GAP,
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
/* whether Cover Flow's album list is of the database: its magic, commit,
   entries and albums' file size, as pictureflow.c's load_album_index() checks
   them. Not Cover Flow's own cache version (pictureflow.cfg): after its
   Rebuild Cache, or with no pictureflow.cfg, a list current here is made
   again at Cover Flow's start or the next Preparing Albums, whichever comes
   first. */
static bool album_list_current(void)
{
    struct tagcache_stat *stat = tagcache_get_stat();
    int32_t head[4];
    int fd = open(ITUNESDB_ALBUM_INDEX, O_RDONLY);
    bool current = fd >= 0 && read(fd, head, sizeof(head)) == sizeof(head)
        && !memcmp(head, ITUNESDB_ALBUM_INDEX_MAGIC, 4)
        && head[1] == tagcache_get_commitid()
        /* the complement for a list of music alone, the usual one, as the
           plugin would make it now */
        && head[2] == ITUNESDB_ALBUM_INDEX_ENTRIES(stat->total_entries,
               ITUNESDB_ALBUM_INDEX_MUSIC_ONLY(tagcache_kind_if_known));
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

#if defined(HAVE_PHOTOS_MENU) && defined(HAVE_PHOTOS_PANEL)
/* the collages are to be made of other photos: a step of Updating Library
   too, its bar from 0 */
static void photos_making(void)
{
    library_update_show(str(LANG_UPDATING_LIBRARY), 0);
}
#endif

/* The steps after the database's phases, timed for health.log: one line
   at the end of the screen, of those that ran, if they took a second or
   more */
enum { STEP_ALBUMS, STEP_YEARS, STEP_ROW_COVERS, STEP_SONGS, STEP_LETTERS,
       STEP_PLAYLISTS, STEPS };
static long step_ticks[STEPS];
static bool step_ran[STEPS];
static long step_start;
/* and the FAT's sectors each read one at a time, and their time */
static unsigned long step_fat[STEPS], step_fat_us[STEPS];
static unsigned long step_fat_at, step_fat_us_at;
static const char * const step_names[STEPS] = {
    "albums", "years", "row covers", "songs", "letters", "playlists"
};

static void step_begin(void)
{
    step_start = current_tick;
    health_fat_reads(&step_fat_at, &step_fat_us_at);
}

static void step_end(int step)
{
    step_ticks[step] += current_tick - step_start;
    step_ran[step] = true;
    unsigned long fat, fat_us;
    health_fat_reads(&fat, &fat_us);
    step_fat[step] += fat - step_fat_at;
    step_fat_us[step] += fat_us - step_fat_us_at;
}

static void steps_note(void)
{
    char line[160];
    int len = 0;
    long all = 0;
    for (int i = 0; i < STEPS; i++)
        all += step_ticks[i];
    for (int i = 0; i < STEPS && all >= HZ; i++)
    {
        if (!step_ran[i] || len >= (int)sizeof(line))
            continue;
        len += snprintf(line + len, sizeof(line) - len, "%s%s %ld.%ld s",
                        len ? ", " : "", step_names[i], step_ticks[i] / HZ,
                        step_ticks[i] % HZ * 10 / HZ);
    }
    if (len)
    {
        health_note("library update, then: %s", line);
        health_note_fat_reads("library update, then", step_names, step_fat,
                              step_fat_us, STEPS);
    }
    memset(step_ticks, 0, sizeof(step_ticks));
    memset(step_ran, 0, sizeof(step_ran));
    memset(step_fat, 0, sizeof(step_fat));
    memset(step_fat_us, 0, sizeof(step_fat_us));
}

/* After a sync the database takes in the new songs before the menu shows,
 * as a stock iPod has them as soon as it starts: the boot logo stays, with
 * the step and how far that is under it, until it's done. Menu left it to
 * finish behind the menu, where it took the memory of music started then,
 * which stopped, and the caches made here weren't. */
static void library_update_screen(const char *words)
{
    /* for this screen, not one shown within it, as after USB */
    const char *outer = update_words;
    update_words = words;
    forward_step = -1;
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
    bool library_kept = true; /* only the check, or Finder's playlists */
    while ((phase = tagcache_get_import(&done, &total))
           != TAGCACHE_IMPORT_NONE)
    {
        if (phase != TAGCACHE_IMPORT_CHECKING
            && phase != TAGCACHE_IMPORT_PLAYLISTS)
            library_kept = false;
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
        /* Cover Flow's slides, by album */
        library_update_count(phase == TAGCACHE_IMPORT_COVERS
                             || phase == TAGCACHE_IMPORT_SLIDES
                             ? UPDATE_COUNT_ALBUMS : UPDATE_COUNT_NONE,
                             done, total);
        library_update_show(library_update_step(phase), percent);
        reset_poweroff_timer(); /* no sleep while the library updates */
        int action = get_action(CONTEXT_STD, HZ / 4);
        if (default_event_handler(action) == SYS_USB_CONNECTED)
            break;
    }
    /* the import's count gone with it: the steps after count their own */
    library_update_count(UPDATE_COUNT_NONE, 0, 0);
    /* left for want of room: nothing made of it either */
    bool no_space = phase == TAGCACHE_IMPORT_NONE && tagcache_take_no_space();
    /* a check that took long but found nothing has nothing to make again, nor
       Finder's playlists made again alone, nor an import that changed neither
       the database nor the covers, with what they make made for this
       database */
    const bool imported = phase == TAGCACHE_IMPORT_NONE && !no_space
        && ((shown && !library_kept
             && !(itunesdb_import_kept_art() && album_list_current()
                  && tagtree_songs_prepared()))
            || tagcache_get_commitid() != prepared_commitid);
#if defined(HAVE_PHOTOS_MENU) && defined(HAVE_PHOTOS_PANEL)
    /* Finder may sync photos only, so this goes by their own database */
    const bool photos_changed = phase == TAGCACHE_IMPORT_NONE && photos_panel_stale();
#else
    /* no collages without Photos, whose plugin makes them, nor without the
       main menu's pane they are shown in */
    const bool photos_changed = false;
#endif
    /* the letters of the tags' alphabets, which Search types with */
    bool letters_stale = phase == TAGCACHE_IMPORT_NONE
                         && music_search_letters_stale();
#ifdef ITUNESDB_HAVE_ART
    /* the covers of songs outside Finder's folder, not made yet, or cut short
       by Menu or USB: by Cover Flow's plugin, without which nothing's made,
       from the database in memory, without which it finds no album */
    const struct tagcache_stat *tc = tagcache_get_stat();
    const bool covers_stale = phase == TAGCACHE_IMPORT_NONE
                              && tc->ready && tc->ramcache
                              && file_exists(PLUGIN_DEMOS_DIR "/pictureflow.rock")
                              && (itunesdb_covers_stale()
                                  || itunesdb_covers_images_changed()
                                  || itunesdb_covers_inbox());
    /* and the covers of the lists' rows, made of Cover Flow's slides, and the
       artists' rows, of those covers */
    const bool row_covers_stale = phase == TAGCACHE_IMPORT_NONE
                                  && (list_covers_stale() || artist_rows_stale());
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
        && !no_space && file_exists(PLUGIN_DEMOS_DIR "/pictureflow.rock"))
    {
        if (itunesdb_covers_room())
        {
            library_update_show(str(LANG_UPDATING_LIBRARY), 0);
            /* it clears the screen as it starts and ends: drawn whole again */
            shown_text = NULL;
            step_begin();
            plugin_load(PLUGIN_DEMOS_DIR "/pictureflow.rock",
                        ITUNESDB_ALBUM_LIST);
            step_end(STEP_ALBUMS);
            shown_text = NULL;
            /* nor its count left, which it clears as its covers end */
            library_update_count(UPDATE_COUNT_NONE, 0, 0);
        }
        /* the computer's covers, which would take the room they need */
        else
            itunesdb_covers_inbox_drop();
    }
#endif
    /* the rest after USB or switching off, come meanwhile: what they'd go
       by is of before it */
    /* the albums' years, which the lists of an artist's albums go by: before
       Songs is prepared, which is kept as they're made */
    step_begin();
    const bool years_made = phase == TAGCACHE_IMPORT_NONE && !held_event
                            && !album_years_ready() && album_years_make();
    if (years_made)
    {
        step_end(STEP_YEARS);
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
        && album_years_ready() && (row_covers_stale || list_covers_stale()
                                   || artist_rows_stale()))
    {
        library_update_count(UPDATE_COUNT_NONE, 0, 0);
        library_update_show(str(LANG_UPDATING_LIBRARY), 0);
        step_begin();
        if (list_covers_stale())
            list_covers_make(library_row_covers_progress,
                             library_playlists_go_on);
        library_update_count(UPDATE_COUNT_NONE, 0, 0);
        /* each artist's cover among those, their albums and songs counted */
        if (!held_event && artist_rows_stale())
            artist_rows_make(library_update_covers_progress,
                             library_playlists_go_on);
        step_end(STEP_ROW_COVERS);
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
    {
        step_begin();
        tagtree_prepare_songs();
        step_end(STEP_SONGS);
    }
    /* not by a database the caches couldn't be made for */
    no_space |= library_take_no_room();
    if (phase == TAGCACHE_IMPORT_NONE && !held_event && !no_space)
        prepared_commitid = tagcache_get_commitid();
    if (letters_stale && !held_event)
    {
        library_update_show(str(LANG_UPDATING_LIBRARY), 0);
        step_begin();
        music_search_letters_make(library_letters_progress);
        step_end(STEP_LETTERS);
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
        step_begin();
        smart_update_at_boot();
        /* the marks of the synced ones, as this import left them */
        ip_synced_load();
        /* the iPod's own found among them moved to Unsynced, before
           their lists of songs are made */
        ip_gather_own();
        lp_index_all(library_playlists_go_on);
        /* noted only where it took long: it runs after every screen */
        if (current_tick - step_start >= HZ)
            step_end(STEP_PLAYLISTS);
    }
    /* the free space counted again, if it wasn't before an import the boot
       began (root_menu()): once its writes are over, USB or a switch off come
       meanwhile held, as by the playlists' indexing above, for what follows */
    if (phase == TAGCACHE_IMPORT_NONE && !held_event)
        free_space_recount(library_playlists_go_on);
    /* The list to resume, read again after all that may have written a
       playlist's file, while this or the boot's logo shows and the disk
       spins: Resume Playback then starts at once. For the main menu, which
       knows nothing wrote one since; not with USB or a switch off waiting */
    playlist_resume_forget();
    if (!held_event && get_current_activity() == ACTIVITY_MAINMENU)
        playlist_preload();
    no_space |= library_take_no_room();
    /* an update's new songs left out for want of memory, the rest of the
       library imported: said once, over Updating Library's screen at its end.
       A check after USB over before the screen came has it shown for the
       note, which would be over what USB's screen left */
    const bool memory_note = phase == TAGCACHE_IMPORT_NONE && !held_event
        && tagcache_take_no_memory_note();
    if ((no_space || memory_note) && !held_event && !shown)
    {
        FOR_NB_SCREENS(i)
            viewportmanager_theme_enable(i, false, NULL);
        shown = true;
        if (!no_space)
        {
            shown_text = NULL;
            library_update_show(library_update_step(TAGCACHE_IMPORT_CHECKING),
                                100);
        }
    }
#if defined(HAVE_PHOTOS_MENU) && defined(HAVE_PHOTOS_PANEL)
    if (shown && photos_changed && !held_event) /* else made the next time */
    {
        if (photos_panel_update(photos_making))
            skin_reload_while_playing();
    }
#endif
    /* What the main menu's pane shows by the row selected, read now, while
       the disk spins, and not as it's first drawn, which could spin up a
       disk stopped by then: the photos' count, which an update of the
       collage above has read already, and the name given in Finder */
    if (phase == TAGCACHE_IMPORT_NONE && !held_event)
    {
#if defined(HAVE_PHOTOS_MENU) && defined(HAVE_PHOTOS_PANEL)
        photos_panel_count();
#endif
        itunesdb_device_name_refresh();
    }
    if (shown)
    {
        no_space |= library_take_no_room();
        if (no_space && !held_event)
            library_no_space_show();
        /* while the screen drawn is the one in memory too: the theme's parts
           come back over a screen cleared in memory only, and a note after
           them was over a mix of the two */
        if (memory_note)
            splash(HZ * 5, ID2P(LANG_LIBRARY_NEW_SONGS_NO_MEMORY));
        lcd_set_background(global_settings.bg_color);
        lcd_set_foreground(global_settings.fg_color);
        lcd_clear_display();
        FOR_NB_SCREENS(i)
            viewportmanager_theme_undo(i, true);
    }
#ifdef ITUNESDB_HAVE_ART
    /* the computer's covers and the scan's tags, the lists' covers made of
       them too */
    itunesdb_covers_inbox_done();
    itunesdb_scan_forget();
#endif
    steps_note();
    if (held_event)
    {
        const long event = held_event;
        held_event = 0;
        default_event_handler(event);
    }
    update_words = outer;
}

void show_library_update(void)
{
    library_update_screen(NULL);
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

#ifdef HAVE_TAGCACHE
/* The menu language chosen has other alphabets first than the library was
   sorted with: sorted again for it at once, the music stopped as for another
   theme, on Updating Library's screen, and what's made of the order made
   again, as after a sync. True if it was. One that can't be now, with
   another import, waits for the next boot or Updating Library. */
bool library_language_changed(void)
{
    if (!tagcache_language_resort_due())
        return false;
    /* the language written at the next idle of the disk: a boot cut short
       before has the old one, and sorts by the one written */
    settings_save();
    if (audio_status() & AUDIO_STATUS_PLAY)
        audio_stop();
    if (!tagcache_resort_for_language())
        return false;
    library_update_screen(str(LANG_SORTING_FOR_LANGUAGE));
#ifdef ITUNESDB_HAVE_ART
    menu_cover_resume_look();
#endif
    return true;
}
#endif

void library_update_after_usb(void)
{
    library_update_pending = true;
}

/* A screen .resume.cfg gave, the root's if it's none: a damaged or hand-made
   file's other numbers had the boot go round without end, or call a screen
   past items[] or one it hasn't. A plugin, which has none, is the loop's own */
static int saved_screen(int screen)
{
    if (screen == GO_TO_ROOT || screen == GO_TO_PLUGIN)
        return screen;
    return screen >= 0 && screen < NUM_ITEMS && items[screen].function
           ? screen : GO_TO_ROOT;
}

void root_menu(void)
{
    global_status.last_screen = saved_screen(global_status.last_screen);
    global_status.last_browser = saved_screen(global_status.last_browser);
    int previous_browser = global_status.last_browser;
    int selected = 0;
    int shortcut_origin = GO_TO_ROOT;

    /* the hang catcher's D1 from here; its D2, the UI away from its buttons,
       from the loop: the waits for the dircache and the boot's Updating
       Library before it took up to 28 s on 13k songs (hang.h) */
    hang_start();
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
    /* the free space counted again, if that's owed, before the check, whose
       room goes by it; after an import the boot began (tagcache_init()),
       on its screen (library_update_screen()). USB come meanwhile checked the
       library after its session, as in database_ram_wait(). */
    int import_done, import_total;
    if (tagcache_get_import(&import_done, &import_total)
        == TAGCACHE_IMPORT_NONE)
        usb_checked |= library_free_recount();
    /* the songs outside Finder's folder, as after USB: the check there was
       lost to a reset or a switch off right after it, and the songs copied
       waited for the next connection. Not again after one that USB, come
       meanwhile, was followed by. */
    if (!usb_checked)
        tagcache_check_sync();
    show_library_update();
    /* which of the playlists are synced, if USB or a switch off held during
       Updating Library left it before it read them: by the list as it is */
    if (!ip_synced_loaded())
        ip_synced_load();
#endif
#ifdef ITUNESDB_HAVE_ART
    /* the cover of the song to resume, by the library as it is now */
    menu_cover_resume_look();
#endif
#ifndef HAVE_TAGCACHE
    playlist_preload();
#endif
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
    hang_run();

    while (true)
    {
#ifdef HAVE_TAGCACHE
        /* the import begins as it shows, not behind whatever was open */
        /* not under music, started meanwhile: an import's memory stops it,
           and the check waits for it */
        if (library_update_pending && !(audio_status() & AUDIO_STATUS_PLAY))
        {
            library_update_pending = false;
            /* not again after a USB session answered during the count, whose
               own pass checked the library or left it pending again */
            if (!library_free_recount() && tagcache_check_sync())
                show_library_update();
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
        switch (next_screen)
        {
            case MENU_ATTACHED_USB:
            case MENU_SELECTED_EXIT:
                /* fall through */
            case GO_TO_ROOT:
                if (last_screen != GO_TO_ROOT)
                    selected = get_selection(last_screen);
                /* the song to resume gone over USB: its list, loaded again
                   as Resume Playback does unless it's there as just read,
                   for the next song still there, or for none and no Resume
                   Playback */
                if (!audio_status() && playlist_resume_gone())
                {
                    if (playlist_resumed() || playlist_resume() >= 0)
                        playlist_check_resume();
                }
                global_status.last_screen = GO_TO_ROOT; /* We've returned to ROOT */
                /* the bottom of the history: none over it, by Menu, held Menu,
                   USB or a boot */
                nav_clear();
                asked.what = ASK_NONE;
                /* When we are in the main menu we want the hardware BACK
                 * button to be handled by HOST instead of rockbox */
                ignore_back_button_stub(true);
                ui_theme_set_section(UI_SECTION_SYSTEM, true);

                next_screen = do_menu(&root_menu_, &selected, NULL, false);
#ifdef HAVE_TAGCACHE
                music_from_main_menu = next_screen == GO_TO_DBBROWSER;
                if (next_screen == GO_TO_SEARCH)
                    music_search_new(); /* Search added to the main menu */
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
            case GO_TO_AUDIOBOOKS: case GO_TO_COMPILATIONS:
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
                /* the screen under it in the history; from the main menu
                   itself, the screen left for it, as before */
                if (nav_count > 0)
                {
                    next_screen = nav_back();
                    break;
                }
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
                /* Play held, or the screen gone dark: Menu in Now Playing back
                   to the screen left, as from any other way there (the
                   owner's decision, 2026-11-25) */
                next_screen = previous_music;
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

                /* an entry of the history, opened again by its key */
                if (nav_again && nav_top())
                    key = ID2P((int)nav_top()->type);
                else
                    nav_push(GO_TO_PLUGIN, NAV_SCREEN);
                nav_top()->type = P2ID((unsigned char *)key);
                nav_again = nav_from_wps = false;
                next_screen = load_plugin_screen(key);
                if (root_menu_leave_asked())
                {
                    next_screen = nav_open_asked();
                    break;
                }
                const bool closed = next_screen == GO_TO_PREVIOUS;

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
                /* closed, for another screen than the one under it in the
                   history (a shortcut's); that one again, as left */
                if (closed && next_screen != GO_TO_PREVIOUS)
                {
                    nav_drop(nav_count - 1);
                    if (nav_top() && next_screen == nav_top()->screen)
                        nav_again = true;
                }
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
            /* a new entry of the history, or the top one again; not for an
               action, as Shuffle Songs, which goes on to Now Playing */
            const bool action =
#ifdef HAVE_TAGCACHE
                screen == GO_TO_SHUFFLE_SONGS || screen == GO_TO_SHUFFLE_ALBUMS ||
#endif
                false;
            if (!nav_again && !action)
                nav_push(screen, nav_kind_of(screen));
#ifdef HAVE_TAGCACHE
            if (screen == GO_TO_PLAYLIST_SONGS)
                next_screen = playlist_songs();
            else
#endif
                next_screen = load_screen(screen);
            nav_again = false;
            nav_from_wps = screen == GO_TO_WPS;
            note_music_origin(screen);
            /* a screen asked for in it: over it, it left as for Now Playing */
            if (root_menu_leave_asked())
                next_screen = nav_open_asked();
            /* an action's entry is none: Menu after it back to the screen
               it was chosen in, the main menu */
            else if (action && next_screen == GO_TO_PREVIOUS && nav_count == 0)
                next_screen = GO_TO_ROOT;
        }
    }
}
