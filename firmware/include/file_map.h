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
#ifndef _FILE_MAP_H_
#define _FILE_MAP_H_

#include <stdbool.h>
#include <sys/types.h>
#include "config.h"

/* Where a part of a file lies on its drive, found once from its clusters, so
   that it's read again by its sectors without the file system: no path looked
   up, no chain of clusters walked, for data read often in small pieces and
   written seldom, as the covers of the lists' rows. Only reads: what is
   written goes through the file as ever. A map is good while no cluster has
   left any file of the volume since it was made and the volume stays
   mounted: a cluster of the file's could then be another's, and USB has the
   disk. Each read takes the file system's lock as a reader, which keeps the
   volume mounted meanwhile, and looks at the volume's count of clusters
   freed both before the transfer and after it: clusters are freed under
   that lock too, as by ftruncate(), and one freed meanwhile fails the read.
   A map no longer good is made again, from the file open. Only on the file
   system of Rockbox' own, not in the simulator. */
#if (CONFIG_PLATFORM & PLATFORM_NATIVE) && !defined(BOOTLOADER)
#define HAVE_FILE_MAP
#include "mv.h"

/* sectors in a row on the drive */
struct file_map_run
{
    unsigned long at;    /* the first's number in the file, in sectors */
    unsigned long count; /* how many */
    sector_t sector;     /* the first's on the drive, its partition's start in */
};

struct file_map
{
    unsigned long gen;   /* of the volume's clusters as it was made */
    int volume;
    unsigned int sector_size; /* in bytes, as the runs count them */
    int runs;            /* how many it has, 0 for none */
};

/* The part of the open file from offset on, length bytes, both of whole
   sectors and within the file, into runs, max of them at most: how many,
   < 0 if it's in more, past the file's end, or the file system's clusters
   changed while it was walked. In map; its runs 0 then, and as of the
   volume as it is, which file_map_current() says until it changes */
int file_map(int fildes, off_t offset, off_t length, struct file_map *map,
             struct file_map_run *runs, int max);
/* size bytes of the file from offset on, both of whole sectors and in the
   part mapped, into buf, aligned as the drive reads into it: 0 read, < 0 if
   the map is no longer good, or the disk failed */
int file_read_mapped(const struct file_map *map, const struct file_map_run *runs,
                     off_t offset, size_t size, void *buf);
/* the volume is mounted, and its clusters as they were when the map was made
   or tried: only a hint, read without the lock, which a read takes */
bool file_map_current(const struct file_map *map);
#endif /* PLATFORM_NATIVE && !BOOTLOADER */

#endif /* _FILE_MAP_H_ */
