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

/* utf8_normalize(): NFC's composing pass alone, in place. Every string must
 * come out byte for byte as utf8proc_reencode(COMPOSE | STABLE) left it: the
 * database holds such strings. tools/nfctest/run.sh checks that and makes
 * nfc_table.h. Follows utf8proc_normalize_utf32() (lib/utf8proc, MIT: see
 * lib/utf8proc/LICENSE.md). */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "rbunicode.h"
#include "nfc_table.h"

#define HANGUL_L 0x1100 /* 19 initials */
#define HANGUL_V 0x1161 /* 21 vowels */
#define HANGUL_T 0x11a7 /* 27 finals follow it */
#define HANGUL_S 0xac00 /* 19 * 21 * 28 syllables */

/* The length of the sequence at s and its code point, or 0 where
   utf8proc_iterate() finds invalid UTF-8. Stops at the terminator. */
static int decode(const unsigned char *s, uint32_t *c)
{
    uint32_t lead = s[0];

    if (lead < 0x80) {
        *c = lead;
        return 1;
    }
    if (lead < 0xc2 || lead > 0xf4 || (s[1] & 0xc0) != 0x80)
        return 0;
    if (lead < 0xe0) {
        *c = (lead & 0x1f) << 6 | (s[1] & 0x3f);
        return 2;
    }
    if ((s[2] & 0xc0) != 0x80)
        return 0;
    if (lead < 0xf0) {
        *c = (lead & 0x0f) << 12 | (s[1] & 0x3f) << 6 | (s[2] & 0x3f);
        return *c < 0x800 || (*c >= 0xd800 && *c < 0xe000) ? 0 : 3;
    }
    if ((s[3] & 0xc0) != 0x80)
        return 0;
    *c = (lead & 0x07) << 18 | (s[1] & 0x3f) << 12 | (s[2] & 0x3f) << 6 |
         (s[3] & 0x3f);
    return *c < 0x10000 || *c > 0x10ffff ? 0 : 4;
}

static int encode(unsigned char *s, uint32_t c)
{
    if (c < 0x80) {
        s[0] = c;
        return 1;
    }
    if (c < 0x800) {
        s[0] = 0xc0 | c >> 6;
        s[1] = 0x80 | (c & 0x3f);
        return 2;
    }
    if (c < 0x10000) {
        s[0] = 0xe0 | c >> 12;
        s[1] = 0x80 | (c >> 6 & 0x3f);
        s[2] = 0x80 | (c & 0x3f);
        return 3;
    }
    s[0] = 0xf0 | c >> 18;
    s[1] = 0x80 | (c >> 12 & 0x3f);
    s[2] = 0x80 | (c >> 6 & 0x3f);
    s[3] = 0x80 | (c & 0x3f);
    return 4;
}

/* May c be a mark or the second half of a pair? */
static bool marked(uint32_t c)
{
    return c < 0x20000 && (nfc_pages[c >> 11] >> (c >> 8 & 7) & 1);
}

static int combining_class(uint32_t c)
{
    int lo = 0, hi = NFC_RUNS;
    uint32_t run;

    if (!marked(c))
        return 0;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (nfc_runs[mid] >> 15 <= c)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return 0;
    run = nfc_runs[lo - 1];
    return c - (run >> 15) <= (run >> 8 & 0x7f) ? run & 0xff : 0;
}

/* What first and second compose to, or 0 */
static uint32_t compose(uint32_t first, uint32_t second)
{
    if (first - HANGUL_L < 19 && second - HANGUL_V < 21)
        return HANGUL_S + ((first - HANGUL_L) * 21 + second - HANGUL_V) * 28;
    if (first - HANGUL_S < 19 * 21 * 28 && (first - HANGUL_S) % 28 == 0 &&
        second - HANGUL_T - 1 < 27)
        return first + second - HANGUL_T;
    if (!marked(second))
        return 0;

    if ((first | second) < 0x10000) {
        uint32_t key = first << 16 | second;
        int lo = 0, hi = NFC_PAIRS;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            uint32_t at = (uint32_t)nfc_pairs[mid][0] << 16 | nfc_pairs[mid][1];
            if (at == key)
                return nfc_pairs[mid][2];
            if (at < key)
                lo = mid + 1;
            else
                hi = mid;
        }
    } else {
        for (int i = 0; i < NFC_WIDE_PAIRS; i++)
            if (nfc_wide_pairs[i][0] == first && nfc_wide_pairs[i][1] == second)
                return nfc_wide_pairs[i][2];
    }
    return 0;
}

void utf8_normalize(char *string)
{
    unsigned char *r, *w, *starter = NULL;
    uint32_t c, starter_c = 0;
    int n, starter_n = 0, max_class = -1;
    bool marks = false;

    if (!string)
        return;

    /* Invalid UTF-8 stays as it is, and so does a string without marks:
       some are constants */
    for (r = (unsigned char *)string; *r; r += n) {
        n = decode(r, &c);
        if (!n)
            return;
        marks |= marked(c);
    }
    if (!marks)
        return;

    /* utf8proc_normalize_utf32()'s loop. A composition is never longer than
       its halves (gen_table.c checks), so w never passes r. */
    for (r = w = (unsigned char *)string; *r; r += n) {
        int class;
        uint32_t both;

        n = decode(r, &c);
        class = combining_class(c);
        if (starter && class > max_class && (both = compose(starter_c, c))) {
            unsigned char *tail = starter + starter_n;
            unsigned char utf8[4];
            int len = encode(utf8, both);

            memmove(starter + len, tail, w - tail);
            memcpy(starter, utf8, len);
            w += len - starter_n;
            starter_c = both;
            starter_n = len;
            continue;
        }
        if (w != r)
            memmove(w, r, n);
        if (class) {
            if (class > max_class)
                max_class = class;
        } else {
            starter = w;
            starter_c = c;
            starter_n = n;
            max_class = -1;
        }
        w += n;
    }
    if (w != r)
        *w = '\0';
}
