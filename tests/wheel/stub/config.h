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
/* The iPod Video's configuration, as far as button-clickwheel.c reads it */
#ifndef STUB_CONFIG_H
#define STUB_CONFIG_H

#define IPOD_4G_PAD 4
#define CONFIG_KEYPAD IPOD_4G_PAD
#define PP5022  5022
#define S5L8701 8701
#define S5L8702 8702
#define S5L8720 8720
#define CONFIG_CPU PP5022
#define CPU_PP
#define IPOD_VIDEO
#define IPOD_ARCH
#define HAVE_SCROLLWHEEL
#define HAVE_WHEEL_ACCELERATION
#define WHEEL_ACCEL_START 360
#define WHEEL_ACCELERATION 3
#define HAVE_WHEEL_POSITION
#define IPOD_ACCESSORY_PROTOCOL

#endif
