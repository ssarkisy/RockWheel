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
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "scroll_engine.h"
#include "backlight.h"
#include "button.h"
#include "action.h"
#include "audio.h"
#include "settings.h"
#include "plugin.h"
#include "playback.h"
#include "buffering.h"
#include "itunesdb.h"
#include "transition.h"
#include "statusbar-skinned.h"
#include "wps_internals.h"
#include "skin_engine.h"
#include "menu_cover.h"

#ifdef ITUNESDB_HAVE_ART

/* The cover beside the menus. It's larger than any Finder keeps and the panel
   is rarely on screen when a track changes, so playback doesn't load it, and
   it's scaled here when it's to be shown, and again only for another cover. */
#define ART_MENU_SIZE LCD_HEIGHT
static struct {
    fb_data pixels[ART_MENU_SIZE * ART_MENU_SIZE];
    short width, height;
    uint32_t offset;
    char path[sizeof(((struct itunesdb_art *)0)->path)];
    int handle;         /* the buffer's it was copied from, or -1 for Finder's */
} menu_art;

void menu_cover_forget(void)
{
    menu_art.width = 0;
}

bool menu_cover_ready(const struct wps_data *data)
{
    struct skin_albumart *aa = SKINOFFSETTOPTR(get_skin_buffer((struct wps_data *)data),
                                               data->albumart);
    struct mp3entry *id3 = audio_current_track();
    struct itunesdb_art art;
    if (!aa || !id3 || !(audio_status() & AUDIO_STATUS_PLAY)
        || global_settings.album_art == AA_OFF
        || !itunesdb_find_track_art(id3, id3->path, &art))
        return false;
    if (menu_art.width > 0 && menu_art.handle < 0 && menu_art.offset == art.offset
        && !strcmp(menu_art.path, art.path))
        return true;

    struct bitmap bm;
    memset(&bm, 0, sizeof(bm));
    bm.width = MIN(aa->width, ART_MENU_SIZE);
    bm.height = MIN(aa->height, ART_MENU_SIZE);
    bm.data = (unsigned char *)menu_art.pixels;
    menu_art.width = 0;
    /* from the cover in the buffer if it's as large as Finder's, as the disk is
       often off by the time the menus open, and turning it on takes 0.6 s */
    const struct bitmap *buffered = playback_current_largest_art();
    const int room = sizeof(bm) + sizeof(menu_art.pixels);
    int size;
    if (buffered && buffered->width >= art.width && buffered->height >= art.height)
        size = itunesdb_scale_art(buffered, &bm, room);
    else
        size = itunesdb_load_art(&art, &bm, room);
    if (size < 0)
        return false;
    menu_art.width = bm.width;
    menu_art.height = bm.height;
    menu_art.offset = art.offset;
    strcpy(menu_art.path, art.path);
    menu_art.handle = -1;
    return true;
}

const fb_data *menu_cover_pixels(int *width, int *height)
{
    *width = menu_art.width;
    *height = menu_art.height;
    return menu_art.pixels;
}
#endif /* ITUNESDB_HAVE_ART */

#ifdef HAVE_MENU_COVER

/* 10 px/s in half steps, one every HZ/20: a whole step jerks at this speed;
   2 s rests, first in the middle */
#define PAN_SPEED   10
#define PAN_HALVES  (2 * PAN_SPEED)
/* Drawn and sent this ahead of being shown, by the display's tick; less than
   a half step, as one frame goes to the display at a time */
#define PAN_LATENCY MAX(HZ / PAN_HALVES - 1, 1)
#define PAN_REST    (2 * HZ)
/* the longest sleep while it can't go or rests, and the most the clock moves
   per look: no jump */
#define PAN_POLL    (HZ / 10)
/* the wheel's last step less than this ago, as list_covers.c's AT_REST: the
   lists' frames go first */
#define PAN_AT_REST (HZ / 30 + 1)

/* Where it was last drawn, by the UI: the part of the screen it shows in, of
   the panel right of the list */
static struct
{
    const struct viewport *vp;  /* compared only: the theme's own may be gone */
    int x0, y0, x1, y1;
    int width, height;          /* the cover's size, as drawn */
    int row;                    /* the cover's at y0 */
    int p0, travel;             /* columns at x0: in the middle, at the right end */
} pan;
static bool pan_shown;          /* drawn by the last render of it, theme unchanged since */
static int pan_h;               /* the half column at x0 on the screen */
static long pan_clock;
static long pan_last;           /* the tick of the last look while it could go */
static long pan_due;            /* the tick the next step's due */
static bool pan_going;

static long leg_ticks(int halves)
{
    return ((long)halves * HZ + PAN_HALVES - 1) / PAN_HALVES;
}

/* the half steps gone t ticks into a leg, and the ticks to the next */
static int leg_moved(long t, long *until)
{
    const int halves = t * PAN_HALVES / HZ;
    *until = leg_ticks(halves + 1) - t;
    return halves;
}

/* The half column at x0 by the clock, and the ticks until it changes: in the
   middle first, then to the right end, then back and forth */
static int pan_at(long t, long *until)
{
    const int travel = 2 * pan.travel, p0 = 2 * pan.p0;
    if (t < PAN_REST)
    {
        *until = PAN_REST - t;
        return p0;
    }
    t -= PAN_REST;
    const long first = leg_ticks(travel - p0);
    if (t < first)
        return p0 + leg_moved(t, until);
    t -= first;
    const long leg = leg_ticks(travel);
    t %= 2 * (PAN_REST + leg);
    if (t < PAN_REST)
    {
        *until = PAN_REST - t;
        return travel;
    }
    t -= PAN_REST;
    if (t < leg)
        return travel - leg_moved(t, until);
    t -= leg;
    if (t < PAN_REST)
    {
        *until = PAN_REST - t;
        return 0;
    }
    return leg_moved(t - PAN_REST, until);
}

static bool pan_can_go(void)
{
    return pan_shown && global_settings.moving_cover
        && menu_art.width == pan.width && menu_art.height == pan.height
        && (audio_status() & AUDIO_STATUS_PLAY)
        && !transition_active() && !plugin_is_running()
#if defined(HAVE_LCD_ENABLE) || defined(HAVE_LCD_SLEEP)
        && lcd_active()
#endif
#ifdef HAVE_BACKLIGHT
        && is_backlight_on(true)
#endif
        && action_waiting() && button_queue_empty()
        && !TIME_BEFORE(current_tick, action_last_tick() + PAN_AT_REST);
}

#if NUM_CORES > 1 && defined(HAVE_LCD_SEND_RECT)
#define PAN_ON_COP
/* The COP sends each step to the display in one go, a few rows at a time;
   the CPU's own updates and its codec's decoder, seconds of PCM ahead, wait
   those few ms. Let in between the parts, a frame of the decoder each time,
   it made the steps reach the display unevenly, up to HZ/8 late. The CPU
   composes the step. Of cached memory, the COP reads only the panel's rows of
   the framebuffer, which the CPU commits first, and writes only its own
   stack. It takes the BCM past the
   CPU's holds of the display: the CPU waits for it, HZ/20 at most, before it
   draws there itself (cop_wait()). */
#define COP_ROWS 60
/* Its thread lives only while the cover shows, and is made only with a slot
   to spare: a codec's decoder on the COP, made as a track starts, finds one.
   Cover Flow's threads and the album cover reader end it first. */
#define COP_SPARE_SLOTS 1
static struct
{
    struct semaphore go;
    volatile bool busy;
    volatile bool quit;
    volatile long done_at;
    int x, y, width, height;
    long show_at;
} cop SHAREDBSS_ATTR;
static long cop_stack[CACHEALIGN_UP(DEFAULT_STACK_SIZE * 2 / sizeof(long))]
    CACHEALIGN_ATTR;
static unsigned int cop_thread_id;
static bool cop_failed;         /* not made: not again until the next stop */
static bool cop_unusable;       /* stuck or too late: the CPU sends the steps
                                   till reboot */
static bool cop_sent;           /* a step given it, not yet seen done */
static long cop_sent_at;
static int cop_late;            /* steps in a row that took over HZ/10 */
static bool cop_overran;        /* a wait gave up: rows it sent since are stale */
static bool cop_end_due;        /* ended once it's done */
static volatile bool cop_ending; /* its stack still in use: none made meanwhile */

static void cop_thread(void)
{
    while (1)
    {
        semaphore_wait(&cop.go, TIMEOUT_BLOCK);
        if (cop.quit)
            break;
        commit_discard_dcache();
        const int end = cop.y + cop.height;
        for (int y = cop.y; y < end; y += COP_ROWS)
            lcd_send_rect(cop.x, y, cop.width, MIN(COP_ROWS, end - y),
                          y + COP_ROWS >= end, cop.show_at);
        cop.done_at = current_tick;
        cop.busy = false;
    }
}

static int free_slots(void)
{
    struct thread_debug_info info;
    int n = 0;
    for (int i = 0; i < MAXTHREADS; i++)
        n += thread_get_debug_info(i, &info) == 0;
    return n;
}

static bool cop_ready(void)
{
    if (cop_unusable || cop_failed || cop_ending)
        return false;
    if (cop_thread_id)
        return true;
    cop_failed = free_slots() <= COP_SPARE_SLOTS;
    if (cop_failed)
        return false;
    cop.quit = false;
    cop.busy = false;
    cop_end_due = false;
    semaphore_init(&cop.go, 1, 0);
    cop_thread_id = create_thread(cop_thread, cop_stack, sizeof(cop_stack), 0,
                                  "moving cover" IF_PRIO(, PRIORITY_PLAYBACK)
                                  IF_COP(, COP));
    cop_failed = !cop_thread_id;
    return !cop_failed;
}

/* The panel's rows the CPU drew in after a wait gave up, sent again over the
   COP's stale ones once it's done; and the thread ended if that was due. With
   the current viewport the main one: lcd_update_rect() sends that one's. */
static void cop_settle(void)
{
    if (cop.busy)
        return;
    if (cop_overran)
    {
        cop_overran = false;
        struct viewport *vp = lcd_set_viewport_ex(NULL, 0);
        lcd_update_rect(cop.x, cop.y, cop.width, cop.height);
        lcd_set_viewport_ex(vp, 0);
    }
    if (cop_end_due && cop_thread_id)
    {
        const unsigned int id = cop_thread_id;
        cop_thread_id = 0;
        cop_end_due = false;
        cop.quit = true;
        semaphore_release(&cop.go);
        cop_ending = true;
        thread_wait(id);
        cop_ending = false;
    }
}

static void cop_wait_for(long ticks)
{
    const long until = current_tick + ticks;
    while (cop.busy && TIME_BEFORE(current_tick, until))
        yield();
    if (cop.busy)
        cop_overran = true;
    cop_settle();
}

static void cop_wait(void)
{
    cop_wait_for(HZ/20);
}

/* Its slot given back: now, before a plugin or the reader, waited for as long
   as cop_free() would; else once it's done */
static void cop_end(bool now)
{
    cop_failed = false;
    while (now && cop_ending) /* the scroll thread ending it: its slot too */
        yield();
    if (!cop_thread_id)
        return;
    cop_end_due = true;
    cop_wait_for(now ? HZ : HZ/20);
    if (now && cop_thread_id)
        cop_unusable = true;
}

/* False while the step before is still being sent: the step waits, the clock
   going on. A COP that took over HZ/10 three steps in a row, or HZ for one,
   isn't used again. */
static bool cop_free(void)
{
    cop_settle();
    if (!cop_sent)
        return true;
    if (cop.busy)
    {
        if (TIME_AFTER(current_tick, cop_sent_at + HZ))
        {
            cop_unusable = true;
            cop_sent = false;
            return true;
        }
        return false;
    }
    cop_sent = false;
    if (TIME_AFTER(cop.done_at, cop_sent_at + HZ/10))
    {
        if (++cop_late >= 3)
            cop_unusable = true;
    }
    else
        cop_late = 0;
    return true;
}
#else
static inline void cop_settle(void) {}
static inline void cop_wait(void) {}
static inline void cop_end(bool now) { (void)now; }
static inline bool cop_free(void) { return true; }
#endif /* PAN_ON_COP */

#ifdef SIMULATOR
/* RBSIM_SCRIPT's waitidle waits for a still screen but for this */
extern volatile long sim_lcd_update_tick;
#endif

/* Half way to the next column: each pixel the floor average of its own and
   its right neighbour's, per RGB565 channel, two pixels a word */
#define AVG2(a, b) (((a) & (b)) + ((((a) ^ (b)) & 0xf7def7deu) >> 1))
static void blend_row(fb_data *dst, const fb_data *src, int n)
{
#if LCD_PIXELFORMAT == RGB565 && defined(ROCKBOX_LITTLE_ENDIAN)
    if (!((uintptr_t)dst & 3) && !(n & 1))
    {
        uint32_t *d = (uint32_t *)dst;
        if (!((uintptr_t)src & 3))
        {
            const uint32_t *w = (const uint32_t *)src;
            for (int i = 0; i < n / 2; i++)
                d[i] = AVG2(w[i], (w[i] >> 16) | ((uint32_t)src[2 * i + 2] << 16));
        }
        else
        {
            const uint32_t *w = (const uint32_t *)(src + 1);
            uint32_t prev = src[0];
            for (int i = 0; i < n / 2; i++)
            {
                d[i] = AVG2(prev | (w[i] << 16), w[i]);
                prev = w[i] >> 16;
            }
        }
        return;
    }
#endif
    for (int i = 0; i < n; i++)
        dst[i] = AVG2((uint32_t)src[i], (uint32_t)src[i + 1]);
}

/* show_at: the tick that shows it, where the display can wait for one */
static void pan_draw(int h, long show_at)
{
    struct viewport *vp = lcd_set_viewport_ex(NULL, 0);
    const int width = pan.x1 - pan.x0;
    const fb_data *src = menu_art.pixels + pan.row * menu_art.width + h / 2;
    for (int y = pan.y0; y < pan.y1; y++, src += menu_art.width)
    {
        if (h & 1)
            blend_row(FBADDR(pan.x0, y), src, width);
        else
            memcpy(FBADDR(pan.x0, y), src, width * sizeof(fb_data));
    }
#ifdef PAN_ON_COP
    if (cop_ready())
    {
        lcd_set_viewport_ex(vp, 0);
        commit_dcache();
        cop.x = pan.x0;
        cop.y = pan.y0;
        cop.width = width;
        cop.height = pan.y1 - pan.y0;
        cop.show_at = show_at;
        cop.busy = true;
        cop_sent = true;
        cop_sent_at = current_tick;
        semaphore_release(&cop.go);
        return;
    }
#endif
#ifdef HAVE_LCD_SEND_RECT
    lcd_send_rect(pan.x0, pan.y0, width, pan.y1 - pan.y0, true, show_at);
#else
    (void)show_at;
#ifdef SIMULATOR
    const long idle_since = sim_lcd_update_tick;
#endif
    /* while its viewport is set: lcd_update_rect() sends the current one's */
    lcd_update_rect(pan.x0, pan.y0, width, pan.y1 - pan.y0);
#ifdef SIMULATOR
    sim_lcd_update_tick = idle_since;
#endif
#endif
    lcd_set_viewport_ex(vp, 0);
}

/* At 80 MHz while it moves: at 30 MHz a frame isn't drawn and sent within
   PAN_LATENCY, the decoder on the COP going first */
static void pan_boost(bool on)
{
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    static bool boosted;
    if (on != boosted)
    {
        boosted = on;
        cpu_boost(on);
    }
#else
    (void)on;
#endif
}

/* On the scroll thread: a step when it's due; the ticks until the next look */
static long pan_tick(void)
{
    cop_settle();
    if (!pan_can_go())
    {
        pan_boost(false);
        pan_going = false;
        return PAN_POLL;
    }
    const long now = current_tick;
    if (pan_going)
        pan_clock += MIN(now - pan_last, PAN_POLL);
    pan_last = now;
    pan_going = true;
    long until;
    const int h = pan_at(pan_clock, &until);
    /* not through a rest but its last PAN_POLL */
    pan_boost(h != pan_h || until <= PAN_POLL);
    if (h != pan_h)
    {
        if (!cop_free())
            return 1;
        /* shown PAN_LATENCY after it was due by the clock, not as the thread
           woke or the frame was ready: the steps go evenly */
        const bool on_time = !TIME_AFTER(pan_due, now)
                             && !TIME_AFTER(now, pan_due + PAN_LATENCY);
        pan_draw(h, (on_time ? pan_due : now) + PAN_LATENCY);
        pan_h = h;
    }
    pan_due = now + until;
    return MIN(until, PAN_POLL);
}

bool menu_cover_take(int handle)
{
    struct bitmap *bm;
    if (!global_settings.moving_cover || !(audio_status() & AUDIO_STATUS_PLAY)
        || bufgetdata(handle, 0, (void *)&bm) <= 0
        || bm->width > ART_MENU_SIZE || bm->height > ART_MENU_SIZE)
        return false;
    if (menu_art.width > 0 && menu_art.handle == handle)
        return true;
    memcpy(menu_art.pixels, bm->data, bm->width * bm->height * sizeof(fb_data));
    menu_art.width = bm->width;
    menu_art.height = bm->height;
    menu_art.handle = handle;
    return true;
}

int menu_cover_place(const struct viewport *vp, int x, int y, int width,
                     int height)
{
    pan_shown = false;
    if (!global_settings.moving_cover)
        return 0;
    const int left = vp->x + x, top = vp->y + y;
    int x0 = MAX(vp->x, left);
    const int x1 = MIN(MIN(vp->x + vp->width, left + width), LCD_WIDTH);
    const int y0 = MAX(MAX(vp->y, top), 0);
    const int y1 = MIN(MIN(vp->y + vp->height, top + height), LCD_HEIGHT);
    /* right of the list, which is drawn over the cover's left part */
    const struct viewport *list = sb_skin_get_info_vp(SCREEN_MAIN);
    if (list && list->y < y1 && list->y + list->height > y0
        && list->x < x1 && list->x + list->width > x0)
    {
        if (list->x > x0)
            return 0;
        x0 = list->x + list->width;
    }
    const int travel = width - (x1 - x0);
    const int p0 = x0 - left;
    if (x1 <= x0 || y1 <= y0 || travel <= 0 || p0 > travel)
        return 0;

    pan.vp = vp;
    pan.x0 = x0;
    pan.y0 = y0;
    pan.x1 = x1;
    pan.y1 = y1;
    pan.width = width;
    pan.height = height;
    pan.row = y0 - top;
    pan.p0 = p0;
    pan.travel = travel;
    long until;
    /* the whole column; a half step is drawn by the next tick */
    pan_h = pan_at(pan_clock, &until) & ~1;
    pan_shown = true;
    return p0 - pan_h / 2;
}

void menu_cover_stop(void)
{
    pan_shown = false;
    pan_boost(false);
    cop_end(false);
}

void menu_cover_wait(void)
{
    cop_wait();
}

void menu_cover_end(void)
{
    pan_shown = false;
    pan_boost(false);
    cop_end(true);
}

void menu_cover_give_slot(void)
{
    cop_end(true);
}

void menu_cover_hidden(const struct viewport *vp)
{
    if (vp == pan.vp)
        menu_cover_stop();
}

bool menu_cover_area(int *x0, int *y0, int *x1, int *y1)
{
    if (!pan_shown)
        return false;
    *x0 = pan.x0;
    *y0 = pan.y0;
    *x1 = pan.x1;
    *y1 = pan.y1;
    return true;
}

void menu_cover_changed(bool on)
{
    menu_cover_end();
    /* turned on, it starts from the middle, where it's drawn while off */
    pan_clock = 0;
    pan_going = false;
    scroll_set_tick_hook(on ? pan_tick : NULL);
    skin_request_full_update(CUSTOM_STATUSBAR);
}

#endif /* HAVE_MENU_COVER */
