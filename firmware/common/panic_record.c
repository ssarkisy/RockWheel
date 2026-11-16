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
 * From ProkBox5g 0.8.1's firmware/common/panic_record.c
 * (https://github.com/prokbox/prokbox5g, d2753d03d2): write, seal, take and
 * peek. Changed for RockWheel: its own record (panic_record.h) sealed by a
 * CRC-32 in its second word instead of their sum, kept in copies voted bit
 * by bit, panic_record_bump() and panic_record_scrub() added, the hang
 * watcher's test left out.
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
#include <stddef.h>
#include "crc32.h"
#include "panic_record.h"

/* Runs with interrupts off from a panic, so no library calls but crc_32():
 * plain loops, and every store goes straight to the (uncached) record. */

#define REC_WORDS (sizeof(struct panic_record) / 4)

/* the CRC as sealed: the magic counted as PANIC_REC_MAGIC whatever it says
 * now, the sum word itself left out */
static uint32_t rec_sum(const struct panic_record *r)
{
    const uint32_t magic = PANIC_REC_MAGIC;
    uint32_t c = crc_32(&magic, 4, 0xffffffff);

    return crc_32((const char *)r + 8, sizeof(*r) - 8, c);
}

static bool rec_valid(const struct panic_record *r)
{
    return r->magic == PANIC_REC_MAGIC && r->sum == rec_sum(r);
}

static void __attribute__((noinline)) copy_str(char *dst, const char *src,
                                               unsigned int size)
{
    unsigned int i = 0;

    if (src)
        for (; i < size - 1 && src[i]; i++)
            dst[i] = src[i];
    for (; i < size; i++)           /* pad: the CRC covers every byte */
        dst[i] = '\0';
}

void panic_record_write(struct panic_record *r, uint32_t kind, uint32_t core,
                        uint32_t pc, uint32_t lr, uint32_t sp,
                        const char *thread, const char *version,
                        const char *msg)
{
    uint32_t count = rec_valid(r) ? r->count + 1 : 1;

    r->magic = 0;                   /* invalid until sealed */
    r->kind = kind;
    r->count = count;
    r->core = core;
    r->pc = pc;
    r->lr = lr;
    r->sp = sp;
    for (unsigned int i = 0; i < 8; i++)
        r->regs[i] = 0;
    r->uptime = 0;
    r->wall = 0;
    for (unsigned int i = 0; i < PANIC_REC_TRACE; i++)
        r->trace[i] = 0;
    copy_str(r->thread, thread, PANIC_REC_THREAD);
    copy_str(r->version, version, PANIC_REC_VERSION);
    copy_str(r->msg, msg, PANIC_REC_MSG);
    panic_record_seal(r);
}

void panic_record_seal(struct panic_record *r)
{
    r->magic = PANIC_REC_MAGIC;
    r->sum = rec_sum(r);
}

bool panic_record_valid(const struct panic_record *r)
{
    return rec_valid(r);
}

bool panic_record_bump(struct panic_record *r)
{
    if (!rec_valid(r))
        return false;
    r->magic = 0;
    r->count++;
    panic_record_seal(r);
    return true;
}

const struct panic_record *panic_record_peek(const struct panic_record *r)
{
    if ((r->magic != PANIC_REC_MAGIC && r->magic != PANIC_REC_USED)
        || r->sum != rec_sum(r))
        return NULL;
    return r;
}

const struct panic_record *panic_record_take(struct panic_record *r)
{
    if (!rec_valid(r))
        return NULL;
    r->magic = PANIC_REC_USED;      /* the CRC still matches as sealed */
    return r;
}

static void vote_into(uint32_t *out, const struct panic_record *area)
{
    const uint32_t *a = (const uint32_t *)&area[0];
    const uint32_t *b = (const uint32_t *)&area[1];
    const uint32_t *c = (const uint32_t *)&area[2];

    _Static_assert(PANIC_REC_COPIES == 3, "the vote is of three");
    for (unsigned int i = 0; i < REC_WORDS; i++)
        out[i] = (a[i] & b[i]) | (a[i] & c[i]) | (b[i] & c[i]);
}

void panic_record_vote(struct panic_record *area)
{
    vote_into((uint32_t *)&area[0], area);  /* word i from words i only */
}

void panic_record_replicate(struct panic_record *area)
{
    const uint32_t *src = (const uint32_t *)&area[0];

    for (unsigned int n = 1; n < PANIC_REC_COPIES; n++)
    {
        uint32_t *dst = (uint32_t *)&area[n];
        for (unsigned int i = 0; i < REC_WORDS; i++)
            dst[i] = src[i];
    }
}

/* At most this many bits the copies disagree on are tried, in ones and
 * pairs: more than the survive test's worst (7 bits in 4096, so about 11 in
 * the three copies), and at most about 1,200 CRCs at boot, a tenth of a
 * second. */
#define MEND_SUSPECTS 48

const struct panic_record *panic_record_recover(struct panic_record *out,
                                                const struct panic_record *area,
                                                int *mended)
{
    uint32_t *w = (uint32_t *)out;
    const uint32_t *a = (const uint32_t *)&area[0];
    const uint32_t *b = (const uint32_t *)&area[1];
    const uint32_t *c = (const uint32_t *)&area[2];
    uint16_t suspect[MEND_SUSPECTS];
    unsigned int n = 0;

    *mended = 0;
    vote_into(w, area);
    if (panic_record_peek(out))
        return out;

    /* a copy whole on its own, the others alike in their damage */
    for (unsigned int k = 0; k < PANIC_REC_COPIES; k++)
    {
        if (panic_record_peek(&area[k]))
        {
            const uint32_t *s = (const uint32_t *)&area[k];
            for (unsigned int i = 0; i < REC_WORDS; i++)
            {
                for (uint32_t d = s[i] ^ w[i]; d; d &= d - 1)
                    (*mended)++;
                w[i] = s[i];
            }
            return out;
        }
    }

    for (unsigned int i = 0; i < REC_WORDS; i++)
    {
        const uint32_t d = (a[i] ^ b[i]) | (a[i] ^ c[i]);
        for (unsigned int bit = 0; d && bit < 32; bit++)
        {
            if (!(d & (1u << bit)))
                continue;
            if (n == MEND_SUSPECTS)
                return NULL;        /* too torn to tell */
            suspect[n++] = i * 32 + bit;
        }
    }

    /* the vote took each suspect bit from two copies: the third's is the
       other value, so taking it is flipping the voted bit */
#define FLIP(s) (w[(s) / 32] ^= 1u << ((s) % 32))
    for (unsigned int i = 0; i < n; i++)
    {
        FLIP(suspect[i]);
        if (panic_record_peek(out))
        {
            *mended = 1;
            return out;
        }
        FLIP(suspect[i]);
    }
    for (unsigned int i = 0; i < n; i++)
    {
        FLIP(suspect[i]);
        for (unsigned int j = i + 1; j < n; j++)
        {
            FLIP(suspect[j]);
            if (panic_record_peek(out))
            {
                *mended = 2;
                return out;
            }
            FLIP(suspect[j]);
        }
        FLIP(suspect[i]);
    }
#undef FLIP
    return NULL;
}

static bool path_lead(char c)
{
    return c == ' ' || c == '\t' || c == '(' || c == '[' || c == '='
        || c == ':' || c == ',' || c == '\'' || c == '"';
}

void panic_record_scrub(char *dst, unsigned int size, const char *src)
{
    unsigned int n = 0;
    char prev = ' ';                /* the start counts as after a space */

    if (!size)
        return;
    while (src && *src && n < size - 1)
    {
        if (*src == '/' && path_lead(prev) && src[1] && src[1] != ' '
            && src[1] != '\t')
        {
            /* after a quote, to the same quote; else to the end */
            char close = (prev == '\'' || prev == '"') ? prev : '\0';

            for (const char *t = "<path>"; *t && n < size - 1; t++)
                dst[n++] = *t;
            while (*src && *src != close)
                src++;
            prev = '>';
            continue;
        }
        prev = *src;
        dst[n++] = *src++;
    }
    dst[n] = '\0';
}
