/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Sergei Sarkisian
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
#ifndef _PHOTOS_PANEL_H_
#define _PHOTOS_PANEL_H_

#include <stdbool.h>
#include "rbpaths.h"
#include "lcd.h"

/* What shows beside the main menu when Photos is selected: a collage of the photos,
   which the Photos plugin makes when it's given this as its parameter. The theme names
   the file, and has an icon for when it isn't there. */
#define PHOTOS_PANEL_FILE ROCKBOX_DIR "/photos_panel.bmp"
#define PHOTOS_PANEL_MAKE "--panel"
#define PHOTOS_PANEL_W 160
#define PHOTOS_PANEL_H 176

/* The albums' collages, which Photos' own menu shows beside an album's row: all in one
   file, made with the collage above, or alone, given this, when the photos are the same
   and the albums may not be. A header of PHOTOS_PANELS_HEADER bytes, zeros after its
   words: PHOTOS_PANELS_MAGIC, a hash of the albums as the plugin read them (their names,
   their photos in order and where their thumbnails are), their number. Then a collage
   for each, in the plugin's order, the Photo Library's first: PHOTOS_PANEL_W by
   PHOTOS_PANEL_H pixels as the screen holds them. The plugin shows them only while the
   hash is that of the database it has just read. */
#define PHOTOS_PANELS_FILE   ROCKBOX_DIR "/photos_panels.dat"
#define PHOTOS_PANELS_MAKE   "--albums"
#define PHOTOS_PANELS_MAGIC  0x33505752 /* "RWP3" */
#define PHOTOS_PANELS_HEADER 512

/* How many photos Finder has synced, from the header of the database */
int photos_count(void);

/* The same, remembered, for what the theme shows */
int photos_panel_count(void);

/* Whether the database has changed since the collage was made */
bool photos_panel_stale(void);

/* Makes them anew, or removes them if there are no photos. True if the main menu's was
   made or removed: the skins have to be loaded again to show that. */
bool photos_panel_update(void);

/* For Photos' menu: the collage in the theme takes these pixels, PHOTOS_PANEL_W by
   PHOTOS_PANEL_H, and its caption this count of photos; the theme is drawn again, not
   yet put on the screen. No pixels with a count of 0, an empty album, for which the
   theme shows what it does without photos. With a count under 0 the caption is the
   library's again, as the plugin leaves it, the pixels the library's collage. False if
   the theme has no such picture. */
bool photos_panel_show(const fb_data *pixels, int count);
/* the count for the theme: that one, or the library's */
int photos_panel_shown_count(void);
/* nothing shown by the plugin any more */
void photos_panel_show_end(void);
/* Whether a collage may be read now: from flash, or from a disk that spins and goes on
   for a while. One asleep isn't woken for it. */
bool photos_panel_disk_free(void);

#endif /* _PHOTOS_PANEL_H_ */
