/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 by Kevin Ferrare
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

#ifndef _GUI_SPLASH_H_
#define _GUI_SPLASH_H_

#include "screen_access.h"
#include "gcc_extensions.h"

/*
 * Puts a splash message centered on all the screens for a given period
 *  - ticks : how long the splash is displayed (in rb ticks)
 *  - fmt : what to say *printf style
 *  /f at the beginning of a string will reset the max width and height seen
 *  /n will end a line /n/n will add a blank line
 *  /t will start a new line and indent the string on that line
 */
extern void splashf(int ticks, const char *fmt, ...) ATTRIBUTE_PRINTF(2, 3);

/*
 * Puts a splash message centered on all the screens for a given period
 *  - ticks : how long the splash is displayed (in rb ticks)
 *  - str : what to say, if this is a LANG_* string (from ID2P)
 *          it will be voiced
 */
#define splash(__ticks, __str) splashf(__ticks, __str)

/* The next change of activity keeps the splashes' size, which it otherwise
   resets, so that the next one covers the last: for a screen opened over a
   splash without being drawn, as a plugin with the theme left on over
   plugin_load()'s "Loading...". False drops it. */
extern void splash_keep_size(bool keep);

/* A hint to be read, as Sort By's where it is (tree.c): as splash(), its
   box the list's whole width, its lines centred, wrapped in two thirds of it
   on the Video and in all of it on the iPod photo, two or three of them, the
   theme's padding round them. Shown, not waited for */
extern void splash_wide(const char *str);

/* set a delay before displaying the progress meter the first time */
extern void splash_progress_set_delay(long delay_ticks);
/*
 * Puts a splash message centered on all the screens with a progressbar
 *  - current : current progress increment
 *  - total : total increments
 *  - fmt : what to say *printf style
 * updates limited internally to 20 fps - call repeatedly to update progress
 */
extern void splash_progress(int current, int total, const char *fmt, ...) ATTRIBUTE_PRINTF(3, 4);
#endif /* _GUI_ICON_H_ */
