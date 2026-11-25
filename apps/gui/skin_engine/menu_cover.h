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
#include "gui/ui_theme.h"

struct viewport;
struct wps_data;

/* The cover in the main menu's pane, and in the radio's screen: only where
   the themes have the pane. Without it nothing here is built, its memory
   neither, and a theme's %C beside the menus shows the buffer's cover. */
#if defined(ITUNESDB_HAVE_ART) \
    && (defined(UI_MENU_PANE) || defined(UI_MAIN_MENU_PANE))
#define HAVE_MENU_ART

/* %?C beside the menus: whether the song playing has a cover of Finder's,
   loaded if not yet; with nothing playing, the song Resume Playback would go
   on with, its cover read once from a disk that spins */
bool menu_cover_ready(const struct wps_data *data);
/* why the song the pane is of, playing or to resume, has no cover, its
   picture being in a format that isn't shown (itunesdb.h); 0 for none */
int menu_cover_reason(void);
const fb_data *menu_cover_pixels(int *width, int *height);
/* the covers made again on Updating Library */
void menu_cover_forget(void);
/* the song to resume is looked up again at the next draw: after a boot's
   Updating Library and after USB, where its file or its cover may be others */
void menu_cover_resume_look(void);
#else
static inline void menu_cover_forget(void) {}
static inline void menu_cover_resume_look(void) {}
#endif

/* With Moving Cover, that cover pans slowly from side to side in the part of
   the theme's panel the list leaves, as a long name scrolls: the scroll
   thread moves it. Where it stands still, as with Moving Cover off, the
   rest is the same: the sharp cover decoded from the song's own picture
   and kept, drawn by the same thread once it comes. */
#if defined(HAVE_MENU_ART) && !defined(HAVE_REMOTE_LCD) \
    && LCD_STRIDEFORMAT != VERTICAL_STRIDE && !defined(BOOTLOADER)
#define HAVE_MENU_COVER
/* The iPod photo's pane is 110 px of a 176 px cover: no Moving Cover, its
   row nor its frames (UI_MAIN_MENU_PANE) */
#ifdef UI_MENU_PANE
#define HAVE_MOVING_COVER
#endif

/* the setting, at boot too */
void menu_cover_changed(bool on);
#ifdef UI_MAIN_MENU_PANE
/* Cover Beside Menu's: the sharp cover's memory with the pane */
void menu_cover_pane_changed(bool on);
#endif

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
/* A theme whose pane is a recess (UI_METRIC_PANE_SHADE): its left wall's
   shadow over the cover drawn there, the screen's rows y0 to y1 of it: of
   its two columns those at or right of the cover's left edge (x0, the
   screen's column); left of that the shadow is the backdrop's, not to be
   darkened again */
void menu_cover_shade(int x0, int y0, int y1);
/* The list's shadow over the pane (UI_METRIC_PANE_SHADOW), where the pane's
   pictures can't have it: over what is drawn on the screen's rows y0 to y1
   of the pane from its column x0, as the cover, a frame of Moving Cover or
   the caution over the note; and over a viewport of a theme with a backdrop,
   as Classic's collage of photos, its pixels that aren't the backdrop's,
   which has the shadow already */
void menu_cover_pane_shadow(int x0, int y0, int y1);
void menu_cover_pane_shadow_drawn(const struct viewport *vp);
/* the glass over the cover shown in the pane (UI_METRIC_COVER_GLASS), over
   the part of it the screen shows, which stays as the cover moves under it */
void menu_cover_glass(void);

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
/* The album cover reader's part (album_header.c): whether the panel's
   picture is to be read, or its moving frames made, on its thread; and that
   given up, as the reader is wanted for a list's covers or to end */
bool menu_cover_to_read(void);
void menu_cover_read(void);
void menu_cover_read_cancel(void);

#else
static inline void menu_cover_changed(bool on) { (void)on; }
#ifdef UI_MAIN_MENU_PANE
static inline void menu_cover_pane_changed(bool on) { (void)on; }
#endif
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
static inline void menu_cover_pane_shadow(int x0, int y0, int y1)
{
    (void)x0; (void)y0; (void)y1;
}
static inline void menu_cover_pane_shadow_drawn(const struct viewport *vp) { (void)vp; }
#endif

#endif /* _MENU_COVER_H_ */
