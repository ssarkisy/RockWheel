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
#ifndef _ALBUM_HEADER_H_
#define _ALBUM_HEADER_H_

#include <stdbool.h>
#include <stdint.h>
#include "tree.h"
#include "screen_access.h"
#include "gui/list.h"

/* The header over an album's songs, as iOS's: its cover, its name in a line
   or two, its album artist, its year and how many songs, in the first rows of
   the list, which scroll away with it. For the list loaded last, if
   tagtree_album() says it's an album's, shown in this list: the rows it
   takes, 0 for none. Its texts from the database in memory, never the disk.
   Its cover asked of a thread of its own, which reads it meanwhile, and
   waited for a moment only if that costs no spin-up: from the playback's
   buffer as the album plays, or with the disk on. The rows below it, the
   songs' and their groups', tell its texts whether they have a scrollbar
   beside them. */
int album_header_open(struct tree_context *c, struct gui_synclist *list,
                      int rows_after);
/* The same over a playlist's songs: its name and "24 songs · 1 h 32 min",
   beside a collage of the covers of its first albums with one, in their
   order (library_playlists.c chose them as its index was written, at
   Updating Library or as its songs changed): of the albums of these songs,
   -1 past them, the note for none; seconds 0 leaves the length out. Nothing
   worked out as it opens: the counts are its index's, and the covers are
   read as an album's. The rows it takes in this list, 0 for none, as with
   no songs */
#define ALBUM_HEADER_COLLAGE 4
int album_header_open_playlist(struct gui_synclist *list, const char *name,
                               int songs, unsigned long seconds,
                               const int32_t *collage, int rows_after);
/* gui_synclist's block: the header with its top at y of the list's viewport */
void album_header_draw(struct screen *display, int y, void *data);
/* Its cover is still being read, the note in its place meanwhile; and it
   came, read or found to be none, to be drawn again: true once */
bool album_header_cover_reading(void);
bool album_header_cover_came(void);
/* the covers made again, or Finder's after an import: the one kept may be
   another's now */
void album_header_forget_cover(void);
/* the cover's read ended, if one is on, and its thread with it: as a plugin
   starts, which may want every thread slot there is; the covers of lists'
   rows asked for (list_covers.h) left unread */
void album_header_reader_wait(void);
/* The reader's thread reads the covers of lists' rows too: started, if it
   isn't on, false if it couldn't be. And where an album's cover is, by one
   of its songs' number in the database, for a read on that thread: 1 found,
   0 none, -1 not known, as with the database busy */
struct itunesdb_art;
bool album_header_reader_start(void);
/* the same for the menus' cover, the thread that moves it left on: its
   caller has seen a slot free for each */
bool album_header_reader_start_beside(void);
int album_header_find_art(int32_t song, struct itunesdb_art *art);
/* whether the header would show a cover for the album of this song, as
   found above, by the covers' indexes in memory, no picture read: 1, 0 none,
   -1 not known. On any thread, the lookup's buffers held by a lock; it may
   wait as the lookup above does */
int album_header_song_cover(int32_t song);

#endif /* _ALBUM_HEADER_H_ */
