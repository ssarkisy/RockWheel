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

/* The covers Updating Library prepares, their parts without the disk
   (apps/covers_table.c): the sums of a picture as a computer makes them, the
   check of a file, the songs' table and the pictures' one, the fit of a frame
   and the shrinking of a picture */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "covers_table.h"
#include "check.h"

static uint32_t rnd_state = 12345;
static uint32_t rnd(void)
{
    rnd_state = rnd_state * 1103515245u + 12345u;
    return rnd_state >> 1;
}

static void test_sums(void)
{
    /* zlib's own check values */
    CHECK_EQ(covers_crc32(COVERS_CRC32_INIT, "123456789", 9), 0xcbf43926u);
    CHECK_EQ(covers_adler32(COVERS_ADLER32_INIT, "Wikipedia", 9), 0x11e60398u);
    CHECK_EQ(covers_crc32(COVERS_CRC32_INIT, "", 0), 0);
    CHECK_EQ(covers_adler32(COVERS_ADLER32_INIT, "", 0), 1);

    /* a part at a time as whole, past Adler's NMAX and its modulo */
    static unsigned char data[200000];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = 0xff - (rnd() & 7); /* high bytes: the sums grow fastest */
    const uint32_t crc = covers_crc32(COVERS_CRC32_INIT, data, sizeof(data));
    const uint32_t adler = covers_adler32(COVERS_ADLER32_INIT, data,
                                          sizeof(data));
    uint32_t c = COVERS_CRC32_INIT, a = COVERS_ADLER32_INIT;
    for (size_t at = 0; at < sizeof(data); )
    {
        size_t n = 1 + rnd() % 40000;
        if (n > sizeof(data) - at)
            n = sizeof(data) - at;
        c = covers_crc32(c, data + at, n);
        a = covers_adler32(a, data + at, n);
        at += n;
    }
    CHECK_EQ(c, crc);
    CHECK_EQ(a, adler);
    /* Adler-32 by its definition, with no modulo deferred */
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < sizeof(data); i++)
    {
        s1 = (s1 + data[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    CHECK_EQ(adler, s2 << 16 | s1);
    /* a byte changed changes both */
    data[777] ^= 1;
    CHECK(covers_crc32(COVERS_CRC32_INIT, data, sizeof(data)) != crc);
    CHECK(covers_adler32(COVERS_ADLER32_INIT, data, sizeof(data)) != adler);
}

static void test_file_check(void)
{
    const uint32_t base = covers_file_check(4000000, 1234, 56789);
    CHECK(base != 0);
    CHECK_EQ(covers_file_check(4000000, 1234, 56789), base);
    CHECK(covers_file_check(4000001, 1234, 56789) != base);
    CHECK(covers_file_check(4000000, 1235, 56789) != base);
    CHECK(covers_file_check(4000000, 1234, 56788) != base);
    /* the fields aren't interchangeable */
    CHECK(covers_file_check(1234, 4000000, 56789) != base);
    CHECK(covers_file_check(0, 0, 0) != 0);
}

static int cmp_song(const void *x, const void *y)
{
    const struct covers_song_ram *a = x, *b = y;
    return (a->path > b->path) - (a->path < b->path);
}

static void test_song_find(void)
{
    enum { N = 1000 };
    static struct covers_song_ram e[N];
    for (int i = 0; i < N; i++)
    {
        e[i].path = (uint64_t)rnd() << 33 | (uint64_t)rnd() << 2 | 1;
        e[i].word = COVERS_WORD(i + 1, i % 4, 0);
        e[i].check = i;
    }
    qsort(e, N, sizeof(e[0]), cmp_song);
    for (int i = 0; i < N; i++)
    {
        const struct covers_song_ram *f = covers_song_find(e, N, e[i].path);
        CHECK(f == &e[i]);
    }
    /* none between, before or after them, nor in none */
    CHECK(covers_song_find(e, N, e[0].path - 1) == NULL);
    CHECK(covers_song_find(e, N, e[N - 1].path + 1) == NULL);
    CHECK(covers_song_find(e, N, e[10].path + 1) == NULL);
    CHECK(covers_song_find(e, 0, e[0].path) == NULL);
    CHECK(covers_song_find(e, 1, e[0].path) == &e[0]);

    /* the word's parts */
    const uint32_t w = COVERS_WORD(COVERS_SLOT_MASK, 3, 0x81);
    CHECK_EQ(COVERS_SLOT_OF(w), COVERS_SLOT_MASK);
    CHECK_EQ(COVERS_REASON_OF(w), 3);
    CHECK_EQ(COVERS_FLAGS_OF(w), 0x81);
    CHECK_EQ(COVERS_SLOT_OF(COVERS_WORD(26000, 0, 0)), 26000);
    CHECK_EQ(COVERS_REASON_OF(COVERS_WORD(26000, 0, 0)), 0);
}

static void test_pictures(void)
{
    enum { N = 3000 };
    const size_t room = covers_pictures_room(N);
    CHECK(room >= 2 * N);
    CHECK((room & (room - 1)) == 0);
    struct covers_picture *t = calloc(room, sizeof(*t));
    static uint32_t crc[N], adler[N], size[N];
    for (int i = 0; i < N; i++)
    {
        crc[i] = rnd();
        adler[i] = rnd();
        size[i] = 1 + rnd() % 3000000;
        /* some of one sum alike, which only the other tells apart */
        if (i % 10 == 9)
            crc[i] = crc[i - 1];
        if (i % 10 == 8 && i > 0)
            adler[i] = adler[i - 1], size[i] = size[i - 1];
        struct covers_picture *p = covers_picture_place(t, room, crc[i],
                                                        adler[i], size[i]);
        CHECK(p != NULL);
        if (!p)
            break;
        CHECK_EQ(p->size, 0); /* new: an empty place */
        p->crc = crc[i];
        p->adler = adler[i];
        p->size = size[i];
        p->slot = i + 1;
    }
    /* each found again in its slot: a song with the same picture shares it */
    for (int i = 0; i < N; i++)
    {
        struct covers_picture *p = covers_picture_place(t, room, crc[i],
                                                        adler[i], size[i]);
        CHECK(p && p->slot == (uint32_t)i + 1);
    }
    /* the same sums of another size are another picture */
    struct covers_picture *q = covers_picture_place(t, room, crc[5], adler[5],
                                                    size[5] + 1);
    CHECK(q && q->size == 0);
    free(t);

    /* full, it says so rather than looping */
    struct covers_picture small[16];
    memset(small, 0, sizeof(small));
    for (int i = 0; i < 16; i++)
    {
        struct covers_picture *p = covers_picture_place(small, 16, i, i, 1);
        CHECK(p && !p->size);
        if (p)
            p->size = 1, p->crc = p->adler = i;
    }
    CHECK(covers_picture_place(small, 16, 99, 99, 1) == NULL);
    CHECK(covers_picture_place(small, 16, 7, 7, 1) != NULL);
}

static void test_fit(void)
{
    int w = 240, h = 240;
    covers_fit(200, 166, &w, &h);
    CHECK_EQ(w, 240);
    CHECK_EQ(h, 199);
    w = h = 240;
    covers_fit(166, 200, &w, &h);
    CHECK_EQ(w, 199);
    CHECK_EQ(h, 240);
    w = h = 200;
    covers_fit(240, 240, &w, &h);
    CHECK_EQ(w, 200);
    CHECK_EQ(h, 200);
    w = h = 200;
    covers_fit(240, 199, &w, &h);
    CHECK_EQ(w, 200);
    CHECK_EQ(h, 166);
    /* a frame of nothing leaves the box */
    w = h = 200;
    covers_fit(0, 10, &w, &h);
    CHECK_EQ(w, 200);
    CHECK_EQ(h, 200);
}

static fb_data rgb(unsigned r, unsigned g, unsigned b)
{
    return (fb_data)(r << 11 | g << 5 | b);
}

static void test_area_scale(void)
{
    static fb_data src[480 * 480], dst[240 * 240];
    static uint32_t acc[6 * 240];

    /* one colour stays itself at any size */
    const fb_data c = rgb(17, 41, 9);
    for (int i = 0; i < 240 * 240; i++)
        src[i] = c;
    covers_area_scale(src, 240, 240, dst, 200, 200, acc);
    int same = 1;
    for (int i = 0; i < 200 * 200; i++)
        same &= dst[i] == c;
    CHECK(same);
    covers_area_scale(src, 240, 199, dst, 200, 166, acc);
    same = 1;
    for (int i = 0; i < 200 * 166; i++)
        same &= dst[i] == c;
    CHECK(same);
    /* white stays white: no rounding over the top */
    for (int i = 0; i < 240 * 240; i++)
        src[i] = 0xffff;
    covers_area_scale(src, 240, 240, dst, 137, 101, acc);
    same = 1;
    for (int i = 0; i < 137 * 101; i++)
        same &= dst[i] == 0xffff;
    CHECK(same);

    /* by two: each the mean of four */
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            src[y * 4 + x] = rgb((x + y) * 4, x * 8 + y, 31 - y * 4);
    covers_area_scale(src, 4, 4, dst, 2, 2, acc);
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++)
        {
            unsigned r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++)
            {
                const fb_data p = src[(2 * y + k / 2) * 4 + 2 * x + k % 2];
                r += p >> 11;
                g += (p >> 5) & 63;
                b += p & 31;
            }
            const fb_data want = rgb((r + 2) / 4, (g + 2) / 4, (b + 2) / 4);
            CHECK_EQ(dst[y * 2 + x], want);
        }

    /* at its own size, the same picture */
    for (int i = 0; i < 64 * 48; i++)
        src[i] = (fb_data)rnd();
    covers_area_scale(src, 64, 48, dst, 64, 48, acc);
    CHECK(!memcmp(src, dst, 64 * 48 * sizeof(fb_data)));

    /* 240 to 200, of halves black and white: the edge's column between, the
       mean kept */
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 240; x++)
            src[y * 240 + x] = x < 120 ? 0 : 0xffff;
    covers_area_scale(src, 240, 240, dst, 200, 200, acc);
    CHECK_EQ(dst[0], 0);
    CHECK_EQ(dst[99], 0);
    CHECK_EQ(dst[100], 0xffff);
    CHECK_EQ(dst[199 * 200 + 199], 0xffff);
    unsigned long sum = 0;
    for (int i = 0; i < 200 * 200; i++)
        sum += dst[i] & 31;
    CHECK(sum == 31ul * 100 * 200);

    /* sizes it refuses leave dst alone */
    dst[0] = 0x1234;
    covers_area_scale(src, 100, 100, dst, 200, 50, acc);
    covers_area_scale(src, 0, 100, dst, 10, 10, acc);
    CHECK_EQ(dst[0], 0x1234);
}

int main(void)
{
    check_time_limit(30);
    test_sums();
    test_file_check();
    test_song_find();
    test_pictures();
    test_fit();
    test_area_scale();
    return check_done("covers");
}
