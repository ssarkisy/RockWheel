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
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "button.h"
#include "action.h"
#include "settings.h"
#include "lang.h"
#include "language.h"
#include "misc.h"
#include "string-extra.h"
#include "sound.h"
#include "audio.h"
#include "pcmbuf.h"
#include "backlight.h"
#include "button.h"
#include "appevents.h"
#include "screen_access.h"
#include "viewport.h"
#include "statusbar-skinned.h"
#include "line.h"
#include "ui_theme.h"
#include "transition.h"
#include "wps.h"
#include "menu.h"
#include "root_menu.h"
#include "rbpaths.h"
#include "wheel_policy.h"
#include "eq.h"
#include "eq_curve.h"
#include "menus/eq_menu.h"
#include "eq_editor.h"

/* The layout of the approved mockups (mockups-r1, report §1), in the screen's
   pixels. The plot spans 20 Hz to 20 kHz across its columns and +-12 dB down
   its rows: half a dB, a step of the wheel, is 3 rows on the Video and 2 on the
   iPod photo, so that every step moves the curve as far */
#ifdef UI_220X176
#define PLOT_X0  7
#define PLOT_X1  212
#define PX_DB    4
#define PLOT_DY  3      /* from the top of the list's place */
#define CHIP_PAD 3
#else
#define PLOT_X0  10
#define PLOT_X1  309
#define PX_DB    6
#define PLOT_DY  2
#define CHIP_PAD 5
#endif
#define RANGE_DB 12
#define PLOT_H   (2 * RANGE_DB * PX_DB + 1)
#define GAIN_MAX (RANGE_DB * 10)        /* tenths of a dB */
#define GAIN_STEP 5

enum { FIELD_FREQ, FIELD_GAIN, FIELD_Q, FIELD_TYPE, FIELDS };

/* Steps of the wheel, a click each, as even as the DSP lets them be. A value off
   these, as a file may have, stays until it's changed, and then goes to the next
   one of them that way. Frequency: a tenth of an octave from 1 kHz, 3 columns on
   the Video's plot, 21 Hz to 19.7 kHz */
static const short freq_steps[] = {
       21,    22,    24,    25,    27,    29,    31,    33,    36,    38,
       41,    44,    47,    51,    54,    58,    62,    67,    72,    77,
       82,    88,    95,   102,   109,   117,   125,   134,   144,   154,
      165,   177,   189,   203,   218,   233,   250,   268,   287,   308,
      330,   354,   379,   406,   435,   467,   500,   536,   574,   616,
      660,   707,   758,   812,   871,   933,  1000,  1072,  1149,  1231,
     1320,  1414,  1516,  1625,  1741,  1866,  2000,  2144,  2297,  2462,
     2639,  2828,  3031,  3249,  3482,  3732,  4000,  4287,  4595,  4925,
     5278,  5657,  6063,  6498,  6964,  7464,  8000,  8574,  9190,  9849,
    10556, 11314, 12126, 12996, 13929, 14929, 16000, 17148, 18379, 19698,
};
/* Q, in the tenths the DSP keeps it in: x1.19, a quarter of a doubling, down
   from 6.4, each step at least a tenth, so that the lowest are 0.1 apart; 0.7
   and 1.0, the default bands', among them */
static const unsigned char q_steps[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 13, 16, 19, 23, 27, 32, 38, 45, 54, 64,
};

/* the ten bands' own frequencies, the Simple view's grid and labels */
static const short band_freq[EQ_NUM_BANDS] = {
    32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000,
};
static const char *const band_label[EQ_NUM_BANDS] = {
    "32", "64", "125", "250", "500", "1k", "2k", "4k", "8k", "16k",
};
/* a band's view's grid by decades, labelled at 100, 1k and 10k */
static const short decade_freq[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000 };

static struct eq_curve curve;
static int sel;            /* the band chosen, as it was left */
static bool band_view;
static int field = FIELD_GAIN;
static bool comparing;
static bool boosted;
static struct viewport ui_vp;

/** colours: the mockups' rule, of the theme's as they are now **/

/* x / 255 for 0 <= x < 65535, without a division */
static int div255(int x)
{
    return (x + 1 + (x >> 8)) >> 8;
}

/* The screen's colour nearest to one of 8 bits a part: LCD_RGBPACK drops the
   low bits, which made the plot's pale steps a shade off the mockups' */
static unsigned rgb_near(int r, int g, int b)
{
    const int r5 = div255(r * 31 + 127), g6 = div255(g * 63 + 127),
              b5 = div255(b * 31 + 127);
    return LCD_RGBPACK((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4),
                       (b5 << 3) | (b5 >> 2));
}

/* a part moved pct percent of the way from x to y, to the nearest */
static int mix_part(int x, int y, int pct)
{
    const int d = (y - x) * pct;
    return x + (d >= 0 ? (d + 50) / 100 : -((50 - d) / 100));
}

/* a moved pct percent of the way to b */
static unsigned mix(unsigned a, unsigned b, int pct)
{
    return rgb_near(mix_part(RGB_UNPACK_RED(a), RGB_UNPACK_RED(b), pct),
                    mix_part(RGB_UNPACK_GREEN(a), RGB_UNPACK_GREEN(b), pct),
                    mix_part(RGB_UNPACK_BLUE(a), RGB_UNPACK_BLUE(b), pct));
}

static struct {
    unsigned ground, text, dim, accent, line, grid, zero, column, fill, band, off;
} col;

/* The accent is the theme's colour for what is chosen: its selection's, at the
   gradient's darker end in Classic (the user's Classic Colour, or its blue), the
   tone's in Neo, Modern's red; Dieter's signal colour, which its selected row's
   name has on a grey bar. The plot's surfaces are steps from the ground to it,
   its lines steps from the ground to black, or white in a dark look; the curve
   in the dark is the gradient's lighter end, or the accent nearer white */
static void make_colours(void)
{
    const bool dark = global_settings.dark_theme;
    const bool signal = ui_metric(UI_METRIC_SELECTION_SIGNAL);
    const unsigned start = ui_colour(UI_COLOUR_SELECTOR_START);
    col.ground = ui_colour(UI_COLOUR_GROUND);
    col.text = ui_colour(UI_COLOUR_TEXT);
    col.dim = ui_colour(UI_COLOUR_VALUE);
    col.accent = signal ? ui_colour(UI_COLOUR_SIGNAL) : ui_colour(UI_COLOUR_SELECTOR_END);
    col.off = mix(col.ground, col.dim, 55);
    if (dark)
    {
        col.grid = mix(col.ground, LCD_WHITE, 7);
        col.zero = mix(col.ground, LCD_WHITE, 22);
        col.column = mix(col.ground, col.accent, 22);
        col.fill = mix(col.ground, col.accent, 38);
        col.band = mix(col.ground, col.accent, 62);
        col.line = !signal && start != col.accent ? start : mix(col.accent, LCD_WHITE, 25);
    }
    else
    {
        col.grid = mix(col.ground, LCD_BLACK, 6);
        col.zero = mix(col.ground, LCD_BLACK, 22);
        col.column = mix(col.ground, col.accent, 10);
        col.fill = mix(col.ground, col.accent, 20);
        col.band = mix(col.ground, col.accent, 40);
        col.line = col.accent;
    }
}

/** pixels, in the screen's coordinates **/

static int plot_top, plot_zero;

static void put(int x, int y, unsigned c)
{
    *FBADDR(x, y) = FB_SCALARPACK(c);
}

/* c over the pixel, a of 256 of it */
static void blend(int x, int y, unsigned c, int a)
{
    if (a <= 0)
        return;
    if (a >= 256)
    {
        put(x, y, c);
        return;
    }
    fb_data *p = FBADDR(x, y);
    const unsigned under = FB_UNPACK_SCALAR_LCD(*p);
    const int r = RGB_UNPACK_RED(under), g = RGB_UNPACK_GREEN(under),
              b = RGB_UNPACK_BLUE(under);
    *p = FB_SCALARPACK(rgb_near(r + ((((int)RGB_UNPACK_RED(c) - r) * a + 128) >> 8),
                                g + ((((int)RGB_UNPACK_GREEN(c) - g) * a + 128) >> 8),
                                b + ((((int)RGB_UNPACK_BLUE(c) - b) * a + 128) >> 8)));
}

static void fill(int x, int y, int w, int h, unsigned c)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            put(xx, yy, c);
}

/* The rows y16a to y16b, in sixteenths, of a column: each pixel by how much of
   it they cover */
static void column_span(int x, int a16, int b16, unsigned c)
{
    if (a16 > b16)
    {
        const int t = a16;
        a16 = b16;
        b16 = t;
    }
    for (int y = a16 >> 4; y <= (b16 + 15) >> 4; y++)
    {
        const int cover = MIN((y + 1) << 4, b16) - MAX(y << 4, a16);
        blend(x, y, c, cover << 4);
    }
}

/* A disc (or a ring of width w16) round (cx16, cy16), radius r16, all in
   sixteenths: each pixel by how many of 4x4 points in it are inside */
static void disc(int cx16, int cy16, int r16, int w16, unsigned c)
{
    const int r2 = r16 * r16, in = r16 - w16, in2 = w16 ? in * in : -1;
    for (int y = (cy16 - r16) >> 4; y <= (cy16 + r16) >> 4; y++)
    {
        for (int x = (cx16 - r16) >> 4; x <= (cx16 + r16) >> 4; x++)
        {
            int n = 0;
            for (int k = 0; k < 16; k++)
            {
                const int dx = (x << 4) + 4 * (k & 3) + 2 - cx16;
                const int dy = (y << 4) + 4 * (k >> 2) + 2 - cy16;
                const int d = dx * dx + dy * dy;
                n += d <= r2 && d > in2;
            }
            blend(x, y, c, n << 4);
        }
    }
}

/** the plot **/

/* where a frequency is across the plot, in sixteenths of a column */
static int freq_x16(int hz)
{
    return PLOT_X0 * 16 + (int)((int64_t)(PLOT_X1 - PLOT_X0) * 16
                                * eq_curve_position(hz) >> 16);
}

/* the point of the curve a column shows: the Video's are the points; the
   photo's, fewer, the nearest */
static int column_point(int x)
{
    return ((x - PLOT_X0) * (EQ_CURVE_POINTS - 1) + (PLOT_X1 - PLOT_X0) / 2)
           / (PLOT_X1 - PLOT_X0);
}

/* a response in hundredths of a dB as a row in sixteenths, within the plot */
static int db_y16(int cdb)
{
    const int y16 = plot_zero * 16 - cdb * PX_DB * 16 / 100;
    return MAX(plot_top * 16, MIN(y16, (plot_top + PLOT_H - 1) * 16));
}

/* A line through one row a column, its pixels as much of them as a pen of
   w16 sixteenths' height covers sweeping from each column's half-way points to
   its neighbours' */
static void curve_line(const short *y16, int w16, unsigned c)
{
    const int n = PLOT_X1 - PLOT_X0 + 1;
    for (int i = 0; i < n; i++)
    {
        int lo = y16[i] - w16 / 2, hi = y16[i] + w16 / 2;
        if (i > 0)
        {
            lo = MIN(lo, (y16[i] + y16[i - 1]) / 2 - w16 / 2);
            hi = MAX(hi, (y16[i] + y16[i - 1]) / 2 + w16 / 2);
        }
        if (i < n - 1)
        {
            lo = MIN(lo, (y16[i] + y16[i + 1]) / 2 - w16 / 2);
            hi = MAX(hi, (y16[i] + y16[i + 1]) / 2 + w16 / 2);
        }
        lo = MAX(lo, plot_top * 16);
        hi = MIN(hi, (plot_top + PLOT_H) * 16);
        column_span(PLOT_X0 + i, lo, hi, c);
    }
}

static short ys[PLOT_X1 - PLOT_X0 + 1];

static void draw_plot(void)
{
    const struct eq_band_setting *bands = global_settings.eq_custom;
    const int n = PLOT_X1 - PLOT_X0 + 1;
    /* the chosen band's column, an octave less 2 px each side */
    if (!band_view && !comparing)
    {
        const int octave16 = (PLOT_X1 - PLOT_X0) * 16 * 6576 >> 16;
        const int half16 = octave16 / 2 - 32;
        const int cx16 = freq_x16(bands[sel].cutoff);
        /* within the plot, as a band at its ends would have it past them */
        const int start = (cx16 - half16 + 8) >> 4;
        const int left = MAX(start, PLOT_X0);
        const int right = MIN(start + ((2 * half16 + 8) >> 4), PLOT_X1);
        if (right >= left)
            fill(left, plot_top, right - left + 1, PLOT_H, col.column);
    }
    /* the grid */
    if (band_view)
    {
        for (int i = 0; i < (int)ARRAYLEN(decade_freq); i++)
            fill((freq_x16(decade_freq[i]) + 8) >> 4, plot_top, 1, PLOT_H, col.grid);
    }
    else
    {
        for (int i = 0; i < EQ_NUM_BANDS; i++)
            fill((freq_x16(band_freq[i]) + 8) >> 4, plot_top, 1, PLOT_H, col.grid);
    }
    for (int db = -RANGE_DB; db <= RANGE_DB; db += RANGE_DB / 2)
    {
        if (db)
            fill(PLOT_X0, plot_zero - db * PX_DB, n, 1, col.grid);
    }
    for (int i = 0; i < n; i++)
        ys[i] = db_y16(eq_curve_total(&curve, column_point(PLOT_X0 + i)));
    if (!comparing)
    {
        /* between the curve and 0 dB, and the band's own share darker */
        for (int i = 0; i < n; i++)
            column_span(PLOT_X0 + i, ys[i], plot_zero * 16, col.fill);
        if (band_view)
        {
            for (int i = 0; i < n; i++)
                column_span(PLOT_X0 + i,
                            db_y16(curve.cdb[sel][column_point(PLOT_X0 + i)]),
                            plot_zero * 16, col.band);
        }
    }
    fill(PLOT_X0, plot_zero, n, 1, col.zero);
    if (comparing)
    {
        /* what is heard now, flat, over the EQ's curve gone grey */
        curve_line(ys, 26, col.off);
        for (int i = 0; i < n; i++)
            ys[i] = plot_zero * 16;
        curve_line(ys, 32, col.line);
    }
    else
        curve_line(ys, 32, col.line);
    /* each band at its own frequency and gain, the chosen one larger; in a
       band's view those at 0 dB rings */
    for (int i = 0; i < EQ_NUM_BANDS; i++)
    {
        const int cx16 = freq_x16(bands[i].cutoff);
        const int y = MAX(plot_top + 3, MIN(plot_zero - bands[i].gain * PX_DB / 10,
                                            plot_top + PLOT_H - 4));
        const int cy16 = y * 16 + 8;
        if (comparing)
            disc(cx16, cy16, 38, 0, col.off);
        else if (i == sel)
        {
            disc(cx16, cy16, 99, 0, col.ground);
            disc(cx16, cy16, 77, 0, col.line);
        }
        else if (band_view && bands[i].gain == 0)
        {
            disc(cx16, cy16, 58, 0, col.ground);
            disc(cx16, cy16, 48, 21, col.dim);
        }
        else
        {
            disc(cx16, cy16, 61, 0, col.ground);
            disc(cx16, cy16, 42, 0, col.line);
        }
    }
}

/** texts **/

static struct {
    int axis, label, value, field_label, field_value;
} fonts;
static int axis_y, label_y, value_y, field_label_y, field_value_y;

static int font_of(enum ui_font role)
{
    const int font = font_find(ui_font_file(role));
    return font >= 0 ? font : screens[SCREEN_MAIN].getuifont();
}

static void make_layout(void)
{
    plot_top = ui_vp.y + PLOT_DY;
    plot_zero = plot_top + RANGE_DB * PX_DB;
    const int bottom = plot_top + PLOT_H - 1;
    fonts.label = font_of(UI_FONT_LEAD);
    fonts.value = font_of(UI_FONT_LIST);
#ifdef UI_220X176
    /* the frequencies 2 px apart on 206 columns: in 12 px where a skin has
       loaded it, as Classic's Now Playing does; else the theme's own, every
       other one where they'd touch (draw_axis()) */
    const int small = font_find(FONT_DIR "/12-Inter-SemiBold.fnt");
    fonts.axis = small >= 0 ? small : font_of(UI_FONT_SUBLINE);
    fonts.field_label = fonts.label;
    fonts.field_value = font_of(global_settings.ui_theme == UI_THEME_CLASSIC
                                ? UI_FONT_HEADER_COUNTS : UI_FONT_MENU_VALUE);
    const int axis_h = font_get(fonts.axis)->height;
    axis_y = bottom + 4;
    label_y = axis_y + axis_h + 3;
    value_y = label_y + 14;
    /* the names in the lists' small font, which keeps their letters, rather
       than the 12 px one, whose 32 glyphs a skin's text shares: on the same
       line as the mockups' 12 px names */
    field_value_y = axis_y + axis_h + 20;
    field_label_y = field_value_y - 5 - font_get(fonts.field_label)->ascent;
#else
    const bool classic = global_settings.ui_theme == UI_THEME_CLASSIC;
    fonts.axis = font_of(UI_FONT_SUBLINE);
    fonts.field_label = fonts.label;
    fonts.field_value = fonts.value;
    axis_y = bottom + (classic ? 6 : 5);
    label_y = axis_y + font_get(fonts.axis)->height + (classic ? 3 : 4);
    value_y = label_y + 17;
    field_label_y = label_y;
    field_value_y = value_y;
#endif
}

static int text_width(int font, const char *text)
{
    int w, h;
    font_getstringsize((const unsigned char *)text, &w, &h, font);
    return w;
}

/* in the screen's coordinates, the text's cell from x, y */
static void text(int font, int x, int y, const char *s, unsigned c)
{
    struct screen *display = &screens[SCREEN_MAIN];
    ui_vp.font = font;
    display->set_drawmode(DRMODE_FG);
    display->set_foreground(c);
    display->putsxy(x - ui_vp.x, y - ui_vp.y, s);
}

static void text_centred(int font, int cx, int y, const char *s, unsigned c)
{
    text(font, cx - text_width(font, s) / 2, y, s, c);
}

/* "64 Hz", "2.3 kHz", "10.5 kHz": three digits at most, the noughts at the
   end of their fraction left out */
static const char *format_freq(char *buf, size_t len, int hz)
{
    if (hz < 1000)
    {
        snprintf(buf, len, "%d Hz", hz);
        return buf;
    }
    const int div = hz < 10000 ? 100 : 10;  /* the kHz as a whole number of these */
    int n = (hz * div + 500) / 1000;
    int whole = n / div, frac = n % div;
    int digits = div == 100 ? 2 : 1;
    while (digits > 0 && frac % 10 == 0)
    {
        frac /= 10;
        digits--;
    }
    if (digits)
        snprintf(buf, len, digits == 2 ? "%d%s%02d kHz" : "%d%s%d kHz", whole,
                 str(LANG_POINT), frac);
    else
        snprintf(buf, len, "%d kHz", whole);
    return buf;
}

/* "+3.5 dB", "−12.0 dB", with the minus sign rather than a hyphen */
static const char *format_gain(char *buf, size_t len, int tenths)
{
    const int a = tenths < 0 ? -tenths : tenths;
    snprintf(buf, len, "%s%d%s%d dB", tenths > 0 ? "+" : tenths < 0 ? "\xe2\x88\x92" : "",
             a / 10, str(LANG_POINT), a % 10);
    return buf;
}

static const char *format_q(char *buf, size_t len, int q)
{
    snprintf(buf, len, "%d%s%d", q / 10, str(LANG_POINT), q % 10);
    return buf;
}

static int type_lang(int type)
{
    return type == EQ_FILTER_LOW_SHELF ? LANG_EQ_LOW_SHELF
         : type == EQ_FILTER_HIGH_SHELF ? LANG_EQ_HIGH_SHELF : LANG_EQ_PEAK;
}

static void draw_axis(void)
{
    if (band_view)
    {
        static const short at[] = { 100, 1000, 10000 };
        static const char *const names[] = { "100", "1k", "10k" };
        for (int i = 0; i < 3; i++)
            text_centred(fonts.axis, (freq_x16(at[i]) + 8) >> 4, axis_y, names[i],
                         col.dim);
        return;
    }
    /* every other one where neighbours would touch, from the first: the
       chosen one's column shows where it is */
    const int chosen = comparing ? -1 : sel;
    bool thin = false;
    for (int i = 1; i < EQ_NUM_BANDS && !thin; i++)
    {
        const int room = (freq_x16(band_freq[i]) - freq_x16(band_freq[i - 1])) >> 4;
        thin = (text_width(fonts.axis, band_label[i - 1])
                + text_width(fonts.axis, band_label[i])) / 2 + 2 > room;
    }
    for (int i = 0; i < EQ_NUM_BANDS; i += thin ? 2 : 1)
        text_centred(fonts.axis, (freq_x16(band_freq[i]) + 8) >> 4, axis_y,
                     band_label[i], i == chosen ? col.text : col.dim);
}

/* the field the wheel changes, under the theme's selected row's bar: Classic's
   gradient, a rounded one where the theme's selection is rounded; the colour
   of the text on it */
static unsigned draw_chip(int x, int y, int w, int h)
{
    struct screen *display = &screens[SCREEN_MAIN];
    x -= ui_vp.x;
    y -= ui_vp.y;
    unsigned ink;
    if (ui_metric(UI_METRIC_SELECTION_RADIUS) > 0)
    {
        line_fill_rounded(display, x, y, w, h,
                          MIN(ui_metric(UI_METRIC_SELECTION_RADIUS), CHIP_PAD),
                          ui_colour(UI_COLOUR_SELECTOR_START));
        ink = ui_colour(UI_COLOUR_SELECTOR_TEXT);
    }
    else
        ink = line_selection_bar(display, x, y, w, h);
    return ui_metric(UI_METRIC_SELECTION_SIGNAL) ? ui_colour(UI_COLOUR_SIGNAL) : ink;
}

static void draw_fields(void)
{
    const struct eq_band_setting *b = &global_settings.eq_custom[sel];
    char text_vals[FIELD_TYPE][24], wide[24];
    const char *names[FIELDS] = {
        str(LANG_EQ_FREQUENCY), str(LANG_EQ_GAIN), str(LANG_EQ_Q),
        str(LANG_EQ_TYPE),
    };
    /* the type's name as the language has it, whatever its length */
    const char *vals[FIELDS] = {
        format_freq(text_vals[FIELD_FREQ], sizeof(text_vals[0]), b->cutoff),
        format_gain(text_vals[FIELD_GAIN], sizeof(text_vals[0]), b->gain),
        format_q(text_vals[FIELD_Q], sizeof(text_vals[0]), b->q),
        str(type_lang(b->type)),
    };
    /* four columns at fixed places, each as wide as its name or its widest
       value, so that none moves as a value changes; the room left between them */
    int widths[FIELDS], sum = 0;
    for (int i = 0; i < FIELDS; i++)
        widths[i] = text_width(fonts.field_label, names[i]);
    static const short freqs[] = { 888, 8880, 18800 };
    for (int i = 0; i < (int)ARRAYLEN(freqs); i++)
        widths[FIELD_FREQ] = MAX(widths[FIELD_FREQ], text_width(fonts.field_value,
                                 format_freq(wide, sizeof(wide), freqs[i])));
    widths[FIELD_GAIN] = MAX(widths[FIELD_GAIN], text_width(fonts.field_value,
                             format_gain(wide, sizeof(wide), -GAIN_MAX)));
    widths[FIELD_GAIN] = MAX(widths[FIELD_GAIN], text_width(fonts.field_value,
                             format_gain(wide, sizeof(wide), -88)));
    static const unsigned char qs[] = { 64, 8, 45 };
    for (int i = 0; i < (int)ARRAYLEN(qs); i++)
        widths[FIELD_Q] = MAX(widths[FIELD_Q], text_width(fonts.field_value,
                              format_q(wide, sizeof(wide), qs[i])));
    for (int t = EQ_FILTER_LOW_SHELF; t <= EQ_FILTER_HIGH_SHELF; t++)
        widths[FIELD_TYPE] = MAX(widths[FIELD_TYPE],
                                 text_width(fonts.field_value, str(type_lang(t))));
    for (int i = 0; i < FIELDS; i++)
        sum += widths[i];
    const int gap = MAX(0, (PLOT_X1 + 1 - PLOT_X0 - sum - 2 * CHIP_PAD) / 3);
    const int value_h = font_get(fonts.field_value)->height;
    int x = PLOT_X0;
    for (int i = 0; i < FIELDS; i++)
    {
        unsigned ink = col.text, label = col.dim;
        if (i == field)
        {
            ink = draw_chip(x - CHIP_PAD, field_value_y - 2,
                            text_width(fonts.field_value, vals[i]) + 2 * CHIP_PAD,
                            value_h + 3);
            label = col.text;
        }
        text(fonts.field_value, x, field_value_y, vals[i], ink);
        text(fonts.field_label, x, field_label_y, names[i], label);
        x += widths[i] + gap + (i == 0 ? 2 * CHIP_PAD : 0);
    }
}

static void draw(void)
{
    struct screen *display = &screens[SCREEN_MAIN];
    make_colours();
    make_layout();
    sb_set_title_text(str(LANG_EQ_CUSTOM), Icon_NOICON, SCREEN_MAIN);
    struct viewport *last = display->set_viewport(&ui_vp);
    display->clear_viewport();
    draw_plot();
    draw_axis();
    if (comparing)
    {
        text(fonts.label, PLOT_X0, label_y, str(LANG_EQ_COMPARE), col.dim);
        text(fonts.value, PLOT_X0, value_y, str(LANG_EQ_COMPARE_OFF), col.text);
    }
    else if (band_view)
        draw_fields();
    else
    {
        char buf[24];
        const struct eq_band_setting *b = &global_settings.eq_custom[sel];
        text(fonts.label, PLOT_X0, label_y, format_freq(buf, sizeof(buf), b->cutoff),
             col.dim);
        text(fonts.value, PLOT_X0, value_y, format_gain(buf, sizeof(buf), b->gain),
             col.text);
    }
    display->set_drawmode(DRMODE_SOLID);
    display->update_viewport();
    display->set_viewport(last);
}

/** changes **/

/* the next step of a value from v, up or down, on steps[] */
static int next_step(int v, int dir, const short *steps, int n)
{
    if (dir > 0)
    {
        for (int i = 0; i < n; i++)
            if (steps[i] > v)
                return steps[i];
    }
    else
    {
        for (int i = n - 1; i >= 0; i--)
            if (steps[i] < v)
                return steps[i];
    }
    return v;
}

static int next_q(int q, int dir)
{
    short steps[ARRAYLEN(q_steps)];
    for (int i = 0; i < (int)ARRAYLEN(q_steps); i++)
        steps[i] = q_steps[i];
    return next_step(q, dir, steps, ARRAYLEN(steps));
}

/* the next half dB, into the range: one past it, from a file, steps in to it */
static int next_gain(int g, int dir)
{
    int to = dir > 0 ? (g >= 0 ? g / GAIN_STEP + 1 : -((-g - 1) / GAIN_STEP)) * GAIN_STEP
                     : (g > 0 ? (g - 1) / GAIN_STEP : -(-g / GAIN_STEP + 1)) * GAIN_STEP;
    if (dir > 0 && g >= GAIN_MAX)
        return g;
    if (dir < 0 && g <= -GAIN_MAX)
        return g;
    return MAX(-GAIN_MAX, MIN(to, GAIN_MAX));
}

/* A precut changed is in what's decoded from now on, which is heard after what's
   decoded already, and its makeup changes the volume as soon as it's set: so
   that the music doesn't step a dB up or down for as long as that plays, each
   makeup waits here for it, in order, put on the volume once what was decoded
   before it has been heard (pcmbuf_until_heard()), paused or not. More than
   there's room for, as while seconds decoded before the editor opened still
   play, take the last one's place: the level is then off only between those
   two changes. They go on waiting after the editor closes: then a timeout
   wakes whatever screen is shown (BUTTON_NONE, as do_sbs_update_callback()
   does: get_action() gives ACTION_NONE, nothing redrawn nor taken as a press),
   whose next get_action() puts them on (GUI_EVENT_ACTIONUPDATE), in its
   thread, as the codec's register can't be written from the tick. A makeup set
   directly (eq_makeup_now()) drops them. */
#define MAKEUPS 16
static struct { int value; unsigned long mark; } makeups[MAKEUPS];
static int makeups_waiting;
static bool editing;
static struct timeout makeups_tmo;

static void makeups_event(unsigned short id, void *data);

static void makeups_drop(void)
{
    makeups_waiting = 0;
    timeout_cancel(&makeups_tmo);
    remove_event(GUI_EVENT_ACTIONUPDATE, makeups_event);
}

static int makeups_wake(struct timeout *tmo)
{
    (void)tmo;
    button_queue_post(BUTTON_NONE, 0);
    return 0;
}

/* those heard put on the volume; the ticks until the next is, or -1 when none
   waits or it waits for the music to play on */
static long makeups_due(void)
{
    int done = 0;
    long until = -1;
    while (done < makeups_waiting)
    {
        until = pcmbuf_until_heard(makeups[done].mark);
        if (until)
            break;
        sound_set_eq_makeup(makeups[done++].value);
    }
    makeups_waiting -= done;
    memmove(&makeups[0], &makeups[done], makeups_waiting * sizeof(*makeups));
    if (!makeups_waiting)
    {
        makeups_drop();
        return -1;
    }
    timeout_cancel(&makeups_tmo);
    if (until > 0 && !editing)
        timeout_register(&makeups_tmo, makeups_wake, until, 0);
    return until;
}

static void makeups_event(unsigned short id, void *data)
{
    (void)id; (void)data;
    makeups_due();
}

static void makeup_put(int value)
{
    const unsigned long mark = pcmbuf_mark();
    if (!makeups_waiting && !pcmbuf_until_heard(mark))
    {
        sound_set_eq_makeup(value);
        return;
    }
    if (makeups_waiting == MAKEUPS)
        makeups_waiting--;
    makeups[makeups_waiting].value = value;
    makeups[makeups_waiting++].mark = mark;
    add_event(GUI_EVENT_ACTIONUPDATE, makeups_event);
}

void eq_makeup_now(int tenth_db)
{
    makeups_drop();
    sound_set_eq_makeup(tenth_db);
}

/* the precut and its makeup as Custom's bands now have it */
static void apply_precut(void)
{
    const int precut = eq_curve_precut(eq_curve_peak(&curve, global_settings.eq_custom));
    if (precut == global_settings.eq_custom_precut
        && precut == (int)global_settings.eq_precut)
        return;
    global_settings.eq_custom_precut = precut;
    global_settings.eq_precut = precut;
    dsp_set_eq_precut(precut);
    makeup_put(global_settings.eq_enabled ? precut : 0);
}

/* The chosen band so many steps on in the field the wheel changes, step by step
   as long as the DSP plays what the curve shows; each step to the DSP at once */
static bool step(int steps)
{
    struct eq_band_setting b = global_settings.eq_custom[sel];
    const int dir = steps > 0 ? 1 : -1;
    const int f = band_view ? field : FIELD_GAIN;
    for (int n = steps < 0 ? -steps : steps; n > 0; n--)
    {
        struct eq_band_setting next = b;
        switch (f)
        {
            case FIELD_FREQ:
                next.cutoff = next_step(b.cutoff, dir, freq_steps, ARRAYLEN(freq_steps));
                break;
            case FIELD_GAIN:
                next.gain = next_gain(b.gain, dir);
                break;
            case FIELD_Q:
                next.q = next_q(b.q, dir);
                break;
            case FIELD_TYPE:
                next.type = (b.type + dir + 3) % 3;
                break;
        }
        /* a frequency, a Q or a type the band couldn't then be raised to its
           top with is no step either */
        struct eq_band_setting top = next;
        if (f != FIELD_GAIN)
            top.gain = GAIN_MAX;
        if (!eq_curve_band_fits(&next) || !eq_curve_band_fits(&top))
            break;
        b = next;
    }
    if (!memcmp(&b, &global_settings.eq_custom[sel], sizeof(b)))
        return false;
    global_settings.eq_custom[sel] = b;
    global_settings.eq_band_settings[sel] = b;
    dsp_set_eq_coefs(sel, &b);
    eq_curve_set_band(&curve, sel, &b);
    apply_precut();
    /* kept at once, as a power-off from the sleep writes only what's waiting */
    settings_save();
    return true;
}

/* While Play is held: what's decoded goes without the bands, the precut and
   its makeup kept, so that what the EQ leaves as it is stays as loud and the
   volume never moves; into that and out of it a crossfade with the filters
   warmed from the music, as while the UI animates (eq.c's light mode) */
static void compare(bool on)
{
    comparing = on;
    dsp_eq_set_bypass(on);
}

static void boost(bool on)
{
    if (on == boosted)
        return;
    boosted = on;
    cpu_boost(on);
}

static void make_curve(void)
{
    for (int i = 0; i < EQ_NUM_BANDS; i++)
        eq_curve_set_band(&curve, i, &global_settings.eq_custom[i]);
}

int eq_editor(bool sought)
{
    eq_curve_init();
    make_curve();
    band_view = false;
    field = FIELD_GAIN;
    comparing = false;
    editing = true;
    /* a click heard within a quarter of a second */
    const bool low_latency = pcmbuf_low_latency();
    pcmbuf_set_low_latency(true);
    /* and what was decoded before that: seconds of it when the editor opens
       on Custom that already was the EQ, so that no seek was made, playing or
       paused (a pause refills the buffer after its fade). Decoded again, as a
       choice in the list is, so that the first steps are heard at once too;
       paused, the seek empties it without playing on, and the refill stops at
       a quarter second. Not after the caller's own seek (sought): that leaves
       START_DATA, half a second, which this would take for seconds */
    if (!sought && pcmbuf_unheard_ticks() > HZ / 2)
        dsp_change_heard_now(NULL);
    /* a held Play compares here, rather than sleeping and turning off */
#ifdef HAVE_SW_POWEROFF
    const bool poweroff = button_get_sw_poweroff_state();
    button_set_sw_poweroff_state(false);
#endif
    const bool was_wide = menu_wide();
    if (!was_wide)
        menu_set_wide(true);
    sb_set_persistent_title(str(LANG_EQ_CUSTOM), Icon_NOICON, SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, &ui_vp);
    /* Left to right in every language, as the frequencies go: in a language
       written from the right the texts would take mirrored places (putsxy()),
       not the plot drawn by pixels under them, and ⏭ would go to the left */
    ui_vp.flags &= ~VP_FLAG_ALIGNMENT_MASK;
    const bool rtl = lang_is_rtl();

    int ret = 1;
    bool redraw = true;
    while (ret > 0)
    {
        /* ten bands at once take more of the CPU than the presets' few, and the
           screen is drawn whole at every step: boosted while it's lit, which
           the next press does again, and not through the sleep */
        boost(is_backlight_on(true));
        if (redraw)
            draw();
        redraw = false;
        long timeout = comparing ? HZ / 20 : HZ / 2;
        const long due = makeups_due();
        if (due > 0 && due < timeout)
            timeout = due;
        const int action = get_action(CONTEXT_TREE, timeout);
        makeups_due();
        if (comparing)
        {
            /* nothing else while it's held, but what the system asks */
            if (!(button_status() & BUTTON_PLAY) || IS_SYSEVENT(action))
            {
                compare(false);
                redraw = true;
                if (IS_SYSEVENT(action)
                    && default_event_handler(action) == SYS_USB_CONNECTED)
                    ret = MENU_ATTACHED_USB;
            }
            continue;
        }
        switch (action)
        {
            case ACTION_STD_NEXT:
            case ACTION_STD_NEXTREPEAT:
                redraw = step(wheel_event_steps(get_action_data()));
                break;
            case ACTION_STD_PREV:
            case ACTION_STD_PREVREPEAT:
                redraw = step(-wheel_event_steps(get_action_data()));
                break;
            case ACTION_LIST_SKIPNEXT:
            case ACTION_LIST_SKIPPREV:
            {
                const int to = (action == ACTION_LIST_SKIPNEXT) != rtl ? sel + 1 : sel - 1;
                redraw = to >= 0 && to < EQ_NUM_BANDS;
                if (redraw)
                    sel = to;
                break;
            }
            case ACTION_STD_OK:
                if (band_view)
                    field = (field + 1) % FIELDS;
                else
                {
                    band_view = true;
                    field = FIELD_GAIN;
                    transition_begin(TRANSITION_FORWARD);
                }
                redraw = true;
                break;
            case ACTION_STD_CANCEL:
                if (band_view)
                {
                    band_view = false;
                    transition_begin(TRANSITION_BACK);
                    redraw = true;
                }
                else
                    ret = 0;
                break;
            case ACTION_STD_MENU:
                ret = GO_TO_ROOT;
                break;
            case ACTION_STD_CONTEXT:
            {
                /* the menu may give Custom other bands: Start From, Reset to
                   Flat. Not boosted there, as the loop here isn't to drop it
                   when the light goes off; it boosts again after */
                boost(false);
                const int menu_ret = eq_editor_menu();
                make_curve();
                sb_set_persistent_title(str(LANG_EQ_CUSTOM), Icon_NOICON, SCREEN_MAIN);
                if (menu_ret)
                    ret = menu_ret;
                redraw = true;
                break;
            }
            case ACTION_LIST_PLAYPAUSE:
                if (audio_status() & AUDIO_STATUS_PLAY)
                {
                    wps_do_playpause(false);
                    do_sbs_update_callback(0, NULL);
                }
                break;
            case ACTION_TREE_STOP:
                compare(true);
                redraw = true;
                break;
            case ACTION_REDRAW:
                redraw = true;
                break;
            default:
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                    ret = MENU_ATTACHED_USB;
                break;
        }
    }

    if (comparing)
        compare(false);
    /* those still waiting, on their own from now */
    editing = false;
    makeups_due();
    if (ret == 0)
        transition_begin(TRANSITION_BACK);
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    if (!was_wide)
        menu_set_wide(false);
#ifdef HAVE_SW_POWEROFF
    button_set_sw_poweroff_state(poweroff);
#endif
    boost(false);
    pcmbuf_set_low_latency(low_latency);
    return ret;
}
