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
#ifndef _MH_WALK_H_
#define _MH_WALK_H_

/* The walks over the mh* records of iTunesDB, ArtworkDB and the Photo
   Database (libgpod's layout): each record at the offset its parent's header
   or the record before leads to, with its header's length at 4 and its total
   length at 8. A record lies within its parent, after the parent's header,
   and its own header within it: on the iPod's 32-bit offsets a damaged length
   can lead back to an earlier record, which a walk would go round as often as
   a count from the file lets it, and a crafted one could lead each walk on
   through the rest of the file. A record that doesn't isn't read, and its
   walk ends there. Not Finder's mhaf, in an mhod of type 6, which has no
   total at 8 and isn't walked. Included where off_t is as the file reading
   it has it. */

#include <stdint.h>
#include <sys/types.h>

/* the first record's parent is the file */
#define MH_FILE_END 0x7fffffff

/* Where the record or header at pos ends, len bytes on: -1 unless that is
   further on and no further than end, where its parent's records end. pos
   is that of a record read, so 0 or more; end is -1 for a parent found
   broken, which has no room. */
static inline off_t next_record(off_t pos, uint32_t len, off_t end)
{
    return pos <= end && len - 1 < (uint32_t)(end - pos) ? (off_t)(pos + len)
                                                          : -1;
}

/* the end of the record at pos, p its first 12 bytes: by its total length,
   within end, with its header within that; -1 if not */
static inline off_t record_end(off_t pos, const unsigned char *p, off_t end)
{
    const uint32_t header = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
    const uint32_t total = p[8] | p[9] << 8 | p[10] << 16 | (uint32_t)p[11] << 24;
    const off_t next = next_record(pos, total, end);
    return next_record(pos, header, next) < 0 ? -1 : next;
}

#endif
