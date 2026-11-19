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
#ifndef _HEALTH_LOG_H
#define _HEALTH_LOG_H

#include "gcc_extensions.h"

/* What went wrong, in ROCKBOX_DIR "/health.log", for someone whose iPod
   misbehaves to send: no song's name or path goes in a line, nor anything
   that tells the very iPod. */

/* A line, kept in memory with the seconds since boot. From threads of the
   CPU only, never from an interrupt or the COP; allocates nothing and
   touches no file. */
void health_note(const char *fmt, ...) ATTRIBUTE_PRINTF(1, 2);

/* One more of a kind of event, by its name, a string that lasts (a
   literal): the kinds counted since boot go on one line at the flush when
   one has grown. From threads of the CPU only; HEALTH_TALLIES kinds at most,
   any more dropped. Allocates nothing and touches no file. */
#define HEALTH_TALLIES 6
void health_tally(const char *name);

/* A line of the volume's free clusters: as USB connects, before the flush;
   the first flush since boot adds one itself if none came before */
void health_note_free(void);

/* The lines since the last time, written: from system_flush(), so as USB
   connects and at power off, and as the iPod falls asleep (SYS_SLEEP_FLUSH,
   misc.c); not during use */
void health_flush(void);

/* As the computer lets the disk go: the time it had it is left out of the
   next line of the CPU's clocks, the USB stack's boost being no use of the
   iPod's */
void health_usb_ended(void);

#endif
