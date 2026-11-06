/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002-2007 Björn Stenberg
 * Copyright (C) 2007-2008 Nicolas Pennequin
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
#include "disk.h"
#include "config.h"
#include <stdio.h>
#include "string-extra.h"
#include "core_alloc.h"
#include "misc.h"
#include "font.h"
#include "system.h"
#include "rbunicode.h"
#include "sound.h"
#include "powermgmt.h"
#ifdef DEBUG
#include "debug.h"
#endif
#include "action.h"
#include "abrepeat.h"
#include "lang.h"
#include "language.h"
#include "statusbar.h"
#include "settings.h"
#include "scrollbar.h"
#include "screen_access.h"
#include "line.h"
#include "playlist.h"
#include "audio.h"
#include "tagcache.h"
#include "list.h"
#include "option_select.h"
#include "buffering.h"

#include "peakmeter.h"
/* Image stuff */
#include "bmp.h"
#ifdef HAVE_ALBUMART
#include "albumart.h"
#include "itunesdb.h"
#endif

#include "cuesheet.h"
#include "playback.h"
#include "backdrop.h"
#include "viewport.h"
#if CONFIG_TUNER
#include "radio.h"
#include "tuner.h"
#endif
#include "root_menu.h"

#include "wps.h"
#include "wps_internals.h"
#include "skin_engine.h"
#include "cover_palette.h"
#include "statusbar-skinned.h"
#include "skin_display.h"

static bool dirty[NB_SCREENS];

/* when all that changes of a skin was last drawn */
static long dynamic_tick[SKINNABLE_SCREENS_COUNT];

void skin_render(struct gui_wps *gwps, unsigned refresh_mode);
bool skin_render_volume_area(int *x, int *y, int *width, int *height);

bool skin_is_dirty(enum screen_type screen)
{
    return dirty[screen] && !(dirty[screen] = false);
}

void skin_mark_dirty(enum screen_type screen)
{
    dirty[screen] = true;
}

/* update a skinned screen, update_type is WPS_REFRESH_* values.
 * Usually it should only be WPS_REFRESH_NON_STATIC
 * A full update will be done if required (skin_do_full_update() == true)
 */
#if LCD_DEPTH == 16
/* The pixels of the screen in the skin's viewports that show, but for where the
   menus' lists go, by a hash, which of them show too: as they were after the
   last redraw, nothing of the skin has to be put on the screen again. A redraw
   of what hadn't changed, as the status bar's every seventh of a second and
   Now Playing's five times a second, put the whole screen on it each time,
   10 ms. Of the hidden viewports too, the panels', it held up the lists for
   100 ms each time. The status bar's left out where its list is, as the
   album art's strip over the main menu's right end: a step of the wheel there
   put all of it on the screen again. False in *known if the skin draws into
   its backdrop instead. */
static uint32_t skin_pixels(struct gui_wps *gwps, enum skinnable_screens skin,
                            bool *known)
{
    uint32_t h = 2166136261u;
    *known = true;
    int lx0 = 0, lx1 = 0, ly0 = 0, ly1 = 0; /* the list's */
    if (skin == CUSTOM_STATUSBAR)
    {
        const struct viewport *list = sb_skin_get_info_vp(SCREEN_MAIN);
        if (list)
        {
            lx0 = list->x;
            lx1 = list->x + list->width;
            ly0 = list->y;
            ly1 = list->y + list->height;
        }
    }
    char *const buffer = get_skin_buffer(gwps->data);
    struct skin_element *viewport = SKINOFFSETTOPTR(buffer, gwps->data->tree);
    if (skin != CUSTOM_STATUSBAR)
    {
        /* Now Playing, which has the screen to itself: all of it, two pixels a
           word. By its viewports, a pixel at a time and those that overlap
           again, it took 36 ms on the iPod, more than putting the screen on it,
           five times a second, and its volume bar went by fits */
        for (; viewport; viewport = SKINOFFSETTOPTR(buffer, viewport->next))
        {
            const struct skin_viewport *svp = SKINOFFSETTOPTR(buffer, viewport->data);
            if (svp && !(svp->hidden_flags & (VP_NEVER_VISIBLE | VP_DRAW_HIDDEN))
                && svp->output_to_backdrop_buffer)
            {
                *known = false;
                return 0;
            }
        }
        for (int y = 0; y < LCD_HEIGHT; y++)
        {
            const uint32_t *word = (const uint32_t *)FBADDR(0, y);
            for (size_t n = LCD_WIDTH * sizeof(fb_data) / sizeof(*word);
                 n >= 4; n -= 4, word += 4)
            {
                h = (h ^ word[0]) * 16777619u;
                h = (h ^ word[1]) * 16777619u;
                h = (h ^ word[2]) * 16777619u;
                h = (h ^ word[3]) * 16777619u;
            }
        }
        return h;
    }
    for (int n = 0; viewport; viewport = SKINOFFSETTOPTR(buffer, viewport->next), n++)
    {
        const struct skin_viewport *svp = SKINOFFSETTOPTR(buffer, viewport->data);
        if (!svp || svp->is_infovp
            || (skin == CUSTOM_STATUSBAR && svp->label == VP_DEFAULT_LABEL))
            continue;
        if (svp->hidden_flags & (VP_NEVER_VISIBLE | VP_DRAW_HIDDEN))
            continue;
        if (svp->output_to_backdrop_buffer)
        {
            *known = false;
            return 0;
        }
        h = (h ^ (uint32_t)n) * 16777619u; /* that it shows */
        const int x0 = MAX(svp->vp.x, 0), y0 = MAX(svp->vp.y, 0);
        const int x1 = MIN(svp->vp.x + svp->vp.width, LCD_WIDTH);
        const int y1 = MIN(svp->vp.y + svp->vp.height, LCD_HEIGHT);
        for (int y = y0; y < y1; y++)
        {
            const bool beside = y >= ly0 && y < ly1;
            const int left_end = beside ? MIN(x1, lx0) : x1;
            const int right_start = beside ? MAX(x0, lx1) : x1;
            const fb_data *p = FBADDR(x0, y);
            for (int x = x0; x < left_end; x++)
                h = (h ^ FB_UNPACK_SCALAR_LCD(p[x - x0])) * 16777619u;
            for (int x = right_start; x < x1; x++)
                h = (h ^ FB_UNPACK_SCALAR_LCD(p[x - x0])) * 16777619u;
        }
    }
    return h;
}

/* of each skin, as the screen was last updated for it */
static uint32_t shown_pixels[SKINNABLE_SCREENS_COUNT];
static bool shown_known[SKINNABLE_SCREENS_COUNT];
#endif

/* what was drawn wasn't put on the screen after all, as the theme was turned
   off: the next redraw is, whatever it shows */
void skin_forget_shown(void)
{
#if LCD_DEPTH == 16
    for (int i = 0; i < SKINNABLE_SCREENS_COUNT; i++)
        shown_known[i] = false;
#endif
}

void skin_update(enum skinnable_screens skin, enum screen_type screen,
                 unsigned int update_type)
{
    struct gui_wps *gwps = skin_get_gwps(skin, screen);
    /* This maybe shouldnt be here,
     * This is also safe for skined screen which dont use the id3 */
    struct mp3entry *id3 = get_wps_state()->id3;
    bool cuesheet_update = (id3 != NULL ? cuesheet_subtrack_changed(id3) : false);
    if (cuesheet_update)
        skin_request_full_update(skin);

    const bool full = skin_do_full_update(skin, screen);
    skin_render(gwps, full ? SKIN_REFRESH_ALL : update_type);
    if (update_type & SKIN_REFRESH_DYNAMIC)
        dynamic_tick[skin] = current_tick;
#if LCD_DEPTH == 16
    if (screen == SCREEN_MAIN)
    {
        bool known;
        const uint32_t now = skin_pixels(gwps, skin, &known);
        const bool same = !full && known && shown_known[skin]
                          && now == shown_pixels[skin];
        shown_pixels[skin] = now;
        shown_known[skin] = known;
        if (same)
            return;
    }
#endif
    skin_mark_dirty(screen);
}

/* Only what shows the volume drawn again, and only that put on the screen, as
   the wheel turns it. All of Now Playing drawn for each step, and all of the
   screen put on it, the bar went by fits and starts. Not shown, as on the
   first step after a while, all that changes is drawn, which shows it. */
void skin_update_volume(enum skinnable_screens skin, enum screen_type screen)
{
    struct gui_wps *gwps = skin_get_gwps(skin, screen);
    int x, y, width, height;
    if (skin_do_full_update(skin, screen))
        skin_request_full_update(skin);
    /* the rest as often as Now Playing's own updates: the wheel turning on,
       they waited for it to stop, the time and the scrolling lines */
    else if (TIME_BEFORE(current_tick, dynamic_tick[skin] + HZ/5))
    {
        skin_render(gwps, SKIN_REFRESH_VOLUME);
        if (skin_render_volume_area(&x, &y, &width, &height))
        {
            /* of the screen, whatever the viewport */
            gwps->display->update_rect(x, y, width, height);
#if LCD_DEPTH == 16
            /* the screen isn't as the last hash of it has it */
            if (screen == SCREEN_MAIN)
                shown_known[skin] = false;
#endif
            return;
        }
    }
    skin_update(skin, screen, SKIN_REFRESH_NON_STATIC);
}

#ifdef AB_REPEAT_ENABLE

#define DIRECTION_RIGHT 1
#define DIRECTION_LEFT -1

static int ab_calc_mark_x_pos(int mark, int capacity,
        int offset, int size)
{
    return offset + ( (size * mark) / capacity );
}

static void ab_draw_vertical_line_mark(struct screen * screen,
                                              int x, int y, int h)
{
    screen->set_drawmode(DRMODE_COMPLEMENT);
    screen->vline(x, y, y+h-1);
}

static void ab_draw_arrow_mark(struct screen * screen,
                                      int x, int y, int h, int direction)
{
    /* draw lines in decreasing size until a height of zero is reached */
    screen->set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
    while( h > 0 )
    {
        screen->vline(x, y, y+h-1);
        h -= 2;
        y++;
        x += direction;
        screen->set_drawmode(DRMODE_COMPLEMENT);
    }
}

void ab_draw_markers(struct screen * screen, int capacity,
                     int x, int y, int w, int h)
{
    bool a_set, b_set;
    unsigned int a, b;
    int xa, xb;

    a_set = ab_get_A_marker(&a);
    b_set = ab_get_B_marker(&b);
    xa = ab_calc_mark_x_pos(a, capacity, x, w);
    xb = ab_calc_mark_x_pos(b, capacity, x, w);
    /* if both markers are set, determine if they're far enough apart
    to draw arrows */
    if ( a_set && b_set )
    {
        int arrow_width = (h+1) / 2;
        if ( (xb-xa) < (arrow_width*2) )
        {
            ab_draw_vertical_line_mark(screen, xa, y, h);
            ab_draw_vertical_line_mark(screen, xb, y, h);
            return;
        }
    }

    if (a_set)
        ab_draw_arrow_mark(screen, xa, y, h, DIRECTION_RIGHT);

    if (b_set)
        ab_draw_arrow_mark(screen, xb, y, h, DIRECTION_LEFT);
}

#endif

/* A peak meter bar with the hold option: the level falls back by the peak
 * release rate - pixels per tick, here of a bar of the given size - and
 * the highest recent level is held for the peak hold time. */
static unsigned long peak_bar_hold(struct progressbar *pb, unsigned long end,
                                   int size)
{
    int release, hold_ticks;
    long delta = current_tick - pb->last_tick;
    int level = end;

    peak_meter_get_times(&release, &hold_ticks);
    pb->last_tick = current_tick;
    if (size <= 0)
        size = 1;
    if (delta >= 0 && delta < HZ)
        level = MAX(level, pb->level - delta * release * MAX_PEAK / size);
    pb->level = level;

    if (TIME_AFTER(current_tick, pb->hold_tick))
        pb->hold_pos = 0;
    if (level > pb->hold_pos)
    {
        pb->hold_pos = level;
        pb->hold_tick = current_tick + hold_ticks;
    }
    return level;
}

#if LCD_DEPTH >= 16
/* Three bars of a sound wave, the format, and the bitrate in lighter letters on the
   same baseline. The font of the viewport is for the format, and a viewport the theme
   names "bitrate" and doesn't show gives the one for the bitrate. */
#define FORMAT_COLOUR   LCD_RGBPACK(0x35, 0x38, 0x38)
/* The bars are a pixel and a half wide and go half a pixel into their first and
   last rows, in the middle of the capitals: a half and a quarter of the colour over
   the white behind */
#define FORMAT_BAR_HALF     LCD_RGBPACK(0x9a, 0x9b, 0x9b)
#define FORMAT_BAR_QUARTER  LCD_RGBPACK(0xcc, 0xcd, 0xcd)
#define BITRATE_COLOUR  LCD_RGBPACK(0x8a, 0x8d, 0x8a)
#define FORMAT_TEXT_X   12
#define FORMAT_TEXT_Y   1
#define BITRATE_GAP     7

void draw_format_bitrate(struct gui_wps *gwps, struct skin_viewport *skin_viewport)
{
    static const struct { signed char x, top, bottom; } bars[] =
        { { 0, 5, 10 }, { 3, 1, 14 }, { 6, 3, 12 } };
    struct screen *display = gwps->display;
    struct viewport *vp = &skin_viewport->vp;
    const struct mp3entry *id3 = get_wps_state()->id3;
    const struct skin_viewport *bitrate_vp =
        skin_find_item("bitrate", SKIN_FIND_VP, gwps->data);
    const int format_font = vp->font;
    const int bitrate_font = bitrate_vp ? bitrate_vp->vp.font : format_font;
    char bitrate[32];
    int format_width, format_height;

    screen_clear_area(display, 0, 0, vp->width, vp->height);
    if (!id3)
        return;

    /* What the database gives at first has no format, but it comes with what the
       path tells, and the bitrate. AAC and ALAC have the same extensions, but
       iTunes makes AAC of 320 kbps at most, and ALAC is nearly always well over
       this. An ALAC file that's under it shows as AAC until the file has been
       read. A bitrate of 1 is the database having none. */
    unsigned int codec = id3->codectype;
    if (codec == AFMT_UNKNOWN)
    {
        codec = id3->path_codectype;
        if (codec == AFMT_MP4_ALAC || codec == AFMT_MP4_AAC
            || codec == AFMT_MP4_AAC_HE)
            codec = id3->bitrate < 400 ? AFMT_MP4_AAC : AFMT_MP4_ALAC;
    }
    if (codec == AFMT_UNKNOWN || codec >= AFMT_NUM_CODECS || id3->bitrate <= 1)
        return;

    const char *format = get_codec_string(codec);
    const unsigned old_fg = display->get_foreground();
    const bool dark = global_settings.dark_theme;
    unsigned colour = dark ? LCD_RGBPACK(0xd8, 0xda, 0xdc) : FORMAT_COLOUR,
             half = dark ? LCD_RGBPACK(0x74, 0x75, 0x76) : FORMAT_BAR_HALF,
             quarter = dark ? LCD_RGBPACK(0x43, 0x44, 0x45) : FORMAT_BAR_QUARTER,
             bitrate_colour = dark ? LCD_RGBPACK(0x8e, 0x90, 0x94) : BITRATE_COLOUR;
#ifdef HAVE_ALBUMART
    const struct cover_palette *palette = cover_palette_get();
    if (palette)
    {
        colour = palette->format;
        half = palette->format_half;
        quarter = palette->format_quarter;
        bitrate_colour = palette->muted;
    }
#endif

    display->set_drawmode(DRMODE_SOLID);
    for (int i = 0; i < (int)ARRAYLEN(bars); i++)
    {
        const int x = bars[i].x, top = bars[i].top, bottom = bars[i].bottom;
        display->set_foreground(colour);
        display->vline(x, top + 1, bottom - 1);
        display->set_foreground(half);
        display->vline(x + 1, top + 1, bottom - 1);
        display->drawpixel(x, top);
        display->drawpixel(x, bottom);
        display->set_foreground(quarter);
        display->drawpixel(x + 1, top);
        display->drawpixel(x + 1, bottom);
    }

    display->set_drawmode(DRMODE_FG);
    display->set_foreground(colour);
    display->putsxy(FORMAT_TEXT_X, FORMAT_TEXT_Y, format);
    display->getstringsize(format, &format_width, &format_height);

    snprintf(bitrate, sizeof(bitrate), str(LANG_BITRATE_KBPS), id3->bitrate);
    vp->font = bitrate_font;
    display->set_foreground(bitrate_colour);
    display->putsxy(FORMAT_TEXT_X + format_width + BITRATE_GAP,
                    FORMAT_TEXT_Y + font_get(format_font)->ascent
                    - font_get(bitrate_font)->ascent, bitrate);
    vp->font = format_font;

    display->set_drawmode(DRMODE_SOLID);
    display->set_foreground(old_fg);
}
#else
void draw_format_bitrate(struct gui_wps *gwps, struct skin_viewport *skin_viewport)
{
    (void)gwps;
    (void)skin_viewport;
}
#endif /* LCD_DEPTH >= 16 */

#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
static bool in_cover_colours(const struct gui_wps *gwps, const struct bitmap *bm)
{
    const struct cover_palette *palette = cover_palette_get();
    return palette && palette->whole_screen && bm->format != FORMAT_MONO
        && gwps->data == skin_get_gwps(WPS, SCREEN_MAIN)->data;
}
#endif

/* What the fill of a bar is drawn from, in the colours of the cover if they are on the
   whole screen: it's drawn from inside the scrollbar code, so a copy of all of it */
static struct bitmap *tinted_bar_bitmap(const struct gui_wps *gwps, struct bitmap *bm)
{
#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
    static struct bitmap tinted;

    if (in_cover_colours(gwps, bm) && bm->alpha_offset == 0)
    {
        void *pixels = cover_palette_bar(bm, global_settings.progress_bar_style == STYLE_IOS);
        if (pixels)
        {
            tinted = *bm;
            tinted.data = pixels;
            return &tinted;
        }
    }
#else
    (void)gwps;
#endif
    return bm;
}

/* the image being drawn keeps its own colours, as the battery does */
static bool keep_own_colours;

/* A bitmap of the theme, in the colours of the cover if they are on the whole screen */
static void draw_bmp_part(struct gui_wps *gwps, const struct bitmap *bm,
                          int src_x, int src_y, int x, int y, int width, int height)
{
#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
    if (width <= LCD_WIDTH && in_cover_colours(gwps, bm))
    {
        static fb_data line[LCD_WIDTH];
        struct bitmap line_bm = *bm;
        const int top = (*gwps->display->current_viewport)->y + y;
        const fb_data *row = (const fb_data *)bm->data + src_y * bm->width + src_x;

        /* an icon of the status bar as the theme has it, if it reads so there */
        bool own_ink = false;
        if (global_settings.status_icons == STATUS_ICONS_OWN
            && cover_palette_in_status_bar(top))
        {
            const int left = (*gwps->display->current_viewport)->x + x;
            int most = 0;
            const fb_data *r = row;
            for (int i = 0; i < height; i++, r += bm->width)
                cover_palette_status_bar_contrast(r, width, left, top + i, &most);
            own_ink = most >= COVER_PALETTE_ICON_CONTRAST;
        }

        if (bm->alpha_offset > 0)
        {
            /* Its alpha channel comes after its pixels, in whole bytes a row, so a line
               of each with the line's alpha after it. The width it's tinted in is all
               of the bitmap's, so its alpha and its pixels stay in step. */
            static uint32_t alpha_line[(LCD_WIDTH * sizeof(fb_data) + LCD_WIDTH / 2) / 4 + 2];
            fb_data *const pixels = (fb_data *)alpha_line;
            const int alpha_stride = (bm->width + 1) / 2;
            const unsigned char *alpha = bm->data + bm->alpha_offset + src_y * alpha_stride;
            if (bm->width > LCD_WIDTH)
            {
                gwps->display->bmp_part(bm, src_x, src_y, x, y, width, height);
                return;
            }
            row -= src_x;
            line_bm.data = (void *)alpha_line;
            line_bm.height = 1;
            line_bm.alpha_offset = bm->width * sizeof(fb_data);
            for (int i = 0; i < height; i++, row += bm->width, alpha += alpha_stride)
            {
                cover_palette_tint_line(row, pixels, bm->width, top + i);
                if (own_ink)
                    cover_palette_keep_ink(row, pixels, bm->width);
                else if (keep_own_colours)
                    cover_palette_keep_colours(row, pixels, bm->width);
                memcpy((unsigned char *)alpha_line + line_bm.alpha_offset, alpha, alpha_stride);
                gwps->display->bmp_part(&line_bm, src_x, 0, x, y + i, width, 1);
            }
            return;
        }

        line_bm.data = (void *)line;
        line_bm.width = width;
        line_bm.height = 1;
        for (int i = 0; i < height; i++, row += bm->width)
        {
            cover_palette_tint_line(row, line, width, top + i);
            if (own_ink)
                cover_palette_keep_ink(row, line, width);
            else if (keep_own_colours)
                cover_palette_keep_colours(row, line, width);
            gwps->display->bmp_part(&line_bm, 0, 0, x, y + i, width, 1);
        }
        return;
    }
#endif
    gwps->display->bmp_part(bm, src_x, src_y, x, y, width, height);
}

/* When the volume bar was last drawn, a change of the volume to draw still, and
   when it came */
static long volume_shown_tick;
static bool volume_pending;
static long volume_changed_tick;

/* the bar's frames at most, apart: the BCM puts all of the screen on the panel
   at each, in more than a frame's time */
#define VOLUME_FRAME_TICKS 2

static unsigned long volume_bar_end(unsigned long end)
{
    volume_shown_tick = current_tick;
    volume_pending = false;
    return end;
}

void skin_volume_changed(enum skinnable_screens skin, enum screen_type screen)
{
    /* At once, unless it was drawn less than a frame's time ago: then at the
       frame's time. A step of the bar a step of the wheel, all of them the
       same, it goes as the wheel does without gliding between: the glide
       didn't feel quick, and steps of 1 to 4% of it went by fits */
    if (!TIME_BEFORE(current_tick, volume_shown_tick + VOLUME_FRAME_TICKS))
        skin_update_volume(skin, screen);
    else
    {
        volume_pending = true;
        volume_changed_tick = current_tick;
    }
}

void draw_progressbar(struct gui_wps *gwps, struct skin_viewport* skin_viewport,
                      int line, struct progressbar *pb)
{
    struct screen *display = gwps->display;
    struct viewport *vp = &skin_viewport->vp;
    struct wps_state *state = get_wps_state();
    struct mp3entry *id3 = state->id3;
    int x = pb->x, y = pb->y, width = pb->width, height = pb->height;
    unsigned long length = 1, end = 0;
    int flags = HORIZONTAL;

    if (height < 0)
        height = font_get(vp->font)->height;

    if (y < 0)
    {
        int line_height = font_get(vp->font)->height;
        /* center the pb in the line, but only if the line is higher than the pb */
        int center = (line_height-height)/2;
        /* if Y was not set calculate by font height,Y is -line_number-1 */
        y = line*line_height + (0 > center ? 0 : center);
    }

    if (pb->type == SKIN_TOKEN_VOLUMEBAR)
    {
        int minvol = sound_min(SOUND_VOLUME);
        int maxvol = sound_max(SOUND_VOLUME);
        length = 1000;
#ifdef HAVE_PERCEPTUAL_VOLUME
        if (global_settings.volume_adjust_mode == VOLUME_ADJUST_PERCEPTUAL)
        {
            /* by the volume's step of the bar's even ones */
            int count;
            end = volume_get_step(&count) * length / MAX(count, 1);
        }
        else
#endif
#if defined(HAVE_PERCEPTUAL_VOLUME) || defined(HAVE_TOUCHSCREEN)
        end = to_normalized_volume(global_status.volume, minvol, maxvol, length);
#else
        end = (global_status.volume - minvol) * length / MAX(maxvol - minvol, 1);
#endif
        end = volume_bar_end(end);
    }
    else if (pb->type == SKIN_TOKEN_BATTERY_PERCENTBAR)
    {
        length = 100;
        end = battery_level();
    }
    else if (pb->type == SKIN_TOKEN_DISK_USEDBAR)
    {
        sector_t size = 0, free = 0;
        volume_size(IF_MV(0,) &size, &free);
        length = size;
        end = size - free;
    }
    else if (pb->type == SKIN_TOKEN_PLAYLIST_PROGRESSBAR)
    {
        unsigned long pl_elapsed, pl_total;
        if (id3 && wps_get_playlist_percent(id3,
                       id3->elapsed + state->ff_rewind_count,
                       &pl_elapsed, &pl_total)
            && pl_total > 0)
        {
            length = pl_total;
            end = pl_elapsed;
        }
    }
    else if (pb->type == SKIN_TOKEN_PEAKMETER_LEFTBAR ||
             pb->type == SKIN_TOKEN_PEAKMETER_RIGHTBAR)
    {
        int left, right, val;
        peak_meter_current_vals(&left, &right);
        val = pb->type == SKIN_TOKEN_PEAKMETER_LEFTBAR ? left : right;
        length = MAX_PEAK;
        end = peak_meter_scale_value(val, length);
        if (pb->hold)
            end = peak_bar_hold(pb, end, pb->horizontal ? width : height);
    }
    else if (pb->type == SKIN_TOKEN_PLAYLIST_PERCENTBAR)
    {
        length = playlist_amount();
        end = playlist_get_display_index();
    }
    else if (pb->type == SKIN_TOKEN_LIST_SCROLLBAR)
    {
        int val, min, max;
        skinlist_get_scrollbar(&val, &min, &max);
        end = val - min;
        length = max - min;
    }
    else if (pb->type == SKIN_TOKEN_SETTINGBAR)
    {
        int val, count;
        get_setting_info_for_bar(pb->setting, pb->setting_offset, &count, &val);
        length = count - 1;
        end = val;
    }
#if CONFIG_TUNER
    else if (in_radio_screen() || (get_radio_status() != FMRADIO_OFF))
    {
#ifdef HAVE_RADIO_RSSI
        if (pb->type == SKIN_TOKEN_TUNER_RSSI_BAR)
        {
            int val = tuner_get(RADIO_RSSI);
            int min = tuner_get(RADIO_RSSI_MIN);
            int max = tuner_get(RADIO_RSSI_MAX);
            end = val - min;
            length = max - min;
        }
        else
#endif
        {
            int min = fm_region_data[global_settings.fm_region].freq_min;
            end = radio_get_current_frequency() - min;
            length = fm_region_data[global_settings.fm_region].freq_max - min;
        }
    }
#endif
    else if (id3 && id3->length)
    {
        length = id3->length;
        end = id3->elapsed + state->ff_rewind_count;
    }

    if (length <= 0)
    {
        length = 1;
        end = 0;
    }

    if (!pb->horizontal)
    {
        /* we want to fill upwards which is technically inverted. */
        flags = INVERTFILL;
    }

    if (pb->invert_fill_direction)
    {
        flags ^= INVERTFILL;
    }

    if (pb->nofill)
    {
        flags |= INNER_NOFILL;
    }

    if (pb->noborder)
    {
        flags |= BORDER_NOFILL;
    }

    if (SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->slider))
    {
        struct gui_img *img = SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->slider);
        if (pb->slider_centre)
        {
            /* clear the slider, which can stand out past the ends */
            if (flags&HORIZONTAL)
                screen_clear_area(display, x - img->bm.width / 2, y,
                                  width + img->bm.width, height);
            else
                screen_clear_area(display, x, y - img->bm.height / 2,
                                  width, height + img->bm.height);
        }
        else
        {
            /* clear the slider */
            screen_clear_area(display, x, y, width, height);

            /* account for the sliders width in the progressbar */
            if (flags&HORIZONTAL)
            {
                width -= img->bm.width;
            }
            else
            {
                height -= img->bm.height;
            }
        }
    }

    if (SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->backdrop))
    {
        struct gui_img *img = SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->backdrop);
        img->bm.data = core_get_data(img->buflib_handle);
#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
        /* the iOS look's track, of its own shade with the cover's colours: the
           theme's tinted went with the background */
        void *track = NULL;
        if (global_settings.progress_bar_style == STYLE_IOS && in_cover_colours(gwps, &img->bm)
            && img->bm.alpha_offset == 0)
            track = cover_palette_bar_track(&img->bm);
        if (track)
        {
            struct bitmap flat = img->bm;
            flat.data = track;
            display->bmp_part(&flat, 0, 0, x, y, pb->width, height);
        }
        else
#endif
        draw_bmp_part(gwps, &img->bm, 0, 0, x, y, pb->width, height);
        flags |= DONT_CLEAR_EXCESS;
    }

    if (!pb->nobar)
    {
        struct gui_img *img = SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->image);
        if (img)
        {
            char *img_data = core_get_data(img->buflib_handle);
            img->bm.data = img_data;
            gui_bitmap_scrollbar_draw(display, tinted_bar_bitmap(gwps, &img->bm),
                                    x, y, width, height,
                                    length, 0, end, flags);
        }
        else
            gui_scrollbar_draw(display, x, y, width, height,
                               length, 0, end, flags);
    }

    /* the held peak of a peak meter bar: a block hold_size pixels thick
     * ending at the held level, cut from the fill image if the bar has one,
     * so that a bar drawn as LED segments holds a segment */
    if (pb->hold && pb->hold_pos > 0 &&
        (pb->type == SKIN_TOKEN_PEAKMETER_LEFTBAR ||
         pb->type == SKIN_TOKEN_PEAKMETER_RIGHTBAR))
    {
        struct gui_img *img = pb->nobar ? NULL :
                        SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->image);
        int hx = x, hy = y, hw = width, hh = height;
        int len, pos, off, size = pb->hold_size;

        if (!img && !(flags&BORDER_NOFILL))
        {
            /* inside the frame, like the bar */
            hx++; hy++; hw -= 2; hh -= 2;
        }
        len = (flags&HORIZONTAL) ? hw : hh;
        pos = len * pb->hold_pos / MAX_PEAK;
        off = pos - size;
        if (off < 0)
        {
            size += off;
            off = 0;
        }
        if (flags&INVERTFILL)
            off = len - pos;

        if (flags&HORIZONTAL)
        {
            hx += off;
            hw = size;
        }
        else
        {
            hy += off;
            hh = size;
        }

        if (img)
        {
            /* the image maps onto the bar 1:1, as in the bar itself */
            int bx = hx - x, by = hy - y;
            img->bm.data = core_get_data(img->buflib_handle);
            hw = MIN(hw, img->bm.width - bx);
            hh = MIN(hh, img->bm.height - by);
            if (hw > 0 && hh > 0)
                draw_bmp_part(gwps, &img->bm, bx, by, hx, hy, hw, hh);
        }
        else if (hw > 0 && hh > 0)
            display->fillrect(hx, hy, hw, hh);
    }

    if (SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->slider))
    {
        int xoff = 0, yoff = 0;
        int w = width, h = height;
        struct gui_img *img = SKINOFFSETTOPTR(get_skin_buffer(gwps->data), pb->slider);
        img->bm.data = core_get_data(img->buflib_handle);

        if (flags&HORIZONTAL)
        {
            w = img->bm.width;
            xoff = width * end / length;
            if (flags&INVERTFILL)
                xoff = width - xoff;
            if (pb->slider_centre)
                xoff -= w / 2;
        }
        else
        {
            h = img->bm.height;
            yoff = height * end / length;
            if (flags&INVERTFILL)
                yoff = height - yoff;
            if (pb->slider_centre)
                yoff -= h / 2;
        }
        if (pb->slider_centre)
        {
            /* over the end of the fill, which shows through the corners
               of the image rather than the backdrop */
            display->set_drawmode(DRMODE_FG);
            draw_bmp_part(gwps, &img->bm, 0, 0, x + xoff, y + yoff, w, h);
            display->set_drawmode(DRMODE_SOLID);
        }
        else
            draw_bmp_part(gwps, &img->bm, 0, 0, x + xoff, y + yoff, w, h);
    }

    if (pb->type == SKIN_TOKEN_PROGRESSBAR)
    {
        if (id3 && id3->length)
        {
#ifdef AB_REPEAT_ENABLE
            if (ab_repeat_mode_enabled())
                ab_draw_markers(display, id3->length, x, y, width, height);
#endif

            if (id3->cuesheet)
                cue_draw_markers(display, id3->cuesheet, id3->length,
                                 x, y+1, width, height-2);
        }
#if 0 /* disable for now CONFIG_TUNER */
        else if (in_radio_screen() || (get_radio_status() != FMRADIO_OFF))
        {
            presets_draw_markers(display, x, y, width, height);
        }
#endif
    }
}

/* clears the area where the image was shown */
void clear_image_pos(struct gui_wps *gwps, struct gui_img *img)
{
    if(!gwps)
        return;
    gwps->display->set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
    gwps->display->fillrect(img->x, img->y, img->bm.width, img->subimage_height);
    gwps->display->set_drawmode(DRMODE_SOLID);
}

#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
/* a line at a time, as only the shade of each pixel matters */
static void draw_cover_tinted_image(struct screen *display, const struct gui_img *img,
                                    int subimage, const fb_data *shades)
{
    static fb_data line[LCD_WIDTH];
    const int width = MIN(img->bm.width, LCD_WIDTH);
    const fb_data *row = (const fb_data *)img->bm.data
                         + img->bm.width * img->subimage_height * subimage;

    for (int y = 0; y < img->subimage_height; y++, row += img->bm.width)
    {
        for (int x = 0; x < width; x++)
        {
            /* by its green, the theme's stars being grey: on the dark theme, which
               has them light on dark, by its lightest part, as the iOS look's red
               star had the green of a shade near the background */
            const unsigned px = FB_UNPACK_SCALAR_LCD(row[x]);
            const int level = global_settings.dark_theme
                ? MAX(MAX(RGB_UNPACK_RED(px), RGB_UNPACK_GREEN(px)), RGB_UNPACK_BLUE(px))
                : RGB_UNPACK_GREEN(px);
            const int shade = level >> 2;
            /* the dark theme's lightest shade is the stars themselves */
            line[x] = shade < COVER_PALETTE_STAR_KEPT ? shades[shade]
                : global_settings.dark_theme ? shades[COVER_PALETTE_STAR_KEPT - 1] : row[x];
        }
        display->bitmap_part(line, 0, 0, STRIDE(display->screen_type, width, 1),
                             img->x, img->y + y, width, 1);
    }
}
#endif

void wps_draw_image(struct gui_wps *gwps, struct gui_img *img,
                    int subimage, struct viewport* vp)
{
    struct screen *display = gwps->display;
    img->bm.data = core_get_data(img->buflib_handle);
    display->set_drawmode(DRMODE_SOLID);

#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
    const struct cover_palette *palette = img->cover_tint ? cover_palette_get() : NULL;
    if (palette && !palette->whole_screen)
        draw_cover_tinted_image(display, img, subimage, palette->star);
    else
#endif
    if (img->is_9_segment)
        display->nine_segment_bmp(&img->bm, 0, 0, vp->width, vp->height);
    else
    {
        keep_own_colours = img->cover_keep
                           && global_settings.status_icons != STATUS_ICONS_COVER;
        draw_bmp_part(gwps, &img->bm, 0, img->subimage_height * subimage,
                      img->x, img->y, img->bm.width, img->subimage_height);
        keep_own_colours = false;
    }
}

#ifdef HAVE_ALBUMART
/* The next track's cover takes a moment to load, and with no cover there the
   backdrop's note would show until then. So the last one stays that long: a copy,
   as the buffer may have dropped it. */
#define ART_KEPT_TIME   (HZ * 3 / 4)
#define ART_KEPT_SIZE   200

static struct {
    fb_data pixels[ART_KEPT_SIZE * ART_KEPT_SIZE];
    short width, height;
    int handle;     /* the one it's a copy of, or -1 for a cover from the database: */
#ifdef ITUNESDB_HAVE_ART
    uint32_t offset;
    char path[sizeof(((struct itunesdb_art *)0)->path)];
    uint32_t colour;
#endif
    bool had_cover; /* it had one when last looked at */
    bool keeping;   /* it has none now, and the copy stays until then */
    long until;
} kept_art[2];

static void art_seen(int index, int handle)
{
    if (handle >= 0)
    {
        kept_art[index].had_cover = true;
        kept_art[index].keeping = false;
    }
    else if (kept_art[index].had_cover)
    {
        kept_art[index].had_cover = false;
        kept_art[index].keeping = true;
        kept_art[index].until = current_tick + ART_KEPT_TIME;
    }
}

static bool art_is_kept(int index)
{
    return kept_art[index].keeping
           && TIME_BEFORE(current_tick, kept_art[index].until);
}

static void keep_album_art(int index, int handle)
{
    struct bitmap *bmp;

    if (kept_art[index].width > 0 && kept_art[index].handle == handle)
        return;
    if (bufgetdata(handle, 0, (void *)&bmp) <= 0)
        return;

    const int width = bmp->width, height = bmp->height;
    if (width > ART_KEPT_SIZE || height > ART_KEPT_SIZE)
    {
        kept_art[index].width = 0;
        return;
    }
    memcpy(kept_art[index].pixels, bmp->data, width * height * sizeof(fb_data));
    kept_art[index].width = width;
    kept_art[index].height = height;
    kept_art[index].handle = handle;
}

/* The cover a theme asks for so with %Cr, as Now Playing's, stands in
   (x, y, w, h) with its reflection under it, as
   Cover Flow's covers have theirs: what the backdrop had there, the theme's
   placeholder note and its shadow, gives way to the background beside it.
   With Angled Cover it's turned a little away from the text, as an iPod
   classic shows it, and larger than the space: its left edge taller and nearer
   the screen's edge, the right one further and shorter, its top and bottom
   edges smoothed. The reflection ends where the space's would. */
/* As measured on a photo of an iPod classic taken straight on, in its screen's
   pixels: the left edge 142 high from 11 px off the screen's edge, the right one
   0.87 of it, 0.96 of the left edge's height wide, its middle 2 px below the
   space's; under it a reflection at half the cover's strength */
#define ANGLE_NEAR  267 /* of 256: the left edge's height to the space's */
#define ANGLE_FAR   223 /* of 256: the right edge's height to the left one's */
#define ANGLE_WIDTH 246 /* of 256: how wide it is turned, to the left edge's height */
#define ANGLE_LEFT  1   /* the left edge out of the space towards the screen's */
#define ANGLE_DROP  2   /* its middle below the space's */
#define ART_SPILL   12  /* how far past the space a cover can go */
#define REFLECTION_ROWS  22
#define REFLECTION_ALPHA 128 /* of 256, at its top, down to none */

#if LCD_DEPTH == 16
/* a of 256 of one, the rest of the other */
static fb_data mix_pixel(fb_data one, fb_data other, int a)
{
    return LCD_RGBPACK_LCD(
        (RGB_UNPACK_RED_LCD(one) * a + RGB_UNPACK_RED_LCD(other) * (256 - a)) >> 8,
        (RGB_UNPACK_GREEN_LCD(one) * a + RGB_UNPACK_GREEN_LCD(other) * (256 - a)) >> 8,
        (RGB_UNPACK_BLUE_LCD(one) * a + RGB_UNPACK_BLUE_LCD(other) * (256 - a)) >> 8);
}

/* the background of each row of the area, as beside the cover */
static fb_data cover_bg[LCD_HEIGHT];

/* A column of the cover in the area's column px, from the source column sx:
   its top and height in 16.16 rows of the area, and its reflection under it,
   as much shorter as the column is, down to the area's foot at most. A row of
   the source is found with a multiply by the column's step: a 64-bit divide
   for each pixel was 60 ms for the cover each time it was drawn. */
static void draw_cover_column(const struct viewport *area, const fb_data *src,
                              int stride, int sx, int sh, int px,
                              long top, long col_h)
{
    if (px < 0 || px >= area->width)
        return;
    const long bottom = top + col_h;
    /* to the foot of the area, where the space's would end: none at the foot
       of the screen */
    const long fade = MAX(((long)area->height << 16) - bottom, 0);
    const int first = MAX(0, (int)(top >> 16));
    const int last = MIN(area->height - 1, (int)((bottom + fade - 1) >> 16));
    if (first > last)
        return;
    /* rows of the source to one of the area's 16.16, in 32.32 */
    const int64_t rows = ((int64_t)sh << 32) / col_h;
    fb_data *fb = area->buffer->get_address_fn(area->x + px, area->y + first);
    const ptrdiff_t step = area->buffer->stride;
    for (int r = first; r <= last; r++, fb += step)
    {
        const long from = (long)r << 16, to = from + 65536, mid = from + 32768;
        const fb_data bg = cover_bg[r];
        /* the reflection at this row, the cover upside down and fading */
        fb_data under = bg;
        if (to > bottom && fade > 0)
        {
            const long below = MAX(mid - bottom, 0);
            int sy = sh - 1 - (int)(((int64_t)below * rows) >> 32);
            sy = MAX(0, MIN(sy, sh - 1));
            const int a = REFLECTION_ALPHA * (fade - below) / fade;
            if (a > 0)
                under = mix_pixel(src[sy * stride + sx], bg, a);
        }
        /* the cover over it, as much of the row as it covers */
        const int cover = MAX(0, (int)((MIN(bottom, to) - MAX(top, from)) >> 8));
        if (cover > 0)
        {
            int sy = (int)(((int64_t)(MIN(mid, bottom - 1) - top) * rows) >> 32);
            sy = MAX(0, MIN(sy, sh - 1));
            const fb_data pixel = src[sy * stride + sx];
            /* above the cover, the background; below, the reflection */
            const fb_data behind = from < top ? bg : under;
            *fb = cover >= 256 ? pixel : mix_pixel(pixel, behind, cover);
        }
        else
            *fb = under;
    }
}
#endif

static void draw_cover(struct gui_wps *gwps, struct skin_albumart *aa,
                       const fb_data *src, int stride,
                       int sw, int sh, int x, int y, int w, int h)
{
    struct screen *display = gwps->display;
#if LCD_DEPTH == 16
    /* the cover a theme asks for so with %Cr, as Now Playing's */
    if (aa->reflect && w > 1 && h > 1)
    {
        struct viewport *vp = display->set_viewport(NULL);
        display->set_viewport(vp);
        /* the space with room around it, and the reflection under it */
        struct viewport area = *vp;
        area.x = MAX(vp->x + x - ART_SPILL, 0);
        area.y = MAX(vp->y + y - ART_SPILL, 0);
        area.width = MIN(vp->x + x + w + ART_SPILL, LCD_WIDTH) - area.x;
        area.height = MIN(vp->y + y + h + REFLECTION_ROWS, LCD_HEIGHT) - area.y;
        const int ax = vp->x + x - area.x, ay = vp->y + y - area.y;
        /* the background left of it, where nothing is drawn over the backdrop:
           right of it the text starts */
        const int bx = MAX(area.x - 2, 1);
        for (int r = 0; r < area.height; r++)
            cover_bg[r] = *(fb_data *)vp->buffer->get_address_fn(bx, area.y + r);
        display->set_viewport(&area);
        for (int r = 0; r < area.height; r++)
        {
            display->set_foreground(cover_bg[r]);
            display->hline(0, area.width - 1, r);
        }

        if (global_settings.angled_cover)
        {
            const int near_h = h * ANGLE_NEAR / 256;
            const int pw = near_h * ANGLE_WIDTH / 256;
            const int left = ax - ANGLE_LEFT;
            const long middle = ((long)(ay + ANGLE_DROP) << 16) + ((long)h << 15);
            for (int c = 0; c < pw; c++)
            {
                /* Across the columns the nearness goes from 1 to ANGLE_FAR,
                   and the place on the cover with it, as a plane in
                   perspective has them: s * ANGLE_FAR / near of the way
                   across, its height near of the left edge's, in 16.16 */
                const long s16 = ((long)c << 16) / pw + (32768 / pw);
                const long near = 65536 - (((256 - ANGLE_FAR) * s16) >> 8);
                const int sx = MIN((int)(((int64_t)s16 * ANGLE_FAR * sw)
                                         / ((int64_t)near * 256)), sw - 1);
                const long col_h = (long)near_h * near;
                draw_cover_column(&area, src, stride, sx, sh, left + c,
                                  middle - col_h / 2, col_h);
            }
        }
        else
        {
            for (int c = 0; c < MIN(w, sw); c++)
                draw_cover_column(&area, src, stride, c, sh, ax + c,
                                  (long)ay << 16, (long)MIN(h, sh) << 16);
        }
        display->set_foreground(vp->fg_pattern);
        display->set_viewport(vp);
        return;
    }
#endif
    display->bitmap_part(src, 0, 0, stride, x, y, MIN(w, sw), MIN(h, sh));
}

/* where Now Playing's cover and its reflection were, the backdrop again */
static void clear_cover_reflection(struct gui_wps *gwps, struct skin_albumart *aa,
                                   int x, int y, int w, int h)
{
#if LCD_DEPTH == 16
    struct screen *display = gwps->display;
    if (!aa->reflect)
        return;
    struct viewport *vp = display->set_viewport(NULL);
    struct viewport area = *vp;
    area.x = MAX(vp->x + x - ART_SPILL, 0);
    area.y = MAX(vp->y + y - ART_SPILL, 0);
    area.width = MIN(vp->x + x + w + ART_SPILL, LCD_WIDTH) - area.x;
    area.height = MIN(vp->y + y + h + REFLECTION_ROWS, LCD_HEIGHT) - area.y;
    display->set_viewport(&area);
    display->set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
    display->fillrect(0, 0, area.width, area.height);
    display->set_drawmode(DRMODE_SOLID);
    display->set_viewport(vp);
#else
    (void)gwps; (void)aa; (void)x; (void)y; (void)w; (void)h;
#endif
}

static void draw_art_pixels(struct gui_wps *gwps, struct skin_albumart *aa,
                            const fb_data *pixels, int width, int height)
{
    int x = aa->x, y = aa->y;

    if (aa->xalign & WPS_ALBUMART_ALIGN_RIGHT)
        x += aa->width - width;
    else if (aa->xalign & WPS_ALBUMART_ALIGN_CENTER)
        x += (aa->width - width) / 2;
    if (aa->yalign & WPS_ALBUMART_ALIGN_BOTTOM)
        y += aa->height - height;
    else if (aa->yalign & WPS_ALBUMART_ALIGN_CENTER)
        y += (aa->height - height) / 2;

    draw_cover(gwps, aa, pixels,
               STRIDE(gwps->display->screen_type, width, height),
               width, height, x, y, width, height);
}

static void draw_kept_album_art(struct gui_wps *gwps, struct skin_albumart *aa,
                                int index)
{
    draw_art_pixels(gwps, aa, kept_art[index].pixels, kept_art[index].width,
                    kept_art[index].height);
}

#if defined(ITUNESDB_HAVE_ART) && defined(AUDIO_FAST_SKIP_PREVIEW)
/* The cover Finder made for the track Now Playing shows, when the buffer has none of it:
   while a skip is on its way to a track that isn't in the buffer, as Now Playing already
   shows what the database tells of it, and for the track that started then, as playback
   leaves its cover to Now Playing */
static bool shown_db_art(int handle, struct itunesdb_art *art)
{
    if (playback_skip_art(art))
        return true;
    const struct mp3entry *id3 = get_wps_state()->id3;
    return handle < 0 && id3 && id3->path[0] && global_settings.album_art != AA_OFF
           && (audio_status() & AUDIO_STATUS_PLAY) && itunesdb_find_art(id3->path, art);
}

/* That cover, in the copy kept between covers */
static bool db_art_ready(const struct wps_data *data, int index, int handle)
{
    struct skin_albumart *aa = SKINOFFSETTOPTR(get_skin_buffer((struct wps_data *)data),
                                               index ? data->albumart2 : data->albumart);
    struct itunesdb_art art;
    if (!aa || !shown_db_art(handle, &art))
        return false;
    if (kept_art[index].width > 0 && kept_art[index].handle < 0
        && kept_art[index].offset == art.offset && !strcmp(kept_art[index].path, art.path))
        return true;

    struct bitmap bm;
    memset(&bm, 0, sizeof(bm));
    bm.width = MIN(aa->width, ART_KEPT_SIZE);
    bm.height = MIN(aa->height, ART_KEPT_SIZE);
    bm.data = (unsigned char *)kept_art[index].pixels;
    kept_art[index].width = 0;
    if (itunesdb_load_art(&art, &bm, sizeof(bm) + sizeof(kept_art[index].pixels)) < 0)
        return false;
    kept_art[index].width = bm.width;
    kept_art[index].height = bm.height;
    kept_art[index].handle = -1;
    kept_art[index].offset = art.offset;
    strcpy(kept_art[index].path, art.path);
    /* one whose album wasn't given its colour at an import */
    kept_art[index].colour = art.colour;
    if (art.colour == COVER_COLOUR_UNKNOWN && global_settings.cover_colours != COVER_COLOURS_OFF)
        kept_art[index].colour = cover_palette_colour(kept_art[index].pixels, bm.width,
                                                      bm.height);
    return true;
}
#endif

#ifdef ITUNESDB_HAVE_ART
/* The cover beside the menus. It's larger than any Finder keeps and the panel is rarely
   on screen when a track changes, so playback doesn't load it, and it's scaled here
   when it's to be shown, and again only for another cover. */
#define ART_MENU_SIZE LCD_HEIGHT
static struct {
    fb_data pixels[ART_MENU_SIZE * ART_MENU_SIZE];
    short width, height;
    uint32_t offset;
    char path[sizeof(((struct itunesdb_art *)0)->path)];
} menu_art;

static bool menu_art_ready(const struct wps_data *data)
{
    struct skin_albumart *aa = SKINOFFSETTOPTR(get_skin_buffer((struct wps_data *)data),
                                               data->albumart);
    struct mp3entry *id3 = audio_current_track();
    struct itunesdb_art art;
    if (!aa || !id3 || !(audio_status() & AUDIO_STATUS_PLAY)
        || global_settings.album_art == AA_OFF
        || !itunesdb_find_art(id3->path, &art))
        return false;
    if (menu_art.width > 0 && menu_art.offset == art.offset
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
    return true;
}
#endif /* ITUNESDB_HAVE_ART */

/* What %?C says: it comes before the %Cd, and a theme may change what it shows by
   it, so going without a cover has to start here already */
bool skin_art_found(const struct wps_data *data, int handle)
{
    if (data != skin_get_gwps(WPS, SCREEN_MAIN)->data)
#ifdef ITUNESDB_HAVE_ART
        return handle >= 0 || menu_art_ready(data);
#else
        return handle >= 0;
#endif

#if defined(ITUNESDB_HAVE_ART) && defined(AUDIO_FAST_SKIP_PREVIEW)
    struct itunesdb_art art;
    if (handle < 0 && shown_db_art(handle, &art))
    {
        art_seen(0, 0); /* as one it has */
        return true;
    }
#endif
    art_seen(0, handle);
    return handle >= 0 || art_is_kept(0);
}

/* Whether Now Playing has a cover to show, with the handle of the buffer's */
bool skin_art_available(int handle)
{
#if defined(ITUNESDB_HAVE_ART) && defined(AUDIO_FAST_SKIP_PREVIEW)
    struct itunesdb_art art;
    if (handle < 0 && shown_db_art(handle, &art))
        return true;
#endif
    return handle >= 0;
}

/* Now Playing's colours for the cover it shows */
bool skin_art_colours_update(const struct wps_data *data)
{
    const int handle = playback_current_aa_hid(data->playback_aa_slot);
#if defined(ITUNESDB_HAVE_ART) && defined(AUDIO_FAST_SKIP_PREVIEW)
    struct itunesdb_art art;
    if (shown_db_art(handle, &art))
    {
        if (art.colour == COVER_COLOUR_UNKNOWN && db_art_ready(data, 0, handle))
            art.colour = kept_art[0].colour;
        if (art.colour != COVER_COLOUR_UNKNOWN)
            return cover_palette_update(COVER_ART_BY_COLOUR, art.colour);
    }
#endif
    return cover_palette_update(handle, playback_current_cover_colour());
}

int skin_art_handle(const struct wps_data *data, int index, int handle)
{
    if (data != skin_get_gwps(WPS, SCREEN_MAIN)->data)
    {
#ifdef ITUNESDB_HAVE_ART
        if (handle < 0 && menu_art_ready(data))
            return SKIN_ART_MENU;
#endif
        return handle;
    }

#if defined(ITUNESDB_HAVE_ART) && defined(AUDIO_FAST_SKIP_PREVIEW)
    if (db_art_ready(data, index, handle))
    {
        /* as one it has, so it stays when the track changes until the next is in */
        art_seen(index, 0);
        return SKIN_ART_KEPT;
    }
#endif
    art_seen(index, handle);
    if (handle < 0 && kept_art[index].width > 0 && art_is_kept(index))
        return SKIN_ART_KEPT;
    return handle;
}

/* True once, when the time for it is up with no cover there: the note can show */
bool skin_art_kept_expired(void)
{
    bool expired = false;
    for (int i = 0; i < 2; i++)
    {
        if (kept_art[i].keeping && !TIME_BEFORE(current_tick, kept_art[i].until))
        {
            kept_art[i].keeping = false;
            expired = true;
        }
    }
    return expired;
}
#endif /* HAVE_ALBUMART */

void wps_display_images(struct gui_wps *gwps, struct viewport* vp)
{
    if(!gwps || !gwps->data || !gwps->display)
        return;
    (void)vp;
    struct wps_data *data = gwps->data;
    struct screen *display = gwps->display;

    /* Start with album art, as it may be drawn over by mask images */
#ifdef HAVE_ALBUMART
    for (int i = 0; i < 2; i++)
    {
        struct skin_albumart *aa = SKINOFFSETTOPTR(get_skin_buffer(data),
                                        i ? data->albumart2 : data->albumart);
        if (aa && aa->draw_handle >= 0)
        {
            draw_album_art(gwps, aa, aa->draw_handle, false);
            /* not what the lists' skin draws, of another size */
            if (data == skin_get_gwps(WPS, SCREEN_MAIN)->data)
                keep_album_art(i, aa->draw_handle);
            aa->draw_handle = -1;
        }
        else if (aa && aa->draw_handle == SKIN_ART_KEPT)
        {
            draw_kept_album_art(gwps, aa, i);
            aa->draw_handle = -1;
        }
#ifdef ITUNESDB_HAVE_ART
        else if (aa && aa->draw_handle == SKIN_ART_MENU)
        {
            draw_art_pixels(gwps, aa, menu_art.pixels, menu_art.width, menu_art.height);
            aa->draw_handle = -1;
        }
#endif
    }
#endif
    struct skin_token_list *list = SKINOFFSETTOPTR(get_skin_buffer(data), data->images);
    while (list)
    {
        struct wps_token *token = SKINOFFSETTOPTR(get_skin_buffer(data), list->token);
        struct gui_img *img = NULL;
        if (token)
            img = (struct gui_img*)SKINOFFSETTOPTR(get_skin_buffer(data), token->value.data);
        if (img) {
            if (img->using_preloaded_icons && img->display >= 0)
            {
                screen_put_icon(display, img->x, img->y, img->display);
            }
            else if (img->loaded)
            {
                if (img->display >= 0)
                {
                    wps_draw_image(gwps, img, img->display, vp);
                }
            }
        }
        list = SKINOFFSETTOPTR(get_skin_buffer(data), list->next);
    }

    display->set_drawmode(DRMODE_SOLID);
}

/* Evaluate the conditional that is at *token_index and return whether a skip
   has ocurred. *token_index is updated with the new position.
*/
int evaluate_conditional(struct gui_wps *gwps, int offset,
                         struct conditional *conditional, int num_options)
{
    if (!gwps)
        return false;

    char result[128];
    const char *value;

    int intval = num_options < 2 ? 2 : num_options;
    /* get_token_value needs to know the number of options in the enum */
    value = get_token_value(gwps, SKINOFFSETTOPTR(get_skin_buffer(gwps->data), conditional->token),
                    offset, result, sizeof(result), &intval);

    /* intval is now the number of the enum option we want to read,
       starting from 1. If intval is -1, we check if value is empty. */
    if (intval == -1)
    {
        if (num_options == 1) /* so %?AA<true> */
            intval = (value && *value) ? 1 : 0; /* returned as 0 for true, -1 for false */
        else
            intval = (value && *value) ? 1 : num_options;
    }
    else if (intval > num_options || intval < 1)
        intval = num_options;

    return intval -1;
}


/* Display a line appropriately according to its alignment format.
   format_align contains the text, separated between left, center and right.
   line is the index of the line on the screen.
   scroll indicates whether the line is a scrolling one or not.
*/
void write_line(struct screen *display, struct align_pos *format_align,
                int line, bool scroll, struct line_desc *linedes)
{
    int left_width = 0;
    int center_width = 0, center_xpos;
    int right_width = 0,  right_xpos;
    int space_width;
    int string_height;
    int scroll_width;
    int viewport_width = display->getwidth();

    /* calculate different string sizes and positions */
    display->getstringsize((unsigned char *)" ", &space_width, &string_height);
    if (format_align->left != 0) {
        display->getstringsize((unsigned char *)format_align->left,
                                &left_width, &string_height);
    }

    if (format_align->right != 0) {
        display->getstringsize((unsigned char *)format_align->right,
                                &right_width, &string_height);
    }

    if (format_align->center != 0) {
        display->getstringsize((unsigned char *)format_align->center,
                                &center_width, &string_height);
    }

    right_xpos = (viewport_width - right_width);
    center_xpos = (viewport_width - center_width) / 2;

    scroll_width = viewport_width;

    /* Checks for overlapping strings.
        If needed the overlapping strings will be merged, separated by a
        space */

    /* CASE 1: left and centered string overlap */
    /* there is a left string, need to merge left and center */
    if (center_width != 0)
    {
        if (left_width != 0 && left_width + space_width > center_xpos) {
            /* replace the former separator '\0' of left and
                center string with a space */
            *(--format_align->center) = ' ';
            /* calculate the new width and position of the merged string */
            left_width = left_width + space_width + center_width;
            /* there is no centered string anymore */
            center_width = 0;
        }
        /* there is no left string, move center to left */
        else if (left_width == 0 && center_xpos < 0) {
            /* move the center string to the left string */
            format_align->left = format_align->center;
            /* calculate the new width and position of the string */
            left_width = center_width;
            /* there is no centered string anymore */
            center_width = 0;
        }
    } /*(center_width != 0)*/

    /* CASE 2: centered and right string overlap */
    /* there is a right string, need to merge center and right */
    if (center_width != 0)
    {
        int center_left_x = center_xpos + center_width;
        if (right_width != 0 && center_left_x + space_width > right_xpos) {
            /* replace the former separator '\0' of center and
                right string with a space */
            *(--format_align->right) = ' ';
            /* move the center string to the right after merge */
            format_align->right = format_align->center;
            /* calculate the new width and position of the merged string */
            right_width = center_width + space_width + right_width;
            right_xpos = (viewport_width - right_width);
            /* there is no centered string anymore */
            center_width = 0;
        }
        /* there is no right string, move center to right */
        else if (right_width == 0 && center_left_x > right_xpos) {
            /* move the center string to the right string */
            format_align->right = format_align->center;
            /* calculate the new width and position of the string */
            right_width = center_width;
            right_xpos = (viewport_width - right_width);
            /* there is no centered string anymore */
            center_width = 0;
        }
    } /*(center_width != 0)*/

    /* CASE 3: left and right overlap
        There is no center string anymore, either there never
        was one or it has been merged in case 1 or 2 */
    /* there is a left string, need to merge left and right */
    if (center_width == 0 && right_width != 0)
    {
        if (left_width != 0 && left_width + space_width > right_xpos) {
            /* replace the former separator '\0' of left and
                right string with a space */
            *(--format_align->right) = ' ';
            /* calculate the new width and position of the string */
            left_width = left_width + space_width + right_width;
            /* there is no right string anymore */
            right_width = 0;
        }
        /* there is no left string, move right to left */
        else if (left_width == 0 && right_xpos < 0) {
            /* move the right string to the left string */
            format_align->left = format_align->right;
            /* calculate the new width and position of the string */
            left_width = right_width;
            /* there is no right string anymore */
            right_width = 0;
        }
    } /* (center_width == 0 && right_width != 0)*/

    if (scroll && ((left_width > scroll_width) ||
                   (center_width > scroll_width) ||
                   (right_width > scroll_width)))
    {
        /* strings can be as large as MAX_LINE which exceeds put_lines()
         * limit for inline strings. Use $t to avoid truncation */
        linedes->scroll = true;
        display->put_line(0, line * string_height, linedes, "$t", format_align->left);
    }
    else
    {
        linedes->scroll = false;
        /* clear the line first */
        display->set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
        display->fillrect(0, line*string_height, viewport_width, string_height);
        display->set_drawmode(DRMODE_SOLID);

        /* Nasty hack: we output an empty scrolling string,
        which will reset the scroller for that line */
        display->puts_scroll(0, line, (unsigned char *)"");
        line *= string_height;
        center_xpos = (viewport_width-center_width)/2;
        right_xpos = viewport_width-right_width;
        /* print aligned strings. print whole line at once so that %Vs works
         * across the full viewport width */
        char *left   = format_align->left   ?: "";
        char *center = format_align->center ?: "";
        char *right  = format_align->right  ?: "";

        display->put_line(0, line, linedes, "$t$*s$t$*s$t", left_width == 0 ? "" : left ,
                center_xpos - left_width, center_width == 0 ? "" : center,
                right_xpos - center_xpos - center_width, right_width == 0 ? "" : right);
    }
}

void draw_peakmeters(struct gui_wps *gwps, int line_number,
                     struct viewport *viewport)
{
    struct wps_data *data = gwps->data;
    if (!data->peak_meter_enabled)
    {
        peak_meter_enable(false);
    }
    else
    {
        int h = font_get(viewport->font)->height;
        int peak_meter_y = line_number * h;

        /* The user might decide to have the peak meter in the last
            line so that it is only displayed if no status bar is
            visible. If so we neither want do draw nor enable the
            peak meter. */
        if (peak_meter_y + h <= viewport->y+viewport->height) {
            peak_meter_enable(true);
            peak_meter_screen(gwps->display, 0, peak_meter_y,
                              MIN(h, viewport->y+viewport->height - peak_meter_y));
        }
    }
}

#ifdef HAVE_ALBUMART
/* Draw the album art bitmap from the given handle ID onto the given WPS.
   Call with clear = true to clear the bitmap instead of drawing it. */
void draw_album_art(struct gui_wps *gwps, struct skin_albumart *aa,
                    int handle_id, bool clear)
{
    if (!gwps || !gwps->data || !gwps->display || !aa || handle_id < 0)
        return;

    struct bitmap *bmp;
    if (bufgetdata(handle_id, 0, (void *)&bmp) <= 0)
        return;

    short x = aa->x;
    short y = aa->y;
    short width = bmp->width;
    short height = bmp->height;

    if (aa->width > 0)
    {
        /* Crop if the bitmap is too wide */
        width = MIN(bmp->width, aa->width);

        /* Align */
        if (aa->xalign & WPS_ALBUMART_ALIGN_RIGHT)
            x += aa->width - width;
        else if (aa->xalign & WPS_ALBUMART_ALIGN_CENTER)
            x += (aa->width - width) / 2;
    }

    if (aa->height > 0)
    {
        /* Crop if the bitmap is too high */
        height = MIN(bmp->height, aa->height);

        /* Align */
        if (aa->yalign & WPS_ALBUMART_ALIGN_BOTTOM)
            y += aa->height - height;
        else if (aa->yalign & WPS_ALBUMART_ALIGN_CENTER)
            y += (aa->height - height) / 2;
    }

    if (!clear)
    {
        /* Draw the bitmap */
        draw_cover(gwps, aa, (fb_data*)bmp->data,
                   STRIDE(gwps->display->screen_type, bmp->width, bmp->height),
                   bmp->width, bmp->height, x, y, width, height);
#ifdef HAVE_LCD_INVERT
        if (global_settings.invert) {
            gwps->display->set_drawmode(DRMODE_COMPLEMENT);
            gwps->display->fillrect(x, y, width, height);
            gwps->display->set_drawmode(DRMODE_SOLID);
        }
#endif
    }
    else
    {
        /* Clear the bitmap */
        gwps->display->set_drawmode(DRMODE_SOLID|DRMODE_INVERSEVID);
        gwps->display->fillrect(x, y, width, height);
        gwps->display->set_drawmode(DRMODE_SOLID);
        clear_cover_reflection(gwps, aa, x, y, width, height);
    }
}
#endif

bool skin_has_sbs(struct gui_wps *gwps)
{
    struct wps_data *data = gwps->data;

    bool draw = false;
    if (data->wps_sb_tag)
        draw = data->show_sb_on_wps;
    else if (statusbar_position(gwps->display->screen_type) != STATUSBAR_OFF)
        draw = true;
    return draw;
}

/* Enter button loop updating peak meter at a high refresh rate */
int skin_wait_for_action(enum skinnable_screens skin, int context, int timeout)
{
    /* Skip updates if peak meter disabled. */
    bool peak_meter_enabled = false;
    FOR_NB_SCREENS(i)
        peak_meter_enabled |= skin_get_gwps(skin, i)->data->peak_meter_enabled;
    if (!peak_meter_enabled && !volume_pending)
        return get_action(context, timeout);

    long next_refresh = current_tick;
    long timeout_tick = current_tick + timeout;

    while (true)
    {
        int action = get_action(context, TIMEOUT_NOBLOCK);
        if (action != ACTION_NONE || !(TIME_BEFORE(current_tick, timeout_tick)))
            return action;

        peak_meter_peek(); /* Read peak values */
        sleep(0); /* Sleep until end of current tick. */

        /* a change of the volume, at its frame's time; not drawn after a
           while, the bar isn't there */
        if (volume_pending)
        {
            if (TIME_AFTER(current_tick, volume_changed_tick + HZ/10))
                volume_pending = false;
            else if (!TIME_BEFORE(current_tick, volume_shown_tick + VOLUME_FRAME_TICKS))
                FOR_NB_SCREENS(i)
                    skin_update_volume(skin, i);
        }
        if (!peak_meter_enabled && !volume_pending)
            return get_action(context, TIME_BEFORE(current_tick, timeout_tick)
                                       ? timeout_tick - current_tick : 0);

        if (!peak_meter_enabled || TIME_BEFORE(current_tick, next_refresh))
            continue;

        FOR_NB_SCREENS(i)
            if (skin_get_gwps(skin, i)->data->peak_meter_enabled)
                skin_update(skin, i, SKIN_REFRESH_PEAK_METER);

        next_refresh += HZ/PEAK_METER_FPS;
    }
}
