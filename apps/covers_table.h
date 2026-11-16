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
#ifndef _COVERS_TABLE_H_
#define _COVERS_TABLE_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "lcd.h"

/* The parts of the covers Updating Library prepares for each song (itunesdb.c)
   that need no disk: what tells a picture and a file, and the tables they're
   found in. Built on the Mac too, by tests/covers. */

/* A picture's bytes, as they lie in the file, by zlib's CRC-32 and Adler-32:
   what a computer can make of them as well. Begin with the INIT values and
   go on a part at a time. */
#define COVERS_CRC32_INIT   0u
#define COVERS_ADLER32_INIT 1u
uint32_t covers_crc32(uint32_t crc, const void *data, size_t size);
uint32_t covers_adler32(uint32_t adler, const void *data, size_t size);

/* What a song's entry kept in memory is checked against as the song plays:
   its file's size and its own picture's place in it, as the tags read then
   tell (0, 0 for none). A file changed since its entry was made has another,
   and shows no cover rather than the old one. */
uint32_t covers_file_check(uint32_t file_size, uint32_t pic_pos,
                           uint32_t pic_size);

/* A song's entry word: the slot of its picture (1 on, 0 for none), why it has
   none when it has a picture (enum covers_reason), and flags */
#define COVERS_SLOT_BITS    20
#define COVERS_SLOT_MASK    ((1u << COVERS_SLOT_BITS) - 1)
#define COVERS_REASON_SHIFT COVERS_SLOT_BITS
#define COVERS_REASON_MASK  0xfu
#define COVERS_FLAGS_SHIFT  24
#define COVERS_SLOT_OF(w)   ((w) & COVERS_SLOT_MASK)
#define COVERS_REASON_OF(w) (((w) >> COVERS_REASON_SHIFT) & COVERS_REASON_MASK)
#define COVERS_FLAGS_OF(w)  ((w) >> COVERS_FLAGS_SHIFT)
#define COVERS_WORD(slot, reason, flags) \
    (((uint32_t)(slot) & COVERS_SLOT_MASK) \
     | ((uint32_t)(reason) & COVERS_REASON_MASK) << COVERS_REASON_SHIFT \
     | (uint32_t)(flags) << COVERS_FLAGS_SHIFT)

/* A song's entry in memory, for a song with a cover of its own or a reason
   it has none: sorted by its path's itunesdb_path_hash() */
struct covers_song_ram {
    uint64_t path;
    uint32_t word;      /* COVERS_WORD() */
    uint32_t check;     /* covers_file_check() of the file it was made of */
};

/* the entry of path in n sorted ones, NULL for none */
const struct covers_song_ram *covers_song_find(const struct covers_song_ram *e,
                                               size_t n, uint64_t path);

/* The pictures a making has: each by its bytes' sums and size, the places
   it has, in an open table of room places, room a power of two at least
   twice the pictures it is to hold. A size of 0 is an empty place. */
struct covers_picture {
    uint32_t crc, adler, size;
    uint16_t slot;          /* of its frame and slide, from 1 */
    uint16_t large;         /* of its large picture, from 1; 0 for none */
    uint16_t width, height; /* of its frame */
    uint32_t colour;
};
/* the room for this many */
size_t covers_pictures_room(size_t pictures);
/* the place of the picture, which has its size then, or the empty one to put
   it in; NULL with the table full */
struct covers_picture *covers_picture_place(struct covers_picture *table,
                                            size_t room, uint32_t crc,
                                            uint32_t adler, uint32_t size);

/* The size a frame fw by fh takes in a box *w by *h, keeping its shape, as
   itunesdb.c's art_fit() and menu_cover.c's cover_fit() have it */
void covers_fit(int fw, int fh, int *w, int *h);

/* src, sw by sh, shrunk to dw by dh (at most as large each way) into dst:
   each pixel the mean of the part of src it covers, by the share of each of
   src's pixels in it. acc is room for 6 * dw words. */
void covers_area_scale(const fb_data *src, int sw, int sh, fb_data *dst,
                       int dw, int dh, uint32_t *acc);

#endif
