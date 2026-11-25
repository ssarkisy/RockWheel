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
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "lcd.h"
#include "font.h"
#include "rbpaths.h"
#include "settings.h"
#include "lang.h"
#include "language.h"
#include "rbunicode.h"
#include "screen_access.h"
#include "viewport.h"
#include "line.h"
#include "ui_theme.h"
#include "wps.h"
#include "audio.h"
#include "playback.h"
#include "itunesdb.h"
#include "skin_engine/skin_engine.h"
#include "skin_engine/wps_internals.h"
#include "skin_engine/menu_cover.h"
#include "statusbar-skinned.h"
#include "cover_caution.h"

#if defined(ITUNESDB_HAVE_ART) && defined(HAVE_LCD_COLOR) \
    && !defined(DISABLE_ALPHA_BITMAP)
#include "cover_marks.h"

extern void lcd_alpha_bitmap_part(const unsigned char *src, int src_x,
                                  int src_y, int stride, int x, int y,
                                  int width, int height);

/* a "?": a 4-bit mask (cover_marks.h), and Classic's shadow under it */
struct mark {
    const unsigned char *ink, *shadow;
    int w, h;
};

/* What a built-in theme draws them with, and where its skins have the places
   they go in: the viewports shown in a cover's place, and the fonts its
   skins load */
struct look {
    const char *tile_vp;    /* Now Playing's tile with no cover */
    const char *np_font;
    unsigned np_fg[2];      /* light, dark */
    int tile_x, tile_y, tile_s; /* Classic's tile in its backdrops, which
                                   their viewports have at other places */
    const char *big_vp;     /* the full view's 200 px note, NULL for a skin
                               that leaves the full view without a cover */
    const char *pane_vp[2]; /* the pane with no cover: playing, stopped */
    const char *pane_font;
    unsigned pane_fg[2];
    int pane_x, pane_w, pane_note_y;
    short pane_cols[4];     /* where its ground has no text (clear_rows()):
                               Classic's title runs to x 316 */
    struct mark large, pane, row, header;
    /* the large marks' colours from top to foot, as its large notes'
       bitmaps have them; both 0 for the notes' colours (ui_colour()) */
    unsigned large_top, large_foot;
    bool classic;           /* white marks on the backdrop's grey */
};

static const struct look looks[UI_THEME_COUNT] = {
    [UI_THEME_CLASSIC] = {
        "info", "12-Inter-SemiBold", { LCD_WHITE, LCD_WHITE }, 12, 41, 136,
        NULL, { "normal", "stopped" }, "14-Inter-SemiBold",
        { LCD_WHITE, LCD_WHITE }, 160, 154, 57, { 160, 161, 162, 319 },
        { NULL, NULL, 0, 0 }, MARK_CLASSIC_PANE, MARK_CLASSIC_ROW,
        MARK_CLASSIC_HEADER, 0, 0, true },
    [UI_THEME_MODERN] = {
        "noart", "14-Inter-Medium",
        { LCD_RGBPACK(0x52, 0x51, 0x52), LCD_RGBPACK(0xb4, 0xb6, 0xbd) },
        0, 0, 0, "bignote", { "note", NULL }, "14-Inter-Medium",
        { LCD_RGBPACK(0x52, 0x51, 0x52), LCD_RGBPACK(0xb4, 0xb6, 0xbd) },
        160, 144, 70, { 312, 313, 314, 315 },
        MARK_MODERN_LARGE, MARK_MODERN_PANE, MARK_MODERN_ROW,
        MARK_MODERN_HEADER,
        LCD_RGBPACK(0xf3, 0x5c, 0x74), LCD_RGBPACK(0x8d, 0x6e, 0xd8), false },
    [UI_THEME_NEO] = {
        "noart", "14-Inter-Medium",
        { LCD_RGBPACK(0x52, 0x4d, 0x4a), LCD_RGBPACK(0xb5, 0xae, 0xa5) },
        0, 0, 0, "bignote", { "note", NULL }, "15-Inter-Medium",
        { LCD_RGBPACK(0x52, 0x51, 0x4a), LCD_RGBPACK(0xb5, 0xb2, 0xa5) },
        160, 144, 62, { 312, 313, 314, 315 },
        MARK_NEO_LARGE, MARK_NEO_LARGE, MARK_NEO_ROW, MARK_NEO_HEADER, 0, 0,
        false },
    /* Neo's marks in Dieter's grey, in its grille's places */
    [UI_THEME_DIETER] = {
        "noart", "14-Inter-Medium",
        { LCD_RGBPACK(0x6b, 0x69, 0x63), LCD_RGBPACK(0x9c, 0x9a, 0x9c) },
        0, 0, 0, "bignote", { "grille", NULL }, "15-Inter-Medium",
        { LCD_RGBPACK(0x6b, 0x69, 0x63), LCD_RGBPACK(0x9c, 0x9a, 0x9c) },
        160, 144, 62, { 312, 313, 314, 315 },
        MARK_NEO_LARGE, MARK_NEO_LARGE, MARK_NEO_ROW, MARK_NEO_HEADER, 0, 0,
        false },
};

static const struct look *look(void)
{
    const int t = global_settings.ui_theme;
    return &looks[t >= 0 && t < UI_THEME_COUNT ? t : UI_THEME_CLASSIC];
}

bool cover_caution_on(void)
{
    return global_settings.cover_caution;
}

static const int texts[][2] = {
    [ITUNESDB_COVER_PNG]         = { LANG_COVER_PNG, LANG_COVER_PNG_ACTION },
    [ITUNESDB_COVER_PROGRESSIVE] = { LANG_COVER_PROGRESSIVE,
                                     LANG_COVER_PROGRESSIVE_ACTION },
    [ITUNESDB_COVER_UNREADABLE]  = { LANG_COVER_UNREADABLE,
                                     LANG_COVER_UNREADABLE_ACTION },
};

int cover_caution_problem(int reason)
{
    return reason > 0 && reason < (int)ARRAYLEN(texts) ? texts[reason][0] : 0;
}

int cover_caution_action(int reason)
{
    return reason > 0 && reason < (int)ARRAYLEN(texts) ? texts[reason][1] : 0;
}

/* a font a skin has loaded, by its name: none is loaded here, as while music
   plays nothing may be taken; the UI's without it */
static int loaded_font(const char *name)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), FONT_DIR "/%s.fnt", name);
    for (int id = FONT_FIRSTUSERFONT; id < MAXFONTS; id++)
    {
        if (font_filename_matches_loaded_id(id, path))
            return id;
    }
    return FONT_UI;
}

/* whether a font has each of the text's letters */
static bool has_glyphs(int font, const char *s)
{
    const struct font *pf = font_get(font);
    const unsigned char *p = (const unsigned char *)s;
    while (*p)
    {
        ucschar_t c;
        p = utf8decode(p, &c);
        if (c < pf->firstchar || c >= pf->firstchar + (unsigned)pf->size)
            return false;
    }
    return true;
}

/* the skin's font for a reason's two lines, or the UI's, the language's own,
   if it hasn't their letters: Classic's 12 px has no Chinese, Japanese or
   Korean */
static int lines_font(const char *name, int reason)
{
    const int font = loaded_font(name);
    return has_glyphs(font, str(cover_caution_problem(reason)))
           && has_glyphs(font, str(cover_caution_action(reason)))
           ? font : FONT_UI;
}

static unsigned mix(unsigned a, unsigned b, int pct)
{
    const unsigned pa = FB_UNPACK_SCALAR_LCD(a), pb = FB_UNPACK_SCALAR_LCD(b);
    return LCD_RGBPACK(
        (RGB_UNPACK_RED(pa) * (100 - pct) + RGB_UNPACK_RED(pb) * pct) / 100,
        (RGB_UNPACK_GREEN(pa) * (100 - pct) + RGB_UNPACK_GREEN(pb) * pct) / 100,
        (RGB_UNPACK_BLUE(pa) * (100 - pct) + RGB_UNPACK_BLUE(pb) * pct) / 100);
}

/* a mask at x, y of the current viewport, row by row from top to foot */
static void mask_rows(const unsigned char *alpha, int w, int h, int x, int y,
                      unsigned top, unsigned foot)
{
    lcd_set_drawmode(DRMODE_FG);
    for (int row = 0; row < h; row++)
    {
        lcd_set_foreground(h > 1 ? mix(top, foot, row * 100 / (h - 1)) : top);
        lcd_alpha_bitmap_part(alpha, 0, row, w, x, y + row, w, 1);
    }
    lcd_set_drawmode(DRMODE_SOLID);
}

/* a large mark at x, y: in the theme's large notes' colours, or the notes'
   ones; Classic's white, shaded down to the backdrop note's grey, over its
   soft shadow */
static void draw_mark(const struct look *lk, const struct mark *m, int x, int y)
{
    if (!lk->classic)
    {
        if (lk->large_top || lk->large_foot)
            mask_rows(m->ink, m->w, m->h, x, y, lk->large_top, lk->large_foot);
        else
            line_draw_note_rows(m->ink, m->w, m->w, m->h, x, y);
        return;
    }
    if (m->shadow)
        mask_rows(m->shadow, m->w, m->h, x, y, LCD_RGBPACK(0x3a, 0x40, 0x46),
                  LCD_RGBPACK(0x2a, 0x2e, 0x33));
    mask_rows(m->ink, m->w, m->h, x, y, LCD_RGBPACK(0xff, 0xff, 0xff),
              LCD_RGBPACK(0xc9, 0xcd, 0xd2));
}

/* The ground under the note again, a row at a time as four columns with
   neither note nor text have it, one for each x mod 4 (cols[x & 3]),
   repeated: a dithered gradient, as Classic's pane, which repeats every
   4 px, stays dithered; the others are flat */
static void clear_rows(int x, int y, int w, int h, const short cols[4])
{
    for (int r = 0; r < h; r++)
    {
        fb_data src[4];
        for (int k = 0; k < 4; k++)
            src[cols[k] & 3] = *FBADDR(cols[k], y + r);
        fb_data *p = FBADDR(x, y + r);
        for (int i = 0; i < w; i++)
            p[i] = src[(x + i) & 3];
    }
}

/* four columns side by side from sx, for clear_rows() */
static void columns(int sx, short cols[4])
{
    for (int k = 0; k < 4; k++)
        cols[k] = sx + k;
}

/* Classic's grey tile, its backdrop's from top to foot but darker at the
   top: white lines on the backdrop's own, light at the top, had 2.4:1 there,
   and dark ones 2:1 at the foot; on this, white has 4.7:1 at least. Dithered
   4x4 to the screen's 16 bits as the backdrop is. */
static void classic_tile(int x, int y, int w, int h)
{
    static const unsigned char bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6,
                                             3, 11, 1, 9, 15, 7, 13, 5 };
    for (int r = 0; r < h; r++)
    {
        const int R = 104 - 30 * r / (h - 1), G = 115 - 34 * r / (h - 1),
                  B = 122 - 40 * r / (h - 1);
        fb_data *p = FBADDR(x, y + r);
        for (int i = 0; i < w; i++)
        {
            const int d = 2 * bayer[(r & 3) * 4 + ((x + i) & 3)] - 15;
            p[i] = FB_RGBPACK(MAX(0, MIN(255, R + d / 4)),
                              MAX(0, MIN(255, G + d / 8)),
                              MAX(0, MIN(255, B + d / 4)));
        }
    }
}

/** A line wrapped in as few lines as wrapping at its spaces takes, where it
    then breaks so that its longest line is the shortest it can be, with no
    word alone at the end. A word wider than the place, as in a language
    without spaces, is broken between its characters. **/
#define LINES_MAX 6
#define WORDS_MAX 24
struct para {
    int n;
    const char *at[LINES_MAX];
    int len[LINES_MAX], w[LINES_MAX];
};

static int width_of(int font, const char *s, int len)
{
    char buf[160];
    if (len >= (int)sizeof(buf))
        len = sizeof(buf) - 1;
    memcpy(buf, s, len);
    buf[len] = '\0';
    int w, h;
    font_getstringsize((const unsigned char *)buf, &w, &h, font);
    return w;
}

/* the words of text: where each starts and ends, a long one in pieces, and
   whether a space comes before it */
struct words {
    int n;
    int start[WORDS_MAX], end[WORDS_MAX];
    bool spaced[WORDS_MAX];
};

static void split(struct words *wd, const char *text, int font, int width)
{
    wd->n = 0;
    const int len = strlen(text);
    int i = 0;
    while (i < len && wd->n < WORDS_MAX)
    {
        while (i < len && text[i] == ' ')
            i++;
        if (i >= len)
            break;
        int e = i;
        while (e < len && text[e] != ' ')
            e++;
        bool spaced = wd->n > 0;
        /* pieces of it that fit, cut at characters (UTF-8 lead bytes) */
        int s = i;
        while (s < e && wd->n < WORDS_MAX)
        {
            int cut = e;
            while (cut > s + 1 && width_of(font, text + s, cut - s) > width)
            {
                cut--;
                while (cut > s + 1 && (text[cut] & 0xc0) == 0x80)
                    cut--;
            }
            wd->start[wd->n] = s;
            wd->end[wd->n] = cut;
            wd->spaced[wd->n] = spaced;
            wd->n++;
            spaced = false;
            s = cut;
        }
        i = e;
    }
}

static void wrap(struct para *p, const char *text, int font, int width)
{
    struct words wd;
    split(&wd, text, font, width);
    p->n = 0;
    if (!wd.n)
        return;
    /* as few lines as greedily */
    int lines = 1, first = 0;
    for (int k = 1; k < wd.n; k++)
    {
        if (width_of(font, text + wd.start[first], wd.end[k] - wd.start[first])
            > width)
        {
            lines++;
            first = k;
        }
    }
    lines = MIN(lines, LINES_MAX);
    /* then where to break them: the words each line starts with, tried in
       turn, the longest line the shortest */
    int starts[LINES_MAX], best[LINES_MAX], best_w = 1 << 30;
    for (int l = 0; l < lines; l++)
        starts[l] = l;
    best[0] = 0;
    while (true)
    {
        int worst = 0;
        for (int l = 0; l < lines; l++)
        {
            const int a = starts[l];
            const int b = l + 1 < lines ? starts[l + 1] - 1 : wd.n - 1;
            worst = MAX(worst, width_of(font, text + wd.start[a],
                                        wd.end[b] - wd.start[a]));
        }
        if (worst < best_w)
        {
            best_w = worst;
            memcpy(best, starts, sizeof(starts));
        }
        /* the next way, the breaks after the first moved on */
        int l = lines - 1;
        while (l > 0 && starts[l] == wd.n - (lines - l))
            l--;
        if (l <= 0)
            break;
        starts[l]++;
        for (int k = l + 1; k < lines; k++)
            starts[k] = starts[k - 1] + 1;
    }
    for (int l = 0; l < lines; l++)
    {
        const int a = best[l];
        const int b = l + 1 < lines ? best[l + 1] - 1 : wd.n - 1;
        p->at[l] = text + wd.start[a];
        p->len[l] = wd.end[b] - wd.start[a];
        p->w[l] = width_of(font, p->at[l], p->len[l]);
    }
    p->n = lines;
}

static int paras_height(const struct para *ps, int n, int font, int gap)
{
    int h = 0;
    for (int i = 0; i < n; i++)
        h += ps[i].n * font_get(font)->height + (i ? gap : 0);
    return h;
}

/* the lines centred on cx from y, in the current viewport */
static void draw_paras(struct screen *display, const struct para *ps, int n,
                       unsigned fg, int font, int cx, int y, int gap)
{
    struct viewport *vp = *display->current_viewport;
    const int old_font = vp->font;
    vp->font = font;
    display->set_drawmode(DRMODE_FG);
    display->set_foreground(fg);
    const int pitch = font_get(font)->height;
    for (int i = 0; i < n; i++)
    {
        for (int l = 0; l < ps[i].n; l++)
        {
            char buf[160];
            const int len = MIN(ps[i].len[l], (int)sizeof(buf) - 1);
            memcpy(buf, ps[i].at[l], len);
            buf[len] = '\0';
            display->putsxy(cx - ps[i].w[l] / 2, y, (const unsigned char *)buf);
            y += pitch;
        }
        y += gap;
    }
    display->set_drawmode(DRMODE_SOLID);
    vp->font = old_font;
}

/* Now Playing's tile: the problem and what to do, in its place */
static void draw_tile(struct screen *display, const struct look *lk,
                      const struct viewport *tile, int reason)
{
    int x = tile->x, y = tile->y, s = MIN(tile->width, tile->height);
    const int font = lines_font(lk->np_font, reason);
    const unsigned fg = lk->np_fg[global_settings.dark_theme];
    /* Classic's tile made again, for the lines' contrast; the others' note
       painted out, the rows its reflection is made of kept */
    if (lk->classic)
    {
        x = lk->tile_x;
        y = lk->tile_y;
        s = lk->tile_s;
        classic_tile(x, y, s, s);
    }
    else
    {
        short cols[4];
        columns(x + 4, cols);
        clear_rows(x + 4, y + 6, s - 8, s - 6 - 24, cols);
    }
    const int margin = 8, width = s - 2 * margin;
    struct para ps[2];
    wrap(&ps[0], str(cover_caution_problem(reason)), font, width);
    wrap(&ps[1], str(cover_caution_action(reason)), font, width);
    const int h = paras_height(ps, 2, font, 6);
    const int top = y + (s - h) / 2;
    draw_paras(display, ps, 2, fg, font, x + s / 2, top, 6);
}

/* the full view: the large mark over the two lines, in its note's place */
static void draw_big(struct screen *display, const struct look *lk,
                     const struct viewport *big, int reason)
{
    const int x = big->x, y = big->y, s = MIN(big->width, big->height);
    short cols[4];
    columns(x + 3, cols);
    clear_rows(x + 6, y + 6, s - 12, s - 12, cols);
    const int font = lines_font(lk->np_font, reason);
    const unsigned fg = lk->np_fg[global_settings.dark_theme];
    struct para ps[2];
    wrap(&ps[0], str(cover_caution_problem(reason)), font, s - 16);
    wrap(&ps[1], str(cover_caution_action(reason)), font, s - 16);
    const int gap = 12;
    const int h = lk->large.h + gap + paras_height(ps, 2, font, 4);
    const int top = y + (s - h) / 2;
    draw_mark(lk, &lk->large, x + (s - lk->large.w) / 2, top);
    draw_paras(display, ps, 2, fg, font, x + s / 2, top + lk->large.h + gap, 4);
}

/* the menus' pane: the mark where the note is and the two lines under it, in
   place of the song's name; the whole in the pane's middle if that's lower */
static void draw_pane(struct screen *display, const struct look *lk,
                      int reason)
{
    const int font = lines_font(lk->pane_font, reason);
    const unsigned fg = lk->pane_fg[global_settings.dark_theme];
    const int x = lk->pane_x, w = LCD_WIDTH - x;
    /* right of the pane's edge; of a recess (UI_METRIC_PANE_SHADE), right of
       its wall's two columns of shadow, which are the backdrop's here, and
       from its backdrop, its floor: columns of the screen's could have the
       text under the grille in them, which was copied along a row */
    const bool recess = ui_metric(UI_METRIC_PANE_SHADE) > 0;
    const int from = x + 1 + recess;
    const fb_data *backdrop = recess ? lcd_get_backdrop() : NULL;
    if (backdrop)
        display->bitmap_part(backdrop, from, 24, STRIDE(SCREEN_MAIN, LCD_WIDTH, LCD_HEIGHT),
                             from, 24, LCD_WIDTH - 2 - from, LCD_HEIGHT - 24);
    else
        clear_rows(from, 24, LCD_WIDTH - 2 - from, LCD_HEIGHT - 24, lk->pane_cols);
    struct para ps[2];
    wrap(&ps[0], str(cover_caution_problem(reason)), font, lk->pane_w);
    wrap(&ps[1], str(cover_caution_action(reason)), font, lk->pane_w);
    const int gap = 14;
    const int h = lk->pane.h + gap + paras_height(ps, 2, font, 4);
    int top = lk->pane_note_y;
    if (top + h > LCD_HEIGHT - 10)
        top = 20 + (LCD_HEIGHT - 20 - h) / 2;
    const int cx = x + w / 2;
    draw_mark(lk, &lk->pane, cx - lk->pane.w / 2, top);
    draw_paras(display, ps, 2, fg, font, cx, top + lk->pane.h + gap, 4);
    /* the list's shadow over what was drawn here anew (menu_cover.h) */
    menu_cover_pane_shadow(from, 24, LCD_HEIGHT);
}

/* a viewport of the skin, if it's drawn now */
static struct viewport *shown_vp(struct wps_data *data, const char *label)
{
    struct skin_viewport *sv = label ? skin_find_item(label, SKIN_FIND_VP, data)
                                     : NULL;
    return sv && !(sv->hidden_flags & (VP_DRAW_HIDDEN | VP_NEVER_VISIBLE))
           ? &sv->vp : NULL;
}

/* The skin's lines from x rightwards and from y down not scrolled: the
   scroll engine draws a long title again on its own timer, over the lines
   painted there. Each render scrolls it anew, and this follows each. */
static void stop_scrolls(struct screen *display, struct wps_data *data,
                         int x, int y)
{
    char *buffer = get_skin_buffer(data);
    for (struct skin_element *e = SKINOFFSETTOPTR(buffer, data->tree); e;
         e = SKINOFFSETTOPTR(buffer, e->next))
    {
        struct skin_viewport *sv = SKINOFFSETTOPTR(buffer, e->data);
        if (sv && sv->vp.x >= x && sv->vp.y + sv->vp.height > y)
            display->scroll_stop_viewport(&sv->vp);
    }
}

/* the theme's own skins, for which the places are known; another's shows
   the note. The places are the iPod Video's skins': the iPod photo's, of the
   same names, show the note */
static bool skins_own(void)
{
#if (LCD_WIDTH == 320) && (LCD_HEIGHT == 240)
    const struct ui_theme *theme = ui_theme_current();
    for (int dark = 0; dark < 2; dark++)
    {
        const char *skin = theme->look[dark].skin;
        if (skin && !strcmp((const char *)global_settings.wps_file, skin)
            && !strcmp((const char *)global_settings.sbs_file, skin))
            return true;
    }
#endif
    return false;
}

/* the whole screen's viewport, with the framebuffer it defaults to; its x as
   given, which a right-to-left language's alignment mirrored */
static struct viewport *screen_viewport(void)
{
    static struct viewport screen_vp;
    memset(&screen_vp, 0, sizeof(screen_vp));
    viewport_set_fullscreen(&screen_vp, SCREEN_MAIN);
    screen_vp.flags &= ~VP_FLAG_ALIGNMENT_MASK;
    return &screen_vp;
}

/* The pane's caution drawn in the frame before, and its place from x: gone
   since, it's cleared to the skin's backdrop or ground before the skin is
   drawn whole, as the skin draws nothing over some of it (Classic's photos
   left the lines' foot showing) */
static bool pane_drawn, pane_restore;
static int pane_drawn_x;

void cover_caution_before(struct gui_wps *gwps, unsigned refresh_mode)
{
    if ((!pane_drawn && !pane_restore)
        || gwps->display->screen_type != SCREEN_MAIN
        || gwps->data != skin_get_gwps(CUSTOM_STATUSBAR, SCREEN_MAIN)->data
        || (refresh_mode & SKIN_REFRESH_ALL) != SKIN_REFRESH_ALL)
        return;
    struct screen *display = gwps->display;
    struct viewport *old = display->set_viewport(screen_viewport());
    display->set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
    display->fillrect(pane_drawn_x, 24, LCD_WIDTH - pane_drawn_x,
                      LCD_HEIGHT - 24);
    display->set_drawmode(DRMODE_SOLID);
    display->set_viewport(old);
    pane_restore = false;
}

/* the caution drawn where it goes, if anywhere: true for the menus' pane */
static bool caution_draw(struct gui_wps *gwps)
{
    struct wps_data *data = gwps->data;
    const bool now_playing = data == skin_get_gwps(WPS, SCREEN_MAIN)->data;
    const int reason = now_playing ? playback_current_cover_reason()
                                   : menu_cover_reason();
    if (!cover_caution_on() || !cover_caution_problem(reason) || !skins_own())
        return false;
    const struct look *lk = look();
    struct screen *display = gwps->display;
    bool pane = false;
    struct viewport *old = display->set_viewport(screen_viewport());
    if (now_playing)
    {
        const int mode = get_wps_state()->display_mode;
        const struct viewport *big = shown_vp(data, lk->big_vp);
        const struct viewport *tile = shown_vp(data, lk->tile_vp);
        if (mode == WPS_MODE_ALBUMART && big)
            draw_big(display, lk, big, reason);
        else if (tile && mode != WPS_MODE_LYRICS)
            draw_tile(display, lk, tile, reason);
    }
    else if (shown_vp(data, lk->pane_vp[0]) || shown_vp(data, lk->pane_vp[1]))
    {
        draw_pane(display, lk, reason);
        stop_scrolls(display, data, lk->pane_x, 24);
        pane_drawn_x = lk->pane_x;
        pane = true;
    }
    display->set_viewport(old);
    return pane;
}

void cover_caution_render(struct gui_wps *gwps)
{
    struct wps_data *data = gwps->data;
    const bool sbs = data == skin_get_gwps(CUSTOM_STATUSBAR, SCREEN_MAIN)->data;
    if (gwps->display->screen_type != SCREEN_MAIN
        || (!sbs && data != skin_get_gwps(WPS, SCREEN_MAIN)->data))
        return;
    const bool pane = caution_draw(gwps);
    if (!sbs)
        return;
    /* gone: the skin drawn whole again, its place cleared first */
    if (pane_drawn && !pane)
    {
        pane_restore = true;
        skin_request_full_update(CUSTOM_STATUSBAR);
        sb_skin_force_next_update();
    }
    pane_drawn = pane;
}

void cover_caution_tile(struct screen *display, int x, int y, int size,
                        int h, bool header)
{
    const struct look *lk = look();
    const struct mark *m = header ? &lk->header : &lk->row;
    const unsigned fg = display->get_foreground();
    display->set_foreground(ui_colour(UI_COLOUR_COVER_TILE));
    display->set_drawmode(DRMODE_SOLID);
    display->fillrect(x, y, size, h);
    if (display->screen_type == SCREEN_MAIN)
        line_draw_note_rows(m->ink, m->w, m->w, MIN(m->h, h), x + (size - m->w) / 2,
                            y + MAX(h - m->h, 0) / 2);
    display->set_foreground(fg);
    display->set_drawmode(DRMODE_SOLID);
}
#else
bool cover_caution_on(void)
{
    return false;
}
int cover_caution_problem(int reason)
{
    (void)reason;
    return 0;
}
int cover_caution_action(int reason)
{
    (void)reason;
    return 0;
}
void cover_caution_render(struct gui_wps *gwps)
{
    (void)gwps;
}
void cover_caution_before(struct gui_wps *gwps, unsigned refresh_mode)
{
    (void)gwps; (void)refresh_mode;
}
void cover_caution_tile(struct screen *display, int x, int y, int size,
                        int h, bool header)
{
    (void)display; (void)x; (void)y; (void)size; (void)h; (void)header;
}
#endif
