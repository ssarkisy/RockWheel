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
/* What the test calls of the base's code (ref/) and of today's drivers */
#ifndef WHEEL_REF_API_H
#define WHEEL_REF_API_H

#include <stdbool.h>

/* ref/apps.c: list.c's, button.c's and wps.c's lines of 1be9ae95b6 */
struct ref_rows {
    int rows, steps;
    bool quick;
    int nb_lines_calls;  /* list_get_nb_lines() asked */
};
void ref_list_rows(unsigned int data, int nb_items, int lines,
                   struct ref_rows *out);
bool ref_list_quick_click(unsigned int data, int nb_items);
int ref_wps_steps(unsigned int data);
int ref_wps_lyrics_ticks(unsigned int data);
int ref_wps_volume_steps(unsigned int data);
int ref_button_wheel_speed(const unsigned int data);
int ref_button_wheel_steps(const unsigned int data);
bool ref_button_wheel_staged(const unsigned int data);
int ref_button_apply_acceleration(const unsigned int data);

/* ref/button-clickwheel.c, 1be9ae95b6's driver (ref/names.h) */
void ref_ipod_4g_button_int(void);
int ref_button_read_device(void);
int ref_wheel_status(void);
void ref_wheel_send_events(bool send);
long ref_button_wheel_untouched_ticks(void);
extern bool ref_wheel_is_touched;
extern int ref_old_wheel_value, ref_new_wheel_value, ref_wheel_delta;
extern unsigned int ref_accumulated_wheel_delta, ref_wheel_velocity;
extern int ref_wheel_accel_stage;

/* firmware/target/arm/ipod/button-clickwheel.c, today's: in step with the
   base's, at its 0.3 s stage time */
struct wheel_config;
struct wheel_state;
void ipod_4g_button_int(void);
int button_read_device(void);
int wheel_status(void);
void wheel_send_events(bool send);
long button_wheel_untouched_ticks(void);
const struct wheel_config *test_wheel_config(void);

/* and as it is (real_names.h) */
void real_ipod_4g_button_int(void);
int real_button_read_device(void);
void real_wheel_send_events(bool send);
const struct wheel_state *real_button_wheel_state(void);
const struct wheel_config *real_test_wheel_config(void);

#endif
