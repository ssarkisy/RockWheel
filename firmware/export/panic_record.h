/***************************************************************************
 *              __________                   __    __________
 *   Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
 *   Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
 *   Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
 *   Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
 *                                               \/        \/               \/
 *
 * Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
 * Not affiliated with or endorsed by Apple Inc.
 *
 * Copyright (C) 2026 prokopto-dev
 *
 * Laid out after ProkBox5g 0.8.1's firmware/export/panic_record.h
 * (https://github.com/prokbox/prokbox5g, d2753d03d2). Changed for RockWheel:
 * its own record, kept in three copies voted bit by bit and sealed by a
 * CRC-32, in 1 KB instead of 1536 bytes, no log tail, no hang watcher's
 * test, and a scrub for file paths.
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
#ifndef _PANIC_RECORD_H_
#define _PANIC_RECORD_H_

/* What panicf() or an ARM exception last reported, kept in RAM that a reset
 * leaves alone, for the next boot to append to .rockbox/crash.log. The
 * functions here are pure (no locks, no yield, no I/O, no library calls but
 * crc_32()) and host-tested; where the record lives, and who writes it, is
 * not here. Whoever holds a record in RAM writes it through its uncached
 * alias only, so no dirty cache line is lost at a reset or written over it
 * later.
 *
 * Menu+Select flips a few bits of the RAM it keeps (the survive test of
 * 2026-11-15: 2 to 7 of 4096), so the record is kept in PANIC_REC_COPIES
 * copies, read back by a vote on each bit. */

#include <stdint.h>
#include <stdbool.h>

/* A new layout takes a new magic: a later build reads what an earlier one
 * wrote. The two differ in three bytes, so that no change to one byte turns
 * one into the other. */
#define PANIC_REC_MAGIC   0x31505752u   /* "RWP1": valid, not yet logged */
#define PANIC_REC_USED    0x31707772u   /* "rwp1": logged by a later boot */
#define PANIC_REC_RESERVE 1024          /* bytes taken off the audio buffer */
#define PANIC_REC_COPIES  3
#define PANIC_REC_TRACE   10            /* return addresses, newest first */
#define PANIC_REC_THREAD  16
#define PANIC_REC_VERSION 24
#define PANIC_REC_MSG     120           /* message, NUL included */

/* kind: what ended the boot. 1 + the number UIE() shows for an exception */
enum panic_rec_kind
{
    PANIC_KIND_PANICF = 0,
    PANIC_KIND_UNDEF_INSTR,             /* UIE 0 */
    PANIC_KIND_PREFETCH_ABORT,          /* UIE 1 */
    PANIC_KIND_DATA_ABORT,              /* UIE 2 */
    PANIC_KIND_DIV_ZERO,                /* UIE 3 */
    PANIC_KIND_SWI,                     /* UIE 4 */
    PANIC_KIND_JUMP_TO_0,               /* UIE 5 */
    PANIC_KIND_HANG = 0x20,             /* the hang catcher: clear of UIE's */
};

struct panic_record
{
    uint32_t magic;
    uint32_t sum;                       /* CRC-32 of every other byte, the
                                           magic counted as PANIC_REC_MAGIC */
    uint32_t kind;                      /* enum panic_rec_kind */
    uint32_t count;                     /* records since the last logged */
    uint32_t core;                      /* CPU 0, COP 1 */
    uint32_t pc;                        /* where it faulted or hung; for
                                           panicf and a jump to 0, the
                                           caller's return address */
    uint32_t lr;                        /* of the mode the exception came
                                           from (IRQ's for a fault in a
                                           handler); 0 for panicf */
    uint32_t sp;
    uint32_t regs[8];                   /* r0-r7 at a data abort, else 0 */
    uint32_t uptime;                    /* current_tick */
    uint32_t wall;                      /* seconds since 1970, RTC at boot +
                                           uptime; 0: unknown */
    uint32_t trace[PANIC_REC_TRACE];    /* the unwinder's, 0 past the last */
    char thread[PANIC_REC_THREAD];
    char version[PANIC_REC_VERSION];    /* RBVERSION: which rockbox.map */
    char msg[PANIC_REC_MSG];
};

_Static_assert(sizeof(struct panic_record) == 272,
               "a new layout takes a new PANIC_REC_MAGIC");
_Static_assert(PANIC_REC_COPIES * sizeof(struct panic_record)
               <= PANIC_REC_RESERVE, "the copies are larger than the reserve");

/* Fill r afresh and seal it. count goes on from a valid record not yet
 * logged, else starts at 1. The registers, uptime, wall and trace are
 * cleared: to fill them, clear magic, set them, seal again. Strings may be
 * NULL; each is cut to its field and NUL-padded. */
void panic_record_write(struct panic_record *r, uint32_t kind, uint32_t core,
                        uint32_t pc, uint32_t lr, uint32_t sp,
                        const char *thread, const char *version,
                        const char *msg);

/* Mark r valid again after its fields were changed. */
void panic_record_seal(struct panic_record *r);

/* r is a valid record not yet logged */
bool panic_record_valid(const struct panic_record *r);

/* A second panic in the same boot: count one more on the first's record,
 * which keeps its contents. false: r was not valid, nothing changed. */
bool panic_record_bump(struct panic_record *r);

/* A valid record not yet logged: mark it logged and return it (its fields
 * stay readable, peek still accepts it), else NULL. Mark it before writing
 * it out, so that a panic while writing never repeats at every boot. */
const struct panic_record *panic_record_take(struct panic_record *r);

/* The last record whole, logged or not, or NULL when it was never sealed or
 * has been torn since. Never changes it. */
const struct panic_record *panic_record_peek(const struct panic_record *r);

/* The copies: an area holds PANIC_REC_COPIES records one after another. */

/* Each bit of copy 0 made the one most copies hold. Used in place from a
 * panic, where no reset came between: copy 0 is then the record to go on
 * from. */
void panic_record_vote(struct panic_record *area);

/* Copy 0 into the others. */
void panic_record_replicate(struct panic_record *area);

/* The record the copies hold, voted into out, or NULL when none is whole
 * (peek's test). A vote torn by a bit flipped alike in two copies is mended
 * by taking one or two of the bits the copies disagree on from the third,
 * when the CRC then holds: it detects any four bits changed in a record this
 * long, so a mended record is the one sealed. *mended: the bits so taken, 0
 * for a whole vote. Never changes area. */
const struct panic_record *panic_record_recover(struct panic_record *out,
                                                const struct panic_record *area,
                                                int *mended);

/* Copy src into dst (size bytes, NUL included) with each file path made
 * "<path>", for text that leaves the iPod. A path is a '/' followed by
 * anything but a space, at the start or after a space, tab, '(', '[', '=',
 * ':', ',' or a quote. After a quote it runs to the same quote; otherwise to
 * the end, since names may hold spaces. */
void panic_record_scrub(char *dst, unsigned int size, const char *src);

#endif /* _PANIC_RECORD_H_ */
