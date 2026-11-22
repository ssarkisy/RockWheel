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
   fstest enospc <img>
       after "setup dirgrow": errno of a create that finds the folder full
       on a full volume
   fstest full <img>
   fstest two <img>
       after "setup full": a second stream's write fails, having taken a
       cluster; is the stream right once the first one's sync gave it back
   fstest wipe <img>
       removes every file and folder: what the FAT has taken then has leaked
   fstest damage <operation> <img>
       of the file whose chain or entry fatimg.py damaged after "setup
       damage": remove, trunc (written again), cfg (written again as the
       settings are), touch (opened to be written, and closed), cut (to
       80000 bytes, in its third cluster), read; or rmdir of the folder
   fstest mark <img>
       after "setup damage": a file cut at a sync, its chain then linked
       into the other file's, removed while open
   fstest fill <img>
       the next writer takes every free cluster: is the other file whole
   fstest cut <img> <journal> <k> <out>
       out: img after the first k writes of the journal, nothing more
   fstest ls <img>
       every folder's entries as readdir() gives them: name, attributes, size
   fstest l1 <img>
       the name readdir() gives a short-only entry after long entries and a
       free one (fat_readdir()'s parse reset)
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

/* file_write.c's: none of the volume's files is the library's */
void library_write_failed(void);
void library_write_failed(void)
{
}
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
/* settings_save_new()'s wait for a save under way: one thread here */
#define sleep(ticks) ((void)(ticks))
#ifndef ARRAYLEN
#define ARRAYLEN(a) (sizeof(a) / sizeof(*(a)))
#endif

/* the functions of apps/settings.c that save and recover the settings, and
   apps/file_write.c in a tree that has it */
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
#if defined(HAVE_SETTINGS_FILE_RECOVER) && !defined(HAVE_SETTINGS_FILE_REPLACE)
    /* .new raised by file_write.c's fw_recover(), .old its own */
    (void)old;
    settings_file_recover(file, new);
#elif defined(HAVE_SETTINGS_FILE_RECOVER)
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

static const char *const dirs[] = { "rocks", "codecs", "langs", "wps",
                                    "themes", "fonts", "playlists" };

/* a .rockbox of a used iPod ahead of what a scenario writes, and a file
   that brings the clusters it allocates up to the FAT's second sector */
static void setup_common(void)
{
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

/* written again shorter, by a cluster: under 64 KB the tail is freed at the
   close, not at the open */
#define MIDFILE ROCKBOX_DIR "/mid.dat"

static void trunc_less_setup(void)
{
    setup_common();
    put_file(MIDFILE, 50000, 14, 4096);
}

static void trunc_less_action(void)
{
    put_file(MIDFILE, 10000, 15, 4096);
}

/* cut to a length that keeps two of its four clusters */
static void ftruncate_action(void)
{
    int fd = open(BIGFILE, O_RDWR);
    if (fd < 0)
        return;
    ftruncate(fd, 40000);
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

/* Writes larger than a cluster on a volume that fills up: the write that
   fails has taken clusters by then, which the size doesn't count. The close
   gives them back; before, a new file's were lost, and an older file kept
   a chain longer than its size */
#define BIG_CHUNK 100000

static bool big_write(int fd, size_t size)
{
    static char buf[BIG_CHUNK];
    off_t at = lseek(fd, 0, SEEK_END);
    bytes(buf, size, FULL_SEED, at);
    return write(fd, buf, size) == (ssize_t)size;
}

/* the file's first write is the one that fails */
static void full_first_action(void)
{
    int fd = open(FULL_FILE, O_CREAT|O_TRUNC|O_WRONLY, 0666);
    if (fd < 0)
        return;
    big_write(fd, BIG_CHUNK);
    close(fd);
}

/* a file with 20000 bytes in it */
static void full_big_action(void)
{
    int fd = open(FULL_FILE, O_CREAT|O_TRUNC|O_WRONLY, 0666);
    if (fd < 0)
        return;
    big_write(fd, 20000);
    big_write(fd, BIG_CHUNK);
    close(fd);
}

#ifdef HAVE_FILE_WRITE
/* A file of ours written whole (file_write.c) over the one before: FW_KEEP's
   is that one or the new one after the next boot's fw_recover(), never none,
   a part, or the name twice; FW_CACHE's may be none, which is made again. A
   playlist made on the iPod is in a folder of its own. */
#define FW_FILE     ROCKBOX_DIR "/itunesdb_unsent.dat"
#define FW_LIST     MOVE_DIR "/Road Trip.m3u8"
#define FW_OLD_SIZE 30000
#define FW_NEW_SIZE 46000
#define FW_OLD_SEED 31
#define FW_NEW_SEED 32

static const char *fw_path = FW_FILE;

/* what fw_put() is given: 40 writes of a size that falls across sectors */
static bool fw_write_file(enum fw_policy policy)
{
    static char buf[32768], room[8192];
    struct fw w;
    fw_open(&w, fw_path, policy, room, sizeof room);
    for (size_t at = 0; at < FW_NEW_SIZE; at += 1150)
    {
        size_t n = MIN(FW_NEW_SIZE - at, 1150);
        bytes(buf, n, FW_NEW_SEED, at);
        fw_put(&w, buf, n);
    }
    return fw_close(&w, true);
}

static void fst_names(char *tmp, char *new, char *old)
{
    snprintf(tmp, MAX_PATH, "%s.tmp", fw_path);
    snprintf(new, MAX_PATH, "%s.new", fw_path);
    snprintf(old, MAX_PATH, "%s.old", fw_path);
}

static void keep_setup(void)
{
    setup_common();
    put_file(fw_path, FW_OLD_SIZE, FW_OLD_SEED, 4096);
}

static void keep_list_setup(void)
{
    fw_path = FW_LIST;
    setup_common();
    if (mkdir(MOVE_DIR) < 0)
        fail("mkdir");
    put_file(FW_LIST, FW_OLD_SIZE, FW_OLD_SEED, 4096);
}

static void keep_action(void)
{
    fw_write_file(FW_KEEP);
}

static void keep_list_action(void)
{
    fw_path = FW_LIST;
    keep_action();
}

static void cache_action(void)
{
    fw_write_file(FW_CACHE);
}

/* the name's file: "old", "new", "none" or "bad"; leftovers counted */
static const char *fw_state(int *left)
{
    char tmp[MAX_PATH], new[MAX_PATH], old[MAX_PATH];
    fst_names(tmp, new, old);
    int fd;
    *left = 0;
    const char *const names[] = { tmp, new, old };
    for (int i = 0; i < 3; i++)
        if ((fd = open(names[i], O_RDONLY)) >= 0)
        {
            close(fd);
            (*left)++;
        }
    return whole(fw_path, FW_OLD_SIZE, FW_OLD_SEED) > 0 ? "old" :
           whole(fw_path, FW_NEW_SIZE, FW_NEW_SEED) > 0 ? "new" :
           whole(fw_path, 1, 0) == 0 ? "none" : "bad";
}

/* the boot: fw_recover() of the file, as fw_recover_files() does for one
   that left something (itunesdb_files_boot(), lp_files_boot()) */
static bool keep_post_of(bool none_ok, char *verdict, size_t max)
{
    fw_recover(fw_path);
    int left;
    const char *state = fw_state(&left);
    snprintf(verdict, max, "%s%s", state, left ? ", leftovers" : "");
    return !left && (!strcmp(state, "old") || !strcmp(state, "new")
                     || (none_ok && !strcmp(state, "none")));
}

static bool keep_post(char *verdict, size_t max)
{
    return keep_post_of(false, verdict, max);
}

static bool keep_list_post(char *verdict, size_t max)
{
    fw_path = FW_LIST;
    return keep_post_of(false, verdict, max);
}

/* none before: none or the new one */
static void keep_first_setup(void)
{
    setup_common();
}

static bool keep_first_post(char *verdict, size_t max)
{
    return keep_post_of(true, verdict, max);
}

/* the boot after a cut that left the new one whole beside the old: the
   boot's own raise of it cut */
static void keep_boot_setup(void)
{
    char tmp[MAX_PATH], new[MAX_PATH], old[MAX_PATH];
    fst_names(tmp, new, old);
    keep_setup();
    put_file(new, FW_NEW_SIZE, FW_NEW_SEED, 4096);
}

/* a cut inside the rename of the old one to .old: the new one alone */
static void keep_lone_setup(void)
{
    char tmp[MAX_PATH], new[MAX_PATH], old[MAX_PATH];
    fst_names(tmp, new, old);
    setup_common();
    put_file(new, FW_NEW_SIZE, FW_NEW_SEED, 4096);
}

/* a cut inside the rename of the new one to the name: the old one alone */
static void keep_old_setup(void)
{
    char tmp[MAX_PATH], new[MAX_PATH], old[MAX_PATH];
    fst_names(tmp, new, old);
    setup_common();
    put_file(old, FW_OLD_SIZE, FW_OLD_SEED, 4096);
}

static void recover_action(void)
{
    fw_recover(fw_path);
}

/* a cache: the old one, the new one or none, never a part */
static bool cache_post(char *verdict, size_t max)
{
    int left;
    const char *state = fw_state(&left);
    snprintf(verdict, max, "%s", state);
    return strcmp(state, "bad") != 0;
}
#endif /* HAVE_FILE_WRITE */

/* A folder's end mark, an entry starting with 0x00, with live entries after
   it. A cut leaves it where a new run of entries at the folder's end crosses
   two sectors and the cache writes the later one first; another system's cut
   may leave it too. By the FAT spec nothing follows the end mark: the
   entries after it are free, and no reader shows them. Our code must grow
   such a folder without a panic, show none of them, and find its own new
   files. */
#define UNSENT ROCKBOX_DIR "/itunesdb_unsent.dat"

static long first_cluster(const char *path);

/* the cache written and emptied, so that the image can be edited below it */
static void cache_drop(void)
{
    dc_lock_cache();
    dc_commit_all(IF_MV(0));
    dc_discard_all(IF_MV(0));
    dc_unlock_cache();
}

/* the image's sector of entry i of the folder whose first cluster is c */
static uint64_t entry_sector(long c, unsigned long i)
{
    uint64_t start, data;
    uint32_t fat[512 / 4];
    fat_get_offsets(IF_MV(0,) &start, &data);
    for (unsigned long n = i / (CLUSTER / 32); n; n--)
    {
        if (host_read(32 + c / 128, 1, fat) < 0)
            fail("FAT");
        c = fat[c % 128] & 0x0fffffff;
    }
    return data / 512 + (uint64_t)(c - 2) * (CLUSTER / 512)
           + i % (CLUSTER / 32) / 16;
}

/* entries from..to of folder dir all zeros on the disk, as the clearing of
   a new cluster leaves them */
static void zero_entries(const char *dir, unsigned long from,
                         unsigned long to)
{
    uint8_t sec[512];
    long c = first_cluster(dir);
    cache_drop();
    for (unsigned long i = from; i <= to; i++)
    {
        uint64_t s = entry_sector(c, i);
        if (host_read(s, 1, sec) < 0)
            fail("read");
        memset(sec + i % 16 * 32, 0, 32);
        if (host_write(s, 1, sec) < 0)
            fail("write");
    }
}

/* the places of path's first entry and its short entry in its folder */
static void entry_run(const char *path, unsigned long *first,
                      unsigned long *last)
{
    struct filestr_base stream;
    if (open_stream_internal(path, FF_ANYTYPE, &stream, NULL) <= 0)
        fail(path);
    const struct fat_dirscan_info *e = &stream.infop->fatfile.e;
    *first = e->entry - e->entries + 1;
    *last = e->entry;
    close_stream_internal(&stream);
}

/* Every name the folder shows: one that may be there by allowed(), and, for
   database_<n>.tcd and Track <n>.m4a, its file whole. The first that may not
   goes to the verdict; how many of the numbered ones to count. */
static bool listing_ok(const char *dir,
                       bool (*allowed)(const char *, const char *),
                       int *count, char *verdict, size_t max)
{
    char path[MAX_PATH];
    DIR *d = opendir(dir);
    if (!d)
        return snprintf(verdict, max, "%s unreadable", dir), false;
    struct dirent *e;
    bool ok = true;
    *count = 0;
    while (ok && (e = readdir(d)))
    {
        int n;
        char c;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (sscanf(e->d_name, "database_%d.tc%c", &n, &c) == 2 && c == 'd')
            ok = same_file(path, 2000 + n * 100, n);
        else if (sscanf(e->d_name, "Track %d.m4%c", &n, &c) == 2 && c == 'a')
            ok = n < 508 && same_file(path, 0, n);
        else if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        else
            ok = allowed(e->d_name, path);
        if (!ok)
            snprintf(verdict, max, "%s shows %s", dir, e->d_name);
        (*count)++;
    }
    closedir(d);
    return ok;
}

static bool rockbox_name(const char *name, const char *path)
{
    (void)path;
    for (unsigned i = 0; i < sizeof dirs / sizeof *dirs; i++)
        if (!strcmp(name, dirs[i]))
            return true;
    return !strcmp(name, "itunesdb_unsent.dat") ||
           !strcmp(name, "itunesdb_unsent.dat.tmp") ||
           !strcmp(name, "itunesdb_unsent.dat.new") ||
           !strcmp(name, "itunesdb_unsent.dat.old") ||
           !strcmp(name, "a new folder of ours") ||
           !strcmp(name, "emptied");
}

/* what a file is: "whole", "none", or "part" if it may be cut short (its
   write cut) and isn't whole; "bad" if it may not */
static const char *file_state(const char *path, size_t size, unsigned seed,
                              bool part, bool *ok)
{
    int w = whole(path, size, seed);
    if (w < 0 && !part)
        *ok = false;
    return w > 0 ? "whole" : !w ? "none" : part ? "part" : "bad";
}

/* the next boot writes a file and renames it over the name, as settings and
   the import do in .rockbox at each boot; it must succeed and be found */
static bool boot_write(char *verdict, size_t max)
{
    put_file(UNSENT ".tmp", 2000, 3, 4096);
    int rc = rename(UNSENT ".tmp", UNSENT);
    bool ok = rc == 0 && same_file(UNSENT, 2000, 3);
    snprintf(verdict, max, "the boot's rename %d, %s", rc,
             ok ? "found" : "NOT FOUND");
    return ok;
}

/* the old one to .old, the new one to the name, .old removed: plain calls,
   as fstest-plain-end-scenario.diff of health2 */
static void plain_setup(void)
{
    setup_common();
    put_file(UNSENT, 30000, 31, 4096);
    put_file(UNSENT ".new", 46000, 32, 4096);
}

static void plain_action(void)
{
    remove(UNSENT ".old");
    rename(UNSENT, UNSENT ".old");
    rename(UNSENT ".new", UNSENT);
    remove(UNSENT ".old");
}

static bool plain_post(char *verdict, size_t max)
{
    char line[100], boot[100];
    bool ok = true;
    int n;
    int old = whole(UNSENT, 30000, 31), new = whole(UNSENT, 46000, 32);
    snprintf(line, sizeof line, "name %s, .new %s, .old %s",
             old > 0 ? "old" : new > 0 ? "new" : !old ? "none" : "bad",
             file_state(UNSENT ".new", 46000, 32, false, &ok),
             file_state(UNSENT ".old", 30000, 31, false, &ok));
    ok = ok && (old > 0 || new >= 0);
    if (!listing_ok(ROCKBOX_DIR, rockbox_name, &n, verdict, max))
        return false;
    ok = boot_write(boot, sizeof boot) && ok;
    if (!listing_ok(ROCKBOX_DIR, rockbox_name, &n, verdict, max))
        return false;
    snprintf(verdict, max, "%s; %s", line, boot);
    return ok;
}

/* a cut's: the run of a file at the folder's end, its entries in the
   sectors before its short entry's zeros, or its first entry if all are in
   one sector; 4 entries, which cross a sector in one layout */
#define HIDDEN_CUT ROCKBOX_DIR "/hidden by a cut, a longer name.dat"

static void stray_cut_setup(void)
{
    unsigned long first, last;
    setup_common();
    put_file(HIDDEN_CUT, 20000, 41, 4096);
    entry_run(HIDDEN_CUT, &first, &last);
    zero_entries(ROCKBOX_DIR, first,
                 last / 16 * 16 > first ? last / 16 * 16 - 1 : first);
}

/* another system's: a whole file's entries after the end mark, the zeros
   before them two entries or, in odd layouts, three */
#define HIDDEN_RUN ROCKBOX_DIR "/hidden whole file.dat"

static void stray_run_setup(void)
{
    unsigned long first, last;
    const char *gone = layout & 1 ? ROCKBOX_DIR "/a gone file.dat" :
                                    ROCKBOX_DIR "/gone.dat";
    setup_common();
    put_file(gone, 100, 40, 100);
    put_file(HIDDEN_RUN, 20000, 41, 4096);
    entry_run(gone, &first, &last);
    zero_entries(ROCKBOX_DIR, first, last);
}

/* our code grows it: a file written and renamed to its name */
static void stray_action(void)
{
    put_file(UNSENT ".tmp", 46000, 32, 4096);
    rename(UNSENT ".tmp", UNSENT);
}

static bool stray_post(char *verdict, size_t max)
{
    char line[100], boot[100];
    bool ok = true;
    int n;
    snprintf(line, sizeof line, "name %s, .tmp %s",
             file_state(UNSENT, 46000, 32, false, &ok),
             file_state(UNSENT ".tmp", 46000, 32, true, &ok));
    if (!listing_ok(ROCKBOX_DIR, rockbox_name, &n, verdict, max))
        return false;
    ok = boot_write(boot, sizeof boot) && ok;
    if (!listing_ok(ROCKBOX_DIR, rockbox_name, &n, verdict, max))
        return false;
    snprintf(verdict, max, "%s; %s", line, boot);
    return ok;
}

/* a folder made there */
static void stray_mkdir_action(void)
{
    mkdir(ROCKBOX_DIR "/a new folder of ours");
}

static bool stray_mkdir_post(char *verdict, size_t max)
{
    char boot[100];
    int n;
    if (!listing_ok(ROCKBOX_DIR, rockbox_name, &n, verdict, max))
        return false;
    bool ok = boot_write(boot, sizeof boot);
    if (!listing_ok(ROCKBOX_DIR, rockbox_name, &n, verdict, max))
        return false;
    snprintf(verdict, max, "folder %s; %s",
             dir_exists(ROCKBOX_DIR "/a new folder of ours") ? "made" : "none",
             boot);
    return ok;
}

/* rmdir of a folder whose only file is behind its end mark: the folder is
   empty to every reader, and goes; the file's cluster stays lost */
#define EMPTIED ROCKBOX_DIR "/emptied"

static void stray_rmdir_setup(void)
{
    unsigned long first, last;
    setup_common();
    if (mkdir(EMPTIED) < 0)
        fail("mkdir");
    put_file(EMPTIED "/hidden whole file.dat", 20000, 41, 4096);
    entry_run(EMPTIED "/hidden whole file.dat", &first, &last);
    zero_entries(EMPTIED, first, first);
}

static void stray_rmdir_action(void)
{
    rmdir(EMPTIED);
}

static bool stray_rmdir_post(char *verdict, size_t max)
{
    char boot[100];
    int n;
    const bool there = dir_exists(EMPTIED);
    if (!listing_ok(ROCKBOX_DIR, rockbox_name, &n, verdict, max))
        return false;
    bool ok = !there || rmdir(EMPTIED) == 0;
    ok = boot_write(boot, sizeof boot) && ok;
    if (!listing_ok(ROCKBOX_DIR, rockbox_name, &n, verdict, max))
        return false;
    snprintf(verdict, max, "folder %s%s; %s", there ? "there" : "gone",
             ok ? "" : ", NOT REMOVED", boot);
    return ok;
}

/* Across clusters: /Music's first cluster full, two files in its second;
   the last three of the first removed, and the first of those made the end
   mark, so the second cluster's entries are after it. A name of seven
   entries doesn't fit before the cluster's end: the folder's second cluster
   is cleared and taken again. One of six fits, and ends at the second
   cluster's first entry. */
#define GROW_SIX   GROWDIR "/Track 600, a song whose name takes six entries " \
                   "in all.m4a"
#define GROW_SEVEN GROWDIR "/Track 601, a song whose name takes seven " \
                   "entries in the folder all.m4a"
#define GROW_BOOT  GROWDIR "/After the cut.m4a"

static void stray_grow_setup(void)
{
    char path[MAX_PATH];
    dirgrow_setup();
    put_file(GROWDIR "/Track 511.m4a", 0, 511, 1);
    put_file(GROWDIR "/Track 512.m4a", 0, 512, 1);
    for (int i = 508; i <= 510; i++)
    {
        snprintf(path, sizeof path, GROWDIR "/Track %03d.m4a", i);
        if (remove(path) < 0)
            fail(path);
    }
    /* '.', '..', then two entries a file: Track 508's first is 1018 */
    zero_entries(GROWDIR, 1018, 1018);
}

static void stray_grow_action(void)
{
    put_file(GROW_SEVEN, 100, 601, 100);
}

static void stray_grow_fit_action(void)
{
    put_file(GROW_SIX, 100, 600, 100);
}

static bool grow_name(const char *name, const char *path)
{
    (void)path;
    return !strcmp(name, strrchr(GROW_SIX, '/') + 1) ||
           !strcmp(name, strrchr(GROW_SEVEN, '/') + 1) ||
           !strcmp(name, strrchr(GROW_BOOT, '/') + 1);
}

static bool stray_grow_post(char *verdict, size_t max)
{
    bool ok = true;
    int n;
    const char *six = file_state(GROW_SIX, 100, 600, true, &ok),
               *seven = file_state(GROW_SEVEN, 100, 601, true, &ok);
    if (!listing_ok(GROWDIR, grow_name, &n, verdict, max))
        return false;
    put_file(GROW_BOOT, 100, 700, 100);
    ok = ok && same_file(GROW_BOOT, 100, 700);
    int after;
    if (!listing_ok(GROWDIR, grow_name, &after, verdict, max))
        return false;
    snprintf(verdict, max, "six %s, seven %s, %d names then %d", six, seven,
             n, after);
    return ok && after == n + 1;
}

/* A rename in one folder whose old and new short entries share a sector
   (fat.c's "apart" false), the new run starting in the sector before, in a
   gap of freed entries: that sector is written once, with the old entries
   freed and the new short one (afc96dd7bd). A cut leaves the file under the
   old name, the new one or the new 8.3 name, never none. From review 1. */
#define RX_DIR  "/T"
#define RX_FROM RX_DIR "/Road Trip.m3u8.tmp"
#define RX_TO   RX_DIR "/Road Trip.m3u8"

static void rename_cross_setup(void)
{
    char path[MAX_PATH];
    unsigned long first, last;
    put_file("/filler.bin", 10 * CLUSTER, 1, 65536);
    if (mkdir(RX_DIR) < 0)
        fail("mkdir");
    for (int i = 0; i < 6; i++)
    {
        snprintf(path, sizeof path, RX_DIR "/f%d.dat", i);
        put_file(path, 100, 50 + i, 100);
    }
    put_file(RX_DIR "/g0.dat", 100, 60, 100);
    put_file(RX_DIR "/g1.dat", 100, 61, 100);
    put_file(RX_FROM, RN_SIZE, RN_SEED, 4096);
    /* '.', '..', f0-f5 at 2-13, g0 and g1 at 14-17, the file at 18-20;
       the new name then takes 14-16, across sectors 0 and 1 */
    entry_run(RX_FROM, &first, &last);
    if (first != 18 || last != 20)
        fail("layout");
    if (remove(RX_DIR "/g0.dat") < 0 || remove(RX_DIR "/g1.dat") < 0)
        fail("remove");
}

static void rename_cross_action(void)
{
    rename(RX_FROM, RX_TO);
}

/* the file's bytes under exactly one name, 8.3 names counted; nothing else
   but f0-f5 */
static bool rename_cross_post(char *verdict, size_t max)
{
    char path[MAX_PATH], names[160] = "";
    int found = 0;
    bool other = false;
    DIR *d = opendir(RX_DIR);
    if (!d)
        return snprintf(verdict, max, "folder unreadable"), false;
    struct dirent *e;
    while ((e = readdir(d)))
    {
        int n;
        char c;
        snprintf(path, sizeof path, RX_DIR "/%s", e->d_name);
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ||
            (sscanf(e->d_name, "f%d.da%c", &n, &c) == 2 && c == 't' &&
             same_file(path, 100, 50 + n)))
            continue;
        if (same_file(path, RN_SIZE, RN_SEED))
            found++;
        else
        {
            other = true;
            strlcat(names, "?", sizeof names);
        }
        strlcat(names, e->d_name, sizeof names);
        strlcat(names, "|", sizeof names);
    }
    closedir(d);
    snprintf(verdict, max, "%s %s", found == 1 ? "under" : found ? "TWICE" :
             "LOST", names);
    return found == 1 && !other;
}

/* A remove whose run crosses a sector (layouts 0-3) or a cluster (4-7),
   with 1 to 4 of its 5 entries before the boundary. Its short entry's sector
   goes to the disk first: a cut leaves the file under its name or gone,
   never under its 8.3 name, and nothing else changes. The entries ahead of
   it are another system's: 8.3 names in lower case as Windows writes them,
   an entry each (so that the run can start anywhere); a file of ours
   follows it. */
#define RM_DIR   "/R"
#define RM_FILE  RM_DIR "/Track 07, a song whose name takes five.m4a"
#define RM_AFTER RM_DIR "/after.dat"
#define RM_BOOT  RM_DIR "/next boot.dat"
#define RM_SIZE  40000
#define RM_SEED  70

static unsigned long rm_split(void)
{
    return 1 + layout % 4;
}

static unsigned long rm_first(void)
{
    return (layout < 4 ? 16 : CLUSTER / 32) - rm_split();
}

static void remove_cross_setup(void)
{
    uint8_t sec[512];
    unsigned long first, last;
    put_file("/filler.bin", 10 * CLUSTER, 1, 65536);
    if (mkdir(RM_DIR) < 0)
        fail("mkdir");
    /* entries 2 to the run's first: f0002.dat on, short ones, NTRes lower
       case, empty */
    long c = first_cluster(RM_DIR);
    cache_drop();
    for (unsigned long i = 2; i < rm_first(); i++)
    {
        uint64_t s = entry_sector(c, i);
        if (host_read(s, 1, sec) < 0)
            fail("read");
        uint8_t *e = sec + i % 16 * 32;
        memset(e, 0, 32);
        snprintf((char *)e, 12, "F%04lu   DAT", i);
        e[11] = 0x20;                   /* archive */
        e[12] = 0x18;                   /* name and extension lower case */
        e[24] = 0x21;                   /* 2016-01-01 */
        e[25] = 0x48;
        if (host_write(s, 1, sec) < 0)
            fail("write");
    }
    put_file(RM_FILE, RM_SIZE, RM_SEED, 4096);
    put_file(RM_AFTER, 100, 71, 100);
    entry_run(RM_FILE, &first, &last);
    if (first != rm_first() || last != first + 4)
        fail("layout");
}

static void remove_cross_action(void)
{
    remove(RM_FILE);
}

/* the run's first entry as the post finds it: after.dat's, 5 entries on,
   tells it (a crash's run has no layout) */
static unsigned long rm_seen;

static bool remove_cross_name(const char *name, const char *path)
{
    unsigned long n;
    char c;
    if (sscanf(name, "f%lu.da%c", &n, &c) == 2 && c == 't')
        return n >= 2 && n < rm_seen && same_file(path, 0, 0);
    return (!strcmp(name, strrchr(RM_FILE, '/') + 1) &&
            same_file(path, RM_SIZE, RM_SEED)) ||
           (!strcmp(name, "after.dat") && same_file(path, 100, 71)) ||
           (!strcmp(name, "next boot.dat") && same_file(path, 100, 72));
}

static bool remove_cross_post(char *verdict, size_t max)
{
    int n, after;
    unsigned long last;
    entry_run(RM_AFTER, &rm_seen, &last);
    rm_seen -= 5;
    const bool there = file_exists(RM_FILE);
    if (!listing_ok(RM_DIR, remove_cross_name, &n, verdict, max))
        return false;
    /* the others: the f entries and after.dat, and the file if there */
    const int others = (int)rm_seen - 2 + 1;
    bool ok = n == others + there && same_file(RM_AFTER, 100, 71);
    put_file(RM_BOOT, 100, 72, 100);
    if (!listing_ok(RM_DIR, remove_cross_name, &after, verdict, max))
        return false;
    ok = ok && after == n + 1;
    snprintf(verdict, max, "the file %s, %d names then %d%s",
             there ? "there" : "gone", n, after, ok ? "" : ", WRONG COUNT");
    return ok;
}

/** The library scan's two files kept through a reset (apps/tagcache.c
    scan_checkpoint()): a song's entry and names through an 8 KB buffer, its
    Sort fields, for every third song, through a 1 KB one into a file made at
    the first; every SCAN_EVERY songs the Sort file's data synced, its header
    over the one at the start and synced again, then the same for the scan's
    file; at the end the same, the close syncing the header. The next boot
    takes a header only if its file holds all it counts, as commit() and
    load_sort_pairs() do, then removes both files, as the commit does. The
    rule: a header taken means all its data is there, and the Sort file's
    covers every song the scan file's does. **/
#define SCAN_TEMP    ROCKBOX_DIR "/database_tmp.tcd"
#define SCAN_SORT    ROCKBOX_DIR "/database_sort_tmp.tcd"
#define SCAN_SONGS   350
#define SCAN_EVERY   100
#define SCAN_MAGIC   0x52574403         /* TAGCACHE_MAGIC */
#define SORT_MAGIC   0x46534252         /* TAGCACHE_SORT_MAGIC */
#define SCAN_ENTRY   160                /* sizeof(struct temp_file_entry) */
#define SCAN_SEED    41                 /* of the bytes after each header */
#define SORT_SEED    42

/* a song's names, and its Sort pair's size, if it has one */
static long scan_names(int i)
{
    return 140 + (i * 37) % 120;
}

static long scan_pair(int i)
{
    return i % 3 ? 0 : 30 + (i * 11) % 50;
}

static int scan_fd = -1, sort_fd = -1;
static char scan_buf[8192], sort_buf[1024];
static size_t scan_used, sort_used;
static long scan_at, sort_at;           /* the bytes after the headers */

static void scan_flush(int fd, char *buf, size_t *used)
{
    if (*used && write(fd, buf, *used) != (ssize_t)*used)
        fail("scan write");
    *used = 0;
}

/* n more bytes of a file's stream through its buffer */
static void scan_put(int fd, char *buf, size_t size, size_t *used,
                     long *at, unsigned seed, long n)
{
    static char data[512];
    if (*used + n > size)
        scan_flush(fd, buf, used);
    bytes(data, n, seed, *at);
    memcpy(buf + *used, data, n);
    *used += n;
    *at += n;
}

/* the data synced, then the header; at the end, the close syncs it */
static void scan_header(int fd, const void *h, size_t size, bool end)
{
    if (fsync(fd) < 0 || lseek(fd, 0, SEEK_SET) != 0
        || write(fd, h, size) != (ssize_t)size
        || (!end && (fsync(fd) < 0 || lseek(fd, 0, SEEK_END) <= 0)))
        fail("scan header");
}

static void scan_headers(int songs, long datasize, long pairs, bool end)
{
    if (sort_fd >= 0)
    {
        const uint32_t h[6] = { SORT_MAGIC, 1, pairs, 0xffffffff, 0, 0 };
        scan_flush(sort_fd, sort_buf, &sort_used);
        scan_header(sort_fd, h, sizeof h, end);
        if (end)
            close(sort_fd);
    }
    const int32_t h[3] = { SCAN_MAGIC, datasize, songs };
    scan_flush(scan_fd, scan_buf, &scan_used);
    scan_header(scan_fd, h, sizeof h, end);
    if (end)
        close(scan_fd);
}

static void scan_action(void)
{
    static const int32_t none[3];
    scan_fd = open(SCAN_TEMP, O_RDWR|O_CREAT|O_TRUNC, 0666);
    if (scan_fd < 0 || write(scan_fd, none, sizeof none) != sizeof none)
        fail("scan open");
    scan_used = sort_used = 0;
    scan_at = sort_at = 0;
    long datasize = 0, pairs = 0;
    for (int i = 0; i < SCAN_SONGS; i++)
    {
        if (scan_pair(i))
        {
            if (sort_fd < 0)
            {
                static const uint32_t zero[6];
                sort_fd = open(SCAN_SORT, O_WRONLY|O_CREAT|O_TRUNC, 0666);
                if (sort_fd < 0
                    || write(sort_fd, zero, sizeof zero) != sizeof zero)
                    fail("sort open");
            }
            scan_put(sort_fd, sort_buf, sizeof sort_buf, &sort_used,
                     &sort_at, SORT_SEED, scan_pair(i));
            pairs++;
        }
        scan_put(scan_fd, scan_buf, sizeof scan_buf, &scan_used, &scan_at,
                 SCAN_SEED, SCAN_ENTRY);
        scan_put(scan_fd, scan_buf, sizeof scan_buf, &scan_used, &scan_at,
                 SCAN_SEED, scan_names(i));
        datasize += scan_names(i);
        if ((i + 1) % SCAN_EVERY == 0)
            scan_headers(i + 1, datasize, pairs, false);
    }
    /* the end of the scan: sort_scan_close(), then do_tagcache_build()'s */
    scan_headers(SCAN_SONGS, datasize, pairs, true);
    sort_fd = scan_fd = -1;
}

/* whether the n bytes after a header are the stream's */
static bool scan_same(int fd, long n, unsigned seed)
{
    static char got[4096], want[4096];
    for (long at = 0; at < n; )
    {
        long k = n - at < (long)sizeof got ? n - at : (long)sizeof got;
        if (read(fd, got, k) != k)
            return false;
        bytes(want, k, seed, at);
        if (memcmp(got, want, k))
            return false;
        at += k;
    }
    return true;
}

static bool scan_post(char *verdict, size_t max)
{
    long songs = -1, pairs = -1;
    const char *bad = NULL;
    int fd = open(SCAN_TEMP, O_RDONLY);
    if (fd >= 0)
    {
        int32_t h[3];
        const off_t size = ffilesize(fd);
        if (read(fd, h, sizeof h) == sizeof h && h[0] == SCAN_MAGIC
            && h[1] >= 0 && h[2] >= 0 && size >= 0
            && (uint64_t)size >= sizeof h + (uint64_t)h[2] * SCAN_ENTRY
                                 + (uint64_t)h[1])
        {
            long names = 0;
            for (int i = 0; i < h[2] && i < SCAN_SONGS; i++)
                names += scan_names(i);
            songs = h[2];
            if (h[2] > SCAN_SONGS || h[1] != names
                || !scan_same(fd, h[2] * SCAN_ENTRY + names, SCAN_SEED))
                bad = "the scan file's songs are not all there";
        }
        close(fd);
    }
    fd = open(SCAN_SORT, O_RDONLY);
    if (fd >= 0)
    {
        uint32_t h[6];
        if (read(fd, h, sizeof h) == sizeof h && h[0] == SORT_MAGIC
            && h[2] <= 0x100000)
        {
            long n = 0, k = 0;
            for (int i = 0; i < SCAN_SONGS && k < (long)h[2]; i++)
                if (scan_pair(i))
                {
                    n += scan_pair(i);
                    k++;
                }
            pairs = h[2];
            if (k < (long)h[2] || ffilesize(fd) < (off_t)sizeof h + n
                || !scan_same(fd, n, SORT_SEED))
                bad = "the Sort file's pairs are not all there";
        }
        close(fd);
    }
    /* the pairs of the songs taken: every third's, from the first */
    if (!bad && songs > 0 && (songs + 2) / 3 > (pairs < 0 ? 0 : pairs))
        bad = "songs taken without their Sort fields";
    remove(SCAN_TEMP);
    remove(SCAN_SORT);
    snprintf(verdict, max, "%s%ssongs %ld, pairs %ld", bad ? bad : "",
             bad ? ": " : "", songs, pairs);
    return !bad;
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
    { "trunc-less",     trunc_less_setup, trunc_less_action, no_post },
    { "ftruncate",      trunc_setup,    ftruncate_action, no_post },
    { "full",           full_setup,     full_action,     full_post },
    { "full-first",     full_setup,     full_first_action, full_post },
    { "full-big",       full_setup,     full_big_action, full_post },
#ifdef HAVE_FILE_WRITE
    { "keep",           keep_setup,     keep_action,     keep_post },
    { "keep-first",     keep_first_setup, keep_action,   keep_first_post },
    { "keep-list",      keep_list_setup, keep_list_action, keep_list_post },
    { "keep-boot",      keep_boot_setup, recover_action, keep_post },
    { "keep-lone",      keep_lone_setup, recover_action, keep_post },
    { "keep-old",       keep_old_setup, recover_action,  keep_post },
    { "cache",          keep_setup,     cache_action,    cache_post },
#endif
    { "plain-end",      plain_setup,    plain_action,    plain_post },
    { "stray-cut",      stray_cut_setup, stray_action,   stray_post },
    { "stray-run",      stray_run_setup, stray_action,   stray_post },
    { "stray-mkdir",    stray_run_setup, stray_mkdir_action,
                        stray_mkdir_post },
    { "stray-rmdir",    stray_rmdir_setup, stray_rmdir_action,
                        stray_rmdir_post },
    { "stray-grow",     stray_grow_setup, stray_grow_action, stray_grow_post },
    { "stray-grow-fit", stray_grow_setup, stray_grow_fit_action,
                        stray_grow_post },
    { "rename-cross",   rename_cross_setup, rename_cross_action,
                        rename_cross_post },
    { "remove-cross",   remove_cross_setup, remove_cross_action,
                        remove_cross_post },
    { "scan-checkpoint", setup_common,  scan_action,     scan_post },
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

/* Two streams of a file on a full volume. The second's write takes the last
   free cluster and fails; the first's sync gives the cluster back, and it
   goes to another file. The second stream must not write by the place it was
   left at, which is the other file's now. */
#define TWO_OTHER ROCKBOX_DIR "/other.dat"

static void two_test(void)
{
    static char buf[BIG_CHUNK], got[4096];
    remove(FULL_SPARE);     /* four clusters free: */
    put_file("/pad.bin", 3 * CLUSTER, 24, 4096);    /* one */
    int fd1 = open(FULL_FILE, O_CREAT|O_TRUNC|O_RDWR, 0666);
    int fd2 = open(FULL_FILE, O_WRONLY);
    if (fd1 < 0 || fd2 < 0)
        fail("open");
    bytes(buf, 20000, FULL_SEED, 0);
    if (write(fd1, buf, 20000) != 20000 || fsync(fd1) < 0)  /* none */
        fail("write");
    remove("/pad.bin");
    put_file("/pad.bin", 2 * CLUSTER, 24, 4096);    /* one */
    lseek(fd2, 16384, SEEK_SET);
    bytes(buf, sizeof buf, FULL_SEED, 16384);
    long failed = write(fd2, buf, sizeof buf);      /* takes it, and fails */
    if (fsync(fd1) < 0)                             /* gives it back */
        fail("fsync");
    put_file(TWO_OTHER, CLUSTER, 25, 4096);         /* which takes it */
    remove("/pad.bin");
    /* the file grows to where the second stream was left, by the first */
    long at = lseek(fd1, 0, SEEK_END);
    bytes(buf, 2 * CLUSTER - at, FULL_SEED, at);
    if (write(fd1, buf, 2 * CLUSTER - at) != 2 * CLUSTER - at)
        fail("write");
    lseek(fd2, 2 * CLUSTER, SEEK_SET);
    bytes(buf, 4096, FULL_SEED, 2 * CLUSTER);
    long more = write(fd2, buf, 4096);
    int c1 = close(fd1), c2 = close(fd2);
    /* the file: its bytes of each place, to its end */
    long size = 0, n;
    bool same = true;
    int fd = open(FULL_FILE, O_RDONLY);
    while (fd >= 0 && same && (n = read(fd, got, sizeof got)) > 0)
    {
        bytes(buf, n, FULL_SEED, size);
        same = !memcmp(got, buf, n);
        size += n;
    }
    close(fd);
    bool other = same_file(TWO_OTHER, CLUSTER, 25);
    printf("two streams: %s: the failed write %ld, the one after it %ld, "
           "closes %d %d, the file %s%ld bytes (%d expected), the other file "
           "%s\n", same && other && size == 2 * CLUSTER + 4096 && more == 4096
           && failed < 0 ? "ok" : "BAD", failed, more, c1, c2,
           same ? "" : "wrong at ", size, 2 * CLUSTER + 4096,
           other ? "whole" : "NOT WHOLE");
}

/* what stays taken once everything is removed has leaked */
static void wipe(const char *dir)
{
    char path[MAX_PATH];
    DIR *d = opendir(dir);
    if (!d)
        fail(dir);
    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir[1] ? dir : "", e->d_name);
        if (dir_get_info(d, e).attribute & ATTR_DIRECTORY)
        {
            wipe(path);
            if (rmdir(path) < 0)
                fail(path);
        }
        else if (remove(path) < 0)
            fail(path);
    }
    closedir(d);
}

/** Damage: a FAT whose chains name clusters wrongly (fatimg.py damage). An
    operation on the damaged file may lose space; it must not touch the
    other file, nor write anywhere by a number that is no cluster's. **/
#define DMG_FILE  ROCKBOX_DIR "/victim.dat"
#define DMG_OTHER ROCKBOX_DIR "/bystander.dat"
#define DMG_DIR   ROCKBOX_DIR "/emptydir"
#define DMG_SIZE  100000    /* four clusters, and over O_TRUNC_THRESH */

/* same_file() of a file that long */
static bool same_long(const char *path, long size, unsigned seed)
{
    static char got[4096], want[4096];
    long total = 0, n;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    while ((n = read(fd, got, sizeof got)) > 0)
    {
        bytes(want, n, seed, total);
        if (memcmp(got, want, n))
            break;
        total += n;
    }
    close(fd);
    return n == 0 && total == size;
}

static void damage_setup(void)
{
    setup_common();
    put_file(DMG_FILE, DMG_SIZE, 31, 4096);
    put_file(DMG_OTHER, DMG_SIZE, 32, 4096);
    if (mkdir(DMG_DIR) < 0)
        fail("mkdir");
}

static bool damage_op(const char *op, char *verdict, size_t max)
{
    bool ok;
    errno = 0;
    if (!strcmp(op, "remove") || !strcmp(op, "rmdir"))
    {
        const bool dir = op[1] == 'm';
        int rc = dir ? rmdir(DMG_DIR) : remove(DMG_FILE);
        int e = errno;
        bool gone = dir ? !dir_exists(DMG_DIR) : !file_exists(DMG_FILE);
        snprintf(verdict, max, "%s %d (errno %d), the name is %s", op, rc,
                 rc < 0 ? e : 0, gone ? "gone" : "there");
        ok = rc == 0 && gone;
    }
    else if (!strcmp(op, "trunc"))
    {
        /* as a file of the database is written again */
        static char buf[10000];
        bytes(buf, sizeof buf, 33, 0);
        int fd = open(DMG_FILE, O_TRUNC|O_WRONLY);
        int e = errno;
        long w = fd < 0 ? -1 : write(fd, buf, sizeof buf);
        int c = fd < 0 ? -1 : close(fd);
        ok = c == 0 && same_file(DMG_FILE, sizeof buf, 33);
        snprintf(verdict, max, "open %s (errno %d), write %ld, close %d, "
                 "the file is %s", fd < 0 ? "failed" : "ok", fd < 0 ? e : 0,
                 w, c, ok ? "the new one" : "not the new one");
    }
    else if (!strcmp(op, "cfg"))
    {
        static char got[65536], want[65536];
        bool saved = cfg_write(DMG_FILE, &cfg_b);
        int e = errno;
        size_t len = cfg_text(&cfg_b, want, sizeof want);
        ok = saved && get_file(DMG_FILE, got, sizeof got) == (long)len
             && !memcmp(got, want, len);
        snprintf(verdict, max, "the save %s (errno %d), the file is %s",
                 saved ? "went well" : "failed", saved ? 0 : e,
                 ok ? "the new one" : "not the new one");
    }
    else if (!strcmp(op, "cut"))
    {
        int fd = open(DMG_FILE, O_RDWR);
        int t = fd < 0 ? -1 : ftruncate(fd, 80000);
        int e = errno;
        int c = fd < 0 ? -1 : close(fd);
        ok = t == 0 && c == 0;
        snprintf(verdict, max, "ftruncate %s (errno %d), close %d",
                 t < 0 ? "failed" : "ok", t < 0 ? e : 0, c);
    }
    else if (!strcmp(op, "touch"))
    {
        int fd = open(DMG_FILE, O_WRONLY);
        int e = errno;
        int c = fd < 0 ? -1 : close(fd);
        ok = c == 0;
        snprintf(verdict, max, "open %s (errno %d), close %d",
                 fd < 0 ? "failed" : "ok", fd < 0 ? e : 0, c);
    }
    else if (!strcmp(op, "read"))
    {
        static char got[4096], want[4096];
        long total = 0, n = -1;
        bool right = true;
        int fd = open(DMG_FILE, O_RDONLY);
        long size = fd < 0 ? -1 : (long)ffilesize(fd);
        while (fd >= 0 && (n = read(fd, got, sizeof got)) > 0)
        {
            bytes(want, n, 31, total);
            right = right && !memcmp(got, want, n);
            total += n;
        }
        int e = errno;
        if (fd >= 0)
            close(fd);
        snprintf(verdict, max, "read %ld of %ld bytes, %s, then %ld (errno "
                 "%d)", total, size,
                 right ? "all the file's own" : "NOT the file's own", n,
                 n < 0 ? e : 0);
        ok = right;
    }
    else
    {
        fprintf(stderr, "fstest: no operation %s\n", op);
        exit(2);
    }
    return ok;
}

static const char *errno_name(int e)
{
    return e == ENOENT ? "ENOENT" : e == EEXIST ? "EEXIST" :
           e == EISDIR ? "EISDIR" : e == EINVAL ? "EINVAL" :
           e == ENOSPC ? "ENOSPC" : e == EBADF ? "EBADF" : e ? "other" : "0";
}

/* A file removed while open is freed by the limit of that moment: here the
   chain was two clusters when the file was opened to be emptied, one after
   the sync that cut it, and is then linked into the other file's, in the
   FAT on the disk. The limit must be the one cluster. (fstest's fat.c checks
   a chain's length at a writer's close, so the last close is a reader's.) */
#define MARK_FILE ROCKBOX_DIR "/mark.dat"

static long first_cluster(const char *path)
{
    struct filestr_base stream;
    if (open_stream_internal(path, FF_ANYTYPE, &stream, NULL) <= 0)
        fail(path);
    long cluster = stream.infop->fatfile.firstcluster;
    close_stream_internal(&stream);
    return cluster;
}

static void mark_test(void)
{
    static char buf[10000];
    uint32_t fat[512 / 4];
    uint64_t start, data;
    put_file(MARK_FILE, 50000, 35, 4096);
    bytes(buf, sizeof buf, 36, 0);
    int fd = open(MARK_FILE, O_TRUNC|O_WRONLY);
    if (fd < 0 || write(fd, buf, sizeof buf) != sizeof buf || fsync(fd) < 0)
        fail("write");
    int rd = open(MARK_FILE, O_RDONLY);
    if (rd < 0 || close(fd) < 0)
        fail("open");
    long mine = first_cluster(MARK_FILE), other = first_cluster(DMG_OTHER);
    /* the cache emptied, the links are changed below it, in both FATs */
    dc_lock_cache();
    dc_commit_all(IF_MV(0));
    dc_discard_all(IF_MV(0));
    dc_unlock_cache();
    fat_get_offsets(IF_MV(0,) &start, &data);
    const long fatsz = (data / 512 - 32) / 2;
    if (host_read(32 + other / 128, 1, fat) < 0)
        fail("FAT");
    const uint32_t second = fat[other % 128];
    for (int copy = 0; copy < 2; copy++)
    {
        const long sector = 32 + copy * fatsz + mine / 128;
        if (host_read(sector, 1, fat) < 0)
            fail("FAT");
        fat[mine % 128] = second;
        if (host_write(sector, 1, fat) < 0)
            fail("FAT");
    }
    int rm = remove(MARK_FILE), c = close(rd);
    put_file(ROCKBOX_DIR "/next.dat", 100, 34, 100);
    bool whole = same_long(DMG_OTHER, DMG_SIZE, 32);
    fat_recalc_free(IF_MV(0));
    fill_free();
    bool after = same_long(DMG_OTHER, DMG_SIZE, 32);
    printf("chain mark: %s: remove %d, close %d, the other file is %s, and "
           "%s after the next writer", rm == 0 && c == 0 && whole && after ?
           "ok" : "BAD", rm, c, whole ? "whole" : "NOT WHOLE",
           after ? "whole" : "NOT WHOLE");
#ifdef FAT_CHAIN_ANY
    const struct fat_free_stops *s = fat_get_free_stops();
    printf("; stops: %u range, %u limit", s->range, s->limit);
#endif
    printf("\n");
}

/* a create that needs another cluster for its folder's entry, and none is
   free */
static void enospc_test(void)
{
    fill_free();
    errno = 0;
    int fd = open(GROWDIR "/Track 511.m4a", O_CREAT|O_WRONLY, 0666);
    printf("open(%s) to create it, the folder and the volume full: fd %d, "
           "errno %s, ENOSPC expected\n", GROWDIR "/Track 511.m4a", fd,
           errno_name(errno));
    if (fd >= 0)
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
               errno_name(e), t[i].want);
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

/* every folder's entries below dir as readdir() gives them */
static void ls_tree(const char *dir, int depth)
{
    char path[MAX_PATH];
    DIR *d = opendir(dir);
    if (!d)
    {
        printf("%s: unreadable\n", dir);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)))
    {
        struct dirinfo info = dir_get_info(d, e);
        snprintf(path, sizeof path, "%s/%s", dir[1] ? dir : "", e->d_name);
        printf("%s %02x %ld\n", path, info.attribute, (long)info.size);
        if ((info.attribute & ATTR_DIRECTORY) && strcmp(e->d_name, ".") &&
            strcmp(e->d_name, "..") && depth < 8)
            ls_tree(path, depth + 1);
    }
    closedir(d);
}

static uint8_t short_checksum(const char *name)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (sum << 7) + (sum >> 1) + (uint8_t)name[i];
    return sum;
}

/* Long entries complete but without their short one, then a free entry,
   then a short-only entry whose 8.3 name gives the long entries' checksum
   (1 in 256 by chance; here the same name): it must read as its 8.3 name */
static void l1_test(void)
{
    static const char longname[] = "Long name.txt";  /* one long entry */
    static const char shortname[] = "LONGNA~1TXT";
    static const unsigned char at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22,
                                          24, 28, 30 };
    uint8_t sec[512];
    if (mkdir("/L1") < 0)
        fail("mkdir");
    long c = first_cluster("/L1");
    cache_drop();
    uint64_t s = entry_sector(c, 2);
    if (host_read(s, 1, sec) < 0)
        fail("read");
    uint8_t *e = sec + 2 * 32;
    memset(e, 0, 3 * 32);
    e[0] = 0x41;                    /* ord 1, the last */
    e[11] = 0x0f;
    e[13] = short_checksum(shortname);
    for (int i = 0; i < 13; i++)
        e[at[i]] = longname[i];
    e[32] = 0xe5;                   /* the free entry */
    memcpy(e + 64, shortname, 11);  /* the short-only entry */
    e[64 + 11] = 0x20;
    if (host_write(s, 1, sec) < 0)
        fail("write");
    DIR *d = opendir("/L1");
    struct dirent *de;
    const char *name = "nothing";
    static char got[MAX_PATH];
    while (d && (de = readdir(d)))
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
            name = strcpy(got, de->d_name);
    if (d)
        closedir(d);
    printf("long entries, a free one, a short-only entry: %s: read as %s, "
           "LONGNA~1.TXT expected\n",
           !strcmp(name, "LONGNA~1.TXT") ? "ok" : "BAD", name);
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
        if (!strcmp(argv[2], "damage"))
            damage_setup();
        else
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
    else if (!strcmp(cmd, "two") && argc == 3)
    {
        fs_mount(argv[2]);
        two_test();
        fs_unmount();
    }
    else if (!strcmp(cmd, "wipe") && argc == 3)
    {
        fs_mount(argv[2]);
        wipe("/");
        fs_unmount();
    }
    else if (!strcmp(cmd, "damage") && argc == 4)
    {
        fs_mount(argv[3]);
        fat_recalc_free(IF_MV(0));
        bool ok = damage_op(argv[2], verdict, sizeof verdict);
        /* the next write commits what the operation left in the cache */
        put_file(ROCKBOX_DIR "/next.dat", 100, 34, 100);
        printf("%s %s; the other file is %s", ok ? "ok:" : "failed:", verdict,
               same_long(DMG_OTHER, DMG_SIZE, 32) ? "whole" : "NOT WHOLE");
#ifdef FAT_CHAIN_ANY
        const struct fat_free_stops *s = fat_get_free_stops();
        if (s->last != FAT_STOP_NONE)
            printf("; stops: %u range, %u limit, the last at %ld after %lu "
                   "freed", s->range, s->limit, s->cluster, s->freed);
        else
            printf("; stops: none");
#endif
        printf("\n");
        fs_unmount();
    }
    else if (!strcmp(cmd, "fill") && argc == 3)
    {
        fs_mount(argv[2]);
        fat_recalc_free(IF_MV(0));
        fill_free();
        printf("%s\n", same_long(DMG_OTHER, DMG_SIZE, 32) ?
               "whole" : "NOT WHOLE");
        fs_unmount();
    }
    else if (!strcmp(cmd, "mark") && argc == 3)
    {
        fs_mount(argv[2]);
        mark_test();
        fs_unmount();
    }
    else if (!strcmp(cmd, "enospc") && argc == 3)
    {
        fs_mount(argv[2]);
        enospc_test();
        fs_unmount();
    }
    else if (!strcmp(cmd, "errno") && argc == 3)
    {
        fs_mount(argv[2]);
        errno_test();
        fs_unmount();
    }
    else if (!strcmp(cmd, "full") && argc == 3)
        full_test(argv[2]);
    else if (!strcmp(cmd, "cut") && argc == 6)
    {
        host_clone(argv[2], argv[5]);
        host_apply(argv[3], argv[5], atol(argv[4]));
    }
    else if (!strcmp(cmd, "ls") && argc == 3)
    {
        fs_mount(argv[2]);
        ls_tree("/", 0);
        fs_unmount();
    }
    else if (!strcmp(cmd, "l1") && argc == 3)
    {
        fs_mount(argv[2]);
        l1_test();
        fs_unmount();
    }
    else
    {
        fprintf(stderr, "usage: see the top of fstest.c\n");
        return 2;
    }
    return 0;
}
