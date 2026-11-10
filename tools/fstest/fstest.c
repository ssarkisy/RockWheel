/* Crash tests of the file system: the real fat.c, disk_cache.c, file.c and
   their neighbours, built with the host's cc over a FAT32 image; run.py runs
   the whole test. A scenario sets up the image, then does what the firmware
   does while each sector written goes to a journal. The image after the
   first k of those writes is the disk a freeze or a dead battery leaves at
   that moment; on it the next boot is played: the settings recovered as
   settings_load() does, then a file filling all free space, which turns an
   entry left on a free cluster into two files on the same clusters.

   fstest list
   fstest setup <scenario> <img> [layout]
   fstest action <scenario> <img> <journal> [seed]
       prints how many writes, and 1 if a rename was in one sector, else 0
   fstest crash <scenario> <img> <journal> <k> <crash-img> <post-img> [1|0]
   fstest errno <img>
   fstest full <img>
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "file.h"
#include "dir.h"
#include "fat.h"
#include "disk_cache.h"
#include "pathfuncs.h"
#include "fileobj_mgr.h"
#include "rb_namespace.h"
#include "file_internal.h"
#include "host.h"

#define CLUSTER 32768

#define CONFIGFILE ROCKBOX_DIR "/config.cfg"
#define RESUMEFILE ROCKBOX_DIR "/.resume.cfg"
#define CONTROLFILE ROCKBOX_DIR "/.playlist_control"
#define PLAYCOUNTS "/iPod_Control/iTunes/Play Counts"

static void fail(const char *what)
{
    fprintf(stderr, "fstest: %s failed, errno %d\n", what, errno);
    exit(2);
}

static void fs_mount(const char *img)
{
    host_open(img);
    init_volume_names();
    filesystem_init();
    fat_init();
    if (fat_mount(IF_MV(0,) IF_MD(0,) 0) < 0)
        fail("mount");
    volume_onmount_internal(IF_MV(0));
}

static void fs_unmount(void)
{
    volume_onunmount_internal(IF_MV(0));
    fat_unmount(IF_MV(0));
    host_close();
}

/** Contents **/

/* the bytes of a file of the test from offset at on */
static void bytes(char *buf, size_t size, unsigned seed, size_t at)
{
    for (size_t i = at; i < at + size; i++)
        buf[i - at] = (char)(i * 131 + seed * 7 + (i >> 9));
}

static void put_file(const char *path, size_t size, unsigned seed,
                     size_t chunk)
{
    static char buf[65536];
    int fd = open(path, O_CREAT|O_TRUNC|O_WRONLY, 0666);
    if (fd < 0)
        fail(path);
    for (size_t at = 0; at < size; at += chunk)
    {
        size_t n = size - at < chunk ? size - at : chunk;
        bytes(buf, n, seed, at);
        if (write(fd, buf, n) != (ssize_t)n)
            fail(path);
    }
    if (close(fd) < 0)
        fail(path);
}

/* the whole file; -1 if missing */
static long get_file(const char *path, char *buf, size_t max)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    long n = read(fd, buf, max);
    close(fd);
    return n;
}

/* a file put_file() wrote */
static bool same_file(const char *path, size_t size, unsigned seed)
{
    static char got[65536], want[65536];
    long n = get_file(path, got, sizeof got);
    bytes(want, size, seed, 0);
    return n == (long)size && !memcmp(got, want, size);
}

/* a settings file: save number v, n lines */
struct cfg { int v; int lines; bool header; };

static size_t cfg_text(const struct cfg *c, char *buf, size_t max)
{
    size_t n = 0;
    if (c->header)
        n += snprintf(buf, max, "# .cfg file created by rockbox fstest - "
                      "http://www.rockbox.org\r\n\r\n");
    for (int i = 0; i < c->lines; i++)
        n += snprintf(buf + n, max - n, "setting %d: value %d of save %d\r\n",
                      i, i * 7 + c->v, c->v);
    return n;
}

/* as settings_write_config() does: fdprintf() a line at a time */
static bool cfg_write(const char *path, const struct cfg *c)
{
    int fd = open(path, O_CREAT|O_TRUNC|O_WRONLY, 0666);
    if (fd < 0)
        return false;
    if (c->header)
        fdprintf(fd, "# .cfg file created by rockbox %s - "
                 "http://www.rockbox.org\r\n\r\n", "fstest");
    for (int i = 0; i < c->lines; i++)
        fdprintf(fd, "setting %d: value %d of save %d\r\n",
                 i, i * 7 + c->v, c->v);
    return close(fd) == 0;
}

/* what settings_save_new() writes with: the save cfg_save() was given */
static const struct cfg *cfg_saving;

static bool settings_write_config(const char *filename, int options)
{
    (void)options;
    return cfg_write(filename, cfg_saving);
}

/* the functions of apps/settings.c that save and recover the settings */
#include "settings_fns.c"

static void names(const char *file, char *tmp, char *new, char *old)
{
    snprintf(tmp, 64, "%s.tmp", file);
    snprintf(new, 64, "%s.new", file);
    snprintf(old, 64, "%s.old", file);
}

/* a save of 1.3 and earlier: a .new promoted at the next boot */
static bool cfg_save13(const char *file, const struct cfg *c)
{
    char tmp[64], new[64], old[64];
    names(file, tmp, new, old);
    return cfg_write(new, c);
}

/* a save of the tree under test */
static bool cfg_save(const char *file, const struct cfg *c)
{
#ifdef HAVE_SETTINGS_SAVE_NEW
    char tmp[64], new[64], old[64];
    names(file, tmp, new, old);
    cfg_saving = c;
    return settings_save_new(new, 0);
#else
    return cfg_save13(file, c);
#endif
}

/* what settings_load() does first */
static void cfg_boot(const char *file)
{
    char tmp[64], new[64], old[64];
    names(file, tmp, new, old);
#ifdef HAVE_SETTINGS_FILE_RECOVER
    settings_file_recover(file, new, old);
#else
    rename_temp_file(new, file, old);
#endif
}

/* which of the saves, the list's of them ended by NULL, the file holds */
static bool cfg_check(const char *file, const struct cfg *const *saves,
                      char *verdict, size_t max)
{
    static char got[65536], want[65536];
    long n = get_file(file, got, sizeof got);
    const char *base = strrchr(file, '/') + 1;
    if (n < 0)
        return snprintf(verdict, max, "%s missing", base), false;
    if (n == 0)
        return snprintf(verdict, max, "%s empty", base), false;
    for (; *saves; saves++)
    {
        size_t len = cfg_text(*saves, want, sizeof want);
        if ((size_t)n == len && !memcmp(got, want, len))
            return snprintf(verdict, max, "%s save %d", base, (*saves)->v),
                   true;
    }
    return snprintf(verdict, max, "%s mixed (%ld bytes)", base, n), false;
}

/** Scenarios **/

/* how many more files the layout puts in .rockbox, ahead of a scenario's
   own: its entries fall in other places of the folder's sectors */
static int layout;

/* a .rockbox of a used iPod ahead of what a scenario writes, and a file
   that brings the clusters it allocates up to the FAT's second sector */
static void setup_common(void)
{
    static const char *dirs[] = { "rocks", "codecs", "langs", "wps",
                                  "themes", "fonts", "playlists" };
    char path[MAX_PATH];
    put_file("/filler.bin", 100 * CLUSTER, 1, 65536);
    if (mkdir(ROCKBOX_DIR) < 0)
        fail("mkdir");
    for (unsigned i = 0; i < sizeof dirs / sizeof *dirs; i++)
    {
        snprintf(path, sizeof path, ROCKBOX_DIR "/%s", dirs[i]);
        if (mkdir(path) < 0)
            fail(path);
    }
    for (int i = 0; i < 12 + layout; i++)
    {
        snprintf(path, sizeof path, ROCKBOX_DIR "/database_%d.tcd", i);
        put_file(path, 2000 + i * 100, i, 4096);
    }
}

static const struct cfg cfg_0 = { 0, 105, true },
                        cfg_a = { 1, 110, true },
                        cfg_b = { 2, 120, true },
                        cfg_c = { 3, 100, true },
                        res_a = { 1, 14, false },
                        res_b = { 2, 15, false };

/* config.cfg a, its .old the save before, and .resume.cfg */
static void settings_setup(void)
{
    setup_common();
    if (!cfg_save(CONFIGFILE, &cfg_0))
        fail("save");
    cfg_boot(CONFIGFILE);
    if (!cfg_save(CONFIGFILE, &cfg_a) || !cfg_save(RESUMEFILE, &res_a))
        fail("save");
    cfg_boot(CONFIGFILE);
    cfg_boot(RESUMEFILE);
}

static void settings_action(void)
{
    cfg_save(CONFIGFILE, &cfg_b);
}

static bool settings_post(char *verdict, size_t max)
{
    static const struct cfg *const saves[] = { &cfg_a, &cfg_b, NULL };
    cfg_boot(CONFIGFILE);
    return cfg_check(CONFIGFILE, saves, verdict, max);
}

/* a save waiting for the boot that promotes it */
static void boot_setup(void)
{
    settings_setup();
    if (!cfg_save(CONFIGFILE, &cfg_b))
        fail("save");
}

static void boot_action(void)
{
    cfg_boot(CONFIGFILE);
}

/* the second save before the next boot; the first goes for the second's
   .new, so a cut may leave the one of the last boot */
static void again_action(void)
{
    cfg_save(CONFIGFILE, &cfg_c);
}

static bool again_post(char *verdict, size_t max)
{
    static const struct cfg *const saves[] = { &cfg_a, &cfg_b, &cfg_c, NULL };
    cfg_boot(CONFIGFILE);
    return cfg_check(CONFIGFILE, saves, verdict, max);
}

#ifdef HAVE_SETTINGS_FILE_RECOVER
/* 1.3 cut short while saving, the next boot this tree's */
static void old_action(void)
{
    cfg_save13(CONFIGFILE, &cfg_b);
}
#endif

static void resume_action(void)
{
    cfg_save(RESUMEFILE, &res_b);
}

static bool resume_post(char *verdict, size_t max)
{
    static const struct cfg *const saves[] = { &res_a, &res_b, NULL };
    cfg_boot(RESUMEFILE);
    return cfg_check(RESUMEFILE, saves, verdict, max);
}

/* a new queue: control file renamed to .old, then a command at a time,
   each made to stick with fsync() as sync_control_unlocked() does */
#define CONTROL_COMMANDS 170

static size_t control_line(int i, char *buf, size_t max)
{
    return snprintf(buf, max, "A:%d:%d:/Music/Some Artist %03d/An Album Of "
                    "Theirs/%02d A Song With A Rather Long Title Indeed, Part "
                    "%d (Remastered Version From The Deluxe Box Set).m4a\n",
                    i, i * 3, i % 50, i % 20 + 1, i);
}

static void playlist_setup(void)
{
    setup_common();
    put_file(CONTROLFILE ".old", 3000, 5, 512);
    put_file(CONTROLFILE, 5000, 6, 512);
}

static void playlist_action(void)
{
    char line[512];
    rename(CONTROLFILE, CONTROLFILE ".old");
    int fd = open(CONTROLFILE, O_CREAT|O_RDWR|O_TRUNC, 0666);
    if (fd < 0)
        return;
    write(fd, "P:2:0:0\n", 8);
    fsync(fd);
    for (int i = 0; i < CONTROL_COMMANDS; i++)
    {
        size_t n = control_line(i, line, sizeof line);
        write(fd, line, n);
        fsync(fd);
    }
    close(fd);
}

/* whatever the cut, the control file is its first commands */
static bool playlist_post(char *verdict, size_t max)
{
    static char got[65536], want[65536];
    long n = get_file(CONTROLFILE, got, sizeof got);
    size_t len = snprintf(want, sizeof want, "P:2:0:0\n");
    for (int i = 0; i < CONTROL_COMMANDS; i++)
        len += control_line(i, want + len, sizeof want - len);
    if (n < 0)
        return snprintf(verdict, max, "control missing"), true;
    if (same_file(CONTROLFILE, 5000, 6))
        return snprintf(verdict, max, "control old"), true;
    if ((size_t)n <= len && !memcmp(got, want, n))
        return snprintf(verdict, max, "control %ld bytes", n), true;
    return snprintf(verdict, max, "control bad (%ld bytes)", n), false;
}

/* Play Counts as itunesdb.c writes it: a .tmp renamed over the old one */
static void rename_setup(void)
{
    setup_common();
    if (mkdir("/iPod_Control") < 0 || mkdir("/iPod_Control/iTunes") < 0)
        fail("mkdir");
    put_file("/iPod_Control/iTunes/iTunesDB", 3 * CLUSTER + 1000, 7, 4096);
    put_file(PLAYCOUNTS, 40000, 8, 4096);
}

static void rename_action(void)
{
    put_file(PLAYCOUNTS ".tmp", 48000, 9, 4096);
    rename(PLAYCOUNTS ".tmp", PLAYCOUNTS);
}

static bool rename_post(char *verdict, size_t max)
{
    const char *state = same_file(PLAYCOUNTS, 40000, 8) ? "old" :
                        same_file(PLAYCOUNTS, 48000, 9) ? "new" : NULL;
    if (state)
        return snprintf(verdict, max, "Play Counts %s", state), true;
    return snprintf(verdict, max, "Play Counts bad"), false;
}

/* Renames in .rockbox, whose entries the layout moves: the old name's short
   entry and the new one's fall in one sector in some layouts, in two in
   others. A file is under one name or the other after a cut, or under none
   (old entries gone, new ones not yet whole); never under both, which is two
   entries on one chain. */
#define RN_FROM  ROCKBOX_DIR "/Road Trip.m3u8.tmp"
#define RN_TO    ROCKBOX_DIR "/Road Trip.m3u8"
#define RN_SIZE  40000
#define RN_SEED  20
#define MOVE_DIR "/Playlists"

/* set by a rename in one folder whose old short entry and new entries all
   are in one sector, written at once: no cut leaves the file under none */
static bool one_sector;

/* the sectors of path's folder holding its first and its short entry */
static bool entry_sectors(const char *path, unsigned long *first,
                          unsigned long *last)
{
    struct filestr_base stream;
    if (open_stream_internal(path, FF_ANYTYPE, &stream, NULL) <= 0)
        return false;
    const struct fat_dirscan_info *e = &stream.infop->fatfile.e;
    *first = (e->entry - e->entries + 1) / (512 / 32);
    *last = e->entry / (512 / 32);
    close_stream_internal(&stream);
    return true;
}

static void rename_in_folder(const char *from, const char *to)
{
    unsigned long f0, f1, t0, t1;
    bool known = entry_sectors(from, &f0, &f1);
    rename(from, to);
    one_sector = known && entry_sectors(to, &t0, &t1) && f1 == t0 && t0 == t1;
}

/* 1 if whole, 0 if missing, -1 if it holds something else */
static int whole(const char *path, size_t size, unsigned seed)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    close(fd);
    return same_file(path, size, seed) ? 1 : -1;
}

/* where the file is, from and to each whole or missing */
static bool moved_post(const char *from, const char *to, char *verdict,
                       size_t max)
{
    int f = whole(from, RN_SIZE, RN_SEED), t = whole(to, RN_SIZE, RN_SEED);
    snprintf(verdict, max, "%s", f < 0 || t < 0 ? "bad" : f && t ? "both" :
             f ? "old name" : t ? "new name" :
             one_sector ? "none in one sector" : "none");
    return f >= 0 && t >= 0 && !(f && t) && (f || t || !one_sector);
}

static void rename_new_setup(void)
{
    setup_common();
    put_file(RN_FROM, RN_SIZE, RN_SEED, 4096);
}

static void rename_new_action(void)
{
    rename_in_folder(RN_FROM, RN_TO);
}

static bool rename_new_post(char *verdict, size_t max)
{
    return moved_post(RN_FROM, RN_TO, verdict, max);
}

/* over an older one of the name: that one is there until the new one is */
static void rename_over_setup(void)
{
    setup_common();
    put_file(RN_TO, 30000, RN_SEED + 1, 4096);
    put_file(RN_FROM, RN_SIZE, RN_SEED, 4096);
}

static bool rename_over_post(char *verdict, size_t max)
{
    int t = whole(RN_TO, RN_SIZE, RN_SEED);
    if (t < 0 && same_file(RN_TO, 30000, RN_SEED + 1))
        return snprintf(verdict, max, "older one"), true;
    return snprintf(verdict, max, t > 0 ? "new one" : t ? "bad" : "none"),
           t > 0;
}

/* to another folder: the two names always in two sectors */
static void rename_move_setup(void)
{
    rename_new_setup();
    if (mkdir(MOVE_DIR) < 0)
        fail("mkdir");
}

static void rename_move_action(void)
{
    rename(RN_FROM, MOVE_DIR "/Road Trip.m3u8");
}

static bool rename_move_post(char *verdict, size_t max)
{
    return moved_post(RN_FROM, MOVE_DIR "/Road Trip.m3u8", verdict, max);
}

/* a folder with its file moved: its '..' changes too */
#define RN_DIR ROCKBOX_DIR "/Road Trip"

static void rename_dir_setup(void)
{
    setup_common();
    if (mkdir(RN_DIR) < 0 || mkdir(MOVE_DIR) < 0)
        fail("mkdir");
    put_file(RN_DIR "/Road Trip.m3u8", RN_SIZE, RN_SEED, 4096);
}

static void rename_dir_action(void)
{
    rename(RN_DIR, MOVE_DIR "/Road Trip");
}

static bool rename_dir_post(char *verdict, size_t max)
{
    return moved_post(RN_DIR "/Road Trip.m3u8",
                      MOVE_DIR "/Road Trip/Road Trip.m3u8", verdict, max);
}

/* a playlist saved into a folder of its own */
#define NEWDIR_FILE "/Playlists/Road Trip.m3u8"

static void newdir_action(void)
{
    mkdir("/Playlists");
    put_file(NEWDIR_FILE, 40000, 10, 4096);
}

static bool newdir_post(char *verdict, size_t max)
{
    static char got[65536];
    long n = get_file(NEWDIR_FILE, got, sizeof got);
    if (n <= 0 || same_file(NEWDIR_FILE, 40000, 10))
        return snprintf(verdict, max, "playlist %ld bytes", n), true;
    return snprintf(verdict, max, "playlist bad (%ld bytes)", n), false;
}

/* a folder whose cluster is full takes another for the next entry */
#define GROWDIR "/Music"

static void dirgrow_setup(void)
{
    char path[MAX_PATH];
    setup_common();
    if (mkdir(GROWDIR) < 0)
        fail("mkdir");
    /* empty files of two entries each, with . and .. the cluster's 1024 */
    for (int i = 0; i < 511; i++)
    {
        snprintf(path, sizeof path, GROWDIR "/Track %03d.m4a", i);
        put_file(path, 0, i, 1);
    }
}

static void dirgrow_action(void)
{
    put_file(GROWDIR "/Track 511.m4a", 100, 511, 100);
}

/* a folder made there: two new clusters, one linked to the full one */
static void mkdir_grow_action(void)
{
    mkdir(GROWDIR "/Live");
}

/* a move into a full folder, which grows for the new name */
#define GROW_TO GROWDIR "/Road Trip.m3u8"

static void rename_grow_setup(void)
{
    dirgrow_setup();
    put_file(RN_FROM, RN_SIZE, RN_SEED, 4096);
}

static void rename_grow_action(void)
{
    rename(RN_FROM, GROW_TO);
}

static bool rename_grow_post(char *verdict, size_t max)
{
    return moved_post(RN_FROM, GROW_TO, verdict, max);
}

/* a rename in a full folder to a longer name: it grows, the same folder */
#define GS_FROM GROWDIR "/Road Trip.tmp"
#define GS_TO   GROWDIR "/Road Trip, the long version of the playlist.m3u8"

static void rename_grow_same_setup(void)
{
    dirgrow_setup();
    remove(GROWDIR "/Track 000.m4a");
    put_file(GS_FROM, RN_SIZE, RN_SEED, 4096);
}

static void rename_grow_same_action(void)
{
    rename_in_folder(GS_FROM, GS_TO);
}

static bool rename_grow_same_post(char *verdict, size_t max)
{
    return moved_post(GS_FROM, GS_TO, verdict, max);
}

/* a rename of the case alone: the old 8.3 name is still there for the new
   one's search */
#define RC_FROM ROCKBOX_DIR "/road trip.m3u8"
#define RC_TO   ROCKBOX_DIR "/Road Trip.m3u8"

static void rename_case_setup(void)
{
    setup_common();
    put_file(RC_FROM, RN_SIZE, RN_SEED, 4096);
}

static void rename_case_action(void)
{
    rename_in_folder(RC_FROM, RC_TO);
}

static bool rename_case_post(char *verdict, size_t max)
{
    int t = whole(RC_TO, RN_SIZE, RN_SEED);
    snprintf(verdict, max, "%s", t > 0 ? "present" : t ? "bad" :
             one_sector ? "none in one sector" : "none");
    return t > 0 || (!t && !one_sector);
}

/* O_TRUNC of a file of 64 KB and more: its clusters freed at the open */
#define BIGFILE ROCKBOX_DIR "/big.dat"
#define SMALLFILE ROCKBOX_DIR "/small.dat"

static void trunc_setup(void)
{
    setup_common();
    put_file(BIGFILE, 100000, 11, 4096);
    put_file(SMALLFILE, 10000, 12, 4096);
}

static void trunc_action(void)
{
    int fd = open(BIGFILE, O_TRUNC|O_WRONLY);
    if (fd < 0)
        return;
    static char buf[10000];
    bytes(buf, sizeof buf, 13, 0);
    write(fd, buf, sizeof buf);
    close(fd);
}

/* a file closed empty: its first cluster freed with its entry's update */
static void zero_action(void)
{
    int fd = open(SMALLFILE, O_TRUNC|O_WRONLY);
    if (fd >= 0)
        close(fd);
}

/* The scan's file written 8 KB at a time on a volume that fills up, as the
   database's scan writes it: the writes past its last cluster fail, and go
   on once another file is removed */
#define FULL_FILE  ROCKBOX_DIR "/database_tmp.tcd"
#define FULL_SPARE "/spare.bin"
#define FULL_CHUNK 8192
#define FULL_SEED  23

/* every cluster taken but the spare file's and two more */
static void full_fill(void)
{
    put_file(FULL_SPARE, 2 * CLUSTER, 21, 4096);
    sector_t free_kb;
    if (!fat_size(IF_MV(0,) NULL, &free_kb))
        fail("fat_size");
    put_file("/full.bin", (free_kb - 2 * CLUSTER / 1024) * 1024, 22, 65536);
}

static void full_setup(void)
{
    setup_common();
    full_fill();
}

/* a write at the file's end, of the bytes of that place */
static bool full_write(int fd)
{
    static char buf[FULL_CHUNK];
    off_t at = lseek(fd, 0, SEEK_END);
    bytes(buf, sizeof buf, FULL_SEED, at);
    return write(fd, buf, sizeof buf) == sizeof buf;
}

static void full_action(void)
{
    int fd = open(FULL_FILE, O_CREAT|O_TRUNC|O_WRONLY, 0666);
    if (fd < 0)
        return;
    for (int failed = 0; failed < 4; )
        failed += !full_write(fd);
    remove(FULL_SPARE);
    for (int failed = 0; failed < 2; )
        failed += !full_write(fd);
    close(fd);
}

/* the file is the start of what was written; the spare is whole or gone */
static bool full_post(char *verdict, size_t max)
{
    static char got[FULL_CHUNK], want[FULL_CHUNK];
    long size = 0, n;
    bool same = true;
    int fd = open(FULL_FILE, O_RDONLY);
    while (fd >= 0 && same && (n = read(fd, got, sizeof got)) > 0)
    {
        bytes(want, n, FULL_SEED, size);
        same = !memcmp(got, want, n);
        size += n;
    }
    if (fd >= 0)
        close(fd);
    int spare = whole(FULL_SPARE, 2 * CLUSTER, 21);
    snprintf(verdict, max, "file %s%ld bytes, spare %s", same ? "" : "bad ",
             fd < 0 ? -1 : size, spare > 0 ? "whole" : spare ? "bad" : "gone");
    return same && spare >= 0;
}

static bool no_post(char *verdict, size_t max)
{
    verdict[0] = '\0';
    (void)max;
    return true;
}

static const struct scenario
{
    const char *name;
    void (*setup)(void);
    void (*action)(void);
    bool (*post)(char *verdict, size_t max);
} scenarios[] =
{
    { "settings",       settings_setup, settings_action, settings_post },
    { "settings-again", boot_setup,     again_action,    again_post },
    { "settings-boot",  boot_setup,     boot_action,     settings_post },
#ifdef HAVE_SETTINGS_FILE_RECOVER
    { "settings-1.3",   settings_setup, old_action,      settings_post },
#endif
    { "resume",         settings_setup, resume_action,   resume_post },
    { "playlist",       playlist_setup, playlist_action, playlist_post },
    { "rename",         rename_setup,   rename_action,   rename_post },
    { "rename-new",     rename_new_setup,  rename_new_action,
                        rename_new_post },
    { "rename-over",    rename_over_setup, rename_new_action,
                        rename_over_post },
    { "rename-move",    rename_move_setup, rename_move_action,
                        rename_move_post },
    { "rename-dir",     rename_dir_setup,  rename_dir_action,
                        rename_dir_post },
    { "rename-grow",    rename_grow_setup, rename_grow_action,
                        rename_grow_post },
    { "rename-grow-same", rename_grow_same_setup, rename_grow_same_action,
                        rename_grow_same_post },
    { "rename-case",    rename_case_setup, rename_case_action,
                        rename_case_post },
    { "newdir",         setup_common,   newdir_action,   newdir_post },
    { "dirgrow",        dirgrow_setup,  dirgrow_action,  no_post },
    { "mkdir-grow",     dirgrow_setup,  mkdir_grow_action, no_post },
    { "trunc",          trunc_setup,    trunc_action,    no_post },
    { "trunc-zero",     trunc_setup,    zero_action,     no_post },
    { "full",           full_setup,     full_action,     full_post },
};

static const struct scenario *find(const char *name)
{
    for (unsigned i = 0; i < sizeof scenarios / sizeof *scenarios; i++)
        if (!strcmp(scenarios[i].name, name))
            return &scenarios[i];
    fprintf(stderr, "fstest: no scenario %s\n", name);
    exit(2);
}

/* The cache writes a volume's dirty sectors back in the order of the slots
   they hold, and which slot a sector takes depends on all that came before:
   the slots are dealt in another order for each seed, by taking every buffer
   and giving them back shuffled. Seed 0 leaves them as a boot does. */
static void deal_slots(unsigned seed)
{
    static void *buf[1024];
    int n = 0;
    while (seed && n < 1024 && (buf[n] = dc_get_buffer()))
        n++;
    srand(seed);
    for (int i = n - 1; i > 0; i--)
    {
        int j = rand() % (i + 1);
        void *t = buf[i];
        buf[i] = buf[j];
        buf[j] = t;
    }
    for (int i = 0; i < n; i++)
        dc_release_buffer(buf[i]);
}

/* the next writer on the disk takes every free cluster */
static void fill_free(void)
{
    static char buf[CLUSTER];
    bytes(buf, sizeof buf, 99, 0);
    int fd = open("/fill.bin", O_CREAT|O_TRUNC|O_WRONLY, 0666);
    if (fd < 0)
        fail("fill");
    while (write(fd, buf, sizeof buf) == sizeof buf)
        ;
    close(fd);
}

static void errno_test(void)
{
    static const struct { const char *path; int oflag; const char *want; }
    t[] =
    {
        { "/no such file", O_RDONLY, "ENOENT" },
        { CONFIGFILE, O_CREAT|O_EXCL|O_WRONLY, "EEXIST" },
        { ROCKBOX_DIR, O_WRONLY, "EISDIR" },
        { CONFIGFILE, O_RDONLY|O_TRUNC, "EINVAL" },
    };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++)
    {
        errno = 0;
        int fd = open(t[i].path, t[i].oflag, 0666);
        int e = errno;
        printf("open(%s): fd %d, errno %s, %s expected\n", t[i].path, fd,
               e == ENOENT ? "ENOENT" : e == EEXIST ? "EEXIST" :
               e == EISDIR ? "EISDIR" : e == EINVAL ? "EINVAL" :
               e == EBADF ? "EBADF" : e ? "other" : "0", t[i].want);
        if (fd >= 0)
            close(fd);
    }
}

/* The FAT's sectors each write past a full volume's last cluster reads, the
   disk cache emptied before it as a large disk's FAT doesn't fit in it: the
   whole FAT once, again after a mount, then only the file's own sector of
   it. On a fresh image of more clusters. */
static bool full_counted_write(int fd, uint64_t data, long *reads)
{
    dc_lock_cache();
    dc_commit_all(IF_MV(0));
    dc_discard_all(IF_MV(0));
    dc_unlock_cache();
    host_count_reads(data / 512);
    errno = 0;
    bool ok = full_write(fd);
    *reads = host_counted_reads();
    if (!ok && errno != ENOSPC)
        fail("ENOSPC");
    return ok;
}

static void full_reads(const char *what, int fd, uint64_t data)
{
    long reads;
    printf("full: %s, sectors of the FAT read by each write that failed:",
           what);
    for (int i = 0; i < 3; i++)
    {
        if (full_counted_write(fd, data, &reads))
            fail("full");
        printf(" %ld", reads);
    }
    printf("\n");
}

static void full_test(const char *img)
{
    uint64_t start, data;
    long reads;
    fs_mount(img);
    fat_get_offsets(IF_MV(0,) &start, &data);
    if (mkdir(ROCKBOX_DIR) < 0)
        fail("mkdir");
    full_fill();
    int fd = open(FULL_FILE, O_CREAT|O_TRUNC|O_WRONLY, 0666);
    if (fd < 0)
        fail("open");
    int written = 0;
    while (full_write(fd))
        written++;
    printf("full: %d writes of 8 KB until the volume was full; the FAT has "
           "%ld sectors\n", written, (long)(data / 512 - 32) / 2);
    full_reads("then", fd, data);
    remove(FULL_SPARE);
    written = 0;
    while (full_counted_write(fd, data, &reads))
        written++;
    printf("full: after a file of 2 clusters was removed, %d more writes "
           "(8 expected), the next failed one read %ld sectors\n", written,
           reads);
    close(fd);
    fs_unmount();
    fs_mount(img);
    fd = open(FULL_FILE, O_WRONLY);
    if (fd < 0)
        fail("open");
    full_reads("after a mount", fd, data);
    close(fd);
    fs_unmount();
}

int main(int argc, char *argv[])
{
    const char *cmd = argc > 1 ? argv[1] : "";
    char verdict[200];

    if (!strcmp(cmd, "list"))
    {
        for (unsigned i = 0; i < sizeof scenarios / sizeof *scenarios; i++)
            printf("%s\n", scenarios[i].name);
    }
    else if (!strcmp(cmd, "setup") && (argc == 4 || argc == 5))
    {
        layout = argc == 5 ? atoi(argv[4]) : 0;
        fs_mount(argv[3]);
        find(argv[2])->setup();
        fs_unmount();
    }
    else if (!strcmp(cmd, "action") && (argc == 5 || argc == 6))
    {
        fs_mount(argv[3]);
        deal_slots(argc == 6 ? atoi(argv[5]) : 0);
        host_journal(argv[4]);
        find(argv[2])->action();
        long n = host_journal_count();
        fs_unmount();
        printf("%ld %d\n", n, one_sector);
    }
    else if (!strcmp(cmd, "crash") && (argc == 8 || argc == 9))
    {
        const struct scenario *s = find(argv[2]);
        one_sector = argc == 9 && atoi(argv[8]);
        host_clone(argv[3], argv[6]);
        host_apply(argv[4], argv[6], atol(argv[5]));
        host_clone(argv[6], argv[7]);
        fs_mount(argv[7]);
        bool ok = s->post(verdict, sizeof verdict);
        /* FSInfo's count may be off after a cut: fat.c built for tests
           panics then where the firmware's finds the disk full */
        fat_recalc_free(IF_MV(0));
        fill_free();
        fs_unmount();
        printf("%s %s\n", ok ? "ok" : "BAD", verdict);
    }
    else if (!strcmp(cmd, "errno") && argc == 3)
    {
        fs_mount(argv[2]);
        errno_test();
        fs_unmount();
    }
    else if (!strcmp(cmd, "full") && argc == 3)
        full_test(argv[2]);
    else
    {
        fprintf(stderr, "usage: see the top of fstest.c\n");
        return 2;
    }
    return 0;
}
