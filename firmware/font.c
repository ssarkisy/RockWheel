/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (c) 2002 by Greg Haerr <greg@censoft.com>
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
/*
 * Rockbox startup font initialization
 * This file specifies which fonts get compiled-in and
 * loaded at startup, as well as their mapping into
 * the FONT_SYSFIXED, FONT_UI and FONT_MP3 ids.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "string-extra.h"
#include "font.h"
#include "file.h"
#include "core_alloc.h"
#include "debug.h"
#include "panic.h"
#include "rbunicode.h"
#include "diacritic.h"
#include "rbpaths.h"
#include "bidi.h"

/* Define LOGF_ENABLE to enable logf output in this file */
//#define LOGF_ENABLE
#include "logf.h"

#define MAX_FONTSIZE_FOR_16_BIT_OFFSETS 0xFFDB

#define FONT_EXT "fnt"
#define GLYPH_CACHE_EXT "gc"

#ifdef UNICODE32
#define FC_HEADER_VAL 0x01000020
#else
#define FC_HEADER_VAL 0x01000010
#endif

/* max static loadable font buffer size */
#ifndef MAX_FONT_SIZE
#if LCD_HEIGHT > 64
#if MEMORYSIZE > 2
#define MAX_FONT_SIZE   60000
#else
#define MAX_FONT_SIZE   10000
#endif
#else
#define MAX_FONT_SIZE   4000
#endif
#endif
#define GLYPHS_TO_CACHE 256

#if MEMORYSIZE < 4
#define FONT_HARD_LIMIT
#endif

#ifndef FONT_HEADER_SIZE
#define FONT_HEADER_SIZE 36
#endif

#ifndef BOOTLOADER
/* Font cache includes */
#include "font_cache.h"
#include "lru.h"
#include "dir.h"
/* JP and TC forms' codes past U+FFFF (made with the fonts by merge_fonts.py) */
#include "font_han_forms.h"
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

/* Define this to try loading /.rockbox/.glyphcache    *
 * when a font specific file fails. This requires the  *
 * user to copy and rename a font glyph cache file     */
//#define TRY_DEFAULT_GLYPHCACHE

/* compiled-in font */
extern struct font sysfont;

#if !defined(BOOTLOADER) || defined(SONY_NWZ_LINUX) || defined(HIBY_LINUX)

/* Glyphs of a block of codes in records of one size, in order: a byte of the
   glyph's width, then its bits, read in one go (themes/tools/merge_fonts.py) */
#define RECORD_BLOCKS 4
struct record_block {
    ucschar_t first, last;
    uint32_t offset; /* the first record's, as a glyph's offset */
    uint32_t size;
};

struct buflib_alloc_data {
    struct font font;    /* must be the first member! */
    char *path; /* font path and filename (allocd at end of buffer) */
    size_t path_bufsz; /* size of path buffer */
    int refcount;        /* how many times has this font been loaded? */
    int records;
    bool forms; /* its glyphs past U+FFFF are font_han_forms.h's */
    struct record_block record[RECORD_BLOCKS];
    unsigned char buffer[];
};
/* the loaders pad a font's tables by their addresses, as the file by offsets */
_Static_assert(offsetof(struct buflib_alloc_data, buffer) % 4 == 0,
               "a font's buffer is 4-byte aligned");
static int buflib_allocations[MAXFONTS];
static int ui_font = FONT_LASTUSERFONT;

static int cache_fd;
static struct font* cache_pf;

static void glyph_cache_save(int font_id);

static int buflibmove_callback(int handle, void* current, void* new)
{
    (void)handle;
    struct buflib_alloc_data *alloc = (struct buflib_alloc_data*)current;
    ptrdiff_t diff = new - current;

    logf("%s %s", __func__, alloc->path);
#define UPDATE(x) if (x) { x = PTR_ADD(x, diff); }

    UPDATE(alloc->font.bits);
    UPDATE(alloc->font.offset);
    UPDATE(alloc->font.width);

    UPDATE(alloc->font.buffer_start);
    UPDATE(alloc->font.buffer_end);
    UPDATE(alloc->font.buffer_position);
    UPDATE(alloc->path);

    UPDATE(alloc->font.cache._index);
    UPDATE(alloc->font.cache._lru._base);
    return BUFLIB_CB_OK;
}
static void lock_font_handle(int handle, bool lock)
{
    if ( handle < 0 )
        return;

    if (lock)
        core_pin(handle);
    else
        core_unpin(handle);
}


#ifndef BOOTLOADER
#define FALLBACK_FONT   FONT_DIR "/16-GNU-Unifont.fnt"
#define FALLBACK_GLYPHS 1024   /* about two screens of text that's all different */
static int fallback_id = -1;

/* The font that gives its place up to any other loaded with every place taken
   (font_set_yielding()), -1 for none */
static int yielding_id = -1;

void font_set_yielding(int font_id)
{
    yielding_id = font_id >= FONT_FIRSTUSERFONT && font_id < MAXFONTS
                  && buflib_allocations[font_id] > 0 ? font_id : -1;
}

/* Alphabets the theme's Inter doesn't have. Going by the alphabet means that nothing
   is read from a font's file to find out, and that spaces and marks of no width, which
   the fallback draws as boxes, stay as they are. */
static inline bool char_wants_fallback(ucschar_t ch)
{
    return (ch >= 0x0590 && ch < 0x1d00)    /* Hebrew, Arabic, Indic, Thai... */
        || (ch >= 0x2e80 && ch < 0xd800)    /* Chinese, Japanese, Hangul */
        || (ch >= 0xf900 && ch < 0xfe00)    /* CJK compatibility, Arabic forms */
        || (ch >= 0xfe30 && ch < 0xfeff)    /* CJK and Arabic forms */
        || (ch >= 0xff00 && ch < 0xfff0);   /* full and half width forms */
}

static inline bool is_hangul(ucschar_t ch)
{
    return (ch >= 0xac00 && ch <= 0xd7a3) || (ch >= 0x3131 && ch <= 0x318e);
}

/* One's own fonts of Hangul, FONT_DIR "/<height>-Hangul.fnt" in the heights of the
   theme's fonts, made by tools/convttf from another typeface: their syllables
   rather than the theme's */
#define HANGUL_GLYPHS 192
static const int hangul_heights[] = { 24, 17, 14 };
static int hangul_ids[ARRAYLEN(hangul_heights)] = { -1, -1, -1 };
static bool hangul_looked_for;

/* a file of a fallback, whose glyphs are only ever asked for other alphabets */
static bool is_fallback_file(const char *path)
{
    const char *dash = strrchr(path, '-');
    return !strcmp(path, FALLBACK_FONT) || (dash && !strcmp(dash, "-Hangul.fnt"));
}

/* Two fonts that 1.4.0 shipped and nothing loads, removed by their shipped
   sizes so that a file of one's own under the name stays. A theme sets the
   lists' font itself (ui_theme_to_settings()), so none is one chosen */
static void font_remove_unused(void)
{
    static const struct {
        const char *name;
        off_t size;
    } unused[] = {
        { "13-Inter-Regular", 5471753 }, { "14-Inter-Regular", 5851181 },
    };
    char path[MAX_PATH];
    for (size_t i = 0; i < ARRAYLEN(unused); i++)
    {
        snprintf(path, sizeof(path), FONT_DIR "/%s.fnt", unused[i].name);
        const int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        const off_t size = ffilesize(fd);
        close(fd);
        if (size != unused[i].size)
            continue;
        /* its glyph cache first: cut short, this runs again */
        char gc[MAX_PATH];
        snprintf(gc, sizeof(gc), FONT_DIR "/%s.gc", unused[i].name);
        remove(gc);
        remove(path);
    }
}

/* Old Library and scripts files and the theme's old Hangul (by their shipped
   sizes, so one's own stay), while fonts/scripts or library.stamp exists */
static void font_remove_old(void)
{
    /* the stamp last: while it's there, this runs again */
    static const char *const library[] = {
        "library-langs.bin", "24-Library.fnt", "17-Library.fnt", "14-Library.fnt",
        "24-Library.gc", "17-Library.gc", "14-Library.gc", "library.stamp",
    };
    static const off_t theme_hangul[ARRAYLEN(hangul_heights)][3] = {
        { 1328651, 1372179, 1622407 }, { 957151, 957851, 1212835 },
        { 861963, 883511, 1065939 },
    };
    if (!dir_exists(FONT_DIR "/scripts") && !file_exists(FONT_DIR "/library.stamp"))
        return;
    char path[MAX_PATH];
    for (size_t i = 0; i < ARRAYLEN(hangul_heights); i++)
    {
        snprintf(path, sizeof(path), FONT_DIR "/%d-Hangul.fnt", hangul_heights[i]);
        const int fd = open(path, O_RDONLY);
        const off_t size = fd >= 0 ? ffilesize(fd) : -1;
        if (fd >= 0)
            close(fd);
        for (size_t k = 0; k < ARRAYLEN(theme_hangul[i]); k++)
        {
            if (size != theme_hangul[i][k])
                continue;
            remove(path);
            snprintf(path, sizeof(path), FONT_DIR "/%d-Hangul.gc", hangul_heights[i]);
            remove(path);
            break;
        }
    }
    /* all of it, only ever the theme's, "._" copies of macOS's too, or the
       folder stays and this runs at every boot */
    DIR *dir = opendir(FONT_DIR "/scripts");
    struct dirent *entry;
    while (dir && (entry = readdir(dir)))
    {
        if (!(dir_get_info(dir, entry).attribute & ATTR_DIRECTORY)
            && snprintf(path, sizeof(path), FONT_DIR "/scripts/%s", entry->d_name)
               < (int)sizeof(path))
            remove(path);
    }
    if (dir)
        closedir(dir);
    rmdir(FONT_DIR "/scripts");
    for (size_t i = 0; i < ARRAYLEN(library); i++)
    {
        snprintf(path, sizeof(path), FONT_DIR "/%s", library[i]);
        /* as 1.3 wrote it, left by a power cut */
        strlcat(path, ".tmp", sizeof(path));
        remove(path);
        path[strlen(path) - 4] = '\0';
        remove(path);
    }
}

void font_load_fallback(void)
{
    if (fallback_id < 0)
        fallback_id = font_load_ex(FALLBACK_FONT, 0, FALLBACK_GLYPHS);

    /* Once, at boot, the music stopped; they stay until the next, as the
       scrolling line may be drawing with them */
    if (hangul_looked_for)
        return;
    hangul_looked_for = true;
    font_remove_old();
    font_remove_unused();
    for (size_t i = 0; i < ARRAYLEN(hangul_heights); i++)
    {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), FONT_DIR "/%d-Hangul.fnt", hangul_heights[i]);
        const int id = file_exists(path) ? font_load_ex(path, 0, HANGUL_GLYPHS) : -1;
        hangul_ids[i] = id < 0 ? -1 : id;
    }
}

unsigned font_lock_all(void)
{
    unsigned locked = 0;
    for (int i = 0; i < MAXFONTS; i++)
    {
        if (buflib_allocations[i] > 0)
        {
            font_lock(i, true);
            locked |= 1u << i;
        }
    }
    return locked;
}

void font_unlock_all(unsigned locked)
{
    for (int i = 0; i < MAXFONTS; i++)
    {
        if (locked & (1u << i))
            font_lock(i, false);
    }
}

/* The theme's fonts with every alphabet (themes/tools/merge_fonts.py): what
   they haven't has no width */
static inline bool font_is_merged(const struct font *pf)
{
    return pf->firstchar + pf->size > 0x10000;
}

/* of the theme's fonts with every alphabet, the one for a font of this height
   that hasn't them: the tallest not over it, or else the shortest */
static struct font *merged_font_for(unsigned int height)
{
    struct font *best = NULL, *shortest = NULL;
    for (int i = 0; i < MAXFONTS; i++)
    {
        if (buflib_allocations[i] <= 0)
            continue;
        struct font *f = font_get(i);
        if (!font_is_merged(f))
            continue;
        if (f->height <= height && (!best || f->height > best->height))
            best = f;
        if (!shortest || f->height < shortest->height)
            shortest = f;
    }
    return best ? best : shortest;
}

/* One's own Hangul for a font of this height: that of its height or the one
   above, as the capitals of the 18 px Inter of the lyrics and the hints are
   those of the menus' 24, or else the tallest */
static struct font *hangul_font_for(unsigned int height)
{
    int best = -1, tallest = -1;
    for (size_t i = 0; i < ARRAYLEN(hangul_heights); i++)
    {
        const int h = hangul_heights[i];
        if (hangul_ids[i] < 0)
            continue;
        if (h >= (int)height && (best < 0 || h < hangul_heights[best]))
            best = i;
        if (tallest < 0 || h > hangul_heights[tallest])
            tallest = i;
    }
    if (best < 0)
        best = tallest;
    return best < 0 ? NULL : font_get(hangul_ids[best]);
}

static int glyph_bytes(struct font *pf, int width);

/* Whether a character has a glyph of its own in one's own font: convttf
   gives one it hasn't the default's width and bits */
static bool own_glyph(struct font *pf, ucschar_t ch)
{
    const int width = font_get_width(pf, ch);
    if (width == 0 || ch == (ucschar_t)pf->defaultchar
        || width != font_get_width(pf, pf->defaultchar))
        return width > 0;
    lock_font_handle(pf->handle, true); /* both bits stay where they are */
    const bool own = memcmp(font_get_bits(pf, pf->defaultchar), font_get_bits(pf, ch),
                            glyph_bytes(pf, width)) != 0;
    lock_font_handle(pf->handle, false);
    return own;
}

/* the code of the glyph of a character's form in a merged font */
static ucschar_t font_han_glyph(struct font *pf, ucschar_t ch, int form)
{
    /* the code's bit in font_han_forms.h, as merge_fonts.py lays them out */
    int i;
    if (ch >= 0x3000 && ch <= 0x9fff)
        i = ch - 0x3000;
    else if (ch >= 0xf900 && ch <= 0xfaff)
        i = ch - 0xf900 + 0x7000;
    else if (ch >= 0xff00 && ch <= 0xffef)
        i = ch - 0xff00 + 0x7200;
    else
        return ch;
    const uint32_t *bits;
    const uint16_t *ranks;
    ucschar_t first;
    if (form == FONT_HAN_JP)
    {
        bits = font_han_jp_bits;
        ranks = font_han_jp_ranks;
        first = FONT_HAN_HIDDEN;
    }
    else if (form == FONT_HAN_TC)
    {
        bits = font_han_tc_bits;
        ranks = font_han_tc_ranks;
        first = FONT_HAN_HIDDEN + FONT_HAN_JP_COUNT;
    }
    else
        return ch;
    if (!((const struct buflib_alloc_data *)pf)->forms)
        return ch;
    const uint32_t word = bits[i >> 5];
    if (!(word & (1u << (i & 31))))
        return ch;
    int rank = ranks[i >> 8] + __builtin_popcount(word & ((1u << (i & 31)) - 1));
    for (int w = (i >> 5) & ~7; w < i >> 5; w++)
        rank += __builtin_popcount(bits[w]);
    return first + rank;
}

struct font* font_for_char(struct font* pf, ucschar_t ch, int form, ucschar_t *glyph)
{
    *glyph = ch;
    if (LIKELY(ch < 0x0590) || pf == &sysfont)
        return pf;
    if (is_hangul(ch))
    {
        struct font *hf = hangul_font_for(pf->height);
        if (hf && own_glyph(hf, ch))
            return hf;
    }
    if (!font_is_merged(pf))
    {
        if (!char_wants_fallback(ch))
        {
            /* punctuation and symbols from the font, but one past all it has
               from Unifont, as the titles that Coldplay gives as "⦵" and "✨":
               the font's blank default made an empty title */
            if (ch < pf->firstchar + pf->size || fallback_id < 0)
                return pf;
            return font_get(fallback_id);
        }
        struct font *mf = merged_font_for(pf->height);
        if (!mf)
            return fallback_id < 0 ? pf : font_get(fallback_id);
        pf = mf;
    }
    /* the width of the glyph drawn: the code's would load that one too */
    const ucschar_t g = font_han_glyph(pf, ch, form);
    if (fallback_id >= 0 && font_get_width(pf, g) == 0)
        return font_get(fallback_id);
    *glyph = g;
    return pf;
}

/* Hangul taller than the line stands on its baseline, as one's own of the
   size above for the 18 px fonts, rather than in its middle */
bool font_on_baseline(struct font *gf, ucschar_t ch)
{
    return is_hangul(ch) && (fallback_id < 0 || gf != font_get(fallback_id));
}

/* the forms of the menus' language, and those of a whole text for the thread
   drawing its parts (font_set_text_form()) */
static int han_form = FONT_HAN_SC;
static int text_form = -1;
static unsigned int text_form_thread;

void font_set_han_form(int form)
{
    han_form = form;
}

void font_set_text_form(int form)
{
    text_form = form;
    text_form_thread = thread_self();
}

static int forced_form(void)
{
    return text_form >= 0 && text_form_thread == thread_self() ? text_form : -1;
}

/* ・ of names in Chinese too isn't one */
static inline bool is_kana(ucschar_t ch)
{
    return (ch >= 0x3040 && ch <= 0x30ff && ch != 0x30fb)
        || (ch >= 0x31f0 && ch <= 0x31ff) || (ch >= 0xff66 && ch <= 0xff9f);
}

int font_han_form(const ucschar_t *text)
{
    const int forced = forced_form();
    if (forced >= 0)
        return forced;
    for (; han_form != FONT_HAN_JP && *text; text++)
    {
        if (is_kana(*text))
            return FONT_HAN_JP;
    }
    return han_form;
}

int font_text_form(const unsigned char *text, size_t maxbytes)
{
    const unsigned char *p = text;
    while (han_form != FONT_HAN_JP && *p && (size_t)(p - text) < maxbytes)
    {
        ucschar_t ch;
        p = utf8decode(p, &ch);
        if (is_kana(ch))
            return FONT_HAN_JP;
    }
    return han_form;
}
#endif /* BOOTLOADER */

void font_lock(int font_id, bool lock)
{
    if( font_id < 0 || font_id >= MAXFONTS )
        return;
    if( buflib_allocations[font_id] > 0 )
        lock_font_handle(buflib_allocations[font_id], lock);
}

static struct buflib_callbacks buflibops = {buflibmove_callback, NULL, NULL };

static inline unsigned char *buffer_from_handle(int handle)
{
    struct buflib_alloc_data *alloc = core_get_data(handle);
    unsigned char* buffer = alloc->buffer;
    return buffer;
}

/* Font cache structures */
static void cache_create(struct font* pf);
static NO_INLINE void glyph_cache_load(const char *font_path, struct font *pf);

/* While a font's remembered glyphs load, as many as 250 of them from three places in its
   file, each place comes in a stretch at a time: a seek and a read of each glyph's width,
   place and bits took half a second at each boot. Only when the memory is there for the
   taking. */
#define FONT_WINDOW 16384
enum { WINDOW_WIDTHS, WINDOW_OFFSETS, WINDOW_BITS, WINDOWS };
static struct {
    struct font *pf;
    unsigned char *data;
    off_t start[WINDOWS];
    ssize_t len[WINDOWS];
    int handle;
} windows;

static void font_windows_open(struct font *pf)
{
    size_t size = STORAGE_PAD(WINDOWS * FONT_WINDOW);
    if (core_allocatable() < size)
        return;
    int handle = core_alloc(size);
    if (handle <= 0)
        return;
    core_pin(handle);
    unsigned char *data = core_get_data(handle);
    STORAGE_ALIGN_BUFFER(data, size);
    windows.handle = handle;
    windows.data = data;
    windows.pf = pf;
    for (int i = 0; i < WINDOWS; i++)
        windows.len[i] = 0;
}

static void font_windows_close(void)
{
    if (windows.pf)
    {
        core_unpin(windows.handle);
        core_free(windows.handle);
    }
    windows.pf = NULL;
}

static ssize_t font_read_at(struct font *pf, int which, int fd, off_t pos,
                            void *buf, size_t size)
{
    if (windows.pf == pf)
    {
        off_t in = pos - windows.start[which];
        if (in < 0 || in + (off_t)size > windows.len[which])
        {
            const off_t start = pos & ~(off_t)511;
            windows.len[which] = 0;
            if (lseek(fd, start, SEEK_SET) == start)
            {
                const ssize_t got = read(fd, windows.data + which * FONT_WINDOW,
                                         FONT_WINDOW);
                windows.len[which] = got > 0 ? got : 0;
            }
            windows.start[which] = start;
            in = pos - start;
        }
        if (in + (off_t)size <= windows.len[which])
        {
            memcpy(buf, windows.data + which * FONT_WINDOW + in, size);
            return size;
        }
    }
    lseek(fd, pos, SEEK_SET);
    return read(fd, buf, size);
}
/* End Font cache structures */

void font_init(void)
{
    int i = 0;
    cache_fd = -1;
    while (i<MAXFONTS)
        buflib_allocations[i++] = -1;
}

/* Check if we have x bytes left in the file buffer */
#define HAVEBYTES(x) (pf->buffer_position + (x) <= pf->buffer_end)

/* Helper functions to read big-endian unaligned short or long from
   the file buffer.  Bounds-checking must be done in the calling
   function.
 */

static short readshort(struct font *pf)
{
    uint16_t s;

    s = *pf->buffer_position++ & 0xff;
    s |= (*pf->buffer_position++ << 8);
    return s;
}

static int32_t readlong(struct font *pf)
{
    uint32_t l;

    l = *pf->buffer_position++ & 0xff;
    l |= *pf->buffer_position++ << 8;
    l |= ((uint32_t)(*pf->buffer_position++)) << 16;
    l |= ((uint32_t)(*pf->buffer_position++)) << 24;
    return l;
}

static int glyph_bytes( struct font *pf, int width )
{
    int ret;
    if (pf->depth)
        ret = ( pf->height * width + 1 ) / 2;
    else
        ret = width * ((pf->height + 7) / 8);
    return (ret + 1) & ~1;
}

/* Load memory font */
static struct font* font_load_in_memory(struct font* pf,
                                        int32_t nwidth,
                                        int32_t noffset )
{
    int i;
    /* variable font data*/
    pf->buffer_position = pf->buffer_start + 36;
    pf->bits = (unsigned char *)pf->buffer_position;
    pf->buffer_position += pf->bits_size*sizeof(unsigned char);

    if (pf->bits_size < MAX_FONTSIZE_FOR_16_BIT_OFFSETS)
    {
        /* pad to 16-bit boundary */
        pf->buffer_position = (unsigned char *)(((intptr_t)pf->buffer_position + 1) & ~1);
    }
    else
    {
        /* pad to 32-bit boundary*/
        pf->buffer_position = (unsigned char *)(((intptr_t)pf->buffer_position + 3) & ~3);
    }

    if (noffset)
    {
        if (pf->bits_size < MAX_FONTSIZE_FOR_16_BIT_OFFSETS)
        {
            pf->long_offset = 0;
            pf->offset = (uint16_t*)pf->buffer_position;

            /* Check we have sufficient buffer */
            if (!HAVEBYTES(noffset * sizeof(uint16_t)))
                return NULL;

            for (i=0; i<noffset; ++i)
            {
                ((uint16_t*)(pf->offset))[i] = (uint16_t)readshort(pf);
            }
        }
        else
        {
            pf->long_offset = 1;
            pf->offset = (uint16_t*)pf->buffer_position;

            /* Check we have sufficient buffer */
            if (!HAVEBYTES(noffset * sizeof(int32_t)))
                return NULL;

            for (i=0; i<noffset; ++i)
            {
                ((uint32_t*)(pf->offset))[i] = (uint32_t)readlong(pf);
            }
        }
    }
    else
        pf->offset = NULL;

    if (nwidth) {
        pf->width = (unsigned char *)pf->buffer_position;
        pf->buffer_position += nwidth*sizeof(unsigned char);
    }
    else
        pf->width = NULL;

    if (pf->buffer_position > pf->buffer_end)
        return NULL;

    return pf;    /* success!*/
}

/* Load cached font */
static struct font* font_load_cached(struct font* pf,
                                     int32_t nwidth,
                                     int32_t noffset)
{
    /* We are now at the bitmap data, this is fixed at 36.. */
    pf->width = NULL;
    pf->bits = NULL;

    /* Calculate offset to offset data */
    pf->buffer_position += pf->bits_size * sizeof(unsigned char);

    if (pf->bits_size < MAX_FONTSIZE_FOR_16_BIT_OFFSETS)
    {
        pf->long_offset = 0;
        /* pad to 16-bit boundary */
        pf->buffer_position = (unsigned char *)(((intptr_t)pf->buffer_position + 1) & ~1);
    }
    else
    {
        pf->long_offset = 1;
        /* pad to 32-bit boundary*/
        pf->buffer_position = (unsigned char *)(((intptr_t)pf->buffer_position + 3) & ~3);
    }

    if (noffset)
        pf->file_offset_offset = (uint32_t)(pf->buffer_position - pf->buffer_start);
    else
        pf->file_offset_offset = 0;

    /* Calculate offset to widths data */
    if (pf->bits_size < MAX_FONTSIZE_FOR_16_BIT_OFFSETS)
        pf->buffer_position += noffset * sizeof(uint16_t);
    else
        pf->buffer_position += noffset * sizeof(uint32_t);

    if (nwidth)
        pf->file_width_offset = (uint32_t)(pf->buffer_position - pf->buffer_start);
    else
        pf->file_width_offset = 0;

    /* Create the cache */
    cache_create(pf);

    return pf;
}

bool font_filename_matches_loaded_id(int font_id, const char *filename)
{
    if ( font_id >= 0 && font_id < MAXFONTS )
    {
        int handle = buflib_allocations[font_id];
        if (handle > 0)
        {
            struct buflib_alloc_data *data = core_get_data(handle);
            logf("%s id: [%d], %s", __func__, font_id, data->path);
            return strcmp(data->path, filename) == 0;
        }
    }
    return false;
}

int font_find(const char *path)
{
    for (int index = 0; index < MAXFONTS; index++)
    {
        if (font_filename_matches_loaded_id(index, path))
            return index;
    }
    return -1;
}

static int find_font_index(const char* path)
{
    for(int index = 0; index < MAXFONTS; index++)
    {
        if(font_filename_matches_loaded_id(index, path))
        {
            logf("%s Found id: [%d], %s", __func__, index, path);
            return index;
        }
    }
    logf("%s %s Not found using id: [%d], FONT_SYSFIXED",
         __func__, path, FONT_SYSFIXED);
    return FONT_SYSFIXED;
}

static size_t font_glyphs_to_bufsize(struct font *pf, int glyphs)
{
    size_t bufsize;

    /* LRU bytes per glyph */
    bufsize = LRU_SLOT_OVERHEAD + sizeof(struct font_cache_entry) +
        sizeof(unsigned short);
    /* Image bytes per glyph */
    bufsize += glyph_bytes(pf, pf->maxwidth);
    bufsize *= glyphs;

    return bufsize;
}

static struct font* font_load_header(int fd, struct font *pheader,
                                     struct font *pf,
                                     uint32_t *nwidth, uint32_t *noffset)
{
    /* Load the header. Readshort() and readlong()              *
     * update buffer_position address as they read              */
    pheader->buffer_start = pheader->buffer_position = (char *)pheader;
    pheader->buffer_size = FONT_HEADER_SIZE;
    pheader->buffer_end = pheader->buffer_start + pheader->buffer_size;

    if (read(fd, pheader, FONT_HEADER_SIZE) != FONT_HEADER_SIZE)
        return NULL;

    /* read magic and version #*/
    if (memcmp(pheader->buffer_position, VERSION, 4) != 0)
        return NULL;

    pheader->buffer_position += 4;

    /* font info*/
    pf->maxwidth      = readshort(pheader);
    pf->height        = readshort(pheader);
    pf->ascent        = readshort(pheader);
    pf->depth         = readshort(pheader);
    pf->firstchar     = readlong(pheader);
    pf->defaultchar   = readlong(pheader);
    pf->size          = readlong(pheader);

    /* get variable font data sizes*/
    /* # words of bitmap_t*/
    pf->bits_size     = readlong(pheader);
    *noffset          = readlong(pheader);
    *nwidth           = readlong(pheader);

    return pf;
}

static uint32_t le32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The record blocks of a merged font, described after its header, and whether
   its forms are font_han_forms.h's: none if they aren't, or if one isn't
   within its codes and its bits */
static int font_load_records(int fd, struct font *pf, struct record_block *record,
                             bool *forms)
{
    unsigned char d[12 + 16 * RECORD_BLOCKS];
    *forms = false;
    if (read(fd, d, sizeof(d)) != sizeof(d) || memcmp(d, "RWFR", 4)
        || le32(d + 4) > RECORD_BLOCKS)
        return 0;
    const int n = le32(d + 4);
    for (int i = 0; i < n; i++)
    {
        struct record_block *r = &record[i];
        const unsigned char *b = d + 12 + 16 * i;
        r->first = le32(b);
        r->last = le32(b + 4);
        r->offset = le32(b + 8);
        r->size = le32(b + 12);
        /* a record within a glyph's entry of the cache (load_cache_entry()) */
        if (r->first < pf->firstchar || r->first > r->last
            || r->last >= pf->firstchar + pf->size
            || r->size < 1 || r->size > 1 + (uint32_t)glyph_bytes(pf, pf->maxwidth)
            || r->offset > (uint32_t)pf->bits_size
            || ((uint32_t)pf->bits_size - r->offset) / r->size < r->last - r->first + 1)
            return 0;
    }
    *forms = le32(d + 8) == FONT_HAN_CHECK;
    return n;
}

/* load a font with room for glyphs, limited to bufsize if not zero */
static bool fonts_held;
static bool held[MAXFONTS];

/* Skins going away and others coming with the same fonts: what is loaded stays, and
   asking for it again doesn't even open the file */
void font_hold(bool hold)
{
    for (int i = 0; i < MAXFONTS; i++)
    {
        if (hold && buflib_allocations[i] > 0)
        {
            struct buflib_alloc_data *pdata = core_get_data(buflib_allocations[i]);
            pdata->refcount++;
            held[i] = true;
        }
        else if (!hold && held[i])
        {
            font_unload(i);
            held[i] = false;
        }
    }
    fonts_held = hold;
}

int font_load_ex( const char *path, size_t buf_size, int glyphs )
{
    /* needed to handle the font properly after it's loaded */
    size_t path_len = strlen(path);
    if ( path_len >= MAX_PATH )
        return -1;

    if (fonts_held)
    {
        int held_id = find_font_index(path);
        if (held_id > FONT_SYSFIXED)
        {
            struct buflib_alloc_data *pd = core_get_data(buflib_allocations[held_id]);
            pd->refcount++;
            return held_id;
        }
    }

    //printf("\nfont_load_ex(%s, %d, %d)\n", path, buf_size, glyphs);
    int fd = open(path, O_RDONLY|O_BINARY);
    if ( fd < 0 )
        return -1;

#ifdef UNICODE32
    if (glyphs && glyphs < 3)
        glyphs = 3; /* Guarantee we'll always have at least 2 after alignment */
#else
    if (glyphs && glyphs < 2)
        glyphs = 2; /* Guarantee we'll always have at least 1 after alignment */
#endif

    /* load font struct f with file header */
    int file_size = ffilesize( fd );
    struct font header;
    struct font f;

    uint32_t nwidth, noffset;
    if ( !font_load_header( fd, &header, &f, &nwidth, &noffset )
#if LCD_DEPTH < 16
        || f.depth
#endif
    )
    {
        close(fd);
        return -1;
    }
    struct record_block record[RECORD_BLOCKS];
    bool forms = false;
    const int records = font_is_merged(&f) ? font_load_records(fd, &f, record, &forms) : 0;

    /* examine f and calc buffer size */
    bool cached = false;
    size_t bufsize = buf_size;
    size_t glyph_buf_size = font_glyphs_to_bufsize( &f, glyphs );

    if ( bufsize && glyphs && bufsize > glyph_buf_size)
         bufsize = glyph_buf_size;
    else
    {
        if ( glyphs )
            bufsize = glyph_buf_size;
        else
            bufsize = MAX_FONT_SIZE;
    }
#ifdef FONT_HARD_LIMIT
    if ( bufsize > MAX_FONT_SIZE )
        bufsize = MAX_FONT_SIZE;
#endif
    if ( bufsize < (size_t) file_size )
        cached = true;
    else
        bufsize = file_size;

    /* check already loaded */
    int font_id = find_font_index(path);

    if (font_id > FONT_SYSFIXED)
    {
        /* already loaded, no need to reload */
        struct buflib_alloc_data *pd = core_get_data(buflib_allocations[font_id]);
        if (pd->font.buffer_size < bufsize || pd->path_bufsz < path_len)
        {
            int old_refcount, old_id;
            size_t old_bufsize = pd->font.buffer_size;
            bool failed = false;
            /* reload the font:
             * 1) save of refcont and id
             * 2) force unload (set refcount to 1 to make sure it get unloaded)
             * 3) reload with the larger buffer
             * 4) restore the id and refcount
             */
            old_id = font_id;
            old_refcount = pd->refcount;
            pd->refcount = 1;
            font_unload(font_id);
            font_id = font_load_ex(path, bufsize, glyphs);
            if (font_id < 0)
            {
                failed = true;
                font_id = font_load_ex(path, old_bufsize, 0);
                /* we couldn't even get the old size, this shouldn't happen */
                if ( font_id < 0 )
                {
                    close(fd);
                    return -1;
                }
            }
            if (old_id != font_id)
            {
                buflib_allocations[old_id] = buflib_allocations[font_id];
                buflib_allocations[font_id] = -1;
                font_id = old_id;
            }
            pd = core_get_data(buflib_allocations[font_id]);
            pd->refcount = old_refcount;
            if(failed)
            {
                /* return error because we didn't satisfy the new buffer size */
                close(fd);
                return -1;
            }
        }
        pd->refcount++;
        //printf("reusing handle %d for %s (count: %d)\n", font_id, path, pd->refcount);
        close(fd);
        return font_id;
    }

    int open_slot = -1;

    if ( fonts_held )
    {
        /* skins coming with other fonts: one held for the
           skins before that none has taken again makes way first, its memory
           with it, rather than the new one taking more, which the audio
           buffer gave, stopping the music; and rather than failing with
           every slot taken. The one whose memory holds the new one with the
           least to spare, else the largest: the first one held, smaller,
           left the rest to the audio buffer all the same */
        const size_t need = bufsize + MAX(path_len + 1, 64);
        int best = -1;
        size_t best_size = 0;
        for (int i = FONT_FIRSTUSERFONT; i < MAXFONTS; i++)
        {
            if (!held[i] || buflib_allocations[i] <= 0)
                continue;
            struct buflib_alloc_data *pd = core_get_data(buflib_allocations[i]);
            if (pd->refcount != 1)
                continue;
            const size_t size = pd->font.buffer_size + pd->path_bufsz;
            if (best < 0 || (size >= need ? best_size < need || size < best_size
                                          : best_size < need && size > best_size))
            {
                best = i;
                best_size = size;
            }
        }
        if (best >= 0)
        {
            held[best] = false;
            font_unload(best);
            if (buflib_allocations[best] < 0)
                open_slot = best;
        }
    }
    for (font_id = FONT_FIRSTUSERFONT; font_id < MAXFONTS && open_slot == -1;
         font_id++)
    {
        if (buflib_allocations[ font_id ] < 0)
            open_slot = font_id;
    }
    /* every place taken: the one of the font that gives way, rather than this
       one failing, unless another than its own owner has asked for it too, as
       a theme or a plugin by its path: theirs isn't to go from under them */
    struct buflib_alloc_data *pd = open_slot == -1 && yielding_id >= 0 ?
        core_get_data(buflib_allocations[yielding_id]) : NULL;
    if (pd && pd->refcount <= 1 + held[yielding_id])
    {
        const int id = yielding_id;
        pd->refcount = 1;
        held[id] = false;
        font_unload(id);
        open_slot = id;
    }
    if ( open_slot == -1 )
    {
        close(fd);
        return -1;
    }
    font_id = open_slot;
    size_t path_bufsz = MAX(path_len + 1, 64); /* enough size for common case */
    /* allocate mem */
    int handle = core_alloc_ex(
                     bufsize + path_bufsz + sizeof( struct buflib_alloc_data ),
                     &buflibops );
    if ( handle <= 0 )
    {
        close(fd);
        return -1;
    }
    struct buflib_alloc_data *pdata;

    pdata = core_get_data_pinned(handle);
    pdata->refcount     = 1;
    pdata->records = records;
    memcpy(pdata->record, record, sizeof(record));
    pdata->forms = forms;
    pdata->path = pdata->buffer + bufsize;
    pdata->path_bufsz = path_bufsz;
    /* save load path so we can recognize this font later */
    memcpy(pdata->path, path, path_len+1);

    /* load and init */
    struct font *pf = &pdata->font;
    memcpy(pf, &f, sizeof( struct font) );

    pf->fd = fd;
    pf->fd_width = pf->fd_offset = -1;
    pf->handle = handle;
    pf->disabled = false;

    pf->buffer_start = buffer_from_handle( pf->handle );
    pf->buffer_position = pf->buffer_start + FONT_HEADER_SIZE;
    pf->buffer_size = bufsize;
    pf->buffer_end = pf->buffer_start + bufsize;

    if ( cached )
    {
        if ( ! font_load_cached( pf, nwidth, noffset ) )
        {
            core_free( handle );
            return -1;
        }

        /* trick to get a small cache for each file section   *
         * during glyph_cache_load()                          */
        pf->fd_width  = open( path, O_RDONLY|O_BINARY );
        pf->fd_offset = open( path, O_RDONLY|O_BINARY );

        font_windows_open( pf );
        glyph_cache_load( path, pf );
        font_windows_close();

        /* cached font: pf->fd stays open until the font is unloaded */
        close( pf->fd_width );
        pf->fd_width = -1;
        close( pf->fd_offset );
        pf->fd_offset = -1;
    }
    else
    {
        lseek( fd, 0, SEEK_SET);
        read(fd, pf->buffer_start, pf->buffer_size);

        close( fd );
        pf->fd = -1;

        if ( ! font_load_in_memory( pf, nwidth, noffset ) )
        {
            core_free( handle );
            return -1;
        }
    }
    buflib_allocations[font_id] = handle;
    //printf("%s -> [%d] -> %d\n", path, font_id, *handle);
    core_put_data_pinned(pdata);
    logf("%s id: [%d], %s", __func__, font_id, path);
    return font_id; /* success!*/
}

int font_load(const char *path)
{
    return font_load_ex(path, MAX_FONT_SIZE, GLYPHS_TO_CACHE);
}

void font_unload(int font_id)
{
    if ( font_id < 0 || font_id >= MAXFONTS )
        return;
    int handle = buflib_allocations[font_id];
    if ( handle < 0 )
        return;
    struct buflib_alloc_data *pdata = core_get_data(handle);
    struct font* pf = &pdata->font;
    pdata->refcount--;
    if (pdata->refcount < 1)
    {
        logf("%s %s", __func__, pdata->path);
        if (pf && pf->fd >= 0)
        {
            glyph_cache_save(font_id);
            close(pf->fd);
        }
        core_free(handle);
        buflib_allocations[font_id] = -1;
        if (font_id == fallback_id)
            fallback_id = -1;
        if (font_id == yielding_id)
            yielding_id = -1;
        for (size_t i = 0; i < ARRAYLEN(hangul_ids); i++)
        {
            if (font_id == hangul_ids[i])
                hangul_ids[i] = -1;
        }

    }
}

void font_unload_all(void)
{
    int i;
    for (i=0; i<MAXFONTS; i++)
    {
        if (buflib_allocations[i] > 0)
        {
            struct buflib_alloc_data *alloc = core_get_data(buflib_allocations[i]);
            alloc->refcount = 1; /* force unload */
            font_unload(i);
        }
    }
}

static void font_disable(int font_id)
{
    if ( font_id < 0 || font_id >= MAXFONTS )
        return;
    int handle = buflib_allocations[font_id];
    if ( handle < 0 )
        return;
    struct buflib_alloc_data *pdata = core_get_data(handle);
    struct font *pf = &pdata->font;

    if (pf->fd >= 0)
    {
        /* save the cache, but it keep it in-RAM so that cache lookups
         * can still succeed on the same font */
        glyph_cache_save(font_id);
        close(pf->fd);
        pf->fd = -1;
        pf->disabled = true;
    }
}

void font_disable_all(void)
{
    for(int i = 0; i < MAXFONTS; i++)
        font_disable(i);
}

static void font_enable(int font_id)
{
    if ( font_id < 0 || font_id >= MAXFONTS )
        return;
    int handle = buflib_allocations[font_id];
    if ( handle < 0 )
        return;
    struct buflib_alloc_data *pdata = core_get_data_pinned(handle);
    struct font *pf = &pdata->font;

    if (pf->disabled && pf->fd < 0)
    {
        pf->fd = open(pdata->path, O_RDONLY);
        pf->disabled = false;
    }
    core_put_data_pinned(pdata);
}

void font_enable_all(void)
{
    for(int i = 0; i < MAXFONTS; i++)
        font_enable(i);
}


/*
 * Return a pointer to an incore font structure.
 * If the requested font isn't loaded/compiled-in,
 * decrement the font number and try again.
 */
struct font* font_get(int font)
{
    bool is_ui_font = (font == FONT_UI);
    struct font* pf;
    if (is_ui_font)
        font = ui_font;
    if (font <= FONT_SYSFIXED || font >= MAXFONTS)
        return &sysfont;

    while (1) {
        if (buflib_allocations[font] > 0)
        {
            struct buflib_alloc_data *alloc = core_get_data(buflib_allocations[font]);
            pf = &alloc->font;
            if (pf && pf->height)
                return pf;
        }
        if (--font < 0)
        {
            if (is_ui_font && ui_font != FONT_LASTUSERFONT)
            {
                /* The users preferred UI font couldn't be loaded */
                ui_font = FONT_LASTUSERFONT;
                font = ui_font; /* try from the top to find something loaded */
            }
            else
                return &sysfont; /* Nothing else is available */
        }
    }
}

/* When FONT_UI is supplied to font_get this font will be tried first if it is
 * not loaded then fall back to default of any loaded font wins */
void set_ui_font(int font)
{
    /* Note: we don't allow setting FONT_SYSFIXED here */
    if (font > FONT_SYSFIXED && font < MAXFONTS)
        ui_font = font;
    else /* default (same as supplying FONT_UI) */
        ui_font = FONT_LASTUSERFONT;
}


/* a glyph's record is read into its entry of the cache as it is */
_Static_assert(offsetof(struct font_cache_entry, bitmap)
               == offsetof(struct font_cache_entry, width) + 1,
               "a record is a glyph's width, then its bits");

/*
 * Reads an entry into cache entry
 */
static void
load_cache_entry(struct font_cache_entry* p, void* callback_data)
{
    struct font* pf = callback_data;

    ucschar_t char_code = p->_char_code;
    int fd;

    lock_font_handle(pf->handle, true);
    const struct buflib_alloc_data *alloc = (const void *)pf;
    const ucschar_t ch = pf->firstchar + char_code;
    for (int i = 0; i < alloc->records; i++)
    {
        const struct record_block *r = &alloc->record[i];
        if (ch < r->first || ch > r->last)
            continue;
        /* its width and bits at once */
        if (font_read_at(pf, WINDOW_BITS, pf->fd,
                         FONT_HEADER_SIZE + r->offset + (ch - r->first) * r->size,
                         &p->width, r->size) != (ssize_t)r->size)
            p->width = 0;
        lock_font_handle(pf->handle, false);
        return;
    }
    if (pf->file_width_offset)
    {
        int width_offset = pf->file_width_offset + char_code;
        /* load via different fd to get this file section cached */
        if(pf->fd_width >=0 )
            fd = pf->fd_width;
        else
            fd = pf->fd;
        font_read_at(pf, WINDOW_WIDTHS, fd, width_offset, &(p->width), 1);
    }
    else
    {
        p->width = pf->maxwidth;
    }

    int32_t bitmap_offset = 0;

    if (pf->file_offset_offset)
    {
        int32_t offset = pf->file_offset_offset + char_code * (pf->long_offset ? sizeof(int32_t) : sizeof(int16_t));
        /* load via different fd to get this file section cached */
        if(pf->fd_offset >=0 )
            fd = pf->fd_offset;
        else
            fd = pf->fd;
        unsigned char tmp[2];
        if (font_read_at(pf, WINDOW_OFFSETS, fd, offset, tmp, 2) == 2)
        {
            bitmap_offset = tmp[0] | (tmp[1] << 8);
            if (pf->long_offset) {
                if (font_read_at(pf, WINDOW_OFFSETS, fd, offset + 2, tmp, 2) == 2)
                    bitmap_offset |= (tmp[0] << 16) | (tmp[1] << 24);
            }
        }
    }
    else
    {
        bitmap_offset = char_code * glyph_bytes(pf, p->width);
    }

    int32_t file_offset = FONT_HEADER_SIZE + bitmap_offset;
    int src_bytes = glyph_bytes(pf, p->width);
    font_read_at(pf, WINDOW_BITS, pf->fd, file_offset, p->bitmap, src_bytes);

    lock_font_handle(pf->handle, false);
}

/*
 * Converts cbuf into a font cache
 */
static void cache_create(struct font* pf)
{
    /* maximum size of rotated bitmap */
    int bitmap_size = glyph_bytes(pf, pf->maxwidth);
    /* reserve one blank glyph that is guaranteed to be available, even
     * when the font file is closed during USB */
    unsigned char *cache_buf = pf->buffer_start + bitmap_size;
    size_t cache_size = pf->buffer_size - bitmap_size;
    ALIGN_BUFFER(cache_buf, cache_size, sizeof(ucschar_t));
    memset(pf->buffer_start, 0, bitmap_size);
    /* Initialise cache */
    font_cache_create(&pf->cache, cache_buf, cache_size, bitmap_size);
}

/*
 * Returns width of character
 */
int font_get_width(struct font* pf, ucschar_t char_code)
{
    int width;
    struct font_cache_entry *e;

    /* check input range*/
    if (char_code < pf->firstchar || char_code >= pf->firstchar+pf->size)
        char_code = pf->defaultchar;
    char_code -= pf->firstchar;

    if (pf->fd >= 0 && pf != &sysfont)
        width = font_cache_get(&pf->cache,char_code,false,load_cache_entry,pf)->width;
    else if (pf->disabled &&
        (e = font_cache_get(&pf->cache,char_code,true,load_cache_entry,pf)))
        width = e->width; /* falls back to pf->maxwidth if !e */
    else if (pf->width)
        width = pf->width[char_code];
    else
        width = pf->maxwidth;

    return width;
}

const unsigned char* font_get_bits(struct font* pf, ucschar_t char_code)
{
    const unsigned char* bits;

    /* check input range*/
    if (char_code < pf->firstchar || char_code >= pf->firstchar+pf->size)
        char_code = pf->defaultchar;
    char_code -= pf->firstchar;

    if (pf->fd >= 0 && pf != &sysfont)
    {
        bits =
            (unsigned char*)font_cache_get(&pf->cache, char_code,
                                false, load_cache_entry, pf)->bitmap;
    }
    else if (pf->disabled)
    {
        /* the font handle is closed, but the cache is intact. Attempt
         * a lookup, which is very likely to succeed. Return a placeholder
         * glyph on miss (again, this is very unlikely */
        struct font_cache_entry *e = font_cache_get(&pf->cache, char_code,
                                                    true, NULL, NULL);
        if (LIKELY(e))
            bits = (unsigned char *) e->bitmap;
        else
        {
            /* Could attempt to find a suitable fallback glyph from the same
             * font. For now just return blank space which is
             * reserved by cache_create() at buffer_start */
            bits = pf->buffer_start;
        }
    }
    else
    {
        /* This font is entirely in RAM */
        bits = pf->bits;
        if (pf->offset)
        {
            if (pf->bits_size < MAX_FONTSIZE_FOR_16_BIT_OFFSETS)
                bits += ((uint16_t*)(pf->offset))[char_code];
            else
                bits += ((uint32_t*)(pf->offset))[char_code];
        }
        else
            bits += char_code * glyph_bytes(pf, pf->maxwidth);
    }

    return bits;
}

extern int open_pathfmt(char *buf, size_t size, int oflag, const char *pathfmt, ...); /*misc.c*/
static NO_INLINE int font_path_open_glyphcache(const char *font_path, int oflags)
{
    char glyph_path[MAX_PATH];
    int dotidx = strlen(font_path) - sizeof(FONT_EXT);
    logf("%s %.*s.%s", __func__, dotidx, font_path, GLYPH_CACHE_EXT);
    return open_pathfmt(glyph_path, sizeof(glyph_path), oflags, 
                        "%.*s.%s", dotidx, font_path, GLYPH_CACHE_EXT);
}

/* call with NULL to flush */
static void glyph_file_write(void* data)
{
    struct font_cache_entry* p = data;
    struct font* pf = cache_pf;
    ucschar_t ch;
    static int buffer_pos = 0;
#define WRITE_BUFFER 256
    static unsigned char buffer[WRITE_BUFFER];

    /* flush buffer & reset */
    if ( data == NULL || buffer_pos >= WRITE_BUFFER)
    {
        write(cache_fd, buffer, buffer_pos);
        buffer_pos = 0;
        if ( data == NULL )
            return;
    }
    if ( p->_char_code == FONT_CACHE_NONE )
        return;

    ch = p->_char_code + pf->firstchar;
#ifdef UNICODE32
    buffer[buffer_pos] = (ch >> 24) & 0xff;
    buffer[buffer_pos+1] = (ch >> 16) & 0xff;
    buffer[buffer_pos+2] = (ch >> 8) & 0xff;
    buffer[buffer_pos+3] = ch & 0xff;
    buffer_pos += 4;
#else
    buffer[buffer_pos] = (ch >> 8) & 0xff;
    buffer[buffer_pos+1] = ch & 0xff;
    buffer_pos += 2;
#endif
    return;
}

/* save the char codes of the loaded glyphs to a file */
static void glyph_cache_save(int font_id)
{
    int fd;

    if( font_id < 0 )
        return;
    int handle = buflib_allocations[font_id];
    if ( handle < 0 )
        return;

    struct buflib_alloc_data *pdata = core_get_data_pinned(handle);
    struct font *pf = &pdata->font;

    if(pf && pf->fd >= 0)
    {
        fd = font_path_open_glyphcache(pdata->path, O_WRONLY|O_CREAT|O_TRUNC);
        if (fd >= 0)
        {
            uint32_t header = FC_HEADER_VAL;
            write(fd, &header, sizeof(header));
            cache_pf = pf;
            cache_fd = fd;
            lru_traverse(&cache_pf->cache._lru, glyph_file_write);
            glyph_file_write(NULL);
            if (cache_fd >= 0)
            {
                close(cache_fd);
                cache_fd = -1;
            }
        }
    }
    core_put_data_pinned(pdata);
    return;
}


static int ucscharcmp(const void *a, const void *b)
{
    return ((int)(*(ucschar_t*)a - *(ucschar_t*)b));
}

static NO_INLINE void glyph_cache_load(const char *font_path, struct font *pf)
{
#define MAX_SORT 256
#ifndef BOOTLOADER
    /* The fallback's glyphs are of other alphabets, few and far apart in its 2 MB:
       loaded ahead from what was seen before, each needed reads of its own, 0.3 s
       at boot for glyphs that may not show at all. They load as they're drawn. */
    if (is_fallback_file(font_path))
        return;
#endif
    if (pf->fd >= 0) {
        int i, size, fd;
        unsigned char tmp[sizeof(ucschar_t)];
        ucschar_t ch;
        ucschar_t glyphs[MAX_SORT];
        ucschar_t glyphs_lru_order[MAX_SORT];
        unsigned int glyph_file_skip=0, glyph_file_size=0;

        int sort_size = pf->cache._capacity;
        if ( sort_size > MAX_SORT )
             sort_size = MAX_SORT;

        fd = font_path_open_glyphcache(font_path, O_RDONLY|O_BINARY);
#ifdef TRY_DEFAULT_GLYPHCACHE
        /* if font specific file fails, try default */
        if (fd < 0)
            fd = open(GLYPH_CACHE_FILE, O_RDONLY|O_BINARY);
#endif
        if (fd >= 0) {
            /* Header */
            uint32_t hdr = 0;
            read(fd, &hdr, sizeof(hdr));
            if (hdr != FC_HEADER_VAL)
                goto latin;
            /* only read what fits */
            glyph_file_size = ffilesize( fd );
            if (glyph_file_size < sizeof(uint32_t))
                goto latin;
            glyph_file_size -= sizeof(uint32_t);
            if ( glyph_file_size > (int)sizeof(ucschar_t)*pf->cache._capacity ) {
                glyph_file_skip = glyph_file_size - sizeof(ucschar_t)*pf->cache._capacity;
                lseek( fd, glyph_file_skip + sizeof(uint32_t), SEEK_SET );
            }
            while(1) {
                for ( size = 0;
                      size < sort_size && read( fd, tmp, sizeof(tmp) ) == sizeof(tmp); )
                {
#ifdef UNICODE32
                    ch = (tmp[0] << 24) | (tmp[1] << 16) | (tmp[2] << 8) | tmp[3];
#else
                    ch = (tmp[0] << 8) | tmp[1];
#endif
#ifndef BOOTLOADER
                    /* the other alphabets of a font with all of them load as
                       they're drawn: each a read of its own, of a file of
                       megabytes, for glyphs that may not show at all */
                    if (ch >= 0x0590 && font_is_merged(pf))
                        continue;
#endif
                    glyphs[size] = ch;
                    glyphs_lru_order[size++] = ch;
                }

                /* sort glyphs array to make sector cache happy */
                qsort((void *)glyphs, size, sizeof(ucschar_t),
                      ucscharcmp );

                /* load font bitmaps */
                for( i = 0; i < size ; i++ )
                    font_get_bits(pf, glyphs[i]);

                /* redo to fix lru order */
                for ( i = 0; i < size ; i++)
                    font_get_bits(pf, glyphs_lru_order[i]);

                if ( size < sort_size )
                    break;
            }

            close(fd);
        } else {
        latin:
            /* the fallback fonts are only ever asked for other alphabets */
            if (is_fallback_file(font_path))
                return;
            /* load latin1 chars into cache */
            for ( ch = 32 ; ch < 256  && ch < pf->cache._capacity + 32; ch++ )
                font_get_bits(pf, ch);
        }
    }
    return;
}
#else /* BOOTLOADER */

void font_init(void)
{
}

void font_lock(int font_id, bool lock)
{
    (void)font_id;
    (void)lock;
}

/*
 * Bootloader only supports the built-in sysfont.
 */
struct font* font_get(int font)
{
    (void)font;
    return &sysfont;
}

/*
 * Returns width of character
 */
int font_get_width(struct font* pf, ucschar_t char_code)
{
    /* check input range*/
    if (char_code < pf->firstchar || char_code >= pf->firstchar+pf->size)
        char_code = pf->defaultchar;
    char_code -= pf->firstchar;

    return pf->width? pf->width[char_code]: pf->maxwidth;
}

const unsigned char* font_get_bits(struct font* pf, ucschar_t char_code)
{
    const unsigned char* bits;

    /* check input range*/
    if (char_code < pf->firstchar || char_code >= pf->firstchar+pf->size)
        char_code = pf->defaultchar;
    char_code -= pf->firstchar;

    /* assume small font with uint16_t offsets*/
    bits = pf->bits + (pf->offset?
            ((uint16_t*)(pf->offset))[char_code]:
            (((pf->height + 7) / 8) * pf->maxwidth * char_code));

    return bits;
}

#endif /* BOOTLOADER */

/*
 * Returns the length (in bytes) of a given NULL terminated string
 *  stops after maxwidth, maxbytes or NULL (\0) whichever occurs first.
 * maxbytes = -1 ignores maxbytes and relies on NULL terminator (\0)
 *  to terminate the string
 * maxwidth = -1 ignores maxwidth
 * stringsize pixel width and height will be returned in *w and *h
 */
int font_measurestring(const unsigned char *str, size_t maxbytes, size_t maxwidth, int *w, int *h, int fontnum)
{
    const unsigned char *start = str;
    size_t bytes;
    struct font* pf = font_get(fontnum);
    font_lock( fontnum, true );
    ucschar_t ch;
    size_t width = 0;
#ifndef BOOTLOADER
    unsigned fallbacks_locked = 0;
    int form = -1; /* the ideographs', found at the first */
    /* Arabic's letters in the joined forms they're drawn in, which are much
       narrower than the letters of the text: measured by those, its lines
       right-aligned fell short of the right edge, and wrapped at two thirds
       of their width. Each form at the bytes of its letter. */
    struct arabic_join join = ARABIC_JOIN_INIT;
    size_t waiting_at = 0;
#endif

    while (true)
    {
        const size_t at = str - start;
        str = utf8decode(str, &ch);
        const bool end = ch == 0 || at >= maxbytes;
        ucschar_t shown[2];
        size_t shown_at[2] = { at, at };
        int n = 0;
#ifndef BOOTLOADER
        const bool waiting = join.prev != NULL;
        if (!waiting && ch < 0x621)
        {
            /* no letter of Arabic waits, and this isn't one: as it is */
            if (!end)
                shown[n++] = ch;
        }
        else
        {
            n = arabic_join_next(&join, end ? 0 : ch, shown);
            if (end || !arabic_join_letter(ch))
            {
                /* the letter that waited, then this character, not the end's
                   0 */
                if (waiting)
                    shown_at[0] = waiting_at;
                if (end)
                    n--;
            }
            else
            {
                /* the letter that waited, and this one waits, unless the two
                   make one */
                shown_at[0] = waiting_at;
                if (n || !waiting)
                    waiting_at = at;
            }
        }
#else
        if (!end)
            shown[n++] = ch;
#endif
        for (int k = 0; k < n; k++)
        {
            ucschar_t c = shown[k];
            if (IS_DIACRITIC(c) || font_char_invisible(c))
                continue;
            c = font_drawn_char(c);
#ifndef BOOTLOADER
            /* in the form it's drawn in, whose width may differ by a pixel */
            if (c >= 0x3000 && form < 0)
            {
                form = forced_form();
                if (form < 0)
                    form = font_text_form(start, maxbytes);
            }
#endif
            struct font* gf = font_for_char(pf, c, form, &c);
#ifndef BOOTLOADER
            if (gf != pf && !fallbacks_locked)
                fallbacks_locked = font_lock_all();
#endif
            int fw = font_get_width(gf, c);
            width += fw;
            if (width > maxwidth)
            {
                width -= fw;
                bytes = shown_at[k];
                goto measured;
            }
        }
        if (end)
        {
            bytes = at;
            break;
        }
    }
measured:

    if ( h )
        *h = pf->height;
    if ( w )
        *w = width;

#ifndef BOOTLOADER
    font_unlock_all(fallbacks_locked);
#endif
    font_lock( fontnum, false );

    return bytes;
}

/*
 * Returns the stringsize of a given NULL terminated string
 *  stops after maxbytes or NULL (\0) whichever occurs first.
 * maxbytes = -1 ignores maxbytes and relies on NULL terminator (\0)
 *  to terminate the string
 */
int font_getstringnsize(const unsigned char *str, size_t maxbytes, int *w, int *h, int fontnum)
{
    int width = 0;
    if (!w)
        w = &width;
    font_measurestring(str, maxbytes, -1, w, h, fontnum);
    return *w;
}

/*
 * Returns the stringsize of a given NULL terminated string.
 */
int font_getstringsize(const unsigned char *str, int *w, int *h, int fontnumber)
{
    return font_getstringnsize(str, -1, w, h, fontnumber);
}

/* -----------------------------------------------------------------
 * vim: et sw=4 ts=8 sts=4 tw=78
 */
