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
#ifndef _CPU_TIME_H
#define _CPU_TIME_H

#include "config.h"
#include <stdint.h>
#include "thread.h"

/* How long the CPU ran at each clock and which threads held it boosted, for
   the health log. Kept by set_cpu_frequency() and cpu_boost() under their
   lock, on whichever core calls them. PP502x only: the clocks below are its
   own. */
#if defined(HAVE_ADJUSTABLE_CPU_FREQ) && defined(CPU_PP502x) \
    && (CONFIG_PLATFORM & PLATFORM_NATIVE)
#define HAVE_CPU_TIME

enum
{
    CPU_TIME_24MHZ, /* CPUFREQ_DEFAULT, with the PLL off */
    CPU_TIME_30MHZ, /* CPUFREQ_NORMAL */
    CPU_TIME_80MHZ, /* CPUFREQ_MAX, boosted */
    CPU_TIME_CLOCKS
};

#define CPU_TIME_NAME  16 /* a thread's name, cut to fit */
#define CPU_TIME_ENDED 4  /* names kept for threads that ended */
#define CPU_TIME_HELD  (MAXTHREADS + CPU_TIME_ENDED + 1)

struct cpu_time
{
    uint64_t usec[CPU_TIME_CLOCKS];
    unsigned int raised; /* times it went up to 80 MHz */
    /* the threads' names, and how long each had a boost of its own up: by
       slot, the slot's last thread that boosted; then threads that ended
       and left their slot to another, by name; the last, with no name, for
       any more of those */
    struct
    {
        char name[CPU_TIME_NAME];
        uint64_t usec;
    } held[CPU_TIME_HELD];
};

/* From set_cpu_frequency() only, as the clock changes */
void cpu_time_clock(long frequency);

/* The time since the last take, which starts anew: from a thread, as it
   takes the frequency's lock */
void cpu_time_take(struct cpu_time *t);
#endif

#endif /* _CPU_TIME_H */
