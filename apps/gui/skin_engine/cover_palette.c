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
#include "buffering.h"
#include "settings.h"
#include "skin_engine.h"
#include "cover_palette.h"

#define HUE_BINS 36
#define HUE_BIN_WIDTH 10

/* How long a cover that has gone keeps its colours, for the next one to come */
#define KEPT_TIME HZ

struct hue_bin
{
    uint32_t weight, r, g, b;
};

/* The largest family of colours of the cover, by a circular histogram of hues, as
   this favours it over small bright details. False if it's grey. */
static bool cover_hue(const fb_data *pixels, int width, int height, int *hue)
{
    static struct hue_bin bins[HUE_BINS];
    uint32_t count = 0;

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
    if (weight * 5 < count * 32)
        return false;

    r /= weight;
    g /= weight;
    b /= weight;
    const int hi = MAX(MAX(r, g), b), lo = MIN(MIN(r, g), b), chroma = hi - lo;
    if (chroma == 0)
        return false;
    if (hi == (int)r)
        *hue = 60 * ((int)g - (int)b) / chroma + ((int)g < (int)b ? 360 : 0);
    else if (hi == (int)g)
        *hue = 60 * ((int)b - (int)r) / chroma + 120;
    else
        *hue = 60 * ((int)r - (int)g) / chroma + 240;
    return true;
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

/* of a channel, out of 65536 */
static uint32_t linear_light(int channel)
{
    const uint32_t x = channel * 257;
    const uint32_t x2 = (x * x) >> 16, x3 = (x2 * x) >> 16;
    return ((821 * x) >> 16) + ((44706 * x2) >> 16) + ((20009 * x3) >> 16);
}

/* As light as this lightness, but for a contrast of 4.5 on white */
static void readable_colour(int hue, int saturation, int lightness, int rgb[3])
{
    for (;;)
    {
        hsl_to_rgb(hue, saturation, lightness, rgb);
        const uint32_t luminance = ((13933 * linear_light(rgb[0])) >> 16)
                                 + ((46871 * linear_light(rgb[1])) >> 16)
                                 + ((4732 * linear_light(rgb[2])) >> 16);
        if (luminance <= 12015 || lightness <= 50)
            return;
        lightness -= 5;
    }
}

/* c of 256 of the colour over white */
static unsigned over_white(const int rgb[3], int c)
{
    return LCD_RGBPACK(255 - (255 - rgb[0]) * c / 256,
                       255 - (255 - rgb[1]) * c / 256,
                       255 - (255 - rgb[2]) * c / 256);
}

static void make_palette(struct cover_palette *palette, int hue, bool grey)
{
    const int s = grey ? 0 : 1;
    int ink[3], rgb[3];

    readable_colour(hue, s * 652, 280, ink);
    palette->ink = LCD_RGBPACK(ink[0], ink[1], ink[2]);
    readable_colour(hue, s * 420, 350, rgb);
    palette->secondary = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    readable_colour(hue, s * 285, 400, rgb);
    palette->muted = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    readable_colour(hue, s * 588, 300, rgb);
    palette->format = LCD_RGBPACK(rgb[0], rgb[1], rgb[2]);
    palette->format_half = over_white(rgb, 128);
    palette->format_quarter = over_white(rgb, 64);

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
static long gone_since;
static bool cover_gone = false;

static void set_active(bool now)
{
    if (active != now)
        skin_request_full_update(WPS);
    active = now;
}

void cover_palette_update(int art_handle)
{
    if (!global_settings.cover_colours)
    {
        palette_handle = -1;
        cover_gone = false;
        set_active(false);
        return;
    }

    if (art_handle < 0)
    {
        if (!active)
            return;
        if (!cover_gone)
        {
            cover_gone = true;
            gone_since = current_tick;
        }
        else if (TIME_AFTER(current_tick, gone_since + KEPT_TIME))
        {
            palette_handle = -1;
            cover_gone = false;
            set_active(false);
        }
        return;
    }

    cover_gone = false;
    if (art_handle == palette_handle)
        return;

    struct bitmap *bmp;
    if (bufgetdata(art_handle, 0, (void *)&bmp) <= 0)
        return;

    static struct cover_palette next;
    int hue = 0;
    const bool grey = !cover_hue((const fb_data *)bmp->data, bmp->width, bmp->height, &hue);
    make_palette(&next, hue, grey);
    palette_handle = art_handle;

    if (!active || memcmp(&next, &palette, sizeof(palette)))
    {
        palette = next;
        skin_request_full_update(WPS);
    }
    active = true;
}

const struct cover_palette *cover_palette_get(void)
{
    return active ? &palette : NULL;
}

unsigned cover_palette_text_colour(unsigned colour)
{
    const int green = RGB_UNPACK_GREEN(colour);
    if (green < 0x40)
        return palette.ink;
    if (green < 0x78)
        return palette.secondary;
    if (green < 0xc0)
        return palette.muted;
    return colour;
}
