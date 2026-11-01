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

#include "config.h"
#include "lcd.h"

#define COVER_PALETTE_STAR_LEVELS 64
#define COVER_PALETTE_STAR_FADE   48 /* where it starts to fade to grey */
#define COVER_PALETTE_STAR_KEPT   60 /* from here on a shade is left as it is */

/* What Now Playing is drawn in when it takes its colours from the cover */
struct cover_palette
{
    unsigned ink, secondary, muted, format, format_half, format_quarter;
    /* a shade of grey in the theme's stars, by its 6 bits of green */
    fb_data star[COVER_PALETTE_STAR_KEPT];
};

/* Called as Now Playing is drawn, with the handle of the cover or a negative one */
void cover_palette_update(int art_handle);
/* NULL if Now Playing has its usual colours */
const struct cover_palette *cover_palette_get(void);
/* The colour for a grey the theme's text is in */
unsigned cover_palette_text_colour(unsigned colour);

#endif /* _COVER_PALETTE_H_ */
