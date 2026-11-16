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

/* file_write.c on simfs.c's disk, with each of its file operations in turn
   failing, or the power cut before it or inside it.

   A disk as fat.c keeps it: a rename frees the old name before it writes
   the new one, so a cut inside a rename leaves the file under neither; a
   cut inside a write leaves part of it. A rename onto a name a file has
   would leave that name twice after a cut there: the test counts every
   one, cut or not, and there may be none.

   Each case writes a file through fw_open()/fw_put()/fw_seek()/fw_close(),
   whole_file_write() or whole_file_open()/whole_file_close(): with and
   without a file before it, with leftovers of an earlier cut beside it,
   with a buffer of each size and none, sizes from nothing to more than a
   buffer, and the caller giving up. Run once without a failure, counting
   its operations, then for each operation n:
   - n fails, with EIO or a full disk (a write short, ENOSPC): fw_close() says
     whether the new file has the name, and if it does, that's what's
     there; if not, the file before is, or after fw_recover() the old or
     the new; a full disk is library_write_failed()'s;
   - the power goes before n, or inside it: after the boot's fw_recover(),
     the name has the file before or the new one, whole, the new one only
     once it was whole; an FW_KEEP file that was there is never lost, an
     FW_CACHE one may be, and is made again.
   After the boot no leftover is beside an FW_KEEP file. What an earlier
   cut left is put right at a boot before the write, as on the iPod; after
   a failure, the next commit is made before any boot, over what that one
   left, and must be whole with nothing beside it. TEST_FW_DEBUG=1 says what a
   name held that was neither file. */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <setjmp.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "file.h"
#include "misc.h"
#include "file_write.h"
#include "stubs.h"
#include "check.h"

/* simfs.c's own, which these wrap (the Makefile renames them) */
int simfs_sim_open(const char *name, int oflag, ...);
ssize_t simfs_sim_write(int fildes, const void *buf, size_t nbyte);
int simfs_sim_close(int fildes);
int simfs_sim_rename(const char *old, const char *new);
int simfs_sim_remove(const char *path);
off_t simfs_sim_lseek(int fildes, off_t offset, int whence);

enum fault { NONE, FAIL_EIO, FAIL_NOSPC, CUT_BEFORE, CUT_INSIDE };
static const char *const fault_names[] = {
    "none", "fails (EIO)", "fails (ENOSPC)", "cut before", "cut inside"
};

static struct {
    enum fault fault;
    long at;            /* the operation it hits, from 1 */
    long ops;           /* operations so far */
    bool cut;           /* the power went: nothing more reaches the disk */
    long twice;         /* renames onto a name a file had */
    char twice_name[MAX_PATH];
    long renames_onto_free;
    int open_fds[64];
    int nopen;
} disk;

static jmp_buf power_cut;

static void fd_opened(int fd)
{
    if (fd >= 0 && disk.nopen < (int)ARRAYLEN(disk.open_fds))
        disk.open_fds[disk.nopen++] = fd;
}

static void fd_closed(int fd)
{
    for (int i = 0; i < disk.nopen; i++)
        if (disk.open_fds[i] == fd)
        {
            disk.open_fds[i] = disk.open_fds[--disk.nopen];
            return;
        }
}

/* The operation's turn: true if it fails, with errno set; a cut before it
   ends the run here */
static bool op_fails(void)
{
    disk.ops++;
    if (disk.ops != disk.at)
        return false;
    switch (disk.fault)
    {
    case FAIL_EIO:
        errno = EIO;
        return true;
    case FAIL_NOSPC:
        errno = ENOSPC;
        return true;
    case CUT_BEFORE:
        disk.cut = true;
        longjmp(power_cut, 1);
    default:
        return false;
    }
}

static bool cut_inside(void)
{
    return disk.ops == disk.at && disk.fault == CUT_INSIDE;
}

int sim_open(const char *name, int oflag, ...)
{
    int mode = 0666;
    if (oflag & O_CREAT)
    {
        va_list ap;
        va_start(ap, oflag);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (op_fails())
        return -1;
    const int fd = simfs_sim_open(name, oflag, mode);
    fd_opened(fd);
    if (cut_inside())
    {
        disk.cut = true;
        longjmp(power_cut, 1);
    }
    return fd;
}

ssize_t sim_write(int fildes, const void *buf, size_t nbyte)
{
    const bool fails = op_fails();
    if (fails && errno == ENOSPC && nbyte > 1)
        return simfs_sim_write(fildes, buf, nbyte / 2); /* what fitted */
    if (fails)
        return -1;
    if (cut_inside())
    {
        if (nbyte > 1)
            simfs_sim_write(fildes, buf, nbyte / 2);
        disk.cut = true;
        longjmp(power_cut, 1);
    }
    return simfs_sim_write(fildes, buf, nbyte);
}

int sim_close(int fildes)
{
    const bool fails = op_fails();
    fd_closed(fildes);
    const int rc = simfs_sim_close(fildes);
    if (cut_inside())
    {
        disk.cut = true;
        longjmp(power_cut, 1);
    }
    return fails ? -1 : rc;
}

off_t sim_lseek(int fildes, off_t offset, int whence)
{
    if (op_fails())
        return -1;
    return simfs_sim_lseek(fildes, offset, whence);
}

int sim_rename(const char *old, const char *new)
{
    if (op_fails())
        return -1;
    if (file_exists(new))
    {
        disk.twice++;
        snprintf(disk.twice_name, sizeof(disk.twice_name), "%s", new);
    }
    if (cut_inside())
    {
        /* fat.c frees the old name first: the file has neither */
        simfs_sim_remove(old);
        disk.cut = true;
        longjmp(power_cut, 1);
    }
    return simfs_sim_rename(old, new);
}

int sim_remove(const char *path)
{
    if (op_fails())
        return -1;
    const int rc = simfs_sim_remove(path);
    if (cut_inside())
    {
        disk.cut = true;
        longjmp(power_cut, 1);
    }
    return rc;
}

/* ---- the files ---- */

#define DIR_PATH "/.rockbox"
#define FILE_PATH DIR_PATH "/kept.dat"

/* what had the name, or will once the boot puts right what an earlier cut
   left (old), and what may instead (alt); -1 for none */
static unsigned char old_data[80000], alt_data[3000], new_data[80000];
static size_t old_size, alt_size;

static uint64_t rng_state = 20261116;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 11);
}

static void host_put(const char *path, const void *data, size_t n)
{
    char p[MAX_PATH * 2];
    FILE *f = fopen(simfs_path(path, p, sizeof(p)), "wb");
    if (!f || fwrite(data, 1, n, f) != n)
    {
        perror(p);
        exit(2);
    }
    fclose(f);
}

/* the file's bytes, or -1 if there's none */
static long host_get(const char *path, unsigned char *buf, size_t size)
{
    char p[MAX_PATH * 2];
    FILE *f = fopen(simfs_path(path, p, sizeof(p)), "rb");
    if (!f)
        return -1;
    const size_t n = fread(buf, 1, size, f);
    fclose(f);
    return (long)n;
}

static bool host_exists(const char *path)
{
    char p[MAX_PATH * 2];
    struct stat st;
    return !stat(simfs_path(path, p, sizeof(p)), &st);
}

static void host_remove(const char *path)
{
    char p[MAX_PATH * 2];
    unlink(simfs_path(path, p, sizeof(p)));
}

/* the folder's names, but the file's own and its leftovers */
static int others_in_folder(void)
{
    char p[MAX_PATH * 2];
    DIR *d = opendir(simfs_path(DIR_PATH, p, sizeof(p)));
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.' && strncmp(e->d_name, "kept.dat", 8))
            n++;
    if (d)
        closedir(d);
    return n;
}

static int leftovers(void)
{
    return host_exists(FILE_PATH ".tmp") + host_exists(FILE_PATH ".new")
           + host_exists(FILE_PATH ".old");
}

/* what's under the name: 0 none, 1 the file before, 2 the new one, -1
   anything else */
static int under_name(size_t new_size)
{
    static unsigned char buf[100000];
    const long n = host_get(FILE_PATH, buf, sizeof(buf));
    if (n < 0)
        return 0;
    if ((size_t)n == new_size && !memcmp(buf, new_data, n))
        return 2;
    if (old_size != (size_t)-1 && (size_t)n == old_size
        && !memcmp(buf, old_data, n))
        return 1;
    if (alt_size != (size_t)-1 && (size_t)n == alt_size
        && !memcmp(buf, alt_data, n))
        return 1;
    if (getenv("TEST_FW_DEBUG"))
    {
        size_t i = 0;
        while (i < (size_t)n && i < new_size && buf[i] == new_data[i])
            i++;
        fprintf(stderr, "under the name: %ld bytes, new %zu, old %zu, the "
                "same as the new up to %zu\n", n, new_size, old_size, i);
    }
    return -1;
}

/* ---- a case ---- */

enum how { FW_BUFFERED, FW_HEADER, WHOLE_WRITE, WHOLE_FD };
static const char *const how_names[] = {
    "fw_put", "fw_put and fw_seek", "whole_file_write", "whole_file_open"
};

struct a_case {
    enum fw_policy policy;
    enum how how;
    size_t buf_size;    /* fw's buffer, 0 for none */
    size_t new_size;
    bool before;        /* a file before it */
    int left;           /* leftovers of a cut before: 0 none, 1 .tmp, 2 .new
                           (whole, newer), 3 .old without the file, 4 .old
                           beside it */
    bool give_up;       /* the caller says it failed */
};

static char fw_buf[8192];

/* the write itself; true if it said the new file has the name */
static bool write_file(const struct a_case *c)
{
    switch (c->how)
    {
    case WHOLE_WRITE:
        return whole_file_write(FILE_PATH, new_data, c->new_size);
    case WHOLE_FD:
    {
        const int fd = whole_file_open(FILE_PATH);
        if (fd < 0)
            return false;
        bool ok = true;
        for (size_t at = 0; at < c->new_size && ok;)
        {
            const size_t n = MIN(c->new_size - at, 3000);
            ok = write(fd, new_data + at, n) == (ssize_t)n;
            at += n;
        }
        return whole_file_close(fd, FILE_PATH, ok && !c->give_up);
    }
    default:
    {
        struct fw w;
        fw_open(&w, FILE_PATH, c->policy, c->buf_size ? fw_buf : NULL,
                c->buf_size);
        /* a header written blank, then again once the rest is known */
        const size_t head = c->how == FW_HEADER ? MIN(c->new_size, 24) : 0;
        static const unsigned char blank[24];
        fw_put(&w, blank, head);
        size_t at = head;
        while (at < c->new_size)
        {
            const size_t most = 1 + rng() % 5000;
            const size_t n = MIN(c->new_size - at, most);
            fw_put(&w, new_data + at, n);
            at += n;
        }
        if (head)
        {
            fw_seek(&w, 0);
            fw_put(&w, new_data, head);
        }
        return fw_close(&w, !c->give_up);
    }
    }
}

static void case_name(const struct a_case *c, char *buf, size_t size)
{
    snprintf(buf, size, "%s %s buf %zu size %zu %s left %d%s",
             c->policy == FW_KEEP ? "FW_KEEP" : "FW_CACHE", how_names[c->how],
             c->buf_size, c->new_size, c->before ? "before" : "new",
             c->left, c->give_up ? " giving up" : "");
}

static void random_bytes(unsigned char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        buf[i] = rng();
}

/* The disk before the write; old_data is then what the name has once the
   boot put right what an earlier cut left: a .new, else the file, else a
   .old. A cut inside that boot's raise of a .new leaves the file before it
   (alt_data). */
static void set_up(const struct a_case *c)
{
    host_remove(FILE_PATH);
    host_remove(FILE_PATH ".tmp");
    host_remove(FILE_PATH ".new");
    host_remove(FILE_PATH ".old");
    old_size = alt_size = (size_t)-1;
    unsigned char other[3000];
    const size_t other_size = 1 + rng() % sizeof(other);
    random_bytes(other, other_size);
    if (c->before)
    {
        old_size = rng() % 3000;
        random_bytes(old_data, old_size);
        host_put(FILE_PATH, old_data, old_size);
    }
    switch (c->left)
    {
    case 1: /* written in part when the power went: never raised */
        host_put(FILE_PATH ".tmp", other, other_size);
        break;
    case 2: /* whole, the power gone before it took the name: the newest */
        host_put(FILE_PATH ".new", other, other_size);
        if (c->before)
        {
            memcpy(alt_data, old_data, old_size);
            alt_size = old_size;
        }
        memcpy(old_data, other, other_size);
        old_size = other_size;
        break;
    case 3: /* the one before waiting, the new one lost inside its rename */
        host_remove(FILE_PATH);
        host_put(FILE_PATH ".old", other, other_size);
        memcpy(old_data, other, other_size);
        old_size = other_size;
        break;
    case 4: /* the one before left beside the new one, which is newer */
        host_put(FILE_PATH ".old", other, other_size);
        if (!c->before)
        {
            memcpy(old_data, other, other_size);
            old_size = other_size;
        }
        break;
    }
    random_bytes(new_data, c->new_size);
    memset(&disk, 0, sizeof(disk));
}

/* the boot after a cut, or after a failure: as a reader finds it */
static void boot(void)
{
    const enum fault was = disk.fault;
    disk.fault = NONE;
    for (int i = 0; i < disk.nopen; i++)
        simfs_sim_close(disk.open_fds[i]);
    disk.nopen = 0;
    fw_recover(FILE_PATH);
    disk.fault = was;
}

static long cases, runs, cut_runs, fail_runs, raised_new_after_cut,
            kept_old_after_cut, lost_cache, next_commits;

static void run(const struct a_case *c, uint64_t seed, enum fault fault,
                long at, long *ops_out)
{
    char name[256];
    case_name(c, name, sizeof(name));
    char what[384];
    snprintf(what, sizeof(what), "%s, op %ld %s", name, at, fault_names[fault]);
    check_case = what;

    rng_state = seed;
    set_up(c);
    disk.fault = fault;
    disk.at = at;
    stubs.write_failed = 0;
    bool said = false;
    const bool kept = c->policy == FW_KEEP && c->how <= FW_HEADER;
    /* what an earlier cut left is the boot's to put right, before the file
       is written (file_write.h) */
    if (c->left)
        boot();
    if (!setjmp(power_cut))
        said = write_file(c);
    runs++;
    if (ops_out)
        *ops_out = disk.ops;

    const size_t new_size = c->new_size;
    const bool had = old_size != (size_t)-1;
    if (!kept)
        CHECK(!host_exists(FILE_PATH ".new") && !host_exists(FILE_PATH ".old"));
    if (disk.cut)
        cut_runs++;
    else
    {
        fail_runs += fault != NONE;
        if (said)
            CHECK_EQ(under_name(new_size), 2);
        if (!said && !c->give_up && fault == FAIL_NOSPC)
            CHECK(stubs.write_failed > 0);
        if (fault == NONE)
        {
            CHECK_EQ(said, !c->give_up);
            if (!(c->how == WHOLE_FD && c->give_up))
                CHECK_EQ(stubs.write_failed, 0);
        }
        /* the next commit, before any boot, over what this one's failure
           left: whole under the name, nothing beside it */
        else if (!said && kept && !c->give_up)
        {
            disk.fault = NONE;
            CHECK(write_file(c));
            CHECK_EQ(under_name(new_size), 2);
            CHECK_EQ(leftovers(), 0);
            disk.fault = fault;
            next_commits++;
        }
    }
    boot();
    const int after = under_name(new_size);
    /* never a part, never the name twice */
    CHECK(after >= 0);
    CHECK_EQ(disk.twice, 0);
    if (said)
        CHECK_EQ(after, 2);
    if (c->give_up)
        CHECK(after != 2 || (new_size == 0 && had && old_size == 0));
    /* the one before, or the new one: an FW_KEEP file is never lost */
    if (kept && had)
        CHECK(after != 0);
    if (disk.cut)
    {
        if (after == 2)
            raised_new_after_cut++;
        else if (after == 1)
            kept_old_after_cut++;
        else if (had)
            lost_cache++;
    }
    CHECK_EQ(leftovers(), 0);
    CHECK_EQ(others_in_folder(), 0);
    check_case = NULL;
}

static void each_run(const struct a_case *c)
{
    long ops = 0;
    const uint64_t seed = rng_state;
    cases++;
    run(c, seed, NONE, 0, &ops);
    for (long n = 1; n <= ops + 1; n++)
        for (enum fault f = FAIL_EIO; f <= CUT_INSIDE; f++)
            run(c, seed, f, n, NULL);
    rng_state = seed;
    rng();
}

/* fw_recover() from each state a cut or an older firmware can leave */
static void recover_states(void)
{
    static const char *const files[] = {
        FILE_PATH, FILE_PATH ".tmp", FILE_PATH ".new", FILE_PATH ".old"
    };
    for (int set = 0; set < 16; set++)
    {
        char what[64];
        snprintf(what, sizeof(what), "recover: state %d", set);
        check_case = what;
        memset(&disk, 0, sizeof(disk));
        for (int i = 0; i < 4; i++)
        {
            host_remove(files[i]);
            if (set & (1 << i))
                host_put(files[i], files[i], strlen(files[i]));
        }
        fw_recover(FILE_PATH);
        unsigned char buf[MAX_PATH];
        const long n = host_get(FILE_PATH, buf, sizeof(buf));
        /* the newest whole: .new, else the file, else .old */
        const char *want = set & 4 ? files[2] : set & 1 ? files[0]
                         : set & 8 ? files[3] : NULL;
        if (want)
            CHECK(n == (long)strlen(want) && !memcmp(buf, want, n));
        else
            CHECK_EQ(n, -1);
        CHECK_EQ(leftovers(), 0);
        CHECK_EQ(disk.twice, 0);
    }
    check_case = NULL;
}

static void leftover_names(void)
{
    char file[MAX_PATH];
    CHECK(fw_leftover("a.m3u8.new", file, sizeof(file))
          && !strcmp(file, "a.m3u8"));
    CHECK(fw_leftover("a.m3u8.OLD", file, sizeof(file))
          && !strcmp(file, "a.m3u8"));
    CHECK(fw_leftover("itunesdb.dat.tmp", file, sizeof(file))
          && !strcmp(file, "itunesdb.dat"));
    CHECK(!fw_leftover("a.m3u8", file, sizeof(file)));
    CHECK(!fw_leftover(".new", file, sizeof(file)));
    CHECK(!fw_leftover("x.new", file, 1));
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <empty folder>\n", argv[0]);
        return 2;
    }
    check_time_limit(600);
    char p[MAX_PATH * 2];
    mkdir(argv[1], 0777);
    simfs_root(argv[1]);
    mkdir(simfs_path(DIR_PATH, p, sizeof(p)), 0777);

    leftover_names();
    recover_states();

    static const size_t sizes[] = { 0, 1, 24, 511, 512, 4096, 9000, 70000 };
    static const size_t bufs[] = { 0, 16, 512, 4096, 8192 };
    for (int policy = FW_CACHE; policy <= FW_KEEP; policy++)
    for (int how = FW_BUFFERED; how <= WHOLE_FD; how++)
    {
        if (how >= WHOLE_WRITE && policy == FW_KEEP)
            continue; /* whole_file_* are FW_CACHE's */
        for (size_t s = 0; s < ARRAYLEN(sizes); s++)
        for (size_t b = 0; b < ARRAYLEN(bufs); b++)
        for (int before = 0; before < 2; before++)
        for (int left = 0; left <= 4; left++)
        for (int give_up = 0; give_up < 2; give_up++)
        {
            if (how >= WHOLE_WRITE && b > 0)
                continue;
            if (how == WHOLE_WRITE && give_up)
                continue;
            /* leftovers matter to FW_KEEP alone, and the sizes to fw_put() */
            if (left && (policy == FW_CACHE || how != FW_BUFFERED))
                continue;
            if (left && (b % 2 || s % 3))
                continue;
            const struct a_case c = {
                policy, how, bufs[b], sizes[s], before, left, give_up
            };
            each_run(&c);
        }
    }
    printf("file_write: %ld cases, %ld runs: %ld with a failure, %ld cut "
           "(new raised %ld, old kept %ld, a cache lost %ld); %ld commits "
           "over a failed one's leftovers\n",
           cases, runs, fail_runs, cut_runs, raised_new_after_cut,
           kept_old_after_cut, lost_cache, next_commits);

    /* the folder emptied */
    host_remove(FILE_PATH);
    rmdir(simfs_path(DIR_PATH, p, sizeof(p)));
    rmdir(argv[1]);
    return check_done("file_write");
}
