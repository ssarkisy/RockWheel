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

/* firmware/hang.c, the hang catcher's detectors, tick by tick: its phases,
 * holds and their re-arming, the two looks in a row, the UI thread alone,
 * and the drive's grace. The scheduler, the drive and the record are
 * stubbed; the file is built here as it is, with the catcher on. The UI
 * thread is the last of the CPU's to wait for a button (7 the main thread,
 * 9 a plugin's own); a long step marks itself alive. */

#define HAVE_HANG_CATCHER
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "kernel.h"
#include "check.h"

/* the stubs hang.c calls */
volatile long current_tick;
static unsigned int self = 1;
static unsigned long held[2];
static long trouble_at = -1;
static bool spinning_up;

unsigned int thread_self(void)
{
    return self;
}

/* the simulator's interrupt mask, which hang_hold() sets around a change */
static int irq_level;
int set_irq_level(int level)
{
    const int old = irq_level;
    irq_level = level;
    return old;
}

unsigned long thread_core_running_usec(unsigned int core)
{
    return core < 2 ? held[core] : 0;
}

static unsigned int gone; /* a thread that has ended */

bool thread_hang_exists(unsigned int thread_id)
{
    return thread_id && thread_id != gone;
}

bool ata_troubled_since(long tick)
{
    return spinning_up || (trouble_at >= 0 && !TIME_BEFORE(trouble_at, tick));
}

static jmp_buf fired_jmp;
static unsigned int fired_found, fired_ui;
static unsigned long fired_ms;

void panic_hang(unsigned int found, unsigned long ms, unsigned int ui)
{
    fired_found = found;
    fired_ms = ms;
    fired_ui = ui;
    longjmp(fired_jmp, 1);
}

#include "../../firmware/hang.c"

/* hang.c's state as at boot */
static void reset(void)
{
    phase = PHASE_BOOT;
    holds = 0;
    ui_thread = 0;
    ui_waiting = false;
    ui_left = 0;
    d2_armed = 0;
    countdown = 0;
    found_before = 0;
    memset(held, 0, sizeof(held));
    trouble_at = -1;
    spinning_up = false;
    self = 1;
    gone = 0;
    current_tick = 1000;
    fired_found = 0;
}

/* n ticks on: the tick at which it fired, or -1 */
static long run(long n)
{
    for (long i = 0; i < n; i++)
    {
        current_tick++;
        if (setjmp(fired_jmp))
            return current_tick;
        hang_tick();
    }
    return -1;
}

/* the UI's thread waits for a button and comes back */
static void ui_waits(void)
{
    hang_ui_wait(true);
    hang_ui_wait(false);
}

static void boot_and_start(void)
{
    reset();
    self = 7;            /* the UI thread */
    hang_start();
}

int main(void)
{
    check_case = "boot: both off";
    reset();
    held[CPU] = 60 * 1000000ul;
    CHECK_EQ(run(10 * HZ), -1);

    check_case = "start: D1 on, twice in a row";
    boot_and_start();
    held[CPU] = 9900000;
    CHECK_EQ(run(5 * HZ), -1);
    held[CPU] = 10500000;
    const long t0 = current_tick;
    const long at = run(HZ);
    CHECK(at > 0 && at - t0 <= 2 * HANG_CHECK_TICKS);
    CHECK_EQ(fired_found, HANG_FOUND_NO_YIELD | CPU);
    CHECK_EQ(fired_ms, 10500);

    check_case = "start: one look over, the next under";
    boot_and_start();
    for (int i = 0; i < 20; i++)
    {
        held[CPU] = (i & 1) ? 11000000 : 0;
        CHECK_EQ(run(HANG_CHECK_TICKS), -1);
    }

#if NUM_CORES > 1 /* not in the simulator's configuration, these tests' */
    check_case = "the COP's D1, from the CPU's tick";
    boot_and_start();
    held[COP] = 12000000;
    CHECK(run(HZ) > 0);
    CHECK_EQ(fired_found, HANG_FOUND_NO_YIELD | COP);
#endif

    check_case = "start: D2 off, as the dircache is waited for";
    boot_and_start();
    ui_waits();
    CHECK_EQ(run(120 * HZ), -1);

    check_case = "run: D2 from the UI's last wait";
    boot_and_start();
    hang_run();
    ui_waits();
    const long left = current_tick;
    CHECK_EQ(run(HANG_UI_AWAY_S * HZ - HANG_CHECK_TICKS), -1);
    const long at2 = run(5 * HZ);
    CHECK(at2 > left + HANG_UI_AWAY_S * HZ);
    CHECK(at2 <= left + HANG_UI_AWAY_S * HZ + 2 * HANG_CHECK_TICKS + 1);
    CHECK_EQ(fired_found, HANG_FOUND_UI_AWAY);
    CHECK_EQ(fired_ui, 7);
    CHECK(fired_ms > HANG_UI_AWAY_S * 1000
          && fired_ms <= (HANG_UI_AWAY_S + 1) * 1000);

    check_case = "run: polls keep it off, waits too";
    boot_and_start();
    hang_run();
    for (int i = 0; i < 600; i++)
    {
        ui_waits();
        CHECK_EQ(run(HZ / 2), -1);
    }
    hang_ui_wait(true);
    CHECK_EQ(run(120 * HZ), -1); /* a long wait for a button */
    hang_ui_wait(false);
    CHECK(run((HANG_UI_AWAY_S + 1) * HZ) > 0);

    check_case = "run: a plugin's own thread waits: it's the UI now";
    boot_and_start();
    hang_run();
    ui_waits();               /* main, then off into thread_wait() */
    self = 9;
    for (int i = 0; i < 2 * HANG_UI_AWAY_S; i++)
    {
        ui_waits();
        CHECK_EQ(run(HZ), -1);
    }
    ui_waits();
    const long left9 = current_tick;
    const long at3 = run((HANG_UI_AWAY_S + 1) * HZ + 2 * HANG_CHECK_TICKS);
    CHECK(at3 > left9 + HANG_UI_AWAY_S * HZ);
    CHECK_EQ(fired_ui, 9);

    check_case = "run: a plugin's thread ended: the main thread is named";
    boot_and_start();
    hang_run();
    ui_waits();
    self = 9;
    ui_waits();               /* the plugin's last wait */
    gone = 9;                 /* it ends; main hangs before it waits again */
    self = 7;
    CHECK(run((HANG_UI_AWAY_S + 1) * HZ + 2 * HANG_CHECK_TICKS) > 0);
    CHECK_EQ(fired_found, HANG_FOUND_UI_AWAY);
    CHECK_EQ(fired_ui, 7);

    check_case = "run: the main thread back at its buttons takes it again";
    boot_and_start();
    hang_run();
    self = 9;
    ui_waits();
    self = 7;
    ui_waits();
    const long left7 = current_tick;
    self = 9; /* the plugin's thread, gone quiet, isn't watched */
    CHECK(run((HANG_UI_AWAY_S + 1) * HZ + 2 * HANG_CHECK_TICKS)
          > left7 + HANG_UI_AWAY_S * HZ);
    CHECK_EQ(fired_ui, 7);

    check_case = "run: the old UI's return from its wait counts for nothing";
    boot_and_start();
    hang_run();
    self = 7;
    hang_ui_wait(true);       /* main waits */
    self = 9;
    hang_ui_wait(true);       /* the plugin's thread waits too: the UI now */
    self = 7;
    hang_ui_wait(false);      /* main comes back: not the UI any more */
    CHECK_EQ(run(60 * HZ), -1); /* the UI, 9, still waits */
    self = 9;
    hang_ui_wait(false);
    const long left9b = current_tick;
    CHECK(run((HANG_UI_AWAY_S + 1) * HZ + 2 * HANG_CHECK_TICKS)
          > left9b + HANG_UI_AWAY_S * HZ);
    CHECK_EQ(fired_ui, 9);

    check_case = "run: a long step marked alive is no absence";
    boot_and_start();
    hang_run();
    ui_waits();
    for (int i = 0; i < 3 * HANG_UI_AWAY_S; i++)
    {
        hang_ui_alive();      /* a playlist saved, a count shown */
        CHECK_EQ(run(HZ), -1);
    }
    hang_ui_alive();
    const long alive_end = current_tick;
    CHECK(run((HANG_UI_AWAY_S + 1) * HZ + 2 * HANG_CHECK_TICKS)
          > alive_end + HANG_UI_AWAY_S * HZ);

    check_case = "run: another thread's alive marks aren't the UI's";
    boot_and_start();
    hang_run();
    ui_waits();
    self = 9;
    long at6 = -1;
    for (int i = 0; i < 2 * HANG_UI_AWAY_S && at6 < 0; i++)
    {
        hang_ui_alive();
        at6 = run(HZ);
    }
    CHECK(at6 > 0);
    CHECK_EQ(fired_ui, 7);

    check_case = "run: an alive mark while waiting changes nothing";
    boot_and_start();
    hang_run();
    hang_ui_wait(true);
    hang_ui_alive();
    CHECK_EQ(run(60 * HZ), -1);
    hang_ui_wait(false);
    CHECK(run((HANG_UI_AWAY_S + 1) * HZ + 2 * HANG_CHECK_TICKS) > 0);

    check_case = "run: since run began, not before";
    boot_and_start();
    ui_waits();
    CHECK_EQ(run(60 * HZ), -1); /* start's long absence */
    hang_run();
    CHECK_EQ(run((HANG_UI_AWAY_S - 1) * HZ), -1);
    CHECK(run(2 * HZ + 2 * HANG_CHECK_TICKS) > 0);

    static const struct { unsigned int hold; bool d1_too; const char *name; }
        hold_cases[] = {
        { HANG_HOLD_USB, true, "USB" },
        { HANG_HOLD_SHUTDOWN, true, "shutdown" },
        { HANG_HOLD_ROLO, true, "ROLO" },
        { HANG_HOLD_ASLEEP, false, "asleep" },
    };
    for (unsigned int c = 0; c < ARRAYLEN(hold_cases); c++)
    {
        char name[64];
        snprintf(name, sizeof(name), "hold %s: D2 off, again from its end",
                 hold_cases[c].name);
        check_case = name;
        boot_and_start();
        hang_run();
        ui_waits();
        hang_hold(hold_cases[c].hold, true);
        CHECK_EQ(run(90 * HZ), -1);
        hang_hold(hold_cases[c].hold, false);
        const long released = current_tick;
        CHECK_EQ(run(HANG_UI_AWAY_S * HZ - HANG_CHECK_TICKS), -1);
        const long at4 = run(5 * HZ);
        CHECK(at4 > released + HANG_UI_AWAY_S * HZ);

        snprintf(name, sizeof(name), "hold %s: D1", hold_cases[c].name);
        check_case = name;
        boot_and_start();
        hang_run();
        ui_waits();
        hang_hold(hold_cases[c].hold, true);
        held[CPU] = 30 * 1000000ul;
        const long at5 = run(HZ);
        if (hold_cases[c].d1_too)
            CHECK_EQ(at5, -1);
        else
        {
            CHECK(at5 > 0);
            CHECK_EQ(fired_found, HANG_FOUND_NO_YIELD | CPU);
        }
    }

    check_case = "two holds: on until both end";
    boot_and_start();
    hang_run();
    ui_waits();
    hang_hold(HANG_HOLD_ASLEEP, true);
    hang_hold(HANG_HOLD_SHUTDOWN, true);
    hang_hold(HANG_HOLD_SHUTDOWN, false); /* a refused power-off asleep */
    CHECK_EQ(run(90 * HZ), -1);
    hang_hold(HANG_HOLD_ASLEEP, false);
    CHECK_EQ(run((HANG_UI_AWAY_S - 1) * HZ), -1);
    CHECK(run(2 * HZ + 2 * HANG_CHECK_TICKS) > 0);

    check_case = "a hold's change leaves the interrupts as they were";
    CHECK_EQ(irq_level, 0);

    check_case = "a hold not held: no re-arm";
    boot_and_start();
    hang_run();
    ui_waits();
    CHECK_EQ(run(10 * HZ), -1);
    hang_hold(HANG_HOLD_USB, false);
    CHECK(run(11 * HZ + 2 * HANG_CHECK_TICKS) > 0);

    check_case = "the drive: 30 s more when it spun up or reset meanwhile";
    boot_and_start();
    hang_run();
    ui_waits();
    CHECK_EQ(run(5 * HZ), -1);
    trouble_at = current_tick;
    CHECK_EQ(run((HANG_UI_AWAY_S + HANG_ATA_GRACE_S) * HZ - 5 * HZ
                 - HANG_CHECK_TICKS), -1);
    CHECK(run(5 * HZ) > 0);

    check_case = "the drive: a reset before the absence gives none";
    boot_and_start();
    hang_run();
    trouble_at = current_tick - 1;
    current_tick += 1;
    ui_waits();
    CHECK(run((HANG_UI_AWAY_S + 1) * HZ + 2 * HANG_CHECK_TICKS) > 0);

    check_case = "the drive: spinning up now";
    boot_and_start();
    hang_run();
    ui_waits();
    spinning_up = true;
    CHECK_EQ(run(40 * HZ), -1);
    spinning_up = false;
    CHECK(run(HZ) > 0);

    check_case = "the tick's clock wraps";
    boot_and_start();
    current_tick = 0x7fffffff - 5 * HZ;
    hang_run();
    ui_waits();
    CHECK_EQ(run((HANG_UI_AWAY_S - 1) * HZ), -1);
    CHECK(run(2 * HZ + 2 * HANG_CHECK_TICKS) > 0);

    return check_done("hang");
}
