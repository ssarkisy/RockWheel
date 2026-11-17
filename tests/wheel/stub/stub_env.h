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
/* The click wheel driver's world, for its host test: button-clickwheel.c, the
   base's and today's, built against the headers of this folder. What it reads
   is in stub_env, what it does is traced in order. */
#ifndef STUB_ENV_H
#define STUB_ENV_H

#include <stdbool.h>
#include <stdint.h>

struct stub_env {
    uint32_t data;       /* CLICKWHEEL_DATA, the controller's packet */
    uint32_t c104;       /* its status, 0x7000c104: bit 26 a packet waits */
    uint32_t usec;       /* USEC_TIMER */
    uint32_t gpioa;      /* GPIOA_INPUT_VAL: bit 5 clear with Hold on */
    bool lit;            /* is_backlight_on(false) */
    bool dark_wheel;     /* button_dark_wheel() */
    bool fine;           /* button_wheel_is_fine() */
    bool press_quiet;    /* button_wheel_is_press_quiet() */
    bool queue_empty;    /* button_queue_empty() */
    unsigned int clears; /* button_queue_clears() */
};
extern struct stub_env stub_env;
extern volatile long current_tick;
extern volatile unsigned long stub_regs[16]; /* registers nothing reads */

enum stub_trace_kind {
    T_BACKLIGHT_ON = 1, /* backlight_on() */
    T_POWEROFF,         /* reset_poweroff_timer() */
    T_POST,             /* button_queue_post(a, b) */
    T_UDELAY,           /* udelay(a) */
    T_OUTL,             /* outl(a, b) */
    T_HOLD_CHANGED,     /* backlight_hold_changed(a) */
};
void stub_trace(int kind, uint32_t a, uint32_t b);

uint32_t stub_inl(uint32_t addr);
void stub_outl(uint32_t value, uint32_t addr);

#endif
