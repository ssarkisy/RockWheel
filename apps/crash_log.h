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
#ifndef _CRASH_LOG_H
#define _CRASH_LOG_H

#include "config.h"
#include "system.h"

#ifdef HAVE_PANIC_RECORD
/* At boot, the disk mounted and the clock read, before music: the record a
   crash left, if any, marked logged and appended to CRASH_LOG_FILE */
void crash_log_boot(void);

/* The thread named by the record crash_log_boot() logged at this boot, or
   NULL for none: the last boot ended otherwise than by a crash it caught */
const char *crash_log_boot_thread(void);
#else
static inline void crash_log_boot(void)
{
}

static inline const char *crash_log_boot_thread(void)
{
    return NULL;
}
#endif

#endif /* _CRASH_LOG_H */
