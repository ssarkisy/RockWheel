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
#ifndef _COVER_PALETTE_H_
#define _COVER_PALETTE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "lcd.h"

/* global_settings.cover_colours */
enum { COVER_COLOURS_OFF = 0, COVER_COLOURS_TEXT, COVER_COLOURS_SCREEN,
       COVER_COLOURS_FILL };

/* global_settings.status_icons: what of the status bar takes the cover's colours
   on the whole screen. Their own: each icon as the theme has it, and the clock
   and title, where they read on the status bar so */
enum { STATUS_ICONS_COVER = 0, STATUS_ICONS_BATTERY, STATUS_ICONS_OWN };

/* A cover's colour is 0xRRGGBB, with this if it's a shade of grey, or else unknown */
#define COVER_COLOUR_GREY    0x01000000u
#define COVER_COLOUR_UNKNOWN 0xffffffffu

#define COVER_PALETTE_STAR_LEVELS 64
#define COVER_PALETTE_STAR_FADE   48 /* where it starts to fade to grey */
#define COVER_PALETTE_STAR_KEPT   60 /* from here on a shade is left as it is */

/* What Now Playing is drawn in when it takes its colours from the cover */
struct cover_palette
{
    bool whole_screen;
    unsigned background;
    unsigned ink, secondary, muted, format, format_half, format_quarter;
    /* a shade of grey in the theme's stars, by its 6 bits of green */
    fb_data star[COVER_PALETTE_STAR_KEPT];
};

/* The colour of a cover */
uint32_t cover_palette_colour(const fb_data *pixels, int width, int height);
/* its hue in degrees */
int cover_palette_hue(uint32_t colour);
/* A flat surface of a theme in a tone: the hue at a saturation and a
   lightness, both out of 1000, in the screen's 16 bits; lighter (in a dark
   look darker) by 5 of the 1000 at a time until text reads on it at 4.5 to 1.
   The lightness it came to goes to shown_lightness, if given */
unsigned cover_palette_surface(int hue, int saturation, int lightness, bool dark,
                               unsigned text, int *shown_lightness);
/* A mark of a theme (a missing cover's note) in a tone: its own lightness at
   the hue and saturation, in the screen's 16 bits. Nothing is read on it, so
   it is not moved for any text */
unsigned cover_palette_toned(unsigned colour, int hue, int saturation);
/* The same for an outline of a pixel on that ground: darker (in a dark look
   lighter) by 5 of the 1000 at a time until it stands 2 to 1 on it, which
   the yellows and greens don't at a middle lightness on a light ground */
unsigned cover_palette_outline(unsigned colour, int hue, int saturation, bool dark,
                               unsigned ground);
/* For a new value of the setting, or the first at boot: gets or frees the memory of
   the whole screen */
void cover_palette_set_mode(int mode);
/* Called as Now Playing is drawn, with the handle of the cover or a negative one, and
   its colour if that's known. True if the colours have changed, and all of it has to
   be drawn again. */
bool cover_palette_update(int art_handle, uint32_t colour);
/* For it, a cover that isn't in the buffer, known by its colour */
#define COVER_ART_BY_COLOUR (-1000)
/* Before the theme changes: what's worked out from its images goes */
void cover_palette_theme_changed(void);
/* Now Playing is left, or a screen comes over it that isn't its own: a
   theme's tone goes back to its user's colour before that screen is drawn,
   and Now Playing takes the cover's again when it is next drawn */
void cover_palette_leave(void);
/* NULL if Now Playing has its usual colours */
const struct cover_palette *cover_palette_get(void);
/* The colour for a grey the theme's text is in */
unsigned cover_palette_text_colour(unsigned colour);
/* The colour for a pixel of a picture of a theme with a tone, whose shape is
   in its alpha: the outline of a small star not set in the theme's colour for
   it, whatever Cover Colours is. With Text and Stars, of a picture marked to
   take the cover's colour, the text's greys in the text's and a paler mark as
   it is; with Fill every grey of any picture in the colour of its part, on
   the status line (status) as it reads there */
unsigned cover_palette_mark_colour(unsigned colour, bool status);
/* Whether a theme with a tone has Now Playing filled with the cover's colour,
   and then the colour of the bar's fill in *bar, if given */
bool cover_palette_tone_filled(unsigned *bar);
struct viewport;
/* the same for the text of a viewport, which in the status bar may keep it */
unsigned cover_palette_text_colour_in(unsigned colour, const struct viewport *vp);
/* the lines of the lyrics not sung now, dim, from the half way colour: clear
   enough of the ground to read, 3 to 1, and still apart from the ink */
unsigned cover_palette_dimmed(unsigned ink, unsigned dim, unsigned ground);
/* whether row y of the screen is in the status bar */
bool cover_palette_in_status_bar(int y);
/* Of a line of an image at (x, y) of the screen: the most contrast any of its
   pixels has with the status bar under it, in hundredths, at least *most. An
   icon reads by its main colour, at 3.5 or more as the icons' own are made to:
   the edges smoothed into the background have less. */
void cover_palette_status_bar_contrast(const fb_data *src, int count, int x, int y,
                                       int *most);
#define COVER_PALETTE_ICON_CONTRAST 350
/* after cover_palette_tint_line(), an icon's ink back as the theme has it, its
   edges between that and the tint, and the light it stands on left tinted */
void cover_palette_keep_ink(const fb_data *src, fb_data *dst, int count);
/* A line of a bitmap of the theme in the colours of the whole screen, for this line
   of the screen */
void cover_palette_tint_line(const fb_data *src, fb_data *dst, int count, int y);
/* the same for the iOS bar's knob, its colour that of the bar's fill */
void cover_palette_knob_line(const fb_data *src, fb_data *dst, int count, int y);
/* after it, the pixels of src with a colour of their own back in dst, as the
   battery's charge: its colour says something */
void cover_palette_keep_colours(const fb_data *src, fb_data *dst, int count);
/* The fill of a progress bar in those colours, or NULL if it's too large. A flat
   one, of iOS's look, has the colour of its corner round it, which is the
   screen's, and no reflection under it. */
struct bitmap;
void *cover_palette_bar(const struct bitmap *bm, bool flat);
/* the iOS look's track under the bar, flat as its fill, or NULL as the theme has it */
void *cover_palette_bar_track(const struct bitmap *bm);
/* Clears the viewport to the plain background of the whole screen, if it's on */
struct screen;
bool cover_palette_clear(struct screen *display, const struct viewport *vp);
/* What to show instead of this backdrop, or NULL for the backdrop itself */
fb_data *cover_palette_backdrop(int backdrop_id);

#endif /* _COVER_PALETTE_H_ */
