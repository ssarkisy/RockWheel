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
#ifndef __ROOT_MENU_H__
#define __ROOT_MENU_H__

#include "config.h"
#include "gcc_extensions.h"

/* Cover Flow, by plugin_load, which knows if it starts the music: at the album
   of the song at, if not NULL. Its Go to Artist leaves it for that song's album
   artist's albums, Menu there back to Cover Flow at that album: in lists of
   their own, or, given go_to (MAX_PATH), from Music's lists it was opened in,
   which go there in place, the song in go_to, "" for none. The plugin's answer,
   PLUGIN_OK to go back */
int root_menu_cover_flow(const char *at, char *go_to);
/* Cover Flow's Go to Artist, the song's file, as it quits: false if it isn't
   offered, as Cover Flow isn't open by root_menu_cover_flow(); with NULL, that
   alone */
bool root_menu_go_to_later(const char *path);
#ifdef HAVE_TAGCACHE
/* The Music lists at the song playing, or at a song by its file, from the menus
   over Now Playing, of Search's songs, a playlist's and Cover Flow: Menu there
   goes back, and the place in the lists stays as it was, unless music was
   started from them. 0 for going back, or else the screen to go to; for a
   song by its file, -1 if it isn't found */
int root_menu_go_to_playing(bool artist);
int root_menu_go_to_track(const char *path, bool artist);
/* the same where the music playing was started, Now Playing's Playing From */
int root_menu_playing_from(void);
/* The list an artist or an album opens, from Search: -1 if it isn't there,
   0 for going back, or else the screen to go to */
int root_menu_go_to_name(int tag, const char *name);
/* and an album's, by its album artist's seek too, as Albums has the albums
   of a name apart; by its name alone with -1 */
int root_menu_go_to_album(const char *name, int32_t artist);
/* Settings > Music Menu > Group by Album Artist's callback: the lists of
   artists change, and where the lists were left in them with them */
void root_menu_artists_grouping_changed(bool on);
#endif
void root_menu(void) NORETURN_ATTR;
#ifdef HAVE_TAGCACHE
/* the boot logo and the import's progress, while there is an import */
void show_library_update(void);
/* After USB, once the screen it came in has closed: from within a plugin's,
   it took the plugins' buffer from under the plugin itself */
void library_update_after_usb(void);
/* on that screen, for a plugin working at the end of it: total is 0 when it
   only says that it still works */
void library_update_progress(int done, int total);
void library_update_covers_progress(int done, int total);
/* USB or power off, come meanwhile: answered once Updating Library is over */
void library_update_hold(long event);
void library_update_songs_progress(int done, int total);
#endif
struct menu_table {
    char *string;
    const struct menu_item_ex *item;
};

struct menu_table *root_menu_get_options(int *nb_options);

enum {
    /* from old menu api, but still required*/
    MENU_ATTACHED_USB = -10,
    MENU_SELECTED_EXIT = -9,

    GO_TO_ROOTITEM_CONTEXT = -5,
    GO_TO_PREVIOUS_MUSIC = -4,
    GO_TO_PREVIOUS_BROWSER = -3,
    GO_TO_PREVIOUS = -2,
    GO_TO_ROOT = -1,
    GO_TO_FILEBROWSER = 0,
#ifdef HAVE_TAGCACHE
    GO_TO_DBBROWSER,
#endif
    GO_TO_WPS,
    GO_TO_MAINMENU,
#ifdef HAVE_RECORDING
    GO_TO_RECSCREEN,
#endif
#if CONFIG_TUNER
    GO_TO_FM,
#endif
    GO_TO_RECENTBMARKS,
    GO_TO_PLUGIN,
    /* Do Not add any items above here unless you want it to be able to 
       be the "start screen" after a boot up. The setting in settings_list.c
       will need editing if this is the case. */
    GO_TO_BROWSEPLUGINS,
    GO_TO_TIMESCREEN,
    GO_TO_PLAYLISTS_SCREEN,
    GO_TO_PLAYLIST_VIEWER,
    GO_TO_SYSTEM_SCREEN,
    GO_TO_SHORTCUTMENU,
    GO_TO_EXTRAS,
#ifdef HAVE_TAGCACHE
    GO_TO_SHUFFLE_SONGS, /* an action, not a screen to come back to */
    GO_TO_SHUFFLE_ALBUMS, /* as well */
#endif
    GO_TO_PHOTOS,
#ifdef HAVE_TAGCACHE
    GO_TO_SEARCH, /* in Music, a screen of its own */
    /* sections of Music added to the main menu by Settings > Main Menu;
       Search and Playlists are the screens above */
    GO_TO_COVER_FLOW,
    GO_TO_ARTISTS,
    GO_TO_ALBUMS,
    GO_TO_SONGS,
    GO_TO_PODCASTS,
    GO_TO_GENRES,
    GO_TO_COMPOSERS,
    GO_TO_AUDIOBOOKS,
#endif
};
#ifndef PLUGIN
extern struct menu_item_ex root_menu_;

void root_menu_load_from_cfg(void* setting, char *value);
char* root_menu_write_to_cfg(void* setting, char*buf, int buf_len);
void root_menu_set_default(void* setting, void* defaultval);
bool root_menu_is_changed(void* setting, void* defaultval);
#endif


#endif /* __ROOT_MENU_H__ */
