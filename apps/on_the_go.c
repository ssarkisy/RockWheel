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
#include <stdio.h>
#include <string.h>
#include "file.h"
#include "misc.h"
#include "plugin.h"
#include "string-extra.h"
#include "playlist.h"
#include "on_the_go.h"
#ifdef HAVE_TAGCACHE
#include "tagtree.h"
#include "library_playlists.h"
#endif

bool otg_add_file(const char *path)
{
    int fd = open(OTG_FILE, O_CREAT|O_WRONLY|O_APPEND, 0666);
    if (fd < 0)
        return false;
    bool ok = fdprintf(fd, "%s\n", path) > 0;
    if (close(fd) < 0)
        ok = false;
    return ok;
}

/* A playlist's songs, from the plugin buffer's first half, read a block at a
   time, into its second, written a block at a time: a read and a write a song
   took long on the iPod. Its notes left out, its paths from its own folder
   made whole. */
bool otg_add_playlist(const char *playlist)
{
    if (!strcasecmp(playlist, OTG_FILE))
        return false;
#ifdef HAVE_TAGCACHE
    /* a smart one as its screen has it */
    lp_sync(playlist);
#endif
    size_t size;
    char *buf = plugin_get_buffer(&size);
    const size_t half = size / 2;
    if (!buf || half < 2 * MAX_PATH)
        return false;
    char *in = buf, *out = buf + half;
    size_t used = 0;

    int fd = open_utf8(playlist, O_RDONLY);
    if (fd < 0)
        return false;
    int otg = open(OTG_FILE, O_CREAT|O_WRONLY|O_APPEND, 0666);
    if (otg < 0)
    {
        close(fd);
        return false;
    }

    size_t dirlen = 0;
    const char *slash = strrchr(playlist, '/');
    if (slash)
        dirlen = slash + 1 - playlist;

    bool ok = true;
    size_t have = 0;
    for (;;)
    {
        const ssize_t n = read(fd, in + have, half - have - 1);
        if (n < 0)
        {
            ok = false;
            break;
        }
        have += n;
        in[have] = '\0';
        const bool end = n == 0;
        char *line = in;
        for (;;)
        {
            char *nl = strchr(line, '\n');
            if (!nl && !end)
                break;
            if (nl)
                *nl = '\0';
            size_t len = strlen(line);
            if (len && line[len - 1] == '\r')
                line[--len] = '\0';
            if (len && line[0] != '#')
            {
                const size_t whole = (line[0] == '/' ? 0 : dirlen) + len;
                if (whole < MAX_PATH)
                {
                    if (used + whole + 1 > half)
                    {
                        ok = ok && write(otg, out, used) == (ssize_t)used;
                        used = 0;
                    }
                    if (line[0] != '/')
                    {
                        memcpy(out + used, playlist, dirlen);
                        used += dirlen;
                    }
                    memcpy(out + used, line, len);
                    used += len;
                    out[used++] = '\n';
                }
            }
            if (!nl)
                break;
            line = nl + 1;
        }
        if (end)
            break;
        /* the line not ended, to the front for the rest of it */
        have = in + have - line;
        memmove(in, line, have);
        if (have >= half - 1)
            have = 0; /* longer than any path */
    }
    if (used)
        ok = ok && write(otg, out, used) == (ssize_t)used;
    if (close(otg) < 0)
        ok = false;
    close(fd);
    return ok;
}

bool otg_add_selection(void)
{
#ifdef HAVE_TAGCACHE
    return tagtree_add_to_file(OTG_FILE);
#else
    return false;
#endif
}

void otg_clear(void)
{
    remove(OTG_FILE);
}

/* On-The-Go as its screen left it, songs removed: written a block at a time
   to a file of its own, which then takes its place. A playlist's own saving
   stops the iPod with a panic if the disk can't take it. */
bool otg_save(struct playlist_info *playlist)
{
    const int n = playlist_amount_ex(playlist);
    if (n <= 0)
    {
        otg_clear();
        return true;
    }
    int fd = open(OTG_FILE ".tmp", O_CREAT|O_WRONLY|O_TRUNC, 0666);
    bool ok = fd >= 0;
    char buf[1024];
    size_t used = 0;
    struct playlist_track_info info;
    for (int i = 0; ok && i < n; i++)
    {
        if (playlist_get_track_info(playlist, i, &info) < 0)
        {
            ok = false;
            break;
        }
        const size_t len = strlen(info.filename);
        if (len + 1 > sizeof(buf))
            continue;
        if (used + len + 1 > sizeof(buf))
        {
            ok = write(fd, buf, used) == (ssize_t)used;
            used = 0;
        }
        memcpy(buf + used, info.filename, len);
        buf[used + len] = '\n';
        used += len + 1;
    }
    if (ok && used)
        ok = write(fd, buf, used) == (ssize_t)used;
    if (fd >= 0 && close(fd) < 0)
        ok = false;
    /* in place of the old one at once: removed first, a power cut between
       lost both */
    if (ok)
        ok = rename(OTG_FILE ".tmp", OTG_FILE) == 0;
    if (!ok)
        remove(OTG_FILE ".tmp");
    return ok;
}

bool otg_empty(void)
{
    int fd = open(OTG_FILE, O_RDONLY);
    if (fd < 0)
        return true;
    const off_t size = ffilesize(fd);
    close(fd);
    return size <= 0;
}
