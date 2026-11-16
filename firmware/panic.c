/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by wavey@wavey.org
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

#include "config.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "panic.h"
#include "lcd.h"
#include "font.h"
#include "debug.h"
#include "led.h"
#include "power.h"
#include "system.h"
#include "logf.h"
#include "rbversion.h"

#ifdef HAVE_RB_BACKTRACE
#include "gcc_extensions.h"
#include "backtrace.h"
#endif
#ifdef HAVE_PANIC_RECORD
#include "panic_record.h"
#include "kernel.h"
#include "corelock.h"
#include "rbpaths.h"
#ifdef SIMULATOR
#include "file.h"
#endif
#endif

#if (defined(CPU_MIPS) && (CONFIG_PLATFORM & PLATFORM_NATIVE))
/* TODO: see comment above exception_dump in system-mips.c */
char panic_buf[128];
#else
static char panic_buf[128];
#endif

#define LINECHARS (LCD_WIDTH/SYSFONT_WIDTH) - 2

static void NORETURN_ATTR panic_finish(int pc, int sp, const char *fmt,
                                       va_list *ap);

#if defined(CPU_PP) && defined(HAVE_RB_BACKTRACE) && !defined(BOOTLOADER) \
    && (CONFIG_PLATFORM & PLATFORM_NATIVE)
/* crt0-pp.S: fn(pc, sp, fmt, ap) on the core's own crash stack, UIE's */
void NORETURN_ATTR uie_stack_call(int pc, int sp, const char *fmt,
                                  va_list *ap,
                                  void (*fn)(int, int, const char *,
                                             va_list *));
#define PANIC_OWN_STACK
#endif

#ifdef HAVE_PANIC_RECORD
/* Uncached, as a panic on the COP and one on the CPU see each other's */
static int panic_saving NOCACHEBSS_ATTR;    /* a save under way */
static int panic_saved NOCACHEBSS_ATTR;     /* this boot's record is in */
static unsigned long panic_clock SHAREDBSS_ATTR;
#ifdef HAVE_CORELOCK_OBJECT
/* the save, one core at a time: the other skips its own */
static struct corelock panic_cl NOCACHEBSS_ATTR;
#endif

void panic_record_set_clock(unsigned long secs)
{
    panic_clock = secs;
}

#ifdef SIMULATOR
/* The simulator's RAM ends with it: its record goes to a file, which the next
   boot's crash_log_boot() reads back */
static void panic_record_sim_store(const struct panic_record *area)
{
    int fd = open(PANIC_RECORD_SIM_FILE, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if (fd >= 0)
    {
        write(fd, area, PANIC_REC_RESERVE);
        close(fd);
    }
}
#endif

void panic_record_save(unsigned int kind, unsigned int pc, unsigned int lr,
                       unsigned int sp, const unsigned int *regs,
                       const char *msg, bool unwind)
{
    struct panic_record *area = panic_record_area();

    /* a fault inside the save leaves what it had written */
    if (!area || panic_saving)
        return;
#ifdef HAVE_CORELOCK_OBJECT
    if (!corelock_try_lock(&panic_cl))
        return;
#endif
    panic_saving = 1;
    panic_record_vote(area);    /* no reset since it was written, or none */
    if (panic_saved)
        panic_record_bump(area); /* what followed the first matters less */
    else
    {
        /* threads exist once the tick runs */
        panic_record_write(area, kind, CURRENT_CORE, pc, lr,
                           sp, current_tick ? thread_self_name() : NULL,
                           RBVERSION, msg);
        area->magic = 0;
        for (unsigned int i = 0; regs && i < 8; i++)
            area->regs[i] = regs[i];
        area->uptime = current_tick;
        area->wall = panic_clock ? panic_clock + current_tick / HZ : 0;
        panic_record_seal(area);
        panic_record_replicate(area);
        panic_saved = 1;
#ifdef BACKTRACE_UNWARMINDER
        if (unwind)
        {
            /* into copy 0 alone: if the unwind faults, copies 1 and 2 keep
               the record without its trace, for the boot's vote */
            area->magic = 0;
            rb_backtrace_collect(pc, sp, (unsigned int *)area->trace,
                                 PANIC_REC_TRACE);
            panic_record_seal(area);
        }
#else
        (void)unwind;
#endif
    }
    panic_record_replicate(area);
#ifdef SIMULATOR
    panic_record_sim_store(area);
#endif
    panic_saving = 0;
#ifdef HAVE_CORELOCK_OBJECT
    corelock_unlock(&panic_cl);
#endif
}

void panic_draw_request(void)
{
    if (!panic_saved)
        return;
    /* at the foot, over what reached it */
    const int h = SYSFONT_HEIGHT, rows = LCD_HEIGHT / h;
    lcd_set_viewport(NULL);
    lcd_setfont(FONT_SYSFIXED);
    lcd_set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
    lcd_fillrect(0, (rows - 3) * h, LCD_WIDTH, 3 * h);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_puts(1, rows - 2, "After the restart, send " CRASH_LOG_SHOWN);
    lcd_puts(1, rows - 1, "to " CRASH_REPORT_ADDRESS);
}
#endif /* HAVE_PANIC_RECORD */

int panic_last_row(void)
{
    int rows = LCD_HEIGHT / SYSFONT_HEIGHT;
#ifdef HAVE_PANIC_RECORD
    if (panic_saved)
        rows -= 3;              /* panic_draw_request()'s */
#endif
    return rows;
}

#if defined(CPU_ARM) && defined(HAVE_RB_BACKTRACE)
void panicf_f( const char *fmt, ...) USED_ATTR;

/* we wrap panicf() here with naked function to catch SP value */
void __attribute__((naked)) panicf( const char *fmt, ...)
{
    (void)fmt;
    asm volatile ("mov r4, sp \n"
                  "b panicf_f \n"
                 );
    while (1);
}

/*
 * "Dude. This is pretty fucked-up, right here."
 */
void panicf_f( const char *fmt, ...)
{
    int sp;

    asm volatile ("mov %[SP],r4 \n"
                  : [SP] "=r" (sp)
                 );

    int pc = (int)__builtin_return_address(0);
#elif defined(BACKTRACE_MIPSUNWINDER)
void panicf( const char *fmt, ... )
{
    /* NOTE: these are obtained by the backtrace lib */
    const int pc = 0;
    const int sp = 0;
#else
void panicf( const char *fmt, ...)
{
    const int pc = 0;
    const int sp = 0;
#endif
    va_list ap;

#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
    /* Disable interrupts */
#if defined(CPU_ARM_CLASSIC)
    disable_interrupt(IRQ_FIQ_STATUS);
#else
    set_irq_level(DISABLE_INTERRUPTS);
#endif
#endif /* SIMULATOR */

    va_start( ap, fmt );
#ifdef PANIC_OWN_STACK
    /* the rest, the message's formatting too, not on the thread's stack: it
       may be the one that overflowed (thread_stkov), or too small. ap points
       into this frame and the caller's, which nothing writes now. */
    uie_stack_call(pc, sp, fmt, &ap, panic_finish);
#else
    panic_finish(pc, sp, fmt, &ap);
#endif
}

/* interrupts off; never returns, so panicf_f's va_end is never reached: on
   ARM and the hosts it does nothing */
static void panic_finish(int pc, int sp, const char *fmt, va_list *ap)
{
    (void)pc;
    (void)sp;
    vsnprintf( panic_buf, sizeof(panic_buf), fmt, *ap );

#ifdef HAVE_PANIC_RECORD
#if defined(CPU_ARM) && defined(HAVE_RB_BACKTRACE)
    panic_record_save(PANIC_KIND_PANICF, pc, 0, sp, NULL, panic_buf, true);
#else
    panic_record_save(PANIC_KIND_PANICF, 0, 0, 0, NULL, panic_buf, false);
#endif
#endif

    lcd_set_viewport(NULL);

    int y = 1;

#if LCD_DEPTH > 1
    lcd_set_backdrop(NULL);
    lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
#endif

    lcd_clear_display();
    lcd_setfont(FONT_SYSFIXED);
    lcd_puts(1, y++, (unsigned char *) "*PANIC* (" RBVERSION ")");
    {
        /* wrap panic line */
        int i, len = strlen(panic_buf);
        for (i=0; i<len; i+=LINECHARS) {
            unsigned char c = panic_buf[i+LINECHARS];
            panic_buf[i+LINECHARS] = 0;
            lcd_puts(1, y++, (unsigned char *)panic_buf+i);
            panic_buf[i+LINECHARS] = c;
        }
    }

#if defined(HAVE_RB_BACKTRACE)
    rb_backtrace(pc, sp, &y);
#endif
#ifdef ROCKBOX_HAS_LOGF
    logf_panic_dump(&y);
#endif
#ifdef HAVE_PANIC_RECORD
    panic_draw_request();
#endif

    lcd_update();
    DEBUGF("%s", panic_buf);

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (cpu_boost_lock())
    {
        set_cpu_frequency(0);
        cpu_boost_unlock();
    }
#endif /* HAVE_ADJUSTABLE_CPU_FREQ */

#ifdef HAVE_ATA_POWER_OFF
    ide_power_enable(false);
#endif

    system_exception_wait(); /* if this returns, try to reboot */
    system_reboot();
    while (1);       /* halt */
}
