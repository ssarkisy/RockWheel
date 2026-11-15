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

#include "config.h"

/* Inverse quantisation of one window group, for ARMv4 and up: the loops of
 * quant_to_spec() over the group's scalefactor bands and windows.
 *
 * int faad_iq_group_arm(struct faad_iq_group *job);
 *
 * job (specrec.c):  0 q: the group's first quantised value, advanced past
 *                     what it takes (int16_t, 4-byte aligned)
 *                   4 out: the group's first output (real_t)
 *                   8 swb_offset, 12 the group's scale factors (int16_t),
 *                  16 num_swb, 20 windows in the group, 24 win_inc (values),
 *                  28 exp_base: the exponent of a scale factor of 0, after
 *                     the IMDCT pre-scaling
 *                  32 tab: iq_table, 36 pow2_table
 *
 * For each band: width = swb_offset[sfb+1] - swb_offset[sfb] as uint16_t;
 * a scale factor outside 0..255 counts as 0; exp = exp_base + sf/4,
 * scf = pow2_table[sf & 3]; for each window, (width + 3) & ~3 values from q
 * go to out + j + win * win_inc, j the sum of the bands' widths before it:
 *
 *     out = MUL_C(iquant(q) << exp, scf)      (exp >= 0)
 *     out = MUL_C(iquant(q) >> -exp, scf)     (exp < 0)
 *
 * with iquant() for BIG_IQ_TABLE: tab[q], -tab[-q], or 0 when |q| >= 8192.
 * Returns 17 if any |q| was 8192 or more, else 0.  The shifts are ARM
 * register shifts as the compiled C does them, by 32 or more included.
 * A zero q gives 0, which MUL_C(0, scf) is, so four zeros in a row are
 * stored without the rest.
 */

    .syntax unified

/* As common.h's ICODE_ATTR_FAAD_LARGE_IRAM */
#if defined(USE_IRAM) && ((CONFIG_CPU == PP5022) || (CONFIG_CPU == PP5024) || \
    (CONFIG_CPU == S5L8702) || (CONFIG_CPU == S5L8700) || (CONFIG_CPU == S5L8701))
    .section .icode,"ax",%progbits
#else
    .section .text.faad_iq_group_arm,"ax",%progbits
#endif
    .align  2
    .global faad_iq_group_arm
    .type   faad_iq_group_arm, %function

@ registers in a window: r0 q, r1 out, r2 values left, r3 the shift,
@ r4 r5 four values, r6 r7 zero, r8 r9 the product, r10 scf, r11 tab,
@ r12 the value, lr scratch
@ stack: the error, job, sfb, j, n, windows left, out of the next window
.set ERR, 0
.set JOB, 4
.set SFB, 8
.set JJ, 12
.set NN, 16
.set WIN, 20
.set OUT, 24
.set FRAME, 28

@ the value in the low half of \src (\op lsl or asr); its sign first, so
@ that one compare does for both the range and the sign
.macro iq_lo src, op
    movs    r12, \src, lsl #16      @ q << 16
    beq     2f                      @ 0 stays 0
    bmi     1f
    cmp     r12, #0x20000000        @ q >= 8192
    bhs     3f
    ldr     lr, [r11, r12, lsr #14]
    mov     lr, lr, \op r3
    smull   r8, r9, lr, r10         @ MUL_C: (x * scf + 2^27) >> 28
    movs    r8, r8, lsr #28
    adc     r12, r8, r9, lsl #4
    b       2f
3:  mov     r12, #17
    str     r12, [sp, #ERR]
    mov     r12, #0
    b       2f
1:  rsb     r12, r12, #0            @ -q << 16
    cmp     r12, #0x20000000
    bhs     3b
    ldr     lr, [r11, r12, lsr #14]
    rsb     lr, lr, #0
    mov     lr, lr, \op r3
    smull   r8, r9, lr, r10
    movs    r8, r8, lsr #28
    adc     r12, r8, r9, lsl #4
2:  str     r12, [r1], #4
.endm

@ the value in the high half of \src
.macro iq_hi src, op
    movs    r12, \src, asr #16
    beq     2f
    bmi     1f
    cmp     r12, #8192
    bhs     3f
    ldr     lr, [r11, r12, lsl #2]
    mov     lr, lr, \op r3
    smull   r8, r9, lr, r10
    movs    r8, r8, lsr #28
    adc     r12, r8, r9, lsl #4
    b       2f
3:  mov     r12, #17
    str     r12, [sp, #ERR]
    mov     r12, #0
    b       2f
1:  rsb     r12, r12, #0
    cmp     r12, #8192
    bhs     3b
    ldr     lr, [r11, r12, lsl #2]
    rsb     lr, lr, #0
    mov     lr, lr, \op r3
    smull   r8, r9, lr, r10
    movs    r8, r8, lsr #28
    adc     r12, r8, r9, lsl #4
2:  str     r12, [r1], #4
.endm

@ every window of a band: r1 the first window's out, r2 = n values each,
@ r12 windows (> 0), r3 the shift amount (\op lsl or asr)
.macro iq_band op
    str     r12, [sp, #WIN]
6:  str     r1, [sp, #OUT]
4:  ldmia   r0!, {r4, r5}
    orrs    r12, r4, r5
    bne     5f
    stmia   r1!, {r6, r7}
    stmia   r1!, {r6, r7}
    subs    r2, r2, #4
    bne     4b
    b       7f
5:  iq_lo   r4, \op
    iq_hi   r4, \op
    iq_lo   r5, \op
    iq_hi   r5, \op
    subs    r2, r2, #4
    bne     4b
7:  ldr     r12, [sp, #WIN]
    subs    r12, r12, #1
    beq     .Lband_done
    str     r12, [sp, #WIN]
    ldr     r1, [sp, #OUT]
    ldr     lr, [sp, #JOB]
    ldr     lr, [lr, #24]           @ win_inc
    add     r1, r1, lr, lsl #2
    ldr     r2, [sp, #NN]
    b       6b
.endm

faad_iq_group_arm:
    stmfd   sp!, {r4-r11, lr}
    sub     sp, sp, #FRAME
    mov     r6, #0
    mov     r7, #0
    str     r6, [sp, #ERR]
    str     r0, [sp, #JOB]
    str     r6, [sp, #SFB]
    str     r6, [sp, #JJ]
    ldr     r11, [r0, #32]          @ tab
    ldr     r0, [r0, #0]            @ q

.Lband:
    ldr     lr, [sp, #JOB]
    ldr     r12, [sp, #SFB]
    ldr     r1, [lr, #16]           @ num_swb
    cmp     r12, r1
    bhs     .Lgroup_done
    ldr     r1, [lr, #8]            @ swb_offset
    add     r1, r1, r12, lsl #1
    ldrh    r2, [r1]
    ldrh    r1, [r1, #2]
    sub     r2, r1, r2
    mov     r2, r2, lsl #16
    mov     r9, r2, lsr #16         @ width
    ldr     r1, [lr, #12]           @ scale factors
    add     r1, r1, r12, lsl #1
    ldrsh   r1, [r1]
    cmp     r1, #255
    movhi   r1, #0                  @ outside 0..255: 0
    ldr     r3, [lr, #28]
    add     r3, r3, r1, asr #2      @ exp
    and     r1, r1, #3
    ldr     r10, [lr, #36]
    ldr     r10, [r10, r1, lsl #2]  @ scf
    add     r2, r9, #3
    bic     r2, r2, #3              @ n
    str     r2, [sp, #NN]
    ldr     r1, [lr, #4]
    ldr     r12, [sp, #JJ]
    add     r1, r1, r12, lsl #2     @ out + j
    add     r12, r12, r9
    str     r12, [sp, #JJ]          @ j of the next band
    ldr     r12, [lr, #20]          @ windows
    cmp     r2, #0
    cmpne   r12, #0
    beq     .Lband_done
    cmp     r3, #0
    blt     .Lright
    iq_band lsl
.Lright:
    rsb     r3, r3, #0
    iq_band asr

.Lband_done:
    ldr     r12, [sp, #SFB]
    add     r12, r12, #1
    str     r12, [sp, #SFB]
    b       .Lband

.Lgroup_done:
    ldr     lr, [sp, #JOB]
    str     r0, [lr, #0]            @ q, past what was taken
    ldr     r0, [sp, #ERR]
    add     sp, sp, #FRAME
    ldmpc   regs=r4-r11

    .size   faad_iq_group_arm, .-faad_iq_group_arm

/* M/S stereo for one band, for ARMv4 and up: ms_decode()'s inner loop.
 *
 * void faad_ms_band_arm(real_t *l, real_t *r, unsigned n);
 *
 * For n values: l' = l + r, r' = l' - (r << 1), with 32-bit wrap-around
 * as in the C.
 */
#if defined(USE_IRAM) && ((CONFIG_CPU == PP5022) || (CONFIG_CPU == PP5024) || \
    (CONFIG_CPU == S5L8702) || (CONFIG_CPU == S5L8700) || (CONFIG_CPU == S5L8701))
    .section .icode,"ax",%progbits
#else
    .section .text.faad_ms_band_arm,"ax",%progbits
#endif
    .align  2
    .global faad_ms_band_arm
    .type   faad_ms_band_arm, %function

faad_ms_band_arm:
    stmfd   sp!, {r4-r10, lr}
    subs    r2, r2, #4
    blt     2f
1:  ldmia   r0, {r3-r6}
    ldmia   r1, {r7-r10}
    add     r3, r3, r7
    sub     r7, r3, r7, lsl #1
    add     r4, r4, r8
    sub     r8, r4, r8, lsl #1
    add     r5, r5, r9
    sub     r9, r5, r9, lsl #1
    add     r6, r6, r10
    sub     r10, r6, r10, lsl #1
    stmia   r0!, {r3-r6}
    stmia   r1!, {r7-r10}
    subs    r2, r2, #4
    bge     1b
2:  adds    r2, r2, #4
    beq     4f
3:  ldr     r3, [r0]
    ldr     r7, [r1]
    add     r3, r3, r7
    sub     r7, r3, r7, lsl #1
    str     r3, [r0], #4
    str     r7, [r1], #4
    subs    r2, r2, #1
    bne     3b
4:  ldmpc   regs=r4-r10

    .size   faad_ms_band_arm, .-faad_ms_band_arm
