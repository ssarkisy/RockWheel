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

/* AAC spectral Huffman decoding for ARMv4 and up: a run of codewords of one
 * codebook, with libfaad's bit reader held in registers.
 *
 * int faad_huff_section_arm(struct faad_huff_job *job, unsigned cb,
 *                           int16_t *sp, int16_t *end);
 *
 * job (huffman.c):  0 bufa, 4 bufb, 8 bits_left, 12 tail: libfaad's bitfile
 *                   16 limit: the last tail at which a codeword may start
 *                   20 tab1: the first-step table, or the binary tree
 *                   24 tab2: the second-step table
 *
 * Decodes codewords of book cb (1..11) into sp until sp reaches end, and
 * returns the number of values written; job's first four words come back as
 * the reader stands after the last codeword.  The values, the bits taken and
 * their order are huffman_spectral_data()'s for each codeword.
 *
 * It stops before a codeword
 *  - when tail is past limit: a codeword takes at most 56 bits, so at most
 *    two more words, and the caller sets limit so that those loads stay
 *    clear of the end of the buffer, where libfaad's reader changes how it
 *    behaves (no_more_reading, error);
 *  - in book 11, before an escape with more than 8 leading ones: no valid
 *    value has one, and huffman_getescape()'s arithmetic for it is left to
 *    the C.
 * The caller decodes what is left one codeword at a time.
 *
 * The bit reader: bufa holds the current word, bits_left (1..32, or 0 as
 * faad_get1bit() may leave it) of its low bits unread, bufb the next word.
 * The next 32 bits are (bufa << (32 - bits_left)) | (bufb >> bits_left),
 * with ARM's register shifts by 32 giving 0.  Taking n bits subtracts n; at
 * zero or below, 32 is added, bufb moves to bufa and the next word is loaded
 * byte-swapped, as faad_flushbits_ex() does.  Where the C would leave
 * bits_left at 0 after a sign bit, this leaves 32 with the next word loaded:
 * the same position, which reads and counts (faad_get_processed_bits,
 * faad_byte_align) the same.
 */

    .syntax unified

/* As common.h's ICODE_ATTR_FAAD_LARGE_IRAM: IRAM only where libfaad puts
   its large code there; the PP5020's 48 KB is full. */
#if defined(USE_IRAM) && ((CONFIG_CPU == PP5022) || (CONFIG_CPU == PP5024) || \
    (CONFIG_CPU == S5L8702) || (CONFIG_CPU == S5L8700) || (CONFIG_CPU == S5L8701))
    .section .icode,"ax",%progbits
#else
    .section .text.faad_huff_section_arm,"ax",%progbits
#endif
    .align  2
    .global faad_huff_section_arm
    .type   faad_huff_section_arm, %function

@ registers
@ r0 bufa, r1 bufb, r2 bits_left, r3 tail, r4 sp, r5 end, r6 limit,
@ r7 tab1, r8 tab2, r9 the 32-bit window, r10-r12 and lr scratch

@ the next word into bufb (r10 scratch)
.macro refill
    add     r2, r2, #32
    mov     r0, r1
    ldr     r1, [r3], #4
    eor     r10, r1, r1, ror #16
    bic     r10, r10, #0x00ff0000
    mov     r1, r1, ror #8
    eor     r1, r1, r10, lsr #8
.endm

@ the next 32 bits into r9
.macro window t=r12
    rsb     \t, r2, #32
    mov     r9, r0, lsl \t
    orr     r9, r9, r1, lsr r2
.endm

@ before a codeword: done, or too near the end of the buffer
.macro next_or_exit
    cmp     r4, r5
    bhs     .Lexit
    cmp     r3, r6
    bhi     .Lexit
.endm

@ take r11 bits, refilling as needed, and go on with the next codeword
.macro take_and_loop top
    subs    r2, r2, r11
    bgt     \top
    refill
    b       \top
.endm

@ the sign bit at window bit r11 for a nonzero value in \v (scratch \t);
@ r11 moves past it
.macro sign v, t
    cmp     \v, #0
    beq     1f
    movs    \t, r9, lsl r11
    add     r11, r11, #1
    rsbmi   \v, \v, #0
1:
.endm

@ the two-step lookup of an n-bit first step: r10 the second-step index
@ (r11, r12 scratch)
.macro two_step n
    mov     r10, r9, lsr #(32 - \n)
    add     r10, r7, r10, lsl #1
    ldrb    r11, [r10, #1]          @ extra bits
    ldrb    r10, [r10]              @ offset
    cmp     r11, #0
    beq     2f
    rsb     r11, r11, #32
    mov     r12, r9, lsl #\n
    add     r10, r10, r12, lsr r11
2:
.endm

faad_huff_section_arm:
    stmfd   sp!, {r0, r2, r4-r11, lr}   @ [sp] job, [sp, #4] the output pointer at entry
    mov     r4, r2
    mov     r5, r3
    mov     r12, r1
    ldmia   r0, {r0, r1, r2, r3, r6, r7, r8}
    cmp     r12, #11
    ldrls   pc, [pc, r12, lsl #2]
    b       .Lexit
    .word   .Lexit                  @ 0: no codewords
    .word   .Lquad2                 @ 1
    .word   .Lquad2                 @ 2
    .word   .Lquadbin_sign          @ 3
    .word   .Lquad2_sign            @ 4
    .word   .Lpairbin               @ 5
    .word   .Lpair2_5               @ 6
    .word   .Lpairbin_sign          @ 7
    .word   .Lpair2_5_sign          @ 8
    .word   .Lpairbin_sign          @ 9
    .word   .Lpair2_6_sign          @ 10
    .word   .Lpair2_esc             @ 11

.Lexit:
    ldr     r12, [sp]
    stmia   r12, {r0, r1, r2, r3}
    ldr     r12, [sp, #4]
    sub     r0, r4, r12
    mov     r0, r0, asr #1
    add     sp, sp, #8
    ldmpc   regs=r4-r11

/* books 1, 2: quadruples, signed values, two-step (5 bits) */
.Lquad2:
    next_or_exit
    window
    two_step 5
    add     r10, r10, r10, lsl #2   @ 5-byte entries: bits, x, y, v, w
    add     r10, r8, r10
    ldrb    r11, [r10]
    ldrsb   r12, [r10, #1]
    strh    r12, [r4], #2
    ldrsb   r12, [r10, #2]
    strh    r12, [r4], #2
    ldrsb   r12, [r10, #3]
    strh    r12, [r4], #2
    ldrsb   r12, [r10, #4]
    strh    r12, [r4], #2
    take_and_loop .Lquad2

/* book 4: quadruples, unsigned values with sign bits, two-step (5 bits) */
.Lquad2_sign:
    next_or_exit
    window
    two_step 5
    add     r10, r10, r10, lsl #2
    add     r10, r8, r10
    ldrb    r11, [r10]
    ldrsb   r12, [r10, #1]
    sign    r12, lr
    strh    r12, [r4], #2
    ldrsb   r12, [r10, #2]
    sign    r12, lr
    strh    r12, [r4], #2
    ldrsb   r12, [r10, #3]
    sign    r12, lr
    strh    r12, [r4], #2
    ldrsb   r12, [r10, #4]
    sign    r12, lr
    strh    r12, [r4], #2
    take_and_loop .Lquad2_sign

/* book 3: quadruples with sign bits, binary tree of 5-byte nodes
   (is_leaf, then the two branch offsets, or the four values of a leaf) */
.Lquadbin_sign:
    next_or_exit
    window
    mov     r10, r7
    mov     r11, #0
    ldrb    r12, [r10]
    cmp     r12, #0
    bne     4f
3:  movs    r12, r9, lsl r11        @ N: the next bit
    add     r11, r11, #1
    ldrsbpl r12, [r10, #1]
    ldrsbmi r12, [r10, #2]
    add     r12, r12, r12, lsl #2
    add     r10, r10, r12
    ldrb    r12, [r10]
    cmp     r12, #0
    beq     3b
4:  ldrsb   r12, [r10, #1]
    sign    r12, lr
    strh    r12, [r4], #2
    ldrsb   r12, [r10, #2]
    sign    r12, lr
    strh    r12, [r4], #2
    ldrsb   r12, [r10, #3]
    sign    r12, lr
    strh    r12, [r4], #2
    ldrsb   r12, [r10, #4]
    sign    r12, lr
    strh    r12, [r4], #2
    take_and_loop .Lquadbin_sign

/* book 5: pairs, signed values, binary tree of 3-byte nodes */
.Lpairbin:
    next_or_exit
    window
    mov     r10, r7
    mov     r11, #0
    ldrb    r12, [r10]
    cmp     r12, #0
    bne     4f
3:  movs    r12, r9, lsl r11
    add     r11, r11, #1
    ldrsbpl r12, [r10, #1]
    ldrsbmi r12, [r10, #2]
    add     r12, r12, r12, lsl #1
    add     r10, r10, r12
    ldrb    r12, [r10]
    cmp     r12, #0
    beq     3b
4:  ldrsb   r12, [r10, #1]
    strh    r12, [r4], #2
    ldrsb   r12, [r10, #2]
    strh    r12, [r4], #2
    take_and_loop .Lpairbin

/* books 7, 9: pairs with sign bits, binary tree of 3-byte nodes */
.Lpairbin_sign:
    next_or_exit
    window
    mov     r10, r7
    mov     r11, #0
    ldrb    r12, [r10]
    cmp     r12, #0
    bne     4f
3:  movs    r12, r9, lsl r11
    add     r11, r11, #1
    ldrsbpl r12, [r10, #1]
    ldrsbmi r12, [r10, #2]
    add     r12, r12, r12, lsl #1
    add     r10, r10, r12
    ldrb    r12, [r10]
    cmp     r12, #0
    beq     3b
4:  ldrsb   r12, [r10, #1]
    sign    r12, lr
    strh    r12, [r4], #2
    ldrsb   r12, [r10, #2]
    sign    r12, lr
    strh    r12, [r4], #2
    take_and_loop .Lpairbin_sign

/* book 6: pairs, signed values, two-step (5 bits), 3-byte entries
   (bits, x, y) */
.Lpair2_5:
    next_or_exit
    window
    two_step 5
    add     r10, r10, r10, lsl #1
    add     r10, r8, r10
    ldrb    r11, [r10]
    ldrsb   r12, [r10, #1]
    strh    r12, [r4], #2
    ldrsb   r12, [r10, #2]
    strh    r12, [r4], #2
    take_and_loop .Lpair2_5

/* books 8 (5 bits) and 10 (6 bits): pairs with sign bits, two-step */
.macro pair2_sign n, top
\top:
    next_or_exit
    window
    two_step \n
    add     r10, r10, r10, lsl #1
    add     r10, r8, r10
    ldrb    r11, [r10]
    ldrsb   r12, [r10, #1]
    sign    r12, lr
    strh    r12, [r4], #2
    ldrsb   r12, [r10, #2]
    sign    r12, lr
    strh    r12, [r4], #2
    take_and_loop \top
.endm
    pair2_sign 5, .Lpair2_5_sign
    pair2_sign 6, .Lpair2_6_sign

@ huffman_getescape() for \v (±16 is an escape, anything else stays):
@ i = 4 plus the count of leading ones, then i bits; |value| = those bits
@ with bit i set.  r9-r11 scratch.
.macro escape v
    cmn     \v, #16
    cmpne   \v, #16
    bne     9f
    window  r11
    mov     r10, #0                 @ leading ones
7:  movs    r9, r9, lsl #1
    bcc     8f
    add     r10, r10, #1
    cmp     r10, #8
    bls     7b
    b       .Lesc_bail
8:  add     r11, r10, #4            @ i
    add     r10, r10, r11
    add     r10, r10, #1            @ bits taken: the ones, the zero, i
    rsb     r11, r11, #32
    mov     r9, r9, lsr r11         @ the i bits
    rsb     r11, r11, #32
    cmp     \v, #0
    mov     \v, #1
    orr     r9, r9, \v, lsl r11
    rsbmi   r9, r9, #0
    mov     \v, r9
    subs    r2, r2, r10
    bgt     9f
    refill
9:
.endm

/* book 11: pairs with sign bits and escapes, two-step (5 bits) */
.Lpair2_esc:
    next_or_exit
    window
    two_step 5
    add     r10, r10, r10, lsl #1
    add     r10, r8, r10
    ldrb    r11, [r10]
    ldrsb   r12, [r10, #1]
    ldrsb   lr, [r10, #2]
    sign    r12, r10
    sign    lr, r10
    cmn     r12, #16
    cmpne   r12, #16
    beq     5f
    cmn     lr, #16
    cmpne   lr, #16
    beq     5f
    strh    r12, [r4], #2
    strh    lr, [r4], #2
    take_and_loop .Lpair2_esc

@ an escape in x or y: from the bits after the codeword and its signs,
@ huffman_getescape() for x, then for y
5:  stmfd   sp!, {r0-r3}            @ the reader at the codeword, for a bail-out
    subs    r2, r2, r11
    bgt     6f
    refill
6:  escape  r12
    escape  lr
    add     sp, sp, #16
    strh    r12, [r4], #2
    strh    lr, [r4], #2
    b       .Lpair2_esc

.Lesc_bail:
    ldmfd   sp!, {r0-r3}
    b       .Lexit


    .size   faad_huff_section_arm, .-faad_huff_section_arm
