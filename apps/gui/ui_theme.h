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
   role, one entry a theme, each with a light and a dark set. Values are read
   at the moment of use, the theme (global_settings.ui_theme) and Dark Theme
   with them. */
enum { UI_THEME_CLASSIC, UI_THEME_NEO, UI_THEME_COUNT };

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
    UI_COLOUR_SELECTOR_START, /* the selected row's, its start and end one for a flat one */
    UI_COLOUR_SELECTOR_END,
    UI_COLOUR_SELECTOR_TEXT,
    UI_COLOUR_QUIET,          /* a chevron on a row not selected, a song's number */
    UI_COLOUR_LEAD,           /* a year or a number before a name */
    UI_COLOUR_HINT_TEXT,
    UI_COLOUR_HINT_BAND,
    UI_COLOUR_SCROLLBAR_THUMB, /* of a capsule */
    UI_COLOUR_INITIAL_TEXT,   /* Quick Scroll's letter */
    UI_COLOUR_COVER_TILE,     /* a cover's place without a cover, under its note */
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
    UI_METRIC_HEADER_COVER_RADIUS,
    /* the selected row: 0 for a bar of the row's width, else the radius of a
       rounded rectangle inset from the row's sides and, in a row of one
       line, from its top and bottom */
    UI_METRIC_SELECTION_RADIUS,
    UI_METRIC_SELECTION_INSET_X,
    UI_METRIC_SELECTION_INSET_Y,
    UI_METRIC_CHEVRON_ALL,    /* on every row that leads on, not the selected one alone */
    UI_METRIC_CHEVRON_ON_PLAY, /* on the main menu's row that plays, as a stock iPod's Now Playing */
    UI_METRIC_PANE_LEFT,      /* the main menu's pane's first column; 0: where the list beside it ends */
    /* the right end of a row: 0 as Classic lays it out, else where a value
       ends, the chevron's left a pixel before, and the gaps between them */
    UI_METRIC_ROW_RIGHT,
    UI_METRIC_ROW_RIGHT_GAP,
    UI_METRIC_SCROLLBAR_CAPSULE, /* 0 the theme's scrollbar, else a capsule's width in the gutter */
    UI_METRIC_GROUP_BAR,      /* a group's name on a bar, or low in its row without */
    UI_METRIC_LEAD_COLUMN,    /* the least width of the column of numbers before names, 0 for none */
    UI_METRIC_SUBLINE_INK,    /* a second line starts where its name's ink does, not its cell */
    UI_METRIC_HINT_RULE,      /* a line of a pixel over the hints, as wide as their text */
    UI_METRIC_ROW_TEXT_DY,    /* a name's shift from the middle of a row of one line */
    UI_METRIC_COVER_RADIUS,   /* Now Playing's covers' corners, 0 for square */
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
    UI_FONT_MENU_VALUE,   /* a setting's value at the right of its row */
    UI_FONT_GROUP,        /* a group's name */
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
    const char *list_font; /* the name the "font" setting has for font[UI_FONT_LIST] */
    bool selector_bevel;
    bool cover_colours;    /* Now Playing's, as Classic has them; else as Off */
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
/* Cover Colours as the theme takes the setting's value */
int ui_theme_cover_colours(int mode);
/* Whether a setting's variable is the theme's to set, from the table: its
   value isn't written to config.cfg in a theme other than Classic, so that a
   firmware without that theme reads Classic's */
bool ui_theme_owns(const void *variable);
/* The theme's names, font and colours into the settings, for Dark Theme or not */
void ui_theme_to_settings(bool dark);
/* whether the skins and the list's font the settings name are the theme's,
   of either look */
bool ui_theme_in_settings(void);

#endif /* _UI_THEME_H_ */
