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

#ifndef _LIBRARY_PLAYLISTS_H_
#define _LIBRARY_PLAYLISTS_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* The playlists of the library, those the import writes, as Songs is: an
   index of each one's songs, by their entries in the database, made at
   Updating Library, their titles from memory as the list opens, no line of
   the file read as it scrolls. In the order the Mac shows them in, as a
   stock iPod has them, the file written in it too, which plays in it. */

#define LP_NO_LINE 0xffffffffu
/* songs in one, as many as the Songs of a full iPod */
#define LP_MAX     32000

struct lp_song {
    uint32_t idx;   /* its entry in the database */
    uint32_t line;  /* its song in the file, LP_NO_LINE if it isn't there yet */
};

struct lp_state {
    uint32_t lines; /* of songs in its file */
    uint32_t unmatched; /* of those, the database hasn't: kept but not shown */
    uint32_t sort;  /* the column it's shown by, as ITUNESDB_SORT_* */
    bool pending;   /* its file is still to be written from its index: other
                       songs, or in another order */
};

/* A playlist's index, if it's of the database as it is: its songs, as many as
   max, and its state; -1 if there's none such. Without songs, how many it
   has. */
int lp_index_read(const char *file, struct lp_song *songs, int max,
                  struct lp_state *state);
/* its pending worked out from them and the lines of its file */
bool lp_index_write(const char *file, const struct lp_song *songs, int count,
                    struct lp_state *state);
/* from its file, the songs of its lines that the database has, its lines
   and the column it's sorted by, in a buffer of so many bytes for reading
   it; -1 if it can't be read, or the database isn't in memory to find them
   in */
int lp_index_build(const char *file, struct lp_song *songs, int max,
                   char *buf, size_t size, struct lp_state *state);
/* in the order of that column, the songs alike as they were */
void lp_sort(struct lp_song *songs, int count, uint32_t sort);
/* whether its file has these songs, in this order, and no others but those
   the database hasn't */
bool lp_in_file_order(const struct lp_song *songs, int count,
                      const struct lp_state *state);
/* its file written from them, as they're listed, and their lines in it */
bool lp_write_file(const char *file, struct lp_song *songs, int count,
                   struct lp_state *state);
/* the import's, as it writes one again: its index no longer of it */
void lp_index_forget(const char *file);
/* before its file is read for its songs on a screen of the user's: written
   from its index if that's ahead of it */
void lp_sync(const char *file);
/* the same file, whether the drive is named in its path or not */
bool lp_same_file(const char *a, const char *b);

/* each of the library's playlists without an index of the database as it is,
   one made, and its file in the order shown: before the menu, on Updating
   Library, for as long as go_on says */
void lp_index_all(bool (*go_on)(void));

/* one of the import's, with the database in memory to show it from */
bool lp_is_library(const char *file);
/* its screen: a playlist viewer's result */
int lp_view(const char *file, int *selection);
/* the same, at the song playing, as the current playlist: if the music is
   from one of the library's as it is, with no songs put in nor taken out;
   -1 if not */
int lp_view_current(void);

#endif /* _LIBRARY_PLAYLISTS_H_ */
