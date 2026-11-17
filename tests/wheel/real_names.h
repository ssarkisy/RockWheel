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
/* Today's driver as it is, beside its copy in step with the base's (built
   with the base's 0.3 s stage time): its names, apart */
#define ipod_4g_button_int           real_ipod_4g_button_int
#define button_init_device           real_button_init_device
#define button_hold                  real_button_hold
#define headphones_inserted          real_headphones_inserted
#define button_read_device           real_button_read_device
#define wheel_status                 real_wheel_status
#define wheel_send_events            real_wheel_send_events
#define button_wheel_untouched_ticks real_button_wheel_untouched_ticks
#define button_wheel_state           real_button_wheel_state
#define test_wheel_config            real_test_wheel_config
#define button_wheel_sensitivity     real_button_wheel_sensitivity
#define button_wheel_get_sensitivity real_button_wheel_get_sensitivity
