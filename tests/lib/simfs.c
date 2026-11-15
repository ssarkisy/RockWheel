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
 * The simulator's file functions (uisimulator/common/filesystem-sim.h) over
 * a folder of the Mac's: the simulator's own are tied to SDL and its disk
 * of several volumes, which the tests don't need. Reads are counted.
 *
 ****************************************************************************/
#define RB_FILESYSTEM_OS
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "config.h"
#include "file.h"
#include "dir.h"
#include "pathfuncs.h"
#include "stubs.h"

/* the host's functions, not the macros of file.h and dir.h */
#undef open
#undef creat
#undef close
#undef read
#undef write
#undef lseek
#undef ftruncate
#undef fsync
#undef remove
#undef rename
#undef mkdir
#undef rmdir
#undef opendir
#undef readdir
#undef closedir

static char root[MAX_PATH] = ".";

void simfs_root(const char *dir)
{
    snprintf(root, sizeof(root), "%s", dir);
}

const char *simfs_path(const char *path, char *buf, size_t size)
{
#ifdef HAVE_MULTIVOLUME
    /* "/<HDD0>/..." is the one volume there is */
    path_strip_volume(path, &path, false);
#endif
    snprintf(buf, size, "%s%s%s", root, *path == '/' ? "" : "/", path);
    return buf;
}

int sim_open(const char *name, int oflag, ...)
{
    char p[MAX_PATH * 2];
    mode_t mode = 0666;
    if (oflag & O_CREAT)
    {
        va_list ap;
        va_start(ap, oflag);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return open(simfs_path(name, p, sizeof(p)), oflag, mode);
}

int sim_creat(const char *name, mode_t mode)
{
    char p[MAX_PATH * 2];
    return creat(simfs_path(name, p, sizeof(p)), mode);
}

int sim_close(int fildes)
{
    return close(fildes);
}

ssize_t sim_read(int fildes, void *buf, size_t nbyte)
{
    const ssize_t n = read(fildes, buf, nbyte);
    stubs.reads++;
    if (n > 0)
        stubs.read_bytes += n;
    return n;
}

ssize_t sim_write(int fildes, const void *buf, size_t nbyte)
{
    return write(fildes, buf, nbyte);
}

off_t sim_lseek(int fildes, off_t offset, int whence)
{
    return lseek(fildes, offset, whence);
}

int sim_ftruncate(int fildes, off_t length)
{
    return ftruncate(fildes, length);
}

int sim_fsync(int fildes)
{
    return fsync(fildes);
}

off_t sim_ffilesize(int fildes)
{
    struct stat st;
    return fstat(fildes, &st) ? -1 : st.st_size;
}

int sim_remove(const char *path)
{
    char p[MAX_PATH * 2];
    return remove(simfs_path(path, p, sizeof(p)));
}

int sim_rename(const char *old, const char *new)
{
    char p[MAX_PATH * 2], q[MAX_PATH * 2];
    return rename(simfs_path(old, p, sizeof(p)), simfs_path(new, q, sizeof(q)));
}

int sim_modtime(const char *path, time_t modtime)
{
    char p[MAX_PATH * 2];
    struct timeval t[2] = { { modtime, 0 }, { modtime, 0 } };
    return utimes(simfs_path(path, p, sizeof(p)), t);
}

bool sim_file_exists(const char *path)
{
    char p[MAX_PATH * 2];
    struct stat st;
    return !stat(simfs_path(path, p, sizeof(p)), &st) && S_ISREG(st.st_mode);
}

/* a folder open: the host's, its path for stat(), and the entry read */
struct simfs_dir {
    DIR *dir;
    char path[MAX_PATH * 2];
    struct sim_dirent entry;
};

DIR *sim_opendir(const char *dirname)
{
    struct simfs_dir *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    simfs_path(dirname, d->path, sizeof(d->path));
    d->dir = opendir(d->path);
    if (!d->dir)
    {
        free(d);
        return NULL;
    }
    return (DIR *)d;
}

struct sim_dirent *sim_readdir(DIR *dirp)
{
    struct simfs_dir *d = (struct simfs_dir *)dirp;
    struct dirent *e = readdir(d->dir);
    if (!e)
        return NULL;
    snprintf(d->entry.d_name, sizeof(d->entry.d_name), "%s", e->d_name);
    d->entry.info.osdirent = e;
    return &d->entry;
}

int sim_closedir(DIR *dirp)
{
    struct simfs_dir *d = (struct simfs_dir *)dirp;
    const int rc = closedir(d->dir);
    free(d);
    return rc;
}

struct dirinfo sim_dir_get_info(DIR *dirp, struct sim_dirent *entry)
{
    struct simfs_dir *d = (struct simfs_dir *)dirp;
    struct dirinfo info = { 0 };
    char p[MAX_PATH * 3];
    struct stat st;
    snprintf(p, sizeof(p), "%s/%s", d->path, entry->d_name);
    if (!stat(p, &st))
    {
        info.attribute = S_ISDIR(st.st_mode) ? ATTR_DIRECTORY : 0;
        info.size = st.st_size;
        info.mtime = st.st_mtime;
    }
    return info;
}

int sim_mkdir(const char *path)
{
    char p[MAX_PATH * 2];
    return mkdir(simfs_path(path, p, sizeof(p)), 0777);
}

int sim_rmdir(const char *path)
{
    char p[MAX_PATH * 2];
    return rmdir(simfs_path(path, p, sizeof(p)));
}

bool sim_dir_exists(const char *dirname)
{
    char p[MAX_PATH * 2];
    struct stat st;
    return !stat(simfs_path(dirname, p, sizeof(p)), &st) && S_ISDIR(st.st_mode);
}
