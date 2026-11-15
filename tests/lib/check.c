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
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <sanitizer/common_interface_defs.h>
#include "check.h"

int check_count, check_failed;
const char *check_case;

void check_fail(const char *file, int line, const char *what)
{
    check_failed++;
    if (check_case)
        fprintf(stderr, "%s:%d: [%s] failed: %s\n", file, line, check_case, what);
    else
        fprintf(stderr, "%s:%d: failed: %s\n", file, line, what);
}

int check_done(const char *suite)
{
    printf("%-10s %d checks, %d failed\n", suite, check_count, check_failed);
    return check_failed ? 1 : 0;
}

static int time_limit;

/* The stack is printed by ASan's symbolizer, which isn't safe in a signal
   handler: should it hang, the alarm ends the program. */
static void timed_out(int sig)
{
    (void)sig;
    signal(SIGALRM, SIG_DFL);
    alarm(2);
    fprintf(stderr, "took more than %d s of CPU, at:\n", time_limit);
    __sanitizer_print_stack_trace();
    _exit(check_failed ? 1 : CHECK_TIMED_OUT);
}

void check_time_limit(int seconds)
{
    const struct itimerval limit = { { 0, 0 }, { seconds, 0 } };
    time_limit = seconds;
    signal(SIGPROF, timed_out);
    setitimer(ITIMER_PROF, &limit, NULL);
}
