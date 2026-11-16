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
/* firmware/export/button.h, as far as button-clickwheel.c reads it; the
   iPod's button codes are its own button-target.h's */
#ifndef STUB_BUTTON_H
#define STUB_BUTTON_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "button-target.h"

#define BUTTON_NONE   0x00000000
#define BUTTON_REPEAT 0x04000000
/* the base's button.h had it, today's wheel_policy.h */
#define WHEEL_DATA_STAGED (1u << 23)

void button_init_device(void);
int button_read_device(void);
bool button_hold(void);
bool headphones_inserted(void);
bool button_dark_wheel(void);
void button_queue_post(long id, intptr_t data);
bool button_queue_empty(void);
unsigned int button_queue_clears(void);
bool button_wheel_is_fine(void);
int wheel_status(void);
void wheel_send_events(bool send);
long button_wheel_untouched_ticks(void);

#endif
