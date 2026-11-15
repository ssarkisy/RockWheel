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
   them, the whole import (itunesdb_import_tracks()) with its playlists and
   its index of covers, then the iPod's name. Each case of make_itunesdb.py's
   is an iPod's folder, read in a process of its own, so that each starts as
   at boot and a crash or a hang is that case's. A case gives what its
   "expect" says, and every one, whole or damaged, without a sanitizer error
   and within its time. */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
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
    }
    fclose(f);
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
    const int count = itunesdb_import_tracks(true, &done, &total);
    if (want.tracks >= 0)
        CHECK_EQ(count, want.tracks);
    if (want.covers >= 0)
        CHECK_EQ(itunesdb_art_count(), want.covers);
    for (int i = 0; i < want.nplaylists; i++)
        check_playlist(i);

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
    printf("%-10s %d cases, %d failed, %d known failures\n", "itunesdb", n,
           failed, known);
    return failed ? 1 : 0;
}
