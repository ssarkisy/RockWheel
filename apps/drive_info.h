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
#ifndef _DRIVE_INFO_H
#define _DRIVE_INFO_H

#include "config.h"

/* What the drive says of itself and how soon it comes on, in
   ROCKBOX_DIR "/drive.txt" (drive_info.c) */

#if (CONFIG_STORAGE & STORAGE_ATA) && (CONFIG_PLATFORM & PLATFORM_NATIVE) \
    && defined(HAVE_ATA_POWER_OFF)
/* at boot, once the settings are in: what was learnt of this drive before, and
   the file written for one it isn't of */
void drive_info_init(void);
/* at USB and at power off: written again if more was learnt */
void drive_info_flush(void);
#else
static inline void drive_info_init(void)
{
}
static inline void drive_info_flush(void)
{
}
#endif

#endif
