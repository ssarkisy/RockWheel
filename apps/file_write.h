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
#ifndef _FILE_WRITE_H_
#define _FILE_WRITE_H_

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* Our own files, whole or not at all. Each is written beside its name, to
   "<path>.tmp", and takes the name only once every write and the close went
   well: a file cut short by a full disk, a failed write or a pulled battery
   is never read under the name. How the new one takes the name is the
   file's policy, by what a cut can cost:

       struct fw w;
       fw_open(&w, path, FW_KEEP, buf, sizeof(buf));
       fw_put(&w, data, size);
       ...
       if (!fw_close(&w, true))
           ...the file before stays

   A rename over an existing name can leave that name twice after a cut, and
   a cut inside a rename can leave a file under neither of its names
   (fat.c): a name is always freed before another file takes it. */
enum fw_policy {
    /* Made again if lost, as a cache is: the old file removed, then the new
       one renamed. A cut between the two leaves none. */
    FW_CACHE,
    /* Can't be made again, as unsent plays or a playlist made on the iPod:
       the new file, once whole, is "<path>.new"; the old one becomes
       "<path>.old" and stays until the new one has the name. Some whole
       copy has a name at every moment, and fw_recover() raises it after a
       cut: the new one if it was whole, else the old. Each FW_KEEP file is
       recovered so at boot, before it's read or written (fw_recover_files(),
       fw_recover()): fw_open() doesn't, as its stack would be the storage
       thread's. */
    FW_KEEP,
};

/* A file being written: the caller's, kept until fw_close() */
struct fw {
    const char *path;   /* the caller's, until fw_close() */
    char *buf;          /* the caller's, or NULL: each fw_put() a write() */
    size_t size;
    size_t used;
    int fd;             /* < 0 if it couldn't be opened */
    bool ok;            /* opened, and each write so far went well */
    bool keep;          /* FW_KEEP */
};

/* Opens "<path>.tmp" to write. Writes go through buf, each block filled to
   its end, a write() costing the disk about as much for 16 bytes as for
   4 KB. False if it can't be opened, and fw_close() then fails. */
bool fw_open(struct fw *w, const char *path, enum fw_policy policy,
             void *buf, size_t size);
/* False once any write failed: the file isn't to take the name */
bool fw_put(struct fw *w, const void *data, size_t len);
/* What buf holds, to the file */
bool fw_flush(struct fw *w);
/* Writes go on from offset, as a header written again over its place */
bool fw_seek(struct fw *w, off_t offset);
/* Closes it and, with ok and every write gone well, puts it in place by
   its policy; else removes it, the file before kept. True if it's in place.
   Either way the file under the name is whole. */
bool fw_close(struct fw *w, bool ok);
/* fw_close() in two: closed, true if every write and the close went well;
   then put in place, as once something else is noted first */
bool fw_end(struct fw *w);
bool fw_place(const char *path, enum fw_policy policy, bool ok);

/* An FW_KEEP file as a cut left it: put right at boot, before it is read
   or written. A "<path>.tmp" is removed, never known whole; a "<path>.new"
   takes the name; without one, a "<path>.old" does if the name has none. */
void fw_recover(const char *path);
/* What a cut can leave of an FW_KEEP file in a folder: its name with
   ".tmp", ".new" or ".old" after it, file the name without that */
bool fw_leftover(const char *name, char *file, size_t size);
/* At boot, fw_recover() of each of files, at most 32, all in dir, that a
   cut left something of: one reading of the folder for all. whole_new, if
   not NULL, says whether a "<file>.new" may take the name, as an older
   firmware may have left one of its own there. */
void fw_recover_files(const char *dir, const char *const *files, int count,
                      bool (*whole_new)(const char *new));

/* A full disk anywhere in the writes is library_write_failed()'s (misc.c),
   which Updating Library tells */

#endif /* _FILE_WRITE_H_ */
