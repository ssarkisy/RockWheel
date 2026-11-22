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
#ifndef _ARTIST_ROWS_H_
#define _ARTIST_ROWS_H_

#include <stdbool.h>
#include <stdint.h>

/* Music > Artists' rows with a cover, as Settings > Appearance > Artist
   Covers has them, a genre's artists too: each artist's cover the one of
   the first of their albums by year that has one, the album their albums'
   list opens on, and "5 albums · 61 songs" under the name, as that list
   counts them. A table of them made at Updating Library, after the table
   of years and the lists' covers (list_covers.dat), whose albums it names
   by their numbers: of the albums' artists and of the songs' own, as Group
   by Album Artist lists them, each with all their songs of music and with
   those off compilations, as Artists has them while Music has
   Compilations. Taken into memory with the covers' (list_covers_apply()),
   music stopped; nothing is read from the disk as a list scrolls. */

struct artist_row {
    int cover;      /* its album's number in the table of years, -1: the note */
    int albums;     /* the rows of its albums' list, Unknown Album's too */
    long songs;
};

/* An artist's row by its name's seek in tag's file, tag_albumartist or
   tag_virt_canonicalartist, of all their songs of music or of those off
   compilations (solo): false without the table in memory, of the library
   as it is, or without the artist in it */
bool artist_rows_get(int tag, bool solo, int32_t seek, struct artist_row *row);
/* the table is in memory and of the library as it is; and has the artists
   of tag, which it lacks when their songs had no room as it was made */
bool artist_rows_ready(void);
bool artist_rows_of(int tag);
/* how many times it was taken in, which the lists made of it go by */
unsigned artist_rows_generation(void);

/* Updating Library: the file isn't of the database, its table of years and
   its compilations as they are; making it, from the database in memory, in
   the plugins' buffer, until go_on() says to stop; and forgotten, as the
   lists' covers are made again, which it chooses among */
bool artist_rows_stale(void);
void artist_rows_make(void (*progress)(int done, int total), bool (*go_on)(void));
void artist_rows_forget(void);

/* Its memory, by list_covers.c as the covers' is: taken in from the file,
   music stopped, true if it is now; and let go of */
bool artist_rows_load(void);
void artist_rows_free(void);
bool artist_rows_loaded(void);

#endif /* _ARTIST_ROWS_H_ */
