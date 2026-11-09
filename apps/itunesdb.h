/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Sergei Sarkisian
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
#ifndef _ITUNESDB_H_
#define _ITUNESDB_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "config.h"

/* the database Finder writes when it syncs the iPod */
#define ITUNESDB_DIR  "/iPod_Control/iTunes"
#define ITUNESDB_FILE ITUNESDB_DIR "/iTunesDB"

/* true when Finder has synced since the last import */
bool itunesdb_changed(void);
bool itunesdb_exists(void);
/* the name given to the iPod in Finder, or else "iPod" */
const char *itunesdb_device_name(void);
/* when Finder last wrote the database */
struct tm;
bool itunesdb_sync_date(struct tm *tm);
bool itunesdb_synced_path(const char *path);

/* Brings the ratings and play counts in line with what
   itunesdb_import_tracks() read after the sync. Runs on the tagcache thread
   once the database has been updated from the files */
void itunesdb_import(void);
void itunesdb_forget_state(void);

/* Writes iPod_Control/iTunes/Play Counts like the stock firmware, for Finder
   to take what was played and rated on the iPod into the library: plays
   since the last sync and the ratings, in iTunesDB's order */
void itunesdb_write_playcounts(void);

/* The first lines of each playlist file the import writes, by which it knows
   its own: written so again from the iPod's own changes to them */
#define ITUNESDB_PLAYLIST_HEAD "#EXTM3U\n"
#define ITUNESDB_PLAYLIST_MARK "# From the iPod's library, written again after each sync"
/* and after them, the column it's sorted by on the Mac, as iTunesDB numbers
   them, if not its own order */
#define ITUNESDB_PLAYLIST_SORT "# Sorted by "
#define ITUNESDB_SORT_MANUAL   1
#define ITUNESDB_SORT_TITLE    3
#define ITUNESDB_SORT_ALBUM    4
#define ITUNESDB_SORT_ARTIST   5
#define ITUNESDB_SORT_GENRE    7
#define ITUNESDB_SORT_COMPOSER 18
/* one of the import's, by its second line */
bool itunesdb_own_playlist(const char *file);
/* The playlists the last import wrote and the folders it made, a line each:
   "<hash> <size> <time> <file>", and "dir <folder>" */
#define ITUNESDB_PLAYLISTS_LIST ROCKBOX_DIR "/itunesdb_playlists.txt"
/* Songs a sync wrote again under another path, as their dbid has it, by
   their old path's hash, sorted by it: for the playlists made on the iPod,
   which keep them, at Updating Library, which then removes the list */
#define ITUNESDB_MOVED_LIST ROCKBOX_DIR "/itunesdb_moved.dat"
struct itunesdb_moved {
    uint64_t old_hash;
    char path[56];      /* "/iPod_Control/Music/F07/CWZV.m4a" and the like */
};
/* the path now of a song moved from the path of old_hash */
bool itunesdb_moved_to(uint64_t old_hash, char *path, size_t size);

/* the hash that a database entry and an iTunesDB track are matched by */
uint64_t itunesdb_path_hash(const char *path);

/* What iTunesDB says a track does differently, as the stock firmware
   plays it */
#define ITUNESDB_REMEMBERS_PLACE 0x01 /* podcasts, audiobooks */
#define ITUNESDB_AUDIOBOOK       0x02 /* plays at the audiobook speed */
#define ITUNESDB_SKIP_SHUFFLE    0x04 /* not in Shuffle Songs */
#define ITUNESDB_PODCAST         0x08

/* Notes what playback needs from iTunesDB: the tracks with flags and where
   each cover is. Until then no track has flags and the covers come from the
   files. At boot after a sync (synced), it also writes Finder's playlists
   as .m3u8 files and keeps the tracks' counts for itunesdb_import(), so
   that iTunesDB is read only once. Returns the number of tracks read; done
   and total, which may be NULL, say how far it is meanwhile. */
int itunesdb_import_tracks(bool synced, volatile int *done, volatile int *total);

/* Reads what itunesdb_import_tracks() noted into memory, at boot before
   music plays. False when that was noted from another iTunesDB, or by a
   firmware before this: the counts are then still to be kept, as they hold
   plays Finder hasn't had, but the tracks are to be imported again. */
bool itunesdb_load_tracks(void);

/* What iTunesDB says of a file Finder added since the last import, as its
   tags would, and its play count and rating: false when it doesn't know the
   file. Kept from itunesdb_import_tracks() until itunesdb_forget_metadata(),
   for the scan. */
struct mp3entry;
struct tagcache_runtime;
bool itunesdb_get_metadata(const char *path, struct mp3entry *id3,
                           struct tagcache_runtime *rt);
void itunesdb_forget_metadata(void);

/* the flags of the file at path, or with this itunesdb_path_hash() */
unsigned itunesdb_track_flags(const char *path);
unsigned itunesdb_hash_flags(uint64_t hash);

/* Sound Check's gains are noted from this iTunesDB, or there is none */
bool itunesdb_gains_ready(void);

/* Puts the gain Finder worked out for this file into its entry, for Sound
   Check to use over any ReplayGain tags it has. Reads the disk: for when a
   track loads. */
void itunesdb_set_track_gain(struct mp3entry *id3);

/* true when the file at path is to play from where it was left: a podcast
   or an audiobook, or any file with Rockbox's automatic resume on */
bool itunesdb_resumes(const char *path);

/* the covers Finder makes for the stock firmware are RGB565 frames */
#if defined(HAVE_ALBUMART) && (LCD_DEPTH == 16) && (LCD_PIXELFORMAT == RGB565)
#define ITUNESDB_HAVE_ART

/* the parameter of Cover Flow's plugin for making its album list and leaving,
   at the end of an import */
#define ITUNESDB_ALBUM_LIST "--album-list"

struct bitmap;

/* where a track's cover is */
struct itunesdb_art {
    char path[64];      /* its ithmb file */
    uint32_t offset;
    int width, height;  /* of the frame */
    int slide;          /* its album's slide for Cover Flow, 0 for none */
    uint32_t colour;    /* its album's colour: see cover_palette.h */
};

/* Cover Flow's slides, one an album, made after an import: covers this
   many pixels a side at most, as PictureFlow shows the one in the middle. That
   is as on a stock iPod, with the room of the status bar it has given to it:
   136 on a screen 240 high */
/* what itunesdb_load_slide() needs in its buffer beyond the slide: its header,
   the way to an aligned address, and the rest of its slot, which it reads whole */
#define ITUNESDB_SLIDE_ROOM 2048
#define ITUNESDB_SLIDE_SIZE MIN(MAX(LCD_HEIGHT * 17 / 30, LCD_WIDTH * 2 / 5), \
                                LCD_HEIGHT * 2 / 3)

/* the cover Finder made for the file at path */
bool itunesdb_find_art(const char *path, struct itunesdb_art *art);

/* The covers of the albums of songs copied outside Finder's folder, made on
   Updating Library in Finder's forms, one an album. An album's key: its name
   and album artist, as the database has them (the track's artist without
   one), Latin letters in either case as one; or else the folder of path. */
struct mp3entry;
uint64_t itunesdb_album_key(const char *album, const char *artist,
                            const char *path);
/* the cover of the track at path with these tags: Finder's, or else its
   album's */
bool itunesdb_find_track_art(const struct mp3entry *id3, const char *path,
                             struct itunesdb_art *art);
/* an album's of ours, by what itunesdb_album_key() takes */
bool itunesdb_find_album_art(const char *album, const char *artist,
                             const char *path, struct itunesdb_art *art);
/* not made yet for the database as it is now */
bool itunesdb_covers_stale(void);
/* Making them, by Cover Flow's plugin: begin with room for this many albums;
   for each, known() when made from the same stamp, and kept: 1, or -1 when
   none was found then, -2 when it was in a format not shown, 0 when not made
   so; or else put() with its frame, up
   to 200 by 200, or NULL for none found, and making() before decoding it;
   then end(). */
bool itunesdb_covers_begin(int albums);
/* none to be made for this database, as without the room for it */
void itunesdb_covers_skip(void);
int itunesdb_cover_known(uint64_t key, uint32_t stamp);
/* said just before an album's cover is decoded: an album the making is cut
   short at twice, as by a cover that crashes the decoder, is given up */
void itunesdb_cover_making(uint64_t key, uint32_t stamp);
/* put() of none, its cover in a format that isn't shown, as PNG: known()
   says -2 for it then */
void itunesdb_cover_unshown(uint64_t key);
/* after put(), the stamp of the image in the album's folder it was made of,
   0 for none; image_of() says the one kept, for the images changed */
void itunesdb_cover_image(uint64_t key, uint32_t image);
uint32_t itunesdb_cover_image_of(uint64_t key);
/* the images outside Finder's folder changed since the covers were made, by
   the check after USB: they're to be looked at again */
bool itunesdb_covers_images_changed(void);
/* 1 made, 0 not, -1 for no room left on the disk, which ends the making */
int itunesdb_cover_put(uint64_t key, uint32_t stamp, const struct bitmap *bm);
/* how they end: every album looked at, those not being gone; cut short by
   Menu or USB, the rest made the next time; or the albums known not all of
   them, those not seen kept */
#define COVERS_CUT    0
#define COVERS_WHOLE  1
#define COVERS_PARTLY 2
void itunesdb_covers_end(int how);

/* reads the cover into bm, scaled up or down to fit its width and height;
   returns the size of the pixels or -1. It opens the ithmb file itself, and
   only when the frame isn't the one read last. */
int itunesdb_load_art(const struct itunesdb_art *art, struct bitmap *bm,
                      int max_size);

/* the same from a cover already in memory, such as one in the buffer */
int itunesdb_scale_art(const struct bitmap *src, struct bitmap *bm, int max_size);

/* both, for a cover shown small, as in an album's header: shrunk to half its
   size or less, each pixel the mean of those of the frame it covers, rather
   than bilinear's four of them */
int itunesdb_load_small_art(const struct itunesdb_art *art, struct bitmap *bm,
                            int max_size);
int itunesdb_scale_small_art(const struct bitmap *src, struct bitmap *bm,
                             int max_size);

/* reads the slide of the cover's album into buf, ITUNESDB_SLIDE_SIZE
   squared pixels at most, column after column, and its size into width and
   height; returns the size of the pixels, or -1 when it isn't made (yet) */
int itunesdb_load_slide(const struct itunesdb_art *art, void *buf,
                        int *width, int *height);
/* the slides file closed, which itunesdb_load_slide() keeps open: as Cover
   Flow ends, and before the disk goes to USB */
void itunesdb_slides_done(void);

/* true when an import left slides to make, or the index of covers is of
   an older version, whose import at boot will, or the slides' colours aren't
   known yet */
bool itunesdb_slides_todo(void);

/* makes them, and notes the colour of each, counting done of total, until
   stop() says to stop: the next call goes on from there. On the tagcache
   thread. */
void itunesdb_make_slides(volatile int *done, volatile int *total,
                          bool (*stop)(void));
#endif

#endif /* _ITUNESDB_H_ */
