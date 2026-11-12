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
#ifndef _MENU_COVER_H_
#define _MENU_COVER_H_

#include <stdbool.h>
#include "config.h"
#include "lcd.h"
#include "itunesdb.h"

struct viewport;
struct wps_data;

#ifdef ITUNESDB_HAVE_ART
/* %?C beside the menus: whether the song playing has a cover of Finder's,
   loaded if not yet; with nothing playing, the song Resume Playback would go
   on with, its cover read once from a disk that spins */
bool menu_cover_ready(const struct wps_data *data);
const fb_data *menu_cover_pixels(int *width, int *height);
/* the covers made again on Updating Library */
void menu_cover_forget(void);
/* the song to resume is looked up again at the next draw: after a boot's
   Updating Library and after USB, where its file or its cover may be others */
void menu_cover_resume_look(void);
#endif

/* With Moving Cover, that cover pans slowly from side to side in the part of
   the theme's panel the list leaves, as a long name scrolls: the scroll
   thread moves it. */
#if defined(ITUNESDB_HAVE_ART) && !defined(HAVE_REMOTE_LCD) \
    && LCD_STRIDEFORMAT != VERTICAL_STRIDE && !defined(BOOTLOADER)
#define HAVE_MENU_COVER

/* the setting, at boot too */
void menu_cover_changed(bool on);

/* The buffer's cover of the song playing, as one outside Finder has, copied
   to pan as Finder's: the buffer's may move or go meanwhile. Not while it's
   off, the cover drawn from the buffer as before. */
bool menu_cover_take(int handle);
/* It's to be drawn at x, y of the theme's viewport vp: how much further right
   to draw it, for where it has come to */
int menu_cover_place(const struct viewport *vp, int x, int y, int width,
                     int height);
/* and what to draw there: the cover's own pixels, or, placed to move, the
   frame it moves by */
const fb_data *menu_cover_placed_pixels(const fb_data *pixels);
/* No step until it's drawn again: all of the theme drawn again, the theme off
   or another loaded. The step being sent waited for, HZ/20 at most, and the
   COP's thread ended once it's done. */
void menu_cover_stop(void);
/* before the screen is drawn for a slide: the step being sent waited for */
void menu_cover_wait(void);
/* before a plugin: as menu_cover_stop(), the thread ended at once */
void menu_cover_end(void);
/* before another thread is made, as the album cover reader's: the COP's
   ended at once, the cover going on */
void menu_cover_give_slot(void);
/* the theme's viewport vp hidden, as when the music stops */
void menu_cover_hidden(const struct viewport *vp);
/* the part of the screen it moves in, while it's shown */
bool menu_cover_area(int *x0, int *y0, int *x1, int *y1);

/* The sharp cover's memory as Album Art has it now, and its moving frames'
   as Moving Cover has it: taken only without music, let go with either off;
   and the scroll thread's hook with them. On the UI's thread, not as a
   theme is drawn. */
void menu_cover_memory(void);
/* A list's frame is drawn, on the UI's thread: the moving cover's step, if
   one is due, with it. Through the wheel's turns the UI doesn't wait long
   enough for the scroll thread's steps, and a list that leaves the pane as
   it is, as Music's menu, showed the cover standing and then jumping */
void menu_cover_frame_step(void);
/* The album cover reader's part (album_header.c): whether the picture is to
   be decoded, on its thread; and that given up, as the reader is wanted for
   a list's covers or to end */
bool menu_cover_to_read(void);
void menu_cover_read(void);
void menu_cover_read_cancel(void);
/* The covers kept after their decode are dropped: what they stood for,
   Finder's covers or those made of the songs' own, may be others now */
void menu_cover_kept_forget(void);

#else
static inline void menu_cover_kept_forget(void) {}
static inline void menu_cover_changed(bool on) { (void)on; }
static inline void menu_cover_stop(void) {}
static inline void menu_cover_wait(void) {}
static inline void menu_cover_end(void) {}
static inline void menu_cover_give_slot(void) {}
static inline void menu_cover_hidden(const struct viewport *vp) { (void)vp; }
static inline void menu_cover_memory(void) {}
static inline void menu_cover_frame_step(void) {}
static inline bool menu_cover_to_read(void) { return false; }
static inline void menu_cover_read(void) {}
static inline void menu_cover_read_cancel(void) {}
#endif

#endif /* _MENU_COVER_H_ */
