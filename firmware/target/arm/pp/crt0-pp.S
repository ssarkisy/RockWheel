/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Linus Nielsen Feltzing
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
#include "cpu.h"

#if defined(HAVE_BOOTDATA) && !defined(BOOTLOADER)
#include "bootdata.h"
#endif

    .section .init.text,"ax",%progbits

    .global    start
start:

/* PortalPlayer bootloader and startup code based on startup.s from the iPodLinux
 * loader
 *
 * Copyright (c) 2003, Daniel Palffy (dpalffy (at) rainstorm.org)
 * Copyright (c) 2005, Bernard Leach <leachbj@bouncycastle.org>
 *
 */
#if CONFIG_CPU == PP5002
    .equ    PROC_ID,     0xc4000000
    .equ    CPU_ICLR,    0xcf001028
    .equ    CPU_CTRL,    0xcf004054
    .equ    COP_ICLR,    0xcf001038
    .equ    COP_CTRL,    0xcf004058
    .equ    CPU_STATUS,  0xcf004050
    .equ    COP_STATUS,  0xcf004050
    .equ    SLEEP,       0x000000ca
    .equ    WAKE,        0x000000ce
    .equ    CPUSLEEPING, 0x00008000
    .equ    COPSLEEPING, 0x00004000
    .equ    CACHE_CTRL,  0xcf004024
    .equ    MMAP_LOG,    0xf000f000 /* MMAP0 */
    .equ    MMAP_PHYS,   0xf000f004
#if MEMORYSIZE > 32
    .equ    MMAP_MASK,   0x00003c00
#else
    .equ    MMAP_MASK,   0x00003e00
#endif
    .equ    MMAP_FLAGS,  0x00003f84
#else
    .equ    PROC_ID,     0x60000000
    .equ    CPU_ICLR,    0x60004028
    .equ    CPU_CTRL,    0x60007000
    .equ    CPU_STATUS,  0x60007000
    .equ    COP_ICLR,    0x60004038
    .equ    COP_CTRL,    0x60007004
    .equ    COP_STATUS,  0x60007004
    .equ    SLEEP,       0x80000000
    .equ    WAKE,        0x00000000
    .equ    CPUSLEEPING, 0x80000000
    .equ    COPSLEEPING, 0x80000000
    .equ    CACHE_CTRL,  0x6000c000
    .equ    MMAP_LOG,    0xf000f000 /* MMAP0 */
    .equ    MMAP_PHYS,   0xf000f004
#if MEMORYSIZE > 32
    .equ    MMAP_MASK,   0x00003c00
#else
    .equ    MMAP_MASK,   0x00003e00
#endif
    .equ    MMAP_FLAGS,  0x00000f84
#endif

    msr    cpsr_c, #0xd3 /* enter supervisor mode, disable IRQ/FIQ */
    b      pad_skip

.space 6*4  /* pad to offset 0x20 */

    .ascii "Rockbox"    /* signature for bootloader checking osos */
    .byte 1             /* osos boot version, only 1 exists for now */

.space 56*4 /* (more than enough) space for exception vectors and mi4 magic */

pad_skip:
    /* Find out which processor we are - r0 should be preserved for the
     * duration of the init to avoid constant reloading of the processor ID.
     * For each stage, CPU proceeds first, then COP.
     */
    ldr    r0, =PROC_ID
    ldrb   r0, [r0]

    /* We need to remap memory from wherever SDRAM is mapped natively, to
       base address 0, so we can put our exception vectors there. We don't
       want to do this remapping while executing from SDRAM, so we copy the
       remapping code to IRAM, then execute from there. Hence, the following
       code is compiled for address 0, but is currently executing at either
       0x28000000 or 0x10000000, depending on chipset version. Do not use any
       absolute addresses until remapping has been done. */

    /* Cores are stepped though the init in turn: CPU then COP. The the remap
       stage is completed by each core in turn and then the COP waits for the
       CPU to finish initializing its kernel where the CPU will wake the COP
       and wait for the COP to finish. This ensures no threading activity
       starts until it is safe. */
    cmp    r0, #0x55

    /* mask all interrupt sources before setting anything up */
    ldreq  r2, =CPU_ICLR
    ldrne  r2, =COP_ICLR
    mvn    r1, #0
    str    r1, [r2]

    /* put us (co-processor) to sleep and wait for CPU to remap */
    ldrne  r2, =COP_CTRL
    movne  r1, #SLEEP
    strne  r1, [r2]
    nop
    nop
    nop

    /* wait for co-processor to sleep then CPU can begin its remapping */
    ldreq  r2, =COP_STATUS
1:
    ldreq  r1, [r2]
    tsteq  r1, #COPSLEEPING
    beq    1b

    /* disable cache and local interrupt vectors - it is really not desireable
       to have them enabled here */
    ldr    r2, =CACHE_CTRL
    mov    r1, #0
    str    r1, [r2]

#if defined(IPOD_VIDEO)
    /* detect 32mb vs 64mb model */
    /* we do this here because after SDRAM is remapped, we already assumed */
    /* its size to be whatever we were compiled for. */

    mov    r2, #0x12000000
    mov    r3, #64
    strb   r3, [r2, #-1]   /* first write 64 to last byte of first 32MB bank */

    mov    r2, #0x14000000
    mov    r3, #32
    strb   r3, [r2, #-1]   /* now write 32 to last byte of second 32MB bank */

    /* now the last word of the first 32MB bank tells you the RAM size */
    /* since on a 32MB model both writes will touch the same actual location */
    /* this is read later on in boot */
#endif

    mov    r2, #0x40000000
    ldr    r3, =remap_start
    ldr    r4, =remap_end

    and    r6, pc, #0xff000000 /* adjust for execute address */
    orr    r3, r3, r6
    orr    r4, r4, r6

    /* copy the code to 0x40000000 */
1:
    ldr    r5, [r3], #4
    str    r5, [r2], #4
    cmp    r3, r4
    blo    1b

    ldr    r4, =MMAP_FLAGS
    orr    r4, r4, r6      /* adjust for execute address */
    ldr    r3, =MMAP_PHYS
    ldr    r2, =MMAP_MASK  /* ldr is more flexible */
    ldr    r1, =MMAP_LOG
    mov    pc, #0x40000000

remap_start:
    str    r2, [r1]
    str    r4, [r3]
    ldr    r1, L_post_remap
    bx     r1
L_post_remap:
    .word remap_end
remap_end:

    cmp    r0, #0x55
    ldr    r4, =COP_CTRL
    /* Wakeup co-processor to let it do remappings */
    moveq  r3, #WAKE
    /* Sleep us (co-processor) and wait for CPU to do kernel initialization */
    movne  r3, #SLEEP
    str    r3, [r4]
    nop
    nop
    nop

    /* Jump to co-processor init */
    ldrne  pc, =cop_init

cpu_init:
    /* Wait for COP to go to sleep before proceeding */
    ldr    r4, =COP_STATUS
1:
    ldr    r3, [r4]
    tst    r3, #COPSLEEPING
    beq    1b

    /* Vectors and IRAM copy is done first since they are reclaimed for
     * other uninitialized sections */

    /* Copy exception handler code to address 0 */
    ldr    r2, =_vectorsstart
    ldr    r3, =_vectorsend
    ldr    r4, =_vectorscopy
1:
    cmp    r3, r2
    ldrhi  r5, [r4], #4
    strhi  r5, [r2], #4
    bhi    1b

    /* Copy the IRAM */
    ldr    r2, =_iramcopy
    ldr    r3, =_iramstart
    ldr    r4, =_iramend
1:
    cmp    r4, r3
    ldrhi  r5, [r2], #4
    strhi  r5, [r3], #4
    bhi    1b

#ifdef HAVE_INIT_ATTR
    /* copy init code to codec buffer */
    ldr    r2, =_initstart
    ldr    r3, =_initend
    ldr    r4, =_initcopy

1:
    cmp     r3, r2
    ldrhi   r5, [r4], #4
    strhi   r5, [r2], #4
    bhi 1b
#endif

    /* Zero out IBSS */
    ldr    r2, =_iedata
    ldr    r3, =_iend
    mov    r4, #0
1:
    cmp    r3, r2
    strhi  r4, [r2], #4
    bhi    1b

    /* Initialise bss section to zero */
    ldr    r2, =_edata
    ldr    r3, =_end
    mov    r4, #0
1:
    cmp    r3, r2
    strhi  r4, [r2], #4
    bhi    1b

    /* Load stack munge value */
    ldr    r4, =0xdeadbeef

#if NUM_CORES > 1
    /* Set up idle stack and munge it with 0xdeadbeef */
    ldr    r2, =cpu_idlestackbegin
    ldr    r3, =cpu_idlestackend
1:
    cmp    r3, r2
    strhi  r4, [r2], #4
    bhi    1b
#endif

    /* Set up stack for IRQ mode */
    msr    cpsr_c, #0x92 /* IRQ disabled, FIQ enabled */
    ldr    sp, =irq_stack
    /* Set up stack for FIQ mode */
    msr    cpsr_c, #0xd1 /* IRQ/FIQ disabled */
    ldr    sp, =fiq_stack

    /* Let svc, abort and undefined modes use irq stack */
    msr    cpsr_c, #0xd3 /* IRQ/FIQ disabled */
    ldr    sp, =irq_stack
    msr    cpsr_c, #0xd7 /* IRQ/FIQ disabled */
    ldr    sp, =irq_stack
    msr    cpsr_c, #0xdb /* IRQ/FIQ disabled */
    ldr    sp, =irq_stack

    /* Switch to sys mode */
    msr    cpsr_c, #0xdf

    /* Set up some stack and munge it with 0xdeadbeef */
    ldr    r2, =stackbegin
    ldr    sp, =stackend
1:
    cmp    sp, r2
    strhi  r4, [r2], #4
    bhi    1b

    /* Delay waking the COP until thread initialization is complete unless dual-core
       support is not enabled in which case the cop_main function does not perform
       any kernel or thread initialization. It's just a trivial sleep loop. */
#if NUM_CORES == 1
    ldr    r4, =COP_CTRL
    mov    r3, #WAKE
    str    r3, [r4]
#endif

    ldr    pc, =main
    /* main() should never return */

cop_init:
#if NUM_CORES > 1
    /* Wait for CPU to go to sleep at the end of its kernel init */
    ldr    r4, =CPU_STATUS
1:
    ldr    r3, [r4]
    tst    r3, #CPUSLEEPING
    beq    1b
#endif

    /* Set up stack for IRQ mode */
    msr    cpsr_c, #0x92 /* IRQ disabled, FIQ enabled */
    ldr    sp, =cop_irq_stack
    /* Set up stack for FIQ mode */
    msr    cpsr_c, #0xd1 /* IRQ/FIQ disabled */
    ldr    sp, =cop_fiq_stack

    /* Let svc, abort and undefined modes use irq stack */
    msr    cpsr_c, #0xd3 /* IRQ/FIQ disabled */
    ldr    sp, =cop_irq_stack
    msr    cpsr_c, #0xd7 /* IRQ/FIQ disabled */
    ldr    sp, =cop_irq_stack
    msr    cpsr_c, #0xdb /* IRQ/FIQ disabled */
    ldr    sp, =cop_irq_stack

    /* Switch to sys mode */
    msr    cpsr_c, #0xdf

    /* Set up idle stack for COP and munge it with 0xdeadbeef */
    ldr    sp, =cop_idlestackend
    ldr    r2, =cop_idlestackbegin
    ldr    r4, =0xdeadbeef
2:
    cmp    sp, r2
    strhi  r4, [r2], #4
    bhi    2b

    /* Run cop_main() in apps/main.c */
    ldr    pc, =cop_main

/* Exception handlers. Will be copied to address 0 after memory remapping */
    .section .vectors,"aw"
    ldr    pc, [pc, #24]
    ldr    pc, [pc, #24]
    ldr    pc, [pc, #24]
    ldr    pc, [pc, #24]
    ldr    pc, [pc, #24]
    ldr    pc, [pc, #24]
    ldr    pc, [pc, #24]
    ldr    pc, [pc, #24]

    /* Exception vectors */
    .global vectors
vectors:
    .word  jump_to_0_handler
    .word  undef_instr_handler
    .word  software_int_handler
    .word  prefetch_abort_handler
    .word  data_abort_handler
    .word  reserved_handler
    .word  irq_handler
    .word  fiq_handler

    .text

/* All illegal exceptions call into UIE with exception address as first
   parameter. This is calculated differently depending on which exception
   we're in. Second parameter is exception number, used for a string lookup
   in UIE.
 */
undef_instr_handler:
    sub    r0, lr, #4
    mov    r1, #0
    b      UIE

/* We run sys mode most of the time, and should never see a software
   exception being thrown. Make it illegal and call UIE.
 */
software_int_handler:
reserved_handler:
    sub    r0, lr, #4
    mov    r1, #4
    b      UIE

prefetch_abort_handler:
    sub    r0, lr, #4
    mov    r1, #1
    b      UIE

/* A call through a null pointer lands on vector 0; nothing else enters the
   image there after the remap. r0 = the caller's return address.
 */
jump_to_0_handler:
    mov    r0, lr
    mrs    r2, cpsr
    msr    cpsr_c, #0xdb /* Undefined mode, IRQ/FIQ disabled */
    msr    spsr_cxsf, r2 /* the caller's mode, whose sp and lr UIE reads */
    mov    r1, #5
    b      UIE

#ifndef BOOTLOADER
/* The way into the crash screen from each handler and __div0: on to
   UIE_sys(pc, num, sp, lr) (system-arm-classic.c) in sys mode, interrupts
   off, on a stack of the core's own, sp and lr being the interrupted mode's.
   The 1 KB irq_stack the IRQ, SVC, abort and undefined modes share is too
   small for the unwinder and the screen's printing, and in abort mode an
   abort that safe_read takes would overwrite the unwinder's lr. */
    .type  UIE, %function
    .global UIE
UIE:
    cmp    r1, #3               /* __div0: a call, in the caller's mode (an */
    beq    1f                   /* IRQ handler's too): its own sp and lr */
    mrs    r3, cpsr
    and    r3, r3, #0x1f
    cmp    r3, #0x1f            /* sys mode has no spsr */
    beq    1f
    mrs    r3, spsr
    and    r3, r3, #0x1f
    cmp    r3, #0x10            /* user mode's registers are sys mode's */
    moveq  r3, #0x1f
    orr    r3, r3, #0xc0
    msr    cpsr_c, r3           /* the mode it came from */
1:
    mov    r2, sp
    mov    r3, lr
    msr    cpsr_c, #0xdf        /* sys mode, IRQ and FIQ off */
#if NUM_CORES > 1
    ldr    r12, =PROC_ID
    ldrb   r12, [r12]
    cmp    r12, #0x55
    ldreq  sp, =uie_stack
    ldrne  sp, =cop_uie_stack
#else
    ldr    sp, =uie_stack
#endif
    b      UIE_sys
    .size  UIE, . - UIE

/* uie_stack_call(pc, sp, fmt, ap, fn): fn(pc, sp, fmt, ap) on that stack, in
   the mode it is called in, never to return: the rest of panicf (panic.c) */
    .type  uie_stack_call, %function
    .global uie_stack_call
uie_stack_call:
    ldr    r12, [sp]            /* fn, the fifth argument */
#if NUM_CORES > 1
    ldr    lr, =PROC_ID         /* lr is free: no return */
    ldrb   lr, [lr]
    cmp    lr, #0x55
    ldreq  sp, =uie_stack
    ldrne  sp, =cop_uie_stack
#else
    ldr    sp, =uie_stack
#endif
    bx     r12
    .size  uie_stack_call, . - uie_stack_call
    .ltorg
#endif /* !BOOTLOADER */

#if defined(HAVE_BOOTDATA) && !defined(BOOTLOADER)
/* boot data structure */
put_boot_data_here
#endif

/* Align stacks to cache line boundary */
    .balign 32

/* 256 words of IRQ stack. Global for the hang catcher's record (panic.c),
   which takes the interrupted pc from the top of the tick's frame. */
    .space 256*4
    .global irq_stack
irq_stack:

/* 256 words of COP IRQ stack */
    .space 256*4
cop_irq_stack:

/* 256 words of FIQ stack */
    .space 256*4
fiq_stack:

/* We'll need this soon - just reserve the symbol */
#if 0
/* 256 words of COP FIQ stack */
    .space 256*4
#endif
cop_fiq_stack:

#ifndef BOOTLOADER
/* the crash stacks, UIE's and panicf's: 2 KB a core, the unwinder and the
   screen's printing taking about 1.6 KB at most. Their own section, which
   app-pp.lds puts directly below the main thread's stack, in whole cache
   lines, the COP's half nearest it. */
    .section .uie_stacks,"aw",%nobits
    .balign 16
    .global uie_stacks
uie_stacks:
    .space 512*4
uie_stack:
#if NUM_CORES > 1
    .space 512*4
cop_uie_stack:
#endif
    .global uie_stacks_end
uie_stacks_end:
#endif
