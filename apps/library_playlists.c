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
#define LP_VERSION 3

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
};

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
        && head.magic == LP_MAGIC && head.version == LP_VERSION
        && head.commitid == (uint32_t)tagcache_get_commitid()
        && head.entries == (uint32_t)tagcache_entry_count()
        && (long)head.size == file_size(file) && head.count <= LP_MAX)
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
    char path[MAX_PATH], tmp[MAX_PATH + 4];
    index_path(itunesdb_path_hash(file), path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    const long size = file_size(file);
    if (size < 0)
        return false;
    mkdir(LP_DIR);
    errno = 0;
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
    {
        library_write_failed();
        return false;
    }
    const struct lp_head head = {
        LP_MAGIC, LP_VERSION, tagcache_get_commitid(), tagcache_entry_count(),
        size, count, state->lines, state->unmatched, state->pending,
        state->sort
    };
    const ssize_t bytes = count * sizeof(*songs);
    bool ok = write(fd, &head, sizeof(head)) == sizeof(head)
              && write(fd, songs, bytes) == bytes;
    if (close(fd) < 0)
        ok = false;
    if (!ok || rename(tmp, path) < 0)
    {
        library_write_failed();
        remove(tmp);
        return false;
    }
    return true;
}

void lp_index_forget(const char *file)
{
    char path[MAX_PATH];
    index_path(itunesdb_path_hash(file), path, sizeof(path));
    remove(path);
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
    const bool indexed = ifd >= 0
        && read(ifd, &head, sizeof(head)) == sizeof(head)
        && head.magic == LP_MAGIC && head.version == LP_VERSION
        && head.commitid == (uint32_t)tagcache_get_commitid()
        && head.entries == (uint32_t)tagcache_entry_count()
        && (long)head.size == old_size && !head.pending
        && head.sort == ITUNESDB_SORT_MANUAL
        && lseek(ifd, sizeof(head) + head.count * sizeof(struct lp_song),
                 SEEK_SET) >= 0;
    if (!indexed)
    {
        if (ifd >= 0)
            close(ifd);
        ifd = -1;
        remove(path);
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
        }
        return;
    }
    char buf[2 * MAX_PATH];
    struct lp_song added[32];
    int n_added = 0;
    bool ok = true;
    size_t have = 0;
    bool end = false;
    while (!end)
    {
        const ssize_t n = read(fd, buf + have, sizeof(buf) - 1 - have);
        if (n <= 0)
            end = true;
        else
            have += n;
        buf[have] = '\0';
        char *line = buf;
        for (;;)
        {
            char *nl = strchr(line, '\n');
            if (!nl && !end)
                break;
            if (nl)
                *nl = '\0';
            size_t len = strlen(line);
            if (len && line[len - 1] == '\r')
                line[--len] = '\0';
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
            if (!nl)
            {
                line += len;
                break;
            }
            line = nl + 1;
        }
        have = buf + have - line;
        memmove(buf, line, have);
        if (have >= sizeof(buf) - 1)
            have = 0;
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
        remove(path);
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
    size_t have = 0;
    bool end = false, full = false;
    while (!end && !full)
    {
        const ssize_t n = read(fd, buf + have, size - 1 - have);
        if (n <= 0)
            end = true;
        else
            have += n;
        buf[have] = '\0';
        char *line = buf;
        for (;;)
        {
            char *nl = strchr(line, '\n');
            if (!nl && !end)
                break;
            if (nl)
                *nl = '\0';
            size_t len = strlen(line);
            if (len && line[len - 1] == '\r')
                line[--len] = '\0';
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
            if (!nl)
            {
                line += len;
                break;
            }
            line = nl + 1;
        }
        /* the line not ended, to the front for the rest of it */
        have = buf + have - line;
        memmove(buf, line, have);
        if (have >= size - 1)
            have = 0; /* longer than any path */
        yield();
    }
    close(fd);
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

/* a line into the block written, the block first if it's full */
struct lp_out {
    int fd;
    char block[2048];
    size_t used;
    bool ok;
};

static void out_line(struct lp_out *out, const char *line, size_t n)
{
    if (out->used + n + 1 > sizeof(out->block))
    {
        out->ok = out->ok
                  && write(out->fd, out->block, out->used) == (ssize_t)out->used;
        out->used = 0;
    }
    if (n + 1 > sizeof(out->block))
    {
        out->ok = false;
        return;
    }
    memcpy(out->block + out->used, line, n);
    out->block[out->used + n] = '\n';
    out->used += n + 1;
}

/* The songs of a file the database hasn't, as of a format it doesn't take,
   after those written: kept as they were rather than lost. How many. */
static uint32_t keep_unmatched(const char *file, struct lp_out *out)
{
    const int fd = open_utf8(file, O_RDONLY);
    if (fd < 0)
        return 0;
    char chunk[256], line[MAX_PATH];
    size_t have = 0;
    bool too_long = false;
    uint32_t kept = 0;
    ssize_t n;
    bool end = false;
    while (out->ok && !end)
    {
        n = read(fd, chunk, sizeof(chunk));
        if (n <= 0)
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

bool lp_write_file(const char *file, struct lp_song *songs, int count,
                   struct lp_state *state)
{
    /* the import's only: written with its mark, a playlist made on the iPod
       would be taken for one and removed at the next sync */
    if (!itunesdb_own_playlist(file))
        return false;
    const uint32_t sort = state->sort;
    char tmp[MAX_PATH + 4], path[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.tmp", file);
    /* its time as Finder last changed it, which its place in Playlists goes by */
    const time_t time = file_time(file);
    struct tagcache_search tcs;
    if (!tagcache_search(&tcs, tag_filename))
        return false;
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool ok = fd >= 0;
    /* its head as the import writes it, which knows it as its own by that */
    char head[sizeof(ITUNESDB_PLAYLIST_HEAD ITUNESDB_PLAYLIST_MARK) + 32];
    int len = snprintf(head, sizeof(head), "%s%s\n", ITUNESDB_PLAYLIST_HEAD,
                       ITUNESDB_PLAYLIST_MARK);
    if (sort > ITUNESDB_SORT_MANUAL)
        len += snprintf(head + len, sizeof(head) - len,
                        ITUNESDB_PLAYLIST_SORT "%lu\n", (unsigned long)sort);
    ok = ok && write(fd, head, len) == len;
    struct lp_out out;
    out.fd = fd;
    out.used = 0;
    out.ok = ok;
    uint32_t line = 0;
    for (int i = 0; out.ok && i < count; i++)
    {
        if (!tagcache_retrieve(&tcs, songs[i].idx, tag_filename, path, sizeof(path)))
        {
            songs[i].line = LP_NO_LINE;
            continue;
        }
        /* as the import writes them */
        const char *name = no_volume(path);
        out_line(&out, name, strlen(name));
        songs[i].line = line++;
        if (!(i & 63))
            yield();
    }
    const uint32_t kept = state->unmatched ? keep_unmatched(file, &out) : 0;
    ok = out.ok;
    if (ok && out.used)
        ok = write(fd, out.block, out.used) == (ssize_t)out.used;
    if (fd >= 0 && close(fd) < 0)
        ok = false;
    tagcache_search_finish(&tcs);
    /* none of its songs found, as with the database going: not empty for it */
    if (count > 0 && line == 0)
        ok = false;
    if (!ok || rename(tmp, file) < 0)
    {
        remove(tmp);
        return false;
    }
    if (time)
        modtime(file, time);
    state->lines = line + kept;
    state->unmatched = kept;
    return true;
}

bool lp_is_library(const char *file)
{
    return file && database_in_ram() && itunesdb_own_playlist(file);
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
   to a file, which Updating Library goes by, then removes. Only those leave
   the playlists made on the iPod; a line the database never had stays, as a
   computer's path, which a playlist put there by hand may have. */
#define LP_GONE   ROCKBOX_DIR "/playlist_gone.dat"
static uint64_t gone_block[32];
static int gone_in_block;

void lp_gone_flush(void)
{
    if (!gone_in_block)
        return;
    const int fd = open(LP_GONE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0)
    {
        write(fd, gone_block, gone_in_block * sizeof(*gone_block));
        close(fd);
    }
    gone_in_block = 0;
}

void lp_gone_note(const char *path)
{
    gone_block[gone_in_block++] = itunesdb_path_hash(path);
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
    char dir[MAX_PATH], tmp[MAX_PATH + 4];
    const bool m3u = playlist_dir(file, dir, sizeof(dir));
    const int fd = m3u ? open(file, O_RDONLY) : open_utf8(file, O_RDONLY);
    if (fd < 0 || size < 2 * MAX_PATH)
    {
        if (fd >= 0)
            close(fd);
        return false;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", file);
    int wfd = -1;
    bool changed = false, ok = true, end = false;
    size_t have = 0;
    char out[1024];
    size_t put = 0;
    /* the lines before the first change are copied once it's known: from the
       file's start, a BOM open_utf8() passed over too */
    off_t kept_to = 0, read_at = lseek(fd, 0, SEEK_CUR);
    if (read_at < 0)
        read_at = 0;
    while (ok && !end)
    {
        const ssize_t n = read(fd, buf + have, size - 1 - have);
        if (n <= 0)
            end = true;
        else
            have += n;
        buf[have] = '\0';
        char *line = buf;
        for (;;)
        {
            char *nl = strchr(line, '\n');
            if (!nl && !end)
                break;
            if (nl)
                *nl = '\0';
            size_t len = strlen(line);
            const off_t line_at = read_at + (line - buf);
            if (len && line[len - 1] == '\r')
                line[--len] = '\0';
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
                wfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                kept_to = line_at;
                const int rfd = open(file, O_RDONLY);
                ok = wfd >= 0 && rfd >= 0;
                for (off_t done = 0; ok && done < kept_to; )
                {
                    const ssize_t k = read(rfd, out, MIN((off_t)sizeof(out),
                                                         kept_to - done));
                    ok = k > 0 && write(wfd, out, k) == k;
                    done += k;
                }
                if (rfd >= 0)
                    close(rfd);
            }
            if (changed && ok && !drop && (len || nl))
            {
                if (replace)
                    text = moved;
                const size_t tlen = strlen(text);
                if (put + tlen + 1 > sizeof(out))
                {
                    ok = write(wfd, out, put) == (ssize_t)put;
                    put = 0;
                }
                if (tlen + 1 <= sizeof(out))
                {
                    memcpy(out + put, text, tlen);
                    out[put + tlen] = '\n';
                    put += tlen + 1;
                }
            }
            if (!nl)
            {
                line += len;
                break;
            }
            line = nl + 1;
        }
        const size_t consumed = line - buf;
        read_at += consumed;
        have -= consumed;
        memmove(buf, line, have);
        if (have >= size - 1)
        {
            read_at += have;
            have = 0;
        }
        yield();
    }
    close(fd);
    if (!changed)
        return false;
    if (ok && put)
        ok = write(wfd, out, put) == (ssize_t)put;
    if (wfd >= 0 && close(wfd) < 0)
        ok = false;
    if (!ok || rename(tmp, file) < 0)
    {
        remove(tmp);
        return false;
    }
    return true;
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
    bool (*go_on)(void);
} all;

static void index_folder(char *path, size_t len, int depth)
{
    DIR *dir = opendir(path);
    if (!dir)
        return;
    struct dirent *e;
    while ((e = readdir(dir)))
    {
        if (e->d_name[0] == '.')
            continue;
        if (all.go_on && !all.go_on())
        {
            all.overflow = true; /* not all seen */
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
        if (is_dir)
        {
            if (depth < 8)
                index_folder(path, strlen(path), depth + 1);
            else
                all.overflow = true;
        }
        else if (ext && (!strcasecmp(ext, ".m3u8") || !strcasecmp(ext, ".m3u")))
        {
            if (lp_index_read(path, NULL, 0, NULL) < 0)
                index_make(path, all.songs, all.max, all.buf, all.size, true);
            if (all.seen_count < LP_SEEN_MAX)
                all.seen[all.seen_count++] = itunesdb_path_hash(path);
            else
                all.overflow = true;
        }
        path[len] = '\0';
        yield();
    }
    closedir(dir);
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
    /* nothing changed since the last whole pass */
    struct lp_mark now, last;
    mark_now(&now, path);
    int fd = open(LP_MARK, O_RDONLY);
    if (fd >= 0)
    {
        const bool same = read(fd, &last, sizeof(last)) == sizeof(last)
                          && !memcmp(&last, &now, sizeof(now));
        close(fd);
        if (same)
            return;
    }
    char *buf = plugin_get_buffer(&size);
    const size_t read_size = 8192;
    if (!buf || size < read_size + LP_SEEN_MAX * sizeof(uint64_t) + 64)
        return;
    buf = (char *)ALIGN_UP((uintptr_t)buf, 8);
    size -= 8;
    /* the songs the database lost since the last pass, sorted, first: as
       many as half the buffer holds, and the rest stay in the playlists */
    lp_gone_flush();
    gone_set = (uint64_t *)buf;
    gone_count = 0;
    int gfd = open(LP_GONE, O_RDONLY);
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
    if (size < read_size + LP_SEEN_MAX * sizeof(uint64_t) + 64)
    {
        gone_set = NULL;
        gone_count = 0;
        return;
    }
    all.seen = (uint64_t *)buf;
    all.seen_count = 0;
    all.overflow = false;
    all.go_on = go_on;
    all.buf = (char *)(all.seen + LP_SEEN_MAX);
    all.size = read_size;
    all.songs = (struct lp_song *)(all.buf + read_size);
    all.max = MIN((buf + size - (char *)all.songs) / sizeof(struct lp_song),
                  LP_MAX);
    cpu_boost(true);
    index_folder(path, strlen(path), 0);
    /* On-The-Go too, out of the catalogue's folder */
    if (file_exists(OTG_FILE))
    {
        if (lp_index_read(OTG_FILE, NULL, 0, NULL) < 0)
            index_make(OTG_FILE, all.songs, all.max, all.buf, all.size, false);
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
        remove(LP_GONE);
        remove_gone();
        /* as the pass left it, files it wrote again in their order too */
        mark_now(&now, path);
        whole_file_write(LP_MARK, &now, sizeof(now));
    }
    cpu_boost(false);
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
    unsigned int gen;       /* of the plugin buffer, which a plugin takes */
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
    char tmp[MAX_PATH + 4];
    snprintf(tmp, sizeof(tmp), "%s.tmp", copy);
    mkdir(LP_DIR);
    const int wfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool ok = wfd >= 0;
    size_t have = 0, put = 0;
    bool end = false;
    while (ok && !end)
    {
        const ssize_t n = read(fd, in + have, in_size - 1 - have);
        if (n <= 0)
            end = true;
        else
            have += n;
        in[have] = '\0';
        char *line = in;
        for (;;)
        {
            char *nl = strchr(line, '\n');
            if (!nl && !end)
                break;
            if (nl)
                *nl = '\0';
            size_t len = strlen(line);
            if (len && line[len - 1] == '\r')
                line[--len] = '\0';
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
                    ok = write(wfd, out, put) == (ssize_t)put;
                    put = 0;
                }
                if (slen + 1 <= out_size)
                {
                    memcpy(out + put, song, slen);
                    out[put + slen] = '\n';
                    put += slen + 1;
                }
            }
            if (!nl)
            {
                line += len;
                break;
            }
            line = nl + 1;
        }
        have = in + have - line;
        memmove(in, line, have);
        if (have >= in_size - 1)
            have = 0;
        yield();
    }
    close(fd);
    if (ok && put)
        ok = write(wfd, out, put) == (ssize_t)put;
    if (wfd >= 0 && close(wfd) < 0)
        ok = false;
    if (!ok || rename(tmp, copy) < 0)
    {
        remove(tmp);
        return NULL;
    }
    return copy;
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
    const int fd = open(LP_PLAYING_FROM, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        fdprintf(fd, "%s\n", copy_source);
        close(fd);
    }
}

/* The rows: Shuffle Songs first, all of the playlist's in random order, as
   Settings > Shuffle in Lists has it, then its songs; none for one without
   songs. The rows before the songs: Shuffle Songs' if there is one, for a
   playlist of as many songs as the lists of Music have it for, its songs of
   the library, which it plays */
static int lp_lead(void)
{
    return global_settings.shuffle_rows_show[SHUFFLE_ROW_PLAYLISTS]
           && lp.total >= SHUFFLE_ROW_MIN_SONGS ? 1 : 0;
}
/* and those before the row the catalogue remembers, as it was left: the
   setting, Reset Settings or songs taken out may have made them others */
static int lp_selection_lead;

/* and On-The-Go's last, as on a stock iPod: Save Playlist, Clear Playlist */
static bool is_save_row(int row)
{
    return lp.otg && lp.count > 0 && row == lp_lead() + lp.count;
}

static bool is_clear_row(int row)
{
    return lp.otg && lp.count > 0 && row == lp_lead() + lp.count + 1;
}

static bool is_song_row(int row)
{
    return row >= lp_lead() && row < lp_lead() + lp.count;
}

static int lp_rows(void)
{
    return lp.count > 0 ? lp_lead() + lp.count + (lp.otg ? 2 : 0) : 0;
}

static bool lp_shuffle_row(int row, void *data)
{
    (void)data;
    return lp_lead() && row == 0;
}

static const char *lp_name(int row, void *data, char *buf, size_t size)
{
    (void)data;
    (void)buf;
    (void)size;
    if (lp_shuffle_row(row, NULL))
        return str(LANG_SHUFFLE_SONGS);
    if (is_save_row(row))
        return str(LANG_SAVE_PLAYLIST);
    if (is_clear_row(row))
        return str(LANG_ON_THE_GO_CLEAR);
    return lp.pool + lp.names[row - lp_lead()];
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
        tagcache_initial(tag, tagcache_sort_key(lp.songs[row - lp_lead()].idx, tag));
    if (!letter)
        return NULL;
    *utf8encode(letter, (unsigned char *)buf) = '\0';
    return buf;
}

static bool lp_playing(int row, void *data)
{
    (void)data;
    return is_song_row(row) && lp.from_this
           && gui_synclist_playing() == (long)lp.songs[row - lp_lead()].idx + 1;
}

/* the music from this song on, or with song < 0 all of them shuffled, from
   the playlist's file, written first if it's behind the list. False if it
   didn't start. */
static bool lp_play(int song)
{
    if (global_settings.party_mode || !warn_on_pl_erase())
        return false;
    /* Now Playing comes later than the slide's usual wait, as from Music */
    transition_wait(HZ);
    if (lp.state.pending || (song >= 0 && lp.songs[song].line == LP_NO_LINE))
    {
        /* the music from it plays on meanwhile: its file, open, is kept
           until it closes */
        splash(0, ID2P(LANG_WAIT));
        if (!lp_write_file(lp.file, lp.songs, lp.total, &lp.state))
            return false;
        lp_index_write(lp.file, lp.songs, lp.total, &lp.state);
        if (song >= 0 && lp.songs[song].line == LP_NO_LINE)
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
    if (song < 0)
    {
        /* whatever the Shuffle setting, as Shuffle Songs in the main menu */
        playlist_shuffle(current_tick, -1);
        playlist_start(0, 0, 0);
        return true;
    }
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

/* the song out of the queue too, if it plays from this playlist: its first
   time after the one playing, else another than that */
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
    const uint32_t line_no = lp.songs[song].line;
    char dir[MAX_PATH], out_path[MAX_PATH], tmp[MAX_PATH + 4], song_file[MAX_PATH];
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
    snprintf(tmp, sizeof(tmp), "%s.tmp", out_path);
    const int wfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool ok = wfd >= 0;
    size_t have = 0, put = 0;
    uint32_t songs_seen = 0, songs_kept = 0;
    bool end = false;
    while (ok && !end)
    {
        const ssize_t n = read(fd, in + have, in_size - 1 - have);
        if (n <= 0)
            end = true;
        else
            have += n;
        in[have] = '\0';
        char *line = in;
        for (;;)
        {
            char *nl = strchr(line, '\n');
            if (!nl && !end)
                break;
            if (nl)
                *nl = '\0';
            size_t len = strlen(line);
            if (len && line[len - 1] == '\r')
                line[--len] = '\0';
            const bool is_song = len && line[0] != '#';
            const bool gone = is_song && songs_seen++ == line_no;
            if ((len || nl) && !gone)
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
                    ok = write(wfd, out, put) == (ssize_t)put;
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
            if (!nl)
            {
                line += len;
                break;
            }
            line = nl + 1;
        }
        have = in + have - line;
        memmove(in, line, have);
        if (have >= in_size - 1)
            have = 0;
        yield();
    }
    close(fd);
    if (ok && put)
        ok = write(wfd, out, put) == (ssize_t)put;
    if (wfd >= 0 && close(wfd) < 0)
        ok = false;
    if (ok && lp.otg && songs_kept == 0)
    {
        remove(tmp);
        remove(lp.file);
        lp_index_forget(lp.file);
        lp.count = lp.total = 0;
        if (song_file[0] && playing_from(lp.file))
            remove_from_queue(song_file);
        return true;
    }
    if (!ok || rename(tmp, out_path) < 0)
    {
        remove(tmp);
        return false;
    }
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
   1", the first number free, in the catalogue; On-The-Go empty again */
static bool lp_save_otg(char *saved, size_t size)
{
    char root[MAX_PATH];
    if (!catalog_directory_name(root, sizeof(root)))
        return false;
    for (int n = 1; n < 1000; n++)
    {
        snprintf(saved, size, "%s/%s %d.m3u8", root, str(LANG_ON_THE_GO), n);
        char folder[MAX_PATH];
        snprintf(folder, sizeof(folder), "%s/%s %d", root, str(LANG_ON_THE_GO), n);
        if (file_exists(saved) || dir_exists(folder))
            continue;
        if (rename(OTG_FILE, saved) < 0)
            return false;
        lp_index_moved(OTG_FILE, saved);
        lp_source_moved(OTG_FILE, saved);
        return true;
    }
    return false;
}

/* a held Select: the menu of the song, as in Songs */
static int lp_song_menu(int song)
{
    char path[MAX_PATH], title[MAX_PATH];
    struct tagcache_search tcs;
    if (!tagcache_search(&tcs, tag_filename))
        return -1;
    const bool found = tagcache_retrieve(&tcs, lp.songs[song].idx, tag_filename,
                                         path, sizeof(path));
    tagcache_search_finish(&tcs);
    if (!found)
        return -1;
    /* its own: what the menu opens may take the plugin buffer */
    strmemccpy(title, lp.pool + lp.names[song], sizeof(title));
    onplay_set_title(title);
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
    if (lp.unloaded)
    {
        if (!was_wide)
            menu_set_wide(false);
        return PLAYLIST_VIEWER_CANCEL;
    }

    /* its name, without .m3u8 */
    char title[MAX_PATH];
    const char *base = strrchr(lp.file, '/');
    strmemccpy(title, base ? base + 1 : lp.file, sizeof(title));
    char *ext = strrchr(title, '.');
    if (ext && ext != title)
        *ext = '\0';
    if (lp.otg)
        strmemccpy(title, str(LANG_ON_THE_GO), sizeof(title));

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
    gui_synclist_set_shuffle_callback(&list, lp_shuffle_row);
    gui_synclist_set_initial_callback(&list, lp_initial);
    gui_synclist_set_nb_items(&list, lp_rows());
    /* at the row it was left at, or a new one at its first song, Shuffle
       Songs above it */
    int sel = selection && *selection >= 0
              ? MAX(*selection - lp_selection_lead + lp_lead(), 0) : lp_lead();
    if (lp.at_playing)
    {
        const long playing = gui_synclist_song_playing();
        for (int i = 0; i < lp.count; i++)
        {
            if ((long)lp.songs[i].idx + 1 == playing)
            {
                sel = lp_lead() + i;
                break;
            }
        }
    }
    gui_synclist_select_item(&list, MAX(MIN(sel, lp_rows() - 1), 0));

    int ret = -1;
    bool redraw = true;
    while (ret < 0)
    {
        /* the plugin buffer taken meanwhile, as by a dock: read again */
        if (plugin_buffer_gen() != lp.gen)
        {
            const int row = gui_synclist_get_sel_pos(&list);
            if (!lp_load())
            {
                ret = PLAYLIST_VIEWER_CANCEL;
                break;
            }
            gui_synclist_set_nb_items(&list, lp_rows());
            gui_synclist_select_item(&list, MAX(MIN(row, lp_rows() - 1), 0));
            redraw = true;
        }
        if (redraw)
        {
            lp.from_this = playing_from(lp.file);
            gui_synclist_draw(&list);
        }
        redraw = false;
        int action;
        if (list_do_action(CONTEXT_TREE | ALLOW_SOFTLOCK, HZ / 2, &list, &action))
            continue;
        /* holding Play goes to the song playing, if there is one */
        if (action == ACTION_TREE_STOP && audio_status())
            action = ACTION_TREE_WPS;
        const int row = gui_synclist_get_sel_pos(&list);
        switch (action)
        {
            case ACTION_STD_OK:
                if (lp.count <= 0)
                    break;
                if (is_save_row(row))
                {
                    char saved[MAX_PATH];
                    if (lp_save_otg(saved, sizeof(saved)))
                    {
                        catalog_select_playlist(saved);
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
                if (lp_shuffle_row(row, NULL))
                {
                    if (lp_play(-1))
                        ret = PLAYLIST_VIEWER_OK;
                    else
                        transition_cancel();
                    redraw = true;
                    break;
                }
                /* the song playing, of this list: to it, paused or not */
                lp.from_this = playing_from(lp.file);
                if (lp.from_this && gui_synclist_song_playing()
                                    == (long)lp.songs[row - lp_lead()].idx + 1)
                {
                    ret = PLAYLIST_VIEWER_OK;
                    break;
                }
                if (lp_play(row - lp_lead()))
                    ret = PLAYLIST_VIEWER_OK;
                else
                    transition_cancel();
                redraw = true;
                break;
            case ACTION_STD_CONTEXT:
            {
                if (lp.count <= 0 || !is_song_row(row))
                    break;
                const int lead = lp_lead();
                /* On-The-Go's song out at once, as on a stock iPod */
                if (lp.otg)
                    lp_remove(row - lead);
                else
                    ret = lp_song_menu(row - lead);
                /* a song fewer, perhaps; none left of On-The-Go, back */
                if (lp.otg && lp.count <= 0)
                {
                    transition_begin(TRANSITION_BACK);
                    ret = PLAYLIST_VIEWER_CANCEL;
                    break;
                }
                /* on the song after it still, Shuffle Songs gone above it
                   if three are left */
                gui_synclist_set_nb_items(&list, lp_rows());
                gui_synclist_select_item(&list, MAX(MIN(row - lead + lp_lead(),
                                                        lp_rows() - 1), 0));
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
    {
        *selection = gui_synclist_get_sel_pos(&list);
        lp_selection_lead = lp_lead();
    }
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
