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

/* The header of an album's songs: its cover at the left, and beside it its
 * name, in a line or two, its album artist and "2019 · 16 songs · 58 min", in
 * the fonts of the theme's Now Playing. It takes the list's first rows, three
 * or four, which none can select, and scrolls away with the songs, as iOS's
 * does; the list's title names the album once it's gone (tree.c). A
 * playlist's songs have one too (library_playlists.c): its name and "24 songs
 * · 1 h 32 min" beside a collage of the covers of its first four albums with
 * one, chosen as its index is written, in two or three rows, as the rows'
 * height takes.
 *
 * The cover is only ever one made already: Finder's frame of the first song
 * (ArtworkDB), or for an album copied outside Finder the one Updating Library
 * made (covers_frames.dat), as Now Playing shows them. Read into a buffer of
 * its own here, 60 by 60, nothing is allocated for it, nor while music plays;
 * the album opened again shows it without a read. The album playing has it
 * from the playback's buffer, without the disk. It's found and read on a
 * thread of its own while the screen is used: a disk asleep takes 0.6 s to
 * spin up, which the wheel and Select don't wait for. The screen shows at
 * once with a note in the cover's place, and the cover comes as it's read;
 * from the buffer or a disk that's on, before the screen, the note not seen.
 * A collage is read the same way, each cover into its cell of that buffer,
 * shown once all are there. */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "font.h"
#include "lcd.h"
#include "lang.h"
#include "language.h"
#include "misc.h"
#include "settings.h"
#include "storage.h"
#include "audio.h"
#include "playback.h"
#include "rbpaths.h"
#include "tagcache.h"
#include "tagtree.h"
#include "itunesdb.h"
#include "album_header.h"
#include "list_covers.h"
#include "gui/cover_caution.h"
#include "gui/skin_engine/menu_cover.h"

#define COVER_MAX    60
/* the cover's side: the theme's, 60 without one of its own */
#define COVER_SIZE   (ui_metric(UI_METRIC_HEADER_COVER_SIZE) > 0 \
                      ? MIN(ui_metric(UI_METRIC_HEADER_COVER_SIZE), COVER_MAX) \
                      : COVER_MAX)
#define COVER_TOP    ui_metric(UI_METRIC_HEADER_COVER_TOP)
#define COVER_GAP    ui_metric(UI_METRIC_HEADER_COVER_GAP) /* from the cover to the texts */
#define TEXT_MARGIN  ui_metric(UI_METRIC_HEADER_TEXT_MARGIN) /* the texts' at the other end, as a setting's value's */
#define NAME_TOP     ui_metric(UI_METRIC_HEADER_NAME_TOP)
#define NAME_LINES   2
/* a name's two lines closer than its font's cells, which have room above
   the capitals; the artist's line just under it, and the year's */
#define NAME_LEADING ui_metric(UI_METRIC_HEADER_NAME_LEADING)
#define ARTIST_GAP   ui_metric(UI_METRIC_HEADER_ARTIST_GAP)
#define COUNTS_GAP   ui_metric(UI_METRIC_HEADER_COUNTS_GAP)
#define HEADER_FOOT  ui_metric(UI_METRIC_HEADER_FOOT) /* below the lowest of the cover and the texts */
#define TEXT_BYTES   192

static struct {
    int rows;  /* of the list, 0 without a header */
    int slack; /* the rows' height over what the cover and the texts take */
    bool playlist; /* a playlist's: its name in a line and its counts, by the
                      cover's middle, no artist */
    int lines; /* the name's */
    char name[NAME_LINES][TEXT_BYTES];
    char artist[TEXT_BYTES];
    char counts[TEXT_BYTES];
    signed char name_form, artist_form; /* of the whole, for their parts */
} header;

#ifdef ITUNESDB_HAVE_ART
/* The cover, of the album these seeks name in the database of this commit,
   or none there as width 0: the note in its place. Its reader fills the pixels
   while width is 0, and only the UI draws them; the two threads are on one
   core, and none of their changes here has a yield in the middle */
static struct {
    fb_data pixels[COVER_MAX * COVER_MAX];
    volatile int width, height;
    volatile int reason; /* none: why its picture isn't shown (itunesdb.h) */
    bool known;
    int32_t album, albumartist;
    int commit;
    /* the read asked for: of the album of this song, from the playback's
       buffer if it's playing; numbered, as another album opened, or the
       covers made again, leave one being read unshown */
    int32_t song;
    bool playing;
    /* or a playlist's collage, of the covers of these songs' albums, -1 past
       them, side pixels a side; read into cells, each a part of the square
       its pixels, packed one after another in pixels, and ok if its cover
       was read: those the reader found, how many, and their places */
    bool is_collage;
    int32_t collage[ALBUM_HEADER_COLLAGE];
    int side;
    int cells;
    struct {
        short x, y, w, h;
        bool ok;
    } cell[ALBUM_HEADER_COLLAGE];
    volatile unsigned wanted;
    volatile bool to_read, reading, came;
} cover;

/* The reader's thread, only while it reads, and its stack, 4 KB: its calls go
   2.75 KB deep at most on the Video, 2.7 KB on the iPod photo, by their
   direct calls' frames in the firmware's code, in a collage's lookup that
   reads a path from the disk: read_collage()'s frame of 416 B, its four
   covers' places, kept off the album's cover's way (2.3 KB) and the menus'
   cover's (2.2 KB, in its JPEG decode) */
static long reader_stack[(DEFAULT_STACK_SIZE + 0xc00) / sizeof(long)];
/* how long the screen waits for a read that costs no spin-up, from the buffer
   or a disk that's on: about 20-30 ms, which are then without the note */
#define COVER_WAIT (HZ / 20)
#endif

#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
/* The note on the tile in the cover's place, the menus' panel's, in Rockbox'
   alpha format (4 bits from 0 opaque to 15 transparent, low nibble first,
   rows run on without padding): made by themes/tools/make_list_glyphs.py */
#include "gui/neo_glyphs.h"
#include "gui/modern_glyphs.h"
#include "gui/dieter_glyphs.h"
#define NOTE_W 29
#define NOTE_H 35
static const unsigned char note_alpha[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xcf, 0x58, 0xf5, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x7b, 0x14, 0x00, 0x40, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xbe, 0x37, 0x00, 0x00, 0x00, 0x00, 0xf4, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xef, 0x7a, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0xff, 0xff,
    0xff, 0xff, 0x9e, 0x26, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf4, 0xff, 0xff, 0xff, 0xbf, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x40, 0xff, 0xff, 0xff, 0xff, 0x0b, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xf4, 0xff, 0xff, 0xff, 0xbf, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x06, 0x40, 0xff, 0xff, 0xff, 0xff,
    0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0xea, 0xff, 0x00, 0xf4, 0xff,
    0xff, 0xff, 0xbf, 0x00, 0x00, 0x00, 0x40, 0xb7, 0xfe, 0xff, 0xff, 0x0f,
    0x40, 0xff, 0xff, 0xff, 0xff, 0x0b, 0x10, 0x84, 0xfc, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x00, 0xf4, 0xff, 0xff, 0xff, 0xbf, 0x00, 0xf6, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0x0f, 0x40, 0xff, 0xff, 0xff, 0xff, 0x0b, 0x60,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0xf4, 0xff, 0xff, 0xff,
    0xbf, 0x00, 0xf6, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x40, 0xff,
    0xff, 0xff, 0xff, 0x0b, 0x60, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0xf4, 0xff, 0xff, 0xff, 0xbf, 0x00, 0xf6, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x0f, 0x40, 0xff, 0xff, 0xff, 0xff, 0x0b, 0x60, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0xf4, 0xff, 0xff, 0xff, 0xbf, 0x00,
    0xf6, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x40, 0xff, 0xff, 0xff,
    0xff, 0x0b, 0x60, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0xf4,
    0xff, 0xff, 0xff, 0xbf, 0x00, 0xf6, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x0f, 0x40, 0xff, 0xff, 0xff, 0xff, 0x0b, 0x60, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0x00, 0xf4, 0xff, 0xff, 0xff, 0xbf, 0x00, 0xf6, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x40, 0xff, 0xff, 0xff, 0xff, 0x0b,
    0x60, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0xf4, 0xff, 0xff,
    0xff, 0xbf, 0x00, 0xf6, 0xff, 0xff, 0xff, 0xff, 0xef, 0xab, 0x0c, 0x40,
    0xff, 0xff, 0xff, 0xde, 0x0b, 0x60, 0xff, 0xff, 0xff, 0xff, 0x4c, 0x00,
    0x00, 0x00, 0xf4, 0xff, 0x6d, 0x01, 0x00, 0x00, 0xf6, 0xff, 0xff, 0xff,
    0x07, 0x00, 0x00, 0x00, 0x40, 0xff, 0x08, 0x00, 0x00, 0x00, 0x60, 0xff,
    0xff, 0xff, 0x07, 0x00, 0x00, 0x00, 0x00, 0xf4, 0x07, 0x00, 0x00, 0x00,
    0x00, 0xf6, 0xff, 0xff, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x40, 0x0b, 0x00,
    0x00, 0x00, 0x00, 0x60, 0xff, 0xff, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x56, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0xff, 0xff, 0x05, 0x00, 0x00,
    0x00, 0x00, 0xc0, 0x02, 0x00, 0x00, 0x00, 0x00, 0xe0, 0xff, 0xff, 0x7f,
    0x00, 0x00, 0x00, 0x00, 0x90, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x90, 0xff,
    0xff, 0xff, 0x1d, 0x00, 0x00, 0x00, 0x90, 0xff, 0x0a, 0x00, 0x00, 0x00,
    0x90, 0xff, 0xff, 0xff, 0xff, 0x5d, 0x00, 0x20, 0xd7, 0xff, 0xff, 0x19,
    0x00, 0x00, 0xc5, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xed, 0xff, 0xff,
    0xff, 0xff, 0xcf, 0xba, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff,
};
extern void lcd_alpha_bitmap_part(const unsigned char *src, int src_x,
                                  int src_y, int stride, int x, int y,
                                  int width, int height);
#endif

/* Now Playing's bold fonts, which its skin keeps loaded from boot, found by
   their files. The list's with a theme without them */
static int artist_font(int list_font)
{
    const int font = font_find(ui_font_file(UI_FONT_HEADER_ARTIST));
    return font < 0 ? list_font : font;
}

/* the name's: the theme's for it, which its skin keeps loaded, or the list's */
static int name_font(int list_font)
{
    const char *const file = ui_font_file(UI_FONT_HEADER_NAME);
    const int font = file ? font_find(file) : -1;
    return font < 0 ? list_font : font;
}

static int counts_font(int list_font)
{
    const int font = font_find(ui_font_file(UI_FONT_HEADER_COUNTS));
    return font < 0 ? list_font : font;
}

/* where the texts go, from the header's top, by their fonts: the name's line
   or two, the album artist's, the year's; and the foot of the lowest of them
   and the cover */
struct layout {
    int name[NAME_LINES];
    int artist, counts, foot;
};

static void lay_out(int lines, int name_font, int list_font, struct layout *l)
{
    const int name_h = font_get(name_font)->height;
    if (header.playlist)
    {
        /* the name and the counts as one, by the cover's middle */
        const int counts_h = font_get(counts_font(list_font))->height;
        const int top = COVER_TOP
                        + (COVER_SIZE - (name_h + COUNTS_GAP + counts_h)) / 2;
        l->name[0] = l->name[1] = top;
        l->artist = l->counts = top + name_h + COUNTS_GAP;
        l->foot = MAX(COVER_TOP + COVER_SIZE, l->counts + counts_h);
        return;
    }
    for (int k = 0; k < NAME_LINES; k++)
        l->name[k] = NAME_TOP + k * (name_h - NAME_LEADING);
    l->artist = l->name[lines - 1] + name_h + ARTIST_GAP;
    l->counts = l->artist + font_get(artist_font(list_font))->height + COUNTS_GAP;
    l->foot = MAX(COVER_TOP + COVER_SIZE,
                  l->counts + (int)font_get(counts_font(list_font))->height);
}

/* len bytes of text into buf, or as many as fit in width with "…" after
   them: its letters whole, and not past buf's end */
static void fit_text(char *buf, size_t size, const char *text, size_t len,
                     int font, int width)
{
    static const char ellipsis[] = "\xe2\x80\xa6"; /* U+2026 */
    bool cut = len > size - sizeof(ellipsis);
    size_t n = cut ? size - sizeof(ellipsis) : len;
    while (n > 0 && (text[n] & 0xc0) == 0x80)
        n--;
    if (!cut && font_getstringnsize(text, n, NULL, NULL, font) <= width)
    {
        memcpy(buf, text, n);
        buf[n] = '\0';
        return;
    }
    const int ellipsis_w = font_getstringsize(ellipsis, NULL, NULL, font);
    while (n > 0 && font_getstringnsize(text, n, NULL, NULL, font) + ellipsis_w > width)
    {
        do
            n--;
        while (n > 0 && (text[n] & 0xc0) == 0x80);
    }
    while (n > 0 && text[n - 1] == ' ')
        n--;
    memcpy(buf, text, n);
    memcpy(buf + n, ellipsis, sizeof(ellipsis));
}

/* The name in a line, or in two broken between words as a hint's lines are,
   the second cut short if it's longer still: how many */
static int wrap_name(const char *name, int font, int width)
{
    const char *end = list_line_end(name, font, width);
    const char *rest = end;
    while (*rest == ' ')
        rest++;
    if (!*rest)
    {
        fit_text(header.name[0], TEXT_BYTES, name, strlen(name), font, width);
        return 1;
    }
    while (end > name && end[-1] == ' ')
        end--;
    fit_text(header.name[0], TEXT_BYTES, name, end - name, font, width);
    fit_text(header.name[1], TEXT_BYTES, rest, strlen(rest), font, width);
    return 2;
}

#ifdef ITUNESDB_HAVE_ART
/* Where the cover of the album of this song is, as Now Playing finds a song's:
   Finder's of the song, or the one made of its own picture, as of a synced
   song Finder had no cover of, or else the album's of ours, by its name and
   album artist as the database has them. The song's path may come from the
   disk, where it's in neither the database's memory nor the dircache. 1
   found, 0 none, -1 not known, as with the database busy or the path not
   read. What it reads into is the reader's, which reads one cover at a
   time: off its stack, 1.5 KB; held by a lock, as a playlist's index looks
   for its albums' covers too, on the UI's thread or the database's
   (library_playlists.c). It may wait: for the lock's other holder, for a
   commit that holds the database (tagcache_search()), and for such a path's
   read */
static struct {
    struct tagcache_search tcs;
    char path[MAX_PATH], name[MAX_PATH], artist[MAX_PATH];
    struct mutex lock;
    bool lock_ready;
} finding;

int album_header_find_art(int32_t song, struct itunesdb_art *art)
{
    struct tagcache_search *tcs = &finding.tcs;
    art->reason = 0;
    /* made at its first use, with no yield between the test and the making */
    if (!finding.lock_ready)
    {
        mutex_init(&finding.lock);
        finding.lock_ready = true;
    }
    mutex_lock(&finding.lock);
    if (!tagcache_search(tcs, tag_title))
    {
        mutex_unlock(&finding.lock);
        return -1;
    }
    int found = -1;
    if (tagcache_retrieve(tcs, song, tag_filename, finding.path, MAX_PATH))
    {
        if (itunesdb_find_track_art(NULL, finding.path, art))
            found = 1;
        else if (tagcache_retrieve(tcs, song, tag_album, finding.name, MAX_PATH)
                 && tagcache_retrieve(tcs, song, tag_albumartist, finding.artist,
                                      MAX_PATH))
            found = itunesdb_find_album_art(finding.name, finding.artist,
                                            finding.path, art) ? 1 : 0;
    }
    tagcache_search_finish(tcs);
    mutex_unlock(&finding.lock);
    return found;
}

int album_header_song_cover(int32_t song)
{
    struct itunesdb_art art;
    return album_header_find_art(song, &art);
}

/* the song of the database entry is of the album these seeks name: tagcache_idx,
   0 for none */
static bool of_album(long tagcache_idx, int32_t album, int32_t albumartist)
{
    return tagcache_idx > 0
           && tagcache_sort_key(tagcache_idx - 1, tag_album) == album
           && tagcache_sort_key(tagcache_idx - 1, tag_albumartist) == albumartist;
}

/* the song playing is of the album, its cover in the playback's buffer: on the
   UI's thread, for audio_current_track() */
static bool album_playing(const struct tagtree_album *album)
{
    const struct mp3entry *id3 = audio_current_track();
    return (audio_status() & AUDIO_STATUS_PLAY) && id3
           && of_album(id3->tagcache_idx, album->album, album->albumartist);
}

/* A collage's cells for so many covers, in the square of side: 4 a quarter
   each; 3 the first's middle half at the left and two quarters at the right;
   2 the middle halves of both; 1 the cover whole */
static void collage_cells(int n, int side)
{
    const int h = side / 2, r = side - h;
    static const signed char at[ALBUM_HEADER_COLLAGE + 1][ALBUM_HEADER_COLLAGE][4] = {
        /* x, y, w, h: 0 for none, 1 for half the side, 2 for the rest of it,
           3 for the whole */
        { { 0 } },
        { { 0, 0, 3, 3 } },
        { { 0, 0, 1, 3 }, { 1, 0, 2, 3 } },
        { { 0, 0, 1, 3 }, { 1, 0, 2, 1 }, { 1, 1, 2, 2 } },
        { { 0, 0, 1, 1 }, { 1, 0, 2, 1 }, { 0, 1, 1, 2 }, { 1, 1, 2, 2 } },
    };
    const int size[4] = { 0, h, r, side };
    for (int k = 0; k < n; k++)
    {
        cover.cell[k].x = at[n][k][0] ? h : 0;
        cover.cell[k].y = at[n][k][1] ? h : 0;
        cover.cell[k].w = size[(int)at[n][k][2]];
        cover.cell[k].h = size[(int)at[n][k][3]];
        cover.cell[k].ok = false;
    }
}

/* The collage of the covers of these songs' albums: those found, each read
   into its cell, the cells packed in the pixels, as long as it's still the
   one wanted. Shown once all are read; one that couldn't be, as with a disk's
   error, a tile, and the collage read again the next time */
static void NO_INLINE read_collage(unsigned wanted, const int32_t *songs,
                                   int side)
{
    struct itunesdb_art art[ALBUM_HEADER_COLLAGE];
    int n = 0;
    bool known = true;
    for (int k = 0; k < ALBUM_HEADER_COLLAGE && songs[k] >= 0; k++)
    {
        const int found = album_header_find_art(songs[k], &art[n]);
        if (found > 0)
            n++;
        else if (found < 0)
            known = false;
    }
    if (wanted != cover.wanted)
        return;
    collage_cells(n, side);
    fb_data *pixels = cover.pixels;
    for (int k = 0; k < n && wanted == cover.wanted; k++)
    {
        struct bitmap bm;
        memset(&bm, 0, sizeof(bm));
        bm.width = cover.cell[k].w;
        bm.height = cover.cell[k].h;
        bm.data = (unsigned char *)pixels;
        const int bytes = bm.width * bm.height * sizeof(fb_data);
        cover.cell[k].ok =
            itunesdb_load_small_art_filled(&art[k], &bm, sizeof(bm) + bytes) >= 0
            && bm.width == cover.cell[k].w && bm.height == cover.cell[k].h;
        known = known && cover.cell[k].ok;
        pixels += cover.cell[k].w * cover.cell[k].h;
    }
    if (wanted == cover.wanted)
    {
        cover.reason = 0;
        cover.cells = n;
        cover.height = side;
        cover.width = n > 0 ? side : 0;
        cover.known = known;
        cover.came = true;
    }
}

/* The reader: each cover asked for, until none is, shown if it's still the
   one wanted; then its thread ends. One it couldn't look for or read, as
   with USB coming or a disk's error, is looked for again the next time the
   album opens; only an album without one is kept as such. The covers of a
   list's rows too, those a frame lacked and those next to them as the wheel
   rests, and the map they're read by made again as asked (list_covers.c),
   after the header's, which its screen waits for.
   Behind the UI, which has the CPU at each of a read's yields before it, as
   a wheel's step comes; the header's as the UI, which waits for it. Last,
   the menus' cover from its song's own picture (menu_cover.c) */
static void cover_reader(void)
{
    while (cover.to_read || list_covers_to_read() || menu_cover_to_read())
    {
        if (!cover.to_read)
        {
            if (list_covers_to_read())
                list_covers_read();
            else
                menu_cover_read();
            continue;
        }
#ifdef HAVE_PRIORITY_SCHEDULING
        thread_set_priority(thread_self(), PRIORITY_USER_INTERFACE);
#endif
        cover.to_read = false;
        const unsigned wanted = cover.wanted;
        if (cover.is_collage)
        {
            int32_t songs[ALBUM_HEADER_COLLAGE];
            memcpy(songs, cover.collage, sizeof(songs));
            read_collage(wanted, songs, cover.side);
#ifdef HAVE_PRIORITY_SCHEDULING
            thread_set_priority(thread_self(), PRIORITY_BACKGROUND);
#endif
            continue;
        }
        const int32_t song = cover.song;
        const int32_t album = cover.album, albumartist = cover.albumartist;
        const bool playing = cover.playing;
        struct itunesdb_art art;
        struct bitmap bm;
        memset(&bm, 0, sizeof(bm));
        bm.width = COVER_SIZE;
        bm.height = COVER_SIZE;
        bm.data = (unsigned char *)cover.pixels;
        const int room = sizeof(bm) + sizeof(cover.pixels);
        int size = -1;
        const int found = album_header_find_art(song, &art);
        if (found > 0)
        {
            /* As large as Finder's in the buffer, as the menus' panel takes
               it, if it's still this album's: the path looked for above may
               have waited for the disk, and the music gone on to another.
               Taken with the track it's of, and scaled, without a yield, as
               the buffer moves */
            long playing_idx = 0;
            const struct bitmap *buffered =
                playing ? playback_current_largest_art_of(&playing_idx) : NULL;
            if (buffered && of_album(playing_idx, album, albumartist)
                && buffered->width >= art.width && buffered->height >= art.height)
                size = itunesdb_scale_small_art(buffered, &bm, room);
            if (size < 0)
                size = itunesdb_load_small_art(&art, &bm, room);
        }
        if (wanted == cover.wanted)
        {
            cover.reason = found == 0 ? art.reason : 0;
            cover.width = size < 0 ? 0 : bm.width;
            cover.height = bm.height;
            cover.known = found == 0 || size >= 0;
            cover.came = true;
        }
#ifdef HAVE_PRIORITY_SCHEDULING
        thread_set_priority(thread_self(), PRIORITY_BACKGROUND);
#endif
    }
    cover.reading = false;
    thread_exit();
}

static bool reader_start(bool give_slot)
{
    if (cover.reading)
        return true;
    cover.reading = true;
    if (give_slot)
        menu_cover_give_slot();
    if (!create_thread(cover_reader, reader_stack, sizeof(reader_stack), 0,
                       "album cover" IF_PRIO(, PRIORITY_BACKGROUND)
                       IF_COP(, CPU)))
        cover.reading = false;
    return cover.reading;
}

bool album_header_reader_start(void)
{
    /* a list's covers before the menus' */
    menu_cover_read_cancel();
    return reader_start(true);
}

bool album_header_reader_start_beside(void)
{
    return reader_start(false);
}

/* The album's cover, unless it's the one read last: asked of the reader, its
   thread started if it isn't on. Waited for only when it costs no spin-up */
static void find_cover(const struct tagtree_album *album)
{
    const int commit = tagcache_get_commitid();
    if (cover.known && !cover.is_collage && cover.album == album->album
        && cover.albumartist == album->albumartist && cover.commit == commit)
        return;
    cover.known = true;
    cover.is_collage = false;
    cover.album = album->album;
    cover.albumartist = album->albumartist;
    cover.commit = commit;
    cover.width = 0;
    cover.reason = 0;
    cover.song = album->first;
    cover.playing = album_playing(album);
    cover.wanted++;
    cover.to_read = true;
    cover.came = false;
    if (!album_header_reader_start())
    {
        cover.to_read = cover.known = false;
        return;
    }
    /* storage_disk_is_active() says so while it spins up too: then the note
       after a frame or so, as with the disk asleep */
    if (cover.playing || storage_disk_is_active())
    {
        const long end = current_tick + COVER_WAIT;
        while (!cover.came && cover.reading && TIME_BEFORE(current_tick, end))
            sleep(1);
        /* in the screen's first frame */
        cover.came = false;
    }
}

/* A playlist's collage, unless it's the one read last: asked of the reader,
   as an album's cover is; the note for none, with nothing to read */
static void find_collage(const int32_t *songs)
{
    const int commit = tagcache_get_commitid();
    const int side = COVER_SIZE;
    if (cover.known && cover.is_collage && cover.commit == commit
        && cover.side == side
        && !memcmp(cover.collage, songs, sizeof(cover.collage)))
        return;
    cover.known = true;
    cover.is_collage = true;
    memcpy(cover.collage, songs, sizeof(cover.collage));
    cover.side = side;
    cover.commit = commit;
    cover.album = cover.albumartist = -1;
    cover.playing = false;
    cover.width = 0;
    cover.cells = 0;
    cover.reason = 0;
    /* one being read before isn't shown */
    cover.wanted++;
    cover.came = false;
    cover.to_read = songs[0] >= 0;
    if (!cover.to_read)
        return;
    if (!album_header_reader_start())
    {
        cover.to_read = cover.known = false;
        return;
    }
    /* four reads, each from a disk that's on: the note first if they take
       longer than an album's */
    if (storage_disk_is_active())
    {
        const long end = current_tick + COVER_WAIT;
        while (!cover.came && cover.reading && TIME_BEFORE(current_tick, end))
            sleep(1);
        cover.came = false;
    }
}
#else
int album_header_song_cover(int32_t song)
{
    (void)song;
    return 0;
}
#endif /* ITUNESDB_HAVE_ART */

/* "16 songs · 58 min", after "2019 · " with a year; the minutes left out if
   0, as for songs without their lengths */
static void counts_text(char *buf, size_t size, int year, int songs,
                        long minutes)
{
    size_t len = 0;
    buf[0] = '\0';
    if (year > 0)
        len = MIN((size_t)snprintf(buf, size, "%d \xc2\xb7 ", year), size - 1); /* U+00B7 */
    len += MIN((size_t)snprintf(buf + len, size - len,
                                plural_str(songs, LANG_ALBUM_SONGS_ONE,
                                           LANG_ALBUM_SONGS_FEW,
                                           LANG_ALBUM_SONGS_MANY),
                                songs), size - 1 - len);
    if (minutes > 0 && len + 3 < size - 1)
    {
        len += snprintf(buf + len, size - len, " \xc2\xb7 ");
        tagtree_minutes_text(minutes, buf + len, size - len);
    }
}

/* the minutes of so many ms, 1 for less than half of one, 0 for none */
static long ms_minutes(unsigned long ms)
{
    return ms ? MAX((long)((ms + 30000) / 60000), 1) : 0;
}

/* the texts' room beside the cover, in the list's whole width */
static int text_width(const struct gui_synclist *list)
{
    return list->parent[SCREEN_MAIN]->width - global_settings.list_text_padding
           - COVER_SIZE - COVER_GAP - TEXT_MARGIN;
}

/* the rows the cover and the texts take in the list, with the slack below */
static void count_rows(const struct gui_synclist *list, int font)
{
    struct layout l;
    lay_out(header.lines, name_font(font), font, &l);
    const int row = list->line_height[SCREEN_MAIN];
    header.rows = (l.foot + HEADER_FOOT + row - 1) / row;
    header.slack = header.rows * row - (l.foot + HEADER_FOOT);
}

/* The album's texts, fitted to width, and its rows: false without its name */
static bool album_texts(struct tree_context *c, const struct tagtree_album *album,
                        const struct gui_synclist *list, int width)
{
    char text[MAX_PATH];
    if (!tagtree_album_text(c, tag_album, text, sizeof(text)))
        return false;
    const int font = list->parent[SCREEN_MAIN]->font;
    header.name_form = font_text_form((const unsigned char *)text, -1);
    font_set_text_form(header.name_form);
    header.lines = wrap_name(text, name_font(font), width);

    /* none for a show whose episodes' authors differ */
    if (album->albumartist < 0
        || !tagtree_album_text(c, tag_albumartist, text, sizeof(text)))
        text[0] = '\0';
    else if (!strcmp(text, UNTAGGED))
        strlcpy(text, str(tagtree_album_is_book(c) ? LANG_UNKNOWN_AUTHOR
                                                   : LANG_UNKNOWN_ARTIST),
                sizeof(text));
    header.artist_form = font_text_form((const unsigned char *)text, -1);
    font_set_text_form(header.artist_form);
    fit_text(header.artist, TEXT_BYTES, text, strlen(text), artist_font(font), width);
    font_set_text_form(-1);

    /* "2019 · 16 songs · 58 min", or without the year if it has none or
       the line is wider than its room, as on the iPod photo's screen */
    if (!tagtree_album_counts(c, text, sizeof(text)))
    {
        const long minutes = ms_minutes(album->length);
        counts_text(text, sizeof(text), album->year, album->songs, minutes);
        if (album->year > 0
            && font_getstringsize(text, NULL, NULL, counts_font(font)) > width)
            counts_text(text, sizeof(text), 0, album->songs, minutes);
    }
    fit_text(header.counts, TEXT_BYTES, text, strlen(text), counts_font(font), width);
    count_rows(list, font);
    return true;
}

int album_header_open(struct tree_context *c, struct gui_synclist *list,
                      int rows_after)
{
    struct tagtree_album album;
    header.rows = 0;
    header.playlist = false;
    const int width = text_width(list);
    if (!tagtree_album(c, &album) || width <= 0
        || !album_texts(c, &album, list, width))
    {
        header.rows = 0;
        return 0;
    }
    /* narrower beside the scrollbar, if the list has one with these rows:
       the rows only grow, as a name takes two lines */
    const int narrower = list_scrollbar_room(list, header.rows + rows_after);
    if (narrower && (width - narrower <= 0
                     || !album_texts(c, &album, list, width - narrower)))
    {
        header.rows = 0;
        return 0;
    }
#ifdef ITUNESDB_HAVE_ART
    find_cover(&album);
#endif
    return header.rows;
}

/* The playlist's texts, fitted to width, and its rows */
static void playlist_texts(const struct gui_synclist *list, const char *name,
                           int songs, unsigned long seconds, int width)
{
    char text[MAX_PATH];
    const int font = list->parent[SCREEN_MAIN]->font;
    header.name_form = font_text_form((const unsigned char *)name, -1);
    font_set_text_form(header.name_form);
    fit_text(header.name[0], TEXT_BYTES, name, strlen(name), name_font(font),
             width);
    font_set_text_form(-1);
    header.lines = 1;
    header.artist[0] = '\0';
    header.artist_form = -1;
    /* "24 songs · 1 h 32 min", its minutes as an album's */
    counts_text(text, sizeof(text), 0, songs,
                seconds ? MAX((long)((seconds + 30) / 60), 1) : 0);
    fit_text(header.counts, TEXT_BYTES, text, strlen(text), counts_font(font), width);
    count_rows(list, font);
}

int album_header_open_playlist(struct gui_synclist *list, const char *name,
                               int songs, unsigned long seconds,
                               const int32_t *collage, int rows_after)
{
    header.rows = 0;
    header.playlist = true;
    const int width = text_width(list);
    if (songs <= 0 || width <= 0)
        return 0;
    playlist_texts(list, name, songs, seconds, width);
    const int narrower = list_scrollbar_room(list, header.rows + rows_after);
    if (narrower)
    {
        if (width - narrower <= 0)
        {
            header.rows = 0;
            return 0;
        }
        playlist_texts(list, name, songs, seconds, width - narrower);
    }
#ifdef ITUNESDB_HAVE_ART
    find_collage(collage);
#else
    (void)collage;
#endif
    return header.rows;
}

/* the cover, or the note on a tile of a group's grey, as a slide without one */
static void draw_cover(struct screen *display, int x, int y)
{
    const int radius = ui_metric(UI_METRIC_HEADER_COVER_RADIUS);
#ifdef ITUNESDB_HAVE_ART
    if (cover.width > 0 && cover.is_collage)
    {
        /* seamless, its outer corners as a cover's */
        const fb_data *pixels = cover.pixels;
        for (int k = 0; k < cover.cells; k++)
        {
            const int w = cover.cell[k].w, h = cover.cell[k].h;
            if (cover.cell[k].ok)
                display->bitmap_part(pixels, 0, 0, STRIDE(SCREEN_MAIN, w, h),
                                     x + cover.cell[k].x, y + cover.cell[k].y,
                                     w, h);
            else
            {
                display->set_foreground(ui_colour(UI_COLOUR_COVER_TILE));
                display->set_drawmode(DRMODE_SOLID);
                display->fillrect(x + cover.cell[k].x, y + cover.cell[k].y,
                                  w, h);
            }
            pixels += w * h;
        }
        line_round_corners(display, x, y, cover.width, cover.height, radius);
        return;
    }
    if (cover.width > 0)
    {
        display->bitmap_part(cover.pixels, 0, 0,
                             STRIDE(SCREEN_MAIN, cover.width, cover.height),
                             x + (COVER_SIZE - cover.width) / 2,
                             y + (COVER_SIZE - cover.height) / 2,
                             cover.width, cover.height);
        /* a square one's corners, where the theme rounds them */
        if (cover.width == COVER_SIZE && cover.height == COVER_SIZE)
            line_round_corners(display, x, y, COVER_SIZE, COVER_SIZE, radius);
        return;
    }
    /* one whose picture isn't shown: the "?" in the note's place */
    if (cover.reason && cover_caution_on())
    {
        cover_caution_tile(display, x, y, COVER_SIZE, COVER_SIZE, true);
        line_round_corners(display, x, y, COVER_SIZE, COVER_SIZE, radius);
        return;
    }
#endif
    display->set_foreground(ui_colour(UI_COLOUR_COVER_TILE));
    display->set_drawmode(DRMODE_SOLID);
    display->fillrect(x, y, COVER_SIZE, COVER_SIZE);
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (display->screen_type == SCREEN_MAIN)
    {
        if (global_settings.ui_theme == UI_THEME_MODERN)
            line_draw_note(modern_note_header_alpha, MODERN_NOTE_HEADER_W,
                           MODERN_NOTE_HEADER_H,
                           x + (COVER_SIZE - MODERN_NOTE_HEADER_W) / 2,
                           y + (COVER_SIZE - MODERN_NOTE_HEADER_H) / 2);
        else if (global_settings.ui_theme == UI_THEME_NEO)
            line_draw_note(neo_note_header_alpha, NEO_NOTE_HEADER_W, NEO_NOTE_HEADER_H,
                           x + (COVER_SIZE - NEO_NOTE_HEADER_W) / 2,
                           y + (COVER_SIZE - NEO_NOTE_HEADER_H) / 2);
        else if (global_settings.ui_theme == UI_THEME_DIETER)
            line_draw_note(dieter_header_tile_alpha, DIETER_HEADER_TILE_W,
                           DIETER_HEADER_TILE_H,
                           x + (COVER_SIZE - DIETER_HEADER_TILE_W) / 2,
                           y + (COVER_SIZE - DIETER_HEADER_TILE_H) / 2);
        else
            line_draw_note(note_alpha, NOTE_W, NOTE_H, x + (COVER_SIZE - NOTE_W) / 2,
                           y + (COVER_SIZE - NOTE_H) / 2);
    }
#endif
    line_round_corners(display, x, y, COVER_SIZE, COVER_SIZE, radius);
}

void album_header_draw(struct screen *display, int y, void *data)
{
    (void)data;
    if (!header.rows)
        return;
    struct viewport *vp = *display->current_viewport;
    const int list_font = vp->font;
    const unsigned fg = display->get_foreground();
    const int pad = global_settings.list_text_padding;
    /* in the middle of its rows' height, where the theme has it so: its own
       insets can then be small enough for two rows of two lines, and three
       rows of one line keep it clear of their ends. A playlist's as an
       album's, at the top in Classic */
    if (ui_metric(UI_METRIC_HEADER_CENTRED))
        y += header.slack / 2;
    /* right to left, the cover at the right end and the texts left of it:
       the viewport puts them at the distance from that end */
    draw_cover(display, VP_IS_RTL(vp) ? vp->width - pad - COVER_SIZE : pad,
               y + COVER_TOP);

    struct layout l;
    lay_out(header.lines, name_font(list_font), list_font, &l);
    const int x = pad + COVER_SIZE + COVER_GAP;
    display->set_drawmode(DRMODE_FG);
    display->set_foreground(fg);
    font_set_text_form(header.name_form);
    vp->font = name_font(list_font);
    for (int k = 0; k < header.lines; k++)
        display->putsxy(x, y + l.name[k], header.name[k]);
    display->set_foreground(LIST_VALUE_COLOUR);
    if (!header.playlist)
    {
        vp->font = artist_font(list_font);
        font_set_text_form(header.artist_form);
        display->putsxy(x, y + l.artist, header.artist);
    }
    font_set_text_form(-1);
    vp->font = counts_font(list_font);
    display->putsxy(x, y + l.counts, header.counts);
    vp->font = list_font;
    display->set_foreground(fg);
    display->set_drawmode(DRMODE_SOLID);
}

bool album_header_cover_reading(void)
{
#ifdef ITUNESDB_HAVE_ART
    return header.rows && cover.reading;
#else
    return false;
#endif
}

bool album_header_cover_came(void)
{
#ifdef ITUNESDB_HAVE_ART
    if (!header.rows || !cover.came)
        return false;
    cover.came = false;
    return true;
#else
    return false;
#endif
}

void album_header_reader_wait(void)
{
#ifdef ITUNESDB_HAVE_ART
    /* the rows' asked for not read: lists' covers wait for the wheel again */
    list_covers_cancel();
    menu_cover_read_cancel();
    /* the flag drops just before the thread ends, without a yield between */
    while (cover.reading)
        sleep(1);
#endif
}

void album_header_forget_cover(void)
{
#ifdef ITUNESDB_HAVE_ART
    /* one being read too, which is of the covers before */
    cover.known = false;
    cover.width = 0;
    cover.cells = 0;
    cover.reason = 0;
    cover.wanted++;
    cover.to_read = false;
#endif
}
