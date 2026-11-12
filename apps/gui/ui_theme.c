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
#include <string.h>
#include "string-extra.h"
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
                    [UI_COLOUR_SELECTOR_START]  = UI_CLASSIC_SELECTOR_START,
                    [UI_COLOUR_SELECTOR_END]    = UI_CLASSIC_SELECTOR_END,
                    [UI_COLOUR_SELECTOR_TEXT]   = UI_CLASSIC_SELECTOR_TEXT,
                    [UI_COLOUR_QUIET]           = RGB(0x66, 0x66, 0x66),
                    [UI_COLOUR_LEAD]            = RGB(0x66, 0x66, 0x66),
                    [UI_COLOUR_HINT_TEXT]       = RGB(0x60, 0x66, 0x70),
                    [UI_COLOUR_HINT_BAND]       = RGB(0xe6, 0xe8, 0xec),
                    [UI_COLOUR_SCROLLBAR_THUMB] = RGB(0x5a, 0x61, 0x63),
                    [UI_COLOUR_INITIAL_TEXT]    = RGB(0xff, 0xff, 0xff),
                    [UI_COLOUR_COVER_TILE]      = RGB(0xe6, 0xe8, 0xec),
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
                    [UI_COLOUR_SELECTOR_START]  = UI_CLASSIC_SELECTOR_START,
                    [UI_COLOUR_SELECTOR_END]    = UI_CLASSIC_SELECTOR_END,
                    [UI_COLOUR_SELECTOR_TEXT]   = UI_CLASSIC_SELECTOR_TEXT,
                    [UI_COLOUR_QUIET]           = RGB(0xa0, 0xa2, 0xa8),
                    [UI_COLOUR_LEAD]            = RGB(0xa0, 0xa2, 0xa8),
                    [UI_COLOUR_HINT_TEXT]       = RGB(0xa0, 0xa6, 0xb0),
                    [UI_COLOUR_HINT_BAND]       = RGB(0x2c, 0x2e, 0x33),
                    [UI_COLOUR_SCROLLBAR_THUMB] = RGB(0x5a, 0x61, 0x63),
                    [UI_COLOUR_INITIAL_TEXT]    = RGB(0xff, 0xff, 0xff),
                    [UI_COLOUR_COVER_TILE]      = RGB(0x2c, 0x2e, 0x33),
                },
                .scrollbar_track = { { 0x2a, 0x2d, 0x33 }, { 0x3c, 0x40, 0x48 } },
                .initial_opacity = 13,
            },
        },
        .list_font = UI_CLASSIC_FONT,
        .selector_bevel = true,
        .cover_colours = true,
        .font = {
            [UI_FONT_LIST]          = FONT_DIR "/" UI_CLASSIC_FONT ".fnt",
            [UI_FONT_HINT]          = FONT_DIR "/18-Inter-SemiBold.fnt",
            [UI_FONT_LEAD]          = FONT_DIR "/17-Inter-SemiBold.fnt",
            [UI_FONT_VALUE]         = FONT_DIR "/17-Inter-Medium.fnt",
            [UI_FONT_SUBLINE]       = FONT_DIR "/14-Inter-SemiBold.fnt",
            [UI_FONT_HEADER_ARTIST] = FONT_DIR "/17-Inter-Bold.fnt",
            [UI_FONT_HEADER_COUNTS] = FONT_DIR "/14-Inter-Bold.fnt",
            [UI_FONT_MENU_VALUE]    = FONT_DIR "/" UI_CLASSIC_FONT ".fnt",
            [UI_FONT_GROUP]         = FONT_DIR "/" UI_CLASSIC_FONT ".fnt",
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
            [UI_METRIC_CHEVRON_ALL]          = 0,
            [UI_METRIC_CHEVRON_ON_PLAY]      = 1,
            [UI_METRIC_GROUP_BAR]            = 1,
        },
        .scrollbar_thumb = {
            RGB(0x29, 0x51, 0xad), RGB(0x4a, 0x8e, 0xd6),
            RGB(0xa5, 0xc7, 0xef), RGB(0x94, 0xbe, 0xef),
            RGB(0x63, 0xa2, 0xe7), RGB(0x6e, 0xac, 0xf7),
            RGB(0x84, 0xc3, 0xff), RGB(0x9a, 0xdb, 0xff),
            RGB(0xad, 0xf4, 0xff), RGB(0x5a, 0x71, 0x7b),
        },
    },
    { /* Neo: the colours its mockup's specification gives as the screen has
         them; rows of 30 px with a flat selection. Its geometry is still
         Classic's where the table has no metric for it */
        .look = {
            {
                .skin = "Neo",
                .backdrop = "-",
                .colour = {
                    [UI_COLOUR_TEXT]            = RGB(0x29, 0x24, 0x21),
                    [UI_COLOUR_GROUND]          = RGB(0xef, 0xeb, 0xe7),
                    [UI_COLOUR_GROUP_TEXT]      = RGB(0x6b, 0x69, 0x63),
                    [UI_COLOUR_GROUP_BAR]       = RGB(0xde, 0xdb, 0xd6),
                    [UI_COLOUR_VALUE]           = RGB(0x6b, 0x69, 0x63),
                    /* darker than the second grey: 4.7 to 1 on the selected row */
                    [UI_COLOUR_VALUE_SELECTED]  = RGB(0x63, 0x5d, 0x5a),
                    [UI_COLOUR_INITIAL_BOX]     = RGB(0x29, 0x24, 0x21),
                    [UI_COLOUR_SCROLLBAR_EDGE]  = RGB(0xef, 0xeb, 0xe7),
                    [UI_COLOUR_SEARCH_SECOND]   = RGB(0x6b, 0x69, 0x63),
                    [UI_COLOUR_SEARCH_FIELD]    = RGB(0xde, 0xdb, 0xd6),
                    [UI_COLOUR_SEARCH_RULE]     = RGB(0xe7, 0xdf, 0xd6),
                    [UI_COLOUR_SEARCH_DIM]      = RGB(0xa5, 0x9e, 0x94),
                    [UI_COLOUR_SELECTOR_START]  = RGB(0xde, 0xdb, 0xd6),
                    [UI_COLOUR_SELECTOR_END]    = RGB(0xde, 0xdb, 0xd6),
                    [UI_COLOUR_SELECTOR_TEXT]   = RGB(0x29, 0x24, 0x21),
                    [UI_COLOUR_QUIET]           = RGB(0xa5, 0x9e, 0x94),
                    [UI_COLOUR_LEAD]            = RGB(0x6b, 0x69, 0x63),
                    [UI_COLOUR_HINT_TEXT]       = RGB(0x6b, 0x69, 0x63),
                    [UI_COLOUR_HINT_BAND]       = RGB(0xef, 0xeb, 0xe7),
                    [UI_COLOUR_SCROLLBAR_THUMB] = RGB(0xa5, 0x9e, 0x94),
                    [UI_COLOUR_INITIAL_TEXT]    = RGB(0xef, 0xeb, 0xe7),
                    [UI_COLOUR_COVER_TILE]      = RGB(0xe7, 0xdf, 0xd6),
                },
                .scrollbar_track = { { 0xef, 0xeb, 0xe7 }, { 0xef, 0xeb, 0xe7 } },
                .initial_opacity = 11,
            },
            {
                .skin = "Neo-Dark",
                .backdrop = "-",
                .colour = {
                    [UI_COLOUR_TEXT]            = RGB(0xef, 0xeb, 0xe7),
                    [UI_COLOUR_GROUND]          = RGB(0x21, 0x1c, 0x18),
                    [UI_COLOUR_GROUP_TEXT]      = RGB(0xa5, 0x9e, 0x94),
                    [UI_COLOUR_GROUP_BAR]       = RGB(0x31, 0x2c, 0x29),
                    [UI_COLOUR_VALUE]           = RGB(0xa5, 0x9e, 0x94),
                    [UI_COLOUR_VALUE_SELECTED]  = RGB(0xa5, 0x9e, 0x94),
                    [UI_COLOUR_INITIAL_BOX]     = RGB(0x6b, 0x69, 0x63),
                    [UI_COLOUR_SCROLLBAR_EDGE]  = RGB(0x21, 0x1c, 0x18),
                    [UI_COLOUR_SEARCH_SECOND]   = RGB(0xa5, 0x9e, 0x94),
                    [UI_COLOUR_SEARCH_FIELD]    = RGB(0x31, 0x2c, 0x29),
                    [UI_COLOUR_SEARCH_RULE]     = RGB(0x31, 0x30, 0x29),
                    [UI_COLOUR_SEARCH_DIM]      = RGB(0x6b, 0x69, 0x63),
                    [UI_COLOUR_SELECTOR_START]  = RGB(0x31, 0x2c, 0x29),
                    [UI_COLOUR_SELECTOR_END]    = RGB(0x31, 0x2c, 0x29),
                    [UI_COLOUR_SELECTOR_TEXT]   = RGB(0xef, 0xeb, 0xe7),
                    [UI_COLOUR_QUIET]           = RGB(0x6b, 0x69, 0x63),
                    [UI_COLOUR_LEAD]            = RGB(0xa5, 0x9e, 0x94),
                    [UI_COLOUR_HINT_TEXT]       = RGB(0xa5, 0x9e, 0x94),
                    [UI_COLOUR_HINT_BAND]       = RGB(0x21, 0x1c, 0x18),
                    [UI_COLOUR_SCROLLBAR_THUMB] = RGB(0x6b, 0x69, 0x63),
                    [UI_COLOUR_INITIAL_TEXT]    = RGB(0xef, 0xeb, 0xe7),
                    [UI_COLOUR_COVER_TILE]      = RGB(0x31, 0x30, 0x29),
                },
                .scrollbar_track = { { 0x21, 0x1c, 0x18 }, { 0x21, 0x1c, 0x18 } },
                .initial_opacity = 13,
            },
        },
        .list_font = "19-Inter-Medium",
        .selector_bevel = false,
        .cover_colours = false,
        .font = {
            [UI_FONT_LIST]          = FONT_DIR "/19-Inter-Medium.fnt",
            [UI_FONT_HINT]          = FONT_DIR "/15-Inter-Regular.fnt",
            [UI_FONT_LEAD]          = FONT_DIR "/15-Inter-Regular.fnt",
            [UI_FONT_VALUE]         = FONT_DIR "/15-Inter-Regular.fnt",
            [UI_FONT_SUBLINE]       = FONT_DIR "/15-Inter-Regular.fnt",
            [UI_FONT_HEADER_ARTIST] = FONT_DIR "/17-Inter-Regular.fnt",
            [UI_FONT_HEADER_COUNTS] = FONT_DIR "/15-Inter-Regular.fnt",
            [UI_FONT_MENU_VALUE]    = FONT_DIR "/17-Inter-Regular.fnt",
            [UI_FONT_GROUP]         = FONT_DIR "/14-Inter-Medium.fnt",
            [UI_FONT_INITIAL]       = FONT_DIR "/53-Inter-Letters.fnt",
        },
        .metric = {
            [UI_METRIC_ROW_HEIGHT]           = 30,
            [UI_METRIC_TEXT_INSET]           = 16,
            [UI_METRIC_INITIAL_HEIGHT]       = 72,
            [UI_METRIC_INITIAL_MARGIN]       = 16,
            [UI_METRIC_CHEVRON_MARGIN]       = 14,
            [UI_METRIC_LEAD_GAP]             = 8,
            /* a value ends at 300, or at 290 before a chevron */
            [UI_METRIC_VALUE_MARGIN]         = 20,
            [UI_METRIC_VALUE_MARGIN_CHEVRON] = 30,
            [UI_METRIC_VALUE_GAP]            = 10,
            [UI_METRIC_HINT_X]               = 16,
            [UI_METRIC_HINT_TOP]             = 6,
            [UI_METRIC_HINT_PITCH]           = 16,
            /* 36 px: the name's part its whole cell of 19, so that nothing of
               a g or a y is cut, its baseline 15 down; the second line's 15
               under it, its baseline 31 down, and two rows */
            [UI_METRIC_NAME_PART_CUT]        = 0,
            [UI_METRIC_SUBLINE_GAP]          = 2,
            [UI_METRIC_NAME_TEXT_DY]         = 0,
            [UI_METRIC_SUBLINE_TOP_DY]       = 0,
            [UI_METRIC_ROW_COVER_GAP]        = 8,
            /* the cover 60 px from the row's inset, the texts at 88 */
            [UI_METRIC_HEADER_COVER_TOP]     = 6,
            [UI_METRIC_HEADER_COVER_GAP]     = 12,
            [UI_METRIC_HEADER_TEXT_MARGIN]   = 16,
            [UI_METRIC_HEADER_NAME_TOP]      = 10,
            [UI_METRIC_HEADER_NAME_LEADING]  = 0,
            [UI_METRIC_HEADER_ARTIST_GAP]    = 2,
            [UI_METRIC_HEADER_COUNTS_GAP]    = 0,
            [UI_METRIC_HEADER_FOOT]          = 6,
            [UI_METRIC_HEADER_COVER_RADIUS]  = 5,
            [UI_METRIC_SELECTION_RADIUS]     = 7,
            [UI_METRIC_SELECTION_INSET_X]    = 6,
            [UI_METRIC_SELECTION_INSET_Y]    = 2,
            [UI_METRIC_CHEVRON_ALL]          = 0,
            [UI_METRIC_CHEVRON_ON_PLAY]      = 0,
            [UI_METRIC_PANE_LEFT]            = 160,
            [UI_METRIC_ROW_RIGHT]            = 20,
            [UI_METRIC_ROW_RIGHT_GAP]        = 10,
            [UI_METRIC_SCROLLBAR_CAPSULE]    = 3,
            [UI_METRIC_GROUP_BAR]            = 0,
            [UI_METRIC_LEAD_COLUMN]          = 24,
            [UI_METRIC_SUBLINE_INK]          = 1,
            [UI_METRIC_HINT_RULE]            = 1,
            [UI_METRIC_ROW_TEXT_DY]          = -1,
            [UI_METRIC_COVER_RADIUS]         = 9,
        },
        /* one quiet tone across it */
        .scrollbar_thumb = {
            RGB(0xa5, 0x9e, 0x94), RGB(0xa5, 0x9e, 0x94), RGB(0xa5, 0x9e, 0x94),
            RGB(0xa5, 0x9e, 0x94), RGB(0xa5, 0x9e, 0x94), RGB(0xa5, 0x9e, 0x94),
            RGB(0xa5, 0x9e, 0x94), RGB(0xa5, 0x9e, 0x94), RGB(0xa5, 0x9e, 0x94),
            RGB(0xa5, 0x9e, 0x94),
        },
    },
};

const struct ui_theme *ui_theme_current(void)
{
    const int theme = global_settings.ui_theme;
    return &ui_themes[theme >= 0 && theme < UI_THEME_COUNT ? theme : UI_THEME_CLASSIC];
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

int ui_theme_cover_colours(int mode)
{
    return ui_theme_current()->cover_colours ? mode : 0;
}

bool ui_theme_owns(const void *variable)
{
    const struct user_settings *const s = &global_settings;
    return variable == s->wps_file || variable == s->sbs_file
#if CONFIG_TUNER
        || variable == s->fms_file
#endif
        || variable == s->font_file || variable == s->backdrop_file
        || variable == &s->fg_color || variable == &s->bg_color
        || variable == &s->lss_color || variable == &s->lse_color
        || variable == &s->lst_color || variable == &s->list_selector_bevel
        || variable == &s->list_text_padding;
}

void ui_theme_to_settings(bool dark)
{
    struct user_settings *const s = &global_settings;
    const struct ui_theme *const theme = ui_theme_current();
    const struct ui_theme_look *const look = &theme->look[dark];

    strmemccpy(s->wps_file, look->skin, sizeof(s->wps_file));
    strmemccpy(s->sbs_file, look->skin, sizeof(s->sbs_file));
#if CONFIG_TUNER
    strmemccpy(s->fms_file, look->skin, sizeof(s->fms_file));
#endif
    strmemccpy(s->backdrop_file, look->backdrop, sizeof(s->backdrop_file));
    strmemccpy(s->font_file, theme->list_font, sizeof(s->font_file));
    s->fg_color = look->colour[UI_COLOUR_TEXT];
    s->bg_color = look->colour[UI_COLOUR_GROUND];
    s->lss_color = look->colour[UI_COLOUR_SELECTOR_START];
    s->lse_color = look->colour[UI_COLOUR_SELECTOR_END];
    s->lst_color = look->colour[UI_COLOUR_SELECTOR_TEXT];
    s->list_selector_bevel = theme->selector_bevel;
    s->list_text_padding = theme->metric[UI_METRIC_TEXT_INSET];
}

bool ui_theme_in_settings(void)
{
    const struct user_settings *const s = &global_settings;
    const struct ui_theme *const theme = ui_theme_current();
    for (int dark = 0; dark < 2; dark++)
    {
        const char *const skin = theme->look[dark].skin;
        if (!strcmp(s->wps_file, skin) && !strcmp(s->sbs_file, skin))
            return !strcmp(s->font_file, theme->list_font);
    }
    return false;
}
