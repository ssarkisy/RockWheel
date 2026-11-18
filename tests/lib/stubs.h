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
#ifndef TESTS_STUBS_H
#define TESTS_STUBS_H

/* what the stubbed firmware was asked to do (stubs.c, simfs.c) */
struct stubs {
    long yields;
    long reads;         /* sim_read()s */
    long read_bytes;
    int write_failed;   /* library_write_failed() */
    int no_room;        /* library_no_room() */
    int health_notes;
    int smart_notes;    /* smart playlists noted */
};
extern struct stubs stubs;
/* tagcache_is_in_ram()'s answer */
extern bool stub_ram_loaded;

/* The disk: a folder of the Mac's, whose paths are the iPod's under it */
void simfs_root(const char *dir);
/* the host's path of an iPod's one */
const char *simfs_path(const char *path, char *buf, size_t size);

#endif
