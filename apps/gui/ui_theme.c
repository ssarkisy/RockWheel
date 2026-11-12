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
#include <stdlib.h>
#include "string-extra.h"
#include "settings.h"
#include "ui_theme.h"
#include "screen_access.h"
#include "viewport.h"
#include "statusbar-skinned.h"
#include "night_mode.h"
#include "skin_engine/skin_engine.h"
#include "skin_engine/cover_palette.h"

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
                    [UI_COLOUR_BITRATE]         = RGB(0x8a, 0x8d, 0x8a),
                    [UI_COLOUR_HINT_TEXT]       = RGB(0x60, 0x66, 0x70),
                    [UI_COLOUR_HINT_BAND]       = RGB(0xe6, 0xe8, 0xec),
                    [UI_COLOUR_SCROLLBAR_THUMB] = RGB(0x5a, 0x61, 0x63),
                    [UI_COLOUR_INITIAL_TEXT]    = RGB(0xff, 0xff, 0xff),
                    [UI_COLOUR_COVER_TILE]      = RGB(0xe6, 0xe8, 0xec),
                    [UI_COLOUR_NOTE_TOP]        = RGB(0x60, 0x66, 0x70),
                    [UI_COLOUR_NOTE_FOOT]       = RGB(0x60, 0x66, 0x70),
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
                    [UI_COLOUR_BITRATE]         = RGB(0x8e, 0x90, 0x94),
                    [UI_COLOUR_HINT_TEXT]       = RGB(0xa0, 0xa6, 0xb0),
                    [UI_COLOUR_HINT_BAND]       = RGB(0x2c, 0x2e, 0x33),
                    [UI_COLOUR_SCROLLBAR_THUMB] = RGB(0x5a, 0x61, 0x63),
                    [UI_COLOUR_INITIAL_TEXT]    = RGB(0xff, 0xff, 0xff),
                    [UI_COLOUR_COVER_TILE]      = RGB(0x2c, 0x2e, 0x33),
                    [UI_COLOUR_NOTE_TOP]        = RGB(0xa0, 0xa6, 0xb0),
                    [UI_COLOUR_NOTE_FOOT]       = RGB(0xa0, 0xa6, 0xb0),
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
        /* Its surfaces' colours here are not shown: each is the name of a
           surface, which the user's tone colours (tone_surfaces). Texts,
           glyphs and a message's plaque are as given */
        .look = {
            {
                .skin = "Neo",
                .backdrop = "-",
                .colour = {
                    [UI_COLOUR_TEXT]            = RGB(0x29, 0x24, 0x21),
                    [UI_COLOUR_GROUND]          = RGB(0xef, 0xeb, 0xe7),
                    [UI_COLOUR_GROUP_TEXT]      = RGB(0x52, 0x51, 0x4a),
                    [UI_COLOUR_GROUP_BAR]       = RGB(0xde, 0xdb, 0xd6),
                    /* the band's own tone: no line shows on it */
                    [UI_COLOUR_GROUP_RULE]      = RGB(0xde, 0xdb, 0xd6),
                    [UI_COLOUR_VALUE]           = RGB(0x52, 0x51, 0x4a),
                    /* small text is a denser grey than the second, which
                       was pale on the LCD: #56524D and #B8B1A8 as the screen
                       has them. On the selected row it is the row's ink, as
                       Classic's is: the row can then be as deep as its tone
                       asks, where the grey would hold it near the ground */
                    [UI_COLOUR_VALUE_SELECTED]  = RGB(0x29, 0x24, 0x21),
                    [UI_COLOUR_INITIAL_BOX]     = RGB(0xc6, 0xcf, 0xde),
                    [UI_COLOUR_SCROLLBAR_EDGE]  = RGB(0xef, 0xeb, 0xe7),
                    [UI_COLOUR_SEARCH_SECOND]   = RGB(0x52, 0x51, 0x4a),
                    [UI_COLOUR_SEARCH_FIELD]    = RGB(0xde, 0xdb, 0xd6),
                    [UI_COLOUR_SEARCH_RULE]     = RGB(0xe7, 0xdf, 0xd6),
                    [UI_COLOUR_SEARCH_DIM]      = RGB(0xa5, 0x9e, 0x94),
                    /* the selected row, Search's chosen letter and
                       Quick Scroll's box */
                    [UI_COLOUR_SELECTOR_START]  = RGB(0xc6, 0xcf, 0xde),
                    [UI_COLOUR_SELECTOR_END]    = RGB(0xc6, 0xcf, 0xde),
                    [UI_COLOUR_SELECTOR_TEXT]   = RGB(0x29, 0x24, 0x21),
                    [UI_COLOUR_QUIET]           = RGB(0xa5, 0x9e, 0x94),
                    [UI_COLOUR_LEAD]            = RGB(0x52, 0x51, 0x4a),
                    [UI_COLOUR_BITRATE]         = RGB(0x52, 0x51, 0x4a),
                    /* a star not set is an outline in the user's colour at
                       this grey's lightness, 55 % (48 % in the dark look):
                       the stars' picture has it in this grey, by which the
                       outline is known in it */
                    [UI_COLOUR_RATING_UNSET]    = RGB(0x8c, 0x8e, 0x8c),
                    /* dark in both looks: the ground's light tone on it
                       is 12.9 to 1 here, 9.1 to 1 in the dark look */
                    [UI_COLOUR_SPLASH_FILL]     = RGB(0x29, 0x24, 0x21),
                    [UI_COLOUR_SPLASH_TEXT]     = RGB(0xef, 0xeb, 0xe7),
                    [UI_COLOUR_HINT_TEXT]       = RGB(0x52, 0x51, 0x4a),
                    [UI_COLOUR_HINT_BAND]       = RGB(0xef, 0xeb, 0xe7),
                    [UI_COLOUR_SCROLLBAR_THUMB] = RGB(0xa5, 0x9e, 0x94),
                    [UI_COLOUR_INITIAL_TEXT]    = RGB(0x29, 0x24, 0x21),
                    [UI_COLOUR_COVER_TILE]      = RGB(0xe7, 0xdf, 0xd6),
                    [UI_COLOUR_NOTE_TOP]        = RGB(0x6b, 0x69, 0x63),
                    [UI_COLOUR_NOTE_FOOT]       = RGB(0xa5, 0x9e, 0x94),
                },
                .scrollbar_track = { { 0xef, 0xeb, 0xe7 }, { 0xef, 0xeb, 0xe7 } },
                .initial_opacity = 16,
            },
            {
                .skin = "Neo-Dark",
                .backdrop = "-",
                .colour = {
                    [UI_COLOUR_TEXT]            = RGB(0xef, 0xeb, 0xe7),
                    [UI_COLOUR_GROUND]          = RGB(0x21, 0x1c, 0x18),
                    [UI_COLOUR_GROUP_TEXT]      = RGB(0xb5, 0xb2, 0xa5),
                    [UI_COLOUR_GROUP_BAR]       = RGB(0x31, 0x2c, 0x29),
                    [UI_COLOUR_GROUP_RULE]      = RGB(0x31, 0x2c, 0x29),
                    [UI_COLOUR_VALUE]           = RGB(0xb5, 0xb2, 0xa5),
                    [UI_COLOUR_VALUE_SELECTED]  = RGB(0xef, 0xeb, 0xe7),
                    [UI_COLOUR_INITIAL_BOX]     = RGB(0x31, 0x34, 0x39),
                    [UI_COLOUR_SCROLLBAR_EDGE]  = RGB(0x21, 0x1c, 0x18),
                    [UI_COLOUR_SEARCH_SECOND]   = RGB(0xb5, 0xb2, 0xa5),
                    [UI_COLOUR_SEARCH_FIELD]    = RGB(0x31, 0x2c, 0x29),
                    [UI_COLOUR_SEARCH_RULE]     = RGB(0x31, 0x30, 0x29),
                    [UI_COLOUR_SEARCH_DIM]      = RGB(0x6b, 0x69, 0x63),
                    [UI_COLOUR_SELECTOR_START]  = RGB(0x31, 0x34, 0x39),
                    [UI_COLOUR_SELECTOR_END]    = RGB(0x31, 0x34, 0x39),
                    [UI_COLOUR_SELECTOR_TEXT]   = RGB(0xef, 0xeb, 0xe7),
                    [UI_COLOUR_QUIET]           = RGB(0x6b, 0x69, 0x63),
                    [UI_COLOUR_LEAD]            = RGB(0xb5, 0xb2, 0xa5),
                    [UI_COLOUR_BITRATE]         = RGB(0xb5, 0xb2, 0xa5),
                    [UI_COLOUR_RATING_UNSET]    = RGB(0x7b, 0x79, 0x7b),
                    [UI_COLOUR_SPLASH_FILL]     = RGB(0x42, 0x3c, 0x39),
                    [UI_COLOUR_SPLASH_TEXT]     = RGB(0xef, 0xeb, 0xe7),
                    [UI_COLOUR_HINT_TEXT]       = RGB(0xb5, 0xb2, 0xa5),
                    [UI_COLOUR_HINT_BAND]       = RGB(0x21, 0x1c, 0x18),
                    [UI_COLOUR_SCROLLBAR_THUMB] = RGB(0x6b, 0x69, 0x63),
                    [UI_COLOUR_INITIAL_TEXT]    = RGB(0xef, 0xeb, 0xe7),
                    [UI_COLOUR_COVER_TILE]      = RGB(0x31, 0x30, 0x29),
                    [UI_COLOUR_NOTE_TOP]        = RGB(0xa5, 0x9e, 0x94),
                    [UI_COLOUR_NOTE_FOOT]       = RGB(0x6b, 0x69, 0x63),
                },
                .scrollbar_track = { { 0x21, 0x1c, 0x18 }, { 0x21, 0x1c, 0x18 } },
                .initial_opacity = 16,
            },
        },
        .list_font = "19-Inter-SemiBold",
        .selector_bevel = false,
        .cover_colours = false,
        .cover_tone = true,
        .font = {
            [UI_FONT_LIST]          = FONT_DIR "/19-Inter-SemiBold.fnt",
            /* a hint 13 px, a setting's value 14 Regular beside its name's
               15 SemiBold: at 12 and 13 px they were small on the LCD */
            [UI_FONT_HINT]          = FONT_DIR "/17-Inter-Medium-13.fnt",
            [UI_FONT_LEAD]          = FONT_DIR "/15-Inter-Medium.fnt",
            [UI_FONT_VALUE]         = FONT_DIR "/15-Inter-Medium.fnt",
            [UI_FONT_SUBLINE]       = FONT_DIR "/15-Inter-Medium.fnt",
            [UI_FONT_HEADER_ARTIST] = FONT_DIR "/17-Inter-Medium-13.fnt",
            [UI_FONT_HEADER_COUNTS] = FONT_DIR "/15-Inter-Medium.fnt",
            [UI_FONT_MENU_VALUE]    = FONT_DIR "/18-Inter-Regular.fnt",
            /* a section's name 14 px SemiBold on its band (the skins'
               font, loaded already): at 11 px low in its row it did not
               read as a section on the LCD */
            [UI_FONT_GROUP]         = FONT_DIR "/18-Inter-SemiBold.fnt",
            [UI_FONT_INITIAL]       = FONT_DIR "/53-Inter-Letters.fnt",
        },
        .metric = {
            [UI_METRIC_ROW_HEIGHT]           = 30,
            [UI_METRIC_TEXT_INSET]           = 14,
            [UI_METRIC_INITIAL_HEIGHT]       = 72,
            [UI_METRIC_INITIAL_MARGIN]       = 16,
            [UI_METRIC_SPLASH_RADIUS]        = 9,
            [UI_METRIC_SPLASH_PAD_X]         = 14,
            [UI_METRIC_SPLASH_PAD_Y]         = 7,
            [UI_METRIC_STATUS_ROWS]          = 21,
            [UI_METRIC_PANEL_HEAD_PLAIN]     = 1,
            [UI_METRIC_CHEVRON_MARGIN]       = 12,
            [UI_METRIC_LEAD_GAP]             = 8,
            /* a value ends at 302, or at 292 before a chevron */
            [UI_METRIC_VALUE_MARGIN]         = 18,
            [UI_METRIC_VALUE_MARGIN_CHEVRON] = 28,
            [UI_METRIC_VALUE_GAP]            = 10,
            [UI_METRIC_VALUE_CUT]            = 1,
            [UI_METRIC_HINT_X]               = 14,
            [UI_METRIC_HINT_TOP]             = 6,
            [UI_METRIC_HINT_PITCH]           = 18,
            /* 36 px: the name's part its whole cell of 19, so that nothing of
               a g or a y is cut, its baseline 15 down; the second line's 15
               under it, its baseline 31 down, and two rows */
            [UI_METRIC_NAME_PART_CUT]        = 0,
            [UI_METRIC_SUBLINE_GAP]          = 2,
            [UI_METRIC_NAME_TEXT_DY]         = 0,
            [UI_METRIC_SUBLINE_TOP_DY]       = 0,
            [UI_METRIC_ROW_COVER_GAP]        = 8,
            /* two rows of one line, so that five songs show under it: the
               cover 52 px and 4 over and under it, the texts at 78, their
               three lines 53 px from 3 down. With a name of two lines it
               is three rows, in their middle */
            [UI_METRIC_HEADER_COVER_TOP]     = 4,
            [UI_METRIC_HEADER_COVER_GAP]     = 12,
            [UI_METRIC_HEADER_TEXT_MARGIN]   = 14,
            [UI_METRIC_HEADER_NAME_TOP]      = 3,
            [UI_METRIC_HEADER_NAME_LEADING]  = 0,
            [UI_METRIC_HEADER_ARTIST_GAP]    = 2,
            [UI_METRIC_HEADER_COUNTS_GAP]    = 0,
            [UI_METRIC_HEADER_FOOT]          = 4,
            [UI_METRIC_HEADER_COVER_RADIUS]  = 5,
            [UI_METRIC_HEADER_COVER_SIZE]    = 52,
            [UI_METRIC_HEADER_CENTRED]       = 1,
            [UI_METRIC_SELECTION_RADIUS]     = 7,
            [UI_METRIC_SELECTION_INSET_X]    = 6,
            [UI_METRIC_SELECTION_INSET_Y]    = 2,
            [UI_METRIC_CHEVRON_ALL]          = 0,
            [UI_METRIC_CHEVRON_ON_PLAY]      = 0,
            [UI_METRIC_PANE_LEFT]            = 160,
            [UI_METRIC_ROW_RIGHT]            = 18,
            [UI_METRIC_ROW_RIGHT_GAP]        = 10,
            [UI_METRIC_SCROLLBAR_CAPSULE]    = 3,
            [UI_METRIC_GROUP_BAR]            = 2,
            [UI_METRIC_LEAD_COLUMN]          = 24,
            [UI_METRIC_SUBLINE_INK]          = 1,
            [UI_METRIC_HINT_RULE]            = 1,
            [UI_METRIC_ROW_TEXT_DY]          = -1,
            [UI_METRIC_COVER_RADIUS]         = 5,
            [UI_METRIC_ROW_COVER_RADIUS]     = 4,
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

#if defined(HAVE_ALBUMART) && defined(HAVE_LCD_COLOR) && LCD_DEPTH >= 16
#define HAVE_COVER_TONE
/* A tone: a hue in degrees, and by look (light, dark) the surfaces'
   saturation and what is added to their lightness, out of 1000. The
   saturation is the colour's where a bright one's is TONE_BRIGHT; the
   depth scales them all (tone_saturation()) */
struct ui_tone {
    short hue;
    short saturation[2];
    short shift[2];
};

#define TONE_BRIGHT_LIGHT 500
#define TONE_BRIGHT_DARK  450
/* The depth: a bright colour's saturation by look. The LCD shows far less
   of a colour than these numbers say: at 50 / 45 with the surfaces 2 %
   lighter (1.5 to 2 % darker in the dark look) the ground was nearly white
   on it */
static const short tone_depth[2] = { 700, 650 };

/* the colours a user chooses from, in the order of enum ui_tone_colour */
static const struct ui_tone tone_set[UI_TONE_COUNT] = {
    [UI_TONE_RED]       = {   0, { 500, 450 }, {   0,   0 } },
    [UI_TONE_YELLOW]    = {  55, { 500, 450 }, {   0,   0 } },
    [UI_TONE_GREEN]     = { 140, { 500, 450 }, {   0,   0 } },
    [UI_TONE_TEAL]      = { 178, { 500, 450 }, {   0,   0 } },
    [UI_TONE_BLUE]      = { 212, { 500, 450 }, {   0,   0 } },
    [UI_TONE_PURPLE]    = { 275, { 500, 450 }, {   0,   0 } },
    [UI_TONE_PINK]      = { 325, { 500, 450 }, {   0,   0 } },
    [UI_TONE_ORANGE]    = {  22, { 600, 620 }, { -10,   5 } },
    [UI_TONE_DESERT]    = {  34, { 240, 300 }, { -30, -25 } },
    [UI_TONE_OLIVE]     = {  80, { 240, 260 }, { -30, -25 } },
    [UI_TONE_GLACIER]   = { 205, { 200, 200 }, { -10,  20 } },
    [UI_TONE_DEEP_BLUE] = { 232, { 300, 550 }, { -30,  10 } },
    [UI_TONE_BURGUNDY]  = { 345, { 260, 520 }, { -30, -10 } },
};

/* A surface's lightness in a tone, out of 1000, by look. A surface is known
   by the colour the look's table or a skin gives it, which is all that
   colour is for in a theme with a tone: the ground; the main menu's pane;
   hairlines and a list's tile; Now Playing's tile, a field, a group's bar;
   the selected row; a bar's track */
static const struct {
    unsigned plain[2];
    short lightness[2];
} tone_surfaces[] = {
    { { RGB(0xef, 0xeb, 0xe7), RGB(0x21, 0x1c, 0x18) }, { 880, 150 } },
    { { RGB(0xe7, 0xe3, 0xde), RGB(0x31, 0x2c, 0x29) }, { 840, 205 } },
    { { RGB(0xe7, 0xdf, 0xd6), RGB(0x31, 0x30, 0x29) }, { 825, 210 } },
    { { RGB(0xde, 0xdb, 0xd6), RGB(0x31, 0x2c, 0x29) }, { 810, 205 } },
    { { RGB(0xc6, 0xcf, 0xde), RGB(0x31, 0x34, 0x39) }, { 740, 255 } },
    { { RGB(0xd6, 0xd3, 0xce), RGB(0x42, 0x3c, 0x39) }, { 760, 280 } },
};
/* entries of it */
#define TONE_GROUND    0
#define TONE_PANE      1
#define TONE_SELECTION 4

/* The tone the theme is in, or NULL in a theme without one, and the
   surfaces' and the note's roles in it for the look it was made for */
static const struct ui_tone *tone;
static unsigned tone_colour[UI_COLOUR_COUNT];
/* the colours of skins' viewports asked for since the tone was made */
static struct {
    const struct ui_tone *in;
    unsigned plain, toned;
} tone_seen[12];
static int tone_seen_count;
/* the user's colour, as the setting had it when it was last taken */
static int tone_choice = UI_TONE_DEFAULT;
/* each colour as a mark beside its name, light and dark */
static const unsigned tone_swatch[UI_TONE_COUNT][2] = {
    [UI_TONE_RED]       = { RGB(0xd6, 0x38, 0x39), RGB(0xd6, 0x51, 0x52) },
    [UI_TONE_YELLOW]    = { RGB(0xe7, 0xd3, 0x29), RGB(0xe7, 0xd7, 0x42) },
    [UI_TONE_GREEN]     = { RGB(0x39, 0xae, 0x63), RGB(0x42, 0xc3, 0x6b) },
    [UI_TONE_TEAL]      = { RGB(0x31, 0xa6, 0xa5), RGB(0x31, 0xbe, 0xbd) },
    [UI_TONE_BLUE]      = { RGB(0x42, 0x8a, 0xd6), RGB(0x5a, 0x96, 0xde) },
    [UI_TONE_PURPLE]    = { RGB(0x9c, 0x59, 0xce), RGB(0xad, 0x71, 0xd6) },
    [UI_TONE_PINK]      = { RGB(0xde, 0x61, 0xad), RGB(0xe7, 0x79, 0xb5) },
    [UI_TONE_ORANGE]    = { RGB(0xef, 0x69, 0x18), RGB(0xef, 0x7d, 0x39) },
    [UI_TONE_DESERT]    = { RGB(0xb5, 0x9a, 0x73), RGB(0xc6, 0xa6, 0x84) },
    [UI_TONE_OLIVE]     = { RGB(0x6b, 0x82, 0x42), RGB(0x7b, 0x96, 0x4a) },
    [UI_TONE_GLACIER]   = { RGB(0x9c, 0xb6, 0xce), RGB(0xad, 0xc7, 0xd6) },
    [UI_TONE_DEEP_BLUE] = { RGB(0x21, 0x2c, 0x84), RGB(0x21, 0x34, 0x9c) },
    [UI_TONE_BURGUNDY]  = { RGB(0x73, 0x20, 0x31), RGB(0x8c, 0x28, 0x42) },
};
/* Now Playing's, by Cover Colours' Whole Screen: the hue of the playing
   track's cover at the bright colours' saturation and lightness */
static struct ui_tone tone_cover = { 0, { 500, 450 }, { 0, 0 } };
/* and by its Fill: no tone of a hue but these colours (its numbers are not
   read) */
static const struct ui_tone tone_filled;
static struct ui_theme_fill tone_fill;

static const unsigned char tone_roles[] = {
    UI_COLOUR_GROUND, UI_COLOUR_GROUP_BAR, UI_COLOUR_GROUP_RULE,
    UI_COLOUR_INITIAL_BOX,
    UI_COLOUR_SCROLLBAR_EDGE, UI_COLOUR_SEARCH_FIELD, UI_COLOUR_SEARCH_RULE,
    UI_COLOUR_SELECTOR_START, UI_COLOUR_SELECTOR_END, UI_COLOUR_HINT_BAND,
    UI_COLOUR_COVER_TILE,
};
/* and the marks that take the hue with them, at their own lightness */
static const unsigned char tone_marks[] = {
    UI_COLOUR_NOTE_TOP, UI_COLOUR_NOTE_FOOT,
};

/* which surface a colour is: one no row has is the surface nearest it */
static int tone_surface_of(unsigned colour, bool dark)
{
    int best = 0, least = 3 * 256;
    for (unsigned i = 0; i < ARRAYLEN(tone_surfaces); i++)
    {
        const unsigned plain = tone_surfaces[i].plain[dark];
        const int red = RGB_UNPACK_RED(colour) - RGB_UNPACK_RED(plain),
                  green = RGB_UNPACK_GREEN(colour) - RGB_UNPACK_GREEN(plain),
                  blue = RGB_UNPACK_BLUE(colour) - RGB_UNPACK_BLUE(plain);
        const int away = abs(red) + abs(green) + abs(blue);
        if (away < least)
        {
            least = away;
            best = i;
        }
    }
    return best;
}

static int tone_saturation(const struct ui_tone *in, bool dark)
{
    const int s = in->saturation[dark] * tone_depth[dark]
                  / (dark ? TONE_BRIGHT_DARK : TONE_BRIGHT_LIGHT);
    return MIN(s, 1000);
}

static unsigned tone_of(const struct ui_tone *in, unsigned colour, bool dark)
{
    const int surface = tone_surface_of(colour, dark);
    if (in == &tone_filled)
        return surface == TONE_GROUND || surface == TONE_PANE ? tone_fill.ground
                                                              : tone_fill.track;
    const int saturation = tone_saturation(in, dark);
    const unsigned *const plain = ui_theme_look(dark)->colour;
    int lightness = tone_surfaces[surface].lightness[dark] + in->shift[dark];

    if (surface == TONE_GROUND)
    {
        /* at least half its step from the pane, which the grey may have
           held nearer than the table has it */
        const int step = (tone_surfaces[TONE_GROUND].lightness[dark]
                          - tone_surfaces[TONE_PANE].lightness[dark]) / 2;
        int pane;
        cover_palette_surface(in->hue, saturation,
                              tone_surfaces[TONE_PANE].lightness[dark]
                              + in->shift[dark], dark, plain[UI_COLOUR_VALUE],
                              &pane);
        lightness = dark ? MIN(lightness, pane + step) : MAX(lightness, pane + step);
    }
    /* the palest thing read on it: the small text's grey, or on the
       selected row its ink */
    return cover_palette_surface(in->hue, saturation, lightness, dark,
                                 plain[surface == TONE_SELECTION
                                       ? UI_COLOUR_VALUE_SELECTED
                                       : UI_COLOUR_VALUE], NULL);
}

static void tone_make(bool dark)
{
    const struct ui_theme_look *const look = ui_theme_look(dark);
    tone_seen_count = 0;
    if (!tone)
        return;
    memcpy(tone_colour, look->colour, sizeof(tone_colour));
    for (unsigned i = 0; i < ARRAYLEN(tone_roles); i++)
        tone_colour[tone_roles[i]] = tone_of(tone, look->colour[tone_roles[i]], dark);
    if (tone == &tone_filled)
    {
        tone_colour[UI_COLOUR_NOTE_TOP] = tone_fill.note_top;
        tone_colour[UI_COLOUR_NOTE_FOOT] = tone_fill.note_foot;
        tone_colour[UI_COLOUR_RATING_UNSET] = tone_fill.outline;
        return;
    }
    for (unsigned i = 0; i < ARRAYLEN(tone_marks); i++)
        tone_colour[tone_marks[i]] = cover_palette_toned(look->colour[tone_marks[i]],
                                                         tone->hue,
                                                         tone_saturation(tone, dark));
    /* a star not set: an outline of a pixel, which has to show on the ground */
    tone_colour[UI_COLOUR_RATING_UNSET] =
        cover_palette_outline(look->colour[UI_COLOUR_RATING_UNSET], tone->hue,
                              tone_saturation(tone, dark), dark,
                              tone_colour[UI_COLOUR_GROUND]);
}
#endif

unsigned ui_colour(enum ui_colour role)
{
#ifdef HAVE_COVER_TONE
    if (tone)
        return tone_colour[role];
#endif
    return ui_theme_look(global_settings.dark_theme)->colour[role];
}

#ifdef HAVE_COVER_TONE
static unsigned tone_surface(const struct ui_tone *in, unsigned colour)
{
    for (int i = 0; i < tone_seen_count; i++)
        if (tone_seen[i].plain == colour && tone_seen[i].in == in)
            return tone_seen[i].toned;
    const unsigned toned = tone_of(in, colour, global_settings.dark_theme);
    if (tone_seen_count < (int)ARRAYLEN(tone_seen))
    {
        tone_seen[tone_seen_count].in = in;
        tone_seen[tone_seen_count].plain = colour;
        tone_seen[tone_seen_count++].toned = toned;
    }
    return toned;
}
#endif

unsigned ui_theme_surface(unsigned colour)
{
#ifdef HAVE_COVER_TONE
    if (tone)
        return tone_surface(tone, colour);
#endif
    return colour;
}

unsigned ui_theme_status_surface(unsigned colour)
{
#ifdef HAVE_COVER_TONE
    /* under Fill a shade of the ground, its hairline the track's colour */
    if (tone == &tone_filled)
        return tone_of(tone, colour, global_settings.dark_theme) == tone_fill.ground
               ? tone_fill.status : tone_fill.track;
    if (tone)
        return tone_surface(tone, colour);
#endif
    return colour;
}

unsigned ui_theme_plain_ground(void)
{
    return ui_theme_current()->cover_tone
           ? ui_theme_look(global_settings.dark_theme)->colour[UI_COLOUR_GROUND]
           : (unsigned)global_settings.bg_color;
}

/* the ground and the selected row, which the lists read from the settings */
static void tone_to_settings(void)
{
    struct user_settings *const s = &global_settings;
    s->bg_color = ui_colour(UI_COLOUR_GROUND);
    s->lss_color = ui_colour(UI_COLOUR_SELECTOR_START);
    s->lse_color = ui_colour(UI_COLOUR_SELECTOR_END);
}

#ifdef HAVE_COVER_TONE
/* the table for the tone, and the main screen's and the skins' grounds */
static void tone_changed(const struct ui_tone *to)
{
    tone = to;
    tone_make(global_settings.dark_theme);
    tone_to_settings();
    screens[SCREEN_MAIN].set_background(global_settings.bg_color);
    skin_surfaces_changed();
}
#endif

unsigned ui_theme_colour_swatch(int colour)
{
#ifdef HAVE_COVER_TONE
    return tone_swatch[colour >= 0 && colour < UI_TONE_COUNT ? colour : UI_TONE_DEFAULT]
                      [global_settings.dark_theme];
#else
    (void)colour;
    return 0;
#endif
}

void ui_theme_set_colour(int colour)
{
#ifdef HAVE_COVER_TONE
    tone_choice = colour >= 0 && colour < UI_TONE_COUNT ? colour : UI_TONE_DEFAULT;
    /* (a cover's tone goes as Now Playing is left, to the colour set here) */
    if (!tone || tone == &tone_set[tone_choice] || tone == &tone_cover
        || tone == &tone_filled)
        return;
    tone_changed(&tone_set[tone_choice]);
    /* the status line and the pane whole, and the lists' viewports anew.
       The status-bar skin is asked for all of itself: at the first level of
       screens the theme's toggle clears round the list and asks only for
       what has changed */
    skin_request_full_update(CUSTOM_STATUSBAR);
    sb_skin_force_next_update();
    viewportmanager_theme_changed(THEME_STATUSBAR);
    night_mode_colours_changed();
#else
    (void)colour;
#endif
}

bool ui_theme_cover_tone(int hue)
{
#ifdef HAVE_COVER_TONE
    if (!tone)
        return false;
    if (hue < 0)
    {
        if (tone != &tone_cover && tone != &tone_filled)
            return false;
        tone_changed(&tone_set[tone_choice]);
        /* the lists' status line has the same glyphs in the same places
           and wouldn't draw them again on the new ground */
        skin_request_full_update(CUSTOM_STATUSBAR);
        return true;
    }
    if (tone == &tone_cover && tone_cover.hue == hue)
        return false;
    unsigned old[UI_COLOUR_COUNT];
    const bool was_cover = tone == &tone_cover;
    memcpy(old, tone_colour, sizeof(old));
    tone_cover.hue = hue;
    tone_changed(&tone_cover);
    /* two hues may come to the same colours of the screen's 16 bits */
    return !was_cover || memcmp(old, tone_colour, sizeof(old));
#else
    (void)hue;
    return false;
#endif
}

bool ui_theme_cover_fill(const struct ui_theme_fill *fill)
{
#ifdef HAVE_COVER_TONE
    if (!tone)
        return false;
    if (tone == &tone_filled && !memcmp(fill, &tone_fill, sizeof(tone_fill)))
        return false;
    tone_fill = *fill;
    tone_changed(&tone_filled);
    return true;
#else
    (void)fill;
    return false;
#endif
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
    const struct ui_theme *const theme = ui_theme_current();
    return theme->cover_colours || theme->cover_tone ? mode : 0;
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
#ifdef HAVE_COVER_TONE
    /* a theme with a tone is never without it: made here for this look,
       before the skins are read, so that they come up in it */
    tone = theme->cover_tone ? &tone_set[tone_choice] : NULL;
    tone_make(dark);
    if (tone)
    {
        s->bg_color = tone_colour[UI_COLOUR_GROUND];
        s->lss_color = tone_colour[UI_COLOUR_SELECTOR_START];
        s->lse_color = tone_colour[UI_COLOUR_SELECTOR_END];
    }
#endif
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
