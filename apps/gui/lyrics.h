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
#ifndef _LYRICS_H_
#define _LYRICS_H_

#include <stdbool.h>
#include "metadata.h"
#include "screen_access.h"

/* The lyrics view of the WPS: lyrics embedded in the track, wrapped to a
   viewport of the skin and scrolled with the wheel */

/* read the lyrics of id3 and wrap them to fit vp, false if there are none;
   for the track they are of already, they stay as they are */
bool lyrics_load(const struct mp3entry *id3, struct viewport *vp);

/* synced lyrics: the line sung at elapsed ms becomes the current one and
   the view follows it, unless the wheel moved it a moment ago; true when
   they should be drawn again */
bool lyrics_follow(unsigned long elapsed);

void lyrics_draw(struct screen *display, struct viewport *vp);

/* move the first line shown by delta lines, false if it did not move */
bool lyrics_scroll(int delta);

#endif /* _LYRICS_H_ */
