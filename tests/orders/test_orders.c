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

/* The Songs orders' sort from the order made before (the hint), with what
   make_src.py takes out of apps/tagtree.c: an order from a hint is either
   the full sort's own, or none, whatever the hint. */

#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "check.h"
#include ORDERS_SRC

static unsigned rnd_state = 12345;
static unsigned rnd(void)
{
    rnd_state = rnd_state * 1103515245 + 12345;
    return rnd_state >> 8;
}

enum { ROWS = 4000 };
static struct song_key keys[ROWS], old_keys[ROWS];
static uint16_t a[ROWS], b[ROWS], c[ROWS], full_a[ROWS], full_b[ROWS];
static uint8_t seen[ROWS / 8 + 1];
static int32_t ids_new[ROWS], ids_old[ROWS];
static struct song_place old_rows[ROWS];
static uint16_t row_of[3 * ROWS];
static int hints_proved, hints_refused;

/* the full sort of n rows, as songs_order_sort() makes it without a hint */
static const uint16_t *full_sort(const struct song_key *k, int n)
{
    for (int i = 0; i < n; i++)
        full_a[i] = i;
    return songs_rows_sort(k, n, full_a, full_b);
}

/* A list of n rows (by name), from one of n_old before: some rows taken
   out, some new ones put in, some keys changed; its order from the old
   one's, with the hint perhaps damaged, is the full sort's or none */
static void check_hint(int n_old, int removed, int added, int changed, int damage)
{
    /* the old list: entries 0.. in a random order by name, random keys
       with many the same */
    const int key_range = 1 + rnd() % 50;
    for (int i = 0; i < n_old; i++)
    {
        ids_old[i] = i;
        old_keys[i] = (struct song_key){ rnd() % 3, rnd() % key_range };
    }
    for (int i = n_old - 1; i > 0; i--)
    {
        const int j = rnd() % (i + 1);
        const int32_t t = ids_old[i];
        ids_old[i] = ids_old[j];
        ids_old[j] = t;
    }
    /* its order, as the file has it */
    const uint16_t *s = full_sort(old_keys, n_old);
    for (int k = 0; k < n_old; k++)
        old_rows[k] = (struct song_place){ s[k], 0 };
    /* the list now: the old one's rows in their order but those taken out,
       with new entries put in among them anywhere; a song keeps its key,
       unless it's changed */
    int n = 0, put_in = 0, rekeyed = 0;
    for (int i = 0; i < n_old && n < ROWS; i++)
    {
        while (added > 0 && rnd() % (n_old + 1) < (unsigned)added && n < ROWS)
        {
            ids_new[n] = n_old + 1000 + n;
            keys[n++] = (struct song_key){ rnd() % 3, rnd() % key_range };
            added--;
            put_in++;
        }
        if (removed > 0 && rnd() % n_old < (unsigned)removed)
        {
            removed--;
            continue;
        }
        ids_new[n] = ids_old[i];
        keys[n] = old_keys[i];
        if (changed > 0 && rnd() % n_old < (unsigned)changed)
        {
            keys[n].lo = rnd() % key_range;
            changed--;
            rekeyed++;
        }
        n++;
    }
    while (added-- > 0 && n < ROWS)
    {
        ids_new[n] = n_old + 1000 + n;
        keys[n++] = (struct song_key){ rnd() % 3, rnd() % key_range };
        put_in++;
    }
    const int entries = ARRAYLEN(row_of);
    memset(row_of, 0xff, sizeof(row_of));
    for (int i = 0; i < n; i++)
        row_of[ids_new[i]] = i;
    /* damaged: two rows of the order swapped, or a row out of range */
    if (damage == 1 && n_old > 2)
    {
        const int x = rnd() % n_old, y = rnd() % n_old;
        const struct song_place t = old_rows[x];
        old_rows[x] = old_rows[y];
        old_rows[y] = t;
    }
    else if (damage == 2 && n_old > 0)
        old_rows[rnd() % n_old].from = n_old + rnd() % 5;
    else if (damage == 3 && n_old > 1)
        ids_old[rnd() % n_old] = ids_old[rnd() % n_old]; /* an entry twice */

    const struct songs_hint hint = {
        old_rows, ids_old, n_old, row_of, entries, c, seen
    };
    const uint16_t *got = songs_order_from_hint(keys, n, &hint, a, b);
    const uint16_t *want = full_sort(keys, n);
    if (got)
    {
        hints_proved++;
        CHECK(!memcmp(got, want, n * sizeof(*got)));
    }
    else
        hints_refused++;
    /* undamaged, with no key changed and fewer than half the rows new, the
       hint gives it */
    if (!damage && !rekeyed && 2 * (n - put_in) > n)
        CHECK(got != NULL);
}

static void check_orders(void)
{
    for (int round = 0; round < 3000; round++)
    {
        const int n_old = rnd() % 1500;
        const int removed = rnd() % 4 == 0 ? 0 : rnd() % (1 + n_old / 10);
        const int added = rnd() % (1 + n_old / 8 + 3);
        const int changed = rnd() % 4 == 0 ? rnd() % 4 : 0;
        const int damage = rnd() % 5 == 0 ? 1 + rnd() % 3 : 0;
        check_hint(n_old, removed, added, changed, damage);
    }
    /* the sizes at their ends: an empty list, one row, all rows new */
    check_hint(0, 0, 5, 0, 0);
    check_hint(1, 0, 0, 0, 0);
    check_hint(1, 1, 0, 0, 0);
    check_hint(3000, 0, 0, 0, 0);
    check_hint(3000, 50, 50, 0, 0);
    printf("orders: %d proved from the hint, %d sorted in full\n",
           hints_proved, hints_refused);
}

int main(void)
{
    check_time_limit(600);
    check_orders();
    return check_done("orders");
}
