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

/* The click wheel's policy (firmware/drivers/wheel_policy.c) on the Mac.
 *
 * Equivalence: the driver of 1be9ae95b6 (ref/button-clickwheel.c, verbatim)
 * and today's (firmware/target/arm/ipod/button-clickwheel.c), both built
 * against stub/, read the same packets at the same times in the same world;
 * whatever each does (backlight, power-off timer, posts with their data, the
 * buttons, the wheel's place, its debug values) must be the same, in the same
 * order. Then the base's lines of list.c, button.c and wps.c (ref/apps.c)
 * against wheel_policy.c's for the data the drivers posted and more, for
 * lists of every length that matters.
 *
 * Behaviour: wheel_policy.c alone, against what the roadmap says it does,
 * so that the checks mean something once the reference is gone.
 *
 * WHEEL_SEED=<n> runs the random sessions from another seed. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include "config.h"
#include "check.h"
#include "wheel_policy.h"
#include "stub/stub_env.h"
#include "ref_api.h"

/* button-target.h's, as the drivers post them */
#define FWD    0x00000010
#define BACK   0x00000020
#define REPEAT 0x04000000

/* What the drivers do, side 0 the base's and 1 today's */
struct rec { int kind; uint32_t a, b; };
#define TRACE_MAX 32
static struct rec trace[2][TRACE_MAX];
static int trace_n[2];
static int side;

struct stub_env stub_env;
volatile long current_tick;
volatile unsigned long stub_regs[16];
unsigned int ref_action_data;
int ref_nb_lines, ref_nb_lines_calls;

void stub_trace(int kind, uint32_t a, uint32_t b)
{
    if (trace_n[side] < TRACE_MAX)
        trace[side][trace_n[side]] = (struct rec){ kind, a, b };
    trace_n[side]++;
}

uint32_t stub_inl(uint32_t addr)
{
    return addr == 0x7000c104 ? stub_env.c104 : 0;
}
void stub_outl(uint32_t value, uint32_t addr) { stub_trace(T_OUTL, value, addr); }
void udelay(unsigned int usecs) { stub_trace(T_UDELAY, usecs, 0); }
void backlight_on(void) { stub_trace(T_BACKLIGHT_ON, 0, 0); }
bool is_backlight_on(bool ignore_always_off)
{
    return !ignore_always_off && stub_env.lit;
}
void backlight_hold_changed(bool hold) { stub_trace(T_HOLD_CHANGED, hold, 0); }
void reset_poweroff_timer(void) { stub_trace(T_POWEROFF, 0, 0); }
int remote_control_rx(void) { return 0; }
bool button_dark_wheel(void) { return stub_env.dark_wheel; }
void button_queue_post(long id, intptr_t data)
{
    stub_trace(T_POST, (uint32_t)id, (uint32_t)data);
}
bool button_queue_empty(void) { return stub_env.queue_empty; }
unsigned int button_queue_clears(void) { return stub_env.clears; }
bool button_wheel_is_fine(void) { return stub_env.fine; }

/* xorshift64*: the sessions' own, the same on every host */
static uint64_t rng;
static uint64_t rnd(void)
{
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return rng * 0x2545F4914F6CDD1DULL;
}
static uint32_t rnd_in(uint32_t lo, uint32_t hi) /* lo..hi */
{
    return lo + (uint32_t)(rnd() % ((uint64_t)hi - lo + 1));
}
static bool chance(int per_mille)
{
    return (int)(rnd() % 1000) < per_mille;
}

/* The world both drivers live in: the time, the UI taking events from the
   queue as it draws, the screen's settings */
struct world {
    const char *name;
    uint64_t t;           /* us since the start */
    uint32_t usec0;       /* USEC_TIMER at it */
    uint32_t tick0;
    int queue;            /* events waiting */
    uint64_t ui_free;     /* when the UI takes the next */
    uint32_t draw_min, draw_max; /* what an event takes it, us */
    /* what came of it */
    long packets, posts, steps, max_stage;
    long posts_at[7];     /* posts by stage */
    long busy_posts;      /* readings with a step but no room */
    bool failed;
    /* the module alone (run_module): its state */
    struct wheel_state ws;
    bool module;
    /* posts seen by run_module, for the behaviour checks */
    int ev_n;
    struct { uint64_t t; int dir; int steps; int stage; bool repeat; } ev[4096];
    int new_touches;
    /* today's driver alone (behaviour checks of its constants) */
    bool driver_only;
    /* when the stage changed, read after each packet */
    int stage_now, stage_n;
    struct { uint64_t t; int from, to; } stage_change[256];
};

static const struct wheel_config video = {
    .clicks = 96, .sensitivity = 4, .accel_start = WHEEL_ACCEL_START,
    .stage_usec = 300000,
};

static void world_init(struct world *w, const char *name, uint32_t usec0,
                       uint32_t tick0)
{
    memset(w, 0, sizeof(*w));
    w->name = name;
    w->usec0 = usec0;
    w->tick0 = tick0;
    w->draw_min = w->draw_max = 20000;
    w->ws = (struct wheel_state)WHEEL_STATE_INIT;
    stub_env.lit = true;
    stub_env.dark_wheel = false;
    stub_env.fine = false;
    stub_env.gpioa = 0x20;
}

static void ui_take(struct world *w)
{
    while (w->queue > 0 && w->t >= w->ui_free)
    {
        w->queue--;
        w->ui_free = w->t + rnd_in(w->draw_min, w->draw_max);
    }
}

static void world_set_time(struct world *w)
{
    stub_env.usec = w->usec0 + (uint32_t)w->t;
    current_tick = (long)(w->tick0 + (uint32_t)(w->t / 10000));
}

static const char *kind_name(int kind)
{
    static const char *names[] = { "?", "backlight_on", "reset_poweroff",
        "post", "udelay", "outl", "hold_changed" };
    return kind > 0 && kind <= T_HOLD_CHANGED ? names[kind] : "?";
}

static void dump_trace(int s)
{
    for (int i = 0; i < trace_n[s] && i < TRACE_MAX; i++)
        fprintf(stderr, "    %s %08" PRIx32 " %08" PRIx32 "\n",
                kind_name(trace[s][i].kind), trace[s][i].a, trace[s][i].b);
}

static void note_post(struct world *w, uint32_t button, uint32_t data)
{
    if (!(button & (FWD | BACK)))
        return;
    const int stage = __builtin_ctz(data & (WHEEL_DATA_STAGED - 1));
    w->posts++;
    w->steps += wheel_data_steps(data);
    w->posts_at[stage]++;
    if (stage > w->max_stage)
        w->max_stage = stage;
    if (w->ev_n < (int)(sizeof(w->ev) / sizeof(w->ev[0])))
    {
        w->ev[w->ev_n].t = w->t;
        w->ev[w->ev_n].dir = button & FWD ? 1 : -1;
        w->ev[w->ev_n].steps = wheel_data_steps(data);
        w->ev[w->ev_n].stage = stage;
        w->ev[w->ev_n].repeat = (button & REPEAT) != 0;
        w->ev_n++;
    }
}

/* the base's driver and today's, reading one packet; or, for the behaviour
   checks, wheel_read() alone */
static void feed_driver(struct world *w, uint32_t packet, bool ready)
{
    stub_env.data = packet;
    stub_env.c104 = ready ? 0x04000000 : 0;
    stub_env.queue_empty = w->queue == 0;

    if (w->driver_only)
    {
        side = 1;
        trace_n[1] = 0;
        ipod_4g_button_int();
        button_read_device();
        side = 0;
        for (int i = 0; i < trace_n[1] && i < TRACE_MAX; i++)
            if (trace[1][i].kind == T_POST)
            {
                w->queue++;
                note_post(w, trace[1][i].a, trace[1][i].b);
            }
        return;
    }

    int btn[2], pos[2];
    long untouched[2];
    for (side = 0; side < 2; side++)
    {
        trace_n[side] = 0;
        if (side == 0)
        {
            ref_ipod_4g_button_int();
            btn[0] = ref_button_read_device();
            pos[0] = ref_wheel_status();
            untouched[0] = ref_button_wheel_untouched_ticks();
        }
        else
        {
            ipod_4g_button_int();
            btn[1] = button_read_device();
            pos[1] = wheel_status();
            untouched[1] = button_wheel_untouched_ticks();
        }
    }
    side = 0;

    const struct wheel_state *s = button_wheel_state();
    bool same = trace_n[0] == trace_n[1] && trace_n[0] <= TRACE_MAX
        && !memcmp(trace[0], trace[1], trace_n[0] * sizeof(struct rec))
        && btn[0] == btn[1] && pos[0] == pos[1] && untouched[0] == untouched[1]
        && ref_wheel_is_touched == s->touched
        && ref_new_wheel_value == s->position
        && ref_old_wheel_value == s->old && ref_wheel_delta == s->delta
        && ref_accumulated_wheel_delta == s->carry
        && ref_wheel_velocity == s->velocity
        && ref_wheel_accel_stage == s->stage;
    check_count++;
    if (!same)
    {
        char what[160];
        snprintf(what, sizeof(what), "%s: packet %ld (%08" PRIx32 ") at %"
                 PRIu64 " us: the drivers differ", w->name, w->packets, packet,
                 w->t);
        check_fail(__FILE__, __LINE__, what);
        fprintf(stderr, "  base: btn %x pos %d untouched %ld touched %d new %d"
                " old %d delta %d carry %u velocity %u stage %d\n",
                btn[0], pos[0], untouched[0], ref_wheel_is_touched,
                ref_new_wheel_value, ref_old_wheel_value, ref_wheel_delta,
                ref_accumulated_wheel_delta, ref_wheel_velocity,
                ref_wheel_accel_stage);
        dump_trace(0);
        fprintf(stderr, "  today: btn %x pos %d untouched %ld touched %d new %d"
                " old %d delta %d carry %u velocity %u stage %d\n",
                btn[1], pos[1], untouched[1], s->touched, s->position, s->old,
                s->delta, s->carry, s->velocity, s->stage);
        dump_trace(1);
        w->failed = true;
    }
    for (int i = 0; i < trace_n[0] && i < TRACE_MAX; i++)
        if (trace[0][i].kind == T_POST)
        {
            w->queue++;
            note_post(w, trace[0][i].a, trace[0][i].b);
        }
}

static bool send_events_now = true;

static void feed_module(struct world *w, uint32_t packet)
{
    const struct wheel_input in = {
        .packet = packet, .usec = stub_env.usec, .tick = (uint32_t)current_tick,
        .fine = stub_env.fine, .queue_empty = w->queue == 0,
        .queue_clears = stub_env.clears, .send_events = send_events_now,
    };
    struct wheel_event ev;
    wheel_read(&w->ws, &video, &in, &ev);
    if (ev.new_touch)
        w->new_touches++;
    if (ev.direction && ev.post)
    {
        w->queue++;
        note_post(w, (ev.direction > 0 ? FWD : BACK) | (ev.repeat ? REPEAT : 0),
                  wheel_data_make(ev.steps, ev.stage));
    }
    else if (ev.direction)
        w->busy_posts++;
}

/* one packet, dt us after the last */
static void packet(struct world *w, uint32_t dt, uint32_t packet)
{
    w->t += dt;
    ui_take(w);
    world_set_time(w);
    w->packets++;
    if (w->module)
    {
        if ((packet & 0x800000ff) == 0x8000001a)
            feed_module(w, packet);
    }
    else
        feed_driver(w, packet, true);

    const int stage = w->module ? w->ws.stage : button_wheel_state()->stage;
    if (stage != w->stage_now)
    {
        if (w->stage_n < (int)(sizeof(w->stage_change)
                               / sizeof(w->stage_change[0])))
        {
            w->stage_change[w->stage_n].t = w->t;
            w->stage_change[w->stage_n].from = w->stage_now;
            w->stage_change[w->stage_n].to = stage;
            w->stage_n++;
        }
        w->stage_now = stage;
    }
}

static uint32_t touched_at(int position)
{
    return 0xc000001a | ((uint32_t)(position & 0x7f) << 16);
}
#define LIFTED 0x8000001a

/* Scripted turns: so many places in so many readings over so long */
static int finger; /* where it is, in places, any number */

static void touch(struct world *w, uint32_t dt, int position)
{
    finger = position;
    packet(w, dt, touched_at(((finger % 96) + 96) % 96));
}

static void turn(struct world *w, int places, int readings, uint32_t us)
{
    const int from = finger;
    for (int i = 1; i <= readings; i++)
    {
        finger = from + places * i / readings;
        packet(w, us / readings, touched_at(((finger % 96) + 96) % 96));
    }
}

static void lift(struct world *w, uint32_t dt)
{
    packet(w, dt, LIFTED);
}

static void wait_us(struct world *w, uint32_t us)
{
    w->t += us;
    ui_take(w);
}

static long total_packets, total_posts;
static long stage_posts[7];

static void world_done(struct world *w)
{
    total_packets += w->packets;
    total_posts += w->posts;
    for (int i = 0; i < 7; i++)
        stage_posts[i] += w->posts_at[i];
}

/* The scripted cases: each run on both drivers in step, and on the module
   alone, whose events the checks read */
typedef void (*script_fn)(struct world *w);

static void sc_slow_turn(struct world *w)
{
    /* a turn in 4 s, readings each 10 ms: 24 steps, a row each */
    touch(w, 10000, 10);
    turn(w, 96, 400, 4000000);
    lift(w, 10000);
}

static void sc_wrap_forward(struct world *w)
{
    /* across 95 -> 0, clockwise, and back across 0 -> 95 */
    touch(w, 10000, 88);
    turn(w, 16, 16, 400000);
    lift(w, 300000);
    touch(w, 10000, 8);
    turn(w, -16, 16, 400000);
    lift(w, 10000);
}

static void sc_seven_bit(struct world *w)
{
    /* the field's places past 95, which the wheel doesn't send: as before */
    touch(w, 10000, 92);
    for (int p = 92; p <= 127; p += 2)
        packet(w, 10000, touched_at(p));
    packet(w, 10000, touched_at(0));
    packet(w, 10000, touched_at(127));
    packet(w, 10000, touched_at(4));
    lift(w, 10000);
}

static void sc_reversal(struct world *w)
{
    touch(w, 10000, 40);
    turn(w, 48, 60, 300000);   /* fast one way */
    turn(w, -48, 60, 300000);  /* and back */
    turn(w, 12, 30, 300000);
    lift(w, 10000);
}

static void sc_gap(struct world *w)
{
    /* readings 61 ms apart: two steps at once start over, one counts; 59 ms
       apart both count */
    touch(w, 10000, 0);
    packet(w, 61000, touched_at(8));   /* two steps after a gap: dropped */
    packet(w, 61000, touched_at(12));  /* one: a step */
    packet(w, 59000, touched_at(20));  /* two within the gap: both */
    packet(w, 60000, touched_at(28));  /* 60 ms is no gap */
    packet(w, 60001, touched_at(36));  /* 60.001 is */
    lift(w, 10000);
}

static void sc_lift(struct world *w)
{
    /* lifted 100 ms: the same touch goes on; 200 ms: a new one */
    touch(w, 10000, 0);
    turn(w, 16, 8, 80000);
    lift(w, 10000);
    wait_us(w, 90000);
    touch(w, 10000, finger + 4);
    turn(w, 16, 8, 80000);
    lift(w, 10000);
    wait_us(w, 190000);
    touch(w, 10000, finger + 8);
    turn(w, 16, 8, 80000);
    lift(w, 10000);
}

static void sc_ui_hang(struct world *w)
{
    /* fast, the UI standing still a second: a turn's steps at most, at the
       stage it was at, which doesn't go on meanwhile */
    w->draw_min = w->draw_max = 30000;
    touch(w, 10000, 0);
    turn(w, 192, 200, 1000000);         /* 2 turns a second for a second */
    w->ui_free = w->t + 1000000;        /* the UI stands still */
    turn(w, 192, 200, 1000000);
    turn(w, 96, 100, 500000);
    lift(w, 10000);
}

static void fast_turn_3s(struct world *w)
{
    touch(w, 10000, 0);
    turn(w, 576, 600, 3000000);  /* 2 turns a second, to stage 6 */
    lift(w, 10000);
}

static void sc_usec_wrap(struct world *w)
{
    /* USEC_TIMER comes round in the middle of a fast turn: 0.9 s from now,
       however far the cases before went */
    w->usec0 = 0xffffffffu - 900000 - (uint32_t)w->t;
    world_set_time(w);
    const uint32_t before = stub_env.usec;
    fast_turn_3s(w);
    CHECK(stub_env.usec < before);   /* it came round */
}

static void sc_stages(struct world *w)
{
    /* each stage reached and dropped: by slowing down, a pause and a turn
       the other way */
    w->draw_min = 5000; w->draw_max = 15000;
    touch(w, 10000, 0);
    turn(w, 480, 500, 2500000);  /* 2 turns a second, 2.5 s: to stage 6 */
    turn(w, 8, 40, 2000000);     /* slow: stage 0 */
    turn(w, 480, 500, 2500000);
    wait_us(w, 300000);          /* a pause */
    turn(w, 480, 500, 2500000);
    turn(w, -480, 500, 2500000); /* the other way */
    lift(w, 10000);
}

static void sc_fine(struct world *w)
{
    /* the volume: steps of two places, the first of four */
    stub_env.fine = true;
    touch(w, 10000, 0);
    turn(w, 96, 400, 4000000);
    turn(w, -2, 1, 50000);       /* back: four places first */
    turn(w, -2, 1, 50000);
    turn(w, -96, 400, 4000000);
    lift(w, 10000);
    stub_env.fine = false;
}

static void sc_clear(struct world *w)
{
    /* steps held while the queue was full are dropped when it's cleared */
    w->draw_min = w->draw_max = 500000;
    touch(w, 10000, 0);
    turn(w, 40, 40, 400000);
    stub_env.clears++;
    w->queue = 0;
    turn(w, 8, 8, 80000);
    lift(w, 10000);
}

static void sc_dark(struct world *w)
{
    /* the screen dark: a touch lights it after 300 ms without a press, a
       step at once unless the wheel works dark */
    stub_env.lit = false;
    stub_env.dark_wheel = true;
    touch(w, 10000, 0);
    turn(w, 0, 40, 400000);
    packet(w, 10000, touched_at(0) | 0x800);   /* Play pressed */
    turn(w, 8, 8, 80000);
    lift(w, 10000);
    stub_env.dark_wheel = false;
    touch(w, 300000, 10);
    turn(w, 8, 8, 80000);
    lift(w, 10000);
    stub_env.lit = true;
}

static void sc_other_packets(struct world *w)
{
    /* the controller's other packets: ignored, or a resync */
    touch(w, 10000, 0);
    packet(w, 5000, 0x8000003a);
    packet(w, 5000, 0xffffffff);
    packet(w, 5000, 0x12345678);
    turn(w, 8, 4, 40000);
    if (!w->module)
        feed_driver(w, touched_at(30), false); /* no packet: no reading */
    lift(w, 10000);
}

static const struct { const char *name; script_fn fn; } scripts[] = {
    { "slow turn", sc_slow_turn },
    { "wrap", sc_wrap_forward },
    { "7-bit field", sc_seven_bit },
    { "reversal", sc_reversal },
    { "60 ms gap", sc_gap },
    { "lift", sc_lift },
    { "1 s UI hang", sc_ui_hang },
    { "USEC_TIMER wrap", sc_usec_wrap },
    { "stages", sc_stages },
    { "fine", sc_fine },
    { "queue clear", sc_clear },
    { "dark screen", sc_dark },
    { "other packets", sc_other_packets },
};
#define SCRIPTS (int)(sizeof(scripts) / sizeof(scripts[0]))

/* the drivers' world goes on from case to case: they keep their state, as
   on the iPod; a long lift between them */
static uint64_t driver_t;
static uint32_t driver_usec0 = 0;

static void run_script_drivers(int i)
{
    struct world w;
    world_init(&w, scripts[i].name, driver_usec0, 0);
    w.t = driver_t + 2000000;
    rng = 0x5eed0000 + i;
    scripts[i].fn(&w);
    driver_t = w.t;
    driver_usec0 = w.usec0;
    world_done(&w);
}

static void run_script_module(struct world *w, int i)
{
    world_init(w, scripts[i].name, 0x10000000, 1000);
    w->module = true;
    rng = 0x5eed0000 + i;
    scripts[i].fn(w);
}

/* ---- the behaviour checks, on wheel_policy.c alone ---- */

static long sum_steps(const struct world *w)
{
    long n = 0;
    for (int i = 0; i < w->ev_n; i++)
        n += w->ev[i].steps * w->ev[i].dir;
    return n;
}

/* wheel_read() alone with its time in hand: ticks of 10 ms from the us */
struct rig {
    struct wheel_state s;
    uint64_t t;           /* us */
    int place;            /* the finger, in places, any number */
    bool busy;            /* the queue full */
    struct wheel_event ev;
};

static void rig_read(struct rig *r, uint32_t dt, int places, bool touched)
{
    r->t += dt;
    r->place += places;
    const struct wheel_input in = {
        .packet = touched ? touched_at(((r->place % 96) + 96) % 96) : LIFTED,
        .usec = (uint32_t)r->t, .tick = (uint32_t)(r->t / 10000),
        .queue_empty = !r->busy, .send_events = true,
    };
    wheel_read(&r->s, &video, &in, &r->ev);
}

/* a first touch, then n steps of 4 places, us apart */
static void rig_start(struct rig *r, int n, uint32_t us)
{
    memset(r, 0, sizeof(*r));
    r->s = (struct wheel_state)WHEEL_STATE_INIT;
    r->t = 5000000;
    rig_read(r, 0, 0, true);
    for (int i = 0; i < n; i++)
        rig_read(r, us, 4, true);
}

/* from now to the middle of the tick `ticks` after the last step's */
static uint32_t rig_ticks_on(const struct rig *r, int ticks)
{
    return (uint32_t)(((uint64_t)r->s.step_tick + ticks) * 10000 + 5000 - r->t);
}

/* The feel the owner confirmed, pinned without the reference: the speed a
   stage needs, its time, the stall rule, the fast drop, the timeouts, the
   lists' bounds. The driver's own constants go through today's driver. */
static void feel(void)
{
    struct rig r;

    /* timeouts, in ticks: they count from the last step's tick, and run out
       once the tick is past it (TIME_AFTER) */
    check_case = "untouch timeout";
    for (int ticks = 15; ticks <= 16; ticks++)
    {
        rig_start(&r, 3, 100000);
        rig_read(&r, 10000, 0, false);               /* lifted */
        rig_read(&r, rig_ticks_on(&r, ticks), 0, true);
        CHECK_EQ(r.ev.new_touch, ticks > 15);        /* 150 ms */
    }
    check_case = "repeat timeout";
    for (int ticks = 24; ticks <= 25; ticks++)
    {
        rig_start(&r, 3, 100000);
        rig_read(&r, rig_ticks_on(&r, ticks), 4, true);
        CHECK_EQ(r.ev.direction, 1);
        CHECK_EQ(r.ev.repeat, ticks < 25);           /* 250 ms */
    }
    check_case = "fast-off timeout";
    for (int ticks = 25; ticks <= 26; ticks++)
    {
        rig_start(&r, 150, 20833);                   /* 2 turns a second */
        CHECK_EQ(r.ev.stage, 6);
        rig_read(&r, rig_ticks_on(&r, ticks), 4, true);
        CHECK_EQ(r.ev.stage, ticks > 25 ? 0 : 6);    /* 250 ms */
    }

    /* slowing down counts at once: a step at 97 deg/s after 1.5 turns a
       second halves the speed, under a turn a second */
    check_case = "fast drop";
    rig_start(&r, 150, 27778);
    CHECK_EQ(r.ev.stage, 6);
    rig_read(&r, 150000, 4, true);
    CHECK(r.ev.repeat);
    CHECK_EQ(r.ev.stage, 0);

    /* the speed counts up to WHEEL_SPEED_MAX, 6 turns a second: after 8,
       steps at half a turn a second halve it to under a turn the fourth */
    check_case = "speed max";
    rig_start(&r, 500, 5208);
    CHECK_EQ(r.ev.stage, 6);
    for (int i = 1; i <= 4; i++)
    {
        rig_read(&r, 83333, 4, true);
        CHECK_EQ(r.ev.stage, i < 4 ? 6 : 0);
    }

    /* the UI standing still: no stage further after 150 ms without a post */
    check_case = "stall";
    rig_start(&r, 0, 0);
    for (int i = 0; i < 200 && r.ev.stage < 2; i++)
        rig_read(&r, 20833, 4, true);
    const int stage0 = r.ev.stage;
    CHECK_EQ(stage0, 2);
    r.busy = true;
    int highest = stage0;
    for (int i = 0; i < 48; i++)                     /* a second */
    {
        rig_read(&r, 20833, 4, true);
        CHECK(!r.ev.post);
        if (r.ev.stage > highest)
            highest = r.ev.stage;
    }
    CHECK(highest <= stage0 + 1);
    r.busy = false;
    rig_read(&r, 20833, 4, true);
    CHECK(r.ev.post && r.ev.steps == 24);            /* a turn at most */
    CHECK(r.ev.stage <= stage0 + 1);

    /* the lists' bounds: two screens, and LONG_LIST */
    check_case = "list bounds";
    const unsigned int fast = wheel_data_make(3, 2);
    CHECK_EQ(wheel_list_rows(fast, 18, 9).rows, 1);
    CHECK_EQ(wheel_list_rows(fast, 19, 9).rows, 12);
    const unsigned int top = wheel_data_make(1, 6);
    CHECK_EQ(wheel_list_rows(top, 500, 9).rows, 8);
    CHECK_EQ(wheel_list_rows(top, 501, 9).rows, 8);
    CHECK_EQ(wheel_list_rows(top, 632, 9).rows, 8);  /* 8 * 17/16 */
    CHECK_EQ(wheel_list_rows(top, 633, 9).rows, 9);  /* 8 * 18/16 */

    /* today's driver alone, its constants: 96 places, 4 a step, stages each
       WHEEL_STAGE_USEC over WHEEL_ACCEL_START, a turn a second here */
    static struct world w;
    check_case = "driver: config";
    CHECK_EQ(WHEEL_ACCEL_START, 360);                /* stub/config.h's too */
    world_init(&w, "driver alone", 0x30000000, 1000);
    w.driver_only = true;
    w.draw_min = w.draw_max = 1000;
    wheel_send_events(true);
    lift(&w, 10000);
    wait_us(&w, 1000000);
    check_case = "driver: slow turn";
    touch(&w, 10000, 0);
    turn(&w, 96, 400, 4000000);
    lift(&w, 10000);
    wait_us(&w, 1000000);
    CHECK_EQ(sum_steps(&w), 24);
    CHECK_EQ(w.max_stage, 0);

    check_case = "driver: 0.9 turn a second";
    w.ev_n = 0;
    w.max_stage = 0;
    touch(&w, 10000, 0);
    turn(&w, 518, 6000, 6000000);
    lift(&w, 10000);
    wait_us(&w, 1000000);
    CHECK_EQ(w.max_stage, 0);

    check_case = "driver: 1.1 turns a second";
    w.max_stage = 0;
    touch(&w, 10000, 0);
    turn(&w, 634, 6000, 6000000);
    lift(&w, 10000);
    wait_us(&w, 1000000);
    CHECK_EQ(w.max_stage, 6);

    check_case = "driver: stage time";
    w.stage_n = 0;
    touch(&w, 10000, 0);
    turn(&w, 576, 3000, 3000000);                   /* 2 turns a second */
    lift(&w, 10000);
    int rises = 0;
    for (int i = 1; i < w.stage_n; i++)
        if (w.stage_change[i].to == w.stage_change[i].from + 1
            && w.stage_change[i - 1].to == w.stage_change[i].from
            && w.stage_change[i].from >= 1)
        {
            /* over 300 ms, and by a step (20.8 ms) and a reading at most */
            const uint64_t dt = w.stage_change[i].t - w.stage_change[i - 1].t;
            CHECK(dt > 300000 && dt <= 300000 + 21000 + 1000);
            rises++;
        }
    CHECK_EQ(rises, 5);                              /* 1 -> 2 ... 5 -> 6 */
    check_case = NULL;
}

static void behaviour(void)
{
    static struct world w;

    check_case = "slow turn";
    run_script_module(&w, 0);
    CHECK_EQ(sum_steps(&w), 24);          /* a turn is 24 steps */
    CHECK_EQ(w.max_stage, 0);             /* a turn in 4 s: no stage */
    CHECK_EQ(w.ev_n, 24);                 /* each posted, the UI idle */
    CHECK(!w.ev[0].repeat && w.ev[1].repeat);

    check_case = "wrap";
    run_script_module(&w, 1);
    long fwd = 0, back = 0;
    for (int i = 0; i < w.ev_n; i++)
        *(w.ev[i].dir > 0 ? &fwd : &back) += w.ev[i].steps;
    CHECK_EQ(fwd, 4);                     /* across 95 -> 0, forward */
    CHECK_EQ(back, 4);                    /* across 0 -> 95, back */

    check_case = "reversal";
    run_script_module(&w, 3);
    CHECK_EQ(sum_steps(&w), 12 - 12 + 3);
    for (int i = 1; i < w.ev_n; i++)
        if (w.ev[i].dir != w.ev[i - 1].dir)
        {
            CHECK_EQ(w.ev[i].stage, 0);  /* a turn back: a row again */
            CHECK(!w.ev[i].repeat);
        }

    check_case = "7-bit field";
    run_script_module(&w, 2);
    /* places past 95, which the wheel doesn't send, as the base took them:
       92 -> 126 by twos steps on at 96, 100 ... 124, its old place going
       round to 0, 4 ... 28; then 0 is 28 back, 127 31 on and 4 24 back, each
       a step as it turns round, the last with the UI busy (10 ms after the
       one before) */
    static const int seven_bit[] = { 1, 1, 1, 1, 1, 1, 1, 1, -1, 1 };
    CHECK_EQ(w.ev_n, 10);
    for (int i = 0; i < w.ev_n && i < 10; i++)
    {
        CHECK_EQ(w.ev[i].dir, seven_bit[i]);
        CHECK_EQ(w.ev[i].steps, 1);
    }
    CHECK_EQ(w.busy_posts, 1);
    CHECK_EQ(w.ws.old, 4);

    check_case = "60 ms gap";
    run_script_module(&w, 4);
    /* 8 after 61 ms: dropped; 4 after 61 ms: a step; 8 after 59: two; 8
       after 60: two; 8 after 60.001: dropped */
    CHECK_EQ(sum_steps(&w), 1 + 2 + 2);

    check_case = "lift";
    run_script_module(&w, 5);
    CHECK_EQ(w.new_touches, 2);           /* the first, and after 200 ms */
    CHECK_EQ(sum_steps(&w), 4 + 1 + 4 + 4);  /* after 100 ms the jump counts */

    check_case = "1 s UI hang";
    run_script_module(&w, 6);
    int carried = 0, stage_before = 0;
    for (int i = 0; i < w.ev_n; i++)
        if (w.ev[i].steps > carried)
        {
            carried = w.ev[i].steps;
            stage_before = i ? w.ev[i - 1].stage : 0;
            CHECK(w.ev[i].stage <= stage_before + 1);
        }
    CHECK_EQ(carried, 24);                /* a turn at most */

    check_case = "USEC_TIMER wrap";
    static struct world w2;
    run_script_module(&w, 7);
    world_init(&w2, "no wrap", 0x10000000, 1000);
    w2.module = true;
    rng = 0x5eed0000 + 7;
    fast_turn_3s(&w2);
    CHECK_EQ(w.ev_n, w2.ev_n);
    bool same = w.ev_n == w2.ev_n;
    for (int i = 0; same && i < w.ev_n; i++)
        same = w.ev[i].t == w2.ev[i].t && w.ev[i].dir == w2.ev[i].dir
               && w.ev[i].steps == w2.ev[i].steps
               && w.ev[i].stage == w2.ev[i].stage
               && w.ev[i].repeat == w2.ev[i].repeat;
    CHECK(same);                          /* as if it hadn't come round */
    CHECK(w.max_stage == 6);

    check_case = "stages";
    run_script_module(&w, 8);
    CHECK_EQ(w.max_stage, 6);
    /* each stage after 300 ms of the last: the first fast step, then 6 */
    int seen[7] = { 0 };
    for (int i = 0; i < w.ev_n; i++)
        seen[w.ev[i].stage]++;
    for (int i = 0; i <= 6; i++)
        CHECK(seen[i] > 0);
    int drops = 0;
    for (int i = 1; i < w.ev_n; i++)
        if (w.ev[i - 1].stage == 6 && w.ev[i].stage == 0)
            drops++;
    CHECK_EQ(drops, 3);                   /* slowed, paused, turned back */

    check_case = "fine";
    run_script_module(&w, 9);
    /* a turn: four places first, a step, then two each; back, four places
       first again, a step: two back alone don't go */
    CHECK_EQ(w.ev[0].steps, 1);
    long up = 0, down = 0;
    for (int i = 0; i < w.ev_n; i++)
        *(w.ev[i].dir > 0 ? &up : &down) += w.ev[i].steps;
    CHECK_EQ(up, 1 + (96 - 4) / 2);
    CHECK_EQ(down, 1 + 96 / 2);

    check_case = "queue clear";
    run_script_module(&w, 10);
    /* 40 places with the UI busy half a second: the first posted, the rest
       held and then dropped by the clear */
    CHECK_EQ(sum_steps(&w), 1 + 2);

    check_case = "data";
    for (unsigned steps = 1; steps <= 127; steps++)
        for (unsigned stage = 0; stage <= 6; stage++)
        {
            const unsigned data = wheel_data_make(steps, stage);
            CHECK_EQ(wheel_data_steps(data), steps);
            CHECK_EQ(wheel_data_speed(data), 1 << stage);
            CHECK(wheel_data_staged(data) && wheel_data_is_wheel(data));
            CHECK_EQ(wheel_data_rows(data),
                     steps * ((1 << stage) < 8 ? (1 << stage) : 8));
            CHECK_EQ(wheel_event_steps(data), steps);
        }
    CHECK_EQ(wheel_event_steps(0), 1);       /* a remote's: a step */
    CHECK_EQ(wheel_data_speed(1u << 24), 0);

    check_case = "rows";
    /* a short list, up to two screens: a row a step, whatever the speed */
    static const int short_lists[] = { 1, 2, 5, 18 };
    for (unsigned i = 0; i < sizeof(short_lists) / sizeof(short_lists[0]); i++)
        for (unsigned stage = 0; stage <= 6; stage++)
        {
            struct wheel_rows r = wheel_list_rows(wheel_data_make(5, stage),
                                                  short_lists[i], 9);
            CHECK_EQ(r.rows, 1);
            CHECK_EQ(r.steps, 1);
            CHECK_EQ(r.speed, 0);
        }
    /* 24 rows on a screen of 9: as far as the steps, 8 rows at most each */
    CHECK_EQ(wheel_list_rows(wheel_data_make(3, 0), 24, 9).rows, 3);
    CHECK_EQ(wheel_list_rows(wheel_data_make(3, 6), 24, 9).rows, 24);
    /* 13000 songs: 8 * sqrt(26) = 40 rows a step at most, at stage 6 */
    CHECK_EQ(wheel_list_rows(wheel_data_make(1, 6), 13000, 9).rows, 40);
    CHECK_EQ(wheel_list_rows(wheel_data_make(1, 5), 13000, 9).rows, 32);
    CHECK_EQ(wheel_list_rows(wheel_data_make(1, 0), 13000, 9).rows, 1);
    CHECK_EQ(wheel_list_rows(wheel_data_make(24, 0), 13000, 9).rows, 24);
    /* 1700 albums: 8 * sqrt(3.4) = 14 */
    CHECK_EQ(wheel_list_rows(wheel_data_make(1, 6), 1700, 9).rows, 14);
    /* a remote's next, no data: a row */
    CHECK_EQ(wheel_list_rows(0, 13000, 9).rows, 1);
    feel();
    check_case = NULL;
}

/* ---- the random sessions, on both drivers ---- */

static void session(struct world *w)
{
    /* the screen: a menu, a long list, the volume, or one that mostly reads
       the wheel itself (send_events off, as rockboy) */
    const int screen = rnd_in(0, 3);
    switch (screen)
    {
        case 0: w->draw_min = 10000; w->draw_max = 30000; break;
        case 1: w->draw_min = 25000; w->draw_max = 70000; break;
        case 2: w->draw_min = 8000;  w->draw_max = 20000; break;
        case 3: w->draw_min = 5000;  w->draw_max = 15000; break;
    }
    stub_env.fine = screen == 2 && !chance(100);
    const bool send = screen != 3 || chance(300);
    ref_wheel_send_events(send);
    wheel_send_events(send);
    stub_env.lit = !chance(150);
    stub_env.dark_wheel = chance(300);

    const uint32_t interval = rnd_in(1000, 12000);   /* between readings */
    double angle = rnd_in(0, 95);
    const int touches = rnd_in(1, 4);
    for (int t = 0; t < touches; t++)
    {
        if (chance(30))
        {
            /* Hold: no readings meanwhile */
            stub_env.gpioa = 0;
            w->t += rnd_in(100000, 2000000);
            packet(w, 0, 0x8000003a);
            stub_env.gpioa = 0x20;
        }
        const int segments = rnd_in(1, 6);
        double speed = 0;
        for (int s = 0; s < segments; s++)
        {
            /* places a second: from a step a second to six turns */
            if (chance(150))
                speed = 0;                               /* resting */
            else
            {
                const double mag = 4.0 * exp2((double)rnd_in(0, 1000)
                                                        * 7.2 / 1000.0);
                const int dir = speed == 0 ? (chance(500) ? 1 : -1)
                              : (chance(200) ? -1 : 1) * (speed > 0 ? 1 : -1);
                speed = dir * mag;
            }
            const uint32_t length = rnd_in(30000, 3000000);
            for (uint32_t done = 0; done < length; )
            {
                uint32_t dt = interval / 2 + rnd_in(0, interval);
                if (chance(5))
                    dt = rnd_in(40000, 160000);           /* the link stalls */
                if (chance(10))
                    dt = rnd_in(50, 1000);                /* close together */
                done += dt;
                angle += speed * dt / 1e6;
                int place = (int)floor(angle);
                if (chance(30))
                    place += chance(500) ? 1 : -1;        /* jitter */
                place = ((place % 96) + 96) % 96;
                uint32_t p = touched_at(place);
                if (chance(5))
                    p |= 0x100u << rnd_in(0, 4);          /* a button */
                if (chance(2))
                    p = chance(500) ? 0x8000003a : (uint32_t)rnd();
                if (chance(3))
                {
                    /* a moment off the wheel, back elsewhere */
                    lift(w, dt);
                    w->t += rnd_in(5000, 250000);
                    angle += (int)rnd_in(0, 30) - 15;
                    continue;
                }
                if (chance(2))
                    w->ui_free = w->t + rnd_in(200000, 1500000); /* a hang */
                if (chance(1))
                {
                    stub_env.clears++;                    /* a new screen */
                    w->queue = 0;
                }
                if (chance(2))
                    w->queue++;                           /* a button event */
                packet(w, dt, p);
            }
        }
        lift(w, interval);
        if (chance(300))
            lift(w, rnd_in(1000, 20000));
        w->t += chance(500) ? rnd_in(10000, 200000) : rnd_in(200000, 5000000);
    }
}

static void random_sessions(uint64_t seed, long min_posts)
{
    static struct world w;
    rng = seed ? seed : 1;
    /* USEC_TIMER starting a minute before it comes round */
    world_init(&w, "random", 0xffffffffu - 60000000u, rnd_in(0, 1u << 28));
    w.t = 0;
    long sessions = 0;
    char name[64];
    snprintf(name, sizeof(name), "random, seed %" PRIu64, seed);
    w.name = name;
    while (w.posts < min_posts && !w.failed)
    {
        session(&w);
        sessions++;
    }
    world_done(&w);
    printf("wheel      random: seed %" PRIu64 ", %ld sessions, %ld packets, "
           "%ld posts, %ld steps, %.1f min, USEC_TIMER came round %d times\n",
           seed, sessions, w.packets, w.posts, w.steps, w.t / 6e7,
           (int)(((uint64_t)w.usec0 + w.t) >> 32));
    if (w.failed)
        fprintf(stderr, "wheel: random sessions differ: WHEEL_SEED=%" PRIu64
                " to run them again\n", seed);
}

/* ---- the data's consumers: base lines and wheel_policy.c's ---- */

static void consumers_one(unsigned int data, int items, int lines)
{
    struct ref_rows ref;
    ref_list_rows(data, items, lines, &ref);

    /* today's list.c: its lines asked of the wheel's data only */
    int calls = 0;
    int l = 0;
    if (wheel_data_is_wheel(data))
    {
        calls++;
        l = lines;
    }
    const struct wheel_rows r = wheel_list_rows(data, items, l);
    const bool quick = r.speed >= 4 && items > 100;

    if (ref.rows != r.rows || ref.steps != r.steps || ref.quick != quick
        || ref.nb_lines_calls != calls
        || ref_list_quick_click(data, items)
           != (wheel_data_speed(data) >= 4 && items > 100)
        || ref_wps_steps(data) != wheel_event_steps(data)
        || ref_wps_lyrics_ticks(data) != (wheel_data_rows(data) > 1
                                          ? wheel_data_rows(data) : 1)
        || ref_wps_volume_steps(data) != (wheel_data_is_wheel(data)
              ? (wheel_event_steps(data) < 6 ? wheel_event_steps(data) : 6) : 2)
        || ref_button_wheel_speed(data) != wheel_data_speed(data)
        || ref_button_wheel_steps(data) != wheel_data_steps(data)
        || ref_button_wheel_staged(data) != wheel_data_staged(data)
        || ref_button_apply_acceleration(data) != wheel_data_rows(data))
    {
        char what[128];
        snprintf(what, sizeof(what), "data %08x, %d items, %d lines: rows %d/%d"
                 " steps %d/%d quick %d/%d", data, items, lines, ref.rows,
                 r.rows, ref.steps, r.steps, ref.quick, quick);
        check_fail(__FILE__, __LINE__, what);
    }
    check_count++;
}

static void consumers(uint64_t seed)
{
    static const int items[] = { 0, 1, 2, 5, 17, 18, 19, 20, 24, 99, 100, 101,
        500, 501, 502, 1000, 1700, 2000, 13000, 32000, 100000 };
    static const int lines[] = { 0, 1, 8, 9, 10 };
    long n = 0;
    /* every staged datum the click wheel posts */
    for (unsigned steps = 0; steps <= 127; steps++)
        for (unsigned stage = 0; stage <= 22; stage++)
            for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++)
                for (unsigned j = 0; j < sizeof(lines) / sizeof(lines[0]); j++)
                {
                    consumers_one(wheel_data_make(steps, stage), items[i],
                                  lines[j]);
                    n++;
                }
    /* and any other: the other drivers' degrees a second, a remote's */
    rng = seed ^ 0xda7a;
    for (int k = 0; k < 200000; k++)
    {
        unsigned int data = (unsigned int)rnd();
        if (chance(300))
            data &= ~WHEEL_DATA_STAGED;
        if (chance(100))
            data &= 0x7fffffff;
        if (chance(100))
            data = 0;
        consumers_one(data, rnd_in(0, 40000), rnd_in(0, 12));
        n++;
    }
    printf("wheel      consumers: %ld data and list lengths\n", n);
}

int main(void)
{
    check_time_limit(300);
    uint64_t seed = 20261115;
    const char *env = getenv("WHEEL_SEED");
    if (env)
        seed = strtoull(env, NULL, 0);

    /* the scripted cases on both drivers, one after another */
    for (int i = 0; i < SCRIPTS; i++)
        run_script_drivers(i);
    printf("wheel      scripted: %d cases, %ld packets, %ld posts; posts by "
           "stage %ld %ld %ld %ld %ld %ld %ld\n", SCRIPTS, total_packets,
           total_posts, stage_posts[0], stage_posts[1], stage_posts[2],
           stage_posts[3], stage_posts[4], stage_posts[5], stage_posts[6]);

    random_sessions(seed, 100000);
    printf("wheel      posts by stage, all: %ld %ld %ld %ld %ld %ld %ld\n",
           stage_posts[0], stage_posts[1], stage_posts[2], stage_posts[3],
           stage_posts[4], stage_posts[5], stage_posts[6]);
    consumers(seed);
    behaviour();
    return check_done("wheel");
}
