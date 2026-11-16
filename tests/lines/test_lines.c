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

/* The six readers of a playlist's lines in library_playlists.c and
   on_the_go.c, each a loop of its own until line_reader.c: built twice, once
   with BASE's files (ref/, made by make_ref.py) and once with the tree's
   (LP_SOURCE and OTG_SOURCE), each in a program of its own. Both run the
   same cases in a folder of their own and write what they did to a trace:
   what each function returned, the state it left, every call it made of the
   firmware below (the database, the queue, yields, reads), and every file
   in the folder afterwards, byte for byte (one over 4 KB by its size and
   SHA-256). The Makefile compares the two traces (check-lines).

   The cases: playlists written for the readers' corners (CRLF, no last
   '\n', a BOM, notes, empty files and lines, a '\r' alone, a line as long as
   the buffer or longer, a block that ends inside a CRLF, a '\0') and random
   ones of all of those; each read by every function, with the buffers at
   their least and larger, with reads cut short at random, and with the n-th
   read or write failing. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <CommonCrypto/CommonDigest.h>

#include LP_SOURCE
#include OTG_SOURCE
#include "stubs.h"

struct stubs stubs;
struct user_settings global_settings;
volatile long current_tick;

static FILE *trace;
#define T(...) fprintf(trace, __VA_ARGS__)

/* the bytes, escaped, so that the trace is text */
static void t_bytes(const char *what, const void *data, size_t n)
{
    const unsigned char *p = data;
    T("%s[%zu] \"", what, n);
    for (size_t i = 0; i < n; i++)
    {
        if (p[i] >= 0x20 && p[i] < 0x7f && p[i] != '"' && p[i] != '\\')
            fputc(p[i], trace);
        else
            T("\\x%02x", p[i]);
    }
    T("\"\n");
}

static void t_str(const char *what, const char *s)
{
    if (s)
        t_bytes(what, s, strlen(s));
    else
        T("%s NULL\n", what);
}

/* ---- the disk: simfs.c's, its reads and writes through these, which can
   be cut short or fail ---- */

ssize_t simfs_sim_read(int fildes, void *buf, size_t nbyte);
ssize_t simfs_sim_write(int fildes, const void *buf, size_t nbyte);

/* xorshift: the reads' own, seeded for each run, and the cases' */
static uint64_t read_rng = 1, case_rng = 20261116;
static uint32_t next_rng(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return (uint32_t)(*state >> 11);
}
#define rng() next_rng(&case_rng)

static long crlf_splits; /* reads that ended between a '\r' and its '\n' */

static struct {
    bool short_reads;   /* each read up to a random count of what's asked */
    long fail_read;     /* the n-th read fails, from 1; 0 for none */
    long fail_write;
    long reads, writes;
} faults;

ssize_t sim_read(int fildes, void *buf, size_t nbyte)
{
    faults.reads++;
    if (faults.fail_read && faults.reads == faults.fail_read)
    {
        T("  read %ld fails\n", faults.reads);
        return -1;
    }
    if (faults.short_reads && nbyte > 1)
        nbyte = 1 + next_rng(&read_rng) % nbyte;
    const ssize_t n = simfs_sim_read(fildes, buf, nbyte);
    char c;
    if (n > 0 && ((char *)buf)[n - 1] == '\r'
        && pread(fildes, &c, 1, lseek(fildes, 0, SEEK_CUR)) == 1 && c == '\n')
        crlf_splits++;
    return n;
}

ssize_t sim_write(int fildes, const void *buf, size_t nbyte)
{
    faults.writes++;
    if (faults.fail_write && faults.writes == faults.fail_write)
    {
        T("  write %ld fails\n", faults.writes);
        return -1;
    }
    return simfs_sim_write(fildes, buf, nbyte);
}

/* ---- the firmware below: a database of every song but those named
   missing, moved, twice or gone; a sync that moved "twice" to "moved" and
   "moved" to "now"; the queue ---- */

static uint64_t fnv(const void *data, size_t n, uint64_t h)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

/* the paths hashed, for itunesdb_moved_to() to know them by their hashes */
#define HASHED_MAX 64
static struct { uint64_t hash; char path[MAX_PATH]; } hashed[HASHED_MAX];
static int hashed_next;

uint64_t itunesdb_path_hash(const char *path)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char *p = path; *p; p++)
    {
        const unsigned char c = (unsigned char)*p;
        const unsigned char lower = c >= 'A' && c <= 'Z' ? c + 32 : c;
        h = fnv(&lower, 1, h);
    }
    hashed[hashed_next].hash = h;
    snprintf(hashed[hashed_next].path, MAX_PATH, "%s", path);
    hashed_next = (hashed_next + 1) % HASHED_MAX;
    return h;
}

long tagcache_find_entry(const char *path)
{
    const long idx = strstr(path, "missing") || strstr(path, "moved")
                     || strstr(path, "twice") || strstr(path, "gone")
                     ? -1 : (long)(itunesdb_path_hash(path) % 50000);
    T("  find %ld ", idx);
    t_str("", path);
    return idx;
}

/* a sync moved "twice" to "moved", and "moved" to "now" */
bool itunesdb_moved_to(uint64_t hash, char *buf, size_t size)
{
    bool moved = false;
    for (int i = 0; i < HASHED_MAX && !moved; i++)
    {
        const char *path = hashed[i].path;
        const char *m = strstr(path, "twice");
        const char *to = "moved";
        if (!m)
        {
            m = strstr(path, "moved");
            to = "now";
        }
        if (hashed[i].hash == hash && m)
        {
            snprintf(buf, size, "%.*s%s%s", (int)(m - path), path, to, m + 5);
            moved = true;
        }
    }
    T("  moved_to %016llx %d\n", (unsigned long long)hash, moved);
    return moved;
}

bool itunesdb_own_playlist(const char *file)
{
    (void)file;
    return false;
}

int tagcache_get_commitid(void)
{
    return 7;
}

int tagcache_entry_count(void)
{
    return 13000;
}

bool tagcache_finds_entries(void)
{
    return true;
}

bool tagcache_search(struct tagcache_search *tcs, int tag)
{
    (void)tcs;
    T("  tagcache_search %d\n", tag);
    return true;
}

bool tagcache_retrieve(struct tagcache_search *tcs, int idxid, int tag,
                       char *buf, long size)
{
    (void)tcs;
    snprintf(buf, size, "/Music/A/s%d.mp3", idxid);
    T("  retrieve %d %d\n", idxid, tag);
    return true;
}

void tagcache_search_finish(struct tagcache_search *tcs)
{
    (void)tcs;
}

/* a line as a path: whole, or from the playlist's folder; with '\\'s as
   '/'s; "BAD" can't be one */
ssize_t playlist_resolve_path(char *buffer, size_t buf_size,
                              const char *src_file, const char *dir)
{
    if (strstr(src_file, "BAD"))
        return -1;
    char tmp[MAX_PATH * 2];
    if (src_file[0] == '/' || src_file[0] == '\\')
        snprintf(tmp, sizeof(tmp), "%s", src_file);
    else
        snprintf(tmp, sizeof(tmp), "%s/%s", dir, src_file);
    for (char *p = tmp; *p; p++)
        if (*p == '\\')
            *p = '/';
    if (strlen(tmp) >= buf_size)
        return -1;
    strcpy(buffer, tmp);
    return 0;
}

static int audio_now;           /* AUDIO_STATUS_PLAY or 0 */
static const char *playing_name;

int audio_status(void)
{
    return audio_now;
}

struct mp3entry *audio_current_track(void)
{
    return NULL;
}

char *playlist_get_name(const struct playlist_info *playlist, char *buf,
                        int buf_size)
{
    (void)playlist;
    if (!playing_name)
        return NULL;
    snprintf(buf, buf_size, "%s", playing_name);
    return buf;
}

int playlist_insert_track(struct playlist_info *playlist, const char *filename,
                          int position, bool queue, bool sync)
{
    (void)playlist;
    T("  insert %d %d %d ", position, queue, sync);
    t_str("", filename);
    return 0;
}

void playlist_sync(struct playlist_info *playlist)
{
    (void)playlist;
    T("  playlist_sync\n");
}

static char plugin_buf[16384];
static size_t plugin_size;

void *plugin_get_buffer(size_t *buffer_size)
{
    *buffer_size = plugin_size;
    return plugin_buf;
}

void yield(void)
{
    stubs.yields++;
}

void library_write_failed(void)
{
    T("  library_write_failed\n");
}

/* misc.c's, verbatim: the tests don't build misc.c */
int open_utf8(const char* pathname, int flags)
{
    ssize_t ret;
    int fd;
    unsigned char bom[BOM_UTF_8_SIZE];

    fd = open(pathname, flags, 0666);
    if(fd < 0)
        return fd;

    if(flags & (O_TRUNC | O_WRONLY))
    {
        ret = write(fd, BOM_UTF_8, BOM_UTF_8_SIZE);
    }
    else
    {
        ret = read(fd, bom, BOM_UTF_8_SIZE);
        /* check for BOM */
        if (ret == BOM_UTF_8_SIZE)
        {
            if(memcmp(bom, BOM_UTF_8, BOM_UTF_8_SIZE))
                lseek(fd, 0, SEEK_SET);
        }
    }
    /* read or write failure, do not continue */
    if (ret < 0)
        close(fd);

    return ret >= 0 ? fd : -1;
}

/* stubs.c's: a byte at a time */
int read_line(int fd, char *buffer, int buffer_size)
{
    if (!buffer || buffer_size-- <= 0)
        return -1;
    int count = 0, num_read = 0;
    ssize_t n = 0;
    while (count < buffer_size)
    {
        unsigned char c;
        n = read(fd, &c, 1);
        if (n <= 0)
            break;
        num_read++;
        if (c == '\n')
            break;
        if (c == '\r')
            continue;
        buffer[count++] = c;
    }
    buffer[count] = '\0';
    return n >= 0 ? num_read : -1;
}

/* ---- the disk's files, by the host's functions (host.c) ---- */

void host_root(const char *dir);
void host_fresh(void);
void host_put(const char *path, const void *data, size_t n);
void host_each_file(void (*f)(const char *path, const void *data, size_t n));
void host_remove_root(void);

/* a file's bytes; a long one by its size and SHA-256 */
static void t_file(const char *path, const void *data, size_t n)
{
    if (n <= 4096)
    {
        t_bytes(path, data, n);
        return;
    }
    unsigned char sha[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data, (CC_LONG)n, sha);
    T("%s[%zu] sha256 ", path, n);
    for (size_t i = 0; i < sizeof(sha); i++)
        T("%02x", sha[i]);
    T("\n");
}

static void t_files(void)
{
    host_each_file(t_file);
}

/* ---- the playlists ---- */

struct text {
    char *data;
    size_t size, room;
};

static void add(struct text *t, const void *data, size_t n)
{
    if (t->size + n > t->room)
    {
        t->room = (t->size + n) * 2 + 64;
        t->data = realloc(t->data, t->room);
    }
    memcpy(t->data + t->size, data, n);
    t->size += n;
}

static void adds(struct text *t, const char *s)
{
    add(t, s, strlen(s));
}

static void addn(struct text *t, char c, size_t n)
{
    while (n--)
        add(t, &c, 1);
}

/* a song's line: one found, missing, moved, moved twice, gone; whole, from
   the folder, under /Volumes, with '\\'s, in Latin-1 or UTF-8, or that
   can't be a path */
static void song_line(struct text *t, int kind, int n)
{
    char line[256];
    static const char *const forms[] = {
        "/Music/A/s%d.mp3", "/Music/missing%d.mp3", "/Music/A/moved%d.mp3",
        "/Music/twice%d.mp3", "/Music/gone%d.mp3", "B/s%d.mp3",
        "B/moved%d.mp3", "/Volumes/iPod/Music/s%d.mp3", "..\\Music\\s%d.mp3",
        "/Music/BAD%d.mp3", "/Music/caf\xe9%d.mp3", "/Music/caf\xc3\xa9%d.mp3",
        "/Music/./dot%d.mp3",
    };
    snprintf(line, sizeof(line), forms[kind % ARRAYLEN(forms)], n);
    adds(t, line);
}

static const char *const ends[] = { "\n", "\r\n", "\r\r\n" };

/* the corners, each a playlist of its own; how many there are */
#define CORNERS 24
static void corner(int k, struct text *t, size_t block)
{
    switch (k)
    {
    case 0: /* empty */
        break;
    case 1: /* one line, no '\n' */
        adds(t, "/Music/A/s1.mp3");
        break;
    case 2: /* LF */
        for (int i = 0; i < 6; i++)
        {
            song_line(t, i, i);
            adds(t, "\n");
        }
        break;
    case 3: /* CRLF */
        for (int i = 0; i < 6; i++)
        {
            song_line(t, i, i);
            adds(t, "\r\n");
        }
        break;
    case 4: /* CRLF, the last line without */
        for (int i = 0; i < 5; i++)
        {
            song_line(t, i, i);
            adds(t, "\r\n");
        }
        song_line(t, 0, 99);
        break;
    case 5: /* a BOM */
        adds(t, "\xef\xbb\xbf#EXTM3U\n/Music/A/s1.mp3\n/Music/A/moved2.mp3\n");
        break;
    case 6: /* a BOM, then a song at once */
        adds(t, "\xef\xbb\xbf/Music/A/s1.mp3\r\n/Music/gone3.mp3\r\n");
        break;
    case 7: /* notes */
        adds(t, "#EXTM3U\n" ITUNESDB_PLAYLIST_SORT "4\n#EXTINF:12,Title\n"
                "/Music/A/s1.mp3\n#EXTINF:13,Other\n/Music/missing2.mp3\n"
                "# Sorted by 9\n");
        break;
    case 8: /* empty lines and a '\r' alone */
        adds(t, "\n\n/Music/A/s1.mp3\n\r\n\n/Music/gone1.mp3\n\r");
        break;
    case 9: /* only a '\r' */
        adds(t, "\r");
        break;
    case 10: /* only '\n's */
        adds(t, "\n\n\n");
        break;
    case 11: /* a line of size - 2, size - 1 and size bytes, between songs */
    case 12:
    case 13:
        adds(t, "/Music/A/s1.mp3\n");
        addn(t, 'x', block - 2 + (k - 11));
        adds(t, "\n/Music/A/s2.mp3\n/Music/moved3.mp3\n");
        break;
    case 14: /* a line longer than three buffers, then songs */
        adds(t, "/Music/gone4.mp3\r\n/");
        addn(t, 'y', 3 * block + 17);
        adds(t, "/Music/A/s5.mp3\r\n/Music/A/s6.mp3\r\n");
        break;
    case 15: /* a long line at the end, no '\n' */
        adds(t, "/Music/A/s1.mp3\n");
        addn(t, 'z', block + 3);
        break;
    case 16: /* the first block ending inside a CRLF: its '\r' the last */
        adds(t, "/Music/A/s1.mp3\r\n/Music/A/s");
        addn(t, '7', block - 2 - t->size);
        adds(t, "\r\n/Music/A/moved8.mp3\r\n");
        break;
    case 17: /* lines of every length to an eighth of the buffer, CRLF */
        for (size_t n = 0; n < block / 8; n++)
        {
            adds(t, "/");
            addn(t, 'a' + n % 26, n);
            adds(t, "\r\n");
        }
        break;
    case 18: /* a '\0' in a line */
        adds(t, "/Music/A/s1.mp3\n/Music/A/s");
        add(t, "\0", 1);
        adds(t, "2.mp3\n/Music/A/s3.mp3\n/Music/gone4.mp3\n");
        break;
    case 19: /* a '\0' in the last line */
        adds(t, "/Music/A/s1.mp3\n/Music/gone2.mp3\n/Music/A/s");
        add(t, "\0", 1);
        adds(t, "3.mp3");
        break;
    case 20: /* songs of every kind, many, CRLF */
        for (int i = 0; i < 80; i++)
        {
            song_line(t, i, i);
            adds(t, "\r\n");
        }
        break;
    case 21: /* the same, every ending */
        for (int i = 0; i < 80; i++)
        {
            song_line(t, i * 7, i);
            adds(t, ends[i % ARRAYLEN(ends)]);
        }
        break;
    case 22: /* a short file, under the BOM's three bytes */
        adds(t, "a\n");
        break;
    case 23: /* the BOM alone */
        adds(t, "\xef\xbb\xbf");
        break;
    }
}

/* a random playlist of every kind of line */
static void random_playlist(struct text *t, size_t block)
{
    if (rng() % 4 == 0)
        adds(t, "\xef\xbb\xbf");
    const int lines = rng() % 120;
    for (int i = 0; i < lines; i++)
    {
        const uint32_t r = rng() % 100;
        if (r < 60)
        {
            const int kind = rng();
            song_line(t, kind, rng() % 1000);
        }
        else if (r < 70)
            adds(t, rng() % 2 ? "#EXTINF:1,x" : ITUNESDB_PLAYLIST_SORT "3");
        else if (r < 78)
            ; /* an empty line */
        else if (r < 82)
            adds(t, "\r");
        else if (r < 88)
            addn(t, 'w', block - 4 + rng() % 8);
        else if (r < 92)
            addn(t, 'v', rng() % (3 * block));
        else if (r < 94)
        {
            adds(t, "/Music/A/n");
            add(t, "\0", 1);
            adds(t, "ul");
        }
        else
        {
            const int kind = rng();
            song_line(t, kind, rng() % 10);
        }
        if (i < lines - 1 || rng() % 2)
            adds(t, ends[rng() % ARRAYLEN(ends)]);
    }
}

/* ---- the runs ---- */

#define PLAYLIST_DIR "/Playlists"
static char file[MAX_PATH];
static struct lp_song songs[LP_MAX];

static int cmp_u64(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* the songs the library lost: the whole "gone" lines' */
static uint64_t gone_list[4096];
static void note_gone(const struct text *t)
{
    gone_count = 0;
    gone_set = gone_list;
    size_t i = 0;
    while (i < t->size && gone_count < (int)ARRAYLEN(gone_list))
    {
        const char *start = t->data + i;
        const char *nl = memchr(start, '\n', t->size - i);
        size_t len = nl ? (size_t)(nl - start) : t->size - i;
        i += len + 1;
        char line[MAX_PATH];
        if (len >= sizeof(line))
            continue;
        memcpy(line, start, len);
        line[len] = '\0';
        if (len && line[len - 1] == '\r')
            line[len - 1] = '\0';
        if (!strncmp(line, "/Music/gone", 11))
            gone_list[gone_count++] = itunesdb_path_hash(line);
    }
    qsort(gone_list, gone_count, sizeof(*gone_list), cmp_u64);
}

static void faults_start(int mode, uint64_t seed)
{
    memset(&faults, 0, sizeof(faults));
    read_rng = seed | 1;
    if (mode == 1)
        faults.short_reads = true;
    else if (mode >= 2 && mode < 8)
        faults.fail_read = mode - 1;
    else if (mode >= 8)
        faults.fail_write = mode - 7;
    stubs.yields = 0;
}

static void faults_end(void)
{
    T("  reads %ld writes %ld yields %ld\n", faults.reads, faults.writes,
      stubs.yields);
    memset(&faults, 0, sizeof(faults));
}

static void t_state(const struct lp_state *st)
{
    T("  state lines %u unmatched %u sort %u pending %d\n", st->lines,
      st->unmatched, st->sort, st->pending);
}

static void t_songs(int n)
{
    for (int i = 0; i < n; i++)
        T("  song %d idx %u line %u\n", i, songs[i].idx, songs[i].line);
}

/* the index of the playlist as it's put, as at Updating Library */
static int setup_index(const char *path, struct lp_state *st)
{
    static char buf[8192];
    const int n = lp_index_build(path, songs, LP_MAX, buf, sizeof(buf), st);
    if (n >= 0)
        lp_index_write(path, songs, n, st);
    return n;
}

static void run_build(const struct text *t, size_t size, int max, int mode)
{
    static char buf[16384];
    struct lp_state st;
    host_fresh();
    host_put(file, t->data, t->size);
    T(" build size %zu max %d mode %d\n", size, max, mode);
    faults_start(mode, size * 31 + max);
    const int n = lp_index_build(file, songs, max, buf, size, &st);
    faults_end();
    T("  returned %d\n", n);
    t_state(&st);
    t_songs(n);
}

static void run_appended(const struct text *t, size_t cut, bool indexed,
                         bool playing, int mode)
{
    struct lp_state st;
    host_fresh();
    host_put(file, t->data, cut);
    T(" appended cut %zu indexed %d playing %d mode %d\n", cut, indexed,
      playing, mode);
    if (indexed)
        T("  setup %d\n", setup_index(file, &st));
    host_put(file, t->data, t->size);
    audio_now = playing ? AUDIO_STATUS_PLAY : 0;
    playing_name = playing ? file : NULL;
    faults_start(mode, cut * 7 + indexed);
    lp_index_appended(file, cut);
    faults_end();
    audio_now = 0;
    playing_name = NULL;
    t_files();
}

static void run_clean(const struct text *t, size_t size, int mode)
{
    static char buf[16384];
    host_fresh();
    host_put(file, t->data, t->size);
    note_gone(t);
    T(" clean size %zu mode %d\n", size, mode);
    faults_start(mode, size * 13);
    const bool changed = clean_playlist(file, buf, size);
    faults_end();
    gone_set = NULL;
    gone_count = 0;
    T("  returned %d\n", changed);
    t_files();
}

static char pool[32768];

static void run_copy(const struct text *t, size_t room, bool second, int mode)
{
    host_fresh();
    host_put(file, t->data, t->size);
    lp.pool = pool;
    lp.pool_used = 100;
    lp.pool_size = lp.pool_used + room;
    playing_name = second ? LP_PLAYING : NULL;
    T(" copy room %zu second %d mode %d\n", room, second, mode);
    faults_start(mode, room * 3 + second);
    const char *copy = make_copy(file);
    faults_end();
    playing_name = NULL;
    t_str("  returned", copy);
    t_files();
}

static void run_remove(const struct text *t, int which, size_t room, bool otg,
                       int mode)
{
    struct lp_state st;
    host_fresh();
    const char *path = otg ? OTG_FILE : file;
    host_put(path, t->data, t->size);
    const int n = setup_index(path, &st);
    if (n <= 0)
        return;
    const int song = which == 0 ? 0 : which == 1 ? n / 2 : n - 1;
    snprintf(lp.file, sizeof(lp.file), "%s", path);
    lp.songs = songs;
    lp.total = n;
    lp.count = 0;
    lp.state = st;
    lp.otg = otg;
    lp.pool = pool;
    lp.pool_used = 64;
    lp.pool_size = lp.pool_used + room;
    T(" remove song %d of %d room %zu otg %d mode %d\n", song, n, room, otg,
      mode);
    faults_start(mode, room * 5 + song);
    const bool ok = lp_remove(song);
    faults_end();
    T("  returned %d total %d lines %u\n", ok, lp.total, lp.state.lines);
    t_songs(lp.total);
    t_files();
}

static void run_append(const struct text *t, size_t size, int target_kind,
                       int mode)
{
    host_fresh();
    host_put(file, t->data, t->size);
    const char *target = target_kind == 0 ? OTG_FILE
                                          : PLAYLIST_DIR "/target.m3u8";
    if (target_kind == 1)
        host_put(target, "/Music/A/old.mp3\n", 17);
    else if (target_kind == 2)
        host_put(target, "/Music/A/old.mp3", 16);
    plugin_size = size;
    T(" append size %zu target %d mode %d\n", size, target_kind, mode);
    faults_start(mode, size * 11 + target_kind);
    const bool ok = playlist_file_append_playlist(target, file);
    faults_end();
    T("  returned %d\n", ok);
    t_files();
}

/* every function on one playlist; all the faults if all, else a few */
static void run_all(const struct text *t, const char *name, bool all)
{
    snprintf(file, sizeof(file), PLAYLIST_DIR "/%s", name);
    T("case %s\n", name);
    t_bytes(" playlist", t->data, t->size);
    const int modes = all ? 12 : 4;
    for (int m = 0; m < modes; m++)
    {
        int mode = m;
        if (!all)
            mode = m == 2 ? 2 + rng() % 6 : m == 3 ? 8 + rng() % 4 : m;
        static const size_t sizes[] = { 2 * MAX_PATH, 2 * MAX_PATH + 1,
                                        777, 4096 };
        for (size_t s = 0; s < ARRAYLEN(sizes); s++)
        {
            run_build(t, sizes[s], LP_MAX, mode);
            run_clean(t, sizes[s], mode);
            run_copy(t, 2 * sizes[s] + s % 2, s % 2, mode);
            run_append(t, 2 * sizes[s] + s % 2, s % 3, mode);
        }
        run_build(t, 2 * MAX_PATH, 3, mode);
        for (int which = 0; which < 3; which++)
            run_remove(t, which, 4 * MAX_PATH + which, which == 2, mode);
        run_remove(t, 1, 8192, false, mode);
        const size_t cuts[] = { 0, t->size / 3, t->size / 2, t->size };
        for (size_t c = 0; c < ARRAYLEN(cuts); c++)
            run_appended(t, cuts[c], c != 0, c % 2, mode);
    }
}

/* a reader that never ends a file is a failure, not a hang */
static void too_long(int sig)
{
    (void)sig;
    fputs("test_lines: still running after 300 s\n", stderr);
    _exit(3);
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <folder>\n", argv[0]);
        return 2;
    }
    signal(SIGALRM, too_long);
    alarm(300);
    host_root(argv[1]);
    simfs_root(argv[1]);
    trace = stdout;
    static const char *const names[] = { "corner.m3u8", "corner.m3u" };
    static const size_t blocks[] = { 2 * MAX_PATH, 777, 4096 };
    for (int k = 0; k < CORNERS; k++)
    {
        /* the corners of size are of each buffer's */
        const int sized = (k >= 11 && k <= 17) ? ARRAYLEN(blocks) : 1;
        for (int b = 0; b < sized; b++)
        {
            struct text t = { 0 };
            corner(k, &t, blocks[b]);
            for (int n = 0; n < 2; n++)
            {
                char name[64];
                snprintf(name, sizeof(name), "c%02d-%zu-%s", k, blocks[b],
                         names[n]);
                run_all(&t, name, true);
            }
            free(t.data);
        }
    }
    for (int r = 0; r < 150; r++)
    {
        struct text t = { 0 };
        random_playlist(&t, blocks[r % ARRAYLEN(blocks)]);
        char name[64];
        snprintf(name, sizeof(name), "r%03d.%s", r, r % 3 ? "m3u8" : "m3u");
        run_all(&t, name, false);
        free(t.data);
    }
    fprintf(stderr, "lines: reads that ended inside a CRLF: %ld\n",
            crlf_splits);
    host_remove_root();
    return 0;
}
