/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Sergei Sarkisian
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

/* The iTunesDB that Finder writes when it syncs the iPod, read after each
   sync like the stock firmware does: its playlists become .m3u8 files, and
   its ratings and play counts go into the database. The file is too big to
   load, so it is read a record at a time. The layout is libgpod's. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "core_alloc.h"
#include "rbunicode.h"
#include "string-extra.h"
#include "pathfuncs.h"
#include "settings.h"
#include "misc.h"
#include "rbpaths.h"
#include "tagcache.h"
#include "logf.h"
#include "lcd.h"
#include "metadata.h"
#include "fixedpoint.h"
#include "itunesdb.h"

#ifdef HAVE_DIRCACHE
#include "dircache.h"
#endif

/* core_alloc(), and once more after the dircache is built if that fails:
   building it at boot takes all the free memory for a while, and an import
   then could read nothing of iTunesDB */
static int db_alloc(size_t size)
{
    int handle = core_alloc(size);
#ifdef HAVE_DIRCACHE
    if (handle <= 0)
    {
        dircache_wait();
        handle = core_alloc(size);
    }
#endif
    return handle;
}


/* what the last import was made from, and the tracks in iTunesDB order with
   the counts they had then, which Play Counts entries are made against */
#define STATE_FILE      ROCKBOX_DIR "/itunesdb.dat"
#define STATE_MAGIC     0x54494252 /* "RBIT" */
#define STATE_VERSION   1
/* the playlists made from iTunesDB, removed again on the next import */
#define PLAYLISTS_FILE  ROCKBOX_DIR "/itunesdb_playlists.txt"

/* the folder where Finder keeps the music */
#define MUSIC_DIR       "/iPod_Control/Music"

/* media types, which a track can have more than one of */
#define MEDIA_PODCAST   0x04
#define MEDIA_AUDIOBOOK 0x08

/* Records start anywhere in a sector. Read from where the sector starts
   into an aligned buffer, the storage driver takes it all in one go;
   otherwise it goes a sector at a time through a buffer of its own */
#define READ_ALIGN      2048  /* the largest sector there is */
#define CACHE_SIZE      65536 /* a multiple of READ_ALIGN */
#define POOL_PER_TRACK  64 /* bytes of path; Finder's take about 35 */
#define WRITE_BUF_SIZE  32768
/* more than any file of ours or Finder's should count: a damaged count would
   wrap around when multiplied by the size of an entry, on this 32-bit CPU */
#define COUNT_MAX       (1 << 20)

struct state_header {
    uint32_t magic;
    uint32_t version;
    uint32_t db_size;
    uint32_t db_mtime;
    uint32_t count;
};

struct state_entry {
    uint64_t hash;
    uint32_t playcount;
    uint8_t rating;  /* 0..100, 20 a star */
    uint8_t media;   /* the media type, 0xff when it doesn't fit */
    uint8_t flags;   /* ITUNESDB_* */
    uint8_t unused;
};

struct track {
    uint32_t id;     /* what playlists refer to */
    uint32_t path;   /* offset in the path pool, 0 for none */
    uint32_t art;    /* the ArtworkDB image id of its cover, 0 for none */
    uint32_t album;  /* its album and the album's artist hashed, 0 for none */
    uint32_t pos;    /* of its mhit in iTunesDB */
    uint32_t sound_check; /* Finder's, 0 for none */
    struct state_entry st;
};

static struct {
    int fd;
    unsigned char *cache;
    off_t cache_pos;
    int cache_len;
    struct track *tracks;
    int count;
    uint32_t *by_id;   /* indexes of tracks[], by id */
    uint32_t *by_hash; /* and by path hash */
    char *pool;
    size_t pool_size, pool_used;
    bool complete;     /* every track listed was read */
} db;
static uint32_t db_size, db_mtime; /* of the iTunesDB read */

static uint32_t le32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static const unsigned char *db_get(off_t pos, size_t len)
{
    if (len > CACHE_SIZE - READ_ALIGN || pos < 0 || pos > 0x7fffffff - (off_t)len)
        return NULL;
    if (pos < db.cache_pos || pos + (off_t)len > db.cache_pos + db.cache_len)
    {
        off_t start = pos & ~(off_t)(READ_ALIGN - 1);
        if (lseek(db.fd, start, SEEK_SET) != start)
            return NULL;
        db.cache_pos = start;
        db.cache_len = read(db.fd, db.cache, CACHE_SIZE);
        if (db.cache_len < (int)(pos - start + len))
        {
            db.cache_len = 0;
            return NULL;
        }
    }
    return db.cache + (pos - db.cache_pos);
}

/* the record with this tag at pos, at least len bytes of it */
static const unsigned char *db_record(off_t pos, const char *tag, size_t len)
{
    const unsigned char *p = db_get(pos, len);
    return (p && !memcmp(p, tag, 4)) ? p : NULL;
}

/* the string of the mhod at pos as UTF-8. iTunesDB strings are UTF-16 */
static bool mhod_string(off_t pos, char *buf, size_t size)
{
    const unsigned char *p = db_record(pos, "mhod", 40);
    if (!p)
        return false;
    size_t units = MIN(le32(p + 28) / 2, (size - 1) / 3);
    p = db_get(pos + 40, units * 2);
    if (!p)
        return false;
    /* not half of a pair: the decoder would take both, 4 bytes for one */
    if (units > 0 && p[units * 2 - 1] >= 0xd8 && p[units * 2 - 1] < 0xe0)
        units--;
    *utf16LEdecode(p, (unsigned char *)buf, units) = '\0';
    return true;
}

/* the string of the mhod at pos hashed as it is, 0 for an empty one */
static uint32_t mhod_hash(off_t pos)
{
    const unsigned char *p = db_record(pos, "mhod", 40);
    if (!p)
        return 0;
    size_t len = MIN(le32(p + 28), 1024);
    p = len ? db_get(pos + 40, len) : NULL;
    if (!p)
        return 0;
    /* FNV-1a */
    uint32_t hash = 0x811c9dc5;
    for (size_t i = 0; i < len; i++)
        hash = (hash ^ p[i]) * 0x01000193;
    return hash;
}

/* the path without its volume: Rockbox has "/<HDD0>/iPod_Control/..." where
   iTunesDB has "/iPod_Control/..." */
static const char *strip_volume(const char *path)
{
#ifdef HAVE_MULTIVOLUME
    path_strip_volume(path, &path, false);
#endif
    return path;
}

/* in Finder's folder, whose files iTunesDB lists */
bool itunesdb_synced_path(const char *path)
{
    return !strncasecmp(strip_volume(path), MUSIC_DIR "/", sizeof(MUSIC_DIR));
}

uint64_t itunesdb_path_hash(const char *path)
{
    /* FNV-1a, ignoring ASCII case like FAT does */
    uint64_t hash = 0xcbf29ce484222325ull;
    path = strip_volume(path);
    for (const unsigned char *p = (const unsigned char *)path; *p; p++)
    {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        hash = (hash ^ c) * 0x100000001b3ull;
    }
    return hash;
}

/* the size and time of iTunesDB, which change with every sync */
static bool db_stamp(uint32_t *size, uint32_t *mtime)
{
    DIR *dir = opendir(ITUNESDB_DIR);
    if (!dir)
        return false;
    bool found = false;
    struct dirent *entry;
    while (!found && (entry = readdir(dir)))
    {
        if (strcasecmp(entry->d_name, "iTunesDB"))
            continue;
        struct dirinfo info = dir_get_info(dir, entry);
        *size = info.size;
        *mtime = info.mtime;
        found = true;
    }
    closedir(dir);
    return found;
}

bool itunesdb_sync_date(struct tm *tm)
{
    uint32_t size, mtime;
    if (!db_stamp(&size, &mtime) || mtime == 0)
        return false;
    const time_t when = mtime;
    gmtime_r(&when, tm);
    return true;
}

static bool read_state_header(struct state_header *hdr)
{
    int fd = open(STATE_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    bool ok = read(fd, hdr, sizeof(*hdr)) == sizeof(*hdr)
        && hdr->magic == STATE_MAGIC && hdr->version == STATE_VERSION
        && hdr->count <= COUNT_MAX;
    close(fd);
    return ok;
}

bool itunesdb_changed(void)
{
    uint32_t size, mtime;
    struct state_header hdr;
    if (!db_stamp(&size, &mtime))
        return false;
    return !read_state_header(&hdr) || hdr.db_size != size
        || hdr.db_mtime != mtime;
}

/* The name given to the iPod in Finder is that of the first playlist, the hidden
   one with the whole library. It's near the middle of a file of 30 MB, reached
   here by a few short reads, so it's read when it's first shown, and again after
   an import. */
static char device_name[128];
static bool device_name_read = false;

static bool read_at(int fd, off_t pos, void *buf, size_t size)
{
    return lseek(fd, pos, SEEK_SET) == pos
           && read(fd, buf, size) == (ssize_t)size;
}

/* false if it couldn't be tried yet, with the disk not there */
static bool read_device_name(void)
{
    unsigned char h[44];
    unsigned char text[2 * (sizeof(device_name) / 3)];
    int fd = open(ITUNESDB_DIR "/iTunesDB", O_RDONLY);

    device_name[0] = '\0';
    if (fd < 0)
        return false;

    off_t pos = 0;
    if (!read_at(fd, pos, h, 16) || memcmp(h, "mhbd", 4))
        goto done;
    pos += le32(h + 4);

    /* the list of playlists */
    while (1)
    {
        if (!read_at(fd, pos, h, 16) || memcmp(h, "mhsd", 4))
            goto done;
        if (le32(h + 12) == 2)
            break;
        if (le32(h + 8) == 0)
            goto done;
        pos += le32(h + 8);
    }
    pos += le32(h + 4);
    if (!read_at(fd, pos, h, 16) || memcmp(h, "mhlp", 4))
        goto done;
    pos += le32(h + 4);

    /* the first of them, if it's the hidden one */
    if (!read_at(fd, pos, h, 44) || memcmp(h, "mhyp", 4) || le32(h + 20) == 0)
        goto done;
    int mhods = le32(h + 12);
    pos += le32(h + 4);

    for (int i = 0; i < mhods; i++)
    {
        if (!read_at(fd, pos, h, 40) || memcmp(h, "mhod", 4))
            goto done;
        if (le32(h + 12) == 1)
        {
            size_t units = MIN(le32(h + 28), sizeof(text)) / 2;
            if (read_at(fd, pos + 40, text, units * 2))
            {
                /* not half of a pair: the decoder would take both */
                if (units > 0 && text[units * 2 - 1] >= 0xd8
                    && text[units * 2 - 1] < 0xe0)
                    units--;
                *utf16LEdecode(text, (unsigned char *)device_name, units) = '\0';
            }
            break;
        }
        if (le32(h + 8) == 0)
            break;
        pos += le32(h + 8);
    }
done:
    close(fd);
    return true;
}

const char *itunesdb_device_name(void)
{
    if (!device_name_read)
        device_name_read = read_device_name();
    return device_name[0] ? device_name : "iPod";
}

bool itunesdb_exists(void)
{
    uint32_t size, mtime;
    return db_stamp(&size, &mtime);
}

/* tracks */

/* What iTunesDB says of the tracks Finder added since the last import, for
   the scan to take instead of each file's tags: reading those took the iPod
   13 ms a song. The records are made as the tracks are read and kept until
   the scan is done. */

#define META_PER_TRACK 256 /* bytes a record takes on average, at most */

/* the strings of a record, in its order, and their mhod types */
enum { META_TITLE, META_ARTIST, META_ALBUM, META_GENRE, META_COMPOSER,
       META_COMMENT, META_ALBUMARTIST, META_GROUPING, META_STRINGS };
static const uint8_t meta_mhods[META_STRINGS] = { 1, 4, 3, 5, 12, 8, 22, 13 };

struct meta_record {
    uint64_t hash;
    uint32_t length;    /* ms */
    uint16_t bitrate;   /* kbit/s */
    uint16_t year;
    uint16_t tracknum;
    uint16_t discnum;
    uint16_t size;      /* with the strings after it, a multiple of 8 */
    uint8_t rating;     /* 0..100 */
    uint8_t unused;
    uint32_t playcount;
    uint32_t unused2;
    /* then the strings, each ended by a zero, ID3V2_BUF_SIZE in all */
};

static struct {
    int handle;
    int count, capacity;
    uint32_t *by_hash;  /* offsets of the records in the pool, by hash */
    char *pool;
    size_t pool_size, pool_used;
    /* while iTunesDB is read: the path hashes of the last import, sorted */
    bool reading;
    int known_handle;
    int known;
    bool full;          /* no room for more records */
} meta;

void itunesdb_forget_metadata(void)
{
    if (meta.handle > 0)
    {
        core_unpin(meta.handle);
        core_free(meta.handle);
    }
    if (meta.known_handle > 0)
    {
        core_unpin(meta.known_handle);
        core_free(meta.known_handle);
    }
    memset(&meta, 0, sizeof(meta));
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int cmp_meta(const void *a, const void *b)
{
    uint64_t x = ((struct meta_record *)(meta.pool + *(const uint32_t *)a))->hash;
    uint64_t y = ((struct meta_record *)(meta.pool + *(const uint32_t *)b))->hash;
    return (x > y) - (x < y);
}

/* the path hashes of the last import, sorted; their count, 0 for none */
static int load_known_hashes(int *handle)
{
    struct state_header hdr;
    *handle = 0;
    if (!read_state_header(&hdr) || hdr.count == 0)
        return 0;
    int fd = open(STATE_FILE, O_RDONLY);
    if (fd < 0)
        return 0;
    *handle = db_alloc(hdr.count * sizeof(uint64_t));
    if (*handle <= 0)
    {
        *handle = 0;
        close(fd);
        return -1;
    }
    core_pin(*handle);
    uint64_t *hashes = core_get_data(*handle);
    struct state_entry chunk[64];
    uint32_t n = 0;
    lseek(fd, sizeof(hdr), SEEK_SET);
    while (n < hdr.count)
    {
        size_t want = MIN(hdr.count - n, ARRAYLEN(chunk));
        if (read(fd, chunk, want * sizeof(chunk[0]))
            != (ssize_t)(want * sizeof(chunk[0])))
            break;
        for (size_t i = 0; i < want; i++)
            hashes[n++] = chunk[i].hash;
    }
    close(fd);
    qsort(hashes, n, sizeof(uint64_t), cmp_u64);
    return n;
}

static bool known_hash(const uint64_t *hashes, int count, uint64_t hash)
{
    int lo = 0, hi = count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (hashes[mid] == hash)
            return true;
        if (hashes[mid] < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}

/* the record of a track into the pool, false when it doesn't fit */
static bool add_metadata(const struct track *t)
{
    static char strings[ID3V2_BUF_SIZE];
    off_t mhods_at[META_STRINGS] = { 0 };
    struct meta_record r;
    memset(&r, 0, sizeof(r));

    const unsigned char *p = db_record(t->pos, "mhit", 0x9c);
    if (!p)
        return true;
    uint32_t header_len = le32(p + 4);
    int mhods = le32(p + 12);
    r.hash = t->st.hash;
    r.playcount = t->st.playcount;
    r.rating = t->st.rating;
    r.length = le32(p + 40);
    r.tracknum = MIN(le32(p + 44), 0xffff);
    r.year = MIN(le32(p + 52), 0xffff);
    r.bitrate = MIN(le32(p + 56), 0xffff);
    r.discnum = MIN(le32(p + 92), 0xffff);

    off_t child = t->pos + header_len;
    for (int i = 0; i < mhods; i++)
    {
        p = db_record(child, "mhod", 16);
        if (!p)
            break;
        uint32_t total = le32(p + 8), type = le32(p + 12);
        for (int k = 0; k < META_STRINGS; k++)
        {
            if (meta_mhods[k] == type)
                mhods_at[k] = child;
        }
        if (total == 0)
            break;
        child += total;
    }

    size_t used = 0;
    for (int k = 0; k < META_STRINGS; k++)
    {
        size_t room = MIN(sizeof(strings) - used - (META_STRINGS - k),
                          ID3V2_MAX_ITEM_SIZE);
        strings[used] = '\0';
        if (mhods_at[k] && room > 1)
            mhod_string(mhods_at[k], strings + used, room);
        used += strlen(strings + used) + 1;
    }

    r.size = ALIGN_UP(sizeof(r) + used, 8);
    if (meta.pool_used + r.size > meta.pool_size)
        return false;
    memcpy(meta.pool + meta.pool_used, &r, sizeof(r));
    memcpy(meta.pool + meta.pool_used + sizeof(r), strings, used);
    meta.by_hash[meta.count++] = meta.pool_used;
    meta.pool_used += r.size;
    return true;
}

/* before iTunesDB is read for an import */
static void metadata_begin(void)
{
    itunesdb_forget_metadata();
    meta.known = load_known_hashes(&meta.known_handle);
    meta.reading = meta.known >= 0;
}

/* once the number of tracks is known: room for the records of all of them,
   less if there isn't enough memory, until metadata_end() gives back what
   the new ones didn't take */
static void metadata_room(int tracks)
{
    if (!meta.reading)
        return;
    /* the new ones and as many replaced, or all when none are known; the
       rest are read from the files */
    int count = MIN(tracks, MAX(tracks - meta.known, 0) * 2 + 256);
    size_t pool = count * META_PER_TRACK;
    while (1)
    {
        meta.handle = db_alloc(count * sizeof(uint32_t) + pool);
        if (meta.handle > 0 || pool < 64*1024)
            break;
        pool /= 2;
    }
    if (meta.handle <= 0)
    {
        meta.handle = 0;
        meta.reading = false;
        return;
    }
    core_pin(meta.handle);
    meta.by_hash = core_get_data(meta.handle);
    meta.pool = (char *)(meta.by_hash + count);
    meta.pool_size = pool;
    meta.capacity = count;
}

/* a track just read, its mhit still in the cache */
static void new_track_metadata(const struct track *t)
{
    if (!meta.reading || meta.full || meta.handle <= 0)
        return;
    const uint64_t *hashes = meta.known_handle > 0 ?
                             core_get_data(meta.known_handle) : NULL;
    if (known_hash(hashes, meta.known, t->st.hash))
        return;
    if (meta.count >= meta.capacity || !add_metadata(t))
        meta.full = true;
}

/* after iTunesDB was read */
static void metadata_end(void)
{
    if (meta.known_handle > 0)
    {
        core_unpin(meta.known_handle);
        core_free(meta.known_handle);
    }
    meta.known_handle = 0;
    meta.reading = false;
    if (meta.handle <= 0)
        return;
    if (meta.count == 0)
    {
        itunesdb_forget_metadata();
        return;
    }
    qsort(meta.by_hash, meta.count, sizeof(uint32_t), cmp_meta);
    core_unpin(meta.handle);
    core_shrink(meta.handle, meta.by_hash,
                meta.capacity * sizeof(uint32_t) + meta.pool_used);
    core_pin(meta.handle);
    meta.by_hash = core_get_data(meta.handle);
    meta.pool = (char *)(meta.by_hash + meta.capacity);
    meta.pool_size = meta.pool_used;
}

bool itunesdb_get_metadata(const char *path, struct mp3entry *id3,
                           struct tagcache_runtime *rt)
{
    if (meta.count == 0)
        return false;
    uint64_t hash = itunesdb_path_hash(path);
    const struct meta_record *r = NULL;
    int lo = 0, hi = meta.count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        const struct meta_record *m =
            (const struct meta_record *)(meta.pool + meta.by_hash[mid]);
        if (m->hash == hash)
        {
            r = m;
            break;
        }
        if (m->hash < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    if (!r)
        return false;

    memset(id3, 0, sizeof(*id3));
    const char *src = (const char *)(r + 1);
    size_t size = r->size - sizeof(*r);
    memcpy(id3->id3v2buf, src, MIN(size, sizeof(id3->id3v2buf)));
    char **tags[META_STRINGS] = {
        &id3->title, &id3->artist, &id3->album, &id3->genre_string,
        &id3->composer, &id3->comment, &id3->albumartist, &id3->grouping
    };
    char *c = id3->id3v2buf;
    for (int k = 0; k < META_STRINGS; k++)
    {
        *tags[k] = *c ? c : NULL;
        c += strlen(c) + 1;
    }
    id3->length = r->length;
    id3->bitrate = r->bitrate;
    id3->year = r->year;
    id3->tracknum = r->tracknum;
    id3->discnum = r->discnum;
    /* as itunesdb_import() would set them right after */
    rt->playcount = r->playcount;
    rt->rating = r->rating / 10;
    rt->lastplayed = 0;
    return true;
}

static void read_track(off_t pos, struct track *track)
{
    char path[MAX_PATH];
    const unsigned char *p = db_record(pos, "mhit", 12);
    memset(track, 0, sizeof(*track));
    track->pos = pos;
    if (!p)
        return;
    uint32_t header_len = le32(p + 4);
    p = db_get(pos, MIN(header_len, 356));
    if (!p || header_len < 0x9c)
        return;
    int mhods = le32(p + 12);
    track->id = le32(p + 16);
    track->st.rating = p[31];
    track->sound_check = le32(p + 76);
    track->st.playcount = le32(p + 80);
    uint32_t media = 0;
    if (header_len >= 212)
    {
        media = le32(p + 208);
        track->st.media = media > 0xfe ? 0xff : media;
    }
    /* The stock firmware remembers the place and leaves the track out of
       Shuffle Songs where iTunes says to, as it does for podcasts and
       audiobooks; those always are, in case Finder leaves it to the media
       type */
    bool spoken = media & (MEDIA_PODCAST | MEDIA_AUDIOBOOK);
    if ((header_len > 166 && p[166]) || spoken)
        track->st.flags |= ITUNESDB_REMEMBERS_PLACE;
    if ((header_len > 165 && p[165]) || spoken)
        track->st.flags |= ITUNESDB_SKIP_SHUFFLE;
    if (media & MEDIA_AUDIOBOOK)
        track->st.flags |= ITUNESDB_AUDIOBOOK;
    else if (media & MEDIA_PODCAST)
        track->st.flags |= ITUNESDB_PODCAST;
    if (header_len >= 356)
        track->art = le32(p + 352);

    /* the album, by its name and the album's artist, or the track's where
       none is given, for the covers of Cover Flow: as UTF-16, unread */
    uint32_t album = 0, album_artist = 0, artist = 0;
    off_t child = pos + header_len;
    for (int i = 0; i < mhods; i++)
    {
        p = db_record(child, "mhod", 16);
        if (!p)
            break;
        uint32_t total = le32(p + 8), type = le32(p + 12);
        if (type == 3 || type == 4 || type == 22)
        {
            uint32_t h = mhod_hash(child);
            *(type == 3 ? &album : type == 4 ? &artist : &album_artist) = h;
        }
        else if (type == 2 && !track->path
                 && mhod_string(child, path, sizeof(path)))
        {
            /* ":iPod_Control:Music:F07:CWZV.m4a" */
            for (char *c = path; *c; c++)
                if (*c == ':')
                    *c = '/';
            size_t len = strlen(path) + 1;
            if (db.pool_used + len <= db.pool_size)
            {
                track->path = db.pool_used;
                memcpy(db.pool + db.pool_used, path, len);
                db.pool_used += len;
                track->st.hash = itunesdb_path_hash(path);
            }
        }
        if (total == 0)
            break;
        child += total;
    }
    if (album)
        track->album = (album * 31 + (album_artist ? album_artist : artist))
                       | 1;
    if (track->st.hash)
        new_track_metadata(track);
}

/* how far itunesdb_import_tracks() has read, for its progress screen: the
   tracks are the first half and the covers the second */
static volatile int *read_done, *read_total;

static void reading(int part, int done, int of)
{
    if (read_done && of > 0)
    {
        *read_total = 2000;
        *read_done = part * 1000 + done * 1000 / of;
    }
}

static bool read_tracks(off_t list)
{
    const unsigned char *p = db_record(list, "mhlt", 12);
    if (!p)
        return false;
    off_t pos = list + le32(p + 4);
    int listed = le32(p + 8);
    int count = MIN(listed, db.count);
    db.count = 0;
    for (int i = 0; i < count; i++)
    {
        p = db_record(pos, "mhit", 12);
        if (!p)
            break;
        uint32_t total = le32(p + 8); /* before read_track moves the cache */
        read_track(pos, &db.tracks[db.count++]);
        if (total == 0)
            break;
        pos += total;
        if ((i & 255) == 0)
        {
            reading(0, i, count);
            yield();
        }
    }
    db.complete = db.count == listed;
    return db.count > 0;
}

static int cmp_id(const void *a, const void *b)
{
    uint32_t x = db.tracks[*(const uint32_t *)a].id;
    uint32_t y = db.tracks[*(const uint32_t *)b].id;
    return (x > y) - (x < y);
}

static int cmp_hash(const void *a, const void *b)
{
    uint64_t x = db.tracks[*(const uint32_t *)a].st.hash;
    uint64_t y = db.tracks[*(const uint32_t *)b].st.hash;
    return (x > y) - (x < y);
}

static struct track *find_by_id(uint32_t id)
{
    int lo = 0, hi = db.count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        struct track *t = &db.tracks[db.by_id[mid]];
        if (t->id == id)
            return t;
        if (t->id < id)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* playlists */

static void playlist_dir(char *buf, size_t size)
{
    const char *dir = PLAYLIST_CATALOG_DEFAULT_DIR;
    if (global_settings.playlist_catalog_dir[0] != '\0')
        dir = global_settings.playlist_catalog_dir;
    strmemccpy(buf, dir, size);
}

/* The playlists file lists those of the last import, a line each:
   "<hash> <size> <file>" with the hash and size of what was written. Each
   file costs the iPod's flash a good part of a second to remove and write
   again, so those that would come out the same are left. */

#define MAX_PLAYLISTS 256

static struct {
    int handle;
    int count;
    struct old_playlist {
        uint64_t file_hash;     /* itunesdb_path_hash() of the file */
        uint64_t hash;          /* of what it holds, 0 if not known */
        uint32_t size;
        uint32_t file;          /* offset of its name in the buffer */
    } *lists;
    char *names;
    /* the files written this time */
    int written_count;
    uint64_t written[MAX_PLAYLISTS];
} pls;

static void load_old_playlists(void)
{
    memset(&pls, 0, sizeof(pls));
    int fd = open(PLAYLISTS_FILE, O_RDONLY);
    if (fd < 0)
        return;
    char line[MAX_PATH + 32];
    off_t size = ffilesize(fd);
    pls.handle = db_alloc(MAX_PLAYLISTS * sizeof(struct old_playlist)
                            + size + 1);
    if (pls.handle <= 0)
    {
        /* all are written again then, so they go first */
        pls.handle = 0;
        while (read_line(fd, line, sizeof(line)) > 0)
        {
            char *file = strchr(line, '/');
            if (file)
                remove(file);
        }
        close(fd);
        return;
    }
    core_pin(pls.handle);
    pls.lists = core_get_data(pls.handle);
    pls.names = (char *)(pls.lists + MAX_PLAYLISTS);
    size_t used = 0;
    while (pls.count < MAX_PLAYLISTS
           && read_line(fd, line, sizeof(line)) > 0)
    {
        struct old_playlist *o = &pls.lists[pls.count];
        char *file = line;
        o->hash = 0;
        o->size = 0;
        if (line[0] != '/')
        {
            /* not from an older version, which listed only the files */
            char *end, high[9];
            strmemccpy(high, line, sizeof(high));
            o->hash = (uint64_t)strtoul(high, NULL, 16) << 32
                      | (uint32_t)strtoul(line + 8, &end, 16);
            o->size = strtoul(end, &end, 10);
            file = end + 1;
            if (*end != ' ' || file[0] != '/')
                continue;
        }
        size_t len = strlen(file) + 1;
        if (used + len > (size_t)size + 1)
            break;
        memcpy(pls.names + used, file, len);
        o->file = used;
        o->file_hash = itunesdb_path_hash(file);
        used += len;
        pls.count++;
    }
    close(fd);
}

static struct old_playlist *find_old_playlist(const char *file)
{
    uint64_t h = itunesdb_path_hash(file);
    for (int i = 0; i < pls.count; i++)
    {
        if (pls.lists[i].file_hash == h
            && !strcasecmp(pls.names + pls.lists[i].file, file))
            return &pls.lists[i];
    }
    return NULL;
}

static bool written_playlist(uint64_t file_hash)
{
    for (int i = 0; i < pls.written_count; i++)
    {
        if (pls.written[i] == file_hash)
            return true;
    }
    return false;
}

/* the old ones not written again go, as Finder removed them */
static void remove_old_playlists(void)
{
    for (int i = 0; i < pls.count; i++)
    {
        if (!written_playlist(pls.lists[i].file_hash))
            remove(pls.names + pls.lists[i].file);
    }
    if (pls.handle > 0)
    {
        core_unpin(pls.handle);
        core_free(pls.handle);
    }
    memset(&pls, 0, sizeof(pls));
}

/* a playlist name as a file name that FAT takes; false if it is too long */
static bool playlist_file(char *buf, size_t size, const char *dir,
                          const char *name)
{
    char clean[MAX_PATH / 2];
    size_t n = 0;
    for (const char *c = name; *c && n < sizeof(clean) - 1; c++)
        clean[n++] = strchr("/\\:*?\"<>|", *c) ? '_' : *c;
    clean[n] = '\0';
    if (n == 0)
        strcpy(clean, "Playlist");

    /* taken by another playlist of this import, or by a file that isn't
       one of the last */
    if ((size_t)snprintf(buf, size, "%s/%s.m3u8", dir, clean) >= size)
        return false;
    for (int i = 2; i < 100; i++)
    {
        if (!written_playlist(itunesdb_path_hash(buf))
            && (find_old_playlist(buf) || !file_exists(buf)))
            break;
        if ((size_t)snprintf(buf, size, "%s/%s %d.m3u8", dir, clean, i) >= size)
            return false;
    }
    return true;
}

/* Files are written through this a block at a time: a write() costs the
   iPod about as much for 16 bytes as for 4 KB */
struct writer {
    int fd;
    size_t used;
    bool ok;
    char buf[WRITE_BUF_SIZE];
};

static struct writer out;

static bool writer_open(struct writer *w, const char *file)
{
    w->fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    w->used = 0;
    w->ok = w->fd >= 0;
    return w->ok;
}

static void writer_flush(struct writer *w)
{
    if (w->used && write(w->fd, w->buf, w->used) != (ssize_t)w->used)
        w->ok = false;
    w->used = 0;
}

static void writer_put(struct writer *w, const void *data, size_t len)
{
    if (w->used + len > sizeof(w->buf))
        writer_flush(w);
    /* more than the buffer holds, as the slides of a large library are */
    if (len > sizeof(w->buf))
    {
        if (write(w->fd, data, len) != (ssize_t)len)
            w->ok = false;
        return;
    }
    memcpy(w->buf + w->used, data, len);
    w->used += len;
}

static bool writer_close(struct writer *w)
{
    writer_flush(w);
    if (w->fd >= 0)
        close(w->fd);
    w->fd = -1;
    return w->ok;
}

static void writer_line(struct writer *w, const char *line)
{
    size_t len = strlen(line);
    if (w->used + len + 1 > sizeof(w->buf))
        writer_flush(w);
    if (len + 1 > sizeof(w->buf))
        return;
    memcpy(w->buf + w->used, line, len);
    w->buf[w->used + len] = '\n';
    w->used += len + 1;
}

static void playlist_line(struct writer *w, const char *line, uint64_t *hash,
                          uint32_t *size)
{
    for (const char *c = line; ; c++)
    {
        unsigned char b = *c ? *c : '\n';
        *hash = (*hash ^ b) * 0x100000001b3ULL;
        if (!*c)
            break;
    }
    *size += strlen(line) + 1;
    if (w)
        writer_line(w, line);
}

/* the lines of a playlist's items from child on, into w if there is one,
   and their hash and size */
static void playlist_items(off_t child, int items, struct writer *w,
                           uint64_t *hash, uint32_t *size)
{
    const unsigned char *p;
    *hash = 0xcbf29ce484222325ULL;
    *size = 0;
    playlist_line(w, "#EXTM3U", hash, size);
    for (int i = 0; i < items; i++)
    {
        p = db_record(child, "mhip", 28);
        if (!p)
            break;
        uint32_t total = le32(p + 8);
        struct track *t = find_by_id(le32(p + 24));
        if (t && t->path)
            playlist_line(w, db.pool + t->path, hash, size);
        if (total == 0)
            break;
        child += total;
    }
}

/* the mhyp at pos as a playlist file, listed in the playlists file */
static void write_playlist(off_t pos, const char *dir, int list_fd,
                           struct writer *w)
{
    char name[MAX_PATH / 2], file[MAX_PATH];
    const unsigned char *p = db_record(pos, "mhyp", 44);
    if (!p)
        return;
    uint32_t header_len = le32(p + 4);
    int mhods = le32(p + 12);
    int items = le32(p + 16);
    bool hidden = le32(p + 20) != 0;   /* the library itself */
    bool podcasts = (p[42] | p[43] << 8) != 0;
    if (hidden || podcasts)
        return;

    name[0] = '\0';
    off_t child = pos + header_len;
    for (int i = 0; i < mhods; i++)
    {
        p = db_record(child, "mhod", 16);
        if (!p)
            return;
        uint32_t total = le32(p + 8);
        if (le32(p + 12) == 1)
            mhod_string(child, name, sizeof(name));
        if (total == 0)
            return;
        child += total;
    }

    if (!playlist_file(file, sizeof(file), dir, name)
        || pls.written_count >= MAX_PLAYLISTS)
        return;

    /* what it would hold, then the file only if that changed */
    uint64_t hash;
    uint32_t size;
    playlist_items(child, items, NULL, &hash, &size);
    struct old_playlist *o = find_old_playlist(file);
    bool same = false;
    if (o && o->hash == hash && o->size == size)
    {
        int fd = open(file, O_RDONLY);
        if (fd >= 0)
        {
            same = ffilesize(fd) == (off_t)size;
            close(fd);
        }
    }
    if (!same)
    {
        if (!writer_open(w, file))
            return;
        playlist_items(child, items, w, &hash, &size);
        if (!writer_close(w))
            hash = 0;
    }
    pls.written[pls.written_count++] = itunesdb_path_hash(file);
    fdprintf(list_fd, "%08lx%08lx %lu %s\n", (unsigned long)(hash >> 32),
             (unsigned long)(uint32_t)hash, (unsigned long)size, file);
}

static void write_playlists(off_t list)
{
    char dir[MAX_PATH];
    const unsigned char *p = list >= 0 ? db_record(list, "mhlp", 12) : NULL;
    load_old_playlists();
    int list_fd = open(PLAYLISTS_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (p && list_fd >= 0)
    {
        off_t pos = list + le32(p + 4);
        int count = le32(p + 8);

        playlist_dir(dir, sizeof(dir));
        mkdir(dir);
        for (int i = 0; i < count; i++)
        {
            p = db_record(pos, "mhyp", 12);
            if (!p)
                break;
            uint32_t total = le32(p + 8);
            write_playlist(pos, dir, list_fd, &out);
            if (total == 0)
                break;
            pos += total;
            yield();
        }
    }
    if (list_fd >= 0)
        close(list_fd);
    remove_old_playlists();
}

/* Ratings and play counts. iTunesDB is read once, at boot after a sync,
   and the tracks' counts are kept in a file until the database has the
   files Finder added, then go into it. */

#define PENDING_FILE    ROCKBOX_DIR "/itunesdb_pending.dat"
#define PENDING_MAGIC   0x50494252 /* "RBIP" */
#define PENDING_VERSION 1

struct pending_header {
    uint32_t magic;
    uint32_t version;
    uint32_t db_size;   /* of the iTunesDB they are from */
    uint32_t db_mtime;
    uint32_t count;
    uint32_t complete;  /* every track listed was read */
};

static struct {
    int handle;
    struct pending_header hdr;
    struct state_entry *tracks; /* in iTunesDB order */
    uint32_t *by_hash;          /* their indexes, by path hash */
} pend;

static void write_pending(void)
{
    struct pending_header hdr = {
        PENDING_MAGIC, PENDING_VERSION, db_size, db_mtime, db.count,
        db.complete
    };
    if (!writer_open(&out, PENDING_FILE))
        return;
    writer_put(&out, &hdr, sizeof(hdr));
    for (int i = 0; i < db.count; i++)
        writer_put(&out, &db.tracks[i].st, sizeof(struct state_entry));
    if (!writer_close(&out))
        remove(PENDING_FILE);
}

static void unload_pending(void)
{
    if (pend.handle > 0)
    {
        core_unpin(pend.handle);
        core_free(pend.handle);
    }
    memset(&pend, 0, sizeof(pend));
}

static int cmp_pending_hash(const void *a, const void *b)
{
    uint64_t x = pend.tracks[*(const uint32_t *)a].hash;
    uint64_t y = pend.tracks[*(const uint32_t *)b].hash;
    return (x > y) - (x < y);
}

/* what was read from the iTunesDB that is there now */
static bool load_pending(void)
{
    uint32_t size, mtime;
    memset(&pend, 0, sizeof(pend));
    int fd = open(PENDING_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    bool ok = read(fd, &pend.hdr, sizeof(pend.hdr)) == sizeof(pend.hdr)
        && pend.hdr.magic == PENDING_MAGIC
        && pend.hdr.version == PENDING_VERSION && pend.hdr.count > 0
        && pend.hdr.count <= COUNT_MAX
        && db_stamp(&size, &mtime) && pend.hdr.db_size == size
        && pend.hdr.db_mtime == mtime;
    if (ok)
    {
        size_t entries = pend.hdr.count * sizeof(struct state_entry);
        pend.handle = db_alloc(entries
                                 + pend.hdr.count * sizeof(uint32_t));
        ok = pend.handle > 0;
        if (ok)
        {
            core_pin(pend.handle);
            pend.tracks = core_get_data(pend.handle);
            pend.by_hash = (uint32_t *)((char *)pend.tracks + entries);
            ok = read(fd, pend.tracks, entries) == (ssize_t)entries;
        }
    }
    close(fd);
    if (!ok)
    {
        unload_pending();
        return false;
    }
    for (uint32_t i = 0; i < pend.hdr.count; i++)
        pend.by_hash[i] = i;
    qsort(pend.by_hash, pend.hdr.count, sizeof(uint32_t), cmp_pending_hash);
    return true;
}

static const struct state_entry *find_pending(uint64_t hash)
{
    int lo = 0, hi = pend.hdr.count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        const struct state_entry *t = &pend.tracks[pend.by_hash[mid]];
        if (t->hash == hash)
            return t;
        if (t->hash < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* 1 to set the counts, 0 to leave the entry, -1 to remove it */
static int update_entry(const char *path, struct tagcache_runtime *rt,
                        void *data)
{
    (void)data;
    const struct state_entry *t = find_pending(itunesdb_path_hash(path));
    if (!t)
    {
        /* Finder renames and removes the files in its folder as it syncs,
           so one that iTunesDB doesn't list is left from an older sync.
           Only trusted when all of iTunesDB could be read. */
        return (pend.hdr.complete && itunesdb_synced_path(path)) ? -1 : 0;
    }
    rt->playcount = t->playcount;
    rt->rating = t->rating / 10; /* the database counts half stars */
    return 1;
}

/* the counts the plays on the iPod are counted from */
static bool write_state(void)
{
    struct state_header hdr = {
        STATE_MAGIC, STATE_VERSION, pend.hdr.db_size, pend.hdr.db_mtime,
        pend.hdr.count
    };
    /* not over the old one: Rockbox truncates a file on its close, so a
       cut there left the new header on the old tracks, whose counts Play
       Counts would then give to others */
    if (!writer_open(&out, STATE_FILE ".tmp"))
        return false;
    writer_put(&out, &hdr, sizeof(hdr));
    for (uint32_t i = 0; i < pend.hdr.count; i++)
        writer_put(&out, &pend.tracks[i], sizeof(struct state_entry));
    if (!writer_close(&out) || rename(STATE_FILE ".tmp", STATE_FILE) < 0)
    {
        remove(STATE_FILE ".tmp");
        return false;
    }
    return true;
}

/* iTunesDB and its tracks, for the import and for the covers */

static int cache_handle, table_handle;
static off_t playlists_list;

/* after every db_load(), loaded or not */
static void db_unload(void)
{
    if (db.fd >= 0)
        close(db.fd);
    if (table_handle > 0)
    {
        core_unpin(table_handle);
        core_free(table_handle);
    }
    if (cache_handle > 0)
    {
        core_unpin(cache_handle);
        core_free(cache_handle);
    }
    cache_handle = table_handle = 0;
    memset(&db, 0, sizeof(db));
    db.fd = -1;
    cpu_boost(false);
}

static bool db_load(void)
{
    off_t tracks_list = -1, pos;
    const unsigned char *p;
    int sections;

    memset(&db, 0, sizeof(db));
    /* fd 0 is a file on the iPod */
    db.fd = -1;
    playlists_list = -1;
    /* before anything can fail, as db_unload() undoes it */
    cpu_boost(true);
    if (!db_stamp(&db_size, &db_mtime))
        return false;
    db.fd = open(ITUNESDB_FILE, O_RDONLY);
    if (db.fd < 0)
        return false;

    /* 4 more bytes for the second half of a surrogate pair at the end */
    size_t cache_size = STORAGE_PAD(CACHE_SIZE + 4);
    cache_handle = db_alloc(cache_size);
    if (cache_handle <= 0)
        return false;
    core_pin(cache_handle);
    void *cache = core_get_data(cache_handle);
    STORAGE_ALIGN_BUFFER(cache, cache_size);
    db.cache = cache;

    /* the sections: tracks are index 1, playlists index 2 */
    p = db_record(0, "mhbd", 24);
    if (!p)
        return false;
    pos = le32(p + 4);
    sections = le32(p + 20);
    for (int i = 0; i < sections; i++)
    {
        p = db_record(pos, "mhsd", 16);
        if (!p)
            break;
        uint32_t header_len = le32(p + 4), total = le32(p + 8);
        uint32_t index = le32(p + 12);
        if (index == 1)
            tracks_list = pos + header_len;
        else if (index == 2)
            playlists_list = pos + header_len;
        if (total == 0)
            break;
        pos += total;
    }
    if (tracks_list < 0 || !(p = db_record(tracks_list, "mhlt", 12)))
        return false;

    db.count = le32(p + 8);
    if ((uint32_t)db.count > COUNT_MAX)
        return false;
    size_t table_size =
        db.count * (sizeof(struct track) + 2 * sizeof(uint32_t));
    db.pool_size = db.count * POOL_PER_TRACK;
    table_handle = db_alloc(table_size + db.pool_size);
    if (table_handle <= 0)
        return false;
    core_pin(table_handle);
    metadata_room(db.count);
    db.tracks = core_get_data(table_handle);
    db.by_id = (uint32_t *)(db.tracks + db.count);
    db.by_hash = db.by_id + db.count;
    db.pool = (char *)(db.by_hash + db.count);
    db.pool_used = 1; /* offset 0 means no path */

    if (!read_tracks(tracks_list))
        return false;
    for (int i = 0; i < db.count; i++)
        db.by_id[i] = db.by_hash[i] = i;
    qsort(db.by_id, db.count, sizeof(uint32_t), cmp_id);
    qsort(db.by_hash, db.count, sizeof(uint32_t), cmp_hash);
    return true;
}

void itunesdb_import(void)
{
    if (!load_pending())
        return; /* the next boot reads iTunesDB again */
    if (tagcache_update_runtime(update_entry, NULL) && write_state())
        remove(PENDING_FILE);
    logf("itunesdb: %lu tracks imported", (unsigned long)pend.hdr.count);
    unload_pending();
}

/* Play Counts, as the stock firmware writes it: a header, then for every
   track in iTunesDB's order its plays since the sync, when it was last
   played, and its rating. Finder adds the plays to the library, takes the
   ratings and removes the file. */

#define PLAYCOUNTS_FILE  ITUNESDB_DIR "/Play Counts"
#define PLAYCOUNTS_HDR   0x60
#define PLAYCOUNTS_ENTRY 0x1c
/* seconds from 1904, which iTunes counts from, to 1970 */
#define MAC_EPOCH        2082844800UL
/* last played values from here on are times, below they are serials */
#define LASTPLAYED_TIME  0x40000000L

/* what the database has now for an iTunesDB track */
struct played {
    uint32_t playcount;
    uint32_t lastplayed;
    uint8_t rating;     /* 0..10 */
    bool found;
};

static struct {
    struct state_entry *state;   /* in iTunesDB order */
    uint32_t *by_hash;
    struct played *now;
    int count;
} pc;

static int cmp_state_hash(const void *a, const void *b)
{
    uint64_t x = pc.state[*(const uint32_t *)a].hash;
    uint64_t y = pc.state[*(const uint32_t *)b].hash;
    return (x > y) - (x < y);
}

static int read_played(const char *path, struct tagcache_runtime *rt,
                       void *data)
{
    (void)data;
    uint64_t hash = itunesdb_path_hash(path);
    int lo = 0, hi = pc.count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        uint32_t i = pc.by_hash[mid];
        if (pc.state[i].hash == hash)
        {
            pc.now[i].playcount = rt->playcount;
            pc.now[i].lastplayed = rt->lastplayed;
            pc.now[i].rating = MIN(rt->rating, 10);
            pc.now[i].found = true;
            break;
        }
        if (pc.state[i].hash < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0; /* only reading */
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

void itunesdb_write_playcounts(void)
{
    struct state_header hdr;
    uint32_t size, mtime;
    int handle = 0, fd = -1;
    bool changed = false;

    /* the entries go in the order of the iTunesDB the state was made from,
       so only when that is the one on the disk */
    if (!db_stamp(&size, &mtime) || !read_state_header(&hdr)
        || hdr.db_size != size || hdr.db_mtime != mtime || hdr.count == 0)
        return;

    pc.count = hdr.count;
    handle = db_alloc(pc.count * (sizeof(struct state_entry)
                        + sizeof(uint32_t) + sizeof(struct played)));
    if (handle <= 0)
        return;
    core_pin(handle);
    pc.state = core_get_data(handle);
    pc.by_hash = (uint32_t *)(pc.state + pc.count);
    pc.now = (struct played *)(pc.by_hash + pc.count);
    memset(pc.now, 0, pc.count * sizeof(struct played));

    fd = open(STATE_FILE, O_RDONLY);
    if (fd < 0)
        goto done;
    lseek(fd, sizeof(hdr), SEEK_SET);
    ssize_t bytes = pc.count * sizeof(struct state_entry);
    bool ok = read(fd, pc.state, bytes) == bytes;
    close(fd);
    fd = -1;
    if (!ok)
        goto done;
    for (int i = 0; i < pc.count; i++)
        pc.by_hash[i] = i;
    qsort(pc.by_hash, pc.count, sizeof(uint32_t), cmp_state_hash);

    if (!tagcache_update_runtime(read_played, NULL))
        goto done;

    /* nothing to tell Finder: no file, as after it took the last one */
    for (int i = 0; i < pc.count && !changed; i++)
        changed = pc.now[i].found
            && (pc.now[i].playcount > pc.state[i].playcount
                || pc.now[i].rating * 10 != pc.state[i].rating);
    if (!changed)
    {
        remove(PLAYCOUNTS_FILE);
        goto done;
    }

    fd = open(PLAYCOUNTS_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        goto done;
    unsigned char buf[PLAYCOUNTS_HDR];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "mhdp", 4);
    put32(buf + 4, PLAYCOUNTS_HDR);
    put32(buf + 8, PLAYCOUNTS_ENTRY);
    put32(buf + 12, pc.count);
    ok = write(fd, buf, PLAYCOUNTS_HDR) == PLAYCOUNTS_HDR;

    static unsigned char entries[PLAYCOUNTS_ENTRY * 64];
    size_t used = 0;
    for (int i = 0; ok && i < pc.count; i++)
    {
        const struct state_entry *st = &pc.state[i];
        const struct played *now = &pc.now[i];
        uint32_t plays = 0, last = 0, rating = st->rating;
        if (now->found)
        {
            if (now->playcount > st->playcount)
                plays = now->playcount - st->playcount;
            if (plays && (long)now->lastplayed >= LASTPLAYED_TIME)
                last = now->lastplayed + MAC_EPOCH;
            rating = now->rating * 10;
        }
        unsigned char *e = entries + used;
        memset(e, 0, PLAYCOUNTS_ENTRY);
        put32(e, plays);
        put32(e + 4, last);
        put32(e + 12, rating);
        used += PLAYCOUNTS_ENTRY;
        if (used == sizeof(entries) || i == pc.count - 1)
        {
            ok = write(fd, entries, used) == (ssize_t)used;
            used = 0;
        }
    }
    close(fd);
    fd = -1;
    if (ok)
    {
        remove(PLAYCOUNTS_FILE);
        rename(PLAYCOUNTS_FILE ".tmp", PLAYCOUNTS_FILE);
    }
    else
        remove(PLAYCOUNTS_FILE ".tmp");

done:
    if (fd >= 0)
        close(fd);
    core_unpin(handle);
    core_free(handle);
    memset(&pc, 0, sizeof(pc));
}

/* What playback looks up for each track it plays: files of a header that
   starts with their magic, version and count, then entries sorted by path
   hash, which each starts with. Every open and every small read costs the
   iPod 20-30 ms, so they are read into memory, at boot and after an
   import: nothing is allocated while music plays, as the audio buffer has
   the memory then and taking some would stop it. */

struct index_header {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
};

struct index {
    int handle;         /* the entries, 0 for none */
    uint32_t count;
    size_t entry_size;
};

/* reads the header into hdr and the entries into memory */
static void load_index(struct index *index, const char *file, void *hdr,
                       size_t hdr_size, uint32_t magic, uint32_t version)
{
    const struct index_header *h = hdr;
    if (index->handle > 0)
        return;
    int fd = open(file, O_RDONLY);
    if (fd < 0)
        return;
    if (read(fd, hdr, hdr_size) == (ssize_t)hdr_size
        && h->magic == magic && h->version == version && h->count > 0
        && h->count <= COUNT_MAX)
    {
        ssize_t size = h->count * index->entry_size;
        int handle = db_alloc(size);
        if (handle > 0)
        {
            core_pin(handle);
            bool ok = read(fd, core_get_data(handle), size) == size;
            core_unpin(handle);
            if (ok)
            {
                index->handle = handle;
                index->count = h->count;
            }
            else
                core_free(handle);
        }
    }
    close(fd);
}

static void forget_index(struct index *index)
{
    if (index->handle > 0)
        core_free(index->handle);
    index->handle = 0;
    index->count = 0;
}

/* copies the entry with this hash into entry */
static bool find_in_index(struct index *index, uint64_t hash, void *entry)
{
    if (index->handle <= 0)
        return false;
    core_pin(index->handle);
    const char *entries = core_get_data(index->handle);
    bool found = false;
    int lo = 0, hi = (int)index->count - 1;
    while (lo <= hi && !found)
    {
        int mid = (lo + hi) / 2;
        const char *e = entries + mid * index->entry_size;
        uint64_t h = *(const uint64_t *)e;
        if (h == hash)
        {
            memcpy(entry, e, index->entry_size);
            found = true;
        }
        else if (h < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    core_unpin(index->handle);
    return found;
}

/* the tracks with flags */
#define FLAGS_FILE      ROCKBOX_DIR "/itunesdb_flags.dat"
#define FLAGS_MAGIC     0x46494252 /* "RBIF" */
#define FLAGS_VERSION   2

struct flags_header {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t db_size;   /* of the iTunesDB they are from */
    uint32_t db_mtime;
};

struct flags_entry {
    uint64_t hash;
    uint32_t flags;
    uint32_t unused;
};

static struct index flags_index = { 0, 0, sizeof(struct flags_entry) };

static void write_flags(void)
{
    if (!writer_open(&out, FLAGS_FILE ".tmp"))
        return;
    struct flags_header hdr = {
        FLAGS_MAGIC, FLAGS_VERSION, 0, db_size, db_mtime
    };
    writer_put(&out, &hdr, sizeof(hdr));
    /* by path hash, which db.by_hash already has them in */
    for (int i = 0; i < db.count; i++)
    {
        const struct track *t = &db.tracks[db.by_hash[i]];
        if (!t->path || !t->st.flags)
            continue;
        struct flags_entry entry = { t->st.hash, t->st.flags, 0 };
        writer_put(&out, &entry, sizeof(entry));
        hdr.count++;
    }
    /* the count, now that it is known */
    writer_flush(&out);
    lseek(out.fd, 0, SEEK_SET);
    writer_put(&out, &hdr, sizeof(hdr));
    if (writer_close(&out))
        rename(FLAGS_FILE ".tmp", FLAGS_FILE);
    logf("itunesdb: %lu tracks with flags", (unsigned long)hdr.count);
}

unsigned itunesdb_hash_flags(uint64_t hash)
{
    struct flags_entry entry;
    if (flags_index.handle <= 0 || !find_in_index(&flags_index, hash, &entry))
        return 0;
    return entry.flags;
}

unsigned itunesdb_track_flags(const char *path)
{
    if (flags_index.handle <= 0)
        return 0;
    return itunesdb_hash_flags(itunesdb_path_hash(path));
}

/* Sound Check: Finder works it out for nearly every track, and iTunesDB holds
   it as 1000 times the power the track is to be brought to. There are too many
   to keep in memory for a look at each when it loads, so they stay in a file. */
#define GAINS_FILE      ROCKBOX_DIR "/itunesdb_gains.dat"
#define GAINS_MAGIC     0x47494252 /* "RBIG" */
#define GAINS_VERSION   1

struct gain_entry {
    uint64_t hash;
    uint32_t sound_check;
    uint32_t unused;
};

static void write_gains(void)
{
    if (!writer_open(&out, GAINS_FILE ".tmp"))
        return;
    struct flags_header hdr = {
        GAINS_MAGIC, GAINS_VERSION, 0, db_size, db_mtime
    };
    writer_put(&out, &hdr, sizeof(hdr));
    /* by path hash, to be searched */
    for (int i = 0; i < db.count; i++)
    {
        const struct track *t = &db.tracks[db.by_hash[i]];
        if (!t->path || !t->sound_check)
            continue;
        struct gain_entry entry = { t->st.hash, t->sound_check, 0 };
        writer_put(&out, &entry, sizeof(entry));
        hdr.count++;
    }
    writer_flush(&out);
    lseek(out.fd, 0, SEEK_SET);
    writer_put(&out, &hdr, sizeof(hdr));
    if (writer_close(&out))
        rename(GAINS_FILE ".tmp", GAINS_FILE);
    logf("itunesdb: %lu tracks with Sound Check", (unsigned long)hdr.count);
}

static bool read_gains_header(int fd, struct flags_header *hdr)
{
    return read(fd, hdr, sizeof(*hdr)) == sizeof(*hdr)
           && hdr->magic == GAINS_MAGIC && hdr->version == GAINS_VERSION;
}

bool itunesdb_gains_ready(void)
{
    uint32_t size, mtime;
    if (!db_stamp(&size, &mtime))
        return true;
    struct flags_header hdr;
    int fd = open(GAINS_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    bool ready = read_gains_header(fd, &hdr)
                 && hdr.db_size == size && hdr.db_mtime == mtime;
    close(fd);
    return ready;
}

void itunesdb_set_track_gain(struct mp3entry *id3)
{
    struct flags_header hdr;
    int fd = open(GAINS_FILE, O_RDONLY);
    if (fd < 0)
        return;
    uint32_t sound_check = 0;
    if (read_gains_header(fd, &hdr))
    {
        const uint64_t hash = itunesdb_path_hash(id3->path);
        int lo = 0, hi = (int)hdr.count - 1;
        while (lo <= hi && !sound_check)
        {
            int mid = (lo + hi) / 2;
            struct gain_entry entry;
            lseek(fd, sizeof(hdr) + mid * sizeof(entry), SEEK_SET);
            if (read(fd, &entry, sizeof(entry)) != sizeof(entry))
                break;
            if (entry.hash == hash)
                sound_check = entry.sound_check;
            else if (entry.hash < hash)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
    }
    close(fd);
    if (!sound_check)
        return;
    /* the factor on the samples, s7.24. A track Finder would raise stays as it
       is: iTunesDB doesn't say where its peaks are, and raised they could clip */
    id3->track_gain = sound_check <= 1000 ? (1L << 24) :
        fp_sqrt(((uint64_t)1000 << 24) / sound_check, 24);
    id3->album_gain = 0;
    id3->album_peak = 0;
}

bool itunesdb_resumes(const char *path)
{
    return global_settings.autoresume_enable
           || (itunesdb_track_flags(path) & ITUNESDB_REMEMBERS_PLACE);
}

#ifdef ITUNESDB_HAVE_ART
/* Covers. Finder scales each one into the ithmb files beside ArtworkDB for
   the stock firmware, as RGB565 frames ready for the screen; the tracks
   point at them by image id. */

#define ARTWORK_DIR     "/iPod_Control/Artwork"
#define ARTWORKDB_FILE  ARTWORK_DIR "/ArtworkDB"
/* where each track's cover is, by path hash */
#define ART_FILE        ROCKBOX_DIR "/itunesdb_art.dat"
#define ART_MAGIC       0x41494252 /* "RBIA" */
#define ART_VERSION     4   /* 2: the format of each entry's frame,
                               3: and its album's slide,
                               4: and the covers of the slides */
#define ART_FORMATS     4
#define FRAME_MAX       (200 * 200)  /* the iPod Video's largest covers */
#define SMALL_FRAME_MAX (100 * 100)  /* what they are checked against */
/* Cover Flow's slides, one an album, made after the import from the cover
   of one of its first tracks: column after column as PictureFlow draws
   them, each in a slot of its own behind a header that says which it is */
#define SLIDES_FILE     ROCKBOX_DIR "/itunesdb_slides.dat"
#define SLIDES_TODO     ROCKBOX_DIR "/itunesdb_slides.todo"
#define SLIDE_MAGIC     0x32494252 /* "RBI2": 2, of frames checked */
/* of a small frame, the large ones being unlike it: the next import tries
   again, as Finder may have written them by then */
#define SLIDE_UNSURE    0x33494252 /* "RBI3" */
#define SLIDE_SOURCES   3
#define SLIDE_PIXELS    (ITUNESDB_SLIDE_SIZE * ITUNESDB_SLIDE_SIZE)
#define SLIDE_SLOT      ((int)ALIGN_UP(sizeof(struct slide_header) \
                                 + SLIDE_PIXELS * sizeof(fb_data), 2048))

struct art_format {
    uint32_t format;    /* of the ithmb files */
    uint32_t width;     /* of their frames */
    uint32_t height;
};

struct art_header {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t nformats;
    struct art_format formats[ART_FORMATS];
    uint32_t nslides;   /* their keys and sources, after the entries */
};

struct art_entry {
    uint64_t hash;
    uint32_t offset;    /* in F<format>_<file>.ithmb */
    uint8_t file;
    uint8_t format;     /* in formats[] */
    uint16_t slide;     /* its album's, from 1; 0 for none */
};

/* a frame in an ithmb file */
struct art_frame {
    uint32_t offset;
    uint8_t file;       /* from 1; 0 for none */
    uint8_t format;     /* in formats[] */
    uint16_t unused;
};

/* What a slide can be made from: the covers of its album's first tracks.
   Finder lists a frame in ArtworkDB before it writes it, and where it reuses
   the place of an old one, another album's cover can be found there for
   long, so each large frame comes with a small one to check it against.
   Each slide also has a key, its album and its cover, 0 when unused. */
struct art_slide {
    struct { struct art_frame large, small; } source[SLIDE_SOURCES];
};

/* in front of each slide */
struct slide_header {
    uint32_t magic;
    uint16_t width, height;
    uint64_t key;
};

/* an ArtworkDB image */
struct art_image {
    uint32_t id;
    uint32_t offset;
    uint16_t file;
    uint16_t format;
    uint32_t source;    /* the size of the image Finder made it from */
    /* a smaller frame of it, to check that one against */
    uint32_t small_offset;
    uint16_t small_file; /* 0 for none */
    uint16_t small_format;
};

static struct art_header art_hdr;

/* the name of an ithmb file in an ArtworkDB mhod, which unlike iTunesDB
   says how it is encoded */
static void read_artwork_name(off_t pos, char *buf, size_t size)
{
    const unsigned char *p = db_record(pos, "mhod", 36);
    buf[0] = '\0';
    if (!p)
        return;
    size_t len = le32(p + 24);
    if (p[28] == 2)
    {
        size_t units = MIN(len / 2, (size - 1) / 3);
        p = db_get(pos + 36, units * 2);
        if (p)
            *utf16LEdecode(p, (unsigned char *)buf, units) = '\0';
    }
    else
    {
        len = MIN(len, size - 1);
        p = db_get(pos + 36, len);
        if (p)
        {
            memcpy(buf, p, len);
            buf[len] = '\0';
        }
    }
}

/* the index's slot of a format of frames, taken if it is new and there is
   room; -1 without */
static int art_format_of(uint32_t format, uint32_t width, uint32_t height)
{
    for (uint32_t i = 0; i < art_hdr.nformats; i++)
    {
        if (art_hdr.formats[i].format == format)
            return art_hdr.formats[i].width == width
                   && art_hdr.formats[i].height == height ? (int)i : -1;
    }
    if (art_hdr.nformats == ART_FORMATS)
        return -1;
    struct art_format *f = &art_hdr.formats[art_hdr.nformats];
    f->format = format;
    f->width = width;
    f->height = height;
    return art_hdr.nformats++;
}

/* The sizes of the ithmb files, each looked up once: in a sync Finder lists
   the frames of the covers before it writes them, and a frame past the end
   of its file left playback to decode the track's own JPEG, for seconds */
static struct { uint32_t format, file; off_t size; } ithmb_sizes[8];
static int ithmb_count;

static off_t ithmb_size(uint32_t format, uint32_t file)
{
    for (int i = 0; i < ithmb_count; i++)
    {
        if (ithmb_sizes[i].format == format && ithmb_sizes[i].file == file)
            return ithmb_sizes[i].size;
    }
    char path[64];
    snprintf(path, sizeof(path), ARTWORK_DIR "/F%lu_%lu.ithmb",
             (unsigned long)format, (unsigned long)file);
    int fd = open(path, O_RDONLY);
    off_t size = fd >= 0 ? ffilesize(fd) : 0;
    if (fd >= 0)
        close(fd);
    if (ithmb_count < (int)ARRAYLEN(ithmb_sizes))
    {
        ithmb_sizes[ithmb_count].format = format;
        ithmb_sizes[ithmb_count].file = file;
        ithmb_sizes[ithmb_count].size = size;
        ithmb_count++;
    }
    return size;
}

/* the largest frame in the mhii at pos that its file has */
static bool read_image(off_t pos, struct art_image *image)
{
    struct { uint32_t format, offset, size, width, height, file; } frames[8];
    int nframes = 0;
    char name[48];
    const unsigned char *p = db_record(pos, "mhii", 52);
    if (!p)
        return false;
    uint32_t header_len = le32(p + 4);
    int children = le32(p + 12);
    image->id = le32(p + 16);
    image->source = le32(p + 48);

    off_t child = pos + header_len;
    for (int i = 0; i < children && nframes < (int)ARRAYLEN(frames); i++)
    {
        p = db_record(child, "mhod", 14);
        if (!p)
            break;
        uint32_t len = le32(p + 4), total = le32(p + 8);
        if ((p[12] | p[13] << 8) == 2
            && (p = db_record(child + len, "mhni", 36)))
        {
            uint32_t mhni_len = le32(p + 4);
            frames[nframes].format = le32(p + 16);
            frames[nframes].offset = le32(p + 20);
            frames[nframes].size = le32(p + 24);
            frames[nframes].height = p[32] | p[33] << 8;
            frames[nframes].width = p[34] | p[35] << 8;
            /* ":F1029_1.ithmb" or "F1029_1.ithmb" */
            read_artwork_name(child + len + mhni_len, name, sizeof(name));
            const char *n = strrchr(name, '_');
            frames[nframes].file = n ? atoi(n + 1) : 0;
            if (frames[nframes].file > 0)
                nframes++;
        }
        if (total == 0)
            break;
        child += total;
    }

    /* the largest RGB565 frame of the image, which is 200x200 where Finder
       made one and else 100x100: it makes the large ones of some covers
       later in a sync, so the first image doesn't tell for all */
    int best = -1, small = -1;
    for (int i = 0; i < nframes; i++)
    {
        /* a cover that isn't square, 200x189 say, Finder pads with white
           to the square frame: that whole frame is the cover, as on the
           stock firmware, where it was left out for its size */
        if (frames[i].size != frames[i].width * frames[i].height * 2)
        {
            uint32_t side = MAX(frames[i].width, frames[i].height);
            while (side * side * 2 < frames[i].size && side < 1024)
                side++;
            if (side * side * 2 == frames[i].size)
                frames[i].width = frames[i].height = side;
        }
        uint32_t pixels = frames[i].width * frames[i].height;
        /* nor of a shape no cover has, which scaling couldn't take */
        if (frames[i].width < 2 || frames[i].height < 2
            || frames[i].width > 2 * frames[i].height
            || frames[i].height > 2 * frames[i].width)
            continue;
        if (frames[i].size != pixels * 2 || pixels > FRAME_MAX
            || (off_t)frames[i].offset + (off_t)frames[i].size
               > ithmb_size(frames[i].format, frames[i].file)
            || art_format_of(frames[i].format, frames[i].width,
                             frames[i].height) < 0)
            continue;
        if (best < 0 || pixels > frames[best].width * frames[best].height)
            best = i;
        if (small < 0 || pixels < frames[small].width * frames[small].height)
            small = i;
    }
    if (best < 0)
        return false;
    image->offset = frames[best].offset;
    image->file = frames[best].file;
    image->format = frames[best].format;
    image->small_file = 0;
    if (small != best
        && frames[small].width * frames[small].height <= SMALL_FRAME_MAX)
    {
        image->small_offset = frames[small].offset;
        image->small_file = frames[small].file;
        image->small_format = frames[small].format;
    }
    return true;
}

static int cmp_image(const void *a, const void *b)
{
    uint32_t x = ((const struct art_image *)a)->id;
    uint32_t y = ((const struct art_image *)b)->id;
    return (x > y) - (x < y);
}

static struct art_image *find_image(struct art_image *images, int count,
                                    uint32_t id)
{
    int lo = 0, hi = count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (images[mid].id == id)
            return &images[mid];
        if (images[mid].id < id)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* ArtworkDB's images, sorted by id; returns the handle of their memory */
static int read_images(struct art_image **images, int *count)
{
    int handle;
    const unsigned char *p;

    close(db.fd);
    db.fd = open(ARTWORKDB_FILE, O_RDONLY);
    db.cache_pos = db.cache_len = 0;
    if (db.fd < 0 || !(p = db_record(0, "mhfd", 24)))
        return -1;
    off_t pos = le32(p + 4);
    int sections = le32(p + 20);
    for (int i = 0; i < sections; i++)
    {
        p = db_record(pos, "mhsd", 14);
        if (!p)
            break;
        uint32_t header_len = le32(p + 4), total = le32(p + 8);
        if ((p[12] | p[13] << 8) == 1 && (p = db_record(pos + header_len, "mhli", 12)))
        {
            off_t item = pos + header_len + le32(p + 4);
            int n = le32(p + 8);
            if ((uint32_t)n > COUNT_MAX)
                return -1;
            handle = db_alloc(n * sizeof(struct art_image) + 1);
            if (handle <= 0)
                return -1;
            core_pin(handle);
            *images = core_get_data(handle);
            for (int k = 0; k < n; k++)
            {
                p = db_record(item, "mhii", 12);
                if (!p)
                    break;
                uint32_t item_total = le32(p + 8);
                if (read_image(item, &(*images)[*count]))
                    (*count)++;
                if (item_total == 0)
                    break;
                item += item_total;
                if ((k & 255) == 0)
                {
                    reading(1, k, n);
                    yield();
                }
            }
            qsort(*images, *count, sizeof(struct art_image), cmp_image);
            return handle;
        }
        if (total == 0)
            break;
        pos += total;
    }
    return -1;
}

/* the index in formats[] of a format of the ithmb files */
static uint8_t art_format_index(uint32_t format)
{
    for (uint32_t i = 0; i < art_hdr.nformats; i++)
    {
        if (art_hdr.formats[i].format == format)
            return i;
    }
    return 0;
}

/* the tracks with covers, by album and then in iTunesDB's order */
static int cmp_album_track(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    uint32_t ax = db.tracks[x].album, ay = db.tracks[y].album;
    if (ax != ay)
        return (ax > ay) - (ax < ay);
    return (x > y) - (x < y);
}

/* the old slides, sorted by key, as places in the slides file */
static const uint64_t *old_keys;
static int cmp_old_key(const void *a, const void *b)
{
    uint64_t x = old_keys[*(const uint32_t *)a];
    uint64_t y = old_keys[*(const uint32_t *)b];
    return (x > y) - (x < y);
}

/* the albums of the tracks in order, sorted by album */
static int count_albums(const uint32_t *order, int n)
{
    int albums = 0;
    for (int i = 0; i < n; i++)
    {
        if (i == 0 || db.tracks[order[i]].album != db.tracks[order[i - 1]].album)
            albums++;
    }
    return albums;
}

/* an album's slide: what it can be made of, from the first of its tracks */
static void set_slide_sources(struct art_slide *slide,
                              struct art_image *images, int art_count,
                              const uint32_t *order, int tracks)
{
    memset(slide, 0, sizeof(*slide));
    for (int i = 0; i < tracks && i < SLIDE_SOURCES; i++)
    {
        const struct art_image *image = find_image(images, art_count,
                                                   db.tracks[order[i]].art);
        slide->source[i].large.offset = image->offset;
        slide->source[i].large.file = image->file;
        slide->source[i].large.format = art_format_index(image->format);
        slide->source[i].small.offset = image->small_offset;
        slide->source[i].small.file = image->small_file;
        slide->source[i].small.format = art_format_index(image->small_format);
    }
}

/* The slides of Cover Flow, one for each album with a cover: in slide_of
   each track's from 1 (0 for none), in keys and sources what they are, by
   place in the slides file, of which there are *count. A slide the last
   index had, of the same album and the same cover in the same size, keeps
   its place and is not made again; itunesdb_make_slides() makes the others,
   in the places the albums gone left. order holds the n tracks with covers
   sorted by album, with room behind them for old_count places and as many
   bytes. */
static void assign_slides(struct art_image *images, int art_count,
                          const uint64_t *old, uint32_t old_count,
                          uint16_t *slide_of, uint32_t *order, int n,
                          uint64_t *keys, struct art_slide *sources,
                          uint32_t *count)
{
    /* the old ones by key, as yet not taken */
    uint32_t *by_key = order + n;
    uint8_t *taken = (uint8_t *)(by_key + old_count);
    for (uint32_t i = 0; i < old_count; i++)
    {
        keys[i] = 0;
        by_key[i] = i;
        taken[i] = 0;
    }
    old_keys = old;
    qsort(by_key, old_count, sizeof(*by_key), cmp_old_key);

    /* the ones kept first, so that a new one doesn't take their place */
    uint32_t used = 0, free_at = 0;
    for (int pass = 0; pass < 2; pass++)
    {
        for (int g = 0; g < n; )
        {
            const struct track *t = &db.tracks[order[g]];
            int end = g + 1;
            while (end < n && db.tracks[order[end]].album == t->album)
                end++;
            const struct art_image *image =
                find_image(images, art_count, t->art);
            const struct art_format *f =
                &art_hdr.formats[art_format_index(image->format)];
            /* with the frame's size: Finder makes the large ones later, and
               a slide from a small one is made again when it has */
            uint64_t key = (uint64_t)t->album << 32
                           | (uint32_t)(image->source * 31
                                        + f->width * f->height);
            int slot = -1;
            if (pass == 0)
            {
                /* the first of that key not taken */
                int lo = 0, hi = old_count;
                while (lo < hi)
                {
                    int mid = (lo + hi) / 2;
                    if (old[by_key[mid]] < key)
                        lo = mid + 1;
                    else
                        hi = mid;
                }
                for (; lo < (int)old_count && old[by_key[lo]] == key; lo++)
                {
                    if (!taken[by_key[lo]])
                    {
                        slot = by_key[lo];
                        break;
                    }
                }
            }
            else if (!slide_of[order[g]])
            {
                /* where an album gone was, or after the rest */
                while (free_at < old_count && taken[free_at])
                    free_at++;
                slot = free_at < old_count ? (int)free_at
                                           : (int)MAX(used, old_count);
                if (free_at < old_count)
                    free_at++;
            }
            if (slot >= 0)
            {
                if ((uint32_t)slot < old_count)
                    taken[slot] = 1;
                keys[slot] = key;
                set_slide_sources(&sources[slot], images, art_count,
                                  &order[g], end - g);
                used = MAX(used, (uint32_t)slot + 1);
                for (int k = g; k < end; k++)
                    slide_of[order[k]] = slot + 1;
            }
            g = end;
        }
    }
    /* the places of albums gone stay empty, but for those at the end */
    for (uint32_t i = 0; i < used && i < old_count; i++)
    {
        if (!taken[i])
            memset(&sources[i], 0, sizeof(sources[i]));
    }
    *count = used;
}

/* the keys of the slides of the index in ART_FILE, which an import is to
   keep, in memory; 0 without */
static int read_old_slides(uint32_t *count)
{
    struct art_header h;
    int handle = 0;
    *count = 0;
    int fd = open(ART_FILE, O_RDONLY);
    if (fd >= 0 && read(fd, &h, sizeof(h)) == sizeof(h)
        && h.magic == ART_MAGIC && h.version == ART_VERSION && h.nslides > 0
        && h.nslides <= COUNT_MAX && h.count <= COUNT_MAX)
    {
        off_t pos = sizeof(h) + (off_t)h.count * sizeof(struct art_entry);
        ssize_t size = h.nslides * sizeof(uint64_t);
        handle = db_alloc(size);
        if (handle > 0)
        {
            core_pin(handle);
            bool ok = lseek(fd, pos, SEEK_SET) == pos
                      && read(fd, core_get_data(handle), size) == size;
            core_unpin(handle);
            if (ok)
                *count = h.nslides;
            else
                handle = core_free(handle);
        }
    }
    if (fd >= 0)
        close(fd);
    return handle;
}

static void write_art_index(int old_handle, uint32_t old_count)
{
    struct art_image *images = NULL;
    int art_count = 0;
    int work = 0, slides = 0;
    memset(&art_hdr, 0, sizeof(art_hdr));
    ithmb_count = 0;
    int handle = read_images(&images, &art_count);
    if (handle <= 0 || art_hdr.nformats == 0)
        goto done;

    /* for each track its slide, and the tracks with covers by album */
    size_t slide_of_size = ALIGN_UP(db.count * sizeof(uint16_t), 4);
    work = db_alloc(slide_of_size + (db.count + old_count) * sizeof(uint32_t)
                    + ALIGN_UP(old_count, 4));
    if (work <= 0)
        goto done;
    core_pin(work);
    uint16_t *slide_of = core_get_data(work);
    uint32_t *order = (uint32_t *)((char *)slide_of + slide_of_size);
    int n = 0;
    for (int i = 0; i < db.count; i++)
    {
        const struct track *t = &db.tracks[i];
        slide_of[i] = 0;
        if (t->path && t->art && t->album
            && find_image(images, art_count, t->art))
            order[n++] = i;
    }
    qsort(order, n, sizeof(*order), cmp_album_track);

    /* the slides: as many as there were and albums at most */
    uint32_t max_slides = count_albums(order, n) + old_count;
    slides = db_alloc(MAX(max_slides, 1)
                      * (sizeof(uint64_t) + sizeof(struct art_slide)));
    if (slides <= 0)
        goto done;
    core_pin(slides);
    uint64_t *keys = core_get_data(slides);
    struct art_slide *sources = (struct art_slide *)(keys + max_slides);
    if (old_handle > 0)
        core_pin(old_handle);
    assign_slides(images, art_count,
                  old_handle > 0 ? core_get_data(old_handle) : NULL, old_count,
                  slide_of, order, n, keys, sources, &art_hdr.nslides);
    if (old_handle > 0)
        core_unpin(old_handle);

    if (!writer_open(&out, ART_FILE ".tmp"))
        goto done;
    art_hdr.magic = ART_MAGIC;
    art_hdr.version = ART_VERSION;
    writer_put(&out, &art_hdr, sizeof(art_hdr));
    /* by path hash, which db.by_hash already has them in */
    uint32_t count = 0;
    for (int i = 0; i < db.count; i++)
    {
        struct track *t = &db.tracks[db.by_hash[i]];
        struct art_image *image = t->path && t->art ?
            find_image(images, art_count, t->art) : NULL;
        if (!image)
            continue;
        struct art_entry entry = {
            t->st.hash, image->offset, image->file,
            art_format_index(image->format), slide_of[db.by_hash[i]]
        };
        writer_put(&out, &entry, sizeof(entry));
        count++;
    }
    writer_put(&out, keys, art_hdr.nslides * sizeof(*keys));
    writer_put(&out, sources, art_hdr.nslides * sizeof(*sources));
    /* the count, now that it is known */
    art_hdr.count = count;
    writer_flush(&out);
    lseek(out.fd, 0, SEEK_SET);
    writer_put(&out, &art_hdr, sizeof(art_hdr));
    if (writer_close(&out))
    {
        /* the slides to make before the index is complete */
        close(open(SLIDES_TODO, O_WRONLY | O_CREAT | O_TRUNC, 0666));
        rename(ART_FILE ".tmp", ART_FILE);
    }
    logf("itunesdb: %lu covers, %lu slides", (unsigned long)count,
         (unsigned long)art_hdr.nslides);

done:
    if (slides > 0)
    {
        core_unpin(slides);
        core_free(slides);
    }
    if (work > 0)
    {
        core_unpin(work);
        core_free(work);
    }
    if (handle > 0)
    {
        core_unpin(handle);
        core_free(handle);
    }
}

/* The index lives in memory, and each frame is read once for all the sizes
   a theme shows it at, into a buffer of its own */

static struct index art_index = { 0, 0, sizeof(struct art_entry) };
static struct index slides_index = { 0, 0, sizeof(uint64_t) }; /* the keys */
static struct art_header index_hdr;
static unsigned char frame_raw[FRAME_MAX * sizeof(fb_data) + 2 * READ_ALIGN]
    STORAGE_ALIGN_ATTR;
static const fb_data *frame_buf;
static char frame_path[64];     /* where the frame is from, "" for none */
static uint32_t frame_offset;
/* one at a time: PictureFlow takes covers while playback does, the slides
   are made meanwhile, and the frame read last is shared */
static struct mutex art_lock;
static bool art_lock_ready;

static void forget_art(void)
{
    forget_index(&art_index);
    forget_index(&slides_index);
    frame_path[0] = '\0';
}

/* the slides' keys after the entries of the index loaded */
static void load_slides_index(void)
{
    if (art_index.handle <= 0 || slides_index.handle > 0
        || index_hdr.nslides == 0 || index_hdr.nslides > COUNT_MAX)
        return;
    int fd = open(ART_FILE, O_RDONLY);
    if (fd < 0)
        return;
    off_t pos = sizeof(index_hdr)
                + (off_t)index_hdr.count * sizeof(struct art_entry);
    ssize_t size = index_hdr.nslides * sizeof(uint64_t);
    int handle = db_alloc(size);
    if (handle > 0)
    {
        core_pin(handle);
        bool ok = lseek(fd, pos, SEEK_SET) == pos
                  && read(fd, core_get_data(handle), size) == size;
        core_unpin(handle);
        if (ok)
        {
            slides_index.handle = handle;
            slides_index.count = index_hdr.nslides;
        }
        else
            core_free(handle);
    }
    close(fd);
}

/* the key of slide i (from 0) of the index, 0 without one */
static uint64_t slide_key(uint32_t i)
{
    if (slides_index.handle <= 0 || i >= slides_index.count)
        return 0;
    return ((const uint64_t *)core_get_data(slides_index.handle))[i];
}

/* where the frame in the ithmb file of a format and number is */
static bool art_place(uint8_t format, uint8_t file, uint32_t offset,
                      struct itunesdb_art *art)
{
    if (format >= index_hdr.nformats || format >= ART_FORMATS)
        return false;
    const struct art_format *f = &index_hdr.formats[format];
    /* as read_image() takes them, for an index that was damaged */
    if (f->width < 2 || f->height < 2 || f->width > LCD_WIDTH * 2
        || f->height > LCD_WIDTH * 2)
        return false;
    if ((size_t)snprintf(art->path, sizeof(art->path),
                         ARTWORK_DIR "/F%lu_%lu.ithmb",
                         (unsigned long)f->format,
                         (unsigned long)file) >= sizeof(art->path))
        return false;
    art->offset = offset;
    art->width = f->width;
    art->height = f->height;
    art->slide = 0;
    return true;
}

bool itunesdb_find_art(const char *path, struct itunesdb_art *art)
{
    struct art_entry entry;
    if (art_index.handle <= 0
        || !find_in_index(&art_index, itunesdb_path_hash(path), &entry)
        || !art_place(entry.format, entry.file, entry.offset, art))
        return false;
    art->slide = entry.slide;
    return true;
}

/* the frame read into raw, from the sector it starts in */
static const fb_data *read_frame(const struct itunesdb_art *art,
                                 unsigned char *raw, size_t room)
{
    size_t size = art->width * art->height * sizeof(fb_data);
    off_t start = art->offset & ~(off_t)(READ_ALIGN - 1);
    size_t skip = art->offset - start;
    size_t want = (skip + size + READ_ALIGN - 1) & ~(size_t)(READ_ALIGN - 1);
    if (want > room)
        return NULL;
    int fd = open(art->path, O_RDONLY);
    /* the last frame of a file may end before the sector does */
    bool ok = fd >= 0 && lseek(fd, start, SEEK_SET) == start
              && read(fd, raw, want) >= (ssize_t)(skip + size);
    if (fd >= 0)
        close(fd);
    return ok ? (const fb_data *)(raw + skip) : NULL;
}

/* the frame, read only when it isn't the one read last */
static const fb_data *get_frame(const struct itunesdb_art *art)
{
    if (art->width * art->height > FRAME_MAX)
        return NULL;
    if (frame_buf && frame_offset == art->offset
        && !strcmp(frame_path, art->path))
        return frame_buf;

    frame_path[0] = '\0';
    frame_buf = read_frame(art, frame_raw, sizeof(frame_raw));
    if (!frame_buf)
        return NULL;
    strmemccpy(frame_path, art->path, sizeof(frame_path));
    frame_offset = art->offset;
    return frame_buf;
}

/* RGB565 spread over 32 bits, green up in the top half, so that there is
   room above each channel to scale all three with one multiply */
#define SPREAD_MASK 0x07e0f81f
#define SPREAD(p)   (((uint32_t)(p) << 16 | (p)) & SPREAD_MASK)
#define UNSPREAD(x) ((fb_data)((x) >> 16 | (x)))

/* from a towards b by f in 32nds, every channel at once */
static inline uint32_t lerp32(uint32_t a, uint32_t b, unsigned f)
{
    return ((a * (32 - f) + b * f) >> 5) & SPREAD_MASK;
}

/* the pixel between four */
static inline fb_data bilerp(fb_data a, fb_data b, fb_data c, fb_data d,
                             unsigned fx, unsigned fy)
{
    uint32_t top = lerp32(SPREAD(a), SPREAD(b), fx);
    uint32_t bottom = lerp32(SPREAD(c), SPREAD(d), fx);
    return UNSPREAD(lerp32(top, bottom, fy));
}

#define ROW_MAX 320 /* the widest cover drawn */

/* one row of the frame scaled across, spread */
static void scale_row(const fb_data *src, uint32_t *dst, int w,
                      const uint16_t *xs, const uint8_t *fxs)
{
    for (int x = 0; x < w; x++)
        dst[x] = lerp32(SPREAD(src[xs[x]]), SPREAD(src[xs[x] + 1]), fxs[x]);
}

/* Bilinear, a row of the frame across once and then the rows down: when
   scaling up, several rows on the screen come from the same two rows of
   the frame */
static void scale_frame(const fb_data *frame, int fw, int fh,
                        fb_data *out, int w, int h, bool columns)
{
    static uint16_t xs[ROW_MAX];
    static uint8_t fxs[ROW_MAX];
    static uint32_t rows[2][ROW_MAX];

    /* sample at pixel centres in 16.16 steps; this CPU has no divide, so
       nothing in the loops may divide */
    long xstep = ((long)fw << 16) / w, ystep = ((long)fh << 16) / h;
    long sx = xstep / 2 - 0x8000;
    for (int x = 0; x < w; x++, sx += xstep)
    {
        long cx = sx < 0 ? 0 : sx;
        int x0 = cx >> 16;
        if (x0 >= fw - 1)
        {
            /* the last column: from it to itself */
            xs[x] = fw - 2 < 0 ? 0 : fw - 2;
            fxs[x] = fw > 1 ? 31 : 0;
        }
        else
        {
            xs[x] = x0;
            fxs[x] = (cx >> 11) & 31;
        }
    }

    uint32_t *upper = rows[0], *lower = rows[1];
    int have_upper = -1, have_lower = -1;  /* which frame rows they hold */
    long sy = ystep / 2 - 0x8000;
    for (int y = 0; y < h; y++, sy += ystep)
    {
        long cy = sy < 0 ? 0 : sy;
        int y0 = cy >> 16, y1 = MIN(y0 + 1, fh - 1);
        unsigned fy = (cy >> 11) & 31;

        if (y0 == have_lower)
        {
            /* down by one row: the lower one becomes the upper */
            uint32_t *t = upper;
            upper = lower;
            lower = t;
            have_upper = have_lower;
            have_lower = -1;
        }
        if (have_upper != y0)
        {
            scale_row(frame + y0 * fw, upper, w, xs, fxs);
            have_upper = y0;
        }
        if (have_lower != y1)
        {
            scale_row(frame + y1 * fw, lower, w, xs, fxs);
            have_lower = y1;
        }
        /* by rows, or column after column for Cover Flow */
        fb_data *o = columns ? out + y : out + y * w;
        const int step = columns ? h : 1;
        for (int x = 0; x < w; x++, o += step)
            *o = UNSPREAD(lerp32(upper[x], lower[x], fy));
    }
}

static void lock_art(void)
{
    if (!art_lock_ready)
    {
        mutex_init(&art_lock);
        art_lock_ready = true;
    }
    mutex_lock(&art_lock);
}

int itunesdb_load_art(const struct itunesdb_art *art, struct bitmap *bm,
                      int max_size)
{
    int fw = art->width, fh = art->height;
    int w = bm->width, h = bm->height;
    if (fw <= 0 || fh <= 0 || w <= 0 || h <= 0)
        return -1;

    /* the largest that fits, up or down, keeping the shape */
    if (w * fh > h * fw)
        w = (h * fw + fh / 2) / fh;
    else
        h = (w * fh + fw / 2) / fw;
    int out_size = w * h * sizeof(fb_data);
    if (w > ROW_MAX || out_size + (int)sizeof(struct bitmap) > max_size)
        return -1;

    lock_art();
    const fb_data *frame = get_frame(art);
    if (frame && w == fw && h == fh)
        memcpy(bm->data, frame, out_size);
    else if (frame)
    {
        cpu_boost(true);
        scale_frame(frame, fw, fh, (fb_data *)bm->data, w, h, false);
        cpu_boost(false);
    }
    mutex_unlock(&art_lock);
    if (!frame)
        return -1;

    bm->width = w;
    bm->height = h;
#if LCD_DEPTH > 1
    bm->format = FORMAT_NATIVE;
    bm->maskdata = NULL;
#endif
#ifdef HAVE_LCD_COLOR
    bm->alpha_offset = 0;
#endif
    return out_size;
}

int itunesdb_load_slide(const struct itunesdb_art *art, void *buf,
                        int *width, int *height)
{
    struct slide_header head;
    uint64_t key = art->slide > 0 ? slide_key(art->slide - 1) : 0;
    if (!key)
        return -1;
    int fd = open(SLIDES_FILE, O_RDONLY);
    if (fd < 0)
        return -1;
    off_t pos = (off_t)(art->slide - 1) * SLIDE_SLOT;
    bool ok = lseek(fd, pos, SEEK_SET) == pos
              && read(fd, &head, sizeof(head)) == sizeof(head)
              && (head.magic == SLIDE_MAGIC || head.magic == SLIDE_UNSURE)
              && head.key == key
              && head.width <= ITUNESDB_SLIDE_SIZE
              && head.height <= ITUNESDB_SLIDE_SIZE;
    ssize_t size = ok ? head.width * head.height * sizeof(fb_data) : 0;
    ok = ok && read(fd, buf, size) == size;
    close(fd);
    if (!ok)
        return -1;
    *width = head.width;
    *height = head.height;
    return size;
}

/* a slot: its header, and the slide behind it with the rest blank */
static unsigned char slide_slot[SLIDE_SLOT] STORAGE_ALIGN_ATTR;
static unsigned char small_raw[SMALL_FRAME_MAX * sizeof(fb_data)
                               + 2 * READ_ALIGN] STORAGE_ALIGN_ATTR;

/* the mean difference in brightness, about 0 to 187, from which frames are
   of different covers: of Finder's, those of one were 0 to 10 and of two
   12 to 77 */
#define FRAMES_DIFFER 11

static inline int frame_luma(fb_data p)
{
    return (p >> 11) * 2 + ((p >> 5) & 63) + (p & 31) * 2;
}

/* whether a large frame and a small one are of one cover, at a grid of the
   small one's pixels */
static bool frames_agree(const fb_data *large, int lw, int lh,
                         const fb_data *small, int sw, int sh)
{
    const long xstep = ((long)lw << 16) / sw, ystep = ((long)lh << 16) / sh;
    long sum = 0;
    int points = 0;
    for (int y = 0; y < sh; y += 4)
    {
        const fb_data *s = small + y * sw;
        const fb_data *l0 = large + MIN(y * ystep >> 16, lh - 2) * lw;
        const fb_data *l1 = l0 + lw;
        for (int x = 0; x < sw; x += 4)
        {
            int lx = MIN(x * xstep >> 16, lw - 2);
            int luma = (frame_luma(l0[lx]) + frame_luma(l0[lx + 1])
                        + frame_luma(l1[lx]) + frame_luma(l1[lx + 1])) / 4;
            int diff = frame_luma(s[x]) - luma;
            sum += diff < 0 ? -diff : diff;
            points++;
        }
    }
    return sum < (long)points * FRAMES_DIFFER;
}

/* The cover to make a slide of, into art, with art_lock held: the first
   large frame like its small one or with none to tell, else the first small
   one, as one Finder hasn't written is more often the large, and then
   *unsure. NULL without. */
static const fb_data *slide_frame(const struct art_slide *slide,
                                  struct itunesdb_art *art, bool *unsure)
{
    *unsure = false;
    for (int i = 0; i < SLIDE_SOURCES; i++)
    {
        const struct art_frame *f = &slide->source[i].large;
        const struct art_frame *g = &slide->source[i].small;
        struct itunesdb_art small;
        if (!f->file || !art_place(f->format, f->file, f->offset, art))
            break;
        const fb_data *frame = get_frame(art);
        if (!frame)
            continue;
        if (!g->file || !art_place(g->format, g->file, g->offset, &small))
            return frame;
        const fb_data *s = read_frame(&small, small_raw, sizeof(small_raw));
        if (!s || frames_agree(frame, art->width, art->height,
                               s, small.width, small.height))
            return frame;
    }
    const struct art_frame *g = &slide->source[0].small;
    if (!g->file || !art_place(g->format, g->file, g->offset, art))
        return NULL;
    *unsure = true;
    return read_frame(art, small_raw, sizeof(small_raw));
}

/* makes slide i of the index in the slides file, which is *size long so
   far, of the sources read from the index: the slide first, then its header,
   so that a slide being made is never taken for the one it will be */
static bool make_slide(int fd, int index_fd, uint32_t i, uint64_t key,
                       off_t *size)
{
    struct art_slide slide;
    struct itunesdb_art art;
    off_t source = sizeof(index_hdr)
                   + (off_t)index_hdr.count * sizeof(struct art_entry)
                   + (off_t)index_hdr.nslides * sizeof(uint64_t)
                   + (off_t)i * sizeof(slide);
    if (lseek(index_fd, source, SEEK_SET) != source
        || read(index_fd, &slide, sizeof(slide)) != sizeof(slide))
        return false;

    struct slide_header *head = (struct slide_header *)slide_slot;
    fb_data *pixels = (fb_data *)(slide_slot + sizeof(*head));
    memset(slide_slot, 0, sizeof(slide_slot));

    /* the places before it blank where the file is shorter: slides that
       couldn't be made */
    off_t pos = (off_t)i * SLIDE_SLOT;
    if (lseek(fd, MIN(*size, pos), SEEK_SET) < 0)
        return false;
    for (; *size < pos; *size += SLIDE_SLOT)
    {
        if (write(fd, slide_slot, SLIDE_SLOT) != SLIDE_SLOT)
            return false;
    }

    lock_art();
    int w = 0, h = 0;
    bool unsure;
    const fb_data *frame = slide_frame(&slide, &art, &unsure);
    if (frame)
    {
        /* the largest that fits the slide, keeping the shape */
        w = h = ITUNESDB_SLIDE_SIZE;
        if (art.width > art.height)
            h = (w * art.height + art.width / 2) / art.width;
        else
            w = (h * art.width + art.height / 2) / art.height;
        scale_frame(frame, art.width, art.height, pixels, w, h, true);
    }
    mutex_unlock(&art_lock);
    if (!frame || write(fd, slide_slot, SLIDE_SLOT) != SLIDE_SLOT)
        return false;
    *size = MAX(*size, pos + (off_t)SLIDE_SLOT);
    head->magic = unsure ? SLIDE_UNSURE : SLIDE_MAGIC;
    head->width = w;
    head->height = h;
    head->key = key;
    return lseek(fd, pos, SEEK_SET) == pos
           && write(fd, head, sizeof(*head)) == sizeof(*head);
}

bool itunesdb_slides_todo(void)
{
    struct art_header h = { 0 };
    int fd = open(ART_FILE, O_RDONLY);
    if (fd >= 0)
    {
        read(fd, &h, 2 * sizeof(uint32_t));
        close(fd);
    }
    return file_exists(SLIDES_TODO)
           || (h.magic == ART_MAGIC && h.version != ART_VERSION);
}

void itunesdb_make_slides(volatile int *done, volatile int *total,
                          bool (*stop)(void))
{
    if (slides_index.handle <= 0)
    {
        remove(SLIDES_TODO);
        return;
    }
    int index_fd = open(ART_FILE, O_RDONLY);
    int fd = open(SLIDES_FILE, O_RDWR | O_CREAT, 0666);
    if (fd < 0 || index_fd < 0)
    {
        if (fd >= 0)
            close(fd);
        if (index_fd >= 0)
            close(index_fd);
        return;
    }
    /* whole slots: a cut short one is made again */
    off_t size = ALIGN_DOWN(ffilesize(fd), SLIDE_SLOT);
    uint32_t count = slides_index.count;
    bool complete = true;
    if (total)
        *total = count;
    cpu_boost(true);
    for (uint32_t i = 0; i < count; i++)
    {
        struct slide_header head;
        if (done)
            *done = i;
        if (stop && stop())
        {
            complete = false;
            break;
        }
        uint64_t key = slide_key(i);
        if (!key)
            continue;
        /* made already, at the last import or before it was cut short */
        off_t pos = (off_t)i * SLIDE_SLOT;
        if (pos + (off_t)SLIDE_SLOT <= size && lseek(fd, pos, SEEK_SET) == pos
            && read(fd, &head, sizeof(head)) == sizeof(head)
            && head.magic == SLIDE_MAGIC && head.key == key)
            continue;
        if (!make_slide(fd, index_fd, i, key, &size))
            complete = false;
        yield();
    }
    cpu_boost(false);
    /* the slides of albums gone at the end */
    off_t end = (off_t)count * (off_t)SLIDE_SLOT;
    if (complete && size > end)
        ftruncate(fd, end);
    close(index_fd);
    close(fd);
    if (complete)
        remove(SLIDES_TODO);
}
#endif /* ITUNESDB_HAVE_ART */

int itunesdb_import_tracks(bool synced, volatile int *done, volatile int *total)
{
    /* a sync may have changed the name */
    device_name_read = false;

    int count = 0;
    read_done = done;
    read_total = total;
    if (synced)
        metadata_begin();
    else
        itunesdb_forget_metadata();
    forget_index(&flags_index);
    remove(FLAGS_FILE);
    remove(GAINS_FILE);
#ifdef ITUNESDB_HAVE_ART
    /* the old covers point into ithmb files Finder has rewritten; their
       slides are kept where they are the same */
    uint32_t old_slides_count;
    int old_slides_handle = read_old_slides(&old_slides_count);
    forget_art();
    remove(ART_FILE);
#endif
    bool loaded = db_load();
    metadata_end();
    if (loaded)
    {
        count = db.count;
        write_flags();
        write_gains();
        if (synced)
        {
            write_playlists(playlists_list);
            write_pending();
        }
#ifdef ITUNESDB_HAVE_ART
        /* last, as it reads ArtworkDB through the same cache */
        write_art_index(old_slides_handle, old_slides_count);
#endif
    }
#ifdef ITUNESDB_HAVE_ART
    if (old_slides_handle > 0)
        core_free(old_slides_handle);
#endif
    db_unload();
    itunesdb_load_tracks();
    read_done = read_total = NULL;
    return count;
}

bool itunesdb_load_tracks(void)
{
    struct flags_header hdr = { 0 };
    uint32_t size, mtime;
    load_index(&flags_index, FLAGS_FILE, &hdr, sizeof(hdr),
               FLAGS_MAGIC, FLAGS_VERSION);
    /* what an older firmware didn't note */
    if (!itunesdb_gains_ready())
        return false;
#ifdef ITUNESDB_HAVE_ART
    load_index(&art_index, ART_FILE, &index_hdr, sizeof(index_hdr),
               ART_MAGIC, ART_VERSION);
    load_slides_index();
    /* an index of covers of an older version: made again with the flags,
       once, as it isn't there or is of this one after */
    if (art_index.handle <= 0)
    {
        struct art_header old = { 0 };
        int fd = open(ART_FILE, O_RDONLY);
        if (fd >= 0)
        {
            read(fd, &old, 2 * sizeof(uint32_t));
            close(fd);
        }
        if (old.magic == ART_MAGIC && old.version != ART_VERSION)
            return false;
    }
#endif
    if (flags_index.handle > 0 || !db_stamp(&size, &mtime))
        return true;
    /* none with flags, from this iTunesDB? */
    return hdr.magic == FLAGS_MAGIC && hdr.version == FLAGS_VERSION
           && hdr.db_size == size && hdr.db_mtime == mtime;
}
