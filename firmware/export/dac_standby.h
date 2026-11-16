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
#ifndef _DAC_STANDBY_H_
#define _DAC_STANDBY_H_

#include <stdbool.h>
#include "config.h"

/* The codec's PLL, DAC, mixers and outputs off while no PCM plays, its bias
   and VMID kept so that the outputs stay where they were. The codec is the
   I2S clock master: with its PLL off nothing clocks the FIFO, so every start
   of PCM wakes it first, and only the mixer starts PCM. */

enum dac_standby_mode
{
    DAC_STANDBY_OFF,   /* never */
    DAC_STANDBY_SLEEP, /* in the iPod's sleep only (powermgmt.c) */
    DAC_STANDBY_IDLE,  /* once PCM has stopped a few seconds */
};

#ifdef HAVE_DAC_STANDBY
void dac_standby_set_mode(enum dac_standby_mode mode);
/* from the power thread: into standby if it's due */
void dac_idle_poll(bool sleeping);
/* before any start of PCM, never under pcm_play_lock(): from standby it waits
   on I2C, in a thread; awake it returns at once, from an interrupt too */
void dac_wake(void);
bool dac_in_standby(void);
/* from a thread: awake now, and no standby until let go; for sound that starts
   PCM from an interrupt (the USB DAC) */
void dac_standby_keep_awake(bool on);
/* times sink_stop_pcm() gave up waiting for the FIFO: a start that missed
   dac_wake() */
extern unsigned int dac_fifo_gave_up;
#else
static inline void dac_standby_set_mode(enum dac_standby_mode mode)
{
    (void)mode;
}
static inline void dac_idle_poll(bool sleeping)
{
    (void)sleeping;
}
static inline void dac_wake(void)
{
}
static inline bool dac_in_standby(void)
{
    return false;
}
static inline void dac_standby_keep_awake(bool on)
{
    (void)on;
}
#endif

#endif /* _DAC_STANDBY_H_ */
