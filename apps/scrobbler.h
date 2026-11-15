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
#ifndef _SCROBBLER_H_
#define _SCROBBLER_H_

#include "config.h"
#include "metadata.h"

/* The plays for Last.fm, in /.scrobbler.log (Audioscrobbler's Portable
   Player Logging 1.1) when Settings > Playback > Last.fm is on. They wait in
   memory and go to the disk at USB and at shutdown, and otherwise at the
   disk's own idle, never waking it: with the music stopped or paused, and
   while it plays only with the log 3/4 full, the battery low, or Last.fm
   switched off with lines waiting. With a clock and the database: the plays
   are timed, and podcasts and books told apart. */
#if CONFIG_RTC && defined(HAVE_TAGCACHE)
#define HAVE_SCROBBLER
/* Its memory, about 18 KB, with Last.fm on: taken at boot by settings_apply()
   or the .cfg's callback, before music, and as it is switched on, when a
   playing track starts again where it was; given back with it off, once no
   line and no play waits in it. Without it nothing is logged.
   scrobbler_setting() is the setting's callback, which tries again after a
   refusal. */
void scrobbler_apply(bool on);
void scrobbler_setting(bool on);
/* playback.c, on the audio thread: a track begins (its TRACK_CHANGE), and it
   ends (its TRACK_FINISH, with that event's flags); scrobbler_stopping()
   comes just before the finish of a stop, scrobbler_stopped() after it */
void scrobbler_track_change(const struct mp3entry *id3);
void scrobbler_track_finish(const struct mp3entry *id3, unsigned int flags);
void scrobbler_stopping(void);
void scrobbler_stopped(void);
/* paused, or playing again */
void scrobbler_pause(bool pause);
/* The playing track's position as it sounds: from the PCM interrupt as each
   chunk starts to play, and from the codec's and the audio thread without
   the PCM lock (audio_pcmbuf_sync_position()), which may lose or count twice
   one step */
void scrobbler_position(unsigned long elapsed);
/* a seek is taken, on the audio thread */
void scrobbler_seek(void);
/* what waits in memory to the log, and the play a stop left waiting to its
   file: at USB, before the computer has the disk, and at shutdown; and at
   the disk's idle, as above */
void scrobbler_flush(void);
#else
static inline void scrobbler_apply(bool on) { (void)on; }
static inline void scrobbler_setting(bool on) { (void)on; }
static inline void scrobbler_track_change(const struct mp3entry *id3)
{ (void)id3; }
static inline void scrobbler_track_finish(const struct mp3entry *id3,
                                          unsigned int flags)
{ (void)id3; (void)flags; }
static inline void scrobbler_stopping(void) {}
static inline void scrobbler_stopped(void) {}
static inline void scrobbler_pause(bool pause) { (void)pause; }
static inline void scrobbler_position(unsigned long elapsed) { (void)elapsed; }
static inline void scrobbler_seek(void) {}
static inline void scrobbler_flush(void) {}
#endif

#endif /* _SCROBBLER_H_ */
