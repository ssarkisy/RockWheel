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

#include "config.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "rbpaths.h"
#include "timefuncs.h"
#include "version.h"
#include "cpu_time.h"
#include "health_log.h"

#if (CONFIG_STORAGE & STORAGE_ATA) && (CONFIG_PLATFORM & PLATFORM_NATIVE) \
    && !defined(IPOD_6G)
#include "ata.h"
#define HAVE_ATA_COUNTERS /* firmware/drivers/ata.c is built */
#endif

#if CONFIG_PLATFORM & PLATFORM_NATIVE
#include "fat.h"
#include "mv.h"
#define HAVE_FAT_LINES /* firmware/common/fat.c is built */
#endif

#if defined(HAVE_USBSTACK) && CONFIG_USBOTG == USBOTG_ARC \
    && (CONFIG_PLATFORM & PLATFORM_NATIVE)
#include "usb_drv.h"
#define HAVE_USB_COUNTERS /* firmware/target/arm/usb-drv-arc.c is built */
#endif

#define HEALTH_FILE     ROCKBOX_DIR "/health.log"
#define HEALTH_OLD_FILE ROCKBOX_DIR "/health.old.log"
#define HEALTH_FILE_MAX (64 * 1024) /* set aside as the old one from this size */
#define NOTE_MAX        256

static char notes[2048];
static int notes_len;
static int notes_lost; /* lines that found it full */

/* No lock: threads of the CPU take turns only where one yields, and neither
   adding a line here nor taking the written ones out in the flush does. */
void health_note(const char *fmt, ...)
{
    char line[NOTE_MAX];
    va_list ap;
    int len = snprintf(line, sizeof(line), "%ld ", current_tick / HZ);
    va_start(ap, fmt);
    vsnprintf(line + len, sizeof(line) - len, fmt, ap);
    va_end(ap);
    len = strlen(line);
    line[len++] = '\n'; /* over the terminator */

    if (notes_len + len <= (int)sizeof(notes))
    {
        memcpy(notes + notes_len, line, len);
        notes_len += len;
    }
    else
        notes_lost++;
}

#ifdef HAVE_ATA_COUNTERS
/* " -5:2 -7:9" for the codes from -first on that were counted */
static void by_code(char *buf, size_t size, const unsigned int *count,
                    int codes, int first, unsigned int *sum)
{
    size_t len = 0;
    buf[0] = '\0';
    for (int i = 0; i < codes; i++)
    {
        if (!count[i])
            continue;
        *sum += count[i];
        if (len < size)
            len += snprintf(buf + len, size - len, " -%d:%u", first + i,
                            count[i]);
    }
}

/* The drive's counters on a line with the flush's second, not an event's,
   when one has grown since the last line written; a longest transfer alone
   only from a second on, and when longer still. Its length, 0 for none. */
static struct
{
    unsigned int events;
    long longest[2];
} ata_noted, ata_seen;

static int ata_line(char *line, size_t size)
{
    const struct ata_counters *c = ata_get_counters();
    char failed[112], retried[64]; /* 14 a code at the most */
    unsigned int failures = 0, retries = 0;

    by_code(failed, sizeof(failed), c->failed, ARRAYLEN(c->failed), 1,
            &failures);
    by_code(retried, sizeof(retried), c->retried, ARRAYLEN(c->retried), 4,
            &retries);
    const long rd = c->longest[0], wr = c->longest[1];
    ata_seen.events = failures + retries + c->bsy_timeouts + c->rdy_timeouts
                      + c->drq_waits;
    ata_seen.longest[0] = rd;
    ata_seen.longest[1] = wr;
    if (ata_seen.events == ata_noted.events
        && (rd < HZ || rd <= ata_noted.longest[0])
        && (wr < HZ || wr <= ata_noted.longest[1]))
        return 0;
    char waited[32] = ""; /* said only when it happened */
    if (c->drq_waits)
        snprintf(waited, sizeof(waited), "; waited for DRQ %u", c->drq_waits);
    int len = snprintf(line, size, "%ld ata: failed %u%s; errors"
                " retried %u%s;"
                " timeouts intrq %u bsy %u rdy %u; longest read %ld.%02ld s,"
                " write %ld.%02ld s%s\n", current_tick / HZ, failures, failed,
                retries, retried, c->retried[3],
                c->bsy_timeouts, c->rdy_timeouts, rd / HZ,
                rd % HZ * 100 / HZ, wr / HZ, wr % HZ * 100 / HZ, waited);
    if (len >= (int)size) /* cut: it still ends its line */
    {
        len = size - 1;
        line[len - 1] = '\n';
    }
    return len;
}

static inline void ata_line_written(void)
{
    ata_noted = ata_seen;
}
#else
static inline int ata_line(char *line, size_t size)
{
    (void)line;
    (void)size;
    return 0;
}

static inline void ata_line_written(void)
{
}
#endif

#ifdef HAVE_USB_COUNTERS
/* The bus's counters on a line with the flush's second, when one has grown
   since the last line written: as the computer connects next, or at power
   off, never while it has the disk. Its length, 0 for none. */
static unsigned int usb_noted, usb_seen;

static int usb_line(char *line, size_t size)
{
    const struct usb_drv_counters *c = usb_drv_get_counters();

    usb_seen = c->resets + c->error_ints + c->halted + c->buffer_errors
               + c->xact_errors + c->prime_failed;
    if (usb_seen == usb_noted)
        return 0;
    int len = snprintf(line, size, "%ld usb: resets in use %u; error"
                " interrupts %u; transfers halted %u, buffer %u, no answer"
                " %u; not taken %u\n", current_tick / HZ, c->resets,
                c->error_ints, c->halted, c->buffer_errors, c->xact_errors,
                c->prime_failed);
    if (len >= (int)size) /* cut: it still ends its line */
    {
        len = size - 1;
        line[len - 1] = '\n';
    }
    return len;
}

static inline void usb_line_written(void)
{
    usb_noted = usb_seen;
}
#else
static inline int usb_line(char *line, size_t size)
{
    (void)line;
    (void)size;
    return 0;
}

static inline void usb_line_written(void)
{
}
#endif

/* The kernel's full tables (kernel.h) on a line as the drive's, when they
   were found full more times than at the last line written. Those that panic
   next are left to the panic record. Its length, 0 for none. */
static unsigned int kernel_noted, kernel_seen;

static int kernel_line(char *line, size_t size)
{
    struct kernel_overflows k;
    kernel_get_overflows(&k);

    unsigned int dropped = k.dropped_others;
    for (int i = 0; i < KERNEL_FULL_QUEUE_OWNERS; i++)
        dropped += k.queues[i].dropped;
    kernel_seen = dropped + k.tables[KERNEL_TABLE_TIMEOUTS];
    if (kernel_seen == kernel_noted)
        return 0;

    int len = snprintf(line, size, "%ld kernel: events dropped from full"
                       " queues %u", current_tick / HZ, dropped);
    const char *sep = " (read by ";
    for (int i = 0; i < KERNEL_FULL_QUEUE_OWNERS && len < (int)size; i++)
    {
        if (!k.queues[i].dropped)
            continue;
        k.queues[i].owner[sizeof(k.queues[i].owner) - 1] = '\0';
        len += snprintf(line + len, size - len, "%s%s %u", sep,
                        k.queues[i].owner, k.queues[i].dropped);
        sep = ", ";
    }
    if (k.dropped_others && len < (int)size)
        len += snprintf(line + len, size - len, "%sothers %u", sep,
                        k.dropped_others);
    if (dropped && len < (int)size)
        len += snprintf(line + len, size - len, ")");
    if (len < (int)size)
        len += snprintf(line + len, size - len, "; timeouts lost %u\n",
                        k.tables[KERNEL_TABLE_TIMEOUTS]);
    if (len >= (int)size) /* cut: it still ends its line */
    {
        len = size - 1;
        line[len - 1] = '\n';
    }
    return len;
}

#ifdef HAVE_FAT_LINES
static bool free_noted; /* since boot */

/* The count is the one kept in memory since the mount (FSInfo's, then every
   cluster taken or freed): nothing is read from the disk for it. */
void health_note_free(void)
{
    sector_t size_kb, free_kb;
    const unsigned int cluster_kb = volume_get_cluster_size(IF_MV(0)) / 1024;
    volume_size(IF_MV(0,) &size_kb, &free_kb);
    if (!cluster_kb)
        return;

    free_noted = true;
    health_note("fat: %lu clusters free of %lu, %u KB each",
                (unsigned long)(free_kb / cluster_kb),
                (unsigned long)(size_kb / cluster_kb), cluster_kb);
}

/* The chains whose freeing stopped short (fat.h), on a line as the drive's,
   when there are more of either kind than at the last line written */
static struct fat_free_stops stops_noted, stops_seen;

static int fat_line(char *line, size_t size)
{
    /* a copy made without a yield: fat.c changes it under other threads */
    stops_seen = *fat_get_free_stops();
    if (stops_seen.range == stops_noted.range
        && stops_seen.limit == stops_noted.limit)
        return 0;
    return snprintf(line, size, "%ld fat: freeing stopped %u times at a"
                    " number outside the volume, %u at the file's limit; the"
                    " last, %s, at cluster %ld after %lu freed\n",
                    current_tick / HZ, stops_seen.range, stops_seen.limit,
                    stops_seen.last == FAT_STOP_RANGE ? "outside" : "limit",
                    stops_seen.cluster, stops_seen.freed);
}

static inline void fat_line_written(void)
{
    stops_noted = stops_seen;
}
#else
void health_note_free(void)
{
}

static inline int fat_line(char *line, size_t size)
{
    (void)line;
    (void)size;
    return 0;
}

static inline void fat_line_written(void)
{
}
#endif

#ifdef HAVE_CPU_TIME
#define CPU_LINE_MIN (60 * 1000000ull) /* the least time a line tells of */
#define CPU_NAMES    8 /* threads kept by name; any more go as others */

/* The time no line has told of yet: each flush takes the firmware's into it,
   and it is kept until a line of a minute or more is written */
static struct
{
    uint64_t usec[CPU_TIME_CLOCKS];
    unsigned int raised;
    struct
    {
        char name[CPU_TIME_NAME];
        uint64_t usec;
    } held[CPU_NAMES];
    uint64_t others;
} cpu_kept;

/* not on the stack, which already has the flush's other lines, as deep in
   a screen's or a plugin's calls as the USB came */
static struct cpu_time cpu_taken;
static char cpu_text[384];

static void cpu_take(void)
{
    cpu_time_take(&cpu_taken);
    for (int c = 0; c < CPU_TIME_CLOCKS; c++)
        cpu_kept.usec[c] += cpu_taken.usec[c];
    cpu_kept.raised += cpu_taken.raised;

    for (int i = 0; i < CPU_TIME_HELD; i++)
    {
        const uint64_t usec = cpu_taken.held[i].usec;
        if (!usec)
            continue;
        const char *name = i == CPU_TIME_HELD - 1 ? "ended threads"
                         : cpu_taken.held[i].name[0] ? cpu_taken.held[i].name
                         : "unnamed";
        int k = 0;
        while (k < CPU_NAMES && cpu_kept.held[k].name[0]
               && strcmp(cpu_kept.held[k].name, name))
            k++;
        if (k == CPU_NAMES)
            cpu_kept.others += usec;
        else
        {
            strcpy(cpu_kept.held[k].name, name);
            cpu_kept.held[k].usec += usec;
        }
    }
}

/* ", name 12.3 s" at len, as far as the line has room; its new length */
static int cpu_held_text(int len, const char *sep, const char *name,
                         uint64_t usec)
{
    const unsigned long tenths = (usec + 50000) / 100000;
    len = MIN(len, (int)sizeof(cpu_text) - 1);
    return len + snprintf(cpu_text + len, sizeof(cpu_text) - len,
                          "%s%s %lu.%lu s", sep, name, tenths / 10,
                          tenths % 10);
}

/* The time at each clock since the last line, or since boot, with the USB
   sessions left out, and the threads that held the boost up longest; with
   the flush's second. Its length, 0 for none: under a minute is kept for the
   next. Holds overlap, and so may add up to more than the time at 80 MHz. */
static int cpu_line(const char **text)
{
    const size_t size = sizeof(cpu_text);
    char *line = cpu_text;
    *text = line;
    cpu_take();

    uint64_t total = 0;
    for (int c = 0; c < CPU_TIME_CLOCKS; c++)
        total += cpu_kept.usec[c];
    if (total < CPU_LINE_MIN)
        return 0;

    unsigned int pm[CPU_TIME_CLOCKS]; /* per mille, rounded */
    for (int c = 0; c < CPU_TIME_CLOCKS; c++)
        pm[c] = (cpu_kept.usec[c] * 1000 + total / 2) / total;
    const unsigned long secs = (total + 500000) / 1000000;
    const unsigned long secs_80 = (cpu_kept.usec[CPU_TIME_80MHZ] + 500000)
                                  / 1000000;
    int len = snprintf(line, size, "%ld cpu: %lu s since start or the last"
                " cpu line, USB not counted; 24 MHz %u.%u%%, 30 MHz %u.%u%%,"
                " 80 MHz %u.%u%% (%lu s, raised %u time%s)", current_tick / HZ,
                secs, pm[CPU_TIME_24MHZ] / 10, pm[CPU_TIME_24MHZ] % 10,
                pm[CPU_TIME_30MHZ] / 10, pm[CPU_TIME_30MHZ] % 10,
                pm[CPU_TIME_80MHZ] / 10, pm[CPU_TIME_80MHZ] % 10, secs_80,
                cpu_kept.raised, cpu_kept.raised == 1 ? "" : "s");

    /* the three that held it longest, then the rest together; one that
       would read 0.0 s goes with the rest */
    bool told[CPU_NAMES] = { false };
    uint64_t others = cpu_kept.others;
    int listed = 0;
    for (; listed < 3; listed++)
    {
        int best = -1;
        for (int k = 0; k < CPU_NAMES; k++)
            if (!told[k] && cpu_kept.held[k].usec >= 50000
                && (best < 0
                    || cpu_kept.held[k].usec > cpu_kept.held[best].usec))
                best = k;
        if (best < 0)
            break;
        told[best] = true;
        len = cpu_held_text(len, listed ? ", " : "; boosted by ",
                            cpu_kept.held[best].name, cpu_kept.held[best].usec);
    }
    for (int k = 0; k < CPU_NAMES; k++)
        if (!told[k])
            others += cpu_kept.held[k].usec;
    if (others >= 50000)
        len = cpu_held_text(len, listed ? ", " : "; boosted by ", "others",
                            others);
    len = MIN(len, (int)size - 2); /* cut: it still ends its line */
    line[len++] = '\n';
    line[len] = '\0';
    return len;
}

static inline void cpu_line_written(void)
{
    memset(&cpu_kept, 0, sizeof(cpu_kept));
}
#endif /* HAVE_CPU_TIME */

void health_usb_ended(void)
{
#ifdef HAVE_CPU_TIME
    cpu_time_take(&cpu_taken); /* and dropped */
#endif
}

void health_flush(void)
{
#ifdef HAVE_FAT_LINES
    if (!free_noted) /* the first flush since boot, if no USB came before */
        health_note_free();
#endif
    /* the drive's line, the bus's, the FAT's, the kernel's and the clocks' go
       to the file itself, after the lines kept: they need no room among them */
    char drive[NOTE_MAX], usb[NOTE_MAX], fat[NOTE_MAX], kernel[NOTE_MAX];
    const int drive_len = ata_line(drive, sizeof(drive));
    const int usb_len = usb_line(usb, sizeof(usb));
    const int fat_len = fat_line(fat, sizeof(fat));
    const int kernel_len = kernel_line(kernel, sizeof(kernel));
    const char *cpu = NULL;
#ifdef HAVE_CPU_TIME
    const int cpu_len = cpu_line(&cpu);
#else
    const int cpu_len = 0;
#endif
    /* lines that come while this writes wait for the next time */
    const int len = notes_len;
    const int lost = notes_lost;
    if (!len && !drive_len && !usb_len && !fat_len && !kernel_len && !cpu_len)
        return;

    int fd = open(HEALTH_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0 && ffilesize(fd) >= HEALTH_FILE_MAX)
    {
        close(fd);
        /* begun again all the same if it can't be set aside */
        remove(HEALTH_OLD_FILE);
        rename(HEALTH_FILE, HEALTH_OLD_FILE);
        fd = open(HEALTH_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }
    if (fd < 0)
        return;
    const off_t size = ffilesize(fd);

    char text[112];
    const struct tm *tm = get_time();
    int n;
    if (valid_time(tm))
        n = snprintf(text, sizeof(text),
                     "--- %04d-%02d-%02d %02d:%02d:%02d, %ld s after start, %s\n",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     tm->tm_hour, tm->tm_min, tm->tm_sec, current_tick / HZ,
                     rbversion);
    else
        n = snprintf(text, sizeof(text), "--- %ld s after start, %s\n",
                     current_tick / HZ, rbversion);
    n = MIN(n, (int)sizeof(text) - 1);
    bool ok = write(fd, text, n) == n
              && (!len || write(fd, notes, len) == len);
    if (ok && lost)
    {
        n = snprintf(text, sizeof(text), "%d more lines found no room\n", lost);
        ok = write(fd, text, n) == n;
    }
    ok = ok && (!drive_len || write(fd, drive, drive_len) == drive_len)
            && (!usb_len || write(fd, usb, usb_len) == usb_len)
            && (!fat_len || write(fd, fat, fat_len) == fat_len)
            && (!kernel_len || write(fd, kernel, kernel_len) == kernel_len)
            && (!cpu_len || write(fd, cpu, cpu_len) == cpu_len);
    /* none of it rather than a part: it is all written again next time */
    if (!ok)
        ftruncate(fd, size);
    if (close(fd) < 0 || !ok)
        return;

    if (drive_len)
        ata_line_written();
    if (usb_len)
        usb_line_written();
    if (fat_len)
        fat_line_written();
    if (kernel_len)
        kernel_noted = kernel_seen;
#ifdef HAVE_CPU_TIME
    if (cpu_len)
        cpu_line_written();
#endif
    notes_len -= len;
    memmove(notes, notes + len, notes_len);
    notes_lost -= lost;
}
