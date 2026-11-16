/***************************************************************************
*             __________               __   ___.
*   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
*   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
*   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
*   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
*                     \/            \/     \/    \/            \/
*
* Copyright (C) 2005 Jens Arnold
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

#include <stdbool.h>
#include <limits.h>
#include "config.h"
#include "cpu.h"
#include "system.h"
#include "timer.h"
#include "logf.h"

/* Free: taken at any priority, a negative one too. The iPod's wheel click
   registers below the backlight fade's 0, so a fade can take it from a click. */
#define TIMER_PRIO_FREE INT_MIN

static int timer_prio = TIMER_PRIO_FREE;
void SHAREDBSS_ATTR (*pfn_timer)(void) = NULL;      /* timer callback */
void SHAREDBSS_ATTR (*pfn_unregister)(void) = NULL; /* unregister callback */

/* Register a user timer, called every <cycles> TIMER_FREQ cycles */
bool timer_register(int reg_prio, void (*unregister_callback)(void),
                    long cycles, void (*timer_callback)(void)
                    IF_COP(, int core))
{
    if (reg_prio <= timer_prio || cycles == 0)
        return false;

    if (!timer_set(cycles, true))
        return false;

    pfn_timer = timer_callback;
    /* NOTE: if unreg cb is defined you are in charge of calling timer_unregister() */
    pfn_unregister = unregister_callback;
    timer_prio = reg_prio;

    return timer_start(IF_COP(core));
}

bool timer_set_period(long cycles)
{
    return timer_set(cycles, false);
}

/* NOTE: unregister callbacks are not called by timer_unregister(), except
* a negative priority's: any timer_unregister(), its owner's too, takes the
* timer from it. Some plugins unregister without owning the timer, which
* would drop the interrupt that ends the iPod wheel click's tone.
* Otherwise the unregister_callback only gets called when your timer gets
* overwritten by a higher priority timer using timer_register() */
void timer_unregister(void)
{
    void (*lost)(void) = timer_prio < 0 ? pfn_unregister : NULL;

    timer_stop();

    pfn_timer = NULL;
    pfn_unregister = NULL;
    timer_prio = TIMER_PRIO_FREE;

    if (lost)
        lost();
}

