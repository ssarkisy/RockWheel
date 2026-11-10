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
#ifndef _FONT_H
#define _FONT_H

#include <stdlib.h>
#include "inttypes.h"
#include "stdbool.h"

/*
 * Incore font and image definitions
 */
#include "config.h"

#ifndef __PCTOOL__
#include "font_cache.h"
#include "sysfont.h"
#endif

/*
 * Fonts are specified by number, and used for display
 * of menu information as well as mp3 filename data.
 * At system startup, up to MAXFONTS fonts are initialized,
 * either by being compiled-in, or loaded from disk.
 * If the font asked for does not exist, then the
 * system uses the next lower font number.  Font 0
 * must be available at system startup.
 * Fonts are specified in firmware/font.c.
 */
enum {
    FONT_SYSFIXED = -1, /* system fixed pitch font*/
    FONT_FIRSTUSERFONT = 0, /* first id for the user fonts */
};
/* 16: the theme's 7, the fallback, one's own Hangul's three and the
   quick-scroll letter, with room to spare. A place costs a few bytes; it's the
   fonts loaded that take memory */
#define MAXUSERFONTS 16

/* SYSFONT, FONT_UI, FONT_UI_REMOTE + MAXUSERFONTS fonts in skins */
#define MAXFONTS (FONT_FIRSTUSERFONT + MAXUSERFONTS)
#define FONT_UI MAXFONTS
#define FONT_LASTUSERFONT (MAXFONTS-1)

/*
 * .fnt loadable font file format definition
 *
 * format                     len  description
 * -------------------------  ---- ------------------------------
 * UCHAR version[4]              4   magic number and version bytes
 * USHORT maxwidth               2   font max width in pixels
 * USHORT height                 2   font height in pixels
 * USHORT ascent                 2   font ascent (baseline) in pixels
 * USHORT depth                  2   depth of the font, 0=1bit and 1=4bit
 * ULONG firstchar               4   first character code in font
 * ULONG defaultchar             4   default character code in font
 * ULONG size                    4   # characters in font
 * ULONG nbits                   4   # bytes imagebits data in file
 * ULONG noffset                 4   # longs offset data in file
 * ULONG nwidth                  4   # bytes width data in file
 * MWIMAGEBITS bits          nbits   image bits variable data
 * [MWIMAGEBITS padded to 16-bit boundary]
 * USHORT offset         noffset*2   offset variable data
 * UCHAR width            nwidth*1   width variable data
 */

/* loadable font magic and version #*/
#define VERSION "RB12"

/* builtin C-based proportional/fixed font structure */
/* based on The Microwindows Project http://microwindows.org */
struct font {
    int          maxwidth;        /* max width in pixels*/
    unsigned int height;          /* height in pixels*/
    int          ascent;          /* ascent (baseline) height*/
    unsigned int firstchar;       /* first character in bitmap*/
    int          size;            /* font size in glyphs*/
    int          depth;           /* depth of the font, 0=1bit and 1=4bit */
    const unsigned char *bits;    /* 8-bit column bitmap data*/
    const void *offset;           /* offsets into bitmap data,
                                     uint16_t if bits_size < 0xFFDB else uint32_t*/
    const unsigned char *width;   /* character widths or NULL if fixed*/
    int          defaultchar;     /* default char (not glyph index)*/
    int32_t      bits_size;       /* # bytes of glyph bits*/

    /* file, buffer and cache management */
    int          fd;              /* fd for the font file. >= 0 if cached */
    int          fd_width;        /* fd for the font file. >= 0 if cached */
    int          fd_offset;       /* fd for the font file. >= 0 if cached */
    int          handle;          /* core_allocator handle */
    unsigned char *buffer_start;    /* buffer to store the font in */
    unsigned char *buffer_position; /* position in the buffer */
    unsigned char *buffer_end;      /* end of the buffer */
    size_t         buffer_size;     /* size of the buffer in bytes */
    bool         disabled;        /* font disabled (use blank as fallback if not in cache) */
#ifndef __PCTOOL__
    struct font_cache cache;
    uint32_t file_width_offset;    /* offset to file width data    */
    uint32_t file_offset_offset;   /* offset to file offset data   */
    int long_offset;
#endif

};

/* font routines*/
void font_init(void) INIT_ATTR;
bool font_filename_matches_loaded_id(int font_id, const char *filename);
int font_load(const char *path);
int font_load_ex(const char *path, size_t buffer_size, int glyphs);
void font_hold(bool hold);
void font_unload(int font_id);
/* The font gives its place up to any other loaded when every place is taken,
   as Quick Scroll's letter does, the least missed: unloaded for it, which its
   user finds by its path (font_find()). One at a time; -1 for none. */
void font_set_yielding(int font_id);
void font_unload_all(void);
void font_lock(int font_id, bool lock);

/* Characters drawn as nothing and taking no room: the joiners, the marks,
   embeddings and isolates of direction, the selectors of a character's form,
   as the one that asks for "❤" as an emoji, and the byte order mark. A font
   drew its blank or Unifont its box for them. */
static inline bool font_char_invisible(ucschar_t ch)
{
    return (ch >= 0x200b && ch <= 0x200f) || (ch >= 0x202a && ch <= 0x202e)
        || (ch >= 0x2060 && ch <= 0x206f) || (ch >= 0xfe00 && ch <= 0xfe0f)
        || ch == 0xfeff || ch == 0x061c || ch == 0x180e;
}

/* What's drawn for a character: one past the plane that the fonts go to, as an
   emoji like "🌎", as an empty square of the theme's font, as on a stock iPod.
   Unifont drew its box of the code in hex. The line and paragraph separators
   as a space, as the words they part shouldn't run into one. */
static inline ucschar_t font_drawn_char(ucschar_t ch)
{
    return ch > 0xffff ? 0x25a1 : (ch == 0x2028 || ch == 0x2029) ? ' ' : ch;
}

/* The theme's fonts of the menus and the text have every alphabet the iPod
   shows in fonts of its own; another font takes those letters from the nearest
   of them not taller, and a letter none has comes from a fallback font, loaded
   with the theme and read from disk only when it's needed. */
#ifdef BOOTLOADER
#define font_for_char(pf, ch, form, glyph) (*(glyph) = (ch), (pf))
#else
/* Unifont; the first call, at boot, also removes the old Library and scripts
   files and loads one's own Hangul */
void font_load_fallback(void);
/* the font of a character, and in *glyph the code of its glyph there, in the
   form given (font_han_form()) */
struct font* font_for_char(struct font* pf, ucschar_t ch, int form, ucschar_t *glyph);
/* whether the glyph of a letter from the font it gave stands on the line's
   baseline when it's taller than the line: Hangul's of the size above the
   line's, cut to stand on that of its own size, rather than centred */
bool font_on_baseline(struct font *gf, ucschar_t ch);
/* every font loaded, while glyphs are drawn: the mask to unlock */
unsigned font_lock_all(void);
void font_unlock_all(unsigned locked);

/* The ideographs and kana in the forms of China, Japan or Taiwan: Japan's in a
   line with kana, else those of the menus' language */
enum { FONT_HAN_SC, FONT_HAN_JP, FONT_HAN_TC };
void font_set_han_form(int form);
int font_han_form(const ucschar_t *text);
/* the form of a text's ideographs, from at most maxbytes of it */
int font_text_form(const unsigned char *text, size_t maxbytes);
/* the form of a whole text for its parts drawn or measured alone, as a row cut
   short with "…" or wrapped lyrics, in the calling thread; -1 for each
   string's own again */
void font_set_text_form(int form);
#endif


/* Closes the file descriptor if the font file (if cached) but keeps
 * the cache intact, so font_get_{bits,width} still work. */
void font_disable_all(void);
/* Re-opens the file descriptor of the font file. Should be called as
 * counter-part of font_disable_all(); */
void font_enable_all(void);

struct font* font_get(int font);
/* the id of a font loaded from this path, as by a skin, or -1: none is; the
   font isn't loaded for it, nor its count of users raised */
int font_find(const char *path);
void set_ui_font(int font); /* when FONT_UI is supplied this font will be tried first */

int font_measurestring(const unsigned char *str, size_t maxbytes, size_t maxwidth, int *w, int *h, int fontnum);
int font_getstringnsize(const unsigned char *str, size_t maxbytes, int *w, int *h, int fontnumber);
int font_getstringsize(const unsigned char *str, int *w, int *h, int fontnumber);
int font_get_width(struct font* ft, ucschar_t ch);
const unsigned char * font_get_bits(struct font* ft, ucschar_t ch);

#endif
