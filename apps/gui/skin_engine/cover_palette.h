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
/* NULL if Now Playing has its usual colours */
const struct cover_palette *cover_palette_get(void);
/* The colour for a grey the theme's text is in */
unsigned cover_palette_text_colour(unsigned colour);
struct viewport;
/* the same for the text of a viewport, which in the status bar may keep it */
unsigned cover_palette_text_colour_in(unsigned colour, const struct viewport *vp);
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
