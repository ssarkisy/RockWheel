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
#include "string-extra.h"
#include "config.h"
#include "system.h"
#include "file.h"
#include "font.h"
#include "rbunicode.h"
#include "lyrics.h"

/* as read from the file, and as UTF-8 which can take up to twice as much */
#define LYRICS_RAW_SIZE  6144
#define LYRICS_TEXT_SIZE (2 * LYRICS_RAW_SIZE)
#define LYRICS_MAX_LINES 512

static unsigned char raw[LYRICS_RAW_SIZE];
static char text[LYRICS_TEXT_SIZE];
static struct {
    unsigned short start, len; /* in text */
} lines[LYRICS_MAX_LINES];
static int line_count;
static int top_line;
static int rows_shown = 1;
/* what the lines are of */
static struct {
    char path[MAX_PATH];
    off_t pos;
    int width, font, height;
} loaded;

static bool read_lyrics(const struct mp3entry *id3)
{
    const struct embedded_lyrics *lyrics = &id3->embedded_lyrics;
    unsigned char *end = (unsigned char *)text;
    int n = 0;

    int fd = open(id3->path, O_RDONLY);
    if (fd < 0)
        return false;
    if (lseek(fd, lyrics->pos, SEEK_SET) == lyrics->pos)
        n = read(fd, raw, MIN(lyrics->size, LYRICS_RAW_SIZE));
    close(fd);
    if (n <= 0)
        return false;

    switch (lyrics->encoding)
    {
        case CHAR_ENC_UTF_16_LE:
        case CHAR_ENC_UTF_16_BE:
            end = utf16decode(raw, end, n / 2, sizeof(text) - 1,
                              lyrics->encoding == CHAR_ENC_UTF_16_LE);
            break;
        case CHAR_ENC_ISO_8859_1:
            end = iso_decode_ex(raw, end, -1, n, sizeof(text) - 1);
            break;
        default:
            memcpy(text, raw, n);
            end += n;
            break;
    }
    *end = '\0'; /* a nil in the text ends it there too */
    return true;
}

static void add_line(const char *start, int len)
{
    if (line_count < LYRICS_MAX_LINES)
    {
        lines[line_count].start = start - text;
        lines[line_count].len = len;
        line_count++;
    }
}

/* Break the text into lines that fit width, at its own line breaks (any of
   \r\n, \n and \r, iTunes uses \r) and else at spaces, or anywhere in a word
   too long for a line */
static void wrap_lyrics(int width, int font)
{
    const char *s = text;
    line_count = 0;

    while (*s && line_count < LYRICS_MAX_LINES)
    {
        const char *p = s;
        const char *pend = s + strcspn(s, "\r\n");

        if (p == pend)
            add_line(p, 0);
        while (p < pend)
        {
            /* what fits, measured once: measuring the line again for each
               letter took the iPod most of the time the view took to show */
            const char *q = p + font_measurestring((const unsigned char *)p,
                                                   pend - p, width,
                                                   NULL, NULL, font);
            const char *space = NULL, *e, *next;
            if (q < pend && *q == ' ')
                space = q; /* all before it fits */
            else if (q < pend)
            {
                for (const char *c = q; c > p; c--)
                {
                    if (c[-1] == ' ')
                    {
                        space = c - 1;
                        break;
                    }
                }
            }

            if (q >= pend)
                e = next = pend;
            else if (space && space > p)
            {
                e = space;
                next = space + 1;
            }
            else /* a long word, but at least a character per line */
                e = next = (q > p) ? q :
                           p + utf8seek((const unsigned char *)p, 1);

            add_line(p, e - p);
            p = next;
            while (p < pend && *p == ' ')
                p++;
        }

        s = pend;
        if (*s == '\r')
            s++;
        if (*s == '\n')
            s++;
    }

    while (line_count > 0 && lines[line_count - 1].len == 0)
        line_count--;
}

bool lyrics_load(const struct mp3entry *id3, struct viewport *vp)
{
    /* taken before the read: vp is in the skin's memory, which can move
       while the file is read */
    int width = vp->width, font = vp->font;
    int height = font_get(font)->height;
    if (!id3 || !id3->has_embedded_lyrics)
    {
        line_count = top_line = 0;
        loaded.path[0] = '\0';
        return false;
    }
    /* the view again for the same track: the lines as they were, and
       scrolled where they were */
    if (line_count > 0 && loaded.pos == id3->embedded_lyrics.pos
        && loaded.width == width && loaded.font == font
        && loaded.height == height && !strcmp(loaded.path, id3->path))
        return true;

    line_count = top_line = 0;
    loaded.path[0] = '\0';
    if (!read_lyrics(id3))
        return false;
    wrap_lyrics(width, font);
    strlcpy(loaded.path, id3->path, sizeof(loaded.path));
    loaded.pos = id3->embedded_lyrics.pos;
    loaded.width = width;
    loaded.font = font;
    loaded.height = height;
    return line_count > 0;
}

void lyrics_draw(struct screen *display, struct viewport *vp)
{
    struct viewport *last_vp = display->set_viewport(vp);
    int height = font_get(vp->font)->height;
    char buf[256];

    /* a plain background, the backdrop can have other things here; the
       text only draws its foreground as its background would be the
       backdrop again */
    unsigned fg = display->get_foreground();
    display->set_foreground(vp->bg_pattern);
    display->fillrect(0, 0, vp->width, vp->height);
    display->set_foreground(fg);
    display->set_drawmode(DRMODE_FG);

    rows_shown = MAX(1, vp->height / height);
    for (int i = 0; i < rows_shown && top_line + i < line_count; i++)
    {
        int len = MIN(lines[top_line + i].len, (int)sizeof(buf) - 1);
        memcpy(buf, text + lines[top_line + i].start, len);
        buf[len] = '\0';
        display->putsxy(0, i * height, buf);
    }

    display->set_drawmode(DRMODE_SOLID);
    display->update_viewport();
    display->set_viewport(last_vp);
}

bool lyrics_scroll(int delta)
{
    int top = MAX(0, MIN(top_line + delta, line_count - rows_shown));
    if (top == top_line)
        return false;
    top_line = top;
    return true;
}
