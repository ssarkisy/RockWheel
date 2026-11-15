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
/* The plays for Last.fm, as Audioscrobbler's Portable Player Logging 1.1 has
   them: after a header, a line a play,

     artist TAB album TAB title TAB track TAB seconds TAB L|S TAB start TAB mbid

   L for a track heard as Last.fm counts a play, S for one skipped before
   that; album, track and MusicBrainz id may be empty (plan: ipod-data
   plans-1.5/lastfm). The lines wait in memory, taken only with Last.fm on.
   They go to the disk at USB and at shutdown, and otherwise only at the
   disk's own idle, as it would sleep, never waking it: once the music has
   stopped or is paused; and while it plays only with the log 3/4 full, the
   battery low but still safe for the disk, or Last.fm switched off with
   lines waiting.

   A stop that keeps the track's place, as a pause ended by power-off, USB or
   the audio buffer giving way to other memory, doesn't end the play: it
   waits, kept over a power-off in SUSPEND_FILE, and the same file played on
   from about that place goes on with it, so that a listening is one line. */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "rbpaths.h"
#include "pcm.h"
#include "audio.h"
#include "core_alloc.h"
#include "powermgmt.h"
#include "timefuncs.h"
#include "settings.h"
#include "version.h"
#include "ata_idle_notify.h"
#include "tagcache.h"
#include "itunesdb.h"
#include "scrobbler.h"
#include "debug.h"

#define SCROBBLER_FILE      "/.scrobbler.log"
/* the play a stop left waiting, over a power-off */
#define SUSPEND_FILE        ROCKBOX_DIR "/scrobbler_play.dat"
#define SUSPEND_MAGIC       0x50535752 /* "RWSP" */

/* The log's times (plan.md, decision 1): the iPod's clock, which keeps the
   local time without its zone, and "#TZ/UNKNOWN" says so; the uploader puts
   them in UTC by the computer's zone */
#define SCROBBLER_TZ        "UNKNOWN"
static unsigned long scrobbler_time(void)
{
    return mktime(get_time());
}

/* Lines that wait for the disk: about 110 to 160 plays, some hours of
   music. Past 3/4 of it they are written at the disk's next idle even while
   the music plays: then the disk spins for the music already, and the
   storage thread writes, not the UI's */
#define SCROBBLER_BUF_SIZE  (16 * 1024)
#define SCROBBLER_FLUSH_AT  (SCROBBLER_BUF_SIZE * 3 / 4)
/* bytes of a name kept, cut before the UTF-8 character that would pass it */
#define SCROBBLER_FIELD_MAX 255
/* The longest line: four fields of names and id, and the numbers */
#define SCROBBLER_LINE_MAX  1280
/* After the lines, the line of the play a stop left waiting; then the room
   for one name as name_of() copies it: a byte longer than a field, for
   put_field() to see where a longer name's character that would pass it
   starts */
#define SCROBBLER_FIELD_ROOM (SCROBBLER_FIELD_MAX + 2)
#define SCROBBLER_ALLOC     (SCROBBLER_BUF_SIZE + SCROBBLER_LINE_MAX \
                             + SCROBBLER_FIELD_ROOM)
#define SUSPENDED_LINE(buf) ((buf) + SCROBBLER_BUF_SIZE)
#define FIELD(buf)          ((buf) + SCROBBLER_BUF_SIZE + SCROBBLER_LINE_MAX)

/* Last.fm's rules: a track longer than 30 s, heard for half its length or
   4 minutes, whichever comes first */
#define SCROBBLER_MIN_LENGTH 30000
#define SCROBBLER_MAX_NEED   240000
/* a track heard less than this isn't in the log at all, as by Rockbox's
   scrobbler plugin */
#define SCROBBLER_SKIP_MIN   500
/* A seek's jump is no listening: the position a seek lands at begins the
   listening again (scrobbler_seek()). A step longer than this is taken for
   a jump too, as of a seek not told of or the chunks of the music from
   before it: playing, the position moves by the frame a codec decodes,
   some 20 to 100 ms, and a second for TTA's */
#define SCROBBLER_STEP_MAX   5000
/* The place a waiting play goes on from: Rewind on Resume (up to 60 s)
   back from where it stopped, or a little past it, and not 0:00, which is
   the track chosen again */
#define RESUME_BEFORE_MS     65000
#define RESUME_AFTER_MS      5000
/* A waiting play older than this, by its start, ends at the next track
   change, and a resume is a play of its own: one line would bear a start
   Last.fm rejects past about 14 days */
#define RESUME_MAX_AGE_S     (7 * 24 * 60 * 60)
/* the battery's charge, in %, at and below which the lines go at each idle
   of the disk, while it is still safe for it */
#define SCROBBLER_LOW_BATTERY 15

/* The lines' memory, core_alloc()ed with Last.fm on, 0 without it. It may
   move as the allocator compacts: its address is taken anew, pinned for as
   long as it is used. Its handle is set under log_mtx, and given back under
   flush_mtx too, which every write to the file holds: nothing frees it under
   a write. */
static int log_handle;
static size_t log_used;             /* bytes of whole lines, from its start */
static struct mutex log_mtx;        /* the memory, the lines, the waiting play */
static struct mutex flush_mtx;      /* one write to the files at a time */
static bool mutexes_ready;
/* none was there as Last.fm was switched on: not asked for again until it
   is switched again, as each try with music would start the track again */
static bool log_refused;
static bool suspend_read;           /* SUSPEND_FILE read, once a boot */

/* The play under way. What of it sounded comes from the PCM interrupt;
   counted only past play_floor, the place a waiting play stopped at, which
   Rewind on Resume plays again. play_started is the clock as it began, 0 for
   none to log; play_logged, its L already in the lines. */
#define POS_NONE (~0ul)
static volatile unsigned long play_heard;
static volatile unsigned long play_pos = POS_NONE;
static volatile unsigned long play_floor;
static unsigned long play_started;
static bool play_logged;
static bool stop_under_way;         /* the finish to come is a stop's */

/* The play a stop left waiting, under log_mtx: its file by hash (0 for none),
   where it stopped, what of it sounded, when it began, whether its L is in
   the lines already; else its S line, if any, in SUSPENDED_LINE */
static struct {
    uint64_t hash;
    unsigned long place, heard, started;
    bool logged;
    size_t len;
    bool dirty;                     /* changed since SUSPEND_FILE was */
    bool on_disk;                   /* SUSPEND_FILE holds one */
} susp;

static void begin_play(void)
{
    pcm_play_lock();
    play_heard = 0;
    play_pos = POS_NONE;
    play_floor = 0;
    pcm_play_unlock();
    /* by the setting alone: the memory, switched on with music, comes as
       the track starts again for it, and is looked for at the play's end */
    play_started = global_settings.scrobbler ? scrobbler_time() : 0;
    play_logged = false;
}

/* Only steps that the music goes on by count: a seek's jump, either way, and
   the track's start, its resume place, are no listening. Also called on the
   codec's and the audio thread without the PCM lock (pcmbuf.c's and
   playback.c's audio_pcmbuf_sync_position()): an interrupt between the two
   stores can lose or count twice one step, at most SCROBBLER_STEP_MAX and
   mostly under 100 ms. */
void scrobbler_position(unsigned long elapsed)
{
    const unsigned long last = play_pos;
    if (last != POS_NONE && elapsed > last
        && elapsed - last <= SCROBBLER_STEP_MAX)
    {
        const unsigned long from = MAX(last, play_floor);
        if (elapsed > from)
            play_heard += elapsed - from;
    }
    play_pos = elapsed;
}

void scrobbler_seek(void)
{
    pcm_play_lock();
    play_pos = POS_NONE;
    pcm_play_unlock();
}

/* podcasts and audiobooks aren't scrobbled */
static bool is_speech(const struct mp3entry *id3)
{
    const int kind = tagcache_id3_kind(id3);
    if (kind == 1 || kind == 2)
        return true;
    return (itunesdb_track_flags(id3->path)
            & (ITUNESDB_PODCAST | ITUNESDB_AUDIOBOOK)) != 0;
}

struct line {
    char *buf;
    size_t len, room;
    bool full;
};

static void put_char(struct line *l, char c)
{
    if (l->len < l->room)
        l->buf[l->len++] = c;
    else
        l->full = true;
}

/* tabs and line ends become spaces, as they separate the fields and lines */
static void put_field(struct line *l, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n > SCROBBLER_FIELD_MAX)
    {
        n = SCROBBLER_FIELD_MAX;
        while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80)
            n--;
    }
    for (size_t i = 0; i < n; i++)
        put_char(l, s[i] == '\t' || s[i] == '\n' || s[i] == '\r' ? ' ' : s[i]);
}

static void put_num(struct line *l, unsigned long n)
{
    char num[12];
    snprintf(num, sizeof(num), "%lu", n);
    put_field(l, num);
}

/* A name of the track as the menus show it, the database's (iTunesDB's for
   a song of Finder's), or its tags' without one. In field, until the next. */
static const char *name_of(const struct mp3entry *id3, int tag,
                           const char *tagged, char *field)
{
    if (id3->tagcache_idx > 0
        && tagcache_get_string(id3->tagcache_idx - 1, tag, field,
                               SCROBBLER_FIELD_ROOM)
        && field[0])
        return field;
    return tagged && tagged[0] ? tagged : NULL;
}

/* The play's line into dst: its length, 0 without an artist or a title,
   which Last.fm takes no play without, or without room. With the memory
   pinned, under log_mtx; nothing yields. */
static size_t format_line(char *dst, size_t room, char *field,
                          const struct mp3entry *id3, unsigned long length,
                          char rating, unsigned long started)
{
    struct line l = { dst, 0, room, false };
    const char *artist = name_of(id3, tag_artist, id3->artist, field);
    if (!artist)
        artist = name_of(id3, tag_albumartist, id3->albumartist, field);
    bool named = artist != NULL;
    put_field(&l, artist);
    put_char(&l, '\t');
    put_field(&l, name_of(id3, tag_album, id3->album, field));
    put_char(&l, '\t');
    const char *title = name_of(id3, tag_title, id3->title, field);
    named = named && title;
    put_field(&l, title);
    put_char(&l, '\t');
    long track = id3->tracknum;
    if (id3->tagcache_idx > 0)
    {
        const long n = tagcache_get_numeric_ram(id3->tagcache_idx - 1,
                                                tag_tracknumber);
        if (n > 0)
            track = n;
    }
    if (track > 0)
        put_num(&l, track);
    put_char(&l, '\t');
    put_num(&l, (length + 500) / 1000);
    put_char(&l, '\t');
    put_char(&l, rating);
    put_char(&l, '\t');
    put_num(&l, started);
    put_char(&l, '\t');
    put_field(&l, id3->mb_track_id);
    put_char(&l, '\n');
    return named && !l.full ? l.len : 0;
}

static void scrobbler_idle(void);

/* the battery low, while a write is still safe for the disk */
static bool battery_low(void)
{
    const int level = battery_level();
    return level >= 0 && level <= SCROBBLER_LOW_BATTERY
           && battery_level_safe();
}

/* a write at the disk's next idle, for what the log or SUSPEND_FILE lacks */
static void write_at_idle(void)
{
    if (log_used || susp.dirty)
        register_storage_idle_func(scrobbler_idle);
}

/* The waiting play's S line to the lines: it is played no further. Under
   log_mtx with the memory pinned. */
static void suspended_end(char *buf)
{
    if (susp.len && SCROBBLER_BUF_SIZE - log_used >= susp.len)
    {
        memcpy(buf + log_used, SUSPENDED_LINE(buf), susp.len);
        log_used += susp.len;
    }
    DEBUGF("scrobbler: the waiting play ends: %d bytes of line\n",
           (int)susp.len);
    susp.hash = 0;
    susp.len = 0;
    susp.dirty = true;
}

/* The play that ends is noted: its line to the lines, or kept as the play
   waiting as a stop that keeps its place leaves it, its L to the lines at
   once (it counts as it is) and its S in SUSPENDED_LINE (more of it may yet
   make it an L). */
static bool note_play(const struct mp3entry *id3, unsigned long length,
                      char rating, unsigned long started, unsigned long heard,
                      bool line, bool suspend, bool logged)
{
    if (!mutexes_ready)
        return false;
    mutex_lock(&log_mtx);
    if (log_handle <= 0)
    {
        mutex_unlock(&log_mtx);
        return false;
    }
    char *const buf = core_get_data_pinned(log_handle);
    /* one waits at a time: another's comes only after a track change, which
       has ended or taken the one before */
    if (suspend && susp.hash)
        suspended_end(buf);

    size_t len = 0;
    const bool to_lines = line && !logged && (!suspend || rating == 'L');
    if (to_lines)
    {
        len = format_line(buf + log_used, SCROBBLER_BUF_SIZE - log_used,
                          FIELD(buf), id3, length, rating, started);
        log_used += len;
    }
    if (suspend)
    {
        susp.hash = itunesdb_path_hash(id3->path);
        susp.place = id3->elapsed;
        susp.heard = heard;
        susp.started = started;
        susp.logged = logged || len;
        susp.len = line && !susp.logged
                   ? format_line(SUSPENDED_LINE(buf), SCROBBLER_LINE_MAX,
                                 FIELD(buf), id3, length, rating, started)
                   : 0;
        susp.dirty = true;
    }
    const bool write_soon = log_used >= SCROBBLER_FLUSH_AT || battery_low();
    core_put_data_pinned(buf);
    mutex_unlock(&log_mtx);

    if (write_soon)
        register_storage_idle_func(scrobbler_idle);
    return len > 0 || suspend;
}

/* A track begins: a play that waited goes on if it is of this file and
   from about where it stopped, as after a power-off, USB or the audio
   buffer starting the track again, and began at most RESUME_MAX_AGE_S ago
   by the clock; else, another track, or a clock set back or not set since,
   ends it. */
void scrobbler_track_change(const struct mp3entry *id3)
{
    begin_play();
    if (!mutexes_ready || !susp.hash)
        return;
    bool registered = false;
    mutex_lock(&log_mtx);
    if (susp.hash && log_handle > 0)
    {
        const unsigned long at = id3 ? id3->elapsed : 0;
        /* play_started is now, by begin_play() */
        if (play_started && at > 0
            && susp.started <= play_started
            && play_started - susp.started <= RESUME_MAX_AGE_S
            && at + RESUME_BEFORE_MS >= susp.place
            && at <= susp.place + RESUME_AFTER_MS
            && itunesdb_path_hash(id3->path) == susp.hash)
        {
            pcm_play_lock();
            play_heard = susp.heard;
            play_floor = susp.place;
            pcm_play_unlock();
            play_started = susp.started;
            play_logged = susp.logged;
            DEBUGF("scrobbler: the waiting play goes on from %lu ms, %lu "
                   "heard\n", at, susp.heard);
            susp.hash = 0;
            susp.len = 0;
            susp.dirty = true;
        }
        else
        {
            char *const buf = core_get_data_pinned(log_handle);
            suspended_end(buf);
            core_put_data_pinned(buf);
            registered = log_used >= SCROBBLER_FLUSH_AT || battery_low();
        }
    }
    mutex_unlock(&log_mtx);
    if (registered)
        register_storage_idle_func(scrobbler_idle);
}

void scrobbler_track_finish(const struct mp3entry *id3, unsigned int flags)
{
    pcm_play_lock();
    const unsigned long heard = play_heard;
    pcm_play_unlock();
    const unsigned long started = play_started;
    const bool logged = play_logged;
    play_started = 0;
    /* A stop keeps the place of a track it cuts short, which a resume plays
       on from; not at its end (TEF_AUTO_SKIP), nor a seek to 0:00 */
    const bool suspend = stop_under_way
                         && !(flags & (TEF_AUTO_SKIP | TEF_REWIND));
    stop_under_way = false;

    /* A seek to 0:00 ends the play, and the track begins again, as a play of
       its own: the finish event comes before the seek is taken */
    if (flags & TEF_REWIND)
        begin_play();

    if (!started || !global_settings.scrobbler || !id3)
        return;
    if (is_speech(id3))
    {
        DEBUGF("scrobbler: a podcast or a book, not in the log\n");
        return;
    }

    unsigned long length = id3->length;
    struct tagcache_numbers n;
    if (!length && id3->tagcache_idx > 0
        && tagcache_get_numbers(id3->tagcache_idx - 1, &n))
        length = n.length;
    if (length <= SCROBBLER_MIN_LENGTH)
    {
        DEBUGF("scrobbler: %lu ms long, not in the log\n", length);
        return;
    }
    const char rating = heard >= MIN(length / 2, SCROBBLER_MAX_NEED) ? 'L' : 'S';
    const bool line = !(rating == 'S' && heard < SCROBBLER_SKIP_MIN);
    const bool noted = note_play(id3, length, rating, started, heard, line,
                                 suspend, logged);
    DEBUGF("scrobbler: %c, %lu ms heard of %lu%s%s: %s, %d bytes wait\n",
           rating, heard, length, suspend ? ", stopped: waits" : "",
           logged ? ", its L in already" : "", noted ? "noted" : "nothing",
           (int)log_used);
    (void)noted;
}

void scrobbler_stopping(void)
{
    stop_under_way = true;
}

/* A play opened paused, as Now Playing resumes one, begins as it first
   sounds: its time is taken again then, also for one that goes on from a
   stop with nothing of it heard yet. Paused, what waits goes at the disk's
   idle, as no music would stutter. */
void scrobbler_pause(bool pause)
{
    if (pause)
    {
        write_at_idle();
        return;
    }
    if (play_started && play_heard == 0)
    {
        const unsigned long now = scrobbler_time();
        DEBUGF("scrobbler: begins as it sounds: %lu, not %lu\n", now,
               play_started);
        play_started = now;
    }
}

/* A new or empty log has the header first, and a line cut short, by a full
   disk or a reset, ends before the new ones. What fails is taken back, for
   the lines to stay in memory and come again. */
static bool write_log(const char *buf, size_t size)
{
    const int fd = open(SCROBBLER_FILE, O_RDWR|O_CREAT, 0666);
    if (fd < 0)
        return false;
    const off_t was = lseek(fd, 0, SEEK_END);
    bool ok = was >= 0;
    if (ok && was == 0)
    {
        char head[96];
        const int len = snprintf(head, sizeof(head),
                                 "#AUDIOSCROBBLER/1.1\n#TZ/" SCROBBLER_TZ
                                 "\n#CLIENT/RockWheel %s\n", rbversion);
        ok = len > 0 && len < (int)sizeof(head)
             && write(fd, head, len) == len;
    }
    else if (ok)
    {
        char last = '\n';
        ok = lseek(fd, was - 1, SEEK_SET) == was - 1
             && read(fd, &last, 1) == 1
             && lseek(fd, 0, SEEK_END) == was;
        if (ok && last != '\n')
            ok = write(fd, "\n", 1) == 1;
    }
    ok = ok && write(fd, buf, size) == (ssize_t)size;
    if (!ok && was >= 0)
        ftruncate(fd, was);
    return close(fd) == 0 && ok;
}

/* SUSPEND_FILE's record, the S line after it */
struct suspend_record {
    uint32_t magic;
    uint32_t hash_lo, hash_hi;
    uint32_t place, heard, started;
    uint32_t logged;
    uint32_t len;
};

/* The waiting play to SUSPEND_FILE, or the file away without one. Under
   both mutexes with the memory pinned: small, and a stop meanwhile would
   change what is written. */
static void write_suspended(const char *buf)
{
    if (!susp.dirty)
        return;
    bool ok;
    if (susp.hash)
    {
        const struct suspend_record r = {
            SUSPEND_MAGIC, (uint32_t)susp.hash, (uint32_t)(susp.hash >> 32),
            susp.place, susp.heard, susp.started, susp.logged, susp.len
        };
        const int fd = open(SUSPEND_FILE, O_WRONLY|O_CREAT|O_TRUNC, 0666);
        ok = fd >= 0 && write(fd, &r, sizeof(r)) == (ssize_t)sizeof(r)
             && write(fd, SUSPENDED_LINE(buf), susp.len) == (ssize_t)susp.len;
        if (fd >= 0)
            ok = close(fd) == 0 && ok;
        if (!ok)
            remove(SUSPEND_FILE);
        susp.on_disk = ok;
    }
    else
    {
        ok = !susp.on_disk || remove(SUSPEND_FILE) == 0 || !file_exists(SUSPEND_FILE);
        if (ok)
            susp.on_disk = false;
    }
    if (ok)
        susp.dirty = false;
    DEBUGF("scrobbler: the waiting play to %s: %s\n", SUSPEND_FILE,
           susp.hash ? (ok ? "written" : "failed")
                     : (ok ? "none" : "not removed"));
}

/* with Last.fm off, its memory goes once no line waits in it. Under both
   mutexes; nothing here yields. */
static void free_if_off(void)
{
    if (!global_settings.scrobbler && log_handle > 0 && !log_used
        && !susp.hash && !susp.dirty)
    {
        log_handle = core_free(log_handle);
        DEBUGF("scrobbler: memory given back\n");
    }
}

void scrobbler_flush(void)
{
    if (!mutexes_ready)
        return;
    mutex_lock(&flush_mtx);
    mutex_lock(&log_mtx);
    const size_t size = log_used;
    /* pinned through the write, which yields: the lines added meanwhile come
       after these, which stay where they are, as no other writes the file */
    char *const buf = log_handle > 0 ? core_get_data_pinned(log_handle) : NULL;
    mutex_unlock(&log_mtx);
    const bool written = buf && size && write_log(buf, size);
    DEBUGF("scrobbler: %d bytes to the log: %s\n", (int)size,
           written ? "written" : "not written");
    mutex_lock(&log_mtx);
    if (buf)
    {
        if (written)
        {
            memmove(buf, buf + size, log_used - size);
            log_used -= size;
        }
        write_suspended(buf);
        core_put_data_pinned(buf);
    }
    free_if_off();
    mutex_unlock(&log_mtx);
    mutex_unlock(&flush_mtx);
}

/* At the disk's idle, as it would sleep: with the music stopped or paused;
   while it plays if the log is 3/4 full, the battery low, or Last.fm
   switched off, for its memory to go */
static void scrobbler_idle(void)
{
    const int status = audio_status();
    DEBUGF("scrobbler: the disk's idle: status %d, %d bytes wait, "
           "battery %d%%\n", status, (int)log_used, battery_level());
    if (!(status & AUDIO_STATUS_PLAY) || (status & AUDIO_STATUS_PAUSE)
        || log_used >= SCROBBLER_FLUSH_AT || !global_settings.scrobbler
        || battery_low())
        scrobbler_flush();
}

void scrobbler_stopped(void)
{
    stop_under_way = false;
    write_at_idle();
}

/* The play a stop left waiting before the last power-off, read into memory
   with the music stopped, once a boot: at boot, before music, with Last.fm
   on then, or else at the first settings apply with the music stopped after
   it is switched on (the switch itself, USB, a theme, night mode, a .cfg). The
   file goes, to be written again at the next write if the play still waits
   then. With a play waiting in memory already, the file is that one's, or
   older, and is left to the next write, which makes it that one's. Under
   log_mtx, the memory there. */
static void read_suspended(void)
{
    if (suspend_read || log_handle <= 0 || audio_status())
        return;
    suspend_read = true;
    if (susp.hash)
    {
        DEBUGF("scrobbler: a play waits in memory: %s left as it is\n",
               SUSPEND_FILE);
        return;
    }
    const int fd = open(SUSPEND_FILE, O_RDONLY);
    if (fd < 0)
        return;
    char *const buf = core_get_data_pinned(log_handle);
    struct suspend_record r;
    const bool ok = read(fd, &r, sizeof(r)) == (ssize_t)sizeof(r)
                    && r.magic == SUSPEND_MAGIC && r.len <= SCROBBLER_LINE_MAX
                    && read(fd, SUSPENDED_LINE(buf), r.len) == (ssize_t)r.len;
    close(fd);
    if (ok)
    {
        susp.hash = ((uint64_t)r.hash_hi << 32) | r.hash_lo;
        susp.place = r.place;
        susp.heard = r.heard;
        susp.started = r.started;
        susp.logged = r.logged;
        susp.len = r.len;
        /* in memory now: the file is written again from it */
        susp.dirty = true;
    }
    core_put_data_pinned(buf);
    susp.on_disk = remove(SUSPEND_FILE) != 0;
    DEBUGF("scrobbler: a waiting play read from %s: %s\n", SUSPEND_FILE,
           ok ? "yes" : "no");
}

/* The audio buffer gives way to the memory, which starts a playing track
   again where it was but leaves a paused one stopped: that one is put back,
   paused. No mutex is held over the allocation: the stop it may cause notes
   the play, which takes log_mtx. */
static void take_memory(void)
{
    if (log_handle > 0 || log_refused)
        return;
    const bool paused = audio_status() & AUDIO_STATUS_PAUSE;
    unsigned long elapsed = 0, offset = 0;
    if (paused)
    {
        const struct mp3entry *id3 = audio_current_track();
        elapsed = id3->elapsed;
        offset = id3->offset;
    }
    const int handle = core_alloc(SCROBBLER_ALLOC);
    if (handle > 0)
    {
        mutex_lock(&log_mtx);
        log_handle = handle;
        mutex_unlock(&log_mtx);
    }
    else
        log_refused = true;
    DEBUGF("scrobbler: %d bytes of memory %s\n", SCROBBLER_ALLOC,
           handle > 0 ? "taken" : "refused");
    if (paused && !(audio_status() & AUDIO_STATUS_PLAY))
    {
        audio_play(elapsed, offset);
        audio_pause();
    }
}

void scrobbler_apply(bool on)
{
    if (!mutexes_ready)
    {
        mutex_init(&log_mtx);
        mutex_init(&flush_mtx);
        mutexes_ready = true;
    }
    if (on)
    {
        take_memory();
        mutex_lock(&log_mtx);
        read_suspended();
        mutex_unlock(&log_mtx);
        return;
    }
    /* Off: the lines that wait stay in it, as they were played with it on,
       the waiting play's S with them, and go to the log at the next write,
       USB, shutdown or the disk's idle, which then gives the memory back; at
       once without them */
    mutex_lock(&flush_mtx);
    mutex_lock(&log_mtx);
    if (susp.hash && log_handle > 0)
    {
        char *const buf = core_get_data_pinned(log_handle);
        suspended_end(buf);
        core_put_data_pinned(buf);
    }
    free_if_off();
    mutex_unlock(&log_mtx);
    mutex_unlock(&flush_mtx);
    write_at_idle();
}

void scrobbler_setting(bool on)
{
    log_refused = false;
    scrobbler_apply(on);
}
