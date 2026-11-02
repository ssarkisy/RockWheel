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
#include "dir.h"
#include "file.h"
#include "plugin.h"
#include "string-extra.h"
#include "photos_panel.h"

#define PHOTOS_DIR "/Photos"
#define PHOTO_DB_NAME "Photo Database"
#define STAMP_FILE ROCKBOX_DIR "/photos_panel.dat"

static int remembered_count = -1;

static uint32_t le32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Reads no more of the database than the count in the header of its list of images */
int photos_count(void)
{
    unsigned char h[24];
    int count = 0;
    const int fd = open(PHOTOS_DIR "/" PHOTO_DB_NAME, O_RDONLY);
    if (fd < 0)
        return 0;
    if (read(fd, h, sizeof(h)) == sizeof(h) && !memcmp(h, "mhfd", 4))
    {
        off_t pos = le32(h + 4);
        const int sections = le32(h + 20);
        for (int i = 0; i < sections; i++)
        {
            if (lseek(fd, pos, SEEK_SET) != pos || read(fd, h, 14) != 14
                || memcmp(h, "mhsd", 4))
                break;
            const uint32_t header = le32(h + 4), total = le32(h + 8);
            if (h[12] == 1)
            {
                if (lseek(fd, pos + header, SEEK_SET) == pos + (off_t)header
                    && read(fd, h, 12) == 12 && !memcmp(h, "mhli", 4))
                    count = le32(h + 8);
                break;
            }
            if (total == 0)
                break;
            pos += total;
        }
    }
    close(fd);
    return count;
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

bool photos_panel_stale(void)
{
    uint32_t now[2], made[2] = { 0, 0 };
    database_stamp(now);
    const int fd = open(STAMP_FILE, O_RDONLY);
    if (fd >= 0)
    {
        if (read(fd, made, sizeof(made)) != sizeof(made))
            made[0] = made[1] = 0;
        close(fd);
    }
    return memcmp(now, made, sizeof(now)) != 0;
}

void photos_panel_update(void)
{
    uint32_t now[2];
    database_stamp(now);
    remembered_count = -1;

    if (photos_panel_count() > 0)
    {
        plugin_load(PLUGIN_APPS_DIR "/photos.rock", PHOTOS_PANEL_MAKE);
        /* it's tried again at the next boot */
        if (!file_exists(PHOTOS_PANEL_FILE))
            return;
    }
    else
        remove(PHOTOS_PANEL_FILE);

    const int fd = open(STAMP_FILE, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if (fd >= 0)
    {
        write(fd, now, sizeof(now));
        close(fd);
    }
}
