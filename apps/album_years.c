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

int32_t album_date_order(int32_t date)
{
    if (date <= 0)
        return 0;
    if (date < ALBUM_DATE_MIN)
        return date * 10000 + 1332;
    const int month = date / 100 % 100, day = date % 100;
    return date - month * 100 - day + (month ? month : 13) * 100 + (day ? day : 32);
}

#ifdef HAVE_TC_RAMCACHE

/* An album: the seeks of its name and album artist in their tags' files,
   which are the same in memory and on disk, its date, and one of its songs,
   whose cover is the album's */
struct album {
    int32_t album, albumartist;
    int32_t date;       /* date_of()'s value, 0 for none */
    int32_t first;      /* the song's number in the database */
};

static int handle;          /* the albums, sorted by name and album artist,
                               and after them the rows' dates */
static int count;

/* A list's rows: their albums' names and dates, by the names' seeks, in as
   many places as a list of an artist's albums has rows, and more; and the
   first album of the name found among the songs, of its album artist */
struct row {
    int32_t album, date, no;
};
#define ROW_PLACES 512
#define ROWS_MOST  (ROW_PLACES / 4 * 3)
static int rows_used;
static int made_commitid = -1;
static unsigned generation;
/* of the albums' seeks as numbered, for what's kept by their numbers to
   know it's of this table */
static uint32_t table_hash;

/* those with no year, or with a year that can't be one, as 0 */
static int year_of(int32_t year)
{
    return year >= 1000 && year <= 2999 ? year : 0;
}

/* A date as the table has it: of a year it counts, its month and its day
   only if they can be, else the year alone; 0 for none */
static int32_t date_of(int32_t value)
{
    const int year = year_of(album_date_year(value));
    if (!year || value < ALBUM_DATE_MIN)
        return year;
    const int month = value / 100 % 100, day = value % 100;
    if (month < 1 || month > 12 || day > 31)
        return year;
    return year * 10000 + month * 100 + day;
}

/* how much of a date there is: 2 with its day, 1 with its month alone, 0 for
   a year alone */
static int date_detail(int32_t date)
{
    return date < ALBUM_DATE_MIN ? 0 : date % 100 ? 2 : 1;
}

/* An album's date from one of its songs' after another's, those it had so
   far, as a name's and a row's from their albums': its year the latest of
   theirs, as Music has it, and of those of that year the latest date with a
   day, or else with a month, so that a song added without its day, as a
   bonus track, leaves the album its day */
static bool date_after(int32_t date, int32_t had)
{
    const int year = album_date_year(date), year_had = album_date_year(had);
    if (year != year_had)
        return year > year_had;
    if (date_detail(date) != date_detail(had))
        return date_detail(date) > date_detail(had);
    return date > had;
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

/* a song: its album's date, as date_after() has it so far. With the
   database's memory held still, as it goes through its songs: nothing waits
   here */
static void count_song(int idx_id, const int32_t *tag_seek, void *data)
{
    struct counting *c = data;
    if (c->full)
        return;
    const int32_t album = tag_seek[tag_album];
    const int32_t albumartist = tag_seek[tag_albumartist];
    const int32_t date = date_of(tag_seek[tag_date] ? tag_seek[tag_date]
                                                     : year_of(tag_seek[tag_year]));
    unsigned i = place_of(album, albumartist, c->mask);
    while (c->table[i].album != -1)
    {
        struct album *a = &c->table[i];
        if (a->album == album && a->albumartist == albumartist)
        {
            if (date_after(date, a->date))
                a->date = date;
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
    c->table[i] = (struct album){ album, albumartist, date, idx_id };
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
            table_hash = 2166136261u;
            for (int i = 0; i < n; i++)
                table_hash = ((table_hash ^ (uint32_t)table[i].album) * 16777619u
                              ^ (uint32_t)table[i].albumartist) * 16777619u;
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

/* the album of this name and album artist, or -1 */
static int album_in(const struct album *albums, int32_t album,
                    int32_t albumartist)
{
    int i = first_of(albums, album);
    if (i < 0)
        return -1;
    for (; i < count && albums[i].album == album; i++)
    {
        if (albums[i].albumartist == albumartist)
            return i;
    }
    return -1;
}

/* the date of the albums of a name (date_after()), from the first of them */
static int32_t name_date(const struct album *albums, int first)
{
    int32_t date = 0;
    for (int i = first; i < count && albums[i].album == albums[first].album; i++)
    {
        if (date_after(albums[i].date, date))
            date = albums[i].date;
    }
    return date;
}

int32_t album_date(int32_t album, int32_t albumartist)
{
    if (!album_years_ready() || count == 0)
        return 0;
    const struct album *albums = core_get_data(handle);
    const int i = album_in(albums, album, albumartist);
    return i < 0 ? 0 : albums[i].date;
}

int album_year(int32_t album, int32_t albumartist)
{
    return album_date_year(album_date(album, albumartist));
}

int album_years_find(int32_t album, int *n)
{
    *n = 0;
    if (!album_years_ready() || count == 0)
        return -1;
    const struct album *albums = core_get_data(handle);
    const int first = first_of(albums, album);
    if (first < 0)
        return -1;
    int end = first;
    while (end < count && albums[end].album == album)
        end++;
    *n = end - first;
    return first;
}

int album_years_number(int32_t album, int32_t albumartist)
{
    if (!album_years_ready() || count == 0)
        return -1;
    return album_in(core_get_data(handle), album, albumartist);
}

/* an album by its number, NULL if there's none of it in the table as it is */
static const struct album *album_no(int no)
{
    if (no < 0 || no >= count || !album_years_ready())
        return NULL;
    return (const struct album *)core_get_data(handle) + no;
}

int32_t album_years_album(int no)
{
    const struct album *a = album_no(no);
    return a ? a->album : -1;
}

int32_t album_years_artist(int no)
{
    const struct album *a = album_no(no);
    return a ? a->albumartist : -1;
}

int32_t album_years_date(int no)
{
    const struct album *a = album_no(no);
    return a ? a->date : 0;
}

int album_years_year(int no)
{
    return album_date_year(album_years_date(no));
}

int32_t album_years_name_date_of(int no)
{
    const struct album *a = album_no(no);
    if (!a)
        return 0;
    const struct album *albums = core_get_data(handle);
    while (no > 0 && albums[no - 1].album == a->album)
        no--;
    return name_date(albums, no);
}

int album_years_name_year_of(int no)
{
    return album_date_year(album_years_name_date_of(no));
}

int32_t album_years_first_track(int no)
{
    const struct album *a = album_no(no);
    return a ? a->first : -1;
}

int album_years_count(void)
{
    return album_years_ready() ? count : 0;
}

uint32_t album_years_hash(void)
{
    return album_years_ready() ? table_hash : 0;
}

static struct row *rows_of(const struct album *albums)
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
    const int no = album_in(albums, album, tag_seek[tag_albumartist]);
    const int32_t date = no < 0 ? 0 : albums[no].date;
    struct row *rows = rows_of(albums);
    unsigned i = place_of(album, 0, ROW_PLACES - 1);
    while (rows[i].album != -1 && rows[i].album != album)
        i = (i + 1) & (ROW_PLACES - 1);
    if (rows[i].album == -1)
    {
        rows[i] = (struct row){ album, 0, no };
        rows_used++;
    }
    if (date_after(date, rows[i].date))
        rows[i].date = date;
}

/* the row of this name the search was told of, or NULL */
static const struct row *row_of(const struct album *albums, int32_t album)
{
    if (rows_used >= ROWS_MOST)
        return NULL;
    const struct row *rows = rows_of(albums);
    unsigned i = place_of(album, 0, ROW_PLACES - 1);
    while (rows[i].album != -1)
    {
        if (rows[i].album == album)
            return &rows[i];
        i = (i + 1) & (ROW_PLACES - 1);
    }
    return NULL;
}

int32_t album_years_row_date(int32_t album)
{
    if (!album_years_ready() || count == 0)
        return 0;
    const struct album *albums = core_get_data(handle);
    const struct row *row = row_of(albums, album);
    if (row)
        return row->date;
    const int first = first_of(albums, album);
    return first < 0 ? 0 : name_date(albums, first);
}

int album_years_row(int32_t album)
{
    return album_date_year(album_years_row_date(album));
}

int album_years_row_album(int32_t album)
{
    if (!album_years_ready() || count == 0)
        return -1;
    const struct album *albums = core_get_data(handle);
    const struct row *row = row_of(albums, album);
    return row ? row->no : first_of(albums, album);
}

int album_name_year(int32_t album)
{
    if (!album_years_ready() || count == 0)
        return 0;
    const struct album *albums = core_get_data(handle);
    const int i = first_of(albums, album);
    return i < 0 ? 0 : album_date_year(name_date(albums, i));
}

unsigned album_years_generation(void)
{
    return generation;
}

#else /* !HAVE_TC_RAMCACHE */

bool album_years_make(void) { return false; }
bool album_years_ready(void) { return false; }
int32_t album_date(int32_t album, int32_t albumartist)
{
    (void)album; (void)albumartist;
    return 0;
}
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
int32_t album_years_row_date(int32_t album) { (void)album; return 0; }
int album_years_row_album(int32_t album) { (void)album; return -1; }
int album_years_find(int32_t album, int *n)
{
    (void)album;
    *n = 0;
    return -1;
}
int album_years_number(int32_t album, int32_t albumartist)
{
    (void)album; (void)albumartist;
    return -1;
}
int32_t album_years_album(int no) { (void)no; return -1; }
int32_t album_years_artist(int no) { (void)no; return -1; }
int32_t album_years_date(int no) { (void)no; return 0; }
int album_years_year(int no) { (void)no; return 0; }
int32_t album_years_name_date_of(int no) { (void)no; return 0; }
int album_years_name_year_of(int no) { (void)no; return 0; }
int32_t album_years_first_track(int no) { (void)no; return -1; }
int album_years_count(void) { return 0; }
uint32_t album_years_hash(void) { return 0; }

#endif /* HAVE_TC_RAMCACHE */
