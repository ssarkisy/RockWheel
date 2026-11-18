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

/* iTunesDB and ArtworkDB read as after a sync: the tracks as db_load() has
   them, the whole import (itunesdb_import_tracks()) with its playlists, its
   index of covers and its records of the tracks' tags, then the iPod's name.
   Each case of make_itunesdb.py's is an iPod's folder, read in a process of
   its own, so that each starts as at boot and a crash or a hang is that
   case's. A case gives what its "expect" says, and every one, whole or
   damaged, without a sanitizer error and within its time. */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
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
#include "applimits.h"
#include "rbpaths.h"
#include "tagcache.h"
#include <ctype.h>
#include <errno.h>
#include "logf.h"
#include "lcd.h"
#include "metadata.h"
#include "fixedpoint.h"
#include "itunesdb.h"
#include "synced_covers.h"
#include "health_log.h"
#include "crc32.h"
#include "sortkey.h"
#include "skin_engine/cover_palette.h"
#include "skin_engine/skin_engine.h"
#include "album_header.h"
#include "list_covers.h"
#include "skin_engine/menu_cover.h"
#include "smart_playlists.h"
#include "library_playlists.h"
#include "check.h"
#include "stubs.h"

/* The iPod's file offsets have 32 bits (firmware/libc/include/sys/types.h),
   the Mac's 64: itunesdb.c's own arithmetic on them wraps here as it does
   there, and a length that leads back to an earlier record does so here too.
   The headers above are read with the Mac's. */
#define off_t int32_t
#include "itunesdb.c"
#undef off_t

/* the Mac's folders from here on, for the cases' list */
#undef dirent
#undef opendir
#undef readdir
#undef closedir
#undef mkdir

/* The most a case may take, of CPU time (check_time_limit()): a loop spends
   it, and the load of other processes doesn't. Past it the case prints where
   it was and ends; the parent kills one still there at twice the time, which
   is then a timeout, or a check's failure if one was logged before. */
#define CASE_CPU_SECONDS 5
#define TIMED_OUT        CHECK_TIMED_OUT /* its exit status then */
/* cases read at once */
#define JOBS 8

/* how a case failed, as "known" names it */
enum outcome { PASSED, CHECK_FAILED, TIMEOUT, CRASHED, NOT_RUN };
static const char *const outcome_names[] = {
    "passed", "check", "timeout", "crash", "not run"
};

#define WANT_PATHS 4
#define WANT_PLAYLISTS 4
#define WANT_META 4
/* a "meta" line's fields: the path, title, artist, album, album artist,
   year, plays and rating */
#define META_FIELDS 8
/* the expectations of a case, -1 or "" for none */
static struct {
    int tracks, loaded, complete, paths, covers, shows;
    int absent[4], nabsent;
    char name[256];
    struct {
        char file[MAX_PATH];
        char paths[WANT_PATHS][MAX_PATH];
        int count;
    } playlists[WANT_PLAYLISTS];
    int nplaylists;
    char unwritten[WANT_PLAYLISTS][MAX_PATH];
    int nunwritten;
    char meta[WANT_META][META_FIELDS][MAX_PATH];
    int nmeta;
    char notes_path[MAX_PATH], notes[512];
    /* the database in memory at the import; the tracks with a record of
       their tags or none, and their digests for the next import, "0" for
       not known, "1" for known (TAGS_PENDING) */
    int ram;
    char record[WANT_PATHS][MAX_PATH], norecord[WANT_PATHS][MAX_PATH];
    int nrecord, nnorecord;
    char digest[WANT_PATHS][MAX_PATH];
    int digest_known[WANT_PATHS], ndigest;
    int known;          /* the outcome of a failure left as it is, or 0 */
    char why[512];
} want;

static void read_expect(const char *dir)
{
    char path[MAX_PATH * 2], line[2048];
    memset(&want, 0, sizeof(want));
    want.tracks = want.loaded = want.complete = want.paths = want.covers
        = want.shows = -1;
    snprintf(path, sizeof(path), "%s/expect", dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\n")] = '\0';
        char *arg = strchr(line, ' ');
        if (arg)
            *arg++ = '\0';
        else
            arg = line + strlen(line);
        if (!strcmp(line, "tracks"))
            want.tracks = atoi(arg);
        else if (!strcmp(line, "loaded"))
            want.loaded = atoi(arg);
        else if (!strcmp(line, "complete"))
            want.complete = atoi(arg);
        else if (!strcmp(line, "paths"))
            want.paths = atoi(arg);
        else if (!strcmp(line, "covers"))
            want.covers = atoi(arg);
        else if (!strcmp(line, "shows"))
            want.shows = atoi(arg);
        else if (!strcmp(line, "absent") && want.nabsent < 4)
            want.absent[want.nabsent++] = atoi(arg);
        else if (!strcmp(line, "name"))
            snprintf(want.name, sizeof(want.name), "%s", arg);
        else if (!strcmp(line, "known"))
        {
            /* "<outcome> <why>" */
            char *why = strchr(arg, ' ');
            if (why)
                *why++ = '\0';
            for (int i = CHECK_FAILED; i < NOT_RUN; i++)
            {
                if (!strcmp(arg, outcome_names[i]))
                    want.known = i;
            }
            snprintf(want.why, sizeof(want.why), "%s", why ? why : "");
        }
        else if (!strcmp(line, "playlist") && want.nplaylists < WANT_PLAYLISTS)
        {
            /* "<file>\t<song>\t<song>…", the songs in their order */
            typeof(want.playlists[0]) *pl = &want.playlists[want.nplaylists++];
            char *field = strtok(arg, "\t");
            snprintf(pl->file, MAX_PATH, "%s", field ? field : "");
            while ((field = strtok(NULL, "\t")) && pl->count < WANT_PATHS)
                snprintf(pl->paths[pl->count++], MAX_PATH, "%s", field);
        }
        else if (!strcmp(line, "notes"))
        {
            /* "<path>\t<text>" */
            char *text = strchr(arg, '\t');
            if (text)
                *text++ = '\0';
            snprintf(want.notes_path, sizeof(want.notes_path), "%s", arg);
            snprintf(want.notes, sizeof(want.notes), "%s", text ? text : "");
        }
        else if (!strcmp(line, "unwritten") && want.nunwritten < WANT_PLAYLISTS)
            snprintf(want.unwritten[want.nunwritten++], MAX_PATH, "%s", arg);
        else if (!strcmp(line, "ram"))
            want.ram = atoi(arg);
        else if (!strcmp(line, "record") && want.nrecord < WANT_PATHS)
            snprintf(want.record[want.nrecord++], MAX_PATH, "%s", arg);
        else if (!strcmp(line, "norecord") && want.nnorecord < WANT_PATHS)
            snprintf(want.norecord[want.nnorecord++], MAX_PATH, "%s", arg);
        else if (!strcmp(line, "digest") && want.ndigest < WANT_PATHS)
        {
            /* "<path> <0|1>" */
            char *known = strrchr(arg, ' ');
            if (known)
                *known++ = '\0';
            want.digest_known[want.ndigest] = known ? atoi(known) : -1;
            snprintf(want.digest[want.ndigest++], MAX_PATH, "%s", arg);
        }
        else if (!strcmp(line, "meta") && want.nmeta < WANT_META)
        {
            /* "<path>\t<title>\t…", a field "" where a tag is none */
            char *rest = arg;
            for (int k = 0; k < META_FIELDS; k++)
            {
                const char *field = rest ? strsep(&rest, "\t") : "";
                snprintf(want.meta[want.nmeta][k], MAX_PATH, "%s", field);
            }
            want.nmeta++;
        }
    }
    fclose(f);
}

/* the digest the import wrote for the track at path for the next one
   (write_tags_pending()), against "digest"'s: -1 for none */
static void check_digest(int n)
{
    const uint64_t hash = itunesdb_path_hash(want.digest[n]);
    int got = -1;
    const int fd = open(TAGS_PENDING, O_RDONLY);
    struct tags_header hdr;
    struct tags_entry e;
    if (fd >= 0 && read(fd, &hdr, sizeof(hdr)) == sizeof(hdr)
        && hdr.magic == TAGS_MAGIC && hdr.version == TAGS_VERSION)
    {
        while (read(fd, &e, sizeof(e)) == sizeof(e))
        {
            if (e.hash == hash)
                got = e.digest != 0;
        }
    }
    if (fd >= 0)
        close(fd);
    CHECK_EQ(got, want.digest_known[n]);
    if (got != want.digest_known[n])
        fprintf(stderr, "digest of %s: %d, not %d\n", want.digest[n], got,
                want.digest_known[n]);
}

/* the songs of a playlist written, in its order, against those expected */
static void check_playlist(int n)
{
    char path[MAX_PATH * 2], line[MAX_PATH];
    FILE *f = fopen(simfs_path(want.playlists[n].file, path, sizeof(path)), "r");
    CHECK(f != NULL);
    if (!f)
        return;
    int songs = 0;
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '#' || line[0] == '\0')
            continue;
        if (songs < want.playlists[n].count)
            CHECK(!strcmp(line, want.playlists[n].paths[songs]));
        songs++;
    }
    fclose(f);
    CHECK_EQ(songs, want.playlists[n].count);
}

/* no playlist written as this file */
static void check_unwritten(int n)
{
    char path[MAX_PATH * 2];
    FILE *f = fopen(simfs_path(want.unwritten[n], path, sizeof(path)), "r");
    CHECK(f == NULL);
    if (f)
        fclose(f);
}

/* what the import's records give for the track at path, as a "meta" line's
   fields; false for none */
static bool track_meta(const char *path, char fields[META_FIELDS][MAX_PATH])
{
    static struct mp3entry id3;
    struct tagcache_runtime rt;
    long date, added;
    memset(&rt, 0, sizeof(rt));
    if (!itunesdb_get_metadata(path, &id3, &rt, &date, &added))
        return false;
    const char *const tags[] = { id3.title, id3.artist, id3.album,
                                 id3.albumartist };
    snprintf(fields[0], MAX_PATH, "%s", path);
    for (int k = 0; k < 4; k++)
        snprintf(fields[1 + k], MAX_PATH, "%s", tags[k] ? tags[k] : "");
    snprintf(fields[5], MAX_PATH, "%d", (int)id3.year);
    snprintf(fields[6], MAX_PATH, "%ld", (long)rt.playcount);
    snprintf(fields[7], MAX_PATH, "%ld", (long)rt.rating);
    return true;
}

static void check_meta(int n)
{
    static char got[META_FIELDS][MAX_PATH];
    const bool found = track_meta(want.meta[n][0], got);
    CHECK(found);
    for (int k = 1; found && k < META_FIELDS; k++)
    {
        CHECK(!strcmp(got[k], want.meta[n][k]));
        if (strcmp(got[k], want.meta[n][k]))
            fprintf(stderr, "    %s, field %d: \"%s\", not \"%s\"\n",
                    want.meta[n][0], k, got[k], want.meta[n][k]);
    }
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static struct track *track_by_id(uint32_t id)
{
    for (int i = 0; i < db.count; i++)
    {
        if (db.tracks[i].id == id)
            return &db.tracks[i];
    }
    return NULL;
}

/* the fields of make_itunesdb.py's tracks, as read_track() has them */
static void check_whole_tracks(void)
{
    struct track *a = track_by_id(101), *b = track_by_id(102);
    struct track *c = track_by_id(103), *d = track_by_id(104);
    CHECK(a && b && c && d);
    if (!a || !b || !c || !d)
        return;
    CHECK(!strcmp(db.pool + a->path, "/iPod_Control/Music/F01/AAAA.mp3"));
    CHECK_EQ(a->st.hash, itunesdb_path_hash("/iPod_Control/Music/F01/AAAA.mp3"));
    CHECK_EQ(a->st.rating, 80);
    CHECK_EQ(a->st.playcount, 3);
    CHECK_EQ(b->st.playcount, 12);
    /* the year alone, as the date is of another year */
    CHECK_EQ(a->released, 1969);
    CHECK_EQ(a->dbid, 0x1000 + 101);
    CHECK_EQ(a->art, 201);
    CHECK_EQ(c->art, 202);
    /* one album, by its name and the album's artist; another */
    CHECK(a->album != 0 && a->album == b->album);
    CHECK(c->album != 0 && c->album != a->album);
    /* a podcast: its place and its mark from the Mac */
    CHECK(d->st.flags & ITUNESDB_PODCAST);
    CHECK(d->st.flags & ITUNESDB_REMEMBERS_PLACE);
    CHECK(d->st.flags & ITUNESDB_SKIP_SHUFFLE);
    CHECK_EQ(d->st.played, 2);
    CHECK_EQ(d->bookmark, 61000);
}

/* The flags the import wrote of make_itunesdb.py's tracks: 103 part of a
   compilation (mhit+30), the others not. Then the file as version 3 left it,
   without the compilations: its flags and marks read meanwhile, the import
   due, and that import's file of this version with the compilation and the
   mark the iPod gave the podcast meanwhile */
static void check_whole_flags(void)
{
    static const char single[] = "/iPod_Control/Music/F02/AAAC.m4a";
    static const char song[] = "/iPod_Control/Music/F01/AAAA.mp3";
    static const char episode[] = "/iPod_Control/Music/F03/AAAD.mp3";
    CHECK(itunesdb_track_flags(single) & ITUNESDB_COMPILATION);
    CHECK(!(itunesdb_track_flags(song) & ITUNESDB_COMPILATION));
    CHECK(!(itunesdb_track_flags(episode) & ITUNESDB_COMPILATION));
    CHECK(itunesdb_track_flags(episode) & ITUNESDB_PODCAST);
    CHECK(itunesdb_flags_ready());

    struct flags_header h;
    struct flags_entry e[8];
    int fd = open(FLAGS_FILE, O_RDWR);
    CHECK(fd >= 0);
    if (fd < 0)
        return;
    const ssize_t got = read(fd, &h, sizeof(h));
    CHECK(got == sizeof(h) && h.version == FLAGS_VERSION && h.count >= 2
          && h.count <= ARRAYLEN(e));
    const ssize_t size = (ssize_t)MIN(h.count, ARRAYLEN(e)) * sizeof(*e);
    CHECK(read(fd, e, size) == size);
    h.version = FLAGS_VERSION_3;
    for (uint32_t i = 0; i < h.count && i < ARRAYLEN(e); i++)
        e[i].flags &= ~ITUNESDB_COMPILATION;
    lseek(fd, 0, SEEK_SET);
    CHECK(write(fd, &h, sizeof(h)) == sizeof(h) && write(fd, e, size) == size);
    close(fd);
    forget_index(&flags_index);

    CHECK(!itunesdb_flags_ready());
    CHECK(!itunesdb_load_tracks());
    CHECK(itunesdb_track_flags(episode) & ITUNESDB_PODCAST);
    CHECK(!(itunesdb_track_flags(single) & ITUNESDB_COMPILATION));
    const bool unplayed = itunesdb_track_flags(episode) & ITUNESDB_UNPLAYED;
    CHECK(itunesdb_set_unplayed(episode, !unplayed));

    volatile int done = 0, total = 0;
    itunesdb_import_tracks(false, &done, &total);
    CHECK(itunesdb_flags_ready());
    CHECK(itunesdb_track_flags(single) & ITUNESDB_COMPILATION);
    CHECK(itunesdb_track_flags(episode) & ITUNESDB_PODCAST);
    CHECK(!(itunesdb_track_flags(episode) & ITUNESDB_UNPLAYED) == unplayed);
}

static int run_case(const char *dir, const char *name)
{
    char log[MAX_PATH * 2];
    snprintf(log, sizeof(log), "%s/log", dir);
    if (!freopen(log, "w", stderr))
        return 2;
    setvbuf(stderr, NULL, _IONBF, 0);
    check_time_limit(CASE_CPU_SECONDS);
    check_case = name;
    simfs_root(dir);
    read_expect(dir);
    core_allocator_init();

    /* the tracks as read for the import */
    const bool loaded = db_load();
    if (want.loaded >= 0)
        CHECK_EQ(loaded, want.loaded);
    if (want.tracks >= 0)
        CHECK_EQ(loaded ? db.count : 0, want.tracks);
    if (want.complete >= 0)
        CHECK_EQ(db.complete, want.complete);
    if (want.shows >= 0)
        CHECK_EQ(db.shows, want.shows);
    if (!strcmp(name, "whole"))
        check_whole_tracks();
    int paths = 0;
    for (int i = 0; loaded && i < db.count; i++)
    {
        /* every path within the pool, ended */
        if (db.tracks[i].path)
        {
            paths++;
            CHECK(db.tracks[i].path < db.pool_used
                  && memchr(db.pool + db.tracks[i].path, '\0',
                            db.pool_used - db.tracks[i].path));
        }
    }
    if (want.paths >= 0)
        CHECK_EQ(paths, want.paths);
    for (int i = 0; i < want.nabsent; i++)
        CHECK(!loaded || !track_by_id(want.absent[i]));
    /* no track read twice: each id is once in the cases' files, and a record
       read again, as by a loop, would give one twice. Their paths, for the
       import's records after it. */
    const int tracks_read = loaded ? db.count : 0;
    uint32_t *ids = calloc(tracks_read + 1, sizeof(*ids));
    char **read_paths = calloc(tracks_read + 1, sizeof(*read_paths));
    for (int i = 0; i < tracks_read; i++)
    {
        ids[i] = db.tracks[i].id;
        read_paths[i] = db.tracks[i].path ? strdup(db.pool + db.tracks[i].path)
                                          : NULL;
    }
    qsort(ids, tracks_read, sizeof(*ids), cmp_u32);
    for (int i = 1; i < tracks_read; i++)
        CHECK(ids[i] == 0 || ids[i] != ids[i - 1]);
    free(ids);
    /* nor an image of ArtworkDB twice, read as write_art_index() reads them,
       sorted by id */
    if (loaded)
    {
        struct art_image *images = NULL;
        int nimages = 0;
        memset(&art_hdr, 0, sizeof(art_hdr));
        memset(&large_hdr, 0, sizeof(large_hdr));
        ithmb_count = 0;
        const int handle = read_images(&images, &nimages);
        for (int i = 1; handle > 0 && i < nimages; i++)
            CHECK(images[i].id != images[i - 1].id);
        if (handle > 0)
        {
            core_unpin(handle);
            core_free(handle);
        }
    }
    /* with TESTS_ACTUAL set, what was read, for writing expectations: in the
       case's folder as "actual" */
    FILE *actual = NULL;
    if (getenv("TESTS_ACTUAL"))
    {
        char file[MAX_PATH * 2];
        snprintf(file, sizeof(file), "%s/actual", dir);
        actual = fopen(file, "w");
    }
    if (actual)
    {
        fprintf(actual, "loaded %d\ntracks %d\ncomplete %d\npaths %d\nids",
                loaded, loaded ? db.count : 0, db.complete, paths);
        for (int i = 0; loaded && i < db.count; i++)
            fprintf(actual, " %lu", (unsigned long)db.tracks[i].id);
        fprintf(actual, "\n");
        fflush(actual);
    }
    db_unload();

    /* then the whole import of a sync, which reads it all again */
    volatile int done = 0, total = 0;
    stub_ram_loaded = want.ram > 0;
    const int count = itunesdb_import_tracks(true, &done, &total);
    for (int i = 0; i < want.nrecord; i++)
    {
        CHECK(itunesdb_has_record(want.record[i]));
        if (!itunesdb_has_record(want.record[i]))
            fprintf(stderr, "no record of %s\n", want.record[i]);
    }
    for (int i = 0; i < want.nnorecord; i++)
    {
        CHECK(!itunesdb_has_record(want.norecord[i]));
        if (itunesdb_has_record(want.norecord[i]))
            fprintf(stderr, "a record of %s\n", want.norecord[i]);
    }
    for (int i = 0; i < want.ndigest; i++)
        check_digest(i);
    if (want.tracks >= 0)
        CHECK_EQ(count, want.tracks);
    if (want.covers >= 0)
        CHECK_EQ(itunesdb_art_count(), want.covers);
    for (int i = 0; i < want.nplaylists; i++)
        check_playlist(i);
    for (int i = 0; i < want.nunwritten; i++)
        check_unwritten(i);
    /* the import's records of the tracks' tags: those expected, and every
       track's asked for, read where they are */
    for (int i = 0; i < want.nmeta; i++)
        check_meta(i);
    /* a podcast's notes, as Now Playing reads them */
    if (want.notes_path[0])
    {
        char text[1024];
        const int len = itunesdb_read_notes(want.notes_path, text,
                                            sizeof(text) - 1);
        text[len] = '\0';
        CHECK(!strcmp(text, want.notes));
        if (strcmp(text, want.notes))
            fprintf(stderr, "    notes \"%s\", not \"%s\"\n", text, want.notes);
    }
    for (int i = 0; i < tracks_read; i++)
    {
        static char got[META_FIELDS][MAX_PATH];
        if (read_paths[i] && track_meta(read_paths[i], got) && actual)
        {
            fprintf(actual, "meta");
            for (int k = 0; k < META_FIELDS; k++)
                fprintf(actual, "%c%s", k ? '\t' : ' ', got[k]);
            fprintf(actual, "\n");
        }
        free(read_paths[i]);
    }
    free(read_paths);
    /* after the records, which the import it makes forgets */
    if (!strcmp(name, "whole"))
        check_whole_flags();

    /* and the name, as the main menu shows it: last, as a loop there would
       leave the rest unread */
    if (actual)
    {
        fprintf(actual, "import %d\ncovers %lu\n", count,
                (unsigned long)itunesdb_art_count());
        fflush(actual);
    }
    const char *device = itunesdb_device_name();
    CHECK(device != NULL && strlen(device) < sizeof(device_name));
    if (want.name[0])
        CHECK(!strcmp(device, want.name));
    if (actual)
    {
        fprintf(actual, "name %s\n", device);
        fclose(actual);
    }
    return check_failed ? 1 : 0;
}

/* ---- itunesdb_files_boot(): what a cut left of the kept files in
   .rockbox, as the boot puts it right (fw_recover_files()) ---- */

static char boot_dir[MAX_PATH * 2];

static void boot_put(const char *path, const void *data, size_t n)
{
    char host[MAX_PATH * 3];
    snprintf(host, sizeof(host), "%s%s", boot_dir, path);
    FILE *f = fopen(host, "wb");
    if (!f || fwrite(data, 1, n, f) != n)
    {
        perror(host);
        exit(2);
    }
    fclose(f);
}

/* the file's bytes as data, -1 if there's none, 0 if others */
static int boot_has(const char *path, const void *data, size_t n)
{
    char host[MAX_PATH * 3], got[256];
    snprintf(host, sizeof(host), "%s%s", boot_dir, path);
    FILE *f = fopen(host, "rb");
    if (!f)
        return -1;
    const size_t r = fread(got, 1, sizeof(got), f);
    fclose(f);
    return r == n && !memcmp(got, data, n);
}

static void boot_clear(void)
{
    static const char *const names[] = {
        STATE_FILE, PENDING_FILE, UNSENT_FILE, FLAGS_FILE, NOTES_FILE,
        IDS_FILE, ITUNESDB_MOVED_LIST, OTG_FILE
    };
    static const char *const suffixes[] = { "", ".tmp", ".new", ".old" };
    for (size_t i = 0; i < ARRAYLEN(names); i++)
        for (size_t k = 0; k < ARRAYLEN(suffixes); k++)
        {
            char host[MAX_PATH * 3];
            snprintf(host, sizeof(host), "%s%s%s", boot_dir, names[i],
                     suffixes[k]);
            unlink(host);
        }
}

/* Each kept file's ".new" is whole, fw_close()'s, and takes the name; only
   the marks' had an older firmware's of its own, tested by its size */
static void files_boot_checks(const char *root)
{
    snprintf(boot_dir, sizeof(boot_dir), "%s-boot", root);
    mkdir(boot_dir, 0777);
    char host[MAX_PATH * 3];
    snprintf(host, sizeof(host), "%s" ROCKBOX_DIR, boot_dir);
    mkdir(host, 0777);
    simfs_root(boot_dir);
    boot_clear();

    /* a cut inside the rename of the name to .old: the .new alone */
    check_case = "files boot: On-The-Go's .new alone";
    boot_put(OTG_FILE ".new", "/a.mp3\n/b.mp3\n", 14);
    /* a cut after the name went to .old */
    boot_put(UNSENT_FILE ".new", "new unsent", 10);
    boot_put(UNSENT_FILE ".old", "old unsent", 10);
    /* a cut after the .new was whole, the name still the old one's */
    boot_put(STATE_FILE, "old state", 9);
    boot_put(STATE_FILE ".new", "new state", 9);
    boot_put(IDS_FILE ".new", "new ids", 7);
    /* the marks: an older firmware's save cut short beside them */
    /* the header, then its two entries, as the file has them */
    const struct flags_header h = { FLAGS_MAGIC, FLAGS_VERSION, 2, 1, 2 };
    struct flags_entry e[2] = { { 11, 1, 0 }, { 22, 1, 0 } };
    unsigned char flags[sizeof(h) + sizeof(e)];
    memcpy(flags, &h, sizeof(h));
    memcpy(flags + sizeof(h), e, sizeof(e));
    boot_put(FLAGS_FILE, flags, sizeof(flags));
    boot_put(FLAGS_FILE ".new", flags, sizeof(flags) - 8);
    itunesdb_files_boot();
    check_case = "files boot: On-The-Go's .new alone";
    CHECK_EQ(boot_has(OTG_FILE, "/a.mp3\n/b.mp3\n", 14), 1);
    check_case = "files boot: the unsent plays' .new beside .old";
    CHECK_EQ(boot_has(UNSENT_FILE, "new unsent", 10), 1);
    check_case = "files boot: itunesdb.dat's .new beside it";
    CHECK_EQ(boot_has(STATE_FILE, "new state", 9), 1);
    check_case = "files boot: the ids' .new alone";
    CHECK_EQ(boot_has(IDS_FILE, "new ids", 7), 1);
    check_case = "files boot: the marks' .new cut short";
    CHECK_EQ(boot_has(FLAGS_FILE, flags, sizeof(flags)), 1);
    check_case = "files boot: nothing left";
    static const char *const left[] = {
        OTG_FILE ".new", UNSENT_FILE ".new", UNSENT_FILE ".old",
        STATE_FILE ".new", STATE_FILE ".old", IDS_FILE ".new",
        FLAGS_FILE ".new", FLAGS_FILE ".old"
    };
    for (size_t i = 0; i < ARRAYLEN(left); i++)
        CHECK_EQ(boot_has(left[i], "", 0), -1);

    /* the marks' .new whole, alone: raised */
    boot_clear();
    e[1].hash = 33;
    memcpy(flags + sizeof(h), e, sizeof(e));
    boot_put(FLAGS_FILE ".new", flags, sizeof(flags));
    itunesdb_files_boot();
    check_case = "files boot: the marks' .new whole, alone";
    CHECK_EQ(boot_has(FLAGS_FILE, flags, sizeof(flags)), 1);
    CHECK_EQ(boot_has(FLAGS_FILE ".new", "", 0), -1);
    check_case = NULL;
    boot_clear();
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* a check the case failed, in its log */
static bool logged_failure(const char *dir)
{
    char log[MAX_PATH * 2], line[1024];
    snprintf(log, sizeof(log), "%s/log", dir);
    FILE *f = fopen(log, "r");
    bool failed = false;
    while (f && !failed && fgets(line, sizeof(line), f))
        failed = strstr(line, "] failed:") != NULL;
    if (f)
        fclose(f);
    return failed;
}

static enum outcome outcome_of(int st, bool killed, const char *dir)
{
    /* a check failed before a loop is that failure, not the loop's */
    if (killed)
        return logged_failure(dir) ? CHECK_FAILED : TIMEOUT;
    if (WIFEXITED(st))
        return WEXITSTATUS(st) == 0 ? PASSED
             : WEXITSTATUS(st) == 1 ? CHECK_FAILED
             : WEXITSTATUS(st) == TIMED_OUT ? TIMEOUT : CRASHED;
    if (WIFSIGNALED(st))
    {
        /* the time limit's, or the alarm of its handler */
        const int sig = WTERMSIG(st);
        if (sig == SIGALRM || sig == SIGPROF || sig == SIGKILL)
            return logged_failure(dir) ? CHECK_FAILED : TIMEOUT;
        return CRASHED;
    }
    return NOT_RUN;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <cases folder> (make_itunesdb.py's)\n", argv[0]);
        return 2;
    }
    const char *root = argv[1];

    files_boot_checks(root);
    if (check_failed)
        return 1;

    /* as many cases as make_itunesdb.py says it made */
    char path[MAX_PATH * 2];
    snprintf(path, sizeof(path), "%s/count", root);
    FILE *f = fopen(path, "r");
    int made = -1;
    if (!f || fscanf(f, "%d", &made) != 1 || made <= 0)
    {
        fprintf(stderr, "%s: no count of the cases\n", path);
        return 2;
    }
    fclose(f);

    DIR *d = opendir(root);
    if (!d)
    {
        perror(root);
        return 2;
    }
    char **names = NULL;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.' || e->d_type != DT_DIR)
            continue;
        names = realloc(names, (n + 1) * sizeof(*names));
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n != made)
    {
        printf("FAIL %d cases found, %d made\n", n, made);
        return 1;
    }
    qsort(names, n, sizeof(*names), cmp_names);

    pid_t *pids = calloc(n, sizeof(*pids));
    double *started = calloc(n, sizeof(*started));
    bool *killed = calloc(n, sizeof(*killed));
    enum outcome *result = calloc(n, sizeof(*result));
    for (int i = 0; i < n; i++)
        result[i] = NOT_RUN;
    int next = 0, running = 0;
    while (next < n || running > 0)
    {
        if (next < n && running < JOBS)
        {
            char dir[MAX_PATH * 2];
            snprintf(dir, sizeof(dir), "%s/%s", root, names[next]);
            fflush(NULL);
            const pid_t pid = fork();
            if (pid == 0)
                _exit(run_case(dir, names[next]));
            if (pid > 0)
            {
                pids[next] = pid;
                started[next++] = now();
                running++;
                continue;
            }
            /* no process for it now: after one ends, or never */
            perror("fork");
            if (running == 0)
                next++;
        }
        int st;
        const pid_t pid = waitpid(-1, &st, WNOHANG);
        if (pid > 0)
        {
            running--;
            for (int i = 0; i < next; i++)
            {
                if (pids[i] == pid)
                {
                    char dir[MAX_PATH * 2];
                    snprintf(dir, sizeof(dir), "%s/%s", root, names[i]);
                    result[i] = outcome_of(st, killed[i], dir);
                }
            }
            continue;
        }
        if (pid < 0 && errno != EINTR)
            break;
        for (int i = 0; i < next; i++)
        {
            if (pids[i] > 0 && result[i] == NOT_RUN && !killed[i]
                && now() - started[i] > 2 * CASE_CPU_SECONDS)
            {
                kill(pids[i], SIGKILL);
                killed[i] = true;
            }
        }
        usleep(5000);
    }

    int failed = 0, known = 0;
    for (int i = 0; i < n; i++)
    {
        char dir[MAX_PATH * 2];
        snprintf(dir, sizeof(dir), "%s/%s", root, names[i]);
        read_expect(dir);
        const enum outcome r = result[i];
        if (want.known)
        {
            if (r == (enum outcome)want.known)
            {
                printf("known %s: %s, %s\n", names[i], outcome_names[r], want.why);
                known++;
                continue;
            }
            /* fixed, or failing another way: either is to be looked at */
            printf("FAIL %s: %s, not the known %s\n", names[i],
                   outcome_names[r], outcome_names[want.known]);
            failed++;
        }
        else if (r == PASSED)
            continue;
        else
        {
            printf("FAIL %s: %s%s\n", names[i], outcome_names[r],
                   killed[i] ? " (killed at twice its time)" : "");
            failed++;
        }
        char log[MAX_PATH * 2], line[1024];
        snprintf(log, sizeof(log), "%s/log", dir);
        FILE *lf = fopen(log, "r");
        for (int k = 0; lf && k < 40 && fgets(line, sizeof(line), lf); k++)
            printf("    %s", line);
        if (lf)
            fclose(lf);
    }
    printf("%-10s %d cases, %d failed, %d known failures; files boot: %d "
           "checks\n", "itunesdb", n, failed, known, check_count);
    return failed ? 1 : 0;
}
