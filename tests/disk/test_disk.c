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
 * misc.c's disk_free_for_ui() and disk_is_flash() against the copies they
 * replace, taken from BASE verbatim (ref/disk_ref.c): photos_panel.c's and
 * tagtree.c's, and menu_cover.c's, which keeps its own for now. Built once
 * for each configuration the predicates tell apart (Makefile, check-disk):
 * the iPod's, the simulator's, a player without a hard disk, and one with a
 * disk that isn't ATA, which no target is (the Makefile checks that). Every
 * state of the table is asked of both, with the tick near 0 and near the
 * 32-bit wraps, as the iPod's tick wraps.
 *
 ****************************************************************************/
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* the firmware the predicates read, as the iPod has it */
#define HZ 100
#define STORAGE_ATA 0x02
#define STORAGE_SD  0x04
#ifndef CONFIG_STORAGE
#define CONFIG_STORAGE STORAGE_ATA
#endif
#define TIME_AFTER(a, b) ((int32_t)((uint32_t)(b) - (uint32_t)(a)) < 0)
#define TIME_BEFORE(a, b) TIME_AFTER(b, a)

static long current_tick;
static struct { int disk_spindown; } global_settings;
static bool ssd, active;
static long last_activity;

static int ata_disk_isssd(void) { return ssd; }
static bool storage_disk_is_active(void) { return active; }
static long storage_last_disk_activity(void) { return last_activity; }

#include "disk_ref.c"

bool disk_is_flash(void);
bool disk_free_for_ui(long margin);
#define DISK_SPINNING (-1)
#include "disk_new.c"

static long checks, failed, not_ata_differ;

static void expect(const char *what, bool ref, bool now, long margin)
{
    checks++;
    if (ref == now)
        return;
    if (failed++ < 20)
        printf("FAIL %s: ref %d, new %d; ssd %d active %d spindown %d s "
               "tick %ld last %ld margin %ld\n", what, ref, now, ssd, active,
               global_settings.disk_spindown, current_tick, last_activity,
               margin);
}

int main(void)
{
    static const long ticks[] = {
        0, 1, 1000, 0x7fffff00L, 0x7fffffffL, 0x80000000L, 0xffffff00L,
        0xffffffffL,
    };
    static const int spindowns[] = { 0, 3, 5, 10, 60, 254 };
    for (unsigned t = 0; t < sizeof(ticks) / sizeof(*ticks); t++)
    for (unsigned s = 0; s < sizeof(spindowns) / sizeof(*spindowns); s++)
    /* the time to spin-down, around 0 and the 2 s margin, and far */
    for (long left = -3 * HZ; left <= 300 * HZ;
         left += (left > -5 && left < 3 * HZ) ? 1 : 7)
    for (int flags = 0; flags < 4; flags++)
    {
        current_tick = ticks[t];
        global_settings.disk_spindown = spindowns[s];
        ssd = flags & 1;
        active = flags & 2;
        last_activity = current_tick + left - spindowns[s] * HZ;
        expect("photos_panel_disk_free = disk_free_for_ui(2 * HZ)",
               ref_photos_panel_disk_free(), disk_free_for_ui(2 * HZ),
               2 * HZ);
#ifdef DISK_NOT_ATA
        /* tagtree.c's asked a disk that isn't ATA to spin, the shared one
           doesn't: counted, not failed, as no target has one */
        if (ref_songs_orders_disk_on() != disk_free_for_ui(DISK_SPINNING))
            not_ata_differ++;
#else
        expect("songs_orders_disk_on = disk_free_for_ui(DISK_SPINNING)",
               ref_songs_orders_disk_on(), disk_free_for_ui(DISK_SPINNING),
               DISK_SPINNING);
#endif
        /* the followers: menu_cover.c's pair onto the shared ones */
        expect("menu_cover disk_spins = !disk_is_flash",
               ref_menu_cover_disk_spins(), !disk_is_flash(), 0);
        expect("menu_cover disk_free = disk_free_for_ui(SHARP_SPINS)",
               ref_menu_cover_disk_free(), disk_free_for_ui(SHARP_SPINS),
               SHARP_SPINS);
        /* any margin, as the header has it: a hard disk (DISK_SPINS, the
           same configurations) that spins and has more than margin ticks
           left, or none set */
        static const long margins[] = { 0, 1, HZ / 2, HZ, 2 * HZ, 5 * HZ };
        for (unsigned m = 0; m < sizeof(margins) / sizeof(*margins); m++)
            expect("disk_free_for_ui(margin) as its header says",
                   !DISK_SPINS || ssd
                   || (active && (!spindowns[s] || left > margins[m])),
                   disk_free_for_ui(margins[m]), margins[m]);
    }
    printf("disk (%s): %ld checks, %ld failed", CONFIG_NAME, checks, failed);
#ifdef DISK_NOT_ATA
    printf("; songs_orders_disk_on differs in %ld states", not_ata_differ);
#endif
    printf("\n");
    return failed != 0;
}
