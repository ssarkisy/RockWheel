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

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "string-extra.h"

#include "scroll_engine.h"
#include "system.h"
#include "line.h"
#include "gcc_extensions.h"
#include "icon.h"
#include "screens.h"
#include "settings.h"
#include "debug.h"
#include "viewport.h"
#include "language.h"
#include "debug.h"
#include "list.h"
#include "font.h"
#include "rbunicode.h"

#ifdef HAVE_REMOTE_LCD
#define MAX_LINES (LCD_SCROLLABLE_LINES + LCD_REMOTE_SCROLLABLE_LINES)
#else
#define MAX_LINES  LCD_SCROLLABLE_LINES
#endif


static void style_line(struct screen *display, int x, int y, struct line_desc *line);

static void put_text(struct screen *display, int x, int y, struct line_desc *line,
                      const char *text, bool prevent_scroll, int text_skip_pixels);
static void draw_chevron(struct screen *display, int y, struct line_desc *line);
static void draw_speaker(struct screen *display, int y, struct line_desc *line);
static void draw_value(struct screen *display, int y, struct line_desc *line);
static int decoration_width(struct screen *display, const struct line_desc *line);

struct line_desc_scroll {
    struct line_desc desc;
    int rtl_cut; /* right to left, the viewport's end cut off for the text */
    /* of a part of a taller row: the row's viewport, the part's top in it and
       its whole width; the text scrolls in part_vps. NULL for another line */
    struct viewport *row_vp;
    int part_top, full_width;
    bool used;
};

/* The rows of a part of a taller row as a viewport of the row's, which the
   part's text is drawn in: its font's cell is taller than the part, and what
   it would draw past it, scrolling, nothing would paint over again. One for
   each screen, where the selected row's name scrolls alone */
static struct viewport part_vps[NB_SCREENS];

void line_scroll_stop_parts(struct screen *display)
{
    display->scroll_stop_viewport(&part_vps[display->screen_type]);
}

/* Allocate MAX_LINES+1 because at the time get_line_desc() is called
 * the scroll engine did not yet determine that it ran out of lines
 * (because puts_scroll_func() wasn't called yet. Therefore we can
 * run out of lines before setting the used field. By allocating
 * one item more we can survive that point and set used to false
 * if the scroll engine runs out of lines */
static struct line_desc_scroll lines[MAX_LINES+1];

static struct line_desc_scroll *get_line_desc(void)
{
    static unsigned line_index;
    struct line_desc_scroll *this;

    do
    {
        this = &lines[line_index++];
        if (line_index >= ARRAYLEN(lines))
            line_index = 0;
    } while (this->used);

    return this;
}

static void scroller(struct scrollinfo *s, struct screen *display)
{
    /* style_line() expects the entire line rect, including padding, to
     * draw selector properly across the text+padding. however struct scrollinfo
     * has only the rect for the text itself, which is off depending on the
     * line padding. this needs to be corrected for calling style_line().
     * The alternative would be to really redraw only the text area,
     * but that would complicate the code a lot */ 
    struct line_desc_scroll *line = s->userdata;
    if (!s->line)
    {
        line->used = false;
    }
    else if (line->row_vp)
    {
        /* the text in its part's viewport, current now, as wide as the
           row's but for what's before it right to left; then what's at the
           right end, in the row's middle, in the row's */
        struct viewport *vp = *display->current_viewport;
        const int width = vp->width;
        vp->width = line->full_width - line->rtl_cut;
        style_line(display, s->x, 0, &line->desc);
        put_text(display, s->x, s->y, &line->desc, s->line, true, s->offset);
        vp->width = width;
        struct viewport *part = display->set_viewport(line->row_vp);
        if (line->desc.style & STYLE_PLAYING)
            draw_speaker(display, line->part_top, &line->desc);
        draw_value(display, line->part_top, &line->desc);
        if (line->desc.style & STYLE_CHEVRON)
            draw_chevron(display, line->part_top, &line->desc);
        display->set_viewport(part);
    }
    else
    {
        int y = s->y - (line->desc.height/2 - display->getcharheight()/2)
                - line->desc.text_dy;
        /* the text's bar again, short of what's before it right to left */
        struct viewport *vp = *display->current_viewport;
        vp->width -= line->rtl_cut;
        style_line(display, s->x, y, &line->desc);
        put_text(display, s->x, s->y, &line->desc, s->line, true, s->offset);
        vp->width += line->rtl_cut;
        if (line->desc.style & STYLE_PLAYING)
            draw_speaker(display, y, &line->desc);
        draw_value(display, y, &line->desc);
        if (line->desc.style & STYLE_CHEVRON)
            draw_chevron(display, y, &line->desc);
    }
}

static void scroller_main(struct scrollinfo *s)
{
    scroller(s, &screens[SCREEN_MAIN]);
}

#ifdef HAVE_REMOTE_LCD
static void scroller_remote(struct scrollinfo *s)
{
    scroller(s, &screens[SCREEN_REMOTE]);
}
#endif

static void (*scrollers[NB_SCREENS])(struct scrollinfo *s) = {
    scroller_main,
#ifdef HAVE_REMOTE_LCD
    scroller_remote,
#endif
};

static void put_icon(struct screen *display, int x, int y,
                     struct line_desc *line,
                     enum themable_icons icon)
{
    unsigned drmode = DRMODE_FG;
    /* Need to change the drawmode:
     * mono icons should behave like text, inverted on the selector bar
     * native (colored) icons should be drawn as-is */
    if (get_icon_format(display->screen_type) == FORMAT_MONO && (line->style & STYLE_INVERT))
        drmode = DRMODE_SOLID | DRMODE_INVERSEVID;

    display->set_drawmode(drmode);
    screen_put_iconxy(display, x, y, icon);
}


static void put_text(struct screen *display,
                      int x, int y, struct line_desc *line,
                      const char *text, bool prevent_scroll,
                      int text_skip_pixels)
{
    /* set drawmode because put_icon() might have changed it */
    unsigned drmode = DRMODE_FG;
    if (line->style & STYLE_INVERT)
        drmode = DRMODE_SOLID | DRMODE_INVERSEVID;

    display->set_drawmode(drmode);

    /* The text short of a speaker or a chevron at the right end, on their strip
       of background: as wide as the line, a title that ended under them didn't
       scroll and lost its end, and one that did never showed it */
    struct viewport *vp = *display->current_viewport;
    const int vp_width = vp->width;
    const int deco = decoration_width(display, line);
    if (deco > 0 && deco < vp->width - x)
        vp->width -= deco;
    /* Right to left, the text runs leftwards from x short of the right end,
       where there may be something before it, as an album's year: the
       viewport ends at x for it. Its scrolling, which the scroll engine
       doesn't mirror, then has the text's area, and leaves that alone */
    int rtl_cut = 0;
    if (VP_IS_RTL(vp) && x > 0 && x < vp->width)
    {
        rtl_cut = x;
        vp->width -= x;
        x = 0;
    }
    /* of a part of a taller row, in the part's rows alone: in a viewport of
       them, the scroller's when it scrolls (part_vps) */
    struct viewport part;
    struct viewport *part_vp = NULL;
    int part_top = 0;
    if (line->row_height > 0 && !prevent_scroll)
    {
        part_top = y - (line->height/2 - display->getcharheight()/2)
                   - line->text_dy;
        part_vp = line->scroll ? &part_vps[display->screen_type] : &part;
        *part_vp = *vp;
        part_vp->y += part_top;
        part_vp->height = line->height;
        y -= part_top;
        display->set_viewport(part_vp);
    }

    if (line->scroll && !prevent_scroll)
    {
        bool scrolls;
        struct line_desc_scroll *line_data = get_line_desc();
        line_data->desc = *line;
        /* precalculate to avoid doing it in the scroller, it's save to
         * do this on the copy of the original line_desc*/
        if (line_data->desc.height == -1)
            line_data->desc.height = display->getcharheight();
        line_data->rtl_cut = rtl_cut;
        line_data->row_vp = part_vp ? vp : NULL;
        line_data->part_top = part_top;
        line_data->full_width = vp_width;
        scrolls = display->putsxy_scroll_func(x, y, text,
            scrollers[display->screen_type], line_data, text_skip_pixels);
        line_data->used = scrolls;
    }
    else
        display->putsxy_scroll_func(x, y, text, NULL, NULL, text_skip_pixels);
    if (part_vp)
        display->set_viewport(vp);
    vp->width = vp_width;
}

/* A line consists of:
 * |[Ss]|[i]|[Ss]|[t]|, where s is empty space (pixels), S is empty space
 * (n space characters), i is an icon and t is the text.
 *
 * All components are optional. However, even if none are specified the whole
 * line will be cleared and redrawn.
 *
 * For empty space with the width of an icon use i and pass Icon_NOICON as
 * corresponding argument.
 */
static void print_line(struct screen *display,
                      int x, int y, struct line_desc *line,
                      const char *fmt, va_list ap)
{
    const char *str;
    bool num_is_valid;
    int ch, num, height;
    int xpos = x;
    int icon_y, icon_h, icon_w;
    enum themable_icons icon;
    char tempbuf[MAX_PATH+32];
    unsigned int tempbuf_idx;
    int max_width = display->getwidth();

    height = line->height == -1 ? display->getcharheight() : line->height;
    icon_h = get_icon_height(display->screen_type);
    icon_w = get_icon_width(display->screen_type);
    tempbuf_idx = 0;
    /* For the icon use a differnet calculation which to round down instead.
     * This tends to align better with the font's baseline for small heights.
     * A proper aligorithm would take the baseline into account but this has
     * worked sufficiently well for us (fix this if needed) */
    icon_y = y + (height - icon_h)/2;
    /* vertically center string on the line
     * x/2 - y/2 rounds up compared to (x-y)/2 if one of x and y is odd */
    y += height/2 - display->getcharheight()/2 + line->text_dy;

    /* parse format string */
    while (xpos < max_width)
    {
        ch = *fmt++;
        /* need to check for escaped '$' */
        if (ch == '$' && *fmt != '$')
        {
            /* extra flag as num == 0 can be valid */
            num_is_valid = false;
            num = 0;
            if (tempbuf_idx)
            {   /* flush pending inline text */
                tempbuf_idx = tempbuf[tempbuf_idx] = 0;
                put_text(display, xpos, y, line, tempbuf, false, 0);
                xpos += display->getstringsize(tempbuf, NULL, NULL);
                if (xpos >= max_width)
                    return;
            }
next:
            ch = *fmt++;
            switch(ch)
            {
                case '*': /* num from parameter list */
                    num = va_arg(ap, int);
                    num_is_valid = true;
                    goto next;

                case 'i': /* icon (without pad) */
                case 'I': /* icon with pad */
                    if (ch == 'i')
                        num = 0;
                    else /* 'I' */
                        if (!num_is_valid)
                            num = 1;
                    icon = va_arg(ap, int);
                    /* draw it, then skip over */
                    if (icon != Icon_NOICON)
                        put_icon(display, xpos + num, icon_y, line, icon);
                    xpos += icon_w + num*2;
                    break;

                case 'S':
                    if (!num_is_valid)
                        num = 1;
                    xpos += num * display->getcharwidth();
                    break;

                case 's':
                    if (!num_is_valid)
                        num = 1;
                    xpos += num;
                    break;

                case 't':
                    str = va_arg(ap, const char *);
                    put_text(display, xpos, y, line, str, false, num);
                    xpos += display->getstringsize(str, NULL, NULL);
                    break;

                default:
                    if (LIKELY(isdigit(ch)))
                    {
                        num_is_valid = true;
                        num = 10*num + ch - '0';
                        goto next;
                    }
                    else
                    {
                        /* any other character here is an erroneous format string */
                        display->putsxyf(xpos, y, "<E:%c>", ch);
                        /* Don't consider going forward, fix the caller */
                        return;
                    }
            }
        }
        else
        {   /* handle string constant in format string */
            if (tempbuf_idx < sizeof(tempbuf)-1)
            {
                tempbuf[tempbuf_idx++] = ch;
            }
            else if (tempbuf_idx == sizeof(tempbuf)-1)
            {
                tempbuf[tempbuf_idx++] = '\0';
                DEBUGF("%s ", ch ? "put_line: String truncated" : "");
            }
            if (!ch)
            {   /* end of format string. flush pending inline string, if any */
                if (tempbuf[0])
                    put_text(display, xpos, y, line, tempbuf, false, 0);
                return;
            }
            else if (ch == '$')
                fmt++; /* escaped '$', display just once */
        }
    }
}

/* The line ends its row at the bottom, as a part of a taller one may not:
   the bevel's dark edge and the separator are under the whole row */
static bool ends_row(const struct line_desc *line, int height)
{
    return line->row_height <= 0 || line->row_top + height >= line->row_height;
}

#ifdef HAVE_LCD_COLOR
/* c1 moved pct percent of the way to c2 */
static unsigned mix_color(unsigned c1, unsigned c2, int pct)
{
    /* signed, the unpacked components are unsigned */
    int r = RGB_UNPACK_RED(c1), g = RGB_UNPACK_GREEN(c1), b = RGB_UNPACK_BLUE(c1);
    r += ((int)RGB_UNPACK_RED(c2) - r) * pct / 100;
    g += ((int)RGB_UNPACK_GREEN(c2) - g) * pct / 100;
    b += ((int)RGB_UNPACK_BLUE(c2) - b) * pct / 100;
    return LCD_RGBPACK(r, g, b);
}

/* A lighter top and a darker bottom edge on a gradient selection bar, as
   on a stock iPod */
static void draw_bevel(struct screen *display, int x, int y, int width,
                       int height, struct line_desc *line)
{
    unsigned fg = display->get_foreground();
    if (line->line == 0 && line->row_top == 0)
    {
        display->set_foreground(mix_color(line->line_color, LCD_WHITE, 45));
        display->hline(x, x + width - 1, y);
    }
    if (line->line == line->nlines - 1 && height > 3
        && ends_row(line, line->height))
    {
        display->set_foreground(mix_color(line->line_end_color, LCD_BLACK, 15));
        display->hline(x, x + width - 1, y + height - 2);
        display->set_foreground(mix_color(line->line_end_color, LCD_BLACK, 35));
        display->hline(x, x + width - 1, y + height - 1);
    }
    display->set_foreground(fg);
}
#endif

static void style_line(struct screen *display,
                       int x, int y, struct line_desc *line)
{
    int style = line->style;
    int width = display->getwidth();
    int height = line->height == -1 ? display->getcharheight() : line->height;
    int bar_height = height;

    /* mask out gradient and colorbar styles for non-color displays */
    if (display->depth < 16 && (style & (STYLE_COLORBAR|STYLE_GRADIENT)))
    {
        style &= ~(STYLE_COLORBAR|STYLE_GRADIENT);
        style |= STYLE_INVERT;
    }

    if (line->separator_height > 0 && (line->line == line->nlines-1)
        && ends_row(line, height))
    {
        int sep_height = MIN(line->separator_height, height);
        display->set_drawmode(DRMODE_FG);
#ifdef HAVE_LCD_COLOR
        display->set_foreground(global_settings.list_separator_color);
#endif
        display->fillrect(x, y + height - sep_height, width, sep_height);
        bar_height -= sep_height;
#ifdef HAVE_LCD_COLOR
        display->set_foreground(global_settings.fg_color);
#endif
    }

    /* mask out gradient and colorbar styles for non-color displays */
    if (display->depth < 16)
    {
        if (style & (STYLE_COLORBAR|STYLE_GRADIENT))
        {
            style &= ~(STYLE_COLORBAR|STYLE_GRADIENT);
            style |= STYLE_INVERT;
        }
        style &= ~STYLE_COLORED;
    }

    switch (style & _STYLE_DECO_MASK)
    {
#ifdef HAVE_LCD_COLOR
        case STYLE_GRADIENT:
        {
            /* of a part, the whole row's gradient from the part's top */
            const int row = line->row_height > 0 ? line->row_height : height;
            display->set_drawmode(DRMODE_FG);
            display->gradient_fillrect_part(x, y, width, bar_height,
                                            line->line_color,
                                            line->line_end_color,
                                            row*line->nlines,
                                            row*line->line + line->row_top);
            if (global_settings.list_selector_bevel)
                draw_bevel(display, x, y, width, bar_height, line);
            break;
        }
        case STYLE_COLORBAR:
            /*display->set_drawmode(DRMODE_FG);*/
            display->set_foreground(line->line_color);
            /*display->fillrect(x, y, width - x, bar_height);*/
            /* Fall through */
#endif
        case STYLE_INVERT:
            display->set_drawmode(DRMODE_FG);
            display->fillrect(x, y, width - x, bar_height);
            break;
        case STYLE_DEFAULT: default:
            display->set_drawmode(DRMODE_BG | DRMODE_INVERSEVID);
            display->fillrect(x, y, width - x, bar_height);
            break;
        case STYLE_NONE:
            break;
    }
#if (LCD_DEPTH > 1 || (defined(LCD_REMOTE_DEPTH) && LCD_REMOTE_DEPTH > 1))
    /* prepare fg and bg colors for text drawing, be careful to not
     * override any previously set colors unless mandated by the style */
    if (display->depth > 1)
    {
        if (style & STYLE_COLORED)
        {
            if (style & STYLE_INVERT)
                display->set_background(line->text_color);
            else
                display->set_foreground(line->text_color);
        }
        else if (style & (STYLE_GRADIENT|STYLE_COLORBAR))
            display->set_foreground(line->text_color);
    }
#endif
}

/* The strip of the line's background from x to the right end, under what's
   drawn there: of a part of a taller row, the whole row's, as what's on it is
   in the row's middle and goes over into the other part. Drawn again with
   what's on it, rather than over it, which would darken its edges */
static void style_strip(struct screen *display, int x, int y,
                        struct line_desc *line)
{
    if (line->row_height <= 0)
    {
        style_line(display, x, y, line);
        return;
    }
    struct line_desc row = *line;
    row.height = line->row_height;
    row.row_top = 0;
    style_line(display, x, y - line->row_top, &row);
}

/* the top of something h high in the middle of the line at y, or of its row */
static int line_middle(struct screen *display, int y,
                       const struct line_desc *line, int h)
{
    if (line->row_height > 0)
        return y - line->row_top + (line->row_height - h) / 2;
    const int height = line->height == -1 ? display->getcharheight() : line->height;
    return y + (height - h) / 2;
}

/* The chevron at the right end of a line of an item that leads to more, as
   on a stock iPod. It goes over whatever text scrolled there, on a strip of
   the line's own background. */
#define CHEVRON_MARGIN 6
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
/* drawn like anti-aliased glyphs: 9x11, lines 2.6 pixels thick, in Rockbox'
   alpha format (4 bits from 0 opaque to 15 transparent, low nibble first,
   rows run on without padding) */
#define CHEVRON_W 9
#define CHEVRON_H 11
static const unsigned char chevron_alpha[] = {
    0x3a, 0xf7, 0xff, 0xff, 0x5f, 0x00, 0xd3, 0xff, 0xff, 0x2d, 0x00, 0x90,
    0xff, 0xff, 0x5e, 0x00, 0x50, 0xfe, 0xff, 0xaf, 0x01, 0x20, 0xfd, 0xff,
    0xbf, 0x00, 0x70, 0xff, 0xaf, 0x01, 0x20, 0xfd, 0x5e, 0x00, 0x50, 0xfe,
    0x2d, 0x00, 0x90, 0xff, 0x5f, 0x00, 0xd3, 0xff, 0xff, 0x3a, 0xf7, 0xff,
    0xff, 0xff,
};
/* the speaker of the song playing, in the same format: made by
   themes/tools/make_list_glyphs.py */
#define SPEAKER_W 15
#define SPEAKER_H 12
static const unsigned char speaker_alpha[] = {
    0xff, 0xff, 0xff, 0xdf, 0xff, 0xbf, 0xfe, 0xff, 0xff, 0xff, 0x6f, 0xf7,
    0xef, 0x40, 0xff, 0xff, 0xff, 0x4e, 0x70, 0xef, 0xaf, 0x80, 0xff, 0xff,
    0x3e, 0x00, 0xe7, 0x70, 0x5f, 0xf1, 0x07, 0x00, 0x00, 0x70, 0x6f, 0xd0,
    0x0b, 0x7b, 0x00, 0x00, 0x00, 0xf7, 0x0d, 0xe9, 0x90, 0x07, 0x00, 0x00,
    0x70, 0xdf, 0x90, 0x0e, 0x79, 0x00, 0x00, 0x00, 0xf7, 0x06, 0xbd, 0xb0,
    0xff, 0xef, 0x03, 0x70, 0x0e, 0xf7, 0x15, 0xff, 0xff, 0xef, 0x04, 0xf7,
    0xfe, 0x0a, 0xf8, 0xff, 0xff, 0xff, 0x76, 0xff, 0x0e, 0xf4, 0xff, 0xff,
    0xff, 0xff, 0xfd, 0xff, 0xeb, 0xff,
};
/* the shuffle arrows of a list's Shuffle Songs, by the same tool */
#define SHUFFLE_W 16
#define SHUFFLE_H 12
static const unsigned char shuffle_alpha[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0x9f, 0xf9, 0xff, 0xbd, 0xcb, 0xff, 0xff,
    0xef, 0x6b, 0x40, 0xfd, 0x01, 0x00, 0xe4, 0xff, 0x1a, 0x00, 0x00, 0xd1,
    0xbd, 0x8b, 0x40, 0xbf, 0x30, 0x6b, 0x40, 0xfd, 0xff, 0xff, 0x0a, 0x15,
    0xe3, 0x9f, 0xf9, 0xff, 0xff, 0xff, 0x7f, 0x10, 0xfd, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x7f, 0x10, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0a, 0x15,
    0xe3, 0x9f, 0xf9, 0xff, 0xbd, 0x8b, 0x40, 0xbf, 0x30, 0x6b, 0x40, 0xfd,
    0x01, 0x00, 0xe4, 0xff, 0x1a, 0x00, 0x00, 0xd1, 0xbd, 0xcb, 0xff, 0xff,
    0xef, 0x6b, 0x40, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0x9f, 0xf9, 0xff,
};
/* the marks of the catalogue's playlists and folders, by the same tool */
#define COMPUTER_W 13
#define COMPUTER_H 12
static const unsigned char computer_alpha[] = {
    0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x00, 0xf0, 0xff, 0xff, 0xff,
    0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x00, 0xf0, 0xff, 0xff,
    0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf7, 0xff,
    0xff, 0x00, 0xf0, 0xff, 0xff, 0xff, 0x2f, 0x00, 0x00, 0x20, 0xff, 0xff,
    0xff, 0x02, 0x00, 0x00, 0xf2, 0xff,
};
#define IPOD_W 10
#define IPOD_H 13
static const unsigned char ipod_alpha[] = {
    0x07, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
    0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x50, 0xee, 0x05, 0x00, 0x00, 0xe0, 0xff, 0x0e, 0x00, 0x00, 0xe0, 0xff,
    0x0e, 0x00, 0x00, 0x50, 0xee, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x07, 0x00, 0x00, 0x00, 0x70,
};
/* the note on the tile in place of a cover in an album's row, the one of an
   album's header (album_header.c) smaller, by the same tool */
#define SMALL_NOTE_W 16
#define SMALL_NOTE_H 19
static const unsigned char small_note_alpha[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xef, 0x7a, 0xa3, 0xff, 0xff, 0xff, 0xdf,
    0x6a, 0x02, 0x00, 0x90, 0xff, 0xff, 0x6d, 0x02, 0x00, 0x00, 0x00, 0x90,
    0xff, 0xff, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x90, 0xff, 0xff, 0x0b, 0x00,
    0x00, 0x73, 0xca, 0x90, 0xff, 0xff, 0x0b, 0x74, 0xeb, 0xff, 0xdf, 0x90,
    0xff, 0xff, 0x0b, 0xfd, 0xff, 0xff, 0xdf, 0x90, 0xff, 0xff, 0x0b, 0xfd,
    0xff, 0xff, 0xdf, 0x90, 0xff, 0xff, 0x0b, 0xfd, 0xff, 0xff, 0xdf, 0x90,
    0xff, 0xff, 0x0b, 0xfd, 0xff, 0xff, 0xdf, 0x90, 0xff, 0xff, 0x0b, 0xfd,
    0xff, 0xff, 0xdf, 0x90, 0xff, 0xff, 0x0b, 0xfd, 0xff, 0xff, 0xdf, 0x90,
    0xff, 0xff, 0x0b, 0xfd, 0xff, 0xef, 0x9a, 0x90, 0xef, 0x48, 0x04, 0xfd,
    0xff, 0x08, 0x00, 0x90, 0x2d, 0x00, 0x00, 0xfd, 0x9f, 0x00, 0x00, 0x90,
    0x04, 0x00, 0x00, 0xfd, 0x4f, 0x00, 0x00, 0xd0, 0x01, 0x00, 0x40, 0xff,
    0x6f, 0x00, 0x00, 0xf9, 0x08, 0x00, 0xe5, 0xff, 0xff, 0x68, 0xc7, 0xff,
    0xdf, 0xdb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
extern void lcd_alpha_bitmap_part(const unsigned char *src, int src_x,
                                  int src_y, int stride, int x, int y,
                                  int width, int height);
#endif

static void draw_chevron(struct screen *display, int y, struct line_desc *line)
{
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (display->screen_type == SCREEN_MAIN)
    {
        int x = display->getwidth() - CHEVRON_W - CHEVRON_MARGIN;
        style_strip(display, x - CHEVRON_MARGIN, y, line);
        display->set_drawmode(DRMODE_FG);
        lcd_alpha_bitmap_part(chevron_alpha, 0, 0, CHEVRON_W,
                              x, line_middle(display, y, line, CHEVRON_H),
                              CHEVRON_W, CHEVRON_H);
        return;
    }
#endif
    /* the font's single angle quotation mark elsewhere */
    static const unsigned char chevron[] = "\xe2\x80\xba"; /* U+203A */
    int w, h;
    display->getstringsize(chevron, &w, &h);
    int x = display->getwidth() - w - CHEVRON_MARGIN;

    style_strip(display, x - CHEVRON_MARGIN, y, line);
    display->set_drawmode((line->style & STYLE_INVERT) ?
                          DRMODE_SOLID|DRMODE_INVERSEVID : DRMODE_FG);
    display->putsxy(x, line_middle(display, y, line, display->getcharheight()),
                    chevron);
}

#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
/* how light a colour of the settings is, roughly */
static int colour_light(unsigned colour)
{
    return RGB_UNPACK_RED(colour) * 77 + RGB_UNPACK_GREEN(colour) * 150
           + RGB_UNPACK_BLUE(colour) * 29;
}

/* the blue of the selection's gradient that stands out more from the list:
   its dark end on a light one, its light start on the dark theme's */
static unsigned speaker_colour(void)
{
    const int bg = colour_light(global_settings.bg_color);
    const int end = colour_light(global_settings.lse_color);
    const int start = colour_light(global_settings.lss_color);
    return abs(end - bg) >= abs(start - bg) ? global_settings.lse_color
                                            : global_settings.lss_color;
}
#endif

/* the line has a value at its right end, drawn: not right to left */
static bool has_value(struct screen *display, const struct line_desc *line)
{
    return line->value[0] && display->screen_type == SCREEN_MAIN && !lang_is_rtl();
}

/* The speaker of the song playing at the right end of its line, as on a stock
   iPod: in the blue of the selection's end on the list, and as the selection's
   text on it, on a strip of the line's own background, which goes on under
   what's right of it, drawn after it. Left of a value and a chevron if the
   line has them, and at the left end with right-to-left text. */
static void draw_speaker(struct screen *display, int y, struct line_desc *line)
{
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (display->screen_type != SCREEN_MAIN)
        return;
    int x;
    if (lang_is_rtl())
        x = CHEVRON_MARGIN; /* where the text ends, without a strip */
    else
    {
        x = display->getwidth() - SPEAKER_W - CHEVRON_MARGIN;
        if (line->style & STYLE_CHEVRON)
            x -= CHEVRON_W + CHEVRON_MARGIN;
        if (has_value(display, line))
            x -= line->value_width + CHEVRON_MARGIN;
        style_strip(display, x - CHEVRON_MARGIN, y, line);
    }
    const bool selected = line->style & (STYLE_INVERT|STYLE_COLORBAR|STYLE_GRADIENT);
    const unsigned fg = display->get_foreground();
    if (!selected)
        display->set_foreground(speaker_colour());
    display->set_drawmode(DRMODE_FG);
    lcd_alpha_bitmap_part(speaker_alpha, 0, 0, SPEAKER_W,
                          x, line_middle(display, y, line, SPEAKER_H),
                          SPEAKER_W, SPEAKER_H);
    display->set_foreground(fg);
#else
    (void)display; (void)y; (void)line;
#endif
}

/* The value at the right end, in its column, which the text never reaches:
   on what was drawn there, the line's background or the speaker's strip.
   Grey as a setting's value, and as the selection's text on it */
static void draw_value(struct screen *display, int y, struct line_desc *line)
{
    if (!has_value(display, line))
        return;
    int right = display->getwidth() - CHEVRON_MARGIN;
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (line->style & STYLE_CHEVRON)
        right -= CHEVRON_W + CHEVRON_MARGIN;
#endif
    const int height = line->height == -1 ? display->getcharheight() : line->height;
    struct viewport *vp = *display->current_viewport;
    const int name_font = vp->font;
    /* in line with the name, in a row of two parts too */
    y += height/2 - display->getcharheight()/2 + line->text_dy + line->value_shift;
    int width;
    font_getstringsize(line->value, &width, NULL, line->value_font);
    const unsigned fg = display->get_foreground();
#ifdef HAVE_LCD_COLOR
    if (line->style & (STYLE_COLORED|STYLE_COLORBAR|STYLE_GRADIENT))
        display->set_foreground(line->text_color);
    else if (line->style & STYLE_INVERT)
        display->set_foreground(display->get_background());
    else
        display->set_foreground(LIST_VALUE_COLOUR);
#endif
    display->set_drawmode(DRMODE_FG);
    vp->font = line->value_font;
    display->putsxy(right - width, y, line->value);
    vp->font = name_font;
    display->set_foreground(fg);
    display->set_drawmode(DRMODE_SOLID);
}

int line_shuffle_width(struct screen *display)
{
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (display->screen_type == SCREEN_MAIN)
        return SHUFFLE_W + CHEVRON_MARGIN;
#endif
    (void)display;
    return 0;
}

#define MARK_COLUMN 13 /* the widest of them */
#define MARK_GAP    4  /* and the names close after it */
int line_mark_width(struct screen *display)
{
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (display->screen_type == SCREEN_MAIN && !lang_is_rtl())
        return MARK_COLUMN + MARK_GAP;
#endif
    (void)display;
    return 0;
}

/* in the middle of their column, in the colour of the names */
void line_draw_mark(struct screen *display, int x, int y,
                    const struct line_desc *line, int mark)
{
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (!line_mark_width(display))
        return;
    const unsigned char *alpha;
    int w, h;
    switch (mark)
    {
        case LINE_MARK_COMPUTER:
            alpha = computer_alpha; w = COMPUTER_W; h = COMPUTER_H;
            break;
        case LINE_MARK_IPOD:
            alpha = ipod_alpha; w = IPOD_W; h = IPOD_H;
            break;
        default:
            return;
    }
    const int height = line->height == -1 ? display->getcharheight() : line->height;
    const unsigned fg = display->get_foreground();
    if (line->style & (STYLE_COLORED|STYLE_COLORBAR|STYLE_GRADIENT))
        display->set_foreground(line->text_color);
    else if (line->style & STYLE_INVERT) /* as the text on the bar */
        display->set_foreground(display->get_background());
    display->set_drawmode(DRMODE_FG);
    lcd_alpha_bitmap_part(alpha, 0, 0, w, x + (MARK_COLUMN - w) / 2,
                          y + (height - h) / 2, w, h);
    display->set_foreground(fg);
    display->set_drawmode(DRMODE_SOLID);
#else
    (void)display; (void)x; (void)y; (void)line; (void)mark;
#endif
}

#define LEAD_GAP 8 /* as a setting's value from its name */
int line_lead_width(struct screen *display, int font, int digits, int *digit)
{
    (void)display;
    int widest = 0;
    for (char d[2] = "0"; d[0] <= '9'; d[0]++)
    {
        int w;
        font_getstringsize(d, &w, NULL, font);
        widest = MAX(widest, w);
    }
    *digit = widest;
    return digits * widest + LEAD_GAP;
}

/* The rows of its line a glyph's ink takes, its pixels at least half drawn:
   false for a font without grey levels, whose bits are laid out otherwise,
   or a glyph without ink. A grey-level glyph is its rows one after another,
   a pixel a nibble, 0 drawn and 15 not (lcd-16bit-common.c) */
static bool glyph_ink(int font, ucschar_t ch, int *top, int *bottom)
{
    struct font *pf = font_get(font);
    if (!pf->depth)
        return false;
    const int width = font_get_width(pf, ch);
    const unsigned char *bits = font_get_bits(pf, ch);
    *top = -1;
    for (int row = 0; row < (int)pf->height; row++)
    {
        for (int x = 0; x < width; x++)
        {
            const int p = row * width + x;
            if (((bits[p >> 1] >> ((p & 1) * 4)) & 0xf) < 8)
            {
                if (*top < 0)
                    *top = row;
                *bottom = row;
                break;
            }
        }
    }
    return *top >= 0;
}

int line_lead_shift(int name_font, int font)
{
    int name_top, name_bottom, top, bottom;
    if (glyph_ink(name_font, 'H', &name_top, &name_bottom)
        && glyph_ink(font, '0', &top, &bottom))
        return (name_top + name_bottom - top - bottom) / 2;
    return font_get(name_font)->ascent - font_get(font)->ascent;
}

int line_value_width(int font, const char *text)
{
    char widest = '0';
    int widest_w = 0;
    for (char d[2] = "0"; d[0] <= '9'; d[0]++)
    {
        int w;
        font_getstringsize(d, &w, NULL, font);
        if (w > widest_w)
        {
            widest = d[0];
            widest_w = w;
        }
    }
    char shape[sizeof(((struct line_desc *)0)->value)];
    strlcpy(shape, text, sizeof(shape));
    for (char *p = shape; *p; p++)
    {
        if (*p >= '0' && *p <= '9')
            *p = widest;
    }
    int width;
    font_getstringsize(shape, &width, NULL, font);
    return width;
}

/* right-aligned, 9 under 10, which right-to-left text mirrors to the name's
   side too: placed as if each of its digits were as wide as the widest, as
   the theme's all are, rather than measured row by row. A year fills its
   column as before */
void line_draw_lead(struct screen *display, int font, int shift, int x, int y,
                    const struct line_desc *line, const char *text,
                    int width, int digit)
{
    x += width - LEAD_GAP - digit * (int)strlen(text);
    const int height = line->height == -1 ? display->getcharheight() : line->height;
    struct viewport *vp = *display->current_viewport;
    const int name_font = vp->font;
    y += height/2 - display->getcharheight()/2 + shift;
    const unsigned fg = display->get_foreground();
#ifdef HAVE_LCD_COLOR
    if (line->style & (STYLE_COLORED|STYLE_COLORBAR|STYLE_GRADIENT))
        display->set_foreground(line->text_color);
    else if (line->style & STYLE_INVERT) /* as the text on the bar */
        display->set_foreground(display->get_background());
    else
        display->set_foreground(LIST_VALUE_COLOUR);
#endif
    display->set_drawmode(DRMODE_FG);
    vp->font = font;
    display->putsxy(x, y, text);
    vp->font = name_font;
    display->set_foreground(fg);
    display->set_drawmode(DRMODE_SOLID);
}

/* before the text, rather than at the end with the speaker and the chevron:
   that the row isn't a song, seen at once. Left alone by the text scrolling
   on the selection, which draws from where the text starts. Right to left at
   the right end, where the text starts, as an album's cover is */
void line_draw_shuffle(struct screen *display, int x, int y,
                       const struct line_desc *line)
{
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (!line_shuffle_width(display))
        return;
    const struct viewport *vp = *display->current_viewport;
    if (VP_IS_RTL(vp))
        x = vp->width - x - SHUFFLE_W;
    const int height = line->height == -1 ? display->getcharheight() : line->height;
    const unsigned fg = display->get_foreground();
    if (line->style & (STYLE_COLORED|STYLE_COLORBAR|STYLE_GRADIENT))
        display->set_foreground(line->text_color);
    display->set_drawmode(DRMODE_FG);
    lcd_alpha_bitmap_part(shuffle_alpha, 0, 0, SHUFFLE_W,
                          x, y + (height - SHUFFLE_H)/2, SHUFFLE_W, SHUFFLE_H);
    display->set_foreground(fg);
    display->set_drawmode(DRMODE_SOLID);
#else
    (void)display; (void)x; (void)y; (void)line;
#endif
}

const char *line_fit(const char *text, int font, int width, char *buf,
                     size_t size)
{
    static const char ellipsis[] = "\xe2\x80\xa6"; /* U+2026 */
    int w;
    font_getstringsize(text, &w, NULL, font);
    if (w <= width)
        return text;
    /* as many letters as fit with "…", whole ones, and no space before it */
    const int ellipsis_w = font_getstringsize(ellipsis, NULL, NULL, font);
    int n = MIN((int)strlen(text), (int)size - (int)sizeof(ellipsis));
    while (n > 0 && (text[n] & 0xc0) == 0x80)
        n--;
    while (n > 0 && font_getstringnsize(text, n, NULL, NULL, font)
                    + ellipsis_w > width)
    {
        do
            n--;
        while (n > 0 && (text[n] & 0xc0) == 0x80);
    }
    while (n > 0 && text[n - 1] == ' ')
        n--;
    if (n <= 0)
        return NULL;
    memcpy(buf, text, n);
    memcpy(buf + n, ellipsis, sizeof(ellipsis));
    return buf;
}

void line_draw_subline(struct screen *display, int font, int x, int y,
                       int top_dy, const struct line_desc *line,
                       const char *text)
{
    struct viewport *vp = *display->current_viewport;
    char buf[MAX_PATH];
    /* cut short in the form of the whole, as when it isn't */
    font_set_text_form(font_text_form((const unsigned char *)text, -1));
    text = line_fit(text, font, vp->width - x - decoration_width(display, line),
                    buf, sizeof(buf));
    if (!text)
    {
        font_set_text_form(-1);
        return;
    }
    const int name_font = vp->font;
    const unsigned fg = display->get_foreground();
#ifdef HAVE_LCD_COLOR
    if (line->style & (STYLE_COLORED|STYLE_COLORBAR|STYLE_GRADIENT))
        display->set_foreground(line->text_color);
    else if (line->style & STYLE_INVERT) /* as the text on the bar */
        display->set_foreground(display->get_background());
    else
        display->set_foreground(LIST_VALUE_COLOUR);
#endif
    display->set_drawmode(DRMODE_FG);
    vp->font = font;
    display->putsxy(x, y + top_dy, text);
    font_set_text_form(-1);
    vp->font = name_font;
    display->set_foreground(fg);
    display->set_drawmode(DRMODE_SOLID);
}

void line_draw_cover(struct screen *display, int x, int y,
                     const fb_data *pixels)
{
    if (pixels)
    {
        display->bitmap_part(pixels, 0, 0,
                             STRIDE(display->screen_type, LINE_COVER_SIZE,
                                    LINE_COVER_SIZE),
                             x, y, LINE_COVER_SIZE, LINE_COVER_SIZE);
        return;
    }
    /* as an album's header has it in its cover's place, smaller */
    const unsigned fg = display->get_foreground();
    display->set_foreground(LIST_GROUP_BAR_COLOUR);
    display->set_drawmode(DRMODE_SOLID);
    display->fillrect(x, y, LINE_COVER_SIZE, LINE_COVER_SIZE);
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (display->screen_type == SCREEN_MAIN)
    {
        display->set_foreground(LIST_GROUP_COLOUR);
        display->set_drawmode(DRMODE_FG);
        lcd_alpha_bitmap_part(small_note_alpha, 0, 0, SMALL_NOTE_W,
                              x + (LINE_COVER_SIZE - SMALL_NOTE_W) / 2,
                              y + (LINE_COVER_SIZE - SMALL_NOTE_H) / 2,
                              SMALL_NOTE_W, SMALL_NOTE_H);
    }
#endif
    display->set_foreground(fg);
    display->set_drawmode(DRMODE_SOLID);
}

/* Quick Scroll's box: as tall as three rows of the menus or two of 36 px, as
   wide or as its text and a margin each side, its corners rounded */
#define INITIAL_HEIGHT 72
#define INITIAL_MARGIN 16
#define INITIAL_RADIUS 11
#define INITIAL_CHARS  8   /* of its text, at most */

#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
/* how much of each pixel of a corner is inside its rounding, in sixteenths,
   by 4x4 points of each: the top left one's, its rows from the top */
static uint8_t initial_corner[INITIAL_RADIUS][INITIAL_RADIUS];

static void initial_corner_make(void)
{
    const int r = 8 * INITIAL_RADIUS; /* in eighths of a pixel */
    for (int y = 0; y < INITIAL_RADIUS; y++)
    {
        for (int x = 0; x < INITIAL_RADIUS; x++)
        {
            int in = 0;
            for (int j = 0; j < 4; j++)
            {
                for (int i = 0; i < 4; i++)
                {
                    const int dx = r - (8 * x + 2 * i + 1);
                    const int dy = r - (8 * y + 2 * j + 1);
                    in += dx * dx + dy * dy <= r * r;
                }
            }
            initial_corner[y][x] = in;
        }
    }
}

/* The blitter's alpha for a pixel of so many sixteenths of the colour over
   what's there: 0 all of it, 15 none, which it takes as 16 */
static unsigned initial_alpha(int sixteenths)
{
    const int a = 16 - sixteenths;
    return a > 8 ? a - 1 : a;
}

static void set_nibble(unsigned char *mask, int i, unsigned a)
{
    mask[i / 2] = i & 1 ? (mask[i / 2] & 0x0f) | a << 4 : (mask[i / 2] & 0xf0) | a;
}

/* a row of the box's alpha, w wide, edge rows from its top or bottom: the
   middle ones all alike, those of the corners rounded */
static void initial_row(unsigned char *mask, int w, int opacity, int edge)
{
    const unsigned a = initial_alpha(opacity);
    memset(mask, a | a << 4, (w + 1) / 2);
    if (edge >= INITIAL_RADIUS)
        return;
    for (int i = 0; i < INITIAL_RADIUS; i++)
    {
        const unsigned c = initial_alpha(opacity * initial_corner[edge][i] / 16);
        set_nibble(mask, i, c);
        set_nibble(mask, w - 1 - i, c);
    }
}
#endif

bool line_font_has(int font, const char *text)
{
    struct font *pf = font_get(font);
    ucschar_t ch;
    for (const unsigned char *p = (const unsigned char *)text; *p; )
    {
        p = utf8decode(p, &ch);
        /* a letter it hasn't has its default one's glyph */
        if (ch != (ucschar_t)pf->defaultchar
            && font_get_bits(pf, ch) == font_get_bits(pf, pf->defaultchar))
            return false;
    }
    return true;
}

void line_draw_initial(struct screen *display, int cx, int cy, const char *text,
                       int font, int text_font)
{
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (display->screen_type != SCREEN_MAIN)
        return;
    /* in Quick Scroll's own font if it has each of the letters, or else in
       the list's, from which another alphabet's come from a fallback */
    struct font *pf = font >= 0 && line_font_has(font, text) ? font_get(font) : NULL;
    ucschar_t ucs[INITIAL_CHARS];
    int n = 0, width = 0;
    for (const unsigned char *p = (const unsigned char *)text;
         *p && n < INITIAL_CHARS; n++)
        p = utf8decode(p, &ucs[n]);
    for (int i = 0; pf && i < n; i++)
        width += font_get_width(pf, ucs[i]);
    if (!pf)
        font_getstringsize(text, &width, NULL, text_font);

    static bool corner_made;
    if (!corner_made)
    {
        initial_corner_make();
        corner_made = true;
    }
    const int w = MIN(MAX(INITIAL_HEIGHT, width + 2 * INITIAL_MARGIN), LCD_WIDTH);
    const int h = INITIAL_HEIGHT;
    const int x = cx - w / 2, y = cy - h / 2;
    const unsigned colour = LIST_INITIAL_COLOUR;
    const int opacity = LIST_INITIAL_OPACITY;
    const unsigned fg = display->get_foreground();
    unsigned char middle[(LCD_WIDTH + 1) / 2], corner[(LCD_WIDTH + 1) / 2];
    initial_row(middle, w, opacity, INITIAL_RADIUS);
    display->set_drawmode(DRMODE_FG);
    for (int row = 0; row < h; row++)
    {
        const int edge = MIN(row, h - 1 - row);
        const unsigned char *mask = middle;
        if (edge < INITIAL_RADIUS)
        {
            initial_row(corner, w, opacity, edge);
            mask = corner;
        }
        /* a lighter top and a darker bottom, as the selection's bar has */
        display->set_foreground(row == 0 ? mix_color(colour, LCD_WHITE, 45)
                                : row == h - 1 ? mix_color(colour, LCD_BLACK, 35)
                                : colour);
        lcd_alpha_bitmap_part(mask, 0, 0, w, x, y + row, w, 1);
    }

    display->set_foreground(LCD_WHITE);
    if (pf)
    {
        /* the capitals' middle in the box's, as the digits of a year have it
           too, rather than that of the glyphs' line, with room for accents */
        static int ink_font = -1, ink_middle;
        if (ink_font != font)
        {
            int top, bottom;
            ink_middle = glyph_ink(font, 'H', &top, &bottom) ?
                         (top + bottom + 1) / 2 : (int)pf->height / 2;
            ink_font = font;
        }
        int gx = cx - width / 2;
        const int gy = cy - ink_middle;
        font_lock(font, true);
        for (int i = 0; i < n; i++)
        {
            const int gw = font_get_width(pf, ucs[i]);
            const unsigned char *bits = font_get_bits(pf, ucs[i]);
            if (pf->depth)
                lcd_alpha_bitmap_part(bits, 0, 0, gw, gx, gy, gw, pf->height);
            else
                display->mono_bitmap_part(bits, 0, 0, gw, gx, gy, gw, pf->height);
            gx += gw;
        }
        font_lock(font, false);
    }
    else
        display->putsxy(cx - width / 2, cy - font_get(text_font)->height / 2,
                        text);
    display->set_foreground(fg);
    display->set_drawmode(DRMODE_SOLID);
#else
    (void)display; (void)cx; (void)cy; (void)text; (void)font; (void)text_font;
#endif
}

/* the width of the strip of a speaker, a value, a chevron, those the line has
   at its right end, which the text stops short of: 0 where they're drawn
   otherwise */
static int decoration_width(struct screen *display, const struct line_desc *line)
{
    if (display->screen_type != SCREEN_MAIN || lang_is_rtl())
        return 0;
    int width = has_value(display, line) ? line->value_width + CHEVRON_MARGIN : 0;
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (line->style & STYLE_PLAYING)
        width += SPEAKER_W + CHEVRON_MARGIN;
    if (line->style & STYLE_CHEVRON)
        width += CHEVRON_W + CHEVRON_MARGIN;
#endif
    return width ? width + CHEVRON_MARGIN : 0;
}

void vput_line(struct screen *display,
              int x, int y, struct line_desc *line,
              const char *fmt, va_list ap)
{
#if (LCD_DEPTH > 1 || (defined(LCD_REMOTE_DEPTH) && LCD_REMOTE_DEPTH > 1))
    /* push and pop fg and bg colors as to not compromise unrelated lines */
    unsigned fg = 0, bg = 0; /* shut up gcc */
    if (display->depth > 1 && line->style > STYLE_INVERT)
    {
        fg = display->get_foreground();
        bg = display->get_background();
    }
#endif
    style_line(display, x, y, line);
    print_line(display, x, y, line, fmt, ap);
    /* the speaker first: its strip goes to the right end */
    if (line->style & STYLE_PLAYING)
        draw_speaker(display, y, line);
    draw_value(display, y, line);
    if (line->style & STYLE_CHEVRON)
        draw_chevron(display, y, line);
#if (LCD_DEPTH > 1 || (defined(LCD_REMOTE_DEPTH) && LCD_REMOTE_DEPTH > 1))
    if (display->depth > 1 && line->style > STYLE_INVERT)
    {
         display->set_drawinfo(DRMODE_SOLID, fg, bg);
    }
    else
#endif
    display->set_drawmode(DRMODE_SOLID);
}

void put_line(struct screen *display,
              int x, int y, struct line_desc *line,
              const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vput_line(display, x, y, line, fmt, ap);
    va_end(ap);
}
