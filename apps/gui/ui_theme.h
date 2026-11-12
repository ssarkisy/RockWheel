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
#ifndef _UI_THEME_H_
#define _UI_THEME_H_

#include <stdbool.h>

/* The look that isn't in the skins: colours, list metrics and font files by
   role, one entry a theme, each with a light and a dark set. Classic is the
   only one. Values are read at the moment of use, Dark Theme with them. */

/* Classic's, where a constant is needed: the settings' defaults */
#define UI_CLASSIC_SKIN          "iClassic-Square"
#define UI_CLASSIC_SKIN_DARK     UI_CLASSIC_SKIN "-Dark"
#define UI_CLASSIC_FONT          "24-Inter-Bold"
#define UI_CLASSIC_BACKDROP      BACKDROP_DIR "/iClassic_bd.bmp"
#define UI_CLASSIC_BACKDROP_DARK BACKDROP_DIR "/iClassic_bd_dark.bmp"
#define UI_CLASSIC_TEXT          LCD_RGBPACK(0x00, 0x00, 0x00)
#define UI_CLASSIC_GROUND        LCD_RGBPACK(0xff, 0xff, 0xff)
#define UI_CLASSIC_SELECTOR_START LCD_RGBPACK(0x6b, 0xa6, 0xde)
#define UI_CLASSIC_SELECTOR_END  LCD_RGBPACK(0x18, 0x6d, 0xbd)
#define UI_CLASSIC_SELECTOR_TEXT LCD_RGBPACK(0xff, 0xff, 0xff)
#define UI_CLASSIC_TEXT_INSET    5
/* the dark ground, as in themes/tools/make_dark_theme.py */
#define UI_CLASSIC_DARK_GROUND_R 18
#define UI_CLASSIC_DARK_GROUND_G 18
#define UI_CLASSIC_DARK_GROUND_B 20

enum ui_colour {
    UI_COLOUR_TEXT,
    UI_COLOUR_GROUND,
    UI_COLOUR_GROUP_TEXT,     /* a group's name and the hints */
    UI_COLOUR_GROUP_BAR,      /* a group's bar, the hint band, a cover's place */
    UI_COLOUR_VALUE,          /* a value, a year or a number before a name */
    UI_COLOUR_VALUE_SELECTED,
    UI_COLOUR_INITIAL_BOX,    /* Quick Scroll's */
    UI_COLOUR_SCROLLBAR_EDGE,
    UI_COLOUR_SEARCH_SECOND,  /* Search: its grey text */
    UI_COLOUR_SEARCH_FIELD,
    UI_COLOUR_SEARCH_RULE,
    UI_COLOUR_SEARCH_DIM,     /* a letter no name goes on with */
    UI_COLOUR_COUNT
};

enum ui_metric {
    UI_METRIC_ROW_HEIGHT,     /* 0: the list font's cell */
    UI_METRIC_TEXT_INSET,
    UI_METRIC_INITIAL_OPACITY, /* Quick Scroll's box, in sixteenths; by look */
    UI_METRIC_INITIAL_HEIGHT,
    UI_METRIC_INITIAL_MARGIN,
    UI_METRIC_CHEVRON_MARGIN,
    UI_METRIC_LEAD_GAP,
    UI_METRIC_VALUE_MARGIN,
    UI_METRIC_VALUE_MARGIN_CHEVRON,
    UI_METRIC_VALUE_GAP,
    UI_METRIC_HINT_X,
    UI_METRIC_HINT_TOP,
    UI_METRIC_HINT_PITCH,
    /* a row of two lines */
    UI_METRIC_NAME_PART_CUT,
    UI_METRIC_SUBLINE_GAP,
    UI_METRIC_NAME_TEXT_DY,
    UI_METRIC_SUBLINE_TOP_DY,
    UI_METRIC_ROW_COVER_GAP,
    /* an album's header */
    UI_METRIC_HEADER_COVER_TOP,
    UI_METRIC_HEADER_COVER_GAP,
    UI_METRIC_HEADER_TEXT_MARGIN,
    UI_METRIC_HEADER_NAME_TOP,
    UI_METRIC_HEADER_NAME_LEADING,
    UI_METRIC_HEADER_ARTIST_GAP,
    UI_METRIC_HEADER_COUNTS_GAP,
    UI_METRIC_HEADER_FOOT,
    UI_METRIC_COUNT
};

enum ui_font {
    UI_FONT_LIST,
    UI_FONT_HINT,
    UI_FONT_LEAD,
    UI_FONT_VALUE,
    UI_FONT_SUBLINE,
    UI_FONT_HEADER_ARTIST,
    UI_FONT_HEADER_COUNTS,
    UI_FONT_INITIAL,
    UI_FONT_COUNT
};

#define UI_SCROLLBAR_THUMB_STEPS 10

struct ui_theme_look {
    const char *skin;      /* the base name of its .wps, .sbs and .fms */
    const char *backdrop;
    unsigned colour[UI_COLOUR_COUNT];
    /* the scrollbar's track, inner then outer, in 8-bit parts: the columns
       between are worked out from these, not from the screen's colours */
    unsigned char scrollbar_track[2][3];
    short initial_opacity;
};

struct ui_theme {
    struct ui_theme_look look[2]; /* light, dark */
    const char *font[UI_FONT_COUNT]; /* files, with their folder */
    short metric[UI_METRIC_COUNT];
    unsigned scrollbar_thumb[UI_SCROLLBAR_THUMB_STEPS]; /* across it */
};

const struct ui_theme *ui_theme_current(void);
/* of the current theme, or of its look by Dark Theme as it is now */
const struct ui_theme_look *ui_theme_look(bool dark);
unsigned ui_colour(enum ui_colour role);
int ui_metric(enum ui_metric metric);
const char *ui_font_file(enum ui_font role);

#endif /* _UI_THEME_H_ */
