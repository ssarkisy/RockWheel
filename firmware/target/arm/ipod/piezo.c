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

#include "thread.h"
#include "system.h"
#include "kernel.h"
#include "usb.h"
#include "logf.h"
#include "piezo.h"
#include "timer.h"

static long piezo_stack[DEFAULT_STACK_SIZE/sizeof(long)];
static const char piezo_thread_name[] = "piezo";
static struct event_queue piezo_queue;
static unsigned int duration;
static bool beeping;

enum {
    Q_PIEZO_BEEP = 1,
    Q_PIEZO_BEEP_FOR_TICK,
    Q_PIEZO_BEEP_FOR_USEC,
    Q_PIEZO_STOP
};

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

/* A timed click is ended by the user timer's interrupt rather than by this
 * thread waiting: the wait never yielded, keeping every other thread off the
 * CPU for the whole click, 4 ms each step of the wheel. Only clicks under
 * 5 ms: the old wait yields for longer ones, and piezo_click_lost() doesn't.
 *
 * The click registers below the backlight fade (0) and plugins (1). A click
 * that finds the timer taken is waited out here as before; a fade or plugin
 * that takes the timer during a click finishes it in piezo_click_lost(). */
#define PIEZO_TIMER_PRIO    (-1)
/* Wait between checks while a click is on the timer, in ticks */
#define PIEZO_CLICK_CHECK   2

static volatile bool click_on_timer; /* sounding, and the timer ends it */
static long click_off;               /* USEC_TIMER when it ends */

/* The timer interrupt, at the click's end */
static void piezo_click_end(void)
{
    piezo_hw_stop();
    click_on_timer = false;
    timer_unregister();
}

/* Called inside the timer_register() of a fade or plugin taking the timer
 * during a click: the click ends at its time, keeping its length (a wait of
 * at most the click), and the timer is theirs. */
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
        if (pfn_timer == piezo_click_end)
            timer_unregister();
        piezo_hw_stop();
        click_on_timer = false;
    }
    restore_irq(oldlevel);
}

/* Whether a timed click still sounds. Some plugins call timer_unregister()
 * without owning the timer, which drops the click's interrupt without
 * piezo_click_lost(): such a click is ended here, at its time if that is
 * still to come. */
static bool piezo_click_sounding(void)
{
    if (!click_on_timer)
        return false;
    if (pfn_timer == piezo_click_end)
        return true;

    while (TIME_BEFORE(USEC_TIMER, click_off))
        if (duration >= 5000) yield();
    piezo_click_cancel();
    return false;
}

static void piezo_thread(void)
{
    struct queue_event ev;
    long piezo_usec_off;

    while(1)
    {
        if (click_on_timer)
            queue_wait_w_tmo(&piezo_queue, &ev, PIEZO_CLICK_CHECK);
        else
            queue_wait(&piezo_queue, &ev);

        /* asked for while a click sounds, under 4 ms after the last one
           began: dropped, as it would only lengthen the tone. The old wait
           never yielded, so no such request could be made then */
        if (piezo_click_sounding()
            && (ev.id == Q_PIEZO_BEEP || ev.id == Q_PIEZO_BEEP_FOR_TICK
                || ev.id == Q_PIEZO_BEEP_FOR_USEC))
            continue;

        switch(ev.id)
        {
            case Q_PIEZO_BEEP:
                piezo_hw_tick((unsigned int)ev.data);
                beeping = true;
                break;
            case Q_PIEZO_BEEP_FOR_TICK:
                piezo_hw_tick((unsigned int)ev.data);
                beeping = true;
                sleep(duration);
                if (beeping)
                    piezo_hw_stop();
                beeping = false;
                /* remove anything that appeared while sleeping */
                queue_clear(&piezo_queue);
                break;
            case Q_PIEZO_BEEP_FOR_USEC:
                piezo_usec_off = USEC_TIMER + duration;
                piezo_hw_tick((unsigned int)ev.data);
                click_off = piezo_usec_off;
                click_on_timer = true;
                if (duration < 5000
                    && timer_register(PIEZO_TIMER_PRIO, piezo_click_lost,
                                   (long)duration * (TIMER_FREQ / 1000000),
                                   piezo_click_end IF_COP(, CPU)))
                {
                    beeping = false;
                    break;
                }
                click_on_timer = false;
                /* the timer is the fade's or a plugin's */
                beeping = true;
                while (TIME_BEFORE(USEC_TIMER, piezo_usec_off))
                    if (duration >= 5000) yield();
                if (beeping)
                    piezo_hw_stop();
                beeping = false;
                /* remove anything that appeared while sleeping */
                queue_clear(&piezo_queue);
                break;
            case Q_PIEZO_STOP:
                piezo_click_cancel();
                if (beeping)
                    piezo_hw_stop();
                beeping = false;
                break;
#ifndef SIMULATOR
            case SYS_USB_CONNECTED:
                /*logf("USB: Piezo core");*/
                piezo_click_cancel();
                piezo_hw_stop();
                queue_clear(&piezo_queue);
                usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
                usb_wait_for_disconnect(&piezo_queue);
                break ;
#endif
            case SYS_TIMEOUT:
                break;
        }
        yield();
    }
}


void piezo_play(unsigned short inv_freq, unsigned char form)
{
    queue_post(&piezo_queue, Q_PIEZO_BEEP,
        (intptr_t)((unsigned int)form << 16 | inv_freq));
}

void piezo_play_for_tick(unsigned short inv_freq,
                    unsigned char form, unsigned int dur)
{
    duration = dur;
    queue_post(&piezo_queue, Q_PIEZO_BEEP_FOR_TICK,
        (intptr_t)((unsigned int)form << 16 | inv_freq));
}

void piezo_play_for_usec(unsigned short inv_freq,
                    unsigned char form, unsigned int dur)
{
    duration = dur;
    queue_post(&piezo_queue, Q_PIEZO_BEEP_FOR_USEC,
        (intptr_t)((unsigned int)form << 16 | inv_freq));
}

void piezo_stop(void)
{
    queue_post(&piezo_queue, Q_PIEZO_STOP, 0);
}

void piezo_clear(void)
{
    queue_clear(&piezo_queue);
    piezo_stop();
}

bool piezo_busy(void)
{
    return !queue_empty(&piezo_queue);
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
    queue_init(&piezo_queue, true);
    create_thread(piezo_thread, piezo_stack, sizeof(piezo_stack), 0,
            piezo_thread_name IF_PRIO(, PRIORITY_REALTIME)
            IF_COP(, CPU));
}

void piezo_button_beep(bool beep, bool force)
{
    /* old on clickwheel action - piezo_play_for_usec(50, 0x80, 400);
       old on button action - piezo_play_for_usec(50, 0x80, 3000); */

    if (force)
        piezo_clear();

    if (queue_empty(&piezo_queue))
    {
        if (beep)
            piezo_play_for_tick(40, 0x80, HZ/5);
        else
            piezo_play_for_usec(91, 0x80, 4000);
    }
}
