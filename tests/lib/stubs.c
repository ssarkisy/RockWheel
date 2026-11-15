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

/* The firmware the tested sources call, as little of it as they need: the
   kernel does nothing, the database is empty, the covers and the screens
   are left alone. What the tests look at is counted in stubs. */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "debug.h"
#include "panic.h"
#include "file.h"
#include "disk.h"
#include "storage.h"
#include "ata_idle_notify.h"
#include "settings.h"
#include "misc.h"
#include "tagcache.h"
#include "health_log.h"
#include "itunesdb.h"
#include "smart_playlists.h"
#include "library_playlists.h"
#include "synced_covers.h"
#include "album_header.h"
#include "list_covers.h"
#include "skin_engine/skin_engine.h"
#include "skin_engine/cover_palette.h"
#include "skin_engine/menu_cover.h"
#include "stubs.h"

struct stubs stubs;

struct user_settings global_settings;

/* the kernel */

void yield(void)
{
    stubs.yields++;
}

unsigned sleep(unsigned ticks)
{
    (void)ticks;
    stubs.yields++;
    return 0;
}

void mutex_init(struct mutex *m)
{
    (void)m;
}

void mutex_lock(struct mutex *m)
{
    (void)m;
}

void mutex_unlock(struct mutex *m)
{
    (void)m;
}

void register_storage_idle_func(void (*function)(void))
{
    (void)function;
}

void debugf(const char *fmt, ...)
{
    if (!getenv("TESTS_DEBUGF"))
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void panicf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("panicf: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}

/* the disk: plenty of room */

void volume_size(IF_MV(int volume,) sector_t *size, sector_t *free)
{
    IF_MV((void)volume;)
    if (size)
        *size = 64 * 1024 * 1024; /* KB */
    if (free)
        *free = 32 * 1024 * 1024;
}

/* apps/misc.c's, as it does them */

void qsort_yielding(void *base, size_t count, size_t size,
                    int (*compare)(const void *, const void *))
{
    qsort(base, count, size, compare);
}

/* a byte at a time, so that, as misc.c's puts back what it read past, the
   rest of a line longer than the buffer is left for the next call */
int read_line(int fd, char *buffer, int buffer_size)
{
    if (!buffer || buffer_size-- <= 0)
    {
        errno = EINVAL;
        return -1;
    }
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

void library_write_failed(void)
{
    stubs.write_failed++;
}

void library_no_room(void)
{
    stubs.no_room++;
}

/* the file whole or not at all: here only as a temporary file renamed */
int whole_file_open(const char *path)
{
    char temp[MAX_PATH];
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    const int fd = creat(temp, 0666);
    if (fd < 0)
        library_write_failed();
    return fd;
}

bool whole_file_close(int fd, const char *path, bool ok)
{
    char temp[MAX_PATH];
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    ok = close(fd) == 0 && ok;
    if (ok)
    {
        remove(path);
        ok = rename(temp, path) == 0;
    }
    if (!ok)
    {
        library_write_failed();
        remove(temp);
    }
    return ok;
}

bool whole_file_write(const char *path, const void *data, size_t size)
{
    const int fd = whole_file_open(path);
    if (fd < 0)
        return false;
    return whole_file_close(fd, path, write(fd, data, size) == (ssize_t)size);
}

void health_note(const char *fmt, ...)
{
    stubs.health_notes++;
    if (!getenv("TESTS_DEBUGF"))
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("health: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* the database: empty, not restoring */

int tagcache_get_commitid(void)
{
    return 1;
}

void tagcache_raise_commitid(void)
{
}

bool tagcache_restoring(void)
{
    return false;
}

bool tagcache_restore_finder(void)
{
    return false;
}

unsigned long tagcache_caches_floor_kb(void)
{
    return 0;
}

int tagcache_path_kind(const char *path)
{
    (void)path;
    return 0;
}

bool tagcache_outside_images(uint32_t *sum)
{
    *sum = 0;
    return true;
}

bool tagcache_update_runtime(int (*update)(const char *filename,
                                           struct tagcache_runtime *rt,
                                           void *data),
                             void *data)
{
    (void)update;
    (void)data;
    return true;
}

int tagcache_entry_count(void)
{
    return 0;
}

bool tagcache_finds_entries(void)
{
    return false;
}

int tagcache_track_kind(int idx_id)
{
    (void)idx_id;
    return 0;
}

bool tagcache_for_each_track(void (*f)(int idx_id, const int32_t *tag_seek,
                                       void *data),
                             void *data)
{
    (void)f;
    (void)data;
    return false;
}

bool tagcache_tag_string(int tag, int32_t seek, char *buf, size_t size)
{
    (void)tag;
    (void)seek;
    if (size)
        buf[0] = '\0';
    return false;
}

/* smart playlists: the rules noted, not worked out */

void smart_import_begin(void)
{
}

void smart_import_note(const char *file, uint32_t time,
                       const struct smart_rules *rules)
{
    (void)file;
    (void)time;
    (void)rules;
    stubs.smart_notes++;
}

bool smart_import_keep(const char *file)
{
    (void)file;
    return false;
}

void smart_import_end(bool complete)
{
    (void)complete;
}

/* the covers and the screens that show them */

void synced_covers_imported(bool covers)
{
    (void)covers;
}

void album_header_forget_cover(void)
{
}

void list_covers_forget(void)
{
}

void lp_index_forget(const char *file)
{
    (void)file;
}

void menu_cover_kept_forget(void)
{
}

void skin_forget_db_art(void)
{
}

uint32_t cover_palette_colour(const fb_data *pixels, int width, int height)
{
    (void)pixels;
    (void)width;
    (void)height;
    return 0;
}
