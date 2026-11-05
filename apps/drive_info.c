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

/* What the drive says of itself, in DRIVE_INFO_FILE. For an adapter for SD cards
 * that is flash without saying so, how soon it read after power-on
 * (firmware/drivers/ata.c): kept, it is flash from the boot on, rather than from
 * the first time its power was taken in each. And the words of its IDENTIFY,
 * without those that would tell the very drive, and where the volume and its
 * clusters begin: what someone whose iPod is slow can send. The file is written
 * at boot only for a drive it isn't of, and at USB and at power off only if the
 * time is new, never while music plays. A drive is told from another by its
 * make, firmware and size, not by all its words: IDENTIFY read again after a
 * wake isn't as it was at boot. */

#include "config.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "system.h"
#include "file.h"
#include "ata.h"
#include "storage.h"
#include "fat.h"
#include "crc32.h"
#include "rbpaths.h"
#include "misc.h"
#include "drive_info.h"

#define DRIVE_INFO_FILE ROCKBOX_DIR "/drive.txt"

static uint32_t written_key; /* of the drive the file is of, 0 for none */
static int written_ms = -1;  /* the power-on time in it */

/* firmware, model and size: another drive of the make and size has it too, and
   is the same flash */
static uint32_t drive_key(const unsigned short *id)
{
    uint32_t crc = crc_32(&id[23], 24 * sizeof(*id), 0xffffffff);
    crc = crc_32(&id[60], 2 * sizeof(*id), crc);
    crc = crc_32(&id[100], 4 * sizeof(*id), crc);
    return crc ? crc : 1;
}

/* the words that tell the very drive, or might */
static bool own_word(int i)
{
    return (i >= 10 && i <= 19)    /* serial number */
        || (i >= 108 && i <= 111)  /* world wide name */
        || (i >= 129 && i <= 159)  /* the vendor's own */
        || (i >= 176 && i <= 205)  /* the media's serial number */
        || i == 255;               /* checksum, of the serial number too */
}

/* an IDENTIFY string, two letters a word, without the spaces after it */
static void id_string(char *s, const unsigned short *id, int first, int words)
{
    int n = 0;
    for (int i = first; i < first + words; i++)
    {
        s[n++] = id[i] >> 8;
        s[n++] = id[i] & 0xff;
    }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\0'))
        n--;
    s[n] = '\0';
    for (int i = 0; i < n; i++)
        if (s[i] < ' ' || s[i] > '~')
            s[i] = '?';
}

static void write_info(const unsigned short *id, uint32_t key)
{
    int fd = whole_file_open(DRIVE_INFO_FILE);
    if (fd < 0)
        return;

    char model[41], firmware[9];
    id_string(model, id, 27, 20);
    id_string(firmware, id, 23, 4);
    struct storage_info info;
    storage_get_info(0, &info);
    const int ms = ata_power_on_ms();

    char text[512];
    int len = snprintf(text, sizeof(text),
             "# What the iPod's drive says of itself, without its serial number\n"
             "key: %08lx\nmodel: %s\nfirmware: %s\n"
             "sectors: %llu of %u bytes\n"
             "flash by what it says: %s\n"
             "power-on: %d\n",
             (unsigned long)key, model, firmware,
             (unsigned long long)info.num_sectors, info.sector_size,
             ata_identify_isssd() ? "yes" : "no", ms);
    uint64_t start, data;
    if (len >= 0 && len < (int)sizeof(text)
        && fat_get_offsets(IF_MV(0,) &start, &data))
        len += snprintf(text + len, sizeof(text) - len,
                        "volume at: %llu\nclusters at: %llu\ncluster: %u\n",
                        (unsigned long long)start, (unsigned long long)data,
                        fat_get_cluster_size(IF_MV(0)));
    bool ok = len < (int)sizeof(text) && write(fd, text, len) == len;
    for (int i = 0; ok && i < ATA_IDENTIFY_WORDS; i += 8)
    {
        char line[48];
        int n = snprintf(line, sizeof(line), "w%03d:", i);
        for (int j = i; j < i + 8; j++)
            n += snprintf(line + n, sizeof(line) - n, " %04x",
                          own_word(j) ? 0 : id[j]);
        line[n++] = '\n';
        ok = write(fd, line, n) == n;
    }
    /* not written, it's tried again as USB connects */
    if (!whole_file_close(fd, DRIVE_INFO_FILE, ok))
        return;

    written_key = key;
    written_ms = ms;
}

void drive_info_init(void)
{
    const unsigned short *id = ata_get_identify();
    const uint32_t key = drive_key(id);

    int fd = open(DRIVE_INFO_FILE, O_RDONLY);
    if (fd >= 0)
    {
        char line[64];
        uint32_t file_key = 0;
        int ms = -1;
        while (ms < 0 && read_line(fd, line, sizeof(line)) > 0)
        {
            char *name, *value;
            if (!settings_parseline(line, &name, &value))
                continue;
            if (!strcmp(name, "key"))
                file_key = strtoul(value, NULL, 16);
            else if (!strcmp(name, "power-on"))
                ms = atoi(value);
        }
        close(fd);
        if (file_key == key && ms >= 0)
        {
            written_key = key;
            written_ms = ms;
            ata_set_power_on_ms(ms);
        }
    }

    if (written_key != key)
        write_info(id, key);
}

void drive_info_flush(void)
{
    const unsigned short *id = ata_get_identify();
    const uint32_t key = drive_key(id);
    if (key != written_key || ata_power_on_ms() != written_ms)
        write_info(id, key);
}
