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
#include "pcmbuf.h"
#include "pcm_mixer.h"
#include "appevents.h"
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
#include "rbpaths.h"
#include "debug.h"

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
    bool crisp;         /* a picture at its own size, not Finder's scaled:
                           it moves by whole pixels */
} menu_art;

#if defined(HAVE_MENU_COVER) && defined(HAVE_JPEG) && defined(HAVE_TAGCACHE)
#define MENU_COVER_SHARP
/* Sharp: the picture in the song's own file, decoded by the album cover
   reader's thread to the size Finder's was scaled to, takes its place, moving
   or still. From flash as its track starts, to be there when the menus show;
   from a disk that spins only as the cover shows. In a block held while
   Album Art is on, taken without music (sharp_memory()): the pixels, then
   the decoder's room, its state 12.5 KB, a row of blocks up to 30.7 KB and
   the scaler's rows 11.5 KB. The UI's thread, the scroll's, the audio's and
   the reader's are on one core: none of their changes here has a yield in
   the middle. */
#define SHARP_ROOM  (54 * 1024)
#define SHARP_BYTES (sizeof(menu_art.pixels) + SHARP_ROOM)
/* a cover by Finder's, as menu_art's */
struct cover_key
{
    uint32_t offset;
    char path[sizeof(menu_art.path)];
};
static struct
{
    int handle;             /* the block's, 0 for none */
    fb_data *pixels;        /* its start: it doesn't move */
    bool refused;           /* no room: not again until music has played */
    bool shown;             /* drawn in place of menu_art.pixels */
    /* the cover it holds, and the size */
    bool held;
    struct cover_key key;
    short width, height, box_width, box_height;
    short box_now_width, box_now_height; /* what the theme's panel takes */
    /* the song playing, by its track's event: its cover, Finder's frame of
       it, and its own picture's place if that's a JPEG */
    bool now;
    struct cover_key now_key;
    short now_frame_width, now_frame_height;
    bool now_source;
    char now_file[MAX_PATH];
    struct mp3_albumart now_aa;
    /* the picture asked of the reader, which these aren't changed under */
    struct cover_key job_key;
    short job_frame_width, job_frame_height;
    short job_width, job_height, job_box_width, job_box_height;
    char file[MAX_PATH];
    struct mp3_albumart aa;
    bool failed;            /* failed_key's has no picture that decodes to it */
    struct cover_key failed_key;
    bool now_large;         /* Finder may have the playing song's cover at
                               the panel's size: asked without the floor */
    bool job_fetch;         /* asked so: no decode if it isn't to be had */
    unsigned job_forgets;   /* kept_forgets at the ask */
    bool unfetched;         /* unfetched_key's wasn't: by the decode's rules */
    struct cover_key unfetched_key;
    long back_off;          /* not asked again before this tick */
    bool gave_way;          /* a decode was given up for the reader's other
                               work, and none asked since */
    /* the reader's: numbered, as a cancel leaves the one being decoded
       unshown */
    volatile unsigned wanted;
    volatile bool to_read, reading;
    volatile int fd;        /* the song's while it's decoded, or -1 */
    volatile bool hold;     /* the reader waits: little music is buffered */
    unsigned reader;        /* its thread, while it reads */
    bool raised;            /* that thread has the UI's priority and turn */
    volatile bool came;     /* shown, not on the screen yet */
    /* With Moving Cover on, two more copies of the cover it holds, in a
       block of their own: a quarter and three quarters of a pixel on, the
       frames it moves by. A whole pixel's frame is crisp and a half's the
       mean of two, and one after the other trembled; these are as soft as
       each other. */
    int phase_handle;
    fb_data *phase[2];
    bool phase_refused;
    bool phased;            /* they are of the cover it holds */
    bool job_phases;        /* the reader makes them only: the cover is held */
} sharp;

static bool same_key(const struct cover_key *key, uint32_t offset,
                     const char *path)
{
    return key->offset == offset && !strcmp(key->path, path);
}

static void set_key(struct cover_key *key, uint32_t offset, const char *path)
{
    key->offset = offset;
    strmemccpy(key->path, path, sizeof(key->path));
}

/* the decode asked for or going on is given up */
static void job_cancel(void)
{
    sharp.to_read = false;
    sharp.wanted++;
    if (sharp.reading && sharp.fd >= 0)
        jpeg_stop_fd = sharp.fd;
}

/* For the reader's other work, which comes first: not asked for 2 s after
   each ask of that, as after an ask of this that came to nothing; and once
   a decode was given up for it, not until none has come for 10 s or the
   theme draws the cover: a list would have it given up at each of its
   covers. */
#define SHARP_GIVE_WAY (10 * HZ)
#define SHARP_AGAIN    (2 * HZ)
void menu_cover_read_cancel(void)
{
    if (sharp.to_read || sharp.reading)
        sharp.gave_way = true;
    job_cancel();
    sharp.back_off = current_tick
                     + (sharp.gave_way ? SHARP_GIVE_WAY : SHARP_AGAIN);
}

/* menu_art's is another now, or none: not drawn from the block */
static void sharp_undraw(void)
{
    sharp.shown = sharp.came = false;
}

/* the settings applied, as after USB: a picture that didn't decode may now */
static void sharp_retry(void)
{
    sharp.failed = sharp.unfetched = false;
}

/* the one it holds is this cover, for this box: shown at once */
static bool sharp_held(const struct itunesdb_art *art, int box_width,
                       int box_height)
{
    if (!sharp.held || !same_key(&sharp.key, art->offset, art->path)
        || sharp.box_width != box_width || sharp.box_height != box_height)
        return false;
    menu_art.width = sharp.width;
    menu_art.height = sharp.height;
    sharp.shown = true;
    return true;
}

/* the theme's panel takes a cover this large: the size to decode to before
   the menus show */
static void sharp_panel(int box_width, int box_height)
{
    sharp.box_now_width = box_width;
    sharp.box_now_height = box_height;
}

/* music plays: a refusal of a block is forgotten, as in sharp_memory() */
static void sharp_plays(void)
{
    sharp.refused = sharp.phase_refused = false;
}

static inline const fb_data *art_pixels(void)
{
    return sharp.shown ? sharp.pixels : menu_art.pixels;
}

/* the frame of the cover as it moves, at the half column h: NULL without
   them, as for Finder's */
static inline const fb_data *moving_pixels(int h)
{
    return sharp.shown && sharp.phased ? sharp.phase[h & 1] : NULL;
}

/* the cover drawn is a crisp one with no such frames to move by: the
   block's, or one the menus fetched that the block couldn't take */
static inline bool sharp_whole(void)
{
    return sharp.shown ? !sharp.phased : menu_art.crisp;
}

static bool disk_free(void);

/* the size Finder's frame is scaled to in a box, as itunesdb.c's art_fit()
   has it */
static void cover_fit(int frame_width, int frame_height, int *width,
                      int *height)
{
    if (*width * frame_height > *height * frame_width)
        *width = (*height * frame_width + frame_height / 2) / frame_height;
    else
        *height = (*width * frame_height + frame_width / 2) / frame_width;
}

/* The decoded covers kept, for the songs Finder wrote no frame of the
   panel's size for: read back the next time in place of the decode. The
   owner's exception to making such files at Updating Library only. A table,
   then up to KEPT_SLOTS slots, each the block's pixels and a sector that
   says whose they are, written after them; the file grows a slot at a time
   and, full, the oldest is written over. All of it is dropped when the
   covers it stands for may have changed: by menu_cover_kept_forget() as
   an import finds them changed, and at the load if the table isn't of the
   covers' indexes as they are (identity), as after another firmware's
   import. A decode asked before a drop isn't stored after it. */
#define KEPT_FILE    ROCKBOX_DIR "/menu_covers.dat"
#define KEPT_MAGIC   0x4b4d4252 /* "RBMK" */
#define KEPT_VERSION 2      /* 2: identity */
#define KEPT_SLOTS   48
#define KEPT_TABLE   2048
#define KEPT_HEAD    512
#define KEPT_SLOT    ((off_t)(sizeof(menu_art.pixels) + KEPT_HEAD))
#define KEPT_WRITE   (16 * 1024)
struct kept_entry
{
    uint32_t offset;        /* the cover's key: its place and file's name */
    uint32_t path;
    uint32_t serial;        /* in the order stored, from 1; 0 for none */
    uint16_t width, height;
};
static struct
{
    uint32_t magic, version;
    uint32_t serial;        /* the last given */
    uint32_t identity;      /* itunesdb_covers_identity() as it was stored */
    struct kept_entry slot[KEPT_SLOTS];
} kept;
static bool kept_loaded;
static int kept_writing = -1;   /* the slot being written, not to be read */
static unsigned kept_forgets;

static uint32_t kept_path(const char *path)
{
    uint32_t hash = 2166136261u;
    while (*path)
        hash = (hash ^ (unsigned char)*path++) * 16777619u;
    return hash;
}

static off_t kept_place(int slot)
{
    return KEPT_TABLE + slot * KEPT_SLOT;
}

/* the table, once: on the UI's thread or the reader's, never the look's */
static void kept_load(void)
{
    const uint32_t identity = itunesdb_covers_identity();
    if (kept_loaded || !identity)
        return;
    kept_loaded = true;
    const int fd = open(KEPT_FILE, O_RDONLY);
    const off_t size = fd >= 0 ? ffilesize(fd) : 0;
    if (fd < 0 || read(fd, &kept, sizeof(kept)) != sizeof(kept)
        || kept.magic != KEPT_MAGIC || kept.version != KEPT_VERSION
        || kept.identity != identity)
        memset(&kept, 0, sizeof(kept));
    kept.magic = KEPT_MAGIC;
    kept.version = KEPT_VERSION;
    kept.identity = identity;
    if (fd >= 0)
        close(fd);
    /* no slot the file doesn't have */
    for (int i = 0; i < KEPT_SLOTS; i++)
    {
        if (kept_place(i + 1) > size)
            kept.slot[i].serial = 0;
    }
}

static int kept_slot(const struct cover_key *key, int width, int height)
{
    if (!kept_loaded)
        return -1;
    const uint32_t path = kept_path(key->path);
    for (int i = 0; i < KEPT_SLOTS; i++)
    {
        const struct kept_entry *e = &kept.slot[i];
        if (e->serial && i != kept_writing && e->offset == key->offset
            && e->path == path && e->width == width && e->height == height)
            return i;
    }
    return -1;
}

/* a slot's picture, if its own sector says it is the table's */
static bool kept_read(int slot, fb_data *to)
{
    const struct kept_entry *e = &kept.slot[slot];
    struct kept_entry head;
    const off_t place = kept_place(slot);
    const off_t head_at = place + (off_t)sizeof(menu_art.pixels);
    const ssize_t size = e->width * e->height * sizeof(fb_data);
    const int fd = open(KEPT_FILE, O_RDONLY);
    const bool ok = fd >= 0 && lseek(fd, head_at, SEEK_SET) == head_at
                    && read(fd, &head, sizeof(head)) == sizeof(head)
                    && !memcmp(&head, e, sizeof(head))
                    && lseek(fd, place, SEEK_SET) == place
                    && read(fd, to, size) == size;
    if (fd >= 0)
        close(fd);
    if (!ok)
        kept.slot[slot].serial = 0;
    return ok;
}

void menu_cover_kept_forget(void)
{
    kept_forgets++;
    memset(&kept, 0, sizeof(kept));
    kept.magic = KEPT_MAGIC;
    kept.version = KEPT_VERSION;
    kept_loaded = true;
    remove(KEPT_FILE);
}

/* The cover of key at that size without a decode, into to: 1 from Finder's
   frame of the panel's size, which the song at file has if Finder was told
   of the format; 2 as kept after its decode; 0 neither. Reads the disk. */
static int cover_fetch(const char *file, const struct cover_key *key,
                       int width, int height, fb_data *to)
{
    struct itunesdb_large_art large;
    if (itunesdb_find_large_art(file, &large) && large.width == width
        && large.height == height && itunesdb_load_large_art(&large, to) > 0)
        return 1;
    kept_load();
    const int slot = kept_slot(key, width, height);
    return slot >= 0 && kept_read(slot, to) ? 2 : 0;
}

/* The block takes the cover the menus have just fetched, if the reader
   isn't in it: drawn from it then, and its moving frames are the look's to
   ask. */
static void sharp_take(const struct cover_key *key, int width, int height,
                       int box_width, int box_height)
{
    if (!sharp.pixels || sharp.to_read || sharp.reading)
        return;
    memcpy(sharp.pixels, menu_art.pixels, width * height * sizeof(fb_data));
    sharp.key = *key;
    sharp.width = width;
    sharp.height = height;
    sharp.box_width = box_width;
    sharp.box_height = box_height;
    sharp.held = true;
    sharp.phased = false;
    sharp.shown = true;
}
#else
#ifdef HAVE_MENU_COVER
void menu_cover_read_cancel(void) {}
bool menu_cover_to_read(void) { return false; }
void menu_cover_read(void) {}
void menu_cover_memory(void) {}
#endif
static inline void job_cancel(void) {}
static inline void sharp_undraw(void) {}
static inline void sharp_retry(void) {}
static inline bool sharp_held(const struct itunesdb_art *art, int box_width,
                              int box_height)
{
    (void)art; (void)box_width; (void)box_height;
    return false;
}
static inline void sharp_panel(int box_width, int box_height)
{
    (void)box_width; (void)box_height;
}
static inline void sharp_plays(void) {}
static inline const fb_data *art_pixels(void) { return menu_art.pixels; }
static inline const fb_data *moving_pixels(int h) { (void)h; return NULL; }
static inline bool sharp_whole(void) { return false; }
#ifdef HAVE_MENU_COVER
void menu_cover_kept_forget(void) {}
#endif
#endif /* MENU_COVER_SHARP */

void menu_cover_forget(void)
{
    menu_art.width = 0;
    sharp_undraw();
    job_cancel();
#ifdef MENU_COVER_SHARP
    /* a cover's key may be another's now: the next track's is taken anew */
    sharp.held = sharp.phased = sharp.failed = sharp.now = false;
    sharp.unfetched = false;
#endif
}

bool menu_cover_ready(const struct wps_data *data)
{
    struct skin_albumart *aa = SKINOFFSETTOPTR(get_skin_buffer((struct wps_data *)data),
                                               data->albumart);
    struct mp3entry *id3 = audio_current_track();
    struct itunesdb_art art;
    if (aa && data == skin_get_gwps(CUSTOM_STATUSBAR, SCREEN_MAIN)->data)
        sharp_panel(MIN(aa->width, ART_MENU_SIZE),
                    MIN(aa->height, ART_MENU_SIZE));
    if (!aa || !id3 || !(audio_status() & AUDIO_STATUS_PLAY)
        || global_settings.album_art == AA_OFF
        || !itunesdb_find_track_art(id3, id3->path, &art))
        return false;
    sharp_plays();
    if (menu_art.width > 0 && menu_art.handle < 0 && menu_art.offset == art.offset
        && !strcmp(menu_art.path, art.path))
        return true;

    struct bitmap bm;
    memset(&bm, 0, sizeof(bm));
    bm.width = MIN(aa->width, ART_MENU_SIZE);
    bm.height = MIN(aa->height, ART_MENU_SIZE);
    bm.data = (unsigned char *)menu_art.pixels;
    const int box_width = bm.width, box_height = bm.height;
    menu_art.width = 0;
    menu_art.crisp = false;
    sharp_undraw();
    if (sharp_held(&art, box_width, box_height))
    {
        menu_art.offset = art.offset;
        strcpy(menu_art.path, art.path);
        menu_art.handle = -1;
        return true;
    }
#ifdef MENU_COVER_SHARP
    /* Finder's frame of this size, or the decoded cover kept: there from
       the first frame, with no decode to wait for */
    int width = box_width, height = box_height;
    struct cover_key key;
    cover_fit(art.width, art.height, &width, &height);
    set_key(&key, art.offset, art.path);
    /* not from a disk that has stopped or is about to: Finder's scaled
       then, and the look asks the reader by its rule */
    const int fetched = disk_free()
        ? cover_fetch(id3->path, &key, width, height, menu_art.pixels) : 0;
    if (fetched)
    {
        menu_art.width = width;
        menu_art.height = height;
        menu_art.offset = art.offset;
        strcpy(menu_art.path, art.path);
        menu_art.handle = -1;
        menu_art.crisp = true;
        sharp_take(&key, width, height, box_width, box_height);
        DEBUGF("menu cover: %s, at the render\n",
               fetched == 1 ? "Finder's frame" : "kept");
        return true;
    }
#endif
    /* Finder's scaled: from the cover in the buffer if it's as large, as a
       disk that spins is often off by the time the menus open, and turning
       it on takes 0.6 s */
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
    /* its decode may have ended during the read above: the block's, then */
    sharp_held(&art, box_width, box_height);
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
static bool pan_still;          /* drawn so, but not to move: Moving Cover
                                   off, or no room for it */
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

/* the UI waits, the cover on the screen as it was drawn: a step, or the
   sharp cover's frame, may be drawn over it */
static bool cover_idle(void)
{
    return menu_art.width == pan.width && menu_art.height == pan.height
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

static bool pan_can_go(void)
{
    return pan_shown && global_settings.moving_cover && cover_idle();
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
   left: it's made beside this one, not in its place. None kept for this
   thread with Moving Cover off or the cover still, which don't make it. */
static bool reader_slot(void)
{
    const bool made = !global_settings.moving_cover || pan_still
                      || cop_thread_id || cop_ending || cop_unusable;
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

/* show_at: the tick that shows it, where the display can wait for one. The
   one frame of a still cover is the CPU's: no thread on the COP for it. */
static void pan_draw(int h, long show_at, bool still)
{
    struct viewport *vp = lcd_set_viewport_ex(NULL, 0);
    const int width = pan.x1 - pan.x0;
    /* the sharp cover moving: its frames, made ahead; Finder's, or a sharp
       one without them: the whole column's, or the mean of two */
    const fb_data *frame = still ? NULL : moving_pixels(h);
    const fb_data *src = (frame ? frame : art_pixels())
                         + pan.row * menu_art.width + h / 2;
    for (int y = pan.y0; y < pan.y1; y++, src += menu_art.width)
    {
        if ((h & 1) && !frame)
            blend_row(FBADDR(pan.x0, y), src, width);
        else
            memcpy(FBADDR(pan.x0, y), src, width * sizeof(fb_data));
    }
#ifdef PAN_ON_COP
    if (!still && cop_ready())
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
    (void)still;
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

static long pan_tick(void);

#ifdef MENU_COVER_SHARP
#if !defined(SIMULATOR) && (CONFIG_STORAGE & STORAGE_ATA) \
    && defined(HAVE_DISK_STORAGE)
#define DISK_SPINS 1
#else
#define DISK_SPINS 0
#endif
/* from a disk that spins: how long the menus rest before it's decoded, and
   what the disk has left then before it would stop */
#define SHARP_REST  (HZ / 2)
#define SHARP_SPINS (2 * HZ)
/* The music buffered, in ms, under which the reader isn't asked and, at
   work, waits. From flash, what pcmbuf.c calls low (LOW_DATA): the
   reader's priority is under the decoder's however full the buffer, so a
   decoder that wants the CPU is passed over for it once in five switches
   of threads at most. From a disk that spins, where the reader's reads
   take the head from the file being buffered, three times that. */
#define SHARP_PCM_FLOOR      1000
#define SHARP_PCM_FLOOR_DISK 3000

static bool disk_spins(void)
{
#if DISK_SPINS
    return !ata_disk_isssd();
#else
    return false;
#endif
}

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

/* The reader's turn. At the background's priority, a block of the picture
   to a yield, it has what the other threads leave, and at a track's start,
   as the song is buffered and decoded ahead, they leave little. */
#define SHARP_PCM_OVER 1000
static void reader_turn(bool raised)
{
    if (raised == sharp.raised)
        return;
    sharp.raised = raised;
#ifdef HAVE_PRIORITY_SCHEDULING
    thread_set_priority(sharp.reader, raised ? PRIORITY_USER_INTERFACE
                                             : PRIORITY_BACKGROUND);
#endif
    jpeg_turn_thread = raised ? sharp.reader : 0;
}

/* what's buffered to play: 16 bits, two channels */
static unsigned long pcm_ms(void)
{
    return (pcmbuf_get_bufsize() - pcmbuf_free())
           / (mixer_get_frequency() * 4 / 1000);
}

/* A track has begun, on the audio's thread: its cover and its own picture's
   place, for the look */
static void track_begun(unsigned short id, void *data)
{
    (void)id;
    const struct mp3entry *id3 = ((struct track_event *)data)->id3;
    struct itunesdb_art art;
    sharp.now = itunesdb_find_track_art(id3, id3->path, &art);
    if (!sharp.now)
        return;
    set_key(&sharp.now_key, art.offset, art.path);
    sharp.now_frame_width = art.width;
    sharp.now_frame_height = art.height;
    struct itunesdb_art finders;
    sharp.now_large = itunesdb_has_large_art()
                      && itunesdb_find_art(id3->path, &finders)
                      && finders.offset == art.offset;
    /* only JPEG can be decoded */
    sharp.now_source = id3->has_embedded_albumart
        && (id3->albumart.type & AA_CLEAR_FLAGS_MASK) == AA_TYPE_JPG;
    strmemccpy(sharp.now_file, id3->path, sizeof(sharp.now_file));
    sharp.now_aa = id3->albumart;
}

/* On the scroll thread, at each look, ten a second at least, whatever the
   screen shows; shows: the cover is on it, the UI idle. The reader is held
   while little music is buffered, and a disk that spins kept on under a
   decode. The picture of the song playing is asked of the reader if the
   block has another's or none, one at a time: a decode for another cover is
   given up first. */
static void sharp_look(bool shows)
{
    if (!sharp.pixels)
        return;
    const bool playing = audio_status() & AUDIO_STATUS_PLAY;
    const bool busy = sharp.to_read || sharp.reading;
    const unsigned long ms = pcm_ms();
    const unsigned long least = disk_spins() ? SHARP_PCM_FLOOR_DISK
                                             : SHARP_PCM_FLOOR;
    sharp.hold = playing && sharp.reading && ms < least;
    jpeg_hold_fd = sharp.hold ? sharp.fd : -1;
    reader_turn(playing && sharp.reading && ms >= least + SHARP_PCM_OVER
                && action_waiting() && button_queue_empty()
                && !TIME_BEFORE(current_tick, action_last_tick() + SHARP_REST));
#if DISK_SPINS
    if (busy && disk_spins())
        storage_spin();
#endif
    if (!playing || !sharp.now)
        return;
    const struct cover_key *key = &sharp.now_key;
    if (busy && !same_key(&sharp.job_key, key->offset, key->path))
        job_cancel();
    const bool has = sharp.held && same_key(&sharp.key, key->offset, key->path)
                     && sharp.box_width == sharp.box_now_width
                     && sharp.box_height == sharp.box_now_height;
    /* it has the cover, but not its moving frames, as with Moving Cover
       turned on since: the reader makes those */
    const bool frames = has && sharp.phase[0] && !sharp.phased;
    if (sharp.to_read || sharp.reading || (has && !frames)
        || !sharp.box_now_width || !sharp.box_now_height
        || plugin_is_running()
        || TIME_BEFORE(current_tick, sharp.back_off))
        return;
    int width = sharp.box_now_width, height = sharp.box_now_height;
    cover_fit(sharp.now_frame_width, sharp.now_frame_height, &width, &height);
    /* to be had by a read: not for the song's own picture nor the music's
       buffer to wait for */
    const bool fetch = !frames
        && !(sharp.unfetched
             && same_key(&sharp.unfetched_key, key->offset, key->path))
        && (sharp.now_large || kept_slot(key, width, height) >= 0);
    if (!fetch
        && ((!frames
             && ((sharp.failed
                  && same_key(&sharp.failed_key, key->offset, key->path))
                 || !sharp.now_source))
            || ms < least))
        return;
    if (!frames && disk_spins()
        && (!shows
            || TIME_BEFORE(current_tick, action_last_tick() + SHARP_REST)
            || !disk_free()))
        return;
    sharp.gave_way = false;
    /* the cover it has is on the screen: the theme takes this one first */
    if (!frames && sharp.shown && (pan_shown || pan_still))
        return;
    if (!reader_slot())
    {
        sharp.back_off = current_tick + SHARP_AGAIN;
        return;
    }
    sharp.job_phases = frames;
    if (frames)
    {
        sharp.to_read = true;
        if (!album_header_reader_start_beside())
        {
            sharp.to_read = false;
            sharp.back_off = current_tick + SHARP_AGAIN;
        }
        return;
    }
    /* the block is this cover's from here: the one it had, if the menus
       still have it as theirs, is Finder's again when they're next drawn */
    if (sharp.shown)
        menu_art.width = 0;
    sharp_undraw();
    sharp.held = sharp.phased = false;
    sharp.job_key = *key;
    sharp.job_frame_width = sharp.now_frame_width;
    sharp.job_frame_height = sharp.now_frame_height;
    sharp.job_box_width = sharp.box_now_width;
    sharp.job_box_height = sharp.box_now_height;
    sharp.job_width = width;
    sharp.job_height = height;
    sharp.job_fetch = fetch;
    sharp.job_forgets = kept_forgets;
    strcpy(sharp.file, sharp.now_file);
    sharp.aa = sharp.now_aa;
    sharp.to_read = true;
    if (!album_header_reader_start_beside())
    {
        sharp.to_read = false;
        sharp.back_off = current_tick + SHARP_AGAIN;
    }
}

/* The reader gives the CPU up, and waits while it's held. False if its
   decode was cancelled meanwhile. */
static bool reader_yield(unsigned wanted)
{
    yield();
    while (sharp.hold && wanted == sharp.wanted)
        sleep(1);
    return wanted == sharp.wanted;
}

/* The picture read whole into the moving frames' block, which is free until
   they are made, if it's there and holds it: both passes of the decoder
   then read memory, and the file isn't sought twice. 32 KB at a time, as
   the buffering thread reads, the hold and the stop looked at between.
   1 read; 0 no room for it: from the file, as without the block; -1
   stopped, not read, or turned down by its first bytes */
#define SHARP_READ (32 * 1024)

/* The picture's frame, if it's in these first bytes of it: one the decoder
   doesn't take (process_markers()), or of a size sharp_decode() turns
   down. False if the frame isn't found there: that is for the decoder. */
static bool frame_no_use(const unsigned char *at, size_t length, int width,
                         int height)
{
    if (length < 2 || at[0] != 0xff || at[1] != 0xd8)
        return false;
    for (size_t i = 2; i + 9 <= length; )
    {
        const unsigned marker = at[i + 1];
        const size_t segment = at[i + 2] << 8 | at[i + 3];
        if (at[i] != 0xff)
            return false;
        if (marker == 0xff)
        {
            i++;
            continue;
        }
        if (marker == 0xc0 || marker == 0xc1)
        {
            const long across = (long)(at[i + 7] << 8 | at[i + 8]) * height;
            const long down = (long)(at[i + 5] << 8 | at[i + 6]) * width;
            return across < (long)width * height || down < (long)width * height
                   || (across > down ? across - down : down - across) * 100
                      > across * 3;
        }
        if (marker > 0xc0 && marker <= 0xcf && marker != 0xc4
            && marker != 0xcc)
            return true;
        /* the picture's data, or no segment with a length */
        if (marker < 0xc0 || (marker >= 0xd0 && marker <= 0xda)
            || segment < 2)
            return false;
        i += 2 + segment;
    }
    return false;
}

static int picture_whole(int fd, int width, int height)
{
    const size_t size = sharp.aa.size;
    unsigned char *room = (unsigned char *)sharp.phase[0];
    if (!room || !size || size > 2 * sizeof(menu_art.pixels)
        || (sharp.aa.type & AA_FLAG_VORBIS_BASE64))
        return 0;
    if (lseek(fd, sharp.aa.pos, SEEK_SET) < 0)
        return -1;
    for (size_t at = 0; at < size; at += SHARP_READ)
    {
        const ssize_t part = MIN(SHARP_READ, size - at);
        if (read(fd, room + at, part) != part)
            return -1;
        if (!at && !(sharp.aa.type & AA_FLAG_ID3_UNSYNC)
            && frame_no_use(room, part, width, height))
            return -1;
        while (sharp.hold && jpeg_stop_fd != fd)
            sleep(1);
        if (jpeg_stop_fd == fd)
            return -1;
    }
    jpeg_mem = room;
    jpeg_mem_size = size;
    jpeg_mem_fd = fd;
    return 1;
}

/* The picture decoded against Finder's cover, by 8 by 8 parts of the two: a
   part's colours on average, in sixteenths of the display's levels. The
   picture's by every third pixel each way; Finder's from its frame scaled
   to 120 a side as the screen's copy is: that is the tone to match. All in
   the decoder's room, which is free by then. */
#define PARTS       64
#define PART_STEP   3
#define FINDER_SIDE 120
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

/* a part's colours of a picture, every step-th pixel each way */
static void part_colours(short *colours, int i, const fb_data *pixels,
                         int width, int height, int step)
{
    const int x0 = i % 8 * width / 8, x1 = (i % 8 + 1) * width / 8;
    const int y0 = i / 8 * height / 8, y1 = (i / 8 + 1) * height / 8;
    int sum[3] = { 0, 0, 0 }, n = 0;
    for (int y = y0; y < y1; y += step)
    {
        for (int x = x0; x < x1; x += step, n++)
            add_colours(sum, pixels[y * width + x]);
    }
    for (int c = 0; c < 3; c++)
        colours[c] = n ? sum[c] * 16 / n : 0;
}

/* 1 measured; 0 cancelled meanwhile; -1 Finder's frame couldn't be read */
static int measure_parts(int width, int height, unsigned wanted)
{
    struct part *part = parts();
    struct bitmap bm;
    struct itunesdb_art art;
    memset(&bm, 0, sizeof(bm));
    bm.width = bm.height = FINDER_SIDE;
    bm.data = (unsigned char *)(part + PARTS);
    memset(&art, 0, sizeof(art));
    strcpy(art.path, sharp.job_key.path);
    art.offset = sharp.job_key.offset;
    art.width = sharp.job_frame_width;
    art.height = sharp.job_frame_height;
    const int size = itunesdb_load_art(&art, &bm,
        sizeof(bm) + FINDER_SIDE * FINDER_SIDE * sizeof(fb_data));
    if (wanted != sharp.wanted)
        return 0;
    if (size < 0)
        return -1;
    for (int i = 0; i < PARTS; i++)
    {
        part_colours(part[i].own, i, sharp.pixels, width, height, PART_STEP);
        part_colours(part[i].finder, i, (const fb_data *)bm.data, bm.width,
                     bm.height, 1);
        if ((i & 7) == 7 && !reader_yield(wanted))
            return 0;
    }
    return 1;
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
        if ((y & 15) == 15 && !reader_yield(wanted))
            return false;
    }
    return true;
}

/* The two frames the cover moves by, from the block: each pixel three parts
   of one and one of its neighbour, a quarter of a pixel on, or one and
   three, three quarters on; the last column its own. False if it was
   cancelled meanwhile: it yields. */
#if LCD_PIXELFORMAT == RGB565 && LCD_DEPTH == 16
/* the three colours at once: apart in a word, with room over each for the
   sum of four, which mix_pixel() divides and packs */
#define MIX_APART 0x07e0f81fu
#define MIX_ROUND 0x00401002u
static inline uint32_t mix_apart(fb_data pixel)
{
    return (pixel | (uint32_t)pixel << 16) & MIX_APART;
}

static inline fb_data mix_pixel(uint32_t sum)
{
    sum = ((sum + MIX_ROUND) >> 2) & MIX_APART;
    return sum | sum >> 16;
}
#else
static inline uint32_t mix_apart(fb_data pixel)
{
    return FB_UNPACK_SCALAR_LCD(pixel);
}

static fb_data mix31(unsigned a, unsigned b)
{
    return FB_SCALARPACK_LCD(LCD_RGBPACK_LCD(
        (3 * RGB_UNPACK_RED_LCD(a) + RGB_UNPACK_RED_LCD(b) + 2) >> 2,
        (3 * RGB_UNPACK_GREEN_LCD(a) + RGB_UNPACK_GREEN_LCD(b) + 2) >> 2,
        (3 * RGB_UNPACK_BLUE_LCD(a) + RGB_UNPACK_BLUE_LCD(b) + 2) >> 2));
}
#endif

static bool make_phases(int width, int height, unsigned wanted)
{
    for (int y = 0; y < height; y++)
    {
        const fb_data *row = sharp.pixels + y * width;
        fb_data *quarter = sharp.phase[0] + y * width;
        fb_data *three = sharp.phase[1] + y * width;
        uint32_t a = mix_apart(row[0]);
        for (int x = 0; x < width - 1; x++)
        {
            const uint32_t b = mix_apart(row[x + 1]);
#ifdef MIX_APART
            quarter[x] = mix_pixel(3 * a + b);
            three[x] = mix_pixel(a + 3 * b);
#else
            quarter[x] = mix31(a, b);
            three[x] = mix31(b, a);
#endif
            a = b;
        }
        quarter[width - 1] = three[width - 1] = row[width - 1];
        if ((y & 15) == 15 && !reader_yield(wanted))
            return false;
    }
    return true;
}

/* The picture's rows shrunk into the block as the decoder makes them, at
   the size its IDCT gives: the cover's or more, under twice. Each pixel of
   the cover is the mean of the picture's it covers, by their parts of 256,
   as resize.c's scaler has it, but of colours already in RGB, as a picture
   decoded straight to the size: the scaler takes the mean in YUV. Its
   tables and rows are at the block's end, where the scaler's rows would
   be. */
struct sink_tap
{
    uint16_t at[3];     /* the picture's pixels one of the cover's covers */
    uint16_t part[3];   /* and each one's part of it, of 256 */
};
struct sink
{
    struct sink_tap column[ART_MENU_SIZE], row[ART_MENU_SIZE];
    uint8_t line[ART_MENU_SIZE][3];     /* a row of the picture across */
    uint16_t sum[2][ART_MENU_SIZE][3];  /* the cover's row in the making,
                                           and the next */
    int width, height;                  /* the cover's */
    int next;                           /* its row to come */
};

static struct sink *sink_at(void)
{
    return (struct sink *)((unsigned char *)sharp.pixels + SHARP_BYTES
                           - sizeof(struct sink));
}

/* one way: from of the picture's pixels to to of the cover's. The pixels
   past the last one covered are that one again, of no part. */
static void sink_taps(struct sink_tap *tap, int from, int to)
{
    for (int i = 0; i < to; i++, tap++)
    {
        /* in to-ths of a pixel of the picture */
        const long start = (long)i * from, end = start + from;
        const int first = start / to, last = (end - 1) / to;
        int left = 256, most = 0;
        for (int k = 0; k < 3; k++)
        {
            const long a = MAX(start, (long)(first + k) * to);
            const long b = MIN(end, (long)(first + k + 1) * to);
            tap->at[k] = MIN(first + k, last);
            tap->part[k] = b > a ? (b - a) * 256 / from : 0;
            left -= tap->part[k];
            if (tap->part[k] > tap->part[most])
                most = k;
        }
        tap->part[most] += left;
    }
}

static void sink_row(uint32_t y, void *row_in, struct scaler_context *ctx)
{
    struct sink *sink = sink_at();
    const struct uint8_rgb *in = row_in;
    const int width = sink->width;
    for (int x = 0; x < width; x++)
    {
        const struct sink_tap *tap = &sink->column[x];
        const struct uint8_rgb *a = in + tap->at[0], *b = in + tap->at[1],
                               *c = in + tap->at[2];
        const unsigned pa = tap->part[0], pb = tap->part[1], pc = tap->part[2];
        sink->line[x][0] = (a->red * pa + b->red * pb + c->red * pc + 128) >> 8;
        sink->line[x][1] = (a->green * pa + b->green * pb + c->green * pc
                            + 128) >> 8;
        sink->line[x][2] = (a->blue * pa + b->blue * pb + c->blue * pc + 128) >> 8;
    }
    /* into the cover's rows it is part of, two at most; the one it ends
       goes into the block, dithered as bmp.c's output_row_8_native() */
    for (int j = sink->next; j < sink->height && sink->row[j].at[0] <= y; j++)
    {
        const struct sink_tap *tap = &sink->row[j];
        const unsigned part = tap->part[y - tap->at[0]];
        uint16_t (*sum)[3] = sink->sum[j & 1];
        for (int x = 0; x < width; x++)
        {
            sum[x][0] += part * sink->line[x][0];
            sum[x][1] += part * sink->line[x][1];
            sum[x][2] += part * sink->line[x][2];
        }
        if (y != tap->at[2])
            continue;
        fb_data *to = sharp.pixels + j * width;
        const int dy = DITHERY(j);
        for (int x = 0; x < width; x++)
        {
            const int delta = ctx->dither ? DITHERXDY(x, dy) : 127;
            unsigned r = (sum[x][0] + 128) >> 8, g = (sum[x][1] + 128) >> 8,
                     b = (sum[x][2] + 128) >> 8;
            r = (31 * r + (r >> 3) + delta) >> 8;
            g = (63 * g + (g >> 2) + delta) >> 8;
            b = (31 * b + (b >> 3) + delta) >> 8;
            to[x] = FB_RGBPACK_LCD(r, g, b);
        }
        memset(sum, 0, sizeof(sink->sum[0]));
        sink->next = j + 1;
    }
}

/* where the decoder's room begins: past all of the block's pixels */
static unsigned int sink_size(struct bitmap *bm)
{
    (void)bm;
    return sizeof(menu_art.pixels);
}

/* a side as the decoder's IDCT makes it: an eighth, a quarter, a half or
   all of it, the smallest not under to (jpeg_load.c's calc_scale()) */
static int idct_side(int side, int to)
{
    int shift = 0;
    while (shift < 3 && (side << shift) < 8 * to)
        shift++;
    return (side << shift) >> 3;
}

/* The sink for a picture of that size decoded to bm's, which becomes the
   IDCT's; NULL, and bm as it was, if that is the cover's own size, which
   needs none, or twice it and over, as of a picture 3840 pixels a side:
   the scaler's then */
static const struct custom_format *sink_for(struct bitmap *bm, int own_width,
                                            int own_height)
{
    static const struct custom_format format =
        { .output_row_8 = sink_row, .get_size = sink_size };
    const int width = bm->width, height = bm->height;
    const int from_width = idct_side(own_width, width);
    const int from_height = idct_side(own_height, height);
    if ((from_width == width && from_height == height)
        || from_width >= 2 * width || from_height >= 2 * height)
        return NULL;
    struct sink *sink = sink_at();
    sink_taps(sink->column, from_width, width);
    sink_taps(sink->row, from_height, height);
    memset(sink->sum, 0, sizeof(sink->sum));
    sink->width = width;
    sink->height = height;
    sink->next = 0;
    bm->width = from_width;
    bm->height = from_height;
    return &format;
}

/* 1 decoded into the block, to the job's size; 0 not looked at or stopped;
   -1 none that would be sharper, or of the cover's proportions, or that
   decodes, as a progressive one, or a read that failed */
static int sharp_decode(int fd, int width, int height)
{
    struct bitmap bm;
    memset(&bm, 0, sizeof(bm));
    bm.data = (unsigned char *)sharp.pixels;
    const int format = FORMAT_NATIVE | FORMAT_DITHER;
    /* read whole where there's room; else its own size first, which is
       all that pass reads */
    const int whole = picture_whole(fd, width, height);
    if (whole < 0)
        return jpeg_stop_fd == fd ? 0 : -1;
    if (!whole && lseek(fd, sharp.aa.pos, SEEK_SET) < 0)
        return 0;
    if (clip_jpeg_fd(fd, sharp.aa.type, sharp.aa.size, &bm, SHARP_BYTES,
                     format | FORMAT_RETURN_SIZE, NULL) <= 0)
        return -1;
    const long across = (long)bm.width * height, down = (long)bm.height * width;
    if (bm.width < width || bm.height < height
        || (across > down ? across - down : down - across) * 100 > across * 3)
        return -1;
    if (!whole && lseek(fd, sharp.aa.pos, SEEK_SET) < 0)
        return 0;
    bm.width = width;
    bm.height = height;
#if DISK_SPINS && defined(HAVE_ATA_DMA)
    /* as list_covers.c */
    ata_dma_yield_always(true);
#endif
    /* the picture's own size again, of across and down */
    const struct custom_format *sink = sink_for(&bm, across / height,
                                                down / width);
    const int rc = clip_jpeg_fd(fd, sharp.aa.type, sharp.aa.size, &bm,
                                SHARP_BYTES - (sink ? sizeof(struct sink) : 0),
                                format | FORMAT_RESIZE, sink);
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

/* The cover just decoded, which the block holds and the menus may show,
   into the kept file, on the reader's thread: as a job, so that the block
   is neither written nor let go under it; a part at a time, the music's
   hold and a cancel looked at between. The slot's own sector is in the last
   part, and the table follows it: cut short, the slot is no one's. */
static void kept_store(unsigned wanted)
{
    if (wanted != sharp.wanted)
        return;
    sharp.reading = true;
    kept_load();
    /* since the ask: a drop meanwhile, and this cover is of the old ones */
    const unsigned forgets = sharp.job_forgets;
    /* past the pixels the decoder's room is free: the sector, and a new
       file's table, are made there */
    unsigned char *block = (unsigned char *)sharp.pixels;
    unsigned char *room = block + sizeof(menu_art.pixels);
    const int fd = open(KEPT_FILE, O_RDWR | O_CREAT, 0666);
    const off_t size = fd >= 0 ? ffilesize(fd) : 0;
    int used = size > KEPT_TABLE ? (size - KEPT_TABLE) / KEPT_SLOT : 0;
    bool ok = fd >= 0 && kept_loaded && forgets == kept_forgets;
    kept.identity = itunesdb_covers_identity();
    if (ok && size < KEPT_TABLE)
    {
        memset(kept.slot, 0, sizeof(kept.slot));
        memset(room, 0, KEPT_TABLE);
        memcpy(room, &kept, sizeof(kept));
        ok = lseek(fd, 0, SEEK_SET) == 0
             && write(fd, room, KEPT_TABLE) == KEPT_TABLE;
        used = 0;
    }
    /* the next of the file, or the oldest's place */
    int slot = MIN(used, KEPT_SLOTS - 1);
    if (used >= KEPT_SLOTS)
    {
        for (int i = 0; i < KEPT_SLOTS; i++)
        {
            if (kept.slot[i].serial < kept.slot[slot].serial)
                slot = i;
        }
    }
    /* its old picture is no more, before any of it is written over */
    if (ok && kept.slot[slot].serial)
    {
        kept.slot[slot].serial = 0;
        ok = lseek(fd, 0, SEEK_SET) == 0
             && write(fd, &kept, sizeof(kept)) == sizeof(kept);
    }
    const struct kept_entry entry = {
        sharp.key.offset, kept_path(sharp.key.path), kept.serial + 1,
        sharp.width, sharp.height
    };
    memset(room, 0, KEPT_HEAD);
    memcpy(room, &entry, sizeof(entry));
    kept_writing = slot;
    ok = ok && lseek(fd, kept_place(slot), SEEK_SET) == kept_place(slot);
    for (off_t at = 0; ok && at < KEPT_SLOT; at += KEPT_WRITE)
    {
        const ssize_t part = MIN(KEPT_WRITE, KEPT_SLOT - at);
        yield();
        while (sharp.hold && wanted == sharp.wanted)
            sleep(1);
        ok = wanted == sharp.wanted && forgets == kept_forgets
             && write(fd, block + at, part) == part;
    }
    if (ok)
    {
        kept.serial = entry.serial;
        kept.slot[slot] = entry;
        ok = lseek(fd, 0, SEEK_SET) == 0
             && write(fd, &kept, sizeof(kept)) == sizeof(kept);
        if (!ok)
            kept.slot[slot].serial = 0;
    }
    if (fd >= 0)
        close(fd);
    kept_writing = -1;
    reader_turn(false);
    sharp.reading = sharp.hold = false;
    DEBUGF("menu cover: %s in slot %d\n", ok ? "kept" : "not kept", slot);
}

void menu_cover_read(void)
{
    const unsigned wanted = sharp.wanted;
    sharp.to_read = false;
    sharp.reader = thread_self();
    if (sharp.job_phases)
    {
        /* the cover is held, and may be drawn: only read here; not held any
           more, or the frames' block let go: nothing to make */
        if (!sharp.held || !sharp.phase[0])
            return;
        sharp.reading = true;
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
        cpu_boost(true);
#endif
        const bool made = make_phases(sharp.width, sharp.height, wanted);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
        cpu_boost(false);
#endif
        reader_turn(false);
        sharp.reading = sharp.hold = false;
        if (made && wanted == sharp.wanted && sharp.held && sharp.phase[0])
            sharp.phased = true;
        return;
    }
    const int width = sharp.job_width, height = sharp.job_height;
    /* never into the block while it's drawn from, nor into one let go */
    if (sharp.shown || !sharp.pixels)
        return;
    sharp.fd = -1;
    /* the block is let go only once this is false */
    sharp.reading = true;
    /* Finder's frame of this size, or the cover kept: no decode */
    /* a disk that has stopped since the ask isn't woken */
    const bool disk = disk_free();
    const int fetched = disk && wanted == sharp.wanted
        ? cover_fetch(sharp.file, &sharp.job_key, width, height, sharp.pixels)
        : 0;
    if (!fetched && sharp.job_fetch)
    {
        /* asked for a read alone, before the music's buffer would have a
           decode: asked again by the decode's rules, or in a while if the
           disk wasn't to be read */
        if (wanted == sharp.wanted && disk)
        {
            sharp.unfetched = true;
            sharp.unfetched_key = sharp.job_key;
        }
        else if (wanted == sharp.wanted)
            sharp.back_off = current_tick + SHARP_AGAIN;
        reader_turn(false);
        sharp.reading = sharp.hold = false;
        return;
    }
    const int fd = !fetched && disk ? open(sharp.file, O_RDONLY) : -1;
    int rc = fetched ? 1 : 0;
    bool phased = false;
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
            if (jpeg_hold_fd == fd)
                jpeg_hold_fd = -1;
        }
        jpeg_mem_fd = -1;
        close(fd);
    }
    /* a fetched one is the cover as it is, held at once: its moving frames
       are a job of their own, when the music has its floor */
    if (rc > 0 && wanted == sharp.wanted && !fetched)
    {
        rc = measure_parts(width, height, wanted);
        if (rc > 0 && !same_picture())
            rc = -1;
        if (rc > 0 && !match_colours(width, height, wanted))
            rc = 0;
        /* its moving frames with it, so that it comes moving as it stays */
        if (rc > 0 && sharp.phase[0])
        {
            if (make_phases(width, height, wanted))
                phased = true;
            else
                rc = 0;
        }
    }
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    cpu_boost(false);
#endif
    reader_turn(false);
    sharp.reading = sharp.hold = false;
    if (wanted != sharp.wanted)
        return;
    if (rc <= 0)
    {
        /* not opened, or the disk not free: again in a while; no picture
           that does, or no frame of Finder's: not for this cover */
        if (!rc)
            sharp.back_off = current_tick + SHARP_AGAIN;
        else
        {
            sharp.failed = true;
            sharp.failed_key = sharp.job_key;
        }
        return;
    }
    sharp.key = sharp.job_key;
    sharp.width = width;
    sharp.height = height;
    sharp.box_width = sharp.job_box_width;
    sharp.box_height = sharp.job_box_height;
    sharp.held = true;
    sharp.phased = phased;
    /* in Finder's place at once, if the menus have that for it */
    if (menu_art.width == width && menu_art.height == height
        && menu_art.handle < 0
        && same_key(&sharp.key, menu_art.offset, menu_art.path))
        sharp.shown = sharp.came = true;
    DEBUGF("menu cover: %s, by the reader\n",
           fetched == 1 ? "Finder's frame" : fetched ? "kept" : "decoded");
    if (!fetched)
        kept_store(wanted);
}

/* The block as Album Art has it: let go with it off; taken with it on, only
   without music, which would stop for it */
static void sharp_memory(bool wanted)
{
    if (!wanted)
    {
        sharp.refused = false;
        if (!sharp.handle)
            return;
        remove_event(PLAYBACK_EVENT_TRACK_CHANGE, track_begun);
        /* Finder's scaled again */
        if (sharp.shown)
            menu_art.width = 0;
        sharp_undraw();
        sharp.now = false;
        /* nothing asked or running as this ends: without the back-off the
           look could ask again as the reader's done */
        for (;;)
        {
            menu_cover_read_cancel();
            if (!sharp.reading)
                break;
            sleep(1);
        }
        sharp.held = sharp.phased = sharp.gave_way = false;
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
        add_event(PLAYBACK_EVENT_TRACK_CHANGE, track_begun);
        /* without music, for the look */
        kept_load();
    }
    else
        sharp.refused = true;
}

/* The moving frames' block as Moving Cover has it, with Album Art's: taken
   as the cover's, only without music; let go with either off */
static void phases_memory(bool wanted)
{
    if (!wanted || !sharp.handle)
    {
        sharp.phase_refused = false;
        if (!sharp.phase_handle)
            return;
        sharp.phased = false;
        /* as the cover's block: nothing asked or running as this ends */
        for (;;)
        {
            menu_cover_read_cancel();
            if (!sharp.reading)
                break;
            sleep(1);
        }
        sharp.phased = false;
        sharp.phase[0] = sharp.phase[1] = NULL;
        sharp.phase_handle = core_free(sharp.phase_handle);
        /* a decode given up for this is asked again at once, the cover
           alone */
        sharp.back_off = current_tick;
        sharp.gave_way = false;
        return;
    }
    if (audio_status())
        sharp.phase_refused = false;
    if (sharp.phase_handle || sharp.phase_refused || audio_status())
        return;
    const int handle = core_alloc_ex(2 * sizeof(menu_art.pixels),
                                     &buflib_ops_locked);
    if (handle > 0)
    {
        sharp.phase_handle = handle;
        sharp.phase[0] = core_get_data(handle);
        sharp.phase[1] = sharp.phase[0] + ART_MENU_SIZE * ART_MENU_SIZE;
    }
    else
        sharp.phase_refused = true;
}

/* The scroll thread's hook: for the steps, and with the block alone for the
   look and a still cover's one frame. Without either nothing runs. */
static void hook_set(bool moving)
{
    scroll_set_tick_hook(moving || sharp.handle ? pan_tick : NULL);
}

void menu_cover_memory(void)
{
    const bool art = global_settings.album_art != AA_OFF;
    phases_memory(art && global_settings.moving_cover);
    sharp_memory(art);
    phases_memory(art && global_settings.moving_cover);
    hook_set(global_settings.moving_cover);
}

/* the sharp one came and isn't on the screen yet */
static bool sharp_came(void)
{
    return sharp.came;
}

/* the cover's drawn from the one shown; all of the theme with it, and the
   reader may be asked at once */
static void sharp_drawn(bool theme)
{
    sharp.came = false;
    if (theme)
    {
        sharp.back_off = current_tick;
        sharp.gave_way = false;
    }
}
#else
static inline void sharp_look(bool shows) { (void)shows; }
static inline void sharp_memory(bool wanted) { (void)wanted; }
static inline void phases_memory(bool wanted) { (void)wanted; }
static inline void hook_set(bool moving)
{
    scroll_set_tick_hook(moving ? pan_tick : NULL);
}
static inline bool sharp_came(void) { return false; }
static inline void sharp_drawn(bool theme) { (void)theme; }
#endif /* MENU_COVER_SHARP */

/* On the scroll thread: a step when it's due; the ticks until the next look */
static long pan_tick(void)
{
    cop_settle();
    const bool can_go = pan_can_go();
    const bool still = pan_still && cover_idle();
    sharp_look(can_go || still);
    if (!can_go)
    {
        pan_boost(false);
        pan_going = false;
        /* still, it only turns sharp: one frame where it is, as a step's */
        if (still && sharp_came())
        {
            pan_draw(2 * pan.p0, current_tick, true);
            sharp_drawn(false);
        }
        return PAN_POLL;
    }
    const long now = current_tick;
    if (pan_going)
        pan_clock += MIN(now - pan_last, PAN_POLL);
    pan_last = now;
    pan_going = true;
    long until;
    int h = pan_at(pan_clock, &until);
    /* A sharp cover without its frames goes by whole columns, to the last
       one passed: half way it would be the mean of two, soft beside the
       crisp one, and one after the other trembles */
    const bool whole = sharp_whole();
    if (whole && (h & 1) && h != pan_h)
        h += h > pan_h ? -1 : 1;
    /* not through a rest but its last PAN_POLL */
    pan_boost(h != pan_h || until <= PAN_POLL);
    /* or the sharp one drawn where it is, unless a step is about to, or it
       is half way where Finder's was */
    if (h != pan_h
        || (sharp_came() && until > PAN_LATENCY && !(whole && (h & 1))))
    {
        if (!cop_free())
            return 1;
        /* shown PAN_LATENCY after it was due by the clock, not as the thread
           woke or the frame was ready: the steps go evenly */
        const bool on_time = !TIME_AFTER(pan_due, now)
                             && !TIME_AFTER(now, pan_due + PAN_LATENCY);
        pan_draw(h, (on_time ? pan_due : now) + PAN_LATENCY, false);
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
    sharp_undraw();
    memcpy(menu_art.pixels, bm->data, bm->width * bm->height * sizeof(fb_data));
    menu_art.width = bm->width;
    menu_art.height = bm->height;
    menu_art.handle = handle;
    return true;
}

int menu_cover_place(const struct viewport *vp, int x, int y, int width,
                     int height)
{
    pan_shown = pan_still = false;
    sharp_drawn(true);
    const int left = vp->x + x, top = vp->y + y;
    int x0 = MAX(vp->x, left);
    const int x1 = MIN(MIN(vp->x + vp->width, left + width), LCD_WIDTH);
    const int y0 = MAX(MAX(vp->y, top), 0);
    const int y1 = MIN(MIN(vp->y + vp->height, top + height), LCD_HEIGHT);
    /* right of the list, which is drawn over the cover's left part */
    const struct viewport *list = sb_skin_peek_info_vp(SCREEN_MAIN);
    if (list && list->y < y1 && list->y + list->height > y0
        && list->x < x1 && list->x + list->width > x0)
    {
        if (list->x > x0)
            return 0;
        x0 = list->x + list->width;
        /* or from where the theme's pane begins, right of a list that ends
           short of it and of a line between them */
        x0 = MAX(x0, ui_metric(UI_METRIC_PANE_LEFT));
    }
    const int travel = width - (x1 - x0);
    const int p0 = x0 - left;
    if (x1 <= x0 || y1 <= y0)
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
    if (!global_settings.moving_cover || travel <= 0 || p0 > travel)
    {
        pan_still = true;
        return 0;
    }
    long until;
    /* the whole column; a half step is drawn by the next tick */
    pan_h = pan_at(pan_clock, &until) & ~1;
    pan_shown = true;
    return p0 - pan_h / 2;
}

const fb_data *menu_cover_placed_pixels(const fb_data *pixels)
{
    /* at an even half column, where it's placed */
    const fb_data *frame = pan_shown ? moving_pixels(0) : NULL;
    return frame ? frame : pixels;
}

void menu_cover_stop(void)
{
    pan_shown = pan_still = false;
    pan_boost(false);
    cop_end(false);
}

void menu_cover_wait(void)
{
    cop_wait();
}

void menu_cover_end(void)
{
    pan_shown = pan_still = false;
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
    const bool art = global_settings.album_art != AA_OFF;
    phases_memory(art && on);
    sharp_memory(art);
    phases_memory(art && on);
    /* turned on, it starts from the middle, where it's drawn while off */
    pan_clock = 0;
    pan_going = false;
    hook_set(on);
    skin_request_full_update(CUSTOM_STATUSBAR);
}

#endif /* HAVE_MENU_COVER */
