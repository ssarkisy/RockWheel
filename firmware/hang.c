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

/* The hang catcher's two detectors (hang.h). Everything here is read and
 * written on the CPU: by its threads, which switch only where one yields,
 * and by its tick, which reads what they write, one word at a time. The
 * holds are also set from an interrupt (a held button's sys_poweroff(),
 * button.c), so their changes are made with it off. */

#include "config.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "hang.h"
#if (CONFIG_STORAGE & STORAGE_ATA)
#include "ata.h"
#endif

#ifdef HAVE_HANG_CATCHER

enum { PHASE_BOOT, PHASE_START, PHASE_RUN };

#define D1_HOLDS (HANG_HOLD_USB | HANG_HOLD_SHUTDOWN | HANG_HOLD_ROLO)
#define D2_HOLDS (D1_HOLDS | HANG_HOLD_ASLEEP)

static unsigned char phase;          /* PHASE_* */
static unsigned int holds;           /* enum hang_hold's */
static unsigned int ui_thread;       /* the thread D2 watches: the last to
                                        wait for a button */
static unsigned int ui_start;        /* hang_start()'s, the main thread */
static bool ui_waiting;              /* in its button wait */
static long ui_left;                 /* when its last button wait ended */
static long d2_armed;                /* when D2 last came on */
static unsigned char countdown;      /* ticks since the last look */
static unsigned int found_before;    /* what the last look found */

static inline void d2_arm(void)
{
    d2_armed = current_tick;
}

void hang_start(void)
{
    ui_thread = ui_start = thread_self();
    phase = PHASE_START;
}

void hang_run(void)
{
    d2_arm();
    phase = PHASE_RUN;
}

void hang_hold(unsigned int hold, bool on)
{
    const int oldlevel = disable_irq_save();
    if (on)
        holds |= hold;
    else if (holds & hold)
    {
        holds &= ~hold;
        d2_arm();
    }
    restore_irq(oldlevel);
}

/* The UI is whichever thread of the CPU waits for buttons, the main thread or
   a plugin's own (puzzles, the SDL games, pdbox), and D2 follows it from its
   next wait: the main thread waits on that one then, as in thread_wait().
   ui_waiting is the last waiter's: two threads in waits at once, the one that
   came back and stopped reading buttons while the other stays in its wait
   would count as away. No thread of the shipped set waits beside the UI's:
   every reader of the queue (the debug screens, the speller, USB's screen,
   ROLO) runs on it. */
void hang_ui_wait(bool waiting)
{
#if NUM_CORES > 1
    if (CURRENT_CORE != CPU) /* written for the CPU's tick, cached */
        return;
#endif
    const unsigned int self = thread_self();
    if (waiting)
        ui_thread = self;
    else if (self != ui_thread)
        return;
    else
        ui_left = current_tick;
    ui_waiting = waiting;
}

void hang_ui_alive(void)
{
#if NUM_CORES > 1
    if (CURRENT_CORE != CPU) /* as hang_ui_wait() */
        return;
#endif
    if (!ui_waiting && thread_self() == ui_thread)
        ui_left = current_tick;
}

/* What a look finds now, 0 for nothing; *ms how long */
static unsigned int look(unsigned long *ms)
{
    if (phase == PHASE_BOOT || (holds & D1_HOLDS))
        return 0;

    for (unsigned int core = 0; core < NUM_CORES; core++)
    {
        const unsigned long usec = thread_core_running_usec(core);
        if (usec > HANG_NO_YIELD_S * 1000000ul)
        {
            *ms = usec / 1000;
            return HANG_FOUND_NO_YIELD | core;
        }
    }

    if (phase != PHASE_RUN || (holds & D2_HOLDS) || ui_waiting)
        return 0;
    /* from its last wait, or from when D2 came on if that was later */
    const long since = TIME_AFTER(d2_armed, ui_left) ? d2_armed : ui_left;
    long limit = HANG_UI_AWAY_S * HZ;
#if (CONFIG_STORAGE & STORAGE_ATA)
    if (ata_troubled_since(since))
        limit += HANG_ATA_GRACE_S * HZ;
#endif
    if (!TIME_AFTER(current_tick, since + limit))
        return 0;
    *ms = (current_tick - since) * (1000 / HZ);
    return HANG_FOUND_UI_AWAY;
}

void hang_tick(void)
{
    if (++countdown < HANG_CHECK_TICKS)
        return;
    countdown = 0;

    /* the same twice in a row: once may be a core that has just been woken
       from idle by this very tick, its run queue no longer empty and its
       thread not picked yet */
    unsigned long ms = 0;
    const unsigned int found = look(&ms);
    /* a plugin's UI thread that has ended since its last wait names nothing:
       the main thread then, which is what is away until it waits again */
    if (found && found == found_before)
        panic_hang(found, ms, thread_hang_exists(ui_thread) ? ui_thread
                                                            : ui_start);
    found_before = found;
}

#endif /* HAVE_HANG_CATCHER */
