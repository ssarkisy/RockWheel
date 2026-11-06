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

/* The fonts of the library: for the alphabets the theme's fonts don't have,
 * Hangul, the ideographs and kana, Hebrew, Arabic and Thai, fonts of just the
 * glyphs that the tags of the library, the names of its playlists and the menus
 * of every language are written in. They're made on the Updating Library screen
 * from big fonts of each alphabet that only lie on the disk (the Hangul ones and
 * those of themes/tools/make_script_fonts.sh), in the three sizes of the theme's
 * fonts, and the firmware takes those glyphs from them rather than from GNU
 * Unifont (firmware/font.c). So a title in Korean, Japanese or Chinese looks as
 * the rest of the text does whatever the language of the menus, with three
 * small fonts however many alphabets the library has. */

#include "config.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "system.h"
#include "file.h"
#include "dir.h"
#include "core_alloc.h"
#include "font.h"
#include "rbunicode.h"
#include "rbpaths.h"
#include "tagcache.h"
#include "settings.h"
#include "menus/exported_menus.h"
#include "library_fonts.h"
#include "misc.h"
#include "plugin.h"

#define LF_VERSION   2
#define LF_STAMP     FONT_DIR "/library.stamp"
#define LF_SCRIPTS   FONT_DIR "/scripts"
#define RB12_HEADER  36

/* The heights of the theme's fonts, and where their baselines are: the
   library's fonts go no higher and no lower, so that their glyphs stand on the
   same line as the letters beside them */
static const struct { int height, ascent; } sizes[] = {
    { 24, 17 }, /* the menus' */
    { 17, 13 }, /* Now Playing's */
    { 14, 11 }, /* small text */
};

/* where the glyphs come from */
enum source {
    SRC_HANGUL = 0,
    SRC_HAN_JP, SRC_HAN_SC, SRC_HAN_TC,
    SRC_WIDE_JP, SRC_WIDE_SC, SRC_WIDE_TC,
    SRC_HEBREW, SRC_ARABIC, SRC_ARABIC_A, SRC_ARABIC_B, SRC_THAI,
    SRC_COUNT
};
static const char *const source_names[SRC_COUNT] = {
    "Hangul", "Han-JP", "Han-SC", "Han-TC", "Wide-JP", "Wide-SC", "Wide-TC",
    "Hebrew", "Arabic", "ArabicA", "ArabicB", "Thai",
};

/* The ideographs as they're drawn in Japan, and in simplified and traditional
   Chinese, which differ in places: each in the forms of the text it's in, by
   kana in it, or by the language of the menus it's of */
enum han { HAN_JP, HAN_SC, HAN_TC, HAN_COUNT };

static bool is_kana(ucschar_t c)
{
    return c >= 0x3040 && c <= 0x30ff;
}

static bool is_hangul(ucschar_t c)
{
    return (c >= 0xac00 && c <= 0xd7a3) || (c >= 0x3131 && c <= 0x318e);
}

static bool is_han(ucschar_t c)
{
    return !is_hangul(c) && ((c >= 0x3000 && c <= 0x9fff) || (c >= 0xff00 && c <= 0xffef));
}

static bool is_hebrew(ucschar_t c)
{
    return c >= 0x0590 && c <= 0x05ff;
}

static bool is_arabic(ucschar_t c)
{
    return (c >= 0x0600 && c <= 0x06ff) || (c >= 0xfb50 && c <= 0xfdff)
        || (c >= 0xfe70 && c <= 0xfeff);
}

static bool is_thai(ucschar_t c)
{
    return c >= 0x0e00 && c <= 0x0e7f;
}

/* a little of a file at a time, as the glyphs are read in order */
#define WINDOW 4096
struct window {
    int fd;
    off_t at;
    int have;
    unsigned char bytes[WINDOW];
};
enum { WIN_WIDTHS, WIN_OFFSETS, WIN_BITS, WINDOWS };

/* a big font a glyph comes from, read as a file: one it hasn't is the default
   character's, at its offset */
struct src_font {
    int fd;               /* -1 not opened yet, -2 not there */
    int height, ascent;
    ucschar_t first, defchar;
    int32_t size, default_offset;
    bool long_offsets;
    off_t offsets_at, widths_at;
};

/* one character a source has, at its place in the library's font */
struct glyph {
    uint16_t c;
    uint8_t source, width;
    int32_t offset; /* in its source */
};

#define GLYPH_MAX 2048 /* bytes of a glyph, 40 px by 40 at most */

/* All that the making takes, in one allocation for that time: a bit for each
   character of the Basic Multilingual Plane that's wanted, and for each
   ideograph which forms it's wanted in */
struct work {
    uint8_t wanted[0x10000 / 8];
    uint8_t han[HAN_COUNT][0x10000 / 8];
    struct src_font sources[SRC_COUNT];
    struct window windows[WINDOWS];
    unsigned char text[4096];
    unsigned char in[GLYPH_MAX], out[GLYPH_MAX];
    enum han ui_han; /* the menus' forms of the ideographs, or HAN_COUNT */
    bool hangul, kana; /* any of them */
};
static struct work *work;

static inline void set_bit(uint8_t *bits, ucschar_t c)
{
    bits[c >> 3] |= 1 << (c & 7);
}

static inline bool get_bit(const uint8_t *bits, ucschar_t c)
{
    return bits[c >> 3] & (1 << (c & 7));
}

/** What the glyphs are of **/

/* the characters of a piece of text, the ideographs in the forms of text without
   kana given */
static void take_text(const unsigned char *s, const unsigned char *end, enum han no_kana)
{
    bool kana = false;
    for (const unsigned char *p = s; p < end && *p; )
    {
        ucschar_t c;
        p = utf8decode(p, &c);
        if (is_kana(c))
            kana = true;
    }
    for (const unsigned char *p = s; p < end && *p; )
    {
        ucschar_t c;
        p = utf8decode(p, &c);
        if (c < 0x0590 || c > 0xffff)
            continue;
        work->hangul |= is_hangul(c);
        work->kana |= is_kana(c);
        if (is_han(c))
            set_bit(work->han[kana ? HAN_JP : no_kana], c);
        if (is_han(c) || is_hangul(c))
            set_bit(work->wanted, c);
        /* the small alphabets whole, as they're joined and stacked as they're
           drawn: a letter's forms are other characters. A letter of each
           says they're wanted. */
        else if (is_hebrew(c))
            set_bit(work->wanted, 0x05d0);
        else if (is_arabic(c))
            set_bit(work->wanted, 0x0627);
        else if (is_thai(c))
            set_bit(work->wanted, 0x0e01);
    }
}

static void take_block(ucschar_t first, ucschar_t last)
{
    for (ucschar_t c = first; c <= last; c++)
        set_bit(work->wanted, c);
}

/* a file of the database: a header, then each string after its length and its
   track's index */
/* How far making the fonts is, of FONTS_STEPS: the characters gathered to
   GATHERED, by the bytes of the tag and language files read, then the fonts
   made of them. The gathering is all of it after a sync that brought no new
   letters, 5 s for 13000 songs, and Updating Library's bar stood empty
   meanwhile. The fonts are made only for letters new to the library. */
#define FONTS_STEPS 1000
#define GATHERED    950
static bool (*fonts_progress)(int done, int total);
static int gather_at, gather_total, gather_told; /* bytes, and steps told */
static bool gather_stopped; /* by USB or switching off, as progress says */

static void gather_read(int bytes)
{
    gather_at += bytes;
    if (!fonts_progress || gather_stopped || gather_total <= 0)
        return;
    const int step = GATHERED * (MIN(gather_at, gather_total) >> 10)
                     / ((gather_total >> 10) + 1);
    if (step != gather_told)
    {
        gather_told = step;
        gather_stopped = !fonts_progress(step, FONTS_STEPS);
    }
}

static bool made_progress(int done, int total)
{
    return fonts_progress(GATHERED + (FONTS_STEPS - GATHERED) * done
                                     / MAX(total, 1), FONTS_STEPS);
}

static void tag_file_path(int tag, char *path, size_t size)
{
    snprintf(path, size, "%s/database_%d.tcd", tagcache_get_stat()->db_path, tag);
}

static void take_tag_file(int tag, enum han no_kana)
{
    char path[MAX_PATH];
    tag_file_path(tag, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    int32_t header[3]; /* magic, size, count */
    if (read(fd, header, sizeof(header)) == sizeof(header))
    {
        for (int32_t i = 0; i < header[2] && !gather_stopped; i++)
        {
            int32_t entry[2];
            if (read(fd, entry, sizeof(entry)) != sizeof(entry)
                || entry[0] < 0 || entry[0] > 0x10000)
                break;
            gather_read(sizeof(entry) + entry[0]);
            const int n = MIN(entry[0], (int)sizeof(work->text) - 1);
            if (read(fd, work->text, n) != n)
                break;
            work->text[n] = '\0';
            if (entry[0] > n)
                lseek(fd, entry[0] - n, SEEK_CUR);
            take_text(work->text, work->text + n, no_kana);
        }
    }
    close(fd);
}

/* A file of strings one after another, as a language's: each string ends with
   a 0, and the bytes between them are what they are */
static void take_strings_file(const char *path, enum han no_kana)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    unsigned char *const buf = work->text;
    const size_t room = sizeof(work->text) - 1;
    size_t have = 0;
    while (!gather_stopped)
    {
        const ssize_t got = read(fd, buf + have, room - have);
        if (got <= 0)
            break;
        gather_read(got);
        have += got;
        buf[have] = '\0';
        /* up to the last end of a string, the rest kept for the next read */
        size_t done = have;
        while (done > 0 && buf[done - 1] != '\0')
            done--;
        if (done == 0)
            done = have; /* a string longer than the buffer: as far as it goes */
        for (size_t i = 0; i < done; )
        {
            const size_t len = strlen((char *)buf + i);
            take_text(buf + i, buf + i + len, no_kana);
            i += len + 1;
        }
        memmove(buf, buf + done, have - done);
        have -= done;
    }
    close(fd);
}

/* the names in a folder, and in the folders in it as deep as given */
static void take_names_in(const char *dir_path, int depth, enum han no_kana)
{
    DIR *dir = opendir(dir_path);
    if (!dir)
        return;
    struct dirent *entry;
    while ((entry = readdir(dir)))
    {
        if (entry->d_name[0] == '.')
            continue;
        const unsigned char *name = (const unsigned char *)entry->d_name;
        take_text(name, name + strlen(entry->d_name), no_kana);
        struct dirinfo info = dir_get_info(dir, entry);
        if ((info.attribute & ATTR_DIRECTORY) && depth > 0)
        {
            char path[MAX_PATH];
            if (snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name)
                < (int)sizeof(path))
                take_names_in(path, depth - 1, no_kana);
        }
    }
    closedir(dir);
}

static enum han han_of_language(const char *file)
{
    return !strncmp(file, "chinese-trad", 12) ? HAN_TC : HAN_SC;
}

/* the languages of the menus, all of them, and their names in the list of them */
static void take_languages(void)
{
    DIR *dir = opendir(LANG_DIR);
    if (dir)
    {
        struct dirent *entry;
        while ((entry = readdir(dir)))
        {
            const char *ext = strrchr(entry->d_name, '.');
            if (!ext || strcasecmp(ext, ".lng"))
                continue;
            char path[MAX_PATH];
            if (snprintf(path, sizeof(path), LANG_DIR "/%s", entry->d_name)
                < (int)sizeof(path))
                take_strings_file(path, han_of_language(entry->d_name));
        }
        closedir(dir);
    }
    const char *file, *name;
    for (int i = 0; language_at(i, &file, &name); i++)
    {
        if (name)
            take_text((const unsigned char *)name,
                      (const unsigned char *)name + strlen(name), han_of_language(file));
    }
}

/** The big fonts the glyphs are taken from **/

static bool window_read(struct window *w, int fd, off_t pos, void *dst, int n)
{
    if (w->fd != fd || pos < w->at || pos + n > w->at + w->have)
    {
        if (n > WINDOW)
            return lseek(fd, pos, SEEK_SET) == pos && read(fd, dst, n) == n;
        w->fd = fd;
        w->at = pos;
        w->have = 0;
        if (lseek(fd, pos, SEEK_SET) != pos)
            return false;
        const ssize_t got = read(fd, w->bytes, WINDOW);
        if (got < n)
            return false;
        w->have = got;
    }
    memcpy(dst, w->bytes + (pos - w->at), n);
    return true;
}

static int glyph_bytes(int height, int width)
{
    return ((height * width + 1) / 2 + 1) & ~1;
}

static uint16_t get16(const unsigned char *p)
{
    return p[0] | p[1] << 8;
}

static uint32_t get32(const unsigned char *p)
{
    return get16(p) | (uint32_t)get16(p + 2) << 16;
}

static bool read_offset(struct src_font *f, int32_t i, int32_t *offset)
{
    unsigned char o[4];
    const int n = f->long_offsets ? 4 : 2;
    if (!window_read(&work->windows[WIN_OFFSETS], f->fd, f->offsets_at + i * n, o, n))
        return false;
    *offset = f->long_offsets ? (int32_t)get32(o) : get16(o);
    return true;
}

static void close_sources(void)
{
    for (int s = 0; s < SRC_COUNT; s++)
    {
        if (work->sources[s].fd >= 0)
            close(work->sources[s].fd);
        work->sources[s].fd = -1;
    }
    for (int i = 0; i < WINDOWS; i++)
        work->windows[i].fd = -1;
}

static struct src_font *open_source(enum source s, int height)
{
    struct src_font *f = &work->sources[s];
    if (f->fd >= 0)
        return f;
    if (f->fd == -2)
        return NULL;
    char path[MAX_PATH];
    if (s == SRC_HANGUL)
        snprintf(path, sizeof(path), FONT_DIR "/%d-Hangul.fnt", height);
    else
        snprintf(path, sizeof(path), LF_SCRIPTS "/%d-%s.fnt", height, source_names[s]);
    unsigned char h[RB12_HEADER];
    f->fd = open(path, O_RDONLY);
    if (f->fd < 0 || read(f->fd, h, sizeof(h)) != sizeof(h) || memcmp(h, "RB12", 4)
        || get16(h + 10) != 1 /* anti-aliased, as the theme's */)
    {
        if (f->fd >= 0)
            close(f->fd);
        f->fd = -2;
        return NULL;
    }
    f->height = get16(h + 6);
    f->ascent = get16(h + 8);
    f->first = get32(h + 12);
    f->defchar = get32(h + 16);
    f->size = get32(h + 20);
    const uint32_t nbits = get32(h + 24), noffset = get32(h + 28);
    f->long_offsets = nbits >= 0xffdb;
    f->offsets_at = f->long_offsets ? ((RB12_HEADER + nbits + 3) & ~3)
                                    : ((RB12_HEADER + nbits + 1) & ~1);
    f->widths_at = f->offsets_at + noffset * (f->long_offsets ? 4 : 2);
    f->default_offset = -1;
    const int32_t d = f->defchar - f->first;
    if (d >= 0 && d < f->size)
        read_offset(f, d, &f->default_offset);
    return f;
}

/* the glyph of c in the font: its width and where its bits are, or false */
static bool source_glyph(struct src_font *f, ucschar_t c, int *width, int32_t *offset)
{
    if (!f || c < f->first || (int32_t)(c - f->first) >= f->size)
        return false;
    const int32_t i = c - f->first;
    unsigned char w;
    if (!window_read(&work->windows[WIN_WIDTHS], f->fd, f->widths_at + i, &w, 1)
        || !read_offset(f, i, offset))
        return false;
    *width = w;
    return w > 0 && (*offset != f->default_offset || c == f->defchar);
}

/* where a character's taken from, in order: the first of them that has it */
static int sources_of(ucschar_t c, enum source list[HAN_COUNT])
{
    if (is_hangul(c))
        list[0] = SRC_HANGUL;
    else if (is_han(c))
    {
        static const enum han order[HAN_COUNT][HAN_COUNT] = {
            { HAN_JP, HAN_SC, HAN_TC }, { HAN_SC, HAN_TC, HAN_JP },
            { HAN_TC, HAN_SC, HAN_JP },
        };
        enum han first;
        if (work->ui_han < HAN_COUNT && get_bit(work->han[work->ui_han], c))
            first = work->ui_han;
        else if (get_bit(work->han[HAN_JP], c))
            first = HAN_JP;
        else if (get_bit(work->han[HAN_TC], c) && !get_bit(work->han[HAN_SC], c))
            first = HAN_TC;
        else
            first = HAN_SC;
        const enum source base = c >= 0xff00 ? SRC_WIDE_JP : SRC_HAN_JP;
        for (int i = 0; i < HAN_COUNT; i++)
            list[i] = base + order[first][i];
        return HAN_COUNT;
    }
    else if (is_hebrew(c))
        list[0] = SRC_HEBREW;
    else if (c >= 0x0600 && c <= 0x06ff)
        list[0] = SRC_ARABIC;
    else if (c >= 0xfb50 && c <= 0xfdff)
        list[0] = SRC_ARABIC_A;
    else if (c >= 0xfe70 && c <= 0xfeff)
        list[0] = SRC_ARABIC_B;
    else if (is_thai(c))
        list[0] = SRC_THAI;
    else
        return 0;
    return 1;
}

/* A glyph of a font as the library's has it: on its baseline, and of its
   height, what's above or below that left out */
static void repack(const unsigned char *src, int src_height, int src_ascent,
                   unsigned char *dst, int height, int ascent, int width)
{
    const int shift = ascent - src_ascent;
    memset(dst, 0xff, glyph_bytes(height, width)); /* 15 is no ink */
    for (int y = 0; y < src_height; y++)
    {
        const int ty = y + shift;
        if (ty < 0 || ty >= height)
            continue;
        for (int x = 0; x < width; x++)
        {
            const int si = y * width + x, di = ty * width + x;
            const int a = (src[si >> 1] >> ((si & 1) * 4)) & 15;
            dst[di >> 1] = (dst[di >> 1] & ~(15 << ((di & 1) * 4))) | a << ((di & 1) * 4);
        }
    }
}

/* What's written, gathered in the plugins' buffer, which is free on Updating
   Library, and written out in as few pieces: a write of a byte or two for
   each character from the first to the last of a font, some 64000 of them,
   took seconds */
static unsigned char *out_buf;
static size_t out_size, out_used;

static bool out_flush(int fd)
{
    const bool ok = out_used == 0 || write(fd, out_buf, out_used) == (ssize_t)out_used;
    out_used = 0;
    return ok;
}

static bool put(int fd, const void *p, size_t n)
{
    if (!out_buf)
        return write(fd, p, n) == (ssize_t)n;
    if (out_used + n > out_size && !out_flush(fd))
        return false;
    if (n > out_size)
        return write(fd, p, n) == (ssize_t)n;
    memcpy(out_buf + out_used, p, n);
    out_used += n;
    return true;
}

static bool put16(int fd, uint16_t v)
{
    const unsigned char b[2] = { v, v >> 8 };
    return put(fd, b, 2);
}

static bool put32(int fd, uint32_t v)
{
    const unsigned char b[4] = { v, v >> 8, v >> 16, v >> 24 };
    return put(fd, b, 4);
}

/* The library's font of a size, of the glyphs its sources have of those wanted
   (glyphs[], in order): false if it couldn't be made */
static bool make_font(int size, struct glyph *glyphs, int count,
                      bool (*progress)(int done, int total), int done, int total)
{
    const int slot = sizes[size].height;
    int ascent = 0, descent = 0, maxwidth = 0, kept = 0;
    for (int i = 0; i < count; i++)
    {
        enum source list[HAN_COUNT];
        const int n = sources_of(glyphs[i].c, list);
        for (int k = 0; k < n; k++)
        {
            struct src_font *f = open_source(list[k], slot);
            int width;
            int32_t offset;
            if (source_glyph(f, glyphs[i].c, &width, &offset)
                && glyph_bytes(f->height, width) <= GLYPH_MAX)
            {
                glyphs[kept].c = glyphs[i].c;
                glyphs[kept].source = list[k];
                glyphs[kept].width = width;
                glyphs[kept].offset = offset;
                kept++;
                ascent = MAX(ascent, f->ascent);
                descent = MAX(descent, f->height - f->ascent);
                maxwidth = MAX(maxwidth, width);
                break;
            }
        }
        if (progress && !(i & 255) && !progress(done + i / 2, total))
        {
            close_sources();
            return false;
        }
    }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), FONT_DIR "/%d-Library.fnt", slot);
    if (kept == 0)
    {
        close_sources();
        remove(path);
        return true; /* none of them in the sources: no font */
    }
    count = kept;
    ascent = MIN(ascent, sizes[size].ascent);
    descent = MIN(descent, slot - sizes[size].ascent);
    const int height = ascent + descent;
    uint32_t nbits = 0;
    for (int i = 0; i < count; i++)
        nbits += glyph_bytes(height, glyphs[i].width);
    const ucschar_t first = glyphs[0].c, last = glyphs[count - 1].c;
    const uint32_t chars = last - first + 1;
    const bool long_offsets = nbits >= 0xffdb;

    int fd = whole_file_open(path);
    if (fd < 0)
    {
        close_sources();
        return false;
    }
    out_buf = plugin_get_buffer(&out_size);
    out_used = 0;
    /* as convttf writes them: the first character the default, of no width
       unless it's a glyph of its own */
    bool ok = put(fd, "RB12", 4) && put16(fd, maxwidth) && put16(fd, height)
              && put16(fd, ascent) && put16(fd, 1) && put32(fd, first)
              && put32(fd, first) && put32(fd, chars) && put32(fd, nbits)
              && put32(fd, chars) && put32(fd, chars);

    /* the glyphs, one after another */
    for (int i = 0; ok && i < count; i++)
    {
        const struct src_font *f = &work->sources[glyphs[i].source];
        const int w = glyphs[i].width;
        ok = window_read(&work->windows[WIN_BITS], f->fd,
                         RB12_HEADER + glyphs[i].offset, work->in,
                         glyph_bytes(f->height, w));
        if (!ok)
            break;
        repack(work->in, f->height, f->ascent, work->out, height, ascent, w);
        ok = put(fd, work->out, glyph_bytes(height, w));
        if (progress && !(i & 255))
            ok = ok && progress(done + count / 2 + i / 2, total);
    }

    /* the offset of every character from the first, 0 for those that aren't
       there, and then their widths, none for those */
    static const unsigned char pad[4];
    if (ok)
        ok = put(fd, pad, long_offsets ? ((nbits + 3) & ~3) - nbits
                                       : ((nbits + 1) & ~1) - nbits);
    for (int pass = 0; ok && pass < 2; pass++)
    {
        uint32_t offset = 0;
        int g = 0;
        for (ucschar_t c = first; ok && c <= last; c++)
        {
            const bool here = g < count && glyphs[g].c == c;
            if (pass == 0)
                ok = long_offsets ? put32(fd, here ? offset : 0)
                                  : put16(fd, here ? offset : 0);
            else
            {
                const unsigned char w = here ? glyphs[g].width : 0;
                ok = put(fd, &w, 1);
            }
            if (here)
            {
                offset += glyph_bytes(height, glyphs[g].width);
                g++;
            }
        }
    }
    ok = ok && out_flush(fd);
    out_buf = NULL;
    close_sources();
    return whole_file_close(fd, path, ok);
}

/** When they're made: after a sync, and when the languages or the alphabets'
    fonts are others, as after an update of the firmware **/

static bool have_sources(void)
{
    return dir_exists(LF_SCRIPTS) || file_exists(FONT_DIR "/24-Hangul.fnt");
}

#define STAMP_WORDS 5

static uint32_t hash_bytes(uint32_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}

static uint32_t hash_name(uint32_t h, const char *p)
{
    while (*p)
        h = h * 31 + (unsigned char)*p++;
    return h;
}

/* the commit of the database, the language of the menus, whose ideographs come
   first, and the names and sizes of the languages' files and the sources' */
static void stamp_of(uint32_t stamp[STAMP_WORDS])
{
    stamp[0] = LF_VERSION;
    stamp[1] = tagcache_get_commitid();
    stamp[2] = hash_name(0, (const char *)global_settings.lang_file);
    stamp[3] = stamp[4] = 0;
    static const char *const dirs[] = { LANG_DIR, LF_SCRIPTS };
    for (int d = 0; d < 2; d++)
    {
        DIR *dir = opendir(dirs[d]);
        if (!dir)
            continue;
        struct dirent *entry;
        while ((entry = readdir(dir)))
        {
            struct dirinfo info = dir_get_info(dir, entry);
            /* in whatever order they're read */
            stamp[3 + d] += hash_name(info.size, entry->d_name);
        }
        closedir(dir);
    }
}

void library_fonts_forget(void)
{
    remove(LF_STAMP);
}

bool library_fonts_stale(void)
{
    if (!have_sources())
        return false;
    uint32_t now[STAMP_WORDS], was[STAMP_WORDS];
    stamp_of(now);
    int fd = open(LF_STAMP, O_RDONLY);
    if (fd < 0)
        return true;
    const bool same = read(fd, was, sizeof(was)) == sizeof(was)
                      && !memcmp(now, was, sizeof(now));
    close(fd);
    return !same;
}

bool library_fonts_make(bool (*progress)(int done, int total))
{
    const int handle = core_alloc(sizeof(struct work));
    if (handle <= 0)
        return false;
    work = core_get_data_pinned(handle);
    memset(work, 0, sizeof(*work));
    /* at full speed, as all else on Updating Library: at 30 MHz it took
       more than twice as long */
    cpu_boost(true);
    for (int s = 0; s < SRC_COUNT; s++)
        work->sources[s].fd = -1;
    for (int i = 0; i < WINDOWS; i++)
        work->windows[i].fd = -1;

    const char *lang = (const char *)global_settings.lang_file;
    work->ui_han = !strcmp(lang, "japanese") ? HAN_JP
                 : !strcmp(lang, "chinese-trad") ? HAN_TC
                 : !strcmp(lang, "chinese-simp") ? HAN_SC : HAN_COUNT;
    const enum han no_kana = work->ui_han == HAN_TC ? HAN_TC : HAN_SC;

    static const int tags[] = { tag_artist, tag_album, tag_title, tag_genre,
                                tag_composer, tag_albumartist, tag_grouping };
    fonts_progress = progress;
    gather_at = gather_total = gather_told = 0;
    gather_stopped = false;
    for (size_t i = 0; progress && i < ARRAYLEN(tags); i++)
    {
        char path[MAX_PATH];
        tag_file_path(tags[i], path, sizeof(path));
        int fd = open(path, O_RDONLY);
        if (fd >= 0)
        {
            gather_total += ffilesize(fd);
            close(fd);
        }
    }
    DIR *langs = progress ? opendir(LANG_DIR) : NULL;
    if (langs)
    {
        /* those take_languages() reads: the folder has the voice's files
           too, a third of it, and the bar ended at 70% */
        struct dirent *entry;
        while ((entry = readdir(langs)))
        {
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && !strcasecmp(ext, ".lng"))
                gather_total += dir_get_info(langs, entry).size;
        }
        closedir(langs);
    }
    for (size_t i = 0; i < ARRAYLEN(tags); i++)
        take_tag_file(tags[i], no_kana);
    take_names_in(global_settings.playlist_catalog_dir[0]
                  ? (const char *)global_settings.playlist_catalog_dir
                  : PLAYLIST_CATALOG_DEFAULT_DIR, 2, no_kana);
    take_languages();
    if (get_bit(work->wanted, 0x05d0))
        take_block(0x0590, 0x05ff);
    if (get_bit(work->wanted, 0x0627))
    {
        take_block(0x0600, 0x06ff);
        take_block(0xfb50, 0xfbff); /* the forms they're joined in */
        take_block(0xfe70, 0xfeff);
    }
    if (get_bit(work->wanted, 0x0e01))
        take_block(0x0e00, 0x0e7f);
    /* the letters Search types Korean and Japanese with (music_search.c) */
    if (work->hangul)
        take_block(0x3131, 0x3163);
    if (work->kana)
        take_block(0x3041, 0x3096);

    /* The same characters, in the same forms, as the fonts there are of, from
       the same fonts and languages: only the database is another, as after a
       sync that brought no new letters, and they stay as they are */
    uint32_t now[STAMP_WORDS + 1], was[STAMP_WORDS + 1];
    stamp_of(now);
    now[STAMP_WORDS] = hash_bytes(hash_bytes(2166136261u, work->wanted,
                                             sizeof(work->wanted)),
                                  work->han, sizeof(work->han)) ^ work->ui_han;
    bool same = false;
    int stamp_fd = open(LF_STAMP, O_RDONLY);
    if (stamp_fd >= 0)
    {
        same = read(stamp_fd, was, sizeof(was)) == sizeof(was)
               && was[0] == now[0] && was[2] == now[2] && was[3] == now[3]
               && was[4] == now[4] && was[STAMP_WORDS] == now[STAMP_WORDS];
        close(stamp_fd);
    }

    int count = 0;
    for (ucschar_t c = 0; !same && c <= 0xffff; c++)
        count += get_bit(work->wanted, c);
    bool ok = !gather_stopped;
    const int list_handle = count && ok ? core_alloc(count * sizeof(struct glyph)) : 0;
    if (!ok)
    {
        /* made the next time */
    }
    else if (same)
    {
        /* kept as they are */
    }
    else if (count && list_handle <= 0)
        ok = false;
    else
    {
        /* each size from all that's wanted, as making one leaves out what its
           sources haven't */
        struct glyph *glyphs = list_handle ? core_get_data_pinned(list_handle) : NULL;
        const int total = count * ARRAYLEN(sizes);
        for (size_t s = 0; ok && s < ARRAYLEN(sizes); s++)
        {
            int n = 0;
            for (ucschar_t c = 0; n < count && c <= 0xffff; c++)
            {
                if (get_bit(work->wanted, c))
                    glyphs[n++].c = c;
            }
            ok = make_font(s, glyphs, n, progress ? made_progress : NULL,
                           count * s, total);
        }
        if (list_handle)
        {
            core_put_data_pinned(glyphs);
            core_free(list_handle);
        }
    }
    core_put_data_pinned(work);
    core_free(handle);
    work = NULL;
    cpu_boost(false);

    if (ok)
        whole_file_write(LF_STAMP, now, sizeof(now));
    return ok;
}
