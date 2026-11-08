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

#ifndef _ON_THE_GO_H_
#define _ON_THE_GO_H_

#include <stdbool.h>
#include "rbpaths.h"

/* On-The-Go, the playlist made on the iPod, as on a stock one: the paths of
   its songs, a line each. In Rockbox's own folder, out of the playlists the
   import writes and removes, so that one of Finder's of the same name doesn't
   make it "On-The-Go 2". */
#define OTG_FILE ROCKBOX_DIR "/on-the-go.m3u8"

/* each true if it was added */
bool otg_add_file(const char *path);
bool otg_add_playlist(const char *playlist);
/* the song, album, artist or the like selected in a list of Music */
bool otg_add_selection(void);
/* the same at the end of another playlist file, target */
bool playlist_file_append(const char *target, const char *path);
bool playlist_file_append_playlist(const char *target, const char *playlist);
bool playlist_file_append_selection(const char *target);

void otg_clear(void);
bool otg_empty(void);
/* its songs as its screen has them now */
struct playlist_info;
bool otg_save(struct playlist_info *playlist);

#endif /* _ON_THE_GO_H_ */
