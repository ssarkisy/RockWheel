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

/* Notes what playback needs from iTunesDB: the tracks with flags and where
   each cover is. Until then no track has flags and the covers come from the
   files. At boot after a sync (synced), it also writes Finder's playlists
   as .m3u8 files and keeps the tracks' counts for itunesdb_import(), so
   that iTunesDB is read only once. Returns the number of tracks read. */
int itunesdb_import_tracks(bool synced);

/* Reads what itunesdb_import_tracks() noted into memory, at boot before
   music plays. False when that was noted from another iTunesDB, or by a
   firmware before this: the counts are then still to be kept, as they hold
   plays Finder hasn't had, but the tracks are to be imported again. */
bool itunesdb_load_tracks(void);

/* the flags of the file at path */
unsigned itunesdb_track_flags(const char *path);

/* true when the file at path is to play from where it was left: a podcast
   or an audiobook, or any file with Rockbox's automatic resume on */
bool itunesdb_resumes(const char *path);

/* the covers Finder makes for the stock firmware are RGB565 frames */
#if defined(HAVE_ALBUMART) && (LCD_DEPTH == 16) && (LCD_PIXELFORMAT == RGB565)
#define ITUNESDB_HAVE_ART

struct bitmap;

/* where a track's cover is */
struct itunesdb_art {
    char path[64];      /* its ithmb file */
    uint32_t offset;
    int width, height;  /* of the frame */
};

/* the cover Finder made for the file at path */
bool itunesdb_find_art(const char *path, struct itunesdb_art *art);

/* reads the cover into bm, scaled up or down to fit its width and height;
   returns the size of the pixels or -1. It opens the ithmb file itself, and
   only when the frame isn't the one read last. */
int itunesdb_load_art(const struct itunesdb_art *art, struct bitmap *bm,
                      int max_size);
#endif

#endif /* _ITUNESDB_H_ */
