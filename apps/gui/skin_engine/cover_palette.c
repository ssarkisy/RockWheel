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
#include <stdbool.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "audio.h"
#include "buffering.h"
#include "core_alloc.h"
#include "settings.h"
#include "skin_engine.h"
#include "wps_internals.h"
#include "screen_access.h"
#include "viewport.h"
#include "cover_palette.h"
#include "night_mode.h"

#define HUE_BINS 36
#define HUE_BIN_WIDTH 10

/* How long a cover that has gone keeps its colours, for the next one to come */
#define KEPT_TIME HZ

#define ABS_DIFF(a, b) ((a) > (b) ? (a) - (b) : (b) - (a))

struct hue_bin
{
    uint32_t weight, r, g, b;
};

/* The largest family of colours of the cover, by a circular histogram of hues, as
   this favours it over small bright details */
uint32_t cover_palette_colour(const fb_data *pixels, int width, int height)
{
    static struct hue_bin bins[HUE_BINS];
    uint32_t count = 0, grey = 0;

    memset(bins, 0, sizeof(bins));
    for (int y = 0; y < height; y++)
    {
        const fb_data *row = pixels + y * width;
        for (int x = 0; x < width; x += 4)
        {
            const unsigned pixel = FB_UNPACK_SCALAR_LCD(row[x]);
            const int r = RGB_UNPACK_RED(pixel), g = RGB_UNPACK_GREEN(pixel),
                      b = RGB_UNPACK_BLUE(pixel);
            const int hi = MAX(MAX(r, g), b), lo = MIN(MIN(r, g), b);
            const int sum = hi + lo, chroma = hi - lo;

            count++;
            grey += (r + g + b) / 3;
            if (chroma == 0 || sum < 18 || sum > 479)
                continue;
            const int saturation = chroma * 256 / (sum > 255 ? 510 - sum : sum);
            if (saturation < 46)
                continue;
            const int from_middle = sum / 2 > 115 ? sum / 2 - 115 : 115 - sum / 2;
            const uint32_t weight = ((90 + (166 * saturation >> 8))
                                     * (256 - (90 * from_middle >> 8))) >> 8;
            int degrees;
            if (hi == r)
                degrees = 60 * (g - b) / chroma + (g < b ? 360 : 0);
            else if (hi == g)
                degrees = 60 * (b - r) / chroma + 120;
            else
                degrees = 60 * (r - g) / chroma + 240;

            struct hue_bin *bin = &bins[(degrees / HUE_BIN_WIDTH) % HUE_BINS];
            bin->weight += weight;
            bin->r += r * weight;
            bin->g += g * weight;
            bin->b += b * weight;
        }
    }

    int best = 0;
    uint32_t score = 0;
    for (int i = 0; i < HUE_BINS; i++)
    {
        const uint32_t value = bins[i].weight * 100
            + 45 * (bins[(i + HUE_BINS - 1) % HUE_BINS].weight
                    + bins[(i + 1) % HUE_BINS].weight);
        if (value > score)
        {
            score = value;
            best = i;
        }
    }

    uint32_t weight = 0, r = 0, g = 0, b = 0;
    for (int i = -1; i <= 1; i++)
    {
        const struct hue_bin *bin = &bins[(best + i + HUE_BINS) % HUE_BINS];
        weight += bin->weight;
        r += bin->r;
        g += bin->g;
        b += bin->b;
    }
    /* a grey cover gets no invented hue: 2.5% of a full weight of 256 per pixel */
    if (weight == 0 || weight * 5 < count * 32)
    {
        const uint32_t level = count ? grey / count : 0;
        return COVER_COLOUR_GREY | level << 16 | level << 8 | level;
    }

    r /= weight;
    g /= weight;
    b /= weight;
    return (r == g && g == b ? COVER_COLOUR_GREY : 0) | r << 16 | g << 8 | b;
}

/* hue in degrees, saturation and lightness out of 1000 */
static void hsl_to_rgb(int hue, int saturation, int lightness, int rgb[3])
{
    static const unsigned char shift[3] = { 0, 240, 120 };
    const int a = saturation * MIN(lightness, 1000 - lightness) / 1000;
    for (int i = 0; i < 3; i++)
    {
        const int k = (shift[i] + hue) % 360;
        const int f = MAX(MIN(MIN(k - 90, 270 - k), 30), -30);
        rgb[i] = (255 * (lightness - a * f / 30) + 500) / 1000;
    }
}

/* the same out of the colour, the hue in degrees */
static void rgb_to_hsl(const int rgb[3], int hsl[3])
{
    const int r = rgb[0], g = rgb[1], b = rgb[2];
    const int hi = MAX(MAX(r, g), b), lo = MIN(MIN(r, g), b);
    const int sum = hi + lo, chroma = hi - lo;
    int hue = 0;
    if (chroma > 0)
    {
        if (hi == r)
            hue = 60 * (g - b) / chroma + (g < b ? 360 : 0);
        else if (hi == g)
            hue = 60 * (b - r) / chroma + 120;
        else
            hue = 60 * (r - g) / chroma + 240;
    }
    hsl[0] = hue % 360;
    hsl[1] = chroma == 0 ? 0 : chroma * 1000 / (sum > 255 ? 510 - sum : sum);
    hsl[2] = sum * 1000 / 510;
}

/* of a channel, out of 65536 */
static uint32_t linear_light(int channel)
{
    const uint32_t x = channel * 257;
    const uint32_t x2 = (x * x) >> 16, x3 = (x2 * x) >> 16;
    return ((821 * x) >> 16) + ((44706 * x2) >> 16) + ((20009 * x3) >> 16);
}

static uint32_t luminance(const int rgb[3])
{
    return ((13933 * linear_light(rgb[0])) >> 16)
         + ((46871 * linear_light(rgb[1])) >> 16)
         + ((4732 * linear_light(rgb[2])) >> 16);
}

/* the contrast ratio of two colours, by 100 */
static int contrast(const int a[3], const int b[3])
{
    const uint32_t x = luminance(a) + 3277, y = luminance(b) + 3277;
    return x > y ? x * 100 / y : y * 100 / x;
}

/* As light as this lightness, but for a contrast of 4.5 on white */
static void readable_colour(int hue, int saturation, int lightness, int rgb[3])
{
    for (;;)
    {
        hsl_to_rgb(hue, saturation, lightness, rgb);
        if (luminance(rgb) <= 12015 || lightness <= 50)
            return;
        lightness -= 5;
    }
}

int cover_palette_hue(uint32_t colour)
{
    const int rgb[3] = { (colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff };
    int hsl[3];
    rgb_to_hsl(rgb, hsl);
    return hsl[0];
}

unsigned cover_palette_toned(unsigned colour, int hue, int saturation)
{
    const int plain[3] = { RGB_UNPACK_RED(colour), RGB_UNPACK_GREEN(colour),
                           RGB_UNPACK_BLUE(colour) };
    int hsl[3], rgb[3];

    rgb_to_hsl(plain, hsl);
    hsl_to_rgb(hue, saturation, hsl[2], rgb);
    return LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
}

#define OUTLINE_CONTRAST 200

unsigned cover_palette_outline(unsigned colour, int hue, int saturation, bool dark,
                               unsigned ground)
{
    const int plain[3] = { RGB_UNPACK_RED(colour), RGB_UNPACK_GREEN(colour),
                           RGB_UNPACK_BLUE(colour) };
    const int under[3] = { RGB_UNPACK_RED(ground), RGB_UNPACK_GREEN(ground),
                           RGB_UNPACK_BLUE(ground) };
    int hsl[3], rgb[3];
    unsigned packed;

    rgb_to_hsl(plain, hsl);
    for (int lightness = hsl[2];; lightness += dark ? 5 : -5)
    {
        hsl_to_rgb(hue, saturation, lightness, rgb);
        packed = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
        const int shown[3] = { RGB_UNPACK_RED(packed), RGB_UNPACK_GREEN(packed),
                               RGB_UNPACK_BLUE(packed) };
        if (contrast(shown, under) >= OUTLINE_CONTRAST || lightness <= 0
            || lightness >= 1000)
            return packed;
    }
}

/* 4.5 to 1 by contrast(), which reads up to 0.01 over the exact ratio */
#define SURFACE_CONTRAST 452

unsigned cover_palette_surface(int hue, int saturation, int lightness, bool dark,
                               unsigned text, int *shown_lightness)
{
    const int ink[3] = { RGB_UNPACK_RED(text), RGB_UNPACK_GREEN(text),
                         RGB_UNPACK_BLUE(text) };
    int rgb[3];
    unsigned packed;

    for (;; lightness += dark ? -5 : 5)
    {
        hsl_to_rgb(hue, saturation, lightness, rgb);
        packed = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
        const int shown[3] = { RGB_UNPACK_RED(packed), RGB_UNPACK_GREEN(packed),
                               RGB_UNPACK_BLUE(packed) };
        if (contrast(shown, ink) >= SURFACE_CONTRAST || lightness <= 0
            || lightness >= 1000)
        {
            if (shown_lightness)
                *shown_lightness = lightness;
            return packed;
        }
    }
}

/* the colour as the screen shows it, in 16 bits */
static void as_shown(const int rgb[3], int out[3])
{
    out[0] = (rgb[0] & 0xf8) | (rgb[0] >> 5);
    out[1] = (rgb[1] & 0xfc) | (rgb[1] >> 6);
    out[2] = (rgb[2] & 0xf8) | (rgb[2] >> 5);
}

/* The colour, its hue kept, taken toward black or white until it reads on the
   ground with a contrast of target by 100, as the screen shows it: toward its
   own side of the ground where the target can be had there, else toward the
   end farther from it, and as far as it goes where that can't be had either.
   Always toward the farther end, a line half way from black ink on a ground
   near the middle went through it to light. */
static void readable_on(const int rgb[3], const int ground[3], int target, int out[3])
{
    static const int white[3] = { 255, 255, 255 }, black[3] = { 0, 0, 0 };
    const uint32_t lc = luminance(rgb), lg = luminance(ground);
    const int *const own = lc > lg ? white : lc < lg ? black : NULL;
    const int *const end = own && contrast(ground, own) >= target ? own
                         : contrast(ground, white) >= contrast(ground, black)
                           ? white : black;
    int c[3] = { rgb[0], rgb[1], rgb[2] }, shown[3];
    target = MIN(target, contrast(ground, end));
    for (int n = 0; n < 200; n++)
    {
        as_shown(c, shown);
        if (contrast(shown, ground) >= target)
            break;
        for (int i = 0; i < 3; i++)
            c[i] += (end[i] - c[i]) * 5 / 100
                  + (end[i] > c[i] ? 1 : end[i] < c[i] ? -1 : 0);
    }
    memcpy(out, c, sizeof(c));
}

/* c of 256 of the colour over the background */
static unsigned over_background(const int rgb[3], const int background[3], int c)
{
    return LCD_RGBPACK(background[0] + (rgb[0] - background[0]) * c / 256,
                       background[1] + (rgb[1] - background[1]) * c / 256,
                       background[2] + (rgb[2] - background[2]) * c / 256);
}

/* The dark theme's background */
static const int dark_background[3] = { THEME_DARK_BG_R, THEME_DARK_BG_G, THEME_DARK_BG_B };

/* As light as this lightness, and for a contrast of 4.5 on the dark theme's background */
static void readable_on_dark(int hue, int saturation, int lightness, int rgb[3])
{
    for (;;)
    {
        hsl_to_rgb(hue, saturation, lightness, rgb);
        if (contrast(rgb, dark_background) >= 450 || lightness >= 950)
            return;
        lightness += 5;
    }
}

/* What the theme's bitmaps and backdrop are drawn in on the whole screen */
static struct
{
    int hue, accent_saturation;
    int background[3], ink[3];
    /* the screen filled with the cover's colour */
    bool fill;
    int header_background[3], header_icon[3], header_ink[3];
    int icon_hsl[3], bar_hsl[3];
    /* the iOS look's flat bar: its fill, which stands out well, and the track under it */
    int flat_fill[3], flat_track[3];
} screen_colours;

/* the red of a nearly empty battery, a warning, and not the pinker red of the iOS
   look's icons, which is its accent and takes the cover's colour as the theme's do */
static bool is_warning_red(const int rgb[3])
{
    const int r = rgb[0], g = rgb[1], b = rgb[2];
    const int hi = MAX(MAX(r, g), b), lo = MIN(MIN(r, g), b);
    const int sum = hi + lo, chroma = hi - lo;
    return hi == r && chroma > 60 && ABS_DIFF(g, b) * 3 < chroma && b <= g + 12
        && chroma * 1000 / (sum > 255 ? 510 - sum : sum) > 450;
}

/* A shade of grey goes from the ink to the background, so highlights and shadows stay,
   and what has colour takes the hue of the cover. The red of a nearly empty battery
   stays, as it's a warning. */
static void tint_rgb(const int rgb[3], int out[3])
{
    const int r = rgb[0], g = rgb[1], b = rgb[2];
    const int hi = MAX(MAX(r, g), b), lo = MIN(MIN(r, g), b);
    const int sum = hi + lo, chroma = hi - lo;
    const int luma = (54 * r + 183 * g + 19 * b) >> 8;
    const int saturation = chroma == 0 ? 0 :
        chroma * 1000 / (sum > 255 ? 510 - sum : sum);

    if (!screen_colours.fill && is_warning_red(rgb))
    {
        out[0] = r;
        out[1] = g;
        out[2] = b;
        return;
    }

    if (screen_colours.fill)
    {
        /* the lightest shades are the images' own background: the stars' has a pale
           panel in it, which a white screen hides and a coloured one doesn't. The dark
           theme's images are the other way up. What has colour is as dark as its
           darkest part, or on the dark theme as light as its lightest, so the iOS
           look's red is ink as the theme's grey is, its smoothed edges going over. */
        const int weight = MIN(chroma, 64);
        const int shade = global_settings.dark_theme ? 255 - (luma + (hi - luma) * weight / 64)
                                                     : luma - (luma - lo) * weight / 64;
        const int level = MIN(shade * 255 / 236, 255);
        for (int i = 0; i < 3; i++)
            out[i] = screen_colours.ink[i]
                   + (screen_colours.background[i] - screen_colours.ink[i]) * level / 255;
        return;
    }

    for (int i = 0; i < 3; i++)
        out[i] = screen_colours.ink[i]
               + (screen_colours.background[i] - screen_colours.ink[i]) * luma / 255;

    /* a smooth step of how much colour it has */
    int step = (chroma * 256 - 1632) * 10 / 816;
    if (step <= 0)
        return;
    step = MIN(step, 256);
    const int blend = ((step * step >> 8) * (768 - 2 * step)) >> 8;

    int colour[3];
    hsl_to_rgb(screen_colours.hue,
               screen_colours.accent_saturation * (650 + 350 * saturation / 1000) / 1000,
               120 + 800 * (sum * 1000 / 510) / 1000, colour);
    /* clear of the screen as an icon is to be seen, 3 to 1, and 4.5 in the grey of a
       grey cover, which has no hue to stand out by: the iOS look's red came out as a
       grey too light for the status bar and the stars. Never more than the pixel
       had on its image's background, so that smoothed edges stay smooth. The dark
       theme's images are the other way up: its background is the ink's end. */
    static const int white[3] = { 255, 255, 255 }, black[3] = { 0, 0, 0 };
    const bool dark = global_settings.dark_theme;
    const int target = MIN(screen_colours.accent_saturation ? 300 : 450,
                           contrast(rgb, dark ? black : white));
    const int *const back = dark ? screen_colours.ink : screen_colours.background;
    const int *const away = dark ? screen_colours.background : screen_colours.ink;
    for (int n = 0; n < 40 && contrast(colour, back) < target; n++)
    {
        for (int i = 0; i < 3; i++)
            colour[i] += (away[i] - colour[i]) * 8 / 100
                       + (away[i] > colour[i] ? 1 : away[i] < colour[i] ? -1 : 0);
    }
    for (int i = 0; i < 3; i++)
        out[i] += (colour[i] - out[i]) * blend / 256;
}

static void unpack_rgb(fb_data pixel, int rgb[3])
{
    const unsigned value = FB_UNPACK_SCALAR_LCD(pixel);
    rgb[0] = RGB_UNPACK_RED(value);
    rgb[1] = RGB_UNPACK_GREEN(value);
    rgb[2] = RGB_UNPACK_BLUE(value);
}

/* the nearest of the screen's colours */
static fb_data pack_nearest(const int rgb[3])
{
    return FB_SCALARPACK(LCD_RGBPACK((rgb[0] * 31 + 127) / 255 * 8,
                                     (rgb[1] * 63 + 127) / 255 * 4,
                                     (rgb[2] * 31 + 127) / 255 * 8));
}

/* Colours of a backdrop this takes at most */
#define BACKDROP_COLOURS 256

/* Where the theme has these */
#define HEADER_HEIGHT    19
#define BAR_X            58
#define BAR_Y            206
#define BAR_WIDTH        204
#define BAR_HEIGHT       14 /* with its reflection under it */
#define BAR_REFLECTION   9
#define REFLECTION_ALPHA 67 /* of 256, where it starts */

/* What the whole screen takes, only while it's chosen */
static struct screen_buffers
{
    fb_data backdrop[LCD_FBHEIGHT * LCD_FBWIDTH]; /* in the colours of the cover */
    /* The theme's backdrop is mostly a few colours, so which each of its pixels is,
       and each of those in the colours of the cover at each place of the dither */
    unsigned char backdrop_index[LCD_FBHEIGHT * LCD_FBWIDTH];
    fb_data backdrop_colours[BACKDROP_COLOURS];
    fb_data dithered[BACKDROP_COLOURS][16];
    /* Each colour of a bitmap is worked out when it's first drawn, and bitmaps have
       few of them, so this takes a moment for a new colour, and then a lookup */
    fb_data tint_table[1 << 16];
    uint32_t tint_known[(1 << 16) / 32];
    /* the same for the status bar when filled, whose icons are drawn differently */
    uint32_t header_cache[1024];
    fb_data bar[LCD_WIDTH * 32]; /* the fill of the progress bar */
    fb_data track[LCD_WIDTH * 32]; /* and the iOS look's track under it */
} *buffers;
static int buffers_handle;
static const void *bar_source; /* of what's in it */
static const void *track_source;

static int tinted_backdrop_id = -1;
static int indexed_backdrop_id = -1;
static int backdrop_colour_count;

/* no memory for them when the setting was chosen: not asked for again as the
   music plays, where each try would start the track again, until another is
   chosen, rather than as the settings are applied again */
static bool buffers_refused;
static int buffers_mode = -1; /* the setting they were asked for in */

/* The audio buffer gives way to them, which starts a playing track again where it was,
   but leaves a paused one stopped */
static void allocate_buffers(void)
{
    if (buffers || buffers_refused)
        return;
    const bool paused = audio_status() & AUDIO_STATUS_PAUSE;
    unsigned long elapsed = 0, offset = 0;
    if (paused)
    {
        const struct mp3entry *id3 = audio_current_track();
        elapsed = id3->elapsed;
        offset = id3->offset;
    }
    const int handle = core_alloc_ex(sizeof(*buffers), &buflib_ops_locked);
    if (handle > 0)
    {
        buffers_handle = handle;
        buffers = core_get_data(handle);
    }
    else
        buffers_refused = true;
    if (paused && !(audio_status() & AUDIO_STATUS_PLAY))
    {
        audio_play(elapsed, offset);
        audio_pause();
    }
}

static void tint_header_line(const fb_data *src, fb_data *dst, int count);

/* as coloured as tint_header_rgb() takes for an icon's colour */
static bool icon_coloured(fb_data pixel)
{
    int rgb[3];
    unpack_rgb(pixel, rgb);
    const int hi = MAX(MAX(rgb[0], rgb[1]), rgb[2]);
    const int lo = MIN(MIN(rgb[0], rgb[1]), rgb[2]);
    const int sum = hi + lo, chroma = hi - lo;
    return chroma > 25 && chroma * 1000 / (sum > 255 ? 510 - sum : sum) > 400;
}

void cover_palette_keep_colours(const fb_data *src, fb_data *dst, int count)
{
    const fb_data transparent = FB_SCALARPACK(TRANSPARENT_COLOR);
    for (int i = 0; i < count; i++)
    {
        if (src[i] != transparent && icon_coloured(src[i]))
            dst[i] = src[i];
    }
}

void cover_palette_tint_line(const fb_data *src, fb_data *dst, int count, int y)
{
    if (screen_colours.fill && y < HEADER_HEIGHT)
    {
        tint_header_line(src, dst, count);
        return;
    }
    fb_data *const tint_table = buffers->tint_table;
    uint32_t *const tint_known = buffers->tint_known;
    const fb_data transparent = FB_SCALARPACK(TRANSPARENT_COLOR);
    for (int i = 0; i < count; i++)
    {
        const fb_data pixel = src[i];
        const unsigned key = (uint16_t)pixel;
        if (!(tint_known[key >> 5] & (1u << (key & 31))))
        {
            fb_data tinted = pixel;
            if (pixel != transparent)
            {
                int rgb[3], out[3];
                unpack_rgb(pixel, rgb);
                tint_rgb(rgb, out);
                tinted = pack_nearest(out);
            }
            tint_table[key] = tinted;
            tint_known[key >> 5] |= 1u << (key & 31);
        }
        dst[i] = tint_table[key];
    }
}

/* On the Fill screen the knob's colour went to the ink, as an icon's does, and the
   fill it sits on is a colour of its own: the colour of that. Elsewhere both take
   the same tint. Grey, as the iPod look's knob, stays as it is. */
void cover_palette_knob_line(const fb_data *src, fb_data *dst, int count, int y)
{
    cover_palette_tint_line(src, dst, count, y);
    if (!screen_colours.fill)
        return;
    const fb_data transparent = FB_SCALARPACK(TRANSPARENT_COLOR);
    const fb_data fill = pack_nearest(screen_colours.flat_fill);
    for (int i = 0; i < count; i++)
    {
        if (src[i] != transparent && icon_coloured(src[i]))
            dst[i] = fill;
    }
}

/* Done once for a backdrop, using the table of the bitmaps' colours for the moment.
   False if it has too many colours, and it then isn't tinted. */
static bool index_backdrop(int id, const fb_data *src)
{
    struct screen_buffers *const b = buffers;
    int count = 0;

    memset(b->tint_known, 0, sizeof(b->tint_known));
    for (int i = 0; i < LCD_FBHEIGHT * LCD_FBWIDTH; i++)
    {
        const unsigned key = (uint16_t)src[i];
        if (!(b->tint_known[key >> 5] & (1u << (key & 31))))
        {
            if (count == BACKDROP_COLOURS)
                return false;
            b->tint_known[key >> 5] |= 1u << (key & 31);
            b->tint_table[key] = count;
            b->backdrop_colours[count++] = src[i];
        }
        b->backdrop_index[i] = b->tint_table[key];
    }
    backdrop_colour_count = count;
    indexed_backdrop_id = id;
    return true;
}

/* Dithered, as its light, nearly grey shades would otherwise round to the next of the
   screen's colours in a way that loses their hue, the worst in the status bar */
static void tint_backdrop(void)
{
    static const unsigned char dither[16] =
        { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    struct screen_buffers *const b = buffers;
    const int id = skin_get_gwps(WPS, SCREEN_MAIN)->data->backdrop_id;
    const fb_data *src = skin_backdrop_buffer(id);

    tinted_backdrop_id = -1;
    if (!src || (id != indexed_backdrop_id && !index_backdrop(id, src)))
        return;

    cpu_boost(true);
    for (int c = 0; c < backdrop_colour_count; c++)
    {
        int rgb[3], out[3];
        unpack_rgb(b->backdrop_colours[c], rgb);
        tint_rgb(rgb, out);
        /* to what packing takes for the screen's brightest: 248 of red and blue and
           252 of green, or the dither's average is too light and more so in those two,
           which turns a light tint pink */
        const int red = out[0] * 248 / 255, green = out[1] * 252 / 255,
                  blue = out[2] * 248 / 255;
        for (int i = 0; i < 16; i++)
        {
            /* it would show on the dark theme's shades, which have no near grey to lose */
            const int d = global_settings.dark_theme ? 8 : dither[i];
            b->dithered[c][i] = FB_SCALARPACK(LCD_RGBPACK(red + d / 2, green + d / 4,
                                                          blue + d / 2));
        }
    }
    const unsigned char *index = b->backdrop_index;
    fb_data *dst = b->backdrop;
    for (int y = 0; y < LCD_HEIGHT; y++)
    {
        const int row = (y & 3) * 4;
        for (int x = 0; x < LCD_WIDTH; x++)
            *dst++ = b->dithered[*index++][row + (x & 3)];
    }
    cpu_boost(false);
    tinted_backdrop_id = id;
}

/* a shade of the status bar, from one of the theme's, which keeps its gradient */
static void header_shade(int tone, int out[3])
{
    const int delta = (tone - 237) * 170 / 255;
    for (int i = 0; i < 3; i++)
        out[i] = MAX(MIN(screen_colours.header_background[i] + delta, 255), 0);
}

/* Icons are all of one weight: their dark parts come out in the icon colour, whether the
   theme has them in grey or in colour, as the play symbol and the battery's charge. */
static void tint_header_rgb(const int rgb[3], int out[3])
{
    const int hi = MAX(MAX(rgb[0], rgb[1]), rgb[2]), lo = MIN(MIN(rgb[0], rgb[1]), rgb[2]);
    const int sum = hi + lo, chroma = hi - lo;
    int luma = (54 * rgb[0] + 183 * rgb[1] + 19 * rgb[2]) >> 8;
    int back[3];

    if (is_warning_red(rgb))
    {
        memcpy(out, rgb, 3 * sizeof(int));
        return;
    }
    if (chroma > 25 && chroma * 1000 / (sum > 255 ? 510 - sum : sum) > 400)
    {
        const int *const icon = screen_colours.icon_hsl;
        const int lightness = icon[2] + (luma * 1000 / 255 - 600) * 35 / 100;
        hsl_to_rgb(icon[0], icon[1], MAX(MIN(lightness, 980), 25), out);
        header_shade(237, back);
        for (int n = 0; n < 30 && contrast(out, back) < 350; n++)
        {
            for (int i = 0; i < 3; i++)
                out[i] += (screen_colours.header_ink[i] - out[i]) * 8 / 100;
        }
        return;
    }
    /* the dark theme's grey images are the other way up */
    if (global_settings.dark_theme)
        luma = 255 - luma;
    if (luma >= 191)
    {
        header_shade(luma, out);
        return;
    }
    header_shade(191, back);
    const int alpha = MIN((191 - luma) * 256 / (191 - 70), 256);
    for (int i = 0; i < 3; i++)
        out[i] = back[i] + (screen_colours.header_icon[i] - back[i]) * alpha / 256;
}

static void tint_header_line(const fb_data *src, fb_data *dst, int count)
{
    uint32_t *const cache = buffers->header_cache;
    const fb_data transparent = FB_SCALARPACK(TRANSPARENT_COLOR);
    for (int i = 0; i < count; i++)
    {
        const fb_data pixel = src[i];
        if (pixel == transparent)
        {
            dst[i] = pixel;
            continue;
        }
        const uint32_t key = (uint16_t)pixel;
        uint32_t *entry = &cache[(key ^ (key >> 7)) % ARRAYLEN(buffers->header_cache)];
        if (*entry == 0xffffffff || *entry >> 16 != key)
        {
            int rgb[3], out[3];
            unpack_rgb(pixel, rgb);
            tint_header_rgb(rgb, out);
            *entry = key << 16 | (uint16_t)pack_nearest(out);
        }
        dst[i] = (fb_data)(uint16_t)*entry;
    }
}

/* a pixel of the bar, as its reflection on the background, alpha of 256 */
static fb_data reflected(fb_data pixel, int alpha)
{
    const int *const bg = screen_colours.background;
    int rgb[3];
    unpack_rgb(pixel, rgb);
    for (int i = 0; i < 3; i++)
        rgb[i] = bg[i] + (rgb[i] - bg[i]) * alpha / 256;
    return FB_SCALARPACK(LCD_RGBPACK(MIN(rgb[0] + 4, 255), MIN(rgb[1] + 2, 255),
                                     MIN(rgb[2] + 4, 255)));
}

/* of row of the reflection, from the bar */
static int reflection_alpha(int row)
{
    return REFLECTION_ALPHA * (BAR_REFLECTION - row) / BAR_REFLECTION;
}

/* A plain background with the cover on it, so nothing of the theme's shadows and
   reflections, but for the status bar and the empty progress bar */
static void fill_backdrop(void)
{
    struct screen_buffers *const b = buffers;
    const int id = skin_get_gwps(WPS, SCREEN_MAIN)->data->backdrop_id;
    const fb_data *src = skin_backdrop_buffer(id);

    tinted_backdrop_id = -1;
    if (!src)
        return;

    cpu_boost(true);
    const fb_data plain = pack_nearest(screen_colours.background);
    for (int i = 0; i < LCD_FBHEIGHT * LCD_FBWIDTH; i++)
        b->backdrop[i] = plain;

    /* the status bar, the battery the theme draws over it not in it */
    for (int y = 0; y < HEADER_HEIGHT - 1; y++)
        tint_header_line(&src[y * LCD_WIDTH], &b->backdrop[y * LCD_WIDTH], LCD_WIDTH);
    /* and the dark line under it isn't one */
    for (int x = 0; x < LCD_WIDTH; x++)
    {
        int rgb[3], out[3];
        unpack_rgb(src[(HEADER_HEIGHT - 1) * LCD_WIDTH + x], rgb);
        header_shade(global_settings.dark_theme ? 110
                     : (54 * rgb[0] + 183 * rgb[1] + 19 * rgb[2]) >> 8, out);
        b->backdrop[(HEADER_HEIGHT - 1) * LCD_WIDTH + x] = pack_nearest(out);
    }

    /* The empty bar a shade of the iOS look's track, a step from the background,
       with the rows of the theme's: it stayed the light theme's on every cover,
       and the fill read on it from as little as 1 to 1 */
    int track_hsl[3], sum = 0;
    rgb_to_hsl(screen_colours.flat_track, track_hsl);
    for (int y = 0; y < BAR_HEIGHT; y++)
    {
        for (int x = 0; x < BAR_WIDTH; x++)
        {
            int rgb[3];
            unpack_rgb(src[(BAR_Y + y) * LCD_WIDTH + BAR_X + x], rgb);
            sum += (54 * rgb[0] + 183 * rgb[1] + 19 * rgb[2]) >> 8;
        }
    }
    const int mean = sum / (BAR_HEIGHT * BAR_WIDTH);
    for (int y = 0; y < BAR_HEIGHT; y++)
    {
        const fb_data *from = &src[(BAR_Y + y) * LCD_WIDTH + BAR_X];
        fb_data *to = &b->backdrop[(BAR_Y + y) * LCD_WIDTH + BAR_X];
        fb_data last = 0, shade = 0;
        for (int x = 0; x < BAR_WIDTH; x++)
        {
            if (x == 0 || from[x] != last)
            {
                int rgb[3];
                last = from[x];
                unpack_rgb(last, rgb);
                const int luma = (54 * rgb[0] + 183 * rgb[1] + 19 * rgb[2]) >> 8;
                const int lightness = track_hsl[2] + (luma - mean) * 1000 / 255 * 40 / 100;
                hsl_to_rgb(track_hsl[0], track_hsl[1], MAX(MIN(lightness, 980), 20), rgb);
                shade = pack_nearest(rgb);
            }
            to[x] = shade;
        }
    }
    for (int y = 0; y < BAR_REFLECTION; y++)
    {
        const int alpha = reflection_alpha(y);
        const fb_data *row = &b->backdrop[(BAR_Y + BAR_HEIGHT - 1 - y) * LCD_WIDTH + BAR_X];
        fb_data *dst = &b->backdrop[(BAR_Y + BAR_HEIGHT + y) * LCD_WIDTH + BAR_X];
        for (int x = 0; x < BAR_WIDTH; x++)
            dst[x] = reflected(row[x], alpha);
    }
    cpu_boost(false);
    tinted_backdrop_id = id;
}

static inline int pixel_luma(fb_data pixel)
{
    int rgb[3];
    unpack_rgb(pixel, rgb);
    return (54 * rgb[0] + 183 * rgb[1] + 19 * rgb[2]) >> 8;
}

/* A flat part of the iOS bar, smoothed against its theme's background at its ends:
   in colour where it's whole, and at its edges as much of that over the background
   as there was of the theme's over its own. A shade by each pixel's lightness made
   the edges lighter than the rest, a white rim at the end of the fill. */
static void flat_bar(const fb_data *src, fb_data *dst, int count, const int colour[3])
{
    const fb_data corner = src[0];
    const int *const bg = screen_colours.background;
    const fb_data plain = pack_nearest(bg), whole = pack_nearest(colour);
    /* it's whole where it's farthest from the corner's lightness: darker on a light
       theme, lighter on a dark one */
    const int corner_luma = pixel_luma(corner);
    int span = 0;
    for (int i = 0; i < count; i++)
        span = MAX(span, abs(pixel_luma(src[i]) - corner_luma));
    fb_data from = corner, to = plain;
    for (int i = 0; i < count; i++)
    {
        if (src[i] == corner)
            dst[i] = plain;
        else
        {
            if (src[i] != from)
            {
                from = src[i];
                const int part = span > 0 ? abs(corner_luma - pixel_luma(from)) * 256 / span
                                          : 256;
                if (part >= 256)
                    to = whole;
                else
                {
                    int out[3];
                    for (int c = 0; c < 3; c++)
                        out[c] = bg[c] + (colour[c] - bg[c]) * MAX(part, 0) / 256;
                    to = pack_nearest(out);
                }
            }
            dst[i] = to;
        }
    }
}

void *cover_palette_bar_track(const struct bitmap *bm)
{
    struct screen_buffers *const b = buffers;
    const fb_data *src = (const fb_data *)bm->data;
    const int count = bm->width * bm->height;
    if (!b || !screen_colours.fill || count > (int)ARRAYLEN(b->track))
        return NULL;
    if (track_source != src)
    {
        track_source = src;
        flat_bar(src, b->track, count, screen_colours.flat_track);
    }
    return b->track;
}

/* The iPod look's bar on the cover's colour: its rows' lightness about a centre,
   spread as the theme's highlight, out of 100 */
static int bar_centre, bar_spread;

static void bar_shade(int luma, int out[3])
{
    const int *const hsl = screen_colours.bar_hsl;
    const int lightness = bar_centre + (luma * 1000 / 255 - 600) * bar_spread / 100;
    hsl_to_rgb(hsl[0], hsl[1], MAX(MIN(lightness, 980), 25), out);
}

/* The centre and the spread for every row to read 3 to 1 on the track, which
   tells what's played from what's left, and on the background: near the centre
   the background gave, with as much of the highlight as that leaves; the best
   there is where that can't be had. Once a palette. */
static void fit_bar(const int *lumas, int rows)
{
    static const int spreads[] = { 115, 90, 60, 30, 0 };
    const int *const bg = screen_colours.background;
    const int *const track = screen_colours.flat_track;
    const int start = screen_colours.bar_hsl[2];
    int best = -1, best_centre = start, best_spread = spreads[0];

    cpu_boost(true);
    for (unsigned s = 0; s < ARRAYLEN(spreads); s++)
    {
        for (int d = 0; d <= 960; d += 10)
        {
            for (int side = -1; side <= 1; side += 2)
            {
                const int centre = start + side * d;
                if ((d == 0 && side > 0) || centre < 40 || centre > 980)
                    continue;
                bar_centre = centre;
                bar_spread = spreads[s];
                int worst = 100000;
                for (int y = 0; y < rows && worst > best; y++)
                {
                    int rgb[3];
                    bar_shade(lumas[y], rgb);
                    worst = MIN(worst, MIN(contrast(rgb, track), contrast(rgb, bg)));
                }
                if (worst > best)
                {
                    best = worst;
                    best_centre = centre;
                    best_spread = spreads[s];
                    if (best >= 300)
                        goto found;
                }
            }
        }
    }
found:
    cpu_boost(false);
    bar_centre = best_centre;
    bar_spread = best_spread;
}

void *cover_palette_bar(const struct bitmap *bm, bool flat)
{
    struct screen_buffers *const b = buffers;
    const fb_data *src = (const fb_data *)bm->data;
    const int width = bm->width;

    if (width * bm->height > (int)ARRAYLEN(b->bar))
        return NULL;
    if (bar_source == src)
        return b->bar;
    bar_source = src;

    if (!screen_colours.fill)
    {
        for (int y = 0; y < bm->height; y++)
            cover_palette_tint_line(src + y * width, b->bar + y * width, width, BAR_Y);
        return b->bar;
    }

    if (flat)
    {
        flat_bar(src, b->bar, width * bm->height, screen_colours.flat_fill);
        return b->bar;
    }

    const int rows = MIN(bm->height, BAR_HEIGHT);
    int lumas[BAR_HEIGHT];
    for (int y = 0; y < rows; y++)
        lumas[y] = pixel_luma(src[y * width + width / 2]);
    fit_bar(lumas, rows);
    fb_data from = *src, to = 0;
    bool known = false;
    for (int i = 0; i < rows * width; i++)
    {
        /* whole rows of the same colour */
        if (!known || src[i] != from)
        {
            int rgb[3], out[3];
            from = src[i];
            known = true;
            unpack_rgb(from, rgb);
            bar_shade((54 * rgb[0] + 183 * rgb[1] + 19 * rgb[2]) >> 8, out);
            to = pack_nearest(out);
        }
        b->bar[i] = to;
    }
    for (int y = rows; y < bm->height; y++)
    {
        const int alpha = reflection_alpha(y - rows);
        const fb_data *row = b->bar + MAX(2 * rows - 1 - y, 0) * width;
        for (int x = 0; x < width; x++)
            b->bar[y * width + x] = reflected(row[x], alpha);
    }
    return b->bar;
}

/* the background mixed towards white or black, opacity of 100, or as far as contrast
   by 100 asks */
static void contrasting_ink(const int bg[3], bool light, int opacity, int target, int out[3])
{
    const int end = light ? 255 : 0;
    const int far_end[3] = { end, end, end };
    target = MIN(target, contrast(bg, far_end));
    for (;; opacity++)
    {
        for (int i = 0; i < 3; i++)
            out[i] = bg[i] + (end - bg[i]) * opacity / 100;
        if (opacity >= 100 || contrast(bg, out) >= target)
            return;
    }
}

/* of the status bar, whose ink is white or black: clear of its light and dark ends */
static int header_contrast(const int colour[3])
{
    int shade[3];
    header_shade(191, shade);
    const int dark = contrast(colour, shade);
    header_shade(255, shade);
    return MIN(dark, contrast(colour, shade));
}

static void make_fill_palette(struct cover_palette *palette, uint32_t colour, bool grey)
{
    static const int white[3] = { 255, 255, 255 }, black[3] = { 0, 0, 0 };
    int *const bg = screen_colours.background, *const hb = screen_colours.header_background,
        *const ink = screen_colours.ink, *const icon = screen_colours.header_icon;
    int rgb[3], hsl[3];

    bg[0] = (colour >> 16) & 0xff;
    bg[1] = (colour >> 8) & 0xff;
    bg[2] = colour & 0xff;
    const bool light = contrast(bg, white) >= contrast(bg, black);

    palette->whole_screen = true;
    palette->background = LCD_RGBPACK(bg[0], bg[1], bg[2]);
    screen_colours.fill = true;
    contrasting_ink(bg, light, 97, 700, ink);
    palette->ink = LCD_RGBPACK(ink[0], ink[1], ink[2]);
    contrasting_ink(bg, light, 84, 450, rgb);
    palette->secondary = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    contrasting_ink(bg, light, 72, 450, rgb);
    palette->muted = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    contrasting_ink(bg, light, 92, 450, rgb);
    palette->format = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    palette->format_half = over_background(rgb, bg, 128);
    palette->format_quarter = over_background(rgb, bg, 64);

    /* the status bar: a shade of the background that its ink can be read on */
    const int *const header_ink = light ? white : black;
    const int away = light ? 0 : 255;
    memcpy(screen_colours.header_ink, header_ink, 3 * sizeof(int));
    for (int i = 0; i < 3; i++)
        hb[i] = light ? (bg[i] * 82 + 255 * 13 + 50) / 100 : (bg[i] * 92 + 255 * 3 + 50) / 100;
    for (int n = 0; n < 100 && header_contrast(header_ink) < 450; n++)
    {
        for (int i = 0; i < 3; i++)
            hb[i] += (away - hb[i]) * 25 / 1000 + (away > hb[i] ? 1 : away < hb[i] ? -1 : 0);
    }
    rgb_to_hsl(bg, hsl);
    hsl_to_rgb(hsl[0], hsl[1] < 50 ? 0 : MAX(MIN(hsl[1] * 55 / 100, 600), 120),
               light ? 780 : 280, icon);
    for (int n = 0; n < 100 && header_contrast(icon) < 350; n++)
    {
        for (int i = 0; i < 3; i++)
            icon[i] += (header_ink[i] - icon[i]) * 4 / 100
                     + (header_ink[i] > icon[i] ? 1 : header_ink[i] < icon[i] ? -1 : 0);
    }
    rgb_to_hsl(icon, screen_colours.icon_hsl);

    /* the progress bar: the cover's hue, standing out from the background */
    const int direction = contrast(bg, white) > contrast(bg, black) ? 10 : -10;
    int lightness = 550;
    const int saturation = grey ? 0 : MAX(MIN(hsl[1], 800), 450);
    hsl_to_rgb(hsl[0], saturation, lightness, rgb);
    for (int n = 0; n < 40 && contrast(bg, rgb) < 240; n++)
    {
        lightness = MAX(MIN(lightness + direction, 840), 160);
        hsl_to_rgb(hsl[0], saturation, lightness, rgb);
    }
    screen_colours.bar_hsl[0] = hsl[0];
    screen_colours.bar_hsl[1] = saturation;
    screen_colours.bar_hsl[2] = lightness;

    /* The iOS look's bar is flat, a fill over a track: the fill well clear of the
       background, 4.8 to 1, and the track a step from it, 1.6 to 1. Its 2.4 of
       the other was the cover's blue on a darker blue, and its track went with
       the background. */
    int *const fill = screen_colours.flat_fill, *const track = screen_colours.flat_track;
    int flat = lightness;
    hsl_to_rgb(hsl[0], saturation, flat, fill);
    for (int n = 0; n < 100 && contrast(bg, fill) < 480; n++)
    {
        flat = MAX(MIN(flat + direction, 960), 40);
        hsl_to_rgb(hsl[0], saturation, flat, fill);
    }
    const int *const toward = direction > 0 ? white : black;
    memcpy(track, bg, 3 * sizeof(int));
    for (int n = 0; n < 100 && contrast(bg, track) < 160; n++)
    {
        for (int i = 0; i < 3; i++)
            track[i] += (toward[i] - track[i]) * 3 / 100
                      + (toward[i] > track[i] ? 1 : toward[i] < track[i] ? -1 : 0);
    }
}

static void make_screen_palette(struct cover_palette *palette, int hue, bool grey)
{
    const int s = grey ? 0 : 1;
    int rgb[3];

    palette->whole_screen = true;
    screen_colours.fill = false;
    screen_colours.hue = hue;
    screen_colours.accent_saturation = s * 598;
    if (global_settings.dark_theme)
    {
        /* its images are light on dark, so what's dark is the background here */
        int *const low = screen_colours.ink, *const high = screen_colours.background;
        hsl_to_rgb(hue, s * 360, 95, low);
        hsl_to_rgb(hue, s * 220, 950, high);
        palette->background = LCD_RGBPACK(low[0], low[1], low[2]);
        readable_on(high, low, 700, rgb);
        palette->ink = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
        hsl_to_rgb(hue, s * 200, 760, rgb);
        readable_on(rgb, low, 450, rgb);
        palette->secondary = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
        hsl_to_rgb(hue, s * 160, 640, rgb);
        readable_on(rgb, low, 450, rgb);
        palette->muted = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
        hsl_to_rgb(hue, s * 220, 860, rgb);
        readable_on(rgb, low, 450, rgb);
        palette->format = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
        palette->format_half = over_background(rgb, low, 128);
        palette->format_quarter = over_background(rgb, low, 64);
        return;
    }
    hsl_to_rgb(hue, s * 340, 949, screen_colours.background);
    hsl_to_rgb(hue, s * 300, 105, screen_colours.ink);
    palette->background = LCD_RGBPACK(screen_colours.background[0],
                                      screen_colours.background[1],
                                      screen_colours.background[2]);
    /* the text's roles each as clear of the background as they're to be */
    const int *const back = screen_colours.background;
    readable_on(screen_colours.ink, back, 700, rgb);
    palette->ink = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    hsl_to_rgb(hue, s * 180, 330, rgb);
    readable_on(rgb, back, 450, rgb);
    palette->secondary = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    hsl_to_rgb(hue, s * 140, 380, rgb);
    readable_on(rgb, back, 450, rgb);
    palette->muted = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    hsl_to_rgb(hue, s * 220, 200, rgb);
    readable_on(rgb, back, 450, rgb);
    palette->format = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    palette->format_half = over_background(rgb, screen_colours.background, 128);
    palette->format_quarter = over_background(rgb, screen_colours.background, 64);
}

static void make_text_palette(struct cover_palette *palette, int hue, bool grey)
{
    static const int white[3] = { 255, 255, 255 };
    const int s = grey ? 0 : 1;
    int ink[3], rgb[3];

    palette->whole_screen = false;
    screen_colours.fill = false;
    if (global_settings.dark_theme)
    {
        readable_on_dark(hue, s * 652, 720, ink);
        readable_on(ink, dark_background, 700, ink);
        palette->ink = LCD_RGBPACK(ink[0], ink[1], ink[2]);
        readable_on_dark(hue, s * 420, 720, rgb);
        palette->secondary = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
        readable_on_dark(hue, s * 285, 620, rgb);
        palette->muted = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
        readable_on_dark(hue, s * 588, 760, rgb);
        palette->format = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
        palette->format_half = over_background(rgb, dark_background, 128);
        palette->format_quarter = over_background(rgb, dark_background, 64);
        /* the stars are light on dark in these images: their shades go up to the ink */
        for (int i = 0; i < COVER_PALETTE_STAR_KEPT; i++)
        {
            const int level = i * 255 / (COVER_PALETTE_STAR_KEPT - 1);
            int star[3];
            for (int c = 0; c < 3; c++)
                star[c] = dark_background[c] + (ink[c] - dark_background[c]) * level / 255;
            palette->star[i] = FB_SCALARPACK(LCD_RGBPACK(star[0], star[1], star[2]));
        }
        return;
    }
    readable_colour(hue, s * 652, 280, ink);
    /* the title's and the time's: 7 to 1, as the rest 4.5 */
    readable_on(ink, white, 700, ink);
    palette->ink = LCD_RGBPACK(ink[0], ink[1], ink[2]);
    readable_colour(hue, s * 420, 350, rgb);
    palette->secondary = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    readable_colour(hue, s * 285, 400, rgb);
    palette->muted = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    readable_colour(hue, s * 588, 300, rgb);
    palette->format = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    palette->format_half = over_background(rgb, white, 128);
    palette->format_quarter = over_background(rgb, white, 64);

    /* Black becomes the ink and white stays, so the stars keep their volume. Their
       images have a light background, which fades to its own grey on the way up to
       the levels that are left as they are. */
    for (int i = 0; i < COVER_PALETTE_STAR_KEPT; i++)
    {
        const int level = i * 255 / (COVER_PALETTE_STAR_LEVELS - 1);
        const int fade = MAX(i - COVER_PALETTE_STAR_FADE, 0);
        const int fade_range = COVER_PALETTE_STAR_KEPT - COVER_PALETTE_STAR_FADE;
        int star[3];
        for (int c = 0; c < 3; c++)
        {
            const int tinted = ink[c] + (255 - ink[c]) * level / 255;
            star[c] = tinted + (level - tinted) * fade / fade_range;
        }
        palette->star[i] = FB_SCALARPACK(LCD_RGBPACK(star[0], star[1], star[2]));
    }
}

/* A cover's colour too near grey to give a theme with a tone its hue: its
   saturation, how far apart its parts are of its brightest, under this of
   100; or the parts under TONE_FAINT_LEVELS of 255 apart, which a nearly
   black one's are whatever that ratio. cover_palette_colour() calls a cover
   grey only when next to none of its pixels has a colour, and counts one by
   its saturation of its lightness, 18 %, which near white a difference of
   a few levels has: a nearly white cover's faint warmth came out as an
   orange screen at the tone's depth */
#define TONE_FAINT_SATURATION 12
#define TONE_FAINT_LEVELS     12

static bool tone_faint(uint32_t colour)
{
    const int r = (colour >> 16) & 0xff, g = (colour >> 8) & 0xff, b = colour & 0xff;
    const int hi = MAX(MAX(r, g), b), chroma = hi - MIN(MIN(r, g), b);
    return chroma < TONE_FAINT_LEVELS || chroma * 100 < TONE_FAINT_SATURATION * hi;
}

/* Fill in a theme with a tone, which has no backdrop: Classic's colours of the
   cover, flat. What the texts' and the marks' roles don't hold: the texts and
   glyphs of the status line, on its shade; the bar's fill; a quiet mark, as a
   large star not set */
static struct
{
    unsigned status_ink, status_secondary, bar, quiet;
} tone_fill;

/* 4.5 and 7 to 1 by contrast(), which reads a saturated colour up to 0.07
   over the exact ratio at the first and 0.2 at the second */
#define TONE_TEXT_CONTRAST  464
#define TONE_TITLE_CONTRAST 724
/* and a colour near black up to 0.15: on a ground of a middle lightness
   the texts are that */
#define TONE_FILL_CONTRAST  472

static void make_tone_fill_palette(struct cover_palette *palette, uint32_t colour,
                                   bool grey, struct ui_theme_fill *surfaces)
{
    unsigned *const role[4] = { &palette->ink, &palette->secondary, &palette->muted,
                                &palette->format };
    const int *const header_ink = screen_colours.header_ink;
    int ground[3], shade[3], shown[3], rgb[3];

    make_fill_palette(palette, colour, grey);
    /* the ground as Classic paints it, the nearest of the screen's colours */
    unpack_rgb(pack_nearest(screen_colours.background), ground);
    palette->background = LCD_RGBPACK(ground[0], ground[1], ground[2]);
    /* Classic's texts, each taken on where it falls short on that ground as
       the screen shows them */
    for (int i = 0; i < 4; i++)
    {
        rgb[0] = RGB_UNPACK_RED(*role[i]);
        rgb[1] = RGB_UNPACK_GREEN(*role[i]);
        rgb[2] = RGB_UNPACK_BLUE(*role[i]);
        readable_on(rgb, ground, i == 0 ? TONE_TITLE_CONTRAST : TONE_FILL_CONTRAST, rgb);
        *role[i] = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    }
    palette->format_half = over_background(rgb, ground, 128);
    palette->format_quarter = over_background(rgb, ground, 64);

    /* the status line: Classic's shade of the ground, flat, taken on from
       its ink until that reads on it as shown */
    const int away = header_ink[0] ? 0 : 255;
    memcpy(shade, screen_colours.header_background, sizeof(shade));
    for (int n = 0; n < 100; n++)
    {
        as_shown(shade, shown);
        if (contrast(shown, header_ink) >= TONE_FILL_CONTRAST)
            break;
        for (int i = 0; i < 3; i++)
            shade[i] += (away - shade[i]) * 25 / 1000
                      + (away > shade[i] ? 1 : away < shade[i] ? -1 : 0);
    }
    as_shown(shade, shown);
    const unsigned *const text[2] = { &palette->ink, &palette->secondary };
    unsigned *const on_shade[2] = { &tone_fill.status_ink, &tone_fill.status_secondary };
    for (int i = 0; i < 2; i++)
    {
        rgb[0] = RGB_UNPACK_RED(*text[i]);
        rgb[1] = RGB_UNPACK_GREEN(*text[i]);
        rgb[2] = RGB_UNPACK_BLUE(*text[i]);
        readable_on(rgb, shown, TONE_FILL_CONTRAST, rgb);
        *on_shade[i] = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    }

    const int *const fill = screen_colours.flat_fill, *const track = screen_colours.flat_track;
    tone_fill.bar = LCD_RGBPACK(fill[0], fill[1], fill[2]);
    /* a mark that is seen, not read: 2.5 to 1 */
    contrasting_ink(ground, header_ink[0] != 0, 45, 250, rgb);
    tone_fill.quiet = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);

    surfaces->ground = palette->background;
    surfaces->status = LCD_RGBPACK(shade[0], shade[1], shade[2]);
    /* the bar's track, Classic's step from the ground: hairlines and a
       missing cover's tile too */
    surfaces->track = LCD_RGBPACK(track[0], track[1], track[2]);
    surfaces->note_top = palette->secondary;
    surfaces->note_foot = tone_fill.quiet;
    /* the outline of a small star not set: a line of a pixel */
    surfaces->outline = palette->muted;
}

/* Text and Stars in a theme with a tone: Classic's roles of the text in the
   cover's hue, each as clear of the theme's ground (in its user's colour) as
   Classic has it of white or of the dark ground. Its stars are pictures
   with their shape in their alpha: cover_palette_mark_colour() */
static void make_tone_text_palette(struct cover_palette *palette, int hue)
{
    const unsigned packed = ui_colour(UI_COLOUR_GROUND);
    const int ground[3] = { RGB_UNPACK_RED(packed), RGB_UNPACK_GREEN(packed),
                            RGB_UNPACK_BLUE(packed) };
    /* saturation and lightness of the ink, the second, the muted and the
       format's, light then dark: make_text_palette()'s */
    static const short roles[2][4][2] = {
        { { 652, 280 }, { 420, 350 }, { 285, 400 }, { 588, 300 } },
        { { 652, 720 }, { 420, 720 }, { 285, 620 }, { 588, 760 } },
    };
    const short (*const role)[2] = roles[global_settings.dark_theme];
    unsigned *const out[4] = { &palette->ink, &palette->secondary, &palette->muted,
                               &palette->format };
    int rgb[3];

    palette->whole_screen = false;
    screen_colours.fill = false;
    for (int i = 0; i < 4; i++)
    {
        hsl_to_rgb(hue, role[i][0], role[i][1], rgb);
        /* the title's and the lyrics' 7 to 1, the rest 4.5: of a saturated
           colour contrast() reads up to 0.07 over the exact ratio there, 0.2
           at 7 to 1 */
        readable_on(rgb, ground, i == 0 ? TONE_TITLE_CONTRAST : TONE_TEXT_CONTRAST, rgb);
        *out[i] = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    }
    palette->format_half = over_background(rgb, ground, 128);
    palette->format_quarter = over_background(rgb, ground, 64);
}

static struct cover_palette palette;
static bool active = false;
/* a theme's tone is the cover's now, not its user's */
static bool tone_shown;
static int palette_handle = -1;
static int palette_mode = COVER_COLOURS_OFF;
static uint32_t palette_colour; /* what it's from, while active */
static long gone_since;
static bool cover_gone = false;

static bool set_inactive(void)
{
    const bool toned = tone_shown;
    tone_shown = false;
    if (toned)
        ui_theme_cover_tone(-1);
    if (!active)
        return toned;
    active = false;
    /* off the tinted one, if it's up */
    if (tinted_backdrop_id >= 0 && lcd_get_backdrop() == buffers->backdrop)
        skin_backdrop_show(tinted_backdrop_id);
    return true;
}

void cover_palette_set_mode(int mode)
{
    mode = ui_theme_cover_colours(mode); /* the setting's callback gives its value */
    palette_handle = -1;
    /* a theme with a tone draws no backdrop in the cover's colours */
    if (ui_theme_current()->cover_tone)
        mode = MIN(mode, COVER_COLOURS_TEXT);
    if (mode != buffers_mode)
        buffers_refused = false;
    buffers_mode = mode;
    if (mode >= COVER_COLOURS_SCREEN)
        allocate_buffers();
    else if (buffers)
    {
        set_inactive();
        tinted_backdrop_id = indexed_backdrop_id = -1;
        core_free(buffers_handle);
        buffers = NULL;
    }
}

bool cover_palette_update(int art_handle, uint32_t colour)
{
    int mode = ui_theme_cover_colours(global_settings.cover_colours);
    if (mode != palette_mode)
        palette_handle = -1;
    palette_mode = mode;
    if (mode == COVER_COLOURS_OFF)
    {
        palette_handle = -1;
        cover_gone = false;
        return set_inactive();
    }

    if (art_handle < 0 && art_handle != COVER_ART_BY_COLOUR)
    {
        if (!active && !tone_shown)
            return false;
        if (!cover_gone)
        {
            cover_gone = true;
            gone_since = current_tick;
        }
        else if (TIME_AFTER(current_tick, gone_since + KEPT_TIME))
        {
            palette_handle = -1;
            cover_gone = false;
            return set_inactive();
        }
        return false;
    }

    cover_gone = false;
    if (art_handle == palette_handle && art_handle != COVER_ART_BY_COLOUR)
        return false;

    /* a cover not from an album made ready at an import, as one in the file */
    if (colour == COVER_COLOUR_UNKNOWN)
    {
        struct bitmap *bmp;
        if (art_handle < 0 || bufgetdata(art_handle, 0, (void *)&bmp) <= 0)
            return false;
        colour = cover_palette_colour((const fb_data *)bmp->data, bmp->width, bmp->height);
    }

    /* the rest of an album */
    if ((active || tone_shown) && palette_handle != -1 && colour == palette_colour)
    {
        palette_handle = art_handle;
        return false;
    }

    /* without the memory for it, which was asked for when it was chosen */
    if (mode >= COVER_COLOURS_SCREEN && !buffers && !ui_theme_current()->cover_tone)
        mode = COVER_COLOURS_TEXT;

    static struct cover_palette next;
    const bool grey = colour & COVER_COLOUR_GREY;
    int hsl[3] = { 0, 0, 0 };
    if (!grey)
    {
        const int rgb[3] = { (colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff };
        rgb_to_hsl(rgb, hsl);
    }
    memset(&next, 0, sizeof(next));
    if (ui_theme_current()->cover_tone)
    {
        /* A theme with a tone, which takes no memory for any of these. Text
           and Stars: its surfaces in its user's colour, the text's roles in
           the cover's hue. Whole Screen: its surfaces in the cover's hue,
           the status line's too. A grey cover, and one nearly grey, leaves
           both as with Off.
           Fill: Classic's, all of it flat in the cover's own colour, a grey
           one's too, the status line a shade of it */
        palette_handle = art_handle;
        palette_colour = colour;
        if ((grey || tone_faint(colour)) && mode != COVER_COLOURS_FILL)
            return set_inactive();
        bool changed;
        if (mode == COVER_COLOURS_FILL)
        {
            struct ui_theme_fill surfaces;
            make_tone_fill_palette(&next, colour, grey, &surfaces);
            changed = !active || !tone_shown || memcmp(&next, &palette, sizeof(palette));
            palette = next;
            active = true;
            changed |= ui_theme_cover_fill(&surfaces);
            tone_shown = true;
        }
        else if (mode == COVER_COLOURS_TEXT)
        {
            changed = tone_shown;
            tone_shown = false;
            ui_theme_cover_tone(-1);
            make_tone_text_palette(&next, hsl[0]);
            changed |= !active || memcmp(&next, &palette, sizeof(palette));
            palette = next;
            active = true;
        }
        else
        {
            changed = active;
            active = false;
            changed |= ui_theme_cover_tone(hsl[0]) || !tone_shown;
            tone_shown = true;
        }
        return changed;
    }
    cpu_boost(true);
    if (mode == COVER_COLOURS_FILL)
        make_fill_palette(&next, colour, grey);
    else if (mode == COVER_COLOURS_SCREEN)
        make_screen_palette(&next, hsl[0], grey);
    else
        make_text_palette(&next, hsl[0], grey);
    cpu_boost(false);
    palette_handle = art_handle;
    palette_colour = colour;

    const bool changed = !active || memcmp(&next, &palette, sizeof(palette));
    palette = next;
    active = true;
    if (changed && palette.whole_screen)
    {
        memset(buffers->header_cache, 0xff, sizeof(buffers->header_cache));
        if (screen_colours.fill)
            fill_backdrop();
        else
            tint_backdrop();
        memset(buffers->tint_known, 0, sizeof(buffers->tint_known));
        bar_source = track_source = NULL;
        if (tinted_backdrop_id >= 0)
            skin_backdrop_show(tinted_backdrop_id);
    }
    return changed;
}

/* From a part of the backdrop with nothing in it, with the same dither */
#define PLAIN_BACKDROP_X 304
#define PLAIN_BACKDROP_Y 40

bool cover_palette_clear(struct screen *display, const struct viewport *vp)
{
    static fb_data line[LCD_WIDTH];

    if (!active || !palette.whole_screen || tinted_backdrop_id < 0)
        return false;
    for (int y = 0; y < vp->height; y++)
    {
        const fb_data *plain = buffers->backdrop + PLAIN_BACKDROP_X
            + (PLAIN_BACKDROP_Y + ((vp->y + y) & 3)) * LCD_WIDTH;
        for (int x = 0; x < vp->width; x++)
            line[x] = plain[(vp->x + x) & 3];
        display->bitmap_part(line, 0, 0, STRIDE(display->screen_type, vp->width, 1),
                             0, y, vp->width, 1);
    }
    return true;
}

fb_data *cover_palette_backdrop(int backdrop_id)
{
    return active && palette.whole_screen && backdrop_id == tinted_backdrop_id
           ? buffers->backdrop : NULL;
}

void cover_palette_leave(void)
{
    if (tone_shown)
    {
        set_inactive();
        palette_handle = -1;
    }
}

const struct cover_palette *cover_palette_get(void)
{
    return active ? &palette : NULL;
}

void cover_palette_theme_changed(void)
{
    set_inactive();
    palette_handle = -1;
    tinted_backdrop_id = indexed_backdrop_id = -1;
    bar_source = track_source = NULL;
}

bool cover_palette_in_status_bar(int y)
{
    return y < HEADER_HEIGHT;
}

/* what the status bar is at (x, y) of the screen */
static void status_bar_rgb(int x, int y, int rgb[3])
{
    x = MAX(0, MIN(x, LCD_WIDTH - 1));
    y = MAX(0, MIN(y, HEADER_HEIGHT - 1));
    unpack_rgb(buffers->backdrop[y * LCD_WIDTH + x], rgb);
}

/* How much of an icon's pixel is its ink rather than the light it stands on in
   the theme, of 256, as tint_header_rgb() sees it: its colour, and its dark parts
   with their smoothed edges */
static int icon_ink(const int rgb[3])
{
    const int hi = MAX(MAX(rgb[0], rgb[1]), rgb[2]), lo = MIN(MIN(rgb[0], rgb[1]), rgb[2]);
    const int sum = hi + lo, chroma = hi - lo;
    if (chroma > 25 && chroma * 1000 / (sum > 255 ? 510 - sum : sum) > 400)
        return 256;
    int luma = (54 * rgb[0] + 183 * rgb[1] + 19 * rgb[2]) >> 8;
    if (global_settings.dark_theme)
        luma = 255 - luma;
    return luma >= 191 ? 0 : MIN((191 - luma) * 256 / (191 - 70), 256);
}

void cover_palette_status_bar_contrast(const fb_data *src, int count, int x, int y,
                                       int *most)
{
    const fb_data transparent = FB_SCALARPACK(TRANSPARENT_COLOR);
    if (!active || !palette.whole_screen || y >= HEADER_HEIGHT)
        return;
    for (int i = 0; i < count; i++)
    {
        if (src[i] == transparent)
            continue;
        int rgb[3], back[3];
        unpack_rgb(src[i], rgb);
        if (icon_ink(rgb) < 256)
            continue;
        status_bar_rgb(x + i, y, back);
        *most = MAX(*most, contrast(rgb, back));
    }
}

void cover_palette_keep_ink(const fb_data *src, fb_data *dst, int count)
{
    const fb_data transparent = FB_SCALARPACK(TRANSPARENT_COLOR);
    for (int i = 0; i < count; i++)
    {
        if (src[i] == transparent)
            continue;
        int rgb[3], tinted[3];
        unpack_rgb(src[i], rgb);
        const int a = icon_ink(rgb);
        if (a == 0)
            continue;
        unpack_rgb(dst[i], tinted);
        for (int k = 0; k < 3; k++)
            rgb[k] = tinted[k] + (rgb[k] - tinted[k]) * a / 256;
        dst[i] = pack_nearest(rgb);
    }
}

unsigned cover_palette_text_colour_in(unsigned colour, const struct viewport *vp)
{
    /* a theme with a tone under Fill: what stands on the status line's shade */
    if (tone_shown && active && palette.whole_screen)
        return vp->y + vp->height <= ui_metric(UI_METRIC_STATUS_ROWS)
               ? cover_palette_mark_colour(colour, true)
               : cover_palette_text_colour(colour);
    if (global_settings.status_icons == STATUS_ICONS_OWN && active
        && palette.whole_screen && vp->y < HEADER_HEIGHT)
    {
        int rgb[3], back[3];
        rgb[0] = RGB_UNPACK_RED(colour);
        rgb[1] = RGB_UNPACK_GREEN(colour);
        rgb[2] = RGB_UNPACK_BLUE(colour);
        status_bar_rgb(vp->x + vp->width / 2, vp->y + vp->height / 2, back);
        if (contrast(rgb, back) >= 450)
            return colour;
    }
    return cover_palette_text_colour(colour);
}

unsigned cover_palette_text_colour(unsigned colour)
{
    const int green = RGB_UNPACK_GREEN(colour);
    if (global_settings.dark_theme)
    {
        if (green >= 0xe0)
            return palette.ink;
        if (green >= 0xa0)
            return palette.secondary;
        if (green >= 0x70)
            return palette.muted;
        return colour;
    }
    if (green < 0x40)
        return palette.ink;
    /* the album's and the 1 of N's grey, 767676, is muted */
    if (green < 0x70)
        return palette.secondary;
    if (green < 0xc0)
        return palette.muted;
    return colour;
}

unsigned cover_palette_mark_colour(unsigned colour, bool status)
{
    const int green = RGB_UNPACK_GREEN(colour);
    /* a star not set is known by the grey the look's table names it with */
    if (colour == ui_theme_look(global_settings.dark_theme)
                      ->colour[UI_COLOUR_RATING_UNSET])
        return ui_colour(UI_COLOUR_RATING_UNSET);
    if (!active)
        return colour;
    if (palette.whole_screen)
    {
        /* Fill: the theme's four greys, from the ink's to the palest, are
           the ink, the second text, the bar's fill and a quiet mark; the
           first two on the status line as they read on its shade */
        const int level = global_settings.dark_theme ? 0xff - green : green;
        if (level < 0x40)
            return status ? tone_fill.status_ink : palette.ink;
        if (level < 0x5c)
            return status ? tone_fill.status_secondary : palette.secondary;
        return (global_settings.dark_theme ? green >= 0x80 : green < 0x80)
               ? tone_fill.bar : tone_fill.quiet;
    }
    if (global_settings.dark_theme)
        return green >= 0xe0 ? palette.ink : green >= 0xa0 ? palette.secondary : colour;
    return green < 0x40 ? palette.ink : green < 0x70 ? palette.secondary : colour;
}

bool cover_palette_tone_filled(unsigned *bar)
{
    if (!tone_shown || !active || !palette.whole_screen)
        return false;
    if (bar)
        *bar = tone_fill.bar;
    return true;
}

unsigned cover_palette_dimmed(unsigned ink, unsigned dim, unsigned ground)
{
    int i[3] = { RGB_UNPACK_RED(ink), RGB_UNPACK_GREEN(ink), RGB_UNPACK_BLUE(ink) };
    int d[3] = { RGB_UNPACK_RED(dim), RGB_UNPACK_GREEN(dim), RGB_UNPACK_BLUE(dim) };
    int g[3] = { RGB_UNPACK_RED(ground), RGB_UNPACK_GREEN(ground),
                 RGB_UNPACK_BLUE(ground) };
    /* 3 to 1, but no nearer the line sung than a third less than its own */
    const int target = MIN(300, contrast(i, g) * 2 / 3);
    readable_on(d, g, target, d);
    /* and on the Fill screen against the ground as it's painted there, its 16
       bits the nearest: the one given has them cut, a level off in a fifth of
       its parts, and the lines came out as much as 8% short of 3 to 1 */
    if (active && palette.whole_screen && screen_colours.fill)
    {
        int painted[3];
        unpack_rgb(pack_nearest(screen_colours.background), painted);
        readable_on(d, painted, target, d);
    }
    return LCD_RGBPACK(d[0], d[1], d[2]);
}
