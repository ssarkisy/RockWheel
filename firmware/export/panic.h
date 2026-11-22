/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by wavey@wavey.org
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

#ifndef __PANIC_H__
#define __PANIC_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "gcc_extensions.h"

void panicf( const char *fmt, ... ) ATTRIBUTE_PRINTF(1, 2) NORETURN_ATTR;

/* The crash record (HAVE_PANIC_RECORD, panic_record.h) */
struct panic_record;

/* Its PANIC_REC_COPIES copies at the audio buffer's end (core_alloc.c),
   through the uncached alias; NULL when the boot found them misplaced */
struct panic_record *panic_record_area(void);

/* The reserve's bytes after the copies, which a reset leaves as it leaves
   them, through the same alias, *size of them; NULL as for the area. The
   database's scan keeps its mark there (tagcache.c, scan_guard_boot()). */
void *panic_record_spare(size_t *size);

/* What ends this boot, from panicf() or UIE() before its screen, with
   interrupts off: a first record, or one more on the count of this boot's
   first. regs: r0-r7, or NULL. unwind: the trace, unwound from pc and sp,
   or after panic_record_stkov() taken from the overflowed stack. */
void panic_record_save(unsigned int kind, unsigned int pc, unsigned int lr,
                       unsigned int sp, const unsigned int *regs,
                       const char *msg, bool unwind);

/* thread_stkov(), on the overflowed thread, before its panicf()
   (HAVE_PANIC_STKOV_SCAN): the record that panic makes on this core takes its
   trace from that thread's stack, up to end: from begin, its bottom, and
   from sp, the one switch_thread() saved, when that is below begin; rather
   than from an unwind, which would end at once: thread_stkov() doesn't
   return, so what follows its calls of panicf() is no code of its own. */
void panic_record_stkov(const void *begin, const void *end, uintptr_t sp);

/* The RTC's seconds since 1970 at current_tick 0, for the records' times */
void panic_record_set_clock(unsigned long secs);

/* Where the next boot writes the record, as the screen names it, and where to
   send it: the same in every language */
#define CRASH_LOG_FILE         ROCKBOX_DIR "/crash.log"
#define CRASH_LOG_SHOWN        ".rockbox/crash.log"
#define CRASH_REPORT_ADDRESS   "s.sarkisian@mail.com"
#ifdef SIMULATOR
#define PANIC_RECORD_SIM_FILE  ROCKBOX_DIR "/.panic_record.bin"
#endif

/* The panic screen's last lines, if a record was kept: what to do with it */
void panic_draw_request(void);

/* The crash screens' text rows in the system font, less the 3 rows
   panic_draw_request() covers when a record was kept: where their trace
   stops */
int panic_last_row(void);

#endif /* __PANIC_H__ */
