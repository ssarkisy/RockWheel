/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006 Jonathan Gordon
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
#ifndef _EXPORTED_MENUS_H
#define _EXPORTED_MENUS_H

#include "menu.h"
/* not needed for plugins */
#ifndef PLUGIN 

extern const struct menu_item_ex 
        display_menu,               /* display_menu.c   */
        playback_settings,          /* playback_menu.c  */
#ifdef HAVE_RECORDING
        recording_settings,         /* recording_menu.c */
        recording_settings_menu,
        peak_meter_menu,            /* also used from within recording_menu */
#endif
        sound_settings,             /* sound_menu.c     */
        settings_menu_item,         /* settings_menu.c  */
        bookmark_settings_menu,
        playlist_settings,          /* playlist_menu.c  */
        viewer_settings_menu,       /* playlist_menu.c  */
        equalizer_menu,             /* eq_menu.c        */
#ifdef AUDIOHW_HAVE_EQ
        audiohw_eq_tone_controls,   /* audiohw_eq_menu.c */
#endif
        radio_settings_menu,        /* radio_menu.c     */
        theme_menu,                 /* theme_menu.c     */
        extras_menu;                /* extras_menu.c    */

/* The settings of the iPod settings menu, from the menus they are in */
extern const struct menu_item_ex
        shuffle_item, repeat_mode, replaygain_type, /* playback_menu.c */
        ipod_playback_settings,
        volume_limit, ipod_sound_settings,          /* sound_menu.c     */
        ipod_eq_menu,                               /* eq_menu.c        */
#ifdef HAVE_BACKLIGHT
        backlight_timeout, ipod_backlight_menu,     /* display_menu.c   */
#endif
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
        brightness_item,
#endif
        codepage_setting,
        battery_capacity, ipod_keyclick_menu,       /* settings_menu.c  */
        sleep_timer_item, language_item,
#ifdef HAVE_TAGCACHE
        ipod_database_menu, autoresume_menu,
#endif
        browse_themes;                              /* theme_menu.c     */

struct browse_folder_info {
    const char* dir;
    int show_options;
};
int browse_folder(void *param); /* in theme_menu.c as it is mostly used there */

#endif /* ! PLUGIN */
#endif /*_EXPORTED_MENUS_H */
