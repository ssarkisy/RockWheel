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

/* Brings the playlists, ratings and play counts in line with iTunesDB. Runs
   on the tagcache thread once the database has been updated from the files */
void itunesdb_import(void);

/* Writes iPod_Control/iTunes/Play Counts like the stock firmware, for Finder
   to take what was played and rated on the iPod into the library: plays
   since the last sync and the ratings, in iTunesDB's order */
void itunesdb_write_playcounts(void);

/* the hash that a database entry and an iTunesDB track are matched by */
uint64_t itunesdb_path_hash(const char *path);

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

/* Notes where each track's cover is, at boot after a sync; until then the
   covers come from the files */
void itunesdb_import_art(void);

/* Reads where the covers are into memory, at boot before music plays */
void itunesdb_load_art_index(void);

/* the cover Finder made for the file at path */
bool itunesdb_find_art(const char *path, struct itunesdb_art *art);

/* reads the cover into bm, scaled up or down to fit its width and height;
   returns the size of the pixels or -1. It opens the ithmb file itself, and
   only when the frame isn't the one read last. */
int itunesdb_load_art(const struct itunesdb_art *art, struct bitmap *bm,
                      int max_size);
#endif

#endif /* _ITUNESDB_H_ */
