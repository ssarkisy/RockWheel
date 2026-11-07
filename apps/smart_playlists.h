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

#ifndef _SMART_PLAYLISTS_H_
#define _SMART_PLAYLISTS_H_

#include <stdbool.h>
#include <stdint.h>

/* The rules of a live smart playlist of iTunesDB (mhod 50 and 51), as far as
   the iPod keeps them up to date itself: by ratings, plays, when last played,
   length and kind, as the stock firmware does */
#define SMART_RULES_MAX 8

struct smart_rule {
    uint32_t field;
    uint32_t action;
    int64_t from, to;           /* values, or dates in Mac time */
    int64_t from_date;          /* "in the last": that many units back, < 0 */
    int64_t from_units;         /* seconds a unit */
};

struct smart_rules {
    uint8_t any;                /* matched by any rule, else by all */
    uint8_t check_rules, check_limits, limit_type;
    uint32_t limit_sort;        /* its top bit: the other way round */
    uint32_t limit_value;
    uint8_t count;
    struct smart_rule rule[SMART_RULES_MAX];
};

/* the import's: before its playlists, for each smart one written, after */
void smart_import_begin(void);
void smart_import_note(const char *file, uint32_t time,
                       const struct smart_rules *rules);
void smart_import_end(bool complete);
/* whether the iPod can work it out by itself */
bool smart_rules_supported(const struct smart_rules *rules);

/* Their songs, from the database as it is now, into their indexes: those of
   the import, on the database's thread, from memory of its own, and their
   files written too, but for the one the music is from, which goes by places
   in its file; before the menu, those not known to be right; and the one
   opened, if ratings or plays changed since, with Loading meanwhile, true
   if it was shown. */
void smart_update_after_import(void);
void smart_update_at_boot(void);
bool smart_prepare(const char *file);

#endif /* _SMART_PLAYLISTS_H_ */
