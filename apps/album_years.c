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

#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "core_alloc.h"
#include "tagcache.h"
#include "album_years.h"

#ifdef HAVE_TC_RAMCACHE

/* An album: the seeks of its name and album artist in their tags' files,
   which are the same in memory and on disk, and the years */
struct album {
    int32_t album, albumartist;
    int16_t year;
    int16_t name_year;  /* the latest of the albums of its name */
};

static int handle;          /* the albums, sorted by name and album artist,
                               and after them the rows' years */
static int count;

/* A list's rows: their albums' names and years, by the names' seeks, in as
   many places as a list of an artist's albums has rows, and more */
struct row {
    int32_t album, year;
};
#define ROW_PLACES 512
#define ROWS_MOST  (ROW_PLACES / 4 * 3)
static int rows_used;
static int made_commitid = -1;
static unsigned generation;

/* those with no year, or with a year that can't be one, as 0 */
static int year_of(int32_t year)
{
    return year >= 1000 && year <= 2999 ? year : 0;
}

/* While they're counted: a table of them by their seeks, of a power of two
   of places, an album in each place used */
struct counting {
    struct album *table;
    unsigned mask;
    int used, most;
    bool full;
};

static unsigned place_of(int32_t album, int32_t albumartist, unsigned mask)
{
    uint32_t h = (uint32_t)album * 2654435761u ^ (uint32_t)albumartist * 40503u;
    return (h ^ (h >> 15)) & mask;
}

/* a song: its album's year, the latest so far. With the database's memory
   held still, as it goes through its songs: nothing waits here */
static void count_song(int idx_id, const int32_t *tag_seek, void *data)
{
    (void)idx_id;
    struct counting *c = data;
    if (c->full)
        return;
    const int32_t album = tag_seek[tag_album];
    const int32_t albumartist = tag_seek[tag_albumartist];
    const int year = year_of(tag_seek[tag_year]);
    unsigned i = place_of(album, albumartist, c->mask);
    while (c->table[i].album != -1)
    {
        struct album *a = &c->table[i];
        if (a->album == album && a->albumartist == albumartist)
        {
            if (year > a->year)
                a->year = year;
            return;
        }
        i = (i + 1) & c->mask;
    }
    /* three quarters full: counted again in a larger one */
    if (c->used >= c->most)
    {
        c->full = true;
        return;
    }
    c->table[i] = (struct album){ album, albumartist, year, 0 };
    c->used++;
}

static int cmp_album(const void *a, const void *b)
{
    const struct album *x = a, *y = b;
    if (x->album != y->album)
        return x->album < y->album ? -1 : 1;
    if (x->albumartist != y->albumartist)
        return x->albumartist < y->albumartist ? -1 : 1;
    return 0;
}

static bool make(void)
{
    if (handle > 0)
        handle = core_free(handle);
    count = 0;
    made_commitid = -1;
    if (!tagcache_finds_entries())
        return false;
    /* about one album for each eight songs: room for twice that many to
       begin with, four times the places */
    unsigned places = 64;
    while (places < (unsigned)tagcache_entry_count() / 2)
        places *= 2;
    for (;;)
    {
        handle = core_alloc(places * sizeof(struct album)
                            + ROW_PLACES * sizeof(struct row));
        if (handle <= 0)
        {
            handle = 0;
            return false;
        }
        struct album *table = core_get_data_pinned(handle);
        for (unsigned i = 0; i < places; i++)
            table[i].album = -1;
        struct counting c = { table, places - 1, 0, places / 4 * 3, false };
        const bool ok = tagcache_for_each_track(count_song, &c);
        if (ok && !c.full)
        {
            /* together at the start, sorted, and the room after them let go */
            int n = 0;
            for (unsigned i = 0; i < places; i++)
            {
                if (table[i].album != -1)
                    table[n++] = table[i];
            }
            qsort(table, n, sizeof(*table), cmp_album);
            for (int i = 0; i < n; )
            {
                int end = i, latest = 0;
                for (; end < n && table[end].album == table[i].album; end++)
                    latest = MAX(latest, table[end].year);
                for (; i < end; i++)
                    table[i].name_year = latest;
            }
            core_put_data_pinned(table);
            if (n > 0)
                core_shrink(handle, core_get_data(handle), n * sizeof(*table)
                                    + ROW_PLACES * sizeof(struct row));
            else
                handle = core_free(handle);
            rows_used = ROWS_MOST; /* none begun */
            count = n;
            made_commitid = tagcache_get_commitid();
            generation++;
            return true;
        }
        core_put_data_pinned(table);
        handle = core_free(handle);
        if (!ok)
            return false;
        places *= 2;
    }
}

/* at full speed: 142 ms for 1688 albums of 13180 songs on an iPod at boot
   without it */
bool album_years_make(void)
{
    cpu_boost(true);
    const bool made = make();
    cpu_boost(false);
    return made;
}

bool album_years_ready(void)
{
    return made_commitid >= 0 && made_commitid == tagcache_get_commitid()
           && tagcache_finds_entries();
}

/* the first of the albums of this name, or -1 */
static int first_of(const struct album *albums, int32_t album)
{
    int lo = 0, hi = count;
    while (lo < hi)
    {
        const int mid = (lo + hi) / 2;
        if (albums[mid].album < album)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < count && albums[lo].album == album ? lo : -1;
}

static int year_in(const struct album *albums, int32_t album,
                   int32_t albumartist)
{
    int i = first_of(albums, album);
    if (i < 0)
        return 0;
    for (; i < count && albums[i].album == album; i++)
    {
        if (albums[i].albumartist == albumartist)
            return albums[i].year;
    }
    return 0;
}

int album_year(int32_t album, int32_t albumartist)
{
    if (!album_years_ready() || count == 0)
        return 0;
    return year_in(core_get_data(handle), album, albumartist);
}

static struct row *rows_of(struct album *albums)
{
    return (struct row *)(albums + count);
}

void album_years_rows_begin(void)
{
    if (!album_years_ready() || count == 0)
    {
        rows_used = ROWS_MOST;
        return;
    }
    struct row *rows = rows_of(core_get_data(handle));
    for (int i = 0; i < ROW_PLACES; i++)
        rows[i].album = -1;
    rows_used = 0;
}

/* with the database's memory held still, as the search goes through its
   songs: nothing waits here */
void album_years_match(const int32_t *tag_seek, void *data)
{
    (void)data;
    if (rows_used >= ROWS_MOST) /* none begun, or the places full */
        return;
    struct album *albums = core_get_data(handle);
    const int32_t album = tag_seek[tag_album];
    const int year = year_in(albums, album, tag_seek[tag_albumartist]);
    struct row *rows = rows_of(albums);
    unsigned i = place_of(album, 0, ROW_PLACES - 1);
    while (rows[i].album != -1 && rows[i].album != album)
        i = (i + 1) & (ROW_PLACES - 1);
    if (rows[i].album == -1)
    {
        rows[i] = (struct row){ album, 0 };
        rows_used++;
    }
    if (year > rows[i].year)
        rows[i].year = year;
}

int album_years_row(int32_t album)
{
    if (!album_years_ready() || count == 0)
        return 0;
    struct album *albums = core_get_data(handle);
    if (rows_used < ROWS_MOST)
    {
        const struct row *rows = rows_of(albums);
        unsigned i = place_of(album, 0, ROW_PLACES - 1);
        while (rows[i].album != -1)
        {
            if (rows[i].album == album)
                return rows[i].year;
            i = (i + 1) & (ROW_PLACES - 1);
        }
    }
    const int first = first_of(albums, album);
    return first < 0 ? 0 : albums[first].name_year;
}

int album_name_year(int32_t album)
{
    if (!album_years_ready() || count == 0)
        return 0;
    const struct album *albums = core_get_data(handle);
    const int i = first_of(albums, album);
    return i < 0 ? 0 : albums[i].name_year;
}

unsigned album_years_generation(void)
{
    return generation;
}

#else /* !HAVE_TC_RAMCACHE */

bool album_years_make(void) { return false; }
bool album_years_ready(void) { return false; }
int album_year(int32_t album, int32_t albumartist)
{
    (void)album; (void)albumartist;
    return 0;
}
int album_name_year(int32_t album) { (void)album; return 0; }
unsigned album_years_generation(void) { return 0; }
void album_years_rows_begin(void) {}
void album_years_match(const int32_t *tag_seek, void *data)
{
    (void)tag_seek; (void)data;
}
int album_years_row(int32_t album) { (void)album; return 0; }

#endif /* HAVE_TC_RAMCACHE */
