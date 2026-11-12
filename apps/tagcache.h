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
#ifdef HAVE_TAGCACHE
#ifndef _TAGCACHE_H
#define _TAGCACHE_H

#include "config.h"
#include "system.h"
#include "metadata.h"
#include "settings.h"

/**
 Note: When adding new tags, make sure to update tags_str in tagcache.c and
 to raise TAGCACHE_FORMAT there; the statistics keep their places.
 */
enum tag_type { tag_artist = 0, tag_album, tag_genre, tag_title,
    tag_filename, tag_composer, tag_comment, tag_albumartist, tag_grouping, tag_year,
    tag_discnumber, tag_tracknumber, tag_virt_canonicalartist, tag_bitrate, tag_length,
    tag_playcount, tag_rating, tag_playtime, tag_lastplayed, tag_commitid, tag_mtime,
    tag_lastelapsed, tag_lastoffset,
    tag_date,      /* of release: yyyymmdd, yyyymm00 for a month alone, 0 for
                      none; its year is tag_year's */
    tag_dateadded, /* local seconds, as tag_lastplayed's times; 0 unknown */
    /* Real tags end here, count them. */
    TAG_COUNT,
    /* Virtual tags */
    tag_virt_basename=TAG_COUNT,
    tag_virt_length_min, tag_virt_length_sec,
    tag_virt_playtime_min, tag_virt_playtime_sec,
    tag_virt_entryage, tag_virt_autoscore,
    tag_virt_kind, /* 1 for a podcast, 2 for an audiobook, from iTunesDB */
    TAG_COUNT_ALL};

/* How many entries to fetch to the seek table at once while searching. */
#define SEEK_LIST_SIZE 32

#define TAGCACHE_MAX_FILTERS 4
#define TAGCACHE_MAX_CLAUSES 32

/* Tag to be used on untagged files. */
#define UNTAGGED "<Untagged>"
/* Maximum length of a single tag. */
#define TAG_MAXLEN (MAX_PATH*2)
/* buffer size for all the (stack allocated & static) buffers handling tc data */
#define TAGCACHE_BUFSZ (TAG_MAXLEN+32)

/* Numeric tags (we can use these tags with conditional clauses). */
#define TAG_BIT(tag) ((uint64_t)1 << (tag))
#define TAGCACHE_NUMERIC_TAGS (TAG_BIT(tag_year) | TAG_BIT(tag_discnumber) | \
    TAG_BIT(tag_tracknumber) | TAG_BIT(tag_length) | TAG_BIT(tag_bitrate) | \
    TAG_BIT(tag_playcount) | TAG_BIT(tag_rating) | TAG_BIT(tag_playtime) | \
    TAG_BIT(tag_lastplayed) | TAG_BIT(tag_commitid) | TAG_BIT(tag_mtime) | \
    TAG_BIT(tag_lastelapsed) | TAG_BIT(tag_lastoffset) | TAG_BIT(tag_date) | \
    TAG_BIT(tag_dateadded) | \
    TAG_BIT(tag_virt_length_min) | TAG_BIT(tag_virt_length_sec) | \
    TAG_BIT(tag_virt_playtime_min) | TAG_BIT(tag_virt_playtime_sec) | \
    TAG_BIT(tag_virt_entryage) | TAG_BIT(tag_virt_autoscore) | \
    TAG_BIT(tag_virt_kind))

/* false for any number past the masks, as tagtree's menus over 100 */
#define TAGCACHE_TAG_IN(mask, tag) \
    ((unsigned)(tag) < 64 && (((mask) >> (tag)) & 1))
#define TAGCACHE_IS_NUMERIC(tag) TAGCACHE_TAG_IN(TAGCACHE_NUMERIC_TAGS, tag)

/* Tags whose files the commit sorts (sortkey.h): an entry's seek in one is
   its place in that order */
#define TAGCACHE_SORTED_TAGS (TAG_BIT(tag_artist) | TAG_BIT(tag_album) | \
    TAG_BIT(tag_genre) | TAG_BIT(tag_composer) | TAG_BIT(tag_comment) | \
    TAG_BIT(tag_albumartist) | TAG_BIT(tag_grouping) | TAG_BIT(tag_title) | \
    TAG_BIT(tag_virt_canonicalartist))
#define TAGCACHE_IS_SORTED(tag) TAGCACHE_TAG_IN(TAGCACHE_SORTED_TAGS, tag)

enum clause { clause_none, clause_is, clause_is_not, clause_gt, clause_gteq,
    clause_lt, clause_lteq, clause_contains, clause_not_contains, 
    clause_begins_with, clause_not_begins_with, clause_ends_with, clause_not_ends_with, 
	clause_oneof, clause_begins_oneof, clause_ends_oneof, clause_contains_oneof, 
	clause_not_oneof, clause_not_begins_oneof, clause_not_ends_oneof, clause_not_contains_oneof,
    clause_logical_or };

struct tagcache_stat {
    char db_path[MAX_PATHNAME+1];  /* Path to DB root directory */

    bool initialized;        /* Is tagcache currently busy? */
    bool readyvalid;         /* Has tagcache ready status been ascertained */
    bool ready;              /* Is tagcache ready to be used? */
    bool ramcache;           /* Is tagcache loaded in ram? */
    bool commit_delayed;     /* Has commit been delayed until next reboot? */
    bool econ;               /* Is endianess correction enabled? */
    volatile bool syncscreen;/* Synchronous operation with debug screen? */
    volatile const char 
        *curentry;           /* Path of the current entry being scanned. */

    int  commit_step;        /* Commit progress */
    int  ramcache_allocated; /* Has ram been allocated for ramcache? */
    int  ramcache_used;      /* How much ram has been really used */
    int  progress;           /* Current progress of disk scan */
    int  processed_entries;  /* Scanned disk entries so far */
    int  total_entries;      /* Total entries in tagcache */
    int  queue_length;       /* Command queue length */

    //const char *uimessage;   /* Pending error message. Implement soon. */
};

enum source_type {source_constant, 
                  source_runtime, 
                  source_current_path /* dont add items after this.
                                       it is used as an index 
                                       into id3_to_search_mapping */
                 };

struct tagcache_search_clause
{
    int tag;
    int type;
    bool numeric;
    int source;
    long numeric_data;
    char *str;
};

struct tagcache_seeklist_entry {
    int32_t seek;
    int32_t flag;
    int32_t idx_id;
};

struct tagcache_search {
    /* For internal use only. */
    int fd, masterfd;
    int idxfd[TAG_COUNT];
    struct tagcache_seeklist_entry seeklist[SEEK_LIST_SIZE];
    int seek_list_count;
    int32_t filter_tag[TAGCACHE_MAX_FILTERS];
    int32_t filter_seek[TAGCACHE_MAX_FILTERS];
    int filter_count;
    struct tagcache_search_clause *clause[TAGCACHE_MAX_CLAUSES];
    int clause_count;
    int list_position;
    int seek_pos;
    long position;
    int entry_count;
    bool valid;
    bool initialized;
    uint32_t *unique_list;
    int unique_list_capacity;
    int unique_list_count;
    /* told of each song that matches, before its name is passed over as
       found already: with the database's memory held still, so it doesn't
       wait for anything */
    void (*match)(const int32_t *tag_seek, void *data);
    void *match_data;

    /* Exported variables. */
    bool ramsearch;      /* Is ram copy of the tagcache being used. */
    bool ramresult;      /* False if result is not static, and must be copied.
                            Currently always false since ramresult buffer is
                            movable */
    int type;            /* The tag type to be searched. Only nonvirtual tags */
    char *result;        /* The result data for all tags. */
    int result_len;      /* Length of the result including \0 */
    int32_t result_seek; /* Current position in the tag data. */
    int32_t idx_id;      /* Entry number in the master index. */
};

#ifdef __PCTOOL__
void tagcache_reverse_scan(void);
/* call this directly instead of tagcache_build in order to not pull
 * on global_settings */
void do_tagcache_build(const char *path[]);
#endif

const char* tagcache_tag_to_str(int tag);

bool tagcache_find_index(struct tagcache_search *tcs, const char *filename);
bool tagcache_check_clauses(struct tagcache_search *tcs,
                            struct tagcache_search_clause **clause, int count);
bool tagcache_search(struct tagcache_search *tcs, int tag);
/* for this search only, as tagcache_search() clears it */
void tagcache_search_set_match(struct tagcache_search *tcs,
                               void (*match)(const int32_t *tag_seek, void *data),
                               void *data);
void tagcache_search_set_uniqbuf(struct tagcache_search *tcs,
                                 void *buffer, long length);
bool tagcache_search_add_filter(struct tagcache_search *tcs,
                                int tag, int seek);
bool tagcache_search_add_clause(struct tagcache_search *tcs,
                                struct tagcache_search_clause *clause);
bool tagcache_get_next(struct tagcache_search *tcs, char *buf, long size);
bool tagcache_retrieve(struct tagcache_search *tcs, int idxid, 
                       int tag, char *buf, long size);
void tagcache_search_finish(struct tagcache_search *tcs);
long tagcache_get_numeric(const struct tagcache_search *tcs, int tag);
/* for a file of iTunesDB's whose own tags are empty */
bool tagcache_fill_text(struct mp3entry *id3, const char *filename);
/* a song's tag_date and year from a tag's text, or NULL: year in as its
   parser took it, and out */
void tagcache_song_date(long *year, long *date, const char *text);
/* a tag_date as "yyyy-mm-dd", or "yyyy-mm" for a month alone */
void tagcache_date_text(char *buf, size_t size, long date);
long tagcache_increase_serial(void);
bool tagcache_import_changelog(void);
/* entries in the database, -1 until known */
int tagcache_get_song_count(void);
int tagcache_get_tag_count(int tag);
/* Where the import after a sync is, for showing its progress; done and
   total are of the files scanned and of the commit's steps */
enum {
    TAGCACHE_IMPORT_NONE = 0, /* none, or done */
    TAGCACHE_IMPORT_READING,  /* iTunesDB */
    TAGCACHE_IMPORT_SCANNING, /* the files, about total of them */
    TAGCACHE_IMPORT_SAVING,   /* the commit */
    TAGCACHE_IMPORT_LOADING,  /* into memory, and the file references */
    TAGCACHE_IMPORT_COUNTS,   /* ratings and play counts */
    TAGCACHE_IMPORT_COVERS,   /* Cover Flow's slides, done of total */
    TAGCACHE_IMPORT_SLIDES,   /* only those, without a sync */
    TAGCACHE_IMPORT_CHECKING, /* the songs outside Finder's folder, without
                                 a sync: found of those known */
};
int tagcache_get_import(int *done, int *total);
/* an import or a check after USB, or a commit, was left for want of room on
   the disk since the last call */
bool tagcache_take_no_space(void);
/* KB the library's caches, as the covers, leave free: what the next import
   of the library needs, so that they don't take its room */
unsigned long tagcache_caches_floor_kb(void);
/* during a restore: the old counts of Finder's songs are this iTunesDB's */
bool tagcache_restore_finder(void);
/* a database of another format built again, until its import is done */
bool tagcache_restoring(void);
/* main() has taken the boot's memory: a commit may have the rest */
void tagcache_boot_done(void);
bool tagcache_check_sync(void);
/* the sum of the images outside Finder's folder, by the last check after USB
   that went through: false without one */
bool tagcache_outside_images(uint32_t *sum);
bool tagcache_create_changelog(struct tagcache_search *tcs);
struct tagcache_runtime {
    long playcount;
    long rating;      /* 0..10 */
    long lastplayed;  /* a serial number, or the time of the play */
    long playtime;    /* ms, of all its plays */
    long commitid;    /* of the commit that added it */
    long lastelapsed; /* where it was left: ms */
    long lastoffset;  /* and the file's byte */
};
bool tagcache_update_runtime(int (*update)(const char *filename,
                                           struct tagcache_runtime *rt,
                                           void *data),
                             void *data);
void tagcache_update_numeric(int idx_id, int tag, long data);
/* the changes to ratings, or to play counts and last plays, since boot: as
   many as there were, for what goes by them to know it's out of date */
long tagcache_runtime_generation(bool ratings);
int tagcache_entry_count(void);
bool tagcache_finds_entries(void);
long tagcache_find_entry(const char *filename);
/* what smart playlists go by, of an entry */
struct tagcache_numbers {
    long length;      /* ms */
    long playcount;
    long playtime;    /* ms, of all its plays */
    long rating;      /* 0..10 */
    long lastplayed;  /* a serial number, or the time of the play */
    int kind;         /* 0 music, 1 podcast, 2 audiobook */
};
/* with each entry's file, or NULL for it without names, which is quicker */
bool tagcache_read_entries(void (*fn)(int idx_id, const char *filename,
                                      const struct tagcache_numbers *n,
                                      void *data),
                           void *data, bool names);
bool tagcache_get_numbers(int idx_id, struct tagcache_numbers *n);
/* A podcast episode as its lists mark it: started, with a place; new, not
   played and without one, by the Mac's mark until one made on the iPod, or
   for a file without that mark by its plays; else played. As it is now, with
   the changes queued, from memory only: played without the database there */
enum { EPISODE_PLAYED, EPISODE_NEW, EPISODE_STARTED };
int tagcache_episode_state(int idx_id);
/* the entry a search's match is of (tagcache_search_set_match()), -1 when
   the search isn't through the database's memory */
int tagcache_match_entry(const int32_t *tag_seek);
/* The mark of a podcast or audiobook Finder synced, not played or played, in
   memory and in the flags (itunesdb_set_unplayed()): false for an entry
   without one. path is the entry's file */
bool tagcache_set_unplayed(int idx_id, const char *path, bool unplayed);
/* the changes to places, plays and marks since boot, which the lists with
   the episodes' states go by */
long tagcache_states_generation(void);
/* an entry's kind as iTunesDB has it, 0 music, 1 podcast, 2 audiobook: from
   memory, 0 without the database there or for no entry */
int tagcache_track_kind(int idx_id);
/* Where an entry was left, in ms, as it is now, a change queued for it too
   (tag_lastelapsed): 0 for none, and without the database in memory. As
   tagcache_get_numbers(), from memory only, for a list's rows as they draw */
long tagcache_track_place(int idx_id);
/* A track's rating and plays as the database has them now: one played again
   from the buffer, as by Previous, has those of when it was buffered, which
   playback copies to it again after any change made to it. Its own without
   the database in memory. */
long tagcache_track_rating(const struct mp3entry *id3);
long tagcache_track_playcount(const struct mp3entry *id3);
long tagcache_sort_key(int idx_id, int tag);
/* The letter Quick Scroll shows for a name of a tag, by its place in the
   tag's file as tagcache_sort_key() has it, from what the commit sorted it
   by (sortkey_initial()): 0 if it isn't known */
ucschar_t tagcache_initial(int tag, long seek);

/* Sort fields, the names that names sort by as Music has them: Sort Artist
   for an artist, Sort Name for a song's title, and so on. The commit sorts
   by them from files of pairs, each a tag, a name and its Sort field: one of
   Finder's songs, which each import writes from iTunesDB, and one of the
   other songs', from the tags of their files. A pair is kept only where the
   Sort field sorts otherwise than the name itself. */
#define TAGCACHE_SORT_FINDER  ROCKBOX_DIR "/itunesdb_sort.dat"
#define TAGCACHE_SORT_MAGIC   0x46534252 /* "RBSF" */
#define TAGCACHE_SORT_VERSION 1
struct tagcache_sort_header {
    uint32_t magic;
    uint32_t version;
    uint32_t count;     /* of pairs */
    uint32_t crc;       /* crc_32() of the pairs after it, from 0xffffffff */
    uint32_t db_size;   /* of the iTunesDB they're from, 0 for the files' */
    uint32_t db_mtime;
};
/* Then each pair: the tag, the sizes of the name and of its Sort field with
   their ends, two bytes each, little-endian, then the two strings */
#define TAGCACHE_SORT_PAIR_HEAD 5
/* the pair's head into head: false for a pair too long to keep */
static inline bool tagcache_sort_pair_head(unsigned char *head, int tag,
                                           size_t name_size, size_t value_size)
{
    if (name_size > TAG_MAXLEN + 1 || value_size > TAG_MAXLEN + 1)
        return false;
    head[0] = tag;
    head[1] = name_size & 0xff;
    head[2] = name_size >> 8;
    head[3] = value_size & 0xff;
    head[4] = value_size >> 8;
    return true;
}

bool tagcache_modify_numeric_entry(struct tagcache_search *tcs, 
                                   int tag, long data);

struct tagcache_stat* tagcache_get_stat(void);
int tagcache_get_commit_step(void);
bool tagcache_prepare_shutdown(void);
void tagcache_shutdown(void);
void tagcache_remove_statefile(void);

void tagcache_screensync_event(void);
void tagcache_screensync_enable(bool state);

#ifdef HAVE_TC_RAMCACHE
bool tagcache_is_in_ram(void);
bool tagcache_ram_pending(void);
void tagcache_ram_load_now(void);
#ifdef HAVE_DIRCACHE
bool tagcache_fill_tags(struct mp3entry *id3, const char *filename);
int tagcache_get_rating(const char *filename);
#endif
void tagcache_unload_ramcache(void);
#endif
void tagcache_commit_finalize(void);
void tagcache_init(void) INIT_ATTR;
bool tagcache_is_initialized(void);
bool tagcache_is_fully_initialized(void);
bool tagcache_is_usable(void);
void tagcache_start_scan(void);
void tagcache_stop_scan(void);
bool tagcache_update(void);
bool tagcache_rebuild(void);
int tagcache_get_max_commit_step(void);
/* the number of commits so far, which a change of the files makes one more */
int tagcache_get_commitid(void);
#ifdef HAVE_TC_RAMCACHE
/* f with the index and the tags of every track of the database in memory,
   in its order: seeks as searches give them, the values of numeric tags;
   false when the database isn't in memory. f mustn't yield. */
bool tagcache_for_each_track(void (*f)(int idx_id, const int32_t *tag_seek,
                                       void *data),
                             void *data);
/* The text of a tag's name by its seek, as a search or the seeks above give
   it, of the database in memory as it is now: false without it, or for
   UNTAGGED, which is no name */
bool tagcache_tag_string(int tag, int32_t seek, char *buf, size_t size);
#endif
#endif
#endif
