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

#ifndef __PCTOOL__
#include "lang.h"
#include "eeprom_settings.h"
#include "itunesdb.h"
#endif
#define USR_CANCEL false
#else/*!defined(PLUGIN)*/
#define USR_CANCEL (tc_stat.commit_delayed == true)
#endif /*!defined(PLUGIN)*/
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

/* Tag Cache Header version 'TCHxx'. Increment when changing internal structures. */
#define TAGCACHE_MAGIC  0x54434811

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

/* Flags */
#define FLAG_DELETED     0x0001  /* Entry has been removed from db */
#define FLAG_DIRCACHE    0x0002  /* Filename is a dircache pointer */
#define FLAG_DIRTYNUM    0x0004  /* Numeric data has been modified */
#define FLAG_TRKNUMGEN   0x0008  /* Track number has been generated  */
#define FLAG_RESURRECTED 0x0010  /* Statistics data has been resurrected */

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
#define TAGCACHE_IS_UNIQUE(tag) (BIT_N(tag) & TAGCACHE_UNIQUE_TAGS)
#define TAGCACHE_IS_SORTED(tag) (BIT_N(tag) & TAGCACHE_SORTED_TAGS)
#define TAGCACHE_IS_NUMERIC_OR_NONUNIQUE(tag) \
    (BIT_N(tag) & (TAGCACHE_NUMERIC_TAGS | ~TAGCACHE_UNIQUE_TAGS))
/* Tags we want to get sorted (loaded to the tempbuf). */
#define TAGCACHE_SORTED_TAGS ((1LU << tag_artist) | (1LU << tag_album) | \
    (1LU << tag_genre) | (1LU << tag_composer) | (1LU << tag_comment) | \
    (1LU << tag_albumartist) | (1LU << tag_grouping) | (1LU << tag_title) | \
    (1LU << tag_virt_canonicalartist))

/* Uniqued tags (we can use these tags with filters and conditional clauses). */
#define TAGCACHE_UNIQUE_TAGS ((1LU << tag_artist) | (1LU << tag_album) | \
    (1LU << tag_genre) | (1LU << tag_composer) | (1LU << tag_comment) | \
    (1LU << tag_albumartist) | (1LU << tag_grouping) | \
    (1LU << tag_virt_canonicalartist))

/* String presentation of the tags defined in tagcache.h. Must be in correct order! */
static const char * const tags_str[] = { "artist", "album", "genre", "title",
    "filename", "composer", "comment", "albumartist", "grouping", "year",
    "discnumber", "tracknumber", "canonicalartist", "bitrate", "length",
    "playcount", "rating", "playtime", "lastplayed", "commitid", "mtime",
    "lastelapsed", "lastoffset"
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
};

static struct master_header current_tcmh;

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

struct tempbuf_id_list {
    long id;
    struct tempbuf_id_list *next;
};

struct tempbuf_searchidx {
    long idx_id;
    char *str;
    int seek;
    struct tempbuf_id_list idlist;
};

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
    /* Sorting can lock up for quite a while, so yield occasionally */
    static long wakeup_tick = 0;
    if (TIME_AFTER(current_tick, wakeup_tick))
    {
        yield();
        wakeup_tick = current_tick + (HZ/25);
        return true;
    }
    return false;
}
#endif /* __PCTOOL__ */

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

static void free_ram_names(void)
{
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
    unsigned flags = itunesdb_hash_flags(hash);
    kinds[idx_id] = (flags & ITUNESDB_AUDIOBOOK) ? 2 :
                    (flags & ITUNESDB_PODCAST) ? 1 : 0;
}

/* the index of the entry in memory idx points at, or -1 */
static long ram_entry_id(const struct index_entry *idx)
{
    if (!tc_stat.ramcache || !tcramcache.hdr || idx < tcramcache.hdr->indices
        || idx >= tcramcache.hdr->indices + current_tcmh.tch.entry_count)
        return -1;
    return idx - tcramcache.hdr->indices;
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
    return kinds[idx_id];
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

static inline void write_item(const char *item)
{
    int len = strlen(item) + 1;

    data_size += len;
    tagfile_wbuf_put(cachefd, item, len);
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
    qsort(names, count, sizeof(*names), cmp_path_idx);
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
    qsort(names, count, sizeof(*names), cmp_path_name);
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
    bool has_grouping;

    DB_LOG("file", path);

    if (cachefd < 0)
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

    /*memset(&id3, 0, sizeof(struct mp3entry)); -- get_metadata does this for us */
    memset(&entry, 0, sizeof(struct temp_file_entry));
    memset(&tracknumfix, 0, sizeof(tracknumfix));
#ifndef __PCTOOL__
    /* what iTunesDB says of a file Finder added, as a stock iPod has it */
    struct tagcache_runtime itdb_rt = { 0, 0, 0 };
    ret = itunesdb_get_metadata(path, &id3, &itdb_rt);
    if (!ret)
#endif
    ret = get_metadata_afmt(&id3, -1, path, afmt, METADATA_EXCLUDE_ID3_PATH);

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
    entry.tag_offset[tag_year] = id3.year;
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

    /* String tags. */
    has_artist = id3.artist != NULL
        && strlen(id3.artist) > 0;
    has_grouping = id3.grouping != NULL
        && strlen(id3.grouping) > 0;
    /* an album without an album artist is its track's artist's, as on a stock
       iPod: Cover Flow tells albums apart by name and album artist */
    if (has_artist && (id3.albumartist == NULL || !strlen(id3.albumartist)))
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
    if (has_grouping)
    {
        ADD_TAG(entry, tag_grouping, &id3.grouping);
    }
    else
    {
        ADD_TAG(entry, tag_grouping, &id3.title);
    }
    entry.data_length = offset;

    /* Write the header */
    tagfile_wbuf_put(cachefd, &entry, sizeof(struct temp_file_entry));

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
    if (has_grouping)
    {
        write_item(id3.grouping);
    }
    else
    {
        write_item(id3.title);
    }

    total_entry_count++;

    #undef ADD_TAG
}
#endif /*!defined(PLUGIN)*/


static bool tempbuf_insert(char *str, int id, int idx_id, bool unique)
{
    struct tempbuf_searchidx *index = (struct tempbuf_searchidx *)tempbuf;
    int len = strlen(str)+1;
    int i;
    unsigned *crcbuf = (unsigned *)&tempbuf[tempbuf_size-4];
    unsigned crc32 = 0xffffffff;
    /* the crc of the lower case string, in one call rather than one for
       each character */
    static char lower[TAG_MAXLEN + 1];
    for (i = 0; str[i] != '\0' && i < len - 1 && i < TAG_MAXLEN; i++)
        lower[i] = tolower(str[i]);
    crc32 = crc_32(lower, i, crc32);

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
    crc_next[tempbufidx] = crc_heads[crc32 & crc_mask];
    crc_heads[crc32 & crc_mask] = tempbufidx;
    tempbufidx++;

    return true;
}

static int compare(const void *p1, const void *p2)
{
    do_timed_yield();

    struct tempbuf_searchidx *e1 = (struct tempbuf_searchidx *)p1;
    struct tempbuf_searchidx *e2 = (struct tempbuf_searchidx *)p2;

    if (strcmp(e1->str, UNTAGGED) == 0)
    {
        if (strcmp(e2->str, UNTAGGED) == 0)
            return 0;
        return -1;
    }
    else if (strcmp(e2->str, UNTAGGED) == 0)
        return 1;

    return strncasecmp(e1->str, e2->str, TAG_MAXLEN);
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

        idlist = &lookup[i]->idlist;
        while (idlist->next != NULL)
            idlist = idlist->next;

        ALIGN_BUFFER(tempbuf_pos, tempbuf_left, alignof(struct tempbuf_id_list));
        tempbuf_left -= sizeof(struct tempbuf_id_list);
        if (tempbuf_left < 0)
            return -1;

        idlist->next = (struct tempbuf_id_list *)&tempbuf[tempbuf_pos];
        tempbuf_pos += sizeof(struct tempbuf_id_list);

        idlist = idlist->next;
        idlist->id = i;
        idlist->next = NULL;

        do_timed_yield();
    }

    if (!merge_sorted(index, old_n))
        qsort(index, tempbufidx, sizeof(struct tempbuf_searchidx), compare);
    memset(lookup, 0, lookup_buffer_depth * sizeof(struct tempbuf_searchidx **));

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

static bool build_numeric_indices(struct tagcache_header *h, int tmpfd)
{
    const long old_count = commit_mh.tch.entry_count;
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
            idx->flag = entrybuf[i].flag;

            if (idx->tag_seek[tag_commitid])
            {
                /* Data has been resurrected. */
                idx->flag |= FLAG_DIRTYNUM;
            }
            else if (tc_stat.ready && current_tcmh.commitid > 0)
            {
                idx->tag_seek[tag_commitid] = current_tcmh.commitid;
                idx->flag |= FLAG_DIRTYNUM;
            }
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
       each, or what build_index() asks for anyway */
    size_t strings = TAGCACHE_IS_SORTED(tag) ?
        datasize + h->datasize + entries * (4 + 12 + 4) : 0;
    return need + MAX(strings, TAGFILE_ENTRY_AVG_LENGTH * entries);
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
    remove_db_file(TAGCACHE_FILE_TEMP);
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
        remove_db_file(TAGCACHE_FILE_TEMP);
        return false;
    }

    if (tch.entry_count == 0)
    {
        /* A scan that found nothing new. Writing every tag file again the
           same way took as long as a commit of new files. */
        masterfd = open_master_fd(&tcmh, false);
        if (masterfd >= 0)
        {
            logf("nothing to commit");
            close(masterfd);
            close(tmpfd);
            remove_db_file(TAGCACHE_FILE_TEMP);
            return true;
        }
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

    /* Beyond here, jump to commit_error to undo locks and restore dircache */
    rc = false;
    read_lock++;

    /* Try to steal every buffer we can :) */
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

    for (i = 0; i < TAG_COUNT && !USR_CANCEL; i++)
    {
        int ret;

        if (TAGCACHE_IS_NUMERIC(i))
            continue;

        tc_stat.commit_step++;
        ret = build_index(i, &tch, tmpfd);
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
    tc_stat.commit_step = 0;
    if (commit_idx)
    {
        tempbuf_size = commit_full_size;
        commit_idx = NULL;
    }
    if (journal_written)
        commit_undo(&journal);
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

    if ( (masterfd = open_master_fd(&myhdr, true)) < 0)
        return;

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
                    return;

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

void tagcache_update_numeric(int idx_id, int tag, long data)
{
    queue_command(CMD_UPDATE_NUMERIC, idx_id, tag, data);
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
                                tag_lastoffset };
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

    update_master_header();

    return true;
}

#endif /* !__PCTOOL__ */

#ifndef __PCTOOL__
/* Calls update() with the filename and runtime values of every entry, and
   writes back the play count and rating if it returns more than 0 and they
   changed, or removes the entry if it returns less than 0. One pass, for
   imports too big to look each file up. */
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
        if (!get_index(masterfd, i, &idx, true)
            || !tagcache_retrieve(&tcs, i, tag_filename, buf, sizeof(buf)))
            continue;

        struct tagcache_runtime rt = {
            idx.tag_seek[tag_playcount], idx.tag_seek[tag_rating],
            idx.tag_seek[tag_lastplayed]
        };
        int action = update(buf, &rt, data);
        if (action < 0)
        {
            /* a file that is gone, like the files a sync replaced */
            if (delete_entry_in(i, masterfd, myhdr.tch.entry_count, fds,
                                true))
                deleted++;
        }
        else if (action > 0
                 && (rt.playcount != idx.tag_seek[tag_playcount]
                     || rt.rating != idx.tag_seek[tag_rating])
                 && get_index(masterfd, i, &idx, false))
        {
            idx.tag_seek[tag_playcount] = rt.playcount;
            idx.tag_seek[tag_rating] = rt.rating;
            idx.flag |= FLAG_DIRTYNUM;
            write_index(masterfd, i, &idx);
        }
        do_timed_yield();
    }
    if (deleted > 0)
        sweep_unused_tags(masterfd, myhdr.tch.entry_count, fds);

    for (int tag = 0; tag < TAG_COUNT; tag++)
    {
        if (fds[tag] >= 0)
            close(fds[tag]);
    }
    close(masterfd);
    tagcache_search_finish(&tcs);
    update_master_header();
    return true;
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

/* Every string of the tag files that no entry uses deletes, as
   delete_entry_in() does for one entry's, in one pass over each file */
static void sweep_unused_tags(int masterfd, long count, int *fds)
{
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
static bool check_event_queue(void)
{
#ifndef __PCTOOL__
    struct queue_event ev;

    if(!queue_peek(&tagcache_queue, &ev))
        return false;

    switch (ev.id)
    {
        case Q_STOP_SCAN:
        case SYS_POWEROFF:
        case SYS_REBOOT:
        case SYS_USB_CONNECTED:
            return true;
    }
#endif /* __PCTOOL__ */

    return false;
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
    for (int i = 0; i < tcmh.tch.entry_count; i++)
    {
        bytesleft -= sizeof(struct index_entry);
        if (bytesleft < 0)
        {
            logf("too big tagcache.");
            goto failure;
        }

        int rc = read_index_entries(fd, &tcramcache.hdr->indices[i], 1);
        if (rc != sizeof (struct index_entry))
        {
            logf("read error #10");
            goto failure;
        }
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
                goto failure;

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

#ifndef __PCTOOL__
/* during an import, which checks the files in Finder's folder against
   iTunesDB: looking for each of them took 10 ms after a sync */
static bool check_skips_synced;
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
    if (tcramcache.handle > 0)
        tcrc_buffer_lock();
    else
        return false;
    /* Wait for any in-progress dircache build to complete */
    dircache_wait();
#else
    if (!auto_update)
        return false;
#endif

    fd = open_tag_fd(&hdr, tag_filename, false); /* open read only*/

    if (fd < 0)
    {
        logf(TAGCACHE_FILE_INDEX " open fail", tag_filename);
        return false;
    }

    processed_dir_count = 0;
    tagr_start(fd);

    while (!check_event_queue())
    {
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

        int idx_id = tfe.idx_id; /* dircache reference clobbers *tfe */
#ifndef __PCTOOL__
        if (check_skips_synced && itunesdb_synced_path(buf))
            continue;
#endif
#ifdef HAVE_DIRCACHE
        struct index_entry *idx = &tcramcache.hdr->indices[idx_id];
        unsigned int searchflag;
        if (!auto_update)
        {
            if(idx->flag & FLAG_DIRCACHE) /* already found */
            {
                continue;
            }
            searchflag = DCS_CACHED_PATH; /* attempt to load cache references */
        }
        else /* If auto updating, check storage too */
        {
            searchflag = DCS_STORAGE_PATH;
        }

        int rc_cache = dircache_search(searchflag | DCS_UPDATE_FILEREF,
                                   &tcrc_dcfrefs[idx_id], buf);

        if (rc_cache > 0)           /* in cache and we have fileref */
        {
            idx->flag |= FLAG_DIRCACHE;
        }
        else if (rc_cache == 0)     /* not in cache but okay */
        {;}
        else if (auto_update && rc_cache == ENOENT)
#else
        if (!file_exists(buf))
#endif /* HAVE_DIRCACHE */
        {
            logf("Entry no longer valid.");
            logf("-> %s / %" PRId32, buf, tfe.tag_length);
            delete_entry(idx_id);
        }

        do_timed_yield();
    }

wend_finished:

#ifdef HAVE_DIRCACHE
    if (tcramcache.handle > 0)
        tcrc_buffer_unlock();
#endif
    close(fd);
    logf("done");

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
    while (!check_event_queue())
    {
        struct dirent *entry = readdir(dir);
        if (entry == NULL)
        {
            success = true;
            break;
        }

        if (is_dotdir_name(entry->d_name))
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
            if (!rockbox_dir(curpath))
                check_dir(curpath, add_files);
        }
        else if (add_files)
        {
            tc_stat.curentry = curpath;

            /* Add a new entry to the temporary db file. */
            add_tagcache(curpath, info.mtime);

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

#ifndef __PCTOOL__
/* this is called by the database tool to not pull in global_settings */
static
#endif
void do_tagcache_build(const char *path[])
{
    struct tagcache_header header;
    bool ret;

    scan_cut_short = false;
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
        return ;
    }

    cachefd = open_db_fd(TAGCACHE_FILE_TEMP, O_RDWR | O_CREAT | O_TRUNC);
    if (cachefd < 0)
    {
        logf("master file open failed: %s", TAGCACHE_FILE_TEMP);
        scan_cut_short = true;
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
    memset(&header, 0, sizeof(struct tagcache_header));
    write(cachefd, &header, sizeof(struct tagcache_header));
    tagfile_wbuf_used = 0;

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
    /* the commit can have the memory */
    itunesdb_forget_metadata();
#endif

commit:
    /* Write the header. */
    tagfile_wbuf_flush(cachefd);
    header.magic = TAGCACHE_MAGIC;
    header.datasize = data_size;
    header.entry_count = total_entry_count;
    lseek(cachefd, 0, SEEK_SET);
    write(cachefd, &header, sizeof(struct tagcache_header));
    close(cachefd);

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
    if (import_phase == TAGCACHE_IMPORT_SCANNING)
        import_phase = TAGCACHE_IMPORT_SAVING;
    if (commit())
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

#ifndef __PCTOOL__
    /* taken before the load, which keeps the cache from moving */
    free_ram_names();
    if (current_tcmh.tch.entry_count > 0)
    {
        ram_names_capacity = current_tcmh.tch.entry_count;
        ram_names_handle = core_alloc(ram_names_capacity
                                      * sizeof(struct path_name));
        if (ram_names_handle <= 0)
            ram_names_handle = ram_names_capacity = 0;

        /* the paths take at most what the filename file has */
        struct tagcache_header tch;
        int fd = open_tag_fd(&tch, tag_filename, false);
        if (fd >= 0)
        {
            close(fd);
            ram_paths_count = current_tcmh.tch.entry_count;
            ram_paths_size = tch.datasize;
            ram_paths_handle = core_alloc(ram_paths_count
                                          * (sizeof(uint32_t) + 1)
                                          + ram_paths_size);
            if (ram_paths_handle > 0)
                memset(core_get_data(ram_paths_handle), 0,
                       ram_paths_count * (sizeof(uint32_t) + 1));
            else
                ram_paths_handle = ram_paths_count = ram_paths_size = 0;
        }
    }
#endif

    /* At first we should load the cache (if exists). */
    tc_stat.ramcache = load_tagcache();

#ifndef __PCTOOL__
    if (tc_stat.ramcache && ram_names_handle > 0 && !ram_names_overflow)
    {
        struct path_name *names = core_get_data_pinned(ram_names_handle);
        qsort(names, ram_names_count, sizeof(*names), cmp_path_name);
        core_put_data_pinned(names);
    }
    else
        free_ram_names();
#endif

    if (!tc_stat.ramcache)
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
static bool update_and_reload(void)
{
    if (db_file_exists(TAGCACHE_FILE_TEMP))
    {
        /* one waits from before: it goes in first, or the scan would be
           skipped for it and miss the new files */
        allocate_tempbuf();
        commit();
        free_tempbuf();
    }
    tagcache_build();
    itunesdb_forget_metadata(); /* if the scan was skipped */
    bool scanned = !scan_cut_short;
    if (import_phase != TAGCACHE_IMPORT_NONE)
        import_phase = TAGCACHE_IMPORT_LOADING;
    if (scanned && db_file_exists(TAGCACHE_FILE_TEMP))
    {
        /* The commit found no buffer to borrow and waits for the next
           boot; take one just for it, as at boot */
        allocate_tempbuf();
        commit();
        free_tempbuf();
    }
#ifdef HAVE_TC_RAMCACHE
    /* a commit unloads it */
    if (!tc_stat.ramcache)
    {
        reallocate_ramcache();
        load_ramcache();
    }
    /* without enough memory for it, the database still works from disk */
    if (!tc_stat.ready)
        tagcache_commit_finalize();
#endif
    return scanned;
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
   then follows. Called once the computer has let go of the disk. */
bool tagcache_check_sync(void)
{
    if (!tc_stat.initialized || import_phase != TAGCACHE_IMPORT_NONE
        || !itunesdb_changed())
        return false;
    import_phase = TAGCACHE_IMPORT_READING;
    queue_post(&tagcache_queue, Q_SYNCED, 0);
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
    }
    close(fd);
    if (left == 0)
        song_count = count;
}

static void tagcache_thread(void)
{
    struct queue_event ev;
    bool check_done = false;
    cpu_boost(true);
    commit_recover();
    /* If the previous cache build/update was interrupted, commit
     * the changes first in foreground. After a sync the import does,
     * behind its progress screen. */
    if (db_file_exists(TAGCACHE_FILE_TEMP) && !sync_found)
    {
#if !(defined(__APPLE__) && (CONFIG_PLATFORM & PLATFORM_SDL))
        static const char *lines[] = {ID2P(LANG_TAGCACHE_BUSY),
                                      ID2P(LANG_TAGCACHE_UPDATE)};
        static const struct text_message message = {lines, 2};

        if (gui_syncyesno_run_w_tmo(HZ * 5, YESNO_YES, str(LANG_TAGCACHE),
                                    &message, NULL, NULL) == YESNO_YES)
#endif
        {
            allocate_tempbuf();
            commit();
            free_tempbuf();
        }
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
        import_total = itunesdb_import_tracks(true, &import_done,
                                              &import_total);
        import_done = 0;
        import_phase = TAGCACHE_IMPORT_SCANNING;
        queue_post(&tagcache_queue, Q_IMPORT_ITUNES, 0);
    }
    else if (!itunesdb_load_tracks())
        itunesdb_import_tracks(false, NULL, NULL);

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
                remove_db_file(TAGCACHE_FILE_TEMP);
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
                import_total = itunesdb_import_tracks(true, &import_done,
                                              &import_total);
                import_done = 0;
                import_phase = TAGCACHE_IMPORT_SCANNING;
                /* fallthrough */
            case Q_IMPORT_ITUNES:
                /* Finder has synced: the new files first, then what
                   iTunesDB says about them. Cut short, as by USB, all of
                   it is tried again: iTunesDB still differs from its stamp,
                   so after USB or at the next boot */
                if (!update_and_reload())
                {
                    import_phase = TAGCACHE_IMPORT_NONE;
                    break;
                }
                check_skips_synced = true;
                check_deleted_file_refs(true);
                check_skips_synced = false;
                import_done = import_total = 0;
                import_phase = TAGCACHE_IMPORT_COUNTS;
                /* the import is tried again on the next boot otherwise */
                if (!db_file_exists(TAGCACHE_FILE_TEMP))
                    itunesdb_import();
#ifdef ITUNESDB_HAVE_ART
                /* the slides of the new covers, ready for Cover Flow */
                import_done = import_total = 0;
                import_phase = TAGCACHE_IMPORT_COVERS;
                itunesdb_make_slides(&import_done, &import_total,
                                     check_event_queue);
#endif
                import_phase = TAGCACHE_IMPORT_NONE;
                break ;

            case Q_START_SCAN:
                check_done = false;
                /* fallthrough */
            case SYS_TIMEOUT:
                if (check_done || !tc_stat.ready)
                    break ;

#ifdef HAVE_TC_RAMCACHE
                if (!tc_stat.ramcache && global_settings.tagcache_ram)
                {
                    load_ramcache();
                    if (global_settings.tagcache_ram == TAGCACHE_RAM_ON)
                        check_deleted_file_refs(global_settings.tagcache_autoupdate);
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
                   waits for it, so at full speed */
                cpu_boost(true);
                run_command_queue(true);
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
    /* a database of an older firmware's, or none, is made again from iTunesDB
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
    if (sync_found)
        import_phase = TAGCACHE_IMPORT_READING;
#ifdef ITUNESDB_HAVE_ART
    else if (itunesdb_slides_todo())
        import_phase = TAGCACHE_IMPORT_SLIDES;
#endif
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
#endif

int tagcache_get_max_commit_step(void)
{
    return (int)(SORTED_TAGS_COUNT)+1;
}
#endif /*!defined(PLUGIN)*/
