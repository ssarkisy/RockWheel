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

/* The size a Cover Flow slide side pixels a side takes of a frame fw by fh,
   keeping its shape, as the slides are made: 1 pixel at least each way */
void covers_slide_fit(int fw, int fh, int side, int *w, int *h);

/** The covers a computer prepared, which Updating Library takes rather than
    decoding the pictures: .rockbox/covers_inbox.dat, written by RockWheel's
    installer while the iPod is a disk, read and then removed by the making
    (itunesdb.c). Byte for byte, as another program writes it; all
    little-endian:
    - the header, struct covers_inbox_header;
    - at 64, the entries, struct covers_inbox_entry, one a picture, in the
      order of their sizes and then of their quick keys, no two alike;
    - from each entry's offset, a multiple of COVERS_INBOX_ALIGN, length bytes
      of its blocks one after another: each a struct covers_inbox_block, its
      pixels, RGB565 as the screen has them, and zeros to a multiple of 32
      bytes. The large picture first, which every entry with blocks has, then
      any of the others.
    A picture is known by its size and quick key alone, and its sums are taken
    as the entry says, unread, as Finder's covers are, but only in a file
    whose time is known and isn't later than this file's own, as the disk has
    both: one later came or changed after the computer's run, and is taken
    only by its sums, which the iPod reads, else made by it. So the computer
    sets this file's time to when it began reading the iPod's files, less 2 s
    as FAT keeps times in 2 s steps: a picture changed while it read is later.
    Two pictures of one size whose first and last parts are alike are left
    out by the computer. Taken for the computer's, as their owner chose,
    with the same size and ends as one of its pictures:
    - a picture that came or changed between the computer's run and the
      making that takes this file, in a file whose time says otherwise, as a
      copy keeps its file's time;
    - the picture of a song Finder synced without its artwork, which the
      computer doesn't read.
    From then on the song's file's time tells it changed, as for every cover
    made here.
    An entry with no blocks (offset 0) is of a picture the iPod has, which its
    sums find. A block of a kind or a size other than the firmware would make
    is passed over, and that size made of the large picture, as for a picture
    decoded. **/
#define COVERS_INBOX_MAGIC   0x49435752u /* "RWCI" */
#define COVERS_INBOX_VERSION 1
#define COVERS_INBOX_RGB565  1
#define COVERS_INBOX_ALIGN   512
#define COVERS_INBOX_MAX     26000 /* entries, as the slots */
#define COVERS_BLOCK_MAGIC   0x50435752u /* "RWCP" */
/* the bytes of a picture's start, and of its end, its quick key is of */
#define COVERS_QUICK_PART    4096

struct covers_inbox_header {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;   /* sizeof(struct covers_inbox_header) */
    uint32_t side;          /* the large pictures' box, ITUNESDB_LARGE_SIDE */
    uint32_t format;        /* COVERS_INBOX_RGB565 */
    uint32_t count;         /* entries, 1 to COVERS_INBOX_MAX */
    uint32_t file_size;     /* the file's bytes, below 2 GB */
    uint32_t index_crc;     /* covers_crc32() of the entries */
    uint32_t colour_kind;   /* how the colours were worked out: taken only as
                               COVERS_COLOUR_KIND (itunesdb.c) */
    char maker[16];         /* the program, for the health log */
    uint32_t made;          /* Unix time, for the log */
    uint32_t unused;
    uint32_t header_crc;    /* covers_crc32() of the bytes before it */
};

struct covers_inbox_entry {
    uint32_t size;          /* of the picture's bytes as the tags' parser
                               places them (albumart.pos and .size), or of
                               the image file */
    uint32_t quick;         /* their quick key */
    uint32_t crc, adler;    /* covers_crc32() and _adler32() of them all, as
                               the song's entry keeps them */
    uint32_t offset;        /* of its blocks, 0 for none */
    uint32_t length;        /* of its blocks */
    uint32_t colour;        /* of its slide, as cover_palette_colour() has it */
    uint32_t unused;        /* 0. In memory, after a take, the making keeps the
                               slot it went to here, and its row's place in
                               length */
};

enum covers_block_kind {
    COVERS_BLOCK_LARGE = 1, /* the picture fitted into side by side, kept from
                               growing: by rows */
    COVERS_BLOCK_FRAME,     /* covers_fit() of that into the 200 px box: rows */
    COVERS_BLOCK_SLIDE,     /* covers_slide_fit() of the frame: column after
                               column, as Cover Flow draws them */
    COVERS_BLOCK_ROW,       /* a list's row's, LINE_COVER_SIZE square, the
                               slide shrunk in its middle on white: rows */
    COVERS_BLOCK_KINDS
};

struct covers_inbox_block {
    uint32_t magic;         /* COVERS_BLOCK_MAGIC */
    uint16_t kind;          /* enum covers_block_kind */
    uint16_t unused;
    uint16_t width, height;
    uint32_t crc;           /* covers_crc32() of its pixels */
    uint32_t picture;       /* the crc of its entry, which it's a part of */
    uint32_t spare[3];
};

/* The quick key of a picture of size bytes: covers_crc32() of its first
   COVERS_QUICK_PART bytes and then of its last as many, one sum, or of all of
   them under twice that */
static inline bool covers_quick_whole(uint32_t size)
{
    return size < 2 * COVERS_QUICK_PART;
}

/* The header of a file of file_size bytes, for large pictures side pixels a
   side: a form this firmware takes, with its entries within the file */
bool covers_inbox_header_ok(const struct covers_inbox_header *h,
                            uint32_t file_size, uint32_t side);
/* the entries in order, no two alike, each one's blocks past the entries and
   within the file */
bool covers_inbox_entries_ok(const struct covers_inbox_entry *e, uint32_t n,
                             uint32_t file_size);
/* the entry of a picture by its size and quick key, or by its size and sums;
   NULL for none */
struct covers_inbox_entry *covers_inbox_find(struct covers_inbox_entry *e,
                                             uint32_t n, uint32_t size,
                                             uint32_t quick);
struct covers_inbox_entry *covers_inbox_find_sums(struct covers_inbox_entry *e,
                                                  uint32_t n, uint32_t size,
                                                  uint32_t crc, uint32_t adler);

/* An entry's blocks, read into run (4-aligned): the pixels of each kind, NULL
   for none, and where its block is from the first */
struct covers_inbox_parts {
    const fb_data *pixels[COVERS_BLOCK_KINDS];
    int width[COVERS_BLOCK_KINDS], height[COVERS_BLOCK_KINDS];
    uint32_t at[COVERS_BLOCK_KINDS];
};
/* Each block of the entry whose crc is picture that is whole and sound, and
   no larger than such a part is: false without its large picture, which the
   others are made of */
bool covers_inbox_blocks(const void *run, uint32_t length, uint32_t picture,
                         int side, struct covers_inbox_parts *parts);

#endif
