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
#ifndef _FREE_SPACE_H
#define _FREE_SPACE_H

#include <stdbool.h>
#include "config.h"

/* The disk's free space counted again by the iPod itself, from the FAT, after
   each USB session and once at the first boot of a firmware: a computer may
   leave FSInfo's count, which the mount takes, off (free_space.c). */

#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
/* as USB connects, before the computer has the disk: the next boot counts,
   unless a count after this session says it needn't */
void free_space_usb_connected(void);
/* as the computer has let the disk go: a count is owed */
void free_space_usb_ended(void);
/* The count owed, if one is: from the main thread, into the plugins' buffer,
   so with no plugin running, and with the music stopped. go_on, if any, is
   asked before each of its reads, and false stops it there, as music starting
   does: it goes on from there the next time, unless the volume is mounted or
   unmounted meanwhile, as for USB, which begins it again. True once none is
   owed. */
bool free_space_recount(bool (*go_on)(void));
#else
/* the simulator's file system is the computer's own */
static inline void free_space_usb_connected(void)
{
}
static inline void free_space_usb_ended(void)
{
}
static inline bool free_space_recount(bool (*go_on)(void))
{
    (void)go_on;
    return true;
}
#endif

#endif
