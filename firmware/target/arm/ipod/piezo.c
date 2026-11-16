/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006-2007 Robert Keevil
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

#include "system.h"
#include "kernel.h"
#include "piezo.h"
#include "timer.h"

static inline void piezo_hw_init(void)
{
#ifndef SIMULATOR
    /*logf("PIEZO: hw_init");*/
    DEV_INIT1 &= ~0xc;
    DEV_EN |= DEV_PWM;
#endif
}

static void piezo_hw_tick(unsigned int form_and_period)
{
#ifndef SIMULATOR
    PWM0_CTRL = 0x80000000 | form_and_period;
#endif
}

static inline void piezo_hw_stop(void)
{
#ifndef SIMULATOR
    PWM0_CTRL = 0;
#endif
}

/* No thread of its own: a request is served in its caller's thread (the
 * keyclick's is the UI's, in get_action()) and a click is ended by the user
 * timer's interrupt. Requests from the COP are dropped, as the timer's
 * interrupt and this state are the CPU's.
 *
 * A click registers below the backlight fade (0) and plugins (1). One that
 * finds the timer taken is waited out by its caller, as the thread did; a fade
 * or plugin that takes the timer during a click, or a plugin unregistering a
 * timer it never had, finishes it in piezo_click_lost(). Only clicks under
 * 5 ms go on the timer: the caller's wait yields for longer ones, and
 * piezo_click_lost() doesn't. */
#define PIEZO_TIMER_PRIO    (-1)
/* A timed tone this long or longer, in us, is waited out yielding */
#define PIEZO_YIELD_USEC    5000

static volatile bool click_on_timer; /* sounding, and the timer ends it */
static long click_off;               /* USEC_TIMER when it ends */
static bool tone_waited;             /* sounding, and its caller waits it out */

/* The timer interrupt, at the click's end */
static void piezo_click_end(void)
{
    piezo_hw_stop();
    click_on_timer = false;
    timer_unregister();
}

/* Called when the timer is taken from a click: inside the timer_register() of
 * a fade or plugin, or the timer_unregister() of a plugin that did not own
 * it. The click ends at its time, keeping its length (a wait of at most the
 * click), and the timer is theirs. */
static void piezo_click_lost(void)
{
    while (click_on_timer && TIME_BEFORE(USEC_TIMER, click_off))
        ; /* its interrupt may still come first */

    int oldlevel = disable_irq_save();
    if (click_on_timer)
    {
        piezo_hw_stop();
        click_on_timer = false;
    }
    restore_irq(oldlevel);
}

/* End a timed click now */
static void piezo_click_cancel(void)
{
    int oldlevel = disable_irq_save();
    if (click_on_timer)
    {
        /* first, as timer_unregister() calls piezo_click_lost() */
        click_on_timer = false;
        if (pfn_timer == piezo_click_end)
            timer_unregister();
        piezo_hw_stop();
    }
    restore_irq(oldlevel);
}

/* Whether a tone may start: not on the COP, nor while a timed tone sounds. A
 * click asked for under 4 ms after the last one began would only lengthen
 * the tone; the thread's wait dropped such requests too. */
static bool piezo_may_start(void)
{
#if NUM_CORES > 1
    if (CURRENT_CORE != CPU)
        return false;
#endif
    return !click_on_timer && !tone_waited;
}

void piezo_play(unsigned short inv_freq, unsigned char form)
{
    if (piezo_may_start())
        piezo_hw_tick((unsigned int)form << 16 | inv_freq);
}

/* Waited out by the caller, sleeping: the wait the thread did */
void piezo_play_for_tick(unsigned short inv_freq,
                    unsigned char form, unsigned int dur)
{
    if (!piezo_may_start())
        return;

    piezo_hw_tick((unsigned int)form << 16 | inv_freq);
    tone_waited = true;
    sleep(dur);
    piezo_hw_stop();
    tone_waited = false;
}

void piezo_play_for_usec(unsigned short inv_freq,
                    unsigned char form, unsigned int dur)
{
    if (!piezo_may_start())
        return;

    long off = USEC_TIMER + dur;
    piezo_hw_tick((unsigned int)form << 16 | inv_freq);
    click_off = off;
    click_on_timer = true;
    if (dur < PIEZO_YIELD_USEC
        && timer_register(PIEZO_TIMER_PRIO, piezo_click_lost,
                          (long)dur * (TIMER_FREQ / 1000000),
                          piezo_click_end IF_COP(, CPU)))
        return;
    click_on_timer = false;

    /* the timer is the fade's or a plugin's, or the tone 5 ms or longer */
    tone_waited = true;
    while (TIME_BEFORE(USEC_TIMER, off))
        if (dur >= PIEZO_YIELD_USEC)
            yield();
    piezo_hw_stop();
    tone_waited = false;
}

/* Not while a caller waits out a tone: the thread did not see a stop then */
void piezo_stop(void)
{
#if NUM_CORES > 1
    if (CURRENT_CORE != CPU)
        return;
#endif
    if (tone_waited)
        return;
    piezo_click_cancel();
    piezo_hw_stop();
}

void piezo_clear(void)
{
    piezo_stop();
}

/* Whether a timed tone sounds, during which tones don't start */
bool piezo_busy(void)
{
    return click_on_timer || tone_waited;
}

/*  conversion factor based on the following data

        period          Hz
        10              8547
        20              4465
        30              3024
        40              2286
        50              1846
        60              1537
        70              1320
        80              1165
        90              1030
        100             928

    someone with better recording/analysing equipment should be able
    to get more accurate figures
*/
unsigned int piezo_hz(unsigned int hz)
{
    if (hz > 0)
        return 91225/hz;
    else
        return 0;
}

void piezo_init(void)
{
    /*logf("PIEZO: init");*/
    piezo_hw_init();
}

void piezo_button_beep(bool beep, bool force)
{
    /* old on clickwheel action - piezo_play_for_usec(50, 0x80, 400);
       old on button action - piezo_play_for_usec(50, 0x80, 3000); */

    if (force)
        piezo_clear();

    if (beep)
        piezo_play_for_tick(40, 0x80, HZ/5);
    else
        piezo_play_for_usec(91, 0x80, 4000);
}
