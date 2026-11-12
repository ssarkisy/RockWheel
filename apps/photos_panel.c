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
#include <stdint.h>
#include <string.h>
#include "config.h"
#include "core_alloc.h"
#include "crc32.h"
#include "dir.h"
#include "file.h"
#include "plugin.h"
#include "system.h"
#include "string-extra.h"
#include "photos_panel.h"
#include "misc.h"
#include "kernel.h"
#include "settings.h"
#include "storage.h"
#include "viewport.h"
#include "skin_engine/skin_engine.h"

#define PHOTOS_DIR "/Photos"
#define PHOTO_DB_NAME "Photo Database"
#define STAMP_FILE ROCKBOX_DIR "/photos_panel.dat"

static int remembered_count = -1;

static uint32_t le32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The place of the section with the list of images, and its sizes, or -1 */
static off_t images_section(int fd, uint32_t *header, uint32_t *total)
{
    unsigned char h[24];
    if (read(fd, h, sizeof(h)) != sizeof(h) || memcmp(h, "mhfd", 4))
        return -1;
    off_t pos = le32(h + 4);
    const int sections = le32(h + 20);
    for (int i = 0; i < sections; i++)
    {
        if (lseek(fd, pos, SEEK_SET) != pos || read(fd, h, 14) != 14
            || memcmp(h, "mhsd", 4))
            return -1;
        *header = le32(h + 4);
        *total = le32(h + 8);
        if (h[12] == 1)
            return pos;
        if (*total == 0)
            return -1;
        pos += *total;
    }
    return -1;
}

/* Reads no more of the database than the count in the header of its list of images */
int photos_count(void)
{
    unsigned char h[12];
    int count = 0;
    uint32_t header, total;
    const int fd = open(PHOTOS_DIR "/" PHOTO_DB_NAME, O_RDONLY);
    if (fd < 0)
        return 0;
    const off_t pos = images_section(fd, &header, &total);
    if (pos >= 0 && lseek(fd, pos + header, SEEK_SET) == pos + (off_t)header
        && read(fd, h, 12) == 12 && !memcmp(h, "mhli", 4))
        count = le32(h + 8);
    close(fd);
    return count;
}

/* Finder writes the database again at every sync, with its header and the albums a little
   different, but the list of images byte for byte the same when the photos are. This
   tells them apart, 0 if it can't. */
static uint32_t images_hash(void)
{
    enum { BUF_SIZE = 16384 };
    uint32_t hash = 0, header, total;
    const int fd = open(PHOTOS_DIR "/" PHOTO_DB_NAME, O_RDONLY);
    if (fd < 0)
        return 0;
    const off_t pos = images_section(fd, &header, &total);
    const int handle = pos >= 0 ? core_alloc(BUF_SIZE) : 0;
    if (handle > 0)
    {
        core_pin(handle);
        char *buf = core_get_data(handle);
        cpu_boost(true);
        hash = 0xffffffff;
        lseek(fd, pos, SEEK_SET);
        while (total > 0)
        {
            const ssize_t got = read(fd, buf, MIN(total, BUF_SIZE));
            if (got <= 0)
            {
                hash = 0;
                break;
            }
            hash = crc_32(buf, got, hash);
            total -= got;
        }
        cpu_boost(false);
        core_unpin(handle);
        core_free(handle);
    }
    close(fd);
    return hash;
}

int photos_panel_count(void)
{
    if (remembered_count < 0)
        remembered_count = photos_count();
    return remembered_count;
}

/* Size and time of the database, both 0 without one */
static void database_stamp(uint32_t stamp[2])
{
    stamp[0] = stamp[1] = 0;
    DIR *dir = opendir(PHOTOS_DIR);
    if (!dir)
        return;
    struct dirent *entry;
    while ((entry = readdir(dir)))
    {
        if (strcasecmp(entry->d_name, PHOTO_DB_NAME))
            continue;
        struct dirinfo info = dir_get_info(dir, entry);
        stamp[0] = info.size;
        stamp[1] = info.mtime;
        break;
    }
    closedir(dir);
}

/* How the collages are made, in their stamp: those made another way, as on
   the panel's gradient of one theme, or without the albums', are made again
   once */
#define PANEL_VERSION 3

static void write_stamp(const uint32_t stamp[2], uint32_t hash)
{
    const uint32_t all[4] = { stamp[0], stamp[1], hash, PANEL_VERSION };
    whole_file_write(STAMP_FILE, all, sizeof(all));
}

bool photos_panel_stale(void)
{
    uint32_t now[2], made[4] = { 0, 0, 0, 0 };
    database_stamp(now);
    const int fd = open(STAMP_FILE, O_RDONLY);
    if (fd >= 0)
    {
        if (read(fd, made, sizeof(made)) != sizeof(made)
            || made[3] != PANEL_VERSION)
            made[0] = made[1] = made[2] = 0;
        close(fd);
    }
    /* Not stale with the database as it was when the collages were made, or
       with none and nothing made of one. Written again, its albums may be
       others with the same photos: photos_panel_update() finds what there
       is to make. */
    return memcmp(now, made, sizeof(now)) != 0;
}

bool photos_panel_update(void (*making)(void))
{
    uint32_t now[2], made[4] = { 0, 0, 0, 0 };
    database_stamp(now);
    remembered_count = -1;
    const int fd = open(STAMP_FILE, O_RDONLY);
    if (fd >= 0)
    {
        if (read(fd, made, sizeof(made)) != sizeof(made)
            || made[3] != PANEL_VERSION)
            made[2] = 0;
        close(fd);
    }
    const uint32_t hash = images_hash();

    if (photos_panel_count() > 0 && made[2] && hash == made[2]
        && file_exists(PHOTOS_PANEL_FILE))
    {
        /* Finder writes the database again at every sync: with the same
           photos the main menu's collage and the skins stay, and the albums'
           are made again only if the plugin reads other albums than they
           were made of */
        plugin_load(PLUGIN_APPS_DIR "/photos.rock", PHOTOS_PANELS_MAKE);
        /* not made, as for want of room, the plugin leaves no file of them:
           tried again at the next boot, the stamp as it was */
        if (file_exists(PHOTOS_PANELS_FILE))
            write_stamp(now, hash);
        return false;
    }
    if (photos_panel_count() > 0)
    {
        /* other photos: the collages are made, which takes a while and
           is said; a sync that wrote the same photos again says nothing */
        if (making)
            making();
        /* the old one put aside: a load refused, as with a plugin open, left
           it there, and its stamp was written as if it were made again */
        remove(PHOTOS_PANEL_FILE ".old"); /* of a make cut short */
        rename(PHOTOS_PANEL_FILE, PHOTOS_PANEL_FILE ".old");
        plugin_load(PLUGIN_APPS_DIR "/photos.rock", PHOTOS_PANEL_MAKE);
        /* it's tried again at the next boot */
        if (!file_exists(PHOTOS_PANEL_FILE))
        {
            rename(PHOTOS_PANEL_FILE ".old", PHOTOS_PANEL_FILE);
            return false;
        }
        remove(PHOTOS_PANEL_FILE ".old");
        /* The main menu's made and the albums' not: the stamp of these
           photos and of no database's time, so that the next boot tries the
           albums' alone again. The skins are loaded for the new collage. */
        if (!file_exists(PHOTOS_PANELS_FILE))
        {
            const uint32_t none[2] = { 0, 0 };
            write_stamp(none, hash);
            return true;
        }
    }
    else
    {
        /* no photos: the skins only if they have a collage to lose, not at
           every sync that writes an empty database again */
        const bool had = file_exists(PHOTOS_PANEL_FILE);
        remove(PHOTOS_PANEL_FILE);
        remove(PHOTOS_PANELS_FILE);
        write_stamp(now, hash);
        return had;
    }

    write_stamp(now, hash);
    return true;
}

static int shown_count = -1;

int photos_panel_shown_count(void)
{
    return shown_count >= 0 ? shown_count : photos_panel_count();
}

bool photos_panel_show(const fb_data *pixels, int count)
{
    if (pixels && !skin_photos_panel_put(pixels, PHOTOS_PANEL_W, PHOTOS_PANEL_H))
        return false;
    if (!pixels && count != 0)
        return false;
    shown_count = count;
    /* the list beside it is drawn over the theme next, by the event the theme
       drawn whole sends, and the two go on the screen together */
    viewportmanager_redraw_unshown();
    return true;
}

void photos_panel_show_end(void)
{
    shown_count = -1;
}

/* as the cover beside the menus has it (disk_free() in menu_cover.c) */
bool photos_panel_disk_free(void)
{
#if !defined(SIMULATOR) && defined(HAVE_DISK_STORAGE) \
    && (CONFIG_STORAGE & STORAGE_ATA)
    if (ata_disk_isssd())
        return true;
    const long spindown = global_settings.disk_spindown * HZ;
    return storage_disk_is_active()
           && (!spindown
               || TIME_BEFORE(current_tick + 2 * HZ,
                              storage_last_disk_activity() + spindown));
#else
    return true;
#endif
}
