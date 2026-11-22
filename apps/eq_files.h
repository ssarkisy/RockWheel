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
#ifndef _EQ_FILES_H_
#define _EQ_FILES_H_

#include <stdbool.h>
#include <stddef.h>
#include "eq.h"

/* The user's EQ presets: the .cfg files of /.rockbox/eqs, and AutoEq's and
   Equalizer APO's ParametricEQ.txt, in folders of it too. The table in RAM has
   their names and folders only: read at boot, after USB and as settings are
   applied from the disk (settings_apply()), never as a list opens; a preset's
   bands are read from its file when it's chosen. Static, no heap: 256 entries
   of 69 bytes, 17 KB. A folder whose one row is a preset is listed as that
   preset in its parent; AutoEq's FixedBandEQ beside its ParametricEQ is not
   listed. */

/* presets and folders in all */
#define EQ_FILES_MAX 256
/* a name as listed, in bytes with its terminator */
#define EQ_NAME_MAX  64
/* folders in folders under /.rockbox/eqs: AutoEq's own tree, a source, a
   target and a model, has three */
#define EQ_DEPTH_MAX 4
/* a preset's path from /.rockbox/eqs, with its terminator: what's left of
   MAX_PATH (260) after "/.rockbox/eqs/" */
#define EQ_PATH_MAX  245
/* the folder of the top level */
#define EQ_TOP       0xff

/* The folder read again */
void eq_files_read(void);
/* in the order of their names, as Music sorts names in the menu language, if
   that changed since */
void eq_files_sort(void);

/* A folder's rows: its folders or its presets, each in that order */
int eq_files_children(int folder, bool folders);
/* the n-th of them, an entry; -1 */
int eq_files_child(int folder, bool folders, int n);
/* where an entry is among its folder's of its kind */
int eq_files_child_pos(int entry);

const char *eq_files_name(int entry);
/* a folder entry's own id, which the entries in it have as their folder */
int eq_files_folder_id(int entry);
int eq_files_folder_of(int entry);
/* the entry of the folder of that id, -1 for the top */
int eq_files_folder_entry(int folder);
/* the entry the entry is in at the given depth (0 its top folder's), or the
   entry itself if it's no deeper; -1 if it's not that deep */
int eq_files_ancestor(int entry, int depth);
/* The file's path from /.rockbox/eqs, false if it has none that fits */
bool eq_files_path(int entry, char *buf, size_t size);
/* by that path, whatever its case; -1 */
int eq_files_find(const char *path);

/* A preset's name as it is listed, from its path */
void eq_files_shown_name(const char *path, char *buf, size_t size);
/* Its bands, the defaults where the file names none, and their precut, read
   from its file now: false for a file that isn't there or isn't a preset,
   which then changes nothing */
bool eq_files_load(const char *path, struct eq_band_setting *bands, int *precut);
/* whether a band from a file or a .cfg is one the EQ takes: its values within
   the settings' ranges and played as the curve shows it */
bool eq_files_band_ok(const struct eq_band_setting *band);

/* A preset of that name in the folder, entry except, or a file of it there */
bool eq_files_taken(int folder, const char *name, int except);
/* A preset written in RockWheel's format at the top, over one of that name
   there (a ParametricEQ.txt of it goes), and listed; false if it couldn't be
   written, and then nothing changed */
bool eq_files_save(const char *name, const struct eq_band_setting *bands,
                   int precut);
/* the entry renamed, now at its name's place; -1 if it couldn't be */
int eq_files_rename(int entry, const char *name);
bool eq_files_delete(int entry);

#endif /* _EQ_FILES_H_ */
