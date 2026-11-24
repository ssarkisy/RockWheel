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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "pathfuncs.h"
#include "action.h"
#include "audio.h"
#include "lang.h"
#include "list.h"
#include "menu.h"
#include "misc.h"
#include "playlist.h"
#include "playlist_viewer.h"
#include "tagtree.h"
#include "playlist_catalog.h"
#include "plugin.h"
#include "settings.h"
#include "splash.h"
#include "tagcache.h"
#include "transition.h"
#include "onplay.h"
#include "filetypes.h"
#include "rbpaths.h"
#include "itunesdb.h"
#include "smart_playlists.h"
#include "rbunicode.h"
#include "on_the_go.h"
#include "library_playlists.h"
#include "root_menu.h"
#include "list_covers.h"
#include "line_reader.h"
#include "file_write.h"
#include "album_header.h"
#include "album_years.h"
#include "core_alloc.h"
#include "ipod_playlists.h"
#include "storage.h"
#include "ata_idle_notify.h"
#include "health_log.h"

/* Rockbox's viewer read them a window of 200 lines at a time from the file,
   again as it scrolled past one, and looked up each title as it showed,
   yielding to the music each time: they scrolled by fits and starts. */

#define LP_DIR     ROCKBOX_DIR "/playlist_index"
/* A playlist made on the iPod plays from a copy of it, as Rockbox keeps a
   playing playlist by the places of its lines in its file: one changed as it
   played would have lost its place, at the next boot too. Two, taken in
   turn, the one playing left until the other is started; the file the copy
   is of, to find it by. */
#define LP_PLAYING      LP_DIR "/playing.m3u8"
#define LP_PLAYING2     LP_DIR "/playing2.m3u8"
#define LP_PLAYING_FROM LP_DIR "/playing.txt"
#define LP_MAGIC   0x504c4958   /* "PLIX" */
/* 4: with what its header shows; 5: a compilation one album in its collage,
   and its header not made marked so. An older one is made again */
#define LP_VERSION 5

struct lp_head {
    uint32_t magic, version;
    /* of the database its entries are of: a new one counts commits from 1
       again, its count of entries tells it apart */
    uint32_t commitid, entries;
    uint32_t size;      /* of the playlist file it was made from or for */
    uint32_t count;
    uint32_t lines;     /* of songs in that file */
    uint32_t unmatched; /* of those, not in the database */
    uint32_t pending;   /* the file is still to be written from it */
    uint32_t sort;
    /* its header's, as struct lp_state has them */
    uint32_t seconds;
    uint32_t collage_seen;
    int32_t collage[LP_COLLAGE];
};

/* the collage the header is given is as long as the index's */
#if LP_COLLAGE != ALBUM_HEADER_COLLAGE
#error "LP_COLLAGE and ALBUM_HEADER_COLLAGE differ"
#endif

/* The covers changed since the playlists' collages were chosen, or a header
   couldn't be made: they're made again at the next Updating Library */
#define LP_COVERS_TODO ROCKBOX_DIR "/playlist_covers.todo"
/* headers left not made, as lookups couldn't tell: a pass that made one so
   leaves LP_COVERS_TODO for the next */
static unsigned headers_unknown;

/* Playlists' rows with their covers (Playlist Covers), kept as an index's
   head is written (below, "Playlists' rows") */
static void rows_put(const char *file, int count, const struct lp_state *state);
static void rows_forget(const char *file);
static void rows_unknown(const char *file);
static void rows_moved(const char *from, const char *to);

static void index_path(uint64_t hash, char *buf, size_t size)
{
    snprintf(buf, size, LP_DIR "/%08lx%08lx.idx", (unsigned long)(hash >> 32),
             (unsigned long)(uint32_t)hash);
}

static long file_size(const char *file)
{
    const int fd = open(file, O_RDONLY);
    if (fd < 0)
        return -1;
    const long size = ffilesize(fd);
    close(fd);
    return size;
}

/* a file's time, as its folder has it, 0 if it isn't found there */
static time_t file_time(const char *file)
{
    char dir[MAX_PATH];
    strmemccpy(dir, file, sizeof(dir));
    char *name = strrchr(dir, '/');
    if (!name)
        return 0;
    *name++ = '\0';
    DIR *d = opendir(dir[0] ? dir : "/");
    if (!d)
        return 0;
    time_t time = 0;
    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (!strcasecmp(e->d_name, name))
        {
            time = dir_get_info(d, e).mtime;
            break;
        }
    }
    closedir(d);
    return time;
}

/* a path without the drive the browser names first on an iPod */
static const char *no_volume(const char *path)
{
#ifdef HAVE_MULTIVOLUME
    path_strip_volume(path, &path, false);
#endif
    return path;
}

/* nor a separator doubled, as the browser gives one back in a folder */
static void tidy_copy(char *buf, size_t size, const char *path)
{
    path = no_volume(path);
    size_t n = 0;
    for (; *path && n + 1 < size; path++)
    {
        if (*path == '/' && n > 0 && buf[n - 1] == '/')
            continue;
        buf[n++] = *path;
    }
    buf[n] = '\0';
}

bool lp_same_file(const char *a, const char *b)
{
    return itunesdb_path_hash(a) == itunesdb_path_hash(b);
}

/* A line of a playlist as its song's path. The import's are whole already;
   one put in Playlists by hand may have them relative to its folder, as
   Windows writes them, or as a Mac has the iPod mounted, under
   /Volumes/<its name>; an .m3u's are in the codepage of the settings. NULL if
   it can't be one. */
static const char *song_path(const char *line, const char *dir, bool m3u,
                             char *buf, size_t size)
{
    /* into buf, which resolving takes a copy of first */
    if (m3u)
    {
        *iso_decode_ex((const unsigned char *)line, (unsigned char *)buf,
                       -1, strlen(line), size - 1) = '\0';
        line = buf;
    }
    if (line[0] == '/' && strncasecmp(line, "/Volumes/", 9)
        && !strchr(line, '\\') && !strstr(line, "/."))
        return line;
    if (playlist_resolve_path(buf, size, line, dir) < 0)
        return NULL;
    if (!strncasecmp(buf, "/Volumes/", 9))
    {
        const char *rest = strchr(buf + 9, '/');
        if (!rest)
            return NULL;
        memmove(buf, rest, strlen(rest) + 1);
    }
    return buf;
}

/* a playlist's folder, which its relative lines are of, and whether it's an
   .m3u, in the codepage */
static bool playlist_dir(const char *file, char *dir, size_t size)
{
    strmemccpy(dir, file, size);
    char *name = strrchr(dir, '/');
    if (name)
        *(name == dir ? name + 1 : name) = '\0';
    const char *ext = strrchr(file, '.');
    return ext && !strcasecmp(ext, ".m3u");
}

static bool is_copy(const char *name)
{
    return lp_same_file(name, LP_PLAYING) || lp_same_file(name, LP_PLAYING2);
}

/* the file the copy playing is of, as LP_PLAYING_FROM has it */
static char copy_source[MAX_PATH];
static bool copy_source_read;

static const char *playing_copy_source(void)
{
    if (!copy_source_read)
    {
        copy_source_read = true;
        copy_source[0] = '\0';
        const int fd = open(LP_PLAYING_FROM, O_RDONLY);
        if (fd >= 0)
        {
            read_line(fd, copy_source, sizeof(copy_source));
            close(fd);
        }
    }
    return copy_source;
}

/* whether the music is from this file, playing or paused: from it, or from
   the copy of it a playlist made on the iPod plays from */
static bool playing_from(const char *file)
{
    char name[MAX_PATH];
    if (!(audio_status() & AUDIO_STATUS_PLAY)
        || !playlist_get_name(NULL, name, sizeof(name)))
        return false;
    if (is_copy(name))
        return lp_same_file(playing_copy_source(), file);
    return lp_same_file(name, file);
}

/* the database in memory, which their songs are found in by their paths */
static bool database_in_ram(void)
{
    return tagcache_finds_entries();
}

/* A song's length to the second, as a playlist's sum adds it: a song added
   or taken out changes it by its own */
static uint32_t song_seconds(uint32_t idx)
{
    const long ms = tagcache_sort_key(idx, tag_length);
    return ms > 0 ? (uint32_t)((ms + 500) / 1000) : 0;
}

/* The albums of a playlist's collage, chosen from the database in memory: in
   its songs' order, each album (song_album()) as the song of it that comes
   first, if the header would show a cover for it. Unknown Album left out.
   As far as LP_COLLAGE_SONGS songs and as many as LP_COLLAGE_LOOKS covers
   looked up, an album found without one not looked up again for a while:
   a big one's later albums aren't looked for. Each song's tags are read
   from memory and each look is in the covers' indexes in memory; only its
   path may be read from the disk, where it's in neither the database's
   memory nor the dircache. */
#define LP_COLLAGE_SONGS  512
#define LP_COLLAGE_LOOKS  32
#define LP_COLLAGE_PASSED 8
struct collage_walk {
    int32_t *collage;   /* its albums' songs, -1 past them */
    uint32_t *seen;     /* songs walked */
    bool on;
    int looks;
    long passed[LP_COLLAGE_PASSED][2]; /* without a cover */
    int passed_count, passed_at;
};

/* A song's album as a collage tells them apart: its name's seek and its
   album artist's; but a compilation's songs, whatever their album artists,
   one album of its name, as Albums' row of a name's compilations has them.
   The database has a song's artist for an album artist it lacks, so each
   artist's songs of a compilation without the tag would be another album,
   its cover in several cells */
#define COMPILATION_ARTIST (-2)
static void song_album(uint32_t idx, long *album, long *artist)
{
    *album = tagcache_sort_key(idx, tag_album);
    *artist = tagcache_track_compilation(idx) ? COMPILATION_ARTIST
                                              : tagcache_sort_key(idx, tag_albumartist);
}

/* the collage not made, and made at the next Updating Library: the note
   shows meanwhile, with the length if that was made (not when the index was
   written without the database in memory) */
static void collage_unknown(int32_t *collage, uint32_t *seen)
{
    *seen = LP_HEADER_UNKNOWN;
    for (int k = 0; k < LP_COLLAGE; k++)
        collage[k] = -1;
    headers_unknown++;
    lp_covers_changed();
}

static void collage_walk_begin(struct collage_walk *w, int32_t *collage,
                               uint32_t *seen, bool fresh)
{
    w->collage = collage;
    w->seen = seen;
    w->on = true;
    w->looks = w->passed_count = w->passed_at = 0;
    if (!fresh)
        return;
    *seen = 0;
    for (int k = 0; k < LP_COLLAGE; k++)
        collage[k] = -1;
}

static int collage_taken(const struct collage_walk *w)
{
    int n = 0;
    while (n < LP_COLLAGE && w->collage[n] >= 0)
        n++;
    return n;
}

/* the walk on by a song: false once it's over */
static bool collage_step(struct collage_walk *w, uint32_t idx)
{
    const int taken = collage_taken(w);
    if (!w->on || taken >= LP_COLLAGE || *w->seen >= LP_COLLAGE_SONGS
        || w->looks >= LP_COLLAGE_LOOKS)
    {
        w->on = false;
        return false;
    }
    (*w->seen)++;
    long album, artist;
    song_album(idx, &album, &artist);
    if (album < 0)
        return true;
    for (int k = 0; k < taken; k++)
    {
        long a, b;
        song_album(w->collage[k], &a, &b);
        if (a == album && b == artist)
            return true;
    }
    for (int k = 0; k < w->passed_count; k++)
    {
        if (w->passed[k][0] == album && w->passed[k][1] == artist)
            return true;
    }
#ifdef HAVE_TC_RAMCACHE
    /* Unknown Album, which has no name */
    char probe[2];
    if (!tagcache_tag_string(tag_album, album, probe, sizeof(probe)))
        return true;
#endif
    w->looks++;
    const int cover = album_header_song_cover(idx);
    /* not known, as with the database busy: no choice kept */
    if (cover < 0)
    {
        collage_unknown(w->collage, w->seen);
        w->on = false;
        return false;
    }
    if (cover > 0)
        w->collage[taken] = idx;
    else
    {
        w->passed[w->passed_at][0] = album;
        w->passed[w->passed_at][1] = artist;
        w->passed_at = (w->passed_at + 1) % LP_COLLAGE_PASSED;
        if (w->passed_count < LP_COLLAGE_PASSED)
            w->passed_count++;
    }
    return true;
}

/* the collage of these songs, the first ones of a playlist */
static void collage_choose(const struct lp_song *songs, int count,
                           struct lp_state *state)
{
    struct collage_walk w;
    collage_walk_begin(&w, state->collage, &state->collage_seen, true);
    for (int i = 0; i < count && collage_step(&w, songs[i].idx); i++)
        ;
}

/* an index's head of the database as it is and of the file as it is */
static bool head_current(const struct lp_head *head, const char *file)
{
    return head->magic == LP_MAGIC && head->version == LP_VERSION
           && head->commitid == (uint32_t)tagcache_get_commitid()
           && head->entries == (uint32_t)tagcache_entry_count()
           && (long)head->size == file_size(file) && head->count <= LP_MAX;
}

int lp_index_read(const char *file, struct lp_song *songs, int max,
                  struct lp_state *state)
{
    char path[MAX_PATH];
    index_path(itunesdb_path_hash(file), path, sizeof(path));
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    struct lp_head head;
    int count = -1;
    if (read(fd, &head, sizeof(head)) == sizeof(head)
        && head_current(&head, file))
    {
        count = head.count;
        if (songs)
        {
            count = MIN(count, max);
            const ssize_t bytes = count * sizeof(*songs);
            if (read(fd, songs, bytes) != bytes)
                count = -1;
        }
        if (state)
        {
            state->lines = head.lines;
            state->unmatched = head.unmatched;
            state->sort = head.sort;
            state->pending = head.pending;
            state->seconds = head.seconds;
            state->collage_seen = head.collage_seen;
            memcpy(state->collage, head.collage, sizeof(state->collage));
        }
    }
    close(fd);
    return count;
}

bool lp_in_file_order(const struct lp_song *songs, int count,
                      const struct lp_state *state)
{
    if ((uint32_t)count + state->unmatched != state->lines)
        return false;
    for (int i = 0; i < count; i++)
    {
        if (songs[i].line == LP_NO_LINE
            || (i > 0 && songs[i].line <= songs[i - 1].line))
            return false;
    }
    return true;
}

bool lp_index_write(const char *file, const struct lp_song *songs, int count,
                    struct lp_state *state)
{
    state->pending = !lp_in_file_order(songs, count, state);
    /* what its header shows, from memory: the songs' lengths added, a few
       covers looked up; not made without the database there, as during a
       commit, rather than none */
    if (tagcache_finds_entries())
    {
        state->seconds = 0;
        for (int i = 0; i < count; i++)
        {
            state->seconds += song_seconds(songs[i].idx);
            if ((i & 1023) == 1023)
                yield();
        }
        collage_choose(songs, count, state);
    }
    else
    {
        state->seconds = LP_HEADER_UNKNOWN;
        collage_unknown(state->collage, &state->collage_seen);
    }
    char path[MAX_PATH];
    index_path(itunesdb_path_hash(file), path, sizeof(path));
    const long size = file_size(file);
    if (size < 0)
        return false;
    mkdir(LP_DIR);
    /* made again from the playlist if lost (FW_CACHE) */
    struct fw w;
    if (!fw_open(&w, path, FW_CACHE, NULL, 0))
        return false;
    struct lp_head head = {
        .magic = LP_MAGIC, .version = LP_VERSION,
        .commitid = tagcache_get_commitid(), .entries = tagcache_entry_count(),
        .size = size, .count = count, .lines = state->lines,
        .unmatched = state->unmatched, .pending = state->pending,
        .sort = state->sort, .seconds = state->seconds,
        .collage_seen = state->collage_seen,
    };
    memcpy(head.collage, state->collage, sizeof(head.collage));
    fw_put(&w, &head, sizeof(head));
    fw_put(&w, songs, count * sizeof(*songs));
    if (!fw_close(&w, true))
        return false;
    rows_put(file, count, state);
    return true;
}

void lp_index_forget(const char *file)
{
    char path[MAX_PATH];
    index_path(itunesdb_path_hash(file), path, sizeof(path));
    remove(path);
    rows_forget(file);
}

/* Songs added at the end of a playlist made on the iPod, from old_size on:
   their entries after those of its index, which isn't made again, the cost
   the same for a playlist of ten songs or of thousands; and if it plays,
   after the queue too. One that can't be added to is made again as it
   opens. */
void lp_index_appended(const char *file, long old_size)
{
    char path[MAX_PATH], dir[MAX_PATH];
    index_path(itunesdb_path_hash(file), path, sizeof(path));
    const bool playing = playing_from(file);
    struct lp_head head;
    int ifd = open(path, O_RDWR);
    bool indexed = ifd >= 0
        && read(ifd, &head, sizeof(head)) == sizeof(head)
        && head.magic == LP_MAGIC && head.version == LP_VERSION
        && head.commitid == (uint32_t)tagcache_get_commitid()
        && head.entries == (uint32_t)tagcache_entry_count()
        && (long)head.size == old_size && !head.pending
        && head.sort == ITUNESDB_SORT_MANUAL
        && lseek(ifd, sizeof(head) + head.count * sizeof(struct lp_song),
                 SEEK_SET) >= 0;
    /* One that was empty or none, as On-The-Go cleared, without an index of
       it: an index begun, of no songs, which those added go on from, so that
       its head, and its row in Playlists, has them at once */
    if (!indexed && old_size == 0 && database_in_ram())
    {
        if (ifd >= 0)
            close(ifd);
        mkdir(LP_DIR);
        ifd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
        head = (struct lp_head){
            .magic = LP_MAGIC, .version = LP_VERSION,
            .commitid = tagcache_get_commitid(),
            .entries = tagcache_entry_count(), .sort = ITUNESDB_SORT_MANUAL,
        };
        for (int k = 0; k < LP_COLLAGE; k++)
            head.collage[k] = -1;
        indexed = ifd >= 0 && write(ifd, &head, sizeof(head)) == sizeof(head);
    }
    /* its collage, if it has fewer than four albums with a cover among all
       its songs, walked to their end: on with the songs added, which come
       after them */
    struct collage_walk walk = { .on = false };
    if (indexed)
    {
        collage_walk_begin(&walk, head.collage, &head.collage_seen, false);
        walk.on = head.collage_seen == head.count;
    }
    if (!indexed)
    {
        if (ifd >= 0)
            close(ifd);
        ifd = -1;
        remove(path);
        /* its songs not known until its index is made again */
        rows_unknown(file);
        if (!playing)
            return;
    }
    const bool m3u = playlist_dir(file, dir, sizeof(dir));
    int fd = open(file, O_RDONLY);
    if (fd < 0 || lseek(fd, old_size, SEEK_SET) != old_size)
    {
        if (fd >= 0)
            close(fd);
        if (ifd >= 0)
        {
            close(ifd);
            remove(path);
            rows_unknown(file);
        }
        return;
    }
    char buf[2 * MAX_PATH];
    struct lp_song added[32];
    int n_added = 0;
    bool ok = true;
    struct line_reader lines;
    line_reader_init(&lines, fd, buf, sizeof(buf));
    while (line_read(&lines))
    {
        if (lines.failed)
        {
            ok = false;
            break;
        }
        char *line;
        size_t len;
        while ((line = line_next(&lines, &len)))
        {
            if (len && line[0] != '#')
            {
                char resolved[MAX_PATH];
                const char *song = song_path(line, dir, m3u, resolved,
                                             sizeof(resolved));
                const long idx = song ? tagcache_find_entry(song) : -1;
                if (ifd >= 0)
                {
                    if (idx >= 0)
                    {
                        if (head.seconds != LP_HEADER_UNKNOWN)
                            head.seconds += song_seconds(idx);
                        collage_step(&walk, idx);
                        added[n_added].idx = idx;
                        added[n_added].line = head.lines;
                        if (++n_added == ARRAYLEN(added))
                        {
                            ok = ok && write(ifd, added, sizeof(added))
                                       == (ssize_t)sizeof(added);
                            head.count += n_added;
                            n_added = 0;
                        }
                    }
                    else
                        head.unmatched++;
                    head.lines++;
                }
                if (playing && song)
                    playlist_insert_track(NULL, song, PLAYLIST_INSERT_LAST,
                                          false, false);
            }
        }
    }
    close(fd);
    if (playing)
        playlist_sync(NULL);
    if (ifd < 0)
        return;
    if (n_added)
    {
        const ssize_t bytes = n_added * sizeof(*added);
        ok = ok && write(ifd, added, bytes) == bytes;
        head.count += n_added;
    }
    head.size = file_size(file);
    ok = ok && head.count <= LP_MAX && lseek(ifd, 0, SEEK_SET) == 0
         && write(ifd, &head, sizeof(head)) == sizeof(head);
    if (close(ifd) < 0 || !ok)
    {
        remove(path);
        rows_unknown(file);
        return;
    }
    struct lp_state now = {
        .seconds = head.seconds, .collage_seen = head.collage_seen,
    };
    memcpy(now.collage, head.collage, sizeof(now.collage));
    rows_put(file, head.count, &now);
}

void lp_covers_changed(void)
{
    close(open(LP_COVERS_TODO, O_WRONLY | O_CREAT | O_TRUNC, 0666));
}

void lp_index_moved(const char *from, const char *to)
{
    /* the same name but for its case: the same index */
    if (lp_same_file(from, to))
        return;
    char a[MAX_PATH], b[MAX_PATH];
    index_path(itunesdb_path_hash(from), a, sizeof(a));
    index_path(itunesdb_path_hash(to), b, sizeof(b));
    remove(b);
    rename(a, b);
    rows_moved(from, to);
}

int lp_index_build(const char *file, struct lp_song *songs, int max,
                   char *buf, size_t size, struct lp_state *state)
{
    state->sort = ITUNESDB_SORT_MANUAL;
    state->lines = state->unmatched = 0;
    state->pending = false;
    if (!database_in_ram() || size < MAX_PATH * 2)
        return -1;
    char dir[MAX_PATH];
    const bool m3u = playlist_dir(file, dir, sizeof(dir));
    const int fd = m3u ? open(file, O_RDONLY) : open_utf8(file, O_RDONLY);
    if (fd < 0)
        return -1;
    max = MIN(max, LP_MAX);
    const size_t sort_len = strlen(ITUNESDB_PLAYLIST_SORT);
    int count = 0;
    uint32_t line_no = 0;
    bool full = false;
    struct line_reader lines;
    line_reader_init(&lines, fd, buf, size);
    while (!full && line_read(&lines))
    {
        if (lines.failed)
            break;
        char *line;
        size_t len;
        while ((line = line_next(&lines, &len)))
        {
            if (line[0] == '#')
            {
                /* of its head, not of another's copied into it */
                if (line_no == 0
                    && !strncmp(line, ITUNESDB_PLAYLIST_SORT, sort_len))
                    state->sort = strtoul(line + sort_len, NULL, 10);
            }
            /* the lines of songs, as the playlist counts them: those after
               max not counted, which stay in the file as they are */
            else if (len)
            {
                if (count >= max)
                {
                    full = true;
                    break;
                }
                char resolved[MAX_PATH];
                const char *song = song_path(line, dir, m3u, resolved,
                                             sizeof(resolved));
                const long idx = song ? tagcache_find_entry(song) : -1;
                if (idx >= 0)
                {
                    songs[count].idx = idx;
                    songs[count].line = line_no;
                    count++;
                }
                else
                    state->unmatched++;
                line_no++;
            }
        }
        yield();
    }
    close(fd);
    if (lines.failed)
        return -1;
    /* those past max not counted: not in the file's order then */
    state->lines = line_no;
    return count;
}

/* What each column is sorted by, as on the Mac: a column's tag, then those
   that order songs alike in it. The strings by their places in the
   database's files, which go as the alphabet. */
#define END 0xff
/* an album's artist after its title, as two of one title are each a whole */
static const unsigned char by_artist[] = {
    tag_artist, tag_album, tag_albumartist, tag_discnumber, tag_tracknumber,
    tag_title, END };
static const unsigned char by_album[] = {
    tag_album, tag_albumartist, tag_discnumber, tag_tracknumber, tag_title,
    END };
static const unsigned char by_title[] = {
    tag_title, tag_artist, tag_album, END };
static const unsigned char by_genre[] = {
    tag_genre, tag_artist, tag_album, tag_albumartist, tag_discnumber,
    tag_tracknumber, END };
static const unsigned char by_composer[] = {
    tag_composer, tag_album, tag_albumartist, tag_discnumber,
    tag_tracknumber, END };
static const unsigned char *sort_tags;

static int cmp_song(const void *a, const void *b)
{
    const struct lp_song *x = a, *y = b;
    for (const unsigned char *t = sort_tags; *t != END; t++)
    {
        const long kx = tagcache_sort_key(x->idx, *t);
        const long ky = tagcache_sort_key(y->idx, *t);
        if (kx != ky)
            return kx < ky ? -1 : 1;
    }
    /* those alike as the file had them, any new after */
    if (x->line != y->line)
        return x->line < y->line ? -1 : 1;
    return (x->idx > y->idx) - (x->idx < y->idx);
}

void lp_sort(struct lp_song *songs, int count, uint32_t sort)
{
    switch (sort)
    {
        case ITUNESDB_SORT_ARTIST:   sort_tags = by_artist; break;
        case ITUNESDB_SORT_ALBUM:    sort_tags = by_album; break;
        case ITUNESDB_SORT_TITLE:    sort_tags = by_title; break;
        case ITUNESDB_SORT_GENRE:    sort_tags = by_genre; break;
        case ITUNESDB_SORT_COMPOSER: sort_tags = by_composer; break;
        default:                     return; /* as the file has them */
    }
    qsort(songs, count, sizeof(*songs), cmp_song);
}

/* a line into the block written, the block first if it's full: a line is
   never split between two writes */
static void out_line(struct fw *out, const char *line, size_t n)
{
    if (out->used + n + 1 > out->size)
        fw_flush(out);
    if (n + 1 > out->size)
    {
        out->ok = false;
        return;
    }
    memcpy(out->buf + out->used, line, n);
    out->buf[out->used + n] = '\n';
    out->used += n + 1;
}

/* The songs of a file the database hasn't, as of a format it doesn't take,
   after those written: kept as they were rather than lost. How many. A
   file that can't be read whole fails the write: it would lose the rest. */
static uint32_t keep_unmatched(const char *file, struct fw *out)
{
    const int fd = open_utf8(file, O_RDONLY);
    if (fd < 0)
    {
        out->ok = false;
        return 0;
    }
    char chunk[256], line[MAX_PATH];
    size_t have = 0;
    bool too_long = false;
    uint32_t kept = 0;
    ssize_t n;
    bool end = false;
    while (out->ok && !end)
    {
        n = read(fd, chunk, sizeof(chunk));
        if (n < 0)
        {
            out->ok = false;
            break;
        }
        if (n == 0)
        {
            end = true;
            n = 1;
            chunk[0] = '\n'; /* the last line, if not ended */
        }
        for (ssize_t i = 0; i < n; i++)
        {
            if (chunk[i] != '\n')
            {
                if (have + 1 < sizeof(line))
                    line[have++] = chunk[i];
                else
                    too_long = true;
                continue;
            }
            if (have && line[have - 1] == '\r')
                have--;
            line[have] = '\0';
            if (have && line[0] != '#' && !too_long
                && tagcache_find_entry(line) < 0)
            {
                out_line(out, line, have);
                kept++;
            }
            have = 0;
            too_long = false;
        }
    }
    close(fd);
    return kept;
}

/* lp_write_file()'s mark of a song it couldn't write, until it knows it
   wrote the others: no line can be it */
#define LP_UNWRITTEN (LP_NO_LINE - 1)

bool lp_write_file(const char *file, struct lp_song *songs, int count,
                   struct lp_state *state)
{
    /* the import's only: written with its mark, a playlist made on the iPod
       would be taken for one and removed at the next sync */
    if (!itunesdb_own_playlist(file))
        return false;
    const uint32_t sort = state->sort;
    char path[MAX_PATH];
    /* its time as Finder last changed it, which its place in Playlists goes by */
    const time_t time = file_time(file);
    struct tagcache_search tcs;
    if (!tagcache_search(&tcs, tag_filename))
        return false;
    char block[2048];
    struct fw out;
    fw_open(&out, file, FW_KEEP, block, sizeof(block));
    /* its head as the import writes it, which knows it as its own by that:
       a write of its own */
    char head[sizeof(ITUNESDB_PLAYLIST_HEAD ITUNESDB_PLAYLIST_MARK) + 32];
    int len = snprintf(head, sizeof(head), "%s%s\n", ITUNESDB_PLAYLIST_HEAD,
                       ITUNESDB_PLAYLIST_MARK);
    if (sort > ITUNESDB_SORT_MANUAL)
        len += snprintf(head + len, sizeof(head) - len,
                        ITUNESDB_PLAYLIST_SORT "%lu\n", (unsigned long)sort);
    fw_put(&out, head, len);
    fw_flush(&out);
    uint32_t written = 0;
    for (int i = 0; out.ok && i < count; i++)
    {
        if (!tagcache_retrieve(&tcs, songs[i].idx, tag_filename, path, sizeof(path)))
        {
            songs[i].line = LP_UNWRITTEN;
            continue;
        }
        /* as the import writes them */
        const char *name = no_volume(path);
        out_line(&out, name, strlen(name));
        written++;
        if (!(i & 63))
            yield();
    }
    const uint32_t kept = state->unmatched ? keep_unmatched(file, &out) : 0;
    bool ok = fw_end(&out);
    tagcache_search_finish(&tcs);
    /* none of its songs found, as with the database going: not empty for it */
    if (count > 0 && written == 0)
        ok = false;
    ok = fw_place(file, FW_KEEP, ok);
    /* The songs' lines in the file as it is: the new ones once it has the
       name, else the old ones, by which the index then says it's still to
       be written. One that couldn't be written is taken as in neither, as
       before. */
    uint32_t line = 0;
    for (int i = 0; i < count; i++)
    {
        if (songs[i].line == LP_UNWRITTEN)
            songs[i].line = LP_NO_LINE;
        else if (ok)
            songs[i].line = line++;
    }
    if (!ok)
        return false;
    if (time)
        modtime(file, time);
    state->lines = line + kept;
    state->unmatched = kept;
    return true;
}

bool lp_viewable(const char *file)
{
    return file && file[0] && database_in_ram();
}

void lp_sync(const char *file)
{
    struct lp_state state;
    if (!file || lp_index_read(file, NULL, 0, &state) < 0 || !state.pending
        || playing_from(file))
        return;
    size_t size;
    struct lp_song *songs = plugin_get_buffer(&size);
    if (!songs)
        return;
    const int max = MIN(size / sizeof(*songs), LP_MAX);
    const int n = lp_index_read(file, songs, max, NULL);
    if (n < 0)
        return;
    splash(0, ID2P(LANG_WAIT));
    if (lp_write_file(file, songs, n, &state))
        lp_index_write(file, songs, n, &state);
}

/* The songs the database lost, as a sync replaced or removed their files
   or they were deleted over USB: by their paths' hashes, a block at a time
   to a file, which Updating Library's pass over the playlists takes, goes
   by, then removes (lp_index_all()). Only those leave the playlists made on
   the iPod; a line the database never had stays, as a computer's path,
   which a playlist put there by hand may have. */
#define LP_GONE   ROCKBOX_DIR "/playlist_gone.dat"
/* LP_GONE as the pass took it, by a rename: notes flushed while it goes
   through the playlists start a new LP_GONE. Removed after a whole pass;
   after one cut short, or for the notes it had no room to read, put back
   into LP_GONE for the next */
#define LP_GONE_TAKEN ROCKBOX_DIR "/playlist_gone_taken.dat"
static uint64_t gone_block[32];
static int gone_in_block;
/* a flush between its open and its close, or the pass's rename: the other
   waits, so that no flush writes into the file just taken */
static bool gone_file_busy;

static void gone_file_lock(void)
{
    /* one core, and threads switch only at a yield: none between the look
       and the mark */
    while (gone_file_busy)
        sleep(1);
    gone_file_busy = true;
}

/* Both the UI thread (a Delete) and the database thread note and flush. The
   block is copied and emptied before the first yield (open's), so that a
   note the other thread makes while this one writes goes into the emptied
   block rather than being lost; two flushes at once each append their own
   copy, one after the other. */
void lp_gone_flush(void)
{
    const int count = MIN(gone_in_block, (int)ARRAYLEN(gone_block));
    if (count <= 0)
        return;
    uint64_t block[ARRAYLEN(gone_block)];
    memcpy(block, gone_block, count * sizeof(*block));
    gone_in_block = 0;
    gone_file_lock();
    const int fd = open(LP_GONE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0)
    {
        write(fd, block, count * sizeof(*block));
        close(fd);
    }
    gone_file_busy = false;
}

/* The taken file's notes from the first'th on, appended to LP_GONE, then
   the taken file removed; kept if they couldn't all be, for the next pass
   to put back (a note in LP_GONE twice then does no harm: its line goes
   once). Whole notes only: a cut one would shift those after it. */
static void gone_put_back(long first)
{
    const int in = open(LP_GONE_TAKEN, O_RDONLY);
    if (in < 0)
        return;
    bool ok = lseek(in, first * (off_t)sizeof(uint64_t), SEEK_SET) >= 0;
    int out = -1;
    uint64_t block[ARRAYLEN(gone_block)];
    while (ok)
    {
        const ssize_t n = read(in, block, sizeof(block));
        if (n < 0)
        {
            ok = false;
            break;
        }
        const ssize_t whole = n - n % (ssize_t)sizeof(uint64_t);
        if (whole > 0)
        {
            if (out < 0)
                out = open(LP_GONE, O_WRONLY | O_CREAT | O_APPEND, 0666);
            ok = out >= 0 && write(out, block, whole) == whole;
        }
        /* the end, past a cut note there */
        if (n < (ssize_t)sizeof(block))
            break;
    }
    close(in);
    if (out >= 0 && close(out) < 0)
        ok = false;
    if (ok)
        remove(LP_GONE_TAKEN);
}

/* LP_GONE taken for the pass; first a taken file a reset or power loss left
   in the middle of a pass put back, as one cut short is. If that can't be,
   the pass reads it as it is, and LP_GONE waits for the next. */
static void gone_take(void)
{
    if (file_exists(LP_GONE_TAKEN))
        gone_put_back(0);
    if (file_exists(LP_GONE_TAKEN))
        return;
    gone_file_lock();
    rename(LP_GONE, LP_GONE_TAKEN); /* none: nothing to take */
    gone_file_busy = false;
}

void lp_gone_note(const char *path)
{
    const uint64_t hash = itunesdb_path_hash(path);
    /* never past the block: a flush empties it before its first yield, so
       it isn't full here; if it ever were, flushed first, or else refused */
    if (gone_in_block >= (int)ARRAYLEN(gone_block))
        lp_gone_flush();
    if (gone_in_block >= (int)ARRAYLEN(gone_block))
        return;
    gone_block[gone_in_block++] = hash;
    if (gone_in_block == (int)ARRAYLEN(gone_block))
        lp_gone_flush();
}

static int cmp_hash64(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* those of the file, sorted, while Updating Library goes through the
   playlists; none otherwise */
static uint64_t *gone_set;
static int gone_count;

static bool gone(const char *path)
{
    const uint64_t hash = itunesdb_path_hash(path);
    int lo = 0, hi = gone_count - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (gone_set[mid] == hash)
            return true;
        if (gone_set[mid] < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}

/* whether the music plays straight from this file, not from a copy of it:
   a playlist of the iPod's own as 1.2 played it, or without the database */
static bool playing_straight_from(const char *file)
{
    char name[MAX_PATH];
    return (audio_status() & AUDIO_STATUS_PLAY)
           && playlist_get_name(NULL, name, sizeof(name))
           && !is_copy(name) && lp_same_file(name, file);
}

/* A playlist made on the iPod as the library is now: a song Finder wrote
   again under another path, as the last sync's dbids tell, at its new path;
   one whose file is gone, deleted from the computer or by a sync, out. Those
   not in the database whose files are there stay, as they play. The file
   written again only if a line changed; through buf, a block at a time. True
   if it was. */
static bool clean_playlist(const char *file, char *buf, size_t size)
{
    char dir[MAX_PATH];
    const bool m3u = playlist_dir(file, dir, sizeof(dir));
    const int fd = m3u ? open(file, O_RDONLY) : open_utf8(file, O_RDONLY);
    if (fd < 0 || size < 2 * MAX_PATH)
    {
        if (fd >= 0)
            close(fd);
        return false;
    }
    struct fw w = { .fd = -1 };
    bool changed = false, ok = true;
    char out[1024];
    size_t put = 0;
    /* the lines before the first change are copied once it's known: from the
       file's start, a BOM open_utf8() passed over too */
    off_t kept_to = 0, read_at = lseek(fd, 0, SEEK_CUR);
    if (read_at < 0)
        read_at = 0;
    struct line_reader lines;
    line_reader_init(&lines, fd, buf, size);
    while (ok && line_read(&lines))
    {
        if (lines.failed)
        {
            ok = false;
            break;
        }
        char *line;
        size_t len;
        while ((line = line_next(&lines, &len)))
        {
            const off_t line_at = read_at + lines.at;
            const char *text = line;
            char moved[MAX_PATH];
            bool drop = false, replace = false;
            if (len && line[0] != '#')
            {
                char resolved[MAX_PATH];
                const char *song = song_path(line, dir, m3u, resolved,
                                             sizeof(resolved));
                if (song && tagcache_find_entry(song) < 0)
                {
                    /* where a sync moved it, and on, if a later one moved it
                       again before this */
                    const char *at = song;
                    for (int hop = 0; hop < 4 && itunesdb_moved_to(
                             itunesdb_path_hash(at), moved, sizeof(moved)); hop++)
                    {
                        replace = true;
                        at = moved;
                        if (tagcache_find_entry(moved) >= 0)
                            break;
                    }
                    if (!replace && gone(song))
                        drop = true;
                }
            }
            if ((drop || replace) && !changed)
            {
                /* the lines before it, as they were */
                changed = true;
                /* each write as the program's own, through out */
                ok = fw_open(&w, file, FW_KEEP, NULL, 0);
                kept_to = line_at;
                const int rfd = open(file, O_RDONLY);
                ok = ok && rfd >= 0;
                for (off_t done = 0; ok && done < kept_to; )
                {
                    const ssize_t k = read(rfd, out, MIN((off_t)sizeof(out),
                                                         kept_to - done));
                    ok = k > 0 && fw_put(&w, out, k);
                    done += k;
                }
                if (rfd >= 0)
                    close(rfd);
            }
            if (changed && ok && !drop)
            {
                if (replace)
                    text = moved;
                const size_t tlen = strlen(text);
                if (put + tlen + 1 > sizeof(out))
                {
                    ok = fw_put(&w, out, put);
                    put = 0;
                }
                if (tlen + 1 <= sizeof(out))
                {
                    memcpy(out + put, text, tlen);
                    out[put + tlen] = '\n';
                    put += tlen + 1;
                }
            }
        }
        yield();
    }
    close(fd);
    if (!changed)
        return false;
    if (ok && put)
        ok = fw_put(&w, out, put);
    return fw_close(&w, ok);
}

/* A playlist's index from its file, in the order it's shown by, and its file
   in that order too if rewrite, else to be written yet */
static int index_make(const char *file, struct lp_song *songs, int max,
                      char *buf, size_t size, bool rewrite)
{
    struct lp_state state;
    int n = lp_index_build(file, songs, max, buf, size, &state);
    if (n < 0)
        return n;
    /* one made on the iPod in its own order, whatever its head says, as it
       was copied from one of the import's; without the songs gone */
    if (!itunesdb_own_playlist(file))
    {
        /* not under the music playing straight from it, which keeps its
           lines by where they are in the file */
        if (state.unmatched > 0 && !playing_straight_from(file)
            && clean_playlist(file, buf, size))
        {
            n = lp_index_build(file, songs, max, buf, size, &state);
            if (n < 0)
                return n;
        }
        state.sort = ITUNESDB_SORT_MANUAL;
        rewrite = false;
    }
    lp_sort(songs, n, state.sort);
    if (rewrite && !lp_in_file_order(songs, n, &state)
        && !playing_from(file))
        lp_write_file(file, songs, n, &state);
    lp_index_write(file, songs, n, &state);
    return n;
}

/** Playlists' rows (Playlist Covers) **/

/* Music > Playlists' rows with their covers: each playlist's songs, length
   and collage's albums as its index's head has them, and each folder's
   playlists, at any depth, from a table beside catalog.dat made at the end of
   the playlists' pass at Updating Library, read whole into memory with the
   covers' (list_covers_apply()), music stopped. Rows are found in it by their
   paths' hashes as they're drawn, no index opened. The iPod's own changes,
   as a head is written again, change it in memory; the file, no longer of
   them, is removed at the first, while the disk is on for the change's own
   writes, and written whole from memory as the disk goes idle, at USB and
   at power off (the storage's idle callback, system_flush()) and before
   the boot's pass is skipped: never as Playlists opens, which would wake a
   sleeping disk. A power cut between the removal and that write leaves no
   file, made again by the next pass, rather than a stale one taken for
   current. A change while they aren't in memory removes the file the same
   way. On-The-Go, outside the catalogue's folder, which the pass's mark
   doesn't see, has its row in the head, written in place at once when the
   file is whole. The boot's pass runs for the file only with Playlist
   Covers on (rows_wanted()): a change with it off, as a smart playlist's
   head written again at boot, which the mark doesn't see, leaves the file
   gone until a boot with it on. A library over the pass's room has a file
   of no rows (LP_ROWS_OFF), which no change removes: the mark sees the
   catalogue's. */
#define LP_ROWS_FILE    LP_DIR "/catalog_rows.dat"
#define LP_ROWS_MAGIC   0x504c5257 /* "PLRW" */
#define LP_ROWS_VERSION 1
/* playlists and folders, at most; and room for the iPod's new ones in
   memory, which grows only as it's taken in again, music stopped */
#define LP_ROWS_MAX     1536
#define LP_ROWS_SPARE   64
#define LP_NO_ALBUM     0xffff
#define LP_ROW_FOLDER   0x0001
/* 0x0002, Unsynced's once, never read: older files have it */
#define LP_ROW_UNKNOWN  0x0004 /* its songs not known: no second line */
/* the head's flags: the pass had more playlists and folders than it keeps,
   Playlist Covers off for the library as it is, the file there so that the
   next boots skip the pass as before. A file of it stops the pass whatever
   its database and On-The-Go: a change of LP_ROWS_MAX changes the version */
#define LP_ROWS_OFF     0x0001

struct lp_row {
    uint64_t hash;      /* itunesdb_path_hash() of its path */
    uint32_t count;     /* its songs, or a folder's playlists */
    uint32_t seconds;   /* LP_HEADER_UNKNOWN for not made */
    uint16_t album[LP_COLLAGE]; /* the collage's, LP_NO_ALBUM past them */
    uint16_t flags;
    uint16_t unused[3];
};

struct lp_rows_head {
    uint32_t magic, version;
    uint32_t commitid, entries; /* of the database, as an index's head */
    uint32_t table;             /* album_years_hash(): the albums' numbers */
    uint32_t count;             /* rows after it, sorted by hash */
    uint32_t unsynced;          /* playlists in Unsynced, at any depth */
    uint32_t flags;             /* LP_ROWS_OFF */
    struct lp_row otg;          /* On-The-Go's, hash 0 for none */
};

/* In memory: the head, and the rows in a block of capacity of them */
static struct lp_rows_head rows_head;
static int rows_handle;
static int rows_capacity;
static bool rows_in;        /* taken in */
/* changed since they were taken in or written, the file gone meanwhile */
static bool rows_dirty;
static bool rows_spoilt;    /* a change couldn't be kept: not shown */
/* the pass is making them, which the heads it writes don't change */
static bool rows_held;
/* The file written from them, by the storage's thread at its idle or by
   this one: nothing changes them nor writes the file meanwhile (rows_wait()).
   Changes come from the UI's thread, and from the import's database thread
   under Updating Library's screen */
static volatile bool rows_busy;
/* A change half made, from rows_changing() to rows_changed(), which can
   wait on the disk: no write from them meanwhile, which would be of half of
   it and taken for current; rows_changed() asks for the idle again */
static volatile bool rows_mid_change;

static void rows_wait(void)
{
    while (rows_busy)
        sleep(1);
}

static bool rows_head_current(const struct lp_rows_head *h)
{
    return h->magic == LP_ROWS_MAGIC && h->version == LP_ROWS_VERSION
           && h->commitid == (uint32_t)tagcache_get_commitid()
           && h->entries == (uint32_t)tagcache_entry_count()
           && h->table == album_years_hash() && album_years_ready()
           && h->count <= LP_ROWS_MAX;
}

/* one of a library over the pass's room (LP_ROWS_OFF): no rows, never
   taken in, nor written in */
static bool rows_head_off(const struct lp_rows_head *h)
{
    return h->magic == LP_ROWS_MAGIC && h->version == LP_ROWS_VERSION
           && (h->flags & LP_ROWS_OFF);
}

static bool rows_file_off(void)
{
    const int fd = open(LP_ROWS_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    struct lp_rows_head h;
    const bool off = read(fd, &h, sizeof(h)) == sizeof(h) && rows_head_off(&h);
    close(fd);
    return off;
}

/* Playlist Covers on, with Covers in Lists, as the settings are, its memory
   taken or not: the boot's pass runs for the file only then */
static bool rows_wanted(void)
{
    return global_settings.playlist_covers && list_covers_wanted();
}

bool lp_rows_loaded(void)
{
    return rows_in;
}

bool lp_rows_ready(void)
{
    return rows_in && !rows_spoilt && rows_head_current(&rows_head);
}

static struct lp_row *rows_data(void)
{
    return rows_handle > 0 ? core_get_data(rows_handle) : NULL;
}

/* The rows changed in memory to the file, with the head, pinned as the disk
   writes from them: by the storage's thread as the disk goes idle, at USB
   and at power off (system_flush()), by lp_rows_free() with the disk on,
   and before the boot's pass is skipped; not while a change is half made.
   A write that failed leaves the file gone and them changed, written by a
   later change's idle, lp_rows_free() with the disk on or the next Updating
   Library, as USB and power off don't ask again. Not of the library as it
   is any more, nothing: the file stays gone, made again by the next pass */
static void rows_flush(void)
{
    if (!rows_dirty || !rows_in || rows_busy || rows_mid_change)
        return;
    if (!lp_rows_ready())
    {
        rows_dirty = false;
        return;
    }
    rows_busy = true;
    /* off the storage thread's 2 KB stack */
    static struct fw w;
    bool ok = fw_open(&w, LP_ROWS_FILE, FW_CACHE, NULL, 0)
              && fw_put(&w, &rows_head, sizeof(rows_head));
    if (ok && rows_head.count > 0)
    {
        core_pin(rows_handle);
        ok = fw_put(&w, rows_data(), rows_head.count * sizeof(struct lp_row));
        core_unpin(rows_handle);
    }
    if (fw_close(&w, ok))
        rows_dirty = false;
    rows_busy = false;
}

static void rows_idle(void)
{
    rows_flush();
}

/* A change made in them, as rows_changing() began it. The first since the
   file was of them: the file removed, the disk on for the change's own
   writes. Their write asked of the disk's next idle */
static void rows_changed(void)
{
    if (!rows_dirty)
        remove(LP_ROWS_FILE);
    rows_dirty = true;
    rows_mid_change = false;
    register_storage_idle_func(rows_idle);
}

void lp_rows_free(void)
{
    rows_wait();
    /* the changes kept only if the disk is on, not woken for them: else the
       file, removed at the first, is made again by the next pass */
    if (rows_dirty && storage_disk_is_active())
        rows_flush();
    unregister_storage_idle_func(rows_idle, false);
    if (rows_handle > 0)
        rows_handle = core_free(rows_handle);
    rows_capacity = 0;
    rows_in = rows_dirty = rows_spoilt = false;
}

bool lp_rows_load(void)
{
    lp_rows_free();
    /* nothing taken while music plays or is paused: its buffer gives way */
    if (audio_status() != 0)
        return false;
    const int fd = open(LP_ROWS_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    struct lp_rows_head h;
    /* one of a library over the pass's room: Playlist Covers off for it */
    bool ok = read(fd, &h, sizeof(h)) == sizeof(h) && rows_head_current(&h)
              && !(h.flags & LP_ROWS_OFF);
    if (ok)
    {
        const int capacity = MIN(h.count + LP_ROWS_SPARE, LP_ROWS_MAX);
        rows_handle = core_alloc(capacity * sizeof(struct lp_row));
        ok = rows_handle > 0;
        if (ok)
        {
            rows_capacity = capacity;
            const ssize_t bytes = h.count * sizeof(struct lp_row);
            core_pin(rows_handle);
            ok = read(fd, rows_data(), bytes) == bytes;
            core_unpin(rows_handle);
        }
        else
            rows_handle = 0;
    }
    close(fd);
    if (!ok)
    {
        lp_rows_free();
        return false;
    }
    rows_head = h;
    rows_in = true;
    return true;
}

/* a row's place by its hash, or where it would go */
static int rows_place(uint64_t hash, bool *found)
{
    const struct lp_row *r = rows_data();
    int lo = 0, hi = rows_head.count;
    while (lo < hi)
    {
        const int mid = (lo + hi) / 2;
        if (r[mid].hash < hash)
            lo = mid + 1;
        else
            hi = mid;
    }
    *found = lo < (int)rows_head.count && r[lo].hash == hash;
    return lo;
}

/* a row of the hash, made in its place if it isn't there and make: NULL
   without it, or without room for it, which spoils them all until they're
   made again */
static struct lp_row *rows_row(uint64_t hash, bool make)
{
    bool found = false;
    const int at = rows_head.count > 0 ? rows_place(hash, &found) : 0;
    if (found)
        return rows_data() + at;
    if (!make)
        return NULL;
    if ((int)rows_head.count >= rows_capacity)
    {
        rows_spoilt = true;
        return NULL;
    }
    struct lp_row *r = rows_data();
    memmove(&r[at + 1], &r[at], (rows_head.count - at) * sizeof(*r));
    rows_head.count++;
    memset(&r[at], 0, sizeof(*r));
    r[at].hash = hash;
    for (int k = 0; k < LP_COLLAGE; k++)
        r[at].album[k] = LP_NO_ALBUM;
    return &r[at];
}

static void rows_drop(uint64_t hash)
{
    bool found = false;
    if (rows_head.count == 0)
        return;
    const int at = rows_place(hash, &found);
    if (!found)
        return;
    struct lp_row *r = rows_data();
    memmove(&r[at], &r[at + 1], (rows_head.count - at - 1) * sizeof(*r));
    rows_head.count--;
}

/* a row's values from an index's state: its collage's songs' albums by their
   numbers, as the lists' covers have them */
static void row_set(struct lp_row *r, int count, const struct lp_state *state)
{
    r->count = MAX(count, 0);
    r->seconds = state->seconds;
    r->flags &= ~LP_ROW_UNKNOWN;
    int n = 0;
    for (int k = 0; k < LP_COLLAGE && state->collage_seen != LP_HEADER_UNKNOWN
                    && state->collage[k] >= 0; k++)
    {
        const int no = tagtree_song_album(state->collage[k]);
        if (no >= 0 && no < LP_NO_ALBUM)
            r->album[n++] = no;
    }
    while (n < LP_COLLAGE)
        r->album[n++] = LP_NO_ALBUM;
}

/* The path of the playlist a change is of, as in_catalog() gives it, off
   the stack: the changes are made on the UI's thread, deep in a menu, whose
   stack is 8 KB, and on the import's database thread under Updating
   Library's screen, which the UI waits on; never two at once, none entered
   again from another */
static char rows_path[MAX_PATH];

/* In the catalogue's folder: the length of its path without the drive and
   the separators doubled, the path so into buf; 0 for one outside it, as
   On-The-Go's */
static size_t in_catalog(const char *file, char *buf, size_t size)
{
    /* off the stack, as rows_path: one change at a time */
    static char root[MAX_PATH], top[MAX_PATH];
    if (!catalog_directory_name(root, sizeof(root)))
        return 0;
    tidy_copy(top, sizeof(top), root);
    size_t n = strlen(top);
    while (n > 1 && top[n - 1] == '/')
        top[--n] = '\0';
    tidy_copy(buf, size, file);
    return n > 0 && !strncasecmp(buf, top, n) && buf[n] == '/' ? n : 0;
}

/* A playlist of path into the counts or out of them, delta 1 or -1: each
   folder's it is in, up to the catalogue's top, and Unsynced's if it's
   there, as the item at the top it is in is or isn't the computer's. A
   folder without a row has one made, of its playlists */
static void rows_folders(const char *path, int delta)
{
    static char p[MAX_PATH]; /* off the stack, as rows_path */
    const size_t root = in_catalog(path, p, sizeof(p));
    if (!root)
        return;
    char *end = strchr(p + root + 1, '/');
    /* the item at the top: a folder, or the playlist itself */
    if (end)
        *end = '\0';
    const bool mine = !ip_is_synced(p);
    if (end)
        *end = '/';
    if (mine)
        rows_head.unsynced = MAX((int)rows_head.unsynced + delta, 0);
    for (; end; end = strchr(end + 1, '/'))
    {
        *end = '\0';
        struct lp_row *r = rows_row(itunesdb_path_hash(p), delta > 0);
        *end = '/';
        if (!r)
            continue;
        r->flags |= LP_ROW_FOLDER;
        r->count = MAX((int)r->count + delta, 0);
    }
}

/* On-The-Go's row changed: in its place in the file's head at once, as the
   pass's mark doesn't see its file, and in memory */
static void rows_otg(const struct lp_row *row)
{
    rows_wait();
    if (rows_in)
        rows_head.otg = *row;
    /* none while the rows changed are still to be written: the file is gone
       until then */
    rows_busy = true;
    const int fd = open(LP_ROWS_FILE, O_RDWR);
    if (fd >= 0)
    {
        struct lp_rows_head h;
        const bool got = read(fd, &h, sizeof(h)) == sizeof(h);
        /* one of no rows stays as it is */
        const bool off = got && rows_head_off(&h);
        bool ok = off;
        if (got && !off && rows_head_current(&h))
        {
            h.otg = *row;
            ok = lseek(fd, 0, SEEK_SET) == 0 && write(fd, &h, sizeof(h)) == sizeof(h);
        }
        /* one half written is made again */
        if ((close(fd) < 0 || !ok) && !off)
            remove(LP_ROWS_FILE);
    }
    rows_busy = false;
}

/* A change of a playlist of the catalogue's: kept in memory, half made
   until rows_changed(), which every caller given true ends it with, and
   written at the disk's idle; or, without them in memory as they are, the
   file removed and made again by the next pass. One of no rows stays, with
   nothing in it a change makes stale */
static bool rows_changing(void)
{
    if (rows_held)
        return false;
    rows_wait();
    if (lp_rows_ready())
    {
        rows_mid_change = true;
        return true;
    }
    if (!rows_file_off())
        remove(LP_ROWS_FILE);
    return false;
}

static void rows_put(const char *file, int count, const struct lp_state *state)
{
    if (rows_held)
        return;
    if (lp_same_file(file, OTG_FILE))
    {
        struct lp_row r = { .hash = itunesdb_path_hash(OTG_FILE) };
        row_set(&r, count, state);
        rows_otg(&r);
        return;
    }
    /* one viewed from elsewhere, as the file browser, has no row */
    if (!in_catalog(file, rows_path, sizeof(rows_path)) || !rows_changing())
        return;
    const uint64_t hash = itunesdb_path_hash(file);
    if (!rows_row(hash, false))
        rows_folders(file, 1);
    struct lp_row *r = rows_row(hash, true);
    if (r)
        row_set(r, count, state);
    rows_changed();
}

void lp_index_new(const char *file)
{
    /* its file of no songs yet, in its own order: what's added to it is
       added to the index then, its head and its row with it */
    struct lp_state state = { .sort = ITUNESDB_SORT_MANUAL };
    lp_index_write(file, NULL, 0, &state);
}

static void rows_unknown(const char *file)
{
    if (rows_held)
        return;
    if (lp_same_file(file, OTG_FILE))
    {
        struct lp_row r = { .hash = itunesdb_path_hash(OTG_FILE),
                            .flags = LP_ROW_UNKNOWN };
        for (int k = 0; k < LP_COLLAGE; k++)
            r.album[k] = LP_NO_ALBUM;
        rows_otg(&r);
        return;
    }
    char *p = rows_path;
    if (!in_catalog(file, p, sizeof(rows_path)) || !rows_changing())
        return;
    struct lp_row *r = rows_row(itunesdb_path_hash(file), false);
    if (r)
        r->flags |= LP_ROW_UNKNOWN;
    rows_changed();
}

/* an index forgotten: of a playlist removed, out of its folders' counts; of
   one written again, as by the import, not known until it's made again */
static void rows_forget(const char *file)
{
    if (rows_held)
        return;
    if (file_exists(file))
    {
        rows_unknown(file);
        return;
    }
    if (lp_same_file(file, OTG_FILE))
    {
        const struct lp_row none = { .hash = 0 };
        rows_otg(&none);
        return;
    }
    char *p = rows_path;
    if (!in_catalog(file, p, sizeof(rows_path)) || !rows_changing())
        return;
    const uint64_t hash = itunesdb_path_hash(file);
    if (rows_row(hash, false))
    {
        rows_drop(hash);
        rows_folders(file, -1);
    }
    rows_changed();
}

/* a playlist moved or renamed, On-The-Go saved as one of the catalogue's */
static void rows_moved(const char *from, const char *to)
{
    if (rows_held)
        return;
    struct lp_row was = { .hash = 0 };
    bool had = false;
    const bool from_otg = lp_same_file(from, OTG_FILE);
    if (from_otg)
    {
        /* its row, from the file's head if not in memory */
        struct lp_rows_head h;
        const int fd = rows_in ? -1 : open(LP_ROWS_FILE, O_RDONLY);
        if (rows_in)
            h = rows_head;
        had = (rows_in || (fd >= 0 && read(fd, &h, sizeof(h)) == sizeof(h)
                           && rows_head_current(&h)))
              && h.otg.hash != 0 && !(h.otg.flags & LP_ROW_UNKNOWN);
        if (fd >= 0)
            close(fd);
        was = h.otg;
        const struct lp_row none = { .hash = 0 };
        rows_otg(&none);
    }
    if (!rows_changing())
        return;
    if (!from_otg)
    {
        const uint64_t hash = itunesdb_path_hash(from);
        const struct lp_row *r = rows_row(hash, false);
        had = r != NULL;
        if (had)
        {
            was = *r;
            rows_drop(hash);
            rows_folders(from, -1);
        }
    }
    if (!in_catalog(to, rows_path, sizeof(rows_path)))
    {
        rows_changed();
        return;
    }
    const uint64_t hash = itunesdb_path_hash(to);
    if (!rows_row(hash, false))
        rows_folders(to, 1);
    struct lp_row *r = rows_row(hash, true);
    if (r)
    {
        if (had)
            *r = was;
        else
            r->flags |= LP_ROW_UNKNOWN;
        r->hash = hash;
        r->flags &= ~LP_ROW_FOLDER;
    }
    rows_changed();
}

/* What a row shows, of the catalogue's as the list draws it */
bool lp_rows_find(uint64_t hash, struct lp_row_shown *shown)
{
    if (!lp_rows_ready())
        return false;
    const struct lp_row *r;
    if (hash == rows_head.otg.hash && hash)
        r = &rows_head.otg;
    else
        r = rows_row(hash, false);
    if (!r)
        return false;
    shown->folder = r->flags & LP_ROW_FOLDER;
    shown->known = !(r->flags & LP_ROW_UNKNOWN);
    shown->count = r->count;
    shown->seconds = r->seconds;
    shown->albums = 0;
    for (int k = 0; k < LP_COLLAGE && r->album[k] != LP_NO_ALBUM; k++)
        shown->album[shown->albums++] = r->album[k];
    return true;
}

int lp_rows_unsynced(void)
{
    return lp_rows_ready() ? (int)rows_head.unsynced : -1;
}

const char *lp_rows_text(const struct lp_row_shown *row, char *buf, size_t size)
{
    if (row->folder)
    {
        snprintf(buf, size, plural_str(row->count, LANG_PLAYLIST_COUNT_ONE,
                                       LANG_PLAYLIST_COUNT_FEW,
                                       LANG_PLAYLIST_COUNT_MANY),
                 (int)row->count);
        return buf;
    }
    if (!row->known)
        return NULL;
    /* "24 songs · 1 h 32 min", as its header has it */
    size_t len = snprintf(buf, size, plural_str(row->count, LANG_ALBUM_SONGS_ONE,
                                                LANG_ALBUM_SONGS_FEW,
                                                LANG_ALBUM_SONGS_MANY),
                          (int)row->count);
    if (row->count > 0 && row->seconds && row->seconds != LP_HEADER_UNKNOWN
        && len + 4 < size)
    {
        len += snprintf(buf + len, size - len, " \xc2\xb7 "); /* U+00B7 */
        long minutes = MAX((long)((row->seconds + 30) / 60), 1);
#if LCD_WIDTH < 320
        /* the iPod photo's narrower row: from an hour on its hours alone,
           the nearest, "2973 songs · 180 h", as the minutes cut the line
           short in many languages; its header has them */
        if (minutes >= 60)
            minutes = (minutes + 30) / 60 * 60;
#endif
        tagtree_minutes_text(minutes, buf + len, size - len);
    }
    return buf;
}

/* The pass's rows, made in its room: each playlist's and folder's as it goes,
   sorted and written at its end */
static struct {
    struct lp_row *rows;
    int count, most;
    bool full;
    uint32_t unsynced;
    struct lp_row otg;
} pass;

static void row_made(struct lp_row *r, const char *path, int count,
                     const struct lp_state *state, uint16_t flags)
{
    memset(r, 0, sizeof(*r));
    r->hash = itunesdb_path_hash(path);
    r->flags = flags;
    if (state && count >= 0)
        row_set(r, count, state);
    else
    {
        r->count = MAX(count, 0);
        r->seconds = LP_HEADER_UNKNOWN;
        for (int k = 0; k < LP_COLLAGE; k++)
            r->album[k] = LP_NO_ALBUM;
        if (!(flags & LP_ROW_FOLDER))
            r->flags |= LP_ROW_UNKNOWN;
    }
}

static void pass_row(const char *path, int count, const struct lp_state *state,
                     uint16_t flags)
{
    if (pass.count >= pass.most)
    {
        pass.full = true;
        return;
    }
    row_made(&pass.rows[pass.count++], path, count, state, flags);
}

static int cmp_row(const void *a, const void *b)
{
    const uint64_t x = ((const struct lp_row *)a)->hash;
    const uint64_t y = ((const struct lp_row *)b)->hash;
    return (x > y) - (x < y);
}

/* The pass's rows to the file; with more than it keeps, a file of none,
   Playlist Covers off for the library as it is (said in health.log), so
   that the next boots skip the pass as before */
static void pass_write(void)
{
    struct lp_rows_head h = {
        .magic = LP_ROWS_MAGIC, .version = LP_ROWS_VERSION,
        .commitid = tagcache_get_commitid(), .entries = tagcache_entry_count(),
        .table = album_years_hash(), .count = pass.full ? 0 : pass.count,
        .unsynced = pass.unsynced, .flags = pass.full ? LP_ROWS_OFF : 0,
        .otg = pass.otg,
    };
    if (pass.full)
    {
        memset(&h.otg, 0, sizeof(h.otg));
        health_note("playlists: more than %d playlists and folders, "
                    "Playlist Covers off", LP_ROWS_MAX);
    }
    else
        qsort(pass.rows, pass.count, sizeof(*pass.rows), cmp_row);
    struct fw w;
    if (!fw_open(&w, LP_ROWS_FILE, FW_CACHE, NULL, 0))
        return;
    fw_put(&w, &h, sizeof(h));
    fw_put(&w, pass.rows, h.count * sizeof(*pass.rows));
    fw_close(&w, true);
}

/* the file of the database and table of years as they are, and On-The-Go's
   row of its file as it is: else the pass makes it again. One of no rows,
   which the pass would make again as it is, the catalogue as the mark has
   it: the pass's room is no larger for a database or On-The-Go */
static bool rows_file_current(void)
{
    const int fd = open(LP_ROWS_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    struct lp_rows_head h;
    const bool got = read(fd, &h, sizeof(h)) == sizeof(h);
    close(fd);
    if (got && rows_head_off(&h))
        return true;
    const bool otg = file_exists(OTG_FILE);
    return got && rows_head_current(&h) && (h.otg.hash != 0) == otg
           && !(otg && (h.otg.flags & LP_ROW_UNKNOWN));
}

/* The indexes, before the menu: of each playlist of the import's in the
   catalogue's folder and those in it, one that isn't of the database as it
   is; and those of playlists gone, removed */
#define LP_SEEN_MAX 1024
static struct {
    struct lp_song *songs;
    int max;
    char *buf;
    size_t size;
    uint64_t *seen;
    int seen_count;
    bool overflow;
    bool stopped;   /* by go_on(), as USB came: the overflow not of the room */
    bool covers;    /* each one's collage chosen again (LP_COVERS_TODO) */
    bool (*go_on)(void);
} all;

/* A playlist's header made again from its index, which is of the database
   as it is, and its head written again in place if it changed: its collage
   chosen again from its first songs, those the choice looks at, and its
   length, if it wasn't made, from all of them, if they fit */
static void collage_again(const char *file)
{
    struct lp_state state;
    const int count = lp_index_read(file, NULL, 0, &state);
    if (count < 0)
        return;
    const bool length = state.seconds == LP_HEADER_UNKNOWN;
    const int n = lp_index_read(file, all.songs,
                                MIN(all.max, length ? LP_MAX : LP_COLLAGE_SONGS),
                                &state);
    if (n < 0)
        return;
    const struct lp_state was = state;
    /* without room for all its songs, the length stays not made, and the
       header without it */
    if (length && n == count)
    {
        state.seconds = 0;
        for (int i = 0; i < n; i++)
        {
            state.seconds += song_seconds(all.songs[i].idx);
            if ((i & 1023) == 1023)
                yield();
        }
    }
    collage_choose(all.songs, MIN(n, LP_COLLAGE_SONGS), &state);
    if (state.seconds == was.seconds && state.collage_seen == was.collage_seen
        && !memcmp(state.collage, was.collage, sizeof(state.collage)))
        return;
    char path[MAX_PATH];
    index_path(itunesdb_path_hash(file), path, sizeof(path));
    const int fd = open(path, O_RDWR);
    if (fd < 0)
        return;
    struct lp_head head;
    bool ok = read(fd, &head, sizeof(head)) == sizeof(head)
              && head_current(&head, file);
    if (ok)
    {
        head.seconds = state.seconds;
        head.collage_seen = state.collage_seen;
        memcpy(head.collage, state.collage, sizeof(head.collage));
        ok = lseek(fd, 0, SEEK_SET) == 0
             && write(fd, &head, sizeof(head)) == sizeof(head);
    }
    /* one half written is made again */
    if (close(fd) < 0 || !ok)
        remove(path);
}

/* A current index that may hold a song noted gone, as by a Delete on the
   iPod, which changes neither the database's commit nor its count: one of
   its songs deleted since it was made, or lines the database hasn't, as a
   deleted one becomes once the viewer writes the index after an edit
   (lp_load() leaves it out). Made again, its file without the
   lines of those gone (clean_playlist()). Looked for only with songs noted
   gone, in the pass's room */
static bool index_has_gone(const char *file)
{
    struct lp_state state;
    if (gone_count <= 0)
        return false;
    const int n = lp_index_read(file, all.songs, all.max, &state);
    if (n >= 0 && state.unmatched > 0)
        return true;
    for (int i = 0; i < n; i++)
    {
        if (tagcache_entry_deleted(all.songs[i].idx))
            return true;
    }
    return false;
}

/* The playlists of a folder and those in it: their indexes, and their rows
   and the folders', those of Unsynced counted, at its top or under a folder
   of it (mine). How many playlists */
static int index_folder(char *path, size_t len, int depth, bool mine)
{
    DIR *dir = opendir(path);
    if (!dir)
        return 0;
    int playlists = 0;
    struct dirent *e;
    while ((e = readdir(dir)))
    {
        if (e->d_name[0] == '.')
            continue;
        if (all.go_on && !all.go_on())
        {
            all.overflow = all.stopped = true; /* not all seen */
            break;
        }
        const bool is_dir = dir_get_info(dir, e).attribute & ATTR_DIRECTORY;
        if ((size_t)snprintf(path + len, MAX_PATH - len, "/%s", e->d_name)
            >= MAX_PATH - len)
        {
            path[len] = '\0';
            all.overflow = true; /* one may have an index */
            continue;
        }
        const char *ext = strrchr(e->d_name, '.');
        /* an item at the top Unsynced has, as tree_catalog_show_item() */
        const bool own = depth == 0 ? !ip_is_synced(path) : mine;
        if (is_dir)
        {
            if (depth < 8)
            {
                const int in = index_folder(path, strlen(path), depth + 1, own);
                pass_row(path, in, NULL, LP_ROW_FOLDER);
                playlists += in;
            }
            else
                all.overflow = true;
        }
        else if (ext && (!strcasecmp(ext, ".m3u8") || !strcasecmp(ext, ".m3u")))
        {
            struct lp_state state;
            int n = lp_index_read(path, NULL, 0, &state);
            if (n < 0 || index_has_gone(path))
            {
                index_make(path, all.songs, all.max, all.buf, all.size, true);
                n = lp_index_read(path, NULL, 0, &state);
            }
            else if (all.covers)
            {
                collage_again(path);
                n = lp_index_read(path, NULL, 0, &state);
            }
            pass_row(path, n, n >= 0 ? &state : NULL, 0);
            playlists++;
            if (own)
                pass.unsynced++;
            if (all.seen_count < LP_SEEN_MAX)
                all.seen[all.seen_count++] = itunesdb_path_hash(path);
            else
                all.overflow = true;
        }
        path[len] = '\0';
        yield();
    }
    closedir(dir);
    return playlists;
}

/* What the last whole pass saw: the database, and the catalogue's folder by
   its playlists' and folders' names, sizes and times, which its listing has,
   none opened. The same at the next boot, nothing to look at: each playlist's
   head and index were read again, two reads each. */
#define LP_MARK      LP_DIR "/catalog.dat"
#define LP_MARK_MAGIC 0x504c4943 /* "PLIC" */
struct lp_mark {
    uint32_t magic, version;
    uint32_t commitid, entries;
    uint64_t catalog;
};

static uint64_t mix(uint64_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

/* What a cut can leave of a playlist's write (fw_leftover()) */
static bool playlist_leftover(const char *name)
{
    const size_t len = strlen(name);
    if (len < 9)
        return false;
    const char *cut = name + len - 4;
    if (strcasecmp(cut, ".tmp") && strcasecmp(cut, ".new")
        && strcasecmp(cut, ".old"))
        return false;
    return !strncasecmp(cut - 5, ".m3u8", 5)
           || !strncasecmp(cut - 4, ".m3u", 4);
}

static uint64_t catalog_print(char *path, size_t len, int depth, uint64_t h)
{
    DIR *dir = opendir(path);
    if (!dir)
        return h;
    struct dirent *e;
    while ((e = readdir(dir)))
    {
        if (e->d_name[0] == '.')
            continue;
        const struct dirinfo info = dir_get_info(dir, e);
        const bool is_dir = info.attribute & ATTR_DIRECTORY;
        const char *ext = strrchr(e->d_name, '.');
        if (!is_dir && !(ext && (!strcasecmp(ext, ".m3u8")
                                 || !strcasecmp(ext, ".m3u"))))
            continue;
        h = mix(h, path, len);
        h = mix(h, e->d_name, strlen(e->d_name) + 1);
        h = mix(h, &info.size, sizeof(info.size));
        h = mix(h, &info.mtime, sizeof(info.mtime));
        if (is_dir && depth < 8
            && (size_t)snprintf(path + len, MAX_PATH - len, "/%s", e->d_name)
               < MAX_PATH - len)
        {
            h = catalog_print(path, strlen(path), depth + 1, h);
            path[len] = '\0';
        }
    }
    closedir(dir);
    return h;
}

/* What a cut left of the playlists' writes in the folder and those in it,
   put right (fw_recover()): each found first, as nothing is renamed in a
   folder while it's read. One that stays, as when a rename fails, ends the
   folder's rounds. */
static void recover_folder(char *path, size_t len, int depth)
{
    uint64_t last = 0;
    for (int round = 0; round < 64; round++)
    {
        DIR *dir = opendir(path);
        if (!dir)
            return;
        struct dirent *e;
        bool found = false;
        while (!found && (e = readdir(dir)))
        {
            found = playlist_leftover(e->d_name)
                    && !(dir_get_info(dir, e).attribute & ATTR_DIRECTORY)
                    && (size_t)snprintf(path + len, MAX_PATH - len, "/%s",
                                        e->d_name) < MAX_PATH - len;
        }
        closedir(dir);
        const uint64_t hash = found ? itunesdb_path_hash(path) : 0;
        if (!found || hash == last)
            break;
        last = hash;
        path[strlen(path) - 4] = '\0'; /* the playlist's own name */
        fw_recover(path);
        path[len] = '\0';
    }
    path[len] = '\0';
    DIR *dir = depth < 8 ? opendir(path) : NULL;
    if (!dir)
        return;
    struct dirent *e;
    while ((e = readdir(dir)))
    {
        if (e->d_name[0] != '.'
            && (dir_get_info(dir, e).attribute & ATTR_DIRECTORY)
            && (size_t)snprintf(path + len, MAX_PATH - len, "/%s", e->d_name)
               < MAX_PATH - len)
            recover_folder(path, strlen(path), depth + 1);
        path[len] = '\0';
    }
    closedir(dir);
}

/* playlist_leftover() of a name in the folder or those in it */
static bool catalog_leftover(char *path, size_t len, int depth)
{
    DIR *dir = opendir(path);
    if (!dir)
        return false;
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(dir)))
    {
        if (e->d_name[0] == '.')
            continue;
        if (!(dir_get_info(dir, e).attribute & ATTR_DIRECTORY))
            found = playlist_leftover(e->d_name);
        else if (depth < 8
                 && (size_t)snprintf(path + len, MAX_PATH - len, "/%s",
                                     e->d_name) < MAX_PATH - len)
            found = catalog_leftover(path, strlen(path), depth + 1);
        path[len] = '\0';
    }
    closedir(dir);
    return found;
}

void lp_files_boot(void)
{
    char path[MAX_PATH];
    if (catalog_directory_name(path, sizeof(path))
        && catalog_leftover(path, strlen(path), 0))
        recover_folder(path, strlen(path), 0);
}

static void mark_now(struct lp_mark *mark, char *path)
{
    mark->magic = LP_MARK_MAGIC;
    mark->version = LP_VERSION;
    mark->commitid = tagcache_get_commitid();
    mark->entries = tagcache_entry_count();
    mark->catalog = catalog_print(path, strlen(path), 0, 0xcbf29ce484222325ull);
}

static void remove_gone(void)
{
    DIR *dir = opendir(LP_DIR);
    if (!dir)
        return;
    char path[MAX_PATH];
    struct dirent *e;
    while ((e = readdir(dir)))
    {
        const char *ext = strrchr(e->d_name, '.');
        if (!ext || e->d_name[0] == '.')
            continue;
        bool keep = !strcasecmp(e->d_name, LP_MARK + sizeof(LP_DIR))
                    || !strcasecmp(e->d_name, LP_ROWS_FILE + sizeof(LP_DIR))
                    || !strcasecmp(e->d_name, LP_PLAYING + sizeof(LP_DIR))
                    || !strcasecmp(e->d_name, LP_PLAYING2 + sizeof(LP_DIR))
                    || !strcasecmp(e->d_name, LP_PLAYING_FROM + sizeof(LP_DIR));
        if (!strcasecmp(ext, ".idx") && ext - e->d_name == 16)
        {
            char high[9];
            strmemccpy(high, e->d_name, sizeof(high));
            const uint64_t hash = (uint64_t)strtoul(high, NULL, 16) << 32
                                  | strtoul(e->d_name + 8, NULL, 16);
            for (int i = 0; i < all.seen_count && !keep; i++)
                keep = all.seen[i] == hash;
        }
        if (!keep)
        {
            snprintf(path, sizeof(path), LP_DIR "/%s", e->d_name);
            remove(path);
        }
    }
    closedir(dir);
}

void lp_index_all(bool (*go_on)(void))
{
    size_t size;
    char path[MAX_PATH];
    /* not made here if there's none */
    if (!database_in_ram() || !catalog_directory_name(path, sizeof(path)))
        return;
    /* nothing changed since the last whole pass, nor the covers, no header
       is left to make, and no song is noted gone: one deleted on the iPod
       changes nothing the mark has */
    const bool covers = file_exists(LP_COVERS_TODO);
    const bool gone_noted = gone_in_block > 0 || file_size(LP_GONE) > 0
                            || file_exists(LP_GONE_TAKEN);
    const unsigned unknown_before = headers_unknown;
    struct lp_mark now, last;
    mark_now(&now, path);
    int fd = open(LP_MARK, O_RDONLY);
    if (fd >= 0)
    {
        const bool same = read(fd, &last, sizeof(last)) == sizeof(last)
                          && !memcmp(&last, &now, sizeof(now));
        close(fd);
        /* and, with Playlist Covers on, Playlists' rows made, for this
           database and its albums: those changed since, as by a smart
           playlist's head written again at boot, written first, the disk on
           now */
        rows_flush();
        if (same && !covers && !gone_noted
            && (!rows_wanted() || rows_file_current()))
            return;
    }
    char *buf = plugin_get_buffer(&size);
    const size_t read_size = 8192;
    const size_t rows_size = LP_ROWS_MAX * sizeof(struct lp_row);
    if (!buf || size < read_size + LP_SEEN_MAX * sizeof(uint64_t) + rows_size + 64)
        return;
    buf = (char *)ALIGN_UP((uintptr_t)buf, 8);
    size -= 8;
    /* the songs the database lost since the last pass, sorted, first: as
       many as half the buffer holds, the rest put back for the next pass */
    lp_gone_flush();
    gone_take();
    gone_set = (uint64_t *)buf;
    gone_count = 0;
    int gfd = open(LP_GONE_TAKEN, O_RDONLY);
    if (gfd >= 0)
    {
        const size_t most = (size / 2) / sizeof(uint64_t);
        const ssize_t got = read(gfd, gone_set, most * sizeof(uint64_t));
        close(gfd);
        if (got > 0)
            gone_count = got / sizeof(uint64_t);
        qsort(gone_set, gone_count, sizeof(uint64_t), cmp_hash64);
        buf += gone_count * sizeof(uint64_t);
        size -= gone_count * sizeof(uint64_t);
    }
    /* the notes the pass goes by: those after them are put back after it */
    const int gone_read = gone_count;
    if (size < read_size + LP_SEEN_MAX * sizeof(uint64_t) + rows_size + 64)
    {
        gone_set = NULL;
        gone_count = 0;
        gone_put_back(0);
        return;
    }
    all.seen = (uint64_t *)buf;
    all.seen_count = 0;
    all.overflow = all.stopped = false;
    all.covers = covers;
    all.go_on = go_on;
    /* the rows, made again: none meanwhile, nor changed by the heads this
       writes, until they're whole */
    pass.rows = (struct lp_row *)(all.seen + LP_SEEN_MAX);
    pass.most = LP_ROWS_MAX;
    pass.count = 0;
    pass.full = false;
    pass.unsynced = 0;
    memset(&pass.otg, 0, sizeof(pass.otg));
    rows_wait();
    remove(LP_ROWS_FILE);
    rows_held = true;
    rows_dirty = false;
    all.buf = (char *)(pass.rows + LP_ROWS_MAX);
    all.size = read_size;
    all.songs = (struct lp_song *)(all.buf + read_size);
    all.max = MIN((buf + size - (char *)all.songs) / sizeof(struct lp_song),
                  LP_MAX);
    cpu_boost(true);
    index_folder(path, strlen(path), 0, false);
    /* On-The-Go too, out of the catalogue's folder */
    if (file_exists(OTG_FILE))
    {
        struct lp_state state;
        int n = lp_index_read(OTG_FILE, NULL, 0, &state);
        if (n < 0 || index_has_gone(OTG_FILE))
        {
            index_make(OTG_FILE, all.songs, all.max, all.buf, all.size, false);
            n = lp_index_read(OTG_FILE, NULL, 0, &state);
        }
        else if (covers)
        {
            collage_again(OTG_FILE);
            n = lp_index_read(OTG_FILE, NULL, 0, &state);
        }
        /* in the head, not of the pass's room */
        row_made(&pass.otg, OTG_FILE, n, n >= 0 ? &state : NULL, 0);
        if (all.seen_count < LP_SEEN_MAX)
            all.seen[all.seen_count++] = itunesdb_path_hash(OTG_FILE);
    }
    gone_set = NULL;
    gone_count = 0;
    if (!all.overflow)
    {
        /* the songs a sync moved or the database lost, out of or moved in
           each playlist made on the iPod now */
        /* the import may need it again, until its state is written */
        if (!itunesdb_changed())
            remove(ITUNESDB_MOVED_LIST);
        gone_put_back(gone_read);
        if (headers_unknown == unknown_before)
            remove(LP_COVERS_TODO);
        remove_gone();
        /* as the pass left it, files it wrote again in their order too */
        mark_now(&now, path);
        whole_file_write(LP_MARK, &now, sizeof(now));
        pass_write();
    }
    else
    {
        /* cut short: every note back for the next pass, those of the
           playlists it went through too, whose lines are gone already */
        gone_put_back(0);
        /* over the pass's room, which leaves it to be done again at each
           boot, as before: Playlist Covers off, said */
        if (!all.stopped)
            health_note("playlists: more than %d playlists, a folder deeper "
                        "than 8 or a path too long: Playlist Covers off",
                        LP_SEEN_MAX);
    }
    rows_held = false;
    cpu_boost(false);
    /* in memory again, as the list shows them, music stopped */
    if (list_covers_taken() && global_settings.playlist_covers)
        lp_rows_load();
    else
        lp_rows_free();
}

/* The screen: its songs, their titles' places in the pool of them, and the
   pool, in the plugin buffer */
static struct {
    char file[MAX_PATH];    /* its own: the catalogue's is written over by
                               Add to Playlist meanwhile */
    struct lp_song *songs;
    uint32_t *names;
    char *pool;
    int count;              /* those shown */
    int total;              /* those it has, all written to its file */
    struct lp_state state;
    bool library;           /* the import's, else made on the iPod */
    bool otg;               /* On-The-Go */
    size_t pool_used;       /* of the pool, by the titles: the rest is free */
    size_t pool_size;
    bool from_this;         /* the music is from this playlist */
    bool waited;            /* Loading was shown as it opened */
    bool at_playing;        /* opened at the song playing */
    bool unloaded;          /* it couldn't be read as it was opened */
    bool covers;            /* its rows in two lines with covers */
    unsigned int gen;       /* of the plugin buffer, which a plugin takes */
    /* the rows of its header at the top (album_header.c), which none
       selects, 0 for none; its name and the list's it came from, which the
       title has while the header shows */
    int header;
    const char *name, *above;
    bool name_shown;
    /* their texts, and the file of the song whose menu is open: here rather
       than on main's stack, under the screens nested over the list, as the
       lists Go to Artist opens from a song's menu */
    char title_buf[MAX_PATH], above_buf[MAX_PATH], menu_file[MAX_PATH];
} lp;

static bool lp_load(void)
{
    size_t size;
    char *buf = plugin_get_buffer(&size);
    lp.gen = plugin_buffer_gen();
    lp.count = lp.total = 0;
    if (!buf || size < 4096)
        return false;
    /* a third for the songs and their titles' places, the rest for titles */
    buf = (char *)ALIGN_UP((uintptr_t)buf, 4);
    size -= 4;
    const int max = MIN((size / 3) / (sizeof(struct lp_song) + sizeof(uint32_t)),
                        LP_MAX);
    lp.songs = (struct lp_song *)buf;
    lp.names = (uint32_t *)(lp.songs + max);
    lp.pool = (char *)(lp.names + max);
    const size_t pool_size = buf + size - lp.pool;

    int n = lp_index_read(lp.file, lp.songs, max, &lp.state);
    if (n < 0)
    {
        /* none yet, as after Rockbox's own update of the database: made now,
           its file written as it plays */
        splash(0, ID2P(LANG_WAIT));
        lp.waited = true;
        n = index_make(lp.file, lp.songs, max, lp.pool, pool_size, false);
        if (n < 0)
            return false;
        n = lp_index_read(lp.file, lp.songs, max, &lp.state);
        if (n < 0)
            return false;
    }
    /* a song deleted on the iPod since the index was made (Delete of a
       podcast's episode): its line in the file then as one the database
       hasn't, until the next pass drops it (index_folder(), or for
       On-The-Go lp_index_all() itself); without its length in the header */
    int kept = 0;
    for (int i = 0; i < n; i++)
    {
        if (!tagcache_entry_deleted(lp.songs[i].idx))
            lp.songs[kept++] = lp.songs[i];
    }
    if (kept < n)
    {
        lp.state.unmatched += n - kept;
        n = kept;
        if (lp.state.seconds != LP_HEADER_UNKNOWN)
        {
            lp.state.seconds = 0;
            for (int i = 0; i < n; i++)
                lp.state.seconds += song_seconds(lp.songs[i].idx);
        }
    }
    lp.count = lp.total = n;

    /* the titles, or for one without a title its file's name */
    struct tagcache_search tcs;
    size_t used = 0;
    const bool search = tagcache_search(&tcs, tag_title);
    for (int i = 0; i < n; i++)
    {
        const size_t room = pool_size - used;
        if (room < 2)
        {
            lp.count = i;
            break;
        }
        lp.names[i] = used;
        char *name = lp.pool + used;
        name[0] = '\0';
        if (search
            && (!tagcache_retrieve(&tcs, lp.songs[i].idx, tag_title, name, room)
                || !name[0] || !strcmp(name, UNTAGGED)))
        {
            char path[MAX_PATH];
            name[0] = '\0';
            if (tagcache_retrieve(&tcs, lp.songs[i].idx, tag_filename,
                                  path, sizeof(path)))
            {
                const char *base = strrchr(path, '/');
                strmemccpy(name, base ? base + 1 : path, room);
                char *ext = strrchr(name, '.');
                if (ext && ext != name)
                    *ext = '\0';
            }
        }
        used += strlen(name) + 1;
    }
    if (search)
        tagcache_search_finish(&tcs);
    lp.pool_used = used;
    lp.pool_size = pool_size;
    return true;
}

/* The copy of a playlist made on the iPod to play from: each line of a song
   as its path, whole, in the file's order, those not in the database too, so
   that the songs' lines in the index are theirs in the copy. Through the
   pool's free part. Which one it is, or NULL. */
static const char *make_copy(const char *file)
{
    char name[MAX_PATH], dir[MAX_PATH];
    const char *copy = playlist_get_name(NULL, name, sizeof(name))
                       && lp_same_file(name, LP_PLAYING) ? LP_PLAYING2
                                                        : LP_PLAYING;
    const size_t room = lp.pool_size - lp.pool_used;
    if (room < 4 * MAX_PATH)
        return NULL;
    const size_t in_size = room / 2;
    char *in = lp.pool + lp.pool_used;
    char *out = in + in_size;
    const size_t out_size = room - in_size;
    const bool m3u = playlist_dir(file, dir, sizeof(dir));
    const int fd = m3u ? open(file, O_RDONLY) : open_utf8(file, O_RDONLY);
    if (fd < 0)
        return NULL;
    mkdir(LP_DIR);
    /* made again as it's played (FW_CACHE); each write as the program's own,
       through out */
    struct fw w;
    bool ok = fw_open(&w, copy, FW_CACHE, NULL, 0);
    size_t put = 0;
    struct line_reader lines;
    line_reader_init(&lines, fd, in, in_size);
    while (ok && line_read(&lines))
    {
        if (lines.failed)
        {
            ok = false;
            break;
        }
        char *line;
        size_t len;
        while ((line = line_next(&lines, &len)))
        {
            /* the lines of songs, as lp_index_build() counts them */
            if (len && line[0] != '#')
            {
                char resolved[MAX_PATH];
                const char *song = song_path(line, dir, m3u, resolved,
                                             sizeof(resolved));
                if (!song)
                    song = line;
                const size_t slen = strlen(song);
                if (put + slen + 1 > out_size)
                {
                    ok = ok && fw_put(&w, out, put);
                    put = 0;
                }
                if (slen + 1 <= out_size)
                {
                    memcpy(out + put, song, slen);
                    out[put + slen] = '\n';
                    put += slen + 1;
                }
            }
        }
        yield();
    }
    close(fd);
    if (ok && put)
        ok = fw_put(&w, out, put);
    return fw_close(&w, ok) ? copy : NULL;
}

static void note_copy_source(const char *file);

void lp_source_moved(const char *from, const char *to)
{
    const char *source = playing_copy_source();
    /* as the source is written, tidy: the browser's on an iPod start with
       its drive */
    char was[MAX_PATH], now[MAX_PATH];
    tidy_copy(was, sizeof(was), from);
    const size_t len = strlen(was);
    if (!source[0] || strncasecmp(source, was, len)
        || (source[len] && source[len] != '/'))
        return;
    char moved[MAX_PATH];
    if (to)
    {
        tidy_copy(now, sizeof(now), to);
        snprintf(moved, sizeof(moved), "%s%s", now, source + len);
    }
    else
        moved[0] = '\0';
    note_copy_source(moved);
}

/* the copy playing is of this file now */
static void note_copy_source(const char *file)
{
    strmemccpy(copy_source, file, sizeof(copy_source));
    copy_source_read = true;
    const int fd = whole_file_open(LP_PLAYING_FROM);
    if (fd < 0)
        return;
    const ssize_t len = strlen(copy_source);
    whole_file_close(fd, LP_PLAYING_FROM,
                     write(fd, copy_source, len) == len
                     && write(fd, "\n", 1) == 1);
}

/* The rows: its header's, the playlist's songs, none for one without songs;
   and On-The-Go's last, as on a stock iPod: Save Playlist, Clear Playlist.
   A song's row is the header's rows on from its place in the songs */
static int song_of(int row)
{
    return row - lp.header;
}

static bool is_save_row(int row)
{
    return lp.otg && lp.count > 0 && song_of(row) == lp.count;
}

static bool is_clear_row(int row)
{
    return lp.otg && lp.count > 0 && song_of(row) == lp.count + 1;
}

static bool is_song_row(int row)
{
    return song_of(row) >= 0 && song_of(row) < lp.count;
}

static bool is_header_row(int row, void *data)
{
    (void)data;
    return row < lp.header;
}

static int lp_rows(void)
{
    return lp.count > 0 ? lp.header + lp.count + (lp.otg ? 2 : 0) : 0;
}

/* the row of a song, or of Save Playlist after them: of the songs' rows, the
   nearest */
static int row_of_song(int song)
{
    return lp.header + MAX(MIN(song, lp_rows() - lp.header - 1), 0);
}

static const char *lp_name(int row, void *data, char *buf, size_t size)
{
    (void)data;
    (void)buf;
    (void)size;
    if (is_save_row(row))
        return str(LANG_SAVE_PLAYLIST);
    if (is_clear_row(row))
        return str(LANG_ON_THE_GO_CLEAR);
    /* the header's, drawn whole rather than by rows */
    if (!is_song_row(row))
        return "";
    return lp.pool + lp.names[song_of(row)];
}

/* Quick Scroll's letter of a song's row, in a playlist sorted by a column:
   of the place of its name for the column's tag, as its songs go by them */
static const char *lp_initial(int row, void *data, char *buf, size_t size)
{
    (void)data;
    int tag;
    switch (lp.state.sort)
    {
        case ITUNESDB_SORT_TITLE:    tag = tag_title; break;
        case ITUNESDB_SORT_ALBUM:    tag = tag_album; break;
        case ITUNESDB_SORT_ARTIST:   tag = tag_artist; break;
        case ITUNESDB_SORT_GENRE:    tag = tag_genre; break;
        case ITUNESDB_SORT_COMPOSER: tag = tag_composer; break;
        default:                     return NULL; /* as the file has them */
    }
    if (!is_song_row(row) || size < 5)
        return NULL;
    const ucschar_t letter =
        tagcache_initial(tag, tagcache_sort_key(lp.songs[song_of(row)].idx, tag));
    if (!letter)
        return NULL;
    *utf8encode(letter, (unsigned char *)buf) = '\0';
    return buf;
}

static bool lp_playing(int row, void *data)
{
    (void)data;
    return is_song_row(row) && lp.from_this
           && gui_synclist_playing() == (long)lp.songs[song_of(row)].idx + 1;
}

/* With Covers in Lists, a song's row as Songs has it: its artist under its
   title, and its album's cover at its left, from memory as it's drawn */
static const char *lp_subline(int row, void *data, char *buf, size_t size)
{
    (void)data;
    return is_song_row(row) ? tagtree_song_subline(lp.songs[song_of(row)].idx,
                                                   buf, size)
                            : NULL;
}

static int lp_cover(int row, void *data, const fb_data **pixels)
{
    (void)data;
    return is_song_row(row) ?
           list_covers_get(tagtree_song_album(lp.songs[song_of(row)].idx), pixels)
           : LIST_COVER_NONE;
}

/* Before each frame: the title the list's it came from while the header
   shows, the playlist's once it has scrolled away, as an album's (tree.c).
   And with covers, as Songs' (tree.c): what it lacks counted anew, its
   rows' covers read now if they can be at once, and the covers of the rows
   next to those shown asked for after theirs, the way the list went first,
   as far again as it went, at least a screen */
static void lp_before_draw(struct gui_synclist *list)
{
    const bool name = list->start_item[SCREEN_MAIN] >= lp.header;
    if (name != lp.name_shown)
    {
        lp.name_shown = name;
        gui_synclist_set_title(list, name ? lp.name : lp.above,
                               list->title_icon);
    }
    if (!lp.covers)
        return;
    static int last_start;
    list_covers_frame();
    list_covers_draw_soon(list);
    const int start = list->start_item[SCREEN_MAIN];
    const int shown = list_get_nb_lines(list, SCREEN_MAIN);
    const bool down = start >= last_start;
    const int far = MIN(shown + 2 * abs(start - last_start), 32);
    last_start = start;
    for (int k = 0; k < far + shown; k++)
    {
        const bool forward = k < far ? down : !down;
        const int d = k < far ? k : k - far;
        const int row = forward ? start + shown + d : start - 1 - d;
        if (!is_song_row(row))
            continue;
        const int album = tagtree_song_album(lp.songs[song_of(row)].idx);
        if (album >= 0)
            list_covers_ahead(album);
    }
}

/* the music from this song on, from the playlist's file, written first if
   it's behind the list. False if it didn't start. Its buffers, as those of
   the other rows' work below, kept out of lp_view()'s frame, which the lists
   opened from a song's menu are nested over */
static bool NO_INLINE lp_play(int song)
{
    /* the song chosen is one sold with a lock on it: said, nothing changes */
    char path[MAX_PATH];
    struct tagcache_search tcs;
    if (tagcache_search(&tcs, tag_filename))
    {
        const bool locked = tagcache_retrieve(&tcs, lp.songs[song].idx, tag_filename,
                                              path, sizeof(path))
                            && track_protected_splash(path);
        tagcache_search_finish(&tcs);
        if (locked)
            return false;
    }
    if (global_settings.party_mode || !warn_on_pl_erase())
        return false;
    /* Now Playing comes later than the slide's usual wait, as from Music */
    transition_wait(HZ);
    if (lp.state.pending || lp.songs[song].line == LP_NO_LINE)
    {
        /* the music from it plays on meanwhile: its file, open, is kept
           until it closes */
        splash(0, ID2P(LANG_WAIT));
        if (!lp_write_file(lp.file, lp.songs, lp.total, &lp.state))
            return false;
        lp_index_write(lp.file, lp.songs, lp.total, &lp.state);
        if (lp.songs[song].line == LP_NO_LINE)
            return false;
    }
    char dir[MAX_PATH];
    const char *copy = NULL;
    if (!lp.library && !(copy = make_copy(lp.file)))
        return false;
    strmemccpy(dir, copy ? copy : lp.file, sizeof(dir));
    char *name = strrchr(dir, '/');
    if (!name)
        return false;
    *name++ = '\0';
    if (playlist_create(dir[0] ? dir : "/", name) < 0)
        return false;
    if (copy)
        note_copy_source(lp.file);
    /* not past its end, as with fewer files allowed in a playlist */
    const int amount = playlist_amount();
    if (amount <= 0)
        return false;
    int start = MIN((int)lp.songs[song].line, amount - 1);
    if (global_settings.playlist_shuffle)
    {
        start = playlist_shuffle(0, start);
        if (!global_settings.play_selected)
            start = 0;
    }
    playlist_start(start, 0, 0);
    return true;
}

/* the song out of the music playing too, if it plays from this playlist:
   its first time after the one playing, else another than that; never a
   copy queued by Play Next or Play Last, which isn't the playlist's */
static void remove_from_queue(const char *path)
{
    const int amount = playlist_amount();
    const int playing = playlist_get_current()->index;
    int found = -1;
    struct playlist_track_info info;
    for (int k = 1; k < amount && found < 0; k++)
    {
        const int i = (playing + k) % amount;
        if (playlist_get_track_info(NULL, i, &info) >= 0
            && !(info.attr & PLAYLIST_ATTR_QUEUED)
            && lp_same_file(info.filename, path))
            found = i;
    }
    if (found >= 0)
    {
        playlist_delete(NULL, found);
        playlist_sync(NULL);
    }
}

/* The music playing straight from this playlist's file moved to a copy of
   it, at the same song and place, before the file changes under it: it
   keeps its songs by where their lines are. */
static void play_from_copy_instead(void)
{
    if (!playing_straight_from(lp.file))
        return;
    const struct mp3entry *id3 = audio_current_track();
    if (!id3 || !id3->path[0])
        return;
    char playing[MAX_PATH], path[MAX_PATH];
    strmemccpy(playing, id3->path, sizeof(playing));
    const unsigned long elapsed = id3->elapsed, offset = id3->offset;
    const bool paused = audio_status() & AUDIO_STATUS_PAUSE;
    int start = -1;
    struct tagcache_search tcs;
    if (tagcache_search(&tcs, tag_filename))
    {
        for (int i = 0; i < lp.total && start < 0; i++)
        {
            if (lp.songs[i].line != LP_NO_LINE
                && tagcache_retrieve(&tcs, lp.songs[i].idx, tag_filename,
                                     path, sizeof(path))
                && lp_same_file(path, playing))
                start = lp.songs[i].line;
        }
        tagcache_search_finish(&tcs);
    }
    const char *copy = start >= 0 ? make_copy(lp.file) : NULL;
    if (!copy)
        return;
    strmemccpy(path, copy, sizeof(path));
    char *name = strrchr(path, '/');
    *name++ = '\0';
    if (playlist_create(path, name) < 0)
        return;
    note_copy_source(lp.file);
    if (global_settings.playlist_shuffle)
        start = playlist_shuffle(current_tick, start);
    playlist_start(start, elapsed, offset);
    if (paused)
        audio_pause();
}

/* A song out of a playlist made on the iPod, as its line goes from the file:
   the file written again a block at a time, its other lines as they were;
   an .m3u's as an .m3u8, in UTF-8, as anything written to it would be.
   On-The-Go with none left is no file, as when it was cleared. The list and
   the index without it, not made again. */
static bool lp_remove(int song)
{
    play_from_copy_instead();
    /* no copy to play from instead: the file stays as the music has it */
    if (playing_straight_from(lp.file))
        return false;
    const uint32_t line_no = lp.songs[song].line;
    char dir[MAX_PATH], out_path[MAX_PATH], song_file[MAX_PATH];
    const bool m3u = playlist_dir(lp.file, dir, sizeof(dir));
    strmemccpy(out_path, lp.file, sizeof(out_path));
    bool convert = false;
    if (m3u)
    {
        char *ext = strrchr(out_path, '.');
        snprintf(ext, sizeof(out_path) - (ext - out_path), ".m3u8");
        convert = !file_exists(out_path) && !dir_exists(out_path);
        if (!convert)
            strmemccpy(out_path, lp.file, sizeof(out_path));
    }
    song_file[0] = '\0';
    struct tagcache_search tcs;
    if (tagcache_search(&tcs, tag_filename))
    {
        tagcache_retrieve(&tcs, lp.songs[song].idx, tag_filename, song_file,
                          sizeof(song_file));
        tagcache_search_finish(&tcs);
    }

    const size_t room = lp.pool_size - lp.pool_used;
    if (room < 4 * MAX_PATH)
        return false;
    const size_t in_size = room / 2, out_size = room - in_size;
    char *in = lp.pool + lp.pool_used, *out = in + in_size;
    const int fd = m3u ? open(lp.file, O_RDONLY) : open_utf8(lp.file, O_RDONLY);
    if (fd < 0)
        return false;
    /* each write as the program's own, through out */
    struct fw w;
    bool ok = fw_open(&w, out_path, FW_KEEP, NULL, 0);
    size_t put = 0;
    uint32_t songs_seen = 0, songs_kept = 0;
    struct line_reader lines;
    line_reader_init(&lines, fd, in, in_size);
    while (ok && line_read(&lines))
    {
        if (lines.failed)
        {
            ok = false;
            break;
        }
        char *line;
        size_t len;
        while ((line = line_next(&lines, &len)))
        {
            const bool is_song = len && line[0] != '#';
            const bool gone = is_song && songs_seen++ == line_no;
            if (!gone)
            {
                char decoded[MAX_PATH];
                const char *text = line;
                if (convert)
                {
                    *iso_decode_ex((const unsigned char *)line,
                                   (unsigned char *)decoded, -1, len,
                                   sizeof(decoded) - 1) = '\0';
                    text = decoded;
                }
                const size_t tlen = strlen(text);
                if (put + tlen + 1 > out_size)
                {
                    ok = ok && fw_put(&w, out, put);
                    put = 0;
                }
                if (tlen + 1 <= out_size)
                {
                    memcpy(out + put, text, tlen);
                    out[put + tlen] = '\n';
                    put += tlen + 1;
                }
                songs_kept += is_song;
            }
        }
        yield();
    }
    close(fd);
    if (ok && put)
        ok = fw_put(&w, out, put);
    ok = fw_end(&w) && ok;
    if (ok && lp.otg && songs_kept == 0)
    {
        fw_place(out_path, FW_KEEP, false);
        remove(lp.file);
        lp_index_forget(lp.file);
        lp.count = lp.total = 0;
        if (song_file[0] && playing_from(lp.file))
            remove_from_queue(song_file);
        return true;
    }
    if (!fw_place(out_path, FW_KEEP, ok))
        return false;
    const bool from_this = playing_from(lp.file);
    if (convert)
    {
        remove(lp.file);
        lp_index_forget(lp.file);
        lp_source_moved(lp.file, out_path);
        strmemccpy(lp.file, out_path, sizeof(lp.file));
    }

    /* the list as the file is now: its songs after it a line earlier */
    memmove(&lp.songs[song], &lp.songs[song + 1],
            (lp.total - song - 1) * sizeof(*lp.songs));
    if (song < lp.count)
        memmove(&lp.names[song], &lp.names[song + 1],
                (lp.count - song - 1) * sizeof(*lp.names));
    lp.total--;
    if (song < lp.count)
        lp.count--;
    for (int i = 0; i < lp.total; i++)
    {
        if (lp.songs[i].line != LP_NO_LINE && lp.songs[i].line > line_no)
            lp.songs[i].line--;
    }
    lp.state.lines--;
    lp_index_write(lp.file, lp.songs, lp.total, &lp.state);
    if (from_this && song_file[0])
        remove_from_queue(song_file);
    return true;
}

/* On-The-Go as a playlist of its own on the iPod, as on a stock one: "On-The-Go
   1", the first number free, in the catalogue, which opens at it; On-The-Go
   empty again */
static bool NO_INLINE lp_save_otg(void)
{
    char root[MAX_PATH], saved[MAX_PATH];
    if (!catalog_directory_name(root, sizeof(root)))
        return false;
    for (int n = 1; n < 1000; n++)
    {
        snprintf(saved, sizeof(saved), "%s/%s %d.m3u8", root, str(LANG_ON_THE_GO), n);
        char folder[MAX_PATH];
        snprintf(folder, sizeof(folder), "%s/%s %d", root, str(LANG_ON_THE_GO), n);
        if (file_exists(saved) || dir_exists(folder))
            continue;
        if (rename(OTG_FILE, saved) < 0)
            return false;
        lp_index_moved(OTG_FILE, saved);
        lp_source_moved(OTG_FILE, saved);
        catalog_select_playlist(saved);
        return true;
    }
    return false;
}

/* The list it was opened from, as its title has it (tree.c): Playlists for
   On-The-Go and for one at the catalogue's top synced from the computer,
   Unsynced for one of the iPod's own there, else its folder's name. Into
   buf if it's that */
static const char * NO_INLINE title_above(char *buf, size_t size)
{
    char root[MAX_PATH];
    const char *slash = strrchr(lp.file, '/');
    if (lp.otg || !slash || !catalog_directory_name(root, sizeof(root)))
        return str(LANG_PLAYLISTS);
    tidy_copy(buf, size, root);
    size_t root_len = strlen(buf);
    while (root_len > 1 && buf[root_len - 1] == '/')
        root_len--;
    const size_t dir_len = slash - lp.file;
    if (dir_len == root_len && !strncasecmp(lp.file, buf, dir_len))
        return str(ip_is_synced(lp.file) ? LANG_PLAYLISTS : LANG_UNSYNCED);
    const char *start = slash;
    while (start > lp.file && start[-1] != '/')
        start--;
    if (start == slash)
        return str(LANG_PLAYLISTS);
    strmemccpy(buf, start, MIN(size, (size_t)(slash - start) + 1));
    return buf;
}

/* Its header over the songs, as an album's: its name, its songs and their
   length, and its collage, as its index has them, nothing worked out now;
   again after another screen, which may have shown an album's, and after a
   change to its songs. Its title as the header shows or not */
static void lp_header_open(struct gui_synclist *list)
{
    /* its length left out until it's made */
    const unsigned long seconds =
        lp.state.seconds == LP_HEADER_UNKNOWN ? 0 : lp.state.seconds;
    lp.header = album_header_open_playlist(list, lp.name,
                                           lp.count > 0 ? lp.total : 0,
                                           seconds, lp.state.collage,
                                           lp_rows() - lp.header);
    gui_synclist_set_block(list, lp.header, lp.header ? album_header_draw : NULL);
    list->callback_is_group = lp.header ? is_header_row : NULL;
    lp.name_shown = list->start_item[SCREEN_MAIN] >= lp.header;
    gui_synclist_set_title(list, lp.name_shown ? lp.name : lp.above,
                           Icon_Playlist);
}

/* the song's file, through a search whose 728 bytes are gone by the menu */
static bool NO_INLINE lp_song_file(int song, char *path, size_t size)
{
    struct tagcache_search tcs;
    if (!tagcache_search(&tcs, tag_filename))
        return false;
    const bool found = tagcache_retrieve(&tcs, lp.songs[song].idx, tag_filename,
                                         path, size);
    tagcache_search_finish(&tcs);
    return found;
}

/* a held Select: the menu of the song, as in Songs */
static int NO_INLINE lp_song_menu(int song)
{
    char *path = lp.menu_file;
    if (!lp_song_file(song, path, sizeof(lp.menu_file)))
        return -1;
    /* the menu's own copy: what the menu opens may take the plugin buffer */
    onplay_set_title(lp.pool + lp.names[song]);
    /* out of one made on the iPod */
    onplay_set_removable(!lp.library);
    const long size_before = file_size(lp.file);
    const int result = onplay(path, FILE_ATTR_AUDIO, CONTEXT_STD, false,
                              ONPLAY_NO_CUSTOMACTION);
    /* songs added to this one from it, as by Add to Playlist */
    if (result != ONPLAY_REMOVE && file_size(lp.file) != size_before)
        lp_load();
    switch (result)
    {
        case ONPLAY_REMOVE:
            lp_remove(song);
            return -1;
        case ONPLAY_START_PLAY:
            return PLAYLIST_VIEWER_OK;
        case ONPLAY_MAINMENU:
            return PLAYLIST_VIEWER_MAINMENU;
        default:
            return -1;
    }
}

int lp_view(const char *file, int *selection)
{
    tidy_copy(lp.file, sizeof(lp.file), file);
    lp.library = itunesdb_own_playlist(lp.file);
    lp.otg = lp_same_file(lp.file, OTG_FILE);
    /* songs have the whole width, as the lists of Music from Artists on, and
       Loading before them is in the middle of it, not of the menu's part */
    const bool was_wide = menu_wide();
    if (!was_wide)
        menu_set_wide(true);
    /* a live smart one as ratings and plays are now, before it shows */
    lp.waited = smart_prepare(lp.file);
    lp.unloaded = !lp_load();
    /* not opened if it can't be read, nor On-The-Go without a song to show,
       as when its songs were deleted on the iPod since (lp_load() leaves
       them out): one emptied here closes itself (ACTION_STD_CONTEXT below),
       and an empty one doesn't open (tree.c) */
    if (lp.unloaded || (lp.otg && lp.count <= 0))
    {
        if (!was_wide)
            menu_set_wide(false);
        return PLAYLIST_VIEWER_CANCEL;
    }

    /* its name, without .m3u8 */
    char *title = lp.title_buf;
    const size_t title_size = sizeof(lp.title_buf);
    const char *base = strrchr(lp.file, '/');
    strmemccpy(title, base ? base + 1 : lp.file, title_size);
    char *ext = strrchr(title, '.');
    if (ext && ext != title)
        *ext = '\0';
    if (lp.otg)
        strmemccpy(title, str(LANG_ON_THE_GO), title_size);
    lp.name = title;
    lp.above = title_above(lp.above_buf, sizeof(lp.above_buf));
    lp.header = 0;

    push_current_activity(ACTIVITY_PLAYLISTVIEWER);
    /* from the screen before, not from Loading; nor over a slide begun, as
       back from Now Playing */
    if (!lp.waited && !transition_pending())
        transition_begin(TRANSITION_FORWARD);

    struct gui_synclist list;
    gui_synclist_init(&list, lp_name, NULL, false, 1, NULL);
    gui_synclist_set_title(&list, title, Icon_Playlist);
    gui_synclist_set_icon_callback(&list, NULL);
    gui_synclist_set_playing_callback(&list, lp_playing);
    gui_synclist_set_initial_callback(&list, lp_initial);
    /* the covers' memory, if turned on as music played, which has stopped;
       one line without them, as Songs has then. On-The-Go's Save Playlist
       and Clear Playlist have neither, their names in the middle of a row as
       high */
    list_covers_apply();
    lp.covers = list_covers_on();
    if (lp.covers)
    {
        gui_synclist_set_subline_callback(&list, lp_subline);
        gui_synclist_set_cover_callback(&list, lp_cover);
        list_covers_opened();
    }
    /* after the rows' height, which its rows are counted by */
    gui_synclist_set_before_draw(&list, lp_before_draw);
    lp_header_open(&list);
    gui_synclist_set_nb_items(&list, lp_rows());
    /* at the song it was left at, or a new one at its first song */
    int sel = selection && *selection >= 0 ? *selection : 0;
    if (lp.at_playing)
    {
        const long playing = gui_synclist_song_playing();
        for (int i = 0; i < lp.count; i++)
        {
            if ((long)lp.songs[i].idx + 1 == playing)
            {
                sel = i;
                break;
            }
        }
    }
    gui_synclist_select_item(&list, row_of_song(sel));

    int ret = -1;
    bool redraw = true;
    while (ret < 0)
    {
        /* the plugin buffer taken meanwhile, as by a dock: read again */
        if (plugin_buffer_gen() != lp.gen)
        {
            const int song = song_of(gui_synclist_get_sel_pos(&list));
            if (!lp_load())
            {
                ret = PLAYLIST_VIEWER_CANCEL;
                break;
            }
            lp_header_open(&list);
            gui_synclist_set_nb_items(&list, lp_rows());
            gui_synclist_select_item(&list, row_of_song(song));
            redraw = true;
        }
        if (redraw)
        {
            lp.from_this = playing_from(lp.file);
            gui_synclist_draw(&list);
        }
        redraw = false;
        /* as list_do_action(), and the covers the reader reads as the wheel
           rests drawn as they come, as in tree.c; the header's collage too,
           looked for each tenth of a second while it's read */
        const bool collage = lp.header && album_header_cover_reading();
        int timeout = list_do_action_timeout(&list, collage ? HZ / 10 : HZ / 2);
        if (lp.covers)
            timeout = list_covers_timeout(timeout);
        keyclick_set_callback(gui_synclist_keyclick_callback, &list);
        int action = get_action(CONTEXT_TREE | ALLOW_SOFTLOCK, timeout);
        if (lp.covers && action != ACTION_NONE)
            list_covers_input();
        if (lp.covers && list_covers_poll())
            redraw = true;
        if (lp.header && album_header_cover_came())
            redraw = true;
        if (gui_synclist_do_button(&list, &action))
            continue;
        const int row = gui_synclist_get_sel_pos(&list);
        switch (action)
        {
            case ACTION_STD_OK:
                if (lp.count <= 0 || row < lp.header)
                    break;
                if (is_save_row(row))
                {
                    if (lp_save_otg())
                    {
                        transition_begin(TRANSITION_BACK);
                        ret = PLAYLIST_VIEWER_CANCEL;
                    }
                    break;
                }
                if (is_clear_row(row))
                {
                    /* as Reset Settings asks, Cancel first */
                    static const struct opt_items items[] = {
                        { STR(LANG_ON_THE_GO_CLEAR) }, { STR(LANG_CANCEL_0) },
                    };
                    int choice = 1;
                    if (set_option(str(LANG_ON_THE_GO_CLEAR), &choice, RB_INT,
                                   items, ARRAYLEN(items), NULL))
                    {
                        ret = PLAYLIST_VIEWER_USB;
                        break;
                    }
                    if (choice == 0)
                    {
                        remove(OTG_FILE);
                        lp_index_forget(OTG_FILE);
                        transition_begin(TRANSITION_BACK);
                        ret = PLAYLIST_VIEWER_CANCEL;
                    }
                    redraw = true;
                    break;
                }
                transition_begin(TRANSITION_FORWARD);
                /* the song playing, of this list: to it, paused or not */
                lp.from_this = playing_from(lp.file);
                if (lp.from_this && gui_synclist_song_playing()
                                    == (long)lp.songs[song_of(row)].idx + 1)
                {
                    ret = PLAYLIST_VIEWER_OK;
                    break;
                }
                if (lp_play(song_of(row)))
                    ret = PLAYLIST_VIEWER_OK;
                else
                    transition_cancel();
                redraw = true;
                break;
            case ACTION_STD_CONTEXT:
            {
                if (lp.count <= 0 || !is_song_row(row))
                    break;
                const int song = song_of(row);
                /* On-The-Go's song out at once, as on a stock iPod */
                if (lp.otg)
                    lp_remove(song);
                else
                    ret = lp_song_menu(song);
                /* a song fewer, perhaps; none left of On-The-Go, back */
                if (lp.otg && lp.count <= 0)
                {
                    transition_begin(TRANSITION_BACK);
                    ret = PLAYLIST_VIEWER_CANCEL;
                    break;
                }
                /* on the song after it still, under the header as it is now */
                lp_header_open(&list);
                gui_synclist_set_nb_items(&list, lp_rows());
                gui_synclist_select_item(&list, row_of_song(song));
                redraw = true;
                break;
            }
            case ACTION_STD_CANCEL:
                transition_begin(TRANSITION_BACK);
                ret = PLAYLIST_VIEWER_CANCEL;
                break;
            case ACTION_TREE_WPS:
                if (audio_status())
                    ret = PLAYLIST_VIEWER_OK;
                break;
            case ACTION_TREE_STOP:
                /* holding Play goes to Now Playing, if it has a song: stopped,
                   it shows it paused and starts nothing (wpsscrn()) */
                if (now_playing_available())
                    ret = PLAYLIST_VIEWER_OK;
                break;
            case ACTION_STD_MENU:
                ret = PLAYLIST_VIEWER_MAINMENU;
                break;
            default:
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                    ret = PLAYLIST_VIEWER_USB;
                break;
        }
    }
    if (selection)
        *selection = MAX(song_of(gui_synclist_get_sel_pos(&list)), 0);
    lp.header = 0;
    pop_current_activity_without_refresh();
    if (!was_wide)
        menu_set_wide(false);
    return ret;
}

/* The file of the playlist the music playing is from, a playlist's of
   Playlists, On-The-Go or another .m3u: not a list of Music's, whose files
   tagtree.c has, nor music started without one; an iPod-made one's own, not
   its copy playing. By the name the music has, and none gone since, as by a
   sync or Delete: the copy's source, read once, and the file, found in the
   directory cache */
static bool playing_playlist(char *name, size_t size)
{
    if (!playlist_get_name(NULL, name, size) || tagtree_is_list_file(name))
        return false;
    const char *ext = strrchr(name, '.');
    if (!ext || (strcasecmp(ext, ".m3u8") && strcasecmp(ext, ".m3u")))
        return false;
    if (is_copy(name))
        strmemccpy(name, playing_copy_source(), size);
    return name[0] && lp_viewable(name) && file_exists(name);
}

bool lp_playing_from_known(void)
{
    char name[MAX_PATH];
    return playing_playlist(name, sizeof(name));
}

int lp_view_playing(void)
{
    char name[MAX_PATH];
    if (!playing_playlist(name, sizeof(name)))
        return -1;
    lp.at_playing = true;
    const int ret = lp_view(name, NULL);
    lp.at_playing = false;
    /* not to be read after all: the songs to come instead, rather than
       nothing */
    if (lp.unloaded)
        return playlist_viewer_ex(NULL, NULL);
    return ret;
}
