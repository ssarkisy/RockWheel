/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Björn Stenberg
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
#ifndef KERNEL_H
#define KERNEL_H

#include "config.h"

#include "system.h"
#include "queue.h"
#include "mutex.h"
#include "mrsw_lock.h"
#include "tick.h"

#ifdef INCLUDE_TIMEOUT_API
#include "timeout.h"
#endif

#ifdef HAVE_SEMAPHORE_OBJECTS
#include "semaphore.h"
#endif

#ifdef HAVE_CORELOCK_OBJECT
#include "corelock.h"
#endif

#define OBJ_WAIT_TIMEDOUT     (-1)
#define OBJ_WAIT_FAILED       0
#define OBJ_WAIT_SUCCEEDED    1

#define TIMEOUT_BLOCK   -1
#define TIMEOUT_NOBLOCK  0

/* The kernel's fixed tables found full since boot (queue.c): for the health
   log, and for the panic record where a panic follows */
enum kernel_table
{
    KERNEL_TABLE_TIMEOUTS,   /* timeout_register(): not registered */
    KERNEL_TABLE_TICK_TASKS, /* tick_add_task(): panics next */
    KERNEL_TABLE_EVENTS,     /* add_event(): panics next */
    KERNEL_TABLE_QUEUES,     /* queue_init() to receive broadcasts: panics
                                next */
    KERNEL_TABLE_COUNT
};

#define KERNEL_FULL_QUEUE_OWNERS 4

struct kernel_overflows
{
    unsigned int tables[KERNEL_TABLE_COUNT];
    /* Events dropped from full queues, by the name the thread waiting on the
       queue had then: the first names to come, then the rest together */
    struct
    {
        unsigned int dropped;
        char owner[16]; /* "" while unused */
    } queues[KERNEL_FULL_QUEUE_OWNERS];
    unsigned int dropped_others;
};

/* Both from either core, in an interrupt or a thread */
void kernel_count_overflow(enum kernel_table table);
void kernel_get_overflows(struct kernel_overflows *copy);

#endif /* KERNEL_H */
