/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Alan Korr
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
#include "config.h"
#include "system.h"
#include <stdio.h>
#include "kernel.h"
#include "thread.h"
#include "string.h"
#include "string-extra.h"
#include "file.h"
#include "cpu_time.h"

#ifndef CPU_FREQ
#define CPU_FREQ (-1)
#endif

long cpu_frequency SHAREDBSS_ATTR = CPU_FREQ;

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
static int boost_counter SHAREDBSS_ATTR = 0;
static bool cpu_idle SHAREDBSS_ATTR = false;
/* the sleep's, apart from cpu_idle (the radio's; and USB's on targets without
   the USB stack): neither ends the other's */
static bool cpu_sleep SHAREDBSS_ATTR = false;

bool cpu_unboosted_is_low(void)
{
    return cpu_idle || cpu_sleep;
}

static long unboosted_frequency(void)
{
    return cpu_unboosted_is_low() ? CPUFREQ_DEFAULT : CPUFREQ_NORMAL;
}

int get_cpu_boost_counter(void)
{
    return boost_counter;
}

#ifdef HAVE_CPU_TIME
/* Written under the frequency's lock by whichever core changes the clock or
   boosts: uncached, as boost_counter is, so that neither core's cached copy
   of a line, written back, undoes what the other wrote (922737a0e9) */
static struct
{
    uint64_t usec[CPU_TIME_CLOCKS];
    unsigned long since_usec;  /* the clock's, from its change or the take */
    long since_tick;
    unsigned int clock;        /* CPU_TIME_* + 1; 0 before the first change */
    unsigned int raised;
    struct holder
    {
        uint64_t usec;         /* its holds ended since the take */
        unsigned long since_usec; /* of the hold up: its start or the take */
        long since_tick;
        unsigned int thread;   /* the id of the slot's last thread to boost */
        unsigned int boosts;   /* its boosts up */
        char name[CPU_TIME_NAME]; /* that thread's */
    } held[CPU_TIME_HELD];     /* as struct cpu_time's */
} cpu_time_kept SHAREDBSS_ATTR;

/* From *since to now, which it becomes: in microseconds, or in ticks from an
   hour on, as the microsecond timer wraps in 71 minutes */
static uint64_t time_since(unsigned long *since_usec, long *since_tick,
                           unsigned long now_usec, long now_tick)
{
    const unsigned long usec = now_usec - *since_usec;
    const unsigned long ticks = now_tick - *since_tick;
    *since_usec = now_usec;
    *since_tick = now_tick;
    return ticks < 3600 * HZ ? usec : (uint64_t)ticks * (1000000 / HZ);
}

void cpu_time_clock(long frequency)
{
    const unsigned long now_usec = USEC_TIMER;
    const long now_tick = current_tick;
    /* as set_cpu_frequency() goes: any other is 24 MHz, the PLL off;
       CPUFREQ_SLEEP, which nothing asks for, counts as 24 too */
    const unsigned int clock = 1 + (frequency == CPUFREQ_MAX ? CPU_TIME_80MHZ
                                  : frequency == CPUFREQ_NORMAL ? CPU_TIME_30MHZ
                                  : CPU_TIME_24MHZ);
    if (cpu_time_kept.clock)
        cpu_time_kept.usec[cpu_time_kept.clock - 1] +=
            time_since(&cpu_time_kept.since_usec, &cpu_time_kept.since_tick,
                       now_usec, now_tick);
    else
    {
        cpu_time_kept.since_usec = now_usec;
        cpu_time_kept.since_tick = now_tick;
    }
    if (clock == 1 + CPU_TIME_80MHZ && cpu_time_kept.clock != clock)
        cpu_time_kept.raised++;
    cpu_time_kept.clock = clock;
}

/* Where the time of an ended thread goes as another takes its slot: the
   ended threads' holder with its name, or a free one, or the last, unnamed */
static struct holder *ended_holder(const char *name)
{
    struct holder *e = &cpu_time_kept.held[MAXTHREADS];
    struct holder *spare = NULL;
    for (; e < &cpu_time_kept.held[CPU_TIME_HELD - 1]; e++)
    {
        if (!e->name[0])
        {
            if (!spare)
                spare = e;
        }
        else if (!strcmp(e->name, name))
            return e;
    }
    if (!spare || !name[0])
        return e;
    strcpy(spare->name, name);
    return spare;
}

/* cpu_boost()'s caller has a boost more or fewer up, under the lock. One
   undone by a thread with none up is taken from the first that has one, as
   the boost is only a count: Moving Cover's, for one, is raised by the scroll
   thread and may be undone by the UI's. Time can then shift between the two
   threads, by about as long as their holds overlap; the totals stay right,
   as a hold is open only while the count is above 0. */
static void cpu_time_boost(bool on)
{
    const unsigned long now_usec = USEC_TIMER;
    const long now_tick = current_tick;
    const unsigned int self = thread_self();
    struct holder *h = &cpu_time_kept.held[THREAD_ID_SLOT(self)];

    if (h->thread != self) /* a new thread in the slot, once in its life */
    {
        /* a thread made again, as the dircache's after each USB, goes on in
           its holder; another's time goes to an ended thread's by its name */
        const char *name = thread_self_name();
        if (!name)
            name = "";
        if (strncmp(h->name, name, sizeof(h->name) - 1))
        {
            if (h->usec || h->boosts)
            {
                struct holder *gone = ended_holder(h->name);
                gone->usec += h->usec;
                if (h->boosts) /* left up: still held, by no thread now */
                {
                    gone->usec += time_since(&h->since_usec, &h->since_tick,
                                             now_usec, now_tick);
                    if (!gone->boosts)
                    {
                        gone->since_usec = now_usec;
                        gone->since_tick = now_tick;
                    }
                    gone->boosts += h->boosts;
                }
            }
            h->usec = 0;
            h->boosts = 0;
            strlcpy(h->name, name, sizeof(h->name));
        }
        h->thread = self;
    }

    if (on)
    {
        if (!h->boosts++)
        {
            h->since_usec = now_usec;
            h->since_tick = now_tick;
        }
        return;
    }

    if (!h->boosts)
    {
        for (h = cpu_time_kept.held; h->boosts == 0; h++)
            if (h == &cpu_time_kept.held[CPU_TIME_HELD - 1])
                return; /* none up: the counter is at 0 already */
    }
    if (!--h->boosts)
        h->usec += time_since(&h->since_usec, &h->since_tick,
                              now_usec, now_tick);
}

void cpu_time_take(struct cpu_time *t)
{
    memset(t, 0, sizeof(*t));
    if (!cpu_boost_lock())
        return;

    const unsigned long now_usec = USEC_TIMER;
    const long now_tick = current_tick;
    for (int c = 0; c < CPU_TIME_CLOCKS; c++)
    {
        t->usec[c] = cpu_time_kept.usec[c];
        cpu_time_kept.usec[c] = 0;
    }
    if (cpu_time_kept.clock)
        t->usec[cpu_time_kept.clock - 1] +=
            time_since(&cpu_time_kept.since_usec, &cpu_time_kept.since_tick,
                       now_usec, now_tick);
    t->raised = cpu_time_kept.raised;
    cpu_time_kept.raised = 0;

    for (int i = 0; i < CPU_TIME_HELD; i++)
    {
        struct holder *h = &cpu_time_kept.held[i];
        memcpy(t->held[i].name, h->name, sizeof(t->held[i].name));
        t->held[i].usec = h->usec;
        h->usec = 0;
        if (h->boosts)
            t->held[i].usec += time_since(&h->since_usec, &h->since_tick,
                                          now_usec, now_tick);
        else if (i >= MAXTHREADS) /* an ended thread's, told: free again */
            h->name[0] = '\0';
    }

    cpu_boost_unlock();
}
#else
static inline void cpu_time_boost(bool on)
{
    (void)on;
}
#endif /* HAVE_CPU_TIME */
#ifdef CPU_BOOST_LOGGING
#define MAX_BOOST_LOG 64
static char cpu_boost_calls[MAX_BOOST_LOG][MAX_PATH];
static int cpu_boost_first = 0;
static int cpu_boost_calls_count = 0;
static int cpu_boost_track_message = 0;
int cpu_boost_log_getcount(void)
{
    return cpu_boost_calls_count;
}
char * cpu_boost_log_getlog_first(void)
{
    char *first;
    cpu_boost_lock();

    first = NULL;

    if (cpu_boost_calls_count)
    {
        cpu_boost_track_message = 1;
        first = cpu_boost_calls[cpu_boost_first];
    }

    cpu_boost_unlock();
    return first;
}

char * cpu_boost_log_getlog_next(void)
{
    int message;
    char *next;

    cpu_boost_lock();

    message = (cpu_boost_track_message+cpu_boost_first)%MAX_BOOST_LOG;
    next = NULL;

    if (cpu_boost_track_message < cpu_boost_calls_count)
    {
        cpu_boost_track_message++;
        next = cpu_boost_calls[message];
    }

    cpu_boost_unlock();
    return next;
}

void cpu_boost_(bool on_off, char* location, int line)
{
    int item = cpu_boost_calls_count;
    if (!cpu_boost_lock())
        return;

    if (cpu_boost_calls_count == MAX_BOOST_LOG)
    {
        cpu_boost_first = (cpu_boost_first+1)%MAX_BOOST_LOG;
        cpu_boost_calls_count--;
        if (cpu_boost_calls_count < 0)
            cpu_boost_calls_count = 0;
        item += cpu_boost_first;
    }
    if (cpu_boost_calls_count < MAX_BOOST_LOG)
    {
        int message = (cpu_boost_first+cpu_boost_calls_count)%MAX_BOOST_LOG;
        snprintf(cpu_boost_calls[message], MAX_PATH,"%d.) %c %d %s:%d",
                 item,on_off?'B':'U',boost_counter,location,line);
        cpu_boost_calls_count++;
    }
#else
void cpu_boost(bool on_off)
{
    if (!cpu_boost_lock())
        return;

#endif /* CPU_BOOST_LOGGING */
    cpu_time_boost(on_off);
    if(on_off)
    {
        /* Boost the frequency if not already boosted */
        if(++boost_counter == 1)
            set_cpu_frequency(CPUFREQ_MAX);
    }
    else
    {
        /* Lower the frequency if the counter reaches 0 */
        if(--boost_counter <= 0)
        {
            set_cpu_frequency(unboosted_frequency());

            /* Safety measure */
            if (boost_counter < 0)
            {
                boost_counter = 0;
            }
        }
    }

    cpu_boost_unlock();
}

void cpu_idle_mode(bool on_off)
{
    if (!cpu_boost_lock())
        return;

    cpu_idle = on_off;

    /* We need to adjust the frequency immediately if the CPU
       isn't boosted */
    if(boost_counter == 0)
        set_cpu_frequency(unboosted_frequency());

    cpu_boost_unlock();
}

void cpu_sleep_mode(bool on_off)
{
    if (!cpu_boost_lock())
        return;

    cpu_sleep = on_off;
    if (boost_counter == 0)
        set_cpu_frequency(unboosted_frequency());

    cpu_boost_unlock();
}
#endif /* HAVE_ADJUSTABLE_CPU_FREQ */
