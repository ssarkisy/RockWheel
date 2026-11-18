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
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "ata_idle_notify.h"
#include "file.h"
#include "dir.h"
#include "mv.h"
#include "core_alloc.h"
#include "rbunicode.h"
#include "string-extra.h"
#include "pathfuncs.h"
#include "settings.h"
#include "misc.h"
#include "file_write.h"
#include "on_the_go.h"
#include "applimits.h"
#include "rbpaths.h"
#include "tagcache.h"
#include <ctype.h>
#include <errno.h>
#include "logf.h"
#include "audio.h"
#include "lcd.h"
#include "metadata.h"
#include "fixedpoint.h"
#include "itunesdb.h"
#include "covers_table.h"
#include "synced_covers.h"
#include "health_log.h"
#include "crc32.h"
#include "sortkey.h"
#include "mh_walk.h"
#ifdef ITUNESDB_HAVE_ART
#include "skin_engine/cover_palette.h"
#include "skin_engine/skin_engine.h"
#ifdef HAVE_TAGCACHE
#include "album_header.h"
#include "list_covers.h"
#include "skin_engine/menu_cover.h"
#endif
#endif

#ifdef HAVE_DIRCACHE
#include "dircache.h"
#endif
#include "smart_playlists.h"
#include "library_playlists.h"

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

/* No memory for iTunesDB's songs (db_load()), as when the boot takes it all
   for a moment (the menus' parse in tagtree_init()): the boot's pass of the
   playlists tries again in a moment (itunesdb_import_playlists()) */
static bool memory_short;


/* what the last import was made from, and the tracks in iTunesDB order with
   the counts they had then, which Play Counts entries are made against */
#define STATE_FILE      ROCKBOX_DIR "/itunesdb.dat"
#define STATE_MAGIC     0x54494252 /* "RBIT" */
/* A new version re-imports the same iTunesDB leaving the counts (counts_kept);
   the header's start is fixed for good. */
#define STATE_VERSION   1
#define STATE_OLD_FILE  ITUNESDB_OLD_STATE
/* the database's commit id when the last import had gone through all of it,
   then which pass that was and the sum of the dates it set (struct imported):
   an older firmware wrote and reads the commit id alone */
#define IMPORTED_FILE   ROCKBOX_DIR "/itunesdb_imported.dat"
/* 2: the pass sets every song's dates from iTunesDB (update_entry()) */
#define IMPORT_PASS     2
/* how far an import after a sync has got, until it's done: it waited once for
   a record of the plays Finder hasn't had, or its pass has begun */
#define PROGRESS_FILE   ROCKBOX_DIR "/itunesdb_progress.dat"
#define IMPORT_WAITED   0x01
#define IMPORT_PASSING  0x02
/* the playlists made from iTunesDB, removed again on the next import */
#define PLAYLISTS_FILE  ITUNESDB_PLAYLISTS_LIST

/* the folder where Finder keeps the music */
#define MUSIC_DIR       "/iPod_Control/Music"

/* media types, which a track can have more than one of */
#define MEDIA_PODCAST   0x04
#define MEDIA_AUDIOBOOK 0x08
/* a movie, a music video or a TV show, which Rockbox can't play */
#define MEDIA_VIDEO     (0x02 | 0x20 | 0x40)

/* A track with a cover but, in a database without image ids, only its 64-bit
   dbid, by which ArtworkDB's image names it: its art until the images are
   read, which give it the image's id */
#define ART_BY_SONG 0xffffffffu
static struct {
    int handle;
    int capacity;
    uint64_t *dbid;     /* of each track, by its index */
    uint32_t *order;    /* the tracks with only that, by it */
    int count;
} art_songs;

static void forget_art_songs(void)
{
    if (art_songs.handle > 0)
    {
        core_unpin(art_songs.handle);
        core_free(art_songs.handle);
    }
    memset(&art_songs, 0, sizeof(art_songs));
}

static void note_art_song(int track, uint64_t dbid)
{
    /* -1 once there was no room, which isn't asked for again each track */
    if (art_songs.handle < 0)
        return;
    if (art_songs.handle == 0)
    {
        art_songs.handle = db_alloc(art_songs.capacity
                                    * (sizeof(uint64_t) + sizeof(uint32_t)));
        if (art_songs.handle <= 0)
        {
            art_songs.handle = -1;
            return;
        }
        core_pin(art_songs.handle);
        art_songs.dbid = core_get_data(art_songs.handle);
        art_songs.order = (uint32_t *)(art_songs.dbid + art_songs.capacity);
    }
    art_songs.dbid[track] = dbid;
}

/* Records start anywhere in a sector. Read from where the sector starts
   into an aligned buffer, the storage driver takes it all in one go;
   otherwise it goes a sector at a time through a buffer of its own */
#define READ_ALIGN      2048  /* the largest sector there is */
#define CACHE_SIZE      65536 /* a multiple of READ_ALIGN */
/* reads of files read through: 1 KB reads of the state and ids files took
   400 disk commands a sync */
#define READ_BLOCK      16384
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
#define STATE_STAMP_SIZE offsetof(struct state_header, count)
_Static_assert(offsetof(struct state_header, magic) == 0
               && offsetof(struct state_header, version) == 4
               && offsetof(struct state_header, db_size) == 8
               && offsetof(struct state_header, db_mtime) == 12,
               "the start of itunesdb.dat's header is kept for good");

struct state_entry {
    uint64_t hash;
    uint32_t playcount;
    uint8_t rating;  /* 0..100, 20 a star */
    uint8_t media;   /* the media type's low byte, which has the video kinds */
    uint8_t flags;   /* ITUNESDB_* */
    uint8_t played;  /* a podcast's or a book's mark on the Mac: 1 played, 2 not */
};

struct track {
    uint32_t id;     /* what playlists refer to */
    uint32_t path;   /* offset in the path pool, 0 for none */
    uint32_t art;    /* the ArtworkDB image id of its cover, 0 for none */
    uint32_t album;  /* its album and the album's artist hashed, 0 for none */
    union {
        uint32_t pos;       /* of its mhit in iTunesDB, as it's read */
        uint32_t notes_end; /* then where the notes' file ends after its
                               notes: where the one before it ends, if it
                               has none */
    };
    uint32_t sound_check; /* Finder's, 0 for none */
    uint32_t lastplayed;  /* Mac time, 0 for never */
    uint32_t bookmark;    /* a podcast's or a book's place on the Mac, ms */
    uint32_t added;       /* as tag_dateadded, 0 for none */
    uint32_t released;    /* track_released()'s */
    uint32_t digest;      /* of its tags (track_digest()) as the database has
                             them after this import, 0 for not known */
    struct state_entry st;
    uint64_t dbid;   /* the song's, the same under another path after a sync */
};

static struct {
    int fd;
    unsigned char *cache;
    off_t cache_pos;
    int cache_len;
    struct track *tracks;
    int count;
    int shows;
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

/* the bytes of the string of an iTunesDB mhod, at least 40 of it read: as
   many as it says, but within its record, which its total length ends */
static uint32_t mhod_string_size(const unsigned char *p)
{
    const uint32_t total = le32(p + 8);
    return total > 40 ? MIN(le32(p + 28), total - 40) : 0;
}

/* seconds from 1904 to 1970, for the times of iTunesDB */
#define MAC_EPOCH_OFFSET 2082844800u

/* tag_date of a track: its mhit's date at 0x8C, Mac seconds, if of its year
   at 52 or there's no year; else 0. The day as Music shows it, without a time
   zone: the iTunes Store's dates are at midnight in California, 07:00 or
   08:00, the others at 00:00 or 12:00, each that day. */
static uint32_t track_date(uint32_t year, uint32_t released)
{
    if (released == 0 || released == 0xffffffff)
        return 0;
    /* the date of a day counted from 1 March of the year 0, as Howard
       Hinnant's civil_from_days() has it: 1 January 1904 is day 695361 */
    const uint32_t days = released / 86400 + 695361;
    const uint32_t era = days / 146097, of_era = days % 146097;
    const uint32_t year_of_era = (of_era - of_era / 1460 + of_era / 36524
                                  - of_era / 146096) / 365;
    const uint32_t of_year = of_era - (365 * year_of_era + year_of_era / 4
                                       - year_of_era / 100);
    const uint32_t from_march = (5 * of_year + 2) / 153;
    const uint32_t day = of_year - (153 * from_march + 2) / 5 + 1;
    const uint32_t month = from_march < 10 ? from_march + 3 : from_march - 9;
    const uint32_t y = era * 400 + year_of_era + (month <= 2);
    if (y < 1000 || y > 2999 || (year != 0 && year != y))
        return 0;
    return y * 10000 + month * 100 + day;
}

/* A track's tag_date and tag_year in one, as add_metadata() takes them from
   its mhit: the date, whose year is the year's; without one the year alone,
   below this; 0 for neither */
#define RELEASED_DATE 10000
static uint32_t track_released(const unsigned char *mhit)
{
    const uint32_t year = MIN(le32(mhit + 52), 0xffff);
    const uint32_t date = track_date(year, le32(mhit + 0x8c));
    return date ? date : MIN(year, RELEASED_DATE - 1);
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
    size_t units = MIN(mhod_string_size(p) / 2, (size - 1) / 3);
    p = db_get(pos + 40, units * 2);
    if (!p)
        return false;
    /* not half of a pair: the decoder would take both, 4 bytes for one */
    if (units > 0 && p[units * 2 - 1] >= 0xd8 && p[units * 2 - 1] < 0xe0)
        units--;
    *utf16LEdecode(p, (unsigned char *)buf, units) = '\0';
    return true;
}

/* the mhod at pos has a string that isn't empty */
static bool mhod_filled(off_t pos)
{
    const unsigned char *p = db_record(pos, "mhod", 40);
    return p && mhod_string_size(p) >= 2;
}

/* the string of the mhod at pos hashed as it is, 0 for an empty one */
static uint32_t mhod_hash(off_t pos)
{
    const unsigned char *p = db_record(pos, "mhod", 40);
    if (!p)
        return 0;
    size_t len = MIN(mhod_string_size(p), 1024);
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
    /* FNV-1a, ignoring ASCII case like FAT does, and a separator doubled, as
       the browser gives one back in a folder ("/Playlists//Sub") */
    uint64_t hash = 0xcbf29ce484222325ull;
    path = strip_volume(path);
    for (const unsigned char *p = (const unsigned char *)path; *p; p++)
    {
        unsigned char c = *p;
        if (c == '/' && p > (const unsigned char *)path && p[-1] == '/')
            continue;
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        hash = (hash ^ c) * 0x100000001b3ull;
    }
    return hash;
}

/* the size and time of a file of a folder, as the disk has them */
static bool file_stamp_in(const char *folder, const char *name, uint32_t *size,
                          uint32_t *mtime)
{
    DIR *dir = opendir(folder);
    if (!dir)
        return false;
    bool found = false;
    struct dirent *entry;
    while (!found && (entry = readdir(dir)))
    {
        if (strcasecmp(entry->d_name, name))
            continue;
        struct dirinfo info = dir_get_info(dir, entry);
        *size = info.size;
        *mtime = info.mtime;
        found = true;
    }
    closedir(dir);
    return found;
}

/* of iTunesDB's folder, whose iTunesDB changes with every sync */
static bool dir_file_stamp(const char *name, uint32_t *size, uint32_t *mtime)
{
    return file_stamp_in(ITUNESDB_DIR, name, size, mtime);
}

/* iTunesDB was found in this boot, whether or not it was read then */
static bool db_seen;

static bool db_stamp(uint32_t *size, uint32_t *mtime)
{
    const bool found = dir_file_stamp("iTunesDB", size, mtime);
    if (found)
        db_seen = true;
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

static bool read_state_header(const char *file, struct state_header *hdr)
{
    int fd = open(file, O_RDONLY);
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
    return !read_state_header(STATE_FILE, &hdr) || hdr.db_size != size
        || hdr.db_mtime != mtime;
}

bool itunesdb_stamp(uint32_t *size, uint32_t *mtime)
{
    return db_stamp(size, mtime);
}

bool itunesdb_imported_stamp(uint32_t *size, uint32_t *mtime)
{
    struct state_header hdr;
    int fd = open(STATE_FILE, O_RDONLY);
    if (fd < 0 && tagcache_restoring())
        fd = open(STATE_OLD_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    const bool ok = read(fd, &hdr, STATE_STAMP_SIZE) == STATE_STAMP_SIZE
                    && hdr.magic == STATE_MAGIC;
    close(fd);
    if (ok)
    {
        *size = hdr.db_size;
        *mtime = hdr.db_mtime;
    }
    return ok;
}

/* The name given to the iPod in Finder is that of the first playlist, the hidden
   one with the whole library. It's near the middle of a file of 30 MB, reached
   here by a few short reads: at the end of Updating Library, while the disk
   spins (itunesdb_device_name_refresh()), and when it's shown before that.
   Not found, as on an iPod Finder never synced, it isn't looked for again
   until then. */
static char device_name[128];
static bool device_name_read = false;

static bool read_at(int fd, off_t pos, void *buf, size_t size)
{
    /* lseek()'s -1 for a failure would pass for a pos of -1 */
    return pos >= 0 && lseek(fd, pos, SEEK_SET) == pos
           && read(fd, buf, size) == (ssize_t)size;
}

static void read_device_name(void)
{
    unsigned char h[44];
    unsigned char text[2 * (sizeof(device_name) / 3)];
    int fd = open(ITUNESDB_DIR "/iTunesDB", O_RDONLY);

    device_name[0] = '\0';
    if (fd < 0)
        return;

    off_t pos = 0, end;
    if (!read_at(fd, pos, h, 24) || memcmp(h, "mhbd", 4))
        goto done;
    int sections = le32(h + 20);
    end = record_end(pos, h, MH_FILE_END);
    pos = next_record(pos, le32(h + 4), end);

    /* the list of playlists, among mhbd's sections as db_load() counts them */
    for (int i = 0; ; i++)
    {
        if (i >= sections || !read_at(fd, pos, h, 16) || memcmp(h, "mhsd", 4))
            goto done;
        const off_t next = record_end(pos, h, end);
        if (next < 0)
            goto done;
        if (le32(h + 12) == 2)
        {
            end = next;
            break;
        }
        pos = next;
    }
    pos = next_record(pos, le32(h + 4), end);
    if (!read_at(fd, pos, h, 16) || memcmp(h, "mhlp", 4))
        goto done;
    pos = next_record(pos, le32(h + 4), end);

    /* the first of them, if it's the hidden one */
    if (!read_at(fd, pos, h, 44) || memcmp(h, "mhyp", 4) || le32(h + 20) == 0)
        goto done;
    int mhods = le32(h + 12);
    end = record_end(pos, h, end);
    pos = next_record(pos, le32(h + 4), end);

    for (int i = 0; i < mhods; i++)
    {
        if (!read_at(fd, pos, h, 40) || memcmp(h, "mhod", 4))
            goto done;
        const off_t next = record_end(pos, h, end);
        if (next < 0)
            goto done;
        if (le32(h + 12) == 1)
        {
            size_t units = MIN(mhod_string_size(h), sizeof(text)) / 2;
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
        pos = next;
    }
done:
    close(fd);
}

const char *itunesdb_device_name(void)
{
    if (!device_name_read)
    {
        read_device_name();
        device_name_read = true;
    }
    return device_name[0] ? device_name : "iPod";
}

void itunesdb_device_name_refresh(void)
{
    device_name_read = false;
    itunesdb_device_name();
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
   the scan is done. So are those of the tracks whose tags changed in
   iTunesDB alone since the import before, as a program other than Finder
   edits them (iOpenPod by default, gtkpod) and the stock firmware shows:
   the scan compares each with the database and adds it again with them
   where they differ (tagcache.c add_tagcache()). */

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
    uint32_t date;      /* of release, track_date()'s */
    uint32_t added;     /* as tag_dateadded, 0 for none */
    /* then the strings, each ended by a zero, ID3V2_BUF_SIZE in all */
};
/* The digest of each track's tags in iTunesDB as the database has them, by
   path hash, of the last import that went through (itunesdb_import()): an
   import makes records only of the tracks whose digest differs. Written as
   the tracks are read, beside it, and put in its place once the import is
   done, as the database then has them. A track it has no digest of, 0 or
   none, as all of them without the file, is compared at the next import with
   the database in memory (meta.compare_all), all such tracks in one but for
   those a pool of records halved for memory leaves to later imports, a
   track whose digest differs at any import. Without the database in memory,
   as with Load to RAM off, those not known are never compared. */
#define TAGS_FILE       ROCKBOX_DIR "/itunesdb_tags.dat"
#define TAGS_PENDING    ROCKBOX_DIR "/itunesdb_tags_pending.dat"
#define TAGS_MAGIC      0x47544252 /* "RBTG" */
#define TAGS_VERSION    1

struct tags_header {
    uint32_t magic;
    uint32_t version;
    uint32_t db_size;   /* of the iTunesDB they are from */
    uint32_t db_mtime;
    uint32_t count;
    uint32_t unused[3]; /* the entries after it aligned for their hash */
};

struct tags_entry {
    uint64_t hash;
    uint32_t digest;    /* track_digest()'s, 0 for not known */
    uint32_t unused;
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
    /* and the digests of the import before (TAGS_FILE), sorted by hash */
    bool digesting;     /* the tracks' digests made, for an import's file */
    int tags_handle;
    int tags_count;     /* < 0 for no file */
    int tags_unknown;   /* the tracks known with no digest in it, 0 or none */
    bool tags_unread;   /* a file there, but not read: kept as it is */
    /* a track not known as the database has it, records for those too: the
       scan compares each with the database in memory */
    bool compare_all;
    int retags, retag_room; /* the records of tracks whose digest differs,
                               and how many may be, the new ones' kept */
    int fresh_left;     /* new ones still to come, about: their room kept */
    /* the paths whose song is the last sync's by its dbid, sorted, then
       those another song or none has now (note_moved_songs()), for the scan
       to give a file's statistics only to the same song */
    int songs_handle;
    int songs_same, songs_other;
} meta;

/* the bytes of the table before the pool: a multiple of 8, so that the
   records, which start with a uint64_t, are aligned as far as the allocation
   is (on the host and the simulator; buflib gives the iPod 4, where ARMv4
   reads a uint64_t as two words) */
static size_t meta_table_size(int count)
{
    return ALIGN_UP(count * sizeof(uint32_t), 8);
}

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
    if (meta.tags_handle > 0)
    {
        core_unpin(meta.tags_handle);
        core_free(meta.tags_handle);
    }
    if (meta.songs_handle > 0)
        core_free(meta.songs_handle);
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
    if (!read_state_header(STATE_FILE, &hdr) || hdr.count == 0)
        return 0;
    int fd = open(STATE_FILE, O_RDONLY);
    if (fd < 0)
        return 0;
    /* with a block behind the hashes to read into, given back after */
    size_t block = READ_BLOCK;
    *handle = db_alloc(hdr.count * sizeof(uint64_t) + block);
    if (*handle <= 0)
    {
        block = 0;
        *handle = db_alloc(hdr.count * sizeof(uint64_t));
    }
    if (*handle <= 0)
    {
        *handle = 0;
        close(fd);
        return -1;
    }
    core_pin(*handle);
    uint64_t *hashes = core_get_data(*handle);
    struct state_entry small[64];
    struct state_entry *chunk = block ? (struct state_entry *)(hashes + hdr.count)
                                      : small;
    const size_t per = block ? block / sizeof(*chunk) : ARRAYLEN(small);
    uint32_t n = 0;
    lseek(fd, sizeof(hdr), SEEK_SET);
    while (n < hdr.count)
    {
        size_t want = MIN(hdr.count - n, per);
        if (read(fd, chunk, want * sizeof(chunk[0]))
            != (ssize_t)(want * sizeof(chunk[0])))
            break;
        for (size_t i = 0; i < want; i++)
            hashes[n++] = chunk[i].hash;
    }
    close(fd);
    if (block)
    {
        core_unpin(*handle);
        core_shrink(*handle, hashes, hdr.count * sizeof(uint64_t));
        core_pin(*handle);
        hashes = core_get_data(*handle);
    }
    qsort_yielding(hashes, n, sizeof(uint64_t), cmp_u64);
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

/* The import's file in place of the last one's, a cut having come between
   the two in tags_promote(): only if that import went through, as the state
   it wrote says */
static void tags_recover(void)
{
    struct tags_header pending;
    struct state_header state;
    if (file_exists(TAGS_FILE) || !read_state_header(STATE_FILE, &state))
        return;
    const int fd = open(TAGS_PENDING, O_RDONLY);
    if (fd < 0)
        return;
    const bool ok = read(fd, &pending, sizeof(pending)) == sizeof(pending)
        && pending.magic == TAGS_MAGIC && pending.version == TAGS_VERSION
        && pending.db_size == state.db_size
        && pending.db_mtime == state.db_mtime;
    close(fd);
    if (ok)
        rename(TAGS_PENDING, TAGS_FILE);
}

/* the digests of the last import (TAGS_FILE) into memory, sorted by hash:
   their count, -1 for none to go by; *unread when the file is there and
   whole but couldn't be read into memory */
static int load_tags(int *handle, bool *unread)
{
    struct tags_header hdr;
    *handle = 0;
    *unread = false;
    tags_recover();
    const int fd = open(TAGS_FILE, O_RDONLY);
    if (fd < 0)
        return -1;
    const size_t size = ffilesize(fd);
    bool ok = read(fd, &hdr, sizeof(hdr)) == sizeof(hdr)
        && hdr.magic == TAGS_MAGIC && hdr.version == TAGS_VERSION
        && hdr.count <= COUNT_MAX
        && size == sizeof(hdr) + hdr.count * sizeof(struct tags_entry);
    if (ok && hdr.count > 0)
    {
        *handle = db_alloc(hdr.count * sizeof(struct tags_entry));
        ok = *handle > 0;
        if (ok)
        {
            core_pin(*handle);
            const ssize_t bytes = hdr.count * sizeof(struct tags_entry);
            ok = read(fd, core_get_data(*handle), bytes) == bytes;
        }
        *unread = !ok;
    }
    close(fd);
    if (!ok)
    {
        if (*handle > 0)
        {
            core_unpin(*handle);
            core_free(*handle);
        }
        *handle = 0;
        return -1;
    }
    return hdr.count;
}

/* the tracks of the last import (meta.known's hashes) the digests don't
   know, 0 or none: both sorted by hash */
static int tags_unknown(void)
{
    if (meta.known <= 0)
        return 0;
    if (meta.tags_handle <= 0)
        return meta.known;
    const uint64_t *known = core_get_data(meta.known_handle);
    const struct tags_entry *e = core_get_data(meta.tags_handle);
    int unknown = 0, j = 0;
    for (int i = 0; i < meta.known; i++)
    {
        while (j < meta.tags_count && e[j].hash < known[i])
            j++;
        if (j == meta.tags_count || e[j].hash != known[i] || !e[j].digest)
            unknown++;
    }
    return unknown;
}

/* the digest the last import had for the track, 0 for none */
static uint32_t old_digest(uint64_t hash)
{
    if (meta.tags_handle <= 0)
        return 0;
    const struct tags_entry *e = core_get_data(meta.tags_handle);
    int lo = 0, hi = meta.tags_count - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (e[mid].hash == hash)
            return e[mid].digest;
        if (e[mid].hash < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

/* The tags of a track that a record has, as iTunesDB has them: its strings'
   mhods (meta_mhods[], at 0 for none) hashed as they are, and its track and
   disc numbers. Never 0. A change it can't see, as one past a string's
   first 1024 bytes, is past what a record takes too. */
static uint32_t track_digest(const off_t *tags_at, uint32_t tracknum,
                             uint32_t discnum)
{
    uint32_t words[META_STRINGS + 2];
    for (int k = 0; k < META_STRINGS; k++)
        words[k] = tags_at[k] ? mhod_hash(tags_at[k]) : 0;
    words[META_STRINGS] = MIN(tracknum, 0xffff);
    words[META_STRINGS + 1] = MIN(discnum, 0xffff);
    /* FNV-1a over their bytes */
    uint32_t digest = 0x811c9dc5;
    for (size_t i = 0; i < ARRAYLEN(words); i++)
    {
        for (int b = 0; b < 32; b += 8)
            digest = (digest ^ ((words[i] >> b) & 0xff)) * 0x01000193;
    }
    return digest ? digest : 1;
}

/* the record of a track into the pool, false when it doesn't fit; its mhit
   ends at end */
static bool add_metadata(const struct track *t, off_t end)
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
    if (header_len >= 0x90)
    {
        r.date = track_date(r.year, le32(p + 0x8c));
        /* a Mac time, as the last play's at 0x58 */
        const uint32_t added = le32(p + 0x68);
        r.added = added > MAC_EPOCH_OFFSET ? added - MAC_EPOCH_OFFSET : 0;
    }
    if (!r.year)
        r.year = r.date / 10000;

    off_t child = next_record(t->pos, header_len, end);
    for (int i = 0; i < mhods; i++)
    {
        p = db_record(child, "mhod", 16);
        const off_t next = p ? record_end(child, p, end) : -1;
        if (next < 0)
            break;
        const uint32_t type = le32(p + 12);
        for (int k = 0; k < META_STRINGS; k++)
        {
            if (meta_mhods[k] == type)
                mhods_at[k] = child;
        }
        child = next;
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
    meta.digesting = meta.reading;
    meta.tags_count = load_tags(&meta.tags_handle, &meta.tags_unread);
    meta.tags_unknown = tags_unknown();
    /* the scan that follows finds the database's tags in memory, as after
       a sync over USB; at boot it is loaded after the scan, and a track not
       known waits for such an import, the disk read for each too slow */
#ifdef HAVE_TC_RAMCACHE
    meta.compare_all = tagcache_is_in_ram();
#endif
}

/* once the number of tracks is known: room for the records of all of them,
   less if there isn't enough memory, until metadata_end() gives back what
   the new ones didn't take */
static void metadata_room(int tracks)
{
    if (!meta.reading)
        return;
    const int fresh = MIN(MAX(tracks - meta.known, 0), tracks);
    /* the new ones and as many replaced, or all when none are known; the
       rest are read from the files. All of them too for a comparison of
       every track, as without the digests of the import before */
    int count = MIN(tracks, fresh * 2 + 256);
    /* and those the digests don't know, all compared at once */
    if (meta.compare_all)
        count = MIN(tracks, count + meta.tags_unknown);
    /* those whose tags changed, after the new ones' room */
    meta.retag_room = count - fresh;
    meta.fresh_left = fresh;
    size_t pool = count * META_PER_TRACK;
    while (1)
    {
        meta.handle = db_alloc(meta_table_size(count) + pool);
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
    meta.pool = (char *)meta.by_hash + meta_table_size(count);
    meta.pool_size = pool;
    meta.capacity = count;
}

/* A track just read, its mhit, which ends at end, still in the cache, and
   t->digest that of its tags now: a record of it if it's new, or if its
   tags changed since the last import or aren't known as the database has
   them; t->digest then what the database will have after this import's
   scan: the old one, or 0, for one with no record */
static void new_track_metadata(struct track *t, off_t end)
{
    if (!meta.digesting)
        return;
    const uint64_t *hashes = meta.known_handle > 0 ?
                             core_get_data(meta.known_handle) : NULL;
    const bool known = known_hash(hashes, meta.known, t->st.hash);
    const uint32_t old = known ? old_digest(t->st.hash) : 0;
    if (known && old == t->digest)
        return; /* the database has its tags as they are */
    /* a known one whose tags in the database aren't known waits for the
       database in memory */
    bool record = meta.reading && !meta.full && meta.handle > 0
                  && (!known || old || meta.compare_all);
    /* not in the room of the new ones still to come, which would be read
       from their files: the next import's */
    if (record && known
        && (meta.retags >= meta.retag_room
            || meta.pool_used + (size_t)(meta.fresh_left + 1) * META_PER_TRACK
               > meta.pool_size))
        record = false;
    else if (record && (meta.count >= meta.capacity
                        || !add_metadata(t, end)))
    {
        /* no more new ones either; a known one leaves them the rest */
        if (!known)
            meta.full = true;
        record = false;
    }
    if (!known && meta.fresh_left > 0)
        meta.fresh_left--;
    if (!record)
        t->digest = old; /* a new one read from its file: not known */
    else if (known)
        meta.retags++;
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
    if (meta.tags_handle > 0)
    {
        core_unpin(meta.tags_handle);
        core_free(meta.tags_handle);
    }
    meta.tags_handle = 0;
    meta.reading = false;
    meta.digesting = false;
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
                meta_table_size(meta.capacity) + meta.pool_used);
    core_pin(meta.handle);
    meta.by_hash = core_get_data(meta.handle);
    meta.pool = (char *)meta.by_hash + meta_table_size(meta.capacity);
    meta.pool_size = meta.pool_used;
}

/* the record of the file at path, NULL for none */
static const struct meta_record *find_record(const char *path)
{
    if (meta.count == 0)
        return NULL;
    const uint64_t hash = itunesdb_path_hash(path);
    int lo = 0, hi = meta.count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        const struct meta_record *m =
            (const struct meta_record *)(meta.pool + meta.by_hash[mid]);
        if (m->hash == hash)
            return m;
        if (m->hash < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

bool itunesdb_has_record(const char *path)
{
    return find_record(path) != NULL;
}

bool itunesdb_get_metadata(const char *path, struct mp3entry *id3,
                           struct tagcache_runtime *rt, long *date,
                           long *added)
{
    const struct meta_record *r = find_record(path);
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
    *date = r->date;
    *added = r->added;
    return true;
}

/* Finder's Sort fields (tagcache.h), from each track as it's read for an
   import: a pair for each of its names whose Sort field sorts it otherwise,
   as it comes. The songs of an album come together, and the pairs they share
   go once, by the last few remembered; a pair written twice anyway is taken
   once by the commit. The Sort mhods, each with the one of the name it's
   of. */
enum { SORT_ARTIST, SORT_ALBUMARTIST, SORT_ALBUM, SORT_TITLE, SORT_COMPOSER,
       SORTS };
static const uint8_t sort_mhods[SORTS] = { 23, 29, 28, 27, 30 };
static const uint8_t sorted_mhods[SORTS] = { 4, 22, 3, 1, 12 };
#define SORT_RECENT 16

static struct {
    bool writing;   /* into out, with the tracks read */
    struct tagcache_sort_header hdr;
    uint32_t recent[SORT_RECENT]; /* hashes of the last pairs written */
    int next;
} sorts;

static void track_sort_fields(const off_t *name_at, const off_t *sort_at);

/* Podcasts' and audiobooks' notes for Now Playing: a header, the texts as UTF-8,
   NOTES_MAX bytes each at most, then an index by path hash. None kept in memory. */
#define NOTES_FILE      ROCKBOX_DIR "/itunesdb_notes.dat"
#define NOTES_MAGIC     0x4e494252 /* "RBIN" */
#define NOTES_VERSION   1
#define NOTES_MAX       8192

struct notes_header {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t db_size;   /* of the iTunesDB they are from */
    uint32_t db_mtime;
    uint32_t index;     /* where the entries are, after the texts */
};

struct notes_entry {
    uint64_t hash;
    uint32_t offset;    /* of its text in the file */
    uint32_t length;
};

static struct {
    struct fw w;        /* through the room, if there is one */
    uint32_t end;       /* of the file so far */
    unsigned char *room; /* lent, while the file is written */
    bool ok;
} notes_out = { .w.fd = -1 };

static unsigned char *notes_room(size_t *size);
static void notes_room_done(void);

/* Through the room, each block of it filled to its end: a sector at a time
   was a command to the disk for each */
static void notes_write(const void *data, size_t len)
{
    if (notes_out.ok)
        notes_out.ok = fw_put(&notes_out.w, data, len);
}

static void notes_begin(void)
{
    const struct notes_header blank = { 0 };
    size_t size = 0;
    notes_out.end = sizeof(blank);
    notes_out.room = notes_room(&size);
    notes_out.ok = fw_open(&notes_out.w, NOTES_FILE, FW_KEEP, notes_out.room,
                           size);
    if (!notes_out.ok && notes_out.room)
    {
        notes_room_done();
        notes_out.room = NULL;
    }
    notes_write(&blank, sizeof(blank));
}

/* The string of the mhod at pos, 0 for none, to the file as UTF-8: where the
   file ends after it. A nil in it would end the text shown: a space. */
static uint32_t notes_put(off_t pos)
{
    const unsigned char *p = pos && notes_out.ok ? db_record(pos, "mhod", 40)
                                                 : NULL;
    if (!p)
        return notes_out.end;
    uint32_t units = mhod_string_size(p) / 2;
    off_t at = pos + 40;
    size_t total = 0;
    bool whole = false;
    while (units > 0 && !whole && notes_out.ok)
    {
        unsigned char buf[512];
        size_t used = 0;
        const uint32_t n = MIN(units, 160);
        uint32_t i = 0;
        p = db_get(at, n * 2);
        if (!p)
            break;
        while (i < n)
        {
            unsigned long c = p[2 * i] | p[2 * i + 1] << 8;
            uint32_t took = 1;
            if (c >= 0xd800 && c < 0xdc00)
            {
                /* a pair, whose other half may be in the next read */
                if (i + 1 >= n)
                {
                    if (n == units)
                        i = n; /* cut off by the string's end: left out */
                    break;
                }
                const unsigned long low = p[2 * i + 2] | p[2 * i + 3] << 8;
                const bool pair = low >= 0xdc00 && low < 0xe000;
                c = pair ? 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00)
                         : 0xfffd;
                took = pair ? 2 : 1;
            }
            else if (c >= 0xdc00 && c < 0xe000)
                c = 0xfffd; /* a second half without its first */
            if (!c)
                c = ' ';
            const size_t len = utf8encode(c, buf + used) - (buf + used);
            if (total + used + len > NOTES_MAX)
            {
                whole = true;
                break;
            }
            used += len;
            i += took;
        }
        /* a lone first half before more of the string: read on from it */
        if (i == 0 && !whole)
            i = 1;
        if (used)
            notes_write(buf, used);
        total += used;
        units -= i;
        at += 2 * i;
    }
    notes_out.end += total;
    return notes_out.end;
}

/* The index and the header, and the file takes its name: or none of it. An
   old file stays until then, and after a failure, when the next boot reads
   again (itunesdb_notes_ready()); without iTunesDB there is none. */
static void notes_end(bool loaded)
{
    bool ok = loaded && notes_out.ok;
    struct notes_header hdr = {
        NOTES_MAGIC, NOTES_VERSION, 0, db_size, db_mtime, notes_out.end
    };
    struct notes_entry batch[32];
    int filled = 0;
    for (int i = 0; ok && i <= db.count; i++)
    {
        if (i < db.count)
        {
            const uint32_t k = db.by_hash[i];
            const struct track *t = &db.tracks[k];
            const uint32_t start = k ? db.tracks[k - 1].notes_end : sizeof(hdr);
            if (!t->path || t->notes_end <= start)
                continue;
            batch[filled++] = (struct notes_entry){
                t->st.hash, start, t->notes_end - start
            };
            hdr.count++;
        }
        if (filled == (int)ARRAYLEN(batch) || (i == db.count && filled))
        {
            notes_write(batch, filled * sizeof(batch[0]));
            ok = notes_out.ok;
            filled = 0;
        }
    }
    if (notes_out.w.fd >= 0)
    {
        ok = ok && notes_out.ok && fw_seek(&notes_out.w, 0)
             && fw_put(&notes_out.w, &hdr, sizeof(hdr));
        fw_close(&notes_out.w, ok);
    }
    if (notes_out.room)
        notes_room_done();
    notes_out.room = NULL;
    if (!loaded && !itunesdb_exists())
        remove(NOTES_FILE);
}

bool itunesdb_notes_ready(void)
{
    uint32_t size, mtime;
    if (!db_stamp(&size, &mtime))
        return true;
    struct notes_header hdr;
    const int fd = open(NOTES_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    const bool ready = read(fd, &hdr, sizeof(hdr)) == sizeof(hdr)
        && hdr.magic == NOTES_MAGIC && hdr.version == NOTES_VERSION
        && hdr.db_size == size && hdr.db_mtime == mtime;
    close(fd);
    return ready;
}

/* The entry of hash in the index, searched in windows read into room, the
   first where its hash would be if they were spread evenly: for a library's
   worth of notes, one read */
static bool notes_find(int fd, const struct notes_header *hdr, uint64_t hash,
                       struct notes_entry *found, void *room, size_t size)
{
    const int per = size / sizeof(*found);
    int lo = 0, hi = (int)hdr->count - 1;
    int at = (int)(((hash >> 32) * (uint64_t)hdr->count) >> 32);
    while (lo <= hi && per > 0)
    {
        const int first = MAX(lo, MIN(at - per / 2, hi - per + 1));
        const int n = MIN(per, hi - first + 1);
        const off_t pos = hdr->index + (off_t)first * sizeof(*found);
        const ssize_t bytes = n * sizeof(*found);
        if (lseek(fd, pos, SEEK_SET) != pos || read(fd, room, bytes) != bytes)
            return false;
        int low = 0, high = n - 1;
        while (low <= high)
        {
            const int mid = (low + high) / 2;
            memcpy(found, (const char *)room + mid * sizeof(*found),
                   sizeof(*found));
            if (found->hash == hash)
                return true;
            if (found->hash < hash)
                low = mid + 1;
            else
                high = mid - 1;
        }
        /* before the window, after it, or in it and not there */
        if (low == 0)
            hi = first - 1;
        else if (low == n)
            lo = first + n;
        else
            return false;
        at = (lo + hi) / 2;
    }
    return false;
}

int itunesdb_read_notes(const char *path, char *buf, size_t size)
{
    struct notes_header hdr;
    struct notes_entry entry;
    int len = 0;
    const int fd = open(NOTES_FILE, O_RDONLY);
    if (fd < 0)
        return 0;
    if (read(fd, &hdr, sizeof(hdr)) == sizeof(hdr) && hdr.magic == NOTES_MAGIC
        && hdr.version == NOTES_VERSION && hdr.count <= COUNT_MAX
        && notes_find(fd, &hdr, itunesdb_path_hash(path), &entry, buf, size)
        && lseek(fd, entry.offset, SEEK_SET) == (off_t)entry.offset)
        len = read(fd, buf, MIN(entry.length, size));
    close(fd);
    return MAX(len, 0);
}

/* the track at pos read, its mhit ending at end; where its notes are in
   iTunesDB, an episode's or a book's description or else its subtitle, 0 for
   none */
static off_t read_track(off_t pos, off_t end, struct track *track)
{
    char path[MAX_PATH];
    const unsigned char *p = db_record(pos, "mhit", 12);
    memset(track, 0, sizeof(*track));
    track->pos = pos;
    if (!p)
        return 0;
    uint32_t header_len = le32(p + 4);
    p = db_get(pos, MIN(header_len, 356));
    if (!p || header_len < 0x9c)
        return 0;
    int mhods = le32(p + 12);
    /* for its digest, the record's numbers in it */
    const uint32_t tracknum = le32(p + 44), discnum = le32(p + 92);
    track->id = le32(p + 16);
    track->st.rating = p[31];
    track->sound_check = le32(p + 76);
    track->st.playcount = le32(p + 80);
    track->lastplayed = le32(p + 0x58);
    const uint32_t added = le32(p + 0x68);
    track->added = added > MAC_EPOCH_OFFSET ? added - MAC_EPOCH_OFFSET : 0;
    track->released = track_released(p);
    uint32_t media = 0;
    if (header_len >= 212)
    {
        media = le32(p + 208);
        /* its low byte, which has the video kinds: the ones above it, such
           as ringtones and voice memos, are sound */
        track->st.media = media & 0xff;
        /* a book's too: Play Counts gives them back as they are */
        if (media & (MEDIA_PODCAST | MEDIA_AUDIOBOOK))
        {
            track->st.played = p[0xb2];
            track->bookmark = le32(p + 0x6c);
        }
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
    if (p[30])
        track->st.flags |= ITUNESDB_COMPILATION;
    track->dbid = le32(p + 0x70) | (uint64_t)le32(p + 0x74) << 32;
    if (header_len >= 356 && le32(p + 352))
        track->art = le32(p + 352);
    else if (p[0x7c] | p[0x7d] << 8)
    {
        /* a cover, but no image id: the image in ArtworkDB names the track
           by its dbid instead. So before iTunes 7's longer record, as
           iOpenPod writes them for an iPod whose database is that old, and
           in iTunes' long records for the iPod Photo, the id left 0 */
        note_art_song(track - db.tracks,
                      le32(p + 0x70) | (uint64_t)le32(p + 0x74) << 32);
        if (art_songs.handle > 0)
            track->art = ART_BY_SONG;
    }

    /* the album, by its name and the album's artist, or the track's where
       none is given, for the covers of Cover Flow: as UTF-16, unread */
    uint32_t album = 0, album_artist = 0, artist = 0;
    off_t name_at[SORTS] = { 0 }, sort_at[SORTS] = { 0 };
    bool sorted = false;
    off_t notes_at = 0, subtitle_at = 0;
    /* the strings a record has, the last of each type as add_metadata()
       takes them */
    off_t tags_at[META_STRINGS] = { 0 };
    off_t child = next_record(pos, header_len, end);
    for (int i = 0; i < mhods; i++)
    {
        p = db_record(child, "mhod", 16);
        const off_t next = p ? record_end(child, p, end) : -1;
        if (next < 0)
            break;
        const uint32_t type = le32(p + 12);
        for (int k = 0; meta.digesting && k < META_STRINGS; k++)
        {
            if (type == meta_mhods[k])
                tags_at[k] = child;
        }
        for (int k = 0; sorts.writing && k < SORTS; k++)
        {
            if (type == sorted_mhods[k])
                name_at[k] = child;
            else if (type == sort_mhods[k])
            {
                sort_at[k] = child;
                sorted = true;
            }
        }
        if (type == 3 || type == 4 || type == 22)
        {
            uint32_t h = mhod_hash(child);
            *(type == 3 ? &album : type == 4 ? &artist : &album_artist) = h;
        }
        else if (spoken && (type == 14 || type == 18) && mhod_filled(child))
            *(type == 14 ? &notes_at : &subtitle_at) = child;
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
        child = next;
    }
    if (album)
        track->album = (album * 31 + (album_artist ? album_artist : artist))
                       | 1;
    if (track->st.hash)
    {
        if (meta.digesting)
            track->digest = track_digest(tags_at, tracknum, discnum);
        new_track_metadata(track, end);
    }
    if (sorted)
        track_sort_fields(name_at, sort_at);
    return !track->st.hash ? 0 : notes_at ? notes_at : subtitle_at;
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

/* the list of tracks at list, in the section that ends at end */
static bool read_tracks(off_t list, off_t end)
{
    const unsigned char *p = db_record(list, "mhlt", 12);
    if (!p)
        return false;
    off_t pos = next_record(list, le32(p + 4), end);
    int listed = le32(p + 8);
    int count = MIN(listed, db.count);
    db.count = 0;
    forget_art_songs();
    art_songs.capacity = count;
    for (int i = 0; i < count; i++)
    {
        p = db_record(pos, "mhit", 12);
        const off_t next = p ? record_end(pos, p, end) : -1;
        if (next < 0)
            break;
        struct track *track = &db.tracks[db.count++];
        /* its notes to their file, last: its place in iTunesDB, which
           shares its field, isn't asked for again */
        track->notes_end = notes_put(read_track(pos, next, track));
        pos = next;
        if ((i & 255) == 0)
        {
            reading(0, i, count);
            yield();
        }
    }
    /* and the last ending where the section does: a count damaged down, as
       by a bit, would pass for all of them, and the import take the rest out
       of the database */
    db.complete = db.count == listed && pos == end;
    /* an iTunesDB of no songs is read whole too: after the last song is taken
       off, the import takes them all out of the database */
    return db.count > 0 || db.complete;
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

/* The playlists file lists those of the last import, a line each, after
   "version <n>": "<hash> <size> <time> <file>" with the hash and size of what
   was written and the time the playlist was last changed in the library,
   which the file has as its own, followed by "smart <file>" for a smart one;
   and "dir <folder>" for each folder it made. Each file costs the iPod's flash
   a good part of a second to remove and write again, so those that would come
   out the same are left. */

/* What the list has, which the lists go by: 2, the smart ones' lines, and
   Music's folders apart from the user's of the same name (1.4.1). A list of
   an older version, or of none as 1.4.0 and before wrote it, has the
   playlists made again at each boot without a sync until one of this version
   is written (itunesdb_playlists_due()). The version's line is too short for
   a playlist's, and 1.4.0's readers pass over it. */
#define PLAYLISTS_VERSION 2
/* The size and time of the iTunesDB whose playlists the boot's pass
   (itunesdb_import_playlists()) couldn't read, or whose songs found no memory
   at any of its tries: not tried again over it, as it would fail the same
   way; the next sync's import makes them. A write of it cut short leaves it
   short, and the pass is tried again. */
#define PLAYLISTS_FAILED ROCKBOX_DIR "/itunesdb_playlists_failed.dat"

/* As many playlists and folders as there are: iTunesDB says how many
   playlists, and the files are given room for their lines. Of 256 playlists
   and 128 folders at most, a library of hundreds lost the rest (2026-11-04). */
/* Folders in folders this deep at most, as the browser goes no further: one
   that iTunes has deeper, or in itself, isn't made, and its playlists go
   into the top folder */
#define MAX_FOLDER_DEPTH (MAX_DIR_LEVELS - 4)
/* An older firmware wrote each playlist, and the playlists file, here
   first: removed as a pass begins. Now each is written beside its own name
   (file_write.c). */
#define PLAYLIST_TEMP   ROCKBOX_DIR "/itunesdb_playlist.tmp"
#define PLAYLISTS_TEMP  ROCKBOX_DIR "/itunesdb_playlists.tmp"

static struct {
    int handle;
    int count, capacity;
    struct old_playlist {
        uint64_t file_hash;     /* itunesdb_path_hash() of the file */
        uint64_t hash;          /* of what it holds, 0 if not known */
        uint32_t size;
        uint32_t time;          /* the file's, 0 if not set */
        uint32_t file;          /* offset of its name in the buffer */
        bool older;             /* listed by a version that didn't mark its files */
        bool smart;             /* with a smart one's line after it */
    } *lists;
    char *names;
    /* the folders made, as offsets of their names */
    int dir_count, dir_capacity;
    uint32_t *dirs;
    /* the files written this time, one a playlist of iTunesDB's at most */
    int written_count, written_capacity;
    uint64_t *written;
    bool list_failed;           /* a line of the new list didn't write */
    /* a record of the playlists in iTunesDB couldn't be read, as of one cut
       short by a sync left off: the playlists after it would have looked gone */
    bool read_failed;
    /* one of iTunesDB's couldn't be written, as with the disk full: its old
       one may be any of those not written again, under its old name or in its
       old folder, so none of them goes this time (remove_old_playlists()). The
       file in its place, if any, stays as it was, though it's another's. */
    bool lost;
} pls;

/* the last import's list as found */
enum old_list { LIST_NONE, LIST_READ, LIST_NO_ROOM };

/* A line of a list file, or none for one too long for the buffer, whose rest
   is passed over too: a part of it could look like a path */
static int read_list_line(int fd, char *line, size_t size)
{
    int n = read_line(fd, line, size);
    if (n > 0 && strlen(line) >= size - 1)
    {
        char rest[64];
        while (read_line(fd, rest, sizeof(rest)) > 0
               && strlen(rest) >= sizeof(rest) - 1)
            ;
        line[0] = '\0';
    }
    return n;
}

/* the lines of a list file, the last with a newline or without: as many as
   it's given room for */
static int count_lines(int fd)
{
    char block[512];
    int lines = 0;
    char last = '\n';
    ssize_t n;
    while ((n = read(fd, block, sizeof(block))) > 0)
    {
        for (ssize_t i = 0; i < n; i++)
            lines += block[i] == '\n';
        last = block[n - 1];
    }
    lseek(fd, 0, SEEK_SET);
    /* as many as anyone has, and the sizes from it in range, of a broken file */
    return MIN(lines + (last != '\n'), 1 << 16);
}

/* The playlists file of the last import, if there is one to go by, and room
   for those of iTunesDB written this time */
static enum old_list load_old_playlists(int playlists)
{
    memset(&pls, 0, sizeof(pls));
    remove(PLAYLIST_TEMP);
    remove(PLAYLISTS_TEMP);
    int fd = open(PLAYLISTS_FILE, O_RDONLY);
    char line[MAX_PATH + 48];
    const int lines = fd >= 0 ? count_lines(fd) : 0;
    const off_t size = fd >= 0 ? ffilesize(fd) : 0;
    pls.written_capacity = MAX(0, MIN(playlists, 1 << 16)); /* as iTunesDB says */
    pls.capacity = pls.dir_capacity = lines;
    pls.handle = db_alloc(pls.written_capacity * sizeof(uint64_t)
                          + lines * sizeof(struct old_playlist)
                          + lines * sizeof(uint32_t) + size + 1);
    if (pls.handle <= 0)
    {
        /* the playlists are left as they are, and the list, for the next */
        pls.handle = 0;
        if (fd >= 0)
            close(fd);
        return LIST_NO_ROOM;
    }
    core_pin(pls.handle);
    pls.written = core_get_data(pls.handle);
    pls.lists = (struct old_playlist *)(pls.written + pls.written_capacity);
    pls.dirs = (uint32_t *)(pls.lists + lines);
    pls.names = (char *)(pls.dirs + lines);
    if (fd < 0)
        return LIST_NONE;
    size_t used = 0;
    while (read_list_line(fd, line, sizeof(line)) > 0)
    {
        char *file = line;
        /* none of one too long, or too short to hold what it should, nor the
           version's */
        if (line[0] == '\0' || (line[0] != '/' && strlen(line) < 18)
            || !strncmp(line, "version ", 8))
            continue;
        if (!strncmp(line, "smart /", 7))
        {
            /* the mark of the playlist on the line before */
            if (pls.count > 0
                && !strcasecmp(pls.names + pls.lists[pls.count - 1].file, line + 6))
                pls.lists[pls.count - 1].smart = true;
            continue;
        }
        if (!strncmp(line, "dir /", 5))
        {
            size_t len = strlen(line + 4) + 1;
            if (pls.dir_count < pls.dir_capacity && used + len <= (size_t)size + 1)
            {
                memcpy(pls.names + used, line + 4, len);
                pls.dirs[pls.dir_count++] = used;
                used += len;
            }
            continue;
        }
        if (pls.count >= pls.capacity)
            continue; /* on to the folders, listed after them */
        struct old_playlist *o = &pls.lists[pls.count];
        o->hash = 0;
        o->size = 0;
        o->time = 0;
        o->older = true;
        o->smart = false;
        if (line[0] != '/')
        {
            /* not from an older version, which listed only the files */
            char *end, high[9];
            strmemccpy(high, line, sizeof(high));
            o->hash = (uint64_t)strtoul(high, NULL, 16) << 32
                      | (uint32_t)strtoul(line + 8, &end, 16);
            o->size = strtoul(end, &end, 10);
            /* and the time, but in the version before, whose files had no mark */
            if (*end == ' ' && end[1] != '/')
            {
                o->time = strtoul(end + 1, &end, 10);
                o->older = false;
            }
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
    return LIST_READ;
}

/* a line of the list being written, a write() each: one that doesn't write
   spoils it, and the old list stays */
static void list_line(struct fw *list, const char *fmt, ...)
{
    char line[MAX_PATH + 48];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(line) || !fw_put(list, line, n))
        pls.list_failed = true;
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

/* The second line of each file written, by which the import knows its own
   when the playlists file is gone, as with a new .rockbox: they are written
   again in place, not as "Name 2", and those Finder removed go */
#define PLAYLIST_HEAD ITUNESDB_PLAYLIST_HEAD
#define PLAYLIST_MARK ITUNESDB_PLAYLIST_MARK

static bool own_playlist(const char *file);
bool itunesdb_own_playlist(const char *file)
{
    return own_playlist(file);
}

static bool own_playlist(const char *file)
{
    static const char head[] = PLAYLIST_HEAD PLAYLIST_MARK "\n";
    char buf[sizeof(head) - 1];
    int fd = open(file, O_RDONLY);
    if (fd < 0)
        return false;
    bool own = read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf)
               && !memcmp(buf, head, sizeof(buf));
    close(fd);
    return own;
}

/* one of ours from the list, which may go or be written again: the user's own
   file of the same name, put there since, stays */
static bool listed_own(const struct old_playlist *o)
{
    const char *file = pls.names + o->file;
    const char *ext = strrchr(file, '.');
    return ext && !strcasecmp(ext, ".m3u8") && (o->older || own_playlist(file));
}

/* our files in a folder and those in it, not written again; with one of
   iTunesDB's lost, kept and listed, what they hold not known */
static void remove_own_files(const char *dir, int depth, struct fw *list)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    char file[MAX_PATH];
    struct dirent *entry;
    while ((entry = readdir(d)))
    {
        if (entry->d_name[0] == '.')
            continue;
        if ((size_t)snprintf(file, sizeof(file), "%s/%s", dir,
                             entry->d_name) >= sizeof(file))
            continue;
        if (dir_get_info(d, entry).attribute & ATTR_DIRECTORY)
        {
            if (depth < MAX_FOLDER_DEPTH)
                remove_own_files(file, depth + 1, list);
            continue;
        }
        const char *ext = strrchr(entry->d_name, '.');
        if (ext && !strcasecmp(ext, ".m3u8")
            && !written_playlist(itunesdb_path_hash(file)) && own_playlist(file))
        {
            if (pls.lost)
                list_line(list, "0000000000000000 0 0 %s\n", file);
            else
                remove(file);
        }
    }
    closedir(d);
}

/* A folder at path whose content is all the import's own, as a pass cut
   short leaves one unlisted: its playlists with the mark, and folders of the
   same; not an empty one, as the user makes, nor one with anything else. A
   Mac's dot files don't count. The path is as it was after. */
static bool imports_only(char *path, size_t size, int depth)
{
    DIR *d = opendir(path);
    if (!d)
        return false;
    const size_t len = strlen(path);
    bool any = false, only = true;
    struct dirent *entry;
    while (only && (entry = readdir(d)))
    {
        if (entry->d_name[0] == '.')
            continue;
        any = true;
        if ((size_t)snprintf(path + len, size - len, "/%s", entry->d_name)
            >= size - len)
            only = false;
        else if (dir_get_info(d, entry).attribute & ATTR_DIRECTORY)
            only = depth < MAX_FOLDER_DEPTH
                   && imports_only(path, size, depth + 1);
        else
        {
            const char *ext = strrchr(entry->d_name, '.');
            only = ext && !strcasecmp(ext, ".m3u8") && own_playlist(path);
        }
        path[len] = '\0';
    }
    closedir(d);
    return any && only;
}

static int cmp_dir_depth(const void *a, const void *b)
{
    const char *x = pls.names + *(const uint32_t *)a;
    const char *y = pls.names + *(const uint32_t *)b;
    return (int)strlen(y) - (int)strlen(x);
}

/* One of the last import's that stays as it is, with one of iTunesDB's lost:
   listed again as it was, a smart one with its mark and rules, to go once an
   import has written them all. Smart by its line, or by the rules the last
   import noted, as a list of 1.4.0 has no such lines */
static void keep_old_playlist(const struct old_playlist *o, struct fw *list)
{
    const char *file = pls.names + o->file;
    const unsigned long high = (unsigned long)(o->hash >> 32);
    const unsigned long low = (unsigned long)(uint32_t)o->hash;
    if (o->older)
    {
        /* without a time, as that version listed it: ours without the mark */
        if (file_exists(file))
            list_line(list, "%08lx%08lx %lu %s\n", high, low,
                      (unsigned long)o->size, file);
        return;
    }
    list_line(list, "%08lx%08lx %lu %lu %s\n", high, low,
              (unsigned long)o->size, (unsigned long)o->time, file);
    if (smart_import_keep(file) || o->smart)
        list_line(list, "smart %s\n", file);
}

/* The old ones not written again go, as Finder removed them; when there was no
   list to go by, any of ours found. With one of iTunesDB's lost, they stay.
   Then the folders made for them that are left empty, the deepest first: one
   that can't go yet, as something is in it, is listed again to be tried the
   next time. */
static void remove_old_playlists(const char *dir, bool listed, const char *made,
                                 size_t made_len, struct fw *list)
{
    for (int i = 0; i < pls.count; i++)
    {
        const struct old_playlist *o = &pls.lists[i];
        if (written_playlist(o->file_hash) || !listed_own(o))
            continue;
        if (pls.lost)
            keep_old_playlist(o, list);
        else
            remove(pls.names + o->file);
    }
    if (!listed)
        remove_own_files(dir, 0, list);
    qsort(pls.dirs, pls.dir_count, sizeof(uint32_t), cmp_dir_depth);
    for (int i = 0; i < pls.dir_count; i++)
    {
        const char *old = pls.names + pls.dirs[i];
        bool kept = false;
        for (size_t k = 0; k < made_len && !kept; k += strlen(made + k) + 1)
            kept = !strcasecmp(made + k, old);
        if (!kept && rmdir(old) < 0 && dir_exists(old)) /* only if it's empty */
            list_line(list, "dir %s\n", old);
    }
}

static void forget_old_playlists(void)
{
    if (pls.handle > 0)
    {
        core_unpin(pls.handle);
        core_free(pls.handle);
    }
    memset(&pls, 0, sizeof(pls));
}

/* a name as FAT takes it for a file or a folder: cut where a character starts,
   and the characters it doesn't take as _ */
static void clean_name(char *clean, size_t size, const char *name, const char *empty)
{
    size_t n = 0;
    for (const char *c = name; *c && n < size - 1; c++)
    {
        const unsigned char b = *c;
        clean[n++] = (b < 0x20 || b == 0x7f || strchr("/\\:*?\"<>|", b)) ? '_' : b;
    }
    /* not in the middle of a character: back to where the one cut starts */
    while (n > 0 && ((unsigned char)name[n] & 0xc0) == 0x80)
        n--;
    /* nor a dot or a space at the end */
    while (n > 0 && (clean[n - 1] == '.' || clean[n - 1] == ' '))
        n--;
    clean[n] = '\0';
    if (n == 0)
        strmemccpy(clean, empty, size);
}

/* a playlist name as a file name that FAT takes; false if it is too long */
static bool playlist_file(char *buf, size_t size, const char *dir,
                          const char *name)
{
    char clean[MAX_PATH / 2];
    clean_name(clean, sizeof(clean), name, "Playlist");

    /* taken by another playlist of this import, or by a file that isn't
       one of ours */
    if ((size_t)snprintf(buf, size, "%s/%s.m3u8", dir, clean) >= size)
        return false;
    for (int i = 2; ; i++)
    {
        const struct old_playlist *o;
        if (!written_playlist(itunesdb_path_hash(buf))
            && (!file_exists(buf) || own_playlist(buf)
                || ((o = find_old_playlist(buf)) && o->older)))
            return true;
        /* none, rather than over a file of the user's */
        if (i == 100
            || (size_t)snprintf(buf, size, "%s/%s %d.m3u8", dir, clean, i) >= size)
            return false;
    }
}

/* Files are written through this a block at a time (file_write.c): a
   write() costs the iPod about as much for 16 bytes as for 4 KB. One file at
   a time. */
static struct fw out;
/* behind a bool it wasn't on a word, and all of it went without DMA */
static char out_buf[WRITE_BUF_SIZE] STORAGE_ALIGN_ATTR;

static bool out_open(const char *file, enum fw_policy policy)
{
    return fw_open(&out, file, policy, out_buf, sizeof(out_buf));
}

/* a line, never split between two writes; one longer than the buffer is
   left out */
static void writer_line(struct fw *w, const char *line)
{
    size_t len = strlen(line);
    if (w->used + len + 1 > w->size)
        fw_flush(w);
    if (len + 1 > w->size)
        return;
    memcpy(w->buf + w->used, line, len);
    w->buf[w->used + len] = '\n';
    w->used += len + 1;
}

static void playlist_line(struct fw *w, const char *line, uint64_t *hash,
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

/* the lines of a playlist's items from child on, to its end at end, into w
   if there is one, and their hash and size; the videos are left out. How many
   are written, and how many videos there were. Its order as Finder shows it,
   if not its own, on a line of the head. */
static int playlist_items(off_t child, off_t end, int items, uint32_t sort,
                          struct fw *w, uint64_t *hash, uint32_t *size,
                          int *videos)
{
    const unsigned char *p;
    int written = 0;
    *videos = 0;
    *hash = 0xcbf29ce484222325ULL;
    *size = 0;
    playlist_line(w, "#EXTM3U", hash, size);
    playlist_line(w, PLAYLIST_MARK, hash, size);
    if (sort > ITUNESDB_SORT_MANUAL)
    {
        char line[32];
        snprintf(line, sizeof(line), ITUNESDB_PLAYLIST_SORT "%lu",
                 (unsigned long)sort);
        playlist_line(w, line, hash, size);
    }
    for (int i = 0; i < items; i++)
    {
        p = db_record(child, "mhip", 28);
        const off_t next = p ? record_end(child, p, end) : -1;
        if (next < 0)
        {
            pls.read_failed = true;
            break;
        }
        struct track *t = find_by_id(le32(p + 24));
        if (t && (t->st.media & MEDIA_VIDEO))
            (*videos)++;
        else if (t && t->path)
        {
            playlist_line(w, db.pool + t->path, hash, size);
            written++;
        }
        child = next;
    }
    return written;
}

/* The folders the playlists are in on the computer, which Finder leaves out of
   an iPod Video's iTunesDB: written by the installer from Music, by the
   persistent IDs that iTunesDB has for the playlists too. A line each:
   "folder <id> <parent id or -> <name>" and "playlist <id> <parent id>". They
   are read at each import, so they show after the sync that follows. */
#define FOLDERS_FILE ROCKBOX_DIR "/playlist_folders.txt"

static struct {
    int handle;
    int folder_count, placed_count;
    int capacity;               /* of each, a line of the file each */
    struct folder {
        uint64_t id, parent;
        uint32_t name;          /* in names */
        uint32_t made_at;       /* its place this time, 1 past its offset in
                                   made, 0 until its first playlist's */
    } *folders;
    struct placed {
        uint64_t id, parent;
    } *placed;
    char *names;
    /* the folders made, or kept, this time, end to end */
    char *made;
    size_t made_len, made_size;
    int made_count;
    /* there was no list to say which were made, as with a new .rockbox: those
       there already are taken to be the import's */
    bool claim;
} fld;

/* the room for the folders made, by so much a folder: one past it isn't
   noted, and stays when it's left empty */
#define MADE_PER_FOLDER 96

/* 16 hex digits, or none for "-" */
static uint64_t parse_id(const char *s, const char **end)
{
    uint64_t id = 0;
    while (*s == ' ')
        s++;
    for (; *s && *s != ' '; s++)
    {
        const char c = *s | 0x20;
        if (c >= '0' && c <= '9')
            id = id << 4 | (c - '0');
        else if (c >= 'a' && c <= 'f')
            id = id << 4 | (c - 'a' + 10);
    }
    *end = s;
    return id;
}

/* false if there was no memory for them */
static bool load_folders(void)
{
    memset(&fld, 0, sizeof(fld));
    int fd = open(FOLDERS_FILE, O_RDONLY);
    if (fd < 0)
        return true;
    const off_t size = ffilesize(fd);
    const int lines = count_lines(fd);
    fld.capacity = lines;
    fld.made_size = lines * MADE_PER_FOLDER + MAX_PATH;
    fld.handle = db_alloc(lines * sizeof(struct folder) + lines * sizeof(struct placed)
                          + fld.made_size + size + 1);
    if (fld.handle <= 0)
    {
        fld.handle = 0;
        close(fd);
        return false;
    }
    core_pin(fld.handle);
    fld.folders = core_get_data(fld.handle);
    fld.placed = (struct placed *)(fld.folders + lines);
    fld.made = (char *)(fld.placed + lines);
    fld.names = fld.made + fld.made_size;
    size_t used = 0;
    char line[MAX_PATH];
    while (read_list_line(fd, line, sizeof(line)) > 0)
    {
        const char *rest;
        if (!strncmp(line, "folder ", 7) && fld.folder_count < fld.capacity)
        {
            struct folder *f = &fld.folders[fld.folder_count];
            f->id = parse_id(line + 7, &rest);
            f->parent = parse_id(rest, &rest);
            while (*rest == ' ')
                rest++;
            const size_t len = strlen(rest) + 1;
            if (!f->id || used + len > (size_t)size + 1)
                continue;
            memcpy(fld.names + used, rest, len);
            f->name = used;
            f->made_at = 0;
            used += len;
            fld.folder_count++;
        }
        else if (!strncmp(line, "playlist ", 9) && fld.placed_count < fld.capacity)
        {
            struct placed *pl = &fld.placed[fld.placed_count];
            pl->id = parse_id(line + 9, &rest);
            pl->parent = parse_id(rest, &rest);
            if (pl->id && pl->parent)
                fld.placed_count++;
        }
    }
    close(fd);
    return true;
}

static void forget_folders(void)
{
    if (fld.handle > 0)
    {
        core_unpin(fld.handle);
        core_free(fld.handle);
    }
    fld.handle = 0;
    fld.folder_count = fld.placed_count = 0;
    fld.made = NULL;
    fld.made_len = 0;
    fld.made_count = 0;
}

static struct folder *find_folder(uint64_t id)
{
    for (int i = 0; id && i < fld.folder_count; i++)
    {
        if (fld.folders[i].id == id)
            return &fld.folders[i];
    }
    return NULL;
}

/* a folder an import made, by the list of the last one */
static bool made_before(const char *path)
{
    for (int i = 0; i < pls.dir_count; i++)
    {
        if (!strcasecmp(pls.names + pls.dirs[i], path))
            return true;
    }
    return false;
}

/* a folder noted as made, or kept, this time: 1 past its offset in made, 0
   if it isn't */
static uint32_t made_now(const char *path)
{
    for (size_t k = 0; k < fld.made_len; k += strlen(fld.made + k) + 1)
    {
        if (!strcasecmp(fld.made + k, path))
            return k + 1;
    }
    return 0;
}

/* another of Music's folders beside this one, in the same folder, by this
   name as its own on the disk would be */
static bool sibling_named(const struct folder *f, const char *name)
{
    for (int i = 0; i < fld.folder_count; i++)
    {
        const struct folder *o = &fld.folders[i];
        if (o == f || o->parent != f->parent)
            continue;
        char clean[MAX_PATH / 4];
        clean_name(clean, sizeof(clean), fld.names + o->name, "Folder");
        if (!strcasecmp(clean, name))
            return true;
    }
    return false;
}

/* The folder in the playlists' one of the folder with this id, made if it isn't
   there, with those it's in: false if it's too deep, in itself, too long or
   can't be made. Each folder's place is chosen once an import, by its first
   playlist, and its others go there */
static bool folder_dir(char *buf, size_t size, const char *dir, uint64_t id)
{
    struct folder *chain[MAX_FOLDER_DEPTH];
    int depth = 0;
    for (struct folder *f = find_folder(id); f; f = find_folder(f->parent))
    {
        if (depth == MAX_FOLDER_DEPTH)
            return false;
        for (int i = 0; i < depth; i++)
        {
            if (chain[i] == f)
                return false;
        }
        chain[depth++] = f;
    }
    strmemccpy(buf, dir, size);
    for (int i = depth - 1; i >= 0; i--)
    {
        if (chain[i]->made_at)
        {
            strmemccpy(buf, fld.made + chain[i]->made_at - 1, size);
            continue;
        }
        char clean[MAX_PATH / 4];
        clean_name(clean, sizeof(clean), fld.names + chain[i]->name, "Folder");
        const size_t len = strlen(buf);
        if ((size_t)snprintf(buf + len, size - len, "/%s", clean) >= size - len)
            return false;
        bool made = mkdir(buf) == 0;
        if (!made && !dir_exists(buf))
            return false;
        /* one that was there already, and no import made, is the user's, in
           Unsynced, and is never written into: this one beside it as
           "Name 2", the first of those free or one an import made before,
           as a playlist does by a file of the user's; and none another
           folder of Music's has this time. With no list, all are taken to be
           the import's; so is one that holds only the import's, unlisted, as
           an import cut short leaves it */
        int num = 1;
        while (!made && (made_now(buf) || (!made_before(buf) && !fld.claim
                                           && !imports_only(buf, size, 0))))
        {
            /* the next name, but none another folder of Music's beside it
               has: its "Name 2" keeps its own */
            do
            {
                if (++num == 100 || (size_t)snprintf(buf + len, size - len, "/%s %d",
                                                     clean, num) >= size - len)
                    return false;
            } while (sibling_named(chain[i], buf + len + 1));
            made = mkdir(buf) == 0;
            if (!made && !dir_exists(buf))
                return false;
        }
        /* noted once, to be removed when it's left empty; and its place for
           the folder's other playlists */
        const size_t n = strlen(buf) + 1;
        if (fld.made_count < fld.capacity && fld.made_len + n <= fld.made_size)
        {
            memcpy(fld.made + fld.made_len, buf, n);
            chain[i]->made_at = fld.made_len + 1;
            fld.made_len += n;
            fld.made_count++;
        }
    }
    return true;
}

/* the folder a playlist is in, 0 for none */
static uint64_t playlist_parent(uint64_t id)
{
    for (int i = 0; i < fld.placed_count; i++)
    {
        if (fld.placed[i].id == id)
            return fld.placed[i].parent;
    }
    return 0;
}

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static int64_t be64(const unsigned char *p)
{
    return (int64_t)((uint64_t)be32(p) << 32 | be32(p + 4));
}

/* A smart playlist's mhod 50: whether it's live, and its limits. As libgpod
   reads it: after the mhod's 24 bytes, a byte each for live, the rules, the
   limits, their kind and their order, the limit at 8, and at 13 the order the
   other way round. */
static bool smart_prefs(off_t pos, uint32_t total, struct smart_rules *r)
{
    if (total < 24 + 14)
        return false;
    const unsigned char *p = db_get(pos, 24 + 14);
    if (!p)
        return false;
    const unsigned char *d = p + 24;
    if (!d[0])
        return false; /* not live: Finder's list, as it is */
    r->check_rules = d[1];
    r->check_limits = d[2];
    r->limit_type = d[3];
    r->limit_sort = d[4] | (d[13] ? 0x80000000u : 0);
    r->limit_value = le32(d + 8);
    return true;
}

/* Its mhod 51, the rules: big-endian after "SLst", their count at 8 and
   whether any one will do at 12, and from 136 on each with its field, its
   action, the length of what follows at 52, and that: for a number or a
   date, its from, its units back, how long a unit is and its to */
static bool smart_rule_list(off_t pos, uint32_t total, struct smart_rules *r)
{
    if (total < 24 + 136)
        return false;
    const unsigned char *p = db_get(pos, total);
    if (!p || memcmp(p + 24, "SLst", 4))
        return false;
    const unsigned char *d = p + 24;
    const uint32_t n = be32(d + 8);
    r->any = be32(d + 12) == 1;
    r->count = MIN(n, SMART_RULES_MAX + 1); /* more can't be worked out */
    size_t off = 24 + 136;
    for (uint32_t i = 0; i < n && i < SMART_RULES_MAX; i++)
    {
        if (off + 56 > total)
            return false;
        const unsigned char *q = p + off;
        struct smart_rule *s = &r->rule[i];
        s->field = be32(q);
        s->action = be32(q + 4);
        const uint32_t len = be32(q + 52);
        if (len > total - off - 56)
            return false;
        if (len >= 0x44 && !(s->action & 0x01000000))
        {
            s->from = be64(q + 56);
            s->from_date = be64(q + 64);
            s->from_units = be64(q + 72);
            s->to = be64(q + 80);
        }
        off += 56 + len;
    }
    return true;
}

/* the mhyp at pos, which ends at end, as a playlist file, listed in the
   playlists file; alone, at boot without a sync (itunesdb_import_playlists()) */
static void write_playlist(off_t pos, off_t end, const char *dir,
                           struct fw *list, struct fw *w, bool alone)
{
    char name[MAX_PATH / 2], file[MAX_PATH], folder[MAX_PATH];
    /* as before this playlist: a read of its own that failed sets it */
    const bool read_failed = pls.read_failed;
    const unsigned char *p = db_record(pos, "mhyp", 0x5c);
    const bool whole = p != NULL;
    if (!p)
        p = db_record(pos, "mhyp", 44);
    if (!p)
    {
        pls.read_failed = true;
        return;
    }
    uint32_t header_len = le32(p + 4);
    int mhods = le32(p + 12);
    int items = le32(p + 16);
    /* the column it's sorted by on the Mac, which a stock iPod shows it by */
    const uint32_t sort = whole ? le32(p + 0x2c) : ITUNESDB_SORT_MANUAL;
    bool hidden = le32(p + 20) != 0;   /* the library itself */
    bool podcasts = (p[42] | p[43] << 8) != 0;
    if (hidden || podcasts)
        return;
    const uint64_t id = le32(p + 0x1c) | (uint64_t)le32(p + 0x20) << 32;
    /* when it was last changed in the library, or else first written here */
    uint32_t changed = header_len >= 0x5c ? le32(p + 0x58) : 0;
    if (changed == 0)
        changed = le32(p + 0x18);
    const uint32_t time = changed > MAC_EPOCH_OFFSET ? changed - MAC_EPOCH_OFFSET : 0;

    name[0] = '\0';
    struct smart_rules smart;
    memset(&smart, 0, sizeof(smart));
    bool live = false, ruled = false;
    bool by_rules = false; /* a smart one, live or not */
    off_t child = next_record(pos, header_len, end);
    for (int i = 0; i < mhods; i++)
    {
        p = db_record(child, "mhod", 16);
        const off_t next = p ? record_end(child, p, end) : -1;
        if (next < 0)
        {
            pls.read_failed = true;
            return;
        }
        uint32_t total = le32(p + 8);
        const uint32_t type = le32(p + 12);
        if (type == 1)
            mhod_string(child, name, sizeof(name));
        else if (type == 50)
        {
            live = smart_prefs(child, total, &smart);
            by_rules = true;
        }
        else if (type == 51)
            ruled = smart_rule_list(child, total, &smart);
        child = next;
    }

    /* what it would hold, then the file only if that changed; none for one of
       videos alone, such as Finder's Movies */
    uint64_t hash;
    uint32_t size;
    int videos;
    if (playlist_items(child, end, items, sort, NULL, &hash, &size, &videos) == 0
        && videos > 0)
        return;
    /* in its folder, if the folders are known */
    const uint64_t parent = playlist_parent(id);
    if (!parent || !folder_dir(folder, sizeof(folder), dir, parent))
        strmemccpy(folder, dir, sizeof(folder));
    if (!playlist_file(file, sizeof(file), folder, name)
        || pls.written_count >= pls.written_capacity)
        return;
    struct old_playlist *o = find_old_playlist(file);
    bool same = false;
    if (o && o->hash == hash && o->size == size)
    {
        int fd = open(file, O_RDONLY);
        if (fd >= 0)
        {
            /* or, alone, as the iPod has written it since, as a live smart
               one by its own ratings and songs: iTunesDB's isn't new, and a
               sync's import writes it */
            same = alone || ffilesize(fd) == (off_t)size;
            close(fd);
        }
    }
    if (!same)
    {
        /* the old one stays whole until the new one is all written, and as it
           is if that can't be, to be written the next time; one elsewhere, as
           under the playlist's old name, too (pls.lost). Kept over a cut, as
           a playlist made on the iPod is: the iPod's own changes to it are
           written in it until the next sync. */
        bool ok = fw_open(w, file, FW_KEEP, out_buf, sizeof(out_buf));
        if (ok)
        {
            playlist_items(child, end, items, sort, w, &hash, &size, &videos);
            /* its list of songs is of the file before: one the same size,
               of the same database, still looked like it. Gone first, as
               the file may be new without it being so. */
            lp_index_forget(file);
        }
        /* not one cut short in place of the whole one, nor without its
           name, for which its folder may have no room */
        if (!fw_close(w, ok && pls.read_failed == read_failed))
        {
            pls.lost = true;
            if (!file_exists(file))
                return;
            hash = 0;
        }
    }
    /* its time as the file's: when it was last changed in the library */
    if (time && (!same || o->time != time))
        modtime(file, time);
    pls.written[pls.written_count++] = itunesdb_path_hash(file);
    /* a live smart one, to be kept up to date as the iPod's ratings change */
    if (live && (ruled || !smart.check_rules))
        smart_import_note(file, time, &smart);
    list_line(list, "%08lx%08lx %lu %lu %s\n", (unsigned long)(hash >> 32),
              (unsigned long)(uint32_t)hash, (unsigned long)size,
              (unsigned long)time, file);
    /* and a line of its own if it's a smart one, which Playlists lists
       first: a line a version before passes over */
    if (by_rules)
        list_line(list, "smart %s\n", file);
}

/* alone: all that's taken in, at boot without a sync
   (itunesdb_import_playlists()), into the bar's second half as they're
   written. False when iTunesDB's playlists couldn't all be read, as they
   wouldn't the next time either */
static bool write_playlists(off_t list, off_t end, bool alone)
{
    char dir[MAX_PATH];
    const unsigned char *p = list >= 0 ? db_record(list, "mhlp", 12) : NULL;
    bool readable = p != NULL;
    const enum old_list old = load_old_playlists(p ? (int)le32(p + 8) : 0);
    if (old == LIST_NO_ROOM)
        return readable;
    const bool listed = old == LIST_READ;
    /* the pass with no memory for the folders would put their playlists in
       the top one: they're left as they are instead, for the next boot, as
       with no room for the old list. A sync's import goes on, as before. */
    if (!load_folders() && alone)
    {
        forget_old_playlists();
        forget_folders();
        return readable;
    }
    fld.claim = !listed;
    playlist_dir(dir, sizeof(dir));
    /* made again by the next import if lost (FW_CACHE): without it, that
       one goes by the mark in the files */
    struct fw list_out;
    const bool opened = fw_open(&list_out, PLAYLISTS_FILE, FW_CACHE, NULL, 0);
    if (p && opened)
    {
        list_line(&list_out, "version %d\n", PLAYLISTS_VERSION);
        smart_import_begin();
        off_t pos = next_record(list, le32(p + 4), end);
        int count = le32(p + 8);

        mkdir(dir);
        for (int i = 0; i < count; i++)
        {
            p = db_record(pos, "mhyp", 12);
            const off_t next = p ? record_end(pos, p, end) : -1;
            if (next < 0)
            {
                pls.read_failed = true;
                break;
            }
            write_playlist(pos, next, dir, &list_out, &out, alone);
            pos = next;
            if (alone)
                reading(1, i + 1, count);
            yield();
        }
        /* A folder is made for a playlist in it, or in one in it, and not for
           nothing: one left empty after this goes, as do those of the last
           import that have nothing now */
        for (size_t k = 0; k < fld.made_len; k += strlen(fld.made + k) + 1)
            list_line(&list_out, "dir %s\n", fld.made + k);
        /* only once this import's are written, and all of iTunesDB's were
           read: nothing goes if they weren't */
        if (!pls.read_failed)
            remove_old_playlists(dir, listed, fld.made, fld.made_len,
                                 &list_out);
        /* a list that didn't all write, or of the part read, leaves the old
           one */
        /* the rules of all, or the old ones */
        smart_import_end(!pls.read_failed);
        readable = !pls.read_failed;
        if (fw_close(&list_out, !pls.list_failed && !pls.read_failed))
            remove(PLAYLISTS_FAILED); /* of an iTunesDB before */
    }
    else if (opened)
        fw_close(&list_out, false);
    forget_old_playlists();
    forget_folders();
    return readable;
}

/* Ratings and play counts. iTunesDB is read once, at boot after a sync,
   and the tracks' counts are kept in a file until the database has the
   files Finder added, then go into it. */

#define PENDING_FILE    ROCKBOX_DIR "/itunesdb_pending.dat"
#define PENDING_MAGIC   0x50494252 /* "RBIP" */
#define PENDING_VERSION 3

struct pending_header {
    uint32_t magic;
    uint32_t version;
    uint32_t db_size;   /* of the iTunesDB they are from */
    uint32_t db_mtime;
    uint32_t count;
    uint32_t complete;  /* every track listed was read */
    uint32_t dates_sum; /* of the dates after the last plays */
};

static struct {
    int handle;
    struct pending_header hdr;
    struct state_entry *tracks; /* in iTunesDB order */
    uint32_t *by_hash;          /* their indexes, by path hash */
    uint32_t *lastplayed;       /* of each, Mac time */
    int dates_handle;           /* for the pass alone (load_pending_dates()) */
    uint32_t *added;            /* of each, struct track's */
    uint32_t *released;
} pend;

/* The tracks, then an empty entry for each podcast show, as Finder counts
   them: it drops a Play Counts with an entry fewer or more than it has
   tracks. Then the time each was last played, then when each was added,
   then when each was released. */
static void write_pending(void)
{
    struct pending_header hdr = {
        PENDING_MAGIC, PENDING_VERSION, db_size, db_mtime,
        db.count + db.shows, db.complete, 2166136261u
    };
    const struct state_entry show = { 0 };
    const uint32_t never = 0;
    for (int i = 0; i < db.count; i++)
        hdr.dates_sum = ((hdr.dates_sum ^ db.tracks[i].added) * 16777619u
                         ^ db.tracks[i].released) * 16777619u;
    if (!out_open(PENDING_FILE, FW_KEEP))
        return;
    fw_put(&out, &hdr, sizeof(hdr));
    for (int i = 0; i < db.count; i++)
        fw_put(&out, &db.tracks[i].st, sizeof(struct state_entry));
    for (int i = 0; i < db.shows; i++)
        fw_put(&out, &show, sizeof(show));
    for (int i = 0; i < db.count; i++)
        fw_put(&out, &db.tracks[i].lastplayed, sizeof(uint32_t));
    for (int i = 0; i < db.shows; i++)
        fw_put(&out, &never, sizeof(never));
    for (int i = 0; i < db.count; i++)
        fw_put(&out, &db.tracks[i].added, sizeof(uint32_t));
    for (int i = 0; i < db.shows; i++)
        fw_put(&out, &never, sizeof(never));
    for (int i = 0; i < db.count; i++)
        fw_put(&out, &db.tracks[i].released, sizeof(uint32_t));
    for (int i = 0; i < db.shows; i++)
        fw_put(&out, &never, sizeof(never));
    /* none rather than one of an iTunesDB before, as when this was written
       in its place */
    if (!fw_close(&out, true))
        remove(PENDING_FILE);
}

/* The tracks' digests as the database will have them once this import is
   done (new_track_metadata()), by hash, for tags_promote(). Only from all of
   iTunesDB: those of the tracks not read would be lost, and their changes
   with them; the last import's file stays, and so do its candidates. */
static void write_tags_pending(void)
{
    remove(TAGS_PENDING);
    if (!db.complete)
        return;
    struct tags_header hdr = {
        TAGS_MAGIC, TAGS_VERSION, db_size, db_mtime, 0, { 0 }
    };
    for (int i = 0; i < db.count; i++)
    {
        const struct track *t = &db.tracks[db.by_hash[i]];
        /* a path twice, as in a damaged iTunesDB: once */
        if (t->st.hash && (i == 0
                           || db.tracks[db.by_hash[i - 1]].st.hash != t->st.hash))
            hdr.count++;
    }
    if (!out_open(TAGS_PENDING, FW_CACHE))
        return;
    fw_put(&out, &hdr, sizeof(hdr));
    for (int i = 0; i < db.count; i++)
    {
        const struct track *t = &db.tracks[db.by_hash[i]];
        if (!t->st.hash || (i > 0
                            && db.tracks[db.by_hash[i - 1]].st.hash == t->st.hash))
            continue;
        const struct tags_entry e = { t->st.hash, t->digest, 0 };
        fw_put(&out, &e, sizeof(e));
    }
    fw_close(&out, true);
}

/* The digest of the Finder song at path as not known, in the import's file
   and the last one's: the scan has added it again from its file's tags, not
   from iTunesDB, which an import with the database in memory then compares
   it with (meta.compare_all) */
void itunesdb_tags_forget(const char *path)
{
    static const char *const files[] = { TAGS_PENDING, TAGS_FILE };
    const uint64_t hash = itunesdb_path_hash(path);
    for (unsigned f = 0; f < ARRAYLEN(files); f++)
    {
        struct tags_header hdr;
        struct tags_entry e;
        const int fd = open(files[f], O_RDWR);
        if (fd < 0)
            continue;
        if (read(fd, &hdr, sizeof(hdr)) == sizeof(hdr)
            && hdr.magic == TAGS_MAGIC && hdr.version == TAGS_VERSION
            && hdr.count <= COUNT_MAX)
        {
            int lo = 0, hi = (int)hdr.count - 1;
            while (lo <= hi)
            {
                const int mid = (lo + hi) / 2;
                const off_t at = sizeof(hdr) + (off_t)mid * sizeof(e);
                if (lseek(fd, at, SEEK_SET) != at
                    || read(fd, &e, sizeof(e)) != sizeof(e))
                    break;
                if (e.hash == hash)
                {
                    if (e.digest)
                    {
                        e.digest = 0;
                        if (lseek(fd, at, SEEK_SET) == at)
                            write(fd, &e, sizeof(e));
                    }
                    break;
                }
                if (e.hash < hash)
                    lo = mid + 1;
                else
                    hi = mid - 1;
            }
        }
        close(fd);
    }
}

void itunesdb_tags_hold(void)
{
    remove(TAGS_PENDING);
}

/* The digests of this import's tracks in place of the last one's, now that
   the database has their tags: only those of the iTunesDB just imported. A
   cut between the removal and the rename leaves the import's file, which
   the next import puts in place first (tags_recover()) */
static void tags_promote(void)
{
    struct tags_header hdr;
    const int fd = open(TAGS_PENDING, O_RDONLY);
    if (fd < 0)
        return;
    const bool ok = read(fd, &hdr, sizeof(hdr)) == sizeof(hdr)
        && hdr.magic == TAGS_MAGIC && hdr.version == TAGS_VERSION
        && hdr.db_size == pend.hdr.db_size && hdr.db_mtime == pend.hdr.db_mtime;
    close(fd);
    if (ok)
    {
        remove(TAGS_FILE);
        if (rename(TAGS_PENDING, TAGS_FILE) == 0)
            return;
    }
    remove(TAGS_PENDING);
}

static void unload_pending(void)
{
    if (pend.handle > 0)
    {
        core_unpin(pend.handle);
        core_free(pend.handle);
    }
    if (pend.dates_handle > 0)
    {
        core_unpin(pend.dates_handle);
        core_free(pend.dates_handle);
    }
    memset(&pend, 0, sizeof(pend));
}

/* The dates of the pending file loaded, for the pass: a sync with nothing new
   goes by their sum and doesn't read them. False without them: no pass. */
static bool load_pending_dates(void)
{
    const size_t dates = pend.hdr.count * sizeof(uint32_t);
    const int fd = open(PENDING_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    pend.dates_handle = db_alloc(MAX(2 * dates, sizeof(uint32_t)));
    bool ok = pend.dates_handle > 0;
    if (ok)
    {
        core_pin(pend.dates_handle);
        pend.added = core_get_data(pend.dates_handle);
        pend.released = pend.added + pend.hdr.count;
        const off_t at = sizeof(pend.hdr) + pend.hdr.count
                         * (sizeof(struct state_entry) + sizeof(uint32_t));
        ok = lseek(fd, at, SEEK_SET) == at
             && read(fd, pend.added, 2 * dates) == (ssize_t)(2 * dates);
    }
    else
        pend.dates_handle = 0;
    close(fd);
    return ok;
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
    uint32_t size = 0, mtime = 0;
    const char *step = "header"; /* the one that fails, for the health log */
    memset(&pend, 0, sizeof(pend));
    int fd = open(PENDING_FILE, O_RDONLY);
    if (fd < 0)
    {
        /* a library without iTunesDB has none to open */
        if (db_seen)
            health_note("import not completed: pending not loaded (open): rc"
                        " %d, errno %d; iTunesDB read %lu/%lx", fd, errno,
                        (unsigned long)db_size, (unsigned long)db_mtime);
        return false;
    }
    bool ok = read(fd, &pend.hdr, sizeof(pend.hdr)) == sizeof(pend.hdr)
        && pend.hdr.magic == PENDING_MAGIC
        && pend.hdr.version == PENDING_VERSION
        && pend.hdr.count <= COUNT_MAX;
    if (ok)
    {
        step = "stamp";
        ok = db_stamp(&size, &mtime) && pend.hdr.db_size == size
            && pend.hdr.db_mtime == mtime;
    }
    if (ok)
    {
        size_t entries = pend.hdr.count * sizeof(struct state_entry);
        size_t times = pend.hdr.count * sizeof(uint32_t);
        step = "alloc";
        pend.handle = db_alloc(MAX(entries + 2 * times, sizeof(uint32_t)));
        ok = pend.handle > 0;
        if (ok)
        {
            step = "read";
            core_pin(pend.handle);
            pend.tracks = core_get_data(pend.handle);
            pend.by_hash = (uint32_t *)((char *)pend.tracks + entries);
            pend.lastplayed = pend.by_hash + pend.hdr.count;
            ok = read(fd, pend.tracks, entries) == (ssize_t)entries
                 && read(fd, pend.lastplayed, times) == (ssize_t)times;
        }
    }
    /* errno is the last error set anywhere: this step's only when "at" is
       short of "needed" */
    if (!ok)
        health_note("import not completed: pending not loaded (%s): at %ld of"
                    " %ld, %lu needed, errno %d; magic %lx version %lu, %lu"
                    " songs, handle %d; iTunesDB %lu/%lx, now %lu/%lx",
                    step, (long)lseek(fd, 0, SEEK_CUR), (long)ffilesize(fd),
                    (unsigned long)(sizeof(pend.hdr) + pend.hdr.count
                        * (sizeof(struct state_entry) + sizeof(uint32_t))),
                    errno, (unsigned long)pend.hdr.magic,
                    (unsigned long)pend.hdr.version,
                    (unsigned long)pend.hdr.count, pend.handle,
                    (unsigned long)pend.hdr.db_size,
                    (unsigned long)pend.hdr.db_mtime,
                    (unsigned long)size, (unsigned long)mtime);
    close(fd);
    if (!ok)
    {
        unload_pending();
        return false;
    }
    return true;
}

static void index_pending(void)
{
    for (uint32_t i = 0; i < pend.hdr.count; i++)
        pend.by_hash[i] = i;
    qsort_yielding(pend.by_hash, pend.hdr.count, sizeof(uint32_t), cmp_pending_hash);
}

/* what IMPORTED_FILE holds */
struct imported {
    int32_t commitid;
    uint32_t pass;      /* IMPORT_PASS */
    uint32_t dates_sum; /* the pending header's */
};

/* Whether the tracks and their counts are those of the last import, and the database
   is the one it went through. Compares in the room of the index, not made yet. */
static bool pending_is_imported(void)
{
    struct imported was;
    int fd = open(IMPORTED_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    /* and its dates: one changed on the Mac alone is new, though no count
       is */
    bool ok = read(fd, &was, sizeof(was)) == sizeof(was)
        && was.commitid == tagcache_get_commitid()
        && was.pass == IMPORT_PASS && was.dates_sum == pend.hdr.dates_sum;
    close(fd);
    if (!ok)
        return false;

    struct state_header hdr;
    fd = open(STATE_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    ok = read(fd, &hdr, sizeof(hdr)) == sizeof(hdr) && hdr.magic == STATE_MAGIC
        && hdr.version == STATE_VERSION && hdr.count == pend.hdr.count;
    const size_t total = pend.hdr.count * sizeof(struct state_entry);
    const size_t room = pend.hdr.count * sizeof(uint32_t);
    for (size_t at = 0; ok && at < total; at += room)
    {
        const size_t part = MIN(room, total - at);
        ok = read(fd, pend.by_hash, part) == (ssize_t)part
            && !memcmp(pend.by_hash, (const char *)pend.tracks + at, part);
    }
    close(fd);
    return ok;
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

/* What Play Counts has offered Finder, which takes a file whole or drops
   it: written as USB connects, before Play Counts, for each song whose plays
   or rating differ from itunesdb.dat, and resolved by the next import, which
   keeps what Finder didn't take. */
#define UNSENT_FILE    ROCKBOX_DIR "/itunesdb_unsent.dat"
#define UNSENT_MAGIC   0x55494252 /* "RBIU" */
#define UNSENT_VERSION 1

struct unsent_header {
    uint32_t magic;
    uint32_t version;
    uint32_t db_size;   /* of the iTunesDB of the itunesdb.dat counted from */
    uint32_t db_mtime;
    uint32_t count;
    uint8_t written;    /* a Play Counts of them by the last connection */
    uint8_t went;       /* one of them went from the disk: Finder read it */
    uint8_t made;       /* by an import, from the database: made again until
                           its pass begins, with what was played since */
    uint8_t unused;
};

struct unsent_entry {
    uint64_t hash;
    uint32_t base;      /* plays in itunesdb.dat */
    uint32_t played;    /* in the database at the connection */
    uint32_t went;      /* of them, in files that went from the disk */
    uint32_t out;       /* in the file on the disk */
    uint32_t last;      /* last played in the last file that went, Mac time */
    uint32_t out_last;  /* in the file on the disk */
    uint8_t base_rating; /* 0..100, itunesdb.dat's */
    uint8_t rating;     /* the database's */
    uint8_t unused[6];
};

static struct {
    int handle;
    struct unsent_header hdr;
    struct unsent_entry *e;     /* by hash */
    bool taken;                 /* Finder took what went */
} uns;

static bool playcounts_left(void);
static bool unsent_make(void);

static void unload_unsent(void)
{
    if (uns.handle > 0)
    {
        core_unpin(uns.handle);
        core_free(uns.handle);
    }
    memset(&uns, 0, sizeof(uns));
}

static int cmp_unsent(const void *a, const void *b)
{
    return cmp_u64(&((const struct unsent_entry *)a)->hash,
                   &((const struct unsent_entry *)b)->hash);
}

/* a Play Counts written and gone from the disk went to Finder: out becomes
   went */
static bool load_unsent(void)
{
    memset(&uns, 0, sizeof(uns));
    const int fd = open(UNSENT_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    bool ok = read(fd, &uns.hdr, sizeof(uns.hdr)) == sizeof(uns.hdr)
        && uns.hdr.magic == UNSENT_MAGIC && uns.hdr.version == UNSENT_VERSION
        && uns.hdr.count > 0 && uns.hdr.count <= COUNT_MAX;
    if (ok)
    {
        const size_t bytes = uns.hdr.count * sizeof(struct unsent_entry);
        uns.handle = db_alloc(bytes);
        ok = uns.handle > 0;
        if (ok)
        {
            core_pin(uns.handle);
            uns.e = core_get_data(uns.handle);
            ok = read(fd, uns.e, bytes) == (ssize_t)bytes;
        }
    }
    close(fd);
    if (!ok)
    {
        unload_unsent();
        return false;
    }
    if (uns.hdr.written && !playcounts_left())
    {
        uns.hdr.went = true;
        for (uint32_t i = 0; i < uns.hdr.count; i++)
        {
            struct unsent_entry *u = &uns.e[i];
            u->went += u->out;
            if (u->out)
                u->last = u->out_last;
            u->out = u->out_last = 0;
        }
    }
    return true;
}

static const struct unsent_entry *find_unsent(uint64_t hash)
{
    int lo = 0, hi = (int)uns.hdr.count - 1;
    while (uns.handle > 0 && lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (uns.e[mid].hash == hash)
            return &uns.e[mid];
        if (uns.e[mid].hash < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* The songs a sync moved, by their old paths' hashes (sort_moved_list()),
   and for each new path, the old entry's numbers: the pass removes an old
   entry before it comes to the new one, which the scan added at the end */
struct moved_song {
    uint64_t hash;              /* of the new path */
    struct tagcache_runtime rt; /* of the old entry */
    bool kept;
};

static struct {
    int handle;
    int count;
    struct itunesdb_moved *by_old;
    struct moved_song *by_new;
} moves;

static int cmp_moved_song(const void *a, const void *b)
{
    return cmp_u64(&((const struct moved_song *)a)->hash,
                   &((const struct moved_song *)b)->hash);
}

static void load_moves(void)
{
    memset(&moves, 0, sizeof(moves));
    const int fd = open(ITUNESDB_MOVED_LIST, O_RDONLY);
    if (fd < 0)
        return;
    const int n = ffilesize(fd) / sizeof(struct itunesdb_moved);
    if (n > 0 && n <= COUNT_MAX)
        moves.handle = db_alloc(n * (sizeof(struct itunesdb_moved)
                                     + sizeof(struct moved_song)));
    if (moves.handle > 0)
    {
        core_pin(moves.handle);
        moves.by_old = core_get_data(moves.handle);
        moves.by_new = (struct moved_song *)(moves.by_old + n);
        if (read(fd, moves.by_old, n * sizeof(*moves.by_old))
            == (ssize_t)(n * sizeof(*moves.by_old)))
        {
            moves.count = n;
            memset(moves.by_new, 0, n * sizeof(*moves.by_new));
            for (int i = 0; i < n; i++)
            {
                char *path = moves.by_old[i].path;
                path[sizeof(moves.by_old[i].path) - 1] = '\0';
                moves.by_new[i].hash = itunesdb_path_hash(path);
            }
            qsort(moves.by_new, n, sizeof(*moves.by_new), cmp_moved_song);
        }
    }
    close(fd);
}

static void unload_moves(void)
{
    if (moves.handle > 0)
    {
        core_unpin(moves.handle);
        core_free(moves.handle);
    }
    memset(&moves, 0, sizeof(moves));
}

/* the hash of the path a song moved to from this one's, 0 for none */
static uint64_t moved_to(uint64_t old_hash)
{
    int lo = 0, hi = moves.count - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (moves.by_old[mid].old_hash == old_hash)
            return itunesdb_path_hash(moves.by_old[mid].path);
        if (moves.by_old[mid].old_hash < old_hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

static struct moved_song *find_moved(uint64_t hash)
{
    int lo = 0, hi = moves.count - 1;
    while (hash && lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (moves.by_new[mid].hash == hash)
            return &moves.by_new[mid];
        if (moves.by_new[mid].hash < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* the entries under their songs' new paths */
static void unsent_moved(void)
{
    for (uint32_t i = 0; i < uns.hdr.count; i++)
    {
        const uint64_t hash = moved_to(uns.e[i].hash);
        if (hash)
            uns.e[i].hash = hash;
    }
    qsort(uns.e, uns.hdr.count, sizeof(*uns.e), cmp_unsent);
}

/* Whether Finder took the files that went, by the new iTunesDB: a file
   taken gives each song the last play sent, to the second, and no fewer plays
   than sent. Finder takes or drops a file whole, so the songs vote; with none
   to say, taken, as a play counted twice on the Mac can't be taken back. */
static bool unsent_taken(void)
{
    int took = 0, dropped = 0;
    for (uint32_t i = 0; i < uns.hdr.count; i++)
    {
        const struct unsent_entry *u = &uns.e[i];
        const struct state_entry *t = find_pending(u->hash);
        /* a podcast or an audiobook can't say: Finder leaves an episode's
           count at 0 though it's played, which read as a file dropped at
           every sync after */
        if (!t || (t->media & (MEDIA_PODCAST | MEDIA_AUDIOBOOK)))
            continue;
        const uint32_t last = pend.lastplayed[t - pend.tracks];
        if (u->went)
        {
            if (u->last && last == u->last)
                took++;
            else if (t->playcount < u->base + u->went || last < u->last)
                dropped++;
        }
        else if (u->rating != u->base_rating)
        {
            if (t->rating == u->rating)
                took++;
            else if (t->rating == u->base_rating)
                dropped++;
        }
    }
    return uns.hdr.went && took >= dropped;
}

/* after a sync: the offers made against the iTunesDB the last import was
   of; false when they can't be known, and the import waits */
static bool unsent_begin(bool passing)
{
    uint32_t size, mtime;
    const bool usable = load_unsent()
        && (uns.hdr.db_size != pend.hdr.db_size
            || uns.hdr.db_mtime != pend.hdr.db_mtime)
        && (!itunesdb_imported_stamp(&size, &mtime)
            || (size == uns.hdr.db_size && mtime == uns.hdr.db_mtime))
        && (!uns.hdr.made || passing);
    if (!usable)
    {
        unload_unsent();
        if (!unsent_make())
            return false;
        if (!load_unsent())
            return !file_exists(UNSENT_FILE);
    }
    unsent_moved();
    uns.taken = unsent_taken();
    return true;
}

/* counts already from this iTunesDB with the unsynced plays (a restore, a
   new STATE_VERSION, or the pass made again over the same iTunesDB,
   itunesdb_pass_due()): leave them */
static bool counts_kept;

static bool counts_of_this_itunesdb(void)
{
    uint32_t size, mtime;
    return tagcache_restore_finder()
        || (itunesdb_imported_stamp(&size, &mtime)
            && size == pend.hdr.db_size && mtime == pend.hdr.db_mtime);
}

/* the Mac's places the pass takes, which the flags have (below) */
static bool place_from_mac(uint64_t hash, struct tagcache_runtime *rt);
static bool flags_places_wait(void);
static void flags_places_taken(void);

/* 1 to set the counts and the dates, 0 to leave the entry, -1 to remove it */
static int update_entry(const char *path, struct tagcache_runtime *rt,
                        void *data)
{
    (void)data;
    const uint64_t hash = itunesdb_path_hash(path);
    const struct state_entry *t = find_pending(hash);
    if (!t)
    {
        /* Finder renames and removes the files in its folder as it syncs,
           so one that iTunesDB doesn't list is left from an older sync.
           Only trusted when all of iTunesDB could be read. */
        if (!pend.hdr.complete || !itunesdb_synced_path(path))
            return 0;
        struct moved_song *m = find_moved(moved_to(hash));
        if (m)
        {
            m->rt = *rt;
            m->kept = true;
        }
        return -1;
    }
    /* its dates are iTunesDB's whatever became of its file: the scan takes
       them only for a new path, and not for all of those (metadata_room()) */
    const size_t track = t - pend.tracks;
    if (pend.added[track])
        rt->dateadded = pend.added[track];
    const uint32_t released = pend.released[track];
    rt->date = released >= RELEASED_DATE ? released : 0;
    rt->year = released >= RELEASED_DATE ? released / 10000 : released;
    /* the counts stay, the Mac's place is still taken */
    if (counts_kept)
    {
        place_from_mac(hash, rt);
        return 1;
    }
    /* a moved song's new entry keeps its old entry's numbers */
    const struct moved_song *m = find_moved(hash);
    if (m && m->kept)
    {
        rt->lastplayed = m->rt.lastplayed;
        rt->playtime = m->rt.playtime;
        rt->commitid = m->rt.commitid;
        rt->lastelapsed = m->rt.lastelapsed;
        rt->lastoffset = m->rt.lastoffset;
    }
    rt->playcount = t->playcount;
    rt->rating = t->rating / 10; /* the database counts half stars */
    const struct unsent_entry *u = find_unsent(hash);
    if (u)
    {
        /* taken: what went, as far as the Mac's count has it, as another
           file of the same iTunesDB may have been dropped */
        const uint32_t had = uns.taken ? MIN(u->base + u->went, t->playcount)
                                       : u->base;
        if (u->played > had)
            rt->playcount += u->played - had;
        if (!uns.taken && u->rating != u->base_rating)
            rt->rating = u->rating / 10;
    }
    place_from_mac(hash, rt);
    return 1;
}

/* the counts the plays on the iPod are counted from */
static bool write_state(void)
{
    struct state_header hdr = {
        STATE_MAGIC, STATE_VERSION, pend.hdr.db_size, pend.hdr.db_mtime,
        pend.hdr.count
    };
    /* The state is of this iTunesDB already, the pass made again over it: a
       reading that fell short doesn't replace a whole one. The import isn't
       done: the pending file stays, for the next boot. */
    if (!pend.hdr.complete && !itunesdb_changed())
        return false;
    /* not over the old one: Rockbox truncates a file on its close, so a
       cut there left the new header on the old tracks, whose counts Play
       Counts would then give to others */
    if (!out_open(STATE_FILE, FW_KEEP))
        return false;
    fw_put(&out, &hdr, sizeof(hdr));
    for (uint32_t i = 0; i < pend.hdr.count; i++)
        fw_put(&out, &pend.tracks[i], sizeof(struct state_entry));
    return fw_close(&out, true);
}

/* iTunesDB and its tracks, for the import and for the covers */

static int cache_handle, table_handle;
/* the list of playlists (section 2) in iTunesDB, and its section's end */
static off_t playlists_list, playlists_end;

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
    forget_art_songs();
    memset(&db, 0, sizeof(db));
    db.fd = -1;
    cpu_boost(false);
}

/* Finder counts each podcast show as a track: the shows are the groups of
   the podcasts' playlist in the third section, which has them as a header
   item each above its episodes */
static int count_shows(off_t list, off_t end)
{
    const unsigned char *p = list >= 0 ? db_record(list, "mhlp", 12) : NULL;
    if (!p)
        return 0;
    const int lists = le32(p + 8);
    off_t pos = next_record(list, le32(p + 4), end);
    int shows = 0;
    for (int i = 0; i < lists; i++)
    {
        p = db_record(pos, "mhyp", 44);
        const off_t next = p ? record_end(pos, p, end) : -1;
        if (next < 0)
            break;
        if (p[42] | p[43] << 8)
        {
            const int mhods = le32(p + 12), items = le32(p + 16);
            off_t child = next_record(pos, le32(p + 4), next);
            for (int k = 0; k < (int64_t)mhods + items; k++)
            {
                p = db_record(child, k < mhods ? "mhod" : "mhip", 20);
                const off_t after = p ? record_end(child, p, next)
                                      : -1;
                if (after < 0)
                    break;
                if (k >= mhods && le32(p + 16) == 0x100)
                    shows++;
                child = after;
            }
        }
        pos = next;
    }
    return shows;
}

static bool db_load(void)
{
    off_t tracks_list = -1, tracks_end = -1, podcast_lists = -1;
    off_t podcasts_end = -1, pos, end;
    const unsigned char *p;
    int sections;

    memset(&db, 0, sizeof(db));
    /* fd 0 is a file on the iPod */
    db.fd = -1;
    playlists_list = playlists_end = -1;
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
    {
        memory_short = true;
        return false;
    }
    core_pin(cache_handle);
    void *cache = core_get_data(cache_handle);
    STORAGE_ALIGN_BUFFER(cache, cache_size);
    db.cache = cache;

    /* the sections: tracks are index 1, playlists index 2 */
    p = db_record(0, "mhbd", 24);
    if (!p)
        return false;
    end = record_end(0, p, MH_FILE_END);
    pos = next_record(0, le32(p + 4), end);
    sections = le32(p + 20);
    for (int i = 0; i < sections; i++)
    {
        p = db_record(pos, "mhsd", 16);
        const off_t next = p ? record_end(pos, p, end) : -1;
        if (next < 0)
            break;
        const off_t list = next_record(pos, le32(p + 4), next);
        const uint32_t index = le32(p + 12);
        if (index == 1)
        {
            tracks_list = list;
            tracks_end = next;
        }
        else if (index == 2)
        {
            playlists_list = list;
            playlists_end = next;
        }
        else if (index == 3)
        {
            podcast_lists = list;
            podcasts_end = next;
        }
        pos = next;
    }
    if (tracks_list < 0 || !(p = db_record(tracks_list, "mhlt", 12)))
        return false;

    db.count = le32(p + 8);
    if ((uint32_t)db.count > COUNT_MAX)
        return false;
    size_t table_size =
        db.count * (sizeof(struct track) + 2 * sizeof(uint32_t));
    db.pool_size = db.count * POOL_PER_TRACK;
    table_handle = db_alloc(MAX(table_size + db.pool_size, sizeof(uint32_t)));
    if (table_handle <= 0)
    {
        memory_short = true;
        return false;
    }
    core_pin(table_handle);
    metadata_room(db.count);
    db.tracks = core_get_data(table_handle);
    db.by_id = (uint32_t *)(db.tracks + db.count);
    db.by_hash = db.by_id + db.count;
    db.pool = (char *)(db.by_hash + db.count);
    db.pool_used = 1; /* offset 0 means no path */

    if (!read_tracks(tracks_list, tracks_end))
        return false;
    db.shows = count_shows(podcast_lists, podcasts_end);
    for (int i = 0; i < db.count; i++)
        db.by_id[i] = db.by_hash[i] = i;
    /* behind the UI, 13000 tracks kept it waiting 190 ms; the table is
       pinned meanwhile */
    qsort_yielding(db.by_id, db.count, sizeof(uint32_t), cmp_id);
    qsort_yielding(db.by_hash, db.count, sizeof(uint32_t), cmp_hash);
    return true;
}

/* Only the new iTunesDB's stamp, over the old one: the file's size stays and so do its
   tracks, so a cut leaves it whole */
static bool stamp_state(void)
{
    struct state_header hdr = {
        STATE_MAGIC, STATE_VERSION, pend.hdr.db_size, pend.hdr.db_mtime,
        pend.hdr.count
    };
    const int fd = open(STATE_FILE, O_WRONLY);
    if (fd < 0)
        return false;
    const bool ok = write(fd, &hdr, sizeof(hdr)) == sizeof(hdr);
    return close(fd) == 0 && ok;
}

/* this boot's import waited for its record */
static bool waited;

static int import_progress(void)
{
    uint8_t flags = 0;
    const int fd = open(PROGRESS_FILE, O_RDONLY);
    if (fd >= 0)
    {
        if (read(fd, &flags, 1) != 1)
            flags = 0;
        close(fd);
    }
    return flags;
}

/* a cut leaves the flags before, none, or these, as one in place did */
static bool import_progress_set(int flags)
{
    const uint8_t b = flags;
    return whole_file_write(PROGRESS_FILE, &b, 1);
}

static void note_imported(void)
{
    const struct imported now = {
        tagcache_get_commitid(), IMPORT_PASS, pend.hdr.dates_sum
    };
    /* none rather than one cut short, nor the one before */
    if (!whole_file_write(IMPORTED_FILE, &now, sizeof(now)))
        remove(IMPORTED_FILE);
}

bool itunesdb_pass_due(void)
{
    uint32_t size, mtime;
    struct imported was;
    if (!db_stamp(&size, &mtime))
        return false;
    /* one over this iTunesDB that didn't finish, of any version's file */
    struct pending_header left;
    int fd = open(PENDING_FILE, O_RDONLY);
    if (fd >= 0)
    {
        const ssize_t stamp = offsetof(struct pending_header, count);
        const bool this_db = read(fd, &left, stamp) == stamp
            && left.magic == PENDING_MAGIC && left.db_size == size
            && left.db_mtime == mtime;
        close(fd);
        if (this_db)
            return true;
    }
    /* none: iTunesDB couldn't be read whole, and wouldn't be now */
    fd = open(IMPORTED_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    const bool ok = read(fd, &was, sizeof(was)) == sizeof(was)
                    && was.pass >= IMPORT_PASS;
    close(fd);
    return !ok;
}

void itunesdb_import(void)
{
    if (!load_pending())
        return; /* the next boot reads iTunesDB again */
    if (pending_is_imported() && !flags_places_wait())
    {
        /* A sync with nothing new: nothing to set and nothing to remove. Plays that
           Finder hasn't taken stay, to go in Play Counts next time. A place
           moved on the Mac alone is new, though the counts aren't. */
        if (stamp_state())
        {
            tags_promote();
            remove(PENDING_FILE);
            remove(PROGRESS_FILE);
            /* what was offered before this sync: none of it was taken, or
               the counts would differ, and it is all offered again */
            if (load_unsent() ? (uns.hdr.db_size != pend.hdr.db_size
                                 || uns.hdr.db_mtime != pend.hdr.db_mtime)
                              : file_exists(UNSENT_FILE))
                remove(UNSENT_FILE);
            unload_unsent();
        }
        else
            health_note("import not completed: nothing new to set, state not"
                        " stamped");
    }
    else
    {
        /* not the same as what went through all of it, if cut short */
        remove(IMPORTED_FILE);
        index_pending();
        /* the pending file stays: the next boot's import (itunesdb_pass_due()) */
        if (!load_pending_dates())
        {
            unload_pending();
            return;
        }
        counts_kept = counts_of_this_itunesdb();
        const int progress = import_progress();
        /* a pass begun before and cut short: what it had changed is under
           the commit id of before it */
        if (progress & IMPORT_PASSING)
            tagcache_raise_commitid();
        bool go = true;
        if (!counts_kept)
        {
            load_moves();
            go = unsent_begin(progress & IMPORT_PASSING);
        }
        /* a boot at most, its tries again in it too: a sync's songs wait for
           the import, and would for good with the memory short each time */
        if (!go && (progress & IMPORT_WAITED) && !waited)
        {
            unload_unsent();
            go = true;
        }
        if (!go)
        {
            waited = true;
            /* with the disk full, the pass rather than a wait at every boot */
            if (!import_progress_set(progress | IMPORT_WAITED))
            {
                unload_unsent();
                go = true;
            }
        }
        if (go)
            import_progress_set(progress | IMPORT_PASSING);
        const bool set = go && tagcache_update_runtime(update_entry, NULL);
        if (set && write_state())
        {
            tags_promote();
            remove(PENDING_FILE);
            remove(STATE_OLD_FILE);
            remove(PROGRESS_FILE);
            flags_places_taken();
            if (!counts_kept)
                remove(UNSENT_FILE);
            /* one that couldn't read all of iTunesDB has removed nothing */
            if (pend.hdr.complete)
                note_imported();
        }
        else
            health_note("import not completed: %s", !go ?
                        "put off a boot, the plays offered to Finder not known"
                        : !set ? "counts not set in the database"
                        : "state not written");
        unload_unsent();
        unload_moves();
    }
    logf("itunesdb: %lu tracks imported", (unsigned long)pend.hdr.count);
    unload_pending();
}

/* The database is going, and its counts and ratings with it: Play Counts must not
   give Finder those as new. The next boot imports them again, and knows every track
   as new, whose tags it then has from iTunesDB and needn't read from the files. */
void itunesdb_forget_state(void)
{
    remove(STATE_FILE);
    remove(IMPORTED_FILE);
    /* of the tags the database had. The pending file stays: of the import
       under way, as a database made anew by its scan has them, and made
       again by each import before it can take the name */
    remove(TAGS_FILE);
    /* as a rebuild sets the database aside, its own go after its import */
    if (!tagcache_restoring())
    {
        remove(STATE_OLD_FILE);
        remove(UNSENT_FILE);
        remove(PROGRESS_FILE);
    }
}

void itunesdb_state_aside(void)
{
    rename(STATE_FILE, STATE_OLD_FILE);
    remove(IMPORTED_FILE);
}

/* Play Counts: a header, then for every track in iTunesDB's order the plays
   Finder hasn't had, when it was last played, and its rating; then an empty
   entry for each podcast show. Finder adds the plays to the library, takes
   the ratings and removes the file. */

#define PLAYCOUNTS_FILE  ITUNESDB_DIR "/Play Counts"
/* Of the Play Counts written here last: its size and time, and the iTunesDB it
   was made against. With nothing new to tell, only that one is removed, and
   only once Finder has synced since: one of an older firmware's, as left by an
   update between its sync and the next, held the only count of its plays */
#define PLAYCOUNTS_MARK  ROCKBOX_DIR "/itunesdb_playcounts.dat"
struct playcounts_mark {
    uint32_t size, mtime;       /* of Play Counts */
    uint32_t db_size, db_mtime; /* of iTunesDB */
};

static void playcounts_mark_write(void)
{
    struct playcounts_mark m;
    if (!dir_file_stamp("Play Counts", &m.size, &m.mtime)
        || !db_stamp(&m.db_size, &m.db_mtime))
        return;
    /* none rather than one cut short, nor the one before */
    if (!whole_file_write(PLAYCOUNTS_MARK, &m, sizeof(m)))
        remove(PLAYCOUNTS_MARK);
}

/* the Play Counts written here last is still on the disk, as Finder hasn't
   read it */
static bool playcounts_mark_left(struct playcounts_mark *m)
{
    uint32_t size, mtime;
    const int fd = open(PLAYCOUNTS_MARK, O_RDONLY);
    if (fd < 0)
        return false;
    const bool read_ok = read(fd, m, sizeof(*m)) == sizeof(*m);
    close(fd);
    return read_ok && dir_file_stamp("Play Counts", &size, &mtime)
           && size == m->size && mtime == m->mtime;
}

static bool playcounts_left(void)
{
    struct playcounts_mark m;
    return playcounts_mark_left(&m);
}

/* nothing new to tell Finder: ours, left unread from before the last sync,
   whose import has had its plays */
static void playcounts_remove_taken(void)
{
    struct playcounts_mark m;
    uint32_t db_size, db_mtime;
    if (playcounts_mark_left(&m) && db_stamp(&db_size, &db_mtime)
        && (db_size != m.db_size || db_mtime != m.db_mtime))
    {
        remove(PLAYCOUNTS_FILE);
        remove(PLAYCOUNTS_MARK);
    }
}
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
    int handle;
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

/* the tracks of a state file, and what the database has of each */
static bool pc_load(const char *file, uint32_t count)
{
    pc.count = count;
    pc.handle = db_alloc(pc.count * (sizeof(struct state_entry)
                           + sizeof(uint32_t) + sizeof(struct played)));
    if (pc.handle <= 0)
        return false;
    core_pin(pc.handle);
    pc.state = core_get_data(pc.handle);
    pc.by_hash = (uint32_t *)(pc.state + pc.count);
    pc.now = (struct played *)(pc.by_hash + pc.count);
    memset(pc.now, 0, pc.count * sizeof(struct played));

    const int fd = open(file, O_RDONLY);
    if (fd < 0)
        return false;
    const ssize_t bytes = pc.count * sizeof(struct state_entry);
    const bool ok = lseek(fd, sizeof(struct state_header), SEEK_SET)
                    == sizeof(struct state_header)
                    && read(fd, pc.state, bytes) == bytes;
    close(fd);
    if (!ok)
        return false;
    for (int i = 0; i < pc.count; i++)
        pc.by_hash[i] = i;
    qsort(pc.by_hash, pc.count, sizeof(uint32_t), cmp_state_hash);
    return tagcache_update_runtime(read_played, NULL);
}

static void pc_free(void)
{
    if (pc.handle > 0)
    {
        core_unpin(pc.handle);
        core_free(pc.handle);
    }
    memset(&pc, 0, sizeof(pc));
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

/* On-The-Go was handed to Finder as the stock firmware gives it, in
   OTGPlaylistInfo, with a note of ours beside it: Finder made "On-The-Go 1" of
   it on the iPod alone, never in Music, as it does of the stock firmware's,
   and dropped it later. On-The-Go stays on the iPod now, and what's left of
   that goes: our OTGPlaylistInfo, as the note has it, then the note. The
   stock firmware's own, of a dual boot, aren't touched. */
#define OTG_INFO_NAME  "OTGPlaylistInfo"
#define OTG_INFO       ITUNESDB_DIR "/" OTG_INFO_NAME
#define OTG_MARK       ROCKBOX_DIR "/itunesdb_otg.dat"
#define OTG_MARK_MAGIC 0x4f544731   /* "OTG1" */

struct otg_mark {
    uint32_t magic;
    uint32_t size, mtime;       /* of OTGPlaylistInfo */
    uint32_t db_size, db_mtime; /* of the iTunesDB its indices were in */
    uint32_t count;             /* the hashes of its songs after this */
};

static void otg_cleanup(void)
{
    struct otg_mark m;
    const int fd = open(OTG_MARK, O_RDONLY);
    if (fd < 0)
        return;
    const bool ok = read(fd, &m, sizeof(m)) == sizeof(m)
                    && m.magic == OTG_MARK_MAGIC;
    close(fd);
    uint32_t size, mtime;
    if (ok && dir_file_stamp(OTG_INFO_NAME, &size, &mtime)
        && size == m.size && mtime == m.mtime)
        remove(OTG_INFO);
    remove(OTG_MARK);
}

static uint32_t podcast_bookmark(uint64_t hash);

static uint32_t plays_to_send(int i)
{
    const struct played *now = &pc.now[i];
    const struct unsent_entry *u = find_unsent(pc.state[i].hash);
    const uint32_t had = pc.state[i].playcount + (u ? u->went : 0);
    return now->found && now->playcount > had ? now->playcount - had : 0;
}

static uint32_t sent_time(const struct played *now)
{
    return (long)now->lastplayed >= LASTPLAYED_TIME ?
           now->lastplayed + MAC_EPOCH : 0;
}

/* what a Play Counts written now offers, by hash; none when nothing differs
   from itunesdb.dat */
static bool write_unsent(uint32_t db_size, uint32_t db_mtime, bool written,
                         bool made)
{
    struct unsent_header h = {
        UNSENT_MAGIC, UNSENT_VERSION, db_size, db_mtime, 0, written,
        uns.hdr.went, made, 0
    };
    if (!out_open(UNSENT_FILE, FW_KEEP))
        return false;
    fw_put(&out, &h, sizeof(h));
    for (int k = 0; k < pc.count; k++)
    {
        const int i = pc.by_hash[k];
        const struct state_entry *st = &pc.state[i];
        const struct played *now = &pc.now[i];
        if (!now->found)
            continue;
        const struct unsent_entry *old = find_unsent(st->hash);
        struct unsent_entry u = {
            .hash = st->hash, .base = st->playcount, .played = now->playcount,
            .went = old ? old->went : 0, .out = plays_to_send(i),
            .last = old ? old->last : 0,
            .base_rating = st->rating, .rating = now->rating * 10
        };
        u.out_last = u.out ? sent_time(now) : 0;
        if (u.played > u.base || u.rating != u.base_rating || u.went)
        {
            fw_put(&out, &u, sizeof(u));
            h.count++;
        }
    }
    fw_seek(&out, 0);
    fw_put(&out, &h, sizeof(h));
    if (h.count)
        return fw_close(&out, true);
    /* none to keep: no file */
    const bool ok = fw_end(&out);
    fw_place(UNSENT_FILE, FW_KEEP, false);
    if (ok)
        remove(UNSENT_FILE);
    return ok;
}

/* None to go by, as at the first sync after an update: what the firmware
   before offered at the connection, from the database against the state the
   import replaces. Written before the import's pass, which a cut repeats;
   false when it can't be, and the pass waits a boot, as after it the
   database would seem to have had nothing to offer. */
static bool unsent_make(void)
{
    struct state_header hdr;
    const char *file = file_exists(STATE_FILE) || !tagcache_restoring() ?
                       STATE_FILE : STATE_OLD_FILE;
    if (!read_state_header(file, &hdr) || hdr.count == 0
        || (hdr.db_size == pend.hdr.db_size && hdr.db_mtime == pend.hdr.db_mtime))
    {
        /* nothing to count from: none to keep, nor one that won't load */
        remove(UNSENT_FILE);
        return true;
    }
    const bool ok = pc_load(file, hdr.count)
                    && write_unsent(hdr.db_size, hdr.db_mtime, true, true);
    pc_free();
    return ok;
}

/* what write_unsent() would write without a Play Counts is on the disk */
static bool unsent_same(void)
{
    int k = 0;
    for (uint32_t j = 0; j < uns.hdr.count; j++)
    {
        const struct unsent_entry *u = &uns.e[j];
        while (k < pc.count && pc.state[pc.by_hash[k]].hash < u->hash)
            k++;
        if (k == pc.count || pc.state[pc.by_hash[k]].hash != u->hash)
            return false;
        const struct played *now = &pc.now[pc.by_hash[k]];
        if (!now->found || now->playcount != u->played
            || now->rating * 10 != u->rating)
            return false;
    }
    return !uns.hdr.written;
}

void itunesdb_write_playcounts(void)
{
    struct state_header hdr;
    uint32_t size, mtime;
    bool changed = false;

    /* the entries go in the order of the iTunesDB the state was made from,
       so only when that is the one on the disk */
    if (!db_stamp(&size, &mtime) || !read_state_header(STATE_FILE, &hdr)
        || hdr.db_size != size || hdr.db_mtime != mtime || hdr.count == 0)
        return;

    otg_cleanup();

    if (!pc_load(STATE_FILE, hdr.count))
        goto done;
    if (!load_unsent() && file_exists(UNSENT_FILE))
    {
        /* what went to Finder is unknown, and would go again */
        goto done;
    }
    if (uns.hdr.db_size != size || uns.hdr.db_mtime != mtime)
        unload_unsent();

    for (int i = 0; i < pc.count && !changed; i++)
        changed = pc.now[i].found
            && (plays_to_send(i) > 0
                || pc.now[i].rating * 10 != pc.state[i].rating);
    if (!changed)
    {
        /* nothing to tell Finder: no file, as after it took the last one */
        if (uns.handle > 0 && !unsent_same())
            write_unsent(size, mtime, false, false);
        playcounts_remove_taken();
        goto done;
    }

    if (!out_open(PLAYCOUNTS_FILE, FW_CACHE))
        goto done;
    unsigned char buf[PLAYCOUNTS_HDR];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "mhdp", 4);
    put32(buf + 4, PLAYCOUNTS_HDR);
    put32(buf + 8, PLAYCOUNTS_ENTRY);
    put32(buf + 12, pc.count);
    fw_put(&out, buf, PLAYCOUNTS_HDR);

    for (int i = 0; i < pc.count; i++)
    {
        const struct state_entry *st = &pc.state[i];
        const struct played *now = &pc.now[i];
        uint32_t plays = 0, last = 0, rating = st->rating;
        if (now->found)
        {
            plays = plays_to_send(i);
            if (plays)
                last = sent_time(now);
            rating = now->rating * 10;
        }
        unsigned char e[PLAYCOUNTS_ENTRY];
        memset(e, 0, sizeof(e));
        put32(e, plays);
        put32(e + 4, last);
        put32(e + 12, rating);
        /* an episode's or a book's place and played mark as the Mac has
           them, which leaves them there as they are */
        if (st->media & (MEDIA_PODCAST | MEDIA_AUDIOBOOK))
        {
            put32(e + 8, podcast_bookmark(st->hash));
            e[0x11] = st->played;
        }
        fw_put(&out, e, sizeof(e));
    }
    /* by another name until what it offers is noted: cut short in between,
       its plays are taken as offered, and offered again after the next
       sync */
    const bool written = fw_end(&out) && write_unsent(size, mtime, true, false);
    if (fw_place(PLAYCOUNTS_FILE, FW_CACHE, written))
        playcounts_mark_write();

done:
    unload_unsent();
    pc_free();
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

/* The tracks with flags. Version 3 has the marks of podcasts and audiobooks
   in the flags (ITUNESDB_MAC_UNPLAYED and after) and a book's bookmark;
   version 4 the compilations too (ITUNESDB_COMPILATION), each a track with
   flags. Their headers and entries are laid out as version 2's, which a
   firmware of another takes for none of its own and reads iTunesDB again;
   version 3's marks are this one's. Written whole by an import and, as a
   mark changes, by itunesdb_save_flags(), each to a file of its own beside
   it that then takes its name: a cut leaves the old file or the new, never
   a part of one. */
#define FLAGS_FILE      ROCKBOX_DIR "/itunesdb_flags.dat"
#define FLAGS_MAGIC     0x46494252 /* "RBIF" */
#define FLAGS_VERSION   4
#define FLAGS_VERSION_3 3
#define FLAGS_VERSION_2 2
#define FLAGS_SPOKEN    (ITUNESDB_PODCAST | ITUNESDB_AUDIOBOOK)

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
    uint32_t bookmark;  /* a podcast's or a book's place on the Mac, ms */
};

static struct index flags_index = { 0, 0, sizeof(struct flags_entry) };
/* the header of the file the index is from, which a save writes again */
static struct flags_header flags_hdr;
/* The marks in memory are newer than the file's; each change counted; and
   the file is being written, by an import or a save, one at a time: these
   change without a yield between a test and a set, the threads on one core */
static bool flags_dirty, flags_busy;
static unsigned flags_changes;
/* the version of the file the import found, whose entries are in memory
   until its own are written: 0 for none */
static int flags_old_version;
/* and the import is of an iTunesDB not taken in yet, a sync's: its pass
   takes the Mac's places. The covers' pass may be the first over it */
static bool flags_new_itunesdb;

/* An import begins: the flags of the one before stay in memory, and their
   file where it is, for this one's to go by */
static void flags_import_begin(bool new_itunesdb)
{
    while (flags_busy)
        sleep(1);
    flags_busy = true;
    flags_new_itunesdb = new_itunesdb;
    flags_old_version = FLAGS_VERSION;
    if (flags_index.handle <= 0)
    {
        struct flags_header hdr = { 0 };
        load_index(&flags_index, FLAGS_FILE, &hdr, sizeof(hdr),
                   FLAGS_MAGIC, FLAGS_VERSION);
        if (flags_index.handle <= 0)
        {
            load_index(&flags_index, FLAGS_FILE, &hdr, sizeof(hdr),
                       FLAGS_MAGIC, FLAGS_VERSION_3);
            flags_old_version = FLAGS_VERSION_3;
        }
        if (flags_index.handle <= 0)
        {
            load_index(&flags_index, FLAGS_FILE, &hdr, sizeof(hdr),
                       FLAGS_MAGIC, FLAGS_VERSION_2);
            flags_old_version = flags_index.handle > 0 ? FLAGS_VERSION_2 : 0;
        }
    }
}

/* and ends, its file written or not: the old flags forgotten, for
   itunesdb_load_tracks() to read what is there */
static void flags_import_end(void)
{
    forget_index(&flags_index);
    flags_old_version = 0;
    flags_dirty = false;
    flags_busy = false;
}

/* A podcast's or a book's marks: the iPod's own if the Mac's is as before,
   else the Mac's; and whether the import's pass takes the Mac's place */
static uint32_t spoken_marks(const struct track *t)
{
    struct flags_entry old;
    const bool had = flags_old_version
                     && find_in_index(&flags_index, t->st.hash, &old);
    const bool marked = had && flags_old_version >= FLAGS_VERSION_3;
    const bool mac_unplayed = t->st.played == 2;
    const bool same = marked && !(old.flags & ITUNESDB_MAC_UNPLAYED) == !mac_unplayed;
    uint32_t marks = mac_unplayed ? ITUNESDB_MAC_UNPLAYED : 0;
    if (same)
        marks |= old.flags & ITUNESDB_UNPLAYED;
    else if (mac_unplayed)
        marks |= ITUNESDB_UNPLAYED;
    /* Only the pass over an iTunesDB not taken in yet. A track new to the
       flags, a bookmark moved, or the Mac's mark turned to played, which
       takes the iPod's place with it; an older version's bookmark counts as
       the same: it had none of a book's. One not taken yet stays so. */
    if (flags_new_itunesdb && (!had || (marked && (old.bookmark != t->bookmark
                                             || (!same && !mac_unplayed)))))
        marks |= ITUNESDB_PLACE_FROM_MAC;
    else if (marked)
        marks |= old.flags & ITUNESDB_PLACE_FROM_MAC;
    return marks;
}

static void write_flags(void)
{
    if (!out_open(FLAGS_FILE, FW_KEEP))
        return; /* the old file stays, with its marks */
    struct flags_header hdr = {
        FLAGS_MAGIC, FLAGS_VERSION, 0, db_size, db_mtime
    };
    fw_put(&out, &hdr, sizeof(hdr));
    /* by path hash, which db.by_hash already has them in */
    for (int i = 0; i < db.count; i++)
    {
        const struct track *t = &db.tracks[db.by_hash[i]];
        if (!t->path || !t->st.flags)
            continue;
        struct flags_entry entry = { t->st.hash, t->st.flags, t->bookmark };
        if (t->st.flags & FLAGS_SPOKEN)
            entry.flags |= spoken_marks(t);
        fw_put(&out, &entry, sizeof(entry));
        hdr.count++;
    }
    /* the count, now that it is known */
    fw_seek(&out, 0);
    fw_put(&out, &hdr, sizeof(hdr));
    /* in the old file's place, which was there until now; none rather than
       the old one if this couldn't be written, as before: read again next
       boot */
    if (!fw_close(&out, true))
        remove(FLAGS_FILE);
    logf("itunesdb: %lu tracks with flags", (unsigned long)hdr.count);
}

/* the entry of this hash in memory, to change in place: NULL for none. Not
   kept over a yield */
static struct flags_entry *flags_entry_of(uint64_t hash)
{
    if (flags_index.handle <= 0)
        return NULL;
    struct flags_entry *entries = core_get_data(flags_index.handle);
    int lo = 0, hi = (int)flags_index.count - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (entries[mid].hash == hash)
            return &entries[mid];
        if (entries[mid].hash < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

void itunesdb_save_flags(void)
{
    if (!flags_dirty || flags_busy || flags_index.handle <= 0
        || flags_hdr.magic != FLAGS_MAGIC || flags_hdr.version != FLAGS_VERSION
        || flags_hdr.count != flags_index.count)
        return;
    flags_busy = true;
    const unsigned changes = flags_changes;
    const size_t size = flags_index.count * sizeof(struct flags_entry);
    /* not through out, which the import may be writing; its own while
       flags_busy is set, off the storage thread's 2 KB stack */
    static struct fw w;
    bool ok = fw_open(&w, FLAGS_FILE, FW_KEEP, NULL, 0);
    if (ok)
    {
        core_pin(flags_index.handle);
        ok = fw_put(&w, &flags_hdr, sizeof(flags_hdr))
             && fw_put(&w, core_get_data(flags_index.handle), size);
        core_unpin(flags_index.handle);
    }
    if (fw_close(&w, ok))
        flags_dirty = changes != flags_changes;
    flags_busy = false;
}

void itunesdb_flush_flags(void)
{
    /* a save under way, as the idle disk's, has what was marked before it:
       this one after it, a few seconds at most */
    for (int wait = 0; flags_busy && wait < 5 * HZ; wait++)
        sleep(1);
    itunesdb_save_flags();
}

bool itunesdb_set_unplayed(const char *path, bool unplayed)
{
    struct flags_entry *e = flags_entry_of(itunesdb_path_hash(path));
    if (!e || !(e->flags & FLAGS_SPOKEN))
        return false;
    if (!(e->flags & ITUNESDB_UNPLAYED) != !unplayed)
    {
        e->flags ^= ITUNESDB_UNPLAYED;
        flags_changes++;
        flags_dirty = true;
        register_storage_idle_func(itunesdb_save_flags);
    }
    return true;
}

/* the Mac's places are taken: none is again by a pass repeated */
static void flags_places_taken(void)
{
    if (flags_index.handle <= 0)
        return;
    struct flags_entry *entries = core_get_data(flags_index.handle);
    bool changed = false;
    for (uint32_t i = 0; i < flags_index.count; i++)
    {
        changed = changed || (entries[i].flags & ITUNESDB_PLACE_FROM_MAC);
        entries[i].flags &= ~ITUNESDB_PLACE_FROM_MAC;
    }
    if (changed)
    {
        flags_changes++;
        flags_dirty = true;
        itunesdb_save_flags();
    }
}

/* a place of the Mac's waits for the import's pass */
static bool flags_places_wait(void)
{
    if (flags_index.handle <= 0)
        return false;
    const struct flags_entry *entries = core_get_data(flags_index.handle);
    for (uint32_t i = 0; i < flags_index.count; i++)
    {
        if ((entries[i].flags & ITUNESDB_PLACE_FROM_MAC)
            && (entries[i].bookmark
                || !(entries[i].flags & ITUNESDB_MAC_UNPLAYED)))
            return true;
    }
    return false;
}

bool itunesdb_flags_ready(void)
{
    uint32_t size, mtime;
    if (!db_stamp(&size, &mtime))
        return true;
    struct flags_header hdr;
    const int fd = open(FLAGS_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    const bool ready = read(fd, &hdr, sizeof(hdr)) == sizeof(hdr)
        && hdr.magic == FLAGS_MAGIC && hdr.version == FLAGS_VERSION
        && (hdr.count > 0 || (hdr.db_size == size && hdr.db_mtime == mtime));
    close(fd);
    return ready;
}

unsigned itunesdb_hash_flags(uint64_t hash)
{
    struct flags_entry entry;
    if (flags_index.handle <= 0 || !find_in_index(&flags_index, hash, &entry))
        return 0;
    return entry.flags;
}

static uint32_t podcast_bookmark(uint64_t hash)
{
    struct flags_entry entry;
    return find_in_index(&flags_index, hash, &entry) ? entry.bookmark : 0;
}

/* An episode's or a book's place as the Mac has it, into rt, if the import
   flagged it: a bookmark moved there, or none once the Mac calls it played.
   False for a track whose place stays the iPod's */
static bool place_from_mac(uint64_t hash, struct tagcache_runtime *rt)
{
    struct flags_entry entry;
    if (!find_in_index(&flags_index, hash, &entry)
        || !(entry.flags & ITUNESDB_PLACE_FROM_MAC)
        || (!entry.bookmark && (entry.flags & ITUNESDB_MAC_UNPLAYED)))
        return false;
    rt->lastelapsed = entry.bookmark;
    rt->lastoffset = 0;
    return true;
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
    if (!out_open(GAINS_FILE, FW_CACHE))
        return;
    struct flags_header hdr = {
        GAINS_MAGIC, GAINS_VERSION, 0, db_size, db_mtime
    };
    fw_put(&out, &hdr, sizeof(hdr));
    /* by path hash, to be searched */
    for (int i = 0; i < db.count; i++)
    {
        const struct track *t = &db.tracks[db.by_hash[i]];
        if (!t->path || !t->sound_check)
            continue;
        struct gain_entry entry = { t->st.hash, t->sound_check, 0 };
        fw_put(&out, &entry, sizeof(entry));
        hdr.count++;
    }
    fw_seek(&out, 0);
    fw_put(&out, &hdr, sizeof(hdr));
    fw_close(&out, true);
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

static void sort_fields_begin(void)
{
    memset(&sorts, 0, sizeof(sorts));
    sorts.hdr = (struct tagcache_sort_header){
        TAGCACHE_SORT_MAGIC, TAGCACHE_SORT_VERSION, 0, 0xffffffff, 0, 0
    };
    sorts.writing = out_open(TAGCACHE_SORT_FINDER, FW_CACHE);
    if (sorts.writing)
        fw_put(&out, &sorts.hdr, sizeof(sorts.hdr));
}

/* In place of the last ones once all of iTunesDB was read, of its stamp.
   Those that couldn't be written are none rather than missing, as with the
   disk full: the import isn't made again at every boot for them. */
static void sort_fields_end(bool loaded)
{
    if (!loaded)
    {
        if (sorts.writing)
            fw_close(&out, false);
        sorts.writing = false;
        return;
    }
    sorts.hdr.db_size = db_size;
    sorts.hdr.db_mtime = db_mtime;
    bool written = false;
    if (sorts.writing)
    {
        fw_seek(&out, 0);
        fw_put(&out, &sorts.hdr, sizeof(sorts.hdr));
        written = fw_close(&out, true);
    }
    sorts.writing = false;
    if (!written)
    {
        sorts.hdr.count = 0;
        sorts.hdr.crc = 0xffffffff;
        whole_file_write(TAGCACHE_SORT_FINDER, &sorts.hdr, sizeof(sorts.hdr));
    }
    logf("itunesdb: %lu Sort fields", (unsigned long)sorts.hdr.count);
}

static void sort_field_put(int tag, const char *name, const char *value)
{
    if (!*name || !*value)
        return;
    const char *a = sortkey_start(name, true), *b = sortkey_start(value, false);
    if (sortkey_group(a) == sortkey_group(b) && !sortkey_compare_from(a, b))
        return;
    const size_t name_size = strlen(name) + 1, value_size = strlen(value) + 1;
    unsigned char head[TAGCACHE_SORT_PAIR_HEAD];
    if (!tagcache_sort_pair_head(head, tag, name_size, value_size))
        return;
    uint32_t hash = crc_32(head, sizeof(head), 0xffffffff);
    hash = crc_32(name, name_size, hash);
    hash = crc_32(value, value_size, hash);
    for (int i = 0; i < SORT_RECENT; i++)
    {
        if (sorts.recent[i] == hash)
            return;
    }
    sorts.recent[sorts.next] = hash;
    sorts.next = (sorts.next + 1) % SORT_RECENT;
    fw_put(&out, head, sizeof(head));
    fw_put(&out, name, name_size);
    fw_put(&out, value, value_size);
    sorts.hdr.crc = crc_32(head, sizeof(head), sorts.hdr.crc);
    sorts.hdr.crc = crc_32(name, name_size, sorts.hdr.crc);
    sorts.hdr.crc = crc_32(value, value_size, sorts.hdr.crc);
    sorts.hdr.count++;
}

/* whether the string of the mhod at pos is empty, or there's none */
static bool mhod_empty(off_t pos)
{
    const unsigned char *p = pos ? db_record(pos, "mhod", 40) : NULL;
    return !p || mhod_string_size(p) == 0;
}

/* A track's Sort fields for each tag the database has its names in, as the
   scan takes them (add_tagcache()): an album without an album artist is its
   artist's, and the canonical artist is the album artist's for a track
   without one. Read only for a track that has one. */
static void track_sort_fields(const off_t *name_at, const off_t *sort_at)
{
    static char name[TAG_MAXLEN + 1], value[TAG_MAXLEN + 1];
    const bool has_artist = !mhod_empty(name_at[SORT_ARTIST]);
    const bool has_albumartist = !mhod_empty(name_at[SORT_ALBUMARTIST]);
    for (int k = 0; k < SORTS; k++)
    {
        if (!sort_at[k] || !name_at[k]
            || !mhod_string(sort_at[k], value, sizeof(value))
            || !mhod_string(name_at[k], name, sizeof(name)))
            continue;
        switch (k)
        {
            case SORT_ARTIST:
                sort_field_put(tag_artist, name, value);
                sort_field_put(tag_virt_canonicalartist, name, value);
                if (!has_albumartist)
                    sort_field_put(tag_albumartist, name, value);
                break;
            case SORT_ALBUMARTIST:
                sort_field_put(tag_albumartist, name, value);
                if (!has_artist)
                    sort_field_put(tag_virt_canonicalartist, name, value);
                break;
            case SORT_ALBUM:
                sort_field_put(tag_album, name, value);
                break;
            case SORT_TITLE:
                sort_field_put(tag_title, name, value);
                break;
            case SORT_COMPOSER:
                sort_field_put(tag_composer, name, value);
                break;
        }
    }
}

bool itunesdb_sort_ready(void)
{
    uint32_t size, mtime;
    if (!db_stamp(&size, &mtime))
        return true;
    struct tagcache_sort_header hdr;
    int fd = open(TAGCACHE_SORT_FINDER, O_RDONLY);
    if (fd < 0)
        return false;
    const bool ready = read(fd, &hdr, sizeof(hdr)) == sizeof(hdr)
        && hdr.magic == TAGCACHE_SORT_MAGIC
        && hdr.version == TAGCACHE_SORT_VERSION
        && hdr.db_size == size && hdr.db_mtime == mtime;
    close(fd);
    return ready;
}

/* A track's place is worked out from its hash, as the hashes are evenly spread, and a
   read around it nearly always has it. A search by halves took a read for each step, and
   the music starts only after this. */
#define GAIN_WINDOW 4096
static uint32_t find_sound_check(int fd, int count, uint64_t hash)
{
    static unsigned char window[GAIN_WINDOW] STORAGE_ALIGN_ATTR;
    const int head = sizeof(struct flags_header), size = sizeof(struct gain_entry);
    int lo = 0, hi = count - 1;
    int at = (int)(((hash >> 32) * (uint64_t)count) >> 32);

    for (int tries = 0; lo <= hi; tries++)
    {
        const off_t pos = head + (off_t)at * size;
        /* the middle of a window on whole sectors, so all of the entry is in it */
        const off_t start = pos > GAIN_WINDOW / 2
                            ? ALIGN_DOWN(pos - GAIN_WINDOW / 2, 1024) : 0;
        if (lseek(fd, start, SEEK_SET) != start)
            return 0;
        const ssize_t got = read(fd, window, GAIN_WINDOW);
        if (got <= 0)
            return 0;
        const int first = start > head ? (start - head + size - 1) / size : 0;
        const int last = MIN((int)((start + got - head) / size), count) - 1;
        int low = MAX(first, lo), high = MIN(last, hi);
        if (low > high)
            return 0;
        const int bottom = low, top = high;
        while (low <= high)
        {
            const int mid = (low + high) / 2;
            struct gain_entry entry;
            memcpy(&entry, window + (head + (off_t)mid * size - start), size);
            if (entry.hash == hash)
                return entry.sound_check;
            if (entry.hash < hash)
                low = mid + 1;
            else
                high = mid - 1;
        }
        /* below the window, above it, or in it and not there */
        if (low == bottom)
            hi = bottom - 1;
        else if (low > top)
            lo = top + 1;
        else
            return 0;
        /* next door the first time, as it's just outside when it's outside */
        at = tries == 0 ? (low == bottom ? hi : lo) : (lo + hi) / 2;
    }
    return 0;
}

void itunesdb_set_track_gain(struct mp3entry *id3)
{
    struct flags_header hdr;
    uint32_t sound_check = 0;
    int fd = open(GAINS_FILE, O_RDONLY);
    if (fd >= 0)
    {
        if (read_gains_header(fd, &hdr))
            sound_check = find_sound_check(fd, (int)hdr.count,
                                           itunesdb_path_hash(id3->path));
        close(fd);
    }
    if (!sound_check)
    {
        /* the file's own ReplayGain tags then, which only lower it as well */
        id3->track_gain = MIN(id3->track_gain, 1L << 24);
        id3->album_gain = MIN(id3->album_gain, 1L << 24);
        return;
    }
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
           || (itunesdb_track_flags(path) & ITUNESDB_REMEMBERS_PLACE)
           || tagcache_path_kind(path) != 0;
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
#define ART_VERSION     5   /* 2: the format of each entry's frame,
                               3: and its album's slide,
                               4: and the covers of the slides,
                               5: slides of the larger size Cover Flow
                                  shows */
#define ART_FORMATS     4
/* Finder's larger frame of each of ART_FILE's entries, in their order, for
   the menus' cover: a format the iPod Video's own covers don't have, which
   Finder writes once the iPod's description lists it (1024: 320x240, the
   picture 240x240 in the middle). In a file of its own, so that the index
   and all that reads it are as they were. Made with the index and never
   kept past it: removed before a new index is put in place. */
#define LARGE_FILE      ROCKBOX_DIR "/itunesdb_art240.dat"
#define LARGE_MAGIC     0x4c494252 /* "RBIL" */
#define LARGE_VERSION   2   /* 2: the sum of the index it goes with */
#define LARGE_PIXELS    (LCD_HEIGHT * LCD_HEIGHT) /* the most of a picture */
#define FRAME_SIDE      200
#define FRAME_MAX       (FRAME_SIDE * FRAME_SIDE) /* the iPod Video's largest covers */
#define SMALL_FRAME_MAX (100 * 100)  /* what they are checked against */
/* Cover Flow's slides, one an album, made after the import from the cover
   of one of its first tracks: column after column as PictureFlow draws
   them, each in a slot of its own behind a header that says which it is */
#define SLIDES_FILE     ROCKBOX_DIR "/itunesdb_slides.dat"
#define SLIDES_TODO     ROCKBOX_DIR "/itunesdb_slides.todo"
/* every slot's header checked at the next making: a slide recorded as made
   didn't load */
#define SLIDES_CHECK    ROCKBOX_DIR "/itunesdb_slides.check"
/* "RBI4": of frames checked, at the size of Cover Flow's larger covers; each
   slot of a file of the smaller ones, which were "RBI2", is made again */
#define SLIDE_MAGIC     0x34494252
/* of a small frame, the large ones being unlike it: the next import tries
   again, as Finder may have written them by then; "RBI3" before */
#define SLIDE_UNSURE    0x35494252 /* "RBI5" */
/* the colours' record of a made slide: of this size only, as a slide of
   another size isn't made */
#define SLIDE_MADE      (SLIDE_MAGIC ^ ITUNESDB_SLIDE_SIZE)
#define SLIDE_SOURCES   3
/* The colour of each slide's cover, which Now Playing can take on: worked out with the
   slide, as a slide isn't made again, and known of each album when a track plays */
#define COLOURS_FILE    ROCKBOX_DIR "/itunesdb_colours.dat"
#define COLOURS_MAGIC   0x43494252 /* "RBIC" */
#define COLOURS_VERSION 2 /* 2: all of the colour, and not its hue */
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

struct large_header {
    uint32_t magic;
    uint32_t version;
    uint32_t count;     /* ART_FILE's, whose entries these go with */
    uint32_t index;     /* and the sum of that file past its header: an
                           index a firmware without this file made since
                           is another's */
    uint32_t format;    /* of the ithmb files, 0 with no such frame */
    uint32_t pitch;     /* pixels a row of their frames */
    uint32_t frames;    /* entries that have one */
};

struct large_entry {
    uint32_t offset;    /* of the frame in F<format>_<file>.ithmb */
    uint16_t width;     /* of the picture in it */
    uint16_t height;
    uint8_t x, y;       /* where the picture starts in the frame */
    uint8_t file;       /* from 1; 0 for none */
    uint8_t unused;
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
_Static_assert(sizeof(struct slide_header) + 15 <= ITUNESDB_SLIDE_ROOM, "no room");
_Static_assert(SLIDE_SLOT + 15 <= ITUNESDB_SLIDE_ROOM + SLIDE_PIXELS * sizeof(fb_data),
               "no room for the whole slot");

/* of the slide in that place, with an index_header in front of them */
struct slide_colour {
    uint64_t key;
    uint32_t colour;
    uint32_t made;      /* SLIDE_MADE if the slot held this key's slide,
                           verified or made, when written; 0 for an unsure
                           slide and in older firmware's files */
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
    /* its frame over FRAME_MAX, of large_hdr's format */
    struct large_entry large;
};

#define COVERS_FILE      ROCKBOX_DIR "/covers.dat"
#define COVERS_FRAMES    ROCKBOX_DIR "/covers_frames.dat"
#define COVERS_SLIDES    ROCKBOX_DIR "/covers_slides.dat"
#define COVERS_MAGIC     0x56434252 /* "RBCV" */
#define COVERS_VERSION   3
/* a frame's place: whole sectors, so that it's one read */
#define COVER_SLOT       ((off_t)ALIGN_UP(FRAME_MAX * sizeof(fb_data), 2048))
/* the numbers of their slides, past Finder's, which have 16 bits */
#define COVER_SLIDE_BASE (1 << 20)
/* places at most: a file's offsets stop at 2 GB, and a table read damaged
   can't point past them; and 16 bits in a song's picture's entry */
#define COVERS_SLOTS_MAX 26000
#if COVERS_SLOTS_MAX >= 0x8000
#error "a song's picture's slot has 15 bits (pictures_of_songs())"
#endif
/* the free space a new place leaves at least, in KB: the files grew till the
   disk was full, and then Play Counts and the database had none */
#define COVERS_FREE_MIN  tagcache_caches_floor_kb()

struct covers_header {
    uint32_t magic;     /* as an index_header */
    uint32_t version;
    uint32_t count;
    int32_t commitid;   /* COVERS_CUT_SHORT while a making isn't over */
    uint64_t making;    /* the album being made then, 0 for none */
    uint32_t making_stamp;
    uint32_t making_tries; /* the makings cut short at it, one after another */
};

struct cover_entry {
    uint64_t key;       /* the album's */
    uint32_t stamp;     /* of what it was made from */
    uint32_t slot;      /* of its frame and slide, from 1; 0 for none found */
    uint16_t width;     /* of the frame; COVER_GIVEN_UP with no slot */
    uint16_t height;
    uint32_t colour;
    uint32_t image;     /* the stamp of the folder's image it was made of, or
                           0: one saved again since is made again */
};
/* an album whose making was cut short twice, by a cover that crashed the
   decoder or hung it: not tried again until its songs change */
#define COVER_GIVEN_UP   0xffff
/* one whose cover is in a format that isn't shown, PNG or progressive JPEG:
   counted for Updating Library's message only as it becomes so */
#define COVER_UNSHOWN    0xfffe
/* the sum of the images outside Finder's folder the covers were last made
   with: changed, as by a cover saved again, they're looked at again */
#define COVERS_IMAGES    ROCKBOX_DIR "/covers_images.dat"

/* The songs the covers are made for, each by its own picture: every song
   Finder has no cover of. A song with no picture has an entry too, so that
   the next making doesn't read its tags again. The file, written whole:
   - its header;
   - the songs' entries (struct song_record), by their paths' hashes;
   - those of them with a cover or a reason it has none, as kept in memory
     for the songs that play (struct covers_song_ram), in the same order;
   - the pictures they have (struct song_picture), by their slots.
   Its slots are those of COVERS_FRAMES and COVERS_SLIDES, which the albums'
   covers take too, an album's being the same slot as its song's. */
#define COVERS_SONGS     ROCKBOX_DIR "/covers_songs.dat"
#define SONGS_MAGIC      0x53434252 /* "RBCS" */
#define SONGS_VERSION    2
/* The 240 px picture of each of the songs' pictures, as Finder's 1024 frame
   for the menus' pane: a header, written last, then the pixels, each in
   whole sectors. Numbered apart from the slots, which the albums' covers
   take too: by slot, each album's left a blank place, hundreds of MB for a
   library whose albums an older firmware made. */
#define COVERS_LARGE     ROCKBOX_DIR "/covers_large.dat"
#define COVER_LARGE_MAGIC 0x4c434252 /* "RBCL" */
#define COVER_LARGE_SIDE ITUNESDB_LARGE_SIDE
#define COVER_LARGE_SLOT ((off_t)ALIGN_UP(sizeof(struct large_slot) \
                          + COVER_LARGE_SIDE * COVER_LARGE_SIDE * sizeof(fb_data), \
                          2048))
/* the large pictures at most, as a file's offsets stop at 2 GB */
#define COVERS_LARGE_MAX ((uint32_t)(0x7fffffff / COVER_LARGE_SLOT))
/* the covers a computer prepared (covers_table.h), and its file while it's
   written */
#define COVERS_INBOX     ROCKBOX_DIR "/covers_inbox.dat"
#define COVERS_INBOX_TMP ROCKBOX_DIR "/covers_inbox.tmp"
/* in a song's entry: seen by the making going on, which no file has */
#define SONG_SEEN        0x80
/* its file changed since its cover was made, which a making cut short
   hasn't made again: none shown, as the song's tags may not be read yet
   to tell (song_cover()) */
#define SONG_STALE       0x40

struct songs_header {
    uint32_t magic;
    uint32_t version;
    uint32_t count;     /* songs */
    uint32_t shown;     /* of them with a cover or a reason */
    uint32_t pictures;
    /* covers.dat's entries' sum when this was written with them: another's,
       as an older firmware's making, may have given their slots to other
       covers, and then neither this nor COVERS_LARGE is taken */
    uint32_t covers;
    uint32_t sum;       /* of the songs' entries */
    uint32_t shown_sum; /* of the rest */
    uint32_t whole;     /* 0 for a making cut short */
    /* the song whose picture was being decoded, and how many makings were
       cut short at it one after another: given up at two */
    uint32_t making_mtime;
    uint64_t making;
    uint32_t making_tries;
    uint32_t unused;
};

struct song_record {
    uint64_t path;      /* itunesdb_path_hash() */
    uint32_t size;      /* of the file */
    uint32_t mtime;     /* of the file, as the database has it; 0 for none
                           known, read again by the next making */
    uint32_t pic_pos;   /* where the picture is in the file, 0 for the
                           folder's image */
    uint32_t pic_size;  /* its bytes */
    uint32_t source;    /* the stamp of the folder's image, 0 for the song's
                           own picture or none */
    uint32_t crc, adler; /* of its bytes, as covers_crc32() and _adler32() */
    uint32_t word;      /* COVERS_WORD() */
    uint32_t check;     /* covers_file_check() */
    uint32_t unused;
};

struct song_picture {
    uint16_t slot;
    uint16_t large;     /* its place in COVERS_LARGE, from 1; 0 for none */
    uint16_t width, height; /* of its frame */
    uint32_t colour;
    uint32_t crc;       /* which its large picture's header has */
};

struct large_slot {
    uint32_t magic;
    uint16_t width, height;
    uint32_t crc, adler;
};


static struct art_header art_hdr;
/* of the import going on, then of the file loaded */
static struct large_header large_hdr;

/* the name of an ithmb file in an ArtworkDB mhod, which unlike iTunesDB
   says how it is encoded */
static void read_artwork_name(off_t pos, off_t end, char *buf, size_t size)
{
    const unsigned char *p = db_record(pos, "mhod", 36);
    buf[0] = '\0';
    /* the record within its mhni, which ends at end, and the name within it,
       as an iTunesDB string (mhod_string_size()) */
    if (!p || record_end(pos, p, end) < 0)
        return;
    const uint32_t total = le32(p + 8);
    size_t len = total > 36 ? MIN(le32(p + 24), total - 36) : 0;
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
static struct { uint32_t format, file; off_t size; } ithmb_sizes[16];
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

/* the largest frame in the mhii at pos, which ends at end, that its file
   has */
static bool read_image(off_t pos, off_t end, struct art_image *image)
{
    struct { uint32_t format, offset, size, width, height, file;
             uint32_t vpad, hpad; } frames[8];
    int nframes = 0;
    char name[48];
    const unsigned char *p = db_record(pos, "mhii", 52);
    if (!p)
        return false;
    uint32_t header_len = le32(p + 4);
    int children = le32(p + 12);
    image->id = le32(p + 16);
    image->source = le32(p + 48);

    off_t child = next_record(pos, header_len, end);
    for (int i = 0; i < children && nframes < (int)ARRAYLEN(frames); i++)
    {
        p = db_record(child, "mhod", 14);
        const off_t next = p ? record_end(child, p, end) : -1;
        if (next < 0)
            break;
        /* a frame: its mhni after the mhod's header, within it */
        const off_t mhni = (p[12] | p[13] << 8) == 2
                           ? next_record(child, le32(p + 4), next) : -1;
        if (mhni >= 0 && (p = db_record(mhni, "mhni", 36)))
        {
            const off_t mhni_end = record_end(mhni, p, next);
            const off_t name_at = next_record(mhni, le32(p + 4), mhni_end);
            frames[nframes].format = le32(p + 16);
            frames[nframes].offset = le32(p + 20);
            frames[nframes].size = le32(p + 24);
            frames[nframes].vpad = p[28] | p[29] << 8;
            frames[nframes].hpad = p[30] | p[31] << 8;
            frames[nframes].height = p[32] | p[33] << 8;
            frames[nframes].width = p[34] | p[35] << 8;
            /* ":F1029_1.ithmb" or "F1029_1.ithmb" */
            read_artwork_name(name_at, mhni_end, name, sizeof(name));
            const char *n = strrchr(name, '_');
            frames[nframes].file = n ? atoi(n + 1) : 0;
            if (frames[nframes].file > 0)
                nframes++;
        }
        child = next;
    }

    /* the largest RGB565 frame of the image, which is 200x200 where Finder
       made one and else 100x100: it makes the large ones of some covers
       later in a sync, so the first image doesn't tell for all */
    int best = -1, small = -1;
    memset(&image->large, 0, sizeof(image->large));
    for (int i = 0; i < nframes; i++)
    {
        /* A frame larger than the covers': never one of those below. Its
           picture is where the record says, as Finder's 320x240 frames of
           a square cover have it (paddings 0 and 40, height 240, width
           field 280): the paddings before the picture, each field to its
           far edge, and the frame the same padding again after it. Only
           the padding across is seen in those; a frame padded down is read
           the same way by guess, and one that isn't of that size by this
           reading isn't taken: its track's cover is decoded. */
        const uint32_t pitch = frames[i].width + frames[i].hpad;
        const uint32_t rows = frames[i].height + frames[i].vpad;
        if (frames[i].size > FRAME_MAX * 2)
        {
            const uint32_t width = frames[i].width - frames[i].hpad;
            const uint32_t height = frames[i].height - frames[i].vpad;
            if (!image->large.file && frames[i].file <= 255
                && frames[i].hpad < frames[i].width && frames[i].hpad <= 255
                && frames[i].vpad < frames[i].height && frames[i].vpad <= 255
                && width >= 2 && height >= 2
                && width * height <= LARGE_PIXELS
                && pitch <= 2 * LCD_WIDTH
                && frames[i].size == pitch * rows * 2
                && (!large_hdr.format
                    || (large_hdr.format == frames[i].format
                        && large_hdr.pitch == pitch))
                && (off_t)frames[i].offset + (off_t)frames[i].size
                   <= ithmb_size(frames[i].format, frames[i].file))
            {
                large_hdr.format = frames[i].format;
                large_hdr.pitch = pitch;
                image->large.offset = frames[i].offset;
                image->large.width = width;
                image->large.height = height;
                image->large.x = frames[i].hpad;
                image->large.y = frames[i].vpad;
                image->large.file = frames[i].file;
            }
            continue;
        }
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
    /* all of it set: the index is compared whole with the last one */
    image->small_offset = 0;
    image->small_file = 0;
    image->small_format = 0;
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

static int cmp_art_song(const void *a, const void *b)
{
    uint64_t x = art_songs.dbid[*(const uint32_t *)a];
    uint64_t y = art_songs.dbid[*(const uint32_t *)b];
    return (x > y) - (x < y);
}

/* the tracks whose images name them by dbid, sorted by it; how many */
static int art_songs_begin(void)
{
    art_songs.count = 0;
    if (art_songs.handle <= 0)
        return 0;
    for (int i = 0; i < db.count; i++)
    {
        if (db.tracks[i].art == ART_BY_SONG)
            art_songs.order[art_songs.count++] = i;
    }
    qsort(art_songs.order, art_songs.count, sizeof(uint32_t), cmp_art_song);
    return art_songs.count;
}

/* an image of the track with that dbid */
static void art_song_found(uint64_t song, uint32_t image)
{
    int lo = 0, hi = art_songs.count;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (art_songs.dbid[art_songs.order[mid]] < song)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (; lo < art_songs.count && art_songs.dbid[art_songs.order[lo]] == song; lo++)
        db.tracks[art_songs.order[lo]].art = image;
}

/* those whose image wasn't found have none */
static void art_songs_end(void)
{
    for (int i = 0; i < art_songs.count; i++)
    {
        struct track *t = &db.tracks[art_songs.order[i]];
        if (t->art == ART_BY_SONG)
            t->art = 0;
    }
    forget_art_songs();
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
    const off_t end = record_end(0, p, MH_FILE_END);
    off_t pos = next_record(0, le32(p + 4), end);
    int sections = le32(p + 20);
    for (int i = 0; i < sections; i++)
    {
        p = db_record(pos, "mhsd", 14);
        const off_t next = p ? record_end(pos, p, end) : -1;
        if (next < 0)
            break;
        const off_t list = next_record(pos, le32(p + 4), next);
        if ((p[12] | p[13] << 8) == 1 && (p = db_record(list, "mhli", 12)))
        {
            off_t item = next_record(list, le32(p + 4), next);
            int n = le32(p + 8);
            if ((uint32_t)n > COUNT_MAX)
                return -1;
            handle = db_alloc(n * sizeof(struct art_image) + 1);
            if (handle <= 0)
                return -1;
            core_pin(handle);
            *images = core_get_data(handle);
            /* the tracks with only their dbid, by it */
            int songs = art_songs_begin();
            for (int k = 0; k < n; k++)
            {
                p = db_record(item, "mhii", 28);
                const off_t item_end = p ? record_end(item, p, next)
                                         : -1;
                if (item_end < 0)
                    break;
                uint64_t song = le32(p + 20) | (uint64_t)le32(p + 24) << 32;
                if (read_image(item, item_end, &(*images)[*count]))
                {
                    if (songs > 0 && song)
                        art_song_found(song, (*images)[*count].id);
                    (*count)++;
                }
                item = item_end;
                if ((k & 255) == 0)
                {
                    reading(1, k, n);
                    yield();
                }
            }
            art_songs_end();
            qsort(*images, *count, sizeof(struct art_image), cmp_image);
            return handle;
        }
        pos = next;
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

/* The import left the covers' index as it was and made no slide: nothing
   made from them is made again. The old index by its header and a hash of the
   rest, before it's removed; and the commit then, which mustn't have changed. */
static bool art_kept;
static int kept_commitid;
struct art_sum {
    bool had;
    struct art_header hdr;
    uint32_t sum;
};
static uint32_t index_sum;

typedef uint32_t __attribute__((__may_alias__)) index_word;

/* FNV-1a a word at a time: crc_32() took a table lookup a nibble. Equal
   bytes hash equal when every piece starts 4-aligned and is whole words, as
   both sides' do; any other split only looks like a change. */
static uint32_t index_hash(const void *data, size_t size, uint32_t h)
{
    const unsigned char *p = data;
    size_t i = 0;
    if (!((uintptr_t)p & 3))
    {
        for (; i + 4 <= size; i += 4)
            h = (h ^ *(const index_word *)(p + i)) * 16777619u;
    }
    for (; i < size; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static void art_file_sum(struct art_sum *a)
{
    a->had = false;
    const int fd = open(ART_FILE, O_RDONLY);
    const int handle = fd >= 0 ? db_alloc(READ_BLOCK) : 0;
    if (handle > 0 && read(fd, &a->hdr, sizeof(a->hdr)) == sizeof(a->hdr))
    {
        core_pin(handle);
        a->sum = 2166136261u;
        ssize_t got;
        while ((got = read(fd, core_get_data(handle), READ_BLOCK)) > 0)
            a->sum = index_hash(core_get_data(handle), got, a->sum);
        a->had = got == 0;
        core_unpin(handle);
    }
    if (handle > 0)
        core_free(handle);
    if (fd >= 0)
        close(fd);
}

/* the rest of the index, hashed as it's written */
static void index_put(const void *data, size_t size)
{
    fw_put(&out, data, size);
    index_sum = index_hash(data, size, index_sum);
}

bool itunesdb_import_kept_art(void)
{
    return art_kept && tagcache_get_commitid() == kept_commitid;
}

/* true once the new index is in place */
static bool write_art_index(int old_handle, uint32_t old_count,
                            const struct art_sum *old)
{
    bool placed = false;
    struct art_image *images = NULL;
    int art_count = 0;
    int work = 0, slides = 0;
    memset(&art_hdr, 0, sizeof(art_hdr));
    memset(&large_hdr, 0, sizeof(large_hdr));
    ithmb_count = 0;
    int handle = read_images(&images, &art_count);
    if (handle <= 0 || art_hdr.nformats == 0)
        goto done;

    /* for each track its slide, and the tracks with covers by album */
    size_t slide_of_size = ALIGN_UP(db.count * sizeof(uint16_t), 4);
    const size_t order_size = (db.count + old_count) * sizeof(uint32_t)
                              + ALIGN_UP(old_count, 4);
    work = db_alloc(slide_of_size + order_size
                    + db.count * sizeof(struct large_entry));
    if (work <= 0)
        goto done;
    core_pin(work);
    uint16_t *slide_of = core_get_data(work);
    uint32_t *order = (uint32_t *)((char *)slide_of + slide_of_size);
    /* the larger frames of the index's entries, in its order */
    struct large_entry *large = (struct large_entry *)((char *)order
                                                       + order_size);
    int n = 0;
    for (int i = 0; i < db.count; i++)
    {
        const struct track *t = &db.tracks[i];
        slide_of[i] = 0;
        if (t->path && t->art && t->album
            && find_image(images, art_count, t->art))
            order[n++] = i;
    }
    qsort_yielding(order, n, sizeof(*order), cmp_album_track);

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

    if (!out_open(ART_FILE, FW_CACHE))
        goto done;
    art_hdr.magic = ART_MAGIC;
    art_hdr.version = ART_VERSION;
    fw_put(&out, &art_hdr, sizeof(art_hdr));
    index_sum = 2166136261u;
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
        index_put(&entry, sizeof(entry));
        large[count] = image->large;
        large_hdr.frames += image->large.file != 0;
        count++;
    }
    index_put(keys, art_hdr.nslides * sizeof(*keys));
    index_put(sources, art_hdr.nslides * sizeof(*sources));
    /* the count, now that it is known */
    art_hdr.count = count;
    fw_seek(&out, 0);
    fw_put(&out, &art_hdr, sizeof(art_hdr));
    if (!fw_end(&out))
        fw_place(ART_FILE, FW_CACHE, false);
    else
    {
        const bool same = old->had && old->sum == index_sum
                          && !memcmp(&old->hdr, &art_hdr, sizeof(art_hdr));
#ifdef HAVE_TAGCACHE
        /* the lists' covers made again, marked before the index is in place */
        if (!same)
            list_covers_forget();
#endif
        /* the slides to make before the index is complete */
        close(open(SLIDES_TODO, O_WRONLY | O_CREAT | O_TRUNC, 0666));
        /* the larger frames' file is never an older index's */
        remove(LARGE_FILE);
        placed = fw_place(ART_FILE, FW_CACHE, true);
        art_kept = same && placed;
        large_hdr.magic = LARGE_MAGIC;
        large_hdr.version = LARGE_VERSION;
        large_hdr.count = count;
        large_hdr.index = index_sum;
        bool written = false;
        if (placed && out_open(LARGE_FILE, FW_CACHE))
        {
            fw_put(&out, &large_hdr, sizeof(large_hdr));
            fw_put(&out, large, count * sizeof(*large));
            written = fw_close(&out, true);
        }
        /* not written: its header alone, of no index, says it was tried,
           and the next boot doesn't import for it again */
        if (placed && !written)
        {
            large_hdr.frames = 0;
            whole_file_write(LARGE_FILE, &large_hdr, sizeof(large_hdr));
        }
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
    return placed;
}

/* The artwork is back in place without its index: an index of no version,
   which the next boot's import makes anew, should the one now be cut */
void itunesdb_art_due(void)
{
    const uint32_t stub[2] = { ART_MAGIC, 0 };
    /* none if it can't be written: the index isn't to stay */
    if (!whole_file_write(ART_FILE, stub, sizeof(stub)))
        remove(ART_FILE);
}

/* The index lives in memory, and each frame is read once for all the sizes
   a theme shows it at, into a buffer of its own */

static struct index art_index = { 0, 0, sizeof(struct art_entry) };
static struct index slides_index = { 0, 0, sizeof(uint64_t) }; /* the keys */
/* of the slides, as uint32_t, then whether each is made as uint8_t; 0 for none */
static int colours_handle;
/* a slide recorded as made didn't load: all are checked at the next making */
static bool slides_suspect;
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
/* LARGE_FILE is of the index loaded, large_hdr its header */
static bool large_ok;
/* the sum of ART_FILE past its header, read once a boot or an import, with
   its header's count; no index: 0 */
static uint32_t art_sum_known, art_sum_count;
static bool art_sum_read;

#ifdef ITUNESDB_FINDER_LARGE
static struct index finder_index;
static void finder_load(void);
static bool finder_whole(void);
static void finder_said_whole(void);
static void finder_work_load(void);
static void finder_work_end(int how);
/* COVERS_LARGE removed as this making began: the places Finder's songs had
   in it are gone */
static bool finder_large_gone;
#endif

static void forget_art(void)
{
    forget_index(&art_index);
    forget_index(&slides_index);
#ifdef ITUNESDB_FINDER_LARGE
    /* of Finder's artwork as it was: loaded again with the covers */
    forget_index(&finder_index);
#endif
    large_ok = false;
    art_sum_read = false;
    if (colours_handle > 0)
        core_free(colours_handle);
    colours_handle = 0;
    frame_path[0] = '\0';
#ifdef HAVE_TAGCACHE
    album_header_forget_cover();
#endif
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

/* the colours file a stretch at a time: a read for each took 0.28 s at boot */
static struct slide_colour colours_block[256];

/* the colours known of the slides of the index loaded, and whether each is
   made: those of a slide no longer in its place aren't */
static void load_colours(void)
{
    if (slides_index.handle <= 0 || colours_handle > 0)
        return;
    int handle = db_alloc(slides_index.count * (sizeof(uint32_t) + 1));
    if (handle <= 0)
        return;
    core_pin(handle);
    uint32_t *colours = core_get_data(handle);
    uint8_t *made = (uint8_t *)(colours + slides_index.count);
    for (uint32_t i = 0; i < slides_index.count; i++)
    {
        colours[i] = COVER_COLOUR_UNKNOWN;
        made[i] = 0;
    }
    struct index_header h;
    int fd = open(COLOURS_FILE, O_RDONLY);
    if (fd >= 0)
    {
        if (read(fd, &h, sizeof(h)) == sizeof(h) && h.magic == COLOURS_MAGIC
            && h.version == COLOURS_VERSION)
        {
            struct slide_colour *c = colours_block;
            const uint32_t count = MIN(h.count, slides_index.count);
            for (uint32_t i = 0; i < count; )
            {
                const ssize_t got = read(fd, c, MIN(count - i, ARRAYLEN(colours_block))
                                                * sizeof(*c));
                if (got < (ssize_t)sizeof(*c))
                    break;
                for (size_t k = 0; k < got / sizeof(*c); k++, i++)
                {
                    if (c[k].key && c[k].key == slide_key(i))
                    {
                        colours[i] = c[k].colour;
                        made[i] = c[k].made == SLIDE_MADE;
                    }
                }
            }
        }
        close(fd);
    }
    core_unpin(handle);
    colours_handle = handle;
}

static uint8_t *slides_made(void)
{
    return (uint8_t *)((uint32_t *)core_get_data(colours_handle)
                       + slides_index.count);
}

static void colour_entry(uint32_t i, struct slide_colour *c)
{
    c->key = slide_key(i);
    c->colour = ((const uint32_t *)core_get_data(colours_handle))[i];
    c->made = slides_made()[i] ? SLIDE_MADE : 0;
}

/* whether the file already holds these records: not rewritten then */
static bool colours_written(void)
{
    struct index_header h;
    const int fd = open(COLOURS_FILE, O_RDONLY);
    bool same = fd >= 0 && read(fd, &h, sizeof(h)) == sizeof(h)
                && h.magic == COLOURS_MAGIC && h.version == COLOURS_VERSION
                && h.count == slides_index.count
                && ffilesize(fd) == (off_t)(sizeof(h) + h.count * sizeof(struct slide_colour));
    struct slide_colour *c = colours_block;
    for (uint32_t i = 0; same && i < slides_index.count; )
    {
        const ssize_t want = MIN(slides_index.count - i, ARRAYLEN(colours_block))
                             * sizeof(*c);
        same = read(fd, c, want) == want;
        for (size_t k = 0; same && k < want / sizeof(*c); k++, i++)
        {
            struct slide_colour now;
            colour_entry(i, &now);
            same = !memcmp(&now, &c[k], sizeof(now));
        }
    }
    if (fd >= 0)
        close(fd);
    return same;
}

static void write_colours(void)
{
    if (colours_handle <= 0 || colours_written()
        || !out_open(COLOURS_FILE, FW_CACHE))
        return;
    struct index_header h = { COLOURS_MAGIC, COLOURS_VERSION, slides_index.count };
    fw_put(&out, &h, sizeof(h));
    for (uint32_t i = 0; i < slides_index.count; i++)
    {
        struct slide_colour c;
        colour_entry(i, &c);
        fw_put(&out, &c, sizeof(c));
    }
    fw_close(&out, true);
}

/* the colour of slide i (from 0) of the index */
static uint32_t slide_colour(uint32_t i)
{
    if (colours_handle <= 0 || i >= slides_index.count)
        return COVER_COLOUR_UNKNOWN;
    return ((const uint32_t *)core_get_data(colours_handle))[i];
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
    art->colour = COVER_COLOUR_UNKNOWN;
    art->reason = ITUNESDB_COVER_SHOWN;
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
    if (entry.slide > 0)
        art->colour = slide_colour(entry.slide - 1);
    return true;
}

static void lock_art(void);

static uint32_t art_sum(void)
{
    if (!art_sum_read)
    {
        struct art_sum a;
        art_file_sum(&a);
        art_sum_read = true;
        art_sum_known = a.had && a.hdr.magic == ART_MAGIC
                        && a.hdr.version == ART_VERSION ? a.sum : 0;
        art_sum_count = a.had ? a.hdr.count : 0;
    }
    return art_sum_known;
}

/* The header of LARGE_FILE if an import made it for the index on the disk:
   false without, as after a firmware that doesn't know the file imported */
static bool large_made(struct large_header *h)
{
    const int fd = open(LARGE_FILE, O_RDONLY);
    const bool made = fd >= 0 && read(fd, h, sizeof(*h)) == sizeof(*h)
                      && h->magic == LARGE_MAGIC
                      && h->version == LARGE_VERSION
                      && h->index == art_sum() && h->count == art_sum_count
                      /* whole, or the header alone of one not written */
                      && (ffilesize(fd) == (off_t)(sizeof(*h) + h->count
                                           * sizeof(struct large_entry))
                          || (!h->frames
                              && ffilesize(fd) == (off_t)sizeof(*h)));
    if (fd >= 0)
        close(fd);
    return made;
}

/* An index of this version whose ArtworkDB is gone, as set aside and the
   import after it cut: itunesdb_load_tracks() has the boot import for it */
bool itunesdb_art_orphaned(void)
{
    return art_sum() && !file_exists(ARTWORKDB_FILE);
}

bool itunesdb_large_art_ready(void)
{
    struct large_header h;
    /* no index of this version: nothing to go with, or its own import */
    return !art_sum() || large_made(&h);
}

/* its header; used if it has frames. False if it isn't made. */
static bool load_large(void)
{
    struct large_header h;
    const bool made = large_made(&h);
    large_ok = made && h.frames && h.count == art_index.count
               && h.pitch >= 2 && h.pitch <= 2 * LCD_WIDTH;
    if (large_ok)
        large_hdr = h;
    return made;
}

uint32_t itunesdb_art_count(void)
{
    return art_index.handle > 0 ? index_hdr.count : 0;
}

bool itunesdb_has_large_art(void)
{
    return large_ok && large_hdr.frames > 0;
}

bool itunesdb_find_large_art(const char *path, struct itunesdb_large_art *art)
{
    if (!large_ok || !large_hdr.frames || art_index.handle <= 0)
        return false;
    /* the entry's place in the index, as find_in_index() looks */
    const uint64_t hash = itunesdb_path_hash(path);
    core_pin(art_index.handle);
    const struct art_entry *entries = core_get_data(art_index.handle);
    int lo = 0, hi = (int)art_index.count - 1, at = -1;
    while (lo <= hi && at < 0)
    {
        const int mid = (lo + hi) / 2;
        if (entries[mid].hash == hash)
            at = mid;
        else if (entries[mid].hash < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    core_unpin(art_index.handle);
    if (at < 0)
        return false;

    struct large_entry e;
    const off_t pos = sizeof(struct large_header) + (off_t)at * sizeof(e);
    const int fd = open(LARGE_FILE, O_RDONLY);
    const bool read_it = fd >= 0 && lseek(fd, pos, SEEK_SET) == pos
                         && read(fd, &e, sizeof(e)) == sizeof(e);
    if (fd >= 0)
        close(fd);
    /* as read_image() takes them, for a file that was damaged */
    if (!read_it || !e.file || e.width < 2 || e.height < 2
        || (uint32_t)e.width * e.height > LARGE_PIXELS
        || e.x + e.width > large_hdr.pitch
        || (size_t)snprintf(art->path, sizeof(art->path),
                            ARTWORK_DIR "/F%lu_%lu.ithmb",
                            (unsigned long)large_hdr.format,
                            (unsigned long)e.file) >= sizeof(art->path))
        return false;
    art->offset = e.offset;
    art->pitch = large_hdr.pitch;
    art->x = e.x;
    art->y = e.y;
    art->width = e.width;
    art->height = e.height;
    return true;
}

int itunesdb_load_large_art(const struct itunesdb_large_art *art,
                            void *pixels)
{
    const size_t row = art->pitch * sizeof(fb_data);
    const int rows_most = (sizeof(frame_raw) - 2 * READ_ALIGN) / row;
    if (rows_most < 1)
        return -1;
    lock_art();
    /* through the covers' own room, a part of the frame at a time */
    frame_path[0] = '\0';
    const int fd = open(art->path, O_RDONLY);
    bool ok = fd >= 0;
    for (int y = 0; ok && y < art->height; y += rows_most)
    {
        const int rows = MIN(rows_most, art->height - y);
        const off_t from = (off_t)art->offset + (off_t)(art->y + y) * row;
        const off_t start = from & ~(off_t)(READ_ALIGN - 1);
        const size_t skip = from - start, size = rows * row;
        const size_t want = (skip + size + READ_ALIGN - 1)
                            & ~(size_t)(READ_ALIGN - 1);
        /* the last frame of a file may end before the sector does */
        ok = lseek(fd, start, SEEK_SET) == start
             && read(fd, frame_raw, want) >= (ssize_t)(skip + size);
        for (int k = 0; ok && k < rows; k++)
            memcpy((fb_data *)pixels + (y + k) * art->width,
                   frame_raw + skip + k * row + art->x * sizeof(fb_data),
                   art->width * sizeof(fb_data));
    }
    if (fd >= 0)
        close(fd);
    mutex_unlock(&art_lock);
    return ok ? (int)(art->width * art->height * sizeof(fb_data)) : -1;
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

/* Shrunk to half or less each way, for an album's header, 60 from 200: each
   pixel the mean of those of the frame it covers. Bilinear's four left out
   most of those ten or so, which looked grainy. Only where it's asked for:
   the other screens' covers as they were */
#define BOX_MAX 8 /* the most pixels of the frame a pixel covers each way */

static bool shrinks(int fw, int fh, int w, int h)
{
    return fw >= 2 * w && fh >= 2 * h && fw <= BOX_MAX * w && fh <= BOX_MAX * h;
}

static void shrink_frame(const fb_data *frame, int fw, int fh,
                         fb_data *out, int w, int h)
{
    static uint16_t xs[ROW_MAX + 1];
    /* 65536 / n, rounded: this CPU has no divide, so nothing in the loops
       may divide */
    static uint32_t recip[BOX_MAX * BOX_MAX + 1];
    if (!recip[1])
    {
        for (int n = 1; n <= BOX_MAX * BOX_MAX; n++)
            recip[n] = (65536 + n / 2) / n;
    }
    for (int x = 0; x <= w; x++)
        xs[x] = x * fw / w;
    for (int y = 0; y < h; y++)
    {
        const int y0 = y * fh / h, y1 = (y + 1) * fh / h;
        for (int x = 0; x < w; x++)
        {
            uint32_t r = 0, g = 0, b = 0;
            for (int sy = y0; sy < y1; sy++)
            {
                const fb_data *p = frame + sy * fw;
                for (int sx = xs[x]; sx < xs[x + 1]; sx++)
                {
                    r += p[sx] >> 11;
                    g += (p[sx] >> 5) & 0x3f;
                    b += p[sx] & 0x1f;
                }
            }
            const uint32_t k = recip[(xs[x + 1] - xs[x]) * (y1 - y0)];
            *out++ = (fb_data)(((r * k + 0x8000) >> 16) << 11
                               | ((g * k + 0x8000) >> 16) << 5
                               | ((b * k + 0x8000) >> 16));
        }
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

/* The frame's buffer lent to the import's notes while it reads the tracks, when no
   cover is read: forget_art() has dropped their index, and the lock keeps one of a
   song outside Finder's folder waiting. In whole sectors. */
static unsigned char *notes_room(size_t *size)
{
    lock_art();
    frame_path[0] = '\0';
    *size = sizeof(frame_raw) - sizeof(frame_raw) % 512;
    return frame_raw;
}

static void notes_room_done(void)
{
    mutex_unlock(&art_lock);
}

/* the size of a frame in bm: the largest that fits, up or down, keeping the
   shape; false when it's more than max_size holds */
static bool art_fit(int fw, int fh, int *w, int *h, int max_size)
{
    if (fw <= 0 || fh <= 0 || *w <= 0 || *h <= 0)
        return false;
    if (*w * fh > *h * fw)
        *w = (*h * fw + fh / 2) / fh;
    else
        *h = (*w * fh + fw / 2) / fw;
    return *w <= ROW_MAX
           && *w * *h * (int)sizeof(fb_data) + (int)sizeof(struct bitmap) <= max_size;
}

/* the frame into bm at w by h, shrunk by averaging if asked */
static int art_put(const fb_data *frame, int fw, int fh, struct bitmap *bm,
                   int w, int h, bool average)
{
    const int out_size = w * h * sizeof(fb_data);
    if (w == fw && h == fh)
        memcpy(bm->data, frame, out_size);
    else
    {
        cpu_boost(true);
        if (average && shrinks(fw, fh, w, h))
            shrink_frame(frame, fw, fh, (fb_data *)bm->data, w, h);
        else
            scale_frame(frame, fw, fh, (fb_data *)bm->data, w, h, false);
        cpu_boost(false);
    }
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

static int load_art(const struct itunesdb_art *art, struct bitmap *bm,
                    int max_size, bool average)
{
    int w = bm->width, h = bm->height;
    if (!art_fit(art->width, art->height, &w, &h, max_size))
        return -1;

    lock_art();
    const fb_data *frame = get_frame(art);
    const int size = frame ? art_put(frame, art->width, art->height, bm, w, h,
                                     average) : -1;
    mutex_unlock(&art_lock);
    return size;
}

static int scale_art(const struct bitmap *src, struct bitmap *bm, int max_size,
                     bool average)
{
    int w = bm->width, h = bm->height;
    if (!art_fit(src->width, src->height, &w, &h, max_size))
        return -1;

    /* not waiting for art_lock, which a read from the disk can hold for long, and
       src could move in the buffer meanwhile: scaling doesn't yield, so it's never
       in scale_frame's or shrink_frame's rows along with another thread */
    return art_put((const fb_data *)src->data, src->width, src->height, bm, w, h,
                   average);
}

int itunesdb_load_art(const struct itunesdb_art *art, struct bitmap *bm,
                      int max_size)
{
    return load_art(art, bm, max_size, false);
}

int itunesdb_scale_art(const struct bitmap *src, struct bitmap *bm, int max_size)
{
    return scale_art(src, bm, max_size, false);
}

int itunesdb_load_small_art(const struct itunesdb_art *art, struct bitmap *bm,
                            int max_size)
{
    return load_art(art, bm, max_size, true);
}

int itunesdb_scale_small_art(const struct bitmap *src, struct bitmap *bm,
                             int max_size)
{
    return scale_art(src, bm, max_size, true);
}

/* the slides file, open while Cover Flow reads it: one open a cover cost a
   look through the folder and the file's setting up, which hold the frames */
static int slides_fd = -1;
/* and of the covers of songs outside Finder's folder */
static struct index covers_index = { 0, 0, sizeof(struct cover_entry) };
static struct covers_header covers_hdr;
static bool covers_have_frames; /* without, a table of albums with none */
static int covers_slides_fd = -1;
static int load_cover_slide(const struct itunesdb_art *art, void *buf,
                            int *width, int *height);

void itunesdb_slides_done(void)
{
    if (slides_fd >= 0)
        close(slides_fd);
    slides_fd = -1;
    if (covers_slides_fd >= 0)
        close(covers_slides_fd);
    covers_slides_fd = -1;
}

int itunesdb_load_slide(const struct itunesdb_art *art, void *buf,
                        int *width, int *height)
{
    if (art->slide > COVER_SLIDE_BASE)
        return load_cover_slide(art, buf, width, height);
    struct slide_header head;
    uint64_t key = art->slide > 0 ? slide_key(art->slide - 1) : 0;
    if (!key)
        return -1;
    if (slides_fd < 0)
        slides_fd = open(SLIDES_FILE, O_RDONLY);
    const int fd = slides_fd;
    if (fd < 0)
        return -1;
    off_t pos = (off_t)(art->slide - 1) * SLIDE_SLOT;
    /* All of it in one read, with its header, to where DMA can reach, as the slots lie
       on whole sectors: the whole slot, so that it's a single command to the disk.
       To the end of the slide's last sector was one more on the 5.5G, whose sectors
       are 1 KB to the disk, for the last 512 bytes through a sector's cache, and the
       reader waits a frame of the UI's for each. Then down to where it was asked
       for, which is in its way only as far as ITUNESDB_SLIDE_ROOM. */
    char *at = buf;
    size_t room = ITUNESDB_SLIDE_ROOM + SLIDE_PIXELS * sizeof(fb_data);
    STORAGE_ALIGN_BUFFER(at, room);
    const ssize_t whole = sizeof(head) + SLIDE_PIXELS * sizeof(fb_data);
    const ssize_t sectors = MIN((ssize_t)SLIDE_SLOT, (ssize_t)room);
    bool ok = lseek(fd, pos, SEEK_SET) == pos && read(fd, at, sectors) >= whole;
    if (ok)
        memcpy(&head, at, sizeof(head));
    if (!ok || (head.magic != SLIDE_MAGIC && head.magic != SLIDE_UNSURE)
        || head.key != key
        || head.width > ITUNESDB_SLIDE_SIZE
        || head.height > ITUNESDB_SLIDE_SIZE)
    {
        if (!ok)
            itunesdb_slides_done(); /* opened again the next time */
        /* not as the colours' record has it: every header is checked at the
           next making */
        if (colours_handle > 0 && (uint32_t)art->slide <= slides_index.count
            && slides_made()[art->slide - 1])
        {
            slides_made()[art->slide - 1] = 0;
            if (!slides_suspect)
                close(open(SLIDES_CHECK, O_WRONLY | O_CREAT | O_TRUNC, 0666));
            slides_suspect = true;
        }
        return -1;
    }
    const ssize_t size = head.width * head.height * sizeof(fb_data);
    memmove(buf, at + sizeof(head), size);
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
    struct index_header colours = { 0 };
    int fd = open(ART_FILE, O_RDONLY);
    if (fd >= 0)
    {
        read(fd, &h, sizeof(h));
        close(fd);
    }
    fd = open(COLOURS_FILE, O_RDONLY);
    if (fd >= 0)
    {
        read(fd, &colours, sizeof(colours));
        close(fd);
    }
    if (file_exists(SLIDES_TODO) || file_exists(SLIDES_CHECK))
        return true;
    if (h.magic != ART_MAGIC)
        return false;
    /* slides of an older kind, by the mark of the first made of them, as one
       of another size isn't taken and Cover Flow then makes each of a cover */
    fd = h.nslides > 0 ? open(SLIDES_FILE, O_RDONLY) : -1;
    if (fd >= 0)
    {
        bool older = false;
        for (int i = 0; i < 8 && !older; i++)
        {
            struct slide_header head;
            const off_t pos = (off_t)i * SLIDE_SLOT;
            if (lseek(fd, pos, SEEK_SET) != pos
                || read(fd, &head, sizeof(head)) != sizeof(head))
                break;
            older = head.magic != 0 && head.magic != SLIDE_MAGIC
                    && head.magic != SLIDE_UNSURE;
        }
        close(fd);
        if (older)
            return true;
    }
    /* of an older version, or slides made before they had colours */
    return h.version != ART_VERSION
           || (h.nslides > 0 && (colours.magic != COLOURS_MAGIC
                                 || colours.version != COLOURS_VERSION
                                 || colours.count != h.nslides));
}

/* of the slide in slide_slot, which is a column after another */
static void note_slide_colour(uint32_t i)
{
    const struct slide_header *head = (const struct slide_header *)slide_slot;
    if (colours_handle > 0)
        ((uint32_t *)core_get_data(colours_handle))[i] = cover_palette_colour(
            (const fb_data *)(slide_slot + sizeof(*head)), head->height, head->width);
}

void itunesdb_make_slides(volatile int *done, volatile int *total,
                          bool (*stop)(void))
{
    if (slides_index.handle <= 0)
    {
        remove(SLIDES_TODO);
        remove(SLIDES_CHECK);
        slides_suspect = false;
        return;
    }
    /* not read meanwhile from what was open before */
    itunesdb_slides_done();
    int index_fd = open(ART_FILE, O_RDONLY);
    errno = 0;
    int fd = open(SLIDES_FILE, O_RDWR | O_CREAT, 0666);
    if (fd < 0 || index_fd < 0)
    {
        if (fd < 0)
            library_write_failed();
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
    const bool check_all = slides_suspect || file_exists(SLIDES_CHECK);
    bool remaking = false;
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
        /* made already, by the colours' record: no header read */
        off_t pos = (off_t)i * SLIDE_SLOT;
        if (!check_all && pos + (off_t)SLIDE_SLOT <= size && colours_handle > 0
            && slides_made()[i] && slide_colour(i) != COVER_COLOUR_UNKNOWN)
            continue;
        if (colours_handle > 0)
            slides_made()[i] = 0;
        bool made = pos + (off_t)SLIDE_SLOT <= size
                    && lseek(fd, pos, SEEK_SET) == pos
                    && read(fd, &head, sizeof(head)) == sizeof(head)
                    && head.magic == SLIDE_MAGIC && head.key == key
                    /* at this size of slides, which a cover fills one way:
                       of another, kept by its slot's being as big, it showed
                       smaller */
                    && MAX(head.width, head.height) == ITUNESDB_SLIDE_SIZE;
        if (made && (colours_handle <= 0 || slide_colour(i) != COVER_COLOUR_UNKNOWN))
        {
            if (colours_handle > 0)
                slides_made()[i] = 1;
            continue;
        }
        if (!remaking)
        {
            /* the lists' covers are made of the slides and their colours */
            remaking = true;
            art_kept = false;
#ifdef HAVE_TAGCACHE
            list_covers_forget();
#endif
        }
        errno = 0;
        if (made ? lseek(fd, pos, SEEK_SET) == pos
                   && read(fd, slide_slot, SLIDE_SLOT) == SLIDE_SLOT
                 : make_slide(fd, index_fd, i, key, &size))
        {
            note_slide_colour(i);
            /* one of a small frame alone is made again at the next import */
            if (colours_handle > 0)
                slides_made()[i] = ((const struct slide_header *)slide_slot)->magic
                                   == SLIDE_MAGIC;
        }
        else
        {
            complete = false;
            /* the disk full: the rest at the next boot, and said so */
            if (errno == ENOSPC)
            {
                library_write_failed();
                break;
            }
        }
        yield();
    }
    cpu_boost(false);
    write_colours();
    /* the slides of albums gone at the end */
    off_t end = (off_t)count * (off_t)SLIDE_SLOT;
    if (complete && size > end)
        ftruncate(fd, end);
    close(index_fd);
    close(fd);
    if (complete)
    {
        remove(SLIDES_TODO);
        remove(SLIDES_CHECK);
        slides_suspect = false;
    }
}

/** The covers of the albums of songs copied outside Finder's folder **/

/* Made on Updating Library by Cover Flow's plugin, in the forms Finder's are
   in, so that they show as fast: a frame of up to 200 by 200 for Now Playing
   and the panel, a slide for Cover Flow and its colour. Decoded as a track
   played, a cover of 1000 by 1000 took seconds for each size the theme has.
   One an album, found by its key: a track finds its album's by its tags,
   with no table of every song's path. */
static uint64_t fnv_step(uint64_t hash, unsigned char c)
{
    return (hash ^ c) * 0x100000001b3ull;
}

/* a tag's text, but for none */
static bool tag_given(const char *s)
{
    return s && *s && strcmp(s, UNTAGGED);
}

uint64_t itunesdb_album_key(const char *album, const char *artist,
                            const char *path)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    if (tag_given(album))
    {
        /* Latin letters in either case as one, as the database's lists
           take them */
        for (const unsigned char *p = (const unsigned char *)album; *p; p++)
            hash = fnv_step(hash, tolower(*p));
        hash = fnv_step(hash, 0);
        if (tag_given(artist))
        {
            for (const unsigned char *p = (const unsigned char *)artist; *p; p++)
                hash = fnv_step(hash, tolower(*p));
        }
    }
    else if (path)
    {
        /* an album without a name is its folder */
        path = strip_volume(path);
        const char *end = strrchr(path, '/');
        hash = fnv_step(hash, '/');
        for (const unsigned char *p = (const unsigned char *)path;
             end && p < (const unsigned char *)end; p++)
            hash = fnv_step(hash, tolower(*p));
    }
    return hash ? hash : 1;
}

/* the sum of covers.dat's entries, which the songs' file goes with; 0 for
   none */
static uint32_t covers_sum;

/* The songs' covers in memory, for the songs that play: those with a cover
   or a reason, and their pictures; taken as covers.dat is, at boot and after
   a making */
static struct index songs_index = { 0, 0, sizeof(struct covers_song_ram) };
static struct index pictures_index = { 0, 0, sizeof(struct song_picture) };
/* COVERS_SONGS goes with covers.dat: its slots and COVERS_LARGE's are its */
static bool songs_trusted;

static uint32_t covers_entries_sum(const struct cover_entry *e, uint32_t n)
{
    return n ? index_hash(e, n * sizeof(*e), 2166136261u) : 0;
}

/* the header of the songs' file, if it's of this version; false without */
static bool songs_header_read(int fd, struct songs_header *h)
{
    return read(fd, h, sizeof(*h)) == sizeof(*h) && h->magic == SONGS_MAGIC
           && h->version == SONGS_VERSION && h->count <= COUNT_MAX
           && h->shown <= h->count && h->pictures <= h->shown
           && ffilesize(fd) == (off_t)(sizeof(*h)
                                       + h->count * sizeof(struct song_record)
                                       + h->shown * sizeof(struct covers_song_ram)
                                       + h->pictures * sizeof(struct song_picture));
}

/* A part of the file into memory, if its sum is as said: 0 without */
static int songs_part_load(int fd, off_t pos, size_t size, uint32_t *sum)
{
    int handle = size ? db_alloc(size) : 0;
    if (handle <= 0)
        return 0;
    core_pin(handle);
    void *data = core_get_data(handle);
    const bool ok = lseek(fd, pos, SEEK_SET) == pos
                    && read(fd, data, size) == (ssize_t)size;
    if (ok)
        *sum = index_hash(data, size, *sum);
    core_unpin(handle);
    if (!ok)
        core_free(handle);
    return ok ? handle : 0;
}

static void songs_load(void)
{
    forget_index(&songs_index);
    forget_index(&pictures_index);
    songs_trusted = false;
    struct songs_header h;
    const int fd = open(COVERS_SONGS, O_RDONLY);
    if (fd < 0)
        return;
    if (songs_header_read(fd, &h) && h.covers == covers_sum)
    {
        songs_trusted = true;
        const off_t at = sizeof(h) + (off_t)h.count * sizeof(struct song_record);
        const size_t shown = h.shown * sizeof(struct covers_song_ram);
        uint32_t sum = 2166136261u;
        const int songs = songs_part_load(fd, at, shown, &sum);
        const int pictures = songs_part_load(fd, at + shown,
                                             h.pictures * sizeof(struct song_picture),
                                             &sum);
        /* the pictures' part may have none, as of songs whose pictures
           are none of them shown */
        if (songs > 0 && (pictures > 0 || !h.pictures) && sum == h.shown_sum)
        {
            songs_index.handle = songs;
            songs_index.count = h.shown;
            pictures_index.handle = pictures;
            pictures_index.count = h.pictures;
        }
        else
        {
            if (songs > 0)
                core_free(songs);
            if (pictures > 0)
                core_free(pictures);
        }
    }
    close(fd);
}

static void covers_load(void)
{
    /* also with no frames, when no album had a cover: the next making knows
       them then, and doesn't look in every album's songs again */
    covers_have_frames = file_exists(COVERS_FRAMES);
    load_index(&covers_index, COVERS_FILE, &covers_hdr, sizeof(covers_hdr),
               COVERS_MAGIC, COVERS_VERSION);
    covers_sum = 0;
    if (covers_index.handle > 0)
    {
        const struct cover_entry *e = core_get_data_pinned(covers_index.handle);
        covers_sum = covers_entries_sum(e, covers_index.count);
        core_put_data_pinned((void *)e);
    }
    songs_load();
#ifdef ITUNESDB_FINDER_LARGE
    finder_load();
#endif
}

static bool cover_art(const struct cover_entry *e, struct itunesdb_art *art)
{
    art->reason = ITUNESDB_COVER_SHOWN;
    if (!e->slot && e->width == COVER_UNSHOWN)
        art->reason = e->height;
    /* none with its frames gone, the track's own then */
    if (!covers_have_frames || !e->slot || e->slot > COVERS_SLOTS_MAX
        || !e->width || !e->height || e->width * e->height > FRAME_MAX)
        return false;
    strmemccpy(art->path, COVERS_FRAMES, sizeof(art->path));
    art->offset = (e->slot - 1) * COVER_SLOT;
    art->width = e->width;
    art->height = e->height;
    art->slide = COVER_SLIDE_BASE + e->slot;
    art->colour = e->colour;
    return true;
}

bool itunesdb_find_album_art(const char *album, const char *artist,
                             const char *path, struct itunesdb_art *art)
{
    struct cover_entry e;
    art->reason = ITUNESDB_COVER_SHOWN;
    return covers_index.handle > 0
           && find_in_index(&covers_index,
                            itunesdb_album_key(album, artist, path), &e)
           && cover_art(&e, art);
}

/* the picture in a slot, of the songs' */
static bool song_picture_of(uint32_t slot, struct song_picture *p)
{
    if (pictures_index.handle <= 0)
        return false;
    core_pin(pictures_index.handle);
    const struct song_picture *e = core_get_data(pictures_index.handle);
    int lo = 0, hi = (int)pictures_index.count - 1;
    bool found = false;
    while (lo <= hi && !found)
    {
        const int mid = (lo + hi) / 2;
        if (e[mid].slot == slot)
        {
            *p = e[mid];
            found = true;
        }
        else if (e[mid].slot < slot)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    core_unpin(pictures_index.handle);
    return found;
}

/* The cover made of the song's own picture: 1 into art; 0 none known of the
   song; -1 none, as its file changed since, or with art->reason why its
   picture isn't shown */
static int song_cover(const struct mp3entry *id3, const char *path,
                      struct itunesdb_art *art)
{
    if (songs_index.handle <= 0)
        return 0;
    core_pin(songs_index.handle);
    const struct covers_song_ram *e =
        covers_song_find(core_get_data(songs_index.handle), songs_index.count,
                         itunesdb_path_hash(path));
    const struct covers_song_ram song = e ? *e : (struct covers_song_ram){ 0 };
    core_unpin(songs_index.handle);
    if (!e)
        return 0;
    if (COVERS_FLAGS_OF(song.word) & SONG_STALE)
        return -1;
    /* the file it was made of, by the tags read of it now: not of a track
       known only by its path yet, as the one a skip goes to */
    if (id3 && id3->filesize && !strcmp(id3->path, path)
        && covers_file_check(id3->filesize,
                             id3->has_embedded_albumart ? id3->albumart.pos : 0,
                             id3->has_embedded_albumart ? id3->albumart.size : 0)
           != song.check)
        return -1;
    art->reason = COVERS_REASON_OF(song.word);
    const uint32_t slot = COVERS_SLOT_OF(song.word);
    struct song_picture p;
    if (!slot || slot > COVERS_SLOTS_MAX || !covers_have_frames
        || !song_picture_of(slot, &p) || !p.width || !p.height
        || p.width * p.height > FRAME_MAX)
        return -1;
    strmemccpy(art->path, COVERS_FRAMES, sizeof(art->path));
    art->offset = (slot - 1) * COVER_SLOT;
    art->width = p.width;
    art->height = p.height;
    art->slide = COVER_SLIDE_BASE + slot;
    art->colour = p.colour;
    art->reason = ITUNESDB_COVER_SHOWN;
    return 1;
}

bool itunesdb_find_track_art(const struct mp3entry *id3, const char *path,
                             struct itunesdb_art *art)
{
    art->reason = ITUNESDB_COVER_SHOWN;
    /* Finder's first, the track's own */
    if (itunesdb_find_art(path, art))
        return true;
    /* then the one made of its own picture, which a track whose picture
       isn't shown or whose file has changed goes without */
    const int own = song_cover(id3, path, art);
    if (own)
        return own > 0;
    /* then its album's, for a track with no picture of its own; not for a
       song without an album, whose folder's cover is for Cover Flow */
    if (!id3 || !tag_given(id3->album))
        return false;
    /* the album artist as the database takes it: the track's artist's
       without one */
    const char *artist = tag_given(id3->albumartist) ? id3->albumartist
                                                     : id3->artist;
    return itunesdb_find_album_art(id3->album, artist, path, art);
}

/* the slide of one of ours, as itunesdb_load_slide() reads Finder's */
static int load_cover_slide(const struct itunesdb_art *art, void *buf,
                            int *width, int *height)
{
    if (covers_slides_fd < 0)
        covers_slides_fd = open(COVERS_SLIDES, O_RDONLY);
    const int fd = covers_slides_fd;
    if (fd < 0)
        return -1;
    off_t pos = (off_t)(art->slide - COVER_SLIDE_BASE - 1) * SLIDE_SLOT;
    char *at = buf;
    size_t room = ITUNESDB_SLIDE_ROOM + SLIDE_PIXELS * sizeof(fb_data);
    STORAGE_ALIGN_BUFFER(at, room);
    struct slide_header head;
    const ssize_t whole = sizeof(head) + SLIDE_PIXELS * sizeof(fb_data);
    const ssize_t sectors = MIN((ssize_t)SLIDE_SLOT, (ssize_t)room);
    if (lseek(fd, pos, SEEK_SET) != pos || read(fd, at, sectors) < whole)
    {
        itunesdb_slides_done(); /* opened again the next time */
        return -1;
    }
    memcpy(&head, at, sizeof(head));
    if (head.magic != SLIDE_MAGIC || head.width > ITUNESDB_SLIDE_SIZE
        || head.height > ITUNESDB_SLIDE_SIZE)
        return -1;
    const ssize_t size = head.width * head.height * sizeof(fb_data);
    memmove(buf, at + sizeof(head), size);
    *width = head.width;
    *height = head.height;
    return size;
}

/* the header's commitid when a making was cut short, by Menu or USB: the rest
   is made at the next Updating Library */
#define COVERS_CUT_SHORT (-1)

/* the songs' file is whole and goes with covers.dat as loaded */
static bool songs_whole(void)
{
    struct songs_header h;
    const int fd = open(COVERS_SONGS, O_RDONLY);
    const bool whole = fd >= 0 && songs_header_read(fd, &h) && h.whole
                       && h.covers == covers_sum;
    if (fd >= 0)
        close(fd);
    return whole;
}

bool itunesdb_covers_stale(void)
{
    /* after a database's change they're made anyway; this is for a first
       start, and for the rest of a making cut short */
    struct covers_header h;
    int fd = open(COVERS_FILE, O_RDONLY);
    if (fd < 0)
        return true;
    bool made = read(fd, &h, sizeof(h)) == sizeof(h) && h.magic == COVERS_MAGIC
                && h.version == COVERS_VERSION
                && h.commitid != COVERS_CUT_SHORT;
    close(fd);
    /* and the songs' own, which the albums' of an older firmware haven't */
#ifdef ITUNESDB_FINDER_LARGE
    /* and the pane's pictures of Finder's songs, made for its artwork now */
    if (made && songs_whole() && !finder_whole())
        return true;
#endif
    return !made || !songs_whole();
}

/* the table written whole, through a file of its own: the writer shared with
   Play Counts, which the tagcache thread writes as USB comes, mixed the two */
static bool write_covers(const struct cover_entry *entries, int n, bool whole)
{
    struct covers_header h = { .magic = COVERS_MAGIC,
                               .version = COVERS_VERSION, .count = n,
                               .commitid = whole ? 0 : COVERS_CUT_SHORT };
    int fd = whole_file_open(COVERS_FILE);
    if (fd < 0)
        return false;
    const ssize_t size = n * sizeof(*entries);
    return whole_file_close(fd, COVERS_FILE,
                            write(fd, &h, sizeof(h)) == sizeof(h)
                            && (!n || write(fd, entries, size) == size));
}

static void covers_images_made(void);
static bool write_songs(const struct song_record *songs, int n,
                        uint32_t covers, bool whole);

/* The songs' file as it is said to be whole, for this table: what a making
   cut short made is kept, those it found changed and didn't make again
   being said to be (SONG_STALE). None, of another table's. */
static void songs_said_whole(uint32_t covers)
{
    struct songs_header h;
    int fd = open(COVERS_SONGS, O_RDWR);
    const bool kept = fd >= 0 && songs_header_read(fd, &h)
                      && h.covers == covers;
    if (kept)
    {
        h.whole = 1;
        if (lseek(fd, 0, SEEK_SET) == 0)
            write(fd, &h, sizeof(h));
    }
    if (fd >= 0)
        close(fd);
    if (!kept)
        write_songs(NULL, 0, covers, true);
}

void itunesdb_covers_skip(void)
{
    /* none made for this database, as when there was no room for making them:
       not taken for one cut short, which would be tried at every boot; nor
       of the computer's */
    itunesdb_covers_inbox_drop();
    if (!itunesdb_covers_stale())
        return;
    covers_load();
    const struct cover_entry *old = covers_index.handle > 0
        ? core_get_data_pinned(covers_index.handle) : NULL;
    const int n = old ? (int)covers_index.count : 0;
    const bool written = write_covers(old, n, true);
    const uint32_t sum = covers_entries_sum(old, n);
    if (old)
        core_put_data_pinned((void *)old);
    /* the songs' as they are, for the same database */
    if (written && !songs_whole())
        songs_said_whole(sum);
#ifdef ITUNESDB_FINDER_LARGE
    if (!finder_whole())
        finder_said_whole();
#endif
    covers_images_made();
    covers_load();
}

/* Making them: the table as it was, then those put meanwhile; each with
   whether its album was seen */
struct cover_work {
    struct cover_entry e;
    bool seen;
};
static int work_handle;
/* COVERS_FREE_MIN, found once a making: it opens the database's files */
static unsigned long covers_floor_kb;
static int work_count, work_old, work_room;
/* the places the bitmaps after the entries have: of the albums' and of the
   songs', a place being free when neither has it; then one of the large
   pictures' places, COVERS_LARGE_MAX */
static uint32_t work_slots;
/* below them, none free: where the next free one is looked for from */
static uint32_t slot_hint, large_hint;
static bool work_changed;   /* an album made or gone */
static bool covers_no_room; /* none found on the disk for a picture */
static bool work_marked;    /* the file said to be in the making */
/* the entries the table on disk has, -1 when it isn't the one the making
   started from */
static int work_disk_count;
/* the album a making before was cut short at, and how many times */
static uint64_t work_prev;
static uint32_t work_prev_stamp, work_prev_tries;

/* The songs a making has: those of the file, by their paths, then those put
   since; and the pictures they have, by their sums */
static int songs_handle, pictures_handle;
static int songs_old, songs_count, songs_room;
static size_t pictures_room;
static bool songs_sorted;   /* all of them by their paths, none put since */
static bool songs_changed;
static bool songs_images_changed;
/* the song whose decode a making before was cut short at, how many times,
   and whether it's given up */
static uint64_t songs_prev;
static uint32_t songs_prev_mtime, songs_prev_tries;
static bool songs_prev_given_up;

static struct cover_work *work_entries(void)
{
    return core_get_data(work_handle);
}

/* a bit for each place, whether an album has it */
static uint8_t *work_used(void)
{
    return (uint8_t *)(work_entries() + work_room);
}

/* and whether a song has it */
static uint8_t *songs_used(void)
{
    return work_used() + (work_slots + 7) / 8;
}

/* whether a large picture has a place in COVERS_LARGE */
static uint8_t *large_used(void)
{
    return songs_used() + (work_slots + 7) / 8;
}
#define LARGE_USED_SIZE ((COVERS_LARGE_MAX + 1 + 7) / 8)

static struct song_record *song_records(void)
{
    return core_get_data(songs_handle);
}

static struct covers_picture *pictures(void)
{
    return core_get_data(pictures_handle);
}

static int work_find(uint64_t key);

/* the table as the making has it before any album's put, said to be cut
   short */
static bool write_work(void)
{
    struct covers_header h = { .magic = COVERS_MAGIC,
                               .version = COVERS_VERSION, .count = work_old,
                               .commitid = COVERS_CUT_SHORT };
    int fd = whole_file_open(COVERS_FILE);
    if (fd < 0)
        return false;
    core_pin(work_handle);
    const struct cover_work *w = work_entries();
    bool ok = write(fd, &h, sizeof(h)) == sizeof(h);
    for (int i = 0; ok && i < work_old; i++)
        ok = write(fd, &w[i].e, sizeof(w[i].e)) == sizeof(w[i].e);
    core_unpin(work_handle);
    return whole_file_close(fd, COVERS_FILE, ok);
}

static void songs_work_free(void)
{
    if (songs_handle > 0)
        core_free(songs_handle);
    if (pictures_handle > 0)
        core_free(pictures_handle);
    songs_handle = pictures_handle = 0;
}

/* The songs of the file into the making, if it goes with covers.dat as
   loaded: their count, and the highest slot they have. Without, the large
   pictures go too, as their slots may be others' now. */
static bool songs_work_load(int songs, uint32_t *top)
{
    struct songs_header h;
    int fd = open(COVERS_SONGS, O_RDONLY);
    const bool valid = fd >= 0 && songs_header_read(fd, &h);
    const bool trusted = valid && h.covers == covers_sum;
    songs_prev = 0;
    songs_prev_tries = 0;
    songs_prev_given_up = false;
    if (valid && h.making)
    {
        songs_prev = h.making;
        songs_prev_mtime = h.making_mtime;
        songs_prev_tries = h.making_tries;
        songs_prev_given_up = h.making_tries >= 2;
    }
    songs_old = trusted ? (int)h.count : 0;
    songs_room = songs_old + MAX(songs, 0) + 1;
    songs_handle = db_alloc(songs_room * sizeof(struct song_record));
    /* the file's pictures, and one more at most for each song */
    pictures_room = covers_pictures_room((trusted ? h.pictures : 0)
                                         + MAX(songs, 0) + 1);
    pictures_handle = songs_handle > 0
        ? db_alloc(pictures_room * sizeof(struct covers_picture)) : 0;
    if (songs_handle <= 0 || pictures_handle <= 0)
    {
        if (fd >= 0)
            close(fd);
        songs_work_free();
        return false;
    }
    memset(pictures(), 0, pictures_room * sizeof(struct covers_picture));
    bool ok = true;
    if (songs_old)
    {
        core_pin(songs_handle);
        const ssize_t size = songs_old * sizeof(struct song_record);
        ok = read(fd, song_records(), size) == size
             && index_hash(song_records(), size, 2166136261u) == h.sum;
        core_unpin(songs_handle);
    }
    /* the pictures' sizes and colours, from the part after the songs' own,
       by their slots: each song's into the table by its sums */
    if (ok && songs_old && h.pictures)
    {
        const off_t at = sizeof(h) + (off_t)h.count * sizeof(struct song_record)
                         + (off_t)h.shown * sizeof(struct covers_song_ram);
        const size_t size = h.pictures * sizeof(struct song_picture);
        const int part = db_alloc(size);
        ok = part > 0 && lseek(fd, at, SEEK_SET) == at;
        if (ok)
        {
            core_pin(part);
            core_pin(songs_handle);
            core_pin(pictures_handle);
            const struct song_picture *sp = core_get_data(part);
            const struct song_record *r = song_records();
            ok = read(fd, (void *)sp, size) == (ssize_t)size;
            for (int k = 0; ok && k < songs_old; k++)
            {
                const uint32_t slot = COVERS_SLOT_OF(r[k].word);
                int lo = 0, hi = (int)h.pictures - 1, at_pic = -1;
                while (slot && lo <= hi && at_pic < 0)
                {
                    const int mid = (lo + hi) / 2;
                    if (sp[mid].slot == slot)
                        at_pic = mid;
                    else if (sp[mid].slot < slot)
                        lo = mid + 1;
                    else
                        hi = mid - 1;
                }
                if (at_pic < 0)
                    continue;
                struct covers_picture *t =
                    covers_picture_place(pictures(), pictures_room, r[k].crc,
                                         r[k].adler, r[k].pic_size);
                if (t && !t->size)
                {
                    t->crc = r[k].crc;
                    t->adler = r[k].adler;
                    t->size = r[k].pic_size;
                    t->slot = slot;
                    t->large = sp[at_pic].large <= COVERS_LARGE_MAX
                               ? sp[at_pic].large : 0;
                    t->width = sp[at_pic].width;
                    t->height = sp[at_pic].height;
                    t->colour = sp[at_pic].colour;
                }
            }
            core_unpin(pictures_handle);
            core_unpin(songs_handle);
            core_unpin(part);
        }
        if (part > 0)
            core_free(part);
    }
    if (fd >= 0)
        close(fd);
    if (!ok)
    {
        /* damaged: all made again */
        songs_old = 0;
        memset(pictures(), 0, pictures_room * sizeof(struct covers_picture));
    }
    if (!trusted || !ok)
    {
        remove(COVERS_LARGE);
#ifdef ITUNESDB_FINDER_LARGE
        finder_large_gone = true;
#endif
        /* and the menus' pane's covers test builds before this kept,
           which nothing reads now: once, as the songs' are first made */
        remove(ROCKBOX_DIR "/menu_covers.dat");
    }
    songs_count = songs_old;
    songs_sorted = true;
    songs_changed = !trusted || !ok;
    *top = 0;
    const struct song_record *r = song_records();
    for (int i = 0; i < songs_old; i++)
    {
        /* a slot past the files' bounds, as of a file read damaged: the
           song's made again */
        if (COVERS_SLOT_OF(r[i].word) > COVERS_SLOTS_MAX)
            song_records()[i].mtime = 0;
        else
            *top = MAX(*top, COVERS_SLOT_OF(r[i].word));
    }
    return true;
}

static void inbox_load(void);
static void inbox_making_over(bool spent);

bool itunesdb_covers_begin(int albums, int songs, bool images_changed)
{
    itunesdb_covers_end(COVERS_CUT);
    covers_floor_kb = COVERS_FREE_MIN;
    covers_no_room = false;
    /* as the file has it, which USB may have changed */
    forget_index(&covers_index);
    covers_load();
    work_old = covers_index.handle > 0 ? (int)covers_index.count : 0;
    /* and where a making before was cut short */
    struct covers_header h;
    int fd = open(COVERS_FILE, O_RDONLY);
    const bool read_ok = fd >= 0 && read(fd, &h, sizeof(h)) == sizeof(h)
                         && h.magic == COVERS_MAGIC
                         && h.version == COVERS_VERSION;
    if (fd >= 0)
        close(fd);
    work_disk_count = read_ok && h.count == (uint32_t)work_old ? work_old : -1;
    work_prev = 0;
    work_prev_tries = 0;
    if (read_ok && h.commitid == COVERS_CUT_SHORT && h.making)
    {
        work_prev = h.making;
        work_prev_stamp = h.making_stamp;
        work_prev_tries = h.making_tries;
    }
    uint32_t songs_top;
#ifdef ITUNESDB_FINDER_LARGE
    finder_large_gone = false;
#endif
    if (!songs_work_load(songs, &songs_top))
        return false;
    songs_images_changed = images_changed;
    /* and one for an album given up */
    work_room = work_old + MAX(albums, 0) + 1;
    const struct cover_entry *old = work_old ? core_get_data(covers_index.handle)
                                             : NULL;
    uint32_t top = songs_top;
    for (int i = 0; i < work_old; i++)
    {
        if (old[i].slot <= COVERS_SLOTS_MAX)
            top = MAX(top, old[i].slot);
    }
    /* places for every album and every picture there can be, past those
       taken */
    work_slots = top + MAX(albums, 0) + MAX(songs, 0) + 1;
    work_handle = db_alloc(MAX(work_room, 1) * sizeof(struct cover_work)
                           + 2 * ((work_slots + 7) / 8) + LARGE_USED_SIZE);
    if (work_handle <= 0)
    {
        work_handle = 0;
        songs_work_free();
        return false;
    }
    /* again, as the allocation may have moved it */
    old = work_old ? core_get_data(covers_index.handle) : NULL;
    struct cover_work *w = work_entries();
    uint8_t *used = work_used();
    memset(used, 0, 2 * ((work_slots + 7) / 8) + LARGE_USED_SIZE);
    uint8_t *song_used = songs_used();
    const struct song_record *r = song_records();
    for (int i = 0; i < songs_old; i++)
    {
        const uint32_t slot = COVERS_SLOT_OF(r[i].word);
        if (slot && slot < work_slots)
            song_used[slot / 8] |= 1 << (slot % 8);
    }
    /* the large pictures' places the file's pictures have: none given to
       another in this making, as the songs' file it started from may be
       taken again after a reset */
    uint8_t *large = large_used();
    const struct covers_picture *t = pictures();
    for (size_t i = 0; i < pictures_room; i++)
    {
        if (t[i].size && t[i].large)
            large[t[i].large / 8] |= 1 << (t[i].large % 8);
    }
    slot_hint = large_hint = 1;
    bool dropped = false;
    for (int i = 0; i < work_old; i++)
    {
        w[i].e = old[i];
        w[i].seen = false;
        if (w[i].e.slot > COVERS_SLOTS_MAX
            || (w[i].e.slot && !covers_have_frames))
        {
            /* of a table read damaged, or with its frames gone: made again */
            w[i].e.slot = 0;
            w[i].e.stamp = 0;
            w[i].e.width = 0;
            dropped = true;
        }
        if (w[i].e.slot)
            used[w[i].e.slot / 8] |= 1 << (w[i].e.slot % 8);
    }
    work_count = work_old;
    work_changed = false;
    work_marked = false;
    if (work_prev && work_prev_tries >= 2)
    {
        /* cut short twice at this album: without a cover, rather than every
           boot stopping there */
        int i = work_find(work_prev);
        if (i < 0)
        {
            /* in its place among the others, as the table on disk has them */
            for (i = work_old; i > 0 && w[i - 1].e.key > work_prev; i--)
                w[i] = w[i - 1];
            memset(&w[i], 0, sizeof(w[i]));
            w[i].e.key = work_prev;
            work_old++;
            work_count++;
        }
        else if (w[i].e.slot)
            used[w[i].e.slot / 8] &= ~(1 << (w[i].e.slot % 8));
        w[i].e.stamp = work_prev_stamp;
        w[i].e.slot = 0;
        w[i].e.width = COVER_GIVEN_UP;
        w[i].e.height = 0;
        w[i].e.colour = COVER_COLOUR_UNKNOWN;
        work_prev = 0;
        work_changed = true;
        dropped = true;
    }
    /* the table on disk as this one at once: places it dropped are taken
       again, and an album given up stays so through another reset, with
       two such covers too */
    if (dropped)
        work_disk_count = write_work() ? work_old : -1;
#ifdef ITUNESDB_FINDER_LARGE
    /* after the rest: its allocations may move the making's */
    finder_work_load();
#endif
    /* the computer's, for a making of the songs: after the making's own
       memory, which it may go without */
    if (songs > 0)
        inbox_load();
    return true;
}

/* the entry of the album being made, -1 for none: the old ones in order */
static int work_find(uint64_t key)
{
    const struct cover_work *w = work_entries();
    int lo = 0, hi = work_old - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (w[mid].e.key == key)
            return mid;
        if (w[mid].e.key < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    for (int i = work_old; i < work_count; i++)
    {
        if (w[i].e.key == key)
            return i;
    }
    return -1;
}

int itunesdb_cover_known(uint64_t key, uint32_t stamp)
{
    if (work_handle <= 0)
        return 0;
    int i = work_find(key);
    if (i < 0 || work_entries()[i].e.stamp != stamp)
        return 0;
    work_entries()[i].seen = true;
    const struct cover_entry *e = &work_entries()[i].e;
    if (e->slot || e->width == COVER_GIVEN_UP)
        return 1;
    return e->width == COVER_UNSHOWN ? -2 : -1;
}

static uint32_t covers_images;
static bool covers_images_read, covers_images_had;

bool itunesdb_covers_images_changed(void)
{
    uint32_t sum;
    if (!tagcache_outside_images(&sum))
        return false;
    if (!covers_images_read)
    {
        covers_images_read = true;
        int fd = open(COVERS_IMAGES, O_RDONLY);
        if (fd >= 0)
        {
            covers_images_had = read(fd, &covers_images, sizeof(covers_images))
                                == sizeof(covers_images);
            close(fd);
        }
    }
    return !covers_images_had || sum != covers_images;
}

/* the images the covers were made with now, once every album was seen */
static void covers_images_made(void)
{
    uint32_t sum;
    if (!tagcache_outside_images(&sum)
        || (covers_images_read && covers_images_had && sum == covers_images))
        return;
    if (whole_file_write(COVERS_IMAGES, &sum, sizeof(sum)))
    {
        covers_images = sum;
        covers_images_read = covers_images_had = true;
    }
}

static bool slot_taken(const uint8_t *used, uint32_t slot)
{
    return slot < work_slots && (used[slot / 8] & (1 << (slot % 8)));
}

static void slot_take(uint8_t *used, uint32_t slot)
{
    if (slot && slot < work_slots)
        used[slot / 8] |= 1 << (slot % 8);
}

/* the first place neither an album nor a song has, taken for one or the
   other; 0 for none left */
static uint32_t free_slot(bool song)
{
    for (uint32_t slot = slot_hint; slot < work_slots && slot <= COVERS_SLOTS_MAX;
         slot++)
    {
        if (!slot_taken(work_used(), slot) && !slot_taken(songs_used(), slot))
        {
            slot_take(song ? songs_used() : work_used(), slot);
            slot_hint = slot + 1;
            return slot;
        }
    }
    return 0;
}

/* a place let go of, of used: another's then, if the other bitmap hasn't it */
static void slot_drop(uint8_t *used, uint32_t slot)
{
    if (slot && slot < work_slots)
    {
        used[slot / 8] &= ~(1 << (slot % 8));
        slot_hint = MIN(slot_hint, slot);
    }
}

/* an album's place let go of */
static void release_slot(uint32_t slot)
{
    slot_drop(work_used(), slot);
}

/* the first large picture's place no picture has, taken; 0 for none left */
static uint32_t free_large(void)
{
    uint8_t *used = large_used();
    for (uint32_t large = large_hint; large <= COVERS_LARGE_MAX; large++)
    {
        if (!(used[large / 8] & (1 << (large % 8))))
        {
            used[large / 8] |= 1 << (large % 8);
            large_hint = large + 1;
            return large;
        }
    }
    return 0;
}

/* one taken here and not written: another's then */
static void drop_large(uint32_t large)
{
    if (large && large <= COVERS_LARGE_MAX)
    {
        large_used()[large / 8] &= ~(1 << (large % 8));
        large_hint = MIN(large_hint, large);
    }
}

/* data at pos of the file, blank places put before it where it's shorter */
static bool write_slot(int fd, off_t pos, const void *data, size_t size)
{
    off_t end = ffilesize(fd);
    if (end < 0 || lseek(fd, MIN(end, pos), SEEK_SET) < 0)
        return false;
    /* the gap from where the file ends, only: a frame is shorter than its
       place, and padding whole places wrote 80 KB more for each album */
    static const unsigned char blank[2048];
    for (end = MIN(end, pos); end < pos; )
    {
        size_t n = MIN((off_t)sizeof(blank), pos - end);
        if (write(fd, blank, n) != (ssize_t)n)
            return false;
        end += n;
    }
    return lseek(fd, pos, SEEK_SET) == pos
           && write(fd, data, size) == (ssize_t)size;
}

/* the file said to be cut short, as it is until the end writes it again, and
   the album being made: a reset or a crash meanwhile left one said to be
   whole, and the rest wasn't made; a cover that crashed the decoder or hung
   it was made again at every boot */
static void mark_making(uint64_t key, uint32_t stamp)
{
    struct covers_header h;
    int fd = open(COVERS_FILE, O_RDWR);
    if (fd < 0 || read(fd, &h, sizeof(h)) != sizeof(h)
        || h.magic != COVERS_MAGIC || h.version != COVERS_VERSION
        || work_disk_count < 0 || h.count != (uint32_t)work_disk_count)
    {
        /* none, or not the table the making started from, whose places it
           takes again: an empty one */
        if (fd >= 0)
            close(fd);
        if (!write_covers(NULL, 0, false))
        {
            /* rather than the old one patched: none, all made again */
            remove(COVERS_FILE);
            work_disk_count = -1;
            return;
        }
        work_disk_count = 0;
        fd = open(COVERS_FILE, O_RDWR);
        if (fd >= 0 && read(fd, &h, sizeof(h)) != sizeof(h))
        {
            close(fd);
            fd = -1;
        }
    }
    if (fd < 0)
        return;
    h.commitid = COVERS_CUT_SHORT;
    h.making_tries = key == work_prev && stamp == work_prev_stamp
                     ? work_prev_tries + 1 : 1;
    h.making = key;
    h.making_stamp = stamp;
    if (lseek(fd, 0, SEEK_SET) == 0)
        write(fd, &h, sizeof(h));
    close(fd);
    work_marked = true;
}

/* room on the disk for the files to grow by this many bytes, the floor
   kept; Updating Library says so if there's none */
static bool covers_room_kb(off_t grow)
{
    const unsigned long floor = work_handle > 0 ? covers_floor_kb
                                                : COVERS_FREE_MIN;
    sector_t free = 0;
    volume_size(IF_MV(0,) NULL, &free);
    if (free >= floor + grow / 1024 + 1)
        return true;
    library_no_room();
    covers_no_room = true;
    return false;
}

/* room for one more place of each file, as a making starts */
bool itunesdb_covers_room(void)
{
    return covers_room_kb(COVER_SLOT + SLIDE_SLOT + COVER_LARGE_SLOT);
}

/* what the file grows by to hold end bytes */
static off_t file_growth(const char *path, off_t end)
{
    const int fd = open(path, O_RDONLY);
    const off_t size = fd >= 0 ? ffilesize(fd) : 0;
    if (fd >= 0)
        close(fd);
    return MAX(end - MAX(size, 0), 0);
}

/* Room for a picture's frame and slide at slot and its large picture at
   large (0 for none): what each file grows by, blank places before it from
   where it ends, as write_slot() puts them. Asked only when one grows: one
   written over in its place takes nothing more. */
static bool covers_room_at(uint32_t slot, uint32_t large)
{
    const off_t grow = file_growth(COVERS_FRAMES, (off_t)slot * COVER_SLOT)
                       + file_growth(COVERS_SLIDES, (off_t)slot * SLIDE_SLOT)
                       + (large ? file_growth(COVERS_LARGE,
                                              (off_t)large * COVER_LARGE_SLOT)
                                : 0);
    return !grow || covers_room_kb(grow);
}

/* A slot's frame of bm's picture, w by h, and its slide, into the files, and
   the colour of the slide into *colour; with art_lock held. The slide as a
   computer made it, if slide isn't NULL, for this size of the frame, *colour
   then left as the caller has it; else both made of the frame here. A slide
   cut short by switching off isn't taken for one: its header goes last, as
   make_slide() writes it. */
static bool write_frame(uint32_t slot, uint64_t key, const fb_data *frame,
                        int fw, int fh, const fb_data *slide,
                        uint32_t *colour)
{
    struct slide_header *head = (struct slide_header *)slide_slot;
    fb_data *pixels = (fb_data *)(slide_slot + sizeof(*head));
    memset(slide_slot, 0, sizeof(slide_slot));
    /* the slide, the largest that fits, keeping the shape */
    int w, h;
    covers_slide_fit(fw, fh, ITUNESDB_SLIDE_SIZE, &w, &h);
    if (slide)
        memcpy(pixels, slide, w * h * sizeof(fb_data));
    else
    {
        cpu_boost(true);
        scale_frame(frame, fw, fh, pixels, w, h, true);
        cpu_boost(false);
        *colour = cover_palette_colour(pixels, h, w);
    }
    head->magic = 0;
    head->width = w;
    head->height = h;
    head->key = key;
    /* a frame read last of this place is another album's now */
    frame_path[0] = '\0';
    itunesdb_slides_done();
    int frames = open(COVERS_FRAMES, O_RDWR | O_CREAT, 0666);
    int slides = open(COVERS_SLIDES, O_RDWR | O_CREAT, 0666);
    const off_t slide_pos = (off_t)(slot - 1) * SLIDE_SLOT;
    bool ok = frames >= 0 && slides >= 0
              && write_slot(frames, (off_t)(slot - 1) * COVER_SLOT, frame,
                            fw * fh * sizeof(fb_data))
              && write_slot(slides, slide_pos, slide_slot, SLIDE_SLOT);
    if (ok)
    {
        head->magic = SLIDE_MAGIC;
        ok = lseek(slides, slide_pos, SEEK_SET) == slide_pos
             && write(slides, head, sizeof(*head)) == sizeof(*head);
    }
    if (frames >= 0)
        ok = close(frames) == 0 && ok;
    if (slides >= 0)
        ok = close(slides) == 0 && ok;
    return ok;
}

int itunesdb_cover_put(uint64_t key, uint32_t stamp, const struct bitmap *bm)
{
    if (work_handle <= 0)
        return 0;
    if (!work_marked)
        mark_making(0, 0); /* cut short, with no album being decoded */
    int i = work_find(key);
    if (i < 0)
    {
        if (work_count >= work_room)
            return 0;
        i = work_count++;
        memset(&work_entries()[i], 0, sizeof(struct cover_work));
        work_entries()[i].e.key = key;
    }
    struct cover_entry e = work_entries()[i].e;
    e.stamp = stamp;
    e.width = e.height = 0;
    e.colour = COVER_COLOUR_UNKNOWN;
    e.image = 0;
    bool ok = true;
    const uint32_t had_slot = e.slot;
    /* a place of its own, not a song's, which keeps its picture */
    if (e.slot && slot_taken(songs_used(), e.slot))
        e.slot = 0;
    if (bm && bm->width > 0 && bm->height > 0
        && bm->width * bm->height <= FRAME_MAX)
    {
        const bool new_slot = !e.slot;
        if (new_slot)
            e.slot = free_slot(false);
        /* the files growing, only with room left on the disk; none, or no
           place left, ends the making, the rest at a boot with room */
        ok = e.slot && covers_room_at(e.slot, 0);
        if (ok)
        {
            lock_art();
            ok = write_frame(e.slot, key, (const fb_data *)bm->data,
                             bm->width, bm->height, NULL, &e.colour);
            mutex_unlock(&art_lock);
        }
        if (ok)
        {
            e.width = bm->width;
            e.height = bm->height;
        }
        else
        {
            /* not known, but as one to make again; its place, written in
               part, for another */
            if (new_slot)
                release_slot(e.slot);
            e.stamp = 0;
            e.slot = 0;
        }
    }
    if (!e.width)
        e.slot = 0;
    /* none, or another: its old place for another */
    if (had_slot != e.slot)
        release_slot(had_slot);
    work_entries()[i].e = e;
    work_entries()[i].seen = true;
    work_changed = true;
    return ok ? 1 : -1;
}

/** The songs' own **/

static int cmp_song_record(const void *a, const void *b)
{
    const uint64_t x = ((const struct song_record *)a)->path;
    const uint64_t y = ((const struct song_record *)b)->path;
    return (x > y) - (x < y);
}

/* the song's entry, -1 for none: the old ones by their paths, then those put
   since */
static int song_find(uint64_t path)
{
    const struct song_record *r = song_records();
    int lo = 0, hi = songs_old - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (r[mid].path == path)
            return mid;
        if (r[mid].path < path)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    for (int i = songs_old; i < songs_count; i++)
    {
        if (r[i].path == path)
            return i;
    }
    return -1;
}

/* all of them by their paths, as the albums look up their songs */
static void songs_sort(void)
{
    if (songs_sorted || songs_handle <= 0)
        return;
    core_pin(songs_handle);
    qsort(song_records(), songs_count, sizeof(struct song_record),
          cmp_song_record);
    core_unpin(songs_handle);
    songs_old = songs_count;
    songs_sorted = true;
}

static bool inbox_has(uint32_t size, uint32_t crc, uint32_t adler);

int itunesdb_song_known(uint64_t path, uint32_t mtime, uint32_t source)
{
    if (songs_handle <= 0)
        return 0;
    const int i = song_find(path);
    if (i < 0)
        return 0;
    struct song_record *r = &song_records()[i];
    /* one whose file the database had no time of: read again */
    if (!r->mtime)
        return 0;
    /* changed: its cover isn't shown until it's made again, which a making
       cut short may leave for the next */
    if (r->mtime != mtime)
    {
        r->word |= (uint32_t)SONG_STALE << COVERS_FLAGS_SHIFT;
        return 0;
    }
    /* a cover of the folder's image, or none, which one may give now */
    if (songs_images_changed && !(COVERS_FLAGS_OF(r->word) & SONG_SEEN)
        && (r->source || !COVERS_SLOT_OF(r->word)))
    {
        if (source == ~0u)
            return 2;
        if (source != r->source)
        {
            r->word |= (uint32_t)SONG_STALE << COVERS_FLAGS_SHIFT;
            return 0;
        }
    }
    /* its file as its entry was made of again, as put back with its time:
       its cover is shown again */
    if (COVERS_FLAGS_OF(r->word) & SONG_STALE)
    {
        r->word &= ~((uint32_t)SONG_STALE << COVERS_FLAGS_SHIFT);
        songs_changed = true;
    }
    r->word |= (uint32_t)SONG_SEEN << COVERS_FLAGS_SHIFT;
    /* a picture that wasn't shown, which the computer's covers may have:
       looked at again, and kept as it is if they haven't. One never summed,
       a PNG, is looked for by its quick key. */
    if (!COVERS_SLOT_OF(r->word) && COVERS_REASON_OF(r->word)
        && itunesdb_inbox_offered()
        && (!r->pic_size || inbox_has(r->pic_size, r->crc, r->adler)))
        return 3;
    return 1;
}

/* the song's entry as given, with this word: in place of its old one, or new */
static bool song_record_put(const struct itunesdb_song_cover *song,
                            uint32_t word)
{
    int i = song_find(song->path);
    if (i < 0)
    {
        if (songs_count >= songs_room)
            return false;
        i = songs_count++;
        songs_sorted = false;
    }
    struct song_record *r = &song_records()[i];
    memset(r, 0, sizeof(*r));
    r->path = song->path;
    r->size = song->size;
    r->mtime = song->mtime;
    r->pic_pos = song->pic_pos;
    r->pic_size = song->pic_size;
    r->source = song->source;
    r->crc = song->crc;
    r->adler = song->adler;
    r->check = song->check;
    r->word = word | (uint32_t)SONG_SEEN << COVERS_FLAGS_SHIFT;
    songs_changed = true;
    return true;
}

int itunesdb_song_picture(const struct itunesdb_song_cover *song)
{
    if (songs_handle <= 0 || work_handle <= 0)
        return 0;
    if (songs_prev_given_up && song->path == songs_prev
        && song->mtime == songs_prev_mtime)
    {
        song_record_put(song, COVERS_WORD(0, ITUNESDB_COVER_UNREADABLE, 0));
        return -1;
    }
    if (!song->pic_size)
        return 0;
    const struct covers_picture *p =
        covers_picture_place(pictures(), pictures_room, song->crc,
                             song->adler, song->pic_size);
    if (!p || !p->size)
        return 0;
    song_record_put(song, COVERS_WORD(p->slot, 0, 0));
    return 1;
}

void itunesdb_song_making(const struct itunesdb_song_cover *song)
{
    if (songs_handle <= 0)
        return;
    if (!work_marked)
        mark_making(0, 0);
    /* in the file's header, kept as it is otherwise: a new one if there is
       none of this version */
    struct songs_header h;
    int fd = open(COVERS_SONGS, O_RDWR);
    if (fd < 0 || !songs_header_read(fd, &h))
    {
        if (fd >= 0)
            close(fd);
        memset(&h, 0, sizeof(h));
        h.magic = SONGS_MAGIC;
        h.version = SONGS_VERSION;
        fd = creat(COVERS_SONGS, 0666);
    }
    if (fd < 0)
        return;
    h.whole = 0;
    h.making_tries = song->path == songs_prev && song->mtime == songs_prev_mtime
                     ? songs_prev_tries + 1 : 1;
    h.making = song->path;
    h.making_mtime = song->mtime;
    if (lseek(fd, 0, SEEK_SET) == 0)
        write(fd, &h, sizeof(h));
    close(fd);
}

/* The parts of a picture a computer made (the inbox's), each NULL for none
   or for one of another size than made here: the frame, the slide, and the
   slide's colour with it */
struct prepared {
    const fb_data *frame, *slide;
    uint32_t colour;
};

/* A song's picture, bm, into p's slot: its frame, slide and colour made of
   it, or taken from pre, or with adopt only its large picture, the rest an
   album's cover of it made before; the frame's size and the colour into p.
   Its large picture into p's large place, if it has one and the picture is
   as large: else none, p->large 0, the pane showing the frame larger. False
   for the frame or slide not written. bm's pixels are changed. */
static bool write_picture(struct bitmap *bm, bool adopt,
                          struct covers_picture *p, const struct prepared *pre)
{
    const uint32_t slot = p->slot;
    const int w = bm->width, h = bm->height;
    int fw = p->width, fh = p->height;
    bool ok = true;
    lock_art();
    if (!adopt)
    {
        /* the frame, 200 by 200 at most, from the picture, each pixel the
           mean of those it covers; its slide and colour from it */
        fw = fh = FRAME_SIDE;
        covers_fit(w, h, &fw, &fh);
        const fb_data *frame = pre ? pre->frame : NULL;
        frame_path[0] = '\0';
        frame_buf = NULL;
        if (!frame)
        {
            fb_data *made = (fb_data *)frame_raw;
            cpu_boost(true);
            /* a picture smaller than the frame, which the decoder didn't
               make larger, is scaled up */
            if (fw <= w && fh <= h)
                covers_area_scale((const fb_data *)bm->data, w, h, made, fw,
                                  fh, (uint32_t *)slide_slot);
            else
                scale_frame((const fb_data *)bm->data, w, h, made, fw, fh,
                            false);
            cpu_boost(false);
            frame = made;
        }
        const fb_data *slide = pre ? pre->slide : NULL;
        if (slide)
            p->colour = pre->colour;
        ok = write_frame(slot, ((uint64_t)p->crc << 32) | p->adler, frame, fw,
                         fh, slide, &p->colour);
        p->width = fw;
        p->height = fh;
    }
    /* the large one at the size the menus' pane fits the frame to */
    int lw = COVER_LARGE_SIDE, lh = COVER_LARGE_SIDE;
    covers_fit(fw, fh, &lw, &lh);
    bool large = false;
    if (ok && p->large && lw <= w && lh <= h)
    {
        if (lw != w || lh != h)
        {
            cpu_boost(true);
            /* shrinking a row at a time, its own pixels can take it */
            covers_area_scale((const fb_data *)bm->data, w, h,
                              (fb_data *)bm->data, lw, lh,
                              (uint32_t *)slide_slot);
            cpu_boost(false);
        }
        const struct large_slot head = { COVER_LARGE_MAGIC, lw, lh,
                                         p->crc, p->adler };
        const off_t pos = (off_t)(p->large - 1) * COVER_LARGE_SLOT;
        const int fd = open(COVERS_LARGE, O_RDWR | O_CREAT, 0666);
        static const struct large_slot none;
        /* its header last, none until then: one cut short isn't taken */
        large = fd >= 0 && write_slot(fd, pos, &none, sizeof(none))
                && write_slot(fd, pos + sizeof(head), bm->data,
                              lw * lh * sizeof(fb_data))
                && lseek(fd, pos, SEEK_SET) == pos
                && write(fd, &head, sizeof(head)) == sizeof(head);
        if (fd >= 0)
            large = close(fd) == 0 && large;
    }
    mutex_unlock(&art_lock);
    /* without it, the pane shows the frame larger */
    if (!large)
    {
        drop_large(p->large);
        p->large = 0;
    }
    return ok;
}

/* itunesdb_song_put()'s, with the parts a computer made of bm, or NULL */
static int song_put(const struct itunesdb_song_cover *song,
                    const struct bitmap *bm, int reason,
                    const struct prepared *pre)
{
    if (songs_handle <= 0 || work_handle <= 0)
        return 0;
    if (!work_marked)
        mark_making(0, 0);
    uint32_t slot = 0;
    if (bm && bm->width > 0 && bm->height > 0 && song->pic_size
        && bm->width <= COVER_LARGE_SIDE && bm->height <= COVER_LARGE_SIDE)
    {
        struct covers_picture *p =
            covers_picture_place(pictures(), pictures_room, song->crc,
                                 song->adler, song->pic_size);
        /* the table full, as it can't be: none put, the song made again
           the next time */
        if (!p)
            return 0;
        if (p->size)
            slot = p->slot; /* made since it was asked */
        else
        {
            struct covers_picture made = { song->crc, song->adler,
                                           song->pic_size, 0, 0, 0, 0,
                                           COVER_COLOUR_UNKNOWN };
            /* an album's cover made of this song before: its frame, slide
               and colour as they are, a song's now too */
            const int a = work_find(song->album);
            const struct cover_entry *e = a >= 0 ? &work_entries()[a].e : NULL;
            const bool adopt = e && song->album && e->stamp == song->album_stamp
                && e->slot && e->slot < work_slots && covers_have_frames
                && !slot_taken(songs_used(), e->slot)
                && e->width && e->height && e->width * e->height <= FRAME_MAX;
            if (adopt)
            {
                made.slot = e->slot;
                made.width = e->width;
                made.height = e->height;
                made.colour = e->colour;
                slot_take(songs_used(), made.slot);
            }
            else
                made.slot = free_slot(true);
            made.large = made.slot ? free_large() : 0;
            /* No place left, no room on the disk for what the files grow
               by, or the frame not written: the making ends with nothing
               put, the rest made at a boot with room. A song put as having
               none was so for good. */
            if (!made.slot || !covers_room_at(made.slot, made.large)
                || !write_picture((struct bitmap *)bm, adopt, &made, pre))
            {
                slot_drop(songs_used(), made.slot);
                drop_large(made.large);
                return -1;
            }
            /* again: the table may have moved meanwhile */
            p = covers_picture_place(pictures(), pictures_room, song->crc,
                                     song->adler, song->pic_size);
            if (p)
                *p = made;
            slot = made.slot;
        }
        reason = ITUNESDB_COVER_SHOWN;
    }
    return song_record_put(song, COVERS_WORD(slot, slot ? 0 : reason, 0))
           ? 1 : 0;
}

int itunesdb_song_put(const struct itunesdb_song_cover *song,
                      const struct bitmap *bm, int reason)
{
    return song_put(song, bm, reason, NULL);
}

/** The covers a computer prepared (covers_table.h): taken by the making
    rather than decoded, each part only at the size it's made here, and the
    file removed once a making is over, or as it can't be taken. **/

/* the colours worked out as cover_palette_colour() does now: another way
   of making them is another kind, and the computer's are made here again */
#define COVERS_COLOUR_KIND 1

static int inbox_handle;    /* its entries, during a making; then the rows */
static uint32_t inbox_count;
static uint32_t inbox_colour_kind;
/* the file's time, as the disk has it: a picture in a file of a later time
   came or changed after the computer's run, and is taken only by its sums,
   read here */
static uint32_t inbox_mtime;
/* the file is done with, to be removed after the lists' covers */
static bool inbox_spent;
static int inbox_taken, inbox_refused, inbox_rows_taken, inbox_newer;
/* after a making, the rows' covers of the pictures it took: by slot */
struct inbox_row {
    uint32_t slot, offset;
};
static uint32_t inbox_rows;

static void inbox_free(void)
{
    if (inbox_handle > 0)
        core_free(inbox_handle);
    inbox_handle = 0;
    inbox_count = inbox_rows = 0;
}

void itunesdb_covers_inbox_drop(void)
{
    inbox_free();
    inbox_spent = false;
    remove(COVERS_INBOX);
    remove(COVERS_INBOX_TMP);
}

void itunesdb_covers_inbox_done(void)
{
    if (inbox_rows_taken)
        health_note("covers: %d rows' covers taken from the computer's",
                    inbox_rows_taken);
    inbox_rows_taken = 0;
    if (inbox_spent)
        itunesdb_covers_inbox_drop();
    else
        inbox_free();
}

bool itunesdb_covers_inbox(void)
{
    return file_exists(COVERS_INBOX);
}

/* refused as a whole: removed, and said why */
static void inbox_refuse(const char *why)
{
    health_note("covers: the computer's refused, %s", why);
    itunesdb_covers_inbox_drop();
}

/* the file's entries into memory, for the making beginning */
static void inbox_load(void)
{
    itunesdb_covers_inbox_done();
    inbox_taken = inbox_refused = inbox_newer = 0;
    /* one a computer's write was cut short in */
    remove(COVERS_INBOX_TMP);
    const int fd = open(COVERS_INBOX, O_RDONLY);
    if (fd < 0)
        return;
    uint32_t time_size;
    if (!file_stamp_in(ROCKBOX_DIR, strrchr(COVERS_INBOX, '/') + 1, &time_size,
                       &inbox_mtime))
    {
        close(fd);
        inbox_refuse("its time");
        return;
    }
    struct covers_inbox_header h;
    const off_t size = ffilesize(fd);
    const bool ok = size > 0 && read(fd, &h, sizeof(h)) == sizeof(h)
                    && covers_inbox_header_ok(&h, size, COVER_LARGE_SIDE);
    const size_t bytes = ok ? h.count * sizeof(struct covers_inbox_entry) : 0;
    const int handle = ok ? db_alloc(bytes) : 0;
    bool read_ok = false;
    if (handle > 0)
    {
        core_pin(handle);
        struct covers_inbox_entry *e = core_get_data(handle);
        read_ok = read(fd, e, bytes) == (ssize_t)bytes
                  && covers_crc32(COVERS_CRC32_INIT, e, bytes) == h.index_crc
                  && covers_inbox_entries_ok(e, h.count, size);
        for (uint32_t i = 0; read_ok && i < h.count; i++)
            e[i].unused = 0;
        core_unpin(handle);
    }
    close(fd);
    if (!ok || !read_ok)
    {
        if (handle > 0)
            core_free(handle);
        /* none for want of memory, the making going on as without one */
        if (ok && handle <= 0)
        {
            health_note("covers: no memory for the computer's");
            inbox_spent = true;
            return;
        }
        inbox_refuse(ok ? "its entries" : "its header");
        return;
    }
    inbox_handle = handle;
    inbox_count = h.count;
    inbox_colour_kind = h.colour_kind;
    /* printable, whatever the file says */
    char maker[sizeof(h.maker) + 1];
    for (size_t i = 0; i < sizeof(h.maker); i++)
        maker[i] = h.maker[i] >= ' ' && h.maker[i] < 0x7f ? h.maker[i]
                   : h.maker[i] ? '?' : '\0';
    maker[sizeof(h.maker)] = '\0';
    health_note("covers: the computer's %lu pictures, by %s",
                (unsigned long)h.count, maker);
}

/* a picture of these sums the inbox has, with its blocks or without */
static bool inbox_has(uint32_t size, uint32_t crc, uint32_t adler)
{
    if (!itunesdb_inbox_offered())
        return false;
    core_pin(inbox_handle);
    const bool has = covers_inbox_find_sums(core_get_data(inbox_handle),
                                            inbox_count, size, crc, adler);
    core_unpin(inbox_handle);
    return has;
}

bool itunesdb_inbox_offered(void)
{
    return inbox_handle > 0 && inbox_count && !inbox_rows;
}

/* A picture in a file of that time known by its quick key: not one later
   than the inbox's file, which came or changed after the computer's run,
   nor one of a time not known (0) */
static bool inbox_time_trusted(uint32_t mtime)
{
    return mtime && mtime <= inbox_mtime;
}

bool itunesdb_inbox_trusted(uint32_t mtime)
{
    if (!itunesdb_inbox_offered())
        return false;
    const bool trusted = inbox_time_trusted(mtime);
    inbox_newer += !trusted;
    return trusted;
}

int itunesdb_inbox_find(struct itunesdb_song_cover *song, uint32_t quick)
{
    if (!itunesdb_inbox_offered())
        return 0;
    /* a file of a later time than the computer's, or of one not known: not
       known by its quick key, as it may have come or changed since */
    if (!inbox_time_trusted(song->mtime))
        return 0;
    core_pin(inbox_handle);
    const struct covers_inbox_entry *e =
        covers_inbox_find(core_get_data(inbox_handle), inbox_count,
                          song->pic_size, quick);
    int found = 0;
    if (e)
    {
        song->crc = e->crc;
        song->adler = e->adler;
        found = e->offset ? 1 : 2;
    }
    core_unpin(inbox_handle);
    return found;
}

int itunesdb_inbox_take(const struct itunesdb_song_cover *song, void *buf,
                        size_t size)
{
    if (!itunesdb_inbox_offered() || songs_handle <= 0 || work_handle <= 0)
        return 0;
    core_pin(inbox_handle);
    struct covers_inbox_entry *base = core_get_data(inbox_handle);
    const struct covers_inbox_entry *e =
        covers_inbox_find_sums(base, inbox_count, song->pic_size, song->crc,
                               song->adler);
    const struct covers_inbox_entry entry = e ? *e : (struct covers_inbox_entry){ 0 };
    const uint32_t at = e ? (uint32_t)(e - base) : 0;
    core_unpin(inbox_handle);
    if (!entry.offset)
        return 0;
    char *run = buf;
    size_t room = size;
    STORAGE_ALIGN_BUFFER(run, room);
    struct covers_inbox_parts parts;
    bool ok = entry.length <= room;
    const int fd = ok ? open(COVERS_INBOX, O_RDONLY) : -1;
    ok = fd >= 0 && lseek(fd, entry.offset, SEEK_SET) == (off_t)entry.offset
         && read(fd, run, entry.length) == (ssize_t)entry.length
         && covers_inbox_blocks(run, entry.length, entry.crc,
                                COVER_LARGE_SIDE, &parts);
    if (fd >= 0)
        close(fd);
    if (!ok)
    {
        /* read and decoded instead, as without one */
        inbox_refused++;
        return 0;
    }
    /* each of the others only at the size it's made of the large picture
       here */
    const int w = parts.width[COVERS_BLOCK_LARGE];
    const int h = parts.height[COVERS_BLOCK_LARGE];
    int fw = FRAME_SIDE, fh = FRAME_SIDE;
    covers_fit(w, h, &fw, &fh);
    int sw, sh;
    covers_slide_fit(fw, fh, ITUNESDB_SLIDE_SIZE, &sw, &sh);
    struct prepared pre = { NULL, NULL, COVER_COLOUR_UNKNOWN };
    if (parts.pixels[COVERS_BLOCK_FRAME]
        && parts.width[COVERS_BLOCK_FRAME] == fw
        && parts.height[COVERS_BLOCK_FRAME] == fh)
        pre.frame = parts.pixels[COVERS_BLOCK_FRAME];
    if (parts.pixels[COVERS_BLOCK_SLIDE]
        && parts.width[COVERS_BLOCK_SLIDE] == sw
        && parts.height[COVERS_BLOCK_SLIDE] == sh
        && inbox_colour_kind == COVERS_COLOUR_KIND)
    {
        pre.slide = parts.pixels[COVERS_BLOCK_SLIDE];
        pre.colour = entry.colour;
    }
    struct bitmap bm = { .width = w, .height = h,
                         .data = (unsigned char *)parts.pixels[COVERS_BLOCK_LARGE] };
    /* said first, as before a decode: one that crashes the making twice is
       given up */
    itunesdb_song_making(song);
    const int put = song_put(song, &bm, ITUNESDB_COVER_SHOWN, &pre);
    if (put <= 0)
        return put;
    inbox_taken++;
    /* where its row's cover is, by the slot it's in, for the lists' covers
       made after the making */
    const int k = song_find(song->path);
    const uint32_t slot = k >= 0 ? COVERS_SLOT_OF(song_records()[k].word) : 0;
    if (slot && parts.pixels[COVERS_BLOCK_ROW])
    {
        core_pin(inbox_handle);
        struct covers_inbox_entry *t =
            (struct covers_inbox_entry *)core_get_data(inbox_handle) + at;
        t->unused = slot;
        t->length = entry.offset + parts.at[COVERS_BLOCK_ROW];
        core_unpin(inbox_handle);
    }
    return 1;
}

static int cmp_inbox_row(const void *a, const void *b)
{
    const uint32_t x = ((const struct inbox_row *)a)->slot;
    const uint32_t y = ((const struct inbox_row *)b)->slot;
    return (x > y) - (x < y);
}

/* The making over: the rows' covers of the pictures it took, by their
   slots, in place of the entries, kept for the lists' covers made next */
static void inbox_making_over(bool spent)
{
    inbox_spent |= spent;
    /* none taken in by this making, or the rows of one before */
    if (inbox_handle <= 0 || inbox_rows)
        return;
    core_pin(inbox_handle);
    const struct covers_inbox_entry *e = core_get_data(inbox_handle);
    struct inbox_row *rows = core_get_data(inbox_handle);
    uint32_t n = 0;
    for (uint32_t i = 0; i < inbox_count; i++)
    {
        /* the row's place first: they overlap */
        const struct inbox_row row = { e[i].unused, e[i].length };
        if (row.slot)
            rows[n++] = row;
    }
    qsort(rows, n, sizeof(*rows), cmp_inbox_row);
    core_unpin(inbox_handle);
    inbox_rows = n;
    /* the rest let go of, for the steps of Updating Library after it */
    if (n)
        core_shrink(inbox_handle, core_get_data(inbox_handle),
                    n * sizeof(*rows));
    if (inbox_taken || inbox_refused || inbox_newer)
        health_note("covers: %d taken from the computer's, %d refused; %d "
                    "lookups of pictures in files later than it or of no "
                    "known time, by their sums only", inbox_taken,
                    inbox_refused, inbox_newer);
    if (!n)
        inbox_free();
}

bool itunesdb_inbox_row(const struct itunesdb_art *art, void *pixels,
                        int side)
{
    if (inbox_handle <= 0 || !inbox_rows || art->slide <= COVER_SLIDE_BASE
        || strcmp(art->path, COVERS_FRAMES))
        return false;
    const uint32_t slot = art->slide - COVER_SLIDE_BASE;
    core_pin(inbox_handle);
    const struct inbox_row *rows = core_get_data(inbox_handle);
    uint32_t lo = 0, hi = inbox_rows, offset = 0;
    while (lo < hi && !offset)
    {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (rows[mid].slot == slot)
            offset = rows[mid].offset;
        else if (rows[mid].slot < slot)
            lo = mid + 1;
        else
            hi = mid;
    }
    core_unpin(inbox_handle);
    if (!offset)
        return false;
    struct covers_inbox_block b;
    const ssize_t bytes = side * side * sizeof(fb_data);
    const int fd = open(COVERS_INBOX, O_RDONLY);
    const bool ok = fd >= 0 && lseek(fd, offset, SEEK_SET) == (off_t)offset
                    && read(fd, &b, sizeof(b)) == sizeof(b)
                    && b.magic == COVERS_BLOCK_MAGIC
                    && b.kind == COVERS_BLOCK_ROW && b.width == side
                    && b.height == side
                    && read(fd, pixels, bytes) == bytes
                    && b.crc == covers_crc32(COVERS_CRC32_INIT, pixels, bytes);
    if (fd >= 0)
        close(fd);
    inbox_rows_taken += ok;
    return ok;
}

/** The scan's tags of the songs it read, for the making after it **/

struct scan_song {
    uint64_t path;      /* itunesdb_path_hash() */
    uint32_t mtime;
    struct itunesdb_scan_tags tags;
    uint32_t unused;
};
/* the most kept: the making reads the tags of any others */
#define SCAN_SONGS_MAX 32768
static int scan_handle, scan_count, scan_room;
static bool scan_sorted;
static bool scan_full;      /* no memory to keep more: none asked for again */

void itunesdb_scan_forget(void)
{
    if (scan_handle > 0)
        core_free(scan_handle);
    scan_handle = scan_count = scan_room = 0;
    scan_full = false;
}

void itunesdb_scan_song(const char *path, uint32_t mtime,
                        const struct mp3entry *id3)
{
    /* nothing taken while music plays */
    if ((audio_status() & AUDIO_STATUS_PLAY) || scan_full
        || scan_count >= SCAN_SONGS_MAX)
        return;
    /* An ID3v2 tag whose Sort fields the scan read, which playback doesn't:
       they take room in id3v2buf, where the picture's frame is read too, and
       may change where it's found, or whether, which the song's check is
       then compared with at playback. Without them the tag is read as
       playback reads it. */
    if (id3->id3v2len
        && (id3->artist_sort || id3->albumartist_sort || id3->album_sort
            || id3->title_sort || id3->composer_sort))
        return;
    if (scan_count >= scan_room)
    {
        const int room = MIN(scan_room ? 2 * scan_room : 1024, SCAN_SONGS_MAX);
        const int handle = db_alloc(room * sizeof(struct scan_song));
        if (handle <= 0)
        {
            /* those after it read by the making */
            scan_full = true;
            return;
        }
        if (scan_handle > 0)
        {
            memcpy(core_get_data(handle), core_get_data(scan_handle),
                   scan_count * sizeof(struct scan_song));
            core_free(scan_handle);
        }
        scan_handle = handle;
        scan_room = room;
    }
    struct scan_song *e = (struct scan_song *)core_get_data(scan_handle)
                          + scan_count++;
    e->path = itunesdb_path_hash(path);
    e->mtime = mtime;
    e->tags.filesize = id3->filesize;
    const bool own = id3->has_embedded_albumart;
    e->tags.pic_pos = own ? id3->albumart.pos : 0;
    e->tags.pic_size = own ? id3->albumart.size : 0;
    e->tags.pic_type = own ? (int32_t)id3->albumart.type : -1;
    e->unused = 0;
    scan_sorted = false;
}

static int cmp_scan_song(const void *a, const void *b)
{
    const uint64_t x = ((const struct scan_song *)a)->path;
    const uint64_t y = ((const struct scan_song *)b)->path;
    return (x > y) - (x < y);
}

bool itunesdb_scan_tags(uint64_t path, uint32_t mtime,
                        struct itunesdb_scan_tags *tags)
{
    if (scan_handle <= 0)
        return false;
    core_pin(scan_handle);
    struct scan_song *e = core_get_data(scan_handle);
    if (!scan_sorted)
    {
        qsort(e, scan_count, sizeof(*e), cmp_scan_song);
        scan_sorted = true;
    }
    int lo = 0, hi = scan_count;
    bool found = false;
    while (lo < hi && !found)
    {
        const int mid = lo + (hi - lo) / 2;
        if (e[mid].path == path)
        {
            /* of the file as the database has it */
            found = e[mid].mtime == mtime;
            if (found)
                *tags = e[mid].tags;
            break;
        }
        if (e[mid].path < path)
            lo = mid + 1;
        else
            hi = mid;
    }
    core_unpin(scan_handle);
    return found;
}

bool itunesdb_song_usable(uint64_t song)
{
    if (songs_handle <= 0)
        return false;
    songs_sort();
    const int k = song_find(song);
    return k >= 0 && COVERS_SLOT_OF(song_records()[k].word)
           && !(COVERS_FLAGS_OF(song_records()[k].word) & SONG_STALE);
}

int itunesdb_cover_of_song(uint64_t key, uint32_t stamp, uint64_t song)
{
    if (work_handle <= 0 || songs_handle <= 0)
        return 0;
    songs_sort();
    const int k = song_find(song);
    uint32_t slot = 0;
    int reason = ITUNESDB_COVER_SHOWN;
    struct covers_picture p = { 0 };
    if (k >= 0)
    {
        const struct song_record *r = &song_records()[k];
        /* none of a song changed since, which a making with songs left out
           for room didn't make again */
        const bool stale = COVERS_FLAGS_OF(r->word) & SONG_STALE;
        slot = stale ? 0 : COVERS_SLOT_OF(r->word);
        reason = stale ? ITUNESDB_COVER_SHOWN : COVERS_REASON_OF(r->word);
        const struct covers_picture *t =
            slot ? covers_picture_place(pictures(), pictures_room, r->crc,
                                        r->adler, r->pic_size) : NULL;
        if (t && t->size && t->slot == slot)
            p = *t;
        else
            slot = 0;
    }
    int i = work_find(key);
    if (i < 0)
    {
        if (work_count >= work_room)
            return 0;
        i = work_count++;
        memset(&work_entries()[i], 0, sizeof(struct cover_work));
        work_entries()[i].e.key = key;
    }
    struct cover_entry *e = &work_entries()[i].e;
    work_entries()[i].seen = true;
    const bool was_unshown = !e->slot && e->width == COVER_UNSHOWN;
    struct cover_entry now = *e;
    now.stamp = stamp;
    now.image = 0;
    now.slot = slot;
    if (slot)
    {
        now.width = p.width;
        now.height = p.height;
        now.colour = p.colour;
    }
    else
    {
        now.width = reason ? COVER_UNSHOWN : 0;
        now.height = reason;
        now.colour = COVER_COLOUR_UNKNOWN;
    }
    if (now.stamp != e->stamp || now.slot != e->slot || now.width != e->width
        || now.height != e->height || now.colour != e->colour
        || now.image != e->image)
    {
        if (e->slot != slot)
        {
            release_slot(e->slot);
            slot_take(work_used(), slot);
        }
        *e = now;
        work_changed = true;
    }
    if (slot)
        return 1;
    return reason && !was_unshown ? 2 : reason ? 1 : 0;
}

static int cmp_picture_slot(const void *a, const void *b)
{
    return (int)((const struct covers_picture *)a)->slot
           - (int)((const struct covers_picture *)b)->slot;
}

/* the pictures the songs to be written have, at the start of the table by
   their slots, a slot once: how many */
static int pictures_of_songs(const struct song_record *songs, int n)
{
    /* a slot has 15 bits at most (COVERS_SLOTS_MAX): the 16th marks it */
    const uint16_t mark = 0x8000;
    struct covers_picture *t = pictures();
    for (int i = 0; i < n; i++)
    {
        if (!COVERS_SLOT_OF(songs[i].word))
            continue;
        struct covers_picture *p =
            covers_picture_place(t, pictures_room, songs[i].crc,
                                 songs[i].adler, songs[i].pic_size);
        if (p && p->size && p->slot == COVERS_SLOT_OF(songs[i].word))
            p->slot |= mark;
    }
    int count = 0;
    for (size_t i = 0; i < pictures_room; i++)
    {
        if (t[i].size && (t[i].slot & mark))
        {
            struct covers_picture p = t[i];
            p.slot &= ~mark;
            t[count++] = p;
        }
    }
    /* by slot, each once: songs of one picture share a slot */
    qsort(t, count, sizeof(*t), cmp_picture_slot);
    int unique = 0;
    for (int i = 0; i < count; i++)
    {
        if (!unique || t[unique - 1].slot != t[i].slot)
            t[unique++] = t[i];
    }
    return unique;
}

/* The entries made of the songs written through slide_slot, art_lock held:
   a write() each was 13 000 and more */
struct songs_out {
    int fd;
    size_t used;
    bool ok;
};

static void songs_out_flush(struct songs_out *o)
{
    if (o->ok && o->used)
        o->ok = write(o->fd, slide_slot, o->used) == (ssize_t)o->used;
    o->used = 0;
}

static void songs_out_put(struct songs_out *o, const void *data, size_t size,
                          uint32_t *sum)
{
    *sum = index_hash(data, size, *sum);
    if (o->used + size > sizeof(slide_slot))
        songs_out_flush(o);
    memcpy(slide_slot + o->used, data, size);
    o->used += size;
}

/* The songs' file, whole, with the parts kept in memory made of n songs, by
   their paths, whose pictures are first in the pictures' table then (or
   none) */
static bool write_songs(const struct song_record *songs, int n,
                        uint32_t covers, bool whole)
{
    struct songs_header h = { .magic = SONGS_MAGIC, .version = SONGS_VERSION,
                              .count = n, .covers = covers, .whole = whole };
    int pics = songs ? pictures_of_songs(songs, n) : 0;
    const int fd = whole_file_open(COVERS_SONGS);
    if (fd < 0)
        return false;
    bool ok = write(fd, &h, sizeof(h)) == sizeof(h);
    const ssize_t size = n * sizeof(*songs);
    h.sum = n ? index_hash(songs, size, 2166136261u) : 2166136261u;
    ok = ok && (!n || write(fd, songs, size) == size);
    h.shown_sum = 2166136261u;
    lock_art();
    struct songs_out out = { fd, 0, ok };
    for (int i = 0; i < n; i++)
    {
        if (!COVERS_SLOT_OF(songs[i].word) && !COVERS_REASON_OF(songs[i].word))
            continue;
        const struct covers_song_ram e = { songs[i].path, songs[i].word,
                                           songs[i].check };
        songs_out_put(&out, &e, sizeof(e), &h.shown_sum);
        h.shown++;
    }
    const struct covers_picture *t = songs ? pictures() : NULL;
    for (int i = 0; i < pics; i++)
    {
        const struct song_picture p = { t[i].slot, t[i].large, t[i].width,
                                        t[i].height, t[i].colour, t[i].crc };
        songs_out_put(&out, &p, sizeof(p), &h.shown_sum);
        h.pictures++;
    }
    songs_out_flush(&out);
    mutex_unlock(&art_lock);
    ok = out.ok && lseek(fd, 0, SEEK_SET) == 0
         && write(fd, &h, sizeof(h)) == sizeof(h);
    return whole_file_close(fd, COVERS_SONGS, ok);
}

static int cmp_cover(const void *a, const void *b)
{
    uint64_t x = ((const struct cover_work *)a)->e.key;
    uint64_t y = ((const struct cover_work *)b)->e.key;
    return (x > y) - (x < y);
}

void itunesdb_covers_end(int how)
{
    if (work_handle <= 0)
    {
        songs_work_free();
        return;
    }
    /* the computer's done with after every song was looked at, or for want
       of room, which they'd take; kept for the rest of one cut short */
    inbox_making_over(how != COVERS_CUT || covers_no_room);
    const bool whole = how != COVERS_CUT;
    core_pin(work_handle);
    struct cover_work *w = work_entries();
    /* the albums not seen are gone, when every one was looked at */
    int n = 0;
    for (int i = 0; i < work_count; i++)
    {
        if (w[i].seen || how != COVERS_WHOLE)
            w[n++] = w[i];
        else
            work_changed = true;
    }
    qsort(w, n, sizeof(*w), cmp_cover);
    /* the entries alone, packed down where they are: the first ones overlap */
    struct cover_entry *entries = (struct cover_entry *)w;
    for (int i = 0; i < n; i++)
        memmove(&entries[i], &w[i].e, sizeof(entries[i]));
    /* written only for a change, or to say where a making stopped, or after
       a making stopped: an update of Finder's songs alone left it as it was */
    const bool write = work_changed || !whole || itunesdb_covers_stale();
    bool written = false;
    if (write)
        written = write_covers(entries, n, whole);
    const uint32_t sum = written ? covers_entries_sum(entries, n) : covers_sum;
    core_unpin(work_handle);
    core_free(work_handle);
    work_handle = 0;

    /* the songs: those not seen are gone after a whole making, and the rest
       is kept; written for a change, a making cut short or covers.dat
       written */
    bool songs_written = false;
    if (songs_handle > 0)
    {
        songs_sort();
        core_pin(songs_handle);
        struct song_record *r = song_records();
        int kept = 0;
        for (int i = 0; i < songs_count; i++)
        {
            const bool seen = COVERS_FLAGS_OF(r[i].word) & SONG_SEEN;
            r[i].word &= ~((uint32_t)SONG_SEEN << COVERS_FLAGS_SHIFT);
            if (seen || how != COVERS_WHOLE)
                r[kept++] = r[i];
            else
                songs_changed = true;
        }
        if (songs_changed || written || !whole || !songs_whole())
        {
            core_pin(pictures_handle);
            songs_written = write_songs(r, kept, sum, whole);
            core_unpin(pictures_handle);
        }
        core_unpin(songs_handle);
    }
    songs_work_free();
    /* before Finder's, which takes its table in memory again */
    itunesdb_scan_forget();
#ifdef ITUNESDB_FINDER_LARGE
    finder_work_end(how);
#endif
    if (whole)
        covers_images_made();
    /* the new tables in memory, for the tracks played from now on, and none
       of the copies of the covers before */
    if (write || songs_written)
    {
        forget_index(&covers_index);
        covers_load();
        skin_forget_db_art();
#ifdef HAVE_TAGCACHE
        album_header_forget_cover();
        list_covers_forget();
#endif
    }
}

/* the large picture's place of the song's picture in our slot, 0 for none */
static uint32_t slot_large(uint32_t slot, struct song_picture *p)
{
    return songs_trusted && slot && slot <= COVERS_SLOTS_MAX
           && song_picture_of(slot, p) && p->large <= COVERS_LARGE_MAX
           ? p->large : 0;
}

/* The large picture of our slot, if its file has it and goes with the
   table: its record, with the frame's size it was made for */
static bool find_large_slot(uint32_t slot, int frame_w, int frame_h,
                            struct itunesdb_large_art *large)
{
    struct song_picture p;
    const uint32_t at = slot_large(slot, &p);
    if (!at)
        return false;
    int lw = COVER_LARGE_SIDE, lh = COVER_LARGE_SIDE;
    covers_fit(frame_w, frame_h, &lw, &lh);
    struct large_slot head;
    const off_t pos = (off_t)(at - 1) * COVER_LARGE_SLOT;
    const int fd = open(COVERS_LARGE, O_RDONLY);
    const bool ok = fd >= 0 && lseek(fd, pos, SEEK_SET) == pos
                    && read(fd, &head, sizeof(head)) == sizeof(head)
                    && head.magic == COVER_LARGE_MAGIC
                    && head.width == lw && head.height == lh
                    /* the song's picture's, as the table has its sum */
                    && head.crc == p.crc;
    if (fd >= 0)
        close(fd);
    if (!ok)
        return false;
    strmemccpy(large->path, COVERS_LARGE, sizeof(large->path));
    large->offset = pos + sizeof(head);
    large->pitch = lw;
    large->x = large->y = 0;
    large->width = lw;
    large->height = lh;
    return true;
}

/* the slot of our cover at art, 0 for one of Finder's */
static uint32_t art_slot(const struct itunesdb_art *art)
{
    if (strcmp(art->path, COVERS_FRAMES) || art->offset % COVER_SLOT)
        return 0;
    return art->offset / COVER_SLOT + 1;
}

#ifdef ITUNESDB_FINDER_LARGE
/** The pane's pictures of the songs Finder has a cover of, on the iPod photo
    (itunesdb.h): each such song's record, by its path, in COVERS_FINDER; its
    picture in a place of COVERS_LARGE, which the songs' own pictures share.
    Made in the covers' making; read for the pane by itunesdb_find_large_of(),
    only while COVERS_FINDER goes with Finder's artwork as it is. **/
#define COVERS_FINDER   ROCKBOX_DIR "/covers_finder.dat"
#define FINDER_MAGIC    0x46434252 /* "RBCF" */
#define FINDER_VERSION  1
#define FINDER_SEEN     1   /* in the making: known, or made */

struct finder_header {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t sum;       /* of the records */
    /* finder_artwork_stamp() of the artwork every record was made or checked
       with: 0 for records not checked since it changed, which aren't shown
       until a making checks them by their frames' sums */
    uint32_t art;
    uint32_t whole;     /* 0 for a making cut short: the rest at the next */
};

struct finder_record {
    uint64_t path;      /* itunesdb_path_hash() */
    uint32_t mtime;     /* of the file, as the database has it; 0 to be made */
    uint32_t check;     /* covers_file_check() */
    uint32_t art;       /* Finder's frame of it: finder_art_stamp() */
    uint32_t frame_crc; /* of that frame's pixels */
    uint32_t pic_size, crc, adler; /* the song's own picture, 0 for none */
    uint16_t large;     /* its pane's picture in COVERS_LARGE, from 1; 0 for
                           none, as of a picture not Finder's cover */
    uint16_t flags;
};

/* in memory, of the songs with a pane's picture: sorted by path */
struct finder_ram {
    uint64_t path;
    uint32_t art, crc, adler, frame_crc;
    uint16_t large;
    uint16_t unused;
};

/* a picture of the making: by its bytes' sums and its Finder's frame, the
   place made of it or 0 for none */
struct finder_pic {
    uint32_t crc, adler, size, frame_crc;
    uint32_t large;
    bool used;
};

static int finder_handle, finder_pics_handle;
static int finder_old, finder_count, finder_room, finder_pics_room;
static bool finder_changed;
/* Finder's artwork changed since the file was made: each record's frame is
   summed again before it's kept */
static bool finder_art_changed;
static uint32_t finder_file_art; /* the file's finder_header.art */
static uint32_t finder_making_art; /* finder_artwork_stamp() as it began */
static bool finder_file_whole;   /* and whole */
/* the last frame summed, by its stamp */
static uint32_t finder_sum_art, finder_sum_crc;
static bool finder_sum_known;

static struct index finder_index = { 0, 0, sizeof(struct finder_ram) };

/* Finder's frame: its file, place and size */
static uint32_t finder_art_stamp(const struct itunesdb_art *art)
{
    uint32_t h = index_hash(art->path, strlen(art->path), 2166136261u);
    const uint32_t v[3] = { art->offset, (uint32_t)art->width,
                            (uint32_t)art->height };
    h = index_hash(v, sizeof(v), h);
    return h ? h : 1;
}

/* Finder's artwork as it is: the imported index's sum (art_sum(), the
   frames' places), and the size and time of ArtworkDB and of each ithmb file,
   which change when Finder writes a cover again in its place; 0 without
   artwork */
static uint32_t finder_artwork_stamp(void)
{
    const uint32_t art = art_sum();
    if (!art)
        return 0;
    uint32_t h = index_hash(&art, sizeof(art), 2166136261u);
    DIR *dir = opendir(ARTWORK_DIR);
    if (!dir)
        return h ? h : 1;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL)
    {
        const char *name = (const char *)e->d_name;
        const size_t len = strlen(name);
        if (strcasecmp(name, "ArtworkDB")
            && (len < 6 || strcasecmp(name + len - 6, ".ithmb")))
            continue;
        const struct dirinfo info = dir_get_info(dir, e);
        const uint32_t v[2] = { (uint32_t)info.size, (uint32_t)info.mtime };
        h = index_hash(name, len, h);
        h = index_hash(v, sizeof(v), h);
    }
    closedir(dir);
    return h ? h : 1;
}

/* the sum of Finder's frame's pixels: false without the frame */
static bool finder_frame_crc(const struct itunesdb_art *art, uint32_t *crc)
{
    const uint32_t stamp = finder_art_stamp(art);
    if (finder_sum_known && finder_sum_art == stamp)
    {
        *crc = finder_sum_crc;
        return true;
    }
    lock_art();
    const fb_data *frame = get_frame(art);
    if (frame)
        *crc = covers_crc32(COVERS_CRC32_INIT, frame,
                            art->width * art->height * sizeof(fb_data));
    mutex_unlock(&art_lock);
    if (!frame)
        return false;
    finder_sum_art = stamp;
    finder_sum_crc = *crc;
    finder_sum_known = true;
    return true;
}

/* the file's header, if it's of this version and whole; false without */
static bool finder_header_read(int fd, struct finder_header *h)
{
    return read(fd, h, sizeof(*h)) == sizeof(*h) && h->magic == FINDER_MAGIC
           && h->version == FINDER_VERSION && h->count <= COUNT_MAX
           && ffilesize(fd) == (off_t)(sizeof(*h)
                                       + h->count * sizeof(struct finder_record));
}

/* not cut short, and made for Finder's artwork as it is, or said not
   checked since it changed (art 0: finder_said_whole(), or a making not of
   every song after it changed, COVERS_PARTLY): a making with nothing to make
   isn't asked for at every boot */
static bool finder_whole(void)
{
    struct finder_header h;
    const int fd = open(COVERS_FINDER, O_RDONLY);
    const bool whole = fd >= 0 && finder_header_read(fd, &h) && h.whole
                       && (!h.art || h.art == finder_artwork_stamp());
    if (fd >= 0)
        close(fd);
    return whole;
}

/* the records into memory, at most room, if their sum is as said: how many,
   -1 without */
static int finder_records_read(int fd, const struct finder_header *h,
                               struct finder_record *r, int room)
{
    if ((int)h->count > room)
        return -1;
    const ssize_t size = h->count * sizeof(*r);
    if (size && read(fd, r, size) != size)
        return -1;
    return index_hash(r, size, 2166136261u) == h->sum ? (int)h->count : -1;
}

/* The songs with a pane's picture into memory, for the pane: with the
   covers, at boot and after a making; none unless every record goes with
   Finder's artwork as it is */
static void finder_load(void)
{
    forget_index(&finder_index);
    struct finder_header h;
    const int fd = open(COVERS_FINDER, O_RDONLY);
    if (fd < 0)
        return;
    if (!finder_header_read(fd, &h) || !h.art
        || h.art != finder_artwork_stamp() || !h.count)
    {
        close(fd);
        return;
    }
    /* a record at a time through a small buffer, those with a picture kept */
    int shown = 0;
    struct finder_record r[16];
    uint32_t sum = 2166136261u;
    for (uint32_t i = 0; i < h.count; )
    {
        const int n = MIN(16, (int)(h.count - i));
        if (read(fd, r, n * sizeof(r[0])) != (ssize_t)(n * sizeof(r[0])))
        {
            close(fd);
            return;
        }
        sum = index_hash(r, n * sizeof(r[0]), sum);
        for (int k = 0; k < n; k++)
            shown += r[k].large && r[k].large <= COVERS_LARGE_MAX;
        i += n;
    }
    const int handle = shown && sum == h.sum
        ? db_alloc(shown * sizeof(struct finder_ram)) : 0;
    if (handle <= 0 || lseek(fd, sizeof(h), SEEK_SET) != sizeof(h))
    {
        if (handle > 0)
            core_free(handle);
        close(fd);
        return;
    }
    core_pin(handle);
    struct finder_ram *e = core_get_data(handle);
    int at = 0;
    bool ok = true;
    for (uint32_t i = 0; ok && i < h.count; )
    {
        const int n = MIN(16, (int)(h.count - i));
        ok = read(fd, r, n * sizeof(r[0])) == (ssize_t)(n * sizeof(r[0]));
        for (int k = 0; ok && k < n; k++)
        {
            if (!r[k].large || r[k].large > COVERS_LARGE_MAX || at >= shown)
                continue;
            e[at].path = r[k].path;
            e[at].art = r[k].art;
            e[at].crc = r[k].crc;
            e[at].adler = r[k].adler;
            e[at].frame_crc = r[k].frame_crc;
            e[at].large = r[k].large;
            e[at].unused = 0;
            at++;
        }
        i += n;
    }
    core_unpin(handle);
    close(fd);
    if (!ok || at != shown)
    {
        core_free(handle);
        return;
    }
    finder_index.handle = handle;
    finder_index.count = shown;
}

/* the pane's picture of Finder's song at path, whose cover is art: the
   record's, if the song still has that frame of Finder's, its pixels as they
   were (a cover written again in its place with the file's size and time as
   they were isn't seen by finder_artwork_stamp()), and the picture's place
   is still its picture's. Reads the disk, as the pane asks only while it
   spins: Finder's frame and the place's header. */
static bool finder_find_large(const char *path, const struct itunesdb_art *art,
                              struct itunesdb_large_art *large)
{
    struct finder_ram e;
    uint32_t frame_crc;
    /* read now, not a making's sum of it */
    finder_sum_known = false;
    if (!find_in_index(&finder_index, itunesdb_path_hash(path), &e)
        || e.art != finder_art_stamp(art)
        || !finder_frame_crc(art, &frame_crc) || frame_crc != e.frame_crc)
        return false;
    int lw = COVER_LARGE_SIDE, lh = COVER_LARGE_SIDE;
    covers_fit(art->width, art->height, &lw, &lh);
    struct large_slot head;
    const off_t pos = (off_t)(e.large - 1) * COVER_LARGE_SLOT;
    const int fd = open(COVERS_LARGE, O_RDONLY);
    const bool ok = fd >= 0 && lseek(fd, pos, SEEK_SET) == pos
                    && read(fd, &head, sizeof(head)) == sizeof(head)
                    && head.magic == COVER_LARGE_MAGIC
                    && head.width == lw && head.height == lh
                    && head.crc == e.crc && head.adler == e.adler;
    if (fd >= 0)
        close(fd);
    if (!ok)
        return false;
    strmemccpy(large->path, COVERS_LARGE, sizeof(large->path));
    large->offset = pos + sizeof(head);
    large->pitch = lw;
    large->x = large->y = 0;
    large->width = lw;
    large->height = lh;
    return true;
}

static bool finder_has_large(const char *path, const struct itunesdb_art *art)
{
    struct finder_ram e;
    return find_in_index(&finder_index, itunesdb_path_hash(path), &e)
           && e.art == finder_art_stamp(art);
}

static struct finder_record *finder_records(void)
{
    return core_get_data(finder_handle);
}

static struct finder_pic *finder_pics(void)
{
    return core_get_data(finder_pics_handle);
}

/* the picture's place in the table, by its sums and Finder's frame: its
   entry, or a free one to put it in; NULL with the table full */
static struct finder_pic *finder_pic_place(uint32_t crc, uint32_t adler,
                                           uint32_t size, uint32_t frame_crc)
{
    struct finder_pic *t = finder_pics();
    const uint32_t start = (crc ^ adler ^ frame_crc) % finder_pics_room;
    for (int n = 0; n < finder_pics_room; n++)
    {
        struct finder_pic *p = &t[(start + n) % finder_pics_room];
        if (!p->used || (p->crc == crc && p->adler == adler && p->size == size
                         && p->frame_crc == frame_crc))
            return p;
    }
    return NULL;
}

static void finder_pic_note(const struct finder_record *r)
{
    if (!r->pic_size || finder_pics_handle <= 0)
        return;
    struct finder_pic *p = finder_pic_place(r->crc, r->adler, r->pic_size,
                                            r->frame_crc);
    if (p && !p->used)
    {
        *p = (struct finder_pic){ r->crc, r->adler, r->pic_size, r->frame_crc,
                                  r->large, true };
    }
}

/* the record of path: those of the file in order, then those put since */
static int finder_find(uint64_t path)
{
    const struct finder_record *r = finder_records();
    int lo = 0, hi = finder_old - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (r[mid].path == path)
            return mid;
        if (r[mid].path < path)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    for (int i = finder_old; i < finder_count; i++)
    {
        if (r[i].path == path)
            return i;
    }
    return -1;
}

static void finder_work_free(void)
{
    if (finder_handle > 0)
        core_free(finder_handle);
    if (finder_pics_handle > 0)
        core_free(finder_pics_handle);
    finder_handle = finder_pics_handle = 0;
}

static bool finder_write(const struct finder_record *r, int n, uint32_t art,
                         bool whole);

/* The file's records into the making, their places taken in COVERS_LARGE,
   room for the songs Finder has covers of; without memory none to make, and
   the file said whole as itunesdb_covers_skip() says it */
static void finder_work_load(void)
{
    finder_work_free();
    finder_sum_known = false;
    finder_making_art = finder_artwork_stamp();
    struct finder_header h;
    const int fd = open(COVERS_FINDER, O_RDONLY);
    const bool valid = fd >= 0 && finder_header_read(fd, &h);
    finder_old = valid ? (int)h.count : 0;
    finder_file_art = valid ? h.art : 0;
    finder_file_whole = valid && h.whole;
    /* the file's songs and every one Finder has a cover of, at most */
    finder_room = finder_old
                  + (art_index.handle > 0 ? (int)art_index.count : 0) + 1;
    finder_handle = db_alloc(finder_room * sizeof(struct finder_record));
    /* a picture for each record at most, the table a third free */
    finder_pics_room = finder_room + finder_room / 2 + 1;
    finder_pics_handle = finder_handle > 0
        ? db_alloc(finder_pics_room * sizeof(struct finder_pic)) : 0;
    if (finder_handle <= 0 || finder_pics_handle <= 0)
    {
        if (fd >= 0)
            close(fd);
        finder_work_free();
        if (!finder_whole())
            finder_said_whole();
        return;
    }
    memset(finder_pics(), 0, finder_pics_room * sizeof(struct finder_pic));
    core_pin(finder_handle);
    if (finder_old && finder_records_read(fd, &h, finder_records(),
                                          finder_room) < 0)
        finder_old = 0; /* damaged: all made again */
    core_unpin(finder_handle);
    if (fd >= 0)
        close(fd);
    finder_count = finder_old;
    finder_changed = !valid;
    finder_art_changed = finder_file_art != finder_making_art;
    /* how far COVERS_LARGE goes */
    const int lfd = open(COVERS_LARGE, O_RDONLY);
    const off_t large_end = lfd >= 0 ? ffilesize(lfd) : 0;
    if (lfd >= 0)
        close(lfd);
    struct finder_record *r = finder_records();
    uint8_t *used = large_used();
    bool gone = false;
    for (int i = 0; i < finder_old; i++)
    {
        r[i].flags = 0;
        /* Made again: COVERS_LARGE gone with the songs' file not trusted; a
           place past the bounds or past the file's end; or one the songs'
           pictures have, as after a reset in the making that took the
           places of a file before (only those are marked yet) */
        const uint32_t l = r[i].large;
        const bool songs = l && l <= COVERS_LARGE_MAX
                           && (used[l / 8] & (1 << (l % 8)));
        const bool past = l && l <= COVERS_LARGE_MAX
                          && (off_t)(l - 1) * COVER_LARGE_SLOT
                             + (off_t)sizeof(struct large_slot) > large_end;
        /* a place given out again in this making, the file's record of it
           to be gone before any is: not one the songs' file has */
        gone |= l && (finder_large_gone || (past && !songs));
        if (l && (finder_large_gone || l > COVERS_LARGE_MAX || past || songs))
        {
            r[i].large = 0;
            r[i].mtime = 0;
            finder_changed = true;
        }
    }
    /* then theirs, not given to another in this making, as the songs' are
       not; several songs of one picture have one place */
    for (int i = 0; i < finder_old; i++)
    {
        if (r[i].large)
            used[r[i].large / 8] |= 1 << (r[i].large % 8);
    }
    /* With COVERS_LARGE gone, or places past its end, the file on disk
       without those places at once, and said cut short: they're given out
       again in this making, and after a reset before its end the next one,
       trusting the songs' file then, would have taken the old records' as
       theirs again */
    if (gone)
    {
        core_pin(finder_handle);
        /* else none, rather than the old ones */
        if (!finder_write(finder_records(), finder_old, finder_file_art, false))
            remove(COVERS_FINDER);
        core_unpin(finder_handle);
        finder_file_whole = false;
    }
}

int itunesdb_finder_large_known(uint64_t path, uint32_t mtime,
                                const struct itunesdb_art *art)
{
    /* none to make: no room for the making, or Finder writes its own
       larger frames, which it doesn't for an iPod photo's description */
    if (finder_handle <= 0 || itunesdb_has_large_art())
        return 1;
    const int i = finder_find(path);
    if (i < 0)
        return 0;
    struct finder_record *r = &finder_records()[i];
    if (!r->mtime || r->mtime != mtime || r->art != finder_art_stamp(art))
    {
        /* changed: no picture until it's made again, which a making cut
           short may leave for the next */
        if (r->large || r->mtime)
        {
            r->large = 0;
            r->mtime = 0;
            finder_changed = true;
        }
        return 0;
    }
    if (finder_art_changed)
    {
        /* Finder's artwork written again: the frame as it was made with */
        uint32_t crc;
        if (!finder_frame_crc(art, &crc) || crc != r->frame_crc)
        {
            r->large = 0;
            r->mtime = 0;
            finder_changed = true;
            return 0;
        }
    }
    r->flags |= FINDER_SEEN;
    finder_pic_note(r);
    return 1;
}

/* the song's record as given, with its frame's sum and its picture's place */
static bool finder_record_put(const struct itunesdb_finder_song *song,
                              const struct itunesdb_art *art,
                              uint32_t frame_crc, uint32_t large)
{
    int i = finder_find(song->path);
    if (i < 0)
    {
        if (finder_count >= finder_room)
            return false;
        i = finder_count++;
    }
    struct finder_record *r = &finder_records()[i];
    *r = (struct finder_record){
        .path = song->path, .mtime = song->mtime, .check = song->check,
        .art = finder_art_stamp(art), .frame_crc = frame_crc,
        .pic_size = song->pic_size, .crc = song->crc, .adler = song->adler,
        .large = large, .flags = FINDER_SEEN };
    finder_changed = true;
    return true;
}

int itunesdb_finder_large_picture(const struct itunesdb_finder_song *song,
                                  const struct itunesdb_art *art)
{
    if (finder_handle <= 0 || work_handle <= 0)
        return 0;
    uint32_t frame_crc = 0;
    /* a decode of it cut short twice, by a crash or a hang: none */
    if (songs_prev_given_up && song->path == songs_prev
        && song->mtime == songs_prev_mtime)
    {
        finder_frame_crc(art, &frame_crc);
        finder_record_put(song, art, frame_crc, 0);
        return -1;
    }
    if (!song->pic_size || !finder_frame_crc(art, &frame_crc))
        return 0;
    const struct finder_pic *p = finder_pic_place(song->crc, song->adler,
                                                  song->pic_size, frame_crc);
    if (!p || !p->used)
        return 0;
    return finder_record_put(song, art, frame_crc, p->large) ? 1 : 0;
}

/* The picture, w by h, shrunk to lw by lh into the place large of
   COVERS_LARGE, its header last; with art_lock held, for slide_slot. bm's
   pixels are changed. */
static bool finder_write_large(struct bitmap *bm, int lw, int lh,
                               uint32_t crc, uint32_t adler, uint32_t large)
{
    if (lw != bm->width || lh != bm->height)
    {
        cpu_boost(true);
        covers_area_scale((const fb_data *)bm->data, bm->width, bm->height,
                          (fb_data *)bm->data, lw, lh, (uint32_t *)slide_slot);
        cpu_boost(false);
    }
    const struct large_slot head = { COVER_LARGE_MAGIC, lw, lh, crc, adler };
    static const struct large_slot none;
    const off_t pos = (off_t)(large - 1) * COVER_LARGE_SLOT;
    const int fd = open(COVERS_LARGE, O_RDWR | O_CREAT, 0666);
    bool ok = fd >= 0 && write_slot(fd, pos, &none, sizeof(none))
              && write_slot(fd, pos + sizeof(head), bm->data,
                            lw * lh * sizeof(fb_data))
              && lseek(fd, pos, SEEK_SET) == pos
              && write(fd, &head, sizeof(head)) == sizeof(head);
    if (fd >= 0)
        ok = close(fd) == 0 && ok;
    return ok;
}

int itunesdb_finder_large_put(const struct itunesdb_finder_song *song,
                              const struct itunesdb_art *art,
                              struct bitmap *bm)
{
    if (finder_handle <= 0 || work_handle <= 0)
        return 0;
    if (!work_marked)
        mark_making(0, 0);
    uint32_t frame_crc;
    uint32_t large = 0;
    int lw = COVER_LARGE_SIDE, lh = COVER_LARGE_SIDE;
    covers_fit(art->width, art->height, &lw, &lh);
    lock_art();
    const fb_data *frame = get_frame(art);
    bool same = false;
    if (frame)
    {
        frame_crc = covers_crc32(COVERS_CRC32_INIT, frame,
                                 art->width * art->height * sizeof(fb_data));
        /* Finder's cover: the picture as large as the pane takes it, and
           like Finder's frame of it */
        same = bm && song->pic_size && bm->width <= COVER_LARGE_SIDE
               && bm->height <= COVER_LARGE_SIDE && lw <= bm->width
               && lh <= bm->height
               && frames_agree((const fb_data *)bm->data, bm->width,
                               bm->height, frame, art->width, art->height);
    }
    if (!frame)
    {
        /* not put: tried again at the next making */
        mutex_unlock(&art_lock);
        return 0;
    }
    if (same)
    {
        large = free_large();
        if (!large || !covers_room_at(0, large)
            || !finder_write_large(bm, lw, lh, song->crc, song->adler, large))
        {
            mutex_unlock(&art_lock);
            drop_large(large);
            return -1;
        }
    }
    mutex_unlock(&art_lock);
    if (song->pic_size)
    {
        struct finder_pic *p = finder_pic_place(song->crc, song->adler,
                                                song->pic_size, frame_crc);
        if (p && !p->used)
            *p = (struct finder_pic){ song->crc, song->adler, song->pic_size,
                                      frame_crc, large, true };
    }
    return finder_record_put(song, art, frame_crc, large) ? 1 : 0;
}

static int cmp_finder_record(const void *a, const void *b)
{
    const uint64_t x = ((const struct finder_record *)a)->path;
    const uint64_t y = ((const struct finder_record *)b)->path;
    return (x > y) - (x < y);
}

/* the file, of n records */
static bool finder_write(const struct finder_record *r, int n, uint32_t art,
                         bool whole)
{
    struct finder_header h = { .magic = FINDER_MAGIC,
                               .version = FINDER_VERSION, .count = n,
                               .art = art, .whole = whole };
    h.sum = index_hash(r, n * sizeof(*r), 2166136261u);
    const int fd = whole_file_open(COVERS_FINDER);
    if (fd < 0)
        return false;
    const ssize_t size = n * sizeof(*r);
    const bool ok = write(fd, &h, sizeof(h)) == sizeof(h)
                    && (!n || write(fd, r, size) == size);
    return whole_file_close(fd, COVERS_FINDER, ok);
}

/* None made for this database (itunesdb_covers_skip(), or no memory for
   the making): the file said whole, its records kept, rather than a making
   at every boot. Of Finder's artwork as it is, they're shown as before;
   else not until a making checks them by their frames' sums (art 0), which
   decodes nothing for a frame unchanged. Without a file, or for one damaged
   or unreadable, an empty one. */
static void finder_said_whole(void)
{
    struct finder_header h;
    const uint32_t stamp = finder_artwork_stamp();
    const int fd = open(COVERS_FINDER, O_RDWR);
    const bool kept = fd >= 0 && finder_header_read(fd, &h);
    if (kept)
    {
        h.whole = 1;
        if (h.art != stamp)
            h.art = 0;
        if (lseek(fd, 0, SEEK_SET) == 0)
            write(fd, &h, sizeof(h));
    }
    if (fd >= 0)
        close(fd);
    if (!kept)
        finder_write(NULL, 0, stamp, true);
}

/* The making's records into the file, by path: those not seen are gone after
   a whole making, kept otherwise. Said to be of Finder's artwork as it is
   unless some weren't checked against it. */
static void finder_work_end(int how)
{
    if (finder_handle <= 0)
    {
        finder_work_free();
        return;
    }
    core_pin(finder_handle);
    struct finder_record *r = finder_records();
    int kept = 0;
    bool all_seen = true;
    for (int i = 0; i < finder_count; i++)
    {
        const bool seen = r[i].flags & FINDER_SEEN;
        r[i].flags = 0;
        all_seen = all_seen && seen;
        if (seen || how != COVERS_WHOLE)
            r[kept++] = r[i];
        else
            finder_changed = true;
    }
    qsort(r, kept, sizeof(*r), cmp_finder_record);
    /* records not seen after Finder's artwork changed weren't checked */
    const uint32_t art = finder_art_changed && !all_seen && how != COVERS_WHOLE
                         ? 0 : finder_making_art;
    const bool whole = how != COVERS_CUT;
    if (finder_changed || art != finder_file_art || whole != finder_file_whole)
        finder_write(r, kept, art, whole);
    core_unpin(finder_handle);
    finder_work_free();
    finder_load();
}
#endif /* ITUNESDB_FINDER_LARGE */

bool itunesdb_find_large_of(const char *path, const struct itunesdb_art *art,
                            struct itunesdb_large_art *large)
{
    const uint32_t slot = art_slot(art);
#ifdef ITUNESDB_FINDER_LARGE
    /* Finder's, or the one made of its song's own picture */
    if (!slot)
        return itunesdb_find_large_art(path, large)
               || finder_find_large(path, art, large);
#endif
    return slot ? find_large_slot(slot, art->width, art->height, large)
                : itunesdb_find_large_art(path, large);
}

bool itunesdb_has_large_of(const char *path, const struct itunesdb_art *art)
{
    struct song_picture p;
    if (art_slot(art))
        return slot_large(art_slot(art), &p) != 0;
#ifdef ITUNESDB_FINDER_LARGE
    if (finder_has_large(path, art))
        return true;
#endif
    struct itunesdb_art finders;
    return itunesdb_has_large_art() && itunesdb_find_art(path, &finders)
           && finders.offset == art->offset && !strcmp(finders.path, art->path);
}
#else /* !ITUNESDB_HAVE_ART */
bool itunesdb_import_kept_art(void)
{
    return false;
}

static unsigned char *notes_room(size_t *size)
{
    (void)size;
    return NULL;
}

static void notes_room_done(void)
{
}
#endif /* ITUNESDB_HAVE_ART */

/* Songs Finder wrote again under another path, as it does a few hundred a
   week and all of them after the iPod is restored: by their dbid, which stays.
   The dbid and path hash of each song of the last sync are kept, sorted by
   dbid; one of this sync with that dbid and another path is noted in the
   moved list by its old path, for the playlists made on the iPod to keep it. */
#define IDS_FILE    ROCKBOX_DIR "/itunesdb_ids.dat"
#define IDS_MAGIC   0x49445331  /* "IDS1" */
struct id_pair {        /* as the file has them */
    uint64_t dbid;
    uint64_t hash;
};
struct id_track {       /* and with the track, to find its path */
    uint64_t dbid;
    uint64_t hash;
    uint32_t track;
};

static int cmp_id_track(const void *a, const void *b)
{
    return cmp_u64(&((const struct id_track *)a)->dbid,
                   &((const struct id_track *)b)->dbid);
}

static int cmp_moved(const void *a, const void *b)
{
    return cmp_u64(&((const struct itunesdb_moved *)a)->old_hash,
                   &((const struct itunesdb_moved *)b)->old_hash);
}

/* The moved list by old path, to be looked up in on the disk: written whole
   in its place, the unsorted one kept until then (FW_KEEP), as with the
   ids its songs can't be told again after this sync */
static void sort_moved_list(void)
{
    int fd = open(ITUNESDB_MOVED_LIST, O_RDONLY);
    if (fd < 0)
        return;
    const off_t size = ffilesize(fd);
    const int n = size / sizeof(struct itunesdb_moved);
    const int handle = n > 1 ? db_alloc(n * sizeof(struct itunesdb_moved)) : 0;
    if (handle <= 0)
    {
        close(fd);
        return;
    }
    core_pin(handle);
    struct itunesdb_moved *m = core_get_data(handle);
    const size_t bytes = n * sizeof(*m);
    const bool whole = read(fd, m, bytes) == (ssize_t)bytes;
    close(fd);
    if (whole)
    {
        qsort(m, n, sizeof(*m), cmp_moved);
        struct fw w;
        fw_open(&w, ITUNESDB_MOVED_LIST, FW_KEEP, NULL, 0);
        fw_put(&w, m, bytes);
        fw_close(&w, true);
    }
    core_unpin(handle);
    core_free(handle);
}

bool itunesdb_moved_to(uint64_t old_hash, char *path, size_t size)
{
    const int fd = open(ITUNESDB_MOVED_LIST, O_RDONLY);
    if (fd < 0)
        return false;
    struct itunesdb_moved m;
    int lo = 0, hi = ffilesize(fd) / sizeof(m) - 1;
    bool found = false;
    while (lo <= hi && !found)
    {
        const int mid = (lo + hi) / 2;
        if (lseek(fd, (off_t)mid * sizeof(m), SEEK_SET) < 0
            || read(fd, &m, sizeof(m)) != sizeof(m))
            break;
        if (m.old_hash == old_hash)
            found = true;
        else if (m.old_hash < old_hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    close(fd);
    if (found)
    {
        m.path[sizeof(m.path) - 1] = '\0';
        strmemccpy(path, m.path, size);
    }
    return found;
}

int itunesdb_same_song(const char *path)
{
    if (meta.songs_handle <= 0)
        return -1;
    const uint64_t hash = itunesdb_path_hash(path);
    const uint64_t *h = core_get_data(meta.songs_handle);
    if (known_hash(h + meta.songs_same, meta.songs_other, hash))
        return 0;
    return known_hash(h, meta.songs_same, hash) ? 1 : -1;
}

/* the paths of the last sync's songs, as note_moved_songs() reads them, by
   whether each still has the same song: the same from the front of room for
   capacity, the others from its back, until same_songs_end() */
static int same_songs_capacity;

static void same_songs_begin(uint32_t capacity)
{
    if (meta.songs_handle > 0)
        core_free(meta.songs_handle);
    meta.songs_handle = meta.songs_same = meta.songs_other = 0;
    same_songs_capacity = 0;
    if (capacity == 0 || capacity > 2 * COUNT_MAX)
        return;
    meta.songs_handle = db_alloc(capacity * sizeof(uint64_t));
    if (meta.songs_handle <= 0)
    {
        meta.songs_handle = 0;
        return;
    }
    core_pin(meta.songs_handle);
    same_songs_capacity = capacity;
}

static void same_songs_note(uint64_t hash, bool same)
{
    if (meta.songs_handle <= 0
        || meta.songs_same + meta.songs_other >= same_songs_capacity)
        return;
    uint64_t *h = core_get_data(meta.songs_handle);
    if (same)
        h[meta.songs_same++] = hash;
    else
        h[same_songs_capacity - ++meta.songs_other] = hash;
}

/* the two sorted, the others right after the same, the room given back; or
   none, if the last sync's weren't all read */
static void same_songs_end(bool whole)
{
    if (meta.songs_handle <= 0)
        return;
    if (!whole)
    {
        core_unpin(meta.songs_handle);
        core_free(meta.songs_handle);
        meta.songs_handle = meta.songs_same = meta.songs_other = 0;
        return;
    }
    uint64_t *h = core_get_data(meta.songs_handle);
    memmove(h + meta.songs_same, h + same_songs_capacity - meta.songs_other,
            meta.songs_other * sizeof(*h));
    qsort_yielding(h, meta.songs_same, sizeof(*h), cmp_u64);
    qsort_yielding(h + meta.songs_same, meta.songs_other, sizeof(*h), cmp_u64);
    core_unpin(meta.songs_handle);
    core_shrink(meta.songs_handle, h,
                MAX(meta.songs_same + meta.songs_other, 1) * sizeof(*h));
}

static void note_moved_songs(void)
{
    const int n = db.count;
    if (n <= 0)
        return;
    /* with a block behind them for the last sync's, if there's room */
    size_t block = READ_BLOCK;
    int handle = db_alloc(n * sizeof(struct id_track) + block);
    if (handle <= 0)
    {
        block = 0;
        handle = db_alloc(n * sizeof(struct id_track));
    }
    if (handle <= 0)
        return;
    core_pin(handle);
    struct id_track *ids = core_get_data(handle);
    int count = 0;
    for (int i = 0; i < n; i++)
    {
        if (db.tracks[i].dbid && db.tracks[i].path)
        {
            ids[count].dbid = db.tracks[i].dbid;
            ids[count].hash = db.tracks[i].st.hash;
            ids[count].track = i;
            count++;
        }
    }
    qsort_yielding(ids, count, sizeof(*ids), cmp_id_track);

    /* the last sync's, a block at a time: of the same dbid and another hash,
       moved */
    struct itunesdb_moved moved[16];
    int n_moved = 0, moved_fd = -1;
    bool changed = true, any_moved = false;
    int fd = open(IDS_FILE, O_RDONLY);
    uint32_t head[2];
    if (fd >= 0 && read(fd, head, sizeof(head)) == sizeof(head)
        && head[0] == IDS_MAGIC)
    {
        changed = head[1] != (uint32_t)count;
        same_songs_begin(head[1]);
        struct id_pair small[64];
        struct id_pair *old = block ? (struct id_pair *)(ids + n) : small;
        const size_t old_size = block ? block : sizeof(small);
        int at = 0;
        ssize_t got;
        uint32_t pairs = 0;
        while ((got = read(fd, old, old_size)) > 0)
        {
            const int k = got / sizeof(*old);
            pairs += k;
            for (int j = 0; j < k; j++)
            {
                while (at < count && ids[at].dbid < old[j].dbid)
                {
                    changed = true; /* one new */
                    at++;
                }
                if (at >= count || ids[at].dbid != old[j].dbid)
                {
                    changed = true; /* gone: its path has another or none */
                    same_songs_note(old[j].hash, false);
                    continue;
                }
                same_songs_note(old[j].hash, ids[at].hash == old[j].hash);
                if (ids[at].hash == old[j].hash)
                    continue;
                changed = true;
                /* a path longer than a record holds isn't noted: cut, it
                   would be another */
                const char *now = db.pool + db.tracks[ids[at].track].path;
                if (strlen(now) >= sizeof(moved[n_moved].path))
                    continue;
                moved[n_moved].old_hash = old[j].hash;
                strmemccpy(moved[n_moved].path, now, sizeof(moved[n_moved].path));
                if (++n_moved == (int)ARRAYLEN(moved))
                {
                    if (moved_fd < 0)
                        moved_fd = open(ITUNESDB_MOVED_LIST,
                                        O_WRONLY | O_CREAT | O_APPEND, 0666);
                    if (moved_fd >= 0)
                        write(moved_fd, moved, sizeof(moved));
                    any_moved = true;
                    n_moved = 0;
                }
            }
            yield();
        }
        same_songs_end(got == 0 && pairs == head[1]);
    }
    if (fd >= 0)
        close(fd);
    if (n_moved)
    {
        if (moved_fd < 0)
            moved_fd = open(ITUNESDB_MOVED_LIST, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (moved_fd >= 0)
            write(moved_fd, moved, n_moved * sizeof(*moved));
        any_moved = true;
    }
    if (moved_fd >= 0)
        close(moved_fd);
    if (any_moved)
        sort_moved_list();
    /* kept over a cut: the songs a sync moves are found by it, which the
       next sync's can't tell again */
    if (changed && out_open(IDS_FILE, FW_KEEP))
    {
        head[0] = IDS_MAGIC;
        head[1] = count;
        fw_put(&out, head, sizeof(head));
        for (int i = 0; i < count; i++)
        {
            const struct id_pair pair = { ids[i].dbid, ids[i].hash };
            fw_put(&out, &pair, sizeof(pair));
        }
        fw_close(&out, true);
    }
    core_unpin(handle);
    core_free(handle);
}

/* Whether a "<file>.new" a cut left may take the name. Only the marks' save
   of an older firmware wrote a .new of its own, in place, which a cut may
   have left short; every other's is fw_close()'s, whole. */
static bool kept_new_whole(const char *new)
{
    if (strcasecmp(new, FLAGS_FILE ".new"))
        return true;
    struct flags_header hdr;
    const int fd = open(new, O_RDONLY);
    if (fd < 0)
        return false;
    const bool whole = read(fd, &hdr, sizeof(hdr)) == sizeof(hdr)
        && hdr.magic == FLAGS_MAGIC && hdr.count <= COUNT_MAX
        && ffilesize(fd) == (off_t)(sizeof(hdr)
                                    + hdr.count * sizeof(struct flags_entry));
    close(fd);
    return whole;
}

void itunesdb_files_boot(void)
{
    static const char *const kept[] = {
        STATE_FILE, PENDING_FILE, UNSENT_FILE, FLAGS_FILE, NOTES_FILE,
        IDS_FILE, ITUNESDB_MOVED_LIST, OTG_FILE
    };
    fw_recover_files(ROCKBOX_DIR, kept, ARRAYLEN(kept), kept_new_whole);
    lp_files_boot();
}

int itunesdb_import_tracks(bool synced, volatile int *done, volatile int *total)
{
    /* a sync may have changed the name */
    device_name_read = false;

    int count = 0;
    read_done = done;
    read_total = total;
    if (synced)
    {
        otg_cleanup();
        metadata_begin();
    }
    else
        itunesdb_forget_metadata();
    /* the pass made again over the same iTunesDB (itunesdb_pass_due()) has
       nothing new from the Mac: no place of its to take over the iPod's.
       One without a sync over an iTunesDB whose sync is still to be taken
       in, as Rebuild Synced Covers' then, takes them as that sync's would:
       its import after keeps them, and would see no bookmark moved */
    flags_import_begin(itunesdb_changed());
    remove(GAINS_FILE);
#ifdef ITUNESDB_HAVE_ART
    /* the old covers point into ithmb files Finder has rewritten; their
       slides are kept where they are the same */
    uint32_t old_slides_count;
    int old_slides_handle = read_old_slides(&old_slides_count);
    struct art_sum old_art;
    art_file_sum(&old_art);
    art_kept = false;
    kept_commitid = tagcache_get_commitid();
    bool art_placed = false;
    forget_art();
    /* with artwork there, an index of no version until the new one is in
       place: cut short, the next boot's import makes it
       (itunesdb_load_tracks()), also when nothing else is left to make */
    if (file_exists(ARTWORKDB_FILE))
        itunesdb_art_due();
    else
        remove(ART_FILE);
    remove(LARGE_FILE);
#endif
    sort_fields_begin();
    notes_begin();
    bool loaded = db_load();
    sort_fields_end(loaded);
    notes_end(loaded);
    /* not over a file that couldn't be read: it still knows the tracks this
       import couldn't tell changed, and an import with the database in
       memory compares them all anyway */
    if (loaded && synced && meta.digesting
        && (!meta.tags_unread || meta.compare_all))
        write_tags_pending();
    else if (synced)
        remove(TAGS_PENDING);
    metadata_end();
    if (loaded)
    {
        count = db.count;
        write_flags();
        write_gains();
        if (synced)
        {
            write_playlists(playlists_list, playlists_end, false);
            write_pending();
            /* only from all of iTunesDB: a song missing would seem gone */
            if (db.complete)
                note_moved_songs();
        }
#ifdef ITUNESDB_HAVE_ART
        /* last, as it reads ArtworkDB through the same cache */
        art_placed = write_art_index(old_slides_handle, old_slides_count,
                                     &old_art);
#endif
    }
#ifdef ITUNESDB_HAVE_ART
    if (old_slides_handle > 0)
        core_free(old_slides_handle);
#ifdef HAVE_TAGCACHE
    if (!art_placed)
        list_covers_forget();
#endif
    /* no index of this artwork: none due either */
    if (!art_placed)
        remove(ART_FILE);
#endif
    /* no iTunesDB: no flags, as before. One that couldn't be read leaves
       the old ones, with the marks made on the iPod, for the next try */
    if (!loaded && !itunesdb_exists())
        remove(FLAGS_FILE);
    flags_import_end();
    db_unload();
    itunesdb_load_tracks();
    read_done = read_total = NULL;
    return count;
}

struct playlists_failed {
    uint32_t size, mtime;   /* of iTunesDB, as db_stamp() has them */
};

bool itunesdb_playlists_due(void)
{
    uint32_t size, mtime;
    if (!db_stamp(&size, &mtime))
        return false;
    int fd = open(PLAYLISTS_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    char line[16];
    bool due = read_line(fd, line, sizeof(line)) <= 0
               || strncmp(line, "version ", 8)
               || strtoul(line + 8, NULL, 10) < PLAYLISTS_VERSION;
    close(fd);
    /* not over this iTunesDB again if its playlists couldn't be read */
    struct playlists_failed failed;
    if (due && (fd = open(PLAYLISTS_FAILED, O_RDONLY)) >= 0)
    {
        due = read(fd, &failed, sizeof(failed)) != sizeof(failed)
              || failed.size != size || failed.mtime != mtime;
        close(fd);
    }
    return due;
}

/* tries of the pass with no memory for iTunesDB's songs, a quarter of a
   second apart: the boot's menus hold all that's free only while
   tagtree_init() reads them */
#define PLAYLISTS_TRIES 5

void itunesdb_import_playlists(volatile int *done, volatile int *total)
{
    read_done = done;
    read_total = total;
    /* the songs read for their paths: none of their notes go to a file,
       which notes_end() of an import before may have left ok but closed */
    notes_out.ok = false;
    bool readable;
    for (int tries = 1; ; tries++)
    {
        memory_short = false;
        readable = db_load() && write_playlists(playlists_list, playlists_end,
                                                true);
        db_unload();
        if (!memory_short || tries == PLAYLISTS_TRIES)
            break;
        sleep(HZ / 4);
    }
    read_done = read_total = NULL;
    /* as it would be the next time: noted, and left for the next sync, also
       when the songs found no memory at any try. A list that couldn't be
       written, as with the disk full, is tried again at the next boot, and so
       are the playlists with no room for the old list or the folders */
    struct playlists_failed failed;
    if (!readable && db_stamp(&failed.size, &failed.mtime))
    {
        whole_file_write(PLAYLISTS_FAILED, &failed, sizeof(failed));
    }
}

bool itunesdb_load_tracks(void)
{
    struct flags_header hdr = { 0 };
    uint32_t size, mtime;
#ifdef ITUNESDB_HAVE_ART
    /* the covers of songs outside Finder's folder, whatever iTunesDB is */
    covers_load();
#endif
    load_index(&flags_index, FLAGS_FILE, &hdr, sizeof(hdr),
               FLAGS_MAGIC, FLAGS_VERSION);
    /* version 3's, without the compilations: its flags and marks meanwhile,
       until the import it has at once writes this version's. Not saved:
       the mark of one changed is taken by the import from memory */
    bool flags_old = false;
    if (flags_index.handle <= 0)
    {
        load_index(&flags_index, FLAGS_FILE, &hdr, sizeof(hdr),
                   FLAGS_MAGIC, FLAGS_VERSION_3);
        flags_old = flags_index.handle > 0;
    }
    if (flags_index.handle > 0)
        flags_hdr = hdr;
    /* what an older firmware didn't note */
    if (flags_old || !itunesdb_gains_ready())
        return false;
#ifdef ITUNESDB_HAVE_ART
    load_index(&art_index, ART_FILE, &index_hdr, sizeof(index_hdr),
               ART_MAGIC, ART_VERSION);
    load_slides_index();
    load_colours();
    /* an index of artwork that is gone, as set aside and the import after
       it cut: the import now, which has none then */
    if (itunesdb_art_orphaned())
        return false;
    /* the artwork set aside for the computer to make anew goes once covers
       of the new are in the index; here, as a cut after the import's index
       has no import at the next boot */
    synced_covers_imported(art_index.handle > 0 && index_hdr.count > 0);
    /* the larger frames go with this index or not at all; an index without
       their file, as one an older firmware made: the import once more */
    if (art_index.handle > 0 && !load_large())
        return false;
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
    /* and the Sort fields, which the covers don't wait for */
    if (!itunesdb_sort_ready())
        return false;
    /* and the notes of podcasts and audiobooks */
    if (!itunesdb_notes_ready())
        return false;
    if (flags_index.handle > 0 || !db_stamp(&size, &mtime))
        return true;
    /* none with flags, from this iTunesDB? */
    return hdr.magic == FLAGS_MAGIC && hdr.version == FLAGS_VERSION
           && hdr.db_size == size && hdr.db_mtime == mtime;
}
