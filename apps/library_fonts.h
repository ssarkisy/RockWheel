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
#ifndef _LIBRARY_FONTS_H
#define _LIBRARY_FONTS_H

#include <stdbool.h>

/* The fonts of the alphabets the library and the menus are written in, other
   than the theme's: FONT_DIR "/<height>-Library.fnt" (library_fonts.c) */

/* the database, the languages or the alphabets' own fonts are others than they
   were made of */
bool library_fonts_stale(void);
/* made anew, with how far that is as it goes; the firmware's are let go of
   first (font_load_library) */
bool library_fonts_make(void (*progress)(int done, int total));

#endif
