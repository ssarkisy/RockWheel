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

#ifndef _ALBUM_YEARS_H_
#define _ALBUM_YEARS_H_

#include <stdbool.h>
#include <stdint.h>

/* The date of each album of the library: an album is its name and its album
   artist, as Cover Flow has them, its year the latest among its songs, of
   those that have one, and its date the latest of its songs of that year
   with a day, or else with a month (tag_date). From the database in memory,
   by the seeks of the names, which its searches give: made at boot and after
   an import or a check that changed the library, never while music plays. */

/* Dates as the table has them: yyyymmdd, yyyymm00 for a month alone, or
   below this the year alone */
#define ALBUM_DATE_MIN 10000
static inline int album_date_year(int32_t date)
{
    return date >= ALBUM_DATE_MIN ? date / 10000 : date;
}

/* made for the library as it is now, the database in memory; false without
   it, or if there was no room */
bool album_years_make(void);
/* made, and for the library as it is */
bool album_years_ready(void);
/* an album's year, by its name's and album artist's seeks; 0 if none */
int album_year(int32_t album, int32_t albumartist);
/* The order of the albums' dates, by year, as Music has releases without a
   day after those with one: those of a year with their day first, then
   those with their month alone, after those of that month, then those of the
   year alone. 0 for no date, which the lists put after them all. */
int32_t album_date_order(int32_t date);
/* the latest year among the albums of this name, of any album artist */
int album_name_year(int32_t album);
/* how many times it was made, which the lists kept of it go by */
unsigned album_years_generation(void);

/* The albums by their numbers in the table, sorted by name and then by album
   artist as the tags' files are: an album's number lasts as long as the
   table's generation. The first album of a name, -1 for none, and in *n how
   many of the name there are, each of another album artist, one after
   another */
int album_years_find(int32_t album, int *n);
/* of an album by its number: its name's and its album artist's seeks, -1 for
   none; its date and its year, 0 for none; one of its songs, by its number
   in the database, -1 for none */
int32_t album_years_album(int no);
int32_t album_years_artist(int no);
int32_t album_years_date(int no);
int album_years_year(int no);
/* and the date and the year of the albums of its name, as their songs' make
   an album's */
int32_t album_years_name_date_of(int no);
int album_years_name_year_of(int no);
int32_t album_years_first_track(int no);
/* how many albums the table has, 0 when it isn't ready; and a hash of their
   seeks in their order, by which what's kept by their numbers is known to be
   of this table, 0 when it isn't ready */
int album_years_count(void);
uint32_t album_years_hash(void);

/* The dates and years of a list's rows of albums among some songs only, as
   an artist's: a row's is the latest, by date_after(), of the albums its
   songs are on. Begun before the rows' search, which tells
   album_years_match() of each song it finds (tagcache_search_set_match); a
   row it wasn't told of gets its name's date. */
void album_years_rows_begin(void);
void album_years_match(const int32_t *tag_seek, void *data);
int32_t album_years_row_date(int32_t album);
int album_years_row(int32_t album);
/* and the number of the row's album, the first of the name found among the
   songs; of the name's first album for a row it wasn't told of; -1 for none */
int album_years_row_album(int32_t album);

#endif /* _ALBUM_YEARS_H_ */
