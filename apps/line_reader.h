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
#ifndef _LINE_READER_H_
#define _LINE_READER_H_

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* A playlist's lines, read from fd a block at a time into the caller's
   buffer, a read a block:

       line_reader_init(&lines, fd, buf, size);
       while (line_read(&lines))
           while ((line = line_next(&lines, &len)))
               ...

   Each line without its '\n' and a '\r' before it; the last one, without a
   '\n', only if something is left in it. A line of size - 1 bytes or more
   before its '\n' is longer than any path: dropped a block at a time, the
   rest of it read as a line. A '\0' hides the line breaks after it in the
   buffer: the lines from its own on are dropped as one too long, or at the
   file's end its own, up to the '\0', is the last. */
struct line_reader {
    int fd;
    char *buf;
    size_t size;
    size_t have;    /* bytes in buf */
    char *next;     /* the next line's start in buf */
    off_t start;    /* buf's place in the file, from where the reader began */
    off_t at;       /* the line line_next() gave last, from there too */
    bool end;       /* read to the end, or a read failed */
    bool failed;    /* a read failed, taken as the end: the block of it has
                       the last line */
};

void line_reader_init(struct line_reader *r, int fd, char *buf, size_t size);
/* The next block after what's left of the line before: false once the end
   was read */
bool line_read(struct line_reader *r);
/* the next line of the block and its length, NULL past its last */
char *line_next(struct line_reader *r, size_t *len);

#endif /* _LINE_READER_H_ */
