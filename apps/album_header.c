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
 * name, in a line or two, its album artist and "2019 · 16 songs", in the fonts
 * of the theme's Now Playing. It takes the list's first rows, three or four,
 * which none can select, and scrolls away with the songs, as iOS's does; the
 * list's title names the album once it's gone (tree.c).
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
 * from the buffer or a disk that's on, before the screen, the note not seen. */

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

#define COVER_SIZE   60
#define COVER_TOP    6
#define COVER_GAP    10 /* from the cover to the texts */
#define TEXT_MARGIN  6  /* the texts' at the other end, as a setting's value's */
#define NAME_TOP     5
#define NAME_LINES   2
/* a name's two lines closer than its font's cells, which have room above
   the capitals; the artist's line just under it, and the year's */
#define NAME_LEADING 2
#define ARTIST_GAP   1
#define COUNTS_GAP   2
#define HEADER_FOOT  6  /* below the lowest of the cover and the texts */
#define TEXT_BYTES   192

static struct {
    int rows;  /* of the list, 0 without a header */
    int lines; /* the name's */
    char name[NAME_LINES][TEXT_BYTES];
    char artist[TEXT_BYTES];
    char counts[TEXT_BYTES];
} header;

#ifdef ITUNESDB_HAVE_ART
/* The cover, of the album these seeks name in the database of this commit,
   or none there as width 0: the note in its place. Its reader fills the pixels
   while width is 0, and only the UI draws them; the two threads are on one
   core, and none of their changes here has a yield in the middle */
static struct {
    fb_data pixels[COVER_SIZE * COVER_SIZE];
    volatile int width, height;
    bool known;
    int32_t album, albumartist;
    int commit;
    /* the read asked for: of the album of this song, from the playback's
       buffer if it's playing; numbered, as another album opened, or the
       covers made again, leave one being read unshown */
    int32_t song;
    bool playing;
    volatile unsigned wanted;
    volatile bool to_read, reading, came;
} cover;

/* The reader's thread, only while it reads, and its stack, 4 KB: its calls go
   1.2 KB deep at most by their frames in the firmware's code, which leaves
   room for the calls through pointers that count can't follow */
static long reader_stack[(DEFAULT_STACK_SIZE + 0xc00) / sizeof(long)];
/* how long the screen waits for a read that costs no spin-up, from the buffer
   or a disk that's on: about 20-30 ms, which are then without the note */
#define COVER_WAIT (HZ / 20)
#endif

#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
/* The note on the tile in the cover's place, the menus' panel's, in Rockbox'
   alpha format (4 bits from 0 opaque to 15 transparent, low nibble first,
   rows run on without padding): made by themes/tools/make_list_glyphs.py */
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

/* A font of the theme's, which its skins keep loaded from boot, found by its
   file: the bold one with Bold Text, as the theme has it then. The list's
   with a theme without either */
static int theme_font(const char *regular, const char *bold, int list_font)
{
    const bool heavier = global_settings.bold_text;
    int font = font_find(heavier ? bold : regular);
    if (font < 0)
        font = font_find(heavier ? regular : bold);
    return font < 0 ? list_font : font;
}

/* the album artist's, as Now Playing's artist, and the year's, as its time */
static int artist_font(int list_font)
{
    return theme_font(FONT_DIR "/17-Inter-Medium.fnt", FONT_DIR "/17-Inter-Bold.fnt",
                      list_font);
}

static int counts_font(int list_font)
{
    return theme_font(FONT_DIR "/14-Inter-SemiBold.fnt", FONT_DIR "/14-Inter-Bold.fnt",
                      list_font);
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
    for (int k = 0; k < NAME_LINES; k++)
        l->name[k] = NAME_TOP + k * (name_h - NAME_LEADING);
    l->artist = l->name[lines - 1] + name_h + ARTIST_GAP;
    l->counts = l->artist + font_get(artist_font(list_font))->height + COUNTS_GAP;
    l->foot = MAX(COVER_TOP + COVER_SIZE,
                  l->counts + font_get(counts_font(list_font))->height);
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
   Finder's of the song, or else the album's of ours, by its name and album
   artist as the database has them. The song's path may come from the disk,
   as before the dircache is ready. 1 found, 0 none, -1 not known, as with
   the database busy or the path not read. What it reads into is the
   reader's, which reads one cover at a time: off its stack, 1.5 KB */
static struct {
    struct tagcache_search tcs;
    char path[MAX_PATH], name[MAX_PATH], artist[MAX_PATH];
} finding;

int album_header_find_art(int32_t song, struct itunesdb_art *art)
{
    struct tagcache_search *tcs = &finding.tcs;
    if (!tagcache_search(tcs, tag_title))
        return -1;
    int found = -1;
    if (tagcache_retrieve(tcs, song, tag_filename, finding.path, MAX_PATH))
    {
        if (itunesdb_find_art(finding.path, art))
            found = 1;
        else if (tagcache_retrieve(tcs, song, tag_album, finding.name, MAX_PATH)
                 && tagcache_retrieve(tcs, song, tag_albumartist, finding.artist,
                                      MAX_PATH))
            found = itunesdb_find_album_art(finding.name, finding.artist,
                                            finding.path, art) ? 1 : 0;
    }
    tagcache_search_finish(tcs);
    return found;
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

/* The reader: each cover asked for, until none is, shown if it's still the
   one wanted; then its thread ends. One it couldn't look for or read, as
   with USB coming or a disk's error, is looked for again the next time the
   album opens; only an album without one is kept as such. The covers of a
   list's rows too, those a frame lacked and those next to them as the wheel
   rests (list_covers.c), after the header's, which its screen waits for.
   Behind the UI, which has the CPU at each of a read's yields before it, as
   a wheel's step comes; the header's as the UI, which waits for it */
static void cover_reader(void)
{
    while (cover.to_read || list_covers_to_read())
    {
        if (!cover.to_read)
        {
            list_covers_read();
            continue;
        }
#ifdef HAVE_PRIORITY_SCHEDULING
        thread_set_priority(thread_self(), PRIORITY_USER_INTERFACE);
#endif
        cover.to_read = false;
        const unsigned wanted = cover.wanted;
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

bool album_header_reader_start(void)
{
    if (cover.reading)
        return true;
    cover.reading = true;
    if (!create_thread(cover_reader, reader_stack, sizeof(reader_stack), 0,
                       "album cover" IF_PRIO(, PRIORITY_BACKGROUND)
                       IF_COP(, CPU)))
        cover.reading = false;
    return cover.reading;
}

/* The album's cover, unless it's the one read last: asked of the reader, its
   thread started if it isn't on. Waited for only when it costs no spin-up */
static void find_cover(const struct tagtree_album *album)
{
    const int commit = tagcache_get_commitid();
    if (cover.known && cover.album == album->album
        && cover.albumartist == album->albumartist && cover.commit == commit)
        return;
    cover.known = true;
    cover.album = album->album;
    cover.albumartist = album->albumartist;
    cover.commit = commit;
    cover.width = 0;
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
#endif /* ITUNESDB_HAVE_ART */

int album_header_open(struct tree_context *c, struct gui_synclist *list)
{
    struct tagtree_album album;
    char text[MAX_PATH];
    header.rows = 0;
    if (!tagtree_album(c, &album)
        || !tagtree_album_text(c, tag_album, text, sizeof(text)))
        return 0;

    const struct viewport *vp = list->parent[SCREEN_MAIN];
    const int font = vp->font;
    const int width = vp->width - global_settings.list_text_padding - COVER_SIZE
                      - COVER_GAP - TEXT_MARGIN;
    if (width <= 0)
        return 0;
    header.lines = wrap_name(text, font, width);

    if (!tagtree_album_text(c, tag_albumartist, text, sizeof(text)))
        text[0] = '\0';
    else if (!strcmp(text, UNTAGGED))
        strlcpy(text, str(LANG_UNKNOWN_ARTIST), sizeof(text));
    fit_text(header.artist, TEXT_BYTES, text, strlen(text), artist_font(font), width);

    /* "2019 · 16 songs", or the songs alone without a year */
    int len = 0;
    if (album.year > 0)
        len = snprintf(text, sizeof(text), "%d \xc2\xb7 ", album.year); /* U+00B7 */
    snprintf(text + len, sizeof(text) - len,
             plural_str(album.songs, LANG_ALBUM_SONGS_ONE, LANG_ALBUM_SONGS_FEW,
                        LANG_ALBUM_SONGS_MANY), album.songs);
    fit_text(header.counts, TEXT_BYTES, text, strlen(text), counts_font(font), width);

    struct layout l;
    lay_out(header.lines, font, font, &l);
    const int row = list->line_height[SCREEN_MAIN];
    header.rows = (l.foot + HEADER_FOOT + row - 1) / row;
#ifdef ITUNESDB_HAVE_ART
    find_cover(&album);
#endif
    return header.rows;
}

/* the cover, or the note on a tile of a group's grey, as a slide without one */
static void draw_cover(struct screen *display, int x, int y)
{
#ifdef ITUNESDB_HAVE_ART
    if (cover.width > 0)
    {
        display->bitmap_part(cover.pixels, 0, 0,
                             STRIDE(SCREEN_MAIN, cover.width, cover.height),
                             x + (COVER_SIZE - cover.width) / 2,
                             y + (COVER_SIZE - cover.height) / 2,
                             cover.width, cover.height);
        return;
    }
#endif
    display->set_foreground(LIST_GROUP_BAR_COLOUR);
    display->set_drawmode(DRMODE_SOLID);
    display->fillrect(x, y, COVER_SIZE, COVER_SIZE);
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (display->screen_type == SCREEN_MAIN)
    {
        display->set_foreground(LIST_GROUP_COLOUR);
        display->set_drawmode(DRMODE_FG);
        lcd_alpha_bitmap_part(note_alpha, 0, 0, NOTE_W,
                              x + (COVER_SIZE - NOTE_W) / 2,
                              y + (COVER_SIZE - NOTE_H) / 2, NOTE_W, NOTE_H);
    }
#endif
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
    /* right to left, the cover at the right end and the texts left of it:
       the viewport puts them at the distance from that end */
    draw_cover(display, VP_IS_RTL(vp) ? vp->width - pad - COVER_SIZE : pad,
               y + COVER_TOP);

    struct layout l;
    lay_out(header.lines, list_font, list_font, &l);
    const int x = pad + COVER_SIZE + COVER_GAP;
    display->set_drawmode(DRMODE_FG);
    display->set_foreground(fg);
    for (int k = 0; k < header.lines; k++)
        display->putsxy(x, y + l.name[k], header.name[k]);
    display->set_foreground(LIST_VALUE_COLOUR);
    vp->font = artist_font(list_font);
    display->putsxy(x, y + l.artist, header.artist);
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
    cover.wanted++;
    cover.to_read = false;
#endif
}
