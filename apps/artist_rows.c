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

/* The artists' rows: artist_rows.h. Made at Updating Library from every
 * song of music in the database's memory, sorted by artist, then album name,
 * then the song's place in the database, as an artist's albums' list finds
 * them: each name a row of that list, its album the first of the name found
 * among the songs and its date the latest of their albums' (album_years.h),
 * the cover the first such row by year whose album has one in the lists'
 * covers. Into list_covers.dat's neighbour, each tag's artists by their
 * names' seeks, read whole into memory with the covers' and looked up by
 * halves as a row is drawn. */

#include "config.h"
#include <string.h>
#include <stdlib.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "core_alloc.h"
#include "rbpaths.h"
#include "audio.h"
#include "settings.h"
#include "plugin.h"
#include "tagcache.h"
#include "itunesdb.h"
#include "album_years.h"
#include "list_covers.h"
#include "file_write.h"
#include "artist_rows.h"

#if defined(ITUNESDB_HAVE_ART) && defined(HAVE_TC_RAMCACHE)

#define ARTISTS_FILE    ROCKBOX_DIR "/list_artists.dat"
#define ARTISTS_MAGIC   0x52414c52 /* "RLAR" */
#define ARTISTS_VERSION 2 /* 2: live, of music alone */
/* none, in the place of an album's number: the table of years has fewer */
#define NO_ALBUM 0xffff

/* the tags Artists lists, as Group by Album Artist has it, and each one's
   place in the file */
#define TAGS 2
static const int tags[TAGS] = { tag_albumartist, tag_virt_canonicalartist };

/* Of the database, its table of years and its compilations' marks, which the
   rows and their numbers are of; and of its songs of music not deleted, the
   songs the rows count, as a deletion leaves the commit as it was: a
   podcast's episode deleted changes nothing here */
struct file_header {
    uint32_t magic, version;
    int32_t commitid;
    uint32_t entries;       /* tagcache_entry_count() */
    uint32_t live;          /* of them, songs of music not deleted */
    uint32_t albums;        /* album_years_count() */
    uint32_t table;         /* album_years_hash() */
    uint32_t compilations;  /* tagcache_compilation_songs_hash() */
    uint32_t count[TAGS];   /* of each tag's artists, after one another */
};

/* an artist: [0] of all their songs of music, [1] of those off
   compilations, 0 songs for an artist with none such */
struct file_entry {
    int32_t seek;
    uint16_t cover[2];      /* NO_ALBUM for the note */
    uint16_t albums[2];
    uint32_t songs[2];
};

/* In memory: the entries of both tags, one after the other, with what the
   header said they're of */
static int handle;
static bool loaded;
static uint32_t count[TAGS];
static struct file_header loaded_of;
static unsigned generation;

/* a song of music, as collect() takes them */
static void count_live(int idx_id, const int32_t *tag_seek, void *data)
{
    (void)tag_seek;
    if (tagcache_track_kind(idx_id) == 0)
        (*(uint32_t *)data)++;
}

/* the songs of music not deleted, from memory: a few milliseconds at 13000 */
static uint32_t live_songs(void)
{
    uint32_t n = 0;
    tagcache_for_each_track(count_live, &n);
    return n;
}

static void header_now(struct file_header *h)
{
    memset(h, 0, sizeof(*h));
    h->magic = ARTISTS_MAGIC;
    h->version = ARTISTS_VERSION;
    h->commitid = tagcache_get_commitid();
    h->entries = tagcache_entry_count();
    h->albums = album_years_count();
    h->table = album_years_hash();
    /* the songs' marks, as the table of years goes by them: the hash of the
       albums' marks with them is 0 where both mark the same songs, as no
       compilation does */
    h->compilations = tagcache_compilation_songs_hash();
}

/* of the library as it is, but for its songs of music not deleted, which
   only the making and taking in look at */
static bool header_current(const struct file_header *h)
{
    struct file_header now;
    header_now(&now);
    return h->magic == now.magic && h->version == now.version
           && h->commitid == now.commitid && h->entries == now.entries
           && h->albums == now.albums && h->table == now.table
           && h->compilations == now.compilations && album_years_ready();
}

bool artist_rows_loaded(void)
{
    return loaded;
}

bool artist_rows_ready(void)
{
    return loaded && header_current(&loaded_of);
}

bool artist_rows_of(int tag)
{
    const int t = tag == tags[0] ? 0 : tag == tags[1] ? 1 : -1;
    return t >= 0 && count[t] > 0 && artist_rows_ready();
}

unsigned artist_rows_generation(void)
{
    return generation;
}

void artist_rows_free(void)
{
    if (handle > 0)
        handle = core_free(handle);
    loaded = false;
    count[0] = count[1] = 0;
}

bool artist_rows_load(void)
{
    artist_rows_free();
    /* nothing taken while music plays or is paused: its buffer gives way */
    if (audio_status() != 0)
        return false;
    struct file_header h;
    const int fd = open(ARTISTS_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    /* each tag's artists no more than the songs, as a whole file has them */
    const uint32_t most = tagcache_entry_count();
    bool ok = read(fd, &h, sizeof(h)) == sizeof(h) && header_current(&h)
              && h.live == live_songs() && h.count[0] <= most
              && h.count[1] <= most;
    const size_t bytes = ok ? (h.count[0] + h.count[1]) * sizeof(struct file_entry) : 0;
    if (ok && bytes > 0)
    {
        handle = core_alloc(bytes);
        ok = handle > 0;
        if (ok)
        {
            /* still where it is as the disk is read into it */
            core_pin(handle);
            ok = read(fd, core_get_data(handle), bytes) == (ssize_t)bytes;
            core_unpin(handle);
        }
        else
            handle = 0;
    }
    close(fd);
    if (!ok)
    {
        artist_rows_free();
        return false;
    }
    count[0] = h.count[0];
    count[1] = h.count[1];
    loaded_of = h;
    loaded = true;
    generation++;
    return true;
}

bool artist_rows_get(int tag, bool solo, int32_t seek, struct artist_row *row)
{
    const int t = tag == tags[0] ? 0 : tag == tags[1] ? 1 : -1;
    if (t < 0 || !artist_rows_ready() || count[t] == 0)
        return false;
    const struct file_entry *e = (const struct file_entry *)core_get_data(handle)
                                 + (t ? count[0] : 0);
    int lo = 0, hi = count[t];
    while (lo < hi)
    {
        const int mid = (lo + hi) / 2;
        if (e[mid].seek < seek)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= (int)count[t] || e[lo].seek != seek)
        return false;
    const int v = solo ? 1 : 0;
    row->cover = e[lo].cover[v] == NO_ALBUM ? -1 : e[lo].cover[v];
    row->albums = e[lo].albums[v];
    row->songs = e[lo].songs[v];
    return true;
}

bool artist_rows_stale(void)
{
    /* none without the database in memory and its table of years, which it's
       made of */
    const struct tagcache_stat *tc = tagcache_get_stat();
    if (!tc->ready || !tc->ramcache || !album_years_ready())
        return false;
    struct file_header h;
    const int fd = open(ARTISTS_FILE, O_RDONLY);
    const bool made = fd >= 0 && read(fd, &h, sizeof(h)) == sizeof(h)
                      && header_current(&h) && h.live == live_songs();
    if (fd >= 0)
        close(fd);
    return !made;
}

void artist_rows_forget(void)
{
    remove(ARTISTS_FILE);
}

/** Making it, at Updating Library **/

/* a song of music, as a list of an artist's albums finds it: its artist's
   seek in the tag's file, its album's name's, its place in the database, its
   album's number, and whether it's off compilations */
struct song {
    int32_t artist, album, idx;
    uint16_t no;
    uint16_t solo;
};

struct collecting {
    struct song *songs;
    int n, most, tag;
    bool full;
};

/* each song of music (Artists lists no other), from memory held still */
static void collect(int idx_id, const int32_t *tag_seek, void *data)
{
    struct collecting *c = data;
    if (c->full || tagcache_track_kind(idx_id) != 0)
        return;
    if (c->n >= c->most)
    {
        c->full = true;
        return;
    }
    const int no = album_years_number(tag_seek[tag_album],
                                      tag_seek[tag_albumartist]);
    c->songs[c->n++] = (struct song){
        tag_seek[c->tag], tag_seek[tag_album], idx_id,
        no >= 0 && no < NO_ALBUM ? no : NO_ALBUM,
        !tagcache_track_compilation(idx_id),
    };
}

static int cmp_song(const void *a, const void *b)
{
    const struct song *x = a, *y = b;
    if (x->artist != y->artist)
        return x->artist < y->artist ? -1 : 1;
    if (x->album != y->album)
        return x->album < y->album ? -1 : 1;
    return x->idx < y->idx ? -1 : x->idx > y->idx;
}

/* A row of albums by year before another, as an artist's list has them
   (tagtree.c's compare_years()): those with a year by it, those without
   after them; of a year, by date; then by name, the order of the names'
   seeks in their sorted file */
static bool row_before(int32_t date, int32_t album, int32_t date2, int32_t album2)
{
    const int y = album_date_year(date), y2 = album_date_year(date2);
    if (y != y2)
        return y > 0 && (y2 <= 0 || y < y2);
    if (y > 0)
    {
        const int32_t o = album_date_order(date), o2 = album_date_order(date2);
        if (o != o2)
            return o < o2;
    }
    return album < album2;
}

static bool named(int tag, int32_t seek)
{
    char probe[2];
    return tagcache_tag_string(tag, seek, probe, sizeof(probe));
}

static bool has_cover(const uint8_t *bits, int no)
{
    return no >= 0 && (bits[no >> 3] & (1 << (no & 7)));
}

/* An artist's entry from their songs, sorted, from first to end */
static void artist_entry(const struct song *songs, int first, int end,
                         const uint8_t *covers, int tag, struct file_entry *e)
{
    int32_t cover_date[2] = { 0, 0 }, cover_album[2] = { 0, 0 };
    int cover[2] = { -1, -1 };
    memset(e, 0, sizeof(*e));
    e->seek = songs[first].artist;
    for (int i = first; i < end; )
    {
        const int32_t album = songs[i].album;
        int32_t date[2] = { 0, 0 };
        int no[2] = { -1, -1 };
        bool any[2] = { false, false };
        for (; i < end && songs[i].album == album; i++)
        {
            const struct song *s = &songs[i];
            const int32_t d = s->no == NO_ALBUM ? 0 : album_years_date(s->no);
            for (int v = 0; v < 2; v++)
            {
                if (v == 1 && !s->solo)
                    continue;
                e->songs[v]++;
                /* the first song of the name found, as the list finds them */
                if (!any[v])
                    no[v] = s->no == NO_ALBUM ? -1 : s->no;
                any[v] = true;
                if (album_years_date_after(d, date[v]))
                    date[v] = d;
            }
        }
        /* Unknown Album a row of the list too, with no cover */
        const bool name = named(tag_album, album);
        for (int v = 0; v < 2; v++)
        {
            if (!any[v])
                continue;
            e->albums[v]++;
            if (name && has_cover(covers, no[v])
                && (cover[v] < 0
                    || row_before(date[v], album, cover_date[v], cover_album[v])))
            {
                cover[v] = no[v];
                cover_date[v] = date[v];
                cover_album[v] = album;
            }
        }
    }
    /* Unknown Artist's songs are no artist's: the note */
    const bool artist = named(tag, e->seek);
    for (int v = 0; v < 2; v++)
        e->cover[v] = artist && cover[v] >= 0 ? cover[v] : NO_ALBUM;
}

void artist_rows_make(void (*progress)(int done, int total), bool (*go_on)(void))
{
    if (!album_years_ready())
        return;
    struct file_header h;
    header_now(&h);
    h.live = live_songs();
    const int albums = album_years_count();
    size_t room;
    unsigned char *buf = plugin_get_buffer(&room);
    /* the parts of the buffer: the covers' bits, the stretch of entries the
       file is written by, and the songs */
    const size_t bits_size = ALIGN_UP(albums / 8 + 1, 16);
    const size_t out_size = 4096;
    if (!buf || albums >= NO_ALBUM || room < bits_size + out_size + 64)
    {
        /* none, made for it all the same: not again at each boot */
        whole_file_write(ARTISTS_FILE, &h, sizeof(h));
        return;
    }
    unsigned char *base = (unsigned char *)ALIGN_UP((uintptr_t)buf, 16);
    room -= base - buf;
    uint8_t *covers = base;
    unsigned char *out = base + bits_size;
    struct song *songs = (struct song *)(out + out_size);
    const int most = (room - bits_size - out_size) / sizeof(struct song);
    /* which albums have a cover in the lists' covers: none if their file
       isn't of the library as it is, which is made again after, and this
       with it (list_covers_make()) */
    if (!list_covers_which(covers, albums))
        memset(covers, 0, bits_size);

    struct fw w;
    if (!fw_open(&w, ARTISTS_FILE, FW_CACHE, out, out_size))
        return;
    bool ok = fw_put(&w, &h, sizeof(h));
    bool stopped = false;
    cpu_boost(true);
    for (int t = 0; t < TAGS && ok && !stopped; t++)
    {
        struct collecting c = { songs, 0, most, tags[t], false };
        h.count[t] = 0;
        progress(t * 2, TAGS * 2);
        /* without room for every song, none of the tag's: its rows as names */
        if (!tagcache_for_each_track(collect, &c) || c.full)
            continue;
        qsort(songs, c.n, sizeof(*songs), cmp_song);
        progress(t * 2 + 1, TAGS * 2);
        stopped = !go_on();
        for (int i = 0, made = 0; i < c.n && ok && !stopped; made++)
        {
            int end = i + 1;
            while (end < c.n && songs[end].artist == songs[i].artist)
                end++;
            struct file_entry e;
            artist_entry(songs, i, end, covers, tags[t], &e);
            ok = fw_put(&w, &e, sizeof(e));
            h.count[t]++;
            i = end;
            if ((made & 63) == 63)
                stopped = !go_on();
        }
    }
    cpu_boost(false);
    ok = ok && !stopped && fw_seek(&w, 0) && fw_put(&w, &h, sizeof(h));
    if (!fw_close(&w, ok))
        return;
    /* in memory again, as the lists show it, music stopped */
    if (list_covers_taken() && global_settings.artist_covers)
        artist_rows_load();
    else
        artist_rows_free();
}

#else /* !(ITUNESDB_HAVE_ART && HAVE_TC_RAMCACHE) */

bool artist_rows_get(int tag, bool solo, int32_t seek, struct artist_row *row)
{
    (void)tag; (void)solo; (void)seek; (void)row;
    return false;
}
bool artist_rows_ready(void) { return false; }
bool artist_rows_of(int tag) { (void)tag; return false; }
unsigned artist_rows_generation(void) { return 0; }
bool artist_rows_stale(void) { return false; }
void artist_rows_make(void (*progress)(int done, int total), bool (*go_on)(void))
{
    (void)progress; (void)go_on;
}
void artist_rows_forget(void) {}
bool artist_rows_load(void) { return false; }
void artist_rows_free(void) {}
bool artist_rows_loaded(void) { return false; }

#endif
