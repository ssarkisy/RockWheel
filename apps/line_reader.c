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
#include <string.h>
#include "config.h"
#include "file.h"
#include "line_reader.h"

void line_reader_init(struct line_reader *r, int fd, char *buf, size_t size)
{
    r->fd = fd;
    r->buf = buf;
    r->size = size;
    r->have = 0;
    r->next = buf;
    r->start = 0;
    r->at = 0;
    r->end = false;
    r->failed = false;
}

bool line_read(struct line_reader *r)
{
    if (r->end)
        return false;
    /* the line not ended, to the front for the rest of it */
    const size_t done = r->next - r->buf;
    r->start += done;
    r->have -= done;
    memmove(r->buf, r->next, r->have);
    if (r->have >= r->size - 1)
    {
        r->start += r->have;
        r->have = 0;
    }
    const ssize_t n = read(r->fd, r->buf + r->have, r->size - 1 - r->have);
    if (n <= 0)
    {
        r->end = true;
        r->failed = n < 0;
    }
    else
        r->have += n;
    r->buf[r->have] = '\0';
    r->next = r->buf;
    return true;
}

char *line_next(struct line_reader *r, size_t *len)
{
    char *line = r->next;
    char *nl = strchr(line, '\n');
    if (!nl && !r->end)
        return NULL;
    if (nl)
        *nl = '\0';
    size_t n = strlen(line);
    if (n && line[n - 1] == '\r')
        line[--n] = '\0';
    if (nl)
        r->next = nl + 1;
    else if (n)
        r->next = line + n; /* at a '\0': nothing after it */
    else
        return NULL;
    r->at = r->start + (line - r->buf);
    *len = n;
    return line;
}
