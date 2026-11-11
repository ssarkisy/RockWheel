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
#include <stdlib.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "storage.h"
#include "core_alloc.h"
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
#include "metadata.h"
#include "bmp.h"
#include "jpeg_load.h"
#include "album_header.h"
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
    short box_width, box_height; /* what Finder's was scaled to fit */
} menu_art;

#if defined(HAVE_MENU_COVER) && defined(HAVE_JPEG) && defined(HAVE_TAGCACHE)
#define MENU_COVER_SHARP
/* Sharp: the picture in the song's own file, decoded by the album cover
   reader's thread to the size Finder's was scaled to, takes its place. In a
   block held only with Moving Cover on, taken without music (sharp_memory()):
   the pixels, then the decoder's room, its state 12.5 KB, a row of blocks up
   to 30.7 KB and the scaler's rows 11.5 KB. The UI's thread, the scroll's
   and the reader's are on one core: none of their changes here has a yield
   in the middle. */
#define SHARP_ROOM  (54 * 1024)
#define SHARP_BYTES (sizeof(menu_art.pixels) + SHARP_ROOM)
static struct
{
    int handle;             /* the block's, 0 for none */
    fb_data *pixels;        /* its start: it doesn't move */
    bool refused;           /* no room: not again until music has played */
    bool shown;             /* drawn in place of menu_art.pixels */
    /* the cover it holds: menu_art's key, and the size */
    bool held;
    uint32_t offset;
    char path[sizeof(menu_art.path)];
    short width, height, box_width, box_height;
    /* the picture of the song playing, taken on the UI's thread, not while
       it's being read */
    bool has_source;
    char file[MAX_PATH];
    struct mp3_albumart aa;
    bool failed;            /* menu_art's has no picture that decodes to it */
    bool tried;             /* asked for since it was drawn last */
    /* the reader's: numbered, as another cover or a cancel leaves the one
       being decoded unshown */
    volatile unsigned wanted;
    volatile bool to_read, reading;
    volatile int fd;        /* the song's while it's decoded, or -1 */
    volatile bool came;     /* shown, not on the screen yet */
} sharp;

void menu_cover_read_cancel(void)
{
    sharp.to_read = false;
    sharp.wanted++;
    if (sharp.reading && sharp.fd >= 0)
        jpeg_stop_fd = sharp.fd;
}

/* menu_art's is another now, or none */
static void sharp_drop(void)
{
    sharp.shown = sharp.came = sharp.failed = sharp.has_source = false;
    menu_cover_read_cancel();
}

/* the settings applied, as after USB: a picture that didn't decode may now */
static void sharp_retry(void)
{
    sharp.failed = false;
}

/* the one it holds is this cover, for this box: shown at once */
static bool sharp_held(const struct itunesdb_art *art, int box_width,
                       int box_height)
{
    if (!sharp.held || sharp.offset != art->offset
        || strcmp(sharp.path, art->path)
        || sharp.box_width != box_width || sharp.box_height != box_height)
        return false;
    menu_art.width = sharp.width;
    menu_art.height = sharp.height;
    sharp.shown = true;
    return true;
}

static void sharp_source(const struct mp3entry *id3)
{
    /* music plays: a refusal of the block is forgotten, as in sharp_memory() */
    sharp.refused = false;
    if (!sharp.pixels || sharp.shown || sharp.failed || sharp.to_read
        || sharp.reading)
        return;
    /* only JPEG can be decoded */
    sharp.has_source = id3->has_embedded_albumart
        && (id3->albumart.type & AA_CLEAR_FLAGS_MASK) == AA_TYPE_JPG;
    if (sharp.has_source
        && (sharp.aa.pos != id3->albumart.pos
            || sharp.aa.size != id3->albumart.size
            || sharp.aa.type != id3->albumart.type
            || strcmp(sharp.file, id3->path)))
    {
        strmemccpy(sharp.file, id3->path, sizeof(sharp.file));
        sharp.aa = id3->albumart;
    }
}

static inline const fb_data *art_pixels(void)
{
    return sharp.shown ? sharp.pixels : menu_art.pixels;
}
#else
#ifdef HAVE_MENU_COVER
void menu_cover_read_cancel(void) {}
bool menu_cover_to_read(void) { return false; }
void menu_cover_read(void) {}
void menu_cover_memory(void) {}
#endif
static inline void sharp_drop(void) {}
static inline void sharp_retry(void) {}
static inline bool sharp_held(const struct itunesdb_art *art, int box_width,
                              int box_height)
{
    (void)art; (void)box_width; (void)box_height;
    return false;
}
static inline void sharp_source(const struct mp3entry *id3) { (void)id3; }
static inline const fb_data *art_pixels(void) { return menu_art.pixels; }
#endif /* MENU_COVER_SHARP */

void menu_cover_forget(void)
{
    menu_art.width = 0;
    sharp_drop();
#ifdef MENU_COVER_SHARP
    sharp.held = false;
#endif
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
    {
        sharp_source(id3);
        return true;
    }

    struct bitmap bm;
    memset(&bm, 0, sizeof(bm));
    bm.width = MIN(aa->width, ART_MENU_SIZE);
    bm.height = MIN(aa->height, ART_MENU_SIZE);
    bm.data = (unsigned char *)menu_art.pixels;
    menu_art.width = 0;
    sharp_drop();
    menu_art.box_width = bm.width;
    menu_art.box_height = bm.height;
    if (sharp_held(&art, bm.width, bm.height))
    {
        menu_art.offset = art.offset;
        strcpy(menu_art.path, art.path);
        menu_art.handle = -1;
        return true;
    }
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
    sharp_source(id3);
    return true;
}

const fb_data *menu_cover_pixels(int *width, int *height)
{
    *width = menu_art.width;
    *height = menu_art.height;
    return art_pixels();
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
   Cover Flow's threads and the album cover reader, for a list, end it first;
   for this cover the reader is made beside it (reader_slot()). */
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

/* a slot for the album cover reader too, this thread's and the spare one
   left: it's made beside this one, not in its place */
static bool reader_slot(void)
{
    const bool made = cop_thread_id || cop_ending || cop_unusable;
    return free_slots() > COP_SPARE_SLOTS + (made ? 0 : 1);
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
static inline bool reader_slot(void) { return true; }
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
    const fb_data *src = art_pixels() + pan.row * menu_art.width + h / 2;
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

#ifdef MENU_COVER_SHARP
#if !defined(SIMULATOR) && (CONFIG_STORAGE & STORAGE_ATA) \
    && defined(HAVE_DISK_STORAGE)
#define DISK_SPINS 1
#else
#define DISK_SPINS 0
#endif
/* how long the menus rest before it's decoded, and what a disk that spins
   has left then before it would stop */
#define SHARP_REST  (HZ / 2)
#define SHARP_SPINS (2 * HZ)

#if DISK_SPINS
static bool disk_spins(void)
{
    return !ata_disk_isssd();
}
#endif

/* Flash, or a disk that spins on: one asleep isn't woken for this */
static bool disk_free(void)
{
#if DISK_SPINS
    if (!disk_spins())
        return true;
    const long spindown = global_settings.disk_spindown * HZ;
    return storage_disk_is_active()
           && (!spindown
               || TIME_BEFORE(current_tick + SHARP_SPINS,
                              storage_last_disk_activity() + spindown));
#else
    return true;
#endif
}

/* On the scroll thread, as the cover shows and the menus rest: the song's
   picture asked of the reader, once each time the cover's drawn */
static void sharp_ask(void)
{
    if (!sharp.pixels || sharp.shown || sharp.failed || sharp.tried
        || sharp.to_read || sharp.reading
        || !sharp.has_source || menu_art.handle >= 0
        || TIME_BEFORE(current_tick, action_last_tick() + SHARP_REST))
        return;
    if (!disk_free())
        return;
    sharp.tried = true;
    if (!reader_slot())
        return;
    sharp.held = false;
    sharp.to_read = true;
    if (!album_header_reader_start_beside())
        sharp.to_read = false;
}

/* The picture decoded against Finder's cover, by 8 by 8 parts of the two,
   every third pixel each way: a part's colours on average, in sixteenths of
   the display's levels. In the decoder's room, which is free by then. */
#define PARTS       64
#define PART_STEP   3
struct part { short own[3], finder[3]; };

static struct part *parts(void)
{
    return (struct part *)(sharp.pixels + ART_MENU_SIZE * ART_MENU_SIZE);
}

static void add_colours(int *sum, fb_data pixel)
{
    const unsigned p = FB_UNPACK_SCALAR_LCD(pixel);
    sum[0] += RGB_UNPACK_RED_LCD(p);
    sum[1] += RGB_UNPACK_GREEN_LCD(p);
    sum[2] += RGB_UNPACK_BLUE_LCD(p);
}

static void measure_parts(int width, int height)
{
    struct part *part = parts();
    for (int i = 0; i < PARTS; i++, part++)
    {
        const int x0 = i % 8 * width / 8, x1 = (i % 8 + 1) * width / 8;
        const int y0 = i / 8 * height / 8, y1 = (i / 8 + 1) * height / 8;
        int own[3] = { 0, 0, 0 }, finder[3] = { 0, 0, 0 }, n = 0;
        for (int y = y0; y < y1; y += PART_STEP)
        {
            for (int x = x0; x < x1; x += PART_STEP, n++)
            {
                add_colours(own, sharp.pixels[y * width + x]);
                add_colours(finder, menu_art.pixels[y * width + x]);
            }
        }
        for (int c = 0; c < 3; c++)
        {
            part->own[c] = n ? own[c] * 16 / n : 0;
            part->finder[c] = n ? finder[c] * 16 / n : 0;
        }
    }
}

/* a part's brightness, in quarters of a level of 255 */
static int light(const short *colours)
{
    return (2 * (colours[0] * 255 / (LCD_MAX_RED * 4))
            + 5 * (colours[1] * 255 / (LCD_MAX_GREEN * 4))
            + colours[2] * 255 / (LCD_MAX_BLUE * 4)) / 8;
}

/* It is the cover Finder made, not another picture the song has: Finder's
   may be lighter or darker all over, as it maps colours its own way, so by
   the parts' differences in brightness less their mean, 10 levels on
   average at most, and that mean 24 */
#define SAME_APART (10 * 4)
#define SAME_TONE  (24 * 4)
static bool same_picture(void)
{
    const struct part *part = parts();
    int tone = 0, apart = 0;
    for (int i = 0; i < PARTS; i++)
        tone += light(part[i].finder) - light(part[i].own);
    tone /= PARTS;
    for (int i = 0; i < PARTS; i++)
        apart += abs(light(part[i].finder) - light(part[i].own) - tone);
    return abs(tone) <= SAME_TONE && apart <= SAME_APART * PARTS;
}

/* Finder's colours given to the picture decoded, which is some 3 % darker,
   so that it takes the cover's place without a change of tone: each
   colour's levels moved along the line that fits the parts' best, by a
   table. False if it was cancelled meanwhile: it yields. */
static bool match_colours(int width, int height, unsigned wanted)
{
    static const unsigned char most[3] =
        { LCD_MAX_RED, LCD_MAX_GREEN, LCD_MAX_BLUE };
    static unsigned char table[3][LCD_MAX_GREEN + 1];
    const struct part *part = parts();
    for (int c = 0; c < 3; c++)
    {
        long own = 0, finder = 0;
        for (int i = 0; i < PARTS; i++)
        {
            own += part[i].own[c];
            finder += part[i].finder[c];
        }
        own /= PARTS;
        finder /= PARTS;
        long spread = 0, along = 0;
        for (int i = 0; i < PARTS; i++)
        {
            const long d = part[i].own[c] - own;
            spread += d * d;
            along += d * (part[i].finder[c] - finder);
        }
        /* the slope in 256ths; 1 for a picture too flat to tell, and never
           far from it */
        long slope = spread >> 8 >= 256 ? along / (spread >> 8) : 256;
        slope = MIN(MAX(slope, 192), 341);
        for (int v = 0; v <= most[c]; v++)
        {
            const long to = slope * (v * 16 - own) / 256 + finder;
            table[c][v] = MIN(MAX((to + 8) >> 4, 0), most[c]);
        }
    }
    for (int y = 0; y < height; y++)
    {
        fb_data *row = sharp.pixels + y * width;
        for (int x = 0; x < width; x++)
        {
            const unsigned p = FB_UNPACK_SCALAR_LCD(row[x]);
            row[x] = FB_SCALARPACK_LCD(
                LCD_RGBPACK_LCD(table[0][RGB_UNPACK_RED_LCD(p)],
                                table[1][RGB_UNPACK_GREEN_LCD(p)],
                                table[2][RGB_UNPACK_BLUE_LCD(p)]));
        }
        if ((y & 15) == 15)
        {
            yield();
            if (wanted != sharp.wanted)
                return false;
        }
    }
    return true;
}

/* 1 decoded into the block, to menu_art's size; 0 not looked at or stopped;
   -1 none that would be sharper, or of the cover's proportions, or that
   decodes, as a progressive one, or a read that failed */
static int sharp_decode(int fd, int width, int height)
{
    struct bitmap bm;
    memset(&bm, 0, sizeof(bm));
    bm.data = (unsigned char *)sharp.pixels;
    const int format = FORMAT_NATIVE | FORMAT_DITHER;
    /* its own size first, which is all this reads */
    if (lseek(fd, sharp.aa.pos, SEEK_SET) < 0)
        return 0;
    if (clip_jpeg_fd(fd, sharp.aa.type, sharp.aa.size, &bm, SHARP_BYTES,
                     format | FORMAT_RETURN_SIZE, NULL) <= 0)
        return -1;
    const long across = (long)bm.width * height, down = (long)bm.height * width;
    if (bm.width < width || bm.height < height
        || (across > down ? across - down : down - across) * 100 > across * 3)
        return -1;
    if (lseek(fd, sharp.aa.pos, SEEK_SET) < 0)
        return 0;
    bm.width = width;
    bm.height = height;
#if DISK_SPINS && defined(HAVE_ATA_DMA)
    /* as list_covers.c */
    ata_dma_yield_always(true);
#endif
    const int rc = clip_jpeg_fd(fd, sharp.aa.type, sharp.aa.size, &bm,
                                SHARP_BYTES, format | FORMAT_RESIZE, NULL);
#if DISK_SPINS && defined(HAVE_ATA_DMA)
    ata_dma_yield_always(false);
#endif
    if (rc > 0)
        return 1;
    return jpeg_stop_fd == fd ? 0 : -1;
}

bool menu_cover_to_read(void)
{
    return sharp.to_read;
}

void menu_cover_read(void)
{
    const unsigned wanted = sharp.wanted;
    const int width = menu_art.width, height = menu_art.height;
    sharp.to_read = false;
    /* never into the block while it's drawn from */
    if (sharp.shown)
        return;
    sharp.fd = -1;
    /* the block is let go only once this is false */
    sharp.reading = true;
    /* a disk that has stopped since the ask isn't woken */
    const int fd = disk_free() ? open(sharp.file, O_RDONLY) : -1;
    int rc = 0;
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    /* the scaler does so itself, but a size that needs none (2, 4, 8 times)
       has no boost */
    cpu_boost(true);
#endif
    if (fd >= 0)
    {
        if (wanted == sharp.wanted)
        {
            sharp.fd = fd;
            rc = sharp_decode(fd, width, height);
            sharp.fd = -1;
            if (jpeg_stop_fd == fd)
                jpeg_stop_fd = -1;
        }
        close(fd);
    }
    if (rc > 0 && wanted == sharp.wanted)
    {
        measure_parts(width, height);
        if (!same_picture())
            rc = -1;
        else if (!match_colours(width, height, wanted))
            rc = 0;
    }
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    cpu_boost(false);
#endif
    sharp.reading = false;
    if (wanted != sharp.wanted || !rc)
        return;
    if (rc < 0)
    {
        sharp.failed = true;
        return;
    }
    sharp.offset = menu_art.offset;
    strcpy(sharp.path, menu_art.path);
    sharp.width = width;
    sharp.height = height;
    sharp.box_width = menu_art.box_width;
    sharp.box_height = menu_art.box_height;
    sharp.held = sharp.shown = sharp.came = true;
}

/* The block as the settings have it: let go with Moving Cover or Album Art
   off; taken with both on, only without music, which would stop for it */
static void sharp_memory(bool wanted)
{
    if (!wanted)
    {
        sharp.refused = false;
        if (!sharp.handle)
            return;
        /* Finder's scaled again */
        if (sharp.shown)
            menu_art.width = 0;
        sharp_drop();
        while (sharp.reading)
            sleep(1);
        sharp.held = false;
        sharp.pixels = NULL;
        sharp.handle = core_free(sharp.handle);
        return;
    }
    /* a refusal is forgotten as music plays: each stop of it tries once */
    if (audio_status())
        sharp.refused = false;
    if (sharp.handle || sharp.refused || audio_status())
        return;
    const int handle = core_alloc_ex(SHARP_BYTES, &buflib_ops_locked);
    if (handle > 0)
    {
        sharp.handle = handle;
        sharp.pixels = core_get_data(handle);
    }
    else
        sharp.refused = true;
}

void menu_cover_memory(void)
{
    sharp_memory(global_settings.moving_cover
                 && global_settings.album_art != AA_OFF);
}

/* the sharp one that came isn't on the screen: the cover drawn again where
   it is, unless a step is about to draw it */
static bool sharp_to_draw(long until)
{
    return sharp.came && until > PAN_LATENCY;
}

/* the cover's drawn from the one shown; all of the theme with it, and the
   reader may be asked again */
static void sharp_drawn(bool theme)
{
    sharp.came = false;
    if (theme)
        sharp.tried = false;
}

/* On the scroll thread, at each look: a disk that spins is kept from
   stopping under a decode asked for or going on, which would wake it, as the
   buffer's fill may end meanwhile and send it to sleep. After the decode it
   runs out its own timer. */
static void sharp_tick(void)
{
#if DISK_SPINS
    if ((sharp.to_read || sharp.reading) && disk_spins())
        storage_spin();
#endif
}
#else
static inline void sharp_ask(void) {}
static inline void sharp_memory(bool wanted) { (void)wanted; }
static inline bool sharp_to_draw(long until) { (void)until; return false; }
static inline void sharp_drawn(bool theme) { (void)theme; }
static inline void sharp_tick(void) {}
#endif /* MENU_COVER_SHARP */

/* On the scroll thread: a step when it's due; the ticks until the next look */
static long pan_tick(void)
{
    cop_settle();
    const bool can_go = pan_can_go();
    if (can_go)
        sharp_ask();
    sharp_tick();
    if (!can_go)
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
    if (h != pan_h || sharp_to_draw(until))
    {
        if (!cop_free())
            return 1;
        /* shown PAN_LATENCY after it was due by the clock, not as the thread
           woke or the frame was ready: the steps go evenly */
        const bool on_time = !TIME_AFTER(pan_due, now)
                             && !TIME_AFTER(now, pan_due + PAN_LATENCY);
        pan_draw(h, (on_time ? pan_due : now) + PAN_LATENCY);
        pan_h = h;
        sharp_drawn(false);
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
    sharp_drop();
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
    sharp_drawn(true);
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
    sharp_retry();
    sharp_memory(on && global_settings.album_art != AA_OFF);
    /* turned on, it starts from the middle, where it's drawn while off */
    pan_clock = 0;
    pan_going = false;
    scroll_set_tick_hook(on ? pan_tick : NULL);
    skin_request_full_update(CUSTOM_STATUSBAR);
}

#endif /* HAVE_MENU_COVER */
