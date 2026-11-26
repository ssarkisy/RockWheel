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
#include "tagcache.h"
#include "playlist.h"
#include "dircache.h"
#if CONFIG_TUNER
#include "radio.h"
#endif
#include "transition.h"
#include "misc.h"
#include "menu.h"
#include "viewport.h"
#include "statusbar-skinned.h"
#include "wps_internals.h"
#include "skin_engine.h"
#include "menu_cover.h"
#include "skin_display.h"
#include "rbpaths.h"
#include "debug.h"

#ifdef HAVE_MENU_ART

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
/* Sharp: the picture at the panel's own size, Finder's large frame of it (the
   Video's 1024, the photo's 1013) or the one Updating Library made, read by
   the album cover reader's thread, takes the place of the 200 px frame scaled
   up, moving or still. From flash as its track starts, to be there when the
   menus show; from a disk that spins only as the cover shows. Nothing is
   decoded: a song without one shows the frame scaled. In a block taken
   without music and then held (sharp_memory()). The UI's thread, the
   scroll's, the audio's and the reader's are on one core: none of their
   changes here has a yield in the middle. */
#define SHARP_BYTES sizeof(menu_art.pixels)
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
    /* the song playing, by its track's event: its cover, and the frame
       that is */
    bool now;
    struct cover_key now_key;
    short now_frame_width, now_frame_height;
    char now_file[MAX_PATH];
    bool now_large;         /* it may have a picture at the panel's size */
    /* the picture asked of the reader, which these aren't changed under */
    struct cover_key job_key;
    short job_frame_width, job_frame_height;
    short job_width, job_height, job_box_width, job_box_height;
    char file[MAX_PATH];
    bool unfetched;         /* unfetched_key's has none to be read */
    struct cover_key unfetched_key;
    long back_off;          /* not asked again before this tick */
    bool gave_way;          /* a read was given up for the reader's other
                               work, and none asked since */
    /* the reader's: numbered, as a cancel leaves the one being read
       unshown */
    volatile unsigned wanted;
    volatile bool to_read, reading;
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

/* the read asked for or going on is given up */
static void job_cancel(void)
{
    sharp.to_read = false;
    sharp.wanted++;
}

/* For the reader's other work, which comes first: not asked for 2 s after
   each ask of that, as after an ask of this that came to nothing; and once
   a read was given up for it, not until none has come for 10 s or the
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

/* the settings applied, as after USB: a picture that wasn't there may be */
static void sharp_retry(void)
{
    sharp.unfetched = false;
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

/* The cover at the panel's size, width by height, of the frame at art read
   into to: Finder's large frame, or the one Updating Library made. Reads the
   disk. */
static bool cover_fetch(const char *file, const struct cover_key *key,
                        int frame_width, int frame_height, int width,
                        int height, fb_data *to)
{
    struct itunesdb_art art;
    struct itunesdb_large_art large;
    memset(&art, 0, sizeof(art));
    strcpy(art.path, key->path);
    art.offset = key->offset;
    art.width = frame_width;
    art.height = frame_height;
    return itunesdb_find_large_of(file, &art, &large) && large.width == width
           && large.height == height && itunesdb_load_large_art(&large, to) > 0;
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
#endif /* MENU_COVER_SHARP */

/* Without music the menus show the cover of the song Resume Playback would
   go on with, still, until another plays. Looked for as the menus are first
   drawn after a boot, USB, a stop or another resume point: the one the
   menus or the block have if it's that song's, else read from a disk that
   spins. What a look came to stands for the resume point it was for, so a
   draw compares a few numbers; one that only a stopped disk left without
   is made again once the disk spins. */
static struct {
    bool looked;        /* has is the answer for index and crc */
    bool has;           /* menu_art's, or the block's, is its cover */
    bool waits;         /* none only for the disk, which had stopped */
    int reason;         /* none: why its picture isn't shown (itunesdb.h) */
    int index;
    uint32_t crc;
} resume;

/* as disk_free(): without the block's code nothing here spares the disk */
static bool resume_disk_free(void)
{
#ifdef MENU_COVER_SHARP
    return disk_free();
#else
    return true;
#endif
}

void menu_cover_resume_look(void)
{
    resume.looked = false;
}

/* the answer stands: nothing to find or to read at this draw */
static bool resume_looked(void)
{
    return resume.looked && resume.index == global_status.resume_index
        && resume.crc == global_status.resume_crc32
        /* the cover let go since, as with Album Art off: looked for again */
        && (!resume.has || menu_art.width > 0)
        && !(resume.waits && resume_disk_free());
}

/* The cover of the song to resume, by the file the status keeps: none for a
   file that isn't the resume point's or is gone. On the iPod the file is
   looked up in the dircache, and its cover in the covers' indexes and the
   database in memory; the simulator reads the disk for the file and the
   tags. Where the dircache isn't ready the disk would be read for the
   file: not one that has stopped. */
static bool resume_find(const char **file, struct itunesdb_art *art)
{
    resume.looked = true;
    resume.has = resume.waits = false;
    resume.reason = 0;
    resume.index = global_status.resume_index;
    resume.crc = global_status.resume_crc32;
    *file = playlist_resume_file();
    if (!*file)
        return false;
#ifdef HAVE_DIRCACHE
    struct dircache_info info;
    dircache_get_info(&info);
    const bool cached = info.status == DIRCACHE_READY;
#else
    const bool cached = false;
#endif
    if (!cached && !resume_disk_free())
    {
        resume.waits = true;
        return false;
    }
    if (!file_exists(*file))
        return false;
    /* Finder's, else the one made of the song's own picture, else its
       album's: as it would play, but for a picture not shown */
    if (itunesdb_find_track_art(NULL, *file, art))
        return true;
    resume.reason = art->reason;
#ifdef HAVE_TAGCACHE
    if (!art->reason)
        return tagcache_find_album_art(*file, art);
#endif
    return false;
}

void menu_cover_forget(void)
{
    menu_art.width = 0;
    resume.looked = false;
    sharp_undraw();
    job_cancel();
#ifdef MENU_COVER_SHARP
    /* a cover's key may be another's now: the next track's is taken anew */
    sharp.held = sharp.phased = sharp.now = false;
    sharp.unfetched = false;
#endif
}

int menu_cover_reason(void)
{
    if (audio_status() & AUDIO_STATUS_PLAY)
        return playback_current_cover_reason();
    return resume.looked && resume.index == global_status.resume_index
           && resume.crc == global_status.resume_crc32 ? resume.reason : 0;
}

bool menu_cover_ready(const struct wps_data *data)
{
    struct skin_albumart *aa = SKINOFFSETTOPTR(get_skin_buffer((struct wps_data *)data),
                                               data->albumart);
    struct mp3entry *id3 = audio_current_track();
    struct itunesdb_art art;
    const bool panel = aa
        && data == skin_get_gwps(CUSTOM_STATUSBAR, SCREEN_MAIN)->data;
    if (panel)
        sharp_panel(MIN(aa->width, ART_MENU_SIZE),
                    MIN(aa->height, ART_MENU_SIZE));
    if (!aa || global_settings.album_art == AA_OFF)
        return false;
    const bool playing = id3 && (audio_status() & AUDIO_STATUS_PLAY);
    const char *file;
    if (playing)
    {
        /* what the menus have from here may be another song's */
        resume.looked = false;
        if (!itunesdb_find_track_art(id3, id3->path, &art))
            return false;
        sharp_plays();
        file = id3->path;
    }
    else
    {
        /* the song to resume: beside the menus only, with nothing else to
           hear, as the radio */
        if (!panel || audio_status()
#if CONFIG_TUNER
            || get_radio_status() != FMRADIO_OFF
#endif
            || global_status.resume_index == -1)
            return false;
        if (resume_looked())
            return resume.has;
        if (!resume_find(&file, &art))
            return false;
    }
    if (menu_art.width > 0 && menu_art.handle < 0 && menu_art.offset == art.offset
        && !strcmp(menu_art.path, art.path))
    {
        resume.has = !playing;
        return true;
    }

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
        resume.has = !playing;
        return true;
    }
#ifdef MENU_COVER_SHARP
    /* the picture of this size, there from the first frame */
    int width = box_width, height = box_height;
    struct cover_key key;
    cover_fit(art.width, art.height, &width, &height);
    set_key(&key, art.offset, art.path);
    /* not from a disk that has stopped or is about to: the frame scaled
       then, and the look asks the reader by its rule */
    const bool fetched = disk_free()
        && cover_fetch(file, &key, art.width, art.height, width, height,
                       menu_art.pixels);
    if (fetched)
    {
        menu_art.width = width;
        menu_art.height = height;
        menu_art.offset = art.offset;
        strcpy(menu_art.path, art.path);
        menu_art.handle = -1;
        menu_art.crisp = true;
        sharp_take(&key, width, height, box_width, box_height);
        resume.has = !playing;
        return true;
    }
    /* a disk that has stopped isn't started for the song to resume: none
       until it spins */
    if (!playing && !disk_free())
    {
        resume.waits = true;
        return false;
    }
#endif
    /* Finder's scaled: from the cover in the buffer if it's as large, as a
       disk that spins is often off by the time the menus open, and turning
       it on takes 0.6 s */
    const struct bitmap *buffered = playing ? playback_current_largest_art()
                                            : NULL;
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
    resume.has = !playing;
    return true;
}

const fb_data *menu_cover_pixels(int *width, int *height)
{
    *width = menu_art.width;
    *height = menu_art.height;
    return art_pixels();
}
#endif /* HAVE_MENU_ART */

#ifdef HAVE_MENU_COVER

/* The setting, where the cover can move (HAVE_MOVING_COVER); else it never
   does, whatever a .cfg says */
#ifdef HAVE_MOVING_COVER
#define MOVING(on) (on)
#else
#define MOVING(on) ((void)(on), false)
#endif
/* the cover beside the menus is drawn: Album Art on, and on the iPod photo
   its pane (Cover Beside Menu) */
#ifdef UI_MAIN_MENU_PANE
#define ART_SHOWN() (global_settings.album_art != AA_OFF \
                     && global_settings.menu_pane)
#else
#define ART_SHOWN() (global_settings.album_art != AA_OFF)
#endif

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
/* the cover of the song to resume moves this long after the last press */
#define PAN_RESUME_REST (60 * HZ)

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
static long pan_last;           /* the tick of the last look */
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

/* the cover on the screen is one that moves: the playing song's, or with
   nothing playing the cover of the song to resume, on a lit screen */
static bool cover_live(void)
{
    return menu_art.width == pan.width && menu_art.height == pan.height
        && ((audio_status() & AUDIO_STATUS_PLAY)
            /* for PAN_RESUME_REST after the last press: it would keep the
               CPU at work for as long as the screen is lit, with nothing
               playing */
            || (resume.has && !audio_status()
                && !TIME_AFTER(current_tick, action_last_tick() + PAN_RESUME_REST)))
        && !transition_active() && !plugin_is_running()
#if defined(HAVE_LCD_ENABLE) || defined(HAVE_LCD_SLEEP)
        && lcd_active()
#endif
#ifdef HAVE_BACKLIGHT
        && is_backlight_on(true)
#endif
        ;
}

/* the UI waits, the cover on the screen as it was drawn: the sharp cover's
   frame may be drawn over it */
static bool cover_idle(void)
{
    return cover_live() && action_waiting() && button_queue_empty()
        && !TIME_BEFORE(current_tick, action_last_tick() + PAN_AT_REST);
}

/* its clock runs: through the wheel's turns too, whose frames then draw it
   where it has come to */
static bool pan_live(void)
{
    return pan_shown && MOVING(global_settings.moving_cover) && cover_live();
}

/* and a step may be drawn: the UI waits with nothing in its queue. Between
   the wheel's steps too: stopped through them it stood for as long as the
   wheel turned */
static bool pan_can_go(void)
{
    return pan_live() && action_waiting() && button_queue_empty();
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
    const bool made = !MOVING(global_settings.moving_cover) || pan_still
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

/* The list's shadow over the pane, as a stock iPod classic's
   (PreviewArea_Shadow_Image, measured in 2.0.5's screenshots): of the pane's
   first columns, each so much of 256 to black, at the theme's strength
   (UI_METRIC_PANE_SHADOW), those under 3 of 256 left out, as
   themes/tools/make_pane_shadow.py has them in the pictures under the pane */
static const unsigned char pane_shadow[] = {
    119, 107, 99, 89, 80, 75, 68, 62, 54, 47, 40, 34, 28, 23, 18, 13, 11, 8, 6, 4
};

int menu_cover_pane_shadow_width(void)
{
    const int strength = ui_metric(UI_METRIC_PANE_SHADOW);
    if (strength <= 0 || ui_metric(UI_METRIC_PANE_LEFT) <= 0)
        return 0;
    int i = 0;
    while (i < (int)ARRAYLEN(pane_shadow) && pane_shadow[i] * strength / 100 >= 3)
        i++;
    return i;
}

/* It is drawn last, over all the pane shows, as the screen goes to the
   display (lcd_update_filter): darkened in the framebuffer, whatever drew
   there. The pictures under the pane have it already, which the slides
   move with the screens: a pixel of the backdrop's is left as it is. The
   band's pixels as last darkened or left, so that each is darkened once
   however often it goes to the display: one that is so still is left; 0
   there, black, which darkening leaves as it is, for a pixel drawn anew
   (pane_shadow_anew()). Static, of the pane's whole height: 9600 bytes on
   the Video, 7040 on the iPod photo. */
#define SHADOW_COLS ARRAYLEN(pane_shadow)
static fb_data shadow_shown[LCD_HEIGHT][SHADOW_COLS];

/* The pane on the screen as the theme drew it, beside a list that leaves it
   (the main menu; on the Video Music's menu and Photos', which have it too),
   with nothing over it: no splash, no screen of its own (Now Playing, the
   radio's) */
static bool pane_shown(void)
{
    if (menu_theme_wide() || !viewportmanager_theme_shown(SCREEN_MAIN)
        || !skin_get_gwps(CUSTOM_STATUSBAR, SCREEN_MAIN)->data->wps_loaded)
        return false;
    switch (get_current_activity())
    {
        case ACTIVITY_WPS:
        case ACTIVITY_FM:
        case ACTIVITY_RECORDING:
            return false;
        default:
            return true;
    }
}

/* the rectangle's part of the band drawn anew, all of it: its pixels to be
   darkened, but for those of the backdrop's */
static void pane_shadow_anew(int x0, int y0, int x1, int y1)
{
    const int left = ui_metric(UI_METRIC_PANE_LEFT);
    const int i0 = MAX(x0 - left, 0), i1 = MIN(x1 - left, (int)SHADOW_COLS);
    for (int y = MAX(y0, 0); y < MIN(y1, LCD_HEIGHT) && i0 < i1; y++)
        memset(&shadow_shown[y][i0], 0, (i1 - i0) * sizeof(fb_data));
}

/* lcd_update_filter: the band's part of the rectangle, if it shows. Each
   column's share of 256 kept, for the theme's strength and pane */
static void pane_shadow_filter(int x, int y, int width, int height)
{
    static int strength = -1, at = -1, cols;
    static unsigned short keep[SHADOW_COLS];
    const int left = ui_metric(UI_METRIC_PANE_LEFT);
    if (left <= 0 || x >= left + (int)SHADOW_COLS || x + width <= left
        || y >= LCD_HEIGHT || y + height <= 0)
        return;
    const int now = ui_metric(UI_METRIC_PANE_SHADOW);
    if (now != strength || left != at)
    {
        strength = now;
        at = left;
        cols = MIN(menu_cover_pane_shadow_width(), LCD_WIDTH - left);
        for (int i = 0; i < cols; i++)
            keep[i] = 256 - pane_shadow[i] * strength / 100;
    }
    const int i0 = MAX(x - left, 0), i1 = MIN(x + width - left, cols);
    const int y0 = MAX(y, 0), y1 = MIN(y + height, LCD_HEIGHT);
    if (i0 >= i1 || !pane_shown())
        return;
    extern struct frame_buffer_t lcd_framebuffer_default;
    fb_data *const fb = lcd_framebuffer_default.fb_ptr;
    const fb_data *const backdrop = lcd_get_backdrop();
    for (int r = y0; r < y1; r++)
    {
        fb_data *p = fb + r * LCD_FBWIDTH + left;
        const fb_data *b = backdrop ? backdrop + r * LCD_FBWIDTH + left : NULL;
        fb_data *shown = shadow_shown[r];
        for (int i = i0; i < i1; i++)
        {
            fb_data c = p[i];
            if (c == shown[i])
                continue;
            /* the backdrop's has it already; else RGB565's channels, which
               the menus' covers have (ITUNESDB_HAVE_ART) */
            if (!b || c != b[i])
            {
                c = (((c >> 11) * keep[i] >> 8) << 11)
                    | ((((c >> 5) & 0x3f) * keep[i] >> 8) << 5)
                    | ((c & 0x1f) * keep[i] >> 8);
                p[i] = c;
            }
            shown[i] = c;
        }
    }
}

void menu_cover_pane_shadow_init(void)
{
    lcd_update_filter = pane_shadow_filter;
}

void menu_cover_shade(int x0, int y0, int y1)
{
    const int shade = ui_metric(UI_METRIC_PANE_SHADE);
    const int x = ui_metric(UI_METRIC_PANE_LEFT);
    if (shade <= 0 || x <= 0 || x + 2 > LCD_WIDTH || x0 > x + 1)
        return;
    struct viewport *vp = lcd_set_viewport_ex(NULL, 0);
    for (int y = MAX(y0, 0); y < MIN(y1, LCD_HEIGHT); y++)
    {
        /* its first column so much to black, the second half that; of
           those the cover is in */
        for (int i = MAX(x0 - x, 0); i < 2; i++)
        {
            fb_data *p = FBADDR(x + i, y);
            const unsigned c = FB_UNPACK_SCALAR_LCD(*p);
            const int keep = 100 - (i ? shade / 2 : shade);
            *p = FB_SCALARPACK_LCD(LCD_RGBPACK(RGB_UNPACK_RED(c) * keep / 100,
                                               RGB_UNPACK_GREEN(c) * keep / 100,
                                               RGB_UNPACK_BLUE(c) * keep / 100));
        }
    }
    lcd_set_viewport_ex(vp, 0);
}

void menu_cover_glass(void)
{
    if (pan_shown || pan_still)
        cover_glass(pan.x0, pan.y0, pan.x1 - pan.x0, pan.y1 - pan.y0, 0);
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
    menu_cover_shade(pan.x0, pan.y0, pan.y1);
    /* the glass stays as the cover moves under it: on every frame */
    menu_cover_glass();
    /* the list's shadow over all of the frame, which lcd_send_rect() sends
       past lcd_update_rect()'s filter */
    pane_shadow_anew(pan.x0, pan.y0, pan.x1, pan.y1);
    lcd_update_filter_rect(pan.x0, pan.y0, width, pan.y1 - pan.y0);
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
/* from a disk that spins: how long the menus rest before it's read, and
   what the disk has left then before it would stop */
#define SHARP_REST  (HZ / 2)
#define SHARP_SPINS (2 * HZ)

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

/* A track has begun, on the audio's thread: its cover, for the look */
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
    sharp.now_large = itunesdb_has_large_of(id3->path, &art);
    strmemccpy(sharp.now_file, id3->path, sizeof(sharp.now_file));
}

/* On the scroll thread, at each look, ten a second at least, whatever the
   screen shows; shows: the cover is on it, the UI idle. A disk that spins is
   kept on under a read. The picture of the song playing is asked of the
   reader if the block has another's or none, one at a time: a read for
   another cover is given up first. */
static void sharp_look(bool shows)
{
    if (!sharp.pixels)
        return;
    const bool playing = audio_status() & AUDIO_STATUS_PLAY;
    const bool busy = sharp.to_read || sharp.reading;
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
    /* one to be read, and not known to have none */
    if (!frames
        && (!sharp.now_large
            || (sharp.unfetched
                && same_key(&sharp.unfetched_key, key->offset, key->path))))
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
    int width = sharp.box_now_width, height = sharp.box_now_height;
    cover_fit(sharp.now_frame_width, sharp.now_frame_height, &width, &height);
    /* the block is this cover's from here: the one it had, if the menus
       still have it as theirs, is the frame again when they're next drawn */
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
    strcpy(sharp.file, sharp.now_file);
    sharp.to_read = true;
    if (!album_header_reader_start_beside())
    {
        sharp.to_read = false;
        sharp.back_off = current_tick + SHARP_AGAIN;
    }
}

/* The reader gives the CPU up. False if its job was cancelled meanwhile. */
static bool reader_yield(unsigned wanted)
{
    yield();
    return wanted == sharp.wanted;
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

bool menu_cover_to_read(void)
{
    return sharp.to_read;
}

void menu_cover_read(void)
{
    const unsigned wanted = sharp.wanted;
    sharp.to_read = false;
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
        sharp.reading = false;
        if (made && wanted == sharp.wanted && sharp.held && sharp.phase[0])
            sharp.phased = true;
        return;
    }
    const int width = sharp.job_width, height = sharp.job_height;
    /* never into the block while it's drawn from, nor into one let go */
    if (sharp.shown || !sharp.pixels)
        return;
    /* the block is let go only once this is false */
    sharp.reading = true;
    /* a disk that has stopped since the ask isn't woken */
    const bool disk = disk_free();
    bool fetched = disk && wanted == sharp.wanted
        && cover_fetch(sharp.file, &sharp.job_key, sharp.job_frame_width,
                       sharp.job_frame_height, width, height, sharp.pixels);
    /* its moving frames with it, so that it comes moving as it stays */
    bool phased = false;
    if (fetched && wanted == sharp.wanted && sharp.phase[0])
    {
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
        cpu_boost(true);
#endif
        phased = make_phases(width, height, wanted);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
        cpu_boost(false);
#endif
    }
    sharp.reading = false;
    if (wanted != sharp.wanted)
        return;
    if (!fetched)
    {
        /* none to be read: not for this cover; the disk not free: again in
           a while */
        if (disk)
        {
            sharp.unfetched = true;
            sharp.unfetched_key = sharp.job_key;
        }
        else
            sharp.back_off = current_tick + SHARP_AGAIN;
        return;
    }
    sharp.key = sharp.job_key;
    sharp.width = width;
    sharp.height = height;
    sharp.box_width = sharp.job_box_width;
    sharp.box_height = sharp.job_box_height;
    sharp.held = true;
    sharp.phased = phased;
    /* in the frame's place at once, if the menus have that for it */
    if (menu_art.width == width && menu_art.height == height
        && menu_art.handle < 0
        && same_key(&sharp.key, menu_art.offset, menu_art.path))
        sharp.shown = sharp.came = true;
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
    const bool art = ART_SHOWN();
    phases_memory(art && MOVING(global_settings.moving_cover));
    sharp_memory(art);
    phases_memory(art && MOVING(global_settings.moving_cover));
    hook_set(MOVING(global_settings.moving_cover));
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
    const bool live = pan_live();
    const long now = current_tick;
    if (live && pan_going)
        pan_clock += MIN(now - pan_last, PAN_POLL);
    pan_last = now;
    pan_going = live;
    if (!can_go)
    {
        pan_boost(false);
        /* still, it only turns sharp: one frame where it is, as a step's */
        if (still && sharp_came())
        {
            pan_draw(2 * pan.p0, current_tick, true);
            sharp_drawn(false);
        }
        /* the UI is busy: looked at again soon, for a step between its frames */
        return live ? 2 : PAN_POLL;
    }
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

void menu_cover_frame_step(void)
{
    /* while the UI waits the scroll thread's steps go, evenly */
    if (!pan_live() || action_waiting())
        return;
    cop_settle();
    long until;
    int h = pan_at(pan_clock, &until);
    if (sharp_whole() && (h & 1) && h != pan_h)
        h += h > pan_h ? -1 : 1;
    if (h == pan_h || !cop_free())
        return;
    pan_draw(h, current_tick + PAN_LATENCY, false);
    pan_h = h;
    sharp_drawn(false);
}

bool menu_cover_take(int handle)
{
    struct bitmap *bm;
    if (!MOVING(global_settings.moving_cover)
        || !(audio_status() & AUDIO_STATUS_PLAY)
        || bufgetdata(handle, 0, (void *)&bm) <= 0
        || bm->width > ART_MENU_SIZE || bm->height > ART_MENU_SIZE)
        return false;
    if (menu_art.width > 0 && menu_art.handle == handle)
        return true;
    sharp_undraw();
    resume.looked = false; /* the menus' is this song's from here */
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
    if (!MOVING(global_settings.moving_cover) || travel <= 0 || p0 > travel)
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
    const bool art = ART_SHOWN();
    phases_memory(art && MOVING(on));
    sharp_memory(art);
    phases_memory(art && MOVING(on));
    /* turned on, it starts from the middle, where it's drawn while off */
    pan_clock = 0;
    pan_going = false;
    hook_set(MOVING(on));
    skin_request_full_update(CUSTOM_STATUSBAR);
}

#ifdef UI_MAIN_MENU_PANE
/* Off, the block is let go at once; on, it's taken without music, as with
   Album Art turned on. The main menu's look changes at its next draw. */
void menu_cover_pane_changed(bool on)
{
    (void)on;
    menu_cover_memory();
    skin_request_full_update(CUSTOM_STATUSBAR);
}
#endif

#endif /* HAVE_MENU_COVER */
