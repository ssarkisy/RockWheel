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

/* What shows beside the main menu when Photos is selected: a collage of the photos,
   which the Photos plugin makes when it's given this as its parameter. The theme names
   the file, and has an icon for when it isn't there. */
#define PHOTOS_PANEL_FILE ROCKBOX_DIR "/photos_panel.bmp"
#define PHOTOS_PANEL_MAKE "--panel"

/* How many photos Finder has synced, from the header of the database */
int photos_count(void);

/* The same, remembered, for what the theme shows */
int photos_panel_count(void);

/* Whether the database has changed since the collage was made */
bool photos_panel_stale(void);

/* Makes it anew, or removes it if there are no photos. The skins have to be loaded again
   to show it. */
void photos_panel_update(void);

#endif /* _PHOTOS_PANEL_H_ */
