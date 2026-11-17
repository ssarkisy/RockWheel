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
#ifndef _SKIN_RATIO_H_
#define _SKIN_RATIO_H_

#include <stdint.h>

/* scale * part / whole, worked out in 64 bits: an unsigned long has 32 on
   the iPod, where 100 * a time in ms wrapped past 11.9 h and 1000 * it past
   71.6 min. whole > 0 */
static inline unsigned long skin_part_of(unsigned long scale, unsigned long part,
                                         unsigned long whole)
{
    return (unsigned long)((uint64_t)scale * part / whole);
}

/* Where a bar's slider stands along the size pixels it goes, at end of
   length (> 0), no further than the bar's end. size * end in 32 bits wrapped
   past 2^32: a 276 px bar's slider past 4 h 19 min of a track */
static inline int skin_bar_place(int size, unsigned long end, unsigned long length)
{
    if (size <= 0)
        return 0;
    return (int)skin_part_of(size, end < length ? end : length, length);
}

#endif /* _SKIN_RATIO_H_ */
