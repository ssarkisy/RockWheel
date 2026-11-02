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

void skin_render(struct gui_wps *gwps, unsigned refresh_mode);

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

    skin_render(gwps, skin_do_full_update(skin, screen) ?
                        SKIN_REFRESH_ALL : update_type);
    skin_mark_dirty(screen);
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
        void *pixels = cover_palette_bar(bm);
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

        if (bm->alpha_offset > 0)
        {
            /* its alpha channel comes after its pixels, so a copy of all of it: the
               theme's only such image is the small knob of the scrubber */
            size_t room;
            unsigned char *copy = cover_palette_scratch(&room);
            const size_t size = bm->alpha_offset + (bm->width + 1) / 2 * bm->height;
            if (size <= room)
            {
                memcpy(copy, bm->data, size);
                for (int i = 0; i < bm->height; i++)
                    cover_palette_tint_line((const fb_data *)bm->data + i * bm->width,
                                            (fb_data *)copy + i * bm->width, bm->width, top);
                line_bm.data = copy;
                gwps->display->bmp_part(&line_bm, src_x, src_y, x, y, width, height);
                return;
            }
            gwps->display->bmp_part(bm, src_x, src_y, x, y, width, height);
            return;
        }

        line_bm.data = (void *)line;
        line_bm.width = width;
        line_bm.height = 1;
        for (int i = 0; i < height; i++, row += bm->width)
        {
            cover_palette_tint_line(row, line, width, top + i);
            gwps->display->bmp_part(&line_bm, 0, 0, x, y + i, width, 1);
        }
        return;
    }
#endif
    gwps->display->bmp_part(bm, src_x, src_y, x, y, width, height);
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
#if defined(HAVE_PERCEPTUAL_VOLUME) || defined(HAVE_TOUCHSCREEN)
        length = 1000;
        end = to_normalized_volume(global_status.volume, minvol, maxvol, length);
#else
        length = maxvol - minvol;
        end = global_status.volume - minvol;
#endif
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
            const int shade = RGB_UNPACK_GREEN(FB_UNPACK_SCALAR_LCD(row[x])) >> 2;
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
        draw_bmp_part(gwps, &img->bm, 0, img->subimage_height * subimage,
                      img->x, img->y, img->bm.width, img->subimage_height);
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
    int handle;
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

static void draw_kept_album_art(struct gui_wps *gwps, struct skin_albumart *aa,
                                int index)
{
    const int width = kept_art[index].width, height = kept_art[index].height;
    int x = aa->x, y = aa->y;

    if (aa->xalign & WPS_ALBUMART_ALIGN_RIGHT)
        x += aa->width - width;
    else if (aa->xalign & WPS_ALBUMART_ALIGN_CENTER)
        x += (aa->width - width) / 2;
    if (aa->yalign & WPS_ALBUMART_ALIGN_BOTTOM)
        y += aa->height - height;
    else if (aa->yalign & WPS_ALBUMART_ALIGN_CENTER)
        y += (aa->height - height) / 2;

    gwps->display->bitmap_part(kept_art[index].pixels, 0, 0,
                               STRIDE(gwps->display->screen_type, width, height),
                               x, y, width, height);
}

/* What %?C says: it comes before the %Cd, and a theme may change what it shows by
   it, so going without a cover has to start here already */
bool skin_art_found(const struct wps_data *data, int handle)
{
    if (data != skin_get_gwps(WPS, SCREEN_MAIN)->data)
        return handle >= 0;

    art_seen(0, handle);
    return handle >= 0 || art_is_kept(0);
}

int skin_art_handle(const struct wps_data *data, int index, int handle)
{
    if (data != skin_get_gwps(WPS, SCREEN_MAIN)->data)
        return handle;

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
        gwps->display->bitmap_part((fb_data*)bmp->data, 0, 0,
                                    STRIDE(gwps->display->screen_type,
                                        bmp->width, bmp->height),
                                   x, y, width, height);
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
    if (!peak_meter_enabled)
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

        if (TIME_BEFORE(current_tick, next_refresh))
            continue;

        FOR_NB_SCREENS(i)
            if (skin_get_gwps(skin, i)->data->peak_meter_enabled)
                skin_update(skin, i, SKIN_REFRESH_PEAK_METER);

        next_refresh += HZ/PEAK_METER_FPS;
    }
}
