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
/* in the order of the Theme row's values */
enum { UI_THEME_CLASSIC, UI_THEME_MODERN, UI_THEME_NEO, UI_THEME_COUNT };

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
    UI_COLOUR_GROUP_RULE,     /* the hairlines of a group's band, with UI_METRIC_GROUP_BAR 2 */
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
    UI_COLOUR_BITRATE,        /* Now Playing: the bitrate after the format's name */
    UI_COLOUR_RATING_UNSET,   /* Now Playing: a star not set, in a theme with a tone */
    UI_COLOUR_SPLASH_FILL,    /* a message's plaque and its text, with UI_METRIC_SPLASH_RADIUS */
    UI_COLOUR_SPLASH_TEXT,
    UI_COLOUR_HINT_TEXT,
    UI_COLOUR_HINT_BAND,
    UI_COLOUR_SCROLLBAR_THUMB, /* of a capsule */
    UI_COLOUR_INITIAL_TEXT,   /* Quick Scroll's letter */
    UI_COLOUR_COVER_TILE,     /* a cover's place without a cover, under its note */
    UI_COLOUR_NOTE_TOP,       /* that note, from its top row to its foot */
    UI_COLOUR_NOTE_FOOT,
    UI_COLOUR_ACCENT,         /* a mark before a name, the speaker, Search's caret */
    UI_COLOUR_ROW_RULE,       /* the hairline under a row */
    UI_COLOUR_SWITCH_ON,
    UI_COLOUR_SWITCH_OFF,
    UI_COLOUR_SCROLLBAR_LINE, /* the track of a scrollbar that is a line */
    UI_COLOUR_LYRICS_DIM,     /* lyrics' lines not sung now, with UI_METRIC_LYRICS_DIM */
    UI_COLOUR_RING_TRACK,     /* the disk's ring's part not used, with UI_METRIC_RING_TRACK */
    UI_COLOUR_GROUP_NAME,     /* a group's name on its band, with UI_METRIC_GROUP_BAR 2 */
    UI_COLOUR_COUNT
};

enum ui_metric {
    UI_METRIC_ROW_HEIGHT,     /* 0: the list font's cell */
    UI_METRIC_TEXT_INSET,
    UI_METRIC_INITIAL_OPACITY, /* Quick Scroll's box, in sixteenths; by look */
    UI_METRIC_INITIAL_HEIGHT,
    UI_METRIC_INITIAL_MARGIN,
    /* a message (splash): 0 for the ground's colour in a frame, else a
       plaque in its own two colours with corners of this radius and no
       frame, its text this far from its sides, and from its top and foot */
    UI_METRIC_SPLASH_RADIUS,
    UI_METRIC_SPLASH_PAD_X,
    UI_METRIC_SPLASH_PAD_Y,
    /* Now Playing's status line with its hairline, in rows from the top:
       under Cover Colours' Fill a shade of the cover's colour in a theme
       with a tone */
    UI_METRIC_STATUS_ROWS,
    /* Cover Flow's tracks: their album's name and artist on the panel's own
       ground with a hairline under them, not in the selected row's colours,
       which made the first track's selection one block with them */
    UI_METRIC_PANEL_HEAD_PLAIN,
    UI_METRIC_CHEVRON_MARGIN,
    UI_METRIC_LEAD_GAP,
    UI_METRIC_VALUE_MARGIN,
    UI_METRIC_VALUE_MARGIN_CHEVRON,
    UI_METRIC_VALUE_GAP,
    UI_METRIC_VALUE_CUT,      /* a setting's value too long beside its name is cut with an ellipsis, not left out */
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
    UI_METRIC_HEADER_COVER_SIZE, /* its cover's side, 0 for 60 */
    UI_METRIC_HEADER_CENTRED, /* an album's header in the middle of its rows' height, not at their top */
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
    /* a group's name: 0 small and low in its row; 1 on a bar, as a row's
       name is; 2 a band as high as the row with a hairline over and under
       it, the name in the group's font in its middle; 3 no bar, the name
       in the group's font and the theme's accent, low in its row */
    UI_METRIC_GROUP_BAR,
    UI_METRIC_LEAD_COLUMN,    /* the least width of the column of numbers before names, 0 for none */
    UI_METRIC_SUBLINE_INK,    /* a second line starts where its name's ink does, not its cell */
    UI_METRIC_HINT_RULE,      /* a line of a pixel over the hints, as wide as their text */
    UI_METRIC_ROW_TEXT_DY,    /* a name's shift from the middle of a row of one line */
    UI_METRIC_COVER_RADIUS,   /* Now Playing's covers' corners, 0 for square */
    UI_METRIC_ROW_COVER_RADIUS, /* a row's cover's or tile's corners, 0 for square */
    UI_METRIC_ROW_RULE,       /* a hairline under each row not selected, from its name's start */
    UI_METRIC_SCROLLBAR_LINE, /* the scrollbar a line of a pixel on a track, so far from the list's end */
    UI_METRIC_SWITCHES,       /* On and Off as a switch at the row's end */
    UI_METRIC_ACCENT_MARKS,   /* the speaker and the marks before names in the accent */
    UI_METRIC_MENU_MARKS,     /* Music's rows each with its mark */
    UI_METRIC_SEARCH_RADIUS,  /* Search's field's corners where the selection has none: its shapes as a rounded theme's */
    UI_METRIC_LYRICS_DIM,     /* lyrics' other lines in the theme's grey, not half way to the ground */
    /* the selected row a bar of one colour with everything on it in the
       selection's text colour: the marks before a name and the speaker too,
       the scrollbar's thumb beside it, a ring round a switch; no hairline
       over it */
    UI_METRIC_SELECTION_INK,
    UI_METRIC_PLAYING_SUBLINE, /* the playing song's second line in the accent */
    /* a list with covers: where its covers stand from the list's edge, and
       its rows without one start there too; 0 for the text's inset */
    UI_METRIC_ROW_COVER_INSET,
    UI_METRIC_RING_TRACK,     /* the disk's ring's track in the theme's colour, not the ground's */
    /* with UI_METRIC_ROW_RIGHT: the chevron's right edge so far from the
       row's end, nearer than UI_METRIC_CHEVRON_MARGIN, and the value beside
       it as much nearer; 0 for that margin */
    UI_METRIC_CHEVRON_INSET,
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
    /* a theme's own, NULL for the list's: found by their files, which the
       theme's skin loads */
    UI_FONT_HEADER_NAME,  /* an album's name over its songs */
    UI_FONT_COUNT
};

#define UI_SCROLLBAR_THUMB_STEPS 10

/* The colours of a theme with a tone, as its user chooses among them */
enum ui_tone_colour {
    UI_TONE_RED, UI_TONE_YELLOW, UI_TONE_GREEN, UI_TONE_TEAL, UI_TONE_BLUE,
    UI_TONE_PURPLE, UI_TONE_PINK, UI_TONE_ORANGE, UI_TONE_DESERT,
    UI_TONE_OLIVE, UI_TONE_GLACIER, UI_TONE_DEEP_BLUE, UI_TONE_BURGUNDY,
    UI_TONE_COUNT
};
#define UI_TONE_DEFAULT UI_TONE_DESERT

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
    bool cover_colours;    /* Now Playing's, drawn as Classic's are */
    /* (a missing cover's note is one of the surfaces below, not of the glyphs:
       it takes the hue at its own lightness) */
    /* The theme is never plain. The colour its user chose lies as a
       cover on its flat surfaces, on every screen: the ground, the selected
       row, fields, tiles, hairlines, a bar's track; texts and glyphs stay */
    bool cover_tone;
    bool flat_cover;       /* Now Playing's cover is never turned: no Angled Cover row */
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
/* Cover Colours as the theme takes the setting's value: Off in a theme that
   takes nothing from the cover */
int ui_theme_cover_colours(int mode);
/* A text's or a mark's colour in a skin, as it is now: in a theme with a
   tone the skins' dense grey (small text; UI_COLOUR_VALUE) and second grey
   (the disk's ring, a bar's fill; UI_COLOUR_NOTE_TOP) are names for the
   tone's colours of them, else as given. A skin's text that is to stay
   grey has a grey of its own (the clock: make_neo_theme.py's CLOCK) */
unsigned ui_theme_skin_text(unsigned colour);
/* A surface's colour (a ground, a fill, a hairline) as it is now: in the
   tone of a theme that has one, else as given */
unsigned ui_theme_surface(unsigned colour);
/* the same for a surface of Now Playing's status line, a shade of the
   ground under Cover Colours' Fill */
unsigned ui_theme_status_surface(unsigned colour);
/* the ground as the look's table has it, for a skin being read */
unsigned ui_theme_plain_ground(void);
/* The user's colour, one of enum ui_tone_colour (the setting's value as it
   is taken: at boot, and on Select). In a theme with a tone the table is
   made and the skins' viewports set, and the screen is to be drawn again
   whole: nothing is loaded */
void ui_theme_set_colour(int colour);
/* that colour as a mark beside its name in the list of them, by look */
unsigned ui_theme_colour_swatch(int colour);
/* Now Playing's tone in a theme that has one: the hue of the playing
   track's cover on the whole screen; a negative hue for the user's colour
   again. True if the colours changed: the table is made and the skins'
   viewports set, and the caller draws the screen again whole */
bool ui_theme_cover_tone(int hue);
/* Now Playing under Cover Colours' Fill in a theme with a tone: no hue but
   the colours Classic makes of the cover, as the screen has them. The
   ground; the status line's; a bar's track, which hairlines and a missing
   cover's tile take too; that tile's note, top and foot; the outline of a
   small star not set */
struct ui_theme_fill {
    unsigned ground, status, track, note_top, note_foot, outline;
};
/* the same as ui_theme_cover_tone() for them */
bool ui_theme_cover_fill(const struct ui_theme_fill *fill);
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
