/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Sergei Sarkisian
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
/* What ref/apps.c's lines of the base call, stubbed; the configuration is
   the tests' own (include/autoconf.h), as wheel_policy.c's in the test */
#ifndef WHEEL_REF_PRELUDE_H
#define WHEEL_REF_PRELUDE_H

#include "config.h"
#include "system.h"
#include "../ref_api.h"

/* the base's button.h */
#define WHEEL_DATA_STAGED (1u << 23)
#define WHEEL_STAGE_ROWS 8

/* the base's names, apart from today's */
#define button_wheel_speed        ref_button_wheel_speed
#define button_wheel_steps        ref_button_wheel_steps
#define button_wheel_staged       ref_button_wheel_staged
#define button_apply_acceleration ref_button_apply_acceleration
#define button_wheel_fine         ref_button_wheel_fine
#define button_wheel_is_fine      ref_button_wheel_is_fine
void ref_button_wheel_fine(bool fine);
bool ref_button_wheel_is_fine(void);

/* the action's data, and a list's lines */
extern unsigned int ref_action_data;
extern int ref_nb_lines, ref_nb_lines_calls;
static inline unsigned int get_action_data(void)
{
    return ref_action_data;
}
#define SCREEN_MAIN 0
struct gui_synclist { int nb_items; };
static inline int list_get_nb_lines(struct gui_synclist *list, int screen)
{
    (void)list; (void)screen;
    ref_nb_lines_calls++;
    return ref_nb_lines;
}

#endif
