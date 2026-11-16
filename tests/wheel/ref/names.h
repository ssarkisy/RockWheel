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
/* The base's driver beside today's in one program: its names, apart */
#define ipod_4g_button_int           ref_ipod_4g_button_int
#define button_init_device           ref_button_init_device
#define button_hold                  ref_button_hold
#define headphones_inserted          ref_headphones_inserted
#define button_read_device           ref_button_read_device
#define wheel_status                 ref_wheel_status
#define wheel_send_events            ref_wheel_send_events
#define button_wheel_untouched_ticks ref_button_wheel_untouched_ticks
#define old_wheel_value              ref_old_wheel_value
#define new_wheel_value              ref_new_wheel_value
#define wheel_delta                  ref_wheel_delta
#define wheel_is_touched             ref_wheel_is_touched
#define accumulated_wheel_delta      ref_accumulated_wheel_delta
#define wheel_velocity               ref_wheel_velocity
#define wheel_accel_stage            ref_wheel_accel_stage
