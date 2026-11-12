/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 by Miika Pekkarinen
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

/*
 *                    TagCache API
 *
 *       ----------x---------x------------------x-----
 *                 |         |                  |              External
 * +---------------x-------+ |       TagCache   |              Libraries
 * | Modification routines | |         Core     |
 * +-x---------x-----------+ |                  |
 *   | (R/W)   |             |                  |           |
 *   |  +------x-------------x-+  +-------------x-----+     |
 *   |  |                      x==x Filters & clauses |     |
 *   |  | Search routines      |  +-------------------+     |
 *   |  |                      x============================x DirCache
 *   |  +-x--------------------+                            | (optional)
 *   |    | (R)                                             |
 *   |    | +-------------------------------+  +---------+  |
 *   |    | | DB Commit (sort,unique,index) |  |         |  |
 *   |    | +-x--------------------------x--+  | Control |  |
 *   |    |   | (R/W)                    | (R) | Thread  |  |
 *   |    |   | +----------------------+ |     |         |  |
 *   |    |   | | TagCache DB Builder  | |     +---------+  |
 *   |    |   | +-x-------------x------+ |                  |
 *   |    |   |   | (R)         | (W)    |                  |
 *   |    |   |   |          +--x--------x---------+        |
 *   |    |   |   |          | Temporary Commit DB |        |
 *   |    |   |   |          +---------------------+        |
 * +-x----x-------x--+                                      |
 * | TagCache RAM DB x==\(W) +-----------------+            |
 * +-----------------+   \===x                 |            |
 *   |    |   |   |      (R) |  Ram DB Loader  x============x DirCache
 * +-x----x---x---x---+   /==x                 |            | (optional)
 * | Tagcache Disk DB x==/   +-----------------+            |
 * +------------------+                                     |
 *
 */

#if !defined(PLUGIN)

/*#define LOGF_ENABLE*/
/*#define LOGF_CLAUSES define to enable logf clause matching (LOGF_ENABLE req'd) */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#ifdef APPLICATION
#include <unistd.h> /* readlink() */
#include <limits.h> /* PATH_MAX */
#endif
#include "config.h"
#include "ata_idle_notify.h"
#include "thread.h"
#include "kernel.h"
#include "system.h"
#include "logf.h"
#include "string-extra.h"
#include "usb.h"
#include "metadata.h"
#include "tagcache.h"
#include "yesno.h"
#include "core_alloc.h"
#include "crc32.h"
#include "misc.h"
#include "settings.h"
#include "dir.h"
#include "pathfuncs.h"
#include "debug.h"
#include "dircache.h"
#include "errno.h"
#include "sortkey.h"
#ifdef __PCTOOL__
#define qsort_yielding qsort
#endif

#ifndef __PCTOOL__
#include "lang.h"
#include "eeprom_settings.h"
#include "itunesdb.h"
#include "audio.h"
#ifndef PLUGIN
#include "music_search.h"
#include "smart_playlists.h"
#include "library_playlists.h"
#include "timefuncs.h"
#endif
#endif
#define USR_CANCEL false
#else/*!defined(PLUGIN)*/
#define USR_CANCEL (tc_stat.commit_delayed == true)
#endif /*!defined(PLUGIN)*/
#if defined(PLUGIN) || defined(__PCTOOL__)
#define health_note(...) do { } while (0) /* the firmware's own log */
#else
#include "health_log.h"
#endif
/*
 * Define this to support non-native endian tagcache files.
 * Databases are always written in native endian so this is
 * basically only necessary to support databases generated
 * by the PC database tool.
 *
 * Adds around 0.5-1.0k of code.
 */
#define TAGCACHE_SUPPORT_FOREIGN_ENDIAN

/* Allow a little drift to the filename ordering (should not be too high/low). */
#define POS_HISTORY_COUNT 4

/* RockWheel's own format, "RWD" and its number. Raise it for any layout
   change (the asserts below) or a change in what a tag means (they can't
   see it). */
#define TAGCACHE_FORMAT 3
#define TAGCACHE_MAGIC  (0x52574400 | TAGCACHE_FORMAT)
/* older layouts whose statistics are restored: RockWheel up to 1.3, Rockbox */
#define TAGCACHE_MAGIC_ROCKWHEEL 0x54434811
#define TAGCACHE_MAGIC_ROCKBOX   0x54434810

/* Dump store/restore header version 'TCSxx'. */
#define TAGCACHE_STATEFILE_MAGIC 0x54435301

/* How much to allocate extra space for ramcache. */
#define TAGCACHE_RESERVE 32768

/*
 * Define how long one entry must be at least (longer -> less memory at commit).
 * Must be at least 4 bytes in length for correct alignment.
 */
#define TAGFILE_ENTRY_CHUNK_LENGTH   8

/* Used to guess the necessary buffer size at commit. */
#define TAGFILE_ENTRY_AVG_LENGTH   16

/* Max events in the internal tagcache command queue. */
#define TAGCACHE_COMMAND_QUEUE_LENGTH 32

/* Idle time before committing events in the command queue. */
#define TAGCACHE_COMMAND_QUEUE_COMMIT_DELAY  HZ*2

/* Dont commit database_tmp data. */
#define TAGCACHE_FILE_NOCOMMIT  "database_commit.ignore"

/* Temporary database containing new tags to be committed to the main db. */
#define TAGCACHE_FILE_TEMP       "database_tmp.tcd"

/* The main database master index and numeric data. */
#define TAGCACHE_FILE_MASTER     "database_idx.tcd"

/* The main database string data. */
#define TAGCACHE_FILE_INDEX      "database_%d.tcd"

/* A commit writes the master index and the sorted tag files anew under
   these names, and puts them in place of the old ones once all are done. */
#define TAGCACHE_FILE_MASTER_NEW "database_idx.new"
#define TAGCACHE_FILE_INDEX_NEW  "database_%d.new"

/* How to undo a commit a reboot cut short, or to finish it. */
#define TAGCACHE_FILE_JOURNAL    "database_journal.tcd"

/* ASCII dumpfile of the DB contents. */
#define TAGCACHE_FILE_CHANGELOG  "database_changelog.txt"

/* Serialized DB. */
#define TAGCACHE_STATEFILE       "database_state.tcd"

/* The Sort fields and the order's mark are the firmware's: the commit's
   plugin and the PC's database tool sort by the names alone, and leave the
   firmware to sort again once */
#if !defined(PLUGIN) && !defined(__PCTOOL__)
#define TAGCACHE_SORT_FIELDS

/* The rules and the Sort fields the tag files were sorted by at a commit: a
   database sorted otherwise, as by an older firmware, is sorted again once
   (order_stale()) */
#define TAGCACHE_FILE_ORDER      "database_order.tcd"
#define TAGCACHE_FILE_ORDER_NEW  "database_order.new"
#define ORDER_MAGIC              0x52424f52 /* "RBOR" */
/* The Sort fields of the songs outside Finder's folder, from their files'
   tags (tagcache.h): those the database has, its next ones from a commit
   until it's done, and those of the songs a scan found, until committed */
#define TAGCACHE_FILE_SORT       "database_sort.tcd"
#define TAGCACHE_FILE_SORT_NEW   "database_sort.new"
#define TAGCACHE_FILE_SORT_SCAN  "database_sort_tmp.tcd"
struct order_mark {
    uint32_t magic;
    uint32_t rules;     /* SORTKEY_RULES_VERSION */
    int32_t commitid;   /* of the database sorted */
    uint32_t finder;    /* the crc of Finder's Sort fields it had */
};
/* After the mark, the letters Quick Scroll shows over lists of names
   (sortkey_initial()), as the tag files it marks have them: for each tag a
   list can be of, where each letter begins in its file, noted as the commit
   writes it, from what it sorted by, the Sort field if a name has one. A
   tag's ideographs, each a letter of its own, may run out of its room: from
   the last place kept on, the letter isn't known. */
#define INITIALS_PER_TAG 96
static const unsigned char initial_tags[] = {
    tag_artist, tag_album, tag_genre, tag_title, tag_composer, tag_albumartist,
    tag_virt_canonicalartist,
};
#define INITIAL_TAGS ARRAYLEN(initial_tags)
static struct initials {
    uint16_t count[INITIAL_TAGS];
    int32_t seek[INITIAL_TAGS][INITIALS_PER_TAG];
    uint16_t letter[INITIAL_TAGS][INITIALS_PER_TAG]; /* 0 for none */
} initials;
/* the commit id of the database they're of, -1 while they're of none */
static int32_t initials_commit = -1;

/* a database of another format, set aside for its statistics while this one
   is built; the mark goes last, after the import */
#define TAGCACHE_RESTORE
#define TAGCACHE_FILE_OLD_MASTER "database_old_idx.tcd"
#define TAGCACHE_FILE_OLD_NAMES  "database_old_4.tcd" /* tag_filename's */
#define TAGCACHE_FILE_OLD_MARK   "database_old.tcd"
#endif /* !PLUGIN && !__PCTOOL__ */

/* Flags */
#define FLAG_DELETED     0x0001  /* Entry has been removed from db */
#define FLAG_DIRCACHE    0x0002  /* Filename is a dircache pointer */
#define FLAG_DIRTYNUM    0x0004  /* Numeric data has been modified */
#define FLAG_TRKNUMGEN   0x0008  /* Track number has been generated  */
#define FLAG_RESURRECTED 0x0010  /* Statistics data has been resurrected */
/* temp file only: restored statistics; keep its commit id, even 0 */
#define FLAG_RESTORED    0x0020

#ifdef __PCTOOL__
#define yield() do { } while(0)
#define sim_sleep(timeout) do { } while(0)
#define do_timed_yield() do { } while(0)
static void db_log(const char *prefix, const char *msg);
#endif

#ifdef __PCTOOL__
#define DB_LOG(__prefix, __file) db_log(__prefix, __file)
#else
#define DB_LOG(...)
#endif

#ifndef __PCTOOL__
/* Tag Cache thread. */
static struct event_queue tagcache_queue SHAREDBSS_ATTR;
static long tagcache_stack[(DEFAULT_STACK_SIZE + 0x4000)/sizeof(long)];
static const char tagcache_thread_name[] = "tagcache";
#endif

/* Previous path when scanning directory tree recursively. */
static char curpath[TAGCACHE_BUFSZ];
/* Shared buffer for several build_index fns to reduce stack usage */
static char build_idx_buf[TAGCACHE_BUFSZ];
static const long build_idx_bufsz = sizeof(build_idx_buf);
/* Used when removing duplicates. */
static char *tempbuf;     /* Allocated when needed. */
static long tempbufidx;   /* Current location in buffer. */
static size_t tempbuf_size; /* Buffer size (TEMPBUF_SIZE). */
static long tempbuf_left; /* Buffer space left. */
static long tempbuf_pos;
#ifndef __PCTOOL__
static int tempbuf_handle;
#endif

#define SORTED_TAGS_COUNT 9
#define TAGCACHE_IS_UNIQUE(tag) TAGCACHE_TAG_IN(TAGCACHE_UNIQUE_TAGS, tag)
#define TAGCACHE_IS_NUMERIC_OR_NONUNIQUE(tag) \
    TAGCACHE_TAG_IN(TAGCACHE_NUMERIC_TAGS | ~TAGCACHE_UNIQUE_TAGS, tag)

/* Uniqued tags (we can use these tags with filters and conditional clauses). */
#define TAGCACHE_UNIQUE_TAGS (TAG_BIT(tag_artist) | TAG_BIT(tag_album) | \
    TAG_BIT(tag_genre) | TAG_BIT(tag_composer) | TAG_BIT(tag_comment) | \
    TAG_BIT(tag_albumartist) | TAG_BIT(tag_grouping) | \
    TAG_BIT(tag_virt_canonicalartist))

/* String presentation of the tags defined in tagcache.h. Must be in correct order! */
static const char * const tags_str[] = { "artist", "album", "genre", "title",
    "filename", "composer", "comment", "albumartist", "grouping", "year",
    "discnumber", "tracknumber", "canonicalartist", "bitrate", "length",
    "playcount", "rating", "playtime", "lastplayed", "commitid", "mtime",
    "lastelapsed", "lastoffset", "date", "dateadded"
#if !defined(LOGF_ENABLE) || !defined(LOGF_CLAUSES)
};
#define logf_clauses(...) do { } while(0)
#else /* strings for logf debugging */
    "tag_virt_basename", "tag_virt_length_min", "tag_virt_length_sec",
    "tag_virt_playtime_min", "tag_virt_playtime_sec",
    "tag_virt_entryage", "tag_virt_autoscore", "tag_virt_kind"
};
/* more debug strings */
static const char * const tag_type_str[] = {
    [clause_none] = "clause_none", [clause_is] = "clause_is",
    [clause_is_not] = "clause_is_not", [clause_gt] = "clause_gt",
    [clause_gteq] = "clause_gteq", [clause_lt] = "clause_lt",
    [clause_lteq] = "clause_lteq", [clause_contains] = "clause_contains",
    [clause_not_contains] = "clause_not_contains",
    [clause_begins_with] = "clause_begins_with",
    [clause_not_begins_with] = "clause_not_begins_with",
    [clause_ends_with] = "clause_ends_with",
    [clause_not_ends_with] = "clause_not_ends_with",
    [clause_oneof] = "clause_oneof",
    [clause_contains_oneof] = "clause_contains_oneof",
    [clause_begins_oneof] = "clause_begins_oneof",
    [clause_ends_oneof] = "clause_ends_oneof",
    [clause_not_oneof] = "clause_not_oneof",
    [clause_not_contains_oneof] = "clause_not_contains_oneof",
    [clause_not_begins_oneof] = "clause_not_begins_oneof",
    [clause_not_ends_oneof] = "clause_not_ends_oneof",
    [clause_logical_or] = "clause_logical_or"
 };
#define logf_clauses logf
#endif /* !defined(LOGF_ENABLE) || !defined(LOGF_CLAUSES) */

#if defined(PLUGIN)
char *itoa_buf(char *buf, size_t bufsz, long int i)
{
    snprintf(buf, bufsz, "%ld", i);
    return buf;
}
#endif

/* Status information of the tagcache. */
static struct tagcache_stat tc_stat;

/* Queue commands. */
enum tagcache_queue {
    Q_STOP_SCAN = 0,
    Q_START_SCAN,
    Q_IMPORT_CHANGELOG,
    Q_UPDATE,
    Q_REBUILD,
    Q_IMPORT_ITUNES,
    Q_SYNCED,
    Q_CHECK_OUTSIDE,
    Q_LOAD_RAM,

    /* Internal tagcache command queue. */
    CMD_UPDATE_MASTER_HEADER,
    CMD_UPDATE_NUMERIC,
};

struct tagcache_command_entry {
    int32_t command;
    int32_t idx_id;
    int32_t tag;
    int32_t data;
};

#ifndef __PCTOOL__
static struct tagcache_command_entry command_queue[TAGCACHE_COMMAND_QUEUE_LENGTH];
static volatile int command_queue_widx = 0;
static volatile int command_queue_ridx = 0;
static struct mutex command_queue_mutex SHAREDBSS_ATTR;
#endif

/* Tag database structures. */

/* Variable-length tag entry in tag files. */
struct tagfile_entry {
    int32_t tag_length;  /* Length of the data in bytes including '\0' */
    int32_t idx_id;      /* Corresponding entry location in index file of not unique tags */
    char tag_data[0];  /* Begin of the tag data */
};

/* Fixed-size tag entry in master db index. */
struct index_entry {
    int32_t tag_seek[TAG_COUNT]; /* Location of tag data or numeric tag data */
    int32_t flag;                /* Status flags */
};

/* Header is the same in every file. */
struct tagcache_header {
    int32_t magic;       /* Header version number */
    int32_t datasize;    /* Data size in bytes */
    int32_t entry_count; /* Number of entries in this file */
};

struct master_header {
    struct tagcache_header tch;
    int32_t serial; /* Increasing counting number */
    int32_t commitid; /* Number of commits so far */
    int32_t dirty;
    /* so other formats can read the statistics */
    int32_t header_size;
    int32_t entry_size;
};

static struct master_header current_tcmh;

/* a commit of no new entries goes through, to sort the database again */
static bool commit_resorting;


#ifdef HAVE_TC_RAMCACHE

#define TC_ALIGN_PTR(p, type, gap_out_p) \
    ({ typeof (p) __p = (p);                                  \
       typeof (p) __palgn = ALIGN_UP(__p, __alignof__(type)); \
       *(gap_out_p) = (char *)__palgn - (char *)__p;          \
       __palgn; })

#define IF_TCRCDC(...) IF_DIRCACHE(__VA_ARGS__)

#ifdef HAVE_DIRCACHE
#define tcrc_dcfrefs \
    ((struct dircache_fileref *)(tcramcache.hdr->tags[tag_filename] + \
                                  sizeof (struct tagcache_header)))
#endif /* HAVE_DIRCACHE */

/* Header is created when loading database to ram. */
struct ramcache_header {
    char *tags[TAG_COUNT];       /* Tag file content (dcfrefs if tag_filename) */
    int entry_count[TAG_COUNT];  /* Number of entries in the indices. */
    struct index_entry indices[0]; /* Master index file content */
};

#ifdef HAVE_EEPROM_SETTINGS
struct statefile_header {
    int32_t magic;                /* Statefile version number */
    struct master_header mh;      /* Header from the master index */
    struct ramcache_header *hdr;  /* Old load address of hdr for relocation */
    struct tagcache_stat tc_stat;
};
#endif /* HAVE_EEPROM_SETTINGS */

/* In-RAM ramcache structure (not persisted) */
static struct tcramcache
{
    struct ramcache_header *hdr;      /* allocated ramcache_header */
    int handle;                       /* buffer handle */
} tcramcache;

static inline void tcrc_buffer_lock(void)
{
    core_pin(tcramcache.handle);
}

static inline void tcrc_buffer_unlock(void)
{
    core_unpin(tcramcache.handle);
}

#else /* ndef HAVE_TC_RAMCACHE */

#define IF_TCRCDC(...)

#endif /* HAVE_TC_RAMCACHE */

/**
 * Full tag entries stored in a temporary file waiting
 * for commit to the cache. */
struct temp_file_entry {
    int32_t tag_offset[TAG_COUNT];
    int16_t tag_length[TAG_COUNT];
    int32_t flag;
    int32_t data_length;
};

_Static_assert(TAGCACHE_FORMAT == 3 && TAG_COUNT == 25
               && sizeof(struct index_entry) == 104
               && sizeof(struct master_header) == 32
               && sizeof(struct temp_file_entry) == 160,
               "the database's layout changed: raise TAGCACHE_FORMAT");
/* Fixed for good, so formats restore each other: the statistics' tag
   positions, the flags as the last word, the header's start, the names
   file's entries; a rating 0..10, a last play below 0x40000000 a serial, else
   a time; the date added in local seconds, 0 unknown. */
_Static_assert(tag_filename == 4 && tag_playcount == 15 && tag_rating == 16
               && tag_playtime == 17 && tag_lastplayed == 18
               && tag_commitid == 19 && tag_lastelapsed == 21
               && tag_lastoffset == 22 && tag_dateadded == 24
               && offsetof(struct index_entry, flag)
                  == sizeof(struct index_entry) - sizeof(int32_t)
               && offsetof(struct master_header, tch.entry_count) == 8
               && offsetof(struct master_header, serial) == 12
               && offsetof(struct master_header, commitid) == 16
               && offsetof(struct master_header, header_size) == 24
               && offsetof(struct master_header, entry_size) == 28
               && sizeof(struct tagcache_header) == 12
               && sizeof(struct tagfile_entry) == 8,
               "the places of the statistics are kept for good");
_Static_assert(TAG_COUNT_ALL <= 64, "a tag past the masks: make them wider");

#ifndef TAGCACHE_RESTORE
static inline bool restoring(void) { return false; }
static inline bool restore_load(void) { return true; }
static inline void restore_free(void) {}
static inline void restore_apply(const char *path, struct temp_file_entry *e)
{
    (void)path; (void)e;
}
static inline void restore_numbers(struct master_header *mh) { (void)mh; }
#endif

struct tempbuf_id_list {
    long id;
    struct tempbuf_id_list *next;
};

struct tempbuf_searchidx {
    long idx_id;
    char *str;
    int seek;
    struct tempbuf_id_list idlist;
    /* what it sorts by, worked out as it's put in: from str + key, past its
       article, or from its Sort field after it; and its group, <Untagged>'s
       first (GROUP_UNTAGGED) and then sortkey_group()'s, each one higher */
    uint16_t key;
    uint8_t group;
};
#define GROUP_UNTAGGED 0

/* The strings of a unique tag in the commit by their crc: each was checked
   against every other one before, which went with the square of the
   library's size. Heads of chains by the crc's low bits, and each entry's
   next, -1 ending them. */
static int32_t *crc_heads, *crc_next;
static uint32_t crc_mask;

/* Lookup buffer for fixing messed up index while after sorting. */
static long commit_entry_count;
/* The master file while a commit works on it, in memory at the end of the
   commit's buffer: rewritten on the disk for every sorted tag, it took the
   commit longer than anything else. The entries already in the database
   come first, then the new ones. */
static struct master_header commit_mh;
static struct index_entry *commit_idx;
static size_t commit_full_size; /* of the buffer, with it */
static long lookup_buffer_depth;
static struct tempbuf_searchidx **lookup;

/* Used when building the temporary file. */
static int cachefd = -1, filenametag_fd;

/* the check without a sync, of the songs copied outside Finder's folder:
   what is in iPod_Control is iTunesDB's, not looked through */
static bool scan_outside;
#ifndef __PCTOOL__
static struct search_roots_ll *scan_roots; /* the scan's, for the count */
static bool outside_counted;
static int outside_known; /* the songs outside Finder's folder known, still there */
static uint32_t outside_images; /* their folders' images, as a sum */
static bool outside_images_valid;
static void count_outside(void);
#endif

/* The tag files and the temporary file are written through this, a block
 * at a time: a write() for each part of each entry took the iPod as long
 * for a few bytes as for a few KB, most of a commit, and 2.8 ms a new file
 * in a scan */
static char tagfile_wbuf[8192];
static size_t tagfile_wbuf_used;

static bool tagfile_wbuf_flush(int fd)
{
    ssize_t used = tagfile_wbuf_used;
    tagfile_wbuf_used = 0;
    return used == 0 || write(fd, tagfile_wbuf, used) == used;
}

static bool tagfile_wbuf_put(int fd, const void *data, size_t len)
{
    if (tagfile_wbuf_used + len > sizeof(tagfile_wbuf)
        && !tagfile_wbuf_flush(fd))
        return false;
    if (len > sizeof(tagfile_wbuf))
        return write(fd, data, len) == (ssize_t)len;
    memcpy(tagfile_wbuf + tagfile_wbuf_used, data, len);
    tagfile_wbuf_used += len;
    return true;
}

/* an import, a check or a commit left for want of room on the disk, until
   tagcache_take_no_space() */
static volatile bool no_space;
/* the scan stopped for a failed write or no room: its files are removed */
static bool scan_failed;

static void scan_write_failed(void)
{
    if (!scan_failed)
        health_note("library scan stopped: write failed, errno %d", errno);
    if (errno == ENOSPC)
        no_space = true;
    scan_failed = true;
}

static void scan_put(const void *data, size_t len)
{
    if (!scan_failed && !tagfile_wbuf_put(cachefd, data, len))
        scan_write_failed();
}

static int total_entry_count = 0;
static int data_size = 0;
static int processed_dir_count;

/* Thread safe locking */
static volatile int write_lock;
static volatile int read_lock;

static bool delete_entry(long idx_id);
static bool delete_entry_in(long idx_id, int masterfd, long count, int *fds,
                            bool sweep_after);
static void sweep_unused_tags(int masterfd, long count, int *fds);
/* the entries that aren't deleted, -1 until they have been counted */
static int song_count = -1;

static inline void str_setlen(char *buf, size_t len)
{
    buf[len] = '\0';
}

const char* tagcache_tag_to_str(int tag)
{
    return tags_str[tag];
}

#ifdef TAGCACHE_SUPPORT_FOREIGN_ENDIAN
static void swap_tagfile_entry(struct tagfile_entry *buf)
{
    if (tc_stat.econ)
    {
        buf->tag_length = swap32(buf->tag_length);
        buf->idx_id = swap32(buf->idx_id);
    }
}

static void swap_index_entry(struct index_entry *buf)
{
    if (tc_stat.econ)
    {
        for (int i = 0; i < TAG_COUNT; ++i)
            buf->tag_seek[i] = swap32(buf->tag_seek[i]);
        buf->flag = swap32(buf->flag);
    }
}

static void swap_tagcache_header(struct tagcache_header *buf)
{
    if (tc_stat.econ)
    {
        buf->magic = swap32(buf->magic);
        buf->datasize = swap32(buf->datasize);
        buf->entry_count = swap32(buf->entry_count);
    }
}

static void swap_master_header(struct master_header *buf)
{
    if (tc_stat.econ)
    {
        swap_tagcache_header(&buf->tch);
        buf->serial = swap32(buf->serial);
        buf->commitid = swap32(buf->commitid);
        buf->dirty = swap32(buf->dirty);
        buf->header_size = swap32(buf->header_size);
        buf->entry_size = swap32(buf->entry_size);
    }
}
#else
static void swap_tagfile_entry(struct tagfile_entry *buf) { (void)buf; }
static void swap_index_entry(struct index_entry *buf) { (void)buf; }
static void swap_tagcache_header(struct tagcache_header *buf) { (void)buf; }
static void swap_master_header(struct master_header *buf) { (void)buf; }
#endif

static ssize_t read_tagfile_entry(int fd, struct tagfile_entry *buf)
{
    ssize_t ret = read(fd, buf, sizeof(*buf));
    if (ret == sizeof(*buf) && tc_stat.econ)
        swap_tagfile_entry(buf);

    return ret;
}

enum e_read_errors {
    e_SUCCESS = 0,
    e_SUCCESS_LEN_ZERO = 1,
    e_ENTRY_SIZEMISMATCH,
    e_TAG_TOOLONG,
    e_TAG_SIZEMISMATCH
};

static enum e_read_errors
read_tagfile_entry_and_tag(int fd, struct tagfile_entry *tfe,
                           char* buf, int bufsz)
{
    if (read_tagfile_entry(fd, tfe) != sizeof(struct tagfile_entry))
        return e_ENTRY_SIZEMISMATCH;

    long tag_length = tfe->tag_length;
    if (tag_length >= bufsz)
        return e_TAG_TOOLONG;

    if (tag_length > 0 && read(fd, buf, tag_length) != tag_length)
        return e_TAG_SIZEMISMATCH;

    str_setlen(buf, tag_length);
    return (tag_length > 0 && *buf) ? e_SUCCESS : e_SUCCESS_LEN_ZERO;
}

static ssize_t read_index_entries(int fd, struct index_entry *buf, size_t count)
{
    ssize_t ret = read(fd, buf, sizeof(*buf) * count);
    for (ssize_t i = 0; i < ret; i += sizeof(*buf))
        swap_index_entry(buf++);

    return ret;
}

static ssize_t write_index_entries(int fd, struct index_entry *buf, size_t count)
{
#ifdef TAGCACHE_SUPPORT_FOREIGN_ENDIAN
    /* one at a time only when they are to be swapped: a write() costs about
       as much for one as for all of them */
    if (tc_stat.econ)
    {
        ssize_t ret = 0;
        for (; count > 0; count--)
        {
            struct index_entry e = *buf++;
            swap_index_entry(&e);

            ssize_t rc = write(fd, &e, sizeof(e));
            if (rc < 0)
                return rc;
            ret += rc;
        }

        return ret;
    }
#endif
    return write(fd, buf, sizeof(*buf) * count);
}

static ssize_t read_tagcache_header(int fd, struct tagcache_header *buf)
{
    ssize_t ret = read(fd, buf, sizeof(*buf));
    if (ret == sizeof(*buf))
        swap_tagcache_header(buf);

    return ret;
}

static ssize_t write_tagcache_header(int fd, struct tagcache_header *buf)
{
    struct tagcache_header e = *buf;
    swap_tagcache_header(&e);
    return write(fd, &e, sizeof(e));
}

static ssize_t read_master_header(int fd, struct master_header *buf)
{
    ssize_t ret = read(fd, buf, sizeof(*buf));
    if (ret == sizeof(*buf))
        swap_master_header(buf);

    return ret;
}

static ssize_t write_master_header(int fd, struct master_header *buf)
{
    struct master_header e = *buf;
    swap_master_header(&e);
    return write(fd, &e, sizeof(e));
}

/*
 * open_db_fd and remove_db_file are noinline to minimize stack usage
 */
static int NO_INLINE open_db_fd(const char* filename, int mode)
{
    char buf[MAX_PATH];

    if(mode & O_CREAT)
    {
        if (mkdir(tc_stat.db_path) < 0 && errno != EEXIST)
            return -1;
    }

    return open_pathfmt(buf, sizeof(buf), mode, "%s/%s",
                        tc_stat.db_path, filename);
}

static int NO_INLINE remove_db_file(const char* filename)
{
    char buf[MAX_PATH];

    snprintf(buf, sizeof(buf), "%s/%s",
             tc_stat.db_path, filename);

    return remove(buf);
}

/* over the file named to, if there is one */
static int NO_INLINE rename_db_file(const char *from, const char *to)
{
    char frompath[MAX_PATH], topath[MAX_PATH];

    snprintf(frompath, sizeof(frompath), "%s/%s", tc_stat.db_path, from);
    snprintf(topath, sizeof(topath), "%s/%s", tc_stat.db_path, to);

    return rename(frompath, topath);
}

/* the scan's files for the commit: its new entries, and their Sort fields */
static void remove_scan_files(void)
{
    remove_db_file(TAGCACHE_FILE_TEMP);
#ifdef TAGCACHE_SORT_FIELDS
    remove_db_file(TAGCACHE_FILE_SORT_SCAN);
#endif
}

#if !defined(__PCTOOL__) && !defined(PLUGIN)
static bool room_for(unsigned long need_kb)
{
    sector_t free_kb = 0;
    volume_size(IF_MV(0,) NULL, &free_kb);
    if (free_kb >= need_kb)
        return true;
    logf("no room: %lu KB free, %lu KB needed", (unsigned long)free_kb,
         need_kb);
    health_note("library update stopped: no room, %lu KB free, %lu KB needed",
                (unsigned long)free_kb, need_kb);
    no_space = true;
    return false;
}

static off_t db_file_size(const char *name)
{
    const int fd = open_db_fd(name, O_RDONLY);
    if (fd < 0)
        return 0;
    const off_t size = ffilesize(fd);
    close(fd);
    return size;
}

/* KB of the database's files: at least what a commit writes anew beside
   them */
static unsigned long db_files_kb(void)
{
    char name[32];
    off_t size = db_file_size(TAGCACHE_FILE_MASTER);
#ifdef TAGCACHE_SORT_FIELDS
    size += db_file_size(TAGCACHE_FILE_SORT);
#endif
    for (int i = 0; i < TAG_COUNT; i++)
    {
        if (TAGCACHE_IS_NUMERIC(i))
            continue;
        snprintf(name, sizeof(name), TAGCACHE_FILE_INDEX, i);
        size += db_file_size(name);
    }
    return size / 1024;
}

/* the larger of the commit's new files (at most the old ones and the new
   entries) and the files made after it, as Play Counts' stamp and Search's
   names (a quarter KB a song; a seventh measured with 13k) */
static bool commit_room(int tmpfd, long new_entries)
{
    const unsigned long songs = new_entries
        + db_file_size(TAGCACHE_FILE_MASTER) / sizeof(struct index_entry);
    return room_for(MAX(db_files_kb() + ffilesize(tmpfd) / 1024, songs / 4)
                    + 1024);
}
#else
#define room_for(need_kb) true
#define commit_room(tmpfd, new_entries) true
#endif

static int open_tag_fd(struct tagcache_header *hdr, int tag, bool write)
{
    int fd;
    char fname[MAX_PATH];

    if (TAGCACHE_IS_NUMERIC(tag) || tag < 0 || tag >= TAG_COUNT)
        return -1;

    fd = open_pathfmt(fname, sizeof(fname),
                      write ? O_RDWR : O_RDONLY, "%s/" TAGCACHE_FILE_INDEX,
                      tc_stat.db_path, tag);
    if (fd < 0)
    {
        logf("%s failed: tag=%d write=%d file= " TAGCACHE_FILE_INDEX,
             __func__, tag, write, tag);
        tc_stat.ready = false;
        return fd;
    }

    /* Check the header. */
    if (read_tagcache_header(fd, hdr) != sizeof(struct tagcache_header) ||
        hdr->magic != TAGCACHE_MAGIC)
    {
        logf("header error");
        tc_stat.ready = false;
        close(fd);
        return -2;
    }

    return fd;
}

static int open_master_fd(struct master_header *hdr, bool write)
{
    int fd;
    int rc;

    fd = open_db_fd(TAGCACHE_FILE_MASTER, write ? O_RDWR : O_RDONLY);
    if (fd < 0)
    {
        logf("master file open failed for R/W");
        tc_stat.ready = false;
        return fd;
    }

    rc = read(fd, hdr, sizeof(struct master_header));
    if (rc != sizeof(struct master_header))
    {
        logf("master file read failed");
        close(fd);
        return -1;
    }

    /* Tagcache files can have either endianness. A device will always
     * create files in its native endianness, but we accept non-native
     * endian files for compatibility reasons. */
    if (hdr->tch.magic == TAGCACHE_MAGIC)
        tc_stat.econ = false;
#ifdef TAGCACHE_SUPPORT_FOREIGN_ENDIAN
    else if (hdr->tch.magic == swap32(TAGCACHE_MAGIC))
    {
        tc_stat.econ = true;
        swap_master_header(hdr);
    }
#endif
    else
    {
        logf("master file bad magic: %08lx\n", (unsigned long)hdr->tch.magic);
        close(fd);
        return -2;
    }

    return fd;
}

static void remove_files(void)
{
    int i;
    char buf[MAX_PATH];
    const int bufsz = sizeof(buf);
    logf("%s", __func__);

    tc_stat.ready = false;
    tc_stat.ramcache = false;
    tc_stat.econ = false;
#if !defined(__PCTOOL__) && !defined(PLUGIN)
    itunesdb_forget_state();
    /* and what's made from it */
    music_search_forget();
#endif
    remove_db_file(TAGCACHE_FILE_MASTER);
    for (i = 0; i < TAG_COUNT; i++)
    {
        if (TAGCACHE_IS_NUMERIC(i))
            continue;

        snprintf(buf, bufsz, "%s/" TAGCACHE_FILE_INDEX,
                 tc_stat.db_path, i);
        remove(buf);
    }
}

static bool check_all_headers(void)
{
    struct master_header myhdr;
    struct tagcache_header tch;
    int tag;
    int fd;

    if ( (fd = open_master_fd(&myhdr, false)) < 0)
        return false;

    close(fd);
    if (myhdr.dirty)
    {
        logf("tagcache is dirty!");
        return false;
    }

    memcpy(&current_tcmh, &myhdr, sizeof(struct master_header));

    for (tag = 0; tag < TAG_COUNT; tag++)
    {
        if (TAGCACHE_IS_NUMERIC(tag))
            continue;

        if ( (fd = open_tag_fd(&tch, tag, false)) < 0)
            return false;

        close(fd);
    }

    return true;
}

static bool update_master_header(void)
{
    struct master_header myhdr;
    int fd;

    if (!tc_stat.ready)
        return false;

    if ( (fd = open_master_fd(&myhdr, true)) < 0)
        return false;

    myhdr.serial = current_tcmh.serial;
    myhdr.commitid = current_tcmh.commitid;
    myhdr.dirty = current_tcmh.dirty;

    /* Write it back */
    lseek(fd, 0, SEEK_SET);
    write_master_header(fd, &myhdr);
    close(fd);

    return true;
}

#if !defined(PLUGIN)
#ifndef __PCTOOL__
static bool do_timed_yield(void)
{
    /* Sorting can lock up for quite a while, so yield occasionally: each tick,
       as the UI waits for it meanwhile, a wheel's step 40 ms at HZ/25 */
    static long wakeup_tick = 0;
    if (TIME_AFTER(current_tick, wakeup_tick))
    {
        yield();
        wakeup_tick = current_tick; /* the next one */
        return true;
    }
    return false;
}
#endif /* __PCTOOL__ */

#ifndef __PCTOOL__
/* main() goes on to take memory once the database is initialized: the menus',
   the playlist's, the audio buffer and the theme. A commit's buffer, all that
   is left and locked, waits for that: with only a few songs a first boot's
   import had committed by the time of tagtree_init(), which got nothing and
   panicked. */
static volatile bool boot_done = false;

void tagcache_boot_done(void)
{
    boot_done = true;
}
#endif

static void allocate_tempbuf(void)
{
    /* Yeah, malloc would be really nice now :) */
    size_t size;
    tempbuf_size = 0;

#ifdef __PCTOOL__
    size = 32*1024*1024;
    tempbuf = malloc(size);
    if (tempbuf)
        tempbuf_size = size;
#else /* !__PCTOOL__ */
    /* before it, a commit is at init, which main() waits for */
    while (tc_stat.initialized && !boot_done)
        sleep(HZ/50);
    /* Need to pass dummy ops to prevent the buffer being moved
     * out from under us, since we yield during the tagcache commit. */
    tempbuf_handle = core_alloc_maximum(&size, &buflib_ops_locked);
    if (tempbuf_handle > 0)
    {
        tempbuf = core_get_data(tempbuf_handle);
        tempbuf_size = size;
    }
#endif /* __PCTOOL__ */

}

static void free_tempbuf(void)
{
    if (tempbuf_size == 0)
        return ;

#ifdef __PCTOOL__
    free(tempbuf);
#else
    tempbuf_handle = core_free(tempbuf_handle);
#endif
    tempbuf = NULL;
    tempbuf_size = 0;
}

#ifndef __PCTOOL__
/* Database entries by a hash of their file's path, sorted, for a binary
 * search: while the database is built, and while it is in memory. Without
 * them a file is looked up by walking every entry, and by reading the
 * filename file through when that fails, which is a second or more on the
 * iPod and holds up playback. */
struct path_name {
    uint64_t hash;
    long idx_id;
    uint32_t mtime;     /* of the entry, in the table of a scan */
};

static int cmp_path_name(const void *a, const void *b)
{
    uint64_t x = ((const struct path_name *)a)->hash;
    uint64_t y = ((const struct path_name *)b)->hash;
    return (x > y) - (x < y);
}


/* the entry of the file at path, or -1 */
static long find_path_name(int handle, long count, const char *path,
                           uint32_t *mtime)
{
    if (handle <= 0)
        return -1;
    uint64_t hash = itunesdb_path_hash(path);
    struct path_name *names = core_get_data_pinned(handle);
    long found = -1, lo = 0, hi = count - 1;
    while (lo <= hi)
    {
        long mid = (lo + hi) / 2;
        if (names[mid].hash == hash)
        {
            found = names[mid].idx_id;
            if (mtime)
                *mtime = names[mid].mtime;
            break;
        }
        else if (names[mid].hash < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    core_put_data_pinned(names);
    return found;
}

/* the changes to places, plays and marks since boot */
static long states_generation;
#endif /* !__PCTOOL__ */

#if defined(HAVE_TC_RAMCACHE) && !defined(__PCTOOL__)
/* the files of the database in memory, filled as it is loaded */
static int ram_names_handle;
static long ram_names_count, ram_names_capacity;
static bool ram_names_overflow; /* then the slow ways are used */

/* And their paths, which are otherwise built from the dircache or read
 * from the disk for each file: Shuffle Songs, a list played and Play
 * Counts take them all. With each entry's kind, for tag_virt_kind. The
 * offsets and kinds are by entry, then come the paths; an offset is one
 * past the path's, 0 for none. */
static int ram_paths_handle;
static long ram_paths_count;
static size_t ram_paths_size, ram_paths_used;
static bool ram_paths_whole; /* none left out for room */

static void free_ram_names(void)
{
    ram_paths_whole = false;
    if (ram_names_handle > 0)
        core_free(ram_names_handle);
    ram_names_handle = 0;
    ram_names_count = ram_names_capacity = 0;
    ram_names_overflow = false;
    if (ram_paths_handle > 0)
        core_free(ram_paths_handle);
    ram_paths_handle = 0;
    ram_paths_count = 0;
    ram_paths_size = ram_paths_used = 0;
}

/* Room for the names and paths of count files, kept for the loads after, as
   an update's, while it's as big. Taken while music played, it made the music
   stop and start again as its buffer gave the room up: then there's none, and
   the dircache finds the files instead. */
static void ram_names_alloc(long count, size_t paths_size)
{
    if (ram_names_handle > 0 && ram_paths_handle > 0
        && ram_names_capacity >= count && ram_paths_count >= count
        && ram_paths_size >= paths_size)
    {
        ram_names_count = 0;
        ram_names_overflow = false;
        ram_paths_used = 0;
        ram_paths_whole = true;
        memset(core_get_data(ram_paths_handle), 0,
               ram_paths_count * (sizeof(uint32_t) + 1));
        return;
    }
    free_ram_names();
    if (count <= 0)
        return;
    const size_t need = count * sizeof(struct path_name)
                        + count * (sizeof(uint32_t) + 1) + paths_size;
    if ((audio_status() & AUDIO_STATUS_PLAY) && core_allocatable() < need)
        return;

    ram_names_capacity = count;
    ram_names_handle = core_alloc(count * sizeof(struct path_name));
    if (ram_names_handle <= 0)
        ram_names_handle = ram_names_capacity = 0;

    /* the paths take at most what the filename file has */
    ram_paths_count = count;
    ram_paths_size = paths_size;
    ram_paths_handle = core_alloc(count * (sizeof(uint32_t) + 1) + paths_size);
    if (ram_paths_handle > 0)
    {
        memset(core_get_data(ram_paths_handle), 0,
               count * (sizeof(uint32_t) + 1));
        ram_paths_whole = true;
    }
    else
        ram_paths_handle = ram_paths_count = ram_paths_size = 0;
}

/* the filename file's size of its paths, for ram_names_alloc */
static size_t ram_paths_needed(void)
{
    struct tagcache_header tch;
    int fd = open_tag_fd(&tch, tag_filename, false);
    if (fd < 0)
        return 0;
    close(fd);
    return tch.datasize;
}

/* with the kind, for a file outside Finder's folder: the check of those after
   USB passes over the others without a look at their paths */
#define RAM_KIND_OUTSIDE 0x80
/* and a podcast's or a book's mark, not played, as the flags have it
   (ITUNESDB_UNPLAYED): changed here as it changes there */
#define RAM_KIND_UNPLAYED 0x40
#define RAM_KIND_MASK 0x0f

/* an entry's kinds byte, by its path and the flags of its hash */
static uint8_t ram_kind_of(const char *path, uint64_t hash)
{
    const unsigned flags = itunesdb_hash_flags(hash);
    return ((flags & ITUNESDB_AUDIOBOOK) ? 2 : (flags & ITUNESDB_PODCAST) ? 1 : 0)
           | ((flags & ITUNESDB_UNPLAYED) ? RAM_KIND_UNPLAYED : 0)
           | (itunesdb_synced_path(path) ? 0 : RAM_KIND_OUTSIDE);
}

static void ram_path_add(long idx_id, const char *path, uint64_t hash)
{
    if (ram_paths_handle <= 0 || idx_id < 0 || idx_id >= ram_paths_count)
        return;
    char *base = core_get_data(ram_paths_handle);
    uint32_t *offsets = (uint32_t *)base;
    uint8_t *kinds = (uint8_t *)(offsets + ram_paths_count);
    char *pool = (char *)(kinds + ram_paths_count);
    size_t len = strlen(path) + 1;
    if (ram_paths_used + len <= ram_paths_size)
    {
        memcpy(pool + ram_paths_used, path, len);
        offsets[idx_id] = ram_paths_used + 1;
        ram_paths_used += len;
    }
    else
        ram_paths_whole = false;
    kinds[idx_id] = ram_kind_of(path, hash);
}

/* The kinds again from the flags an import has just written, for a database
   that stays in memory: a sync that adds or removes no file makes no commit,
   and the marks on the screen were those before it */
static void ram_kinds_refresh(void)
{
    if (!tc_stat.ramcache || ram_paths_handle <= 0)
        return;
    const char *base = core_get_data_pinned(ram_paths_handle);
    const uint32_t *offsets = (const uint32_t *)base;
    uint8_t *kinds = (uint8_t *)(offsets + ram_paths_count);
    const char *pool = (const char *)(kinds + ram_paths_count);
    for (long i = 0; i < ram_paths_count; i++)
    {
        if (offsets[i])
        {
            const char *path = pool + offsets[i] - 1;
            kinds[i] = ram_kind_of(path, itunesdb_path_hash(path));
        }
        if ((i & 1023) == 1023)
            yield();
    }
    core_put_data_pinned((void *)base);
    states_generation++;
}

/* the index of the entry in memory idx points at, or -1 */
static long ram_entry_id(const struct index_entry *idx)
{
    if (!tc_stat.ramcache || !tcramcache.hdr || idx < tcramcache.hdr->indices
        || idx >= tcramcache.hdr->indices + current_tcmh.tch.entry_count)
        return -1;
    return idx - tcramcache.hdr->indices;
}

/* the paths of all the entries in memory, as loaded */
static bool ram_paths_all(void)
{
    return tc_stat.ramcache && ram_paths_handle > 0 && ram_paths_whole
           && !ram_names_overflow /* nor their paths added */
           && ram_paths_count >= current_tcmh.tch.entry_count;
}

/* the path of the entry into buf, from memory */
static bool ram_path(long idx_id, char *buf, size_t size)
{
    if (!tc_stat.ramcache || ram_paths_handle <= 0
        || idx_id < 0 || idx_id >= ram_paths_count)
        return false;
    const char *base = core_get_data_pinned(ram_paths_handle);
    const uint32_t *offsets = (const uint32_t *)base;
    const char *pool = base + ram_paths_count * (sizeof(uint32_t) + 1);
    uint32_t off = offsets[idx_id];
    bool ok = off > 0 && strmemccpy(buf, pool + off - 1, size) != NULL;
    core_put_data_pinned((void *)base);
    return ok;
}

static int ram_kind(long idx_id)
{
    if (!tc_stat.ramcache || ram_paths_handle <= 0
        || idx_id < 0 || idx_id >= ram_paths_count)
        return 0;
    const uint8_t *kinds = (const uint8_t *)core_get_data(ram_paths_handle)
                           + ram_paths_count * sizeof(uint32_t);
    return kinds[idx_id] & RAM_KIND_MASK;
}

/* the entry's kinds byte in memory, to read or change; NULL without it */
static uint8_t *ram_kinds_byte(long idx_id)
{
    if (!tc_stat.ramcache || ram_paths_handle <= 0
        || idx_id < 0 || idx_id >= ram_paths_count)
        return NULL;
    return (uint8_t *)core_get_data(ram_paths_handle)
           + ram_paths_count * sizeof(uint32_t) + idx_id;
}
#endif

#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
/* find the ramcache entry corresponding to the file indicated by
 * filename and dc (it's corresponding dircache id). */
static long find_entry_ram(const char *filename)
{
    static long last_pos = 0;
    struct dircache_fileref dcfref;

    /* Check if tagcache is loaded into ram. */
    if (!tc_stat.ramcache
        || global_settings.tagcache_ram != TAGCACHE_RAM_ON)
        return -1;

#ifndef __PCTOOL__
    if (ram_names_handle > 0)
    {
        long idx_id = find_path_name(ram_names_handle, ram_names_count,
                                     filename, NULL);
        if (idx_id >= 0 && idx_id < current_tcmh.tch.entry_count
            && !(tcramcache.hdr->indices[idx_id].flag & FLAG_DELETED))
            return idx_id;
        return -1;
    }
#endif

    if (dircache_search(DCS_CACHED_PATH | DCS_UPDATE_FILEREF, &dcfref,
                        filename) <= 0)
    {
        logf("tagcache: file not found.");
        return -1;
    }

    /* Search references */
    int end_pos = current_tcmh.tch.entry_count;
    while (1)
    {
        for (int i = last_pos; i < end_pos; i++)
        {
            do_timed_yield();

            if (!(tcramcache.hdr->indices[i].flag & FLAG_DIRCACHE))
                continue;

            int cmp = dircache_fileref_cmp(&tcrc_dcfrefs[i], &dcfref);
            if (cmp < 3)
                continue;

            last_pos = MAX(0, i - 3);
            return i;
        }

        if (last_pos == 0)
        {
            last_pos = MAX(0, end_pos - 3);
            break;
        }

        end_pos = last_pos;
        last_pos = 0;
    }

    return -1;
}
#endif /* defined (HAVE_TC_RAMCACHE) && defined (HAVE_DIRCACHE) */

static long find_entry_disk(const char *filename_raw, bool localfd)
{
    struct tagfile_entry tfe;
    struct tagcache_header tch;
    static long last_pos = -1;
    long pos_history[POS_HISTORY_COUNT];
    unsigned int pos_history_idx = 0;
    unsigned int i;

    char buf[TAGCACHE_BUFSZ];
    const long bufsz = sizeof(buf);

    int fd;
    int pos = -1;
    long idx = -1;

    bool found = false;

    const char *filename = filename_raw;

#ifdef APPLICATION
    char pathbuf[PATH_MAX]; /* Note: Don't use MAX_PATH here, it's too small */
    if (realpath(filename, pathbuf) == pathbuf)
        filename = pathbuf;
#endif /* APPLICATION */

    if (!tc_stat.ready)
        return -2;

    fd = filenametag_fd;
    if (fd < 0 || localfd)
    {
        last_pos = -1;
        if ( (fd = open_tag_fd(&tch, tag_filename, false)) < 0)
            return -1;
    }

    check_again:

    if (last_pos > 0) /* pos gets cached to prevent reading from beginning */
        pos = lseek(fd, last_pos, SEEK_SET);
    else /* start back at beginning */
        pos = lseek(fd, sizeof(struct tagcache_header), SEEK_SET);

    long tag_length = strlen(filename) + 1; /* include NULL */

    if (tag_length < bufsz)
    {
        while (true)
        {
            for (i = pos_history_idx-1; i < pos_history_idx; i--)
                pos_history[i+1] = pos_history[i];
            pos_history[0] = pos;

            if (read_tagfile_entry(fd, &tfe) != sizeof(struct tagfile_entry))
            {
                logf("size mismatch find entry");
                break;
            }
            else
            {
                pos += sizeof(struct tagfile_entry) + tfe.tag_length;
                /* don't read the entry unless the length matches */
                if (tfe.tag_length == tag_length)
                {
                    if(read(fd, buf, tfe.tag_length) != tag_length)
                    {
                        logf("read error #2");
                        close(fd);
                        if (!localfd)
                            filenametag_fd = -1;
                        last_pos = -1;
                        return -3;
                    }
                    if (!strncmp(filename, buf, tag_length))
                    {
                        last_pos = pos_history[pos_history_idx];
                        found = true;
                        idx = tfe.idx_id;
                        break ;
                    }
                }
                else
                    lseek(fd, pos, SEEK_SET);

            }
        }
        if (pos_history_idx < POS_HISTORY_COUNT - 1)
            pos_history_idx++;
    }

    /* Not found? */
    if (!found)
    {
        if (last_pos > 0) /* start back at the beginning */
        {
            last_pos = -1;
            logf("seek again");
            goto check_again;
        }

        idx = -4;
    }

    if (fd != filenametag_fd || localfd)
        close(fd);

    return idx;
}

static int find_index(const char *filename)
{
    long idx_id = -1;

#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
    idx_id = find_entry_ram(filename);
#if !defined(__PCTOOL__)
    /* what the table of the files doesn't have isn't in the database */
    if (tc_stat.ramcache && ram_names_handle > 0)
        return idx_id;
#endif
#endif

    if (idx_id < 0)
        idx_id = find_entry_disk(filename, true);

    return idx_id;
}

bool tagcache_find_index(struct tagcache_search *tcs, const char *filename)
{
    /* NOTE: on ret==true you need to call tagcache_search_finish(tcs) yourself */
    int idx_id;

    if (!tc_stat.ready)
        return false;

    idx_id = find_index(filename);
    if (idx_id < 0)
        return false;

    if (!tagcache_search(tcs, tag_filename))
        return false;

    tcs->entry_count = 0;
    tcs->idx_id = idx_id;

    return true;
}

static bool get_index(int masterfd, int idxid,
                      struct index_entry *idx, bool use_ram)
{
    bool localfd = false;

    if (idxid < 0)
    {
        logf("Incorrect idxid: %d", idxid);
        return false;
    }

#ifdef HAVE_TC_RAMCACHE
    if (tc_stat.ramcache && use_ram)
    {
        /* as a read of a place past the end of the file fails */
        if (idxid >= current_tcmh.tch.entry_count)
            return false;

        if (tcramcache.hdr->indices[idxid].flag & FLAG_DELETED)
            return false;

        *idx = tcramcache.hdr->indices[idxid];
        return true;
    }
#endif /* HAVE_TC_RAMCACHE */

    if (masterfd < 0)
    {
        struct master_header tcmh;

        localfd = true;
        masterfd = open_master_fd(&tcmh, false);
        if (masterfd < 0)
            return false;
    }

    lseek(masterfd, idxid * sizeof(struct index_entry)
          + sizeof(struct master_header), SEEK_SET);
    if (read_index_entries(masterfd, idx, 1) != sizeof(struct index_entry))
    {
        logf("read error #3");
        if (localfd)
            close(masterfd);

        return false;
    }

    if (localfd)
        close(masterfd);

    if (idx->flag & FLAG_DELETED)
        return false;

    return true;

    (void)use_ram;
}

#ifndef __PCTOOL__

static bool write_index(int masterfd, int idxid, struct index_entry *idx)
{
    /* We need to exclude all memory only flags & tags when writing to disk. */
    if (idx->flag & FLAG_DIRCACHE)
    {
        logf("memory only flags!");
        return false;
    }

#ifdef HAVE_TC_RAMCACHE
    /* Only update numeric data. Writing the whole index to RAM by memcpy
     * destroys dircache pointers!
     */
    if (tc_stat.ramcache)
    {
        struct index_entry *idx_ram = &tcramcache.hdr->indices[idxid];

        for (int tag = 0; tag < TAG_COUNT; tag++)
        {
            if (TAGCACHE_IS_NUMERIC(tag))
            {
                idx_ram->tag_seek[tag] = idx->tag_seek[tag];
            }
        }

        /* Don't touch the dircache flag or attributes. */
        idx_ram->flag = (idx->flag & 0x0000ffff)
            | (idx_ram->flag & (0xffff0000 | FLAG_DIRCACHE));
    }
#endif /* HAVE_TC_RAMCACHE */

    lseek(masterfd, idxid * sizeof(struct index_entry)
          + sizeof(struct master_header), SEEK_SET);
    if (write_index_entries(masterfd, idx, 1) != sizeof(struct index_entry))
    {
        logf("write error #3");
        logf("idxid: %d", idxid);
        return false;
    }

    return true;
}

#endif /* !__PCTOOL__ */

static bool open_files(struct tagcache_search *tcs, int tag)
{
    if (tcs->idxfd[tag] < 0)
    {
        char fname[MAX_PATH];
        tcs->idxfd[tag] = open_pathfmt(fname, sizeof(fname),
                                       O_RDONLY, "%s/" TAGCACHE_FILE_INDEX,
                                       tc_stat.db_path, tag);
        if (tcs->idxfd[tag] < 0)
        {
            logf("File not open!");
            return false;
        }
    }

    return true;
}

static bool retrieve(struct tagcache_search *tcs, int idx_id,
                     struct index_entry *idx, int tag, char *buf, long bufsz)
{
    bool success = false;
    bool is_basename = false;
    struct tagfile_entry tfe;
    long seek;

    if (tag == tag_virt_basename)
    {
        tag = tag_filename;
        is_basename = true;
    }

    if (TAGCACHE_IS_NUMERIC(tag))
        goto failure;

    seek = idx->tag_seek[tag];
    if (seek < 0)
    {
        logf("Retrieve failed");
        goto failure;
    }

#ifdef HAVE_TC_RAMCACHE
    if (tcs->ramsearch)
    {
#ifndef __PCTOOL__
        /* A clause in a search passes the entry in memory with an idx_id
           from before; tagcache_retrieve() a copy with the right one */
        if (ram_entry_id(idx) >= 0)
            idx_id = ram_entry_id(idx);
        if (tag == tag_filename && ram_path(idx_id, buf, bufsz))
            success = true;
        else
#endif
#ifdef HAVE_DIRCACHE
        if (tag == tag_filename && (idx->flag & FLAG_DIRCACHE))
        {
            if (dircache_get_fileref_path(&tcrc_dcfrefs[idx_id], buf, bufsz) >= 0)
                success = true;
        }
        else
#endif /* HAVE_DIRCACHE */
        if (tag != tag_filename)
        {
            struct tagfile_entry *ep =
                (struct tagfile_entry *)&tcramcache.hdr->tags[tag][seek];
            strmemccpy(buf, ep->tag_data, bufsz);
            success = true;
        }
    }
#endif /* HAVE_TC_RAMCACHE */

    if (!success && open_files(tcs, tag))
    {
        lseek(tcs->idxfd[tag], seek, SEEK_SET);
        switch (read_tagfile_entry_and_tag(tcs->idxfd[tag], &tfe, buf, bufsz))
        {
            case e_ENTRY_SIZEMISMATCH:
                logf("read error #5");
                break;
            case e_TAG_TOOLONG:
                logf("too long tag #5");
                break;
            case e_TAG_SIZEMISMATCH:
                logf("read error #6");
                break;
            default:
                logf("unknown_error");
                break;
            case e_SUCCESS_LEN_ZERO:
            case e_SUCCESS:
                success = true;
                break;
        }
    }

    if (success)
    {
        if (is_basename)
        {
            char* basename = strrchr(buf, '/');
            if (basename != NULL)
                memmove(buf, basename + 1, strlen(basename)); /* includes NULL */
        }
        return true;
    }

failure:
    str_setlen(buf, 0);
    return false;
}

#define COMMAND_QUEUE_IS_EMPTY (command_queue_ridx == command_queue_widx)

static long tc_find_tag(int tag, int idx_id, const struct index_entry *idx)
{
#ifndef __PCTOOL__
    if (! COMMAND_QUEUE_IS_EMPTY && TAGCACHE_IS_NUMERIC(tag))
    {
        /* Attempt to find tag data through store-to-load forwarding in
           command queue */
        long result = -1;

        mutex_lock(&command_queue_mutex);

        int ridx = command_queue_widx;

        while (ridx != command_queue_ridx)
        {
            if (--ridx < 0)
                ridx = TAGCACHE_COMMAND_QUEUE_LENGTH - 1;

            if (command_queue[ridx].command == CMD_UPDATE_NUMERIC
                && command_queue[ridx].idx_id == idx_id
                && command_queue[ridx].tag == tag)
            {
                result = command_queue[ridx].data;
                break;
            }
        }

        mutex_unlock(&command_queue_mutex);

        if (result >= 0)
        {
            logf("tc_find_tag: Recovered tag %d value %lX from write queue",
                 tag, (unsigned long) result);
            return result;
        }
    }
#else
    (void)idx_id;
#endif

    return idx->tag_seek[tag];
}

static inline long sec_in_ms(long ms)
{
    return (ms/1000) % 60;
}

static inline long min_in_ms(long ms)
{
    return (ms/1000) / 60;
}

static long check_virtual_tags(int tag, int idx_id,
                               const struct index_entry *idx)
{
    long data = 0;

    switch (tag)
    {
        case tag_virt_length_sec:
            data = sec_in_ms(tc_find_tag(tag_length, idx_id, idx));
            break;

        case tag_virt_length_min:
            data = min_in_ms(tc_find_tag(tag_length, idx_id, idx));
            break;

        case tag_virt_playtime_sec:
            data = sec_in_ms(tc_find_tag(tag_playtime, idx_id, idx));
            break;

        case tag_virt_playtime_min:
            data = min_in_ms(tc_find_tag(tag_playtime, idx_id, idx));
            break;

        case tag_virt_kind:
#if defined(HAVE_TC_RAMCACHE) && !defined(__PCTOOL__)
            /* a search in memory passes the entry, not its index */
            if (ram_entry_id(idx) >= 0)
                idx_id = ram_entry_id(idx);
            data = ram_kind(idx_id);
#endif
            break;

        case tag_virt_autoscore:
            if (tc_find_tag(tag_length, idx_id, idx) == 0
                || tc_find_tag(tag_playcount, idx_id, idx) == 0)
            {
                data = 0;
            }
            else
            {
                /* A straight calculus gives:
                     autoscore = 100 * playtime / length / playcout (1)
                   Now, consider the euclidian division of playtime by length:
                     playtime = alpha * length + beta
                   With:
                     0 <= beta < length
                   Now, (1) becomes:
                     autoscore = 100 * (alpha / playcout + beta / length / playcount)
                   Both terms should be small enough to avoid any overflow
                */
                long playtime = tc_find_tag(tag_playtime, idx_id, idx);
                long length = tc_find_tag(tag_length, idx_id, idx);
                long playcount = tc_find_tag(tag_playcount, idx_id, idx);
                data = 100 * (playtime / length) + (100 * (playtime % length)) / length;
                data /= playcount;
            }
            break;

        /* How many commits before the file has been added to the DB. */
        case tag_virt_entryage:
            data = current_tcmh.commitid
                   - tc_find_tag(tag_commitid, idx_id, idx) - 1;
            break;

        case tag_virt_basename:
            tag = tag_filename; /* return filename; caller handles basename */
            /* FALLTHRU */

        default:
            data = tc_find_tag(tag, idx_id, idx);
    }

    return data;
}

long tagcache_get_numeric(const struct tagcache_search *tcs, int tag)
{
    struct index_entry idx;

    if (!tc_stat.ready)
        return false;

    if (!TAGCACHE_IS_NUMERIC(tag))
        return -1;

    if (!get_index(tcs->masterfd, tcs->idx_id, &idx, true))
        return -2;

    return check_virtual_tags(tag, tcs->idx_id, &idx);
}

inline static bool str_ends_with(const char *str1, const char *str2)
{
    logf_clauses("%s %s %s", str1, __func__, str2);
    int str_len = strlen(str1);
    int clause_len = strlen(str2);

    if (clause_len > str_len)
        return false;

    return !strcasecmp(&str1[str_len - clause_len], str2);
}

inline static bool str_oneof(const char *str, const char *list)
{
    logf_clauses("%s %s %s", str, __func__, list);
    const char *sep;
    int l, len = strlen(str);

    while (*list)
    {
        sep = strchr(list, '|');
        l = sep ? (intptr_t)sep - (intptr_t)list : (int)strlen(list);
        if ((l==len) && !strncasecmp(str, list, len))
            return true;
        list += sep ? l + 1 : l;
    }

    return false;
}

inline static bool str_begins_ends_oneof(const char *str, const char *list, bool begins)
{
    logf_clauses("%s %s (%s) %s", str, __func__, begins ? "begins" : "ends", list);
    const char *sep;
    int l, p, len = strlen(str);

    while (*list)
    {
        sep = strchr(list, '|');
        l = sep ? (intptr_t)sep - (intptr_t)list : (int)strlen(list);
        p = begins ? 0 : len - l;
        if (l <= len && !strncasecmp(&str[p], list, l))
            return true;
        list += sep ? l + 1 : l;
    }

    return false;
}

inline static bool str_contains_oneof(const char *str, char *list)
{
    logf_clauses("%s %s %s", str, __func__, list);
	const char *sep;
	int l, len = strlen(str);
	
	while (*list)
	{
		sep = strchr(list, '|');
		l = sep ? (intptr_t)sep - (intptr_t)list : (int)strlen(list);
		list[l] = '\0';
		if (l <= len && strcasestr(str, list) != NULL) {
			if (sep) list[l] = '|';
			return true;
		}
		if (sep) list[l] = '|';
		list += sep ? l + 1 : l;
	}
	
	return false;
}

static bool check_against_clause(long numeric, const char *str,
                                 const struct tagcache_search_clause *clause)
{
    if (clause->numeric)
    {
        switch (clause->type)
        {
            case clause_is:
                return numeric == clause->numeric_data;
            case clause_is_not:
                return numeric != clause->numeric_data;
            case clause_gt:
                return numeric > clause->numeric_data;
            case clause_gteq:
                return numeric >= clause->numeric_data;
            case clause_lt:
                return numeric < clause->numeric_data;
            case clause_lteq:
                return numeric <= clause->numeric_data;
            default:
                logf("Incorrect numeric tag: %d", clause->type);
        }
    }
    else
    {
        switch (clause->type)
        {
            case clause_is:
                return !strcasecmp(clause->str, str);
            case clause_is_not:
                return strcasecmp(clause->str, str);
            case clause_gt:
                return 0>strcasecmp(clause->str, str);
            case clause_gteq:
                return 0>=strcasecmp(clause->str, str);
            case clause_lt:
                return 0<strcasecmp(clause->str, str);
            case clause_lteq:
                return 0<=strcasecmp(clause->str, str);
            case clause_contains:
                return (strcasestr(str, clause->str) != NULL);
            case clause_not_contains:
                return (strcasestr(str, clause->str) == NULL);
            case clause_begins_with:
                return (strcasestr(str, clause->str) == str);
            case clause_not_begins_with:
                return (strcasestr(str, clause->str) != str);
            case clause_ends_with:
                return str_ends_with(str, clause->str);
            case clause_not_ends_with:
                return !str_ends_with(str, clause->str);
            case clause_oneof:
                return str_oneof(str, clause->str);
            case clause_not_oneof:
                return !str_oneof(str, clause->str);
            case clause_ends_oneof:
                /* Fall-Through */
            case clause_begins_oneof:
                return str_begins_ends_oneof(str, clause->str,
                                             clause->type == clause_begins_oneof);
            case clause_not_ends_oneof:
                /* Fall-Through */
            case clause_not_begins_oneof:
                return !str_begins_ends_oneof(str, clause->str,
                                            clause->type == clause_not_begins_oneof);
			case clause_contains_oneof:
				return str_contains_oneof(str, clause->str);
			case clause_not_contains_oneof:
				return !str_contains_oneof(str, clause->str);
            default:
                logf("Incorrect tag: %d", clause->type);
        }
    }

    return false;
}

static bool check_clauses(struct tagcache_search *tcs,
                          struct index_entry *idx,
                          struct tagcache_search_clause **clauses, int count)
{
    int i;

    /* Go through all conditional clauses. */
    for (i = 0; i < count; i++)
    {
        int seek;
        char buf[256];
        const int bufsz = sizeof(buf);
        char *str = buf;
        struct tagcache_search_clause *clause = clauses[i];

        logf_clauses("%s clause %d %s %s [%ld] %s",
            "Checking",  i, tag_type_str[clause->type],
            tags_str[clause->tag],  clause->numeric_data,
            (clause->numeric || clause->str == NULL) ? "[NUMERIC?]" : clause->str);

        if (clause->type == clause_logical_or)
        {
            logf_clauses("Bailing");
            break; /* all conditions before logical-or satisfied --
                      stop processing clauses */
        }
        seek = check_virtual_tags(clause->tag, tcs->idx_id, idx);

#ifdef HAVE_TC_RAMCACHE
        if (tcs->ramsearch)
        {
            struct tagfile_entry *tfe;

            if (!TAGCACHE_IS_NUMERIC(clause->tag))
            {
                if (clause->tag == tag_filename
                    || clause->tag == tag_virt_basename)
                {
                    retrieve(tcs, tcs->idx_id, idx, clause->tag,
                             buf, bufsz);
                }
                else
                {
                    tfe = (struct tagfile_entry *)
                                        &tcramcache.hdr->tags[clause->tag][seek];
                    /* str points to movable data, but no locking required here,
                     * as no yield() is following */
                    str = tfe->tag_data;
                }
            }
        }
        else
#endif /* HAVE_TC_RAMCACHE */
        {
            struct tagfile_entry tfe;

            if (!TAGCACHE_IS_NUMERIC(clause->tag))
            {
                int tag = clause->tag;
                if (tag == tag_virt_basename)
                    tag = tag_filename;

                int fd = tcs->idxfd[tag];
                lseek(fd, seek, SEEK_SET);

                switch (read_tagfile_entry_and_tag(fd, &tfe, str, bufsz))
                {
                    case e_SUCCESS_LEN_ZERO: /* Check if entry has been deleted. */
                        return false;
                    case e_SUCCESS:
                        if (clause->tag == tag_virt_basename)
                        {
                            char *basename = strrchr(str, '/');
                            if (basename)
                                str = basename + 1;
                        }
                        break;
                    case e_ENTRY_SIZEMISMATCH:
                        logf("read error #15");
                        return false;
                    case e_TAG_TOOLONG:
                        logf("too long tag #6");
                        return false;
                    case e_TAG_SIZEMISMATCH:
                        logf("read error #16");
                        return false;
                    default:
                        logf("unknown_error");
                        break;;
                }
            }
        }

        if (!check_against_clause(seek, str, clause))
        {
            /* Clause failed -- try finding a logical-or clause */
            while (++i < count)
            {
                if (clauses[i]->type == clause_logical_or)
                    break;
            }

            if (i < count)        /* Found logical-or? */
                continue;         /* Check clauses after logical-or */

            return false;
        }

        logf_clauses("%s clause %d %s %s [%ld] %s",
            "Found",  i, tag_type_str[clause->type],
            tags_str[clause->tag],  clause->numeric_data,
            (clause->numeric || clause->str == NULL) ? "[NUMERIC?]" : clause->str);
    }

    return true;
}

bool tagcache_check_clauses(struct tagcache_search *tcs,
                            struct tagcache_search_clause **clause, int count)
{
    struct index_entry idx;

    if (count == 0)
        return true;

    if (!get_index(tcs->masterfd, tcs->idx_id, &idx, true))
        return false;

    return check_clauses(tcs, &idx, clause, count);
}

static bool add_uniqbuf(struct tagcache_search *tcs, uint32_t id)
{
    int i;

    /* If uniq buffer is not defined we must return true for search to work. */
    if (tcs->unique_list == NULL || (!TAGCACHE_IS_UNIQUE(tcs->type)
                                     && !TAGCACHE_IS_NUMERIC(tcs->type)))
    {
        return true;
    }

    for (i = 0; i < tcs->unique_list_count; i++)
    {
        /* Return false if entry is found. */
        if (tcs->unique_list[i] == id)
        {
            //logf("%d Exists @ %d", id, i);
            return false;
        }
    }

    if (tcs->unique_list_count < tcs->unique_list_capacity)
    {
        tcs->unique_list[i] = id;
        tcs->unique_list_count++;
    }

    return true;
}

static bool build_lookup_list(struct tagcache_search *tcs)
{
    struct index_entry entry;
    int i, j;

    tcs->seek_list_count = 0;

#ifdef HAVE_TC_RAMCACHE
    if (tcs->ramsearch)
    {
        tcrc_buffer_lock(); /* lock because below makes a pointer to movable data */

        for (i = tcs->seek_pos; i < current_tcmh.tch.entry_count; i++)
        {
            struct tagcache_seeklist_entry *seeklist;
            /* idx points to movable data, don't yield or reload */
            struct index_entry *idx = &tcramcache.hdr->indices[i];
            if (tcs->seek_list_count == SEEK_LIST_SIZE)
                break ;

            /* Skip deleted files. */
            if (idx->flag & FLAG_DELETED)
                continue;

            /* Go through all filters.. */
            for (j = 0; j < tcs->filter_count; j++)
            {
                if (idx->tag_seek[tcs->filter_tag[j]] != tcs->filter_seek[j])
                {
                    break ;
                }
            }

            if (j < tcs->filter_count)
                continue ;

            /* Check for conditions. */
            if (!check_clauses(tcs, idx, tcs->clause, tcs->clause_count))
                continue;
            if (tcs->match)
                tcs->match(idx->tag_seek, tcs->match_data);
            /* Add to the seek list if not already in uniq buffer (doesn't yield)*/
            if (!add_uniqbuf(tcs, idx->tag_seek[tcs->type]))
                continue;

            /* Lets add it. */
            seeklist = &tcs->seeklist[tcs->seek_list_count];
            seeklist->seek = idx->tag_seek[tcs->type];
            seeklist->flag = idx->flag;
            seeklist->idx_id = i;
            tcs->seek_list_count++;
        }

        tcrc_buffer_unlock();

        tcs->seek_pos = i;

        return tcs->seek_list_count > 0;
    }
#endif /* HAVE_TC_RAMCACHE */

    if (tcs->masterfd < 0)
    {
        struct master_header tcmh;
        tcs->masterfd = open_master_fd(&tcmh, false);
    }

    lseek(tcs->masterfd, tcs->seek_pos * sizeof(struct index_entry) +
            sizeof(struct master_header), SEEK_SET);

    while (read_index_entries(tcs->masterfd, &entry, 1) == sizeof(struct index_entry))
    {
        struct tagcache_seeklist_entry *seeklist;

        if (tcs->seek_list_count == SEEK_LIST_SIZE)
            break ;

        i = tcs->seek_pos;
        tcs->seek_pos++;

        /* Check if entry has been deleted. */
        if (entry.flag & FLAG_DELETED)
            continue;

        /* Go through all filters.. */
        for (j = 0; j < tcs->filter_count; j++)
        {
            if (entry.tag_seek[tcs->filter_tag[j]] != tcs->filter_seek[j])
                break ;
        }

        if (j < tcs->filter_count)
            continue ;

        /* Check for conditions. */
        if (!check_clauses(tcs, &entry, tcs->clause, tcs->clause_count))
            continue;
        if (tcs->match)
            tcs->match(entry.tag_seek, tcs->match_data);

        /* Add to the seek list if not already in uniq buffer. */
        if (!add_uniqbuf(tcs, entry.tag_seek[tcs->type]))
            continue;

        /* Lets add it. */
        seeklist = &tcs->seeklist[tcs->seek_list_count];
        seeklist->seek = entry.tag_seek[tcs->type];
        seeklist->flag = entry.flag;
        seeklist->idx_id = i;
        tcs->seek_list_count++;

        yield();
    }

    return tcs->seek_list_count > 0;
}


bool tagcache_search(struct tagcache_search *tcs, int tag)
{
    /* NOTE: call tagcache_search_finish(&tcs) when finished or BAD things may happen (TM) */
    struct tagcache_header tag_hdr;
    struct master_header   master_hdr;
    int i;

    while (read_lock)
        sleep(1);

    memset(tcs, 0, sizeof(struct tagcache_search));
    if (tc_stat.commit_step > 0 || !tc_stat.ready)
        return false;

    tcs->position = sizeof(struct tagcache_header);
    tcs->type = tag;
    tcs->seek_pos = 0;
    tcs->list_position = 0;
    tcs->seek_list_count = 0;
    tcs->filter_count = 0;
    tcs->masterfd = -1;

    for (i = 0; i < TAG_COUNT; i++)
        tcs->idxfd[i] = -1;

#ifndef HAVE_TC_RAMCACHE
    tcs->ramsearch = false;
#else
    tcs->ramsearch = tc_stat.ramcache;
    if (tcs->ramsearch)
    {
        tcs->entry_count = tcramcache.hdr->entry_count[tcs->type];
    }
    else
#endif
    {
        /* Always open as R/W so we can pass tcs to functions that modify data also
         * without failing. */
        tcs->masterfd = open_master_fd(&master_hdr, true);
        if (tcs->masterfd < 0)
            return false;

        if (!TAGCACHE_IS_NUMERIC(tcs->type))
        {
            tcs->idxfd[tcs->type] = open_tag_fd(&tag_hdr, tcs->type, false);
            if (tcs->idxfd[tcs->type] < 0)
                return false;

            tcs->entry_count = tag_hdr.entry_count;
        }
        else
        {
            tcs->entry_count = master_hdr.tch.entry_count;
        }
    }

    tcs->valid = true;
    tcs->initialized = true;
    write_lock++;

    return true;
}

void tagcache_search_set_match(struct tagcache_search *tcs,
                               void (*match)(const int32_t *tag_seek, void *data),
                               void *data)
{
    tcs->match = match;
    tcs->match_data = data;
}

void tagcache_search_set_uniqbuf(struct tagcache_search *tcs,
                                 void *buffer, long length)
{
    tcs->unique_list = (uint32_t *)buffer;
    tcs->unique_list_capacity = length / sizeof(*tcs->unique_list);
    tcs->unique_list_count = 0;
    memset(tcs->unique_list, 0, tcs->unique_list_capacity);
}

bool tagcache_search_add_filter(struct tagcache_search *tcs,
                                int tag, int seek)
{
    if (tcs->filter_count == TAGCACHE_MAX_FILTERS)
        return false;

    if (TAGCACHE_IS_NUMERIC_OR_NONUNIQUE(tag))
        return false;

    tcs->filter_tag[tcs->filter_count] = tag;
    tcs->filter_seek[tcs->filter_count] = seek;
    tcs->filter_count++;

    return true;
}

bool tagcache_search_add_clause(struct tagcache_search *tcs,
                                struct tagcache_search_clause *clause)
{
    int i;
    int clause_count = tcs->clause_count;

    if (clause_count >= TAGCACHE_MAX_CLAUSES)
    {
        logf("Too many clauses");
        return false;
    }

    if (clause->type != clause_logical_or)
    {
        /* BUGFIX OR'd clauses seem to be mishandled once made into a filter */
        if (clause_count <= 1 || tcs->clause[clause_count - 1]->type != clause_logical_or)
        {
            /* Check if there is already a similar filter in present (filters are
             * much faster than clauses).
             */
            for (i = 0; i < tcs->filter_count; i++)
            {
                if (tcs->filter_tag[i] == clause->tag)
                {
                    return true;
                }
            }
        }

        if (!TAGCACHE_IS_NUMERIC(clause->tag) && tcs->idxfd[clause->tag] < 0)
        {
            char fname[MAX_PATH];
            tcs->idxfd[clause->tag] = open_pathfmt(fname, sizeof(fname), O_RDONLY,
                                                   "%s/" TAGCACHE_FILE_INDEX,
                                                   tc_stat.db_path, clause->tag);
        }
    }

    tcs->clause[tcs->clause_count] = clause;
    tcs->clause_count++;

    return true;
}

static bool get_next(struct tagcache_search *tcs, bool is_numeric, char *buf, long bufsz)
{
    struct tagfile_entry entry;
#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
    long flag = 0;
#endif

    if (tcs->idxfd[tcs->type] < 0 && !is_numeric
#ifdef HAVE_TC_RAMCACHE
        && !tcs->ramsearch
#endif
        )
        return false;

    /* Relative fetch. */
    if (tcs->filter_count > 0 || tcs->clause_count > 0 || is_numeric
#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
        /* We need to retrieve flag status for dircache. */
        || (tcs->ramsearch && tcs->type == tag_filename)
#endif
        )
    {
        struct tagcache_seeklist_entry *seeklist;

        /* Check for end of list. */
        if (tcs->list_position == tcs->seek_list_count)
        {
            tcs->list_position = 0;

            /* Try to fetch more. */
            if (!build_lookup_list(tcs))
            {
                tcs->valid = false;
                return false;
            }
        }

        seeklist = &tcs->seeklist[tcs->list_position];
#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
        flag = seeklist->flag;
#endif
        tcs->position = seeklist->seek;
        tcs->idx_id = seeklist->idx_id;
        tcs->list_position++;
    }
    else
    {
        if (tcs->entry_count == 0)
        {
            tcs->valid = false;
            return false;
        }

        tcs->entry_count--;
    }

    tcs->result_seek = tcs->position;

    if (is_numeric)
    {
        itoa_buf(buf, bufsz, tcs->position);
        tcs->result = buf;
        tcs->result_len = strlen(buf) + 1;
        return true;
    }

    /* Direct fetch. */
#ifdef HAVE_TC_RAMCACHE
    if (tcs->ramsearch)
    {
#ifdef HAVE_DIRCACHE
#ifndef __PCTOOL__
        if (tcs->type == tag_filename && ram_path(tcs->idx_id, buf, bufsz))
        {
            tcs->result_len = strlen(buf) + 1;
            tcs->result     = buf;
            tcs->ramresult  = false;
            return true;
        }
#endif
        if (tcs->type == tag_filename && (flag & FLAG_DIRCACHE))
        {
            ssize_t len = dircache_get_fileref_path(&tcrc_dcfrefs[tcs->idx_id],
                                                    buf, bufsz);
            if (len >= 0)
            {
                tcs->result_len = len + 1;
                tcs->result     = buf;
                tcs->ramresult  = false;
                return true;
            }
            /* else do it the hard way */
        }
#endif /* HAVE_DIRCACHE */

        if (tcs->type != tag_filename)
        {
            struct tagfile_entry *ep;

            ep = (struct tagfile_entry *)&tcramcache.hdr->tags[tcs->type][tcs->position];
            /* don't return ep->tag_data directly as it may move */
            tcs->result_len = strlcpy(buf, ep->tag_data, bufsz) + 1;
            tcs->result = buf;
            tcs->idx_id = ep->idx_id;
            tcs->ramresult = false; /* was true before we copied to buf too */

            /* Increase position for the next run. This may get overwritten. */
            tcs->position += sizeof(struct tagfile_entry) + ep->tag_length;

            return true;
        }
    }
#endif /* HAVE_TC_RAMCACHE */

    if (!open_files(tcs, tcs->type))
    {
        tcs->valid = false;
        return false;
    }

    /* Seek stream to the correct position and continue to direct fetch. */
    lseek(tcs->idxfd[tcs->type], tcs->position, SEEK_SET);

    switch (read_tagfile_entry_and_tag(tcs->idxfd[tcs->type], &entry, buf, bufsz))
    {
        case e_SUCCESS_LEN_ZERO:
        case e_SUCCESS:
             break;
        case e_ENTRY_SIZEMISMATCH:
            logf("read error #5");
            tcs->valid = false;
            return false;
        case e_TAG_TOOLONG:
            tcs->valid = false;
            logf("too long tag #2");
            logf("P:%lX/%" PRIX32, (unsigned long) tcs->position, entry.tag_length);
            return false;
        case e_TAG_SIZEMISMATCH:
            tcs->valid = false;
            logf("read error #4");
            return false;
    }

    /**
     Update the position for the next read (this may be overridden
     if filters or clauses are being used).
     */
    tcs->position += sizeof(struct tagfile_entry) + entry.tag_length;
    str_setlen(buf, entry.tag_length);

    tcs->result = buf;
    tcs->result_len = entry.tag_length + 1;
    tcs->idx_id = entry.idx_id;
    tcs->ramresult = false;

    return true;
}

bool tagcache_get_next(struct tagcache_search *tcs, char *buf, long size)
{
    if (tcs->valid && tagcache_is_usable())
    {
        bool is_numeric = TAGCACHE_IS_NUMERIC(tcs->type);
        while (get_next(tcs, is_numeric, buf, size))
        {
            if (tcs->result_len > 1)
                return true;
        }
    }
#ifdef LOGF_ENABLE
    if (tcs->unique_list_count > 0)
        logf(" uniqbuf: %d used / %d avail", tcs->unique_list_count, tcs->unique_list_capacity);
#endif

    return false;
}

bool tagcache_retrieve(struct tagcache_search *tcs, int idxid,
                       int tag, char *buf, long size)
{
    struct index_entry idx;

    *buf = '\0';
    if (!get_index(tcs->masterfd, idxid, &idx, true))
        return false;

    return retrieve(tcs, idxid, &idx, tag, buf, size);
}

void tagcache_search_finish(struct tagcache_search *tcs)
{
    int i;

    if (!tcs->initialized)
        return;

    if (tcs->masterfd >= 0)
    {
        close(tcs->masterfd);
        tcs->masterfd = -1;
    }

    for (i = 0; i < TAG_COUNT; i++)
    {
        if (tcs->idxfd[i] >= 0)
        {
            close(tcs->idxfd[i]);
            tcs->idxfd[i] = -1;
        }
    }

    tcs->ramsearch = false;
    tcs->valid = false;
    tcs->initialized = 0;
    if (write_lock > 0)
        write_lock--;
}


#ifndef __PCTOOL__
/* a song's tag_date into the room left of id3v2buf, as Track Info shows a
   file's date */
static void fill_date(struct mp3entry *id3, char *buf, long remaining,
                      long date)
{
    if (date > 0 && remaining > 1)
    {
        tagcache_date_text(buf, remaining, date);
        id3->year_string = buf;
    }
}
#endif

#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
static struct tagfile_entry *get_tag(const struct index_entry *entry, int tag)
{
    return (struct tagfile_entry *)&tcramcache.hdr->tags[tag][entry->tag_seek[tag]];
}

static long get_tag_numeric(const struct index_entry *entry, int tag, int idx_id)
{
    return check_virtual_tags(tag, idx_id, entry);
}

static char* get_tag_string(const struct index_entry *entry, int tag)
{
    char* s = get_tag(entry, tag)->tag_data;
    return strcmp(s, UNTAGGED) ? s : NULL;
}

/* The rating of a file, from the database in RAM, or 0: it's not in the file */
int tagcache_get_rating(const char *filename)
{
    if (!tc_stat.ready || !tc_stat.ramcache)
        return 0;

    int idx_id = find_entry_ram(filename);
    if (idx_id < 0)
        return 0;

    return get_tag_numeric(&tcramcache.hdr->indices[idx_id], tag_rating, idx_id);
}

bool tagcache_fill_tags(struct mp3entry *id3, const char *filename)
{
    struct index_entry *entry;
    int idx_id;

    if (!tc_stat.ready || !tc_stat.ramcache)
        return false;

    /* Find the corresponding entry in tagcache. */

    if (filename != NULL)
        memset(id3, 0, sizeof(struct mp3entry));
    else /* Note: caller clears id3 prior to call */
        filename = id3->path;

    idx_id = find_entry_ram(filename);
    if (idx_id < 0)
        return false;

    /* Not in id3->path, which being set says all of this is what the file said */
    id3->path_codectype = probe_file_format(filename);

    entry = &tcramcache.hdr->indices[idx_id];

    char* buf = id3->id3v2buf;
    ssize_t remaining = sizeof(id3->id3v2buf);

    /* this macro sets id3 strings by copying to the id3v2buf */
#define SET(x, y) do                                                           \
    {                                                                          \
        if (remaining > 0)                                                     \
        {                                                                      \
            x          = NULL; /* initialize with null if tag doesn't exist */ \
            char* src = get_tag_string(entry, y);                              \
            if (src)                                                           \
            {                                                                  \
                x = buf;                                                       \
                size_t len = strlcpy(buf, src, remaining) +1;                  \
                buf += len; remaining -= len;                                  \
            }                                                                  \
        }                                                                      \
    } while(0)


    SET(id3->title,         tag_title);
    SET(id3->artist,        tag_artist);
    SET(id3->album,         tag_album);
    SET(id3->genre_string,  tag_genre);
    SET(id3->composer,      tag_composer);
    SET(id3->comment,       tag_comment);
    SET(id3->albumartist,   tag_albumartist);
    SET(id3->grouping,      tag_grouping);

    id3->length     = get_tag_numeric(entry, tag_length, idx_id);
    id3->playcount  = get_tag_numeric(entry, tag_playcount, idx_id);
    id3->rating     = get_tag_numeric(entry, tag_rating, idx_id);
    id3->lastplayed = get_tag_numeric(entry, tag_lastplayed, idx_id);
    id3->score      = get_tag_numeric(entry, tag_virt_autoscore, idx_id) / 10;
    id3->year       = get_tag_numeric(entry, tag_year, idx_id);
    fill_date(id3, buf, remaining, get_tag_numeric(entry, tag_date, idx_id));

    id3->discnum = get_tag_numeric(entry, tag_discnumber, idx_id);
    id3->tracknum = get_tag_numeric(entry, tag_tracknumber, idx_id);
    id3->bitrate = get_tag_numeric(entry, tag_bitrate, idx_id);
    if (id3->bitrate == 0)
        id3->bitrate = 1;

    if (itunesdb_resumes(filename))
    {
        id3->elapsed = get_tag_numeric(entry, tag_lastelapsed, idx_id);
        logf("tagcache_fill_tags: Set elapsed for %s to %lX\n",
             id3->title, id3->elapsed);

        id3->offset = get_tag_numeric(entry, tag_lastoffset, idx_id);
        logf("tagcache_fill_tags: Set offset for %s to %lX\n",
             id3->title, id3->offset);
    }

    return true;
}
#endif /* defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE) */

#ifndef __PCTOOL__
/* The words of a file the file itself has none of, from the database: for
   one iTunesDB lists, what it says, as the stock firmware shows it. iOpenPod
   leaves the files it converts without tags. The rest of id3 stays the file's.
   The buffering thread calls it, which must never wait for a commit: one
   taking the audio buffer waits for that thread, through the audio thread */
bool tagcache_fill_text(struct mp3entry *id3, const char *filename)
{
    static const int tags[] = {
        tag_title, tag_artist, tag_album, tag_genre,
        tag_composer, tag_comment, tag_albumartist, tag_grouping
    };
    char **fields[] = {
        &id3->title, &id3->artist, &id3->album, &id3->genre_string,
        &id3->composer, &id3->comment, &id3->albumartist, &id3->grouping
    };
    long year, date, disc, track;
#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
    /* from memory alone, which a commit unloads before it starts */
    if (!tc_stat.ready || !tc_stat.ramcache)
        return false;
    int idx_id = find_entry_ram(filename);
    if (idx_id < 0)
        return false;
    const struct index_entry *entry = &tcramcache.hdr->indices[idx_id];
#else
    /* the simulator's, without the database in memory: from the disk,
       unless a commit has it */
    struct tagcache_search tcs;
    if (read_lock || !tagcache_find_index(&tcs, filename))
        return false;
#endif

    /* what else of the file's could be in id3v2buf, written over */
    id3->disc_string = id3->track_string = id3->year_string = NULL;
    id3->mb_track_id = NULL;
    char *buf = id3->id3v2buf;
    long remaining = sizeof(id3->id3v2buf);
    for (unsigned i = 0; i < ARRAYLEN(tags); i++)
    {
        *fields[i] = NULL;
        if (remaining <= 1)
            continue;
#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
        const char *src = get_tag_string(entry, tags[i]);
        if (!src)
            continue;
        strlcpy(buf, src, remaining);
#else
        if (!tagcache_retrieve(&tcs, tcs.idx_id, tags[i], buf, remaining)
            || !strcmp(buf, UNTAGGED))
            continue;
#endif
        if (!*buf)
            continue;
        *fields[i] = buf;
        long len = strlen(buf) + 1;
        buf += len;
        remaining -= len;
    }
#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
    year = get_tag_numeric(entry, tag_year, idx_id);
    date = get_tag_numeric(entry, tag_date, idx_id);
    disc = get_tag_numeric(entry, tag_discnumber, idx_id);
    track = get_tag_numeric(entry, tag_tracknumber, idx_id);
#else
    year = tagcache_get_numeric(&tcs, tag_year);
    date = tagcache_get_numeric(&tcs, tag_date);
    disc = tagcache_get_numeric(&tcs, tag_discnumber);
    track = tagcache_get_numeric(&tcs, tag_tracknumber);
    tagcache_search_finish(&tcs);
#endif
    if (!id3->year && year > 0)
    {
        id3->year = year;
        fill_date(id3, buf, remaining, date);
    }
    if (!id3->discnum && disc > 0)
        id3->discnum = disc;
    if (id3->tracknum <= 0 && track > 0)
        id3->tracknum = track;
    return true;
}
#endif

#ifdef TAGCACHE_SORT_FIELDS
/* The Sort fields the scan finds in the tags of songs outside Finder's
   folder, for the commit (load_sort_pairs()): in a file of their own made
   with the first, through a buffer, its header written as the scan's is */
static int sort_scan_fd = -1;
static struct tagcache_sort_header sort_scan_header;
static unsigned char sort_scan_buf[1024];
static size_t sort_scan_used;

static bool sort_scan_flush(void)
{
    bool ok = !sort_scan_used
        || write(sort_scan_fd, sort_scan_buf, sort_scan_used)
           == (ssize_t)sort_scan_used;
    sort_scan_used = 0;
    return ok;
}

static void sort_scan_put(const void *data, size_t len)
{
    if (sort_scan_used + len > sizeof(sort_scan_buf) && !sort_scan_flush())
    {
        scan_write_failed();
        return;
    }
    memcpy(sort_scan_buf + sort_scan_used, data, len);
    sort_scan_used += len;
    sort_scan_header.crc = crc_32(data, len, sort_scan_header.crc);
}

/* the Sort field of a song's name for a tag, kept if it sorts otherwise */
static void sort_scan_add(int tag, const char *name, const char *value)
{
    if (!name || !value || !*name || !*value)
        return;
    const char *a = sortkey_start(name, true), *b = sortkey_start(value, false);
    if (sortkey_group(a) == sortkey_group(b) && !sortkey_compare_from(a, b))
        return;
    unsigned char head[TAGCACHE_SORT_PAIR_HEAD];
    const size_t name_size = strlen(name) + 1, value_size = strlen(value) + 1;
    if (!tagcache_sort_pair_head(head, tag, name_size, value_size))
        return;
    if (sort_scan_fd < 0)
    {
        sort_scan_fd = open_db_fd(TAGCACHE_FILE_SORT_SCAN,
                                  O_WRONLY | O_CREAT | O_TRUNC);
        if (sort_scan_fd < 0)
            return;
        sort_scan_header = (struct tagcache_sort_header){
            TAGCACHE_SORT_MAGIC, TAGCACHE_SORT_VERSION, 0, 0xffffffff, 0, 0
        };
        /* counted at the end: none till then, if cut short */
        struct tagcache_sort_header none = sort_scan_header;
        none.magic = 0;
        write(sort_scan_fd, &none, sizeof(none));
        sort_scan_used = 0;
    }
    sort_scan_put(head, sizeof(head));
    sort_scan_put(name, name_size);
    sort_scan_put(value, value_size);
    sort_scan_header.count++;
}

/* at the end of the scan, with its file of new entries */
static void sort_scan_close(void)
{
    if (sort_scan_fd < 0)
        return;
    if (!sort_scan_flush() || lseek(sort_scan_fd, 0, SEEK_SET) != 0
        || write(sort_scan_fd, &sort_scan_header, sizeof(sort_scan_header))
           != sizeof(sort_scan_header))
        scan_write_failed();
    close(sort_scan_fd);
    sort_scan_fd = -1;
}

/* A song's Sort fields for each tag the database has its names in, as
   add_tagcache() takes them: the album artist is the artist's where there's
   none, and so the canonical artist the album artist's */
static void sort_scan_song(const struct mp3entry *id3, bool has_artist,
                           bool albumartist_is_artist)
{
    sort_scan_add(tag_artist, id3->artist, id3->artist_sort);
    sort_scan_add(tag_albumartist, id3->albumartist, albumartist_is_artist
                  ? id3->artist_sort : id3->albumartist_sort);
    if (has_artist)
        sort_scan_add(tag_virt_canonicalartist, id3->artist, id3->artist_sort);
    else
        sort_scan_add(tag_virt_canonicalartist, id3->albumartist,
                      id3->albumartist_sort);
    sort_scan_add(tag_album, id3->album, id3->album_sort);
    sort_scan_add(tag_title, id3->title, id3->title_sort);
    sort_scan_add(tag_composer, id3->composer, id3->composer_sort);
}
#endif /* TAGCACHE_SORT_FIELDS */

/* The scan's file of new entries, its header written once they're counted.
   The check outside Finder's folder makes it only for one it finds: made and
   removed again at every USB connection, it wrote to the disk for nothing. */
static bool open_scan_temp(void)
{
#ifdef TAGCACHE_SORT_FIELDS
    remove_db_file(TAGCACHE_FILE_SORT_SCAN); /* of another scan's, cut short */
#endif
    cachefd = open_db_fd(TAGCACHE_FILE_TEMP, O_RDWR | O_CREAT | O_TRUNC);
    if (cachefd < 0)
    {
        logf("master file open failed: %s", TAGCACHE_FILE_TEMP);
        scan_write_failed();
        return false;
    }
    struct tagcache_header header;
    memset(&header, 0, sizeof(struct tagcache_header));
    if (write(cachefd, &header, sizeof(struct tagcache_header))
        != sizeof(struct tagcache_header))
    {
        scan_write_failed();
        close(cachefd);
        cachefd = -1;
        remove_scan_files();
        return false;
    }
    tagfile_wbuf_used = 0;
    return true;
}

static inline void write_item(const char *item)
{
    int len = strlen(item) + 1;

    data_size += len;
    scan_put(item, len);
}

static int check_if_empty(char **tag)
{
    int length;

    if (*tag == NULL || **tag == '\0')
    {
        *tag = UNTAGGED;
        return sizeof(UNTAGGED); /* Tag length */
    }

    length = strlen(*tag);
    if (length > TAG_MAXLEN)
    {
        logf("over length tag: %s", *tag);
        length = TAG_MAXLEN;
        str_setlen((*tag), length);
    }

    return length + 1;
}

#ifdef __PCTOOL__
static void db_log(const char *prefix, const char *msg)
{
    /* Crude logging for the sim - to aid in debugging */
    int logfd = open(ROCKBOX_DIR "/database.log",
                     O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (logfd >= 0)
    {
        write(logfd, prefix, strlen(prefix));
        write(logfd, ": ", 2);
        write(logfd, msg, strlen(msg));
        write(logfd, "\n", 1);
        close(logfd);
    }
}
#endif

#ifndef __PCTOOL__
/* While the database is built, the files it already has, by a hash of their
 * path, and its index open. Without them every file is looked up by reading
 * the filename file through, which takes about a second for a library of
 * 13k files on the iPod, and the index is opened for each of them. */
static int build_names_handle;
static long build_names_count;
static bool build_names_timed; /* the names have the times of their entries */
/* the database in memory has every name, which the scan looks up first */
static bool build_names_in_ram;
static int build_masterfd = -1;
#endif /* !__PCTOOL__ */

/* the import after a sync, for tagcache_get_import() */
static bool sync_found;
static volatile int import_phase;
static volatile int import_done, import_total;
#ifdef HAVE_TC_RAMCACHE
/* the database's thread is past its start: the database checked, imported if
   synced, and room taken for it in memory */
static volatile bool thread_started = false;
#endif

#ifndef __PCTOOL__

static int cmp_path_idx(const void *a, const void *b)
{
    long x = ((const struct path_name *)a)->idx_id;
    long y = ((const struct path_name *)b)->idx_id;
    return (x > y) - (x < y);
}

/* The times of the entries into their names, from the master file read in
   order through buf: the scan read the entry of each file it knew to tell
   whether the file had changed, 0.2 ms each when the database wasn't in
   memory. False unless every name got its time. */
static bool load_build_times(struct path_name *names, long count, char *buf,
                             size_t size)
{
    struct master_header mh;
    int fd = open_master_fd(&mh, false);
    if (fd < 0)
        return false;
    qsort_yielding(names, count, sizeof(*names), cmp_path_idx);
    struct index_entry *e = (struct index_entry *)buf;
    long per = size / sizeof(struct index_entry), k = 0;
    bool ok = true;
    for (long i = 0; ok && i < mh.tch.entry_count && k < count; i += per)
    {
        long n = MIN(per, mh.tch.entry_count - i);
        if (read_index_entries(fd, e, n) != (ssize_t)(n * sizeof(*e)))
            ok = false;
        for (long j = 0; ok && j < n && k < count; j++)
        {
            if (names[k].idx_id != i + j)
                continue;
            /* the scan doesn't take a deleted one as known */
            if (e[j].flag & FLAG_DELETED)
                ok = false;
            names[k++].mtime = e[j].tag_seek[tag_mtime];
            if (k < count && names[k].idx_id <= i + j)
                ok = false; /* the same entry twice, or out of order */
        }
        yield();
    }
    close(fd);
    return ok && k == count;
}

/* reads the rest of the filename file, fd just past its header */
static void load_build_names(int fd, long entry_count)
{
    enum { CHUNK = 32768 };
    if (fd < 0 || entry_count <= 0 || tc_stat.econ)
        return;
#ifndef __PCTOOL__
    /* not while music plays and its buffer has the memory: taken, it stopped
       the music and started it again; the scan goes the slow way then */
    if ((audio_status() & AUDIO_STATUS_PLAY)
        && core_allocatable() < entry_count * sizeof(struct path_name) + CHUNK)
        return;
#endif
    int handle = core_alloc(entry_count * sizeof(struct path_name)
                            + CHUNK + TAG_MAXLEN + sizeof(struct tagfile_entry));
    if (handle <= 0)
        return;
    core_pin(handle);
    struct path_name *names = core_get_data(handle);
    char *buf = (char *)(names + entry_count);
    long count = 0;
    size_t have = 0;
    bool ok = true;
    for (;;)
    {
        ssize_t got = read(fd, buf + have, CHUNK);
        if (got < 0)
            ok = false;
        if (got <= 0)
            break;
        have += got;
        size_t pos = 0;
        while (have - pos >= sizeof(struct tagfile_entry))
        {
            /* copied: an entry needn't start on a word */
            struct tagfile_entry e;
            memcpy(&e, buf + pos, sizeof(e));
            size_t len = e.tag_length;
            const char *name = buf + pos + sizeof(e);
            if (len > TAG_MAXLEN || e.idx_id < 0 || count >= entry_count)
            {
                ok = false;
                break;
            }
            if (have - pos < sizeof(e) + len)
                break;
            /* a removed file's name is blanked */
            if (len > 0 && name[0] != '\0' && memchr(name, '\0', len))
            {
                names[count].hash = itunesdb_path_hash(name);
                names[count].idx_id = e.idx_id;
                count++;
            }
            pos += sizeof(e) + len;
        }
        if (!ok)
            break;
        memmove(buf, buf + pos, have - pos);
        have -= pos;
        yield();
    }
    lseek(fd, sizeof(struct tagcache_header), SEEK_SET);
    if (!ok || have != 0 || count == 0)
    {
        core_unpin(handle);
        core_free(handle);
        return;
    }
    build_names_timed = load_build_times(names, count, buf,
                                         CHUNK + TAG_MAXLEN
                                         + sizeof(struct tagfile_entry));
    qsort_yielding(names, count, sizeof(*names), cmp_path_name);
    core_unpin(handle);
    core_shrink(handle, names, count * sizeof(struct path_name));
    build_names_handle = handle;
    build_names_count = count;
}

static void free_build_names(void)
{
    build_names_timed = false;
    build_names_in_ram = false;
    if (build_names_handle > 0)
        core_free(build_names_handle);
    build_names_handle = 0;
    build_names_count = 0;
    if (build_masterfd >= 0)
        close(build_masterfd);
    build_masterfd = -1;
}

/* the index of the database's entry for the file at path, or -1, and its
   time when *timed */
static long find_known_file(const char *path, uint32_t *mtime, bool *timed)
{
    *timed = false;
    if (build_names_in_ram)
        return -1; /* not in memory, so new */
    if (build_names_handle <= 0)
        return filenametag_fd >= 0 ? find_entry_disk(path, false) : -1;
    *timed = build_names_timed;
    return find_path_name(build_names_handle, build_names_count, path, mtime);
}
#endif /* !__PCTOOL__ */


#ifdef TAGCACHE_RESTORE
#define RESTORE_MAGIC 0x4f445752 /* "RWDO" */
struct restore_mark {
    uint32_t magic;
    /* of the iTunesDB the old counts were imported from, 0 if none */
    uint32_t db_size, db_mtime;
    int32_t serial, commitid;
};
static struct restore_mark restore_from;
static bool restore_pending;

/* same set as resurrection in build_numeric_indices() */
static const unsigned char restore_tags[] = {
    tag_playcount, tag_rating, tag_playtime, tag_lastplayed, tag_commitid,
    tag_lastelapsed, tag_lastoffset, tag_dateadded
};
#define RESTORE_TAGS ARRAYLEN(restore_tags)

/* every live entry, zeros too: a rating cleared on the iPod stays cleared */
struct restore_entry {
    uint64_t hash;
    int32_t value[RESTORE_TAGS];
};
#define RESTORE_CHUNK 32768
static int restore_handle;
static long restore_count;
/* the same iTunesDB as the old import, or none: the old counts are right */
static bool restore_finder;

/* a restorable master of another format, its header as this format's; the
   layout must match the file size; native byte order only */
static bool old_master(int fd, struct master_header *mh)
{
    const int32_t min_entry = (tag_lastoffset + 2) * sizeof(int32_t);
    const int32_t old_header = offsetof(struct master_header, header_size);
    memset(mh, 0, sizeof(*mh));
    const ssize_t got = read(fd, mh, sizeof(*mh));
    const uint32_t magic = mh->tch.magic;
    if ((magic == TAGCACHE_MAGIC_ROCKWHEEL || magic == TAGCACHE_MAGIC_ROCKBOX)
        && got >= old_header)
    {
        /* without the sizes, whose words are the first entry's */
        mh->header_size = old_header;
        mh->entry_size = min_entry;
    }
    else if ((magic & ~0xffu) != (TAGCACHE_MAGIC & ~0xffu)
             || magic == TAGCACHE_MAGIC || got != sizeof(*mh)
             || mh->header_size < (int32_t)sizeof(*mh))
        return false;
    const off_t size = ffilesize(fd);
    return mh->entry_size >= min_entry
        && mh->entry_size % sizeof(int32_t) == 0
        && mh->entry_size <= RESTORE_CHUNK && mh->tch.entry_count >= 0
        && size >= mh->header_size
        && mh->tch.entry_count <= (size - mh->header_size) / mh->entry_size
        && size == mh->header_size + (off_t)mh->tch.entry_count * mh->entry_size;
}

static void restore_read_mark(void)
{
    memset(&restore_from, 0, sizeof(restore_from));
    const int fd = open_db_fd(TAGCACHE_FILE_OLD_MARK, O_RDONLY);
    restore_pending = fd >= 0;
    if (fd < 0)
        return;
    /* unreadable: Finder counts from iTunesDB */
    if (read(fd, &restore_from, sizeof(restore_from)) != sizeof(restore_from)
        || restore_from.magic != RESTORE_MAGIC)
        memset(&restore_from, 0, sizeof(restore_from));
    close(fd);
}

bool tagcache_restore_finder(void)
{
    return restore_pending && restore_finder;
}

bool tagcache_restoring(void)
{
    return restore_pending;
}

static bool restoring(void)
{
    return restore_pending;
}

static int cmp_restore(const void *a, const void *b)
{
    const uint64_t x = ((const struct restore_entry *)a)->hash;
    const uint64_t y = ((const struct restore_entry *)b)->hash;
    return (x > y) - (x < y);
}

static void restore_hash_names(uint64_t *hashes, long entries, char *buf)
{
    memset(hashes, 0, entries * sizeof(*hashes));
    const int fd = open_db_fd(TAGCACHE_FILE_OLD_NAMES, O_RDONLY);
    if (fd < 0)
        return;
    size_t have = 0;
    bool ok = lseek(fd, sizeof(struct tagcache_header), SEEK_SET)
              == sizeof(struct tagcache_header);
    while (ok)
    {
        const ssize_t got = read(fd, buf + have, RESTORE_CHUNK);
        if (got <= 0)
            break;
        have += got;
        size_t pos = 0;
        while (have - pos >= sizeof(struct tagfile_entry))
        {
            /* copied: an entry needn't start on a word */
            struct tagfile_entry e;
            memcpy(&e, buf + pos, sizeof(e));
            const size_t len = e.tag_length;
            const char *name = buf + pos + sizeof(e);
            if (len > TAG_MAXLEN)
            {
                ok = false;
                break;
            }
            if (have - pos < sizeof(e) + len)
                break;
            /* a removed file's name is blanked; one past the entries is of a
               commit cut short */
            if (e.idx_id >= 0 && e.idx_id < entries && len > 0
                && name[0] != '\0' && memchr(name, '\0', len))
                hashes[e.idx_id] = itunesdb_path_hash(name);
            pos += sizeof(e) + len;
        }
        memmove(buf, buf + pos, have - pos);
        have -= pos;
        yield();
    }
    close(fd);
}

/* the old master's live entries with a name into table, only those with
   statistics if stats_only; how many, 0 on a read error; NULL counts only */
static long restore_read_entries(int fd, const struct master_header *mh,
                                 const uint64_t *hashes, char *buf,
                                 struct restore_entry *table, bool stats_only)
{
    const long per = RESTORE_CHUNK / mh->entry_size;
    const int flag = mh->entry_size / sizeof(int32_t) - 1;
    long n = 0;
    if (lseek(fd, mh->header_size, SEEK_SET) != mh->header_size)
        return 0;
    for (long i = 0; i < mh->tch.entry_count; i += per)
    {
        const long k = MIN(per, mh->tch.entry_count - i);
        if (read(fd, buf, k * mh->entry_size) != (ssize_t)(k * mh->entry_size))
            return 0;
        for (long j = 0; j < k; j++)
        {
            const int32_t *e = (const int32_t *)(buf + j * mh->entry_size);
            if ((e[flag] & FLAG_DELETED) || hashes[i + j] == 0)
                continue;
            struct restore_entry r = { .hash = hashes[i + j] };
            bool any = false;
            for (unsigned t = 0; t < RESTORE_TAGS; t++)
            {
                /* a tag past an older format's entry, as the date added of
                   format 1's, is none: not its flags or the next entry */
                if (restore_tags[t] < flag)
                    r.value[t] = e[restore_tags[t]];
                /* nearly every song has a date added: those with plays
                   or ratings are the ones kept */
                any |= r.value[t] != 0 && restore_tags[t] != tag_dateadded;
            }
            if (stats_only && !any)
                continue;
            if (table)
                table[n] = r;
            n++;
        }
        yield();
    }
    return n;
}

/* Songs a sync moved, by the import's list: their entries under the new
   path's hash, one move each. The list is sorted by the old hash
   (sort_moved_list()), as the table: one walk. */
static bool restore_moved(struct restore_entry *table, long count)
{
    const int fd = open(ITUNESDB_MOVED_LIST, O_RDONLY);
    if (fd < 0)
        return false;
    struct itunesdb_moved m[16];
    long at = 0;
    bool moved = false;
    ssize_t got;
    while ((got = read(fd, m, sizeof(m))) >= (ssize_t)sizeof(m[0]))
    {
        for (int k = 0; k < got / (ssize_t)sizeof(m[0]); k++)
        {
            while (at < count && table[at].hash < m[k].old_hash)
                at++;
            m[k].path[sizeof(m[k].path) - 1] = '\0';
            for (; at < count && table[at].hash == m[k].old_hash; at++)
            {
                table[at].hash = itunesdb_path_hash(m[k].path);
                moved = true;
            }
        }
    }
    close(fd);
    return moved;
}

/* only for the import's scan, never while music plays: false makes the scan
   wait. Short of memory, only the songs with statistics; then none. */
static bool restore_load(void)
{
    if (!restore_pending || import_phase != TAGCACHE_IMPORT_SCANNING)
        return true;
    if (audio_status() & AUDIO_STATUS_PLAY)
        return false;
    uint32_t size, mtime;
    restore_finder = !itunesdb_stamp(&size, &mtime)
                     || (size == restore_from.db_size
                         && mtime == restore_from.db_mtime);
    struct master_header mh;
    const int fd = open_db_fd(TAGCACHE_FILE_OLD_MASTER, O_RDONLY);
    if (fd < 0)
        return true;
    const long entries = old_master(fd, &mh) ? mh.tch.entry_count : 0;
    const int names = entries > 0 ?
        core_alloc(entries * sizeof(uint64_t) + RESTORE_CHUNK + TAG_MAXLEN
                   + sizeof(struct tagfile_entry)) : 0;
    bool stats_only = false, lost = entries > 0 && names <= 0;
    if (names > 0)
    {
        core_pin(names);
        uint64_t *hashes = core_get_data(names);
        char *buf = (char *)(hashes + entries);
        restore_hash_names(hashes, entries, buf);
        restore_handle = core_alloc(entries * sizeof(struct restore_entry));
        if (restore_handle <= 0)
        {
            stats_only = true;
            const long n = restore_read_entries(fd, &mh, hashes, buf, NULL,
                                                true);
            restore_handle = n > 0 ?
                core_alloc(n * sizeof(struct restore_entry)) : 0;
            lost = n > 0 && restore_handle <= 0;
        }
        if (restore_handle > 0)
        {
            core_pin(restore_handle);
            struct restore_entry *table = core_get_data(restore_handle);
            restore_count = restore_read_entries(fd, &mh, hashes, buf, table,
                                                 stats_only);
            qsort_yielding(table, restore_count, sizeof(*table), cmp_restore);
            /* with the same iTunesDB, the old database has every move */
            if (!restore_finder && restore_moved(table, restore_count))
                qsort_yielding(table, restore_count, sizeof(*table),
                               cmp_restore);
            core_shrink(restore_handle, table,
                        MAX(restore_count, 1) * sizeof(*table));
            core_unpin(restore_handle);
        }
        core_unpin(names);
        core_free(names);
    }
    close(fd);
    if (lost)
        logf("no memory for the old statistics: not restored");
    return true;
}

static void restore_free(void)
{
    if (restore_handle > 0)
        core_free(restore_handle);
    restore_handle = 0;
    restore_count = 0;
}

/* Finder songs' counts and ratings too: the import after sets them, from what
   Finder hasn't had of them */
static void restore_apply(const char *path, struct temp_file_entry *entry)
{
    if (restore_handle <= 0)
        return;
    const uint64_t hash = itunesdb_path_hash(path);
    struct restore_entry *table = core_get_data_pinned(restore_handle);
    struct restore_entry old;
    long lo = 0, hi = restore_count - 1, found = -1;
    while (lo <= hi && found < 0)
    {
        const long mid = (lo + hi) / 2;
        if (table[mid].hash == hash)
            found = mid;
        else if (table[mid].hash < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    if (found >= 0)
        old = table[found];
    core_put_data_pinned(table);
    if (found < 0)
        return;
    entry->flag |= FLAG_RESTORED;
    for (unsigned i = 0; i < RESTORE_TAGS; i++)
    {
        const int tag = restore_tags[i];
        /* a Finder song's is iTunesDB's */
        if (tag == tag_dateadded)
        {
            if (!entry->tag_offset[tag])
                entry->tag_offset[tag] = old.value[i];
            continue;
        }
        entry->tag_offset[tag] = old.value[i];
        if (old.value[i])
            entry->flag |= FLAG_DIRTYNUM;
    }
}

/* go on from the old numbers so caches of the old database go stale */
static void restore_numbers(struct master_header *mh)
{
    if (!restore_pending)
        return;
    mh->serial = restore_from.serial;
    mh->commitid = restore_from.commitid;
}



#endif /* TAGCACHE_RESTORE */

static int month_days(int year, int month)
{
    static const unsigned char days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
        return 29;
    return days[month - 1];
}

/* the number n digits at s write, or -1 if they aren't all digits */
static int date_digits(const char *s, int n)
{
    int value = 0;
    for (int i = 0; i < n; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        value = value * 10 + s[i] - '0';
    }
    return value;
}

/* true if only spaces follow, or with time an ISO time first, as
   "T07:00:00Z" or " 07:00" */
static bool date_ends(const char *s, bool time)
{
    if (time && (*s == 'T' || *s == ' ') && date_digits(s + 1, 2) >= 0
        && s[3] == ':' && date_digits(s + 4, 2) >= 0)
    {
        s += 6;
        while (*s && strchr("0123456789:.+-Z", *s))
            s++;
    }
    while (*s == ' ')
        s++;
    return !*s;
}

/* Text written as yyyy-mm-dd, yyyy/mm/dd or yyyymmdd, any with a time after
   it, or as yyyy-mm, of a year 1000-2999 that the parser took (all of it, as
   atoi() takes yyyymmdd, or none): the date and its year, or the year alone
   where no such date can be, as 2019-02-29. Other text keeps the parser's
   year, but a number past four digits, which is none. */
void tagcache_song_date(long *year, long *date, const char *text)
{
    int y = -1, m = 0, d = 0;
    bool written = false;    /* as a date */
    bool month_only = false; /* as yyyy-mm */
    bool ends = true;        /* with nothing after it but a time */
    *date = 0;
    if (text)
    {
        while (*text == ' ')
            text++;
        y = date_digits(text, 4);
        const char sep = y >= 0 ? text[4] : '\0';
        if ((sep == '-' || sep == '/') && (m = date_digits(text + 5, 2)) >= 0)
        {
            if (text[7] == sep && (d = date_digits(text + 8, 2)) >= 0)
            {
                written = true;
                ends = date_ends(text + 10, true);
            }
            else if (sep == '-')
            {
                d = 0;
                written = month_only = true;
                ends = date_ends(text + 7, false);
            }
        }
        else if (y >= 0 && (m = date_digits(text + 4, 2)) >= 0
                 && (d = date_digits(text + 6, 2)) >= 0)
        {
            written = true;
            ends = date_ends(text + 8, true);
        }
    }
    const long ymd = y * 10000L + m * 100 + d;
    if (written && y >= 1000 && y <= 2999
        && (*year == 0 || *year == y || *year == ymd))
    {
        *year = y;
        if (ends && m >= 1 && m <= 12
            && (month_only || (d >= 1 && d <= month_days(y, m))))
            *date = ymd;
    }
    else if (*year >= 10000)
        *year = 0;
}

void tagcache_date_text(char *buf, size_t size, long date)
{
    if (date % 100)
        snprintf(buf, size, "%04ld-%02ld-%02ld", date / 10000, date / 100 % 100,
                 date % 100);
    else
        snprintf(buf, size, "%04ld-%02ld", date / 10000, date / 100 % 100);
}

/* GCC 3.4.6 for Coldfire can choose to inline this function. Not a good
 * idea, as it uses lots of stack and is called from a recursive function
 * (check_dir).
 */
static void NO_INLINE add_tagcache(char *path, unsigned long mtime)
{
    #define ADD_TAG(entry, tag, data) \
        /* Adding tag */                              \
        entry.tag_length[tag] = check_if_empty(data); \
        entry.tag_offset[tag] = offset;               \
        offset += entry.tag_length[tag]

    struct mp3entry id3;
    struct temp_file_entry entry;
    bool ret;
    int idx_id = -1;
    char tracknumfix[3];
    int offset = 0;
    int path_length = strlen(path);
    bool has_artist;

    DB_LOG("file", path);

    if (cachefd < 0 && !scan_outside)
        return ;

    /* Check for overlength file path. */
    if (path_length > MAX_PATH || path_length > TAG_MAXLEN)
    {
        /* Path can't be shortened. */
        logf("Too long path: %s", path);
        DB_LOG("error", "path too long");
        return ;
    }

    /* Check if the file is supported. */
    int afmt = probe_file_format(path);
    if (afmt == AFMT_UNKNOWN)
        return;
    if (import_phase == TAGCACHE_IMPORT_SCANNING)
        import_done++;

    /* Check if the file is already cached. */
#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
    idx_id = find_entry_ram(path);
#endif

    /* Be sure the entry doesn't exist. */
    uint32_t known_mtime = 0;
    bool timed = false;
#ifndef __PCTOOL__
    if (idx_id < 0)
        idx_id = find_known_file(path, &known_mtime, &timed);
    int masterfd = build_masterfd;
#else
    if (filenametag_fd >= 0 && idx_id < 0)
        idx_id = find_entry_disk(path, false);
    int masterfd = -1;
#endif

    /* Check if file has been modified. */
    if (idx_id >= 0)
    {
        struct index_entry idx;

        /* TODO: Mark that the index exists (for fast reverse scan) */
        //found_idx[idx_id/8] |= idx_id%8;

        if (!timed)
        {
            if (!get_index(masterfd, idx_id, &idx, true))
            {
                logf("failed to retrieve index entry");
                DB_LOG("error", "failed to retrieve index entry");
                return ;
            }
            known_mtime = idx.tag_seek[tag_mtime];
        }

        if ((unsigned long)known_mtime == mtime)
        {
            /* No changes to file. */
            return ;
        }

        /* Metadata might have been changed. Delete the entry. */
        logf("Re-adding: %s", path);
        DB_LOG("info", "re-adding");
        if (!delete_entry(idx_id))
        {
            logf("delete_entry failed: %d", idx_id);
            DB_LOG("error", "delete entry failed");
            return ;
        }
    }

#ifndef __PCTOOL__
    /* the check outside Finder's folder steps its bar by the songs whose tags
       it reads: new, or changed */
    if (scan_outside)
    {
        if (!outside_counted)
            count_outside();
        import_done++;
    }
#endif

    /*memset(&id3, 0, sizeof(struct mp3entry)); -- get_metadata does this for us */
    memset(&entry, 0, sizeof(struct temp_file_entry));
    memset(&tracknumfix, 0, sizeof(tracknumfix));
    int metadata_flags = METADATA_EXCLUDE_ID3_PATH;
#ifdef TAGCACHE_SORT_FIELDS
    /* the Sort fields of Finder's songs are iTunesDB's: those of the others
       are their tags' */
    const bool sort_fields = !itunesdb_synced_path(path);
    if (sort_fields)
        metadata_flags |= METADATA_SORT_FIELDS;
#endif
    long date = 0, added = 0;
    bool finder = false;
#ifndef __PCTOOL__
    /* what iTunesDB says of a file Finder added, as a stock iPod has it */
    struct tagcache_runtime itdb_rt = { 0 };
    ret = itunesdb_get_metadata(path, &id3, &itdb_rt, &date, &added);
    finder = ret;
    if (!ret)
#endif
    ret = get_metadata_afmt(&id3, -1, path, afmt, metadata_flags);

    if (!ret)
    {
        logf("get_metadata failed: %s", path);
        DB_LOG("error", "get_metadata failed");
        return ;
    }

    logf("-> %s", path);

    if (id3.tracknum < 0)              /* Track number missing? */
    {
        id3.tracknum = -1;
    }

    /* Numeric tags */
    long year = id3.year;
    if (!finder)
        tagcache_song_date(&year, &date, id3.year_string);
    entry.tag_offset[tag_year] = year;
    entry.tag_offset[tag_date] = date;
    entry.tag_offset[tag_dateadded] = added;
    entry.tag_offset[tag_discnumber] = id3.discnum;
    entry.tag_offset[tag_tracknumber] = id3.tracknum;
    entry.tag_offset[tag_length] = id3.length;
    entry.tag_offset[tag_bitrate] = id3.bitrate;
    entry.tag_offset[tag_mtime] = mtime;
#ifndef __PCTOOL__
    /* and its counts, which the import would otherwise write to each new
       entry on the disk after the commit */
    entry.tag_offset[tag_playcount] = itdb_rt.playcount;
    entry.tag_offset[tag_rating] = itdb_rt.rating;
    if (itdb_rt.playcount || itdb_rt.rating)
        entry.flag |= FLAG_DIRTYNUM;
#endif
    restore_apply(path, &entry);

    /* String tags. */
    has_artist = id3.artist != NULL
        && strlen(id3.artist) > 0;
    /* an album without an album artist is its track's artist's, as on a stock
       iPod: Cover Flow tells albums apart by name and album artist */
    const bool albumartist_is_artist = has_artist
        && (id3.albumartist == NULL || !strlen(id3.albumartist));
    if (albumartist_is_artist)
        id3.albumartist = id3.artist;

    ADD_TAG(entry, tag_filename, &path);
    ADD_TAG(entry, tag_title, &id3.title);
    ADD_TAG(entry, tag_artist, &id3.artist);
    ADD_TAG(entry, tag_album, &id3.album);
    ADD_TAG(entry, tag_genre, &id3.genre_string);
    ADD_TAG(entry, tag_composer, &id3.composer);
    ADD_TAG(entry, tag_comment, &id3.comment);
    ADD_TAG(entry, tag_albumartist, &id3.albumartist);
    if (has_artist)
    {
        ADD_TAG(entry, tag_virt_canonicalartist, &id3.artist);
    }
    else
    {
        ADD_TAG(entry, tag_virt_canonicalartist, &id3.albumartist);
    }
    ADD_TAG(entry, tag_grouping, &id3.grouping);
    entry.data_length = offset;

    /* the check outside Finder's folder makes the file with its first */
    if (cachefd < 0 && !open_scan_temp())
        return;
#ifdef TAGCACHE_SORT_FIELDS
    if (sort_fields)
        sort_scan_song(&id3, has_artist, albumartist_is_artist);
#endif

    /* Write the header */
    scan_put(&entry, sizeof(struct temp_file_entry));

    /* And tags also... Correct order is critical */
    write_item(path);
    write_item(id3.title);
    write_item(id3.artist);
    write_item(id3.album);
    write_item(id3.genre_string);
    write_item(id3.composer);
    write_item(id3.comment);
    write_item(id3.albumartist);
    if (has_artist)
    {
        write_item(id3.artist);
    }
    else
    {
        write_item(id3.albumartist);
    }
    write_item(id3.grouping);

    total_entry_count++;

    #undef ADD_TAG
}
#endif /*!defined(PLUGIN)*/


/* the crc of a name in lower case, in one call rather than one for each
   character: by which the commit finds a name again, and its Sort field */
static unsigned name_crc(const char *str)
{
    static char lower[TAG_MAXLEN + 1];
    int i;
    for (i = 0; str[i] != '\0' && i < TAG_MAXLEN; i++)
        lower[i] = tolower(str[i]);
    return crc_32(lower, i, 0xffffffff);
}

#ifdef TAGCACHE_SORT_FIELDS
/* The Sort fields, the names that names sort by as Music has them, while a
   commit loads them: from the files of Finder's, of the files' tags kept and
   of those the scan just found, in this order of precedence. At the end of
   the commit's buffer, below the master file, sorted to be looked up by tag
   and crc. */
enum { SORT_FROM_FINDER, SORT_FROM_SCAN, SORT_FROM_FILES };
struct sort_pair {
    uint32_t crc;       /* of the name, name_crc()'s */
    const char *name;
    const char *value;
    uint8_t tag;
    uint8_t from;
    bool used;          /* its name is still one of its tag's */
};
static struct sort_pair *sort_pairs;
static long sort_pair_count;
static size_t sort_values_size;  /* of their Sort fields, for the buffer */
static uint32_t sort_finder_crc; /* of Finder's file, the commit's order mark's */
static bool sort_files_kept;     /* those of files' tags were read */
static int sort_build_tag = -1;  /* the tag build_index() puts in, or -1 */

/* the value of the name for the tag the commit builds, NULL for none */
static const char *sort_value(unsigned crc, const char *name)
{
    long lo = 0, hi = sort_pair_count;
    while (lo < hi)
    {
        const long mid = (lo + hi) / 2;
        const struct sort_pair *p = &sort_pairs[mid];
        if (p->tag < sort_build_tag || (p->tag == sort_build_tag && p->crc < crc))
            lo = mid + 1;
        else
            hi = mid;
    }
    const char *value = NULL;
    for (; lo < sort_pair_count && sort_pairs[lo].tag == sort_build_tag
           && sort_pairs[lo].crc == crc; lo++)
    {
        if (strcasecmp(sort_pairs[lo].name, name))
            continue;
        sort_pairs[lo].used = true;
        if (!value)
            value = sort_pairs[lo].value;
    }
    return value;
}

/* the place of a tag among those of Quick Scroll's letters, -1 for one of
   no list's */
static int initial_slot(int tag)
{
    for (size_t i = 0; i < INITIAL_TAGS; i++)
    {
        if (initial_tags[i] == tag)
            return i;
    }
    return -1;
}

/* The commit writes a name of the tag of that slot at seek, its letter that,
   0 for none: noted if it's another than the one before. In the last room
   left, that the letters from there on aren't known. */
static void initial_note(int slot, int32_t seek, ucschar_t letter)
{
    const int n = initials.count[slot];
    if (letter > 0xffff)
        letter = 0; /* beyond what's noted */
    if (n == INITIALS_PER_TAG || (n > 0 && initials.letter[slot][n - 1] == letter))
        return;
    initials.seek[slot][n] = seek;
    initials.letter[slot][n] = n == INITIALS_PER_TAG - 1 ? 0 : letter;
    initials.count[slot] = n + 1;
}
#endif /* TAGCACHE_SORT_FIELDS */

static bool tempbuf_insert(char *str, int id, int idx_id, bool unique)
{
    struct tempbuf_searchidx *index = (struct tempbuf_searchidx *)tempbuf;
    int len = strlen(str)+1;
    int i;
    unsigned *crcbuf = (unsigned *)&tempbuf[tempbuf_size-4];
    const unsigned crc32 = name_crc(str);

    if (unique)
    {
        /* Check if the crc does not exist -> entry does not exist for sure. */
        for (i = crc_heads[crc32 & crc_mask]; i >= 0; i = crc_next[i])
        {
            if (crcbuf[-i] != crc32)
                continue;

            if (!strcasecmp(str, index[i].str))
            {
                if (id < 0 || id >= lookup_buffer_depth)
                {
                    logf("lookup buf overf.: %d", id);
                    return false;
                }

                lookup[id] = &index[i];
                return true;
            }
        }
    }

    /* Insert to CRC buffer. */
    crcbuf[-tempbufidx] = crc32;
    tempbuf_left -= 4;

    /* Insert it to the buffer. */
    tempbuf_left -= len;
    if (tempbuf_left - 4 < 0 || tempbufidx >= commit_entry_count)
    {
        logf("temp buf error rem: %ld idx: %ld / %ld",
             tempbuf_left, tempbufidx, commit_entry_count-1);
        return false;
    }
    if (id >= lookup_buffer_depth)
    {
        logf("lookup buf overf. #2: %d", id);
        return false;
    }

    if (id >= 0)
    {
        lookup[id] = &index[tempbufidx];
        index[tempbufidx].idlist.id = id;
    }
    else
        index[tempbufidx].idlist.id = -1;

    index[tempbufidx].idlist.next = NULL;
    index[tempbufidx].idx_id = idx_id;
    index[tempbufidx].seek = -1;
    index[tempbufidx].str = &tempbuf[tempbuf_pos];
    memcpy(index[tempbufidx].str, str, len);
    tempbuf_pos += len;

    /* what it sorts by: its Sort field, copied after it, as Music takes
       one, with its article; or itself without its own */
    const char *key;
    if (!strcmp(str, UNTAGGED))
    {
        key = index[tempbufidx].str;
        index[tempbufidx].group = GROUP_UNTAGGED;
    }
    else
    {
        bool article = true;
        key = index[tempbufidx].str;
#ifdef TAGCACHE_SORT_FIELDS
        const char *value = sort_build_tag >= 0 ? sort_value(crc32, str) : NULL;
        const int value_len = value ? (int)strlen(value) + 1 : 0;
        if (value && tempbuf_left - value_len - 4 >= 0)
        {
            key = memcpy(&tempbuf[tempbuf_pos], value, value_len);
            tempbuf_pos += value_len;
            tempbuf_left -= value_len;
            article = false;
        }
#endif
        key = sortkey_start(key, article);
        index[tempbufidx].group = 1 + sortkey_group(key);
    }
    index[tempbufidx].key = key - index[tempbufidx].str;
    crc_next[tempbufidx] = crc_heads[crc32 & crc_mask];
    crc_heads[crc32 & crc_mask] = tempbufidx;
    tempbufidx++;

    return true;
}

/* by the rules of sortkey.c, <Untagged> first */
static int compare(const void *p1, const void *p2)
{
    do_timed_yield();

    const struct tempbuf_searchidx *e1 = p1, *e2 = p2;
    if (e1->group != e2->group)
        return e1->group < e2->group ? -1 : 1;
    if (e1->group == GROUP_UNTAGGED)
        return 0;
    const int d = sortkey_compare_from(e1->str + e1->key, e2->str + e2->key);
    return d ? d : strcmp(e1->str, e2->str);
}

/* The old entries, the first old_n, came in sorted from the old tag file:
   the new ones are sorted alone and merged in. Sorting all again was the
   largest part of a commit. False when the old ones aren't sorted. */
static bool merge_sorted(struct tempbuf_searchidx *index, long old_n)
{
    long new_n = tempbufidx - old_n;
    if (old_n == 0)
        return false;
    for (long i = 1; i < old_n; i++)
    {
        if (compare(&index[i - 1], &index[i]) > 0)
            return false;
    }
    if (new_n <= 0)
        return true;

    long pos = tempbuf_pos, left = tempbuf_left;
    ALIGN_BUFFER(pos, left, alignof(struct tempbuf_searchidx));
    if (left < (long)(new_n * sizeof(*index)))
        return false;
    struct tempbuf_searchidx *added = (struct tempbuf_searchidx *)&tempbuf[pos];

    qsort(&index[old_n], new_n, sizeof(*index), compare);
    memcpy(added, &index[old_n], new_n * sizeof(*index));
    /* from the end, an old one before a new one it equals */
    long i = old_n - 1, j = new_n - 1, k = tempbufidx - 1;
    while (j >= 0)
    {
        if (i >= 0 && compare(&index[i], &added[j]) > 0)
            index[k--] = index[i--];
        else
            index[k--] = added[j--];
    }
    return true;
}

static int tempbuf_sort(int fd, long old_n)
{
    struct tempbuf_searchidx *index = (struct tempbuf_searchidx *)tempbuf;
    struct tagfile_entry fe;
    int i;
    int length;

    /* Generate reverse lookup entries. */
    for (i = 0; i < lookup_buffer_depth; i++)
    {
        struct tempbuf_id_list *idlist;

        if (!lookup[i])
            continue;

        if (lookup[i]->idlist.id == i)
            continue;

        ALIGN_BUFFER(tempbuf_pos, tempbuf_left, alignof(struct tempbuf_id_list));
        tempbuf_left -= sizeof(struct tempbuf_id_list);
        if (tempbuf_left < 0)
            return -1;

        /* after the head: the order doesn't matter, and walking to the end
           was quadratic */
        struct tempbuf_id_list *head = &lookup[i]->idlist;
        idlist = (struct tempbuf_id_list *)&tempbuf[tempbuf_pos];
        tempbuf_pos += sizeof(struct tempbuf_id_list);
        idlist->id = i;
        idlist->next = head->next;
        head->next = idlist;

        do_timed_yield();
    }

    if (!merge_sorted(index, old_n))
        qsort(index, tempbufidx, sizeof(struct tempbuf_searchidx), compare);
    memset(lookup, 0, lookup_buffer_depth * sizeof(struct tempbuf_searchidx **));

#ifdef TAGCACHE_SORT_FIELDS
    const int initials_of = initial_slot(sort_build_tag);
    if (initials_of >= 0)
        initials.count[initials_of] = 0;
#endif
    off_t pos = lseek(fd, 0, SEEK_CUR);
    tagfile_wbuf_used = 0;
    for (i = 0; i < tempbufidx; i++)
    {
        struct tempbuf_id_list *idlist = &index[i].idlist;

        /* Fix the lookup list. */
        while (idlist != NULL)
        {
            if (idlist->id >= 0)
                lookup[idlist->id] = &index[i];
            idlist = idlist->next;
        }

        index[i].seek = pos;
#ifdef TAGCACHE_SORT_FIELDS
        if (initials_of >= 0)
            initial_note(initials_of, pos, index[i].group == GROUP_UNTAGGED ? 0
                         : sortkey_initial(index[i].str + index[i].key));
#endif
        length = strlen(index[i].str) + 1;
        fe.tag_length = length;
        fe.idx_id = index[i].idx_id;

        /* Check the chunk alignment. */
        if ((fe.tag_length + sizeof(struct tagfile_entry))
            % TAGFILE_ENTRY_CHUNK_LENGTH)
        {
            fe.tag_length += TAGFILE_ENTRY_CHUNK_LENGTH -
                ((fe.tag_length + sizeof(struct tagfile_entry))
                 % TAGFILE_ENTRY_CHUNK_LENGTH);
        }
        pos += sizeof(struct tagfile_entry) + fe.tag_length;

        struct tagfile_entry e = fe;
        swap_tagfile_entry(&e);
        if (!tagfile_wbuf_put(fd, &e, sizeof(e)))
        {
            logf("tempbuf_sort: write error #1");
            return -1;
        }

        if (!tagfile_wbuf_put(fd, index[i].str, length))
        {
            logf("tempbuf_sort: write error #2");
            return -2;
        }

        /* Write some padding. */
        if (fe.tag_length - length > 0)
            tagfile_wbuf_put(fd, "XXXXXXXX", fe.tag_length - length);

        do_timed_yield();
    }

    if (!tagfile_wbuf_flush(fd))
    {
        logf("tempbuf_sort: write error #3");
        return -3;
    }

    return i;
}

inline static struct tempbuf_searchidx* tempbuf_locate(int id)
{
    if (id < 0 || id >= lookup_buffer_depth)
        return NULL;

    return lookup[id];
}


inline static int tempbuf_find_location(int id)
{
    struct tempbuf_searchidx *entry;

    entry = tempbuf_locate(id);
    if (entry == NULL)
        return -1;

    return entry->seek;
}

/* Files read in order go through this, a block at a time: a read() for
   each part of each entry cost the iPod about as much for a few bytes as
   for a few KB. The temporary file of a commit took 2 s for 1200 new songs,
   loading the database into memory 1.7 s for 5700 songs. */
struct bufread {
    int fd;
    off_t pos;  /* of buf[0] in the file */
    int len;    /* bytes in buf */
    off_t at;   /* where the next read starts */
    char *buf;
    int size;
};

static char tmpr_buf[16384] STORAGE_ALIGN_ATTR;
static char tagr_buf[16384] STORAGE_ALIGN_ATTR;
static struct bufread tmpr; /* the temporary file in a commit */
static struct bufread tagr; /* a tag file */

static void br_start(struct bufread *br, int fd, off_t at, char *buf,
                     int size)
{
    br->fd = fd;
    br->pos = br->len = 0;
    br->at = at;
    br->buf = buf;
    br->size = size;
}

static ssize_t br_read(struct bufread *br, void *data, size_t len)
{
    size_t done = 0;
    while (done < len)
    {
        if (br->at < br->pos || br->at >= br->pos + br->len)
        {
            br->pos = br->at & ~(off_t)2047;
            br->len = 0;
            if (lseek(br->fd, br->pos, SEEK_SET) != br->pos)
                break;
            ssize_t n = read(br->fd, br->buf, br->size);
            if (n <= br->at - br->pos)
                break;
            br->len = n;
        }
        size_t n = MIN(len - done, (size_t)(br->pos + br->len - br->at));
        memcpy((char *)data + done, br->buf + (br->at - br->pos), n);
        done += n;
        br->at += n;
    }
    return done;
}

static void tmpr_start(int fd, off_t at)
{
    br_start(&tmpr, fd, at, tmpr_buf, sizeof(tmpr_buf));
}

static ssize_t tmpr_read(void *data, size_t len)
{
    return br_read(&tmpr, data, len);
}

/* tag files from where open_tag_fd() left them */
static void tagr_start(int fd)
{
    br_start(&tagr, fd, sizeof(struct tagcache_header), tagr_buf,
             sizeof(tagr_buf));
}

static ssize_t tagr_read_entry(struct tagfile_entry *tfe)
{
    ssize_t ret = br_read(&tagr, tfe, sizeof(*tfe));
    if (ret == sizeof(*tfe) && tc_stat.econ)
        swap_tagfile_entry(tfe);
    return ret;
}

/* as read_tagfile_entry_and_tag() */
static enum e_read_errors tagr_read_entry_and_tag(struct tagfile_entry *tfe,
                                                  char *buf, int bufsz)
{
    if (tagr_read_entry(tfe) != sizeof(struct tagfile_entry))
        return e_ENTRY_SIZEMISMATCH;

    long tag_length = tfe->tag_length;
    if (tag_length >= bufsz)
        return e_TAG_TOOLONG;

    if (tag_length > 0 && br_read(&tagr, buf, tag_length) != tag_length)
        return e_TAG_SIZEMISMATCH;

    str_setlen(buf, tag_length);
    return (tag_length > 0 && *buf) ? e_SUCCESS : e_SUCCESS_LEN_ZERO;
}

/* the clock's time, in the scale of the times of plays, or 0 if it's before
   this firmware was built, as after the battery ran flat */
static long clock_time(void)
{
#if CONFIG_RTC && !defined(PLUGIN) && !defined(__PCTOOL__)
    struct tm *tm = get_time();
    if (valid_time(tm) && tm->tm_year + 1900 >= YEAR)
        return mktime(tm);
#endif
    return 0;
}

#define DATEADDED_MIN 978307200 /* 2001-01-01 */
/* The date added of a song the commit adds without one: the commit's time,
   now, if it's new to a database there was, so a copy's songs have one time;
   else its file's time, from 2001 to now; else 0 */
static long date_added(long mtime, bool new_song, long now)
{
    if (new_song && now)
        return now;
    if (mtime >= DATEADDED_MIN && (!now || mtime <= now))
        return mtime;
    return 0;
}

static bool build_numeric_indices(struct tagcache_header *h, int tmpfd)
{
    const long old_count = commit_mh.tch.entry_count;
    const long now = clock_time();
    struct temp_file_entry *entrybuf = (struct temp_file_entry *)tempbuf;
    int max_entries;
    int entries_processed = 0;
    int i, j;

    max_entries = tempbuf_size / sizeof(struct temp_file_entry) - 1;

    logf("Building numeric indices...");
    tmpr_start(tmpfd, sizeof(struct tagcache_header));

    while (entries_processed < h->entry_count && !USR_CANCEL)
    {
        int count = MIN(h->entry_count - entries_processed, max_entries);

        /* Read in as many entries as possible. */
        for (i = 0; i < count; i++)
        {
            struct temp_file_entry *tfe = &entrybuf[i];
            int datastart;

            /* Read in numeric data. */
            if (tmpr_read(tfe, sizeof(struct temp_file_entry)) !=
                sizeof(struct temp_file_entry))
            {
                logf("read fail #1");
                return false;
            }

            datastart = tmpr.at;

            /**
             * Read string data from the following tags:
             * - tag_filename
             * - tag_artist
             * - tag_album
             * - tag_title
             *
             * A crc32 hash is calculated from the read data
             * and stored back to the data offset field kept in memory.
             */
#define tmpdb_read_string_tag(tag) \
    tmpr.at = datastart + tfe->tag_offset[tag]; \
    if ((unsigned long)tfe->tag_length[tag] > (unsigned long)build_idx_bufsz) \
    { \
        logf("read fail: buffer overflow"); \
        return false; \
    } \
    \
    if (tmpr_read(build_idx_buf, tfe->tag_length[tag]) != \
        tfe->tag_length[tag]) \
    { \
        logf("read fail #2"); \
        return false; \
    } \
    str_setlen(build_idx_buf, tfe->tag_length[tag]); \
    \
    tfe->tag_offset[tag] = crc_32(build_idx_buf, strlen(build_idx_buf), 0xffffffff); \
    tmpr.at = datastart

            tmpdb_read_string_tag(tag_filename);
            tmpdb_read_string_tag(tag_artist);
            tmpdb_read_string_tag(tag_album);
            tmpdb_read_string_tag(tag_title);

            /* Seek to the end of the string data. */
            tmpr.at += tfe->data_length;
        }

        /* Check if we can resurrect some deleted runtime statistics data. */
        for (i = 0; i < old_count && !USR_CANCEL; i++)
        {
            struct index_entry *idx = &commit_idx[i];

            /**
             * Skip unless the entry is marked as being deleted
             * or the data has already been resurrected.
             */
            if (!(idx->flag & FLAG_DELETED) || (idx->flag & FLAG_RESURRECTED))
                continue;

            /* Now try to match the entry. */
            /**
             * To succesfully match a song, the following conditions
             * must apply:
             *
             * For numeric fields: tag_length
             * - Full identical match is required
             *
             * If tag_filename matches, no further checking necessary.
             *
             * For string hashes: tag_artist, tag_album, tag_title
             * - All three of these must match
             */
            for (j = 0; j < count; j++)
            {
                struct temp_file_entry *tfe = &entrybuf[j];

                /* Try to match numeric fields first. */
                if (tfe->tag_offset[tag_length] != idx->tag_seek[tag_length])
                    continue;

                /* Now it's time to do the hash matching. */
                if (tfe->tag_offset[tag_filename] != idx->tag_seek[tag_filename])
                {
                    int match_count = 0;

                    /* No filename match, check if we can match two other tags. */
#define tmpdb_match(tag) \
    if (tfe->tag_offset[tag] == idx->tag_seek[tag]) \
        match_count++

                    tmpdb_match(tag_artist);
                    tmpdb_match(tag_album);
                    tmpdb_match(tag_title);

                    if (match_count < 3)
                    {
                        /* Still no match found, give up. */
                        continue;
                    }
                }

                /* A match found, now copy & resurrect the statistical data. */
#define tmpdb_copy_tag(tag) \
    tfe->tag_offset[tag] = idx->tag_seek[tag]

                tmpdb_copy_tag(tag_playcount);
                tmpdb_copy_tag(tag_rating);
                tmpdb_copy_tag(tag_playtime);
                tmpdb_copy_tag(tag_lastplayed);
                tmpdb_copy_tag(tag_commitid);
                tmpdb_copy_tag(tag_lastelapsed);
                tmpdb_copy_tag(tag_lastoffset);
                /* its tags changed, it keeps the date it was added */
                if (!tfe->tag_offset[tag_dateadded])
                    tmpdb_copy_tag(tag_dateadded);

                /* Avoid processing this entry again. */
                idx->flag |= FLAG_RESURRECTED;

                logf("Entry resurrected");
            }
        }


        /* Commit the data to the index. */
        for (i = 0; i < count && !USR_CANCEL; i++)
        {
            struct index_entry *idx =
                &commit_idx[old_count + entries_processed + i];

            for (j = 0; j < TAG_COUNT; j++)
            {
                if (!TAGCACHE_IS_NUMERIC(j))
                    continue;

                idx->tag_seek[j] = entrybuf[i].tag_offset[j];
            }
            idx->flag = entrybuf[i].flag & ~FLAG_RESTORED;

            bool new_song = false;
            if (entrybuf[i].flag & FLAG_RESTORED)
                ; /* its commit id is the old database's, 0 too */
            else if (idx->tag_seek[tag_commitid])
            {
                /* Data has been resurrected. */
                idx->flag |= FLAG_DIRTYNUM;
            }
            else if (tc_stat.ready && current_tcmh.commitid > 0)
            {
                idx->tag_seek[tag_commitid] = current_tcmh.commitid;
                idx->flag |= FLAG_DIRTYNUM;
                new_song = true;
            }
            if (!idx->tag_seek[tag_dateadded])
                idx->tag_seek[tag_dateadded] =
                    date_added(idx->tag_seek[tag_mtime], new_song, now);
        }

        entries_processed += count;
        logf("%d/%" PRId32 " entries processed", entries_processed, h->entry_count);
    }

    return true;
}

/**
 * Return values:
 *     > 0   success
 *    == 0   temporary failure
 *     < 0   fatal error
 */
static int build_index(int index_type, struct tagcache_header *h, int tmpfd)
{
    int i;
    struct tagcache_header tch;
    const long old_count = commit_mh.tch.entry_count;
    int fd = -1;
    bool error = false;
    long old_n = 0; /* entries from the old tag file, sorted */

    logf("Building index: %d", index_type);

    /* Check the number of entries we need to allocate ram for. */
    commit_entry_count = h->entry_count + 1 + old_count;

    /* Open the index file, which contains the tag names. A sorted one is
       only read: it is written anew. */
    fd = open_tag_fd(&tch, index_type, !TAGCACHE_IS_SORTED(index_type));
    if (fd >= 0)
    {
        logf("tch.datasize=%" PRId32, tch.datasize);
        lookup_buffer_depth = 1 +
        /* First part */ commit_entry_count +
        /* Second part */ (tch.datasize / TAGFILE_ENTRY_CHUNK_LENGTH);
    }
    else
    {
        lookup_buffer_depth = 1 +
        /* First part */ commit_entry_count +
        /* Second part */ 0;
    }

    logf("lookup_buffer_depth=%ld", lookup_buffer_depth);
    logf("commit_entry_count=%ld", commit_entry_count);

    /* Allocate buffer for all index entries from both old and new
     * tag files. */
    tempbufidx = 0;
    tempbuf_pos = commit_entry_count * sizeof(struct tempbuf_searchidx);

    /* Allocate lookup buffer. The first portion of commit_entry_count
     * contains the new tags in the temporary file and the second
     * part for locating entries already in the db.
     *
     *  New tags  Old tags
     * +---------+---------------------------+
     * |  index  | position/ENTRY_CHUNK_SIZE |  lookup buffer
     * +---------+---------------------------+
     *
     * Old tags are inserted to a temporary buffer with position:
     *     tempbuf_insert(position/ENTRY_CHUNK_SIZE, ...);
     * And new tags with index:
     *     tempbuf_insert(idx, ...);
     *
     * The buffer is sorted and written into tag file:
     *     tempbuf_sort(...);
     * leaving master index locations messed up.
     *
     * That is fixed using the lookup buffer for old tags:
     *     new_seek = tempbuf_find_location(old_seek, ...);
     * and for new tags:
     *     new_seek = tempbuf_find_location(idx);
     */
    lookup = (struct tempbuf_searchidx **)&tempbuf[tempbuf_pos];
    tempbuf_pos += lookup_buffer_depth * sizeof(void **);
    memset(lookup, 0, lookup_buffer_depth * sizeof(void **));

    /* and the chains by crc, a head for each entry or more */
    for (crc_mask = 1; crc_mask < (uint32_t)commit_entry_count; crc_mask <<= 1);
    crc_heads = (int32_t *)&tempbuf[tempbuf_pos];
    tempbuf_pos += crc_mask * sizeof(int32_t);
    crc_next = (int32_t *)&tempbuf[tempbuf_pos];
    tempbuf_pos += commit_entry_count * sizeof(int32_t);
    memset(crc_heads, 0xff, crc_mask * sizeof(int32_t));
    crc_mask--;

    /* And calculate the remaining data space used mainly for storing
     * tag data (strings). */
    tempbuf_left = tempbuf_size - tempbuf_pos - 8;
    if (tempbuf_left - TAGFILE_ENTRY_AVG_LENGTH * commit_entry_count < 0)
    {
        logf("Buffer way too small!");
        close(fd);
        return 0;
    }

    if (fd >= 0)
    {
        /**
         * If tag file contains unique tags (sorted index), we will load
         * it entirely into memory so we can resort it later for use with
         * chunked browsing.
         */
        if (TAGCACHE_IS_SORTED(index_type))
        {
            logf("loading tags...");
            tagr_start(fd);
            for (i = 0; i < tch.entry_count && !USR_CANCEL; i++)
            {
                struct tagfile_entry entry;
                int loc = tagr.at;
                bool ret;
                switch (tagr_read_entry_and_tag(&entry, build_idx_buf, build_idx_bufsz))
                {
                    case e_SUCCESS_LEN_ZERO: /* Skip deleted entries. */
                        continue;
                    case e_SUCCESS:
                         break;
                    case e_ENTRY_SIZEMISMATCH:
                        logf("read error #7");
                        close(fd);
                        return -2;
                    case e_TAG_TOOLONG:
                        logf("too long tag #3");
                        close(fd);
                        return -2;
                    case e_TAG_SIZEMISMATCH:
                        logf("read error #8");
                        close(fd);
                        return -2;
                }

                /**
                 * Save the tag and tag id in the memory buffer. Tag id
                 * is saved so we can later reindex the master lookup
                 * table when the index gets resorted.
                 */
                ret = tempbuf_insert(build_idx_buf, loc/TAGFILE_ENTRY_CHUNK_LENGTH
                                     + commit_entry_count, entry.idx_id,
                                     TAGCACHE_IS_UNIQUE(index_type));
                if (!ret)
                {
                    close(fd);
                    return -3;
                }
                do_timed_yield();
            }
            logf("done");
            old_n = tempbufidx;
        }
        else
            tempbufidx = tch.entry_count;
    }
    else if (!TAGCACHE_IS_SORTED(index_type))
    {
        logf("Create New Index: %d", index_type);
        /**
         * Creating new index file to store the tags. No need to preload
         * anything whether the index type is sorted or not.
         *
         * Note: although we are creating a file under the db path, it must
         * already exist by this point so no mkdir is required.
         */
        fd = open_pathfmt(build_idx_buf, build_idx_bufsz,
                          O_WRONLY | O_CREAT | O_TRUNC,
                          "%s/" TAGCACHE_FILE_INDEX,
                          tc_stat.db_path, index_type);
        if (fd < 0)
        {
            logf(TAGCACHE_FILE_INDEX " open fail", index_type);
            return -2;
        }

        tch.magic = TAGCACHE_MAGIC;
        tch.entry_count = 0;
        tch.datasize = 0;

        if (write_tagcache_header(fd, &tch) != sizeof(struct tagcache_header))
        {
            logf("header write failed");
            close(fd);
            return -2;
        }
    }

    if (TAGCACHE_IS_SORTED(index_type))
    {
        /* the old file, now in the buffer, stays until the commit is done */
        if (fd >= 0)
            close(fd);
        fd = open_pathfmt(build_idx_buf, build_idx_bufsz,
                          O_WRONLY | O_CREAT | O_TRUNC,
                          "%s/" TAGCACHE_FILE_INDEX_NEW,
                          tc_stat.db_path, index_type);
        if (fd < 0)
        {
            logf(TAGCACHE_FILE_INDEX_NEW " open fail", index_type);
            return -2;
        }

        tch.magic = TAGCACHE_MAGIC;
        tch.entry_count = 0;
        tch.datasize = 0;

        if (write_tagcache_header(fd, &tch) != sizeof(struct tagcache_header))
        {
            logf("header write failed");
            close(fd);
            return -2;
        }
    }

    /**
     * Load new unique tags in memory to be sorted later and added
     * to the master lookup file.
     */
    if (TAGCACHE_IS_SORTED(index_type))
    {
        tmpr_start(tmpfd, sizeof(struct tagcache_header));
        /* h is the header of the temporary file containing new tags. */
        logf("inserting new tags...");
        for (i = 0; i < h->entry_count && !USR_CANCEL; i++)
        {
            struct temp_file_entry entry;

            if (tmpr_read(&entry, sizeof(struct temp_file_entry)) !=
                sizeof(struct temp_file_entry))
            {
                logf("read fail #3");
                error = true;
                goto error_exit;
            }

            /* Read data. */
            if (entry.tag_length[index_type] >= build_idx_bufsz)
            {
                logf("too long entry!");
                error = true;
                goto error_exit;
            }

            tmpr.at += entry.tag_offset[index_type];
            if (tmpr_read(build_idx_buf, entry.tag_length[index_type]) !=
                entry.tag_length[index_type])
            {
                logf("read fail #4");
                error = true;
                goto error_exit;
            }
            str_setlen(build_idx_buf, entry.tag_length[index_type]);

#if defined(PLUGIN)
            if (user_check_tag(index_type, build_idx_buf))
#endif /*defined(PLUGIN)*/
            {
                if (TAGCACHE_IS_UNIQUE(index_type))
                    error = !tempbuf_insert(build_idx_buf, i, -1, true);
                else
                    error = !tempbuf_insert(build_idx_buf, i,
                                            old_count + i, false);

                if (error)
                {
                    logf("insert error");
                    goto error_exit;
                }
            }
            /* Skip to next. */
            tmpr.at += entry.data_length - entry.tag_offset[index_type] -
                       entry.tag_length[index_type];
            do_timed_yield();
        }
        logf("done");


        /* Sort the buffer data and write it to the index file. */
        i = tempbuf_sort(fd, old_n);
        if (i < 0)
            goto error_exit;
        logf("sorted %d tags", i);

        /**
         * Now update all indexes in the master lookup file.
         */
        logf("updating indices...");
        for (i = 0; i < old_count && !USR_CANCEL; i++)
        {
            struct index_entry *e = &commit_idx[i];

            /* We can just ignore deleted entries. */
            if (e->flag & FLAG_DELETED)
                continue;

            e->tag_seek[index_type] = tempbuf_find_location(
                e->tag_seek[index_type]/TAGFILE_ENTRY_CHUNK_LENGTH
                + commit_entry_count);

            if (e->tag_seek[index_type] < 0)
            {
                logf("update error: %" PRId32 "/%d/%ld",
                     e->flag, i, old_count);
                error = true;
                goto error_exit;
            }

            do_timed_yield();
        }
        logf("done");
    }

    /**
     * Walk through the temporary file containing the new tags.
     */
    // build_normal_index(h, tmpfd, masterfd, idx);
    logf("updating new indices...");
    tmpr_start(tmpfd, sizeof(struct tagcache_header));
    off_t fd_pos = lseek(fd, 0, SEEK_END);
    tagfile_wbuf_used = 0;
    for (i = 0; i < h->entry_count && !USR_CANCEL; i++)
    {
        struct index_entry *e = &commit_idx[old_count + i];

        if (!TAGCACHE_IS_SORTED(index_type))
        {
            struct temp_file_entry entry;
            struct tagfile_entry fe;

            if (tmpr_read(&entry, sizeof(struct temp_file_entry)) !=
                sizeof(struct temp_file_entry))
            {
                logf("read fail #7");
                error = true;
                break ;
            }

            /* Read data. */
            if (entry.tag_length[index_type] >= build_idx_bufsz)
            {
                logf("too long entry!");
                logf("length=%d", entry.tag_length[index_type]);
                logf("pos=0x%02lx", (unsigned long) tmpr.at);
                error = true;
                break ;
            }

            tmpr.at += entry.tag_offset[index_type];
            if (tmpr_read(build_idx_buf, entry.tag_length[index_type]) !=
                entry.tag_length[index_type])
            {
                logf("read fail #8");
                logf("offset=0x%02" PRIx32, entry.tag_offset[index_type]);
                logf("length=0x%02x", entry.tag_length[index_type]);
                error = true;
                break ;
            }

            /* Write to index file. */
            e->tag_seek[index_type] = fd_pos;
            fe.tag_length = entry.tag_length[index_type];
            fe.idx_id = old_count + i;
            struct tagfile_entry swapped = fe;
            swap_tagfile_entry(&swapped);
            if (!tagfile_wbuf_put(fd, &swapped, sizeof(swapped))
                || !tagfile_wbuf_put(fd, build_idx_buf, fe.tag_length))
            {
                logf("write fail #9");
                error = true;
                break ;
            }
            fd_pos += sizeof(fe) + fe.tag_length;
            tempbufidx++;

            /* Skip to next. */
            tmpr.at += entry.data_length - entry.tag_offset[index_type] -
                       entry.tag_length[index_type];
        }
        else
        {
            /* Locate the correct entry from the sorted array. */
            e->tag_seek[index_type] = tempbuf_find_location(i);
            if (e->tag_seek[index_type] < 0)
            {
                logf("entry not found (%d)", i);
                error = true;
                break ;
            }
        }

        do_timed_yield();
    }
    logf("done");

    if (!TAGCACHE_IS_SORTED(index_type) && !tagfile_wbuf_flush(fd))
        error = true;
    /* Finally write the header. */
    tch.magic = TAGCACHE_MAGIC;
    tch.entry_count = tempbufidx;
    tch.datasize = lseek(fd, 0, SEEK_END) - sizeof(struct tagcache_header);
    lseek(fd, 0, SEEK_SET);
    write_tagcache_header(fd, &tch);

    if (index_type != tag_filename)
        h->datasize += tch.datasize;
    logf("s:%d/%" PRId32 "/%" PRId32, index_type, tch.datasize, h->datasize);
    error_exit:

    close(fd);

    if (error)
        return -2;

    return 1;
}

/* the free memory a commit takes rather than the dircache's */
#define COMMIT_TEMPBUF_MIN (4*1024*1024)

/* What build_index() takes of the buffer for a tag, at most. A build that
 * found too little half way had rewritten the tags before it, and the commit
 * done again then put the new entries of those in twice. h is the temporary
 * file's header, count the entries in the database. */
static size_t commit_tempbuf_need(int tag, const struct tagcache_header *h,
                                  long count)
{
    struct tagcache_header tch;
    long datasize = 0;
    int fd = open_tag_fd(&tch, tag, false);
    if (fd >= 0)
    {
        datasize = tch.datasize;
        close(fd);
    }
    size_t entries = h->entry_count + 1 + count;
    size_t depth = 1 + entries + datasize / TAGFILE_ENTRY_CHUNK_LENGTH;
    size_t heads = 1;
    while (heads < entries)
        heads <<= 1;
    size_t need = entries * sizeof(struct tempbuf_searchidx)
                  + depth * sizeof(void **)
                  + (heads + entries) * sizeof(int32_t) + 8;
    /* the strings, old and new, with a crc, an id list and alignment for
       each, and their Sort fields, or what build_index() asks for anyway */
    size_t strings = TAGCACHE_IS_SORTED(tag) ?
        datasize + h->datasize + entries * (4 + 12 + 4) : 0;
#ifdef TAGCACHE_SORT_FIELDS
    if (strings)
        strings += sort_values_size;
#endif
    return need + MAX(strings, TAGFILE_ENTRY_AVG_LENGTH * entries);
}

#ifdef TAGCACHE_SORT_FIELDS
/* a file of Sort fields, at path or the database's file of that name, past
   its header: -1 for none */
static int open_sort_file(const char *path, bool in_db,
                          struct tagcache_sort_header *h)
{
    char buf[MAX_PATH];
    int fd = in_db ? open_pathfmt(buf, sizeof(buf), O_RDONLY, "%s/%s",
                                  tc_stat.db_path, path)
                   : open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    if (read(fd, h, sizeof(*h)) != sizeof(*h) || h->magic != TAGCACHE_SORT_MAGIC
        || h->version != TAGCACHE_SORT_VERSION || h->count > 0x100000)
    {
        close(fd);
        return -1;
    }
    return fd;
}

/* the crc of Finder's Sort fields, 0 for none */
static uint32_t finder_sort_crc(void)
{
    struct tagcache_sort_header h;
    int fd = open_sort_file(TAGCACHE_SORT_FINDER, false, &h);
    if (fd < 0)
        return 0;
    close(fd);
    return h.crc;
}

static int cmp_sort_pair(const void *p1, const void *p2)
{
    const struct sort_pair *a = p1, *b = p2;
    if (a->tag != b->tag)
        return a->tag < b->tag ? -1 : 1;
    if (a->crc != b->crc)
        return a->crc < b->crc ? -1 : 1;
    return a->from < b->from ? -1 : a->from > b->from;
}

/* The pairs of the three files of Sort fields into the end of the commit's
   buffer, below the master file: false if they don't fit. Small: 24 pairs in
   a library of 13,000 songs, 1 KB, and some 12,000 in one with a reading of
   every name, 0.8 MB. */
static bool load_sort_pairs(void)
{
    static const struct {
        const char *path;
        bool in_db;
        uint8_t from;
    } files[] = {
        { TAGCACHE_SORT_FINDER, false, SORT_FROM_FINDER },
        { TAGCACHE_FILE_SORT_SCAN, true, SORT_FROM_SCAN },
        { TAGCACHE_FILE_SORT, true, SORT_FROM_FILES },
    };
    int fds[ARRAYLEN(files)];
    long sizes[ARRAYLEN(files)];
    size_t bytes = 0, count = 0;
    sort_pairs = NULL;
    sort_pair_count = 0;
    sort_values_size = 0;
    sort_finder_crc = 0;
    sort_files_kept = false;
    for (size_t k = 0; k < ARRAYLEN(files); k++)
    {
        struct tagcache_sort_header h;
        fds[k] = open_sort_file(files[k].path, files[k].in_db, &h);
        sizes[k] = 0;
        if (fds[k] < 0)
            continue;
        sizes[k] = ffilesize(fds[k]) - sizeof(h);
        bytes += MAX(sizes[k], 0);
        count += h.count;
        if (files[k].from == SORT_FROM_FINDER)
            sort_finder_crc = h.crc;
        else
            sort_files_kept = true;
    }
    const size_t need = count * sizeof(struct sort_pair) + ALIGN_UP(bytes, 4);
    bool ok = need + 1024 <= (size_t)tempbuf_size;
    if (ok && count > 0)
    {
        char *room = (char *)ALIGN_DOWN(tempbuf + tempbuf_size - need, 4);
        tempbuf_size = (room - tempbuf) & ~0x03;
        sort_pairs = (struct sort_pair *)room;
        char *data = (char *)(sort_pairs + count);
        for (size_t k = 0; k < ARRAYLEN(files); k++)
        {
            if (fds[k] < 0 || sizes[k] <= 0
                || read(fds[k], data, sizes[k]) != sizes[k])
                continue;
            const char *p = data, *end = data + sizes[k];
            while (end - p >= TAGCACHE_SORT_PAIR_HEAD
                   && sort_pair_count < (long)count)
            {
                const unsigned char *head = (const unsigned char *)p;
                const int tag = head[0];
                const long name_size = head[1] | head[2] << 8;
                const long value_size = head[3] | head[4] << 8;
                const char *name = p + TAGCACHE_SORT_PAIR_HEAD;
                const char *value = name + name_size;
                if (name_size < 2 || value_size < 2 || value_size > end - value
                    || name[name_size - 1] || value[value_size - 1])
                    break;
                if (tag < TAG_COUNT && TAGCACHE_IS_SORTED(tag))
                {
                    sort_pairs[sort_pair_count++] = (struct sort_pair){
                        name_crc(name), name, value, tag, files[k].from, false
                    };
                    sort_values_size += value_size;
                }
                p = value + value_size;
            }
            data += sizes[k];
        }
        qsort(sort_pairs, sort_pair_count, sizeof(*sort_pairs), cmp_sort_pair);
    }
    for (size_t k = 0; k < ARRAYLEN(files); k++)
    {
        if (fds[k] >= 0)
            close(fds[k]);
    }
    return ok;
}

/* The Sort fields of files' tags that the commit found names for, those the
   scan found over those kept, as the database's next ones: a song gone with
   the last of its name takes its pair with it. Put in place with the tag
   files (commit_swap()). */
static bool write_sort_files(void)
{
    if (!sort_files_kept)
        return true;
    int fd = open_db_fd(TAGCACHE_FILE_SORT_NEW, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return false;
    struct tagcache_sort_header h = {
        TAGCACHE_SORT_MAGIC, TAGCACHE_SORT_VERSION, 0, 0xffffffff, 0, 0
    };
    bool ok = write(fd, &h, sizeof(h)) == sizeof(h);
    tagfile_wbuf_used = 0;
    for (long i = 0; ok && i < sort_pair_count; i++)
    {
        const struct sort_pair *p = &sort_pairs[i];
        if (p->from == SORT_FROM_FINDER || !p->used)
            continue;
        /* the first of a name, the scan's if it has one */
        bool first = true;
        for (long j = i - 1; first && j >= 0 && sort_pairs[j].tag == p->tag
                             && sort_pairs[j].crc == p->crc; j--)
        {
            first = sort_pairs[j].from == SORT_FROM_FINDER
                    || strcasecmp(sort_pairs[j].name, p->name);
        }
        unsigned char head[TAGCACHE_SORT_PAIR_HEAD];
        const size_t name_size = strlen(p->name) + 1;
        const size_t value_size = strlen(p->value) + 1;
        if (!first || !tagcache_sort_pair_head(head, p->tag, name_size,
                                               value_size))
            continue;
        h.crc = crc_32(head, sizeof(head), h.crc);
        h.crc = crc_32(p->name, name_size, h.crc);
        h.crc = crc_32(p->value, value_size, h.crc);
        ok = tagfile_wbuf_put(fd, head, sizeof(head))
             && tagfile_wbuf_put(fd, p->name, name_size)
             && tagfile_wbuf_put(fd, p->value, value_size);
        h.count++;
    }
    ok = ok && tagfile_wbuf_flush(fd) && lseek(fd, 0, SEEK_SET) == 0
         && write(fd, &h, sizeof(h)) == sizeof(h);
    if (close(fd) < 0)
        ok = false;
    return ok;
}

/* The commit's database is sorted by the rules and the Sort fields it had:
   put in place with its files (commit_swap()), so that a commit finished or
   undone after a reboot has the mark of its files. With the letters noted as
   they were written. */
static bool write_order_mark(int32_t commitid)
{
    const struct order_mark mark = {
        ORDER_MAGIC, SORTKEY_RULES_VERSION, commitid, sort_finder_crc
    };
    int fd = open_db_fd(TAGCACHE_FILE_ORDER_NEW, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return false;
    const bool ok = write(fd, &mark, sizeof(mark)) == sizeof(mark)
                    && write(fd, &initials, sizeof(initials)) == sizeof(initials);
    return close(fd) == 0 && ok;
}

/* Quick Scroll's letters from the order's mark, if it's of the database as it
   is: at boot, and after a commit that didn't go through, which may have
   noted some of its own */
static void initials_load(void)
{
    initials_commit = -1;
    struct master_header mh;
    int fd = open_master_fd(&mh, false);
    if (fd < 0)
        return;
    close(fd);
    struct order_mark mark;
    fd = open_db_fd(TAGCACHE_FILE_ORDER, O_RDONLY);
    if (fd < 0)
        return;
    bool ok = read(fd, &mark, sizeof(mark)) == sizeof(mark)
              && mark.magic == ORDER_MAGIC && mark.rules == SORTKEY_RULES_VERSION
              && mark.commitid == mh.commitid
              && read(fd, &initials, sizeof(initials)) == sizeof(initials);
    close(fd);
    for (size_t i = 0; ok && i < INITIAL_TAGS; i++)
        ok = initials.count[i] <= INITIALS_PER_TAG;
    if (ok)
        initials_commit = mark.commitid;
}

/* Songs taken out of the database, and the counts of a changelog, leave the
   others in their order, but count as a commit for what's made from it (its
   commit id from from to current_tcmh's): the order's mark goes with them, or
   the next boot sorted it again for nothing */
static void order_mark_follow(int32_t from)
{
    struct order_mark mark;
    int fd = open_db_fd(TAGCACHE_FILE_ORDER, O_RDWR);
    if (fd < 0)
        return;
    if (read(fd, &mark, sizeof(mark)) == sizeof(mark)
        && mark.magic == ORDER_MAGIC && mark.commitid == from)
    {
        mark.commitid = current_tcmh.commitid;
        if (lseek(fd, 0, SEEK_SET) != 0
            || write(fd, &mark, sizeof(mark)) != sizeof(mark))
            mark.magic = 0;
    }
    if (close(fd) < 0 || mark.magic != ORDER_MAGIC)
    {
        remove_db_file(TAGCACHE_FILE_ORDER);
        initials_commit = -1;
    }
    /* and the letters, which are of the same places */
    else if (initials_commit == from)
        initials_commit = mark.commitid;
}
#endif /* TAGCACHE_SORT_FIELDS */

/* The letter a name for a tag is listed under, Quick Scroll's, by its place
   in the tag's file: 0 if it isn't known. From memory, without a read. */
ucschar_t tagcache_initial(int tag, long seek)
{
#ifdef TAGCACHE_SORT_FIELDS
    const int slot = initial_slot(tag);
    if (slot < 0 || initials_commit < 0
        || initials_commit != current_tcmh.commitid)
        return 0;
    const int32_t *seeks = initials.seek[slot];
    const int n = initials.count[slot];
    if (n == 0 || seek < seeks[0])
        return 0;
    /* the last letter that begins where it is or before */
    int lo = 0, hi = n - 1;
    while (lo < hi)
    {
        const int mid = (lo + hi + 1) / 2;
        if (seeks[mid] <= seek)
            lo = mid;
        else
            hi = mid - 1;
    }
    return initials.letter[slot][lo];
#else
    (void)tag;
    (void)seek;
    return 0;
#endif
}

/* A commit on disk until it is done. While it builds the new files, a
   reboot undoes it, and the temporary file is committed again; once it puts
   them in place, a reboot finishes that. */
enum { JOURNAL_BUILDING = 1, JOURNAL_SWAPPING };

struct commit_journal
{
    int32_t magic;
    int32_t state;
    int32_t fresh;  /* no database before, so undoing removes it */
    /* the tag files written to in place, as they were: their sizes, -1 for
       none, and their headers */
    int32_t size[TAG_COUNT];
    struct tagcache_header hdr[TAG_COUNT];
};

static bool write_journal(const struct commit_journal *j)
{
    int fd = open_db_fd(TAGCACHE_FILE_JOURNAL, O_WRONLY | O_CREAT);
    if (fd < 0)
        return false;
    bool ok = write(fd, j, sizeof(*j)) == sizeof(*j);
    close(fd);
    return ok;
}

/* puts the new files in place of the old ones, and lets the temporary file
   go */
static bool commit_swap(void)
{
    char from[32], to[32];

    if (rename_db_file(TAGCACHE_FILE_MASTER_NEW, TAGCACHE_FILE_MASTER) < 0
        && errno != ENOENT)
        return false;
    for (int tag = 0; tag < TAG_COUNT; tag++)
    {
        if (!TAGCACHE_IS_SORTED(tag))
            continue;
        snprintf(from, sizeof(from), TAGCACHE_FILE_INDEX_NEW, tag);
        snprintf(to, sizeof(to), TAGCACHE_FILE_INDEX, tag);
        if (rename_db_file(from, to) < 0 && errno != ENOENT)
            return false;
    }
#ifdef TAGCACHE_SORT_FIELDS
    if (rename_db_file(TAGCACHE_FILE_SORT_NEW, TAGCACHE_FILE_SORT) < 0
        && errno != ENOENT)
        return false;
    if (rename_db_file(TAGCACHE_FILE_ORDER_NEW, TAGCACHE_FILE_ORDER) < 0
        && errno != ENOENT)
        return false;
#endif
    remove_scan_files();
    remove_db_file(TAGCACHE_FILE_JOURNAL);
    return true;
}

/* j is NULL when the journal itself was cut short, before anything else
   was written */
static void commit_undo(const struct commit_journal *j)
{
    char name[32];

    remove_db_file(TAGCACHE_FILE_MASTER_NEW);
    for (int tag = 0; tag < TAG_COUNT; tag++)
    {
        if (!TAGCACHE_IS_SORTED(tag))
            continue;
        snprintf(name, sizeof(name), TAGCACHE_FILE_INDEX_NEW, tag);
        remove_db_file(name);
    }
#ifdef TAGCACHE_SORT_FIELDS
    remove_db_file(TAGCACHE_FILE_SORT_NEW);
    remove_db_file(TAGCACHE_FILE_ORDER_NEW);
#endif

    if (j && j->fresh)
        remove_files();
    else if (j)
    {
        for (int tag = 0; tag < TAG_COUNT; tag++)
        {
            if (TAGCACHE_IS_NUMERIC(tag) || TAGCACHE_IS_SORTED(tag))
                continue;
            snprintf(name, sizeof(name), TAGCACHE_FILE_INDEX, tag);
            if (j->size[tag] < 0)
            {
                remove_db_file(name);
                continue;
            }
            int fd = open_db_fd(name, O_RDWR);
            if (fd >= 0)
            {
                ftruncate(fd, j->size[tag]);
                write(fd, &j->hdr[tag], sizeof(j->hdr[tag]));
                close(fd);
            }
        }
    }
    remove_db_file(TAGCACHE_FILE_JOURNAL);
}

/* Undoes or finishes a commit a reboot cut short */
static void commit_recover(void)
{
    struct commit_journal j;
    int fd = open_db_fd(TAGCACHE_FILE_JOURNAL, O_RDONLY);
    if (fd < 0)
        return;
    bool whole = read(fd, &j, sizeof(j)) == sizeof(j)
                 && j.magic == TAGCACHE_MAGIC;
    close(fd);

    logf("commit cut short, %s", whole && j.state == JOURNAL_SWAPPING ?
         "finishing it" : "undoing it");
    if (whole && j.state == JOURNAL_SWAPPING)
        commit_swap();
    else
        commit_undo(whole ? &j : NULL);
}

/* the master file into commit_idx, which has room for capacity entries,
   and the new ones of the temporary file h after them */
static bool load_master(bool fresh, long capacity,
                        const struct tagcache_header *h)
{
    long count = 0;
    if (fresh)
    {
        /* Just to be sure we are clean. */
        remove_files();
        memset(&commit_mh, 0, sizeof(commit_mh));
        commit_mh.tch = *h;
        commit_mh.tch.entry_count = 0;
        commit_mh.tch.datasize = 0;
        commit_mh.dirty = true;
        restore_numbers(&commit_mh);
    }
    else
    {
        int fd = open_master_fd(&commit_mh, false);
        if (fd < 0)
            return false;
        count = commit_mh.tch.entry_count;
        bool ok = count + h->entry_count <= capacity
                  && read_index_entries(fd, commit_idx, count)
                     == (ssize_t)(count * sizeof(struct index_entry));
        close(fd);
        if (!ok)
            return false;
    }
    memset(&commit_idx[count], 0, h->entry_count * sizeof(struct index_entry));
    return true;
}

/* the master file from memory, under the new name */
static bool write_master(struct master_header *mh)
{
    int fd = open_db_fd(TAGCACHE_FILE_MASTER_NEW,
                        O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return false;
    mh->header_size = sizeof(struct master_header);
    mh->entry_size = sizeof(struct index_entry);
    ssize_t size = mh->tch.entry_count * sizeof(struct index_entry);
    bool ok = write_master_header(fd, mh) == sizeof(*mh)
              && write_index_entries(fd, commit_idx, mh->tch.entry_count)
                 == size;
    close(fd);
    return ok;
}

static bool commit(void)
{
    struct tagcache_header tch;
    struct master_header   tcmh;
    int i, len, rc;
    int tmpfd;
    int masterfd;
    bool no_room;
#ifdef HAVE_DIRCACHE
    bool dircache_buffer_stolen = false;
#endif
#ifdef HAVE_TC_RAMCACHE
    bool ramcache_buffer_stolen = false;
#endif
    struct commit_journal journal;
    bool journal_written = false;
    logf("committing tagcache");

    while (write_lock)
        sleep(1);

    /* one that failed to put its files in place would leave them mixed */
    commit_recover();

#if !defined(PLUGIN)
    int fd = open_db_fd(TAGCACHE_FILE_NOCOMMIT, O_RDONLY);
    if (fd >= 0)
    {
        logf("canceling commit");
        tc_stat.commit_delayed = true;
        close(fd);
        tmpfd = -1;
    }
    else
#endif /*!defined(PLUGIN)*/
    {
        tmpfd = open_db_fd(TAGCACHE_FILE_TEMP, O_RDONLY);
    }
    if (tmpfd < 0)
    {
        logf("nothing to commit");
        return true;
    }


    /* Load the header. */
    len = sizeof(struct tagcache_header);
    rc = read(tmpfd, &tch, len);

    if (tch.magic != TAGCACHE_MAGIC || rc != len)
    {
        logf("incorrect tmpheader");
        close(tmpfd);
        remove_scan_files();
        return false;
    }

    if (tch.entry_count == 0 && !commit_resorting)
    {
        /* A scan that found nothing new. Writing every tag file again the
           same way took as long as a commit of new files. */
        masterfd = open_master_fd(&tcmh, false);
        if (masterfd >= 0)
        {
            logf("nothing to commit");
            close(masterfd);
            close(tmpfd);
            remove_scan_files();
            return true;
        }
    }

    if (!commit_room(tmpfd, tch.entry_count))
    {
        close(tmpfd);
        remove_scan_files();
        return false;
    }

    /* Fully initialize existing headers (if any) before going further. */
    tc_stat.ready = check_all_headers();

#ifdef HAVE_EEPROM_SETTINGS
    remove_db_file(TAGCACHE_STATEFILE);
#endif

    /* At first be sure to unload the ramcache! */
#ifdef HAVE_TC_RAMCACHE
    tc_stat.ramcache = false;
#ifndef __PCTOOL__
    free_ram_names();
#endif
#endif

    rc = false;

    /* Try to steal every buffer we can :) Before read_lock: taking the audio
       buffer's stops the music, whose track's end counts its play, which waits
       for read_lock to be let go, and the two waited on each other for good */
#if defined(HAVE_DIRCACHE) && !defined(PLUGIN) && !defined(__PCTOOL__)
    /* Free memory first: taking the dircache's means building it again
       afterwards, a walk over the whole disk. While music plays the audio
       buffer has the memory, and too little is left. */
    bool tempbuf_taken = false;
    if (tempbuf_size == 0)
    {
        allocate_tempbuf();
        if (tempbuf_size >= COMMIT_TEMPBUF_MIN)
            tempbuf_taken = true;
        else
            free_tempbuf();
    }
#endif
#ifdef HAVE_DIRCACHE
    if (tempbuf_size == 0)
    {
        /* Suspend dircache to free its allocation. */
        dircache_free_buffer();
        dircache_buffer_stolen = true;

        allocate_tempbuf();
    }
#endif /* HAVE_DIRCACHE */

#ifdef HAVE_TC_RAMCACHE
    if (tempbuf_size == 0 && tc_stat.ramcache_allocated > 0)
    {
        tcrc_buffer_lock();
        tempbuf = (char *)(tcramcache.hdr + 1);
        tempbuf_size = tc_stat.ramcache_allocated - sizeof(struct ramcache_header) - 128;
        tempbuf_size &= ~0x03;
        ramcache_buffer_stolen = true;
    }
#endif /* HAVE_TC_RAMCACHE */

#if defined(PLUGIN)
    if (tempbuf_size == 0)
    {
        tempbuf = rb->plugin_get_audio_buffer(&tempbuf_size);
        tempbuf_size &= ~0x03;
    }
#endif /*defined(PLUGIN)*/

    /* Beyond here, jump to commit_error to undo locks and restore dircache */
    read_lock++;

    /* And finally fail if there are no buffers available. */
    if (tempbuf_size == 0)
    {
        logf("delaying commit until next boot");
        tc_stat.commit_delayed = true;
        close(tmpfd);
        goto commit_error;
    }

    logf("commit %" PRId32 " entries...", tch.entry_count);

    memset(&journal, 0, sizeof(journal));
    journal.magic = TAGCACHE_MAGIC;
    journal.state = JOURNAL_BUILDING;

    /* Every tag must fit before the first is rewritten, next to the master
       file in memory */
    long count = 0;
    {
        struct master_header mh;
        int fd = open_master_fd(&mh, false);
        if (fd >= 0)
        {
            count = mh.tch.entry_count;
            close(fd);
        }
        else
            journal.fresh = true;

        size_t master_size = (count + tch.entry_count)
                             * sizeof(struct index_entry);
        if (master_size + 1024 > tempbuf_size)
        {
            logf("buffer too small for the master file, delaying commit");
            tc_stat.commit_delayed = true;
            close(tmpfd);
            goto commit_error;
        }
        commit_full_size = tempbuf_size;
        commit_idx = STORAGE_ALIGN_DOWN(
            (struct index_entry *)(tempbuf + tempbuf_size - master_size));
        tempbuf_size = ((char *)commit_idx - tempbuf) & ~0x03;
#ifdef TAGCACHE_SORT_FIELDS
        /* and the Sort fields below it, for every tag */
        if (!load_sort_pairs())
        {
            logf("buffer too small for the Sort fields, delaying commit");
            tc_stat.commit_delayed = true;
            close(tmpfd);
            goto commit_error;
        }
#endif

        for (i = 0; i < TAG_COUNT; i++)
        {
            if (TAGCACHE_IS_NUMERIC(i)
                || commit_tempbuf_need(i, &tch, count) <= (size_t)tempbuf_size)
                continue;
            logf("buffer too small for tag %d, delaying commit", i);
            tc_stat.commit_delayed = true;
            close(tmpfd);
            goto commit_error;
        }
    }

    /* The old database stays as it is until the new one is done: the
       journal first, as it is what undoes the rest */
    for (i = 0; i < TAG_COUNT; i++)
    {
        if (TAGCACHE_IS_NUMERIC(i) || TAGCACHE_IS_SORTED(i))
            continue;
        journal.size[i] = -1;
        int fd = open_pathfmt(build_idx_buf, build_idx_bufsz, O_RDONLY,
                              "%s/" TAGCACHE_FILE_INDEX, tc_stat.db_path, i);
        if (fd >= 0)
        {
            journal.size[i] = ffilesize(fd);
            if (read(fd, &journal.hdr[i], sizeof(journal.hdr[i]))
                != sizeof(journal.hdr[i]))
                journal.size[i] = -1;
            close(fd);
        }
    }
    journal_written = true;
    if (!write_journal(&journal))
    {
        close(tmpfd);
        goto commit_error;
    }

    update_master_header();
    if (!load_master(journal.fresh, count + tch.entry_count, &tch))
    {
        logf("master load failed");
        close(tmpfd);
        goto commit_error;
    }

    /* Now create the index files. */
    tc_stat.commit_step = 0;
    tch.datasize = 0;
    tc_stat.commit_delayed = false;
#ifdef TAGCACHE_SORT_FIELDS
    /* Quick Scroll's letters are noted anew as they're written */
    initials_commit = -1;
#endif

    for (i = 0; i < TAG_COUNT && !USR_CANCEL; i++)
    {
        int ret;

        if (TAGCACHE_IS_NUMERIC(i))
            continue;

        tc_stat.commit_step++;
#ifdef TAGCACHE_SORT_FIELDS
        sort_build_tag = i;
#endif
        ret = build_index(i, &tch, tmpfd);
#ifdef TAGCACHE_SORT_FIELDS
        sort_build_tag = -1;
#endif
        if (ret <= 0)
        {
            close(tmpfd);
            logf("tagcache failed init");
            if (ret == 0)
                tc_stat.commit_delayed = true;

            tc_stat.commit_step = 0;
            goto commit_error;
        }
        do_timed_yield();
    }

    if (!build_numeric_indices(&tch, tmpfd))
    {
        logf("Failure to commit numeric indices");
        close(tmpfd);
        tc_stat.commit_step = 0;
        goto commit_error;
    }

    close(tmpfd);

    /* the steps stay counted until the files are swapped: the progress
       bar after a sync showed the master write and swap as 45% */
    if (!USR_CANCEL)
    {
        /* Update the master index headers. */
        tcmh = commit_mh;
        tcmh.tch.entry_count += tch.entry_count;
        tcmh.tch.datasize = sizeof(struct master_header)
            + sizeof(struct index_entry) * tcmh.tch.entry_count
            + tch.datasize;
        tcmh.dirty = false;
        tcmh.commitid++;

        bool master_written = write_master(&tcmh);
#ifdef TAGCACHE_SORT_FIELDS
        master_written = master_written && write_sort_files()
                         && write_order_mark(tcmh.commitid);
#endif
        tempbuf_size = commit_full_size;
        commit_idx = NULL;
        if (!master_written)
            goto commit_error;

        /* all written: from here on, a reboot finishes the commit */
        journal.state = JOURNAL_SWAPPING;
        if (!write_journal(&journal))
            goto commit_error;
        journal_written = false;
        if (!commit_swap())
            goto commit_error;
#ifdef TAGCACHE_SORT_FIELDS
        initials_commit = tcmh.commitid;
#endif

        logf("tagcache committed");
        song_count = -1;
        tagcache_commit_finalize();

#if defined(HAVE_TC_RAMCACHE)
        if (ramcache_buffer_stolen)
        {
            tempbuf = NULL;
            tempbuf_size = 0;
            ramcache_buffer_stolen = false;
            tcrc_buffer_unlock();
        }

        /* Reload tagcache only if we committed new entries */
        if (tc_stat.ramcache_allocated > 0 && tch.entry_count > 0)
            tagcache_start_scan();
#endif /* HAVE_TC_RAMCACHE */

        rc = true;
    } /*!USR_CANCEL*/

commit_error:
    /* by the free space, before the undo frees what was written: errno
       isn't the failed write's here */
    no_room = !rc && !room_for(1024);
    tc_stat.commit_step = 0;
#ifdef TAGCACHE_SORT_FIELDS
    sort_pairs = NULL; /* in the buffer */
    sort_pair_count = 0;
    sort_values_size = 0;
#endif
    if (commit_idx)
    {
        tempbuf_size = commit_full_size;
        commit_idx = NULL;
    }
    if (journal_written)
        commit_undo(&journal);
#ifdef TAGCACHE_SORT_FIELDS
    /* those of the database as it stayed */
    if (initials_commit < 0)
        initials_load();
#endif
#ifdef HAVE_TC_RAMCACHE
    if (ramcache_buffer_stolen)
    {
        tempbuf = NULL;
        tempbuf_size = 0;
        tcrc_buffer_unlock();
    }
#endif /* HAVE_TC_RAMCACHE */

    read_lock--;

#if defined(HAVE_DIRCACHE) && !defined(PLUGIN) && !defined(__PCTOOL__)
    if (tempbuf_taken)
        free_tempbuf();
#endif
#ifdef HAVE_DIRCACHE
    /* Resume the dircache, if we stole the buffer. */
    if (dircache_buffer_stolen)
    {
        free_tempbuf();
        dircache_resume();
    }
#endif /* HAVE_DIRCACHE */

    /* as a commit refused for room: not tried again at each boot without
       the message */
    if (no_room)
        remove_scan_files();
    return rc;
}

void tagcache_commit_finalize(void)
{
    tc_stat.ready = check_all_headers();
    tc_stat.readyvalid = true;
}

#if !defined(PLUGIN)
#ifndef __PCTOOL__

static bool modify_numeric_entry(int masterfd, int idx_id, int tag, long data)
{
    struct index_entry idx;

    if (!tc_stat.ready)
        return false;

    if (!TAGCACHE_IS_NUMERIC(tag))
        return false;

    if (!get_index(masterfd, idx_id, &idx, false))
        return false;

    idx.tag_seek[tag] = data;
    idx.flag |= FLAG_DIRTYNUM;

    return write_index(masterfd, idx_id, &idx);
}

#if 0
bool tagcache_modify_numeric_entry(struct tagcache_search *tcs,
                                   int tag, long data)
{
    struct master_header myhdr;

    if (tcs->masterfd < 0)
    {
        if ( (tcs->masterfd = open_master_fd(&myhdr, true)) < 0)
            return false;
    }

    return modify_numeric_entry(tcs->masterfd, tcs->idx_id, tag, data);
}
#endif

static bool command_queue_is_full(void)
{
    int next;

    next = command_queue_widx + 1;
    if (next >= TAGCACHE_COMMAND_QUEUE_LENGTH)
        next = 0;

    return (next == command_queue_ridx);
}

static void command_queue_sync_callback(void)
{
    struct master_header myhdr;
    int masterfd;

    mutex_lock(&command_queue_mutex);

    /* not to be had, as after a rebuild took the files: what's queued waits
       for the next try, and the queue stays free for playback to add to, which
       otherwise waited on it from then on */
    if ( (masterfd = open_master_fd(&myhdr, true)) < 0)
    {
        mutex_unlock(&command_queue_mutex);
        return;
    }

    while (command_queue_ridx != command_queue_widx)
    {
        struct tagcache_command_entry *ce = &command_queue[command_queue_ridx];

        switch (ce->command)
        {
            case CMD_UPDATE_MASTER_HEADER:
            {
                close(masterfd);
                update_master_header();

                /* Re-open the masterfd. */
                if ( (masterfd = open_master_fd(&myhdr, true)) < 0)
                {
                    mutex_unlock(&command_queue_mutex);
                    return;
                }

                break;
            }
            case CMD_UPDATE_NUMERIC:
            {
                modify_numeric_entry(masterfd, ce->idx_id, ce->tag, ce->data);
                break;
            }
        }

        if (++command_queue_ridx >= TAGCACHE_COMMAND_QUEUE_LENGTH)
            command_queue_ridx = 0;
    }

    close(masterfd);

    tc_stat.queue_length = 0;
    mutex_unlock(&command_queue_mutex);
}

static void run_command_queue(bool force)
{
    if (COMMAND_QUEUE_IS_EMPTY)
        return;

    if (force || command_queue_is_full())
        command_queue_sync_callback();
    else
        register_storage_idle_func(command_queue_sync_callback);
}

static void queue_command(int cmd, long idx_id, int tag, long data)
{
    while (1)
    {
        int next;

        mutex_lock(&command_queue_mutex);
        next = command_queue_widx + 1;
        if (next >= TAGCACHE_COMMAND_QUEUE_LENGTH)
            next = 0;

        /* Make sure queue is not full. */
        if (next != command_queue_ridx)
        {
            struct tagcache_command_entry *ce = &command_queue[command_queue_widx];

            ce->command = cmd;
            ce->idx_id = idx_id;
            ce->tag = tag;
            ce->data = data;

            command_queue_widx = next;

            tc_stat.queue_length++;

            mutex_unlock(&command_queue_mutex);
            break;
        }

        /* Queue is full, try again later... */
        mutex_unlock(&command_queue_mutex);
        sleep(1);
    }
}

long tagcache_increase_serial(void)
{
    long old;

    if (!tc_stat.ready)
        return -2;

    while (read_lock)
        sleep(1);

    old = current_tcmh.serial++;
    queue_command(CMD_UPDATE_MASTER_HEADER, 0, 0, 0);

    return old;
}

static long rating_generation, plays_generation;

void tagcache_update_numeric(int idx_id, int tag, long data)
{
    if (tag == tag_rating)
        rating_generation++;
    else if (tag == tag_playcount || tag == tag_lastplayed)
        plays_generation++;
    if (tag == tag_playcount || tag == tag_lastelapsed)
        states_generation++;
    queue_command(CMD_UPDATE_NUMERIC, idx_id, tag, data);
}

long tagcache_runtime_generation(bool ratings)
{
    return ratings ? rating_generation : plays_generation;
}

/* the entries, those removed too: as many as a pass over them can give */
int tagcache_entry_count(void)
{
    return current_tcmh.tch.entry_count;
}

/* whether files are found in memory by their paths, all of them */
bool tagcache_finds_entries(void)
{
#ifdef HAVE_TC_RAMCACHE
    return tc_stat.ready && tc_stat.ramcache && ram_names_handle > 0;
#else
    return false;
#endif
}

/* the entry of a file, from memory, or -1 */
long tagcache_find_entry(const char *filename)
{
#ifdef HAVE_TC_RAMCACHE
    if (tagcache_finds_entries())
    {
        const long idx_id = find_path_name(ram_names_handle, ram_names_count,
                                           filename, NULL);
        if (idx_id >= 0 && idx_id < current_tcmh.tch.entry_count
            && !(tcramcache.hdr->indices[idx_id].flag & FLAG_DELETED))
            return idx_id;
    }
#endif
    (void)filename;
    return -1;
}

static void entry_numbers(int idx_id, const struct index_entry *idx,
                          struct tagcache_numbers *n)
{
    n->length = tc_find_tag(tag_length, idx_id, idx);
    n->playcount = tc_find_tag(tag_playcount, idx_id, idx);
    n->playtime = tc_find_tag(tag_playtime, idx_id, idx);
    n->rating = tc_find_tag(tag_rating, idx_id, idx);
    n->lastplayed = tc_find_tag(tag_lastplayed, idx_id, idx);
#if defined(HAVE_TC_RAMCACHE)
    n->kind = ram_kind(idx_id);
#else
    n->kind = 0;
#endif
}

/* a numeric tag's value queued for an entry, the newest, else its own */
static long queued_or(int tag, int idx_id, const struct index_entry *idx)
{
    for (int ridx = command_queue_widx; ridx != command_queue_ridx; )
    {
        if (--ridx < 0)
            ridx = TAGCACHE_COMMAND_QUEUE_LENGTH - 1;
        if (command_queue[ridx].command == CMD_UPDATE_NUMERIC
            && command_queue[ridx].idx_id == idx_id
            && command_queue[ridx].tag == tag)
            return command_queue[ridx].data;
    }
    return idx->tag_seek[tag];
}

/* One entry's numbers as they are now, the changes queued for it too: from
   memory only, as a skin asks each frame. Without the queue's lock, which the
   storage thread holds over the disk's writes as it empties the queue: Now
   Playing waited on it. Nothing here yields, and an entry written there has
   its memory changed before it leaves the queue, so it's one or the other. */
bool tagcache_get_numbers(int idx_id, struct tagcache_numbers *n)
{
#ifdef HAVE_TC_RAMCACHE
    if (!tc_stat.ready || !tc_stat.ramcache || !tcramcache.hdr || idx_id < 0
        || idx_id >= current_tcmh.tch.entry_count)
        return false;
    const struct index_entry *idx = &tcramcache.hdr->indices[idx_id];
    if (idx->flag & FLAG_DELETED)
        return false;
    n->length = queued_or(tag_length, idx_id, idx);
    n->playcount = queued_or(tag_playcount, idx_id, idx);
    n->playtime = queued_or(tag_playtime, idx_id, idx);
    n->rating = queued_or(tag_rating, idx_id, idx);
    n->lastplayed = queued_or(tag_lastplayed, idx_id, idx);
    n->kind = ram_kind(idx_id);
    return true;
#else
    (void)idx_id;
    (void)n;
    return false;
#endif
}

int tagcache_track_kind(int idx_id)
{
#ifdef HAVE_TC_RAMCACHE
    return tc_stat.ready ? ram_kind(idx_id) : 0;
#else
    (void)idx_id;
    return 0;
#endif
}

long tagcache_track_place(int idx_id)
{
#ifdef HAVE_TC_RAMCACHE
    if (!tc_stat.ready || !tc_stat.ramcache || !tcramcache.hdr || idx_id < 0
        || idx_id >= current_tcmh.tch.entry_count)
        return 0;
    const struct index_entry *idx = &tcramcache.hdr->indices[idx_id];
    return (idx->flag & FLAG_DELETED) ? 0 : queued_or(tag_lastelapsed, idx_id, idx);
#else
    (void)idx_id;
    return 0;
#endif
}

long tagcache_states_generation(void)
{
    return states_generation;
}

int tagcache_episode_state(int idx_id)
{
#ifdef HAVE_TC_RAMCACHE
    if (!tc_stat.ready || !tc_stat.ramcache || !tcramcache.hdr || idx_id < 0
        || idx_id >= current_tcmh.tch.entry_count)
        return EPISODE_PLAYED;
    const struct index_entry *idx = &tcramcache.hdr->indices[idx_id];
    const uint8_t *kind = ram_kinds_byte(idx_id);
    if ((idx->flag & FLAG_DELETED) || !kind)
        return EPISODE_PLAYED;
    if (queued_or(tag_lastelapsed, idx_id, idx) > 0)
        return EPISODE_STARTED;
    /* Finder's by its mark; another by its plays, as it has no mark */
    if (*kind & RAM_KIND_MASK)
        return (*kind & RAM_KIND_UNPLAYED) ? EPISODE_NEW : EPISODE_PLAYED;
    return queued_or(tag_playcount, idx_id, idx) > 0 ? EPISODE_PLAYED : EPISODE_NEW;
#else
    (void)idx_id;
    return EPISODE_PLAYED;
#endif
}

int tagcache_match_entry(const int32_t *tag_seek)
{
#ifdef HAVE_TC_RAMCACHE
    return ram_entry_id((const struct index_entry *)tag_seek);
#else
    (void)tag_seek;
    return -1;
#endif
}

bool tagcache_set_unplayed(int idx_id, const char *path, bool unplayed)
{
#ifdef HAVE_TC_RAMCACHE
    uint8_t *kind = ram_kinds_byte(idx_id);
    if (!kind || !(*kind & RAM_KIND_MASK) || !itunesdb_set_unplayed(path, unplayed))
        return false;
    if (unplayed)
        *kind |= RAM_KIND_UNPLAYED;
    else
        *kind &= ~RAM_KIND_UNPLAYED;
    states_generation++;
    return true;
#else
    (void)idx_id; (void)path; (void)unplayed;
    return false;
#endif
}

long tagcache_track_rating(const struct mp3entry *id3)
{
    struct tagcache_numbers n;
    return id3->tagcache_idx && tagcache_get_numbers(id3->tagcache_idx - 1, &n)
           ? n.rating : id3->rating;
}

long tagcache_track_playcount(const struct mp3entry *id3)
{
    struct tagcache_numbers n;
    return id3->tagcache_idx && tagcache_get_numbers(id3->tagcache_idx - 1, &n)
           ? n.playcount : id3->playcount;
}

/* What an entry sorts by for a tag, from the database in memory: a number's
   value, or for a string of a sorted file its place there, which goes as the
   alphabet. 0 without the memory. */
long tagcache_sort_key(int idx_id, int tag)
{
#ifdef HAVE_TC_RAMCACHE
    /* as it is in memory, without the queue, which has only what playing
       changes: nothing waited for in the middle of a sort */
    if (tc_stat.ramcache && idx_id >= 0 && idx_id < current_tcmh.tch.entry_count
        && (TAGCACHE_IS_NUMERIC(tag) || TAGCACHE_IS_SORTED(tag)))
        return tcramcache.hdr->indices[idx_id].tag_seek[tag];
#endif
    (void)idx_id;
    (void)tag;
    return 0;
}

/* Each entry's file and what smart playlists go by, to read them only: from
   memory when the database is there, and its numbers as changed even if not
   yet written, as the queue has them */
bool tagcache_read_entries(void (*fn)(int idx_id, const char *filename,
                                      const struct tagcache_numbers *n,
                                      void *data),
                           void *data, bool names)
{
    struct tagcache_search tcs;
    struct master_header myhdr;
    struct index_entry idx;
    char buf[TAGCACHE_BUFSZ];

    if (!tagcache_search(&tcs, tag_filename))
        return false;
    const int masterfd = open_master_fd(&myhdr, false);
    if (masterfd < 0)
    {
        tagcache_search_finish(&tcs);
        return false;
    }
    for (int i = 0; i < myhdr.tch.entry_count; i++)
    {
        if (!get_index(masterfd, i, &idx, true) || (idx.flag & FLAG_DELETED)
            || (names && !tagcache_retrieve(&tcs, i, tag_filename, buf, sizeof(buf))))
            continue;
        struct tagcache_numbers n;
        entry_numbers(i, &idx, &n);
        fn(i, names ? buf : NULL, &n, data);
        /* now and then: music may play meanwhile */
        if (!(i & 63))
            yield();
    }
    close(masterfd);
    tagcache_search_finish(&tcs);
    return true;
}
#endif /* !__PCTOOL__ */

static bool write_tag(int fd, const char *tagstr, const char *datastr)
{
    char buf[512];
    const int bufsz = sizeof(buf);
    int i;

    snprintf(buf, bufsz, "%s=\"", tagstr);

    for (i = strlen(buf); i < (long)sizeof(buf)-4; i++)
    {
        if (*datastr == '\0')
            break;

        if (*datastr == '"' || *datastr == '\\')
            buf[i++] = '\\';

        else if (*datastr == '\n')
        {
            buf[i++] = '\\';
            buf[i] = 'n';
            continue;
        }

        buf[i] = *(datastr++);
    }

    str_setlen(buf, bufsz - 1);
    strmemccpy(&buf[i], "\" ", (bufsz - i - 1));

    write(fd, buf, i + 2);

    return true;
}

#ifndef __PCTOOL__

static bool read_tag(char *dest, long size,
                     const char *src, const char *tagstr)
{
    int pos;
    char current_tag[32];

    while (*src != '\0')
    {
        /* Skip all whitespace */
        while (*src == ' ')
            src++;

        if (*src == '\0')
            break;

        pos = 0;
        /* Read in tag name */
        while (*src != '=' && *src != ' ')
        {
            current_tag[pos] = *src;
            src++;
            pos++;

            if (*src == '\0' || pos >= (int) sizeof(current_tag))
                return false;
        }

        str_setlen(current_tag, pos);

        /* Read in tag data */

        /* Find the start. */
        while (*src != '"' && *src != '\0')
            src++;

        if (*src == '\0' || *(++src) == '\0')
            return false;

        /* Read the data. */
        for (pos = 0; pos < size; pos++)
        {
            if (*src == '\0')
                break;

            if (*src == '\\')
            {
                src++;
                if (*src == 'n')
                    dest[pos] = '\n';
                else
                    dest[pos] = *src;

                src++;
                continue;
            }

            if (*src == '\0')
                break;

            if (*src == '"')
            {
                src++;
                break;
            }

            dest[pos] = *(src++);
        }

        str_setlen(dest, pos);

        if (!strcasecmp(tagstr, current_tag))
            return true;
    }

    return false;
}

static int parse_changelog_line(int line_n, char *buf, void *parameters)
{
    struct index_entry idx;
    char tag_data[TAGCACHE_BUFSZ];
    int idx_id;
    long masterfd = (long)(intptr_t)parameters;
    const int import_tags[] = { tag_playcount, tag_rating, tag_playtime,
                                tag_lastplayed, tag_commitid, tag_lastelapsed,
                                tag_lastoffset, tag_dateadded };
    int i;
    (void)line_n;

    if (*buf == '#')
        return 0;

    /* logf("%d/%s", line_n, buf); */
    if (!read_tag(tag_data, sizeof tag_data, buf, "filename"))
    {
        logf("%d/filename missing", line_n);
        logf("-> %s", buf);
        return 0;
    }

    idx_id = find_index(tag_data);
    if (idx_id < 0)
    {
        logf("%d/entry not found", line_n);
        return 0;
    }

    if (!get_index(masterfd, idx_id, &idx, false))
    {
        logf("%d/failed to retrieve index entry", line_n);
        return 0;
    }

    /* Stop if tag has already been modified. */
    if (idx.flag & FLAG_DIRTYNUM)
        return 0;

    logf("%d/import: %s", line_n, tag_data);

    idx.flag |= FLAG_DIRTYNUM;
    for (i = 0; i < (long)(sizeof(import_tags)/sizeof(import_tags[0])); i++)
    {
        int data;

        if (!read_tag(tag_data, sizeof tag_data, buf,
                      tagcache_tag_to_str(import_tags[i])))
        {
            continue;
        }

        data = atoi(tag_data);
        if (data < 0)
            continue;

        idx.tag_seek[import_tags[i]] = data;

        if (import_tags[i] == tag_lastplayed && data >= current_tcmh.serial)
            current_tcmh.serial = data + 1;
        else if (import_tags[i] == tag_commitid && data >= current_tcmh.commitid)
            current_tcmh.commitid = data + 1;
    }

    return write_index(masterfd, idx_id, &idx) ? 0 : -5;
}

bool tagcache_import_changelog(void)
{
    struct master_header myhdr;
    struct tagcache_header tch;
    int clfd;
    long masterfd;
    char buf[2048];
    const int bufsz = sizeof(buf);

    if (!tc_stat.ready)
        return false;

    while (read_lock)
        sleep(1);

    clfd = open_db_fd(TAGCACHE_FILE_CHANGELOG, O_RDONLY);
    if (clfd < 0)
    {
        logf("failure to open changelog");
        return false;
    }

    if ( (masterfd = open_master_fd(&myhdr, true)) < 0)
    {
        close(clfd);
        return false;
    }

    write_lock++;
#ifdef TAGCACHE_SORT_FIELDS
    const int32_t commitid = current_tcmh.commitid;
#endif

    filenametag_fd = open_tag_fd(&tch, tag_filename, false);

    fast_readline(clfd, buf, bufsz, (void *)(intptr_t)masterfd,
                  parse_changelog_line);

    close(clfd);
    close(masterfd);

    if (filenametag_fd >= 0)
    {
        close(filenametag_fd);
        filenametag_fd = -1;
    }

    write_lock--;

#ifdef TAGCACHE_SORT_FIELDS
    if (update_master_header() && current_tcmh.commitid != commitid)
        order_mark_follow(commitid);
#else
    update_master_header();
#endif

    return true;
}

#endif /* !__PCTOOL__ */

#ifndef __PCTOOL__
/* the runtime values into an entry: false when it had them */
static bool runtime_set(struct index_entry *idx,
                        const struct tagcache_runtime *rt)
{
    const long values[] = {
        rt->playcount, rt->rating, rt->lastplayed, rt->playtime, rt->commitid,
        rt->lastelapsed, rt->lastoffset
    };
    static const unsigned char tags[] = {
        tag_playcount, tag_rating, tag_lastplayed, tag_playtime, tag_commitid,
        tag_lastelapsed, tag_lastoffset
    };
    bool changed = false;
    for (unsigned i = 0; i < ARRAYLEN(tags); i++)
    {
        changed |= idx->tag_seek[tags[i]] != values[i];
        idx->tag_seek[tags[i]] = values[i];
    }
    return changed;
}

/* Calls update() with the filename and runtime values of every entry, and
   writes them back if it returns more than 0 and they changed, or removes
   the entry if it returns less than 0. One pass, for
   imports too big to look each file up. False if an entry's counts couldn't
   be written, or it couldn't be removed: the import is then done again, rather
   than noted as done with the disk's old counts and ratings, or a song whose
   file is gone, back after a reboot. */
bool tagcache_update_runtime(int (*update)(const char *filename,
                                           struct tagcache_runtime *rt,
                                           void *data),
                             void *data)
{
    struct tagcache_search tcs;
    struct master_header myhdr;
    struct index_entry idx;
    char buf[TAGCACHE_BUFSZ];
    int masterfd;

    if (!tagcache_search(&tcs, tag_filename))
        return false;

    masterfd = open_master_fd(&myhdr, true);
    if (masterfd < 0)
    {
        tagcache_search_finish(&tcs);
        return false;
    }
    int fds[TAG_COUNT];
    for (int tag = 0; tag < TAG_COUNT; tag++)
        fds[tag] = -1;
    int deleted = 0;
    bool failed = false;

    for (int i = 0; i < myhdr.tch.entry_count; i++)
    {
        if (import_phase == TAGCACHE_IMPORT_COUNTS)
        {
            import_done = i;
            import_total = myhdr.tch.entry_count;
        }
        /* From memory when the database is there, as it is for Play Counts
           on every connection to a computer. Only an entry that changes is
           read from the disk: the copy in memory has seeks of its own. */
        /* one that can't be read, or whose name can't, is left as it is, as
           one iTunesDB hasn't: failed for it, the import would be done at
           every boot, and Play Counts never written */
        if (!get_index(masterfd, i, &idx, true)
            || !tagcache_retrieve(&tcs, i, tag_filename, buf, sizeof(buf)))
            continue;

        struct tagcache_runtime rt = {
            idx.tag_seek[tag_playcount], idx.tag_seek[tag_rating],
            idx.tag_seek[tag_lastplayed], idx.tag_seek[tag_playtime],
            idx.tag_seek[tag_commitid], idx.tag_seek[tag_lastelapsed],
            idx.tag_seek[tag_lastoffset]
        };
        int action = update(buf, &rt, data);
        if (action < 0)
        {
            /* a file that is gone, like the files a sync replaced */
            if (delete_entry_in(i, masterfd, myhdr.tch.entry_count, fds,
                                true))
            {
                deleted++;
#if !defined(__PCTOOL__) && !defined(PLUGIN)
                lp_gone_note(buf);
#endif
            }
            else
                failed = true;
        }
        else if (action > 0 && runtime_set(&idx, &rt))
        {
            /* the disk's copy, written back changed: one that can't be read
               leaves the counts unset, and the import to be done again */
            if (get_index(masterfd, i, &idx, false))
            {
                runtime_set(&idx, &rt);
                idx.flag |= FLAG_DIRTYNUM;
                failed |= !write_index(masterfd, i, &idx);
            }
            else
                failed = true;
        }
        do_timed_yield();
    }
    if (deleted > 0)
    {
#if !defined(__PCTOOL__) && !defined(PLUGIN)
        lp_gone_flush();
#endif
        sweep_unused_tags(masterfd, myhdr.tch.entry_count, fds);
        /* what's made from the database goes by this, and is out of date: Cover Flow's
           albums and the prepared lists of songs */
        current_tcmh.commitid++;
    }

    for (int tag = 0; tag < TAG_COUNT; tag++)
    {
        if (fds[tag] >= 0)
            close(fds[tag]);
    }
    failed |= close(masterfd) < 0;
    tagcache_search_finish(&tcs);
#ifdef TAGCACHE_SORT_FIELDS
    if (update_master_header() && deleted > 0)
        order_mark_follow(current_tcmh.commitid - 1);
#else
    update_master_header();
#endif
    return !failed;
}
#endif /* !__PCTOOL__ */

bool tagcache_create_changelog(struct tagcache_search *tcs)
{
    struct master_header myhdr;
    struct index_entry idx;
    char buf[TAGCACHE_BUFSZ];
    const int bufsz = sizeof(buf);
    char temp[32];
    int clfd;
    int i, j;

    if (!tc_stat.ready)
        return false;

    if (!tagcache_search(tcs, tag_filename))
        return false;

    /* Initialize the changelog */
    clfd = open_db_fd(TAGCACHE_FILE_CHANGELOG, O_WRONLY | O_CREAT | O_TRUNC);
    if (clfd < 0)
    {
        logf("failure to open changelog");
        tagcache_search_finish(tcs);
        return false;
    }

    if (tcs->masterfd < 0)
    {
        if ( (tcs->masterfd = open_master_fd(&myhdr, false)) < 0)
        {
            close(clfd);
            tagcache_search_finish(tcs);
            return false;
        }
    }
    else
    {
        lseek(tcs->masterfd, 0, SEEK_SET);
        read_master_header(tcs->masterfd, &myhdr);
    }

    write(clfd, "## Changelog version 1\n", 23);

    for (i = 0; i < myhdr.tch.entry_count; i++)
    {
        if (read_index_entries(tcs->masterfd, &idx, 1) != sizeof(struct index_entry))
        {
            logf("read error #9");
            tagcache_search_finish(tcs);
            close(clfd);
            return false;
        }

        /* Skip until the entry found has been modified. */
        if (! (idx.flag & FLAG_DIRTYNUM) )
            continue;

        /* Skip deleted entries too. */
        if (idx.flag & FLAG_DELETED)
            continue;

        /* Now retrieve all tags. */
        for (j = 0; j < TAG_COUNT; j++)
        {
            if (TAGCACHE_IS_NUMERIC(j))
            {
                itoa_buf(temp, sizeof temp, (int)idx.tag_seek[j]);
                write_tag(clfd, tagcache_tag_to_str(j), temp);
                continue;
            }

            tcs->type = j;
            tagcache_retrieve(tcs, i, tcs->type, buf, bufsz);
            write_tag(clfd, tagcache_tag_to_str(j), buf);
        }

        write(clfd, "\n", 1);
        do_timed_yield();
    }

    close(clfd);

    tagcache_search_finish(tcs);

    return true;
}

/* masterfd is the master file, open for writing, with count entries, and
   fds[] the tag files, opened as needed and left open for the next one:
   closing each after a write made a deletion take a tenth of a second.
   With sweep_after, the tags it leaves unused are deleted later by
   sweep_unused_tags(), for all the entries deleted at once. */
static bool delete_entry_in(long idx_id, int masterfd, long count, int *fds,
                            bool sweep_after)
{
    int tag, i;
    struct index_entry idx, myidx;
    int in_use[TAG_COUNT];

    logf("delete_entry(): %ld", idx_id);

#ifdef HAVE_TC_RAMCACHE
    /* At first mark the entry removed from ram cache. */
    if (tc_stat.ramcache)
        tcramcache.hdr->indices[idx_id].flag |= FLAG_DELETED;
#endif

    lseek(masterfd, sizeof(struct master_header)
          + idx_id * sizeof(struct index_entry), SEEK_SET);
    if (read_index_entries(masterfd, &myidx, 1) != sizeof(struct index_entry))
    {
        logf("delete_entry(): read error");
        return false;
    }

    if (myidx.flag & FLAG_DELETED)
    {
        logf("delete_entry(): already deleted!");
        return false;
    }

    myidx.flag |= FLAG_DELETED;
    lseek(masterfd, -(off_t)sizeof(struct index_entry), SEEK_CUR);
    if (write_index_entries(masterfd, &myidx, 1) != sizeof(struct index_entry))
    {
        logf("delete_entry(): write_error #1");
        return false;
    }

    /* Now check which tags are no longer in use (if any): a look at every
       entry for each deleted one, which took a sync that replaced 9000 files
       many minutes; those are left to one sweep after */
    for (tag = 0; tag < TAG_COUNT; tag++)
        in_use[tag] = sweep_after;
#ifdef HAVE_TC_RAMCACHE
    /* its own, and the sweep from memory has no names to go by */
    if (sweep_after && tc_stat.ramcache)
        in_use[tag_filename] = 0;
#endif

    lseek(masterfd, sizeof(struct master_header), SEEK_SET);
    for (i = 0; i < count && !sweep_after; i++)
    {
        struct index_entry *idxp;

#ifdef HAVE_TC_RAMCACHE
        /* Use RAM DB if available for greater speed */
        if (tc_stat.ramcache)
            idxp = &tcramcache.hdr->indices[i];
        else
#endif
        {
            if (read_index_entries(masterfd, &idx, 1) != sizeof(struct index_entry))
            {
                logf("delete_entry(): read error #2");
                return false;
            }
            idxp = &idx;
        }

        if (idxp->flag & FLAG_DELETED)
            continue;

        for (tag = 0; tag < TAG_COUNT; tag++)
        {
            if (TAGCACHE_IS_NUMERIC(tag))
                continue;

            if (idxp->tag_seek[tag] == myidx.tag_seek[tag])
                in_use[tag]++;
        }
    }

    /* Now delete all tags no longer in use. */
    for (tag = 0; tag < TAG_COUNT; tag++)
    {
        struct tagcache_header tch;
        int oldseek = myidx.tag_seek[tag];

        if (TAGCACHE_IS_NUMERIC(tag))
            continue;

        /**
         * Replace tag seek with a hash value of the field string data.
         * That way runtime statistics of moved or altered files can be
         * resurrected.
         */
#ifdef HAVE_TC_RAMCACHE
        if (tc_stat.ramcache && tag != tag_filename)
        {
            struct tagfile_entry *tfe;
            int32_t *seek = &tcramcache.hdr->indices[idx_id].tag_seek[tag];

            /* crc_32 is assumed not to yield (why would it...?) */
            tfe = (struct tagfile_entry *)&tcramcache.hdr->tags[tag][*seek];
            *seek = crc_32(tfe->tag_data, strlen(tfe->tag_data), 0xffffffff);
            myidx.tag_seek[tag] = *seek;
        }
        else
#endif /* HAVE_TC_RAMCACHE */
        {
            struct tagfile_entry tfe;

            /* Open the index file, which contains the tag names. */
            if (fds[tag] < 0
                && (fds[tag] = open_tag_fd(&tch, tag, true)) < 0)
                return false;

            /* Skip the header block */
            lseek(fds[tag], myidx.tag_seek[tag], SEEK_SET);

            switch (read_tagfile_entry_and_tag(fds[tag], &tfe,
                                               build_idx_buf, build_idx_bufsz))
            {
                case e_SUCCESS_LEN_ZERO:
                    logf("deleted_entry(): SUCCESS");
                    /* FALL THROUGH */
                case e_SUCCESS:
                     break;
                case e_ENTRY_SIZEMISMATCH:
                    logf("delete_entry(): read error #3");
                    return false;
                case e_TAG_TOOLONG:
                    logf("too long tag #4");
                    return false;
                case e_TAG_SIZEMISMATCH:
                    logf("delete_entry(): read error #3");
                    return false;
            }

            myidx.tag_seek[tag] = crc_32(build_idx_buf,
                                         strlen(build_idx_buf), 0xffffffff);
        }

        if (in_use[tag])
        {
            logf("in use: %d/%d", tag, in_use[tag]);
            continue;
        }

#ifdef HAVE_TC_RAMCACHE
        /* Delete from ram. */
        if (tc_stat.ramcache && tag != tag_filename)
        {
            struct tagfile_entry *tagentry =
                    (struct tagfile_entry *)&tcramcache.hdr->tags[tag][oldseek];
            str_setlen(tagentry->tag_data, 0);
        }
#endif /* HAVE_TC_RAMCACHE */

        /* Open the index file, which contains the tag names. */
        if (fds[tag] < 0
            && (fds[tag] = open_tag_fd(&tch, tag, true)) < 0)
            return false;

        /* Skip the header block */
        lseek(fds[tag], oldseek + sizeof(struct tagfile_entry), SEEK_SET);

        /* Debug, print 10 first characters of the tag
        read(fd, buf, 10);
        buf[10]='\0';
        logf("TAG:%s", buf);
        lseek(fd, -10, SEEK_CUR);
        */

        /* Write first data byte in tag as \0 */
        write(fds[tag], "", 1);
    }

    /* Write index entry back into master index. */
    lseek(masterfd, sizeof(struct master_header) +
          (idx_id * sizeof(struct index_entry)), SEEK_SET);
    if (write_index_entries(masterfd, &myidx, 1) != sizeof(struct index_entry))
    {
        logf("delete_entry(): write_error #2");
        return false;
    }

    song_count = -1;

    return true;
}

static int cmp_seek(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

static bool seek_in(const int32_t *sorted, int n, int32_t seek)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (sorted[mid] == seek)
            return true;
        if (sorted[mid] < seek)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}

#ifdef HAVE_TC_RAMCACHE
/* The same from the strings in memory, which lie as they do in the files, so only a
   string to delete goes to the disk, with a bit for each place a string can start in
   place of a sorted list. Going over the files took much longer than the whole import
   around it. Filenames aren't in memory: delete_entry_in() has done those. */
static void sweep_unused_tags_ram(long count, int *fds)
{
    tcrc_buffer_lock(); /* the strings are movable, and writing lets others run */
    for (int tag = 0; tag < TAG_COUNT; tag++)
    {
        struct tagcache_header tch;
        if (TAGCACHE_IS_NUMERIC(tag) || tag == tag_filename)
            continue;

        char *const strings = tcramcache.hdr->tags[tag];
        const long entries = tcramcache.hdr->entry_count[tag];
        long size = sizeof(struct tagcache_header);
        for (long i = 0; i < entries; i++)
        {
            const struct tagfile_entry *tfe =
                (const struct tagfile_entry *)&strings[size];
            if (tfe->tag_length <= 0)
                break;
            size += sizeof(struct tagfile_entry) + tfe->tag_length;
        }

        const size_t words = size / (4 * 32) + 1;
        const int handle = core_alloc(words * sizeof(uint32_t));
        if (handle <= 0)
            continue; /* left in the files, shown in no list of a tag's own */
        core_pin(handle);
        uint32_t *used = core_get_data(handle);
        memset(used, 0, words * sizeof(uint32_t));
        for (long i = 0; i < count; i++)
        {
            const struct index_entry *e = &tcramcache.hdr->indices[i];
            const int32_t seek = e->tag_seek[tag];
            if (!(e->flag & FLAG_DELETED) && seek >= 0 && seek < size)
                used[seek / (4 * 32)] |= 1u << ((seek / 4) % 32);
        }

        for (long pos = sizeof(struct tagcache_header); pos < size; )
        {
            struct tagfile_entry *tfe = (struct tagfile_entry *)&strings[pos];
            if (tfe->tag_data[0] != '\0'
                && !(used[pos / (4 * 32)] & (1u << ((pos / 4) % 32))))
            {
                if (fds[tag] < 0 && (fds[tag] = open_tag_fd(&tch, tag, true)) < 0)
                    break;
                lseek(fds[tag], pos + sizeof(struct tagfile_entry), SEEK_SET);
                write(fds[tag], "", 1);
                str_setlen(tfe->tag_data, 0);
            }
            pos += sizeof(struct tagfile_entry) + tfe->tag_length;
        }
        core_unpin(handle);
        core_free(handle);
        do_timed_yield();
    }
    tcrc_buffer_unlock();
}
#endif /* HAVE_TC_RAMCACHE */

/* Every string of the tag files that no entry uses deletes, as
   delete_entry_in() does for one entry's, in one pass over each file */
static void sweep_unused_tags(int masterfd, long count, int *fds)
{
#ifdef HAVE_TC_RAMCACHE
    if (tc_stat.ramcache)
    {
        sweep_unused_tags_ram(count, fds);
        return;
    }
#endif
    int handle = core_alloc(count * sizeof(int32_t));
    if (handle <= 0)
        return; /* left in the files, shown in no list of a tag's own */
    core_pin(handle);
    int32_t *used = core_get_data(handle);

    for (int tag = 0; tag < TAG_COUNT; tag++)
    {
        struct tagcache_header tch;
        struct index_entry idx;
        if (TAGCACHE_IS_NUMERIC(tag))
            continue;

        /* the seeks still in use, sorted */
        int n = 0;
        lseek(masterfd, sizeof(struct master_header), SEEK_SET);
        for (long i = 0; i < count; i++)
        {
            const struct index_entry *e = &idx;
#ifdef HAVE_TC_RAMCACHE
            if (tc_stat.ramcache)
                e = &tcramcache.hdr->indices[i];
            else
#endif
            if (read_index_entries(masterfd, &idx, 1) != sizeof(idx))
                break;
            if (!(e->flag & FLAG_DELETED))
                used[n++] = e->tag_seek[tag];
        }
        qsort(used, n, sizeof(int32_t), cmp_seek);

        if (fds[tag] < 0 && (fds[tag] = open_tag_fd(&tch, tag, true)) < 0)
            continue;
        off_t size = ffilesize(fds[tag]);
        off_t pos = sizeof(struct tagcache_header);
        while (pos + (off_t)sizeof(struct tagfile_entry) < size)
        {
            struct tagfile_entry tfe;
            char first;
            lseek(fds[tag], pos, SEEK_SET);
            if (read(fds[tag], &tfe, sizeof(tfe)) != sizeof(tfe)
                || tfe.tag_length <= 0
                || read(fds[tag], &first, 1) != 1)
                break;
            int32_t seek = pos;
            if (first != '\0' && !seek_in(used, n, seek))
            {
                lseek(fds[tag], pos + sizeof(tfe), SEEK_SET);
                write(fds[tag], "", 1);
#ifdef HAVE_TC_RAMCACHE
                if (tc_stat.ramcache && tag != tag_filename)
                    str_setlen(((struct tagfile_entry *)
                                &tcramcache.hdr->tags[tag][pos])->tag_data, 0);
#endif
            }
            pos += sizeof(tfe) + tfe.tag_length;
        }
        do_timed_yield();
    }

    core_unpin(handle);
    core_free(handle);
}

static bool delete_entry(long idx_id)
{
    struct master_header myhdr;
    int fds[TAG_COUNT];
    int masterfd = open_master_fd(&myhdr, true);
    if (masterfd < 0)
        return false;

    for (int tag = 0; tag < TAG_COUNT; tag++)
        fds[tag] = -1;
    bool ok = delete_entry_in(idx_id, masterfd, myhdr.tch.entry_count, fds,
                              false);
    for (int tag = 0; tag < TAG_COUNT; tag++)
    {
        if (fds[tag] >= 0)
            close(fds[tag]);
    }
    close(masterfd);
    return ok;
}

/**
 * Returns true if there is an event waiting in the queue
 * that requires the current operation to be aborted.
 */
/* the pass at boot that finds each file in the dircache for its reference:
   13000 of them took 84 s on the iPod, in the background */
static bool refs_pass;

static bool check_event_queue(void)
{
#ifndef __PCTOOL__
    /* Anywhere in the queue, not its head alone: other broadcasts stand
       before SYS_USB_CONNECTED (SYS_USB_LUN_LOCKED always, the charger's
       often), and the thread takes none while it scans. */
    static const long stops[][2] = {
        { Q_STOP_SCAN, Q_STOP_SCAN },
        { SYS_POWEROFF, SYS_POWEROFF },
        { SYS_REBOOT, SYS_REBOOT },
        { SYS_USB_CONNECTED, SYS_USB_CONNECTED },
        /* the last two, in the refs pass only: it gives way to an import or
           a check, which a screen waits for. Files lists find by their paths
           in memory, so references left out cost little. */
        { Q_SYNCED, Q_SYNCED },
        { Q_CHECK_OUTSIDE, Q_CHECK_OUTSIDE },
    };
    /* the count in the flags is of the filters after the first */
    return queue_peek_ex(&tagcache_queue, NULL,
                         ARRAYLEN(stops) - (refs_pass ? 1 : 3), stops);
#else
    return false;
#endif /* __PCTOOL__ */
}

#ifdef HAVE_TC_RAMCACHE

static void fix_ramcache(void* old_addr, void* new_addr)
{
    ptrdiff_t offpos = new_addr - old_addr;
    for (int i = 0; i < TAG_COUNT; i++)
        tcramcache.hdr->tags[i] += offpos;
}

static int move_cb(int handle, void* current, void* new)
{
    (void)handle;
    fix_ramcache(current, new);
    tcramcache.hdr = new;
    return BUFLIB_CB_OK;
}

static struct buflib_callbacks ops = {
    .move_callback = move_cb,
    .shrink_callback = NULL,
};

static bool allocate_tagcache(void)
{
    tc_stat.ramcache_allocated = 0;
    tcramcache.handle = 0;
    tcramcache.hdr = NULL;

    /* Load the header. */
    struct master_header tcmh;
    int fd = open_master_fd(&tcmh, false);
    if (fd < 0)
        return false;

    close(fd);

    /**
     * Now calculate the required cache size plus
     * some extra space for alignment fixes.
     */
    size_t alloc_size = tcmh.tch.datasize + 256 + TAGCACHE_RESERVE +
        sizeof(struct ramcache_header) + TAG_COUNT*sizeof(void *);
#ifdef HAVE_DIRCACHE
    alloc_size += tcmh.tch.entry_count*sizeof(struct dircache_fileref);
#endif

    int handle = core_alloc_ex(alloc_size, &ops);
    if (handle <= 0)
        return false;

    tcramcache.handle = handle;
    tcramcache.hdr = core_get_data(handle);
    tc_stat.ramcache_allocated = alloc_size;

    memset(tcramcache.hdr, 0, sizeof(struct ramcache_header));
    memcpy(&current_tcmh, &tcmh, sizeof current_tcmh);
    logf("tagcache: %d bytes allocated.", tc_stat.ramcache_allocated);
    /* Not the room for the files' names too, here at the start: as much again
       took the audio buffer's room, whose shrinking waits for the audio
       thread, which didn't answer yet, and this thread stood still for good.
       load_ramcache takes it. */

    return true;
}

#ifdef HAVE_EEPROM_SETTINGS
static bool tagcache_dumpload(void)
{
    struct statefile_header shdr;
    int fd, rc, handle;

    tcramcache.handle = 0;
    tcramcache.hdr = NULL;

    fd = open_db_fd(TAGCACHE_STATEFILE, O_RDONLY);
    if (fd < 0)
    {
        logf("no tagcache statedump");
        return false;
    }

    /* Check the statefile memory placement */
    rc = read(fd, &shdr, sizeof(struct statefile_header));
    if (rc != sizeof(struct statefile_header)
        || shdr.magic != TAGCACHE_STATEFILE_MAGIC
        || shdr.mh.tch.magic != TAGCACHE_MAGIC)
    {
        logf("incorrect statefile");
        close(fd);
        return false;
    }

    /* Lets allocate real memory and load it */
    handle = core_alloc_ex(shdr.tc_stat.ramcache_allocated, &ops);
    if (handle <= 0)
    {
        logf("alloc failure");
        return false;
    }

    tcramcache.handle = handle;
    tcrc_buffer_lock();
    tcramcache.hdr = core_get_data(handle);
    rc = read(fd, tcramcache.hdr, shdr.tc_stat.ramcache_allocated);
    tcrc_buffer_unlock();

    close(fd);

    if (rc != shdr.tc_stat.ramcache_allocated)
    {
        logf("read failure!");
        core_free(handle);
        return false;
    }

    tc_stat = shdr.tc_stat;

    /* Now fix the pointers */
    fix_ramcache(shdr.hdr, tcramcache.hdr);

    /* Load the tagcache master header (should match the actual DB file header). */
    memcpy(&current_tcmh, &shdr.mh, sizeof current_tcmh);

    return true;
}

static bool tagcache_dumpsave(void)
{
    struct statefile_header shdr;
    int fd;

    if (!tc_stat.ramcache)
        return false;

    fd = open_db_fd(TAGCACHE_STATEFILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
    {
        logf("failed to create a statedump");
        return false;
    }

    /* Create the header */
    shdr.magic = TAGCACHE_STATEFILE_MAGIC;
    shdr.hdr = tcramcache.hdr;
    memcpy(&shdr.mh, &current_tcmh, sizeof current_tcmh);
    memcpy(&shdr.tc_stat, &tc_stat, sizeof tc_stat);
    write(fd, &shdr, sizeof shdr);

    /* And dump the data too */
    tcrc_buffer_lock();
    write(fd, tcramcache.hdr, tc_stat.ramcache_allocated);
    tcrc_buffer_unlock();
    close(fd);

    return true;
}
#endif /* HAVE_EEPROM_SETTINGS */

/* a load stopped for USB or switching off, not one that failed: the database
   is as good as before, and is loaded again after */
static bool load_cut_short;

static bool load_tagcache(void)
{
    /* DEBUG: After tagcache commit and dircache rebuild, hdr-sturcture
     * may become corrupt. */

    bool ok = false;
    ssize_t bytesleft = tc_stat.ramcache_allocated - sizeof(struct ramcache_header);
    int fd;

#ifdef HAVE_DIRCACHE
    /* Wait for any in-progress dircache build to complete */
    dircache_wait();
#endif /* HAVE_DIRCACHE */

    logf("loading tagcache to ram...");

    tcrc_buffer_lock(); /* lock for the rest of the scan, simpler to handle */

    fd = open_db_fd(TAGCACHE_FILE_MASTER, O_RDONLY);
    if (fd < 0)
    {
        logf("tagcache open failed");
        goto failure;
    }

    struct master_header tcmh;
    if (read_master_header(fd, &tcmh) != sizeof(struct master_header) ||
        tcmh.tch.magic != TAGCACHE_MAGIC)
    {
        logf("incorrect header");
        goto failure;
    }

    /* Master header copy should already match, this can be redundant to do. */
    current_tcmh = tcmh;

    /* Load the master index table. */
    /* in reads of many, and not a call for each of them, letting the UI go
       between them: all at once, 1.2 MB of 13000 songs, it waited 200 ms, as
       the disk is waited on rather than given up below the UI's priority */
#define INDICES_A_READ 512
    if (tcmh.tch.entry_count < 0
        || tcmh.tch.entry_count > bytesleft / (ssize_t)sizeof(struct index_entry))
    {
        logf("too big tagcache.");
        goto failure;
    }
    const ssize_t indices_size =
        tcmh.tch.entry_count * (ssize_t)sizeof(struct index_entry);
    bytesleft -= indices_size;
    for (int done = 0; done < tcmh.tch.entry_count; done += INDICES_A_READ)
    {
        const int count = MIN(INDICES_A_READ, tcmh.tch.entry_count - done);
        if (read_index_entries(fd, &tcramcache.hdr->indices[done], count)
            != count * (ssize_t)sizeof(struct index_entry))
        {
            logf("read error #10");
            goto failure;
        }
        yield();
    }

    close(fd);
    fd = -1;

    /* Load the tags right after the index entries */
    char *p = (char *)&tcramcache.hdr->indices[tcmh.tch.entry_count];

    for (int tag = 0; tag < TAG_COUNT; tag++)
    {
        ssize_t rc;

        if (TAGCACHE_IS_NUMERIC(tag))
            continue;

        p = TC_ALIGN_PTR(p, struct tagcache_header, &rc);
        bytesleft -= rc;
        if (bytesleft < (ssize_t)sizeof(struct tagcache_header))
        {
            logf("Too big tagcache #10.5");
            goto failure;
        }

        tcramcache.hdr->tags[tag] = p;

        /* Load the header */
        struct tagcache_header *tch = (struct tagcache_header *)p;
        p += sizeof(struct tagcache_header);
        bytesleft -= sizeof (struct tagcache_header);

        fd = open_tag_fd(tch, tag, false);
        if (rc < 0)
            goto failure;
        tagr_start(fd);

        /* Load the entries for this tag */
        for (tcramcache.hdr->entry_count[tag] = 0;
             tcramcache.hdr->entry_count[tag] < tch->entry_count;
             tcramcache.hdr->entry_count[tag]++)
        {
            /* Abort if we got a critical event in queue */
            if (do_timed_yield() && check_event_queue())
            {
                load_cut_short = true;
                goto failure;
            }

            p = TC_ALIGN_PTR(p, struct tagfile_entry, &rc);
            bytesleft -= rc;
            if (bytesleft < (ssize_t)sizeof(struct tagfile_entry))
            {
                logf("Too big tagcache #10.75");
                goto failure;
            }

            struct tagfile_entry *fe = (struct tagfile_entry *)p;
            off_t pos = tagr.at;

            /* Load the header for the tag itself */
            if (tagr_read_entry(fe) != sizeof(struct tagfile_entry))
            {
                /* End of lookup table. */
                logf("read error #11");
                goto failure;
            }

            int idx_id = fe->idx_id; /* dircache reference clobbers *fe */
            struct index_entry *idx = &tcramcache.hdr->indices[idx_id];

            if (idx_id != -1 || tag == tag_filename) /* filename NOT optional */
            {
                if (idx_id < 0 || idx_id >= tcmh.tch.entry_count)
                {
                    logf("corrupt tagfile entry:tag=%d:idxid=%d", tag, idx_id);
                    goto failure;
                }

                /* A deleted entry keeps hashes in place of its seeks, to
                   bring its statistics back if the file returns */
                if (idx->tag_seek[tag] != pos && !(idx->flag & FLAG_DELETED))
                {
                    logf("corrupt data structures!:");
                    logf("  tag_seek[%d]=%" PRId32 ":pos=%ld", tag,
                         idx->tag_seek[tag], (long) pos);
                    goto failure;
                }
            }

            /* We have a special handling for the filename tags; neither the
               paths nor the entry headers are stored; only the tagcache header
               and dircache references are. */
            if (tag == tag_filename)
            {
            #ifdef HAVE_DIRCACHE
                if (idx->flag & FLAG_DIRCACHE)
                {
                    /* This flag must not be used yet. */
                    logf("internal error!");
                    goto failure;
                }

                p += sizeof (struct dircache_fileref);
                bytesleft -= sizeof (struct dircache_fileref);
            #endif /* HAVE_DIRCACHE */

                char filename[TAGCACHE_BUFSZ];
                if (fe->tag_length >= (long)sizeof(filename)-1)
                {
                    br_read(&tagr, filename, 10);
                    str_setlen(filename, 10);
                    logf("TAG:%s", filename);
                    logf("too long filename");
                    goto failure;
                }

                /* Without the dircache the names are read only to check the
                   files, or for the tables of the files */
                if ((idx->flag & FLAG_DELETED)
#ifndef HAVE_DIRCACHE
                    || (!global_settings.tagcache_autoupdate
#ifndef __PCTOOL__
                        && ram_names_handle <= 0
#endif
                       )
#endif
                    )
                {
                    /* seek over tag data instead of reading */
                    tagr.at += fe->tag_length;
                    continue;
                }

                if (br_read(&tagr, filename, fe->tag_length) != fe->tag_length)
                {
                    logf("read error #12");
                    goto failure;
                }
#ifndef __PCTOOL__
                if (ram_names_count >= ram_names_capacity)
                    ram_names_overflow = true;
                else if (filename[0])
                {
                    filename[fe->tag_length] = '\0';
                    struct path_name *names = core_get_data(ram_names_handle);
                    uint64_t hash = itunesdb_path_hash(filename);
                    names[ram_names_count].hash = hash;
                    names[ram_names_count].idx_id = idx_id;
                    ram_names_count++;
                    ram_path_add(idx_id, filename, hash);
                }
#endif
                continue;
            }

            bytesleft -= sizeof(struct tagfile_entry) + fe->tag_length;
            if (bytesleft < 0)
            {
                logf("too big tagcache #2");
                logf("tl: %" PRId32, fe->tag_length);
                logf("bl: %ld", (long) bytesleft);
                goto failure;
            }

            p = fe->tag_data;
            rc = br_read(&tagr, p, fe->tag_length);
            p += rc;

            if (rc != fe->tag_length)
            {
                logf("read error #13");
                logf("rc=0x%04x", (unsigned int)rc); // 0x431
                logf("len=0x%04" PRIx32, fe->tag_length); // 0x4000
                logf("pos=0x%04lx", (unsigned long) tagr.at); // 0x433
                logf("tag=0x%02x", tag); // 0x00
                goto failure;
            }
        }

    #ifdef HAVE_DIRCACHE
        if (tag == tag_filename)
            p = (char *)&tcrc_dcfrefs[tcmh.tch.entry_count];
    #endif /* HAVE_DIRCACHE */

        close(fd);
    }

    tc_stat.ramcache_used = tc_stat.ramcache_allocated - bytesleft;
    logf("tagcache loaded into ram!");
    logf("utilization: %d%%", 100*tc_stat.ramcache_used / tc_stat.ramcache_allocated);

    ok = true;

failure:
    if (fd >= 0)
        close(fd);

    tcrc_buffer_unlock();
    return ok;
}
#endif /* HAVE_TC_RAMCACHE */

/* A name of macOS's own, passed over: the Trash, whose songs went into the
   library, and the "._" file beside each one copied to FAT with its attributes,
   which every scan opened and turned down. Not every name with a dot first, as
   the file browser hides: "...Baby One More Time" is an album's. name ends at
   a '/' or at its end. */
static bool hidden_name(const char *name)
{
    static const char * const folders[] = {
        ".Trashes", ".Spotlight-V100", ".fseventsd", ".TemporaryItems",
        ".DocumentRevisions-V100", ".Trash",
    };
    if (name[0] != '.')
        return false;
    /* and .Trash-1000 as Linux makes */
    if (name[1] == '_' || !strncmp(name, ".Trash-", 7))
        return true;
    for (size_t i = 0; i < ARRAYLEN(folders); i++)
    {
        size_t len = strlen(folders[i]);
        if (!strncasecmp(name, folders[i], len)
            && (name[len] == '\0' || name[len] == '/'))
            return true;
    }
    return false;
}

/* What the stock firmware keeps in iPod_Control besides the songs, as the
   games iTunes puts on an iPod with their sounds (Games_RO): no music of the
   library, though the scan after a sync took their .wav files for songs, an
   album in Cover Flow each. Of iPod_Control only Music, Finder's folder, is
   looked through, and the entries of the rest are removed. */
static bool ipod_control_private(const char *path)
{
    static const char dir[] = "/iPod_Control/";
#ifdef HAVE_MULTIVOLUME
    path_strip_volume(path, &path, false);
#endif
    if (strncasecmp(path, dir, sizeof(dir) - 1))
        return false;
    const char *rest = path + sizeof(dir) - 1;
    const char *slash = strchr(rest, '/');
    const size_t len = slash ? (size_t)(slash - rest) : strlen(rest);
    return len != 5 || strncasecmp(rest, "Music", 5);
}

/* a path with such a name in it */
static bool hidden_path(const char *path)
{
    for (const char *s = path; (s = strchr(s, '/')); s++)
    {
        if (hidden_name(s + 1))
            return true;
    }
    return false;
}

#ifndef __PCTOOL__
/* during an import, which checks the files in Finder's folder against
   iTunesDB: looking for each of them took 10 ms after a sync */
static bool check_skips_synced;
#endif

#ifdef HAVE_DIRCACHE
/* whether the volume of the path is mounted: the files of one that isn't are
   said to be gone too */
static bool path_volume_mounted(const char *path)
{
#ifdef HAVE_MULTIVOLUME
    const char *rest;
    const int volume = path_strip_volume(path, &rest, false);
    if (volume != ROOT_VOLUME && volume >= 0)
        return volume_ismounted(volume);
#else
    (void)path;
#endif
    return true;
}
#endif

/* The entries the check of the files removes, through the files opened once
   and their strings swept at the end in one pass, as the import's: each by
   itself opened the files, went over all the entries for its strings and
   closed them again, 0.17 s on the iPod */
static int gone_masterfd = -1;
static long gone_count;
static int gone_fds[TAG_COUNT];

static bool remove_gone(long idx_id)
{
    /* by itself while music plays: the sweep takes memory, which the music's
       buffer would give up, stopping it */
    if (audio_status() & AUDIO_STATUS_PLAY)
        return delete_entry(idx_id);
    if (gone_masterfd < 0)
    {
        struct master_header hdr;
        gone_masterfd = open_master_fd(&hdr, true);
        if (gone_masterfd < 0)
            return false;
        gone_count = hdr.tch.entry_count;
        for (int tag = 0; tag < TAG_COUNT; tag++)
            gone_fds[tag] = -1;
    }
    return delete_entry_in(idx_id, gone_masterfd, gone_count, gone_fds, true);
}

static void remove_gone_done(void)
{
    if (gone_masterfd < 0)
        return;
    /* not for music started meanwhile, whose buffer would give the sweep its
       memory: the strings stay in the files, as when there's none */
    if (!(audio_status() & AUDIO_STATUS_PLAY))
        sweep_unused_tags(gone_masterfd, gone_count, gone_fds);
    for (int tag = 0; tag < TAG_COUNT; tag++)
    {
        if (gone_fds[tag] >= 0)
            close(gone_fds[tag]);
    }
    close(gone_masterfd);
    gone_masterfd = -1;
}

#ifdef HAVE_DIRCACHE
/* where the last entry's file was found, for the next: the entries are in
   their folders' order */
static struct dircache_hint check_hint;
#endif

/* An entry's file, looked for: 0 for one of Finder's, which the import goes
   by, 1 for one that's there, 2 for one gone, whose entry is removed */
static int check_file_ref(long idx_id, const char *path, bool auto_update)
{
#ifndef __PCTOOL__
    if (check_skips_synced && itunesdb_synced_path(path))
        return 0;
#endif
    /* one of macOS's, or of iPod_Control's other folders, from a scan before
       they were passed over */
    if (auto_update && (hidden_path(path) || ipod_control_private(path)))
        return remove_gone(idx_id) ? 2 : 1;
#ifdef HAVE_DIRCACHE
    struct index_entry *idx = &tcramcache.hdr->indices[idx_id];
    unsigned int searchflag;
    if (!auto_update)
    {
        if (idx->flag & FLAG_DIRCACHE) /* already found */
            return 1;
        searchflag = DCS_CACHED_PATH; /* attempt to load cache references */
    }
    else /* If auto updating, check storage too */
    {
        searchflag = DCS_STORAGE_PATH;
    }

    /* from where the last was found: from the start of the path, 13000 songs
       outside Finder's folder, in 1000 artists' folders, took 54 s, at every
       boot and connection. One not found so is looked for from the start, which
       says if it's gone. */
    int rc_cache = dircache_search_next(searchflag | DCS_UPDATE_FILEREF,
                                        &tcrc_dcfrefs[idx_id], path, &check_hint);

    if (rc_cache > 0)           /* in cache and we have fileref */
        idx->flag |= FLAG_DIRCACHE;
    /* 0: not in cache but okay. A file that isn't there fails with ENOENT
       in errno: compared with the result itself, none was ever removed */
    if (rc_cache >= 0 || !auto_update || errno != ENOENT
        || !path_volume_mounted(path))
        return 1;
#else
    if (file_exists(path))
        return 1;
#endif /* HAVE_DIRCACHE */
    logf("Entry no longer valid.");
    logf("-> %s", path);
    if (!remove_gone(idx_id))
        return 1;
#if !defined(__PCTOOL__) && !defined(PLUGIN)
    /* out of the playlists made on the iPod too, at Updating Library */
    lp_gone_note(path);
#endif
    return 2;
}

#if defined(HAVE_TC_RAMCACHE) && !defined(__PCTOOL__)
/* The files of the entries looked for, by their paths in memory. The paths
   stay where they are for it, and Finder's are passed over without a copy:
   a copy, a lookup of the paths and a look at the queue for every one took
   0.31 s for 13000, a look at Finder's paths still 0.17 s. */
static void ram_check_file_refs(char *buf, size_t bufsz, bool auto_update,
                                int *outside, int *deleted)
{
    const char *base = core_get_data_pinned(ram_paths_handle);
    const uint32_t *offsets = (const uint32_t *)base;
    const uint8_t *kinds = (const uint8_t *)(offsets + ram_paths_count);
    const char *pool = (const char *)(kinds + ram_paths_count);
    for (long i = 0; i < current_tcmh.tch.entry_count; i++)
    {
        if (!(i & 63) && check_event_queue())
            break;
        do_timed_yield();
        processed_dir_count++;
        /* the bar's first half: each file gone took 0.17 s to remove, and a
           hundred left it empty */
        if (scan_outside)
        {
            import_done = i;
            import_total = 2 * current_tcmh.tch.entry_count;
        }
        if (check_skips_synced && !(kinds[i] & RAM_KIND_OUTSIDE))
            continue;
        uint32_t off = offsets[i];
        /* none for a name empty */
        if (!off || (tcramcache.hdr->indices[i].flag & FLAG_DELETED))
            continue;
        strmemccpy(buf, pool + off - 1, bufsz);
        int res = check_file_ref(i, buf, auto_update);
        *outside += res > 0;
        *deleted += res == 2;
    }
    core_put_data_pinned((void *)base);
}
#endif

static bool check_deleted_file_refs(bool auto_update)
{
    int fd;
    bool ret = true;
    char buf[TAGCACHE_BUFSZ];
    const int bufsz = sizeof(buf);
    struct tagfile_entry tfe;
    struct tagcache_header hdr;

    logf("reverse scan...");

#ifdef HAVE_DIRCACHE
    /* not while a commit has it unloaded: its entries are written below, and
       the database can be bigger than its room by then */
    if (tcramcache.handle > 0 && tc_stat.ramcache)
        tcrc_buffer_lock();
    else
        return false;
    /* Wait for any in-progress dircache build to complete */
    dircache_wait();
#else
    if (!auto_update)
        return false;
#endif

    processed_dir_count = 0;
    int deleted = 0, outside = 0;
    refs_pass = !auto_update;
#ifdef HAVE_DIRCACHE
    dircache_hint_init(&check_hint);
#endif

#if defined(HAVE_TC_RAMCACHE) && !defined(__PCTOOL__)
    /* From the paths in memory when they're all there: read from the filename
       file, 13000 of them took 0.44 s at every USB connection */
    if (ram_paths_all())
    {
        fd = -1;
        ram_check_file_refs(buf, bufsz, auto_update, &outside, &deleted);
        goto wend_finished;
    }
#endif

    fd = open_tag_fd(&hdr, tag_filename, false); /* open read only*/

    if (fd < 0)
    {
        logf(TAGCACHE_FILE_INDEX " open fail", tag_filename);
#ifdef HAVE_DIRCACHE
        /* the cache locked above, which kept it from moving from then on */
        tcrc_buffer_unlock();
#endif
        refs_pass = false;
        return false;
    }

    tagr_start(fd);

    while (!check_event_queue())
    {
        /* first, as most go on to the next at once: at the end, the 13000 found
           already in the dircache read their names for 200 ms without it */
        do_timed_yield();
        int res = tagr_read_entry_and_tag(&tfe, buf, bufsz);
        processed_dir_count++;

        switch (res)
        {
            case e_ENTRY_SIZEMISMATCH:
                logf("size mismatch entry EOF?"); /* likely EOF */
                ret = false;
                goto wend_finished;
            case e_TAG_TOOLONG:
                logf("too long tag");
                ret = false;
                goto wend_finished;
            case e_TAG_SIZEMISMATCH:
                logf("size mismatch tag - read error #14");
                ret = false;
                goto wend_finished;
            case e_SUCCESS:
                break;
            case e_SUCCESS_LEN_ZERO:
                continue;
        }

        res = check_file_ref(tfe.idx_id, buf, auto_update);
        outside += res > 0;
        deleted += res == 2;
    }

wend_finished:

#ifdef HAVE_DIRCACHE
    if (tcramcache.handle > 0)
        tcrc_buffer_unlock();
#endif
    if (fd >= 0)
        close(fd);
    refs_pass = false;
    /* after the unlock: the sweep locks the strings in memory itself */
    remove_gone_done();
    logf("done");

#ifndef __PCTOOL__
    /* the bar's second half, the scan after, by the songs new: half way
       while those known are passed over, quickly */
    if (scan_outside)
    {
        outside_known = MAX(outside - deleted, 0);
        import_total = 2;
        import_done = 1;
    }
#endif
    if (deleted > 0)
    {
        /* what's made from the database goes by this, as after an import that
           removed songs: the lists kept, the prepared one of all songs, Cover
           Flow's albums. A commit counts one too, but with no song new, none
           comes */
        current_tcmh.commitid++;
#ifdef TAGCACHE_SORT_FIELDS
        if (update_master_header())
            order_mark_follow(current_tcmh.commitid - 1);
#else
        update_master_header();
#endif
#if !defined(__PCTOOL__) && !defined(PLUGIN)
        /* the songs gone, kept for the playlists made on the iPod */
        lp_gone_flush();
#endif
    }

    return ret;
}

/* Note that this function must not be inlined, otherwise the whole point
 * of having the code in a separate function is lost.
 */
static void NO_INLINE check_ignore(const char *dirname,
    int *ignore, int *unignore)
{
    char newpath[MAX_PATH];
    const int bufsz = sizeof(newpath);

    /* check for a database.ignore file */
    snprintf(newpath, bufsz, "%s/database.ignore", dirname);
    *ignore = file_exists(newpath);
    /* check for a database.unignore file */
    snprintf(newpath, bufsz, "%s/database.unignore", dirname);
    *unignore = file_exists(newpath);
}

/* max roots on native. on application more can be added via malloc() */
#define MAX_STATIC_ROOTS 12

static struct search_roots_ll {
    const char *path;
    struct search_roots_ll * next;
} roots_ll[MAX_STATIC_ROOTS];

/* check if the path is already included in the search roots, by the
 * means that the path itself or one of its parents folders is in the list */
static bool search_root_exists(const char *path)
{
    struct search_roots_ll *this;
    for(this = &roots_ll[0]; this; this = this->next)
    {
        size_t root_len = strlen(this->path);
        /* check if the link target is inside of an existing search root
         * don't add if target is inside, we'll scan it later */
        if (!strncmp(this->path, path, root_len))
            return true;
    }
    return false;
}

#ifdef APPLICATION
/*
 * This adds a path to the search roots, possibly during traveling through
 * the filesystem. It only adds if the path is not inside an already existing
 * search root.
 *
 * Returns true if it added the path to the search roots
 *
 * Windows 2000 and greater supports symlinks, but they don't provide
 * realpath() or readlink(), and symlinks are rarely used on them so
 * ignore this for windows for now
 **/
static bool add_search_root(const char *name)
{
    (void)name;
#ifndef WIN32
    struct search_roots_ll *this, *prev = NULL;
    char target[MAX_PATH];
    const int target_bufsz = sizeof(target);
    /* Okay, realpath() is almost completely broken on android
     *
     * It doesn't accept NULL for resolved_name to dynamically allocate
     * the resulting path; and it assumes resolved_name to be PATH_MAX
     * (actually MAXPATHLEN, but it's the same [as of 2.3]) long
     * and blindly writes to the end if it
     *
     * therefore use sufficiently large static storage here
     * Note that PATH_MAX != MAX_PATH
     **/
    static char abs_target[PATH_MAX];
    ssize_t len;

    len = readlink(name, target, target_bufsz-1);
    if (len < 0)
        return false;

    str_setlen(target, len);
    if (realpath(target, abs_target) == NULL)
        return false;

    if (search_root_exists(abs_target))
        return false;

    /* get the end of the list */
    for(this = &roots_ll[0]; this; prev = this, this = this->next);

    if (prev)
    {
        size_t len = strlen(abs_target) + 1; /* count \0 */
        this = malloc(sizeof(struct search_roots_ll) + len );
        if (!this || len > MIN(PATH_MAX, MAX_PATH))
        {
            logf("Error at adding a search root: %s", this ? "path too long":"OOM");
            free(this);
            prev->next = NULL;
            return false;
        }
        this->path = ((char*)this) + sizeof(struct search_roots_ll);
        strcpy((char*)this->path, abs_target); /* ok to cast const away here */
        this->next = NULL;
        prev->next = this;
        logf("Added %s to the search roots\n", abs_target);
        return true;
    }
#endif
    return false;
}

static int free_search_root_single(struct search_roots_ll * start)
{
    if (start < &roots_ll[0] && start >= &roots_ll[MAX_STATIC_ROOTS])
    {
        free(start->next);
        return sizeof(struct search_roots_ll);
    }
    return 0;
}

static int free_search_roots(struct search_roots_ll * start)
{
    int ret = 0;
    if (start->next)
    {
        ret += free_search_root_single(start->next);
    }
    return ret;
}
#else /* native, simulator */
#define add_search_root(a) do {} while(0)
#define free_search_roots(a) do {} while(0)
#endif

/* Rockbox's own folder: it has no music, as its database.ignore says, but
   the scan went through all of it, 500 files on the iPod, for a
   database.unignore that isn't there */
static bool rockbox_dir(const char *path)
{
#ifdef HAVE_MULTIVOLUME
    path_strip_volume(path, &path, false);
#endif
    return !strcasecmp(path, ROCKBOX_DIR);
}

/* Finder's, with iTunesDB's songs */
static bool finder_dir(const char *path)
{
#ifdef HAVE_MULTIVOLUME
    path_strip_volume(path, &path, false);
#endif
    return !strcasecmp(path, "/iPod_Control");
}

/* the check of the songs outside it passes over it */
static bool outside_skips(const char *path)
{
    return scan_outside && finder_dir(path);
}

#ifndef __PCTOOL__
/* An image of a folder outside Finder's, as a cover may be: in a sum of
   their names, sizes and times, in any order, which tells that a cover was
   saved again, added or taken away. Finder's photos aren't covers. */
static void add_outside_image(const char *path, const struct dirinfo *info)
{
    const char *dot = strrchr(path, '.');
    if (!dot || (strcasecmp(dot, ".jpg") && strcasecmp(dot, ".jpeg")
                 && strcasecmp(dot, ".png") && strcasecmp(dot, ".bmp")))
        return;
    const char *p = path;
#ifdef HAVE_MULTIVOLUME
    path_strip_volume(path, &p, false);
#endif
    if (!strncasecmp(p, "/Photos/", 8))
        return;
    uint32_t h = 2166136261u;
    for (const unsigned char *c = (const unsigned char *)p; *c; c++)
        h = (h ^ *c) * 16777619u;
    h ^= (uint32_t)info->size * 2654435761u;
    h ^= (uint32_t)info->mtime * 40503u;
    outside_images += h;
}

bool tagcache_outside_images(uint32_t *sum)
{
    *sum = outside_images;
    return outside_images_valid;
}
#endif

static bool check_dir(const char *dirname, int add_files)
{
    int success = false;

    DIR *dir = opendir(dirname);
    if (!dir)
    {
        logf("tagcache: opendir(%s) failed", dirname);
        return false;
    }

    /* check for a database.ignore and database.unignore; Finder's folders
       have none, and after a sync over USB, without the dircache yet,
       looking cost 11 ms a folder */
    int ignore = 0, unignore = 0;
#ifndef __PCTOOL__
    if (!itunesdb_synced_path(dirname))
#endif
        check_ignore(dirname, &ignore, &unignore);

    /* don't do anything if both ignore and unignore are there */
    if (ignore != unignore)
        add_files = unignore;

    /* Recursively scan the dir. */
    while (!scan_failed && !check_event_queue())
    {
        struct dirent *entry = readdir(dir);
        if (entry == NULL)
        {
            success = true;
            break;
        }

        if (is_dotdir_name(entry->d_name) || hidden_name(entry->d_name))
            continue;

        struct dirinfo info = dir_get_info(dir, entry);
        size_t len = strlen(curpath);
        path_append(&curpath[len-1], PA_SEP_HARD, entry->d_name,
                    sizeof (curpath) - len);

        processed_dir_count++;
        if (info.attribute & ATTR_DIRECTORY)
        {
#ifndef SIMULATOR
            /* don't follow symlinks to dirs, but try to add it as a search root
             * this makes able to avoid looping in recursive symlinks */
            if (info.attribute & ATTR_LINK)
                add_search_root(curpath);
            else
#endif /* SIMULATOR */
            if (!rockbox_dir(curpath) && !outside_skips(curpath)
                && !ipod_control_private(curpath))
                check_dir(curpath, add_files);
        }
        else if (!add_files)
        {
#ifndef __PCTOOL__
            if (scan_outside)
                add_outside_image(curpath, &info);
#endif
        }
        /* nor a file of iPod_Control's own, beside its folders: it would be
           added at each import and removed by the check after it */
        else if (!ipod_control_private(curpath))
        {
#ifndef __PCTOOL__
            if (scan_outside)
                add_outside_image(curpath, &info);
#endif
            tc_stat.curentry = curpath;

            /* Add a new entry to the temporary db file. */
            add_tagcache(curpath, info.mtime);
            /* the UI between them, as a tick goes by: the tags of hundreds
               read one after another, the bar under Checking Songs was drawn
               again only at the end */
            do_timed_yield();

            /* Wait until current path for debug screen is read and unset. */
            while (tc_stat.syncscreen && tc_stat.curentry != NULL)
                yield();

            tc_stat.curentry = NULL;
        }

        str_setlen(curpath, len);
    }

    closedir(dir);

    return success;
}

#ifndef __PCTOOL__
/* The songs in the folder at countpath and those in it, outside Finder's and
   as database.ignore leaves them, without their tags: in the dircache, in
   memory */
static char countpath[TAGCACHE_BUFSZ];
static int count_dir(bool add_files)
{
    int count = 0;
    DIR *dir = opendir(countpath);
    if (!dir)
        return 0;
    int ignore, unignore;
    check_ignore(countpath, &ignore, &unignore);
    if (ignore != unignore)
        add_files = unignore;
    struct dirent *entry;
    while (!check_event_queue() && (entry = readdir(dir)) != NULL)
    {
        if (is_dotdir_name(entry->d_name) || hidden_name(entry->d_name))
            continue;
        struct dirinfo info = dir_get_info(dir, entry);
        size_t len = strlen(countpath);
        path_append(&countpath[len-1], PA_SEP_HARD, entry->d_name,
                    sizeof (countpath) - len);
        if (info.attribute & ATTR_DIRECTORY)
        {
#ifndef SIMULATOR
            if (!(info.attribute & ATTR_LINK))
#endif
            if (!rockbox_dir(countpath) && !finder_dir(countpath))
                count += count_dir(add_files);
        }
        else if (add_files && probe_file_format(countpath) != AFMT_UNKNOWN)
            count++;
        str_setlen(countpath, len);
        do_timed_yield();
    }
    closedir(dir);
    return count;
}

/* The songs outside Finder's folder: a sync's bar goes by them and iTunesDB's;
   the check's second half by those new, counted at the first new one, whose
   tags are read. A check with nothing new walks the folders once. */
static void count_outside(void)
{
    outside_counted = true;
    int songs = 0;
    for (struct search_roots_ll *r = scan_roots; r; r = r->next)
    {
        strmemccpy(countpath, r->path, sizeof(countpath));
        if (dir_exists(countpath))
            songs += count_dir(true);
    }
    if (!scan_outside)
    {
        import_total += songs;
        return;
    }
    /* none new, the tags of some changed: by all those known, rather than
       full at the first */
    const int fresh = songs > outside_known ? songs - outside_known
                                            : MAX(outside_known, 1);
    import_total = 2 * fresh;
    import_done = fresh;
}
#endif /* !__PCTOOL__ */

void tagcache_screensync_event(void)
{
    tc_stat.curentry = NULL;
}

void tagcache_screensync_enable(bool state)
{
    tc_stat.syncscreen = state;
}

/* a scan stopped before it got through the files, as by USB: what it found
   waits in the temp file, and an import after it would take the files it
   didn't reach for ones Finder knows nothing of */
static bool scan_cut_short;

#if !defined(__PCTOOL__) && !defined(PLUGIN)
/* An import of songs: half a KB a song for the scan's file, the database's
   files or a quarter KB a song for the commit's, and the reserve */
static unsigned long import_need_kb(unsigned long songs)
{
    return songs / 2 + MAX(db_files_kb(), songs / 4) + LIBRARY_RESERVE_KB;
}

static bool scan_room(void)
{
    return room_for(import_need_kb(MAX(import_total, 0)));
}

/* the next import's room, and that of the caches made after the covers, as
   Search's names (a quarter KB a song and 1 MB): else those took the import's */
unsigned long tagcache_caches_floor_kb(void)
{
    const unsigned long songs = db_file_size(TAGCACHE_FILE_MASTER)
                                / sizeof(struct index_entry);
    return import_need_kb(songs) + songs / 4 + 1024;
}
#endif

#ifndef __PCTOOL__
/* this is called by the database tool to not pull in global_settings */
static
#endif
void do_tagcache_build(const char *path[])
{
    struct tagcache_header header;
    bool ret;

    scan_cut_short = false;
    scan_failed = false;
    str_setlen(curpath, 0);
    data_size = 0;
    total_entry_count = 0;
    processed_dir_count = 0;

#ifdef HAVE_DIRCACHE
    dircache_wait();
#endif

    logf("updating tagcache");

    cachefd = open_db_fd(TAGCACHE_FILE_TEMP, O_RDONLY);
    if (cachefd >= 0)
    {
        logf("skipping, cache already waiting for commit");
        close(cachefd);
        cachefd = -1;
        /* scan skipped: keep the import, and the forget, from running */
        scan_cut_short = restoring();
        return ;
    }

    if (!restore_load())
    {
        scan_cut_short = true;
        return ;
    }
    if (!scan_outside && !open_scan_temp())
    {
        scan_cut_short = true;
        restore_free();
        return ;
    }

    filenametag_fd = open_tag_fd(&header, tag_filename, false);
#ifndef __PCTOOL__
#if defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
    /* as after a sync over USB: a table of the filename file took a second
       to make for 6800 songs, for nothing */
    build_names_in_ram = tc_stat.ramcache && ram_names_handle > 0
                         && global_settings.tagcache_ram == TAGCACHE_RAM_ON;
#endif
    if (filenametag_fd >= 0 && !build_names_in_ram)
    {
        struct master_header mh;
        load_build_names(filenametag_fd, header.entry_count);
        build_masterfd = open_master_fd(&mh, false);
    }
#endif

    cpu_boost(true);

    logf("Scanning files...");
    /* Scan for new files. */
    ret = true;

    if (path[0] == NULL)
    {
        logf("tagcache path empty, skipping...");
        goto commit;
    }

    roots_ll[0].path = path[0];
    roots_ll[0].next = NULL;

#if defined(HAVE_MULTIVOLUME) && !defined(SIMULATOR) && !defined(__PCTOOL__) && !defined(APPLICATION)
    extern bool ns_volume_is_visible(int volume); /*rb_namespace.c*/
    /* i is for the path vector, j for the roots_ll array */
    int i = 1, j = 1;
    bool added = false;
    char volnamebuf[NUM_VOLUMES][VOL_MAX_LEN + 1];
    /* we can just parse the root directory ('/') and get to any mounted
    * volume but we can also enumerate a volume in the root directory
    * when this occurs it leads to multiple entries since the files can
    * be reached through multiple paths ex, /Foo could also be /SD1/Foo
    * we used to hide the volume that was mapped but then when you switch
    * from the sd to the internal the paths don't map to the right volume
    * instead we will attempt to rewrite the root with any non-hidden volumes
    * failing that just leave the paths alone */
    if (!strcmp(PATH_ROOTSTR, path[0]))
    {
        i = 0;
        j = 0;
    }
    /* path can be skipped , but root_ll entries can't */
    for(; path[i] && j < MAX_STATIC_ROOTS; i++)
    {
        /* check if the link target is inside of an existing search root
         * don't add if target is inside, we'll scan it later */
        if (!added && !strcmp(PATH_ROOTSTR, path[i]))
        {
            for (int v = 0; v < NUM_VOLUMES; v++)
            {
                if (ns_volume_is_visible(v))
                {
                    make_volume_root(v, volnamebuf[v]);
                    roots_ll[j].path = volnamebuf[v];
                    if (j > 0)
                        roots_ll[j-1].next = &roots_ll[j];
                    j++;
                    added = true;
                }
            }
            if(!added)
                j = 1;
            added = true;
            continue;
        }
#else
    /* i is for the path vector, j for the roots_ll array
     * path can be skipped , but root_ll entries can't */
    for(int i = 1, j = 1; path[i] && j < MAX_STATIC_ROOTS; i++)
    {
#endif /*def HAVE_MULTIVOLUME*/
        if (search_root_exists(path[i])) /* skip this path */
            continue;

        roots_ll[j].path = path[i];
        roots_ll[j-1].next = &roots_ll[j];
        j++;
    }

    struct search_roots_ll * this;
#ifndef __PCTOOL__
    /* for count_outside() */
    scan_roots = &roots_ll[0];
    outside_counted = false;
    /* before the walk, which can take Finder's folder first */
    if (import_phase == TAGCACHE_IMPORT_SCANNING)
    {
        count_outside();
#ifndef PLUGIN
        if (!scan_room())
        {
            scan_failed = true;
            ret = false;
        }
#endif
    }
#endif
    /* check_dir might add new roots */
    for(this = &roots_ll[0]; this; this = this->next)
    {
        logf("Search root %s", this->path);
        strmemccpy(curpath, this->path, sizeof(curpath));

        if (ret)
        {
            if (dir_exists(this->path))
                ret = check_dir(this->path, true);
            else
                logf("Dir not found %s", this->path);
        }
    }
    free_search_roots(&roots_ll[0]);
#ifndef __PCTOOL__
    scan_roots = NULL;
    /* the images' sum of the whole walk only */
    if (scan_outside && ret)
        outside_images_valid = true;
    /* the commit can have the memory */
    itunesdb_forget_metadata();
#endif

commit:
    restore_free();
#ifdef TAGCACHE_SORT_FIELDS
    sort_scan_close();
#endif
    if (cachefd >= 0)
    {
        /* Write the header. */
        if (!tagfile_wbuf_flush(cachefd))
            scan_write_failed();
        header.magic = TAGCACHE_MAGIC;
        header.datasize = data_size;
        header.entry_count = total_entry_count;
        lseek(cachefd, 0, SEEK_SET);
        if (write(cachefd, &header, sizeof(struct tagcache_header))
            != sizeof(struct tagcache_header))
            scan_write_failed();
        close(cachefd);
        cachefd = -1;
        /* The check's, cut short as by USB or switching off, isn't left: the
           next boot asked whether to commit it. The next check finds them
           again. */
        if (!ret && scan_outside)
            remove_scan_files();
    }
    if (scan_failed)
    {
        remove_scan_files();
        ret = false;
    }

    if (filenametag_fd >= 0)
    {
        close(filenametag_fd);
        filenametag_fd = -1;
    }
#ifndef __PCTOOL__
    free_build_names();
#endif

    if (!ret)
    {
        logf("Aborted.");
        scan_cut_short = true;
        cpu_boost(false);
        return ;
    }

    /* Commit changes to the database. */
#ifdef __PCTOOL__
    allocate_tempbuf();
#endif
    /* the check outside Finder's folder shows a screen only for a change */
    if (import_phase == TAGCACHE_IMPORT_SCANNING
        || (import_phase == TAGCACHE_IMPORT_CHECKING && total_entry_count > 0))
        import_phase = TAGCACHE_IMPORT_SAVING;
    if (scan_outside && total_entry_count == 0)
        ; /* none new, and no file made for them */
    else if (commit())
    {
        logf("tagcache built!");
    }
#ifdef __PCTOOL__
    free_tempbuf();
#endif

#ifdef HAVE_TC_RAMCACHE
    if (tcramcache.hdr)
    {
        /* Import runtime statistics if we just initialized the db. */
        if (current_tcmh.serial == 0)
            queue_post(&tagcache_queue, Q_IMPORT_CHANGELOG, 0);
    }
#endif

    cpu_boost(false);
}

#ifndef __PCTOOL__
void tagcache_build(void)
{
    char *vect[MAX_STATIC_ROOTS + 1]; /* +1 to ensure NULL sentinel */
    char str[sizeof(global_settings.tagcache_scan_paths)];
    strmemccpy(str, global_settings.tagcache_scan_paths, sizeof(str));

    int res = split_string(str, ':', vect, MAX_STATIC_ROOTS);
    vect[res] = NULL;

    do_tagcache_build((const char**)vect);
}
#endif /* __PCTOOL__ */

#ifdef HAVE_TC_RAMCACHE
static void load_ramcache(void)
{
    if (!tcramcache.hdr)
        return ;

    cpu_boost(true);
    /* not searched while it loads again, as after an update that found
       nothing new, which left it as it was: its names were gone meanwhile */
    tc_stat.ramcache = false;

#ifndef __PCTOOL__
    /* taken before the load, which keeps the cache from moving */
    ram_names_alloc(current_tcmh.tch.entry_count, ram_paths_needed());
#endif

    /* At first we should load the cache (if exists). It's searched once its
       paths are in order: their sort lets other threads go, and a track
       looked up meanwhile was found in none of them, its play not counted */
    load_cut_short = false;
    const bool loaded = load_tagcache();

#ifndef __PCTOOL__
    if (loaded && ram_names_handle > 0 && !ram_names_overflow)
    {
        struct path_name *names = core_get_data_pinned(ram_names_handle);
        qsort_yielding(names, ram_names_count, sizeof(*names), cmp_path_name);
        core_put_data_pinned(names);
    }
    else
        free_ram_names();
#endif
    tc_stat.ramcache = loaded;

    /* Stopped by USB, as on connecting the iPod just after it started, it is
       loaded again once idle: taken for a failure, the database was off until
       the next boot, and the songs copied over USB went unchecked */
    if (!tc_stat.ramcache && !load_cut_short)
    {
        /* If loading failed, it must indicate some problem with the db
         * so disable it entirely to prevent further issues. */
        tc_stat.ready = false;
        tcramcache.hdr = NULL;
        int handle = tcramcache.handle;
        tcramcache.handle = 0;
        /* or the next commit borrows the freed buffer and panics */
        tc_stat.ramcache_allocated = 0;
        core_free(handle);
    }

    cpu_boost(false);
}

/* The cache is allocated at boot for the database as it was. When a commit
   has made the database larger than that, loading it fails and switches
   the database off until the next boot, so allocate it anew first. */
static void reallocate_ramcache(void)
{
    if (!global_settings.tagcache_ram)
        return;
    tc_stat.ramcache = false;
    if (tcramcache.handle > 0)
    {
        int handle = tcramcache.handle;
        tcramcache.hdr = NULL;
        tcramcache.handle = 0;
        tc_stat.ramcache_allocated = 0;
        core_free(handle);
    }
    allocate_tagcache();
}

void tagcache_unload_ramcache(void)
{
    tc_stat.ramcache = false;
#ifndef __PCTOOL__
    free_ram_names();
#endif
    /* Just to make sure there is no statefile present. */
    // remove_db_file(TAGCACHE_STATEFILE);
}
#endif /* HAVE_TC_RAMCACHE */

#ifndef __PCTOOL__

/*
 * db_file_exists is noinline to minimize stack usage
 */
static bool NO_INLINE db_file_exists(const char* filename)
{
    char buf[MAX_PATH];

    snprintf(buf, sizeof(buf), "%s/%s", tc_stat.db_path, filename);

    return file_exists(buf);
}

/* Updates the database from the files, also when the commit had to wait
   for a buffer, and loads it into the cache again at its new size. False
   when the scan was cut short: its part waits for the next try. */
/* the database in memory again after a commit, which unloads it */
static void reload_after_commit(void)
{
#ifdef HAVE_TC_RAMCACHE
    if (!tc_stat.ramcache)
    {
        reallocate_ramcache();
        load_ramcache();
    }
#endif
}

/* Whether the tag files are sorted otherwise than by these rules and the Sort
   fields Finder's songs have now: by an older firmware, by the commit's
   plugin, or before a sync changed the Sort fields. At boot, from the order's
   mark and two headers. */
static bool order_stale(void)
{
    struct master_header mh;
    int fd = open_master_fd(&mh, false);
    if (fd < 0)
        return false; /* none: the first commit sorts it */
    close(fd);
    struct order_mark mark;
    fd = open_db_fd(TAGCACHE_FILE_ORDER, O_RDONLY);
    if (fd < 0)
        return true;
    /* with Quick Scroll's letters after it */
    const bool read_whole = read(fd, &mark, sizeof(mark)) == sizeof(mark)
        && ffilesize(fd) == (off_t)(sizeof(mark) + sizeof(initials));
    close(fd);
    return !read_whole || mark.magic != ORDER_MAGIC
           || mark.rules != SORTKEY_RULES_VERSION
           || mark.commitid != mh.commitid
           || mark.finder != finder_sort_crc();
}

/* The database sorted again if it's stale, by a commit of no new entries but
   of every tag file, under the import's screen: once at the first boot after
   an update, and after a sync that changed Sort fields. Its new commit id has
   the caches made from it made again, as after a sync. True once sorted. */
static bool resort_if_stale(void)
{
    if (!order_stale())
        return false;
    /* an empty file of new entries, unless one waits with some */
    const bool made = !db_file_exists(TAGCACHE_FILE_TEMP);
    if (made)
    {
        const struct tagcache_header none = { TAGCACHE_MAGIC, 0, 0 };
        int fd = open_db_fd(TAGCACHE_FILE_TEMP, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0)
            return false;
        const bool ok = write(fd, &none, sizeof(none)) == sizeof(none);
        if (close(fd) < 0 || !ok)
        {
            remove_db_file(TAGCACHE_FILE_TEMP);
            return false;
        }
    }
    logf("sorting the database again");
    const int phase = import_phase;
    import_phase = TAGCACHE_IMPORT_SAVING;
    commit_resorting = true;
    allocate_tempbuf();
    commit();
    free_tempbuf();
    commit_resorting = false;
    /* A commit that didn't go through, as for want of memory, leaves the
       file: waiting, it would have every scan skipped, and the boot ask
       whether to commit it. The mark stays stale, for the next try. */
    if (made)
        remove_db_file(TAGCACHE_FILE_TEMP);
    import_phase = phase;
    return !order_stale();
}

/* the entries of the scan's file waiting for a commit, -1 for none: 0 for
   an empty one, or one cut short before its header was written */
static long temp_entries(void)
{
    struct tagcache_header h;
    int fd = open_db_fd(TAGCACHE_FILE_TEMP, O_RDONLY);
    if (fd < 0)
        return -1;
    const bool whole = read(fd, &h, sizeof(h)) == sizeof(h)
                       && h.magic == TAGCACHE_MAGIC;
    close(fd);
    return whole ? h.entry_count : 0;
}

/* the new files of a scan before, as of one whose commit found no buffer */
static void commit_waiting(void)
{
    if (db_file_exists(TAGCACHE_FILE_TEMP))
    {
        allocate_tempbuf();
        commit();
        free_tempbuf();
    }
}

static bool update_and_reload(void)
{
    /* one waits from before: it goes in first, or the scan would be skipped
       for it and miss the new files */
    commit_waiting();
    tagcache_build();
    itunesdb_forget_metadata(); /* if the scan was skipped */
    bool scanned = !scan_cut_short;
    if (import_phase != TAGCACHE_IMPORT_NONE
        && import_phase != TAGCACHE_IMPORT_CHECKING)
        import_phase = TAGCACHE_IMPORT_LOADING;
    if (scanned && db_file_exists(TAGCACHE_FILE_TEMP))
    {
        /* The commit found no buffer to borrow and waits for the next
           boot; take one just for it, as at boot */
        allocate_tempbuf();
        commit();
        free_tempbuf();
    }
    /* a sync can change the Sort fields alone, which no commit has taken */
    if (scanned)
        resort_if_stale();
#ifdef HAVE_TC_RAMCACHE
    reload_after_commit();
    /* without enough memory for it, the database still works from disk */
    if (!tc_stat.ready)
        tagcache_commit_finalize();
#endif
    /* a commit left for room has the import tried again, as a scan cut short */
    return scanned && !no_space;
}

/* How many different values a tag has, as artists or albums */
int tagcache_get_tag_count(int tag)
{
    struct tagcache_header header;
    const int fd = open_tag_fd(&header, tag, false);
    if (fd < 0)
        return -1;
    close(fd);
    return header.entry_count;
}

int tagcache_get_song_count(void)
{
    return song_count;
}

bool tagcache_take_no_space(void)
{
    const bool was = no_space;
    no_space = false;
    return was;
}

int tagcache_get_import(int *done, int *total)
{
    int phase = import_phase;
    *done = import_done;
    *total = import_total;
    if (phase == TAGCACHE_IMPORT_SAVING)
    {
        *done = tc_stat.commit_step;
        *total = tagcache_get_max_commit_step();
    }
    return phase;
}

/* What a boot does after a sync, for one over Rockbox's own USB mode, which
   doesn't reboot: true when there is an import, which tagcache_get_import()
   then follows. Without a sync, the songs outside Finder's folder are looked
   for, as copied there by hand: FLAC, which Finder doesn't take. Called once
   the computer has let go of the disk. */
bool tagcache_check_sync(void)
{
    if (!tc_stat.initialized || import_phase != TAGCACHE_IMPORT_NONE)
        return false;
    /* a build restoring statistics goes on, also without iTunesDB */
    if (itunesdb_changed()
        || (restore_pending && !(audio_status() & AUDIO_STATUS_PLAY)))
    {
        no_space = false;
        import_phase = TAGCACHE_IMPORT_READING;
        queue_post(&tagcache_queue, Q_SYNCED, 0);
        return true;
    }
    if (!tc_stat.ready
#ifdef HAVE_TC_RAMCACHE
        /* from the disk, every name was read and every file looked up, for
           seconds at every connection; in memory, it's loaded first if a
           connection stopped its load */
        || !global_settings.tagcache_ram
#endif
        /* its own pass in the background finds them: the screen waited for
           a scan of the whole disk */
        || global_settings.tagcache_autoupdate
        /* after a plugin, music started meanwhile: a commit would take its
           buffer and stop it. The songs come with the next connection. */
        || (audio_status() & AUDIO_STATUS_PLAY))
        return false;
    import_done = import_total = 0;
    outside_known = 0; /* until the pass over those gone says */
    no_space = false;
    import_phase = TAGCACHE_IMPORT_CHECKING;
    queue_post(&tagcache_queue, Q_CHECK_OUTSIDE, 0);
    return true;
}


static void count_songs(void)
{
    int count = 0;
#ifdef HAVE_TC_RAMCACHE
    if (tc_stat.ramcache)
    {
        tcrc_buffer_lock();
        for (int i = 0; i < current_tcmh.tch.entry_count; i++)
            if (!(tcramcache.hdr->indices[i].flag & FLAG_DELETED))
                count++;
        tcrc_buffer_unlock();
        song_count = count;
        return;
    }
#endif
    /* from the disk until the database is in memory, through a buffer
       free between scans and commits */
    struct master_header hdr;
    struct index_entry *entries = (struct index_entry *)tagr_buf;
    const int per = sizeof(tagr_buf) / sizeof(struct index_entry);
    int fd = open_master_fd(&hdr, false);
    if (fd < 0)
        return;
    int left = hdr.tch.entry_count;
    while (left > 0)
    {
        int n = MIN(left, per);
        if (read_index_entries(fd, entries, n) != (ssize_t)(n * sizeof(struct index_entry)))
            break;
        for (int i = 0; i < n; i++)
            if (!(entries[i].flag & FLAG_DELETED))
                count++;
        left -= n;
        /* the UI goes between them: all of 13000 songs kept it waiting
           200 ms, as a read below its priority waits on the disk */
        yield();
    }
    close(fd);
    if (left == 0)
        song_count = count;
}

#ifdef HAVE_TC_RAMCACHE
/* The files' references from the dircache, at boot, or with an update their
   check. The references are wanted only for files whose paths didn't fit in
   memory, which finds them otherwise: 13000 took 84 s on the iPod, in the
   background at every boot. */
static void check_file_refs(bool auto_update)
{
    if (!auto_update && ram_paths_all())
        return;
    check_deleted_file_refs(auto_update);
}
#endif

/* the names file left in its place by a cut after the master's rename */
static bool names_left(const char *names)
{
    struct tagcache_header h;
    if (db_file_exists(TAGCACHE_FILE_OLD_NAMES))
        return false;
    const int fd = open_db_fd(names, O_RDONLY);
    if (fd < 0)
        return false;
    const bool left = read(fd, &h, sizeof(h)) == sizeof(h)
                      && h.magic != TAGCACHE_MAGIC;
    close(fd);
    return left;
}

/* Boot: set aside a database of another format, its mark, master and names
   in that order. A cut before the master's rename starts again; one after it
   finishes here. */
static bool restore_set_aside(void)
{
    char names[32];
    struct master_header mh;
    snprintf(names, sizeof(names), TAGCACHE_FILE_INDEX, tag_filename);
    int fd = open_db_fd(TAGCACHE_FILE_MASTER, O_RDONLY);
    const bool found = fd >= 0 && old_master(fd, &mh);
    if (fd >= 0)
        close(fd);
    if (found)
    {
        /* while the master is here, none of them is this one's */
        remove_db_file(TAGCACHE_FILE_OLD_MASTER);
        remove_db_file(TAGCACHE_FILE_OLD_NAMES);
        struct restore_mark mark = {
            RESTORE_MAGIC, 0, 0, mh.serial, mh.commitid
        };
        itunesdb_imported_stamp(&mark.db_size, &mark.db_mtime);
        fd = open_db_fd(TAGCACHE_FILE_OLD_MARK, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0)
            return false;
        const bool written = write(fd, &mark, sizeof(mark)) == sizeof(mark);
        if (close(fd) < 0 || !written
            || rename_db_file(TAGCACHE_FILE_MASTER, TAGCACHE_FILE_OLD_MASTER) < 0)
            return false;
        restore_pending = true;
        restore_from = mark;
    }
    else if (!restore_pending || !names_left(names))
        return false;
    itunesdb_state_aside();
    /* without them nothing is restored, but the build goes on */
    rename_db_file(names, TAGCACHE_FILE_OLD_NAMES);
    /* an old-format journal can't be read: drop it */
    commit_undo(NULL);
    remove_scan_files();
    remove_files();
    return true;
}

static void restore_forget(void)
{
    if (!restore_pending)
        return;
    remove_db_file(TAGCACHE_FILE_OLD_MASTER);
    remove_db_file(TAGCACHE_FILE_OLD_NAMES);
    remove_db_file(TAGCACHE_FILE_OLD_MARK);
    remove(ITUNESDB_OLD_STATE);
    restore_pending = false;
}

static void tagcache_thread(void)
{
    struct queue_event ev;
    bool check_done = false;
    cpu_boost(true);
    commit_recover();
    /* If the previous cache build/update was interrupted, commit
     * the changes first in foreground. After a sync the import does,
     * behind its progress screen. One of no new entries, as a sort cut
     * short leaves, has nothing to ask about. */
    bool scan_waits = false; /* still, declined or its commit failed */
    const long waiting = sync_found ? -1 : temp_entries();
    if (waiting == 0)
        remove_scan_files();
    else if (waiting > 0)
    {
        bool now = true;
#if !(defined(__APPLE__) && (CONFIG_PLATFORM & PLATFORM_SDL))
        static const char *lines[] = {ID2P(LANG_TAGCACHE_BUSY),
                                      ID2P(LANG_TAGCACHE_UPDATE)};
        static const struct text_message message = {lines, 2};

        now = gui_syncyesno_run_w_tmo(HZ * 5, YESNO_YES, str(LANG_TAGCACHE),
                                      &message, NULL, NULL) == YESNO_YES;
#endif
        if (now)
        {
            allocate_tempbuf();
            commit();
            free_tempbuf();
        }
        scan_waits = db_file_exists(TAGCACHE_FILE_TEMP);
    }

#ifdef HAVE_TC_RAMCACHE
#ifdef HAVE_EEPROM_SETTINGS
    if (firmware_settings.initialized && firmware_settings.disk_clean
        && global_settings.tagcache_ram)
    {
        check_done = tagcache_dumpload();
    }

    remove_db_file(TAGCACHE_STATEFILE);
#endif /* HAVE_EEPROM_SETTINGS */

    /* Allocate space for the tagcache if found on disk. */
    if (global_settings.tagcache_ram && !tc_stat.ramcache)
        allocate_tagcache();
#endif /* HAVE_TC_RAMCACHE */

    cpu_boost(false);
    tc_stat.initialized = true;

    if (sync_found)
    {
        /* what playback needs first: it is quick, and the old covers point
           into ithmb files that Finder has rewritten */
        cpu_boost(true);
        import_total = itunesdb_import_tracks(true, &import_done,
                                              &import_total);
        cpu_boost(false);
        import_done = 0;
        import_phase = TAGCACHE_IMPORT_SCANNING;
        queue_post(&tagcache_queue, Q_IMPORT_ITUNES, 0);
    }
    else if (!itunesdb_load_tracks())
    {
        /* what an older firmware didn't note, once: tagcache_init() has put up
           the import's screen for it */
        import_phase = TAGCACHE_IMPORT_READING;
        itunesdb_import_tracks(false, &import_done, &import_total);
#ifndef ITUNESDB_HAVE_ART
        import_phase = TAGCACHE_IMPORT_NONE;
#endif
    }

    /* Sorted by older rules, or before Sort fields an import took in, as
       after an update: sorted again once, before the menu. After a sync, the
       import's commit does; and not with the scan's file still waiting,
       which the sort would commit too, behind the menu: there was no screen
       for it. */
#ifdef HAVE_TC_RAMCACHE
    bool refs_due = false; /* the check of the files after the load, below */
#endif
    if (!sync_found && !scan_waits && resort_if_stale())
    {
#ifdef HAVE_TC_RAMCACHE
        /* In memory before the screen goes, as after a sync: the boot's menu
           didn't wait for it, and what Updating Library makes then goes
           through all of it, as Cover Flow's albums. The check of the files
           is left to the pass behind the menu, as after the boot's own load:
           without every path in memory, it took more than a minute. */
        const int phase = import_phase;
        import_phase = TAGCACHE_IMPORT_LOADING;
        reload_after_commit();
        refs_due = tc_stat.ramcache;
        import_phase = phase;
#endif
    }
    /* the screen tagcache_init() put up for it */
    if (import_phase == TAGCACHE_IMPORT_SAVING)
        import_phase = TAGCACHE_IMPORT_NONE;
    /* Quick Scroll's letters, unless a commit just noted them */
    if (initials_commit < 0)
        initials_load();

    /* The header check, on the background but at once: the song count and
       an import after a sync wait for it */
    if (!tc_stat.ready)
        tagcache_commit_finalize();

#ifdef ITUNESDB_HAVE_ART
    /* Cover Flow's slides that an import without a sync left to make, or
       one cut short: before the menu, as after a sync */
    if (!sync_found)
    {
        if (itunesdb_slides_todo())
        {
            import_done = import_total = 0;
            import_phase = TAGCACHE_IMPORT_SLIDES;
            itunesdb_make_slides(&import_done, &import_total,
                                 check_event_queue);
        }
        /* also when the import found none to make after all */
        import_phase = TAGCACHE_IMPORT_NONE;
    }
#endif
#ifdef HAVE_TC_RAMCACHE
    thread_started = true;
#endif

    while (1)
    {
        run_command_queue(false);
        /* counted as soon as the database is there, from the disk before it
           is in memory: at boot that took loading it and checking every
           file against the dircache first, seconds of "Calculating" */
        if (song_count < 0 && tc_stat.ready && tc_stat.commit_step == 0)
            count_songs();

        queue_wait_w_tmo(&tagcache_queue, &ev, HZ);

        switch (ev.id)
        {
            case Q_IMPORT_CHANGELOG:
                tagcache_import_changelog();
                break;

            case Q_REBUILD:
                remove_files();
                remove_scan_files();
                tagcache_build();
                break;

            case Q_UPDATE:
                tagcache_build();
#ifdef HAVE_TC_RAMCACHE
                load_ramcache();
#endif
                check_deleted_file_refs(true);
                break ;

            case Q_SYNCED:
                /* as at boot after a sync */
                cpu_boost(true);
                import_total = itunesdb_import_tracks(true, &import_done,
                                              &import_total);
#ifdef HAVE_TC_RAMCACHE
                ram_kinds_refresh();
#endif
                cpu_boost(false);
                import_done = 0;
                import_phase = TAGCACHE_IMPORT_SCANNING;
                /* fallthrough */
            case Q_IMPORT_ITUNES:
                /* Finder has synced: the new files first, then what
                   iTunesDB says about them. Cut short, as by USB, all of
                   it is tried again: iTunesDB still differs from its stamp,
                   so after USB or at the next boot */
                cpu_boost(true);
                if (!update_and_reload())
                {
                    import_phase = TAGCACHE_IMPORT_NONE;
                    cpu_boost(false);
                    break;
                }
                check_skips_synced = true;
                check_deleted_file_refs(true);
                check_skips_synced = false;
                import_done = import_total = 0;
                import_phase = TAGCACHE_IMPORT_COUNTS;
                /* the import is tried again on the next boot otherwise */
                if (!db_file_exists(TAGCACHE_FILE_TEMP))
                {
                    itunesdb_import();
                    /* a place the pass took from the Mac, for the lists */
                    states_generation++;
                    /* itunesdb.dat now matches, or there's no iTunesDB:
                       the statistics are all back */
                    if (!itunesdb_changed())
                        restore_forget();
                    /* the live smart playlists, by the counts just in */
                    smart_update_after_import();
                }
                else
                    health_note("import not completed: scan file waiting %s",
                                tc_stat.commit_delayed ?
                                "for its commit, put off for memory or cancelled"
                                : "though its commit was not put off");
#ifdef ITUNESDB_HAVE_ART
                /* the slides of the new covers, ready for Cover Flow */
                import_done = import_total = 0;
                import_phase = TAGCACHE_IMPORT_COVERS;
                itunesdb_make_slides(&import_done, &import_total,
                                     check_event_queue);
#endif
                import_phase = TAGCACHE_IMPORT_NONE;
                cpu_boost(false);
                break ;

            case Q_CHECK_OUTSIDE:
                /* The songs outside Finder's folder: those gone first, so
                   that a file moved or renamed has its counts and rating back
                   when the scan adds it again, then the new ones */
                cpu_boost(true);
                check_skips_synced = scan_outside = true;
                outside_images = 0;
                outside_images_valid = false;
                /* new files one waits with, from before: their entries are
                   looked for too, in memory */
                commit_waiting();
                /* and after a load USB stopped */
                reload_after_commit();
#ifdef HAVE_TC_RAMCACHE
                /* not from the disk, for seconds, as without the memory */
                if (tc_stat.ramcache)
#endif
                {
                    check_deleted_file_refs(true);
                    update_and_reload();
                }
                check_skips_synced = scan_outside = false;
                import_phase = TAGCACHE_IMPORT_NONE;
                cpu_boost(false);
                break ;

            case Q_START_SCAN:
                check_done = false;
                /* fallthrough */
            case Q_LOAD_RAM: /* the idle second's pass at once */
            case SYS_TIMEOUT:
#ifdef HAVE_TC_RAMCACHE
                /* lists wait for it, so it loads even after the check: as
                   when a commit unloaded it after that */
                if (check_done && tagcache_ram_pending())
                {
                    load_ramcache();
                    if (global_settings.tagcache_ram == TAGCACHE_RAM_ON)
                        check_file_refs(false);
                    break;
                }
#endif
                if (check_done || !tc_stat.ready)
                    break ;

#ifdef HAVE_TC_RAMCACHE
                /* loaded now, or by the boot's sort, which left this to do
                   after it, in the same order */
                if ((!tc_stat.ramcache || refs_due)
                    && global_settings.tagcache_ram)
                {
                    if (!tc_stat.ramcache)
                        load_ramcache();
                    refs_due = false;
                    if (global_settings.tagcache_ram == TAGCACHE_RAM_ON)
                        check_file_refs(global_settings.tagcache_autoupdate);
                    if (tc_stat.ramcache && global_settings.tagcache_autoupdate)
                        tagcache_build();
                }
                else
#endif /* HAVE_RC_RAMCACHE */
                if (global_settings.tagcache_autoupdate)
                {
                    tagcache_build();

                    /* This will be very slow unless dircache is enabled
                       or target is flash based, but do it anyway for
                       consistency. */
                    check_deleted_file_refs(true);
                }

                logf("tagcache check done");

                check_done = true;
                break ;

            case Q_STOP_SCAN:
                break ;

            case SYS_POWEROFF:
            case SYS_REBOOT:
                break ;

            case SYS_USB_CONNECTED:
                logf("USB: TagCache");
                /* what was played and rated since the last sync, for Finder
                   to take in before the disk is handed over; the computer
                   waits for it, so at full speed. The song playing counts too:
                   playback stops as USB connects, and its play is noted once
                   its fade-out is over, which was after Play Counts was
                   written, so that it came a sync late or not at all */
                cpu_boost(true);
                /* the stop notes the play before the status drops; 3 s was
                   short of a disk spinning up for the track the audio thread
                   was opening. Still bounded, for a stop that waited on this
                   thread would hold USB up for good. */
                for (int wait = 0; wait < 10 * HZ
                     && (audio_status() & AUDIO_STATUS_PLAY); wait += HZ / 20)
                    sleep(HZ / 20);
                run_command_queue(true);
                /* the marks made on the iPod, for the import after the sync */
                itunesdb_flush_flags();
                itunesdb_write_playcounts();
                cpu_boost(false);
                usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
                usb_wait_for_disconnect(&tagcache_queue);
                break ;
        }
    }
}

bool tagcache_prepare_shutdown(void)
{
    if (tagcache_get_commit_step() > 0)
        return false;

    tagcache_stop_scan();
    while (read_lock || write_lock)
        sleep(1);

    return true;
}

void tagcache_shutdown(void)
{
    /* Flush the command queue. */
    run_command_queue(true);
    itunesdb_flush_flags();

#if defined(HAVE_EEPROM_SETTINGS) && defined(HAVE_TC_RAMCACHE)
    if (tc_stat.ramcache)
        tagcache_dumpsave();
#endif
}

void tagcache_remove_statefile(void)
{
    remove_db_file(TAGCACHE_STATEFILE);
}

static int get_progress(void)
{
    int total_count = -1;

#ifdef HAVE_DIRCACHE
    struct dircache_info dcinfo;
    dircache_get_info(&dcinfo);
    if (dcinfo.status != DIRCACHE_IDLE &&
        ((!tc_stat.ramcache) || current_tcmh.tch.entry_count == 0))
    {
        total_count = dcinfo.entry_count;
    }
    else
#endif /* HAVE_DIRCACHE */
#ifdef HAVE_TC_RAMCACHE
    {
        if (tcramcache.hdr && tc_stat.ramcache)
            total_count = current_tcmh.tch.entry_count;
    }
#endif /* HAVE_TC_RAMCACHE */

    if (total_count < 0)
        return -1;
    else if (total_count == 0)
        return 0;
    else if (processed_dir_count > total_count)
        return 100;
    else
        return processed_dir_count * 100 / total_count;
}

struct tagcache_stat* tagcache_get_stat(void)
{
    tc_stat.total_entries = current_tcmh.tch.entry_count;
    tc_stat.progress = get_progress();
    tc_stat.processed_entries = processed_dir_count;

    return &tc_stat;
}

void tagcache_start_scan(void)
{
    queue_post(&tagcache_queue, Q_START_SCAN, 0);
}

bool tagcache_update(void)
{
    if (!tc_stat.ready)
        return false;

    queue_post(&tagcache_queue, Q_UPDATE, 0);
    return false;
}

bool tagcache_rebuild(void)
{
    queue_post(&tagcache_queue, Q_REBUILD, 0);
    return false;
}

void tagcache_stop_scan(void)
{
    queue_post(&tagcache_queue, Q_STOP_SCAN, 0);
}

#endif /* !__PCTOOL__ */


void tagcache_init(void)
{
    memset(&tc_stat, 0, sizeof(struct tagcache_stat));
    memset(&current_tcmh, 0, sizeof(struct master_header));
    filenametag_fd = -1;
    write_lock = read_lock = 0;

#ifndef __PCTOOL__
    strmemccpy(tc_stat.db_path, global_settings.tagcache_db_path,
               sizeof(tc_stat.db_path));
    /* known before the thread starts, for the progress screen */
    sync_found = itunesdb_changed();
    restore_read_mark();
    if (restore_set_aside())
        sync_found = true;
    else if (restore_pending)
    {
        struct master_header master;
        const int fd = open_master_fd(&master, false);
        if (fd >= 0)
            close(fd);
        /* only with this format's master is itunesdb.dat the import's: the
           build goes on until then, also without iTunesDB */
        if (fd >= 0 && !sync_found && itunesdb_exists())
            restore_forget();
        else
        {
            /* the old firmware's, left by a cut in the set-aside */
            if (fd < 0)
                itunesdb_state_aside();
            sync_found = true;
        }
    }
    /* a database that can't be read, or none, is made again from iTunesDB
       as after a sync; but for one in a commit that was cut short, which the
       thread is yet to put right */
    if (!sync_found && itunesdb_exists()
        && !db_file_exists(TAGCACHE_FILE_JOURNAL))
    {
        struct master_header master;
        int fd = open_master_fd(&master, false);
        if (fd >= 0)
            close(fd);
        else
        {
            remove_files();
            sync_found = true;
        }
    }
    if (sync_found || !itunesdb_gains_ready() || !itunesdb_sort_ready()
        || !itunesdb_flags_ready() || !itunesdb_notes_ready())
        import_phase = TAGCACHE_IMPORT_READING;
#ifdef ITUNESDB_HAVE_ART
    else if (itunesdb_slides_todo())
        import_phase = TAGCACHE_IMPORT_SLIDES;
#endif
    /* the database to sort again, under the screen, but for a scan's file
       waiting, which the boot asks about first and whose commit sorts it */
    else if (temp_entries() <= 0 && order_stale())
        import_phase = TAGCACHE_IMPORT_SAVING;
    mutex_init(&command_queue_mutex);
    queue_init(&tagcache_queue, true);
    create_thread(tagcache_thread, tagcache_stack,
                  sizeof(tagcache_stack), 0, tagcache_thread_name
                  IF_PRIO(, PRIORITY_BACKGROUND)
                  IF_COP(, CPU));
#else
    /* use default DB path */
    strcpy(tc_stat.db_path, ROCKBOX_DIR);
    tc_stat.initialized = true;
    allocate_tempbuf();
    commit();
    free_tempbuf();
    tc_stat.ready = check_all_headers();
#endif
}

#ifdef __PCTOOL__
void tagcache_reverse_scan(void)
{
    logf("Checking for deleted files");
    check_deleted_file_refs(true);
}
#endif

bool tagcache_is_initialized(void)
{
    return tc_stat.initialized;
}
bool tagcache_is_fully_initialized(void)
{
    return tc_stat.readyvalid;
}
bool tagcache_is_usable(void)
{
    return tc_stat.initialized && tc_stat.ready;
}
#ifdef HAVE_TC_RAMCACHE
bool tagcache_is_in_ram(void)
{
    return tc_stat.ramcache;
}

/* to be in memory in a moment: room made for it at the start, and its loading
   left only for the database's thread to be idle a second, as after Updating
   Library. Not during an import, which holds it back for its whole length.
   Before that start, which finds whether there's one to load, as good as. */
bool tagcache_ram_pending(void)
{
    if (!thread_started) /* but for an import, which loads it itself */
        return global_settings.tagcache_ram
               && import_phase == TAGCACHE_IMPORT_NONE;
    return global_settings.tagcache_ram && tcramcache.hdr && !tc_stat.ramcache
           && tc_stat.ready && import_phase == TAGCACHE_IMPORT_NONE;
}

#ifndef __PCTOOL__
/* loaded now rather than once its thread has been idle a second, as the
   boot logo waits for it; once loaded, or its first look done, nothing */
void tagcache_ram_load_now(void)
{
    queue_post(&tagcache_queue, Q_LOAD_RAM, 0);
}
#endif
#endif
int tagcache_get_commit_step(void)
{
    return tc_stat.commit_step;
}
int tagcache_get_commitid(void)
{
    return current_tcmh.commitid;
}

#ifdef HAVE_TC_RAMCACHE
bool tagcache_for_each_track(void (*f)(int idx_id, const int32_t *tag_seek,
                                       void *data),
                             void *data)
{
    if (!tc_stat.ramcache)
        return false;
    tcrc_buffer_lock(); /* the entries are movable */
    for (int i = 0; i < current_tcmh.tch.entry_count; i++)
    {
        const struct index_entry *idx = &tcramcache.hdr->indices[i];
        if (!(idx->flag & FLAG_DELETED))
            f(i, idx->tag_seek, data);
    }
    tcrc_buffer_unlock();
    return true;
}

bool tagcache_tag_string(int tag, int32_t seek, char *buf, size_t size)
{
    /* copied without a yield, as retrieve() reads it: the memory moves
       only as some is taken */
    if (!tc_stat.ramcache || seek < 0 || tag < 0 || tag >= TAG_COUNT
        || TAGCACHE_IS_NUMERIC(tag) || tag == tag_filename)
        return false;
    const struct tagfile_entry *ep =
        (const struct tagfile_entry *)&tcramcache.hdr->tags[tag][seek];
    if (!strcmp(ep->tag_data, UNTAGGED))
        return false;
    strmemccpy(buf, ep->tag_data, size);
    return true;
}
#endif

int tagcache_get_max_commit_step(void)
{
    return (int)(SORTED_TAGS_COUNT)+1;
}
#endif /*!defined(PLUGIN)*/
