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
#include "config.h"
#include <string.h>
#include <ctype.h>
#include "system.h"
#include "file.h"
#include "dir.h"
#include "core_alloc.h"
#include "itunesdb.h"
#include "synced_covers.h"

#ifdef ITUNESDB_HAVE_ART

#define DESCRIPTION_FILE    "/iPod_Control/Device/SysInfoExtended"
/* as it was before the format was added; never read again */
#define DESCRIPTION_KEPT    DESCRIPTION_FILE ".original"
#define DESCRIPTION_NEW     DESCRIPTION_FILE ".new"
/* what USB gives of the file (usb_storage.c): 62 pages of 248 bytes */
#define DESCRIPTION_MAX     15376
#define COVERS_DIR          "/iPod_Control/Artwork"
#define COVERS_DB           COVERS_DIR "/ArtworkDB"
/* The old artwork until a sync has made the new. Without its ArtworkDB,
   which goes first, it is no one's artwork and is only removed. */
#define ASIDE_DIR           "/iPod_Control/Artwork.set-aside"
#define ASIDE_DB            ASIDE_DIR "/ArtworkDB"
#define ASIDE_FILES         64 /* more ithmb files than Finder makes */

#define KEY_SPECS   "<key>ImageSpecifications</key>"
#define KEY_COVERS  "<key>AlbumArt</key>"
#define KEY_FORMAT  "<key>1024</key>"
#define ID_FORMAT   "<integer>1024</integer>"

/* where s is in [from, to), the only one there, or NULL */
static const char *only(const char *from, const char *to, const char *s)
{
    const char *at = strstr(from, s);
    if (!at || at + strlen(s) > to)
        return NULL;
    const char *next = strstr(at + 1, s);
    return next && next + strlen(s) <= to ? NULL : at;
}

static const char *past_space(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

/* The list after a key: from past its <array> to its </array>, with no
   list inside it; false for any other shape */
static bool list_after(const char *text, const char *end, const char *key,
                       const char **from, const char **to)
{
    const char *p = only(text, end, key);
    if (!p)
        return false;
    p = past_space(p + strlen(key));
    if (strncmp(p, "<array>", 7))
        return false;
    p += 7;
    const char *close = strstr(p, "</array>");
    const char *inner = strstr(p, "<array");
    if (!close || (inner && inner < close))
        return false;
    *from = p;
    *to = close;
    return true;
}

/* The one change Finder takes: the model's own photo format 1024 (320x240),
   as ImageSpecifications has it, byte for byte, at the end of the AlbumArt
   list. A format the model hasn't, or another size of one it has, and
   Finder refuses the iPod. text ends in a zero at text[len]. */
static enum synced_covers_description
description_look(const char *text, size_t len, const char **entry,
                 size_t *entry_len, const char **at)
{
    const char *end = text + len;
    const char *from, *to;
    /* XML and whole; a binary list has zeros */
    if (len < 16 || strlen(text) != len || strncmp(text, "<?xml", 5))
        return DESCRIPTION_UNKNOWN;
    /* nothing that can hold the look of a list's end */
    if (strstr(text, "<!--") || strstr(text, "<![CDATA["))
        return DESCRIPTION_UNKNOWN;
    const char *last = end;
    while (last > text && isspace((unsigned char)last[-1]))
        last--;
    if (last - text < 8 || strncmp(last - 8, "</plist>", 8))
        return DESCRIPTION_UNKNOWN;
    if (!list_after(text, end, KEY_COVERS, &from, &to))
        return DESCRIPTION_UNKNOWN;
    /* a list of formats by their keys, the last one closed right before
       the list's end */
    const char *tail = to;
    while (tail > from && isspace((unsigned char)tail[-1]))
        tail--;
    if (strncmp(past_space(from), "<key>", 5)
        || tail - from < 7 || strncmp(tail - 7, "</dict>", 7))
        return DESCRIPTION_UNKNOWN;
    const char *has = strstr(from, KEY_FORMAT);
    const char *id = strstr(from, ID_FORMAT);
    if ((has && has < to) || (id && id < to))
        return DESCRIPTION_READY;
    *at = to;

    if (!list_after(text, end, KEY_SPECS, &from, &to))
        return DESCRIPTION_UNKNOWN;
    const char *key = only(from, to, KEY_FORMAT);
    if (!key)
        return DESCRIPTION_UNKNOWN;
    const char *p = past_space(key + strlen(KEY_FORMAT));
    if (strncmp(p, "<dict>", 6))
        return DESCRIPTION_UNKNOWN;
    p += 6;
    const char *close = strstr(p, "</dict>");
    const char *inner = strstr(p, "<dict");
    if (!close || close > to || (inner && inner < close))
        return DESCRIPTION_UNKNOWN;
    /* the frames the firmware reads: 320x240 of this format */
    id = strstr(p, ID_FORMAT);
    const char *width = strstr(p, "<integer>320</integer>");
    const char *height = strstr(p, "<integer>240</integer>");
    if (!id || id > close || !width || width > close
        || !height || height > close)
        return DESCRIPTION_UNKNOWN;
    /* with the space after it, up to the next entry or the list's end */
    close = past_space(close + 7);
    if (close > to)
        return DESCRIPTION_UNKNOWN;
    *entry = key;
    *entry_len = close - key;
    if (len + *entry_len > DESCRIPTION_MAX)
        return DESCRIPTION_UNKNOWN;
    return DESCRIPTION_CHANGED;
}

static bool put(int fd, const char *data, size_t len)
{
    return write(fd, data, len) == (ssize_t)len;
}

/* The file as it was is kept beside it, once; the new one is written whole
   under another name and takes the old one's place by a rename: at a cut
   the description is the old or the new */
static bool description_write(const char *text, size_t len, const char *entry,
                              size_t entry_len, const char *at)
{
    if (!file_exists(DESCRIPTION_KEPT))
    {
        int fd = open(DESCRIPTION_KEPT ".tmp", O_WRONLY | O_CREAT | O_TRUNC,
                      0666);
        if (fd < 0)
            return false;
        bool ok = put(fd, text, len);
        ok = close(fd) == 0 && ok;
        if (!ok || rename(DESCRIPTION_KEPT ".tmp", DESCRIPTION_KEPT) != 0)
        {
            remove(DESCRIPTION_KEPT ".tmp");
            return false;
        }
    }
    int fd = open(DESCRIPTION_NEW, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    bool ok = put(fd, text, at - text) && put(fd, entry, entry_len)
              && put(fd, at, text + len - at);
    ok = close(fd) == 0 && ok;
    if (!ok || rename(DESCRIPTION_NEW, DESCRIPTION_FILE) != 0)
    {
        remove(DESCRIPTION_NEW);
        return false;
    }
    return true;
}

enum synced_covers_description synced_covers_describe(void)
{
    enum synced_covers_description is = DESCRIPTION_UNKNOWN;
    int fd = open(DESCRIPTION_FILE, O_RDONLY);
    if (fd < 0)
        return DESCRIPTION_NONE;
    const off_t size = ffilesize(fd);
    int handle = 0;
    if (size > 0 && size <= DESCRIPTION_MAX)
        handle = core_alloc(size + 1);
    if (handle > 0)
    {
        core_pin(handle);
        char *text = core_get_data(handle);
        const char *entry = NULL, *at = NULL;
        size_t entry_len = 0;
        const bool whole = read(fd, text, size) == size;
        close(fd);
        fd = -1;
        text[size] = '\0';
        if (whole)
            is = description_look(text, size, &entry, &entry_len, &at);
        if (is == DESCRIPTION_CHANGED
            && !description_write(text, size, entry, entry_len, at))
            is = DESCRIPTION_UNKNOWN;
        core_unpin(handle);
        core_free(handle);
    }
    if (fd >= 0)
        close(fd);
    return is;
}

/* What goes from the folder set aside besides ArtworkDB:
   F<format>_<number>.ithmb, as Finder names the frames' files, and what a
   Mac leaves in a folder it was shown, "._" files and .DS_Store */
static bool removable_name(const char *name)
{
    const char *p = name;
    if (!strncmp(name, "._", 2) || !strcmp(name, ".DS_Store"))
        return true;
    if (*p++ != 'F' || !isdigit((unsigned char)*p))
        return false;
    while (isdigit((unsigned char)*p))
        p++;
    if (*p++ != '_' || !isdigit((unsigned char)*p))
        return false;
    while (isdigit((unsigned char)*p))
        p++;
    return !strcmp(p, ".ithmb");
}

/* The only removal: ArtworkDB, the frames' files and a Mac's own leavings
   in the folder set aside, by their names, and the folder if nothing else
   is in it. No folder is gone into. ArtworkDB first: cut short, what is
   left is known as to be removed, and isn't put back. */
static void aside_remove(void)
{
    if (file_exists(ASIDE_DB) && remove(ASIDE_DB) != 0)
        return;
    for (int i = 0; i < ASIDE_FILES; i++)
    {
        char path[sizeof(ASIDE_DIR) + 40];
        path[0] = '\0';
        DIR *dir = opendir(ASIDE_DIR);
        if (!dir)
            return;
        struct dirent *entry;
        while ((entry = readdir(dir)))
        {
            struct dirinfo info = dir_get_info(dir, entry);
            const size_t len = strlen(entry->d_name);
            if (!(info.attribute & ATTR_DIRECTORY) && len < 40
                && removable_name(entry->d_name))
            {
                memcpy(path, ASIDE_DIR "/", sizeof(ASIDE_DIR));
                memcpy(path + sizeof(ASIDE_DIR), entry->d_name, len + 1);
                break;
            }
        }
        closedir(dir);
        if (!path[0] || remove(path) != 0)
            break;
    }
    rmdir(ASIDE_DIR);
}

bool synced_covers_waiting(void)
{
    return file_exists(ASIDE_DB);
}

bool synced_covers_present(void)
{
    return file_exists(COVERS_DB);
}

bool synced_covers_set_aside(void)
{
    if (file_exists(ASIDE_DB))
        return false;
    /* what a cut left of one removed */
    if (dir_exists(ASIDE_DIR))
    {
        aside_remove();
        if (dir_exists(ASIDE_DIR))
            return false;
    }
    return rename(COVERS_DIR, ASIDE_DIR) == 0;
}

bool synced_covers_put_back(void)
{
    if (!file_exists(ASIDE_DB))
        return false;
    /* a folder the computer made with nothing in it yet; one with files is
       the new artwork, and stays */
    if (dir_exists(COVERS_DIR) && rmdir(COVERS_DIR) != 0)
        return false;
    /* cut before the import: the next boot makes the covers' index */
    itunesdb_art_due();
    return rename(ASIDE_DIR, COVERS_DIR) == 0;
}

void synced_covers_imported(bool covers)
{
    if (dir_exists(ASIDE_DIR) && (covers || !file_exists(ASIDE_DB)))
        aside_remove();
}

void synced_covers_boot(void)
{
    synced_covers_describe();
    if (dir_exists(ASIDE_DIR) && !file_exists(ASIDE_DB))
        aside_remove();
}

#else /* !ITUNESDB_HAVE_ART */
void synced_covers_boot(void) {}
#endif
