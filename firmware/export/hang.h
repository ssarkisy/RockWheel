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
#ifndef _HANG_H_
#define _HANG_H_

/* The hang catcher (HAVE_HANG_CATCHER, firmware/hang.c), from the kernel's
 * tick on the CPU, every HANG_CHECK_TICKS:
 * - D1: a core whose thread has kept it HANG_NO_YIELD_S without going
 *   through switch_thread() (no yield, sleep or wait), its run queue not
 *   empty: a spin with interrupts on, on either core;
 * - D2: the UI thread away from its button queue HANG_UI_AWAY_S, from the
 *   end of one button_get_w_tmo() wait to the next call: blocked, or busy
 *   while it yields. The UI thread is the last of the CPU's to wait for a
 *   button. A plugin that works longer without reading one shows the HANG
 *   screen: the shipped ones read them (extras_menu.c).
 * Either, at two looks in a row, makes a crash record of kind
 * PANIC_KIND_HANG and shows the HANG screen, which waits for Menu+Select
 * (panic_hang(), panic.c); the next boot writes the record to crash.log.
 *
 * The limits come from a calibration of a day's use on the owner's Video and
 * photo (2026-11-21/22): the longest legitimate stretch was 1.46 s (D1,
 * Updating Library's letters on 13k songs) and absence 4.66 s (D2, a play of
 * the whole library); 3 times those are under the floors set beforehand,
 * which stand. D2 is off before root_menu()'s loop, where the wait for the
 * dircache alone took 28 s. */

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "system.h"

#define HANG_CHECK_TICKS     (HZ / 10)
#define HANG_NO_YIELD_S      10
#define HANG_UI_AWAY_S       20
/* D2's more when the drive spun up or was reset during the absence: about
   30 s, a drive's own worst for a soft reset (ata.c, perform_soft_reset(),
   whose retries wait longer still), which a UI reading from it waits */
#define HANG_ATA_GRACE_S     30
/* the HANG screen's light, out after this long without a button */
#define HANG_LIGHT_S         60

/* What fired, for the record: the detector, and D1's core */
#define HANG_FOUND_NO_YIELD  0x100 /* | the core */
#define HANG_FOUND_UI_AWAY   0x200

/* Times either detector is held off. D2 starts its count again as each ends,
   as what it held would otherwise count as the UI's absence. */
enum hang_hold
{
    HANG_HOLD_USB      = 0x01, /* both: the UI's USB screen (misc.c) */
    HANG_HOLD_SHUTDOWN = 0x02, /* both: a power-off or reboot asked for, until
                                  refused (powermgmt.c) */
    HANG_HOLD_ROLO     = 0x04, /* both: another firmware loaded (rolo.c) */
    HANG_HOLD_ASLEEP   = 0x08, /* D2: the iPod's idle sleep (powermgmt.c) */
};

#ifdef HAVE_HANG_CATCHER
/* root_menu() begins: D1 on (until then, boot's long steps, both off) */
void hang_start(void);
/* root_menu()'s loop: D2 on too, for the thread that called hang_start() */
void hang_run(void);
void hang_hold(unsigned int hold, bool on);
/* button_get_w_tmo(), before (true) and after its wait */
void hang_ui_wait(bool waiting);
/* A long step of the UI that reads no button meanwhile, going on: as a wait
   for one, for D2. In the loop of each that may take longer than D2's limit
   (a whole library's playlist saved), never in one that could spin for good
   while it yields. */
void hang_ui_alive(void);
/* TIMER1(), each tick, with interrupts off */
void hang_tick(void);

/* thread.c, read on the CPU while the other core goes on: how long the
   thread a core runs has kept it, 0 when the core has none to run */
unsigned long thread_core_running_usec(unsigned int core);
/* the thread of that id is still there (thread.c), from the tick too */
bool thread_hang_exists(unsigned int thread_id);

/* A thread as the HANG record names it (thread.c), read as it is */
struct thread_hang_info
{
    char name[16];          /* as the thread list shows it */
    char owner[16];         /* the thread it is blocked on, "" if none */
    const void *wq;         /* the wait queue it is blocked on, or NULL */
    const char *state;      /* "running", "ready", "sleeping", "blocked"... */
    uintptr_t stack_begin, stack_end;
    /* its context as switch_thread() saved it, stale if running: sp after
       switch_thread()'s own push, lr a return inside switch_thread(), r4-r11
       switch_thread()'s by then (the caller's are in that push, panic.c) */
    uint32_t sp, lr;
    uint32_t regs[8];
    unsigned int core;
    bool running;           /* its core's running thread */
    bool overflowed;        /* its stack's guard word is gone */
};

/* core >= 0: the thread that core runs; else the one of thread_id. false if
   there is none. */
bool thread_get_hang_info(int core, unsigned int thread_id,
                          struct thread_hang_info *info);

/* The target's, for the HANG screen with interrupts off: the panel woken by
   polling, with no tick, event or yield, if it slept (the LCD driver); the
   light on at its brightness, without the fade's timer (the backlight's) */
void lcd_awake_crash(void);
void backlight_hw_crash_on(void);
void backlight_hw_crash_off(void);
/* the click wheel read by polling, with interrupts off: true if a packet came
   with a button down or the wheel touched (button-clickwheel.c) */
bool button_crash_pressed(void);
#else
static inline void hang_start(void) {}
static inline void hang_run(void) {}
static inline void hang_hold(unsigned int hold, bool on)
{
    (void)hold;
    (void)on;
}
static inline void hang_ui_wait(bool waiting)
{
    (void)waiting;
}
static inline void hang_ui_alive(void) {}
#endif /* HAVE_HANG_CATCHER */

#ifdef HAVE_HANG_SCREEN
/* The HANG screen's text, drawn beforehand in the UI font as the language
 * and the theme are set (apps/hang_text.c), since at the hang no font may
 * be read, and blitted at the hang (panic.c). Each line is a mono bitmap
 * (lcd_mono_bitmap()'s form: a byte 8 rows, its lowest bit the top) of its
 * own width and the font's height, one after another in bits. Without it,
 * as while it is being drawn, the screen is in English in the system font.
 * The memory is always kept (the owner's choice (A), 2026-11-20): nothing is
 * allocated for it, while music plays least of all. */
/* Measured in the simulator in every language and each theme's UI font: at
   most 4056 bytes and 7 lines on the Video's screen, 3570 and 8 on the
   photo's (Hindi), whose 17-row lines take three bytes a column too */
#define HANG_TEXT_BYTES 4608
#define HANG_TEXT_LINES 10

struct hang_text
{
    volatile bool ready;
    unsigned char lines;
    unsigned char height;                  /* a line's rows */
    unsigned fg[2], bg;                    /* colours: the text, the note */
    short x[HANG_TEXT_LINES], y[HANG_TEXT_LINES]; /* on the screen */
    unsigned short width[HANG_TEXT_LINES];
    unsigned char colour[HANG_TEXT_LINES]; /* 0 the text, 1 the note */
    unsigned char bits[HANG_TEXT_BYTES];
};

extern struct hang_text hang_text;

/* What fired (HANG_FOUND_*), for how long in ms, and D2's thread, from
   hang_tick() with interrupts off: the record and the HANG screen, on the
   CPU's crash stack; never returns. In the simulator, for its screen and
   its record (no detector runs there). */
void panic_hang(unsigned int found, unsigned long ms, unsigned int ui)
    NORETURN_ATTR;
#endif /* HAVE_HANG_SCREEN */

#endif /* _HANG_H_ */
