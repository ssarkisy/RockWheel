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
/* PP5022's registers that button-clickwheel.c touches */
#ifndef STUB_CPU_H
#define STUB_CPU_H

#include "stub_env.h"

#define USEC_TIMER       (stub_env.usec)
#define GPIOA_INPUT_VAL  (stub_env.gpioa)
#define DEV_RS           (stub_regs[0])
#define DEV_EN           (stub_regs[1])
#define DEV_INIT1        (stub_regs[2])
#define CPU_INT_EN       (stub_regs[3])
#define CPU_HI_INT_EN    (stub_regs[4])
#define CPU_HI_INT_DIS   (stub_regs[5])
#define GPIOA_ENABLE     (stub_regs[6])
#define GPIOA_OUTPUT_EN  (stub_regs[7])
#define DEV_OPTO         0x00010000
#define INIT_BUTTONS     0x00040000
#define HI_MASK          (1 << 30)
#define I2C_MASK         (1 << 8)

#endif
