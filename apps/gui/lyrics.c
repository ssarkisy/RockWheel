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
#include <stdio.h>
#include <string.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "font.h"
#include "lcd.h"
#include "misc.h"
#include "settings.h"
#include "rbunicode.h"
#include "lyrics.h"
#include "skin_engine/cover_palette.h"

/* as read from the file, and as UTF-8 which can take up to twice as much */
#define LYRICS_RAW_SIZE  8192
#define LYRICS_TEXT_SIZE (2 * LYRICS_RAW_SIZE)
#define LYRICS_MAX_LINES 512
#define LYRICS_MAX_TIMES 512
/* .lrc files for tracks whose own names say nothing, as Finder's */
#define LYRICS_DIR       "/Lyrics"
/* the view stays where the wheel left it this long, then follows again */
#define LYRICS_HOLD      (4*HZ)

static unsigned char raw[LYRICS_RAW_SIZE];
static char text[LYRICS_TEXT_SIZE];
static struct {
    unsigned short start, len; /* in text */
    short time;                /* the timed line it shows, or -1 */
} lines[LYRICS_MAX_LINES];
static int line_count;
static int top_line;
static int rows_shown = 1;

/* Synced lyrics, as LRC has them: when each line is sung, in ms, in that
   order; none for plain text */
static struct timed_line {
    long ms;
    unsigned short start, len; /* in text */
    short line;                /* its first line in lines[] */
} times[LYRICS_MAX_TIMES];
static int time_count;
static int current = -1;       /* the line sung now, or -1 before the first */
static long hold_until;

/* what the lines are of, also when the track has none */
static struct {
    char path[MAX_PATH];
    int width, font, height;
    bool found;
} loaded;

/* The codepage of an .lrc that isn't UTF-8: the one set for tags, unless
   that is UTF-8 too, as it is by default; then Cyrillic when a third of
   the letters are high bytes, as in an old Russian file, else Western */
static int legacy_codepage(int n)
{
    if (global_settings.default_codepage != UTF_8)
        return -1;
    int high = 0, letters = 0;
    for (int i = 0; i < n; i++)
    {
        unsigned char c = raw[i];
        if (c >= 0xc0)
            high++;
        if (c >= 0xc0 || (unsigned)((c | 0x20) - 'a') < 26)
            letters++;
    }
    return high * 3 > letters ? WIN_1251 : WIN_1252;
}

/* the text from raw[] as UTF-8 */
static void decode(int n, enum character_encoding encoding)
{
    unsigned char *end = (unsigned char *)text;
    switch (encoding)
    {
        case CHAR_ENC_UTF_16_LE:
        case CHAR_ENC_UTF_16_BE:
            end = utf16decode(raw, end, n / 2, sizeof(text) - 1,
                              encoding == CHAR_ENC_UTF_16_LE);
            break;
        case CHAR_ENC_ISO_8859_1:
            /* -1: the codepage set for tags */
            end = iso_decode_ex(raw, end, -1, n, sizeof(text) - 1);
            break;
        default:
            memcpy(text, raw, n);
            end += n;
            break;
    }
    *end = '\0'; /* a nil in the text ends it there too */
}

static bool read_embedded(const struct mp3entry *id3)
{
    const struct embedded_lyrics *lyrics = &id3->embedded_lyrics;
    int n = 0;
    int fd = open(id3->path, O_RDONLY);
    if (fd < 0)
        return false;
    if (lseek(fd, lyrics->pos, SEEK_SET) == lyrics->pos)
        n = read(fd, raw, MIN(lyrics->size, LYRICS_RAW_SIZE));
    close(fd);
    if (n <= 0)
        return false;
    decode(n, lyrics->encoding);
    return true;
}

static bool valid_utf8(const unsigned char *s, int n)
{
    for (int i = 0; i < n; i++)
    {
        int extra = s[i] < 0x80 ? 0 : (s[i] & 0xe0) == 0xc0 ? 1 :
                    (s[i] & 0xf0) == 0xe0 ? 2 : (s[i] & 0xf8) == 0xf0 ? 3 : -1;
        if (extra < 0)
            return false;
        if (i + extra >= n)
            return true; /* cut by the read, as a long file is */
        for (; extra > 0; extra--)
            if ((s[++i] & 0xc0) != 0x80)
                return false;
    }
    return true;
}

static bool read_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    int n = read(fd, raw, LYRICS_RAW_SIZE);
    close(fd);
    if (n <= 0)
        return false;

    /* UTF-8 with or without its mark, UTF-16 by its mark, else a
       codepage of one byte a letter, as the old players' files are */
    if (n >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf)
    {
        memmove(raw, raw + 3, n - 3);
        decode(n - 3, CHAR_ENC_UTF_8);
    }
    else if (n >= 2 && (raw[0] == 0xff || raw[0] == 0xfe)
             && (raw[1] ^ raw[0]) == 0x01)
    {
        bool le = raw[0] == 0xff;
        memmove(raw, raw + 2, n - 2);
        decode(n - 2, le ? CHAR_ENC_UTF_16_LE : CHAR_ENC_UTF_16_BE);
    }
    else if (valid_utf8(raw, n))
        decode(n, CHAR_ENC_UTF_8);
    else
        *iso_decode_ex(raw, (unsigned char *)text, legacy_codepage(n), n,
                       sizeof(text) - 1) = '\0';
    return true;
}

/* An .lrc file for the track: of the same name beside it, else in /Lyrics
   by "artist - title" or by the title, as Finder's own file names are
   random */
static bool read_lrc(const struct mp3entry *id3)
{
    char path[MAX_PATH];
    strlcpy(path, id3->path, sizeof(path));
    char *dot = strrchr(path, '.');
    char *slash = strrchr(path, '/');
    if (dot && dot > slash && (size_t)(dot - path) + 5 <= sizeof(path))
    {
        strcpy(dot, ".lrc");
        if (file_exists(path) && read_file(path))
            return true;
    }

    if (!id3->title || !dir_exists(LYRICS_DIR))
        return false;
    for (int pass = 0; pass < 2; pass++)
    {
        int len;
        if (pass == 0)
        {
            if (!id3->artist)
                continue;
            len = snprintf(path, sizeof(path), LYRICS_DIR "/%s - %s",
                           id3->artist, id3->title);
        }
        else
            len = snprintf(path, sizeof(path), LYRICS_DIR "/%s", id3->title);
        if (len + 5 > (int)sizeof(path))
            continue;
        int dir = sizeof(LYRICS_DIR);
        fix_path_part(path, dir, len - dir);
        strcpy(path + len, ".lrc");
        if (file_exists(path) && read_file(path))
            return true;
    }
    return false;
}

/* [mm:ss], [mm:ss.x] to [mm:ss.xxx] or [mm:ss:xx] at p, or the same in <>
   as an enhanced file times each word: its length, 0 if it is none */
static int parse_time(const char *p, char open, char close, long *ms)
{
    const char *s = p;
    long min = 0, frac = 0;
    int digits = 0;
    if (*s++ != open)
        return 0;
    while (*s >= '0' && *s <= '9' && digits < 4)
    {
        min = min * 10 + (*s++ - '0');
        digits++;
    }
    if (!digits || *s++ != ':'
        || !(s[0] >= '0' && s[0] <= '5') || !(s[1] >= '0' && s[1] <= '9'))
        return 0;
    long sec = (s[0] - '0') * 10 + (s[1] - '0');
    s += 2;
    if (*s == '.' || *s == ':')
    {
        int n = 0;
        for (s++; *s >= '0' && *s <= '9'; s++)
        {
            if (n < 3)
            {
                frac = frac * 10 + (*s - '0');
                n++;
            }
        }
        if (!n)
            return 0;
        for (; n < 3; n++)
            frac *= 10;
    }
    if (*s++ != close)
        return 0;
    *ms = (min * 60 + sec) * 1000 + frac;
    return s - p;
}

static const char *skip_spaces(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static const char *line_end(const char *s)
{
    return s + strcspn(s, "\r\n");
}

static const char *next_line(const char *s)
{
    if (*s == '\r')
        s++;
    if (*s == '\n')
        s++;
    return s;
}

/* Lyrics in LRC, two lines or more starting with a time: the lines in
   times[], sorted, their words packed at the start of text without the
   times. Lines without one, as [ar:] and the like, are left out, and
   [offset:] applies. Plain text is left as it is. */
static void parse_synced(void)
{
    long ms;
    int timed = 0;
    time_count = 0;
    for (const char *s = text; *s && timed < 2; s = next_line(line_end(s)))
        if (parse_time(skip_spaces(s), '[', ']', &ms))
            timed++;
    if (timed < 2)
        return;

    /* the words go back over what was read, never ahead of it */
    char *out = text;
    long offset = 0;
    for (const char *s = text; *s; )
    {
        const char *eol = line_end(s);
        const char *p = skip_spaces(s);
        long at[8];
        int n = 0, len;
        while ((len = parse_time(p, '[', ']', &ms)) > 0)
        {
            if (n < 8)
                at[n++] = ms;
            p += len;
        }
        if (n == 0)
        {
            /* a positive offset shows the lines sooner */
            if (!strncmp(p, "[offset:", 8))
            {
                const char *q = p + 8;
                bool neg = *q == '-';
                if (*q == '-' || *q == '+')
                    q++;
                for (offset = 0; *q >= '0' && *q <= '9'; q++)
                    offset = offset * 10 + (*q - '0');
                if (neg)
                    offset = -offset;
            }
        }
        else
        {
            char *start = out;
            p = skip_spaces(p);
            while (p < eol)
            {
                long word;
                if ((len = parse_time(p, '<', '>', &word)) > 0)
                {
                    p += len;
                    continue;
                }
                *out++ = *p++;
            }
            while (out > start && out[-1] == ' ')
                out--;
            for (int i = 0; i < n && time_count < LYRICS_MAX_TIMES; i++)
            {
                times[time_count].ms = at[i];
                times[time_count].start = start - text;
                times[time_count].len = out - start;
                time_count++;
            }
        }
        s = next_line(eol);
    }

    /* in the order they are sung, the same line where a chorus repeats */
    for (int i = 0; i < time_count; i++)
        times[i].ms = MAX(0, times[i].ms - offset);
    for (int i = 1; i < time_count; i++)
    {
        struct timed_line t = times[i];
        int j = i;
        for (; j > 0 && times[j - 1].ms > t.ms; j--)
            times[j] = times[j - 1];
        times[j] = t;
    }
}

static void add_line(const char *start, int len, int time)
{
    if (line_count < LYRICS_MAX_LINES)
    {
        lines[line_count].start = start - text;
        lines[line_count].len = len;
        lines[line_count].time = time;
        line_count++;
    }
}

/* One line of the text, p to pend, into lines that fit width: at spaces,
   or anywhere in a word too long for a line */
static void wrap_span(const char *p, const char *pend, int time,
                      int width, int font)
{
    if (p == pend)
        add_line(p, 0, time);
    while (p < pend && line_count < LYRICS_MAX_LINES)
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

        add_line(p, e - p, time);
        p = next;
        while (p < pend && *p == ' ')
            p++;
    }
}

/* the lines of the text at its own line breaks (any of \r\n, \n and \r,
   iTunes uses \r), or of each timed line */
static void wrap_lyrics(int width, int font)
{
    line_count = 0;
    if (time_count > 0)
    {
        for (int i = 0; i < time_count; i++)
        {
            times[i].line = MIN(line_count, LYRICS_MAX_LINES - 1);
            wrap_span(text + times[i].start,
                      text + times[i].start + times[i].len, i, width, font);
        }
        return;
    }

    for (const char *s = text; *s && line_count < LYRICS_MAX_LINES; )
    {
        const char *pend = line_end(s);
        wrap_span(s, pend, -1, width, font);
        s = next_line(pend);
    }
    while (line_count > 0 && lines[line_count - 1].len == 0)
        line_count--;
}

bool lyrics_load(const struct mp3entry *id3, struct viewport *vp)
{
    /* taken before the reads: vp is in the skin's memory, which can move
       while a file is read */
    int width = vp->width, font = vp->font;
    int height = font_get(font)->height;
    if (!id3)
        return false;
    /* the view again for the same track: the lines as they were, and
       scrolled where they were; or none, without looking again */
    if (loaded.path[0] && loaded.width == width && loaded.font == font
        && loaded.height == height && !strcmp(loaded.path, id3->path))
        return loaded.found;

    line_count = top_line = time_count = 0;
    current = -1;
    hold_until = current_tick;
    rows_shown = MAX(1, vp->height / height);

    /* the track's own lyrics, unless they aren't synced and an .lrc is */
    bool embedded = id3->has_embedded_lyrics && read_embedded(id3);
    if (embedded)
        parse_synced();
    bool found = embedded;
    if (time_count == 0 && read_lrc(id3))
    {
        parse_synced();
        if (time_count > 0 || !embedded)
            found = true; /* synced, or plain text in an .lrc */
        else
            read_embedded(id3); /* the track's own plain text is first */
    }
    if (found)
        wrap_lyrics(width, font);

    strlcpy(loaded.path, id3->path, sizeof(loaded.path));
    loaded.width = width;
    loaded.font = font;
    loaded.height = height;
    loaded.found = found && line_count > 0;
    return loaded.found;
}

bool lyrics_follow(unsigned long elapsed)
{
    if (time_count == 0)
        return false;
    int now = current;
    if (now >= 0 && times[now].ms > (long)elapsed)
        now = -1; /* back, as after a seek */
    while (now + 1 < time_count && times[now + 1].ms <= (long)elapsed)
        now++;
    bool changed = now != current;
    current = now;
    if (TIME_BEFORE(current_tick, hold_until))
        return changed;

    /* the line sung now a third of the way down */
    int top = current < 0 ? 0 : times[current].line - rows_shown / 3;
    top = MAX(0, MIN(top, line_count - rows_shown));
    if (top != top_line)
    {
        top_line = top;
        changed = true;
    }
    return changed;
}

#ifdef HAVE_LCD_COLOR
/* the lines not sung now: half way from the text's color to the ground */
static unsigned dimmed(unsigned fg, unsigned bg)
{
    return LCD_RGBPACK((RGB_UNPACK_RED(fg) + RGB_UNPACK_RED(bg)) / 2,
                       (RGB_UNPACK_GREEN(fg) + RGB_UNPACK_GREEN(bg)) / 2,
                       (RGB_UNPACK_BLUE(fg) + RGB_UNPACK_BLUE(bg)) / 2);
}
#endif

void lyrics_draw(struct screen *display, struct viewport *vp)
{
    /* clean, as the skin sets its viewports: a dirty one made the WPS
       redraw everything at its next turn, five times a second */
    struct viewport *last_vp = display->set_viewport_ex(vp,
                                                        VP_FLAG_VP_SET_CLEAN);
    int height = font_get(vp->font)->height;
    char buf[256];

    /* a plain background, the backdrop can have other things here; the
       text only draws its foreground as its background would be the
       backdrop again */
    unsigned fg = display->get_foreground();
    unsigned dim = fg;
#ifdef HAVE_LCD_COLOR
    if (time_count > 0)
        dim = dimmed(fg, vp->bg_pattern);
#endif
#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
    if (!cover_palette_clear(display, vp))
#endif
    {
        display->set_foreground(vp->bg_pattern);
        display->fillrect(0, 0, vp->width, vp->height);
    }
    display->set_drawmode(DRMODE_FG);

    rows_shown = MAX(1, vp->height / height);
    for (int i = 0; i < rows_shown && top_line + i < line_count; i++)
    {
        int l = top_line + i;
        int len = MIN(lines[l].len, (int)sizeof(buf) - 1);
        memcpy(buf, text + lines[l].start, len);
        buf[len] = '\0';
        display->set_foreground(time_count == 0 || lines[l].time == current
                                ? fg : dim);
        display->putsxy(0, i * height, buf);
    }

    display->set_foreground(fg);
    display->set_drawmode(DRMODE_SOLID);
    display->update_viewport();
    display->set_viewport_ex(last_vp, VP_FLAG_VP_SET_CLEAN);
}

bool lyrics_scroll(int delta)
{
    /* a look elsewhere: synced lyrics follow the song again later */
    hold_until = current_tick + LYRICS_HOLD;
    int top = MAX(0, MIN(top_line + delta, line_count - rows_shown));
    if (top == top_line)
        return false;
    top_line = top;
    return true;
}

