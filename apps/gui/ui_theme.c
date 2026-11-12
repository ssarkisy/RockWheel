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
#include "lcd.h"
#include "font.h"
#include "settings.h"
#include "ui_theme.h"

#ifdef HAVE_LCD_COLOR
#define RGB(r, g, b) LCD_RGBPACK(r, g, b)
#else
#define RGB(r, g, b) 0
#endif

static const struct ui_theme ui_themes[] = {
    { /* Classic */
        .look = {
            {
                .skin = UI_CLASSIC_SKIN,
                .backdrop = UI_CLASSIC_BACKDROP,
                .colour = {
                    [UI_COLOUR_TEXT]            = RGB(0x00, 0x00, 0x00),
                    [UI_COLOUR_GROUND]          = RGB(0xff, 0xff, 0xff),
                    [UI_COLOUR_GROUP_TEXT]      = RGB(0x60, 0x66, 0x70),
                    [UI_COLOUR_GROUP_BAR]       = RGB(0xe6, 0xe8, 0xec),
                    [UI_COLOUR_VALUE]           = RGB(0x66, 0x66, 0x66),
                    [UI_COLOUR_VALUE_SELECTED]  = RGB(0xff, 0xff, 0xff),
                    [UI_COLOUR_INITIAL_BOX]     = RGB(0x00, 0x00, 0x00),
                    /* the scrollbar's, measured on a stock 5G */
                    [UI_COLOUR_SCROLLBAR_EDGE]  = RGB(0x5a, 0x61, 0x63),
                    [UI_COLOUR_SEARCH_SECOND]   = RGB(0x66, 0x66, 0x66),
                    [UI_COLOUR_SEARCH_FIELD]    = RGB(0xe6, 0xe8, 0xec),
                    [UI_COLOUR_SEARCH_RULE]     = RGB(0xd4, 0xd6, 0xda),
                    [UI_COLOUR_SEARCH_DIM]      = RGB(0xc8, 0xca, 0xce),
                },
                .scrollbar_track = { { 0xc6, 0xcf, 0xd6 }, { 0xef, 0xf7, 0xff } },
                .initial_opacity = 11,
            },
            {
                .skin = UI_CLASSIC_SKIN_DARK,
                .backdrop = UI_CLASSIC_BACKDROP_DARK,
                .colour = {
                    [UI_COLOUR_TEXT]            = RGB(0xf2, 0xf2, 0xf6),
                    [UI_COLOUR_GROUND]          = RGB(UI_CLASSIC_DARK_GROUND_R,
                                                      UI_CLASSIC_DARK_GROUND_G,
                                                      UI_CLASSIC_DARK_GROUND_B),
                    [UI_COLOUR_GROUP_TEXT]      = RGB(0xa0, 0xa6, 0xb0),
                    [UI_COLOUR_GROUP_BAR]       = RGB(0x2c, 0x2e, 0x33),
                    [UI_COLOUR_VALUE]           = RGB(0xa0, 0xa2, 0xa8),
                    [UI_COLOUR_VALUE_SELECTED]  = RGB(0xff, 0xff, 0xff),
                    /* lighter than the dark list, over whose black a dark
                       box didn't show */
                    [UI_COLOUR_INITIAL_BOX]     = RGB(0x5a, 0x5c, 0x64),
                    [UI_COLOUR_SCROLLBAR_EDGE]  = RGB(0x5a, 0x61, 0x63),
                    [UI_COLOUR_SEARCH_SECOND]   = RGB(0xa0, 0xa2, 0xa8),
                    [UI_COLOUR_SEARCH_FIELD]    = RGB(0x2c, 0x2e, 0x33),
                    [UI_COLOUR_SEARCH_RULE]     = RGB(0x3a, 0x3c, 0x42),
                    [UI_COLOUR_SEARCH_DIM]      = RGB(0x4a, 0x4c, 0x52),
                },
                .scrollbar_track = { { 0x2a, 0x2d, 0x33 }, { 0x3c, 0x40, 0x48 } },
                .initial_opacity = 13,
            },
        },
        .font = {
            [UI_FONT_LIST]          = FONT_DIR "/" UI_CLASSIC_FONT ".fnt",
            [UI_FONT_HINT]          = FONT_DIR "/18-Inter-SemiBold.fnt",
            [UI_FONT_LEAD]          = FONT_DIR "/17-Inter-SemiBold.fnt",
            [UI_FONT_VALUE]         = FONT_DIR "/17-Inter-Medium.fnt",
            [UI_FONT_SUBLINE]       = FONT_DIR "/14-Inter-SemiBold.fnt",
            [UI_FONT_HEADER_ARTIST] = FONT_DIR "/17-Inter-Bold.fnt",
            [UI_FONT_HEADER_COUNTS] = FONT_DIR "/14-Inter-Bold.fnt",
            [UI_FONT_INITIAL]       = FONT_DIR "/53-Inter-Letters.fnt",
        },
        .metric = {
            [UI_METRIC_ROW_HEIGHT]           = 0,
            [UI_METRIC_TEXT_INSET]           = UI_CLASSIC_TEXT_INSET,
            [UI_METRIC_INITIAL_HEIGHT]       = 72,
            [UI_METRIC_INITIAL_MARGIN]       = 16,
            [UI_METRIC_CHEVRON_MARGIN]       = 6,
            [UI_METRIC_LEAD_GAP]             = 8,
            [UI_METRIC_VALUE_MARGIN]         = 6,
            [UI_METRIC_VALUE_MARGIN_CHEVRON] = 21,
            [UI_METRIC_VALUE_GAP]            = 8,
            [UI_METRIC_HINT_X]               = 6,
            [UI_METRIC_HINT_TOP]             = 6,
            [UI_METRIC_HINT_PITCH]           = 21,
            [UI_METRIC_NAME_PART_CUT]        = 5,
            [UI_METRIC_SUBLINE_GAP]          = 3,
            [UI_METRIC_NAME_TEXT_DY]         = 2,
            [UI_METRIC_SUBLINE_TOP_DY]       = 0,
            [UI_METRIC_ROW_COVER_GAP]        = 8,
            [UI_METRIC_HEADER_COVER_TOP]     = 6,
            [UI_METRIC_HEADER_COVER_GAP]     = 10,
            [UI_METRIC_HEADER_TEXT_MARGIN]   = 6,
            [UI_METRIC_HEADER_NAME_TOP]      = 5,
            [UI_METRIC_HEADER_NAME_LEADING]  = 2,
            [UI_METRIC_HEADER_ARTIST_GAP]    = 1,
            [UI_METRIC_HEADER_COUNTS_GAP]    = 2,
            [UI_METRIC_HEADER_FOOT]          = 6,
        },
        .scrollbar_thumb = {
            RGB(0x29, 0x51, 0xad), RGB(0x4a, 0x8e, 0xd6),
            RGB(0xa5, 0xc7, 0xef), RGB(0x94, 0xbe, 0xef),
            RGB(0x63, 0xa2, 0xe7), RGB(0x6e, 0xac, 0xf7),
            RGB(0x84, 0xc3, 0xff), RGB(0x9a, 0xdb, 0xff),
            RGB(0xad, 0xf4, 0xff), RGB(0x5a, 0x71, 0x7b),
        },
    },
};

const struct ui_theme *ui_theme_current(void)
{
    return &ui_themes[0];
}

const struct ui_theme_look *ui_theme_look(bool dark)
{
    return &ui_theme_current()->look[dark];
}

unsigned ui_colour(enum ui_colour role)
{
    return ui_theme_look(global_settings.dark_theme)->colour[role];
}

int ui_metric(enum ui_metric metric)
{
    if (metric == UI_METRIC_INITIAL_OPACITY)
        return ui_theme_look(global_settings.dark_theme)->initial_opacity;
    return ui_theme_current()->metric[metric];
}

const char *ui_font_file(enum ui_font role)
{
    return ui_theme_current()->font[role];
}
