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

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "file.h"
#include "kernel.h"
#include "core_alloc.h"
#include "plugin.h"
#include "audio.h"
#include "playlist.h"
#include "string-extra.h"
#include "timefuncs.h"
#include "rbpaths.h"
#include "tagcache.h"
#include "itunesdb.h"
#include "smart_playlists.h"
#include "splash.h"
#include "lang.h"
#include "library_playlists.h"

/* Live smart playlists, as the stock firmware keeps them: a song rated five
   stars on the iPod is in "5 Stars" at once, one played in "Recently Played",
   songs copied outside Finder's folder too. Their rules are read at the import,
   and their songs worked out from the database as it is: those still in them
   where they were, those new at the end, into their indexes, which their
   screens show; their files are written from those after a sync, or as one
   starts to play. A playlist with a rule of another kind, as of names or the
   date added, stays as Finder wrote it. */

#define SMART_FILE    ROCKBOX_DIR "/itunesdb_smart.dat"
#define SMART_MAGIC   0x534d5254 /* "SMRT" */
#define SMART_VERSION 2

struct smart_head {
    uint32_t magic, version, count;
};

/* each playlist: a field no longer used, its path's length, when Finder
   last changed it, and its rules; then its path */
struct smart_record {
    uint64_t unused;
    uint32_t path_len;
    uint32_t time;
    struct smart_rules rules;
};

/* iTunesDB's fields and actions, as libgpod and the iPodLinux wiki name them */
#define F_TIME       0x0d   /* length, ms */
#define F_PLAYCOUNT  0x16
#define F_LASTPLAYED 0x17   /* a date */
#define F_RATING     0x19   /* 20 a star */
#define F_KIND       0x3c   /* media kind, bits */

#define A_IS         0x00000001
#define A_GREATER    0x00000010
#define A_LESS       0x00000040
#define A_RANGE      0x00000100
#define A_IN_LAST    0x00000200
#define A_AND        0x00000400
#define A_NOT        0x02000000

#define LIMIT_MINUTES 1
#define LIMIT_SONGS   3
#define LIMIT_HOURS   4

#define SORT_RANDOM   0x02
#define SORT_PLAYS    0x14
#define SORT_PLAYED   0x15
#define SORT_RATING   0x17
#define SORT_REVERSE  0x80000000u

/* seconds from 1904, iTunes' dates, to 1970 */
#define MAC_EPOCH     2082844800LL
/* last played values from here on are times, below they are serials */
#define LASTPLAYED_TIME 0x40000000L

bool smart_rules_supported(const struct smart_rules *r)
{
    if (r->count > SMART_RULES_MAX)
        return false;
    if (r->check_rules)
    {
        for (int i = 0; i < r->count; i++)
        {
            const uint32_t field = r->rule[i].field;
            const uint32_t action = r->rule[i].action & ~A_NOT;
            if (r->rule[i].action & ~(A_NOT | 0xffff))
                return false; /* of strings */
            /* not yet by when last played, as the import doesn't take the
               Mac's plays: "Recently Played" would lose them all; nor by
               length, whose unit isn't known for sure */
            switch (field)
            {
                case F_PLAYCOUNT:
                case F_RATING:
                    if (action != A_IS && action != A_GREATER && action != A_LESS
                        && action != A_RANGE)
                        return false;
                    break;
                case F_KIND:
                    if (action != A_AND)
                        return false;
                    break;
                default:
                    return false;
            }
        }
    }
    if (r->check_limits)
    {
        if (r->limit_type != LIMIT_SONGS && r->limit_type != LIMIT_MINUTES
            && r->limit_type != LIMIT_HOURS)
            return false;
        const uint32_t sort = r->limit_sort & ~SORT_REVERSE;
        if (sort != SORT_RANDOM && sort != SORT_PLAYS && sort != SORT_RATING)
            return false;
    }
    return true;
}

/* The import's: a file of the smart playlists it wrote, the old one until the
   new one is whole */
static int import_fd = -1;
static uint32_t import_count;
static bool import_ok;

void smart_import_begin(void)
{
    import_count = 0;
    import_fd = open(SMART_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    struct smart_head head = { SMART_MAGIC, SMART_VERSION, 0 };
    import_ok = import_fd >= 0
                && write(import_fd, &head, sizeof(head)) == sizeof(head);
}

void smart_import_note(const char *file, uint32_t time,
                       const struct smart_rules *rules)
{
    if (import_fd < 0 || !import_ok || !smart_rules_supported(rules))
        return;
    struct smart_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.path_len = strlen(file);
    rec.time = time;
    rec.rules = *rules;
    import_ok = write(import_fd, &rec, sizeof(rec)) == sizeof(rec)
                && write(import_fd, file, rec.path_len) == (ssize_t)rec.path_len;
    import_count++;
}

void smart_import_end(bool complete)
{
    if (import_fd < 0)
        return;
    struct smart_head head = { SMART_MAGIC, SMART_VERSION, import_count };
    if (import_ok)
        import_ok = lseek(import_fd, 0, SEEK_SET) == 0
                    && write(import_fd, &head, sizeof(head)) == sizeof(head);
    if (close(import_fd) < 0)
        import_ok = false;
    import_fd = -1;
    if (!complete || !import_ok || rename(SMART_FILE ".tmp", SMART_FILE) < 0)
        remove(SMART_FILE ".tmp");
}


/* What a pass over the database gives of each song: its numbers */
struct sp_entry {
    int32_t idx;            /* in the database */
    int32_t length;         /* ms */
    uint32_t last;          /* Mac time, 0 for none */
    uint16_t plays;
    uint8_t rating;         /* 0..100, as iTunes has it */
    uint8_t kind;           /* iTunes' bits */
};

#define FLAG_MATCH 1
#define FLAG_KEPT  2

static struct {
    struct sp_entry *entries;   /* as the database has them, by idx */
    uint32_t *chosen;           /* a limit's, in its order */
    uint8_t *flags;
    struct lp_song *songs;      /* a playlist's, as it had them and has them */
    char *in;                   /* its file read, if it has no index yet */
    size_t in_size;
    int count, max;
    bool overflow;
    int64_t now;                /* Mac time, 0 if the clock isn't set */
} sp;

static void read_entry(int idx_id, const char *filename,
                       const struct tagcache_numbers *n, void *data)
{
    (void)filename;
    (void)data;
    if (sp.count >= sp.max)
    {
        sp.overflow = true;
        return;
    }
    struct sp_entry *e = &sp.entries[sp.count++];
    e->idx = idx_id;
    e->length = n->length;
    e->last = n->lastplayed >= LASTPLAYED_TIME ?
              (uint32_t)(n->lastplayed + MAC_EPOCH) : 0;
    e->plays = MIN(n->playcount, 0xffff);
    e->rating = MIN(n->rating, 10) * 10;
    static const uint8_t kinds[] = { 0x01, 0x04, 0x08 };
    e->kind = n->kind >= 0 && n->kind < 3 ? kinds[n->kind] : 0x01;
}

/* the song of this entry of the database, or -1 */
static int entry_of(uint32_t idx)
{
    int lo = 0, hi = sp.count - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        const uint32_t at = sp.entries[mid].idx;
        if (at == idx)
            return mid;
        if (at < idx)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static bool rule_met(const struct smart_rule *r, const struct sp_entry *e)
{
    int64_t v;
    switch (r->field)
    {
        case F_TIME:        v = e->length; break;
        case F_PLAYCOUNT:   v = e->plays; break;
        case F_RATING:      v = e->rating; break;
        case F_LASTPLAYED:  v = e->last; break;
        case F_KIND:        v = e->kind; break;
        default:            return false;
    }
    const bool negated = r->action & A_NOT;
    bool met;
    switch (r->action & ~A_NOT)
    {
        case A_IS:      met = v == r->from; break;
        case A_GREATER: met = v > r->from; break;
        case A_LESS:    met = v < r->from; break;
        case A_RANGE:
            met = v >= MIN(r->from, r->to) && v <= MAX(r->from, r->to);
            break;
        case A_IN_LAST:
            /* not without the time now */
            met = sp.now && v && v > sp.now + r->from_date * r->from_units;
            break;
        case A_AND:     met = (v & r->from) != 0; break;
        default:        met = false; break;
    }
    return met != negated;
}

static bool rules_met(const struct smart_rules *r, const struct sp_entry *e)
{
    if (!r->check_rules || r->count == 0)
        return true;
    for (int i = 0; i < r->count; i++)
    {
        const bool met = rule_met(&r->rule[i], e);
        if (r->any && met)
            return true;
        if (!r->any && !met)
            return false;
    }
    return !r->any;
}

/* the order of a limit: most first, or least with its top bit */
static uint32_t limit_sort;

static int64_t sort_key(const struct sp_entry *e)
{
    switch (limit_sort & ~SORT_REVERSE)
    {
        case SORT_PLAYS:  return e->plays;
        case SORT_PLAYED: return e->last;
        case SORT_RATING: return e->rating;
        default:          return 0;
    }
}

static int cmp_limit(const void *a, const void *b)
{
    const uint32_t i = *(const uint32_t *)a, j = *(const uint32_t *)b;
    const int64_t x = sort_key(&sp.entries[i]);
    const int64_t y = sort_key(&sp.entries[j]);
    const int c = (x > y) - (x < y);
    if (c)
        return (limit_sort & SORT_REVERSE) ? c : -c;
    /* those alike as the database has them, the same each time */
    return (i > j) - (i < j);
}

/* how much of the limit a song takes, and all of it */
static int64_t limit_part(const struct smart_rules *r, const struct sp_entry *e)
{
    return r->limit_type == LIMIT_SONGS ? 1 : e->length;
}

static int64_t limit_whole(const struct smart_rules *r)
{
    switch (r->limit_type)
    {
        case LIMIT_MINUTES: return (int64_t)r->limit_value * 60 * 1000;
        case LIMIT_HOURS:   return (int64_t)r->limit_value * 3600 * 1000;
        default:            return r->limit_value;
    }
}

/* The songs a playlist has, by the database as it is, from the n it had in
   sp.songs: those still in it where they were, the new ones at the end, with
   no line in its file; a limit's, if others than it had, in its order, by
   chance those it had first. How many. */
static int choose(const struct smart_rules *r, int n)
{
    const int most = MIN(sp.max, LP_MAX);
    for (int i = 0; i < sp.count; i++)
        sp.flags[i] = rules_met(r, &sp.entries[i]) ? FLAG_MATCH : 0;
    if (!r->check_limits)
    {
        int out = 0;
        for (int i = 0; i < n; i++)
        {
            const int e = entry_of(sp.songs[i].idx);
            if (e >= 0 && sp.flags[e] == FLAG_MATCH)
            {
                sp.flags[e] |= FLAG_KEPT;
                sp.songs[out++] = sp.songs[i];
            }
        }
        for (int e = 0; e < sp.count && out < most; e++)
        {
            if (sp.flags[e] == FLAG_MATCH)
            {
                sp.songs[out].idx = sp.entries[e].idx;
                sp.songs[out].line = LP_NO_LINE;
                out++;
            }
        }
        return out;
    }

    limit_sort = r->limit_sort;
    const bool random = (r->limit_sort & ~SORT_REVERSE) == SORT_RANDOM;
    int chosen = 0, kept = 0;
    if (random)
    {
        /* those it has first, so that it doesn't change each time */
        for (int i = 0; i < n; i++)
        {
            const int e = entry_of(sp.songs[i].idx);
            if (e >= 0 && sp.flags[e] == FLAG_MATCH)
            {
                sp.flags[e] |= FLAG_KEPT;
                sp.chosen[chosen++] = e;
            }
        }
        kept = chosen;
    }
    for (int e = 0; e < sp.count; e++)
    {
        if (sp.flags[e] == FLAG_MATCH)
            sp.chosen[chosen++] = e;
    }
    if (random)
    {
        for (int i = chosen - 1; i > kept; i--)
        {
            const int j = kept + rand() % (i - kept + 1);
            const uint32_t t = sp.chosen[i];
            sp.chosen[i] = sp.chosen[j];
            sp.chosen[j] = t;
        }
    }
    else
        qsort(sp.chosen, chosen, sizeof(uint32_t), cmp_limit);
    const int64_t whole = limit_whole(r);
    int64_t used = 0;
    int fit = 0;
    for (int i = 0; i < chosen && fit < most; i++)
    {
        const int64_t part = limit_part(r, &sp.entries[sp.chosen[i]]);
        if (used + part > whole)
        {
            if (r->limit_type == LIMIT_SONGS)
                break;
            continue; /* a shorter one may fit still */
        }
        used += part;
        sp.chosen[fit++] = sp.chosen[i];
    }
    /* the same songs, whose order the column it's shown by gives: as they
       were, their lines with them */
    for (int i = 0; i < sp.count; i++)
        sp.flags[i] &= ~FLAG_KEPT;
    for (int i = 0; i < fit; i++)
        sp.flags[sp.chosen[i]] |= FLAG_KEPT;
    bool same = fit == n;
    for (int i = 0; i < n && same; i++)
    {
        const int e = entry_of(sp.songs[i].idx);
        same = e >= 0 && (sp.flags[e] & FLAG_KEPT);
        if (same)
            sp.flags[e] &= ~FLAG_KEPT; /* once */
    }
    if (!same)
    {
        for (int i = 0; i < fit; i++)
        {
            sp.songs[i].idx = sp.entries[sp.chosen[i]].idx;
            sp.songs[i].line = LP_NO_LINE;
        }
    }
    return fit;
}

/* a hash of the songs in their order, for whether they changed */
static uint64_t songs_hash(int n)
{
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)n;
    for (int i = 0; i < n; i++)
        h = (h ^ sp.songs[i].idx) * 0x100000001b3ull;
    return h;
}

/* When each was last seen to be right, as of this boot: the changes to ratings
   and to plays then, by its place in the rules file. Those by ratings alone
   needn't be looked at after a song played. */
#define SMART_KNOWN 64
static struct {
    long ratings, plays;
    int commitid;           /* of the database then: a commit has other songs */
    bool known, by_plays;
} checked[SMART_KNOWN];

static bool rules_by_plays(const struct smart_rules *r)
{
    if (r->check_limits)
    {
        const uint32_t sort = r->limit_sort & ~SORT_REVERSE;
        if (sort == SORT_PLAYS || sort == SORT_PLAYED || sort == SORT_RANDOM)
            return true;
    }
    for (int i = 0; r->check_rules && i < r->count; i++)
    {
        if (r->rule[i].field == F_PLAYCOUNT || r->rule[i].field == F_LASTPLAYED)
            return true;
    }
    return false;
}

static bool still_known(int i)
{
    return i < SMART_KNOWN && checked[i].known
           && checked[i].commitid == tagcache_get_commitid()
           && checked[i].ratings == tagcache_runtime_generation(true)
           && (!checked[i].by_plays
               || checked[i].plays == tagcache_runtime_generation(false));
}

/* the place of each in the rules file, from the second on */
static bool next_record(int fd, off_t *pos, struct smart_record *rec,
                        char *file, size_t size)
{
    if (lseek(fd, *pos, SEEK_SET) != *pos
        || read(fd, rec, sizeof(*rec)) != sizeof(*rec)
        || rec->path_len >= size
        || read(fd, file, rec->path_len) != (ssize_t)rec->path_len)
        return false;
    file[rec->path_len] = '\0';
    *pos += sizeof(*rec) + rec->path_len;
    return true;
}

/* Those of the rules file not known to be right, or the one of that file
   only: each worked out from the numbers of the database in memory, into its
   index, and with files into its file too, but for the one the music is
   from, which it goes by places in. With Loading on the screen if show. */
static void run_rules(char *buf, size_t size, const char *only, bool files,
                      bool show)
{
    const int fd = open(SMART_FILE, O_RDONLY);
    if (fd < 0)
        return;
    struct smart_head head;
    if (read(fd, &head, sizeof(head)) != sizeof(head)
        || head.magic != SMART_MAGIC || head.version != SMART_VERSION
        || head.count == 0)
    {
        close(fd);
        return;
    }

    /* the buffer: the songs, a limit's, a byte each, a playlist's, and a block
       to read its file */
    const size_t per = sizeof(struct sp_entry) + sizeof(uint32_t) + 1
                       + sizeof(struct lp_song);
    const size_t block = 8192;
    memset(&sp, 0, sizeof(sp));
    buf = (char *)ALIGN_UP((uintptr_t)buf, 8);
    if (size < block + 64 * per + 16)
    {
        close(fd);
        return;
    }
    sp.max = (size - block - 16) / per;
    sp.entries = (struct sp_entry *)buf;
    sp.songs = (struct lp_song *)(sp.entries + sp.max);
    sp.chosen = (uint32_t *)(sp.songs + sp.max);
    sp.flags = (uint8_t *)(sp.chosen + sp.max);
    sp.in = (char *)(sp.flags + sp.max);
    sp.in_size = block;

    if (show)
        splash(0, ID2P(LANG_WAIT));
    if (!tagcache_read_entries(read_entry, NULL, false) || sp.overflow
        || sp.count == 0)
    {
        close(fd);
        return;
    }
#if CONFIG_RTC
    {
        struct tm *tm = get_time();
        sp.now = valid_time(tm) ? (int64_t)mktime(tm) + MAC_EPOCH : 0;
    }
#endif
    char playing[MAX_PATH];
    playing[0] = '\0';
    if (audio_status() & AUDIO_STATUS_PLAY)
        playlist_get_name(NULL, playing, sizeof(playing));

    const long ratings = tagcache_runtime_generation(true);
    const long plays = tagcache_runtime_generation(false);
    const int commitid = tagcache_get_commitid();
    char file[MAX_PATH];
    off_t pos = sizeof(head);
    struct smart_record rec;
    for (uint32_t i = 0; i < head.count
                         && next_record(fd, &pos, &rec, file, sizeof(file)); i++)
    {
        if ((only && !lp_same_file(file, only)) || (!only && still_known(i)))
            continue;
        bool right = true;
        struct lp_state state;
        int n = -1;
        /* one gone or of rules not worked out here: nothing to do, known so,
           not looked at again each boot */
        if (file_exists(file) && smart_rules_supported(&rec.rules))
        {
            n = lp_index_read(file, sp.songs, sp.max, &state);
            const bool fresh = n < 0;
            if (fresh)
                n = lp_index_build(file, sp.songs, sp.max, sp.in, sp.in_size,
                                   &state);
            if (n < 0)
                continue;
            const uint64_t before = songs_hash(n);
            n = choose(&rec.rules, n);
            lp_sort(sp.songs, n, state.sort);
            if (fresh || songs_hash(n) != before || (files && state.pending))
            {
                if (files && !lp_in_file_order(sp.songs, n, &state)
                    && !lp_same_file(file, playing))
                    lp_write_file(file, sp.songs, n, &state);
                right = lp_index_write(file, sp.songs, n, &state);
            }
        }
        if (i < SMART_KNOWN && right)
        {
            checked[i].ratings = ratings;
            checked[i].plays = plays;
            checked[i].commitid = commitid;
            checked[i].known = true;
            checked[i].by_plays = rules_by_plays(&rec.rules);
        }
        yield();
    }
    close(fd);
}

/* one at a time: the import's, on the database's thread, and a screen's */
static void run(char *buf, size_t size, const char *only, bool files, bool show)
{
    static bool running;
    if (running)
        return;
    running = true;
    run_rules(buf, size, only, files, show);
    running = false;
}

void smart_update_after_import(void)
{
    const int count = tagcache_entry_count();
    if (count <= 0)
        return;
    /* each one looked at again, of the database just in */
    memset(checked, 0, sizeof(checked));
    const size_t size = (size_t)count * (sizeof(struct sp_entry)
                        + sizeof(uint32_t) + 1 + sizeof(struct lp_song))
                        + 8192 + 64;
    const int handle = core_alloc(size);
    if (handle <= 0)
        return;
    core_pin(handle);
    run(core_get_data(handle), size, NULL, true, false);
    core_unpin(handle);
    core_free(handle);
}

/* whether any isn't known to be right, without reading the database */
static bool any_unknown(const char *only)
{
    const int fd = open(SMART_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    struct smart_head head;
    bool unknown = false;
    if (read(fd, &head, sizeof(head)) == sizeof(head)
        && head.magic == SMART_MAGIC && head.version == SMART_VERSION)
    {
        char file[MAX_PATH];
        off_t pos = sizeof(head);
        struct smart_record rec;
        for (uint32_t i = 0; i < head.count && !unknown
                             && next_record(fd, &pos, &rec, file, sizeof(file)); i++)
        {
            if (!only || lp_same_file(file, only))
                unknown = !still_known(i);
        }
    }
    close(fd);
    return unknown;
}

void smart_update_at_boot(void)
{
    if (!tagcache_finds_entries() || !any_unknown(NULL))
        return;
    size_t size;
    char *buf = plugin_get_buffer(&size);
    if (!buf)
        return;
    cpu_boost(true);
    run(buf, size, NULL, false, false);
    cpu_boost(false);
}

bool smart_prepare(const char *file)
{
    if (!file || !tagcache_finds_entries() || !any_unknown(file))
        return false;
    size_t size;
    char *buf = plugin_get_buffer(&size);
    if (!buf)
        return false;
    cpu_boost(true);
    run(buf, size, file, false, true);
    cpu_boost(false);
    return true;
}
