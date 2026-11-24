/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006 Dan Everton
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
#ifndef _EQ_MENU_H
#define _EQ_MENU_H

#include "menu.h"
#include "config.h"
/* Various user interface limits and sizes */
#define EQ_CUTOFF_MIN        20
#define EQ_CUTOFF_MAX     22040
#define EQ_CUTOFF_STEP       10
#define EQ_CUTOFF_FAST_STEP 100
#define EQ_GAIN_MIN       (-240)
#define EQ_GAIN_MAX         240
#define EQ_GAIN_STEP          1
#define EQ_GAIN_FAST_STEP    10
#define EQ_Q_MIN              1
#define EQ_Q_MAX             64
#define EQ_Q_STEP             1
#define EQ_Q_FAST_STEP       10

#define EQ_USER_DIVISOR      10

bool eq_browse_presets(void);
int eq_menu_graphical(void);

/* utility functions for settings_list.c */
const char* eq_q_format(char* buffer, size_t buffer_size, int value,
                        const char* unit);
const char* eq_precut_format(char* buffer, size_t buffer_size, int value,
                             const char* unit);

/* callbacks for settings_list.c */
void eq_enabled_option_callback(bool enabled);

/* the bands and precut of the preset the EQ is set to as this firmware has them, for
   one chosen with an older firmware, and the user's preset as its file is now; with
   read_files, the user's presets read from the disk first (eq_files_read()), and a
   setting on one of the earphones' presets from before moved to its file */
void eq_update_preset(bool read_files);
/* At boot, before the music and before the settings are applied, which read the
   user's presets: the presets for particular earphones the EQ once had built
   in, written into /.rockbox/eqs as the user's own, at each boot until a boot
   has written each of them or found its name taken (eq_menu.c), then never
   again. Only from init(): INIT_ATTR, its code overwritten after (config.h) */
void eq_phones_boot(void);

/* the EQ set to Custom (global_settings.eq_custom), as it is */
void eq_set_custom(void);
/* The editor's menu, Select held in it: Save Preset..., Start From, Reset to
   Flat. GO_TO_ROOT for Menu held, MENU_ATTACHED_USB after USB, else 0 */
int eq_editor_menu(void);

#endif
