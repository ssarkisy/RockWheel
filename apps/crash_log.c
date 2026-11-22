/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
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

/* The crash record a panic, an exception or a hang left in RAM
 * (firmware/panic.c, panic_record.h), written out as text at the next boot,
 * before music: what a user sends. Paths are scrubbed from the message; the
 * addresses are turned into functions on the Mac (crash_symbols.py) by the
 * map file of the version named. */

#include "config.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "rbpaths.h"
#include "timefuncs.h"
#include "panic.h"
#include "panic_record.h"
#include "crash_log.h"

#ifdef HAVE_PANIC_RECORD

#define CRASH_OLD_FILE ROCKBOX_DIR "/crash.old.log"
#define CRASH_FILE_MAX (16 * 1024) /* set aside as the old one from this size */

static const char *kind_name(uint32_t kind)
{
    static const char * const names[] = {
        "panicf", "undefined instruction", "prefetch abort", "data abort",
        "divide by zero", "SWI", "jump to 0"
    };

    if (kind < ARRAYLEN(names))
        return names[kind];
    return kind == PANIC_KIND_HANG ? "hang" : "unknown";
}

static void put_time(int fd, const char *what, uint32_t secs)
{
    time_t t = secs;
    struct tm tm;

    if (!secs || !gmtime_r(&t, &tm))
    {
        fdprintf(fd, "%s: unknown\n", what);
        return;
    }
    fdprintf(fd, "%s: %04d-%02d-%02d %02d:%02d:%02d\n", what,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void write_record(const struct panic_record *r, int mended,
                         uint32_t now)
{
    int fd = open(CRASH_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0 && ffilesize(fd) >= CRASH_FILE_MAX)
    {
        close(fd);
        /* begun again all the same if it can't be set aside */
        remove(CRASH_OLD_FILE);
        rename(CRASH_LOG_FILE, CRASH_OLD_FILE);
        fd = open(CRASH_LOG_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }
    if (fd < 0)
        return;

    char text[PANIC_REC_MSG];
    char name[PANIC_REC_THREAD];
    char version[PANIC_REC_VERSION];

    /* the fields as strings whatever they hold */
    panic_record_scrub(text, sizeof(text), r->msg);
    strmemccpy(name, r->thread, sizeof(name));
    strmemccpy(version, r->version, sizeof(version));

    fdprintf(fd, "== crash record\n");
    fdprintf(fd, "version: %s\n", version);
    put_time(fd, "when", r->wall);
    fdprintf(fd, "uptime: %lu.%02lu s\n", (unsigned long)r->uptime / HZ,
             (unsigned long)r->uptime % HZ * (100 / HZ));
    fdprintf(fd, "what: %s\n", kind_name(r->kind));
    fdprintf(fd, "message: %s\n", text);
    fdprintf(fd, "thread: %s\n", name);
    fdprintf(fd, "core: %s\n", r->core ? "cop" : "cpu");
    if (r->count > 1)
        fdprintf(fd, "count: %lu\n", (unsigned long)r->count);
    fdprintf(fd, "pc: %08lx\nlr: %08lx\nsp: %08lx\n", (unsigned long)r->pc,
             (unsigned long)r->lr, (unsigned long)r->sp);
    for (int i = 0; i < 8; i++)
    {
        if (r->regs[i])
        {
            fdprintf(fd, "regs:");
            for (int j = 0; j < 8; j++)
                fdprintf(fd, " %08lx", (unsigned long)r->regs[j]);
            fdprintf(fd, "\n");
            break;
        }
    }
    fdprintf(fd, "trace:");
    for (int i = 0; i < PANIC_REC_TRACE && r->trace[i]; i++)
        fdprintf(fd, " %08lx", (unsigned long)r->trace[i]);
    fdprintf(fd, "\n");
    if (mended)
        fdprintf(fd, "mended: %d bits\n", mended);
    put_time(fd, "logged", now);
    close(fd);
}

/* the thread of the record logged at this boot, "" for none */
static char boot_thread[PANIC_REC_THREAD];

const char *crash_log_boot_thread(void)
{
    return boot_thread[0] ? boot_thread : NULL;
}

void crash_log_boot(void)
{
    struct tm *tm = get_time();
    const uint32_t now = valid_time(tm) ? (uint32_t)mktime(tm) : 0;
    if (now)
        panic_record_set_clock(now - current_tick / HZ);

    struct panic_record *area = panic_record_area();
    if (!area)
        return;

#ifdef SIMULATOR
    /* the simulator's RAM, kept in a file across its runs */
    int fd = open(PANIC_RECORD_SIM_FILE, O_RDONLY);
    if (fd < 0)
        return;
    bool whole = read(fd, area, PANIC_REC_RESERVE) == PANIC_REC_RESERVE;
    close(fd);
    if (!whole)
        return;
#endif

    struct panic_record rec;
    int mended;
    const bool found = panic_record_recover(&rec, area, &mended) != NULL;
    /* marked logged before it is written out: a panic while writing it isn't
       logged again at every boot */
    const bool fresh = found && panic_record_take(&rec);
    /* the copies made whole, or cleared if none could be read: each reset
       flips a few of their bits, which would add up until the mending search
       ran at every boot */
    if (found)
    {
        area[0] = rec;
        panic_record_replicate(area);
    }
    else
        memset(area, 0, PANIC_REC_COPIES * sizeof(*area));
#ifdef SIMULATOR
    fd = open(PANIC_RECORD_SIM_FILE, O_WRONLY | O_TRUNC);
    if (fd >= 0)
    {
        write(fd, area, PANIC_REC_RESERVE);
        close(fd);
    }
#endif
    if (fresh)
    {
        /* a name the record held, NUL or not */
        strmemccpy(boot_thread, rec.thread, sizeof(boot_thread));
        if (!boot_thread[0])
            strcpy(boot_thread, "?");
        write_record(&rec, mended, now);
    }
}

#endif /* HAVE_PANIC_RECORD */
