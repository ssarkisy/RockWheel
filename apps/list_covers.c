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

/* The covers of albums in their rows: list_covers.h. Each is the one Cover
 * Flow shows, Finder's or the one Updating Library made for an album copied
 * outside Finder, shrunk from its slide to 32 px by averaging the pixels each
 * covers, made at Updating Library into list_covers.dat: a sector a cover, in
 * the order of Albums by year, after an index of the albums by their numbers
 * in the table of years. Read by the file's sectors on the disk, found once
 * as the file's index is taken in (file_map.h), without the file system's
 * work of a read by its name, whose path and chain of clusters held the CPU
 * a millisecond a cover on the iPod; by its name if they can't be. The rows
 * a frame is about to draw have theirs read just before it, on the UI's
 * thread, by the sectors from a disk that's on, a few milliseconds at most;
 * those still lacking and those of the rows next to them are read once the
 * wheel rests, a few in a row in one read, by the thread that reads an
 * album's header's. The wheel never waits for a disk coming on, and a row
 * shows the note in a cover's place until its cover comes, as on an iPod
 * classic, and an album without a cover keeps it. */

#include "config.h"
#include <string.h>
#include <stdlib.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "file_map.h"
#include "button.h"
#include "storage.h"
#include "core_alloc.h"
#include "settings.h"
#include "audio.h"
#include "lcd.h"
#include "rbpaths.h"
#include "plugin.h"
#include "gui/line.h"
#include "gui/list.h"
#include "tagcache.h"
#include "itunesdb.h"
#include "album_years.h"
#include "album_header.h"
#include "list_covers.h"

#ifdef ITUNESDB_HAVE_ART

#define SIDE        LINE_COVER_SIZE
#define SLOT_SIZE   (SIDE * SIDE * (int)sizeof(fb_data)) /* a sector, read as one */
#define NO_SLOT     0xffff

/* The file: its header, an entry for each album of the table of years by its
   number, and from a sector's start on the covers, one a slot, of the albums
   with one in the order of their years, as Albums by year has them. Made
   whole beside it and put in its place; again only as the database or its
   covers change, which leaves list_covers.todo */
#define COVERS_FILE    ROCKBOX_DIR "/list_covers.dat"
#define COVERS_TODO    ROCKBOX_DIR "/list_covers.todo"
#define COVERS_MAGIC   0x434c4252 /* "RBLC" */
#define COVERS_VERSION 1

struct file_header {
    uint32_t magic, version;
    int32_t commitid;   /* of the database whose table of years it's of */
    uint32_t count;     /* of albums, and their entries */
    uint32_t table;     /* album_years_hash() of the table */
    uint32_t slots;     /* of covers */
    uint32_t side;
    uint32_t unused;
};

struct file_entry {
    uint64_t key;       /* itunesdb_album_key() of its name and album artist */
    uint32_t source;    /* of the cover its slot was made of, 0 for none */
    uint16_t slot;      /* NO_SLOT for none */
    uint16_t unused;
};

static off_t data_start(uint32_t count)
{
    return ALIGN_UP(sizeof(struct file_header) + count * sizeof(struct file_entry),
                    SLOT_SIZE);
}

/* where the disk puts what's read at once, as STORAGE_ALIGN_BUFFER takes it,
   whatever the CPU's cache lines */
#define READ_ALIGN  64
static unsigned char *read_aligned(unsigned char *p)
{
    return (unsigned char *)ALIGN_UP((uintptr_t)p, READ_ALIGN);
}

#define KEPT        256   /* covers kept, 512 KB: what some 40 screens showed */
#define STRETCH     16    /* covers the making writes at once */
/* Covers a read takes at most, those in a row in the file, 8 KB: each read
   its own turn of the reader, which gives way to the UI between them, as
   Cover Flow's loader does its slides, and in each transfer it waits for
   (ata_dma_yield_always()). Two at least, which the map's check reads into */
#define READ_COVERS 4
#define WANTED_MAX  48    /* of a frame's rows, and of those next to them */
#define POLL        (HZ / 10) /* how often a list looks for those come */
/* The wheel turning, a step less than this ago: the reader then reads
   nothing, as the wheel's steps come faster than the frames and leave it no
   time, and each frame reads its own rows' covers (list_covers_draw_soon());
   the reader's come drawn only once it rests, as a frame meanwhile would
   hold the next step up */
#define AT_REST     (HZ / 30 + 1)
/* how long the reads before a frame take at most, those left lacking read
   before the next frame or by the reader; and the covers one of them takes,
   those in a row in the file, 16 KB: a screen's in Albums by year */
#define DRAW_SOON_US 6000
#define READ_NOW_COVERS 8
/* the pieces of the file on the disk mapped at most, 2 KB: one if it's in a
   row, and those of a disk whose free room is all in pieces; and how many
   places it's in pieces the map's check reads the covers either side of */
#define MAP_RUNS    128
#define CHECK_BREAKS 2

/* Taken as the setting is turned on, and only then: where it is doesn't
   move, so that the lists draw from it and the reader writes into it as it
   is, each at its own time. The covers by their slots in the file, the
   stages each thread reads into, and the map of where the covers are on the
   disk: made as the file's index is taken in, with the reader stopped, or
   again on the reader's thread as the wheel rests, and read by either */
struct covers {
    fb_data pixels[KEPT][SIDE * SIDE];
    int16_t slot[KEPT];         /* the file's slot each place has, -1 for none */
    uint32_t shown[KEPT];       /* the frame it was last drawn or read in */
    unsigned char stage[READ_COVERS * SLOT_SIZE + READ_ALIGN];
    /* the UI's, which reads as the reader waits on the disk with its own */
    unsigned char stage_now[READ_NOW_COVERS * SLOT_SIZE + READ_ALIGN];
#ifdef HAVE_FILE_MAP
    struct file_map map;        /* its runs 0 for none: read by the file's name */
    struct file_map_run runs[MAP_RUNS];
#endif
};
static struct covers *covers;   /* NULL with the setting off, or no room */
static int covers_handle;
/* no room when it was turned on: not asked for again as the settings are
   applied again, until it's turned on again */
static bool refused;
/* turned on as music played: taken once it has stopped */
static bool pending;
/* The file's slot of each album, as it was taken in with the memory: its
   table's count and hash, and where its covers start */
static int index_handle;
static int index_count;
static uint32_t index_table;
static off_t index_data;
static int index_slots;
#ifdef HAVE_FILE_MAP
/* the runs of the map that were checked against the file, kept as it's let
   go of as a read fails; 0 as the file is made again */
static int map_checked;
#endif
/* how many times they were forgotten, as a read in the middle is of before */
static unsigned forgets;
static uint32_t frame;

/* the slots the last frame lacked, and those of the rows next to it, asked
   for once it's drawn and the wheel rests; and those the reader is to read,
   from next on, each part in their order in the file */
static int16_t lacking[WANTED_MAX], ahead[WANTED_MAX];
static int lacking_count, ahead_count;
static bool asked;
static int16_t to_read[2 * WANTED_MAX];
static volatile int to_read_count, to_read_next;
/* the last button a list with covers took, as the wheel's steps */
static long last_input;

/* Flash, which keeps its power while the screen is lit and comes on from
   its sleep in 35-50 ms (ata.c): woken as a list with covers opens, or as a
   frame would read its covers but for it (wake_ahead()). A disk that spins,
   or flash without power, comes on in 0.6 s and more */
#if !defined(SIMULATOR) && (CONFIG_STORAGE & STORAGE_ATA) \
    && defined(HAVE_ATA_POWER_OFF) && defined(HAVE_FILE_MAP)
#define FLASH_WAKES 1
#else
#define FLASH_WAKES 0
#endif

/* the reader's transfers give the CPU up at once (ata_dma_yield_always());
   the UI's before a frame keep it as they wait (ata_spin_waits()) */
#if !defined(SIMULATOR) && defined(HAVE_ATA_DMA) && (CONFIG_STORAGE & STORAGE_ATA)
#define READ_YIELDS 1
#else
#define READ_YIELDS 0
#endif
#if !defined(SIMULATOR) && (CONFIG_STORAGE & STORAGE_ATA) && defined(USEC_TIMER)
#define READS_SPIN 1
#else
#define READS_SPIN 0
#endif

/* the time in us, for the reads before a frame (DRAW_SOON_US) */
static unsigned long usec_now(void)
{
#ifdef USEC_TIMER
    return USEC_TIMER;
#else
    return current_tick * (1000000 / HZ);
#endif
}

/* a cover the reader read since the list was last drawn, which draws it
   again as the wheel rests */
static volatile bool came;

/* the wheel's last step less than AT_REST ago */
static bool wheel_turning(void)
{
    return TIME_BEFORE(current_tick, last_input + AT_REST);
}

#ifdef HAVE_FILE_MAP
/* The map checked against the file, open: a few of its covers read both
   ways, the first and the last, and those either side of where it's in
   pieces, as a sector wrong shows another album's cover. How many were, -1
   if one differs or can't be read */
static int check_map(int fd)
{
    const struct file_map *map = &covers->map;
    unsigned char *by_name = read_aligned(covers->stage);
    unsigned char *by_sectors = by_name + SLOT_SIZE;
    int slots[2 + 2 * CHECK_BREAKS], n = 0;
    slots[n++] = 0;
    for (int i = 1; i < map->runs && i <= CHECK_BREAKS; i++)
    {
        /* the cover the piece begins in, which may go on from the one before,
           and the cover before it */
        const int slot = ((off_t)covers->runs[i].at * map->sector_size
                          - index_data) / SLOT_SIZE;
        slots[n++] = slot - 1;
        slots[n++] = slot;
    }
    slots[n++] = index_slots - 1;
    for (int k = 0; k < n; k++)
    {
        const off_t pos = index_data + (off_t)slots[k] * SLOT_SIZE;
        if (slots[k] < 0 || lseek(fd, pos, SEEK_SET) != pos
            || read(fd, by_name, SLOT_SIZE) != SLOT_SIZE
            || file_read_mapped(map, covers->runs, pos, SLOT_SIZE, by_sectors)
            || memcmp(by_name, by_sectors, SLOT_SIZE))
            return -1;
    }
    return n;
}

/* Where the covers of the file open are on the disk, of its index as it was
   taken in, with the reader stopped or on its thread: checked, and none if
   it's wrong, or in more pieces than kept, read by the file's name then.
   Made again, as any file's clusters freed have the map made again, the
   same runs as those checked before aren't checked again: the same file's
   in the same sectors. They're kept in the stage meanwhile, free then */
static void map_covers(int fd, bool again)
{
    struct file_map_run *before = (struct file_map_run *)read_aligned(covers->stage);
    const int checked_before = again ? map_checked : 0;
    memcpy(before, covers->runs, checked_before * sizeof(*before));
    map_checked = 0;
    const int runs = file_map(fd, index_data, (off_t)index_slots * SLOT_SIZE,
                              &covers->map, covers->runs, MAP_RUNS);
    if (runs <= 0)
        return;
    if ((runs != checked_before
         || memcmp(before, covers->runs, runs * sizeof(*before)))
        && check_map(fd) < 0)
        covers->map.runs = 0;
    else
        map_checked = runs;
}

/* the map made again, on the reader's thread, as the disk's files have
   changed since, one's clusters freed, or it was unmounted for USB: of the
   file as it is now */
static void map_again(void)
{
    const int fd = open(COVERS_FILE, O_RDONLY);
    if (fd < 0)
        return;
    map_covers(fd, true);
    close(fd);
}
#endif /* HAVE_FILE_MAP */

static void forget(void)
{
    forgets++;
    to_read_count = to_read_next = 0;
    if (covers)
        memset(covers->slot, 0xff, sizeof(covers->slot));
}

/* The file's index, for the table of years as it is: its slots taken in, as
   the memory is, without music, and as the file's made again; and where the
   covers are on the disk, with the file open once more. The reader done
   first with the file and the map as they were */
void list_covers_load(void)
{
    album_header_reader_wait();
    if (index_handle > 0)
        index_handle = core_free(index_handle);
    index_count = 0;
#ifdef HAVE_FILE_MAP
    if (covers)
        covers->map.runs = 0;
#endif
    const int count = album_years_count();
    if (!covers || count <= 0)
        return;
    struct file_header h;
    int fd = open(COVERS_FILE, O_RDONLY);
    if (fd < 0)
        return;
    if (read(fd, &h, sizeof(h)) == sizeof(h) && h.magic == COVERS_MAGIC
        && h.version == COVERS_VERSION && h.side == SIDE
        && h.commitid == tagcache_get_commitid() && h.count == (uint32_t)count
        && h.table == album_years_hash())
        index_handle = core_alloc(count * sizeof(uint16_t));
    if (index_handle > 0)
    {
        /* a stretch at a time, as the colours of the slides are read */
        static struct file_entry e[64];
        int n = 0;
        while (n < count)
        {
            const ssize_t got = read(fd, e, MIN(count - n, (int)ARRAYLEN(e))
                                            * sizeof(*e));
            if (got < (ssize_t)sizeof(*e))
                break;
            uint16_t *slots = core_get_data(index_handle);
            for (size_t k = 0; k < got / sizeof(*e); k++)
                slots[n++] = e[k].slot < h.slots ? e[k].slot : NO_SLOT;
        }
        if (n == count)
        {
            index_count = count;
            index_table = h.table;
            index_data = data_start(count);
            index_slots = h.slots;
#ifdef HAVE_FILE_MAP
            if (index_slots > 0)
                map_covers(fd, false);
#endif
        }
        else
            index_handle = core_free(index_handle);
    }
    close(fd);
    forget();
}

/* the memory of the covers, without music, which it would stop */
static void take(void)
{
    const int handle = core_alloc_ex(sizeof(struct covers), &buflib_ops_locked);
    if (handle > 0)
    {
        covers_handle = handle;
        covers = core_get_data(handle);
#ifdef HAVE_FILE_MAP
        memset(&covers->map, 0, sizeof(covers->map));
#endif
        list_covers_load();
    }
    else
        refused = true;
}

static void let_go(void)
{
    /* not while the reader writes into them */
    album_header_reader_wait();
    if (index_handle > 0)
        index_handle = core_free(index_handle);
    index_count = 0;
    core_free(covers_handle);
    covers_handle = 0;
    covers = NULL;
}

void list_covers_apply(void)
{
    const bool wanted = global_settings.list_covers
                        && global_settings.album_art != AA_OFF;
    if (!wanted)
    {
        refused = pending = false;
        if (covers)
            let_go();
        return;
    }
    if (covers || refused)
        return;
    /* the audio buffer would give way, the music stopped for it */
    pending = audio_status() != 0;
    if (!pending)
        take();
}

void list_covers_changed(bool on)
{
    (void)on;
    refused = false;
    list_covers_apply();
}

/* the file's index taken in, of the table of years as it is */
static bool index_ready(void)
{
    return index_handle > 0 && album_years_count() == index_count
           && album_years_hash() == index_table;
}

bool list_covers_on(void)
{
    return covers && global_settings.album_art != AA_OFF && index_ready();
}

void list_covers_forget(void)
{
    forget();
    /* the file made again at the next Updating Library */
    close(open(COVERS_TODO, O_WRONLY | O_CREAT | O_TRUNC, 0666));
}

/* an album's slot in the file, -1 for none or none known, as with the file
   of another table */
static int slot_of(int no)
{
    if (!covers || no < 0 || no >= index_count || !index_ready())
        return -1;
    const int slot = ((const uint16_t *)core_get_data(index_handle))[no];
    return slot == NO_SLOT ? -1 : slot;
}

static int kept_place(int slot)
{
    for (int i = 0; i < KEPT; i++)
    {
        if (covers->slot[i] == slot)
            return i;
    }
    return -1;
}

/* a slot into a list of them, once */
static void want(int16_t *list, int *count, int slot)
{
    for (int i = 0; i < *count; i++)
    {
        if (list[i] == slot)
            return;
    }
    if (*count < WANTED_MAX)
        list[(*count)++] = slot;
}

int list_covers_get(int no, const fb_data **pixels)
{
    const int slot = slot_of(no);
    if (slot < 0)
        return LIST_COVER_NOTE;
    const int place = kept_place(slot);
    if (place >= 0)
    {
        covers->shown[place] = frame;
        *pixels = covers->pixels[place];
        return LIST_COVER_PIXELS;
    }
    want(lacking, &lacking_count, slot);
    return LIST_COVER_NOTE;
}

void list_covers_ahead(int no)
{
    const int slot = slot_of(no);
    if (slot >= 0 && kept_place(slot) < 0)
        want(ahead, &ahead_count, slot);
}

void list_covers_frame(void)
{
    frame++;
    lacking_count = ahead_count = 0;
    asked = false;
    /* those come so far drawn in it */
    came = false;
}

static int cmp_slot(const void *a, const void *b)
{
    return *(const int16_t *)a - *(const int16_t *)b;
}

void list_covers_input(void)
{
    last_input = current_tick;
}

/* the sooner of a wait for a button, as get_action() takes it, and ticks */
static int sooner(int timeout, int ticks)
{
    ticks = MAX(ticks, 0);
    return timeout < 0 || timeout > ticks ? ticks : timeout;
}

/* What the frame lacked, then the rows next to it, each in the file's order,
   in place of what was asked for before, which the wheel has turned past.
   Those next to it only from a disk that's on, which they don't wake */
static void ask(void)
{
    asked = true;
    const int ahead_n = storage_disk_is_active() ? ahead_count : 0;
    memcpy(to_read, lacking, lacking_count * sizeof(*lacking));
    qsort(to_read, lacking_count, sizeof(*to_read), cmp_slot);
    memcpy(to_read + lacking_count, ahead, ahead_n * sizeof(*ahead));
    qsort(to_read + lacking_count, ahead_n, sizeof(*to_read), cmp_slot);
    to_read_next = 0;
    to_read_count = lacking_count + ahead_n;
    if (to_read_count > 0)
        album_header_reader_start();
}

#if FLASH_WAKES
/* Flash that's asleep or off as a list with covers opens, or as a frame
   would read its covers but for it: woken by the storage's thread
   (storage_wake_ahead()), the UI not waiting for it, so that the covers come
   with the next frame. Once until it's on, or asked again WAKE_AGAIN on if
   it hasn't come on. A disk that spins isn't: it spins up as the reader
   reads them. While it comes on, the storage's thread holds the disk's
   lock, and a read of the UI's waits for it, 35-50 ms from asleep, a frame,
   as it would have waited to wake it itself */
#define WAKE_AGAIN (2 * HZ)
static bool waking;
static long waking_tick;

static void wake_ahead(void)
{
    if (storage_disk_is_active() || !ata_disk_isssd())
        return;
    if (!waking || TIME_AFTER(current_tick, waking_tick + WAKE_AGAIN))
    {
        waking = true;
        waking_tick = current_tick;
        storage_wake_ahead();
    }
}

/* the disk on since it was woken: woken again only once it has gone asleep
   or off again */
static void woken(void)
{
    if (waking && storage_disk_is_active())
        waking = false;
}
#endif /* FLASH_WAKES */

void list_covers_opened(void)
{
#if FLASH_WAKES
    if (covers)
        wake_ahead();
#endif
}

int list_covers_timeout(int timeout)
{
    if (!covers)
        return timeout;
#if FLASH_WAKES
    woken();
#endif
    /* Asked for once the wheel rests and no button waits, whose frame comes
       first and asks again: as it turns, a frame reads its own before it's
       drawn (list_covers_draw_soon()) */
    const long rest = last_input + AT_REST;
    const bool turning = TIME_BEFORE(current_tick, rest);
    const bool to_ask = lacking_count + ahead_count > 0 && !asked;
    if (to_ask && !turning && button_queue_empty())
        ask();
    /* looked again as it rests, to ask, and draw what's come */
    if (turning && (to_ask || to_read_next < to_read_count || came))
        return sooner(timeout, rest - current_tick);
    if (to_read_next < to_read_count)
        timeout = sooner(timeout, POLL);
    return timeout;
}

bool list_covers_poll(void)
{
    /* Those come drawn once no button waits and the wheel rests: a frame as
       it turns would hold its next step up, whose own frame draws them */
    if (!covers || !came || !button_queue_empty() || wheel_turning())
        return false;
    came = false;
    return true;
}

bool list_covers_to_read(void)
{
    /* none while a button waits: the UI's first */
    return covers && to_read_next < to_read_count && button_queue_empty();
}

void list_covers_cancel(void)
{
    to_read_count = to_read_next = 0;
}

/* The place a cover is read into: an empty one, or else the one drawn
   longest ago; never one of the frame now, which shows it. -1 for none */
static int place_to_fill(void)
{
    int oldest = -1;
    for (int i = 0; i < KEPT; i++)
    {
        if (covers->slot[i] < 0)
            return i;
        if (covers->shown[i] != frame
            && (oldest < 0 || (int32_t)(covers->shown[i] - covers->shown[oldest]) < 0))
            oldest = i;
    }
    return oldest;
}

/* by the file's name, from open to close */
static bool read_by_name(off_t pos, ssize_t size, unsigned char *stage)
{
    const int fd = open(COVERS_FILE, O_RDONLY);
    const bool ok = fd >= 0 && lseek(fd, pos, SEEK_SET) == pos
                    && read(fd, stage, size) == size;
    if (fd >= 0)
        close(fd);
    return ok;
}

/* The covers a frame lacks read before it's drawn: by the file's sectors,
   with a map good, from a disk that's on, not coming on, which would hold the
   UI as long. Flash asleep woken meanwhile, the UI not waiting, for the next
   frame; a disk asleep left to the reader as the wheel rests. In the
   simulator by the file's name, which costs it nothing of the iPod's */
static bool reads_now(void)
{
#if defined(HAVE_FILE_MAP)
    if (covers->map.runs <= 0 || !file_map_current(&covers->map))
        return false;
#if FLASH_WAKES
    /* a hard disk's seek waited out before each frame would hold the wheel:
       its covers come from the reader as the wheel rests */
    if (!ata_disk_isssd())
        return false;
    if (ata_disk_is_on())
        return true;
    wake_ahead();
    return false;
#else
    return storage_disk_is_active();
#endif
#elif defined(SIMULATOR)
    return true;
#else
    return false;
#endif
}

static bool read_now(off_t pos, ssize_t size, unsigned char *stage)
{
#if defined(HAVE_FILE_MAP)
    const int rc = file_read_mapped(&covers->map, covers->runs, pos, size, stage);
    if (rc == 0)
        return true;
    /* the disk's files changed meanwhile, or it failed: the reader reads by
       the file's name, and maps it again once they change */
    covers->map.runs = 0;
    return false;
#else
    return read_by_name(pos, size, stage);
#endif
}

void list_covers_draw_soon(struct gui_synclist *list)
{
    if (!covers || !list->callback_get_cover)
        return;
    /* the rows it draws: each lacking cover wanted, as the frame does */
    const int start = list->start_item[SCREEN_MAIN];
    const int end = MIN(start + list_get_nb_lines(list, SCREEN_MAIN),
                        list->nb_items);
    for (int row = start; row < end; row++)
    {
        const fb_data *pixels;
        list->callback_get_cover(row, list->data, &pixels);
    }
    int16_t now[WANTED_MAX];
    const int count = lacking_count;
    memcpy(now, lacking, count * sizeof(*now));
    /* counted again as it's drawn, those still lacking then */
    lacking_count = 0;
    if (count == 0 || !reads_now())
        return;

    /* In the file's order, those near each other in a read, as long as
       there's time. Waiting on the disk without yielding: a screen's took
       1.7 ms on the iPod, while a thread yielded to kept the CPU for
       milliseconds, and the last rows went without */
    qsort(now, count, sizeof(*now), cmp_slot);
    unsigned char *stage = read_aligned(covers->stage_now);
    const unsigned long start_us = usec_now();
#if READS_SPIN
    ata_spin_waits(true);
#endif
    for (int k = 0, n; k < count && usec_now() - start_us < DRAW_SOON_US; k += n)
    {
        const int first = now[k];
        n = 1;
        while (k + n < count && now[k + n] - first < READ_NOW_COVERS)
            n++;
        const int last = now[k + n - 1];
        if (!read_now(index_data + (off_t)first * SLOT_SIZE,
                      (last - first + 1) * SLOT_SIZE, stage))
            break;
        for (int i = k; i < k + n; i++)
        {
            /* the reader's meanwhile, as this one waited on the disk */
            if (kept_place(now[i]) >= 0)
                continue;
            const int place = place_to_fill();
            if (place < 0)
                break;
            memcpy(covers->pixels[place], stage + (now[i] - first) * SLOT_SIZE,
                   SLOT_SIZE);
            covers->slot[place] = now[i];
            covers->shown[place] = frame;
        }
    }
#if READS_SPIN
    ata_spin_waits(false);
#endif
}

/* On the reader's thread: the next slot asked for that isn't kept, and those
   after it as far as a read takes, in one read, each into a place then. The
   slots taken before the read, which waits for the disk while the lists ask
   for others in their place; of the file as it was taken in, which isn't
   made again while lists show. By its sectors if they're mapped; by its
   name otherwise. Only as the wheel rests: while it turns, those asked for
   wait for it to rest, as they're asked for again then, and each frame
   reads its own (list_covers_draw_soon()), the disk not taken from it */
void list_covers_read(void)
{
    if (wheel_turning())
    {
        list_covers_cancel();
        return;
    }
    int16_t batch[READ_COVERS] = { 0 };
    int n = 0, first = -1, last = -1;
    while (to_read_next < to_read_count && n < READ_COVERS)
    {
        const int slot = to_read[to_read_next];
        if (first < 0)
        {
            to_read_next++;
            if (kept_place(slot) < 0)
                batch[n++] = first = last = slot;
        }
        else if (slot > last && slot - first < READ_COVERS)
        {
            to_read_next++;
            batch[n++] = last = slot;
        }
        else
            break;
    }
    if (n == 0)
        return;

    const unsigned forgets_then = forgets;
    unsigned char *stage = read_aligned(covers->stage);
    const ssize_t size = (last - first + 1) * SLOT_SIZE;
    const off_t pos = index_data + (off_t)first * SLOT_SIZE;
    /* on a thread behind the UI (album_header.c), not waiting out its
       transfers on flash, as such threads do for a while: the CPU is the UI's
       as soon as each starts */
#if READ_YIELDS
    ata_dma_yield_always(true);
#endif
    bool ok = false;
#ifdef HAVE_FILE_MAP
    /* made again as the disk's files have changed since it was made or tried,
       as the wheel rests: while it turns, a read without a good map hasn't
       come this far (above) */
    if (!file_map_current(&covers->map))
        map_again();
    if (covers->map.runs > 0)
    {
        ok = !file_read_mapped(&covers->map, covers->runs, pos, size, stage);
        /* The disk's files changed meanwhile, or it failed: by the name from
           now on, until they change, as the map's made again then; this read
           too, unless the wheel has turned meanwhile */
        if (!ok)
            covers->map.runs = 0;
    }
#endif
    if (!ok && !wheel_turning())
        ok = read_by_name(pos, size, stage);
#if READ_YIELDS
    ata_dma_yield_always(false);
#endif
    if (!ok || forgets != forgets_then || !covers)
        return;
    for (int i = 0; i < n; i++)
    {
        const int slot = batch[i];
        if (kept_place(slot) >= 0)
            continue;
        const int place = place_to_fill();
        if (place < 0)
            break;
        memcpy(covers->pixels[place], stage + (slot - first) * SLOT_SIZE, SLOT_SIZE);
        covers->slot[place] = slot;
        covers->shown[place] = frame;
        came = true;
    }
    /* the UI's turn before the next read, as Cover Flow's loader's */
    yield();
}

/** Making the file, at Updating Library **/

bool list_covers_stale(void)
{
    /* made whether the setting is on or not, so that turning it on shows them
       at once; none without anything to make them from */
    const struct tagcache_stat *tc = tagcache_get_stat();
    if (global_settings.album_art == AA_OFF || !tc->ready || !tc->ramcache)
        return false;
    if (file_exists(COVERS_TODO))
        return true;
    struct file_header h;
    const int fd = open(COVERS_FILE, O_RDONLY);
    const bool made = fd >= 0 && read(fd, &h, sizeof(h)) == sizeof(h)
                      && h.magic == COVERS_MAGIC && h.version == COVERS_VERSION
                      && h.side == SIDE && h.commitid == tagcache_get_commitid();
    if (fd >= 0)
        close(fd);
    return !made;
}

/* an album's key, as Cover Flow keys albums, from the names in memory */
static uint64_t album_key(int no)
{
    static char name[MAX_PATH], artist[MAX_PATH];
    if (!tagcache_tag_string(tag_album, album_years_album(no), name, sizeof(name)))
        name[0] = '\0';
    if (!tagcache_tag_string(tag_albumartist, album_years_artist(no), artist,
                             sizeof(artist)))
        artist[0] = '\0';
    return itunesdb_album_key(name, artist, NULL);
}

/* what a cover is made of, as its frame and slide are: another after a sync
   that changed it, or Updating Library's of its folder's image again */
static uint32_t art_source(const struct itunesdb_art *art)
{
    uint32_t h = 2166136261u;
    for (const char *p = art->path; *p; p++)
        h = (h ^ (unsigned char)*p) * 16777619u;
    const uint32_t parts[] = {
        art->offset, art->width, art->height, art->slide, art->colour
    };
    for (size_t i = 0; i < ARRAYLEN(parts); i++)
        h = (h ^ parts[i]) * 16777619u;
    return h | 1;
}

/* in the middle of its place, on white, as Finder makes covers square */
static void centre(fb_data *pixels, int w, int h)
{
    const int left = (SIDE - w) / 2, top = (SIDE - h) / 2;
    for (int y = h - 1; y >= 0; y--)
        memmove(&pixels[(top + y) * SIDE + left], &pixels[y * w],
                w * sizeof(*pixels));
    for (int y = 0; y < SIDE; y++)
    {
        for (int x = 0; x < SIDE; x++)
        {
            if (y < top || y >= top + h || x < left || x >= left + w)
                pixels[y * SIDE + x] = LCD_WHITE;
        }
    }
}

/* A slide, w by h a column after another, as Cover Flow has them, into a
   cover by rows: each pixel the mean of those of the slide it covers, which
   this CPU's lack of a divide has done by 65536ths */
#define BOX_MAX 64
static void shrink_slide(const fb_data *slide, int w, int h, fb_data *out)
{
    static uint32_t recip[BOX_MAX + 1];
    if (!recip[1])
    {
        for (int n = 1; n <= BOX_MAX; n++)
            recip[n] = (65536 + n / 2) / n;
    }
    const int big = MAX(w, h);
    const int ow = MAX(1, (SIDE * w + big / 2) / big);
    const int oh = MAX(1, (SIDE * h + big / 2) / big);
    for (int ox = 0; ox < ow; ox++)
    {
        const int x0 = ox * w / ow, x1 = MAX((ox + 1) * w / ow, x0 + 1);
        for (int oy = 0; oy < oh; oy++)
        {
            const int y0 = oy * h / oh, y1 = MAX((oy + 1) * h / oh, y0 + 1);
            uint32_t r = 0, g = 0, b = 0;
            for (int x = x0; x < x1; x++)
            {
                const fb_data *p = slide + x * h;
                for (int y = y0; y < y1; y++)
                {
                    r += p[y] >> 11;
                    g += (p[y] >> 5) & 0x3f;
                    b += p[y] & 0x1f;
                }
            }
            const uint32_t k = recip[MIN((x1 - x0) * (y1 - y0), BOX_MAX)];
            out[oy * ow + ox] = (fb_data)(((r * k + 0x8000) >> 16) << 11
                                          | ((g * k + 0x8000) >> 16) << 5
                                          | ((b * k + 0x8000) >> 16));
        }
    }
    if (ow < SIDE || oh < SIDE)
        centre(out, ow, oh);
}

/* an album's cover into out, from its slide, or its frame without one */
static bool make_cover(const struct itunesdb_art *art, fb_data *out,
                       unsigned char *slide)
{
    int w, h;
    if (art->slide > 0 && itunesdb_load_slide(art, slide, &w, &h) > 0)
    {
        shrink_slide((const fb_data *)slide, w, h, out);
        return true;
    }
    struct bitmap bm;
    memset(&bm, 0, sizeof(bm));
    bm.width = SIDE;
    bm.height = SIDE;
    bm.data = (unsigned char *)out;
    if (itunesdb_load_small_art(art, &bm, sizeof(bm) + SLOT_SIZE) < 0)
        return false;
    if (bm.width < SIDE || bm.height < SIDE)
        centre(out, bm.width, bm.height);
    return true;
}

/* the albums with covers by their dates, those of a date by their numbers,
   which go by name: as Albums by year has them */
static int cmp_by_year(const void *a, const void *b)
{
    const int x = *(const uint16_t *)a, y = *(const uint16_t *)b;
    /* those without a date after them all */
    const uint32_t ox = album_date_order(album_years_date(x)) - 1;
    const uint32_t oy = album_date_order(album_years_date(y)) - 1;
    if (ox != oy)
        return ox < oy ? -1 : 1;
    return x - y;
}

/* the old entries by key, their numbers sorted, for the covers kept */
static const struct file_entry *old_entries;
static int cmp_old_key(const void *a, const void *b)
{
    const uint64_t x = old_entries[*(const uint16_t *)a].key;
    const uint64_t y = old_entries[*(const uint16_t *)b].key;
    return (x > y) - (x < y);
}

/* the old album of a key and source with a cover in the old file, -1 for none */
static int old_cover(const uint16_t *by_key, int n, uint64_t key, uint32_t source)
{
    int lo = 0, hi = n;
    while (lo < hi)
    {
        const int mid = (lo + hi) / 2;
        if (old_entries[by_key[mid]].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (; lo < n && old_entries[by_key[lo]].key == key; lo++)
    {
        const struct file_entry *e = &old_entries[by_key[lo]];
        if (e->source == source && e->slot != NO_SLOT)
            return by_key[lo];
    }
    return -1;
}

/* A file of no covers for the database, as one too large to make: made for
   it all the same, so that Updating Library isn't shown at each start for it,
   and again after the next sync */
static void make_none(void)
{
    const struct file_header h = {
        .magic = COVERS_MAGIC, .version = COVERS_VERSION,
        .commitid = tagcache_get_commitid(), .side = SIDE,
    };
    if (whole_file_write(COVERS_FILE, &h, sizeof(h)))
        remove(COVERS_TODO);
    list_covers_load();
}

/* Made from the old file's covers that are of the same albums and covers,
   the others from their slides: in the plugins' buffer, free at Updating
   Library. Each album's cover looked for first, then the file written in
   the order of their years, a stretch of covers a write */
void list_covers_make(void (*progress)(int done, int total), bool (*go_on)(void))
{
    /* nothing read from the file meanwhile, the old one swapped for none too;
       by its sectors not even after, until it's mapped again as it's taken in,
       as they're being written or freed */
    album_header_reader_wait();
#ifdef HAVE_FILE_MAP
    if (covers)
        covers->map.runs = 0;
    map_checked = 0;
#endif
    const int count = album_years_count();
    if (count <= 0 || count >= NO_SLOT)
    {
        make_none();
        return;
    }
    size_t room;
    unsigned char *buf = plugin_get_buffer(&room);
    struct file_header old_h = { 0 };
    int old_fd = open(COVERS_FILE, O_RDONLY);
    if (old_fd >= 0
        && (read(old_fd, &old_h, sizeof(old_h)) != sizeof(old_h)
            || old_h.magic != COVERS_MAGIC || old_h.version != COVERS_VERSION
            || old_h.side != SIDE || old_h.count >= NO_SLOT))
        old_h.count = 0;
    const int old_count = old_fd >= 0 ? (int)old_h.count : 0;
    /* the parts of the buffer: the albums' entries, the slots' albums, the
       old entries and their order by key, a slide, a stretch of covers */
    const size_t slide_room = ITUNESDB_SLIDE_ROOM
        + ITUNESDB_SLIDE_SIZE * ITUNESDB_SLIDE_SIZE * sizeof(fb_data);
    const size_t need = count * (sizeof(struct file_entry) + sizeof(uint16_t))
        + old_count * (sizeof(struct file_entry) + sizeof(uint16_t))
        + slide_room + STRETCH * SLOT_SIZE + READ_ALIGN + 16;
    if (!buf || room < need)
    {
        if (old_fd >= 0)
            close(old_fd);
        make_none();
        return;
    }
    struct file_entry *entries = (struct file_entry *)ALIGN_UP((uintptr_t)buf, 8);
    struct file_entry *old = entries + count;
    uint16_t *order = (uint16_t *)(old + old_count);
    uint16_t *by_key = order + count;
    /* the slide aligned by itunesdb_load_slide() within its room */
    unsigned char *slide = (unsigned char *)(by_key + old_count);
    unsigned char *out = read_aligned(slide + slide_room);
    bool ok = old_count == 0
              || read(old_fd, old, old_count * sizeof(*old))
                 == (ssize_t)(old_count * sizeof(*old));
    const int usable_old = ok ? old_count : 0;
    old_entries = old;
    for (int i = 0; i < usable_old; i++)
        by_key[i] = i;
    qsort(by_key, usable_old, sizeof(*by_key), cmp_old_key);

    /* each album's key and what its cover is, and those with one by year */
    cpu_boost(true);
    const int total = 2 * count;
    int slots = 0;
    bool stopped = false;
    for (int no = 0; no < count && !stopped; no++)
    {
        struct itunesdb_art art;
        const int32_t song = album_years_first_track(no);
        const bool found = song >= 0 && album_header_find_art(song, &art) > 0;
        entries[no] = (struct file_entry){
            .key = album_key(no), .source = found ? art_source(&art) : 0,
            .slot = NO_SLOT,
        };
        if (found)
            order[slots++] = no;
        if ((no & 15) == 0)
        {
            progress(no, total);
            stopped = !go_on();
        }
    }
    qsort(order, slots, sizeof(*order), cmp_by_year);
    for (int s = 0; s < slots; s++)
        entries[order[s]].slot = s;

    const struct file_header h = {
        .magic = COVERS_MAGIC, .version = COVERS_VERSION,
        .commitid = tagcache_get_commitid(), .count = count,
        .table = album_years_hash(), .slots = slots, .side = SIDE,
    };
    /* the same albums with the same covers in the same places: only the
       database it's of, which is in the header alone */
    bool same = !stopped && usable_old == count && old_h.table == h.table
                && old_h.slots == (uint32_t)slots;
    for (int no = 0; no < count && same; no++)
        same = old[no].key == entries[no].key && old[no].source == entries[no].source
               && old[no].slot == entries[no].slot;
    if (same)
    {
        close(old_fd);
        old_fd = -1;
        const int fd = open(COVERS_FILE, O_WRONLY);
        stopped = fd < 0 || write(fd, &h, sizeof(h)) != sizeof(h);
        if (fd >= 0)
            close(fd);
    }

    /* the covers from a sector's start, zeros before it: written, as the
       iPod's file system seeks no further than a file's end */
    const int fd = same || stopped ? -1 : whole_file_open(COVERS_FILE);
    const ssize_t pad = data_start(count) - sizeof(h) - count * sizeof(*entries);
    memset(out, 0, pad);
    ok = fd >= 0 && write(fd, &h, sizeof(h)) == sizeof(h)
         && write(fd, entries, count * sizeof(*entries))
            == (ssize_t)(count * sizeof(*entries))
         && write(fd, out, pad) == pad;
    /* the covers, a stretch at a time */
    for (int s = 0; s < slots && ok && !stopped; s++)
    {
        const int no = order[s];
        fb_data *cover = (fb_data *)(out + (s % STRETCH) * SLOT_SIZE);
        const int was = old_cover(by_key, usable_old, entries[no].key,
                                  entries[no].source);
        const off_t old_pos = data_start(old_count) + (off_t)(was >= 0 ? old[was].slot : 0)
                              * SLOT_SIZE;
        struct itunesdb_art art;
        const bool kept = was >= 0 && lseek(old_fd, old_pos, SEEK_SET) == old_pos
                          && read(old_fd, cover, SLOT_SIZE) == SLOT_SIZE;
        if (!kept && !(album_header_find_art(album_years_first_track(no), &art) > 0
                       && make_cover(&art, cover, slide)))
        {
            /* gone meanwhile, or unreadable: blank, with the note shown */
            memset(cover, 0xff, SLOT_SIZE);
            entries[no].slot = NO_SLOT;
            entries[no].source = 0;
        }
        if (s % STRETCH == STRETCH - 1 || s == slots - 1)
        {
            const ssize_t size = (s % STRETCH + 1) * SLOT_SIZE;
            ok = write(fd, out, size) == size;
        }
        if ((s & 15) == 0)
        {
            progress(count + s * count / MAX(slots, 1), total);
            stopped = !go_on();
        }
    }
    cpu_boost(false);
    if (old_fd >= 0)
        close(old_fd);
    if (fd >= 0)
    {
        /* the entries again, as some covers couldn't be made */
        ok = ok && !stopped && lseek(fd, sizeof(h), SEEK_SET) == sizeof(h)
             && write(fd, entries, count * sizeof(*entries))
                == (ssize_t)(count * sizeof(*entries));
        if (!whole_file_close(fd, COVERS_FILE, ok))
            return;
    }
    else if (!same || stopped)
        return;
    remove(COVERS_TODO);
    /* those kept were of the file before */
    list_covers_load();
}

#else /* !ITUNESDB_HAVE_ART */

void list_covers_apply(void) {}
void list_covers_changed(bool on) { (void)on; }
bool list_covers_on(void) { return false; }
int list_covers_get(int no, const fb_data **pixels)
{
    (void)no; (void)pixels;
    return LIST_COVER_NOTE;
}
void list_covers_ahead(int no) { (void)no; }
void list_covers_frame(void) {}
int list_covers_timeout(int timeout) { return timeout; }
bool list_covers_poll(void) { return false; }
void list_covers_forget(void) {}
bool list_covers_to_read(void) { return false; }
void list_covers_read(void) {}
void list_covers_cancel(void) {}
bool list_covers_stale(void) { return false; }
void list_covers_make(void (*progress)(int done, int total), bool (*go_on)(void))
{
    (void)progress; (void)go_on;
}
void list_covers_load(void) {}
void list_covers_input(void) {}
void list_covers_opened(void) {}
void list_covers_draw_soon(struct gui_synclist *list) { (void)list; }

#endif /* ITUNESDB_HAVE_ART */
