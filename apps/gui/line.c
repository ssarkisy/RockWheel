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
static int decoration_width(struct screen *display, const struct line_desc *line);

struct line_desc_scroll {
    struct line_desc desc;
    int rtl_cut; /* right to left, the viewport's end cut off for the text */
    bool used;
};

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
    else
    {
        int y = s->y - (line->desc.height/2 - display->getcharheight()/2);
        /* the text's bar again, short of what's before it right to left */
        struct viewport *vp = *display->current_viewport;
        vp->width -= line->rtl_cut;
        style_line(display, s->x, y, &line->desc);
        put_text(display, s->x, s->y, &line->desc, s->line, true, s->offset);
        vp->width += line->rtl_cut;
        if (line->desc.style & STYLE_PLAYING)
            draw_speaker(display, y, &line->desc);
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
        scrolls = display->putsxy_scroll_func(x, y, text,
            scrollers[display->screen_type], line_data, text_skip_pixels);
        line_data->used = scrolls;
    }
    else
        display->putsxy_scroll_func(x, y, text, NULL, NULL, text_skip_pixels);
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
    y += height/2 - display->getcharheight()/2;

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
    if (line->line == 0)
    {
        display->set_foreground(mix_color(line->line_color, LCD_WHITE, 45));
        display->hline(x, x + width - 1, y);
    }
    if (line->line == line->nlines - 1 && height > 3)
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

    if (line->separator_height > 0 && (line->line == line->nlines-1))
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
            display->set_drawmode(DRMODE_FG);
            display->gradient_fillrect_part(x, y, width, bar_height,
                                            line->line_color,
                                            line->line_end_color,
                                            height*line->nlines,
                                            height*line->line);
            if (global_settings.list_selector_bevel)
                draw_bevel(display, x, y, width, bar_height, line);
            break;
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
extern void lcd_alpha_bitmap_part(const unsigned char *src, int src_x,
                                  int src_y, int stride, int x, int y,
                                  int width, int height);
#endif

static void draw_chevron(struct screen *display, int y, struct line_desc *line)
{
    int height = line->height == -1 ? display->getcharheight() : line->height;
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (display->screen_type == SCREEN_MAIN)
    {
        int x = display->getwidth() - CHEVRON_W - CHEVRON_MARGIN;
        style_line(display, x - CHEVRON_MARGIN, y, line);
        display->set_drawmode(DRMODE_FG);
        lcd_alpha_bitmap_part(chevron_alpha, 0, 0, CHEVRON_W,
                              x, y + (height - CHEVRON_H)/2,
                              CHEVRON_W, CHEVRON_H);
        return;
    }
#endif
    /* the font's single angle quotation mark elsewhere */
    static const unsigned char chevron[] = "\xe2\x80\xba"; /* U+203A */
    int w, h;
    display->getstringsize(chevron, &w, &h);
    int x = display->getwidth() - w - CHEVRON_MARGIN;

    style_line(display, x - CHEVRON_MARGIN, y, line);
    display->set_drawmode((line->style & STYLE_INVERT) ?
                          DRMODE_SOLID|DRMODE_INVERSEVID : DRMODE_FG);
    display->putsxy(x, y + height/2 - display->getcharheight()/2, chevron);
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

/* The speaker of the song playing at the right end of its line, as on a stock
   iPod: in the blue of the selection's end on the list, and as the selection's
   text on it, on a strip of the line's own background. Left of a chevron if
   the line has both, and at the left end with right-to-left text. */
static void draw_speaker(struct screen *display, int y, struct line_desc *line)
{
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (display->screen_type != SCREEN_MAIN)
        return;
    const int height = line->height == -1 ? display->getcharheight() : line->height;
    int x;
    if (lang_is_rtl())
        x = CHEVRON_MARGIN; /* where the text ends, without a strip */
    else
    {
        x = display->getwidth() - SPEAKER_W - CHEVRON_MARGIN;
        if (line->style & STYLE_CHEVRON)
            x -= CHEVRON_W + CHEVRON_MARGIN;
        style_line(display, x - CHEVRON_MARGIN, y, line);
    }
    const bool selected = line->style & (STYLE_INVERT|STYLE_COLORBAR|STYLE_GRADIENT);
    const unsigned fg = display->get_foreground();
    if (!selected)
        display->set_foreground(speaker_colour());
    display->set_drawmode(DRMODE_FG);
    lcd_alpha_bitmap_part(speaker_alpha, 0, 0, SPEAKER_W,
                          x, y + (height - SPEAKER_H)/2, SPEAKER_W, SPEAKER_H);
    display->set_foreground(fg);
#else
    (void)display; (void)y; (void)line;
#endif
}

int line_shuffle_width(struct screen *display)
{
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (display->screen_type == SCREEN_MAIN && !lang_is_rtl())
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
   on the selection, which draws from where the text starts. */
void line_draw_shuffle(struct screen *display, int x, int y,
                       const struct line_desc *line)
{
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (!line_shuffle_width(display))
        return;
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

/* the width of the strip of a speaker, a chevron or both at the line's right
   end, which the text stops short of: 0 where they're drawn otherwise */
static int decoration_width(struct screen *display, const struct line_desc *line)
{
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (display->screen_type != SCREEN_MAIN || lang_is_rtl())
        return 0;
    int width = 0;
    if (line->style & STYLE_PLAYING)
        width += SPEAKER_W + CHEVRON_MARGIN;
    if (line->style & STYLE_CHEVRON)
        width += CHEVRON_W + CHEVRON_MARGIN;
    return width ? width + CHEVRON_MARGIN : 0;
#else
    (void)display; (void)line;
    return 0;
#endif
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
