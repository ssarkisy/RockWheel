/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
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
#ifndef _NIGHT_MODE_H_
#define _NIGHT_MODE_H_

#include <stdbool.h>
#include <stddef.h>

/* The plain background of the dark theme, as in themes/tools/make_dark_theme.py */
#define THEME_DARK_BG_R 18
#define THEME_DARK_BG_G 18
#define THEME_DARK_BG_B 20

/* At boot, after the settings are read and before they're applied: the theme's files
   and what the mode has changed, for the time it is now */
void night_mode_boot(void);

/* A file of settings is about to be read while running: the same goes for it, and
   night_mode_boot() is called afterwards */
void night_mode_settings_loading(void);

/* From the action loop of the UI. True if the screen has to be drawn again, as the mode
   has just changed by its schedule. It only does in a context that can draw again. */
bool night_mode_poll(int context);

/* Callbacks of the settings */
void dark_theme_changed(bool dark);
void night_mode_changed(bool on);
void night_action_changed(bool on);
void night_brightness_changed(int value);
void night_schedule_changed(bool on);
const char *night_time_format(char *buffer, size_t buffer_size, int value,
                              const char *unit);

/* After a value of the mode has been chosen on its screen */
void night_settings_chosen(void);

/* The mode from the menu over Now Playing */
void dark_theme_toggle(void);
void night_mode_toggle(void);

/* What the theme's change means for Now Playing, once: true if it has to be set up again */
bool night_mode_theme_changed(void);

#endif /* _NIGHT_MODE_H_ */
