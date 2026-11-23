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
#ifndef _HANG_TEXT_H_
#define _HANG_TEXT_H_

#include "config.h"
#include "system.h"

#ifdef HAVE_HANG_SCREEN
/* The HANG screen's text drawn anew (hang.h's hang_text), in the UI font and
   the theme's colours, by the UI thread, if the language, the UI font, the
   theme or Dark Theme changed since it was drawn: as the skins are loaded
   again, at the end of settings_apply(true) (a .cfg's language), and as a
   font is chosen in the file browser. No memory but hang_text's; a font's
   glyphs read as for any text. */
void hang_text_render(void);
#else
static inline void hang_text_render(void) {}
#endif

#endif /* _HANG_TEXT_H_ */
