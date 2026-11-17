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

static void test_slide_fit(void)
{
    int w, h;
    covers_slide_fit(200, 200, 136, &w, &h);
    CHECK_EQ(w, 136);
    CHECK_EQ(h, 136);
    covers_slide_fit(200, 150, 136, &w, &h);
    CHECK_EQ(w, 136);
    CHECK_EQ(h, 102);
    covers_slide_fit(150, 200, 99, &w, &h);
    CHECK_EQ(w, 74);
    CHECK_EQ(h, 99);
    /* a frame 1 px wide or high: a pixel still, not none */
    covers_slide_fit(1, 200, 99, &w, &h);
    CHECK_EQ(w, 1);
    CHECK_EQ(h, 99);
    covers_slide_fit(200, 1, 99, &w, &h);
    CHECK_EQ(w, 99);
    CHECK_EQ(h, 1);
}

/** The computer's covers: a file made here as the installer makes one **/

#define INBOX_SIDE 240
static unsigned char inbox[1 << 22];

/* a block of kind, w by h of colour c, for picture, at p: its length */
static uint32_t put_block(unsigned char *p, int kind, int w, int h,
                          fb_data c, uint32_t picture)
{
    struct covers_inbox_block b;
    memset(&b, 0, sizeof(b));
    b.magic = COVERS_BLOCK_MAGIC;
    b.kind = kind;
    b.width = w;
    b.height = h;
    b.picture = picture;
    fb_data *px = (fb_data *)(p + sizeof(b));
    for (int i = 0; i < w * h; i++)
        px[i] = c;
    b.crc = covers_crc32(COVERS_CRC32_INIT, px, w * h * sizeof(fb_data));
    memcpy(p, &b, sizeof(b));
    return sizeof(b) + ((w * h * sizeof(fb_data) + 31) & ~31u);
}

/* n pictures, each a large picture and a frame: the file's size */
static uint32_t make_inbox(uint32_t n)
{
    memset(inbox, 0, sizeof(inbox));
    struct covers_inbox_header *h = (struct covers_inbox_header *)inbox;
    struct covers_inbox_entry *e = (struct covers_inbox_entry *)(h + 1);
    uint32_t at = (sizeof(*h) + n * sizeof(*e) + COVERS_INBOX_ALIGN - 1)
                  & ~(COVERS_INBOX_ALIGN - 1);
    for (uint32_t i = 0; i < n; i++)
    {
        /* by size and then quick key: two of each size */
        e[i].size = 1000 + i / 2;
        e[i].quick = 7 * i;
        e[i].crc = 0x1000 + i;
        e[i].adler = 0x2000 + i;
        e[i].offset = at;
        uint32_t len = put_block(inbox + at, COVERS_BLOCK_LARGE, 24, 18,
                                 (fb_data)i, e[i].crc);
        len += put_block(inbox + at + len, COVERS_BLOCK_FRAME, 20, 15,
                         (fb_data)~i, e[i].crc);
        e[i].length = len;
        at = (at + len + COVERS_INBOX_ALIGN - 1) & ~(COVERS_INBOX_ALIGN - 1);
    }
    h->magic = COVERS_INBOX_MAGIC;
    h->version = COVERS_INBOX_VERSION;
    h->header_size = sizeof(*h);
    h->side = INBOX_SIDE;
    h->format = COVERS_INBOX_RGB565;
    h->count = n;
    h->file_size = at;
    h->index_crc = covers_crc32(COVERS_CRC32_INIT, e, n * sizeof(*e));
    h->colour_kind = 1;
    h->header_crc = covers_crc32(COVERS_CRC32_INIT, h,
                                 offsetof(struct covers_inbox_header,
                                          header_crc));
    return at;
}

static void fix_header(void)
{
    struct covers_inbox_header *h = (struct covers_inbox_header *)inbox;
    h->header_crc = covers_crc32(COVERS_CRC32_INIT, h,
                                 offsetof(struct covers_inbox_header,
                                          header_crc));
}

static void test_inbox_header(void)
{
    CHECK_EQ(sizeof(struct covers_inbox_header), 64);
    CHECK_EQ(sizeof(struct covers_inbox_entry), 32);
    CHECK_EQ(sizeof(struct covers_inbox_block), 32);
    const uint32_t size = make_inbox(10);
    struct covers_inbox_header *h = (struct covers_inbox_header *)inbox;
    CHECK(covers_inbox_header_ok(h, size, INBOX_SIDE));
    /* another screen's, another size of file, a byte changed */
    CHECK(!covers_inbox_header_ok(h, size, 176));
    CHECK(!covers_inbox_header_ok(h, size - 1, INBOX_SIDE));
    CHECK(!covers_inbox_header_ok(h, size + 1, INBOX_SIDE));
    h->count ^= 0x100;
    CHECK(!covers_inbox_header_ok(h, size, INBOX_SIDE));
    h->count ^= 0x100;
    CHECK(covers_inbox_header_ok(h, size, INBOX_SIDE));
    /* other versions and forms, with their crc right */
    static const size_t fields[] = {
        offsetof(struct covers_inbox_header, magic),
        offsetof(struct covers_inbox_header, version),
        offsetof(struct covers_inbox_header, header_size),
        offsetof(struct covers_inbox_header, format),
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
    {
        uint32_t *f = (uint32_t *)(inbox + fields[i]);
        const uint32_t was = *f;
        *f = was + 1;
        fix_header();
        CHECK(!covers_inbox_header_ok(h, size, INBOX_SIDE));
        *f = was;
        fix_header();
    }
    /* no entries, too many, or more than the file holds */
    const uint32_t count = h->count;
    static const uint32_t counts[] = { 0, COVERS_INBOX_MAX + 1, 0x08000000 };
    for (size_t i = 0; i < 3; i++)
    {
        h->count = counts[i];
        fix_header();
        CHECK(!covers_inbox_header_ok(h, size, INBOX_SIDE));
    }
    h->count = count;
    fix_header();
    CHECK(covers_inbox_header_ok(h, size, INBOX_SIDE));
    /* a file at 2 GB or past it */
    h->file_size = 0x80000000u;
    fix_header();
    CHECK(!covers_inbox_header_ok(h, 0x80000000u, INBOX_SIDE));
}

static void test_inbox_entries(void)
{
    const uint32_t size = make_inbox(10);
    struct covers_inbox_entry *e =
        (struct covers_inbox_entry *)(inbox + sizeof(struct covers_inbox_header));
    CHECK(covers_inbox_entries_ok(e, 10, size));
    /* out of order, twice the same, a size of 0 */
    struct covers_inbox_entry t = e[3];
    e[3] = e[4];
    e[4] = t;
    CHECK(!covers_inbox_entries_ok(e, 10, size));
    e[4] = e[3];
    CHECK(!covers_inbox_entries_ok(e, 10, size));
    make_inbox(10);
    e[0].size = 0;
    CHECK(!covers_inbox_entries_ok(e, 10, size));
    /* blocks not aligned, among the entries, past the end, too short */
    make_inbox(10);
    e[5].offset += 4;
    CHECK(!covers_inbox_entries_ok(e, 10, size));
    make_inbox(10);
    e[5].offset = 0 + COVERS_INBOX_ALIGN * 0;
    CHECK(covers_inbox_entries_ok(e, 10, size)); /* none: one the iPod has */
    e[5].offset = COVERS_INBOX_ALIGN;
    if (sizeof(struct covers_inbox_header) + 10 * sizeof(*e) > COVERS_INBOX_ALIGN)
        CHECK(!covers_inbox_entries_ok(e, 10, size));
    make_inbox(10);
    e[9].length = size - e[9].offset + 1;
    CHECK(!covers_inbox_entries_ok(e, 10, size));
    make_inbox(10);
    e[9].length = sizeof(struct covers_inbox_block) - 1;
    CHECK(!covers_inbox_entries_ok(e, 10, size));
    /* an offset and a length that wrap past 4 GB */
    make_inbox(10);
    e[9].length = 0xffffffffu - e[9].offset + 2;
    CHECK(!covers_inbox_entries_ok(e, 10, size));
}

static void test_inbox_find(void)
{
    const uint32_t n = 1001;
    make_inbox(n);
    struct covers_inbox_entry *e =
        (struct covers_inbox_entry *)(inbox + sizeof(struct covers_inbox_header));
    for (uint32_t i = 0; i < n; i++)
    {
        CHECK(covers_inbox_find(e, n, e[i].size, e[i].quick) == &e[i]);
        CHECK(covers_inbox_find_sums(e, n, e[i].size, e[i].crc, e[i].adler)
              == &e[i]);
        /* the same quick key with another size, the same size with another
           key or other sums */
        CHECK(!covers_inbox_find(e, n, e[i].size + 1000, e[i].quick));
        CHECK(!covers_inbox_find(e, n, e[i].size, e[i].quick + 1));
        CHECK(!covers_inbox_find_sums(e, n, e[i].size, e[i].crc, e[i].adler + 1));
        CHECK(!covers_inbox_find_sums(e, n, e[i].size + 1000, e[i].crc,
                                      e[i].adler));
    }
    CHECK(!covers_inbox_find(e, 0, e[0].size, e[0].quick));
    CHECK(!covers_inbox_find_sums(e, 0, e[0].size, e[0].crc, e[0].adler));
    /* the quick key: a picture under twice the part is summed whole */
    CHECK(covers_quick_whole(2 * COVERS_QUICK_PART - 1));
    CHECK(!covers_quick_whole(2 * COVERS_QUICK_PART));
}

static void test_inbox_blocks(void)
{
    make_inbox(3);
    struct covers_inbox_entry *e =
        (struct covers_inbox_entry *)(inbox + sizeof(struct covers_inbox_header));
    struct covers_inbox_parts parts;
    const unsigned char *run = inbox + e[1].offset;
    CHECK(covers_inbox_blocks(run, e[1].length, e[1].crc, INBOX_SIDE, &parts));
    CHECK_EQ(parts.width[COVERS_BLOCK_LARGE], 24);
    CHECK_EQ(parts.height[COVERS_BLOCK_LARGE], 18);
    CHECK_EQ(parts.width[COVERS_BLOCK_FRAME], 20);
    CHECK(parts.pixels[COVERS_BLOCK_LARGE][0] == 1);
    CHECK(parts.pixels[COVERS_BLOCK_FRAME][0] == (fb_data)~1u);
    CHECK(parts.pixels[COVERS_BLOCK_SLIDE] == NULL);
    CHECK(parts.at[COVERS_BLOCK_LARGE] == 0);
    /* another picture's blocks */
    CHECK(!covers_inbox_blocks(run, e[1].length, e[0].crc, INBOX_SIDE, &parts));
    /* cut short: the frame's pixels lost, the large picture kept */
    CHECK(covers_inbox_blocks(run, e[1].length - 1, e[1].crc, INBOX_SIDE,
                              &parts));
    CHECK(parts.pixels[COVERS_BLOCK_FRAME] == NULL);
    CHECK(!covers_inbox_blocks(run, 40, e[1].crc, INBOX_SIDE, &parts));
    /* a pixel of the frame changed: the frame passed over */
    unsigned char *frame = inbox + e[1].offset
                           + parts.at[COVERS_BLOCK_LARGE] + 32
                           + ((24 * 18 * 2 + 31) & ~31) + 32;
    frame[3] ^= 1;
    CHECK(covers_inbox_blocks(run, e[1].length, e[1].crc, INBOX_SIDE, &parts));
    CHECK(parts.pixels[COVERS_BLOCK_FRAME] == NULL);
    frame[3] ^= 1;
    /* a pixel of the large picture changed: none */
    inbox[e[1].offset + 40] ^= 1;
    CHECK(!covers_inbox_blocks(run, e[1].length, e[1].crc, INBOX_SIDE, &parts));
    inbox[e[1].offset + 40] ^= 1;
    /* a large picture larger than the screen's, or a frame above 200 */
    make_inbox(1);
    uint32_t len = put_block(inbox + e[0].offset, COVERS_BLOCK_LARGE, 241, 10,
                             1, e[0].crc);
    CHECK(!covers_inbox_blocks(inbox + e[0].offset, len, e[0].crc, INBOX_SIDE,
                               &parts));
    len = put_block(inbox + e[0].offset, COVERS_BLOCK_LARGE, 240, 10, 1,
                    e[0].crc);
    len += put_block(inbox + e[0].offset + len, COVERS_BLOCK_FRAME, 201, 10,
                     1, e[0].crc);
    /* a kind not known, then a slide and a row */
    len += put_block(inbox + e[0].offset + len, 9, 4, 4, 1, e[0].crc);
    len += put_block(inbox + e[0].offset + len, COVERS_BLOCK_SLIDE, 136, 6,
                     1, e[0].crc);
    len += put_block(inbox + e[0].offset + len, COVERS_BLOCK_ROW, 32, 32, 1,
                     e[0].crc);
    CHECK(covers_inbox_blocks(inbox + e[0].offset, len, e[0].crc, INBOX_SIDE,
                              &parts));
    CHECK(parts.pixels[COVERS_BLOCK_FRAME] == NULL);
    CHECK_EQ(parts.width[COVERS_BLOCK_SLIDE], 136);
    CHECK_EQ(parts.width[COVERS_BLOCK_ROW], 32);
    /* the large picture not first */
    len = put_block(inbox + e[0].offset, COVERS_BLOCK_FRAME, 20, 10, 1, e[0].crc);
    len += put_block(inbox + e[0].offset + len, COVERS_BLOCK_LARGE, 24, 12,
                     1, e[0].crc);
    CHECK(!covers_inbox_blocks(inbox + e[0].offset, len, e[0].crc, INBOX_SIDE,
                               &parts));
    /* a block saying it's larger than what's left */
    len = put_block(inbox + e[0].offset, COVERS_BLOCK_LARGE, 240, 240, 1,
                    e[0].crc);
    CHECK(!covers_inbox_blocks(inbox + e[0].offset, len - 32, e[0].crc,
                               INBOX_SIDE, &parts));
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
    test_slide_fit();
    test_inbox_header();
    test_inbox_entries();
    test_inbox_find();
    test_inbox_blocks();
    return check_done("covers");
}
