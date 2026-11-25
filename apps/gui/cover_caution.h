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
#ifndef _COVER_CAUTION_H_
#define _COVER_CAUTION_H_

#include <stdbool.h>

/* Missing Covers Caution: a song whose picture is in a format the iPod can't
   show (itunesdb.h's reasons, known as Updating Library prepared it) says
   why in the cover's place, rather than the note of a song without one.
   Now Playing: the problem and what to do, in its tile; its full view, if
   shown as such a song comes and the skin keeps it (Classic's goes back to
   the tile): a "?" over them; the menus' pane: the "?" where the note is and
   the two lines under it; list rows and an album's header: the "?" in the
   note's place. With the built-in themes' own skins, which these are drawn
   for; other skins show the note. */

struct screen;
struct gui_wps;

/* Settings > Appearance > Missing Covers Caution is on */
bool cover_caution_on(void);
/* the reason's two lines (LANG_ ids), the problem and what to do; 0 for none */
int cover_caution_problem(int reason);
int cover_caution_action(int reason);
/* After a skin's render, in the same frame: over Now Playing's tile, its
   full view or the menus' pane, for a song with a reason */
void cover_caution_render(struct gui_wps *gwps);
/* Before it, with its refresh mode: the pane's place cleared for the skin
   drawn whole, after the caution drawn there */
void cover_caution_before(struct gui_wps *gwps, unsigned refresh_mode);
/* a row's cover tile (LINE_COVER_SIZE) or an album header's, size wide and
   h high, with the "?" in the note's place */
void cover_caution_tile(struct screen *display, int x, int y, int size,
                        int h, bool header);

#endif
