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
#ifndef _WHEEL_POLICY_H_
#define _WHEEL_POLICY_H_

/* The wheel's policy without its hardware: what the click wheel's readings
 * make of a turn (steps, the acceleration's stages, the steps held while
 * the UI is busy), how an event's data carries it, and how far a list goes
 * for it. Its functions read nothing but their arguments, so the host tests
 * (tests/wheel) run them as the iPod does. */

#include <stdbool.h>
#include <stdint.h>

/* An event's data, as a driver with HAVE_WHEEL_ACCELERATION posts it:
 *    [31] use acceleration: on every post of the click wheel's; a driver that
 *         measures degrees a second sets it only when turned fast enough
 * [30:24] the steps it went, those the UI was too busy for included (1-127)
 *    [23] staged: the speed is in the rows a step goes, as Apple's patent
 *         has it (US 7,312,785); a driver that measures degrees a
 *         second leaves it clear
 *  [22:0] the speed: degrees a second, or the rows a step goes */
#define WHEEL_DATA_WHEEL  (1u << 31)
#define WHEEL_DATA_STAGED (1u << 23)
/* the most rows a step goes by the wheel's stages, in a list of up to 500 */
#define WHEEL_STAGE_ROWS 8

/* bit 31: the click wheel's every step, a degree driver's fast ones */
static inline bool wheel_data_is_wheel(unsigned int data)
{
    return (data & WHEEL_DATA_WHEEL) != 0;
}

/* the steps, those the UI was too busy for included; of any data */
static inline int wheel_data_steps(unsigned int data)
{
    return (data >> 24) & 0x7f;
}

/* the click wheel's: so many steps, each 1 << stage rows */
static inline unsigned int wheel_data_make(unsigned int steps,
                                           unsigned int stage)
{
    return WHEEL_DATA_WHEEL | (steps << 24) | WHEEL_DATA_STAGED
           | (1u << stage);
}

/* whether its speed is in stages, the rows a step goes, as the click wheel's
   driver has them, and not in degrees a second */
bool wheel_data_staged(unsigned int data);
/* the items a step goes at the wheel's speed: over 1 when it's fast enough to
   go further; 0 without bit 31 */
int wheel_data_speed(unsigned int data);
/* the steps the wheel went, each as far as its speed takes it: by stages,
   eight times at most, as Apple's patent (US 7,312,785) suggests */
int wheel_data_rows(unsigned int data);
/* the steps of a wheel event without its speed, as the volume and the
   scrubber take them; 1 without bit 31: a remote's, or a degree driver's
   slow step */
int wheel_event_steps(unsigned int data);

/* What a wheel event moves a list by */
struct wheel_rows {
    int rows;   /* the rows the selection goes, 1 at least for any post */
    int steps;  /* the steps without the speed, 1 at least: a letter each */
    int speed;  /* wheel_data_speed(), 0 in a short list */
};
/* for a list of `items` rows, `lines` of them on the screen; `lines` is read
   only for the wheel's data, so the caller may pass anything for another's */
struct wheel_rows wheel_list_rows(unsigned int data, int items, int lines);

/* The click wheel's readings (button-clickwheel.c) */

/* a packet's finger and its place, highest 0x5F, clockwise increasing */
#define WHEEL_PACKET_TOUCHED 0x40000000u
static inline bool wheel_packet_touched(uint32_t packet)
{
    return (packet & WHEEL_PACKET_TOUCHED) != 0;
}
static inline int wheel_packet_position(uint32_t packet)
{
    return (packet >> 16) & 0x7f;
}
/* and its buttons: Select 0x100, ⏭ 0x200, ⏮ 0x400, Play 0x800, Menu 0x1000 */
#define WHEEL_PACKET_BUTTONS 0x00001f00u
static inline bool wheel_packet_pressed(uint32_t packet)
{
    return (packet & WHEEL_PACKET_BUTTONS) != 0;
}

/* A button down, and this long after the first reading with none, to the
   tick: the finger pressing, rolling and lifting off isn't a turn. Menu, Play
   and ⏮/⏭ are pressed on the ring itself, and a thumb on Select often lies on
   its inner edge; the few places it went were a step, and the list Menu went
   back to opened on its row and moved by one. The wheel follows the finger
   meanwhile, so the next step counts from where it is then, and nothing held
   back comes out later. Not in a game (button_wheel_press_quiet()), which
   turns the wheel with a button held; in every other plugin, Cover Flow,
   Photos and the text viewers among them (plugin_load()). To be tuned on
   the iPod; the host test takes it from here */
#define WHEEL_PRESS_QUIET_USEC 150000

/* what differs by model */
struct wheel_config {
    int clicks;              /* places in a turn: 96 */
    int sensitivity;         /* places a step, at least: by Wheel
                                Sensitivity, 4 or (Low) 6 on the Video and
                                the Photo, 6 or 9 on a nano */
    unsigned int accel_start;/* degrees a second, WHEEL_ACCEL_START */
    uint32_t stage_usec;     /* a stage's time: WHEEL_STAGE_USEC */
    bool dead_zone;          /* the dead zone at a touch's start */
};

/* The driver's. Written only where it reads the wheel, in its interrupt; the
   UI may read it (button_wheel_untouched_ticks(), the debug screen) */
struct wheel_state {
    bool touched;            /* a finger on the wheel */
    int position;            /* the last reading's place */
    int old;                 /* where the steps taken end, -1 before a touch */
    int delta;               /* the last reading's move, or its whole steps */
    unsigned int carry;      /* places gone since the last post */
    unsigned int velocity;   /* degrees a second, smoothed */
    int stage;               /* the acceleration's stage: 1 << stage rows */
    int last_dir;            /* the last step's way, 0 after a touch */
    unsigned int seen_clears;/* button_queue_clears() when carry was taken */
    uint32_t step_usec;      /* the last step's time */
    uint32_t reading_usec;   /* the last reading's */
    uint32_t stage_usec;     /* when the stage was last reached or held */
    uint32_t post_usec;      /* the last post's */
    uint32_t step_tick;      /* the last step's tick */
    uint32_t untouch_tick;   /* when the finger lifted */
    bool pressed;            /* a button down, or let go a moment ago: no
                                steps, the wheel following the finger, but in
                                a game (press_quiet) */
    bool down;               /* a button down at the last reading */
    uint32_t release_tick;   /* the first reading with none after one */
    bool dz_armed;           /* the dead zone not yet gone through */
    int dz_acc;              /* the places gone in it */
};
#define WHEEL_STATE_INIT { .old = -1 }

/* A reading, with what the driver knows as it comes */
struct wheel_input {
    uint32_t packet;         /* the controller's word */
    uint32_t usec;           /* USEC_TIMER, read for a touched packet */
    uint32_t tick;           /* current_tick */
    bool fine;               /* button_wheel_is_fine() */
    bool press_quiet;        /* button_wheel_is_press_quiet(), of any packet */
    bool queue_empty;        /* button_queue_empty() */
    unsigned int queue_clears; /* button_queue_clears() */
    bool send_events;        /* false while a screen reads the wheel itself */
};

/* What it makes of it */
struct wheel_event {
    int direction;           /* a step: 1 forward, -1 back, 0 none */
    bool repeat;             /* on the same way: BUTTON_REPEAT */
    bool post;               /* to post: wheel_data_make(steps, stage) */
    int steps;               /* the steps since the last post, 1-127 */
    int stage;
    bool new_touch;          /* the first reading of a touch: no step yet */
};

void wheel_read(struct wheel_state *s, const struct wheel_config *config,
                const struct wheel_input *in, struct wheel_event *ev);

/* the click wheel driver's state, for the debug screen */
const struct wheel_state *button_wheel_state(void);
/* Settings > General > Wheel Sensitivity, its values in the setting's order:
   Low, a step of 6 places and the dead zone; Medium, 4 and the dead zone;
   High, 4 and none, the wheel as it was. Taken at the driver's next reading.
   The games (their folder, plugin_load()) run at High, made for the wheel
   as it was; every other plugin, as Cover Flow, Photos and the text
   viewers, keeps the setting */
enum {
    WHEEL_SENSITIVITY_LOW, WHEEL_SENSITIVITY_MEDIUM, WHEEL_SENSITIVITY_HIGH
};
void button_wheel_sensitivity(int sensitivity);
/* the one given last, which a plugin's run gives back after it */
int button_wheel_get_sensitivity(void);

#endif /* _WHEEL_POLICY_H_ */
