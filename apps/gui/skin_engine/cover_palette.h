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
/* Before the theme changes: what's worked out from its images goes */
void cover_palette_theme_changed(void);
/* NULL if Now Playing has its usual colours */
const struct cover_palette *cover_palette_get(void);
/* The colour for a grey the theme's text is in */
unsigned cover_palette_text_colour(unsigned colour);
/* A line of a bitmap of the theme in the colours of the whole screen, for this line
   of the screen */
void cover_palette_tint_line(const fb_data *src, fb_data *dst, int count, int y);
/* The fill of a progress bar in those colours, or NULL if it's too large */
struct bitmap;
void *cover_palette_bar(const struct bitmap *bm);
/* Room for a moment for a copy of a bitmap, while the whole screen is on */
void *cover_palette_scratch(size_t *size);
/* Clears the viewport to the plain background of the whole screen, if it's on */
struct screen;
struct viewport;
bool cover_palette_clear(struct screen *display, const struct viewport *vp);
/* What to show instead of this backdrop, or NULL for the backdrop itself */
fb_data *cover_palette_backdrop(int backdrop_id);

#endif /* _COVER_PALETTE_H_ */
