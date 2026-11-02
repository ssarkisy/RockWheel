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
} screen_colours;

/* the red of a nearly empty battery, a warning */
static bool is_warning_red(const int rgb[3])
{
    const int r = rgb[0], g = rgb[1], b = rgb[2];
    const int hi = MAX(MAX(r, g), b), lo = MIN(MIN(r, g), b);
    const int sum = hi + lo, chroma = hi - lo;
    return hi == r && chroma > 60 && ABS_DIFF(g, b) * 3 < chroma
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
           theme's images are the other way up. */
        const int level = MIN((global_settings.dark_theme ? 255 - luma : luma) * 255 / 236,
                              255);
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
    uint32_t scratch[512];
    fb_data bar[LCD_WIDTH * 32]; /* the fill of the progress bar */
} *buffers;
static int buffers_handle;
static const void *bar_source; /* of what's in it */

static int tinted_backdrop_id = -1;
static int indexed_backdrop_id = -1;
static int backdrop_colour_count;

/* The audio buffer gives way to them, which starts a playing track again where it was,
   but leaves a paused one stopped */
static void allocate_buffers(void)
{
    if (buffers)
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
    if (paused && !(audio_status() & AUDIO_STATUS_PLAY))
    {
        audio_play(elapsed, offset);
        audio_pause();
    }
}

static void tint_header_line(const fb_data *src, fb_data *dst, int count);

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

void *cover_palette_scratch(size_t *size)
{
    *size = sizeof(buffers->scratch);
    return buffers->scratch;
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

/* in an accent's colour, with the highlights and shadows of the theme's colour */
static void accent_shade(int luma, const int accent[3], int out[3])
{
    const int centre = MAX(MIN(accent[2], 820), 160);
    const int lightness = centre + (luma * 1000 / 255 - 600) * 115 / 100;
    hsl_to_rgb(accent[0], accent[1], MAX(MIN(lightness, 980), 25), out);
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

    /* with a battery in it, the theme's for a full one, as an icon like the rest */
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

    for (int y = 0; y < BAR_HEIGHT; y++)
    {
        const fb_data *from = &src[(BAR_Y + y) * LCD_WIDTH + BAR_X];
        fb_data *to = &b->backdrop[(BAR_Y + y) * LCD_WIDTH + BAR_X];
        if (!global_settings.dark_theme)
            memcpy(to, from, BAR_WIDTH * sizeof(fb_data));
        /* light on the colour in both themes */
        else for (int x = 0; x < BAR_WIDTH; x++)
        {
            int rgb[3];
            unpack_rgb(from[x], rgb);
            const int grey = MIN(255 - rgb[1] + 20, 255);
            rgb[0] = rgb[1] = rgb[2] = grey;
            to[x] = pack_nearest(rgb);
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

void *cover_palette_bar(const struct bitmap *bm)
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

    const int rows = MIN(bm->height, BAR_HEIGHT);
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
            accent_shade((54 * rgb[0] + 183 * rgb[1] + 19 * rgb[2]) >> 8,
                         screen_colours.bar_hsl, out);
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
        palette->ink = LCD_RGBPACK(high[0], high[1], high[2]);
        hsl_to_rgb(hue, s * 200, 760, rgb);
        palette->secondary = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
        hsl_to_rgb(hue, s * 160, 640, rgb);
        palette->muted = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
        hsl_to_rgb(hue, s * 220, 860, rgb);
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
    palette->ink = LCD_RGBPACK(screen_colours.ink[0], screen_colours.ink[1],
                               screen_colours.ink[2]);
    hsl_to_rgb(hue, s * 180, 330, rgb);
    palette->secondary = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    hsl_to_rgb(hue, s * 140, 380, rgb);
    palette->muted = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    hsl_to_rgb(hue, s * 220, 200, rgb);
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

static struct cover_palette palette;
static bool active = false;
static int palette_handle = -1;
static int palette_mode = COVER_COLOURS_OFF;
static uint32_t palette_colour; /* what it's from, while active */
static long gone_since;
static bool cover_gone = false;

static bool set_inactive(void)
{
    if (!active)
        return false;
    active = false;
    /* off the tinted one, if it's up */
    if (tinted_backdrop_id >= 0 && lcd_get_backdrop() == buffers->backdrop)
        skin_backdrop_show(tinted_backdrop_id);
    return true;
}

void cover_palette_set_mode(int mode)
{
    palette_handle = -1;
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
    int mode = global_settings.cover_colours;
    if (mode != palette_mode)
        palette_handle = -1;
    palette_mode = mode;
    if (mode == COVER_COLOURS_OFF)
    {
        palette_handle = -1;
        cover_gone = false;
        return set_inactive();
    }

    if (art_handle < 0)
    {
        if (!active)
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
    if (art_handle == palette_handle)
        return false;

    /* a cover not from an album made ready at an import, as one in the file */
    if (colour == COVER_COLOUR_UNKNOWN)
    {
        struct bitmap *bmp;
        if (bufgetdata(art_handle, 0, (void *)&bmp) <= 0)
            return false;
        colour = cover_palette_colour((const fb_data *)bmp->data, bmp->width, bmp->height);
    }

    /* the rest of an album */
    if (active && palette_handle >= 0 && colour == palette_colour)
    {
        palette_handle = art_handle;
        return false;
    }

    /* if there was no memory for it when it was chosen */
    if (mode >= COVER_COLOURS_SCREEN)
    {
        allocate_buffers();
        if (!buffers)
            mode = COVER_COLOURS_TEXT;
    }

    static struct cover_palette next;
    const bool grey = colour & COVER_COLOUR_GREY;
    int hsl[3] = { 0, 0, 0 };
    if (!grey)
    {
        const int rgb[3] = { (colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff };
        rgb_to_hsl(rgb, hsl);
    }
    memset(&next, 0, sizeof(next));
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
        bar_source = NULL;
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

const struct cover_palette *cover_palette_get(void)
{
    return active ? &palette : NULL;
}

void cover_palette_theme_changed(void)
{
    set_inactive();
    palette_handle = -1;
    tinted_backdrop_id = indexed_backdrop_id = -1;
    bar_source = NULL;
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
    if (green < 0x78)
        return palette.secondary;
    if (green < 0xc0)
        return palette.muted;
    return colour;
}
