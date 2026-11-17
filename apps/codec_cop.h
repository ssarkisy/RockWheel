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
#ifndef CODEC_COP_H
#define CODEC_COP_H

#include "config.h"
#include <stdbool.h>
#include <stddef.h>

/* A codec's run on the second core: its codec_run() goes on a thread of the
   COP, while the thread that called codec_cop_run() hosts it on the CPU,
   doing what the codec asks of playback (codec_cop.c) */
#if NUM_CORES > 1 && (CONFIG_PLATFORM & PLATFORM_NATIVE) \
    && !defined(BOOTLOADER)
#define HAVE_CODEC_COP

struct codec_api;

/* codec_cop_run()'s status when the codec didn't start on the COP */
#define CODEC_COP_NOT_RUN (-100)

struct codec_cop_result
{
    const char *why;     /* why it didn't run there, or NULL */
    bool commanded;      /* a command or a seek's end reached the codec */
    bool made_pcm;       /* it gave PCM */
    bool at_end;         /* it had read the file to its end */
    bool bounce_iram;    /* the CPU's bounce buffer was in the codec's IRAM */
    size_t stack_used;   /* the most of its stack the run used, in bytes */
};

/* Runs the loaded codec on the COP, with host's functions done on the CPU
   by this thread; stereo_mode and depth as the codec configured them at its
   load. msg_waiting, if any, tells when a message waits for the codec, which
   then gets its get_command() from host; NULL asks host at every one.
   Returns the codec's status, or CODEC_COP_NOT_RUN with nothing done. */
int codec_cop_run(struct codec_api *host, int stereo_mode, int depth,
                  bool (*msg_waiting)(void), struct codec_cop_result *res);

/* A message for the hosting thread, about to be queued: wakes it if a run
   goes on */
void codec_cop_kick(void);

/* The switch: formats that run on the COP; the others stay on the CPU */
bool codec_cop_wanted(int afmt);

/* A track's way, for the health log: on the COP, or on the CPU and why */
void codec_cop_count(int afmt, bool on_cop, const char *why);
/* a run there that the codec's error ended after sound began or a seek */
void codec_cop_note_ended(int afmt);
/* the counts since the last time, as a line of the health log */
void codec_cop_note_counts(void);
#endif /* NUM_CORES > 1 && PLATFORM_NATIVE */

#endif /* CODEC_COP_H */
