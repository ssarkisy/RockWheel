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

/* Shorter than a stock iPod's 270 ms, whatever the width, and slowing to its
   end the more: most of the way at once, by its time, a position each tick, as
   fast as the display takes them. As long as the stock one's, it felt slow. */
#define SLIDE_TICKS (HZ * 20 / 100)
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
/* Now Playing stays still, the other screen sliding off it or over it, its
   own title and panel with it: a busy screen, Now Playing flickered sliding,
   and the panel's top stayed behind its panel */
static bool old_wps;

/* a row of the main framebuffer */
static fb_data *row(int y)
{
    return FBADDR(0, y);
}

/* A title bar as long as a list beside the theme's panel, whose top is right
   of it, gets longer or shorter with the slide when the list does: its icons at
   the end that moves, the bar's colour before them, and past it the panel's
   top where the slide has brought the panel, the bar's colour beside. At once,
   the panel's top showed where the panel would come, over the old screen,
   while the panel was still on its way */
struct bar
{
    fb_data *title;                /* the screen's title rows */
    fb_data colour[LCD_HEIGHT];    /* its bar's, row by row, where it ends */
    int icons;                     /* its icons from here to its end */
};
static struct bar old_bar, new_bar;
static bool old_bar_read;
static struct
{
    bool on;
    bool narrower;                 /* the new screen's list is */
    const struct bar *narrow, *wide;
    int narrow_width;
    int from, to;                  /* where it ends as the slide begins, ends */
} moving;

/* The title rows of the screen shown, the bar's colour at its end, from its
   backdrop, and where its icons begin, as the theme has them. Without a
   backdrop, no bar moves */
static bool bar_read(struct bar *bar, fb_data *title, int width)
{
    bar->title = title;
    memcpy(title, row(0), top * LCD_WIDTH * sizeof(fb_data));
    const fb_data *backdrop = lcd_get_backdrop();
    if (!backdrop || width <= 0)
        return false;
    for (int y = 0; y < top; y++)
        bar->colour[y] = backdrop[y * LCD_WIDTH + width - 1];
    const int icons = sb_skin_bar_icons(SCREEN_MAIN, top, width);
    bar->icons = icons >= 0 ? icons : width;
    return true;
}

/* The bar ending at end: the new title's words, the narrower bar's icons at
   the end, the wider bar's colour between; past it the narrower screen's
   panel, its top shifted as its rows below are, by shift */
static void bar_draw(int end, int shift)
{
    const struct bar *narrow = moving.narrow;
    const int words = narrow->icons;
    const int icons = moving.narrow_width - words;
    for (int y = 0; y < top; y++)
    {
        fb_data *dst = row(y);
        const fb_data *from = narrow->title + y * LCD_WIDTH;
        const fb_data colour = moving.wide->colour[y];
        memcpy(dst, new_bar.title + y * LCD_WIDTH, words * sizeof(fb_data));
        for (int x = words; x < end - icons; x++)
            dst[x] = colour;
        memcpy(dst + end - icons, from + words, icons * sizeof(fb_data));
        for (int x = end; x < LCD_WIDTH; x++)
        {
            const int panel_x = x + shift;
            dst[x] = panel_x >= moving.narrow_width && panel_x < LCD_WIDTH ?
                     from[panel_x] : colour;
        }
    }
}

/* where in the narrower screen a column shows, less the column: as the rows
   below slide, off of the width come, keep to come */
static int bar_shift(int off, int keep)
{
    const bool forward = pending_dir == TRANSITION_FORWARD;
    if (moving.narrower)
        return forward ? -keep : keep;
    return forward ? off : -off;
}

/* whether the bar moves, from where to where: when the list gets wider or
   narrower beside a panel, both bars read */
static void bar_plan(int width)
{
    moving.on = false;
    if (top <= 0 || width == old_width || !old_bar_read
        || !bar_read(&new_bar, new_pic + rows * LCD_WIDTH, width))
        return;
    const bool narrower = width < old_width;
    moving.narrower = narrower;
    moving.narrow = narrower ? &new_bar : &old_bar;
    moving.wide = narrower ? &old_bar : &new_bar;
    moving.narrow_width = MIN(width, old_width);
    moving.from = old_width;
    moving.to = width;
    moving.on = true;
}

/* a row of a snapshot, its title rows kept after the rest */
static const fb_data *pic_row(const fb_data *pic, int y)
{
    return pic + (y < top ? rows + y : y - top) * LCD_WIDTH;
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
    /* the title rows after the rest, where the snapshot has room */
    old_bar_read = top > 0 && bar_read(&old_bar, old_pic + rows * LCD_WIDTH, old_width);
    old_wps = get_current_activity() == ACTIVITY_WPS;
    lcd_set_viewport(vp);

    pending_dir = dir;
    pending = true;
    lcd_hold_updates(HOLD_TICKS);
}

bool transition_pending(void)
{
    return pending;
}

void transition_wait(long ticks)
{
    if (pending)
        lcd_hold_updates(ticks);
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

    /* a list that gets wider or narrower, as Music's menu beside the panel and
       Artists, slides with all of the screen under the title, the panel too:
       it came at once, as on a stock iPod, which felt abrupt */
    int width = slide_width();
    struct viewport *vp = lcd_set_viewport(NULL);
    const bool new_wps = get_current_activity() == ACTIVITY_WPS;
    const bool still = old_wps != new_wps;
    moving.on = false;
    if (still)
        memcpy(new_pic + rows * LCD_WIDTH, row(0), top * LCD_WIDTH * sizeof(fb_data));
    else
        bar_plan(width);
    if (width != old_width || still)
        width = LCD_WIDTH;

    for (int y = 0; y < rows; y++)
        memcpy(new_pic + y * LCD_WIDTH, row(top + y), LCD_WIDTH * sizeof(fb_data));
    /* the new title and panel at once, the rest sliding; or the bar as long as
       the old one; or with Now Playing, all of both as they were */
    lcd_own_updates(true);
    if (moving.on)
        bar_draw(moving.from, bar_shift(0, width));
    if (top > 0 && !still)
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
        /* fast, then settling: an ease out of the third power */
        int left = SLIDE_TICKS - t;
        int off = width - width * left * left * left
                          / (SLIDE_TICKS * SLIDE_TICKS * SLIDE_TICKS);
        int keep = width - off;
        if (still)
        {
            /* the screen that isn't Now Playing as it would slide, Now
               Playing where it is */
            for (int y = 0; y < LCD_HEIGHT; y++)
            {
                fb_data *dst = row(y);
                const fb_data *o = pic_row(old_pic, y);
                const fb_data *n = pic_row(new_pic, y);
                if (pending_dir == TRANSITION_FORWARD)
                {
                    memcpy(dst, old_wps ? o : o + off, keep * sizeof(fb_data));
                    memcpy(dst + keep, new_wps ? n + keep : n, off * sizeof(fb_data));
                }
                else
                {
                    memcpy(dst, new_wps ? n : n + keep, off * sizeof(fb_data));
                    memcpy(dst + off, old_wps ? o + off : o, keep * sizeof(fb_data));
                }
            }
            lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
            continue;
        }
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
        if (moving.on)
        {
            /* as far as the slide has come; at its end the new title itself */
            if (t < SLIDE_TICKS)
                bar_draw(moving.from + (moving.to - moving.from) * off / width,
                         bar_shift(off, keep));
            else
                memcpy(row(0), new_bar.title, top * LCD_WIDTH * sizeof(fb_data));
            lcd_update_rect(0, 0, width, top + rows);
        }
        else
            lcd_update_rect(0, top, width, rows);
    } while (t < SLIDE_TICKS);
    cpu_boost(false);
    /* what other threads drew meanwhile, such as a splash over it, shows now */
    if (lcd_own_updates(false))
        lcd_update();
    lcd_set_viewport(vp);
}

#endif /* LCD_DEPTH == 16 && rows one after another && !BOOTLOADER */
