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
#ifndef _LIST_COVERS_H_
#define _LIST_COVERS_H_

#include <stdbool.h>
#include "lcd.h"

/* The covers of albums at the left of their rows in Music's lists of albums,
   as on an iPod classic: Settings > Appearance > Covers in Lists. Updating
   Library makes them, 32 px a side (LINE_COVER_SIZE), from Cover Flow's
   slides, into a file of their own in the order of Albums by year, so that a
   screen's covers are one short read. Kept in memory taken as the setting is
   turned on without music, and let go as it's turned off: none is taken while
   music plays. A list shows those kept, a note in the place of the others,
   and has them read, with those of the rows next to it, by the thread that
   reads an album's header's (album_header.c), the list drawn again as they
   come. */

/* the memory taken or let go as the setting and Album Art are: as settings
   are applied, at boot before music, and as either is changed; asked for
   while music plays, it's taken once that has stopped, as a list opens */
void list_covers_apply(void);
/* the setting's callback */
void list_covers_changed(bool on);
/* the setting is on and its memory taken: lists of albums show covers */
bool list_covers_on(void);

/* What an album's row shows in its column of covers (enum list_cover), by
   the album's number in the table of years (album_years.h), -1 for none, as
   Unknown Album: its cover's pixels in *pixels, which stay where they are,
   if it's kept; the note otherwise, the cover then one the list lacks, to be
   read; and the note for an album without one */
int list_covers_get(int no, const fb_data **pixels);
/* an album of a row next to those shown, the way the list goes first: its
   cover read after theirs */
void list_covers_ahead(int no);
/* A list's loop: before each of its frames, as what it lacks is counted anew;
   the time it waits for a button, as what the frame lacked is asked for, less
   while it's read; and after the wait, true if one has come since, for the
   list to be drawn again, as the wheel rests */
void list_covers_frame(void);
int list_covers_timeout(int timeout);
bool list_covers_poll(void);
/* a button the list took, as a wheel's step: as it turns, the reader reads
   nothing and what it has read isn't drawn until it rests, each frame
   reading its own rows' covers (list_covers_draw_soon()) */
void list_covers_input(void);
/* a list with covers opened: flash asleep or off woken meanwhile, without
   waiting for it, as its covers are read next */
void list_covers_opened(void);
/* Before each frame, after list_covers_frame(): the covers of the rows it
   draws that aren't kept read now, on the UI's thread, by the file's sectors
   from a disk that's on, a few milliseconds at most, so that the frame draws
   them, as the wheel turns faster than the reader gets the CPU */
struct gui_synclist;
void list_covers_draw_soon(struct gui_synclist *list);
/* the covers made again, or Finder's after an import: those kept may be
   other albums', and the file is made again at Updating Library */
void list_covers_forget(void);

/* For the reader's thread: covers asked for, and reading the next of them, as
   many as one read of the file has; and those asked for let go of, as its
   thread is waited for, or another list shows */
bool list_covers_to_read(void);
void list_covers_read(void);
void list_covers_cancel(void);

/* Updating Library: the file isn't made for the database and its covers as
   they are, with the setting on or off, so that turning it on shows them at
   once; making it, after the table of years and Cover Flow's slides, with its
   progress, until go_on() says to stop, cut short then and made the next
   time; and the file's index of albums taken in with the memory, once the
   table of years is made at boot. Until it's made, the lists show no covers */
bool list_covers_stale(void);
void list_covers_make(void (*progress)(int done, int total), bool (*go_on)(void));
void list_covers_load(void);

#endif /* _LIST_COVERS_H_ */
