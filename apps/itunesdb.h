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
bool itunesdb_synced_path(const char *path);

/* Brings the ratings and play counts in line with what
   itunesdb_import_tracks() read after the sync. Runs on the tagcache thread
   once the database has been updated from the files */
void itunesdb_import(void);

/* Writes iPod_Control/iTunes/Play Counts like the stock firmware, for Finder
   to take what was played and rated on the iPod into the library: plays
   since the last sync and the ratings, in iTunesDB's order */
void itunesdb_write_playcounts(void);

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
};

/* Cover Flow's slides, one an album, made after an import: covers this
   many pixels a side at most, as PictureFlow shows the one in the middle */
#define ITUNESDB_SLIDE_SIZE MIN(MAX(LCD_HEIGHT / 2, LCD_WIDTH * 2 / 5), \
                                LCD_HEIGHT * 2 / 3)

/* the cover Finder made for the file at path */
bool itunesdb_find_art(const char *path, struct itunesdb_art *art);

/* reads the cover into bm, scaled up or down to fit its width and height;
   returns the size of the pixels or -1. It opens the ithmb file itself, and
   only when the frame isn't the one read last. */
int itunesdb_load_art(const struct itunesdb_art *art, struct bitmap *bm,
                      int max_size);

/* reads the slide of the cover's album into buf, ITUNESDB_SLIDE_SIZE
   squared pixels at most, column after column, and its size into width and
   height; returns the size of the pixels, or -1 when it isn't made (yet) */
int itunesdb_load_slide(const struct itunesdb_art *art, void *buf,
                        int *width, int *height);

/* true when an import left slides to make, or the index of covers is of
   an older version, whose import at boot will */
bool itunesdb_slides_todo(void);

/* makes them, counting done of total, until stop() says to stop: the next
   call goes on from there. On the tagcache thread. */
void itunesdb_make_slides(volatile int *done, volatile int *total,
                          bool (*stop)(void));
#endif

#endif /* _ITUNESDB_H_ */
