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
#include <string.h>
#include "covers_table.h"

/* zlib's: the polynomial reflected, the sum inverted before and after */
static uint32_t crc_table[256];

uint32_t covers_crc32(uint32_t crc, const void *data, size_t size)
{
    if (!crc_table[1])
    {
        for (uint32_t n = 0; n < 256; n++)
        {
            uint32_t c = n;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[n] = c;
        }
    }
    const unsigned char *p = data;
    crc = ~crc;
    while (size--)
        crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/* zlib's: the most bytes before the sums are taken modulo, as they'd
   overflow 32 bits after */
#define ADLER_BASE 65521u
#define ADLER_NMAX 5552

uint32_t covers_adler32(uint32_t adler, const void *data, size_t size)
{
    const unsigned char *p = data;
    uint32_t a = adler & 0xffff, b = adler >> 16;
    while (size > 0)
    {
        size_t n = size < ADLER_NMAX ? size : ADLER_NMAX;
        size -= n;
        while (n--)
        {
            a += *p++;
            b += a;
        }
        a %= ADLER_BASE;
        b %= ADLER_BASE;
    }
    return b << 16 | a;
}

uint32_t covers_file_check(uint32_t file_size, uint32_t pic_pos,
                           uint32_t pic_size)
{
    /* FNV-1a of the three, never 0 */
    const uint32_t v[3] = { file_size, pic_pos, pic_size };
    uint32_t h = 2166136261u;
    for (int i = 0; i < 3; i++)
    {
        for (int k = 0; k < 32; k += 8)
            h = (h ^ ((v[i] >> k) & 0xff)) * 16777619u;
    }
    return h ? h : 1;
}

const struct covers_song_ram *covers_song_find(const struct covers_song_ram *e,
                                               size_t n, uint64_t path)
{
    size_t lo = 0, hi = n;
    while (lo < hi)
    {
        const size_t mid = lo + (hi - lo) / 2;
        if (e[mid].path == path)
            return &e[mid];
        if (e[mid].path < path)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

size_t covers_pictures_room(size_t pictures)
{
    size_t room = 16;
    while (room < 2 * pictures)
        room *= 2;
    return room;
}

struct covers_picture *covers_picture_place(struct covers_picture *table,
                                            size_t room, uint32_t crc,
                                            uint32_t adler, uint32_t size)
{
    size_t i = (crc ^ adler * 2654435761u ^ size) & (room - 1);
    for (size_t n = 0; n < room; n++, i = (i + 1) & (room - 1))
    {
        struct covers_picture *p = &table[i];
        if (!p->size
            || (p->crc == crc && p->adler == adler && p->size == size))
            return p;
    }
    return NULL;
}

void covers_fit(int fw, int fh, int *w, int *h)
{
    if (fw <= 0 || fh <= 0)
        return;
    if (*w * fh > *h * fw)
        *w = (*h * fw + fh / 2) / fh;
    else
        *h = (*w * fh + fw / 2) / fw;
}

/* Each of src's pixels spans dw units across and each of dst's sw, so that a
   row is sw * dw units both ways; down likewise. A pixel of src lies in one
   of dst's or across the edge between two, as dst is no larger. */
static void add_row(const fb_data *row, int sw, int dw, uint32_t *h)
{
    memset(h, 0, 3 * dw * sizeof(*h));
    int x = 0;
    uint32_t edge = sw; /* where dst's pixel x ends */
    for (int i = 0; i < sw; i++)
    {
        const unsigned p = row[i];
        const uint32_t r = p >> 11, g = (p >> 5) & 0x3f, b = p & 0x1f;
        const uint32_t start = (uint32_t)i * dw, end = start + dw;
        uint32_t *o = &h[3 * x];
        if (end <= edge)
        {
            o[0] += r * dw;
            o[1] += g * dw;
            o[2] += b * dw;
            if (end == edge && x + 1 < dw)
            {
                x++;
                edge += sw;
            }
        }
        else
        {
            const uint32_t in = edge - start, out = end - edge;
            o[0] += r * in;
            o[1] += g * in;
            o[2] += b * in;
            x++;
            edge += sw;
            o += 3;
            o[0] += r * out;
            o[1] += g * out;
            o[2] += b * out;
        }
    }
}

static void put_row(const uint32_t *acc, int dw, uint64_t recip, fb_data *out)
{
    static const uint32_t top[3] = { 0x1f, 0x3f, 0x1f };
    for (int x = 0; x < dw; x++)
    {
        uint32_t c[3];
        for (int k = 0; k < 3; k++)
        {
            c[k] = (uint32_t)(((uint64_t)acc[3 * x + k] * recip
                               + 0x80000000u) >> 32);
            if (c[k] > top[k])
                c[k] = top[k];
        }
        out[x] = (fb_data)(c[0] << 11 | c[1] << 5 | c[2]);
    }
}

void covers_area_scale(const fb_data *src, int sw, int sh, fb_data *dst,
                       int dw, int dh, uint32_t *acc)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || dw > sw || dh > sh)
        return;
    uint32_t *h = acc + 3 * dw, *v = acc;
    /* 1 / (sw * sh) in 32 bits of fraction: a pixel of dst is that many
       units of src's pixels' values */
    const uint64_t area = (uint64_t)sw * sh;
    const uint64_t recip = ((1ull << 32) + area / 2) / area;
    memset(v, 0, 3 * dw * sizeof(*v));
    int y = 0;
    uint32_t edge = sh; /* where dst's row y ends */
    for (int j = 0; j < sh; j++)
    {
        add_row(src + j * sw, sw, dw, h);
        const uint32_t start = (uint32_t)j * dh, end = start + dh;
        if (end <= edge)
        {
            for (int k = 0; k < 3 * dw; k++)
                v[k] += h[k] * dh;
            if (end == edge)
            {
                put_row(v, dw, recip, dst + y * dw);
                memset(v, 0, 3 * dw * sizeof(*v));
                y++;
                edge += sh;
            }
        }
        else
        {
            const uint32_t in = edge - start, out = end - edge;
            for (int k = 0; k < 3 * dw; k++)
                v[k] += h[k] * in;
            put_row(v, dw, recip, dst + y * dw);
            y++;
            edge += sh;
            for (int k = 0; k < 3 * dw; k++)
                v[k] = h[k] * out;
        }
    }
}

void covers_slide_fit(int fw, int fh, int side, int *w, int *h)
{
    *w = *h = side;
    if (fw <= 0 || fh <= 0)
        return;
    /* a pixel at least, as a frame 1 px wide gives none at 99 */
    if (fw > fh)
        *h = (side * fh + fw / 2) / fw;
    else
        *w = (side * fw + fh / 2) / fh;
    if (*w < 1)
        *w = 1;
    if (*h < 1)
        *h = 1;
}

bool covers_inbox_header_ok(const struct covers_inbox_header *h,
                            uint32_t file_size, uint32_t side)
{
    if (h->magic != COVERS_INBOX_MAGIC || h->version != COVERS_INBOX_VERSION
        || h->header_size != sizeof(*h) || h->side != side
        || h->format != COVERS_INBOX_RGB565 || !h->count
        || h->count > COVERS_INBOX_MAX || h->file_size != file_size
        || file_size > 0x7fffffffu
        || h->header_crc != covers_crc32(COVERS_CRC32_INIT, h,
                                         offsetof(struct covers_inbox_header,
                                                  header_crc)))
        return false;
    return sizeof(*h) + (uint64_t)h->count * sizeof(struct covers_inbox_entry)
           <= file_size;
}

static int cmp_key(uint32_t size, uint32_t quick,
                   const struct covers_inbox_entry *e)
{
    if (size != e->size)
        return size < e->size ? -1 : 1;
    if (quick != e->quick)
        return quick < e->quick ? -1 : 1;
    return 0;
}

bool covers_inbox_entries_ok(const struct covers_inbox_entry *e, uint32_t n,
                             uint32_t file_size)
{
    const uint64_t data = sizeof(struct covers_inbox_header)
                          + (uint64_t)n * sizeof(*e);
    for (uint32_t i = 0; i < n; i++)
    {
        if (!e[i].size || (i && cmp_key(e[i].size, e[i].quick, &e[i - 1]) <= 0))
            return false;
        if (!e[i].offset)
            continue;
        if (e[i].offset % COVERS_INBOX_ALIGN || e[i].offset < data
            || e[i].length < sizeof(struct covers_inbox_block)
            || (uint64_t)e[i].offset + e[i].length > file_size)
            return false;
    }
    return true;
}

struct covers_inbox_entry *covers_inbox_find(struct covers_inbox_entry *e,
                                             uint32_t n, uint32_t size,
                                             uint32_t quick)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi)
    {
        const uint32_t mid = lo + (hi - lo) / 2;
        const int c = cmp_key(size, quick, &e[mid]);
        if (!c)
            return &e[mid];
        if (c > 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

struct covers_inbox_entry *covers_inbox_find_sums(struct covers_inbox_entry *e,
                                                  uint32_t n, uint32_t size,
                                                  uint32_t crc, uint32_t adler)
{
    /* the first of that size, then those after it */
    uint32_t lo = 0, hi = n;
    while (lo < hi)
    {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (e[mid].size < size)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (; lo < n && e[lo].size == size; lo++)
    {
        if (e[lo].crc == crc && e[lo].adler == adler)
            return &e[lo];
    }
    return NULL;
}

bool covers_inbox_blocks(const void *run, uint32_t length, uint32_t picture,
                         int side, struct covers_inbox_parts *parts)
{
    /* the most a part may be each way: the frame's box is 200 px whatever
       the screen, and a slide or a row is no larger than the large picture */
    static const int frame_side = 200;
    memset(parts, 0, sizeof(*parts));
    const unsigned char *p = run;
    uint32_t at = 0;
    while (length - at >= sizeof(struct covers_inbox_block))
    {
        struct covers_inbox_block b;
        memcpy(&b, p + at, sizeof(b));
        if (b.magic != COVERS_BLOCK_MAGIC)
            break;
        const uint32_t bytes = (uint32_t)b.width * b.height * sizeof(fb_data);
        const uint32_t room = length - at - sizeof(b);
        const uint32_t padded = (bytes + 31) & ~31u;
        if (padded > room)
            break;
        const int most = b.kind == COVERS_BLOCK_FRAME ? frame_side : side;
        const fb_data *pixels = (const fb_data *)(p + at + sizeof(b));
        if (b.kind > 0 && b.kind < COVERS_BLOCK_KINDS && !parts->pixels[b.kind]
            && b.width && b.height && b.width <= most && b.height <= most
            && b.picture == picture
            && b.crc == covers_crc32(COVERS_CRC32_INIT, pixels, bytes))
        {
            parts->pixels[b.kind] = pixels;
            parts->width[b.kind] = b.width;
            parts->height[b.kind] = b.height;
            parts->at[b.kind] = at;
        }
        /* the large picture first, which the others are of */
        if (!parts->pixels[COVERS_BLOCK_LARGE])
            return false;
        at += sizeof(b) + padded;
    }
    return parts->pixels[COVERS_BLOCK_LARGE] != NULL;
}
