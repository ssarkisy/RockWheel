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
 * The click wheel's reading comes from button-clickwheel.c:
 * Copyright (C) 2002 by Daniel Stenberg
 * iPod driver based on code from the ipodlinux project - http://ipodlinux.org
 * Copyright (c) 2003-2005 Bernard Leach (leachbj@bouncycastle.org)
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
#include "system.h"
#include "kernel.h"
#include "wheel_policy.h"

/* WHEEL_ACCEL_FACTOR = 2^16 / WHEEL_ACCEL_START
 *
 * WHEEL_ACCEL_START: the speed in degrees a second, configured per target,
 * above which a driver that measures degrees a second starts accelerating.
 * The factor is scaled by 1<<16 for the 32-bit integer operations below. */
#define WHEEL_ACCEL_FACTOR (1<<16)/WHEEL_ACCEL_START

bool wheel_data_staged(unsigned int data)
{
    return (data & WHEEL_DATA_WHEEL) && (data & WHEEL_DATA_STAGED);
}

int wheel_data_speed(unsigned int data)
{
    if ((data & WHEEL_DATA_WHEEL) == 0)
        return 0;
    if (data & WHEEL_DATA_STAGED)
        return data & (WHEEL_DATA_STAGED - 1);

    /* read driver's velocity from data */
    unsigned int v = data & 0xffffff;

    /* v = 28.4 fixed point */
    v = (WHEEL_ACCEL_FACTOR * v)>>(16-4);

    /* Calculate real numbers item to scroll based upon acceleration
     * setting, use correct roundoff */
#if   (WHEEL_ACCELERATION == 1)
    v = (v*v     + (1<< 7))>> 8;
#elif (WHEEL_ACCELERATION == 2)
    v = (v*v*v   + (1<<11))>>12;
#elif (WHEEL_ACCELERATION == 3)
    v = (v*v*v*v + (1<<15))>>16;
#endif

    return v;
}

int wheel_data_rows(unsigned int data)
{
    int delta = wheel_data_steps(data);
    const int v = wheel_data_speed(data);

    if (v > 1)
        delta *= wheel_data_staged(data) ? MIN(v, WHEEL_STAGE_ROWS) : v;

    return delta;
}

int wheel_event_steps(unsigned int data)
{
    if (wheel_data_is_wheel(data))
        return MAX(1, wheel_data_steps(data));
    return 1;
}

/* A short list, as a menu, goes a row a step, without the steps the UI was
   too busy for, through which the main menu went two and three at once, and
   faster not either: Apple's patent (US 7,312,785) doesn't speed up a list of
   twenty or fewer, four of its screens; two screens here. A long one goes as
   far as the wheel did, and further: by the square root of how many times
   LONG_LIST it is, 13000 songs five times, 1700 albums twice; with the
   wheel's stages, they double on to that many times their most. Turned
   slowly, the wheel still goes a row a step. */
#define LONG_LIST 500

struct wheel_rows wheel_list_rows(unsigned int data, int items, int lines)
{
    struct wheel_rows r;

    if (wheel_data_is_wheel(data) && items <= 2 * lines)
        data = 1u << 24;
    /* a step at least, as for a remote's next and previous, of no data */
    r.rows = MAX(1, wheel_data_rows(data));
    r.speed = wheel_data_speed(data);
    r.steps = MAX(1, wheel_data_steps(data));
    if (r.speed > 1 && items > LONG_LIST)
    {
        const unsigned target = (unsigned)items * 256 / LONG_LIST;
        unsigned scale = 16; /* in sixteenths */
        while ((scale + 1) * (scale + 1) <= target)
            scale++;
        if (wheel_data_staged(data))
            r.rows = wheel_data_steps(data)
                     * MIN(r.speed, WHEEL_STAGE_ROWS * (int)scale / 16);
        else
            r.rows = r.rows * (int)scale / 16;
    }
    return r;
}

#if CONFIG_KEYPAD == IPOD_4G_PAD && !defined(IPOD_MINI)
/* The click wheel's readings, for button-clickwheel.c. Times are 32 bits
   that come round, USEC_TIMER's each 72 minutes: compared as TIME_AFTER()
   does, on the difference */

#define WHEEL_FAST_OFF_TIMEOUT   250000 /* timeout for acceleration = 250ms */
#define WHEEL_REPEAT_TIMEOUT     250000 /* timeout for button repeat = 250ms */
#define WHEEL_UNTOUCH_TIMEOUT    150000 /* timeout for untouching wheel = 150ms */
#define USEC_TICKS(usec)         ((usec) / (1000000 / HZ))
/* degree/sec: no thumb is faster, but the position can jump by a quarter of the wheel
   as a finger lifts, which would make the list leap */
#define WHEEL_SPEED_MAX          2160
/* A reading after none for longer, of a wheel touched all the while: from where
   the finger is then, and not a jump of all it went meanwhile. The wheel's link
   was reset, or the finger was off it a moment: then the list went 5 rows at
   once, turned slowly */
#define WHEEL_GAP_USEC 60000
/* Over accel_start (WHEEL_ACCEL_START), a turn a second, each stage_usec
   (WHEEL_STAGE_USEC) of it doubles the rows a step goes, to 1 << WHEEL_STAGES,
   as far as a list lets it; slower, a turn the other way or a pause, and it's
   a row again. The doubling and its eight rows a step at most are Apple's
   patent's (US 7,312,785), which gives no speed or time: the speed and the
   time are RockWheel's, and the pause's 250 ms Rockbox's (FS#7738) */
#define WHEEL_STAGES     6
/* The steps held back while the queue is full, a turn at most: after the UI
   stood a second, all of them at the stage it was at went thousands of rows.
   A clear of the queue drops them too, as it does what it had. */
#define WHEEL_CARRY_MAX(config) ((unsigned int)(config)->clicks)
/* The UI standing still: an event not taken for this long, while one is drawn
   in under 60 ms. The stages don't go on then, as the list doesn't show them:
   stopped whenever the queue was full, they didn't go on as a long list was
   drawn whole each step */
#define WHEEL_STALL_USEC 150000
/* The dead zone (wheel_config.dead_zone, Wheel Sensitivity's Low and
   Medium), Apple's iPod classic 6G wheel decoder's as read from its disassembly
   (github.com/davidmonterocrespo24/qemu-ipod-classic,
   include/hw/arm/ipod_classic/ipod_classic_wheel.h): at a touch's start,
   and after any lift, a reading's move of WHEEL_DZ_PASS places goes through
   at once; smaller ones add up, and once they come to WHEEL_DZ those are
   dropped and what is past them goes on. Here also after a press's quiet,
   and after the gap rule's fresh start, as from a touch */
#define WHEEL_DZ_PASS 6
#define WHEEL_DZ      4

static inline bool after(uint32_t a, uint32_t b)
{
    return (int32_t)(b - a) < 0;
}

void wheel_read(struct wheel_state *s, const struct wheel_config *config,
                const struct wheel_input *in, struct wheel_event *ev)
{
    ev->direction = 0;
    ev->repeat = false;
    ev->post = false;
    ev->steps = 0;
    ev->new_touch = false;

    /* a button down, or let go under WHEEL_PRESS_QUIET_USEC ago: from the
       first reading without it, as the controller may send none while a
       finger holds a button still. Kept in a game too, which has no quiet
       (button_wheel_press_quiet()): Menu let go there, as a game quits on
       its release, is quiet in the screen it goes back to. The ticks since
       then counted unsigned, as after() would hold it on once 2^31 of them
       had gone by without a reading */
    if (wheel_packet_pressed(in->packet))
        s->pressed = s->down = true;
    else if (s->down)
    {
        s->down = false;
        s->release_tick = in->tick;
    }
    else if (s->pressed && in->tick - s->release_tick
                           > (uint32_t)USEC_TICKS(WHEEL_PRESS_QUIET_USEC))
        s->pressed = false;

    if (!wheel_packet_touched(in->packet))
    {
        /* In this case the finger was lifted from the scrollwheel. */
        s->dz_armed = true;
        s->dz_acc = 0;
        if (s->touched)
            s->untouch_tick = in->tick;
        s->touched = false;
        ev->stage = s->stage;
        return;
    }

    const uint32_t usec = in->usec;
    const int position = wheel_packet_position(in->packet);
    const int before = s->position;     /* the dead zone's */
    s->position = position;

    /* Check whether the scrollwheel was untouched by accident or by will. */
    /* This is needed because wheel may be untoched very shortly during rotation */
    if (!s->touched
        && after(in->tick, s->step_tick + USEC_TICKS(WHEEL_UNTOUCH_TIMEOUT)))
    {
        /* wheel has been really untouched -> reset internal variables */
        s->old      = -1;
        s->velocity = 0;
        s->stage    = 0;
        s->carry    = 0;
        s->last_dir = 0;
    }
    else
    {
        /* wheel was shortly untouched by accident -> leave internal variables */
        s->touched = true;
    }

    const uint32_t since_reading = (usec - s->reading_usec) & 0x7fffffff;
    s->reading_usec = usec;

    if (s->old < 0)
    {
        /* scrollwheel was touched for the first time after finger lifting */
        s->old = position;
        s->touched = true;
        s->dz_armed = true;
        s->dz_acc = 0;
        ev->new_touch = true;
        ev->stage = s->stage;
        return;
    }

    if (s->pressed && in->press_quiet)
    {
        /* as from a touch where the finger is */
        s->old      = position;
        s->delta    = 0;
        s->carry    = 0;
        s->velocity = 0;
        s->stage    = 0;
        s->last_dir = 0;
        s->dz_armed = true;
        s->dz_acc   = 0;
        ev->stage = s->stage;
        return;
    }

    if (config->dead_zone && s->dz_armed)
    {
        /* the finger followed until it has gone through the zone */
        int move = position - before;
        if (move < -config->clicks / 2)
            move += config->clicks;
        else if (move > config->clicks / 2)
            move -= config->clicks;
        if (move >= WHEEL_DZ_PASS || move <= -WHEEL_DZ_PASS)
            s->dz_armed = false;
        else
        {
            s->dz_acc += move;
            if (s->dz_acc < WHEEL_DZ && s->dz_acc > -WHEEL_DZ)
            {
                s->old = position;
                s->delta = 0;
                ev->stage = s->stage;
                return;
            }
            const int past = s->dz_acc > 0 ? s->dz_acc - WHEEL_DZ
                                           : s->dz_acc + WHEEL_DZ;
            s->old = (position - past + config->clicks) % config->clicks;
            s->dz_armed = false;
        }
    }

    const int clicks = config->clicks;
    const int sensitivity = config->sensitivity;
    int delta = position - s->old;

    /* Taking into account wrapping during transition from highest
     * to lowest wheel position and back */
    if      (delta < -clicks/2)
        delta += clicks; /* Forward wrapping case */
    else if (delta >  clicks/2)
        delta -= clicks; /* Backward wrapping case */
    /* two steps and more at once after none for a while: turned
       slowly, readings far apart are of a click or two */
    if (since_reading > WHEEL_GAP_USEC
        && (delta >= 2 * sensitivity || delta <= -2 * sensitivity))
    {
        s->old = position;
        delta = 0;
        /* a fresh start, so the dead zone's again: the move that passed it
           is the one dropped here */
        s->dz_armed = true;
        s->dz_acc = 0;
    }
    s->delta = delta;
    /* the steps it went, and what's left of one stays: a step
       is sensitivity of the wheel's 96 places, 6 at Wheel
       Sensitivity's Low (16 a turn) and 4 at Medium and High
       (24), however its readings come. 15 degrees a row is
       Apple's web iPod's, and the research's best fit for
       Apple's 5.5G firmware run in an emulator, with a dead zone
       at a touch's start, as Low and Medium have here */
    const int direction = delta < 0 ? -1 : 1;

    /* Fine, half a step as far (button_wheel_fine), but the first
       one way, after a touch, the other way or a pause: a whole
       step still, sensitivity places, as a finger at rest went
       back and forth by twos, or crept on */
    const int step = in->fine ? sensitivity / 2 : sensitivity;
    const bool going_on =
        s->last_dir == direction
        && after(s->step_tick + USEC_TICKS(WHEEL_REPEAT_TIMEOUT), in->tick);
    const int first = going_on ? step : sensitivity;

    /* Getting the direction from the delta.
     * Need at least some clicks to be sure to avoid haptic fuzziness */
    if (delta < first && delta > -first)
    {
        ev->stage = s->stage;
        return;
    }

    /* the time since the last step, in 31 bits */
    int32_t v = (usec - s->step_usec) & 0x7fffffff;

    /* undo signedness, and whole steps only */
    delta = delta > 0 ? delta : -delta;
    delta -= delta % step;
    s->delta = delta;

    /* add the current delta */
    if (in->queue_clears != s->seen_clears)
    {
        s->seen_clears = in->queue_clears;
        s->carry = 0;
    }
    s->carry = MIN(s->carry + delta, WHEEL_CARRY_MAX(config));

    v = v ? (1000000 * delta) / v : 0;  /* clicks/sec = 1000000 * clicks/usec */
    /* conversion to degree/sec, wrapping round in 32 bits as the firmware's
       arithmetic does (-fno-strict-overflow): only readings under 9 us apart
       would go past them */
    v = (int32_t)((uint32_t)v * 360u) / clicks;
    v = (v<0) ? -v : v;                 /* undo signedness */
    v = MIN(v, WHEEL_SPEED_MAX);

    /* some velocity filtering to smooth things out, but slowing
       down to land on an item counts at once: over a turn passed
       with big steps otherwise */
    if (v < (int32_t)s->velocity / 2)
        s->velocity = (s->velocity + (uint32_t)v) / 2;
    else
        s->velocity = (15 * s->velocity + (uint32_t)v) / 16;
    /* limit to 24 bit */
    s->velocity = MIN(s->velocity, 0xffffffu);

    /* direction reversals must nullify acceleration and accumulator */
    if (direction != s->last_dir)
    {
        s->last_dir = direction;
        s->velocity = 0;
        s->carry    = 0;
    }
    /* on same direction REPEAT is assumed when new click is within timeout */
    else if (after(s->step_tick + USEC_TICKS(WHEEL_REPEAT_TIMEOUT), in->tick))
    {
        ev->repeat = true;
    }
    /* timeout nullifies acceleration and accumulator */
    if (after(in->tick, s->step_tick + USEC_TICKS(WHEEL_FAST_OFF_TIMEOUT)))
    {
        s->velocity = 0;
        s->carry    = 0;
    }

    if (s->velocity < config->accel_start)
    {
        s->stage = 0;
        s->stage_usec = usec;
    }
    /* no further while the UI stands still and doesn't show it */
    else if (!in->queue_empty && after(usec, s->post_usec + WHEEL_STALL_USEC))
        s->stage_usec = usec;
    else if (s->stage < WHEEL_STAGES
             && after(usec, s->stage_usec + config->stage_usec))
    {
        s->stage++;
        s->stage_usec = usec;
    }

    /* The queue should have no other events when scrolling */
    if (in->send_events && in->queue_empty)
    {
        /* each sensitivity's clicks = scrolling 1 item, half as many fine.
           The post's count is the steps since the last one posted: those the
           queue was too full for went nowhere, the list behind the finger as
           the UI drew; and the rows each goes, at the stage it's at */
        ev->post = true;
        ev->steps = MAX(1u, MIN(s->carry / step, 127u));
        s->post_usec = usec;
        s->carry = 0;
    }
    /* a screen that reads where the wheel is has the steps */
    if (!in->send_events)
        s->carry = 0;

    ev->direction = direction;
    ev->stage = s->stage;
    s->step_usec = usec;
    s->step_tick = in->tick;
    s->old = (s->old + direction * delta + clicks) % clicks;
}
#endif /* IPOD_4G_PAD */
