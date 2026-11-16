/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Sergei Sarkisian
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
#ifndef STUB_SYSTEM_H
#define STUB_SYSTEM_H

#include "stub_env.h"

#define MIN(a, b) (((a)<(b))?(a):(b))
#define MAX(a, b) (((a)>(b))?(a):(b))
/* firmware/export/system.h's, on the iPod's 32-bit long, which the firmware's
   -fno-strict-overflow wraps round: the host's long is 64 bits */
#define TIME_AFTER(a,b)  ((int32_t)((uint32_t)(b) - (uint32_t)(a)) < 0)
#define TIME_BEFORE(a,b) TIME_AFTER(b,a)

#define inl(a)    stub_inl(a)
#define outl(v,a) stub_outl(v, a)
void udelay(unsigned int usecs);

#endif
