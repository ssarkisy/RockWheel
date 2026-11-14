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
    /* the list's scrollbar that is a line, which the row is under */
    struct line_bar bar;
    bool has_bar;
    bool used;
};

static struct line_bar list_bar;
static bool list_bar_set;

void line_set_bar(const struct line_bar *bar)
{
    list_bar_set = bar != NULL;
    if (bar)
        list_bar = *bar;
}

void line_draw_bar(struct screen *display, const struct line_bar *bar,
                   int y, int height)
{
#ifdef HAVE_LCD_COLOR
    const struct viewport *vp = *display->current_viewport;
    const int x = bar->x - vp->x;
    const int last = y + height - 1;
    const unsigned fg = display->get_foreground();
    display->set_drawmode(DRMODE_SOLID);
    if (MAX(y, bar->top) <= MIN(last, bar->bottom))
    {
        display->set_foreground(ui_colour(UI_COLOUR_SCROLLBAR_LINE));
        display->vline(x, MAX(y, bar->top) - vp->y, MIN(last, bar->bottom) - vp->y);
    }
    if (MAX(y, bar->thumb_top) <= MIN(last, bar->thumb_bottom))
    {
        display->set_foreground(ui_colour(UI_COLOUR_SCROLLBAR_THUMB));
        display->vline(x, MAX(y, bar->thumb_top) - vp->y,
                       MIN(last, bar->thumb_bottom) - vp->y);
    }
    /* over the selected row: its bar again in the track's place, and the
       thumb in its text's colour */
    const int sel_top = MAX(MAX(y, bar->top), bar->sel_top);
    const int sel_bottom = MIN(MIN(last, bar->bottom), bar->sel_bottom);
    if (sel_top <= sel_bottom)
    {
        display->set_foreground(bar->sel_ground);
        display->vline(x, sel_top - vp->y, sel_bottom - vp->y);
        if (MAX(sel_top, bar->thumb_top) <= MIN(sel_bottom, bar->thumb_bottom))
        {
            display->set_foreground(bar->sel_ink);
            display->vline(x, MAX(sel_top, bar->thumb_top) - vp->y,
                           MIN(sel_bottom, bar->thumb_bottom) - vp->y);
        }
    }
    display->set_foreground(fg);
#else
    (void)display; (void)bar; (void)y; (void)height;
#endif
}

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
        if (line->has_bar)
            line_draw_bar(display, &line->bar, line->row_vp->y,
                          line->row_vp->height);
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
        if (line->has_bar)
            line_draw_bar(display, &line->bar, vp->y + y, line->desc.height);
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


/* text in width, ending in "…" if it is wider: its buffer in a frame of
   its own, which only the UI's draws have */
static NO_INLINE void put_fitted(struct screen *display, int x, int y,
                                 const char *text, int font, int width)
{
    char fitted[MAX_PATH];
    /* cut short in the form of the whole, as when it isn't */
    font_set_text_form(font_text_form((const unsigned char *)text, -1));
    const char *shown = line_fit(text, font, width, fitted, sizeof(fitted));
    display->putsxy_scroll_func(x, y, shown ? shown : "", NULL, NULL, 0);
    font_set_text_form(-1);
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
        line_data->has_bar = list_bar_set && display->screen_type == SCREEN_MAIN;
        if (line_data->has_bar)
            line_data->bar = list_bar;
        scrolls = display->putsxy_scroll_func(x, y, text,
            scrollers[display->screen_type], line_data, text_skip_pixels);
        line_data->used = scrolls;
    }
    /* A row's name too long for its place ends in "…" in a theme with a
       place at every row's right end (UI_METRIC_ROW_RIGHT): cut bare before
       a chevron it read as a fault. Not the scroller's draws of the row
       that scrolls (prevent_scroll), which show the text's parts as they
       are, on a thread with a small stack */
    else if (!prevent_scroll && display->screen_type == SCREEN_MAIN
             && ui_metric(UI_METRIC_ROW_RIGHT) && line->height > 0
             && !VP_IS_RTL(vp) && text_skip_pixels == 0 && vp->width > x)
        put_fitted(display, x, y, text, vp->font, vp->width - x);
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

/* The selection as a rounded rectangle in colour, on the ground: the rows
   top to top + height of one row_h high, from clip_x to the right, the rest
   of it left alone. Its corners' pixels are the colour mixed with the ground
   by how much of each is inside the rounding, set rather than blended, so
   that drawing a strip of it again gives the same. */
static void selection_rounded(struct screen *display, int clip_x, int y,
                              int top, int height, int row_h, int inset_y,
                              unsigned colour)
{
    const int r = ui_metric(UI_METRIC_SELECTION_RADIUS);
    const int x0 = ui_metric(UI_METRIC_SELECTION_INSET_X);
    const int x1 = display->getwidth() - x0; /* past its last column */
    const int h = row_h - 2 * inset_y;
    const unsigned ground = display->get_background();
    const unsigned fg = display->get_foreground();
    display->set_drawmode(DRMODE_SOLID);
    for (int row = MAX(top, inset_y); row < MIN(top + height, row_h - inset_y); row++)
    {
        const int edge = MIN(row - inset_y, h - 1 - (row - inset_y));
        const int yy = y + row - top;
        int in = 0; /* the columns of a corner in this row */
        if (edge < r)
        {
            in = r;
            for (int i = 0; i < r; i++)
            {
                /* by 4x4 points of the pixel, in eighths */
                int cover = 0;
                for (int k = 0; k < 16; k++)
                {
                    const int dx = 8 * r - (8 * i + 2 * (k & 3) + 1);
                    const int dy = 8 * r - (8 * edge + 2 * (k >> 2) + 1);
                    cover += dx * dx + dy * dy <= 64 * r * r;
                }
                if (!cover)
                    continue;
                display->set_foreground(mix_color(ground, colour, cover * 100 / 16));
                if (x0 + i >= clip_x)
                    display->drawpixel(x0 + i, yy);
                if (x1 - 1 - i >= clip_x)
                    display->drawpixel(x1 - 1 - i, yy);
            }
        }
        display->set_foreground(colour);
        if (MAX(x0 + in, clip_x) <= x1 - 1 - in)
            display->hline(MAX(x0 + in, clip_x), x1 - 1 - in, yy);
    }
    display->set_foreground(fg);
}

void line_fill_rounded(struct screen *display, int x, int y, int w, int h,
                       int r, unsigned colour)
{
    const unsigned ground = display->get_background();
    const unsigned fg = display->get_foreground();
    display->set_drawmode(DRMODE_SOLID);
    r = MIN(r, MIN(w, h) / 2);
    for (int row = 0; row < h; row++)
    {
        const int edge = MIN(row, h - 1 - row);
        int in = 0;
        if (edge < r)
        {
            in = r;
            for (int i = 0; i < r; i++)
            {
                int cover = 0;
                for (int k = 0; k < 16; k++)
                {
                    const int dx = 8 * r - (8 * i + 2 * (k & 3) + 1);
                    const int dy = 8 * r - (8 * edge + 2 * (k >> 2) + 1);
                    cover += dx * dx + dy * dy <= 64 * r * r;
                }
                if (!cover)
                    continue;
                display->set_foreground(mix_color(ground, colour, cover * 100 / 16));
                display->drawpixel(x + i, y + row);
                display->drawpixel(x + w - 1 - i, y + row);
            }
        }
        display->set_foreground(colour);
        if (x + in <= x + w - 1 - in)
            display->hline(x + in, x + w - 1 - in, y + row);
    }
    display->set_foreground(fg);
}

unsigned line_selection_bar(struct screen *display, int x, int y, int width,
                            int height)
{
#ifdef HAVE_LCD_COLOR
    if (display->depth >= 16)
    {
        struct line_desc line = LINE_DESC_DEFINIT;
        line.height = height;
        line.line_color = global_settings.lss_color;
        line.line_end_color = line.line_color;
        if (global_settings.cursor_style == SYNCLIST_CURSOR_GRADIENT)
            line.line_end_color = global_settings.lse_color;
        display->set_drawmode(DRMODE_FG);
        display->gradient_fillrect(x, y, width, height, line.line_color,
                                   line.line_end_color);
        if (global_settings.list_selector_bevel)
            draw_bevel(display, x, y, width, height, &line);
        return global_settings.lst_color;
    }
#endif
    display->set_drawmode(DRMODE_SOLID);
    display->fillrect(x, y, width, height);
    return display->get_background();
}

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
            if (ui_metric(UI_METRIC_SELECTION_RADIUS) > 0
                && display->screen_type == SCREEN_MAIN)
            {
                /* the ground under it first, as the default style has it,
                   but the gutter right of it, where the scrollbar is */
                display->set_drawmode(DRMODE_BG | DRMODE_INVERSEVID);
                display->fillrect(x, y, width - x
                                  - ui_metric(UI_METRIC_SELECTION_INSET_X), bar_height);
                /* an item of several lines is one rectangle over them all */
                selection_rounded(display, x, y,
                                  row * line->line + line->row_top, bar_height,
                                  row * line->nlines,
                                  line->row_height > 0 ? 0
                                  : ui_metric(UI_METRIC_SELECTION_INSET_Y),
                                  line->line_color);
                break;
            }
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
#define CHEVRON_MARGIN ui_metric(UI_METRIC_CHEVRON_MARGIN)
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
/* drawn like anti-aliased glyphs: 9x11, lines 2.6 pixels thick, in Rockbox'
   alpha format (4 bits from 0 opaque to 15 transparent, low nibble first,
   rows run on without padding) */
static const unsigned char chevron_alpha[] = {
    0x3a, 0xf7, 0xff, 0xff, 0x5f, 0x00, 0xd3, 0xff, 0xff, 0x2d, 0x00, 0x90,
    0xff, 0xff, 0x5e, 0x00, 0x50, 0xfe, 0xff, 0xaf, 0x01, 0x20, 0xfd, 0xff,
    0xbf, 0x00, 0x70, 0xff, 0xaf, 0x01, 0x20, 0xfd, 0x5e, 0x00, 0x50, 0xfe,
    0x2d, 0x00, 0x90, 0xff, 0x5f, 0x00, 0xd3, 0xff, 0xff, 0x3a, 0xf7, 0xff,
    0xff, 0xff,
};
/* the speaker of the song playing, in the same format: made by
   themes/tools/make_list_glyphs.py */
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
/* the marks of the catalogue's playlists and folders, by the same tool */
static const unsigned char computer_alpha[] = {
    0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x00, 0xf0, 0xff, 0xff, 0xff,
    0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x00, 0xf0, 0xff, 0xff,
    0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf7, 0xff,
    0xff, 0x00, 0xf0, 0xff, 0xff, 0xff, 0x2f, 0x00, 0x00, 0x20, 0xff, 0xff,
    0xff, 0x02, 0x00, 0x00, 0xf2, 0xff,
};
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
/* the marks of a podcast's episodes, new and begun, by the same tool */
static const unsigned char dot_alpha[] = {
    0xaf, 0x02, 0xa2, 0xaf, 0x00, 0x00, 0xa0, 0x02, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x00, 0xa2, 0x00, 0x00, 0xa0, 0xaf, 0x02, 0xa2,
    0xff,
};
static const unsigned char half_dot_alpha[] = {
    0xaf, 0x02, 0xa2, 0xaf, 0x00, 0x76, 0xa0, 0x02, 0x70, 0x7f, 0x02, 0x00,
    0xf7, 0x0b, 0x02, 0x70, 0x7f, 0xa2, 0x00, 0x76, 0xa0, 0xaf, 0x02, 0xa2,
    0xff,
};
extern void lcd_alpha_bitmap_part(const unsigned char *src, int src_x,
                                  int src_y, int stride, int x, int y,
                                  int width, int height);
/* The marks by theme: Classic's above, Neo's and Modern's from their mockups' export */
#include "neo_glyphs.h"
#include "modern_glyphs.h"
struct glyph {
    const unsigned char *alpha;
    unsigned char w, h, frame; /* its frame of those one under another */
};
enum { GLYPH_CHEVRON, GLYPH_SPEAKER, GLYPH_COMPUTER, GLYPH_IPOD,
       GLYPH_SMALL_NOTE, GLYPH_DOT, GLYPH_HALF_DOT, GLYPH_COUNT };
static const struct glyph glyphs[UI_THEME_COUNT][GLYPH_COUNT] = {
    [UI_THEME_CLASSIC] = {
        [GLYPH_CHEVRON]    = { chevron_alpha, 9, 11, 0 },
        [GLYPH_SPEAKER]    = { speaker_alpha, 15, 12, 0 },
        [GLYPH_COMPUTER]   = { computer_alpha, 13, 12, 0 },
        [GLYPH_IPOD]       = { ipod_alpha, 10, 13, 0 },
        [GLYPH_SMALL_NOTE] = { small_note_alpha, 16, 19, 0 },
        [GLYPH_DOT]        = { dot_alpha, 7, 7, 0 },
        [GLYPH_HALF_DOT]   = { half_dot_alpha, 7, 7, 0 },
    },
    [UI_THEME_MODERN] = {
        [GLYPH_CHEVRON]    = { modern_chevron_alpha, MODERN_CHEVRON_W, MODERN_CHEVRON_H, 0 },
        [GLYPH_SPEAKER]    = { modern_speaker_alpha, MODERN_SPEAKER_W, MODERN_SPEAKER_H, 0 },
        [GLYPH_COMPUTER]   = { modern_source_alpha, MODERN_SOURCE_W, MODERN_SOURCE_H, 0 },
        [GLYPH_IPOD]       = { modern_source_alpha, MODERN_SOURCE_W, MODERN_SOURCE_H, 1 },
        [GLYPH_SMALL_NOTE] = { modern_note_small_alpha, MODERN_NOTE_SMALL_W, MODERN_NOTE_SMALL_H, 0 },
        [GLYPH_DOT]        = { modern_episode_dots_alpha, MODERN_EPISODE_DOTS_W, MODERN_EPISODE_DOTS_H, 0 },
        [GLYPH_HALF_DOT]   = { modern_episode_dots_alpha, MODERN_EPISODE_DOTS_W, MODERN_EPISODE_DOTS_H, 1 },
    },
    [UI_THEME_NEO] = {
        [GLYPH_CHEVRON]    = { neo_chevron_alpha, NEO_CHEVRON_W, NEO_CHEVRON_H, 0 },
        [GLYPH_SPEAKER]    = { neo_speaker_alpha, NEO_SPEAKER_W, NEO_SPEAKER_H, 0 },
        [GLYPH_COMPUTER]   = { neo_source_alpha, NEO_SOURCE_W, NEO_SOURCE_H, 0 },
        [GLYPH_IPOD]       = { neo_source_alpha, NEO_SOURCE_W, NEO_SOURCE_H, 1 },
        [GLYPH_SMALL_NOTE] = { neo_note_small_alpha, NEO_NOTE_SMALL_W, NEO_NOTE_SMALL_H, 0 },
        [GLYPH_DOT]        = { neo_episode_dots_alpha, NEO_EPISODE_DOTS_W, NEO_EPISODE_DOTS_H, 0 },
        [GLYPH_HALF_DOT]   = { neo_episode_dots_alpha, NEO_EPISODE_DOTS_W, NEO_EPISODE_DOTS_H, 1 },
    },
};

static const struct glyph *glyph(int which)
{
    const int theme = global_settings.ui_theme;
    return &glyphs[theme >= 0 && theme < UI_THEME_COUNT ? theme : UI_THEME_CLASSIC][which];
}

#define CHEVRON_W    (glyph(GLYPH_CHEVRON)->w)
#define CHEVRON_H    (glyph(GLYPH_CHEVRON)->h)
#define SPEAKER_W    (glyph(GLYPH_SPEAKER)->w)
#define SPEAKER_H    (glyph(GLYPH_SPEAKER)->h)
#define SMALL_NOTE_W (glyph(GLYPH_SMALL_NOTE)->w)
#define SMALL_NOTE_H (glyph(GLYPH_SMALL_NOTE)->h)
#define DOT_W        (glyph(GLYPH_DOT)->w)
#define DOT_H        (glyph(GLYPH_DOT)->h)

static void draw_glyph(int which, int x, int y)
{
    const struct glyph *const g = glyph(which);
    lcd_alpha_bitmap_part(g->alpha, 0, g->frame * g->h, g->w, x, y, g->w, g->h);
}

/* a decoration's colour on the selected row, and on another: the selection's
   text and the colour given, or the theme's own quiet ones where it lays the
   row's end out itself */
static unsigned on_selection(const struct line_desc *line)
{
    /* a row a list colours without selecting it: its own colour, not the
       selected row's */
    if (!(line->style & (STYLE_INVERT|STYLE_COLORBAR|STYLE_GRADIENT)))
        return (unsigned)line->text_color;
    return ui_metric(UI_METRIC_ROW_RIGHT) ? ui_colour(UI_COLOUR_VALUE_SELECTED)
                                          : (unsigned)line->text_color;
}

static unsigned off_selection(unsigned colour)
{
    return ui_metric(UI_METRIC_ROW_RIGHT) ? ui_colour(UI_COLOUR_QUIET) : colour;
}

/* whether the line is of one of these styles in the colours of its selection
   or its list for what is on it besides its text: not one whose selection is
   of its background and its text alone (STYLE_NAME_ONLY) */
static bool coloured(const struct line_desc *line, int styles)
{
    return (line->style & styles) && !(line->style & STYLE_NAME_ONLY);
}
#endif

/* how much nearer the row's end than its margin a theme has the chevron,
   and with it the value beside it and where a name stops before them */
static int chevron_shift(void)
{
    const int inset = ui_metric(UI_METRIC_CHEVRON_INSET);
    return inset ? CHEVRON_MARGIN - inset : 0;
}

static void draw_chevron(struct screen *display, int y, struct line_desc *line)
{
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (display->screen_type == SCREEN_MAIN)
    {
        int x = display->getwidth() - CHEVRON_W - CHEVRON_MARGIN + chevron_shift();
        const bool selected = line->style & (STYLE_INVERT|STYLE_COLORBAR|STYLE_GRADIENT);
        const unsigned fg = display->get_foreground();
        /* on a row not selected, which only the theme with one on every
           row has, the list's ground is there already */
        if (selected || !ui_metric(UI_METRIC_CHEVRON_ALL))
            style_strip(display, x - (ui_metric(UI_METRIC_ROW_RIGHT) ?
                                      ui_metric(UI_METRIC_ROW_RIGHT_GAP) - 2
                                      : CHEVRON_MARGIN), y, line);
        if (ui_metric(UI_METRIC_ROW_RIGHT))
            display->set_foreground(coloured(line, STYLE_INVERT|STYLE_COLORBAR|STYLE_GRADIENT) ?
                                    on_selection(line) : ui_colour(UI_COLOUR_QUIET));
        display->set_drawmode(DRMODE_FG);
        draw_glyph(GLYPH_CHEVRON, x, line_middle(display, y, line, CHEVRON_H));
        display->set_foreground(fg);
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

#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
/* Where the theme lays the row's end out itself: the column a value ends
   before, right of the chevron's place if the row has one; or, past the
   value too, the one the speaker ends before */
static int row_right(struct screen *display, const struct line_desc *line,
                     bool past_value)
{
    const int gap = ui_metric(UI_METRIC_ROW_RIGHT_GAP);
    int right = display->getwidth() - ui_metric(UI_METRIC_ROW_RIGHT);
    if (line->style & STYLE_CHEVRON)
        right -= gap - chevron_shift();
    if (past_value && has_value(display, line))
        right -= line->value_width + gap;
    return right;
}
#endif

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
        if (ui_metric(UI_METRIC_ROW_RIGHT))
            x = row_right(display, line, true) - SPEAKER_W;
        style_strip(display, x - CHEVRON_MARGIN, y, line);
    }
    const bool selected = coloured(line, STYLE_INVERT|STYLE_COLORBAR|STYLE_GRADIENT);
    const unsigned fg = display->get_foreground();
    if (ui_metric(UI_METRIC_ACCENT_MARKS))
        display->set_foreground(selected && ui_metric(UI_METRIC_SELECTION_INK) ?
                                on_selection(line) : ui_colour(UI_COLOUR_ACCENT));
    else if (ui_metric(UI_METRIC_ROW_RIGHT))
        display->set_foreground(selected ? on_selection(line) : LIST_VALUE_COLOUR);
    else if (!selected)
        display->set_foreground(speaker_colour());
    display->set_drawmode(DRMODE_FG);
    draw_glyph(GLYPH_SPEAKER, x, line_middle(display, y, line, SPEAKER_H));
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
    if (ui_metric(UI_METRIC_ROW_RIGHT))
        right = row_right(display, line, false);
#endif
    const int height = line->height == -1 ? display->getcharheight() : line->height;
    struct viewport *vp = *display->current_viewport;
    const int name_font = vp->font;
    /* in line with the name, in a row of two parts too */
    /* but not with the theme's shift of a name in a row of one line */
    y += height/2 - display->getcharheight()/2 + line->text_dy + line->value_shift
         - (line->row_height <= 0 ? ui_metric(UI_METRIC_ROW_TEXT_DY) : 0);
    int width;
    font_getstringsize(line->value, &width, NULL, line->value_font);
    const unsigned fg = display->get_foreground();
#ifdef HAVE_LCD_COLOR
    if (coloured(line, STYLE_COLORED|STYLE_COLORBAR|STYLE_GRADIENT))
        display->set_foreground(on_selection(line));
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

#define MARK_COLUMN 13 /* the widest of them */
/* and the names close after it; 22 px from the mark's start to the name's
   where the theme has marks of its own */
#define MARK_GAP    (ui_metric(UI_METRIC_ACCENT_MARKS) ? 9 : 4)
int line_mark_width(struct screen *display)
{
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (display->screen_type == SCREEN_MAIN && !lang_is_rtl())
        return MARK_COLUMN + MARK_GAP;
#endif
    (void)display;
    return 0;
}

#define STATE_GAP 5 /* from an episode's mark to its name */
int line_mark_column(struct screen *display, int mark)
{
    if (mark < LINE_MARK_NEW)
        return line_mark_width(display);
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (display->screen_type == SCREEN_MAIN)
        return DOT_W + STATE_GAP;
#endif
    return 0;
}

#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
/* An episode's mark at the start of its column, the names' end of it right
   to left: in the speaker's colour, as the selection's text on it; in the
   middle of the name's capitals */
static void draw_state(struct screen *display, int x, int y,
                       const struct line_desc *line, int mark)
{
    if (mark == LINE_MARK_EMPTY || !line_mark_column(display, mark))
        return;
    const int height = line->height == -1 ? display->getcharheight() : line->height;
    const unsigned fg = display->get_foreground();
    if (coloured(line, STYLE_COLORBAR|STYLE_GRADIENT))
        display->set_foreground(on_selection(line));
    else if (line->style & STYLE_INVERT)
        display->set_foreground(display->get_background());
    else
        display->set_foreground(off_selection(speaker_colour()));
    display->set_drawmode(DRMODE_FG);
    draw_glyph(mark == LINE_MARK_NEW ? GLYPH_DOT : GLYPH_HALF_DOT,
               x + (lang_is_rtl() ? STATE_GAP : 0),
               y + (height - DOT_H) / 2 + line->text_dy);
    display->set_foreground(fg);
    display->set_drawmode(DRMODE_SOLID);
}
#endif

/* in the middle of their column, in the colour of the names */
void line_draw_mark(struct screen *display, int x, int y,
                    const struct line_desc *line, int mark)
{
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (mark >= LINE_MARK_NEW)
    {
        draw_state(display, x, y, line, mark);
        return;
    }
    if (!line_mark_width(display))
        return;
    const int height = line->height == -1 ? display->getcharheight() : line->height;
    const unsigned fg = display->get_foreground();
    if (mark >= LINE_MARK_PLUS)
    {
        /* the theme's own, in its accent: Modern alone has them */
        const bool plus = mark == LINE_MARK_PLUS;
        const int w = plus ? MODERN_PLUS_W : MODERN_MUSIC_MARKS_W;
        const int h = plus ? MODERN_PLUS_H : MODERN_MUSIC_MARKS_H;
        if (mark == LINE_MARK_BLANK)
            return;
        display->set_foreground(ui_metric(UI_METRIC_SELECTION_INK)
                                && coloured(line, STYLE_COLORBAR|STYLE_GRADIENT) ?
                                on_selection(line) : ui_colour(UI_COLOUR_ACCENT));
        display->set_drawmode(DRMODE_FG);
        lcd_alpha_bitmap_part(plus ? modern_plus_alpha : modern_music_marks_alpha,
                              0, plus ? 0 : (mark - LINE_MARK_MENU) * h, w,
                              x + (MARK_COLUMN - w - (w > MARK_COLUMN)) / 2,
                              y + (height - h) / 2, w, h);
        display->set_foreground(fg);
        display->set_drawmode(DRMODE_SOLID);
        return;
    }
    int which;
    switch (mark)
    {
        case LINE_MARK_COMPUTER:
            which = GLYPH_COMPUTER;
            break;
        case LINE_MARK_IPOD:
            which = GLYPH_IPOD;
            break;
        default:
            return;
    }
    const int w = glyph(which)->w, h = glyph(which)->h;
    if (coloured(line, STYLE_COLORED|STYLE_COLORBAR|STYLE_GRADIENT))
        display->set_foreground(on_selection(line));
    else if (line->style & STYLE_INVERT) /* as the text on the bar */
        display->set_foreground(display->get_background());
    else
        display->set_foreground(off_selection(fg));
    display->set_drawmode(DRMODE_FG);
    draw_glyph(which, x + (MARK_COLUMN - w) / 2, y + (height - h) / 2);
    display->set_foreground(fg);
    display->set_drawmode(DRMODE_SOLID);
#else
    (void)display; (void)x; (void)y; (void)line; (void)mark;
#endif
}

#define LEAD_GAP ui_metric(UI_METRIC_LEAD_GAP) /* as a setting's value from its name */
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
    /* at least the theme's column, in which a short one is centred */
    return MAX(digits * widest + LEAD_GAP, ui_metric(UI_METRIC_LEAD_COLUMN));
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

int line_ink_left(int font, const char *text)
{
    struct font *pf = font_get(font);
    ucschar_t ch;
    if (!text || !*text || !pf->depth)
        return 0;
    utf8decode((const unsigned char *)text, &ch);
    font_lock(font, true);
    const int width = font_get_width(pf, ch);
    const unsigned char *bits = font_get_bits(pf, ch);
    int left = 0;
    /* a letter the font hasn't is another font's glyph, of another height
       and depth: not read as this one's */
    if (ch != (ucschar_t)pf->defaultchar
        && bits == font_get_bits(pf, pf->defaultchar))
        bits = NULL;
    for (int x = 0; bits && x < width; x++)
    {
        bool ink = false;
        for (int row = 0; row < (int)pf->height && !ink; row++)
        {
            const int p = row * width + x;
            ink = ((bits[p >> 1] >> ((p & 1) * 4)) & 0xf) < 8;
        }
        if (ink)
        {
            left = x;
            break;
        }
    }
    font_lock(font, false);
    return left;
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
   side too: a number by its own width, so that its last digit ends the
   column in a font whose digits differ in width as in one whose digits are
   all as wide. The dash of an album without a year is not measured: it
   stands where a text of as many digits as it has bytes would start */
void line_draw_lead(struct screen *display, int font, int shift, int x, int y,
                    const struct line_desc *line, const char *text,
                    int width, int digit)
{
    int lead_w = digit * (int)strlen(text);
    const char *p = text;
    while (*p >= '0' && *p <= '9')
        p++;
    if (!*p)
        font_getstringsize(text, &lead_w, NULL, font);
    if (width == ui_metric(UI_METRIC_LEAD_COLUMN) && lead_w + LEAD_GAP <= width)
        x += (width - LEAD_GAP - lead_w) / 2; /* in the middle of its cell */
    else
        x += width - LEAD_GAP - lead_w;
    const int height = line->height == -1 ? display->getcharheight() : line->height;
    struct viewport *vp = *display->current_viewport;
    const int name_font = vp->font;
    y += height/2 - display->getcharheight()/2 + shift;
    const unsigned fg = display->get_foreground();
#ifdef HAVE_LCD_COLOR
    if (coloured(line, STYLE_COLORED|STYLE_COLORBAR|STYLE_GRADIENT))
        display->set_foreground(on_selection(line));
    else if (line->style & STYLE_INVERT) /* as the text on the bar */
        display->set_foreground(display->get_background());
    else
        display->set_foreground(ui_colour(UI_COLOUR_LEAD));
#endif
    display->set_drawmode(DRMODE_FG);
    vp->font = font;
    display->putsxy(x, y, text);
    vp->font = name_font;
    display->set_foreground(fg);
    display->set_drawmode(DRMODE_SOLID);
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
    char buf[MAX_PATH], accent[48];
    const int width = vp->width - x - decoration_width(display, line);
    /* cut short in the form of the whole, as when it isn't */
    font_set_text_form(font_text_form((const unsigned char *)text, -1));
    /* the accented start is a run of its own, placed as in the whole */
    const char *end = strchr(text, SUBLINE_ACCENT_END), *first = NULL;
    int first_w = 0, text_w = 0;
    if (end)
    {
        size_t n = MIN((size_t)(end - text), sizeof(accent) - 1);
        while (n > 0 && (text[n] & 0xc0) == 0x80) /* whole letters */
            n--;
        memcpy(accent, text, n);
        accent[n] = '\0';
        first = line_fit(accent, font, width, buf, sizeof(buf));
        text = first == accent ? end + 1 : NULL; /* nothing after a cut one */
        if (first)
            font_getstringsize(first, &first_w, NULL, font);
    }
    if (text)
        text = line_fit(text, font, width - first_w, buf, sizeof(buf));
    if (!text && !first)
    {
        font_set_text_form(-1);
        return;
    }
    if (text && first && VP_IS_RTL(vp))
        font_getstringsize(text, &text_w, NULL, font);
    const int name_font = vp->font;
    const unsigned fg = display->get_foreground();
#ifdef HAVE_LCD_COLOR
    unsigned colour = LIST_VALUE_COLOUR, accent_colour = LIST_VALUE_COLOUR;
#ifndef DISABLE_ALPHA_BITMAP
    accent_colour = speaker_colour();
#endif
    if (coloured(line, STYLE_COLORED|STYLE_COLORBAR|STYLE_GRADIENT))
    {
        colour = on_selection(line);
        accent_colour = line->text_color;
    }
    else if (line->style & STYLE_INVERT) /* as the text on the bar */
        colour = accent_colour = display->get_background();
    else if (ui_metric(UI_METRIC_ROW_RIGHT))
        /* the row's news, as its name; in a theme with a tone a tint of
           the user's colour, as the small text beside it is */
        accent_colour = ui_theme_current()->cover_tone ? ui_colour(UI_COLOUR_ACCENT)
                                                       : (unsigned)global_settings.fg_color;
    /* the playing song's, in the theme's accent where it isn't selected */
    if (ui_metric(UI_METRIC_PLAYING_SUBLINE) && (line->style & STYLE_PLAYING)
        && !coloured(line, STYLE_COLORBAR|STYLE_GRADIENT|STYLE_INVERT))
        colour = ui_colour(UI_COLOUR_ACCENT);
#endif
    display->set_drawmode(DRMODE_FG);
    vp->font = font;
    if (first)
    {
#ifdef HAVE_LCD_COLOR
        display->set_foreground(accent_colour);
#endif
        display->putsxy(x + text_w, y + top_dy, first);
    }
    if (text)
    {
#ifdef HAVE_LCD_COLOR
        display->set_foreground(colour);
#endif
        display->putsxy(x + (text_w ? 0 : first_w), y + top_dy, text);
    }
    font_set_text_form(-1);
    vp->font = name_font;
    display->set_foreground(fg);
    display->set_drawmode(DRMODE_SOLID);
}

/* the top corners, and the bottom ones too unless top_only */
static void round_corners(struct screen *display, int x, int y, int w, int h, int r,
                          bool top_only)
{
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (r <= 0 || r > 16 || display->screen_type != SCREEN_MAIN)
        return;
    const unsigned fg = display->get_foreground();
    display->set_foreground(display->get_background());
    display->set_drawmode(DRMODE_FG);
    for (int row = 0; row < r; row++)
    {
        /* the ground over each pixel by how much of it is outside the
           rounding: the left corner's row, and the right one's mirrored */
        unsigned char left[8] = { 0 }, right[8] = { 0 };
        for (int i = 0; i < r; i++)
        {
            int cover = 0;
            for (int k = 0; k < 16; k++)
            {
                const int dx = 8 * r - (8 * i + 2 * (k & 3) + 1);
                const int dy = 8 * r - (8 * row + 2 * (k >> 2) + 1);
                cover += dx * dx + dy * dy <= 64 * r * r;
            }
            const unsigned a = cover > 8 ? cover - 1 : cover; /* 15 is none */
            left[i / 2] |= a << (i & 1 ? 4 : 0);
            right[(r - 1 - i) / 2] |= a << ((r - 1 - i) & 1 ? 4 : 0);
        }
        lcd_alpha_bitmap_part(left, 0, 0, r, x, y + row, r, 1);
        lcd_alpha_bitmap_part(right, 0, 0, r, x + w - r, y + row, r, 1);
        if (top_only)
            continue;
        lcd_alpha_bitmap_part(left, 0, 0, r, x, y + h - 1 - row, r, 1);
        lcd_alpha_bitmap_part(right, 0, 0, r, x + w - r, y + h - 1 - row, r, 1);
    }
    display->set_foreground(fg);
    display->set_drawmode(DRMODE_SOLID);
#else
    (void)display; (void)x; (void)y; (void)w; (void)h; (void)r; (void)top_only;
#endif
}

void line_round_corners(struct screen *display, int x, int y, int w, int h, int r)
{
    round_corners(display, x, y, w, h, r, false);
}

void line_round_top_corners(struct screen *display, int x, int y, int w, int r)
{
    round_corners(display, x, y, w, r, r, true);
}

void line_fill_rounded_over(struct screen *display, int x, int y, int w, int h,
                            int r, unsigned colour)
{
    const unsigned fg = display->get_foreground();
    display->set_foreground(colour);
    display->set_drawmode(DRMODE_SOLID);
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    r = MIN(r, MIN(w, h) / 2);
    if (r <= 0 || r > 16 || display->screen_type != SCREEN_MAIN)
#endif
    {
        display->fillrect(x, y, w, h);
        display->set_foreground(fg);
        return;
    }
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    display->fillrect(x, y + r, w, h - 2 * r);
    display->fillrect(x + r, y, w - 2 * r, r);
    display->fillrect(x + r, y + h - r, w - 2 * r, r);
    display->set_drawmode(DRMODE_FG);
    for (int row = 0; row < r; row++)
    {
        /* the colour over each pixel by how much of it is inside the
           rounding: the left corner's row, and the right one's mirrored */
        unsigned char left[8] = { 0 }, right[8] = { 0 };
        for (int i = 0; i < r; i++)
        {
            int cover = 0;
            for (int k = 0; k < 16; k++)
            {
                const int dx = 8 * r - (8 * i + 2 * (k & 3) + 1);
                const int dy = 8 * r - (8 * row + 2 * (k >> 2) + 1);
                cover += dx * dx + dy * dy <= 64 * r * r;
            }
            const unsigned a = 15 - cover * 15 / 16; /* 0 is all of it */
            left[i / 2] |= a << (i & 1 ? 4 : 0);
            right[(r - 1 - i) / 2] |= a << ((r - 1 - i) & 1 ? 4 : 0);
        }
        lcd_alpha_bitmap_part(left, 0, 0, r, x, y + row, r, 1);
        lcd_alpha_bitmap_part(right, 0, 0, r, x + w - r, y + row, r, 1);
        lcd_alpha_bitmap_part(left, 0, 0, r, x, y + h - 1 - row, r, 1);
        lcd_alpha_bitmap_part(right, 0, 0, r, x + w - r, y + h - 1 - row, r, 1);
    }
    display->set_foreground(fg);
    display->set_drawmode(DRMODE_SOLID);
#endif
}

void line_draw_note(const unsigned char *alpha, int w, int h, int x, int y)
{
    line_draw_note_rows(alpha, w, w, h, x, y);
}

void line_draw_note_rows(const unsigned char *alpha, int stride, int w, int h,
                         int x, int y)
{
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    const unsigned top = ui_colour(UI_COLOUR_NOTE_TOP);
    const unsigned foot = ui_colour(UI_COLOUR_NOTE_FOOT);
    lcd_set_drawmode(DRMODE_FG);
    if (top == foot || h < 2)
    {
        lcd_set_foreground(top);
        lcd_alpha_bitmap_part(alpha, 0, 0, stride, x, y, w, h);
        return;
    }
    for (int row = 0; row < h; row++)
    {
        lcd_set_foreground(mix_color(top, foot, row * 100 / (h - 1)));
        lcd_alpha_bitmap_part(alpha, 0, row, stride, x, y + row, w, 1);
    }
#else
    (void)alpha; (void)stride; (void)w; (void)h; (void)x; (void)y;
#endif
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
    display->set_foreground(ui_colour(UI_COLOUR_COVER_TILE));
    display->set_drawmode(DRMODE_SOLID);
    display->fillrect(x, y, LINE_COVER_SIZE, LINE_COVER_SIZE);
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (display->screen_type == SCREEN_MAIN)
    {
        const struct glyph *const g = glyph(GLYPH_SMALL_NOTE);
        line_draw_note(g->alpha, g->w, g->h, x + (LINE_COVER_SIZE - g->w) / 2,
                       y + (LINE_COVER_SIZE - g->h) / 2);
    }
#endif
    display->set_foreground(fg);
    display->set_drawmode(DRMODE_SOLID);
}

/* Quick Scroll's box: as tall as three rows of the menus or two of 36 px, as
   wide or as its text and a margin each side, its corners rounded */
#define INITIAL_HEIGHT ui_metric(UI_METRIC_INITIAL_HEIGHT)
#define INITIAL_MARGIN ui_metric(UI_METRIC_INITIAL_MARGIN)
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

    display->set_foreground(ui_colour(UI_COLOUR_INITIAL_TEXT));
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
    if (ui_metric(UI_METRIC_ROW_RIGHT))
    {
        /* from the leftmost of them, a gap before it, to the row's end */
        int left;
        if (line->style & STYLE_PLAYING)
            left = row_right(display, line, true) - SPEAKER_W;
        else if (has_value(display, line))
            left = row_right(display, line, false) - line->value_width;
        else if (line->style & STYLE_CHEVRON)
            return CHEVRON_W + CHEVRON_MARGIN + 4 - chevron_shift();
        else if (line->height <= 0) /* no list's row: a skin's line, a message's */
            return 0;
        else /* to two pixels inside the selection's end */
            return ui_metric(UI_METRIC_SELECTION_INSET_X) + 2;
        return display->getwidth() - left + ui_metric(UI_METRIC_ROW_RIGHT_GAP);
    }
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
