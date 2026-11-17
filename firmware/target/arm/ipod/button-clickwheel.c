/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Daniel Stenberg
 *
 * iPod driver based on code from the ipodlinux project - http://ipodlinux.org
 * Adapted for Rockbox in December 2005
 * Original file: linux/arch/armnommu/mach-ipod/keyboard.c
 * Copyright (c) 2003-2005 Bernard Leach (leachbj@bouncycastle.org)
 *
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

/*
 * Rockbox button functions
 */

#include <stdlib.h>
#include <limits.h>
#include "config.h"
#include "cpu.h"
#include "system.h"
#include "button.h"
#include "kernel.h"
#include "backlight.h"
#include "serial.h"
#include "power.h"
#include "powermgmt.h"
#include "wheel_policy.h"
#ifdef IPOD_NANO2G
#include "pmu-target.h"
#endif
#if defined(IPOD_6G) || defined(IPOD_NANO3G) || defined(IPOD_NANO4G)
#include "pmu-target.h"
#include "clocking-s5l8702.h"
#endif
#ifdef HAVE_MIKEY_REMOTE
#include "mikey-target.h"    /* inline earphone remote */
#endif

#ifdef CPU_PP
#define CLICKWHEEL_DATA   (*(volatile unsigned long*)(0x7000c140))
#elif CONFIG_CPU==S5L8701 || CONFIG_CPU==S5L8702 || CONFIG_CPU==S5L8720
#define CLICKWHEEL_DATA   WHEELRX
#else
#error CPU architecture not supported!
#endif

#define WHEELCLICKS_PER_ROTATION     96 /* wheelclicks per full rotation */

/* This amount of clicks is needed for at least scrolling 1 item. Choose small values 
 * to have high sensitivity but few precision, choose large values to have less 
 * sensitivity and good precision. */
#if defined(IPOD_NANO) || defined(IPOD_NANO2G)
#define WHEEL_SENSITIVITY 6 /* iPod nano has smaller wheel, lower sensitivity needed */
#else
#define WHEEL_SENSITIVITY 4 /* default sensitivity */
#endif

/* Over WHEEL_ACCEL_START, a turn a second, each WHEEL_STAGE_USEC of it doubles
   the rows a step goes, as far as a list lets it (wheel_policy.c). 0.4 s, the
   RockWheel Photo port's, chosen by the owner on the Video; RockWheel had
   0.3 s before. Apple's patent (US 7,312,785) gives no time */
#define WHEEL_STAGE_USEC 400000

/* What the wheel's turns make (wheel_policy.c): its state, written only here,
   in the interrupt */
static struct wheel_state wheel = WHEEL_STATE_INIT;
static const struct wheel_config wheel_config = {
    .clicks      = WHEELCLICKS_PER_ROTATION,
    .sensitivity = WHEEL_SENSITIVITY,
    .accel_start = WHEEL_ACCEL_START,
    .stage_usec  = WHEEL_STAGE_USEC,
};

/* Variable to use for setting button status in interrupt handler */
static int int_btn = BUTTON_NONE;
#ifdef HAVE_WHEEL_POSITION
    static int wheel_position = -1;
    static bool send_events = true;
#endif

#if CONFIG_CPU==S5L8701 || CONFIG_CPU==S5L8702 || CONFIG_CPU==S5L8720
static struct semaphore button_init_wakeup;
#endif

#ifdef CPU_PP
static void opto_i2c_init(void)
{
    DEV_EN |= DEV_OPTO;
    DEV_RS |= DEV_OPTO;
    udelay(5);
    DEV_RS &= ~DEV_OPTO; /* finish reset */
    DEV_INIT1 |= INIT_BUTTONS; /* enable buttons (needed for "hold"-detection) */

    outl(0xc00a1f00, 0x7000c100);
    outl(0x01000000, 0x7000c104);
}
#endif

/* With the screen dark, a touch lights it once the wheel has gone a step, or
   once the finger has stayed a moment without pressing a button: Play and
   ⏮/⏭, which work without looking, are pressed on the ring itself, and their
   touch lit the screen before the press came, or as ⏭ was held. A button that
   lights it does so itself (button_tick). */
#define TOUCH_LIGHT_USEC 300000
static unsigned long touch_since_usec;
static bool touch_play; /* a button was pressed in this touch */

static inline int ipod_4g_button_read(void)
{
    int whl = -1;
    int btn = BUTTON_NONE;

#ifdef CPU_PP    
    if ((inl(0x7000c104) & 0x04000000) != 0) 
    {
#endif
        unsigned status = CLICKWHEEL_DATA;

        if ((status & 0x800000ff) == 0x8000001a) 
        {
            struct wheel_input in = { .packet = status };
            struct wheel_event ev;

            if (status & 0x00000100)
                btn |= BUTTON_SELECT;
            if (status & 0x00000200)
                btn |= BUTTON_RIGHT;
            if (status & 0x00000400)
                btn |= BUTTON_LEFT;
            if (status & 0x00000800)
                btn |= BUTTON_PLAY;
            if (status & 0x00001000)
                btn |= BUTTON_MENU;
            if (wheel_packet_touched(status))
            {
                unsigned long usec = USEC_TIMER;
                
                /* Highest wheel = 0x5F, clockwise increases */
                whl = wheel_packet_position(status);
                
                if (!wheel.touched)
                {
                    touch_since_usec = usec;
                    touch_play = false;
                }
                if (btn)
                    touch_play = true;
                /* switch on backlight (again), reset power-off timer; on Now
                   Playing, as Wake Screen may have it, not with the volume */
                if (is_backlight_on(false)
                    || (!touch_play && !button_dark_wheel()
                        && TIME_AFTER(usec, touch_since_usec + TOUCH_LIGHT_USEC)))
                    backlight_on();
                reset_poweroff_timer();

                in.usec = usec;
                in.fine = button_wheel_is_fine();
                in.queue_empty = button_queue_empty();
                in.queue_clears = button_queue_clears();
#ifdef HAVE_WHEEL_POSITION
                in.send_events = send_events;
#else
                in.send_events = true;
#endif
            }
            /* the steps it went, the stages and what the queue had no
               room for (wheel_policy.c); and a finger lifted */
            in.tick = current_tick;
            in.press_quiet = button_wheel_is_press_quiet();
            wheel_read(&wheel, &wheel_config, &in, &ev);

            if (ev.direction)
            {
                /* turned: it's to be seen */
                if (!button_dark_wheel() || is_backlight_on(false))
                    backlight_on();
                if (ev.post)
                    button_queue_post((ev.direction > 0 ? BUTTON_SCROLL_FWD
                                                        : BUTTON_SCROLL_BACK)
                                      | (ev.repeat ? BUTTON_REPEAT : 0),
                                      wheel_data_make(ev.steps, ev.stage));
            }
        }
        else if ((status & 0x800000ff) != 0x8000003a &&
                 status != 0xFFFFFFFF)
        {
            udelay(2000);
            outl(inl(0x7000c100) & ~0x60000000, 0x7000c100);
            outl(inl(0x7000c104) | 0x04000000, 0x7000c104);
            outl(inl(0x7000c100) | 0x60000000, 0x7000c100);
        }
#if CONFIG_CPU==S5L8701 || CONFIG_CPU==S5L8702 || CONFIG_CPU==S5L8720
        else if ((status & 0x8000FFFF) == 0x8000023A)
        {
            if (status & 0x00010000)
                btn |= BUTTON_SELECT;
            if (status & 0x00020000)
                btn |= BUTTON_RIGHT;
            if (status & 0x00040000)
                btn |= BUTTON_LEFT;
            if (status & 0x00080000)
                btn |= BUTTON_PLAY;
            if (status & 0x00100000)
                btn |= BUTTON_MENU;
            semaphore_release(&button_init_wakeup);
        }
#endif

#ifdef CPU_PP    
    }
#endif

#ifdef HAVE_WHEEL_POSITION
    /* Save the new absolute wheel position */
    wheel_position = whl;
#endif
    return btn;
}

const struct wheel_state *button_wheel_state(void)
{
    return &wheel;
}

#ifdef HAVE_WHEEL_POSITION
int wheel_status(void)
{
    return wheel_position;
}
 
void wheel_send_events(bool send)
{
    send_events = send;
}

long button_wheel_untouched_ticks(void)
{
    /* the sensor is off, and no reading would say it's let go */
    if (button_hold())
        return LONG_MAX;
    return wheel.touched ? 0 : (long)((uint32_t)current_tick - wheel.untouch_tick);
}
#endif

#ifdef CPU_PP
void ipod_4g_button_int(void)
{
    CPU_HI_INT_DIS = I2C_MASK;

    /* The following delay was 250 in the ipodlinux source, but 50 seems to 
       work fine - tested on Nano, Color/Photo and Video. */
    udelay(50);

    int_btn = ipod_4g_button_read();
    
    outl(inl(0x7000c104) | 0x0c000000, 0x7000c104);
    outl(0x400a1f00, 0x7000c100);

    CPU_HI_INT_EN = I2C_MASK;
}

void button_init_device(void)
{
    opto_i2c_init();

    /* fixes first button press being ignored */
#if defined(IPOD_4G) || defined(IPOD_COLOR)
    outl(inl(0x7000c100) & ~0x60000000, 0x7000c100);
    outl(inl(0x7000c104) | 0x04000000, 0x7000c104);
    outl(inl(0x7000c100) | 0x60000000, 0x7000c100);
#endif
    /* hold button - enable as input */
    GPIOA_ENABLE |= 0x20;
    GPIOA_OUTPUT_EN &= ~0x20; 
    
    /* unmask interrupt */
    CPU_INT_EN = HI_MASK;
    CPU_HI_INT_EN = I2C_MASK;
}

bool button_hold(void)
{
    return (GPIOA_INPUT_VAL & 0x20)?false:true;
}

bool headphones_inserted(void)
{
    return (GPIOA_INPUT_VAL & 0x80)?true:false;
}
#else
void INT_WHEEL(void)
{
    int clickwheel_events = WHEELINT;

    /* Clear interrupts */
    if (clickwheel_events & 4) WHEELINT = 4;
    if (clickwheel_events & 2) WHEELINT = 2;
    if (clickwheel_events & 1) WHEELINT = 1;

    int_btn = ipod_4g_button_read();
}

static void s5l_clickwheel_init(void)
{
#if CONFIG_CPU==S5L8701
    PWRCONEXT &= ~1;
    PCON15 = (PCON15 & ~0xFFFF0000) | 0x22220000;
    PUNK15 = 0xF0;
    WHEEL08 = 0x3A980;
    WHEEL00 = 0x280000;
    WHEEL10 = 3;
    PCON10 = (PCON10 & ~0xFF0) | 0x10;
    PDAT10 |= 2;
    WHEELTX = 0x8000023A;
    WHEEL04 |= 1;
    PDAT10 &= ~2;
#elif CONFIG_CPU==S5L8702 || CONFIG_CPU==S5L8720
    clockgate_enable(CLOCKGATE_CWHEEL, true);
    PCON14 = (PCON14 & ~0x00ffff00) | 0x00222200;
    WHEEL00 = 0; /* stop s5l8702 controller */
    WHEELINT = 7;
    WHEEL10 = 1;
    WHEEL00 = 0x380000;
    WHEEL08 = 0x20000;
    WHEELTX = 0x8000023A;
    WHEEL04 |= 1;
#endif
}

void button_init_device(void)
{
    semaphore_init(&button_init_wakeup, 1, 0);
#if CONFIG_CPU==S5L8701
    INTMSK |= (1<<26);
#endif
    s5l_clickwheel_init();
    semaphore_wait(&button_init_wakeup, HZ / 10);
#ifdef HAVE_MIKEY_REMOTE
    mikey_init();    /* start inline earphone remote polling */
#endif
}

bool button_hold(void)
{
#if CONFIG_CPU==S5L8701
    bool value = (PDAT14 & (1 << 6)) == 0;
    if (value)
        PCON15 = PCON15 & ~0xffff0000;
    else PCON15 = (PCON15 & ~0xffff0000) | 0x22220000;
    return value;
#elif CONFIG_CPU==S5L8702 || CONFIG_CPU==S5L8720
    return pmu_holdswitch_locked();
#endif
}

bool headphones_inserted(void)
{
#if CONFIG_CPU==S5L8701
    return ((PDAT14 & (1 << 5)) != 0);
#elif CONFIG_CPU==S5L8702 || CONFIG_CPU==S5L8720
#ifdef IPOD_NANO3G
    /* GPIO 7.0, high with headphones in (measured) */
    return (PDAT(7) & (1 << 0)) != 0;
#else
    return ((PDAT10 & (1 << 6)) != 0);
#endif
#endif
}
#endif

/*
 * Get button pressed from hardware
 */
int button_read_device(void)
{
    static bool hold_button = false;
    bool hold_button_old;

    /* normal buttons */
    hold_button_old = hold_button;
    hold_button = button_hold();

    if (hold_button != hold_button_old)
    {
#ifndef BOOTLOADER
        backlight_hold_changed(hold_button);
#endif
        
        if (hold_button)
        {
            /* what was held as it locked isn't any more: with the sensor off,
               nothing would tell it let go, and it was held on, to repeat or
               power off */
            int_btn = BUTTON_NONE;
#ifdef CPU_PP
            /* lock -> disable wheel sensor */
            DEV_EN &= ~DEV_OPTO;
#elif CONFIG_CPU==S5L8701
            /*pmu_ldo_power_off(1);*/ /* disable clickwheel power supply */
            WHEEL00 = 0;
            WHEEL10 = 0;
            PWRCONEXT |= 1;
#elif CONFIG_CPU==S5L8702 || CONFIG_CPU==S5L8720
            WHEEL00 = 0;
            PCON14 = (PCON14 & ~0x00ffff00) | 0x000e0e00;
            clockgate_enable(CLOCKGATE_CWHEEL, false);
#endif
        }
        else
        {
#ifdef CPU_PP
            /* unlock -> enable wheel sensor */
            DEV_EN |= DEV_OPTO;
            opto_i2c_init();
#elif CONFIG_CPU==S5L8701
            /*pmu_ldo_power_on(1);*/ /* enable clickwheel power supply */
            s5l_clickwheel_init();
#elif CONFIG_CPU==S5L8702 || CONFIG_CPU==S5L8720
            s5l_clickwheel_init();
#endif
        }
    }

    /* The int_btn variable is set in the button interrupt handler */
    int btn = int_btn;
#ifdef IPOD_ACCESSORY_PROTOCOL
    btn |= remote_control_rx();
#endif
#ifdef HAVE_MIKEY_REMOTE
    /* not gated by the hold switch, matching the OF: the inline remote
     * keeps working while the device is locked */
    btn |= mikey_button_read();
#endif
    return btn;
}
