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
#ifndef _CHAPTERS_H_
#define _CHAPTERS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "metadata.h"

/* A track's chapters, read from its file as it is buffered; the playing
   track's are copied to the audio buffer's scratch, where mp3entry.chapters
   points */
#define CHAPTERS_MAX   255
#define CHAPTERS_SIZE  8192     /* the most a track's take */
#define CHAPTER_NAME   63       /* bytes of a name kept, UTF-8 cut whole */

struct chapter {
    uint32_t start;             /* ms */
    uint16_t name;              /* where its name is in the names */
    uint16_t len;               /* 0 without one */
};

struct chapters {
    uint8_t count;
    uint8_t slots;              /* room for chapters; the names follow it */
    uint16_t names;             /* bytes of names used */
    int16_t current;            /* as chapters_changed() last saw it */
    int16_t jumped;             /* the chapter a jump went to, -1 for none */
    struct chapter ch[];
};

/* As the track is buffered, on the audio thread: how many chapters the file
   at fd has to keep, 0 for fewer than two, and the bytes they take; then
   they are read into that room. False if fewer than two are left of them. */
int chapters_measure(int fd, const struct mp3entry *id3, size_t *size);
bool chapters_read(int fd, const struct mp3entry *id3, struct chapters *c,
                   int slots, size_t size);

/* The playing track's, by its id3, if it is an episode or a book: 0 chapters
   without them, as for a song whatever its tags say */
int chapters_count(const struct mp3entry *id3);
/* The chapter at this time, -1 before the first or without chapters. A jump
   lands where the codec can, some way before the chapter's start, and paused
   it stays there: for the playing position the chapter jumped to is the
   answer until playback is past its start or another seek is asked for. */
int chapters_at(const struct mp3entry *id3, unsigned long elapsed);
unsigned long chapters_start(const struct mp3entry *id3, int chapter);
/* its name, NULL without one */
const char *chapters_name(const struct mp3entry *id3, int chapter,
                          char *buf, size_t size);
/* true once as the track goes into another chapter */
bool chapters_changed(struct mp3entry *id3);
/* Next (1): to the next chapter's start. Previous (-1): to this chapter's
   start, or to the one before in its first three seconds. False where there
   is no such chapter, for the caller to skip as for a track without. */
bool chapters_skip(const struct mp3entry *id3, int direction);
/* The list of the playing track's chapters: 0 as it's left, GO_TO_WPS after
   a chapter is chosen, GO_TO_ROOT */
int chapters_screen(void);

#endif /* _CHAPTERS_H_ */
