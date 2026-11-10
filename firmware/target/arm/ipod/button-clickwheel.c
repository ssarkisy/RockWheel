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

#define WHEEL_FAST_OFF_TIMEOUT   250000 /* timeout for acceleration = 250ms */
#define WHEEL_REPEAT_TIMEOUT     250000 /* timeout for button repeat = 250ms */
#define WHEEL_UNTOUCH_TIMEOUT    150000 /* timeout for untouching wheel = 150ms */
/* degree/sec: no thumb is faster, but the position can jump by a quarter of the wheel
   as a finger lifts, which would make the list leap */
#define WHEEL_SPEED_MAX          2160

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

int  old_wheel_value  = -1;
int  new_wheel_value  = 0;
static int  repeat           = 0;
int  wheel_delta      = 0;
bool wheel_is_touched = false;
unsigned int  accumulated_wheel_delta = 0;
static unsigned int  wheel_repeat            = 0;
unsigned int  wheel_velocity          = 0;
static unsigned long last_wheel_usec         = 0;
/* A reading after none for longer, of a wheel touched all the while: from where
   the finger is then, and not a jump of all it went meanwhile. The wheel's link
   was reset, or the finger was off it a moment: then the list went 5 rows at
   once, turned slowly */
#define WHEEL_GAP_USEC 60000
static unsigned long last_reading_usec       = 0;
/* As Apple's: over WHEEL_ACCEL_START, a turn a second, each WHEEL_STAGE_USEC of
   it doubles the rows a step goes, to 1 << WHEEL_STAGES, as far as a list lets
   it; slower, a turn the other way or a pause, and it's a row again */
#define WHEEL_STAGE_USEC 300000
#define WHEEL_STAGES     6
int wheel_accel_stage = 0;
static unsigned long stage_since_usec        = 0;
/* The steps held back while the queue is full, a turn at most: after the UI
   stood a second, all of them at the stage it was at went thousands of rows.
   A clear of the queue drops them too, as it does what it had. */
#define WHEEL_CARRY_MAX  WHEELCLICKS_PER_ROTATION
static unsigned int seen_clears              = 0;
/* The UI standing still: an event not taken for this long, while one is drawn
   in under 60 ms. The stages don't go on then, as the list doesn't show them:
   stopped whenever the queue was full, they didn't go on as a long list was
   drawn whole each step */
#define WHEEL_STALL_USEC 150000
static unsigned long last_post_usec          = 0;
/* the last step's tick, for what's timed from it but its speed: USEC_TIMER
   comes round each 72 minutes, after which a touch went on from a flick of
   then, at its stage */
static long last_step_tick                   = 0;

/* Variable to use for setting button status in interrupt handler */
static int int_btn = BUTTON_NONE;
#ifdef HAVE_WHEEL_POSITION
    static int wheel_position = -1;
    static bool send_events = true;
    static long untouch_tick;
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
            if (status & 0x40000000) 
            {
                unsigned long usec = USEC_TIMER;
                
                /* Highest wheel = 0x5F, clockwise increases */
                new_wheel_value = (status >> 16) & 0x7f;
                whl = new_wheel_value;
                
                if (!wheel_is_touched)
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
                
                /* Check whether the scrollwheel was untouched by accident or by will. */
                /* This is needed because wheel may be untoched very shortly during rotation */
                if ( (!wheel_is_touched)
                     && TIME_AFTER(current_tick, last_step_tick + WHEEL_UNTOUCH_TIMEOUT / (1000000 / HZ)) )
                {
                    /* wheel has been really untouched -> reset internal variables */
                    old_wheel_value         = -1;
                    wheel_velocity          = 0;
                    wheel_accel_stage       = 0;
                    accumulated_wheel_delta = 0;
                    wheel_repeat            = BUTTON_NONE;
                }
                else
                {
                    /* wheel was shortly untouched by accident -> leave internal variables */
                    wheel_is_touched = true;
                }

                const unsigned long since_reading = (usec - last_reading_usec) & 0x7fffffff;
                last_reading_usec = usec;

                if (old_wheel_value >= 0)
                {
                    /* This is for later = BUTTON_SCROLL_TOUCH;*/
                    wheel_delta = new_wheel_value - old_wheel_value;
                    unsigned int wheel_keycode = BUTTON_NONE;

                    /* Taking into account wrapping during transition from highest 
                     * to lowest wheel position and back */
                    if      (wheel_delta < -WHEELCLICKS_PER_ROTATION/2)
                        wheel_delta += WHEELCLICKS_PER_ROTATION; /* Forward wrapping case */
                    else if (wheel_delta >  WHEELCLICKS_PER_ROTATION/2)
                        wheel_delta -= WHEELCLICKS_PER_ROTATION; /* Backward wrapping case */
                    /* two steps and more at once after none for a while: turned
                       slowly, readings far apart are of a click or two */
                    if (since_reading > WHEEL_GAP_USEC
                        && (wheel_delta >= 2 * WHEEL_SENSITIVITY
                            || wheel_delta <= -2 * WHEEL_SENSITIVITY))
                    {
                        old_wheel_value = new_wheel_value;
                        wheel_delta = 0;
                    }
                    /* the steps it went, and what's left of one stays: a step
                       is four of the wheel's 96, and a turn 24 of them, however
                       its readings come, as in Apple's own scrolling */
                    const int direction = wheel_delta < 0 ? -1 : 1;
    
                    /* Fine, half a step as far (button_wheel_fine), but the first
                       one way, after a touch, the other way or a pause: four of
                       the 96 still, as a finger at rest went back and forth by
                       twos, or crept on */
                    const int step = button_wheel_is_fine() ? WHEEL_SENSITIVITY / 2
                                                            : WHEEL_SENSITIVITY;
                    const bool going_on =
                        wheel_repeat == (unsigned int)(direction > 0 ? BUTTON_SCROLL_FWD
                                                                     : BUTTON_SCROLL_BACK)
                        && TIME_BEFORE(current_tick, last_step_tick
                                       + WHEEL_REPEAT_TIMEOUT / (1000000 / HZ));
                    const int first = going_on ? step : WHEEL_SENSITIVITY;

                    /* Getting direction and wheel_keycode from wheel_delta.
                     * Need at least some clicks to be sure to avoid haptic fuzziness */
                    if      (wheel_delta >=  first)
                        wheel_keycode = BUTTON_SCROLL_FWD;
                    else if (wheel_delta <= -first)
                        wheel_keycode = BUTTON_SCROLL_BACK;
                    else 
                        wheel_keycode = BUTTON_NONE;

                    if (wheel_keycode != BUTTON_NONE)
                    {
                        long v = (usec - last_wheel_usec) & 0x7fffffff;
                        /* turned: it's to be seen */
                        if (!button_dark_wheel() || is_backlight_on(false))
                            backlight_on();
                        
                        /* undo signedness, and whole steps only */
                        wheel_delta = (wheel_delta>0) ? wheel_delta : -wheel_delta;
                        wheel_delta -= wheel_delta % step;
                        
                        /* add the current wheel_delta */
                        if (button_queue_clears() != seen_clears)
                        {
                            seen_clears = button_queue_clears();
                            accumulated_wheel_delta = 0;
                        }
                        accumulated_wheel_delta = MIN(accumulated_wheel_delta + wheel_delta,
                                                      WHEEL_CARRY_MAX);

                        v = v ? (1000000 * wheel_delta) / v : 0;  /* clicks/sec = 1000000 * clicks/usec */
                        v = (v * 360) / WHEELCLICKS_PER_ROTATION; /* conversion to degree/sec */
                        v = (v<0) ? -v : v;                       /* undo signedness */
                        v = MIN(v, WHEEL_SPEED_MAX);
            
                        /* some velocity filtering to smooth things out, but slowing
                           down to land on an item counts at once: over a turn passed
                           with big steps otherwise */
                        if (v < (long)wheel_velocity / 2)
                            wheel_velocity = (wheel_velocity + v) / 2;
                        else
                            wheel_velocity = (15 * wheel_velocity + v) / 16;
                        /* limit to 24 bit */
                        wheel_velocity = (wheel_velocity>0xffffff) ? 0xffffff : wheel_velocity;

                        /* assume REPEAT = off */
                        repeat = 0;
                        
                        /* direction reversals must nullify acceleration and accumulator */
                        if (wheel_keycode != wheel_repeat)
                        {
                            wheel_repeat            = wheel_keycode;
                            wheel_velocity          = 0;
                            accumulated_wheel_delta = 0;
                        }
                        /* on same direction REPEAT is assumed when new click is within timeout */
                        else if (TIME_BEFORE(current_tick, last_step_tick + WHEEL_REPEAT_TIMEOUT / (1000000 / HZ)))
                        {
                            repeat = BUTTON_REPEAT;
                        }
                        /* timeout nullifies acceleration and accumulator */
                        if (TIME_AFTER(current_tick, last_step_tick + WHEEL_FAST_OFF_TIMEOUT / (1000000 / HZ)))
                        {
                            wheel_velocity          = 0;
                            accumulated_wheel_delta = 0;
                        }

                        if (wheel_velocity < WHEEL_ACCEL_START)
                        {
                            wheel_accel_stage = 0;
                            stage_since_usec = usec;
                        }
                        /* no further while the UI stands still and doesn't show it */
                        else if (!button_queue_empty()
                                 && TIME_AFTER(usec, last_post_usec + WHEEL_STALL_USEC))
                            stage_since_usec = usec;
                        else if (wheel_accel_stage < WHEEL_STAGES
                                 && TIME_AFTER(usec, stage_since_usec + WHEEL_STAGE_USEC))
                        {
                            wheel_accel_stage++;
                            stage_since_usec = usec;
                        }

#ifdef HAVE_WHEEL_POSITION
                        if (send_events) 
#endif
                        /* The queue should have no other events when scrolling */
                        if (button_queue_empty())
                        {
                            /* each WHEEL_SENSITIVITY clicks = scrolling 1 item,
                               half as many fine */
                            accumulated_wheel_delta /= step;
#ifdef HAVE_SCROLLWHEEL
                            /* use data-format for HAVE_SCROLLWHEEL */
                            /* always use acceleration mode (1<<31) */
                            /* the message post count is the steps since the last
                               one posted: those the queue was too full for went
                               nowhere, the list behind the finger as the UI drew;
                               and the rows each goes, at the stage it's at */
                            const unsigned int steps =
                                MAX(1u, MIN(accumulated_wheel_delta, 127u));
                            button_queue_post(wheel_keycode | repeat,
                                       (1<<31) | (steps << 24) | WHEEL_DATA_STAGED
                                       | (1u << wheel_accel_stage));
                            last_post_usec = usec;

#else
                            button_queue_post(wheel_keycode | repeat, 
                                       (accumulated_wheel_delta << 16) | new_wheel_value);
#endif
                            accumulated_wheel_delta = 0;
                        }
#ifdef HAVE_WHEEL_POSITION
                        /* a screen that reads where the wheel is has the steps */
                        if (!send_events)
                            accumulated_wheel_delta = 0;
#endif
                        last_wheel_usec = usec;
                        last_step_tick = current_tick;
                        old_wheel_value = (old_wheel_value + direction * wheel_delta
                                           + WHEELCLICKS_PER_ROTATION)
                                          % WHEELCLICKS_PER_ROTATION;
                    }
                }
                else
                {
                    /* scrollwheel was touched for the first time after finger lifting */
                    old_wheel_value = new_wheel_value;
                    wheel_is_touched = true;
                }
            }
            else
            {
                /* In this case the finger was lifted from the scrollwheel. */
#ifdef HAVE_WHEEL_POSITION
                if (wheel_is_touched)
                    untouch_tick = current_tick;
#endif
                wheel_is_touched = false; 
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
    return wheel_is_touched ? 0 : current_tick - untouch_tick;
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
