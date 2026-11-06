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
#ifndef _MUSIC_SEARCH_H_
#define _MUSIC_SEARCH_H_

#include <stdbool.h>

/* Search in Music, as on an Apple TV: a row of letters the wheel goes along,
   Select adds one, and what matches is listed under it as the typing pauses.
   The screen to go to, as root_menu has them. */
int music_search_screen(void *param);
/* as it's opened from Music: nothing typed */
void music_search_new(void);

/* Which letters of the alphabets other than the Latin one the library's names
   have, for the ring: found again on Updating Library when the database is
   another than they're of */
bool music_search_letters_stale(void);
/* its files, made from a database that has gone: a new one counts its commits
   from 1 again, and the old names' songs would be played */
void music_search_forget(void);
/* progress is told how many names are done of about how many */
void music_search_letters_make(void (*progress)(int done, int total));

#endif /* _MUSIC_SEARCH_H_ */
