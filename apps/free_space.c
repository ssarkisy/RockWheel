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

/* The volume's free space as the FAT has it. The file system takes FAT32's
   count of free clusters from the FSInfo sector at the mount, and keeps it by
   its own writes; a computer needn't keep that count right, it's a hint, and
   after a sync About said 119 GB free of a card that had 60. So the FAT
   itself is counted again after each USB session, and at the first boot of a
   firmware, as one before it may have left FSInfo off; the count found is
   written to FSInfo (fat.c's fat_recount_free()). After USB it comes before
   the library's check, so that what goes by the free space has it right from
   then on: the room the import, Cover Flow's covers, the lists' covers and
   Songs' files are checked for, whose lack is the Not Enough Space screen;
   About; the theme's disk tags and its disk ring. */

#include "config.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "rbpaths.h"
#include "crc32.h"
#include "version.h"
#include "fat.h"
#include "mv.h"
#include "audio.h"
#include "plugin.h"
#include "health_log.h"
#include "free_space.h"

/* The firmware whose count was made: a boot of another counts again, and so
   does a boot with no file, as after a USB session whose count wasn't made:
   one that ended in Apple's disk mode, which reboots (usb.c), or in a
   switch-off or a reset before the count. Removed as USB connects, before the
   main thread lets the disk go, written after the next count. */
#define MARK_FILE   ROCKBOX_DIR "/free_count.dat"
#define MARK_MAGIC  0x46524545  /* "FREE" */

struct free_mark
{
    uint32_t magic;
    uint32_t firmware;  /* crc_32() of rbversion */
};

/* a step's read: 256 sectors, fat.c's most */
#define STEP_BYTES  (128 * 1024)

static bool mark_looked;    /* the file read, at the first call */
static bool mark_done;      /* and it says this firmware's count was made */
static bool owed;
static const char *owed_for = "at boot";

static uint32_t firmware(void)
{
    return crc_32(rbversion, strlen(rbversion), 0xffffffff);
}

static void mark_look(void)
{
    if (mark_looked)
        return;
    mark_looked = true;
    struct free_mark m;
    const int fd = open(MARK_FILE, O_RDONLY);
    mark_done = fd >= 0 && read(fd, &m, sizeof(m)) == sizeof(m)
                && m.magic == MARK_MAGIC && m.firmware == firmware();
    if (fd >= 0)
        close(fd);
    if (!mark_done)
        owed = true;
}

void free_space_usb_connected(void)
{
    mark_look();
    if (mark_done && remove(MARK_FILE) == 0)
        mark_done = false;
}

void free_space_usb_ended(void)
{
    mark_look();
    owed = true;
    owed_for = "after USB";
}

static void note(const struct fat_recount *r)
{
    const unsigned long cluster = volume_get_cluster_size(IF_MV(0));
    char off[64] = ", as kept";
    if (r->found != r->kept)
    {
        const bool fewer = r->found < r->kept;
        const unsigned long by = fewer ? r->kept - r->found
                                       : r->found - r->kept;
        /* in units of 1000, as About's */
        snprintf(off, sizeof(off), ", %lu %s than kept (%llu MB)", by,
                 fewer ? "fewer" : "more",
                 (unsigned long long)by * cluster / 1000000);
    }
    /* the count's own time, its steps', not the pauses between them; none
       where the target can't tell it */
    char took[48] = "";
    if (r->us)
        snprintf(took, sizeof(took), " in %lu ms, %lu ms of it reading",
                 r->us / 1000, r->read_us / 1000);
    health_note("fat: free clusters counted again %s: %lu%s; the FAT's %lu"
                " sectors in %lu reads%s", owed_for, r->found, off,
                r->sectors, r->reads, took);
}

bool free_space_recount(bool (*go_on)(void))
{
    mark_look();
    if (!owed)
        return true;
    /* the plugins' buffer is a plugin's while one runs; and none taken
       under music, nor kept once some starts (a dock's Play) */
    if (plugin_is_running() || (audio_status() & AUDIO_STATUS_PLAY))
        return false;
    if (go_on && !go_on())
        return false;

    size_t room = 0;
    unsigned char *buf = plugin_get_buffer(&room);
    if (!buf)
        return false;
    /* for the drive's DMA, as the file system's own buffers */
    STORAGE_ALIGN_BUFFER(buf, room);
    /* not into the little a resident plugin may leave */
    if (room < STEP_BYTES / 8)
        return false;
    room = MIN(room, STEP_BYTES);

    struct fat_recount r;
    int rc;
    cpu_boost(true);
    while ((rc = volume_recount_free(IF_MV(0,) buf, room, &r)) > 0
           && (!go_on || go_on()) && !(audio_status() & AUDIO_STATUS_PLAY))
        ;
    cpu_boost(false);
    if (rc > 0)
        return false; /* stopped: see free_space.h */

    owed = false;
    if (rc < 0)
    {
        /* owed again after the next USB session, and at each boot until a
           count is made */
        health_note("fat: free clusters not counted again %s: error %d",
                    owed_for, rc);
        return true;
    }
    note(&r);
    if (!mark_done)
    {
        const struct free_mark m = { MARK_MAGIC, firmware() };
        const int fd = open(MARK_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0)
        {
            const bool whole = write(fd, &m, sizeof(m)) == sizeof(m);
            mark_done = close(fd) == 0 && whole;
        }
    }
    return true;
}
