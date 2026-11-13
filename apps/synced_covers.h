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
#ifndef SYNCED_COVERS_H
#define SYNCED_COVERS_H

#include <stdbool.h>
#include <stddef.h>

/* The covers the computer puts on the iPod at a sync, made again by it: the
   iPod's description lists the 320x240 format the main menu's cover is read
   from, and the artwork is set aside, so that the next sync writes every
   cover anew. Nothing is kept of this but what is on the disk. */

enum synced_covers_description
{
    DESCRIPTION_NONE,       /* no file: the firmware's own is given, which
                               lists the format */
    DESCRIPTION_READY,      /* lists the format */
    DESCRIPTION_CHANGED,    /* did not, and does now */
    DESCRIPTION_UNKNOWN,    /* not of the shape this changes, or not written:
                               as it was */
};

/* Has the description list the format: at boot, before USB can read it, and
   from the action. Not under music: it takes memory for the file. */
enum synced_covers_description synced_covers_describe(void);

/* At boot: the description, and what a cut left of a folder being removed */
void synced_covers_boot(void);

/* the old artwork is set aside, and no sync has made the new yet */
bool synced_covers_waiting(void);
/* there is artwork to set aside */
bool synced_covers_present(void);
bool synced_covers_set_aside(void);
/* false if the computer has begun the new artwork, or with none aside */
bool synced_covers_put_back(void);
/* After an import: with new covers in place, the folder set aside goes */
void synced_covers_imported(bool covers);

#endif /* SYNCED_COVERS_H */
