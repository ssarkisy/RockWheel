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
#include "config.h"
#include <string.h>
#include "lcd.h"
#include "kernel.h"
#include "system.h"
#include "statusbar-skinned.h"
#include "misc.h"
#include "plugin.h"
#include "transition.h"

#if LCD_DEPTH == 16 && LCD_STRIDEFORMAT != VERTICAL_STRIDE && !defined(BOOTLOADER)

/* As long as a stock iPod's, whatever the width, and slowing to its end as its
   does: by its time, a position each tick, as fast as the display takes them */
#define SLIDE_TICKS (HZ * 27 / 100)
/* while the next screen is drawn: as a slow one, Songs of a large library,
   would keep the display still for longer than this, it shows then */
#define HOLD_TICKS (HZ/2)

static bool pending;
static enum transition_dir pending_dir;
/* The two screens, 300 KB of their own: borrowed memory, the plugins' buffer
   or the audio buffer, isn't free for the slide's time. The core lends the
   plugins' to the playlist viewer and the theme's parser, and taking from the
   audio buffer stops the music */
static fb_data old_pic[LCD_WIDTH * LCD_HEIGHT];
static fb_data new_pic[LCD_WIDTH * LCD_HEIGHT];
static int top, rows;              /* what slides: below the title bar */
static int old_width;              /* and from the left, of the old screen */

/* a row of the main framebuffer */
static fb_data *row(int y)
{
    return FBADDR(0, y);
}

/* How much of the width moves: a list beside the theme's panel, which stays as a
   stock iPod's does, or all of it */
static int slide_width(void)
{
    switch (get_current_activity())
    {
        case ACTIVITY_WPS:
        case ACTIVITY_FM:
        case ACTIVITY_RECORDING:
            return LCD_WIDTH;
        default:
            break;
    }
    struct viewport *info = sb_skin_get_info_vp(SCREEN_MAIN);
    const int width = info ? MIN(info->x + info->width, LCD_WIDTH) : LCD_WIDTH;
    return width > 0 ? width : LCD_WIDTH;
}

void transition_begin(enum transition_dir dir)
{
    transition_cancel();
    /* a plugin's menus are over a screen of its own, which it draws as it
       likes: none there */
    if (plugin_is_running())
        return;

    struct viewport *info = sb_skin_get_info_vp(SCREEN_MAIN);
    top = info ? info->y : 0;
    old_width = slide_width();
    rows = LCD_HEIGHT - top;
    if (rows <= 0)
        return;

    struct viewport *vp = lcd_set_viewport(NULL);
    for (int y = 0; y < rows; y++)
        memcpy(old_pic + y * LCD_WIDTH, row(top + y), LCD_WIDTH * sizeof(fb_data));
    lcd_set_viewport(vp);

    pending_dir = dir;
    pending = true;
    lcd_hold_updates(HOLD_TICKS);
}

bool transition_pending(void)
{
    return pending;
}

void transition_cancel(void)
{
    if (!pending)
        return;
    pending = false;
    if (lcd_release_updates())
        lcd_update();
}

void transition_run(void)
{
    if (!pending)
        return;
    pending = false;
    /* the time was up and the screen shown */
    if (!lcd_release_updates())
    {
        lcd_update();
        return;
    }

    /* a list that gets wider or narrower comes at once, as on a stock iPod */
    const int width = slide_width();
    if (width != old_width)
    {
        lcd_update();
        return;
    }

    struct viewport *vp = lcd_set_viewport(NULL);
    for (int y = 0; y < rows; y++)
        memcpy(new_pic + y * LCD_WIDTH, row(top + y), LCD_WIDTH * sizeof(fb_data));
    /* the new title and panel at once, the rest sliding */
    lcd_own_updates(true);
    if (top > 0)
        lcd_update_rect(0, 0, LCD_WIDTH, top);
    if (width < LCD_WIDTH)
        lcd_update_rect(width, top, LCD_WIDTH - width, rows);

    /* a frame of the whole width is 12 ms boosted, more than the tick it has:
       unboosted, every other position was dropped */
    cpu_boost(true);
    const long start = current_tick;
    long now = start;
    int t;
    do
    {
        while (current_tick == now)
            yield();
        now = current_tick;
        t = MIN(now - start, SLIDE_TICKS);
        /* fast, then settling: an ease out */
        int left = SLIDE_TICKS - t;
        int off = width - width * left * left / (SLIDE_TICKS * SLIDE_TICKS);
        int keep = width - off;
        for (int y = 0; y < rows; y++)
        {
            fb_data *dst = row(top + y);
            const fb_data *o = old_pic + y * LCD_WIDTH;
            const fb_data *n = new_pic + y * LCD_WIDTH;
            if (pending_dir == TRANSITION_FORWARD)
            {
                memcpy(dst, o + off, keep * sizeof(fb_data));
                memcpy(dst + keep, n, off * sizeof(fb_data));
            }
            else
            {
                memcpy(dst, n + width - off, off * sizeof(fb_data));
                memcpy(dst + off, o, keep * sizeof(fb_data));
            }
        }
        lcd_update_rect(0, top, width, rows);
    } while (t < SLIDE_TICKS);
    cpu_boost(false);
    /* what other threads drew meanwhile, such as a splash over it, shows now */
    if (lcd_own_updates(false))
        lcd_update();
    lcd_set_viewport(vp);
}

#endif /* LCD_DEPTH == 16 && rows one after another && !BOOTLOADER */
