/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2007 by Jonathan Gordon
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

/* This file contains the code to draw the list widget on BITMAP LCDs. */

#include "config.h"
#include "system.h"
#include "lcd.h"
#include "font.h"
#include "button.h"
#include "string.h"
#include "string-extra.h"
#include "settings.h"
#include "kernel.h"
#include "file.h"

#include "action.h"
#include "screen_access.h"
#include "list.h"
#include "scrollbar.h"
#include "lang.h"
#include "language.h"
#include "sound.h"
#include "misc.h"
#include "viewport.h"
#include "statusbar-skinned.h"
#include "debug.h"
#include "line.h"
#include "fixedpoint.h"
#include "rbunicode.h"
#include "rbpaths.h"

#define ICON_PADDING 1
#define ICON_PADDING_S "1"

#ifdef HAVE_TOUCHSCREEN
/* used in gui_synclist->scroll_mode */
enum {
    SCROLL_NONE = 0,        /* no scrolling */
    SCROLL_BAR,             /* scroll by using the scrollbar */
    SCROLL_SWIPE,           /* scroll by wiping over the screen */
    SCROLL_KINETIC,         /* state after releasing swipe */
};
#endif

/* these are static to make scrolling work */
static struct viewport list_text[NB_SCREENS], title_text[NB_SCREENS];

/* list-private helpers from the generic list.c (move to header?) */
int gui_list_get_item_offset(struct gui_synclist * gui_list, int item_width,
                             int text_pos, struct screen * display,
                             struct viewport *vp);
bool list_display_title(struct gui_synclist *list, enum screen_type screen);
int list_get_nb_lines(struct gui_synclist *list, enum screen_type screen);

void gui_synclist_scroll_stop(struct gui_synclist *lists)
{
    FOR_NB_SCREENS(i)
    {
        screens[i].scroll_stop_viewport(&list_text[i]);
        screens[i].scroll_stop_viewport(&title_text[i]);
        line_scroll_stop_parts(&screens[i]);
        screens[i].scroll_stop_viewport(lists->parent[i]);
    }
}

/* Draw the list...
    internal screen layout:
        -----------------
        |TI|  title     |   TI is title icon
        -----------------
        | | |            |
        |S|I|            |   S - scrollbar
        | | | items      |   I - icons
        | | |            |
        ------------------

        Note: This image is flipped horizontally when the language is a
        right-to-left one (Hebrew, Arabic)
*/

static int list_icon_width(enum screen_type screen)
{
    return get_icon_width(screen) + ICON_PADDING * 2;
}

static void _default_listdraw_fn(struct list_putlineinfo_t *list_info)
{
    struct screen *display = list_info->display; 
    int x = list_info->x;
    int y = list_info->y;
    int item_indent = list_info->item_indent;
    int item_offset = list_info->item_offset;
    int icon = list_info->icon;
    bool is_selected = list_info->is_selected;
    bool is_title = list_info->is_title;
    bool show_cursor = list_info->show_cursor;
    bool have_icons = list_info->have_icons;
    struct line_desc *linedes = list_info->linedes;
    const char *dsp_text = list_info->dsp_text;

    if (is_title)
    {
        if (have_icons)
            display->put_line(x, y, linedes, "$"ICON_PADDING_S"I$t",
                    icon, dsp_text);
        else
            display->put_line(x, y, linedes, "$t", dsp_text);
    }
    else if (show_cursor && have_icons)
    {
    /* the list can have both, one of or neither of cursor and item icons,
     * if both don't apply icon padding twice between the icons */
        display->put_line(x, y, 
                linedes, "$*s$"ICON_PADDING_S"I$i$"ICON_PADDING_S"s$*t",
                item_indent, is_selected ? Icon_Cursor : Icon_NOICON,
                icon, item_offset, dsp_text);
    }
    else if (show_cursor || have_icons)
    {
        display->put_line(x, y, linedes, "$*s$"ICON_PADDING_S"I$*t", item_indent,
                show_cursor ? (is_selected ? Icon_Cursor:Icon_NOICON):icon,
                item_offset, dsp_text);
    }
    else
    {
        display->put_line(x, y, linedes, "$*s$*t", item_indent, item_offset, dsp_text);
    }
}

static bool draw_title(struct screen *display,
                       struct gui_synclist *list,
                       list_draw_item *callback_draw_item)
{
    const int screen = display->screen_type;
    struct viewport *title_text_vp = &title_text[screen];
    struct line_desc linedes = LINE_DESC_DEFINIT;

    if (sb_set_title_text(list->title, list->title_icon, screen))
        return false; /* the sbs is handling the title */
    display->scroll_stop_viewport(title_text_vp);
    if (!list_display_title(list, screen))
        return false;
    *title_text_vp = *(list->parent[screen]);
    linedes.height = list->line_height[screen];
    title_text_vp->height = linedes.height;

#if LCD_DEPTH > 1
    /* XXX: Do we want to support the separator on remote displays? */
    if (display->screen_type == SCREEN_MAIN && global_settings.list_separator_height != 0)
        linedes.separator_height = abs(global_settings.list_separator_height)
                                + (lcd_get_dpi() > 200 ? 2 : 1);
#endif

#ifdef HAVE_LCD_COLOR
    if (list->title_color >= 0)
        linedes.style |= (STYLE_COLORED|list->title_color);
#endif
    linedes.scroll = true;

    display->set_viewport(title_text_vp);
    int icon = list->title_icon;
    int icon_w = list_icon_width(display->screen_type);
    bool have_icons = false;
    if (icon != Icon_NOICON && list->show_icons)
    {
        have_icons = true;
    }

    struct list_putlineinfo_t list_info =
    {
        .x = 0, .y = 0, .item_indent = 0, .item_offset = 0,
         .line = -1, .icon = icon, .icon_width = icon_w,
        .display = display, .vp = title_text_vp, .linedes = &linedes, .list = list,
        .dsp_text = list->title,
        .is_selected = false, .is_title = true, .show_cursor = false,
        .have_icons = have_icons
    };
    callback_draw_item(&list_info);

    return true;
}

/* The band of hints under a list of settings: the hint of the row selected,
   in three lines at most of a font of Now Playing, which its skin keeps loaded
   from boot, found by its path rather than loaded for it. 14 px in two lines
   was too small to read at a glance, and 17 px hardly larger. */
#define HINT_X      6
#define HINT_TOP    6
#define HINT_PITCH  21
#define HINT_LINES  3
#define HINT_BYTES  192

static int hint_font(void)
{
    int font = font_find(FONT_DIR "/18-Inter-SemiBold.fnt");
    if (font < 0)
        font = font_find(FONT_DIR "/18-Inter-Bold.fnt"); /* with Bold Text */
    return font; /* -1, as for a moment as the theme loads: the band empty */
}

/* Albums' years and songs' numbers, lighter than the names they're before:
   in the font of Now Playing's song, which its skin keeps loaded from boot.
   In the list's own as the theme loads, or with a theme without it. */
static int list_lead_font(int list_font)
{
    int font = font_find(FONT_DIR "/17-Inter-SemiBold.fnt");
    if (font < 0)
        font = font_find(FONT_DIR "/17-Inter-Bold.fnt"); /* with Bold Text */
    return font < 0 ? list_font : font;
}

/* Songs' lengths at the right of an album's rows, in the font of Now
   Playing's artist: lighter than the numbers before the names, whose column
   is narrow. Bold with Bold Text, as the theme has it then. In the list's own
   as the theme loads, or with a theme without them. */
static int list_value_font(int list_font)
{
    static const char *const fonts[2] = {
        FONT_DIR "/17-Inter-Medium.fnt", FONT_DIR "/17-Inter-Bold.fnt"
    };
    const int first = global_settings.bold_text ? 1 : 0;
    int font = font_find(fonts[first]);
    if (font < 0)
        font = font_find(fonts[!first]);
    return font < 0 ? list_font : font;
}

/* The second line of a row of two, as an album's year and artist, in the font
   of Now Playing's year: smaller and lighter than the name over it. Bold with
   Bold Text, as the theme has it then. In the list's own as the theme loads,
   or with a theme without them. */
int list_subline_font(int list_font)
{
    static const char *const fonts[2] = {
        FONT_DIR "/14-Inter-SemiBold.fnt", FONT_DIR "/14-Inter-Bold.fnt"
    };
    const int first = global_settings.bold_text ? 1 : 0;
    int font = font_find(fonts[first]);
    if (font < 0)
        font = font_find(fonts[!first]);
    return font < 0 ? list_font : font;
}

/* A row of two lines: the name's part, as high as the name's capitals, its
   accents and what goes below the line take, which its font's cell has some
   rows more than, and the second line's under it, its cell and a gap. With
   the theme's 24 and 14 px fonts, 19 and 17: six rows on the screen, and its
   two lines read as one row, closer than to the next one's name. The name's
   cell starts a row above its part and ends four rows into the next, which
   it draws nothing in (line.c keeps it to its part) */
#define NAME_PART_CUT  5 /* the name's cell's rows outside what it draws */
#define SUBLINE_GAP    3
#define NAME_TEXT_DY   2 /* its cell two rows lower than in the middle of its part */
#define SUBLINE_TOP_DY 0
#define COVER_GAP      8 /* from a cover to the text */
static int list_name_height(int font)
{
    return font_get(font)->height - NAME_PART_CUT;
}

int list_two_lines_height(int font)
{
    return list_name_height(font) + font_get(list_subline_font(font))->height
           + SUBLINE_GAP;
}

/* Quick Scroll's letter over a list, in Inter of 52 px, its capitals three
   times as tall as the list's: loaded whole after the theme's fonts, music
   stopped, and kept as they are, so that a fast turn of the wheel never waits
   for the disk. It gives its place up to any font loaded later with every
   place taken, a theme's or the library's, and comes again with the theme
   next; meanwhile the letter is in the list's own font. */
#define INITIAL_FONT   FONT_DIR "/53-Inter-Letters.fnt"
#define INITIAL_GLYPHS 160 /* more than it has: all of it read */

void list_load_initial_font(void)
{
    if (font_find(INITIAL_FONT) < 0)
        font_set_yielding(font_load_ex(INITIAL_FONT, 0, INITIAL_GLYPHS));
}

/* the letter for the row selected while it's due, NULL if it isn't or the
   row has none */
static const char *list_initial(struct gui_synclist *list, enum screen_type screen,
                                char *buf, size_t size)
{
    if (screen != SCREEN_MAIN || !list->callback_get_initial || !list->initial_until
        || !TIME_BEFORE(current_tick, list->initial_until))
        return NULL;
    const char *text = list->callback_get_initial(list->selected_item, list->data,
                                                  buf, size);
    return text && *text ? text : NULL;
}

/* What doesn't start a line of Chinese or Japanese: the punctuation that
   closes what's before it and Japanese's small kana. "。" went to a line of
   its own once its font's spacing made it as wide as a letter. */
static bool keeps_with_before(ucschar_t ch)
{
    switch (ch)
    {
    case 0x3001: case 0x3002:                           /* 、。 */
    case 0x3009: case 0x300b: case 0x300d: case 0x300f: /* 〉》」』 */
    case 0x3011: case 0x3015: case 0x3017: case 0x3019: /* 】〕〗〙 */
    case 0x301b: case 0x30fb: case 0x30fc:              /* 〛・ー */
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: /* ぁぃぅぇ */
    case 0x3049: case 0x3063: case 0x3083: case 0x3085: /* ぉっゃゅ */
    case 0x3087: case 0x308e:                           /* ょゎ */
    case 0x30a1: case 0x30a3: case 0x30a5: case 0x30a7: /* ァィゥェ */
    case 0x30a9: case 0x30c3: case 0x30e3: case 0x30e5: /* ォッャュ */
    case 0x30e7: case 0x30ee: case 0x30f5: case 0x30f6: /* ョヮヵヶ */
    case 0xff01: case 0xff09: case 0xff0c: case 0xff0e: /* ！），． */
    case 0xff1a: case 0xff1b: case 0xff1f:              /* ：；？ */
    case 0xff3d: case 0xff5d:                           /* ］｝ */
        return true;
    }
    return false;
}

/* Chinese and Japanese, which have no spaces: a line may end before any of
   their letters but those */
static bool breaks_before(ucschar_t ch)
{
    return ((ch >= 0x2e80 && ch <= 0x9fff) || (ch >= 0xf900 && ch <= 0xfaff)
            || (ch >= 0xff00 && ch <= 0xffef)) && !keeps_with_before(ch);
}

/* A line of the text from p, as wide as width at most: up to the last space,
   or letter of Chinese or Japanese, where it fits, or, for a word longer than
   a line, as many of its letters, one at least. Measured as it's drawn, by
   the parts between the places a line may end, where Arabic's letters stop
   joining: they take the forms of the letters beside them, and a mark drawn
   over the letter before it, as Hindi's vowels, takes no width. Its end.
   tools/check_hints.py wraps them the same. */
static const char *hint_line(const char *p, int font, int width)
{
    const char *q = p, *end = NULL;
    int sum = 0;
    while (true)
    {
        ucschar_t ch;
        const char *next = (const char *)utf8decode((const unsigned char *)q, &ch);
        if (ch == 0 || (q > p && (ch == ' ' || breaks_before(ch))))
        {
            const char *from = end ? end : p;
            int w;
            /* no longer than the line's buffer, whatever its width */
            if (q - p > HINT_BYTES - 4)
                break;
            font_getstringnsize((const unsigned char *)from, q - from, &w, NULL, font);
            if (sum + w > width)
                break;
            sum += w;
            end = q;
            if (ch == 0)
                break;
        }
        q = next;
    }
    if (end)
        return end;
    /* the letters that fit, of those that start and end within the buffer */
    int n = font_measurestring((const unsigned char *)p, HINT_BYTES - 4, width,
                               NULL, NULL, font);
    while (n > HINT_BYTES - 4)
    {
        do
            n--;
        while (n > 0 && (p[n] & 0xc0) == 0x80);
    }
    if (n > 0)
        return p + n;
    ucschar_t ch;
    return (const char *)utf8decode((const unsigned char *)p, &ch);
}

const char *list_line_end(const char *p, int font, int width)
{
    return hint_line(p, font, width);
}

void list_draw_hint(struct screen *display, struct viewport *band,
                    const char *text)
{
    const int font = hint_font();
    band->font = font < 0 ? FONT_UI : font;
    display->set_viewport(band);
    /* its own grey, as a group's bar, over the theme's backdrop */
    display->set_foreground(LIST_GROUP_BAR_COLOUR);
    display->set_drawmode(DRMODE_SOLID);
    display->fillrect(0, 0, band->width, band->height);
    if (!text || !*text || font < 0)
        return;
    const int width = band->width - 2 * HINT_X;
    display->set_foreground(LIST_GROUP_COLOUR);
    display->set_drawmode(DRMODE_FG);
    const char *p = text;
    for (int i = 0; i < HINT_LINES && *p; i++)
    {
        while (*p == ' ')
            p++;
        int w;
        const char *end = hint_line(p, font, width);
        char line[HINT_BYTES];
        size_t n = MIN((size_t)(end - p), sizeof(line) - 4);
        memcpy(line, p, n);
        line[n] = '\0';
        p = end;
        while (*p == ' ')
            p++;
        if (i == HINT_LINES - 1 && *p)
        {
            /* more than three lines' worth, as a translation too long: cut
               short where "…" still fits */
            static const char ellipsis[] = "\xe2\x80\xa6";
            int ew;
            font_getstringsize(ellipsis, &ew, NULL, band->font);
            while (n > 0 && font_getstringnsize(line, n, &w, NULL, band->font)
                            + ew > width)
            {
                do
                    n--;
                while (n > 0 && (line[n] & 0xc0) == 0x80);
            }
            memcpy(line + n, ellipsis, sizeof(ellipsis));
        }
        /* at the right end in a language written so: the viewport's, a copy
           of the list's, aligns it there */
        display->putsxy(HINT_X, HINT_TOP + i * HINT_PITCH, line);
    }
    display->set_drawmode(DRMODE_SOLID);
}

void list_draw(struct screen *display, struct gui_synclist *list)
{
    int start, end, item_offset, i;
    const int screen = display->screen_type;
    list_draw_item *callback_draw_item;

    const int list_start_item = list->start_item[screen];
    const bool scrollbar_in_left = (list->scrollbar == SCROLLBAR_LEFT);
    const bool scrollbar_in_right = (list->scrollbar == SCROLLBAR_RIGHT);
    const bool show_cursor = (list->cursor_style == SYNCLIST_CURSOR_NOSTYLE);
    const bool have_icons = list->callback_get_item_icon && list->show_icons;

    struct viewport *parent = (list->parent[screen]);
    struct line_desc linedes = LINE_DESC_DEFINIT;
    bool show_title;
    struct viewport *list_text_vp = &list_text[screen];
    int indent = 0;

    if (list->callback_draw_item != NULL)
        callback_draw_item = list->callback_draw_item;
    else
        callback_draw_item = _default_listdraw_fn;

    struct viewport * last_vp = display->set_viewport(parent);
    display->clear_viewport();
    if (!list->scroll_all)
        display->scroll_stop_viewport(list_text_vp);
    line_scroll_stop_parts(display);
    *list_text_vp = *parent;
    if ((show_title = draw_title(display, list, callback_draw_item)))
    {
        int title_height = title_text[screen].height;
        list_text_vp->y += title_height;
        list_text_vp->height -= title_height;
    }

    const int nb_lines = list_get_nb_lines(list, screen);
    int draw_lines = nb_lines;

    /* a list of hints: its rows, and the band below them to the end with the
       hint of the row selected. A row without one has the rows go on over the
       band, though the list scrolls as if it were there: no row moves as the
       hints come and go, and the selection never goes under it */
    struct viewport band = *list_text_vp;
    const char *hint = NULL;
    if (list_hint_band(list, screen) > 0)
    {
        hint = list->callback_get_hint(list->selected_item, list->data);
        if (hint && *hint)
        {
            list_text_vp->height = nb_lines * list->line_height[screen];
            band.y = list_text_vp->y + list_text_vp->height;
            band.height = parent->y + parent->height - band.y;
        }
        else
        {
            hint = NULL;
            draw_lines = list_text_vp->height / list->line_height[screen];
        }
    }

    linedes.height = list->line_height[screen];
    /* a list may show no selection, a size of 0, as Search's while its
       letters are typed: then its lines are each of one, as when it shows one */
    const int item_lines = MAX(list->selected_size, 1);
    linedes.nlines = item_lines;
#if LCD_DEPTH > 1
    /* XXX: Do we want to support the separator on remote displays? */
    if (display->screen_type == SCREEN_MAIN)
        linedes.separator_height = abs(global_settings.list_separator_height);
#endif
    start = list_start_item;
    end = start + draw_lines;

#ifdef HAVE_TOUCHSCREEN
    /* y_pos needs to be clamped now since it can overflow the maximum
     * in some cases, and we have no easy way to prevent this beforehand */
    int max_y_pos = list->nb_items * linedes.height - list_text[screen].height;
    if (max_y_pos > 0 && list->y_pos > max_y_pos)
        list->y_pos = max_y_pos;

    int draw_offset = list_start_item * linedes.height - list->y_pos;
    /* draw some extra items to not have empty lines at the top and bottom */
    if (draw_offset > 0)
    {
        /* make it negative for more consistent apparence when switching
         * directions */
        draw_offset -= linedes.height;
        if (start > 0)
            start--;
    }
    else if (draw_offset < 0) {
        if(end < list->nb_items)
            end++;
    }

    /* If the viewport is not an exact multiple of the line height, then
     * there will be space for one more partial line. */
    int spare_space = list_text_vp->height - linedes.height * nb_lines;
    if(nb_lines < list->nb_items && spare_space > 0 && end < list->nb_items)
        if(end < list->nb_items)
            end++;
#else
    #define draw_offset 0
#endif

    /* draw the scrollbar if its needed */
    if (list->scrollbar != SCROLLBAR_OFF)
    {
        /* if the scrollbar is shown the text viewport needs to shrink */
        if (nb_lines < list->nb_items)
        {
            struct viewport vp = *list_text_vp;
            vp.width = SCROLLBAR_WIDTH;
#ifndef HAVE_TOUCHSCREEN
            /* touchscreens must use full viewport height
             * due to pixelwise rendering */
            vp.height = linedes.height * nb_lines;
#endif
            list_text_vp->width -= SCROLLBAR_WIDTH;
            if (scrollbar_in_right)
                vp.x += list_text_vp->width;
            else /* left */
                list_text_vp->x += SCROLLBAR_WIDTH;
            struct viewport *last = display->set_viewport(&vp);

#ifndef HAVE_TOUCHSCREEN
            /* button targets go itemwise */
            int scrollbar_items = list->nb_items;
            int scrollbar_min = list_start_item;
            int scrollbar_max = list_start_item + nb_lines;
#else
            /* touchscreens use pixelwise scrolling */
            int scrollbar_items = list->nb_items * linedes.height;
            int scrollbar_min = list->y_pos;
            int scrollbar_max = list->y_pos + list_text_vp->height;
#endif
            gui_scrollbar_draw(display,
                    (scrollbar_in_left? 0: 1), 0, SCROLLBAR_WIDTH-1, vp.height,
                    scrollbar_items, scrollbar_min, scrollbar_max, VERTICAL);
            display->set_viewport(last);
        }
        /* shift everything a bit in relation to the title */
        else if (!VP_IS_RTL(list_text_vp) && scrollbar_in_left)
            indent += SCROLLBAR_WIDTH;
        else if (VP_IS_RTL(list_text_vp) && scrollbar_in_right)
            indent += SCROLLBAR_WIDTH;
    }

    display->set_viewport(list_text_vp);
    int icon_w = list_icon_width(screen);
    int character_width = display->getcharwidth();

    struct list_putlineinfo_t list_info =
    {
        .x = 0, .y = 0, .vp = list_text_vp, .list = list,
        .icon_width = icon_w, .is_title = false, .show_cursor = show_cursor,
        .have_icons = have_icons, .linedes = &linedes, .display = display
    };

    /* the column of the texts before the names, as albums' years, measured
       once for the list in its font; its height against the names' once a
       frame, two glyphs looked at */
    int lead_w = 0, lead_font = -1, lead_shift = 0;
    if (list->callback_get_lead && list->lead_digits > 0 && screen == SCREEN_MAIN)
    {
        lead_font = list_lead_font(list_text_vp->font);
        if (list->lead_font != lead_font)
        {
            list->lead_width = line_lead_width(display, lead_font,
                                               list->lead_digits,
                                               &list->lead_digit);
            list->lead_font = lead_font;
        }
        lead_w = list->lead_width;
        lead_shift = line_lead_shift(list_text_vp->font, lead_font);
    }

    /* the column of the values at the right end, as songs' lengths: as the
       lead's, none right to left, where the names run to that end */
    linedes.value[0] = '\0';
    const bool values = list->callback_get_value && list->value_widest[0]
                        && screen == SCREEN_MAIN && !VP_IS_RTL(list_text_vp);
    if (values)
    {
        const int value_font = list_value_font(list_text_vp->font);
        if (list->value_font != value_font)
        {
            list->value_width = line_value_width(value_font, list->value_widest);
            list->value_font = value_font;
        }
        linedes.value_font = value_font;
        linedes.value_width = list->value_width;
        linedes.value_shift = line_lead_shift(list_text_vp->font, value_font);
    }

    /* Quick Scroll's letter over the rows, if it's due and the row selected
       has one: that row doesn't scroll meanwhile, which would draw its text
       over the box */
    char initial_buf[16];
    const char *initial = list_initial(list, screen, initial_buf, sizeof(initial_buf));

    /* the block at the top, as much of it as shows: what's above the
       viewport cut off */
    if (start < list->block_rows && screen == SCREEN_MAIN)
        list->callback_draw_block(display, -start * linedes.height + draw_offset,
                                  list->data);

    /* rows of two lines: the name's part, then the second line's, in its
       font; and their covers' column, if the list has one */
    const int row_h = linedes.height;
    const bool two_lines = list->callback_get_subline && screen == SCREEN_MAIN;
    const int name_h = two_lines ? list_name_height(list_text_vp->font) : 0;
    const int subline_font = two_lines ? list_subline_font(list_text_vp->font) : -1;
    const bool covers = two_lines && list->callback_get_cover;

    for (i=MAX(start, list->block_rows); i<end && i<list->nb_items; i++)
    {
        /* do the text */
        enum themable_icons icon;
        unsigned const char *s;
        extern char simplelist_buffer[SIMPLELIST_MAX_LINES * SIMPLELIST_MAX_LINELENGTH];
        /*char entry_buffer[MAX_PATH]; use the buffer from gui/list.c instead */
        unsigned char *entry_name;
        int line = i - start;
        int line_indent = 0;
        int style = STYLE_DEFAULT;
        bool is_selected = false;
        s = list->callback_get_item_name(i, list->data, simplelist_buffer,
                                         sizeof(simplelist_buffer));
        if (P2ID((unsigned char *)s) > VOICEONLY_DELIMITER)
            entry_name = "";
        else
            entry_name = P2STR(s);

        while (*entry_name == '\t')
        {
            line_indent++;
            entry_name++;
        }
        if (line_indent)
        {
            if (list->show_icons)
                line_indent *= icon_w;
            else
                line_indent *= character_width;
        }
        /* the padding moves the text only, the selection stays full width */
        line_indent += indent + global_settings.list_text_padding;
        /* an album's cover, or the note in its place, where the text would
           start, the text after their column; at the right end right to
           left, as the text starts there */
        const fb_data *cover_pixels = NULL;
        const int cover = covers ? list->callback_get_cover(i, list->data,
                                                            &cover_pixels)
                                 : LIST_COVER_NONE;
        const int cover_x = VP_IS_RTL(list_text_vp) ?
            list_text_vp->width - line_indent - LINE_COVER_SIZE : line_indent;
        if (cover != LIST_COVER_NONE)
            line_indent += LINE_COVER_SIZE + COVER_GAP;
        /* a Shuffle Songs: its arrows where the text would start, the text
           after them */
        const int shuffle_x = line_indent;
        const bool shuffle = list->callback_is_shuffle
                             && list->callback_is_shuffle(i, list->data)
                             && line_shuffle_width(display);
        if (shuffle)
            line_indent += line_shuffle_width(display);
        /* a playlist's or folder's mark, its name after its column */
        const int mark_x = line_indent;
        const int mark = !shuffle && list->callback_get_mark ?
                         list->callback_get_mark(i, list->data) : LINE_MARK_NONE;
        if (mark != LINE_MARK_NONE)
            line_indent += line_mark_width(display);
        /* an album's year or a song's number, its name after their column,
           even with its cell empty */
        char lead_buf[12];
        const int lead_x = line_indent;
        const char *lead = lead_w && !shuffle && mark == LINE_MARK_NONE ?
            list->callback_get_lead(i, list->data, lead_buf, sizeof(lead_buf)) : NULL;
        if (lead)
            line_indent += lead_w;

        /* position the string at the correct offset place */
        int item_width,h;
        display->getstringsize(entry_name, &item_width, &h);
        item_offset = gui_list_get_item_offset(list, item_width, indent + (list->show_icons ? icon_w : 0),
                display, list_text_vp);

        /* draw the selected line */
        if(
#ifdef HAVE_TOUCHSCREEN
            /* don't draw it during scrolling */
            list->scroll_mode == SCROLL_NONE &&
#endif
                i >= list->selected_item
                && i <  list->selected_item + list->selected_size)
        {/* The selected item must be displayed scrolling */
#ifdef HAVE_LCD_COLOR
            if (list->selection_color)
            {
                /* Display gradient line selector */
                style = STYLE_GRADIENT;
                linedes.text_color = list->selection_color->text_color;
                linedes.line_color = list->selection_color->line_color;
                linedes.line_end_color = list->selection_color->line_end_color;
            }
            else
#endif
            if (list->cursor_style == SYNCLIST_CURSOR_INVERT
#ifdef HAVE_REMOTE_LCD
                    /* the global_settings.cursor_style check is here to make
                    * sure if they want the cursor instead of bar it will work
                    */
                    || (display->depth < 16 && list->cursor_style)
#endif
            )
            {
                /* Display inverted-line-style */
                style = STYLE_INVERT;
            }
#ifdef HAVE_LCD_COLOR
            else if (list->cursor_style == SYNCLIST_CURSOR_COLOR)
            {
                /* Display colour line selector */
                style = STYLE_COLORBAR;
                linedes.text_color = global_settings.lst_color;
                linedes.line_color = global_settings.lss_color;
            }
            else if (list->cursor_style == SYNCLIST_CURSOR_GRADIENT)
            {
                /* Display gradient line selector */
                style = STYLE_GRADIENT;
                linedes.text_color = global_settings.lst_color;
                linedes.line_color = global_settings.lss_color;
                linedes.line_end_color = global_settings.lse_color;
            }
#endif
            is_selected = true;
        }
        
#ifdef HAVE_LCD_COLOR
        /* if the list has a color callback */
        if (list->callback_get_item_color)
        {
            int c = list->callback_get_item_color(i, list->data);
            if (c >= 0)
            {   /* if color selected */
                linedes.text_color = c;
                style |= STYLE_COLORED;
            }
        }
#endif
        /* the song playing, a speaker at the right end, on any row */
        if (list->callback_is_playing && list->callback_is_playing(i, list->data))
            style |= STYLE_PLAYING;

        /* it's at the right end, over the start of right-to-left text */
        if (is_selected && global_settings.list_selector_chevron
            && !lang_is_rtl()
            && list->callback_has_children
            && list->callback_has_children(i, list->data))
            style |= STYLE_CHEVRON;
        linedes.style = style;
        /* of two lines, only the selected row's name: it's drawn in its part
           alone (line.c); none under Quick Scroll's letter */
        linedes.scroll = !initial && (is_selected || (list->scroll_all && !two_lines));
        linedes.line = i % item_lines;
        if (values)
        {
            char value_buf[sizeof(linedes.value)];
            const char *value = list->callback_get_value(i, list->data, value_buf,
                                                         sizeof(value_buf));
            strlcpy(linedes.value, value ? value : "", sizeof(linedes.value));
        }
        icon = list->callback_get_item_icon ?
                    list->callback_get_item_icon(i, list->data) : Icon_NOICON;


        list_info.y = line * row_h + draw_offset;
        list_info.is_selected = is_selected;
        list_info.item_indent = line_indent;
        list_info.line = i;
        list_info.icon = icon;
        list_info.dsp_text = entry_name;
        list_info.item_offset = item_offset;

        /* A row of two lines: the second line's part, its background or the
           selection's gradient there; the name's part over it, whose speaker
           and chevron, in the row's middle, go down into it; then the second
           line's text. Each part is drawn in its own rows only. A row of one
           line is one part, its text in the middle */
        char subline_buf[MAX_PATH];
        const char *subline = two_lines ?
            list->callback_get_subline(i, list->data, subline_buf,
                                       sizeof(subline_buf)) : NULL;
        if (subline)
        {
            struct line_desc part = linedes;
            part.style &= ~(STYLE_CHEVRON|STYLE_PLAYING);
            part.value[0] = '\0';
            part.scroll = false;
            part.height = row_h - name_h;
            part.row_top = name_h;
            part.row_height = row_h;
            display->put_line(list_info.x, list_info.y + name_h, &part, "");
            linedes.height = name_h;
            linedes.row_height = row_h;
            linedes.text_dy = NAME_TEXT_DY;
        }

        callback_draw_item(&list_info);
        if (subline)
            line_draw_subline(display, subline_font,
                              list_info.x + line_indent + (have_icons ? icon_w : 0),
                              list_info.y + name_h, SUBLINE_TOP_DY, &linedes,
                              subline);
        if (shuffle)
            line_draw_shuffle(display, list_info.x + shuffle_x, list_info.y,
                              &linedes);
        if (mark != LINE_MARK_NONE)
            line_draw_mark(display, list_info.x + mark_x, list_info.y,
                           &linedes, mark);
        if (lead && *lead)
            line_draw_lead(display, lead_font, lead_shift, list_info.x + lead_x,
                           list_info.y, &linedes, lead, lead_w,
                           list->lead_digit);
        if (cover != LIST_COVER_NONE)
            line_draw_cover(display, list_info.x + cover_x,
                            list_info.y + (row_h - LINE_COVER_SIZE) / 2,
                            cover == LIST_COVER_PIXELS ? cover_pixels : NULL);
        linedes.height = row_h;
        linedes.row_height = 0;
        linedes.text_dy = 0;
    }
    if (hint)
        list_draw_hint(display, &band, hint);
    if (initial)
    {
        /* in the middle of the rows and of the list's width, from the left
           of a copy of its viewport, as in a language written right to left:
           of as many rows as fit */
        struct viewport vp = *list_text_vp;
        vp.x = parent->x;
        vp.width = parent->width;
        vp.height = list_text_vp->height / row_h * row_h;
        vp.flags &= ~VP_FLAG_ALIGNMENT_MASK;
        display->set_viewport(&vp);
        line_draw_initial(display, vp.width / 2, vp.height / 2, initial,
                          font_find(INITIAL_FONT), vp.font);
        list->initial_shown = true;
    }
    display->set_viewport(last_vp);
}

#if defined(HAVE_TOUCHSCREEN)
/* This needs to be fixed if we ever get more than 1 touchscreen on a target. */

static void do_touch_scroll(struct gui_synclist *gui_list, int new_y_pos)
{
    if (new_y_pos < 0)
        new_y_pos = 0;

    int line_height = gui_list->line_height[SCREEN_MAIN];
    int new_start = new_y_pos / line_height;
    int nb_lines = list_get_nb_lines(gui_list, SCREEN_MAIN);
    if (new_start > gui_list->nb_items - nb_lines)
    {
        new_start = gui_list->nb_items - nb_lines;
        new_y_pos = new_start * line_height;
    }

    int new_item = new_start + nb_lines/2;
    if (gui_list->selected_size > 1)
        new_item -= new_item % gui_list->selected_size;

    gui_list->selected_item = new_item;
    gui_list->start_item[SCREEN_MAIN] = new_start;
    gui_list->y_pos = new_y_pos;
}

/* Handle touch scrolling using the scrollbar. Pass the screen y-coordinate
 * of the current touch position. */
static int scrollbar_scroll(struct gui_synclist *gui_list, int y)
{
    const enum screen_type screen = SCREEN_MAIN;
    const int nb_lines = list_get_nb_lines(gui_list, screen);

    if (nb_lines < gui_list->nb_items)
    {
        const int line_height = gui_list->line_height[screen];
        int bar_height = list_text[screen].height;
        int bar_y = y - list_text[screen].y;

        if (bar_y < 0)
            bar_y = 0;
        else if(bar_y >= bar_height)
            bar_y = bar_height - 1;

        int new_y_pos = (bar_y * gui_list->nb_items * line_height) / bar_height;
        new_y_pos -= (nb_lines * line_height) / 2;

        do_touch_scroll(gui_list, new_y_pos);

        return ACTION_REDRAW;
    }

    return ACTION_NONE;
}

/* Handle swipe scrolling on a list. 'delta' is the distance to scroll
 * relative to gui_list->scroll_base_y, which should be set up at the
 * beginning of scrolling. */
static int swipe_scroll(struct gui_synclist *gui_list, int delta)
{
    /* nothing to do if the list does not scroll */
    const int nb_lines = list_get_nb_lines(gui_list, SCREEN_MAIN);
    if (nb_lines >= gui_list->nb_items)
        return ACTION_NONE;

    do_touch_scroll(gui_list, gui_list->scroll_base_y - delta);

    return ACTION_REDRAW;
}

/* kinetic scrolling, based on
 *
 * v = a*t + v0 and ds = v*dt
 *
 * In each (fixed interval) timeout, the list is advanced by ds, then
 * the v is reduced by a.
 * This way we get a linear and smooth deceleration of the scrolling
 *
 * As v is the difference of distance per time unit, v is passed (as
 * pixels moved since the last call) to the scrolling function which takes
 * care of the pixel accurate drawing
 *
 * v0 is dertermined by averaging the last 4 movements of the list
 * (the pixel and time difference is used to compute each v)
 *
 * influenced by http://stechz.com/tag/kinetic/
 * We take the easy and smooth first approach (until section "Drawbacks"),
 * since its drawbacks don't apply for us since our timers seem to be
 * relatively accurate
 */

#define SIGN(a) ((a) < 0 ? -1 : 1)
#define RELOAD_INTERVAL (HZ/25)
#define RELOAD_INTERVAL_FP ((RELOAD_INTERVAL << LIST_KINETIC_FRACBITS) / HZ)

struct kinetic_cb_data {
    struct gui_synclist *list;
    long velocity;
    long distance;
    long scroll_duration;
    long press_duration;
};

struct kinetic {
    /* callback for performing the scroll animation */
    struct kinetic_cb_data cb_data;
    struct timeout tmo;
};

static struct kinetic kinetic;
static struct gesture_vel list_gvel;

/*
 * NOTE: defaults were chosen for the Shanling Q1, and may need
 *       tuning for other devices
 */

const struct list_kinetic_scroll_settings list_kinetic_scroll_accel_default = {
    .a0 = 200 << LIST_KINETIC_FRACBITS,         /* 200.0 */
    .a1 = (6 << LIST_KINETIC_FRACBITS) / 5,     /* 1.2 */
};

const struct list_kinetic_scroll_settings list_kinetic_scroll_brake_default = {
    .a0 = 200 << LIST_KINETIC_FRACBITS,         /* 200.0 */
    .a1 = (1 << LIST_KINETIC_FRACBITS) / 2,     /* 0.5 */
};

const struct list_kinetic_scroll_settings list_kinetic_scroll_decel_default = {
    .a0 = 2000 << LIST_KINETIC_FRACBITS,        /* 2000.0 */
    .a1 = (1 << LIST_KINETIC_FRACBITS) / 2,     /* 0.5 */
    .delay = 125 * HZ / 1000,                   /* 125 ms */
};

const struct list_kinetic_scroll_settings list_kinetic_scroll_press_default = {
    .a0 = 35000 << LIST_KINETIC_FRACBITS,       /* 35000.0 */
    .a1 = 4 << LIST_KINETIC_FRACBITS,           /* 4.0 */
    .delay = 250 * HZ / 1000,                   /* 250 ms */
};

static void kinetic_stop_scrolling(struct kinetic *k, struct gui_synclist *list)
{
    if (k->cb_data.list == list)
    {
        k->cb_data.list = NULL;
        k->cb_data.velocity = 0;
        k->cb_data.distance = 0;
        timeout_cancel(&k->tmo);
    }
}

/* helper for gui/list.c to cancel scrolling if a normal button event comes */
void _gui_synclist_stop_kinetic_scrolling(struct gui_synclist *list)
{
    if (list->scroll_mode == SCROLL_KINETIC)
    {
        kinetic_stop_scrolling(&kinetic, list);
        list->scroll_mode = SCROLL_NONE;
    }
}

static long kinetic_calc_accel(long input, long duration,
                               const struct list_kinetic_scroll_settings *param)
{
    if (duration < param->delay)
        return 0;

    long x1 = input < 0 ? -input : input;
    long r = fp_mul(param->a1, x1, LIST_KINETIC_FRACBITS) +
             param->a0 + (1 << (LIST_KINETIC_FRACBITS - 1));

    return SIGN(input) * r;
}

static int kinetic_callback(struct timeout *tmo)
{
    struct kinetic_cb_data *data = (struct kinetic_cb_data*)tmo->data;
    struct gui_synclist *list = data->list;
    int pixel_diff, action;

    /* deal with cancellation */
    if (!list || list->scroll_mode != SCROLL_KINETIC)
        return 0;

    long abs_vel = data->velocity < 0 ? -data->velocity : data->velocity;
    long vel_sgn = SIGN(data->velocity);

    data->distance += fp_mul(abs_vel, RELOAD_INTERVAL_FP, LIST_KINETIC_FRACBITS);
    data->scroll_duration += RELOAD_INTERVAL;

    pixel_diff = data->distance >> LIST_KINETIC_FRACBITS;
    data->distance -= pixel_diff << LIST_KINETIC_FRACBITS;

    action = swipe_scroll(list, pixel_diff * vel_sgn);
    if (action == ACTION_REDRAW)
    {
        /* force the list to redraw */
        button_queue_post(BUTTON_REDRAW, 0);
    }

    /* calculate and apply deceleration */
    long abs_decel = 0;
    abs_decel += kinetic_calc_accel(abs_vel, data->scroll_duration,
                                    &global_settings.kinetic_scroll_decel);
    abs_decel += kinetic_calc_accel(abs_vel, data->press_duration,
                                    &global_settings.kinetic_scroll_press);
    abs_decel = fp_mul(abs_decel, RELOAD_INTERVAL_FP, LIST_KINETIC_FRACBITS);

    /*
     * Ensure velocity is smoothly reduced to zero to avoid jerky
     * scrolling near the end of scrolling.
     */
    if (abs_vel < (3 << LIST_KINETIC_FRACBITS))
        data->velocity = 0;
    else if (abs_vel < abs_decel)
        data->velocity /= 3;
    else
        data->velocity -= SIGN(data->velocity) * abs_decel;

    /* stop scrolling if we didn't move, it means we hit the end */
    if (list->y_pos == list->scroll_base_y && pixel_diff != 0)
        data->velocity = 0;
    else
        /* update base y since our scroll distance doesn't accumulate. */
        list->scroll_base_y = list->y_pos;

    if (data->velocity == 0)
    {
        list->scroll_mode = SCROLL_NONE;
        return 0;
    }

    return RELOAD_INTERVAL;
}

/* Computes the initial velocity v0 and sets up the timer */
static bool kinetic_start_scrolling(struct kinetic *k, struct gui_synclist *list)
{
    int xvel, yvel;
    gesture_vel_get(&list_gvel, &xvel, &yvel);
    if (yvel == 0)
        return false;

    long yvel_fp = yvel << LIST_KINETIC_FRACBITS;
    int vel_sgn = SIGN(k->cb_data.velocity);

    if (list->scroll_mode == SCROLL_KINETIC)
    {
        long accel;
        if (SIGN(yvel) == vel_sgn)
            accel = kinetic_calc_accel(k->cb_data.velocity, LONG_MAX,
                                       &global_settings.kinetic_scroll_accel);
        else
            accel = -kinetic_calc_accel(k->cb_data.velocity, LONG_MAX,
                                        &global_settings.kinetic_scroll_brake);

        yvel_fp += accel;
    }

    k->cb_data.list = list;
    k->cb_data.velocity += yvel_fp;
    /* also reset distance & scroll duration after a direction change */
    if (list->scroll_mode != SCROLL_KINETIC ||
        vel_sgn != SIGN(k->cb_data.velocity))
    {
        k->cb_data.distance = 0;
        k->cb_data.scroll_duration = 0;
    }

    list->scroll_mode = SCROLL_KINETIC;
    list->scroll_base_y = list->y_pos;
    timeout_register(&k->tmo, kinetic_callback, RELOAD_INTERVAL,
                     (intptr_t)&k->cb_data);
    return true;
}

#define OUTSIDE    0
#define TITLE_TEXT (1<<0)
#define TITLE_ICON (1<<1)
#define SCROLLBAR  (1<<2)
#define LIST_TEXT  (1<<3)
#define LIST_ICON  (1<<4)

#define TITLE      (TITLE_TEXT|TITLE_ICON)
#define LIST       (LIST_TEXT|LIST_ICON)

static int get_click_location(struct gui_synclist *list, int x, int y)
{
    const enum screen_type screen = SCREEN_MAIN;
    int retval = OUTSIDE;

    struct viewport *parent = list->parent[screen];
    struct viewport *text = &list_text[screen];
    struct viewport *title = &title_text[screen];

    if (viewport_point_within_vp(parent, x, y))
    {
        /* see if the title was clicked */
        if (viewport_point_within_vp(title, x, y))
            retval = TITLE_TEXT;

        /* check the title icon */
        if (list->title_icon != Icon_NOICON && list->show_icons)
        {
            int width = list_icon_width(screen);
            struct viewport vp = *title;
            if (VP_IS_RTL(&vp))
                vp.x += vp.width;
            else
                vp.x -= width;
            vp.width = width;
            if (viewport_point_within_vp(&vp, x, y))
                retval = TITLE_ICON;
        }

        /* check scrollbar if shown */
        if (retval == OUTSIDE)
        {
            bool on_scrollbar_clicked;
            int adj_x = x - text->x;
            switch (list->scrollbar)
            {
                case SCROLLBAR_OFF:
                    /*fall-through*/
                default:
                    on_scrollbar_clicked = false;
                    break;
                case SCROLLBAR_LEFT:
                    on_scrollbar_clicked = adj_x <= SCROLLBAR_WIDTH;
                    break;
                case SCROLLBAR_RIGHT:
                    on_scrollbar_clicked = adj_x > (text->x + text->width - SCROLLBAR_WIDTH);
                    break;
            }
            if (on_scrollbar_clicked)
                retval = SCROLLBAR;
        }

        /* check the text area */
        if (retval == OUTSIDE)
        {
            text = &list_text[screen];
            if (viewport_point_within_vp(text, x, y))
                retval = LIST_TEXT;
            else /* if all fails, it must be on the list icons */
                retval = LIST_ICON;
        }
    }
    return retval;
}


static int list_do_flick(const struct gesture_event *gevent)
{
    if (!global_settings.touch_enable_flick_shortcuts)
        return ACTION_NONE;

    switch (gesture_flick_get(gevent))
    {
    case GESTURE_FLICK_TOP:
        return ACTION_STD_QUICKSCREEN;
    case GESTURE_FLICK_LEFT:
        return ACTION_STD_CANCEL;
    case GESTURE_FLICK_RIGHT:
        return ACTION_TREE_WPS;
    default:
        return ACTION_NONE;
    }
}

unsigned gui_synclist_do_touchscreen(struct gui_synclist *list)
{
    struct touchevent tevent;
    struct gesture_event gevent;

    action_get_touch_event(&tevent);
    if (!action_gesture_get_event(&gevent))
        return ACTION_NONE;

    const enum screen_type screen = SCREEN_MAIN;
    struct viewport *list_vp = list->parent[screen];
    int adj_x = gevent.x - list_vp->x;
    int adj_y = gevent.y - list_vp->y;
    int line_height = list->line_height[screen];
    int start_item = list->start_item[screen];
    int start_y = start_item * line_height;
    int action = ACTION_NONE;
    int click_loc;

    if (action_gesture_is_pressed())
        kinetic.cb_data.press_duration = gevent.last_tick - gevent.start_tick;
    else
        kinetic.cb_data.press_duration = -1;

    switch (gevent.id)
    {
    case GESTURE_NONE:
        if (!action_gesture_is_pressed())
            break;
        /* fallthrough */

    case GESTURE_TAP:
    case GESTURE_LONG_PRESS:
        /* In kinetic mode taps and presses only decelerate scrolling.
         * User needs to wait until scrolling stops to select an item. */
        if (list->scroll_mode == SCROLL_KINETIC)
        {
            if (gevent.id != GESTURE_NONE)
                gesture_vel_reset(&list_gvel);
            break;
        }

        click_loc = get_click_location(list, gevent.x, gevent.y);
        if (click_loc & LIST)
        {
            int line;
            if(!skinlist_get_item(&screens[screen], list, adj_x, adj_y, &line))
            {
                line = (adj_y - (start_y - list->y_pos)) / line_height;
                if (list_display_title(list, screen))
                    line -= 1;
            }

            int new_item = start_item + line;
            if (new_item < list->nb_items)
            {
                if (list->selected_size > 1)
                    new_item -= new_item % list->selected_size;

                list->selected_item = new_item;
                gui_synclist_speak_item(list);

                if (gevent.id == GESTURE_TAP)
                    action = ACTION_STD_OK;
                else if (gevent.id == GESTURE_LONG_PRESS)
                    action = ACTION_STD_CONTEXT;
                else
                    action = ACTION_REDRAW;
            }
        }
        else if ((click_loc & TITLE) && gevent.id == GESTURE_TAP)
        {
            if (click_loc & TITLE_TEXT)
                action = ACTION_STD_CANCEL;
            else if (click_loc & TITLE_ICON)
                action = ACTION_STD_MENU;
        }
        else if (gevent.id != GESTURE_NONE && (click_loc & SCROLLBAR))
        {
            action = scrollbar_scroll(list, gevent.y);

            /* allow long press to become a motion event */
            if (gevent.id != GESTURE_TAP)
                break;
        }

        if (action != ACTION_REDRAW)
            gui_synclist_select_item(list, list->selected_item);
        if (gevent.id != GESTURE_NONE)
            action_gesture_reset();
        break;

    case GESTURE_DRAGSTART:
        gesture_vel_reset(&list_gvel);
        /* fallthrough */

    case GESTURE_DRAG:
        gesture_vel_process(&list_gvel, &tevent);

        action = list_do_flick(&gevent);
        if (action != ACTION_NONE)
        {
            action_gesture_reset();
            break;
        }

        if (list->scroll_mode == SCROLL_NONE)
        {
            list->scroll_base_y = list->y_pos;
            click_loc = get_click_location(list, gevent.ox, gevent.oy);

            if (click_loc & SCROLLBAR)
                list->scroll_mode = SCROLL_BAR;
            else if (click_loc & LIST)
                list->scroll_mode = SCROLL_SWIPE;
        }

        if (list->scroll_mode == SCROLL_BAR)
            action = scrollbar_scroll(list, gevent.y);
        else if (list->scroll_mode == SCROLL_SWIPE)
            action = swipe_scroll(list, gevent.y - gevent.oy);
        break;

    case GESTURE_RELEASE:
        if (list->scroll_mode == SCROLL_BAR)
            list->scroll_mode = SCROLL_NONE;
        else if(!kinetic_start_scrolling(&kinetic, list) &&
                list->scroll_mode != SCROLL_KINETIC)
            list->scroll_mode = SCROLL_NONE;

        action_gesture_reset();
        action = ACTION_REDRAW;
        break;

    default:
        break;
    }

    return action;
}

#endif
