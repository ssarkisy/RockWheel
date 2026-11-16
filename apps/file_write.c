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

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "misc.h"
#include "file_write.h"

/* The names made beside a file (".tmp", ".new", ".old"), and the block a
   lone copy is copied through, are static rather than on the stack: the
   storage thread, which saves the marks at the disk's idle
   (itunesdb_save_flags()), has 2 KB, and the file system's own calls under
   these take most of it. One user at a time, under fw_mutex. */
static char name_tmp[MAX_PATH], name_new[MAX_PATH], name_old[MAX_PATH];
static struct mutex fw_mutex;
static bool fw_mutex_ready;

static void fw_lock(void)
{
    /* at the first use, which settings_load() makes: tested and set without
       a yield between, the threads on one core */
    if (!fw_mutex_ready)
    {
        mutex_init(&fw_mutex);
        fw_mutex_ready = true;
    }
    mutex_lock(&fw_mutex);
}

static void fw_unlock(void)
{
    mutex_unlock(&fw_mutex);
}

/* path with a suffix after it, false if that doesn't fit */
static bool fw_name(char *buf, const char *path, const char *suffix)
{
    const size_t len = strlen(path);
    if (len + 5 > MAX_PATH)
        return false;
    memcpy(buf, path, len);
    memcpy(buf + len, suffix, 5);
    return true;
}

/* path's three, under the lock */
static bool fw_names(const char *path)
{
    return fw_name(name_tmp, path, ".tmp") && fw_name(name_new, path, ".new")
           && fw_name(name_old, path, ".old");
}

/* No file has the name: renamed onto one, a file has it twice until the
   one it replaces is gone, and a cut between leaves both (fat.c) */
static bool name_free(const char *name)
{
    return remove(name) == 0 || errno == ENOENT;
}

/* .new, whole, takes the name; the file that had it waits as .old until
   then. Under the lock, the names made. Inlined, as are keep_place() and
   place(): one frame less each above the file system's rename, on the
   storage thread's 2 KB stack (itunesdb_save_flags()). */
static inline __attribute__((always_inline)) bool keep_raise(const char *path)
{
    const bool had = file_exists(path);
    /* .old is older than the file with the name */
    if (had && (!name_free(name_old) || rename(path, name_old) < 0))
        return false;
    if (rename(name_new, path) < 0)
    {
        if (had)
            rename(name_old, path);
        return false;
    }
    remove(name_old);
    return true;
}

/* .tmp, written and closed: whole once it is .new */
static inline __attribute__((always_inline)) bool keep_place(const char *path)
{
    if (!name_free(name_new) || rename(name_tmp, name_new) < 0)
        return false;
    if (keep_raise(path))
        return true;
    /* the file before has the name again; without it, both stay for
       fw_recover() */
    if (file_exists(path))
        remove(name_new);
    return false;
}

/* .tmp, whole and closed if ok, takes the name, or is removed. A failure
   of its own, as a rename for want of room, is library_write_failed()'s. */
static inline __attribute__((always_inline))
bool place(const char *path, bool keep, bool ok)
{
    fw_lock();
    if (!fw_names(path))
        ok = false;
    else if (ok)
    {
        /* FW_CACHE: the old one gone first, a moment of none if the power
           goes then, which is made again */
        ok = keep ? keep_place(path)
                  : name_free(path) && rename(name_tmp, path) == 0;
        if (!ok)
            library_write_failed();
    }
    if (!ok)
        remove(name_tmp);
    fw_unlock();
    return ok;
}

/* under the lock */
static int open_tmp(const char *path)
{
    if (!fw_name(name_tmp, path, ".tmp"))
        return -1;
    /* errno from here to the close is this file's */
    errno = 0;
    const int fd = open(name_tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        library_write_failed();
    return fd;
}

static void recover_locked(const char *path);

bool fw_open(struct fw *w, const char *path, enum fw_policy policy,
             void *buf, size_t size)
{
    w->path = path;
    w->buf = buf;
    w->size = buf ? size : 0;
    w->used = 0;
    w->keep = policy == FW_KEEP;
    /* What a cut left beside an FW_KEEP file is put right at boot, before
       it is written (file_write.h); what a failed commit left, by the next
       commit (keep_place()) */
    fw_lock();
    w->fd = open_tmp(path);
    fw_unlock();
    w->ok = w->fd >= 0;
    return w->ok;
}

static bool fw_write(struct fw *w, const void *data, size_t len)
{
    if (w->ok && write(w->fd, data, len) != (ssize_t)len)
    {
        w->ok = false;
        library_write_failed();
    }
    return w->ok;
}

bool fw_flush(struct fw *w)
{
    if (w->used)
        fw_write(w, w->buf, w->used);
    w->used = 0;
    return w->ok;
}

/* Each block filled to its end, whatever falls across it: a write that
   starts or ends inside a sector costs the disk several commands where a
   whole block costs one */
bool fw_put(struct fw *w, const void *data, size_t len)
{
    /* without a buffer, a write() each, as the caller's own were */
    if (!w->size)
        return fw_write(w, data, len);
    const char *from = data;
    while (len > 0 && w->ok)
    {
        /* more than the buffer holds at once, as the slides of a large
           library are */
        if (w->used == 0 && len >= w->size)
        {
            const size_t whole = len - len % w->size;
            fw_write(w, from, whole);
            from += whole;
            len -= whole;
            continue;
        }
        const size_t part = MIN(len, w->size - w->used);
        memcpy(w->buf + w->used, from, part);
        w->used += part;
        from += part;
        len -= part;
        if (w->used == w->size)
            fw_flush(w);
    }
    return w->ok;
}

bool fw_seek(struct fw *w, off_t offset)
{
    if (fw_flush(w) && lseek(w->fd, offset, SEEK_SET) != offset)
    {
        w->ok = false;
        library_write_failed();
    }
    return w->ok;
}

/* The file's last sector goes to the disk as it closes, and that can fail,
   as with the disk full at a new cluster: the file is then short */
bool fw_end(struct fw *w)
{
    if (w->fd >= 0)
    {
        fw_flush(w);
        if (close(w->fd) < 0 && w->ok)
        {
            w->ok = false;
            library_write_failed();
        }
    }
    w->fd = -1;
    w->used = 0;
    return w->ok;
}

bool fw_place(const char *path, enum fw_policy policy, bool ok)
{
    return place(path, policy == FW_KEEP, ok);
}

bool fw_close(struct fw *w, bool ok)
{
    if (!ok && w->fd >= 0)
    {
        w->used = 0; /* given up: nothing more written */
        close(w->fd);
        w->fd = -1;
    }
    return place(w->path, w->keep, fw_end(w) && ok);
}

/* The one copy there is, from (.new or .old), takes the name as a copy
   written to .tmp through block, the other of the two names' buffers: a
   rename of it, cut inside, would leave it under none */
static bool copy_raise(const char *from, const char *path, char *block)
{
    const int in = open(from, O_RDONLY);
    if (in < 0)
        return false;
    const int out = open(name_tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool ok = out >= 0;
    ssize_t n = 1;
    while (ok && n > 0)
    {
        n = read(in, block, MAX_PATH);
        ok = n >= 0 && (n == 0 || write(out, block, n) == n);
    }
    close(in);
    if (out >= 0)
        ok = close(out) == 0 && ok;
    if (ok && rename(name_tmp, path) == 0)
    {
        remove(from);
        return true;
    }
    remove(name_tmp);
    return false;
}

static void recover_locked(const char *path)
{
    if (!fw_names(path))
        return;
    remove(name_tmp);
    const bool named = file_exists(path);
    /* the new one was whole: it takes the name, the file before kept
       until it has */
    if (file_exists(name_new))
    {
        if (named || file_exists(name_old))
            keep_raise(path);
        else
            copy_raise(name_new, path, name_old);
    }
    else if (named)
        remove(name_old);
    /* cut as the new one took the name: the one before */
    else if (file_exists(name_old))
        copy_raise(name_old, path, name_new);
}

void fw_recover(const char *path)
{
    fw_lock();
    recover_locked(path);
    fw_unlock();
}

bool fw_leftover(const char *name, char *file, size_t size)
{
    static const char *const suffixes[] = { ".tmp", ".new", ".old" };
    const size_t len = strlen(name);
    if (len <= 4)
        return false;
    for (size_t i = 0; i < ARRAYLEN(suffixes); i++)
    {
        if (!strcasecmp(name + len - 4, suffixes[i]))
            return (size_t)snprintf(file, size, "%.*s", (int)(len - 4), name)
                   < size;
    }
    return false;
}

void fw_recover_files(const char *dir, const char *const *files, int count,
                      bool (*whole_new)(const char *new))
{
    uint32_t found = 0;
    fw_lock();
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)))
    {
        /* the name a leftover is of, in a buffer of the lock's */
        if (!fw_leftover(e->d_name, name_old, MAX_PATH))
            continue;
        for (int i = 0; i < count && i < 32; i++)
        {
            const char *name = strrchr(files[i], '/');
            if (name && !strcasecmp(name_old, name + 1))
                found |= 1u << i;
        }
    }
    if (d)
        closedir(d);
    for (int i = 0; i < count && i < 32; i++)
    {
        if (!(found & (1u << i)))
            continue;
        if (whole_new && fw_name(name_new, files[i], ".new")
            && file_exists(name_new) && !whole_new(name_new))
            remove(name_new);
        recover_locked(files[i]);
    }
    fw_unlock();
}

/* The FW_CACHE writer for a descriptor of the caller's (plugins have it) */
int whole_file_open(const char *path)
{
    fw_lock();
    const int fd = open_tmp(path);
    fw_unlock();
    return fd;
}

/* ok: each write went well; the caller's writes weren't seen here, so a
   failure, its own or theirs, goes to library_write_failed() */
bool whole_file_close(int fd, const char *path, bool ok)
{
    ok = close(fd) == 0 && ok;
    if (!ok)
        library_write_failed();
    return place(path, false, ok);
}

bool whole_file_write(const char *path, const void *data, size_t size)
{
    const int fd = whole_file_open(path);
    if (fd < 0)
        return false;
    return whole_file_close(fd, path, write(fd, data, size) == (ssize_t)size);
}
