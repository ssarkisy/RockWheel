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

#ifndef _IPOD_PLAYLISTS_H_
#define _IPOD_PLAYLISTS_H_

#include <stdbool.h>

/* Playlists made on the iPod, and those put in Playlists by hand, beside the
   ones synced from Finder: the synced ones the import writes again after
   each sync, and the others are the iPod's own, to change on it. */

/* Which of the files and folders in Playlists are the import's, as its list
   has them: read once, at boot after the library is up to date, as only an
   import changes them. Its room is taken then, none while music plays. */
void ip_synced_load(void);
/* a playlist or a folder of the import's, by its path; false if unknown */
bool ip_is_synced(const char *path);

/* a playlist of Playlists the iPod may change: not the import's */
bool ip_editable(const char *path);
/* a folder of Playlists the iPod may rename or delete: not the import's, nor
   with one of its playlists in it */
bool ip_folder_editable(const char *path);

/* These ask a name with the letter ring, or what to do, and do it: 1 if
   done, 0 if not, -1 for USB. What's made, renamed or moved goes to buf. */
int ip_new_playlist(const char *dir, char *made, size_t size);
int ip_new_folder(const char *dir, char *made, size_t size);
int ip_rename(const char *path, char *renamed, size_t size);
int ip_move(const char *path, const char *to_dir, char *moved_to, size_t size);
/* a playlist, or a folder with the playlists in it; title is its name */
int ip_delete(const char *path, const char *title);

/* A song, a playlist's songs, or those of the song, album, artist or the
   like selected in a list of Music, at the end of a playlist made on the
   iPod, its index and the queue, if it plays, with them */
bool ip_add_file(const char *target, const char *path);
bool ip_add_playlist(const char *target, const char *playlist);
bool ip_add_selection(const char *target);
/* One of the catalogue's made on the iPod, or a new one, picked: 1, 0 if
   none was, -1 for USB or the main menu */
int ip_pick_playlist(char *picked, size_t size);

#endif /* _IPOD_PLAYLISTS_H_ */
