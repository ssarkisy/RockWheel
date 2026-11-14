/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2013 Thomas Martitz
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

#ifndef __LINE_H__
#define __LINE_H__

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#include "lcd.h"
#include "screens.h"

/* Possible line decoration styles. Specify one of
 * STYLE_NONE, _DEFAULT, _INVERT, _COLORBAR or _GRADIENT, and optionally
 * or with STYLE_COLORED specifying line_desc.text_color */
enum line_styles {
    /* Just print the text. Do not clear or draw line decorations */
    STYLE_NONE       = 0x00,
    /* Line background filled with the bg color (or backdrop if present) */ 
    STYLE_DEFAULT    = 0x01,
    /* Like STYLE_DFEAULT except that text and background color will be swapped */
    STYLE_INVERT     = 0x02,
    /* Line background filled with line color line_desc.line_color */
    STYLE_COLORBAR   = 0x04,
    /* Line background filled with gradient, colors taken from
     * line_desc.line_color and line_desc.line_end_color */
    STYLE_GRADIENT   = 0x08,
    /* Modifier for the text color, which will be taken from line_desc.text_color */
    STYLE_COLORED    = 0x10,
    /* Modifier for a chevron at the right end of the line, for a list item
     * that leads to more */
    STYLE_CHEVRON    = 0x20,
    /* Modifier for a speaker at the right end of the line, for the song
     * playing, paused too */
    STYLE_PLAYING    = 0x40,
    /* These are used internally */
    _STYLE_DECO_MASK = 0x0f,
    _STYLE_MODE_MASK = 0x7F,
};

struct line_desc {
    /* height of the line (in pixels). -1 to inherit the height
     * from the font. The text will be centered if the height is larger,
     * but the decorations will span the entire height */
    int height;
    /* multiline support: For some decorations (e.g. gradient) to work
     * across multiple lines (e.g. to draw a line selector across 2 lines)
     * the line index and line count must be known. For normal, single
     * lines specify nlines=1 and line=0 */
    /* line count of a group */
    int16_t nlines;
    /* index of the line in the group */
    int16_t line;
    /* line text color if STYLE_COLORED is specified, in native
     * lcd format (convert with LCD_RGBPACK() if necessary) */
    unsigned text_color;
    /* line color if STYLE_COLORBAR or STYLE_GRADIENT is specified, in native
     * lcd format (convert with LCD_RGBPACK() if necessary) */
    unsigned line_color, line_end_color;
    /* line decorations, see STYLE_DEFAULT etc. */
    enum line_styles style;
    /* whether the line can scroll */
    bool scroll;
    /* height of the line separator (in pixels). 0 to disable drawing
     * of the separator */
    int8_t separator_height;
    /* A short text at the right end, as a song's length on an album's
     * screen, grey as a setting's value: in value_font, value_shift pixels
     * below the text's top (line_lead_shift()), against the right of a
     * column value_width wide, which the text stops short of; left of a
     * chevron, right of a speaker. "" for none, and none right to left */
    int8_t value_shift;
    int16_t value_font, value_width;
    char value[24];
    /* A part of a taller row, as an album's name over its year and artist:
     * the row's height and the part's top in it, 0 for a line that is a
     * row. The selection's gradient and bevel go over the whole row, part by
     * part, and the speaker and the chevron in its middle, on strips of the
     * whole row's height. text_dy moves the text down from the part's middle */
    int16_t row_height, row_top;
    int8_t text_dy;
};

/* default initializer, can be used for static initialitation also.
 * This initializer will result in single lines without style that don't scroll */
#define LINE_DESC_DEFINIT { .style = STYLE_DEFAULT, .height = -1, .separator_height = 0, .line = 0, .nlines = 1, .scroll = false }

/**
 * Print a line at a given pixel postion, using decoration information from
 * line and content information from the format specifier. The format specifier
 * can include tags that depend on further parameters given to the function
 * (similar to the well-known printf()).
 *
 * Tags start with the $ sign. Below is a list of the currently supported tags:
 * $s - insert a column (1px wide) of empty space.
 * $S - insert a column (1 char wide) of empty space.
 * $i - insert an icon. put_line() expects a corresponding parameter of the
 *      type 'enum themable_icons'. If Icon_NOICON is passed, then empty
 *      space (icon-wide) will be inserted.
 * $I - insert an icon with padding. Works like $i but additionally
 *      adds a column (1px wide) of empty space on either side of the icon.
 * $t - insert text. put_line() expects a corresponding parameter of the type
 *      'const char *'
 * $$ - insert a '$' char, use it to escape $.
 *
 * $I, $s and $S support the following two forms:
 * $n[IsS] - inserts n columns (pixels/chars respectively) of empty space
 * $*[IsS] - inserts n columns (pixels/chars respectively) of empty space. put_line()
 *           expects a correspinding paramter of the type 'int' that specifies n.
 *
 * $t supports the following two forms:
 * $nt     - skips the first n pixels when displaying the string
 * $*t     - skips the first n pixels when displaying the string. put_line()
 *           expects a correspinding paramter of the type 'int' that specifies n.
 *
 * Inline text will be printed as is (be sure to escape '$') and can be freely
 * intermixed with tags. Inline text will be truncated after MAX_PATH+31 bytes.
 * If you have a longer inline string use a separate buffer and pass that via $t,
 * which does not suffer from this truncation.
 * 
 * Text can appear anywhere, before or after (or both) tags. However, when the
 * line can scroll, only the last piece of text (whether inline or via $t) can
 * scroll. This is due to a scroll_engine limitation.
 *
 * x, y - pixel postion of the line.
 * line - holds information for the line decorations
 * fmt  - holds the text and optionally format tags
 * ...  - additional paramters for the format tags
 *
 */
void  put_line(struct screen *display,
               int x, int y, struct line_desc *line,
               const char *fmt, ...);


/**
 * Print a line at a given pixel postion, using decoration information from
 * line and content information from the format specifier. The format specifier
 * can include tags that depend on further parameters given to the function
 * (similar to the well-known vprintf()).
 *
 * For details, see put_line(). This function is equivalent, except for
 * accepting a va_list instead of a variable paramter list.
 */
void vput_line(struct screen *display,
               int x, int y, struct line_desc *line,
               const char *fmt, va_list ap);

/* The marks of the catalogue's playlists and folders before their names, in a
   column of their own: a computer for those synced from it, an iPod for those
   made on it */
#define LINE_MARK_MENU_COUNT 11
enum line_mark { LINE_MARK_NONE, LINE_MARK_COMPUTER, LINE_MARK_IPOD,
                 /* a theme's own, in the same column: a plus before a row
                    that makes something new, the column empty, and Music's
                    rows' marks, LINE_MARK_MENU and the row's number in
                    LINE_MARK_MENU_COUNT */
                 LINE_MARK_PLUS, LINE_MARK_BLANK, LINE_MARK_MENU,
                 LINE_MARK_MENU_LAST = LINE_MARK_MENU + LINE_MARK_MENU_COUNT - 1,
                 /* and of a podcast's episodes, in a narrower column: a dot
                    for a new one, a half dot for one begun, and the column
                    empty for another, its name in line with theirs */
                 LINE_MARK_NEW, LINE_MARK_HALF, LINE_MARK_EMPTY };
int line_mark_width(struct screen *display);
/* the column of this mark, 0 where it isn't drawn: an episode's right to
   left too, at x from the other end, as the covers' */
int line_mark_column(struct screen *display, int mark);
/* A short text before the name, as an album's year or a song's number: grey,
   as a setting's value, in a column as wide as so many of the font's widest
   digit and a gap, against the gap's side of it, as numbers in a column go.
   The column's width, and in *digit the widest digit's: measured once for a
   list, in the font it's drawn in. Its top, shift pixels below the name's:
   its digits in the middle of the name's capitals, which are taller, rather
   than on their baseline, where they looked low (line_lead_shift(), on the
   baseline with a font without grey levels). Drawn at x of a line put at y,
   after it, in a column of these. */
int line_lead_width(struct screen *display, int font, int digits, int *digit);
int line_lead_shift(int name_font, int font);
/* The column of a line's value (line_desc.value) as wide as text would be in
   font, each of its digits as wide as the font's widest, as the longest of a
   list's values is measured once for it */
int line_value_width(int font, const char *text);
void line_draw_lead(struct screen *display, int font, int shift, int x, int y,
                    const struct line_desc *line, const char *text,
                    int width, int digit);
void line_draw_mark(struct screen *display, int x, int y,
                    const struct line_desc *line, int mark);
/* A list's scrollbar that is a line of a pixel over its rows' end: its column
   and the first and last rows of its track and of its thumb, of the screen.
   A row whose text scrolls paints itself again from its start to its end,
   over the bar: line_set_bar() names the bar for the rows put after it, NULL
   for none, and each of them draws its own rows of the bar again after
   itself. On the main screen. */
struct line_bar {
    int16_t x, top, bottom, thumb_top, thumb_bottom;
    /* the selected row's rows where the theme has everything on it in its
       text's colour (UI_METRIC_SELECTION_INK): the thumb so there and no
       track; sel_bottom under sel_top for none */
    int16_t sel_top, sel_bottom;
};
void line_set_bar(const struct line_bar *bar);
/* the bar's rows from y, of the screen, in the viewport set */
void line_draw_bar(struct screen *display, const struct line_bar *bar,
                   int y, int height);
/* the scrolling of the text of the parts of taller rows stopped, as a list's
   is as it's drawn again */
void line_scroll_stop_parts(struct screen *display);
/* text as wide as width at most in font: itself if it fits, or else in buf
   as many of its letters as fit with "…" after them; NULL if none does */
const char *line_fit(const char *text, int font, int width, char *buf,
                     size_t size);
/* The second line of a row of two, as an album's year and artist under its
   name: in font, grey as a setting's value, as the selection's text on it,
   from x of the part put at y, its cell top_dy pixels below the part's top,
   and cut short with "…" before the speaker and the chevron. Its start up to
   a SUBLINE_ACCENT_END, which isn't drawn, is in the speaker's blue instead,
   as a show's new episodes: who makes a line of a tag's text takes the mark
   out of it. The part's background is drawn already, by a put_line() of no
   text */
#define SUBLINE_ACCENT_END '\x1f'
void line_draw_subline(struct screen *display, int font, int x, int y,
                       int top_dy, const struct line_desc *line,
                       const char *text);
/* An album's cover at x, y of a row: pixels of LINE_COVER_SIZE a side, or
   NULL for the note on a tile in its place, as an album without one has and
   one not read yet */
#define LINE_COVER_SIZE 32
/* A rectangle in colour on the viewport's ground, its corners rounded to
   radius r, their pixels mixed with the ground */
void line_fill_rounded(struct screen *display, int x, int y, int w, int h,
                       int r, unsigned colour);
/* The same over whatever the screen shows there: its corners' pixels blended
   with what is under them, so not to be drawn twice over itself */
void line_fill_rounded_over(struct screen *display, int x, int y, int w, int h,
                            int r, unsigned colour);
/* The column of a text's first glyph in that font where its ink starts: for
   two texts in two fonts to start at the same pixel */
int line_ink_left(int font, const char *text);
/* The corners of a picture w by h at x, y painted over in the ground's
   colour, to a rounding of radius r; nothing for 0 */
void line_round_corners(struct screen *display, int x, int y, int w, int h, int r);
/* Only the top two, of a picture w wide at x, y: one whose reflection under
   it goes on from its foot */
void line_round_top_corners(struct screen *display, int x, int y, int w, int r);
/* A missing cover's note, a mask of w by h as a font's glyphs have alpha, at
   x, y: in the theme's colours for it, from its top row to its foot */
void line_draw_note(const unsigned char *alpha, int w, int h, int x, int y);
/* the same of a mask whose rows are stride pixels apart, as a picture's
   alpha is kept: each row starts a byte. Blended over what the screen shows:
   the caller has just filled its place */
void line_draw_note_rows(const unsigned char *alpha, int stride, int w, int h,
                         int x, int y);
void line_draw_cover(struct screen *display, int x, int y,
                     const fb_data *pixels);
/* Quick Scroll's letter, or a year, in a box of its own around it, its middle
   at cx, cy of the viewport set: in white over the list, which shows through
   the box. In font, Quick Scroll's own, or in text_font for a letter that
   font hasn't or without it (-1). On the main screen only. */
void line_draw_initial(struct screen *display, int cx, int cy, const char *text,
                       int font, int text_font);
bool line_font_has(int font, const char *text);
/* The bar a list's selected row has, for what is chosen outside a list, in
   the current viewport: returns the colour of the text on it */
unsigned line_selection_bar(struct screen *display, int x, int y, int width,
                            int height);

#endif /* __LINE_H__*/
