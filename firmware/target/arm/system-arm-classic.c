/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006 by Thom Johansen
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
#include "system.h"
#include <stdio.h>
#include "lcd.h"
#include "font.h"
#include "gcc_extensions.h"

#include <get_sp.h>
#include <backtrace.h>
#ifdef HAVE_PANIC_RECORD
#include "panic.h"
#include "panic_record.h"
#include "safe_read.h"
#endif

static const char* const uiename[] = {
    "Undefined instruction",
    "Prefetch abort",
    "Data abort",
    "Divide by zero",
    "SWI",
    "Jump to 0"
};

void __attribute__((weak,naked)) data_abort_handler(void)
{
    asm volatile(
        "sub    r0, lr, #8  \n"
        "mov    r1, #2      \n"
        "b      UIE         \n"
        );
}

void __attribute__((weak,naked)) software_int_handler(void)
{
    asm volatile(
        "sub    r0, lr, #4  \n"
        "mov    r1, #4      \n"
        "b      UIE         \n"
        );
}

void __attribute__((weak,naked)) reserved_handler(void)
{
    asm volatile(
        "sub    r0, lr, #4  \n"
        "mov    r1, #4      \n"
        "b      UIE         \n"
        );
}

void __attribute__((weak,naked)) prefetch_abort_handler(void)
{
    asm volatile(
        "sub    r0, lr, #4  \n"
        "mov    r1, #1      \n"
        "b      UIE         \n"
        );
}

void __attribute__((weak,naked)) undef_instr_handler(void)
{
    asm volatile(
        "sub    r0, lr, #4    \n"
#ifdef USE_THUMB
        "mrs    r1, spsr      \n"
        "tst    r1, #(1 << 5) \n" // T bit set ?
        "subne  r0, lr, #2    \n" // if yes, offset to THUMB instruction
#endif
        "mov    r1, #0        \n"
        "b      UIE           \n"
        );
}

/* An exception from crt0-pp.S's crash stacks themselves, a fault while a
   panic formats its message or draws: UIE's frames go over those its sp
   points at, so there is nothing to unwind */
static inline bool on_crash_stack(unsigned int sp)
{
#if defined(CPU_PP) && !defined(BOOTLOADER)
    extern unsigned char uie_stacks[], uie_stacks_end[];
    return sp >= (uintptr_t)uie_stacks && sp < (uintptr_t)uie_stacks_end;
#else
    (void)sp;
    return false;
#endif
}

/* Unexpected Interrupt or Exception handler. Currently only deals with
   exceptions, but will deal with interrupts later. sp and lr are those of
   the mode the exception came from (from crt0-pp.S's UIE, for __div0, a
   call, its caller's own).
 */
static void NORETURN_ATTR uie_show(unsigned int pc, unsigned int num,
                                   unsigned int sp, unsigned int lr)
{
    /* safe guard variable - we call backtrace() only on first
     * UIE call. This prevent endless loop if backtrace() touches
     * memory regions which cause abort
     */
    static bool triggered = false;

#if defined(HAVE_PANIC_RECORD) && defined(HAVE_RB_BACKTRACE)
    /* the unwind of the record, then the screen's, as one: nothing again
       after a fault in either */
    const bool unwind = !triggered && !on_crash_stack(sp);
    triggered = true;
    /* data_abort_handler kept r0-r7 (lib/unwarminder/safe_read.S) */
    panic_record_save(PANIC_KIND_UNDEF_INSTR + num, pc, lr, sp,
                      num == 2 ? data_abort_regs : NULL, uiename[num],
                      unwind);
#else
    (void)lr;
#endif

#if LCD_DEPTH > 1
    lcd_set_backdrop(NULL);
    lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
#endif
    unsigned line = 0;

    /* to the screen, should the fault have come while the HANG screen's
       text was drawn into its bitmap (hang_text.c) */
    lcd_glyph_sink_cancel();
    lcd_setfont(FONT_SYSFIXED);
    lcd_set_viewport(NULL);
    lcd_clear_display();
    lcd_putsf(0, line++, "%s at %08x" IF_COP(" (%d)"), uiename[num], pc IF_COP(, CURRENT_CORE));

#if !defined(CPU_ARM7TDMI) && (CONFIG_CPU != RK27XX) /* arm7tdmi has no MPU/MMU */
    if(num == 1 || num == 2) /* prefetch / data abort */
    {
        register unsigned status;

#if ARM_ARCH >= 6
        /* ARMv6 has 2 different registers for prefetch & data aborts */
        if(num == 1)    /* instruction prefetch abort */
            asm volatile( "mrc p15, 0, %0, c5, c0, 1\n" : "=r"(status));
        else
#endif
            asm volatile( "mrc p15, 0, %0, c5, c0, 0\n" : "=r"(status));

        lcd_putsf(0, line++, "FSR 0x%x", status);

        unsigned int domain = (status >> 4) & 0xf;
        unsigned int fault = status & 0xf;
#if ARM_ARCH >= 6
        fault |= (status & (1<<10)) >> 6; /* fault is 5 bits on armv6 */
#endif
        lcd_putsf(0, line++, "(domain %d, fault %d)", domain, fault);

        if(num == 2) /* data abort */
        {
            register unsigned address;
            /* read FAR (fault address register) */
            asm volatile( "mrc p15, 0, %0, c6, c0\n" : "=r"(address));
            lcd_putsf(0, line++, "address 0x%8x", address);
#if ARM_ARCH >= 6
            lcd_putsf(0, line++, (status & (1<<11)) ? "(write)" : "(read)");
#endif
        }
    }   /* num == 1 || num == 2 // prefetch/data abort */
#endif /* !defined(CPU_ARM7TDMI */

#if defined(HAVE_PANIC_RECORD) && defined(HAVE_RB_BACKTRACE)
    if (unwind)
        rb_backtrace(pc, sp, &line);
    panic_draw_request();
#elif defined(HAVE_RB_BACKTRACE)
    if (!triggered && !on_crash_stack(sp))
    {
        triggered = true;
        rb_backtrace(pc, sp, &line);
    }
#else
    (void)triggered;
    (void)sp;
#endif

    lcd_update();

    disable_interrupt(IRQ_FIQ_STATUS);

    system_exception_wait(); /* If this returns, try to reboot */
    system_reboot();
    while (1);       /* halt */
}

#if defined(CPU_PP) && !defined(BOOTLOADER)
/* From crt0-pp.S's UIE: in sys mode, interrupts off, on the core's own stack */
void NORETURN_ATTR UIE_sys(unsigned int pc, unsigned int num,
                           unsigned int sp, unsigned int lr);
void NORETURN_ATTR UIE_sys(unsigned int pc, unsigned int num,
                           unsigned int sp, unsigned int lr)
{
    uie_show(pc, num, sp, lr);
}
#else
void NORETURN_ATTR UIE(unsigned int pc, unsigned int num)
{
#ifdef HAVE_RB_BACKTRACE
    uie_show(pc, num, __get_sp(), __get_lr());
#else
    uie_show(pc, num, 0, 0);
#endif
}
#endif

/* Needs to be here or gcc won't find it */
void __attribute__((naked)) __div0(void)
{
    asm volatile (
        "ldr    r0, [sp]    \r\n"
        "sub    r0, r0, #4  \r\n"
        "mov    r1, #3      \r\n"
        "b      UIE         \r\n"
    );
}
