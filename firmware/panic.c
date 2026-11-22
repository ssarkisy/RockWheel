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

#ifdef HAVE_PANIC_STKOV_SCAN
/* thread_stkov()'s, for the record made next on the same core. Uncached, as
   the cores' entries share a line. */
static struct
{
    uintptr_t begin, end;       /* the overflowed thread's stack */
    uintptr_t sp;               /* its sp as switch_thread() saved it */
} stkov[NUM_CORES] NOCACHEBSS_ATTR;

void panic_record_stkov(const void *begin, const void *end, uintptr_t sp)
{
    stkov[CURRENT_CORE].begin = (uintptr_t)begin;
    stkov[CURRENT_CORE].end = (uintptr_t)end;
    stkov[CURRENT_CORE].sp = sp;
}

/* IRAM's end: the codecs' and plugins' follows the core's (plugin.lds) */
#if CONFIG_CPU == PP5022 || CONFIG_CPU == PP5024
#define IRAM_CODE_END 0x40020000
#else
#define IRAM_CODE_END 0x40018000
#endif

/* where code may run from: the core's .text, the codec's and the plugin's
   buffers, and all of IRAM. In the buffers and IRAM (IBSS, IDATA, the idle
   stacks and the codec thread's stack among it) data lies among the code:
   only stkov_after_call()'s test then tells a return address from it. */
static bool stkov_is_code(uintptr_t a)
{
    extern unsigned char _loadaddress[], _textend[];
    extern unsigned char codecbuf[], pluginbuf[];

    return (a >= (uintptr_t)_loadaddress && a < (uintptr_t)_textend)
        || (a >= (uintptr_t)codecbuf && a < (uintptr_t)codecbuf + CODEC_SIZE)
        || (a >= (uintptr_t)pluginbuf
            && a < (uintptr_t)pluginbuf + PLUGIN_BUFFER_SIZE)
        || (a >= 0x40000000 && a < IRAM_CODE_END);
}

/* a: an ARM return address, the code before it a call: a bl into code, or
   mov lr, pc before a bx, a mov pc or an ldr pc */
static bool stkov_after_call(uint32_t a)
{
    if ((a & 3) || !stkov_is_code(a) || !stkov_is_code(a - 8))
        return false;
    const uint32_t insn = *(const uint32_t *)(a - 4);
    if ((insn >> 28) == 0xf)
        return false;
    if ((insn & 0x0f000000) == 0x0b000000)
        return stkov_is_code(a + 4 + ((int32_t)(insn << 8) >> 6));
    return *(const uint32_t *)(a - 8) == 0xe1a0e00f
        && ((insn & 0x0ffffff0) == 0x012fff10       /* bx rm */
            || (insn & 0x0ffffff0) == 0x01a0f000    /* mov pc, rm */
            || (insn & 0x0c50f000) == 0x0410f000);  /* ldr pc, [...] */
}

/* The frame of the function a is in, as at its call that returns to a:
   its prologue's push of lr, the nearest before a, and the sp it then
   subtracts, as GCC lays this firmware's frames out; 0 when unknown, as for
   a function that moves sp otherwise */
static uint32_t stkov_frame(uint32_t a)
{
    uint32_t p = a - 8, size = 0;       /* a - 4 is the call */
    for (int i = 0; ; i++, p -= 4)
    {
        if (i >= 2048 || !stkov_is_code(p))
            return 0;
        const uint32_t insn = *(const uint32_t *)p;
        if ((insn & 0xffff4000) == 0xe92d4000)      /* push {..., lr} */
        {
            for (uint32_t r = insn & 0xffff; r; r &= r - 1)
                size += 4;
            break;
        }
        if (insn == 0xe52de004)                     /* str lr, [sp, #-4]! */
        {
            size = 4;
            break;
        }
    }
    /* sub sp, sp, #imm in the few after it, before any branch */
    for (int i = 0; i < 8 && (p += 4) < a - 4; i++)
    {
        const uint32_t insn = *(const uint32_t *)p;
        if ((insn & 0x0e000000) == 0x0a000000 || (insn & 0xf000) == 0xf000)
            break;                                  /* b, bl, or to pc */
        if ((insn & 0xfffff000) == 0xe24dd000)
        {
            const uint32_t imm = insn & 0xff, rot = (insn >> 7) & 0x1e;
            size += rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
        }
        else if ((insn & 0xf000) == 0xd000)
            return 0;                               /* sp changed otherwise */
    }
    return size;
}

struct stkov_walk
{
    uintptr_t slot, end;        /* where the last return address was */
    uint32_t a, pc;
};

/* the next return address up from w's: in the slot its function's frame
   puts the next, else the next word up that is one (but the record's pc,
   panicf_f's saved lr); 0 at the end */
static uint32_t stkov_up(struct stkov_walk *w)
{
    const uint32_t size = stkov_frame(w->a);
    uintptr_t p = w->slot + size;
    if (size && size < 0x10000 && p < w->end)
    {
        const uint32_t a = *(const uint32_t *)p;
        if (a != w->pc && stkov_after_call(a))
            return w->slot = p, w->a = a;
    }
    for (p = w->slot + 4; p < w->end; p += 4)
    {
        const uint32_t a = *(const uint32_t *)p;
        if (a != w->pc && stkov_after_call(a))
            return w->slot = p, w->a = a;
    }
    return 0;
}

/* The chain up from the lowest return address in [p, end) whose next two
   are where its frames put them, as is seldom so of a stale local that only
   looks like one (else from the lowest of all), to the stack's end or
   stop. Into out, at most STKOV_CHAIN; how many. */
#define STKOV_CHAIN 48
static int stkov_chain(uintptr_t p, uintptr_t end, uintptr_t stop,
                       uintptr_t stack_end, uint32_t pc, uint32_t *out)
{
    struct stkov_walk first = { 0, 0, 0, 0 };
    for (int tries = 0; p < end && tries < 32; p += 4)
    {
        const uint32_t a = *(const uint32_t *)p;
        if (a == pc || !stkov_after_call(a))
            continue;
        struct stkov_walk w = { p, stack_end, a, pc };
        if (!tries++)
            first = w;
        const uint32_t size1 = stkov_frame(a);
        if (!stkov_up(&w) || w.slot != p + size1)
            continue;
        const uint32_t size2 = stkov_frame(w.a);
        const uintptr_t slot1 = w.slot;
        if (stkov_up(&w) && w.slot == slot1 + size2)
        {
            first = (struct stkov_walk){ p, stack_end, a, pc };
            break;
        }
    }
    if (!first.a)
        return 0;
    int n = 0;
    do
        out[n++] = first.a;
    while (n < STKOV_CHAIN && stkov_up(&first) && first.slot < stop);
    return n;
}

/* the chains, the stale and the live: cache lines of their own, as only
   the core that saves the record writes them */
static uint32_t stkov_chains[2][STKOV_CHAIN] CACHEALIGN_ATTR;

/* The trace of a Stkov record, the deepest first.
   - Above the sp switch_thread() saved are the thread's calls as it
     switched out: the live chain. When that sp is below the stack's bottom,
     the overflow is still under way, and this chain is the one that
     overflowed.
   - Below this panic's own frames is what an overflow that has since
     returned left: the stale frames of the calls that went deepest, as far
     as later calls left them whole. The panic's frames are thread_stkov's,
     from panicf's sp up to the saved one, and panicf_f's below it, whose
     only return address is the record's pc. thread_stkov() names the
     thread without formatting, so that nothing else of the panic's is
     there.
   - Each chain is followed by its functions' prologues, as rb_backtrace's
     unwind stops in some of them (ata_transfer_sectors), and where a frame
     isn't known, by the next word that is a return address.
   - Ten return addresses are picked evenly from the stale chain, then the
     live one, the deepest and the outermost among them: the deepest ten
     alone would name only the leaf, not the nested screens that took it
     there. When a keyboard opened in a playlist picker from a context menu
     overflowed as the status bar's redraw read glyphs from a font file,
     those ten were all ata, fat and font calls.
   - Nothing below the stack's bottom is read but the live chain. */
static void stkov_trace(unsigned int core, uintptr_t panic_sp, uint32_t pc,
                        uint32_t *out)
{
    const uintptr_t begin = stkov[core].begin & ~3, end = stkov[core].end & ~3;
    const uintptr_t sp = stkov[core].sp & ~3;

    /* a stack, and a saved sp in it or at most 64 KB below it */
    if (begin >= end || end - begin > 0x100000 || sp > end
        || sp + 0x10000 < begin)
        return;

    uint32_t *const stale = stkov_chains[0], *const live = stkov_chains[1];
    const uintptr_t own = MIN(panic_sp, sp);
    const int s = own > begin ? stkov_chain(begin, own, own, end, pc, stale)
                              : 0;
    const int l = stkov_chain(sp, end, end, end, pc, live);

    const int n = s + l;
    for (int k = 0; k < PANIC_REC_TRACE && k < n; k++)
    {
        const int i = n <= PANIC_REC_TRACE ? k
                      : k * (n - 1) / (PANIC_REC_TRACE - 1);
        out[k] = i < s ? stale[i] : live[i - s];
    }
}
_Static_assert(PANIC_REC_TRACE >= 2, "stkov_trace() divides by it less 1");
#endif /* HAVE_PANIC_STKOV_SCAN */

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
#ifdef HAVE_PANIC_STKOV_SCAN
            if (stkov[CURRENT_CORE].end)
                stkov_trace(CURRENT_CORE, sp, pc, area->trace);
            else
#endif
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
