/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Linus Nielsen Feltzing
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
#include <stdlib.h>
#include "cpu.h"
#include "kernel.h"
#include "thread.h"
#include "i2c.h"
#include "debug.h"
#include "rtc.h"
#include "usb.h"
#include "power.h"
#include "system.h"
#include "button.h"
#include "timer.h"
#include "backlight.h"
#include "backlight-target.h"

static int brightness = 1;   /* 1 to 32 */
static int current_dim = 16; /* default after enabling the backlight dimmer */
static bool enabled = false;

void backlight_hw_brightness(int val)
{
    int oldlevel;

    /* With the circuit off there's nothing to tell, and the pulses would leave its
       line high: it's told when it comes on. Night Mode changes this at any time. */
    if (!enabled)
    {
        brightness = val;
        return;
    }

    if (current_dim < val)
    {
        do
        {
            oldlevel = disable_irq_save();
            GPIO_CLEAR_BITWISE(GPIOD_OUTPUT_VAL, 0x80);
            udelay(10);
            GPIO_SET_BITWISE(GPIOD_OUTPUT_VAL, 0x80);
            restore_irq(oldlevel);
            udelay(10);
        }
        while (++current_dim < val);
    }
    else if (current_dim > val)
    {
        do
        {
            oldlevel = disable_irq_save();
            GPIO_CLEAR_BITWISE(GPIOD_OUTPUT_VAL, 0x80);
            udelay(200);
            GPIO_SET_BITWISE(GPIOD_OUTPUT_VAL, 0x80);
            restore_irq(oldlevel);
            udelay(10);
        }
        while (--current_dim > val);
    }
    brightness = val;
}

void _backlight_hw_enable(bool on)
{
#ifdef HAVE_LCD_SLEEP
    if (on)
        /* If the fade-out is interrupted, enabled will be true, but 
           lcd_awake() needs to be called anyways because the LCD 
           may be sleeping.
         */
        lcd_awake();
#endif

    if (on == enabled)
        return;

    if (on)
    {
        GPIO_SET_BITWISE(GPIOB_OUTPUT_VAL, 0x08);
        GPIO_SET_BITWISE(GPIOD_OUTPUT_VAL, 0x80);
        sleep(HZ/100);
        current_dim = 16;
        enabled = true;
        backlight_hw_brightness(brightness);
    }
    else
    {
        GPIO_CLEAR_BITWISE(GPIOD_OUTPUT_VAL, 0x80);
        GPIO_CLEAR_BITWISE(GPIOB_OUTPUT_VAL, 0x08);
        sleep(HZ/20);
    }
    enabled = on;
}

#ifdef HAVE_HANG_CATCHER
#include "hang.h"
/* The HANG screen's light (panic.c), interrupts off: the circuit on with the
   clock's waits instead of sleep(), the panel already woken by
   lcd_awake_crash(), at the brightness set; the LED steady, as the fade's
   TIMER2 toggles it no more. By the lines themselves, not enabled, which a
   hang inside _backlight_hw_enable()'s sleeps leaves behind them: the
   dimmer is told its level in steps from the one it has, 16 only after a
   power-up, so a circuit already on keeps current_dim. */
void backlight_hw_crash_on(void)
{
    const bool on = (GPIOB_OUTPUT_VAL & 0x08) && (GPIOD_OUTPUT_VAL & 0x80);
    if (!on)
    {
        /* off, or half off: off whole as long as _backlight_hw_enable()
           keeps it so, then on from the dimmer's power-up level */
        GPIO_CLEAR_BITWISE(GPIOD_OUTPUT_VAL, 0x80);
        GPIO_CLEAR_BITWISE(GPIOB_OUTPUT_VAL, 0x08);
        udelay(50000);
        GPIO_SET_BITWISE(GPIOB_OUTPUT_VAL, 0x08);
        GPIO_SET_BITWISE(GPIOD_OUTPUT_VAL, 0x80);
        udelay(10000);
        current_dim = 16;
    }
    else if (!enabled)
    {
        /* switched on, its wait cut short before the level was set */
        udelay(10000);
        current_dim = 16;
    }
    enabled = true;
    backlight_hw_brightness(brightness);
    _backlight_led_on();
}

/* and out, as _backlight_off_normal() without its sleep */
void backlight_hw_crash_off(void)
{
    _backlight_led_off();
    GPIO_CLEAR_BITWISE(GPIOD_OUTPUT_VAL, 0x80);
    GPIO_CLEAR_BITWISE(GPIOB_OUTPUT_VAL, 0x08);
    enabled = false;
}
#endif

/* Switch the backlight on. Works only if the backlight circuit is enabled.
 * Called in ISR context for fading, so it must be fast. */
void _backlight_led_on(void)
{
    GPIO_SET_BITWISE(GPIOL_OUTPUT_VAL, 0x80);
}

/* Switch the backlight off. Keeps the backlight circuit enabled.
 * Called in ISR context for fading, so it must be fast. */
void _backlight_led_off(void)
{
    GPIO_CLEAR_BITWISE(GPIOL_OUTPUT_VAL, 0x80);
}

bool backlight_hw_init(void)
{
    GPIO_SET_BITWISE(GPIOB_ENABLE, 0x08);
    GPIO_SET_BITWISE(GPIOB_OUTPUT_EN, 0x08);
    GPIO_SET_BITWISE(GPIOD_ENABLE, 0x80);
    GPIO_SET_BITWISE(GPIOD_OUTPUT_EN, 0x80);
    _backlight_hw_enable(true);
    GPIO_SET_BITWISE(GPIOL_ENABLE, 0x80);
    GPIO_SET_BITWISE(GPIOL_OUTPUT_EN, 0x80);
    GPIO_SET_BITWISE(GPIOL_OUTPUT_VAL, 0x80);

    return true;
}
