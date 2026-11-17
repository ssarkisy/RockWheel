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

/* apps/gui/skin_engine/skin_ratio.h: a bar's slider's place and the skins'
 * percentages, against the expressions they replaced as the iPod worked them
 * out, its unsigned long of 32 bits (uint32_t here): the same wherever those
 * didn't wrap, and right where they did */

#include <stdint.h>
#include <stdio.h>
#include "skin_ratio.h"
#include "check.h"

/* draw_progressbar()'s, xoff = width * end / length: the int made unsigned */
static int old_place(int size, uint32_t end, uint32_t length)
{
    return (int)((uint32_t)size * end / length);
}

/* skin_tokens.c's, 1000UL * elapsed_ms / length and 100 * elapsed / length */
static uint32_t old_part(uint32_t scale, uint32_t part, uint32_t whole)
{
    return scale * part / whole;
}

static uint32_t rng = 0x2545f491u;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* the bars' lengths in the skins, and either side of them */
static const int sizes[] = { 1, 2, 11, 100, 192, 255, 256, 276, 291, 292, 296, 320 };
#define N_SIZES (int)(sizeof(sizes) / sizeof(sizes[0]))

/* each end of a length that the old one worked out without wrapping */
static void below_limit(void)
{
    for (int i = 0; i < N_SIZES; i++)
    {
        const int size = sizes[i];
        const uint32_t limit = UINT32_MAX / (uint32_t)size; /* size * limit fits */
        const uint32_t lengths[] = { 1, 2, 999, 1000, 60000, 180000, 3600000,
                                     limit / 3, limit - 1, limit };
        for (int l = 0; l < 10 + 200; l++)
        {
            const uint32_t length = l < 10 ? lengths[l] : 1 + rnd() % limit;
            if (length == 0)
                continue;
            const uint32_t ends[] = { 0, 1, length / 7, length / 3, length / 2,
                                      length - 1, length };
            for (int e = 0; e < 7 + 50; e++)
            {
                const uint32_t end = e < 7 ? ends[e]
                                     : (uint32_t)(rnd() % ((uint64_t)length + 1));
                CHECK_EQ(skin_bar_place(size, end, length), old_place(size, end, length));
            }
        }
    }
    /* the percentages, with every scale the skins have: 100, a %?px's
       options, 1000 */
    const uint32_t scales[] = { 2, 5, 10, 100, 1000 };
    for (int s = 0; s < 5; s++)
    {
        const uint32_t limit = UINT32_MAX / scales[s];
        for (int k = 0; k < 2000; k++)
        {
            const uint32_t whole = 1 + rnd() % limit;
            const uint32_t part = (uint32_t)(rnd() % ((uint64_t)whole + 1));
            CHECK_EQ(skin_part_of(scales[s], part, whole), old_part(scales[s], part, whole));
        }
        CHECK_EQ(skin_part_of(scales[s], limit, limit), old_part(scales[s], limit, limit));
    }
}

/* past it: the old one wraps, the new one doesn't */
static void past_limit(void)
{
    /* Dieter's bar of 276 px: a track of 5 h at 4 h 30 min, 90% of the way */
    const uint32_t five_h = 5u * 3600 * 1000, at = 45u * 360 * 1000;
    CHECK_EQ(skin_bar_place(276, at, five_h), 248);
    CHECK(old_place(276, at, five_h) != 248);
    printf("bar 276 px, 5 h track at 4 h 30 min: was %d, is %d\n",
           old_place(276, at, five_h), skin_bar_place(276, at, five_h));

    /* along a track of 10 h a second at a time: never back, never past the
       end, within a pixel of the exact place; the old one goes back */
    const uint32_t ten_h = 10u * 3600 * 1000;
    for (int i = 0; i < N_SIZES; i++)
    {
        const int size = sizes[i];
        int prev = 0, old_prev = 0, old_back = 0;
        for (uint32_t end = 0; end <= ten_h; end += 1000)
        {
            const int place = skin_bar_place(size, end, ten_h);
            const double exact = (double)size * end / ten_h;
            CHECK(place >= prev && place <= size);
            CHECK(place <= exact && place > exact - 1);
            prev = place;
            const int old = old_place(size, end, ten_h);
            if (old < old_prev)
                old_back++;
            old_prev = old;
        }
        /* one wraps once size * 10 h is past 2^32: 120 px and up */
        if ((uint64_t)size * ten_h > UINT32_MAX)
            CHECK(old_back > 0);
        else
            CHECK_EQ(old_back, 0);
    }

    /* %pX's estimate: 1000 * a time in ms, 72 min of a 2 h track */
    const uint32_t two_h = 2u * 3600 * 1000, min72 = 72u * 60 * 1000;
    CHECK_EQ(skin_part_of(1000, min72, two_h), 600);
    CHECK(old_part(1000, min72, two_h) != 600);
    printf("%%pX's estimate, 72 min of 2 h, in thousandths: was %u, is %lu\n",
           old_part(1000, min72, two_h), skin_part_of(1000, min72, two_h));

    /* %px: 100 * a time in ms, 12 h of a 13 h track */
    const uint32_t h13 = 13u * 3600 * 1000, h12 = 12u * 3600 * 1000;
    CHECK_EQ(skin_part_of(100, h12, h13), 92);
    CHECK(old_part(100, h12, h13) != 92);
    printf("%%px, 12 h of 13 h: was %u%%, is %lu%%\n",
           old_part(100, h12, h13), skin_part_of(100, h12, h13));
}

/* the ends: past the length the bar's end, and a bar of no length */
static void edges(void)
{
    CHECK_EQ(skin_bar_place(276, 200000, 180000), 276);
    CHECK_EQ(skin_bar_place(276, UINT32_MAX, 1), 276);
    CHECK_EQ(skin_bar_place(276, 0, UINT32_MAX), 0);
    CHECK_EQ(skin_bar_place(276, UINT32_MAX, UINT32_MAX), 276);
    CHECK_EQ(skin_bar_place(0, 5, 10), 0);
    CHECK_EQ(skin_bar_place(-15, 5, 10), 0);
    CHECK_EQ(skin_part_of(1000, UINT32_MAX, UINT32_MAX), 1000);
}

int main(void)
{
    below_limit();
    past_limit();
    edges();
    return check_done("skin_ratio");
}
