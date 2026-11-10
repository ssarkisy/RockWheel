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
#ifndef _TRANSITION_H_
#define _TRANSITION_H_

/* The slide between two screens, as on a stock iPod: the new one comes in
   from the right going into a menu, and from the left going back.
   transition_begin() keeps what the screen shows and holds the display while
   the next screen is drawn; get_action() runs the slide once it's waited for,
   with that screen drawn in full. */
enum transition_dir {
    TRANSITION_FORWARD,
    TRANSITION_BACK,
};

#if LCD_DEPTH == 16 && LCD_STRIDEFORMAT != VERTICAL_STRIDE && !defined(BOOTLOADER)
void transition_begin(enum transition_dir dir);
void transition_run(void);
/* begun, and not run yet: the screen being drawn is to slide in */
bool transition_pending(void);
/* begun, and not over yet */
bool transition_active(void);
/* the display as drawn, without a slide, as before a plugin loads */
void transition_cancel(void);
/* the next screen comes later than most, as Now Playing after a song is
   chosen: the slide waits for it this long from now */
void transition_wait(long ticks);
#else
static inline void transition_begin(enum transition_dir dir) { (void)dir; }
static inline void transition_run(void) {}
static inline bool transition_pending(void) { return false; }
static inline bool transition_active(void) { return false; }
static inline void transition_cancel(void) {}
static inline void transition_wait(long ticks) { (void)ticks; }
#endif

#endif /* _TRANSITION_H_ */
