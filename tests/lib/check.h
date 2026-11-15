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

/* The host tests' checks: each failure is printed where it is, and the
   program's exit status is check_done()'s */
#ifndef TESTS_CHECK_H
#define TESTS_CHECK_H

#include <stdio.h>

extern int check_count, check_failed;
extern const char *check_case; /* named in failures, NULL for none */

void check_fail(const char *file, int line, const char *what);
int check_done(const char *suite);

/* At most this much CPU time, user and system as ITIMER_PROF counts it: past
   it, where the program was is printed and it ends, with CHECK_TIMED_OUT, or
   1 if a check had failed before. Not RLIMIT_CPU: macOS sends no SIGXCPU to a
   loop spent in system calls, as one of lseek() and read() is. */
#define CHECK_TIMED_OUT 3
void check_time_limit(int seconds);

#define CHECK(cond) \
    do { \
        check_count++; \
        if (!(cond)) \
            check_fail(__FILE__, __LINE__, #cond); \
    } while (0)

/* with the values compared, for those that are numbers */
#define CHECK_EQ(a, b) \
    do { \
        const long long check_a_ = (long long)(a), check_b_ = (long long)(b); \
        check_count++; \
        if (check_a_ != check_b_) \
        { \
            char check_buf_[256]; \
            snprintf(check_buf_, sizeof(check_buf_), "%s == %s (%lld, %lld)", \
                     #a, #b, check_a_, check_b_); \
            check_fail(__FILE__, __LINE__, check_buf_); \
        } \
    } while (0)

#endif
