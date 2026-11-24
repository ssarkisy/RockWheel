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
#include <ctype.h>
#include <errno.h>
#include "system.h"
#include "file.h"
#include "dir.h"
#include "rbpaths.h"
#include "misc.h"
#include "string-extra.h"
#include "sortkey.h"
#include "eq_curve.h"
#include "eq_files.h"

extern struct eq_band_setting eq_defaults[EQ_NUM_BANDS];

/* A file over this is no preset: one is a few hundred bytes */
#define EQ_FILE_MAX_BYTES 8192
/* the ranges of Rockbox's EQ settings (eq_menu.h), Hz and tenths */
#define CUTOFF_MIN 20
#define CUTOFF_MAX 22040
#define Q_MIN      1
#define Q_MAX      64
#define GAIN_MAX   240

/* AutoEq names its files so, after the model's name; the list leaves it out */
#define APO_SUFFIX " ParametricEQ"
/* and its graphic EQ, beside it in a model's folder, which is no list of
   filters: not listed */
#define GRAPHIC_SUFFIX " GraphicEQ"
/* its ten fixed bands, listed with this in their name, but not beside its
   ParametricEQ */
#define FIXED_SUFFIX " FixedBandEQ"

enum { KIND_CFG, KIND_TXT, KIND_FOLDER, KIND_GONE /* as promote() works */ };

/* an entry's flags */
#define F_APO      1 /* a .txt whose file has APO_SUFFIX after the name */
#define F_APO_SEEN 2 /* a ParametricEQ of the name is beside it, a .cfg won */
#define F_OWN_DIR  4 /* a preset whose file is in a folder of its name in dir */
#define F_HIDDEN   8 /* a folder not listed, only on a preset's path */
#define F_TXT_SEEN 16 /* a plain .txt of the name is beside it, which it won */

struct eq_entry {
    char name[EQ_NAME_MAX];     /* as listed */
    uint8_t folder;             /* the id of the folder it's listed in, EQ_TOP */
    uint8_t id;                 /* a folder's own */
    uint8_t kind;
    uint8_t flags;
    uint8_t dir;                /* a preset's: the folder its file is in */
};

static struct eq_entry entries[EQ_FILES_MAX];
static int count;
static int next_id;
/* the alphabets first (sortkey_get_first()) the order is by */
static unsigned sorted_first;

/* out of the stack: the UI's thread alone uses these */
static struct eq_band_setting reading[EQ_NUM_BANDS];
static char line[256];
static char path[MAX_PATH], other[MAX_PATH];
static char text[640];

static int compare(const char *a, const char *b)
{
    return sortkey_compare(a, b);
}

/** the table **/

int eq_files_folder_entry(int folder)
{
    for (int i = 0; folder != EQ_TOP && i < count; i++)
    {
        if (entries[i].kind == KIND_FOLDER && entries[i].id == folder)
            return i;
    }
    return -1;
}

static int depth_of(int i)
{
    int depth = 0;
    for (int f = entries[i].folder; f != EQ_TOP && depth <= EQ_DEPTH_MAX; depth++)
    {
        const int e = eq_files_folder_entry(f);
        if (e < 0)
            break;
        f = entries[e].folder;
    }
    return depth;
}

/* the folders' part of a path, "A/B/" for an entry in B in A */
static bool folder_path(int folder, char *buf, size_t size)
{
    const char *parts[EQ_DEPTH_MAX + 1];
    int n = 0;
    for (int f = folder; f != EQ_TOP; n++)
    {
        const int e = eq_files_folder_entry(f);
        if (e < 0 || n > EQ_DEPTH_MAX)
            return false;
        parts[n] = entries[e].name;
        f = entries[e].folder;
    }
    size_t len = 0;
    buf[0] = '\0';
    while (n-- > 0 && len < size)
        len += snprintf(buf + len, size - len, "%s/", parts[n]);
    return len < size;
}

/* a file's name in its folder: its name, AutoEq's suffix and its extension */
static bool file_name(const char *name, int kind, bool suffix, char *buf, size_t size)
{
    return (size_t)snprintf(buf, size, "%s%s%s", name, suffix ? APO_SUFFIX : "",
                            kind == KIND_TXT ? ".txt" : kind == KIND_CFG ? ".cfg" : "")
           < size;
}

/* a preset's folders, as its file has them: "A/B/" */
static bool dir_path(const struct eq_entry *e, char *buf, size_t size)
{
    if (!folder_path(e->dir, buf, size))
        return false;
    const size_t len = strlen(buf);
    return !(e->flags & F_OWN_DIR)
           || (size_t)snprintf(buf + len, size - len, "%s/", e->name) < size - len;
}

bool eq_files_path(int i, char *buf, size_t size)
{
    if (i < 0 || i >= count || entries[i].kind == KIND_FOLDER
        || !dir_path(&entries[i], buf, size))
        return false;
    const size_t len = strlen(buf);
    return file_name(entries[i].name, entries[i].kind, entries[i].flags & F_APO,
                     buf + len, size - len);
}

static void remove_at(int i)
{
    memmove(&entries[i], &entries[i + 1], (count - i - 1) * sizeof(*entries));
    count--;
}

/* As the table is read, the folders still to be read, at its end, the newest
   at its lowest place: their room is the presets', and when there's too little
   for a folder's presets the newest noted go. Each is its name, with the id of
   the folder it's in as folder and its depth as id. */
static int pending;

/* into its place by name; -1 if the table is full */
static int insert(const struct eq_entry *e)
{
    if (count >= EQ_FILES_MAX - pending)
        return -1;
    int lo = 0, hi = count;
    while (lo < hi)
    {
        const int mid = (lo + hi) / 2;
        if (compare(e->name, entries[mid].name) < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    memmove(&entries[lo + 1], &entries[lo], (count - lo) * sizeof(*entries));
    entries[lo] = *e;
    count++;
    return lo;
}

/* a preset of that name in the folder, else -1 */
static int sibling(int folder, const char *name, int except)
{
    for (int i = 0; i < count; i++)
    {
        if (i != except && entries[i].folder == folder && entries[i].kind != KIND_FOLDER
            && !strcasecmp(entries[i].name, name))
            return i;
    }
    return -1;
}

/* folders without a preset in them, at any depth, go; a hidden one stays as
   long as a preset's file is in it */
static void prune(void)
{
    bool gone = true;
    while (gone)
    {
        gone = false;
        for (int i = 0; i < count; i++)
        {
            if (entries[i].kind != KIND_FOLDER)
                continue;
            int k = 0;
            while (k < count && entries[k].folder != entries[i].id
                   && (entries[k].kind == KIND_FOLDER || entries[k].dir != entries[i].id))
                k++;
            if (k == count)
            {
                remove_at(i);
                gone = true;
                break;
            }
        }
    }
}

/* a folder found, to be read after the one it's in */
static void note_folder(const char *name, int parent, int depth)
{
    struct eq_entry *e = &entries[EQ_FILES_MAX - 1 - pending++];
    memset(e, 0, sizeof(*e));
    strmemccpy(e->name, name, EQ_NAME_MAX);
    e->folder = parent;
    e->id = depth;
    e->kind = KIND_FOLDER;
}

/* the oldest noted */
static struct eq_entry next_folder(void)
{
    const struct eq_entry e = entries[EQ_FILES_MAX - 1];
    pending--;
    memmove(&entries[EQ_FILES_MAX - pending], &entries[EQ_FILES_MAX - pending - 1],
            pending * sizeof(*entries));
    return e;
}

/* room for n more in the table, the newest folders noted given up for it if
   need be */
static bool make_room(int n)
{
    while (count + n > EQ_FILES_MAX - pending && pending)
        pending--;
    return count + n <= EQ_FILES_MAX - pending;
}

/* a file of the folder's that could be a preset, as it is listed: false for
   one that couldn't */
static bool file_entry(struct dirent *d, const struct dirinfo *info,
                       int folder, struct eq_entry *e)
{
    const char *n = d->d_name;
    const size_t len = strlen(n);
    if (len <= 4 || info->size > EQ_FILE_MAX_BYTES)
        return false;
    memset(e, 0, sizeof(*e));
    e->folder = folder;
    e->dir = folder;
    if (!strcasecmp(n + len - 4, ".cfg"))
        e->kind = KIND_CFG;
    else if (!strcasecmp(n + len - 4, ".txt"))
        e->kind = KIND_TXT;
    else
        return false;
    size_t shown = len - 4;
    const size_t suffix = sizeof(APO_SUFFIX) - 1;
    const size_t graphic = sizeof(GRAPHIC_SUFFIX) - 1;
    if (e->kind == KIND_TXT && shown > suffix
        && !strncasecmp(n + shown - suffix, APO_SUFFIX, suffix))
    {
        shown -= suffix;
        e->flags = F_APO;
    }
    else if (e->kind == KIND_TXT && shown > graphic
             && !strncasecmp(n + shown - graphic, GRAPHIC_SUFFIX, graphic))
        return false;
    if (shown >= EQ_NAME_MAX)
        return false;
    strmemccpy(e->name, n, shown + 1);
    return true;
}

/* AutoEq's FixedBandEQ of a model whose ParametricEQ is in the folder: not
   listed, whichever of them comes first */
static int fixed_of(int folder, const char *name)
{
    char fixed[EQ_NAME_MAX];
    if ((size_t)snprintf(fixed, sizeof(fixed), "%s" FIXED_SUFFIX, name) >= sizeof(fixed))
        return -1;
    const int i = sibling(folder, fixed, -1);
    return i >= 0 && entries[i].kind == KIND_TXT && !(entries[i].flags & F_APO) ? i : -1;
}

static void drop_fixed(int folder, const char *name)
{
    const int i = fixed_of(folder, name);
    if (i >= 0)
        remove_at(i);
}

static bool is_fixed_beside_apo(int folder, const struct eq_entry *e)
{
    const size_t len = strlen(e->name), fixed = sizeof(FIXED_SUFFIX) - 1;
    if (e->kind != KIND_TXT || (e->flags & F_APO) || len <= fixed
        || strcasecmp(e->name + len - fixed, FIXED_SUFFIX))
        return false;
    char model[EQ_NAME_MAX];
    strmemccpy(model, e->name, len - fixed + 1);
    const int i = sibling(folder, model, -1);
    return i >= 0 && (entries[i].flags & (F_APO | F_APO_SEEN));
}

/* folders opened in one reading at most: a full table of presets each in a
   folder of its own takes a reading of each, and of the folders on their way;
   twice the table's room is that and more, and bounds the walk through a big
   tree with no presets (AutoEq's measurements) to as many */
#define FOLDERS_READ_MAX (2 * EQ_FILES_MAX)
static int folders_read;

/* One folder into the table, name at rel (its path, "" or "A/B/"), with that
   id, in the folder parent, in one reading of it: its .cfg and .txt files that
   could be presets, which are read only when chosen, and its folders, noted to
   be read after it while there's room, which its presets take back first (the
   newest noted go first, make_room()). Below the top, a folder is listed with
   all of its presets or none, with its own row only if it has presets or
   folders noted: false when there's no room for them, the table full. Of
   one name, one row: a .cfg over a ParametricEQ.txt over a plain .txt, in
   whatever order they are read, the row keeping which of the others are
   beside it; a model's FixedBandEQ beside its ParametricEQ: not listed */
static bool read_folder(int id, int parent, int depth, const char *name,
                        const char *rel)
{
    snprintf(path, sizeof(path), EQS_DIR "/%s", rel);
    const size_t rel_len = strlen(rel);
    const int own = id != EQ_TOP; /* its row */
    const int before = count;
    int noted = 0;              /* of its folders, still noted */
    bool fits = true;
    folders_read++;
    DIR *dir = opendir(path);
    if (!dir)
        return true;
    struct dirent *d;
    while (fits && (d = readdir(dir)))
    {
        const struct dirinfo info = dir_get_info(dir, d);
        const size_t len = strlen(d->d_name);
        /* a Mac's ._ file beside each, ., .. and what's hidden; names the
           table or a path has no room for */
        if (d->d_name[0] == '.' || rel_len + len + 1 >= EQ_PATH_MAX)
            continue;
        if (info.attribute & ATTR_DIRECTORY)
        {
            if (depth < EQ_DEPTH_MAX && len < EQ_NAME_MAX
                && count + own + 1 <= EQ_FILES_MAX - pending)
            {
                note_folder(d->d_name, id, depth + 1);
                noted++;
            }
            continue;
        }
        struct eq_entry e;
        if (!file_entry(d, &info, id, &e) || is_fixed_beside_apo(id, &e))
            continue;
        if (e.flags & F_APO)
            drop_fixed(id, e.name);
        const int same = sibling(id, e.name, -1);
        if (same >= 0)
        {
            /* of one name, a .cfg first, then a ParametricEQ, then a plain
               .txt, in whatever order they come: the one listed keeps which
               of the others are beside it, to go with it (forms_of()) */
            struct eq_entry *s = &entries[same];
            const bool plain = s->kind == KIND_TXT && !(s->flags & F_APO);
            if (e.kind == KIND_CFG && s->kind == KIND_TXT)
            {
                s->flags = ((s->flags & (F_APO | F_APO_SEEN)) ? F_APO_SEEN : 0)
                           | (plain ? F_TXT_SEEN : 0) | (s->flags & F_TXT_SEEN);
                s->kind = KIND_CFG;
            }
            else if ((e.flags & F_APO) && plain)
                s->flags = F_APO | F_TXT_SEEN;
            else if (e.flags & F_APO)
                s->flags |= F_APO_SEEN;
            else if (e.kind == KIND_TXT)
                s->flags |= F_TXT_SEEN;
            continue;
        }
        const int had = pending;
        fits = make_room(own + 1);
        noted -= MIN(noted, had - pending);
        if (fits)
            insert(&e);
    }
    closedir(dir);
    if (!fits)
    {
        /* as many as fit at the top; below it none */
        for (int i = count - 1; own && i >= 0; i--)
        {
            if (entries[i].folder == id && entries[i].kind != KIND_FOLDER)
                remove_at(i);
        }
        return false;
    }
    if (own && (count > before || noted))
    {
        struct eq_entry e;
        memset(&e, 0, sizeof(e));
        strmemccpy(e.name, name, EQ_NAME_MAX);
        e.folder = parent;
        e.id = id;
        e.kind = KIND_FOLDER;
        /* its one preset, and nothing else: listed in the parent instead, as
           promote() does, already here so that an AutoEq model's folder takes
           no room. Of the folder's name, the folder needs no row */
        if (count == before + 1 && !noted)
        {
            int i = 0;
            while (entries[i].folder != id)
                i++;
            if (sibling(parent, entries[i].name, i) < 0)
            {
                entries[i].folder = parent;
                if (!strcasecmp(entries[i].name, name))
                {
                    entries[i].flags |= F_OWN_DIR;
                    entries[i].dir = parent;
                    return true;
                }
                e.flags = F_HIDDEN;
            }
        }
        insert(&e);
        next_id++;
    }
    return true;
}

/* A folder whose one row is a preset: the preset listed in the folder's
   parent instead, unless one of its name is there, and the folder not. Its
   file still is in the folder: when they have one name (AutoEq's model
   folders) the preset says so (F_OWN_DIR) and the folder goes from the
   table, else the folder stays, hidden, for its path. read_folder() does it
   for each folder as it's read; here, for a parent left with that one row
   after, a pass for each level. */
static void promote(void)
{
    static uint8_t rows[EQ_TOP], last[EQ_TOP];
    for (int pass = 0; pass <= EQ_DEPTH_MAX; pass++)
    {
        memset(rows, 0, sizeof(rows));
        for (int i = 0; i < count; i++)
        {
            const struct eq_entry *e = &entries[i];
            if (e->folder == EQ_TOP || (e->flags & F_HIDDEN))
                continue;
            if (rows[e->folder] < 2)
                rows[e->folder]++;
            last[e->folder] = i;
        }
        bool changed = false;
        for (int f = 0; f < count; f++)
        {
            struct eq_entry *folder = &entries[f];
            if (folder->kind != KIND_FOLDER || (folder->flags & F_HIDDEN)
                || rows[folder->id] != 1)
                continue;
            /* its one row a preset still, not a folder gone this pass */
            struct eq_entry *e = &entries[last[folder->id]];
            if ((e->kind != KIND_CFG && e->kind != KIND_TXT)
                || sibling(folder->folder, e->name, -1) >= 0)
                continue;
            e->folder = folder->folder;
            if (e->dir == folder->id && !(e->flags & F_OWN_DIR)
                && !strcasecmp(e->name, folder->name))
            {
                e->flags |= F_OWN_DIR;
                e->dir = folder->folder;
                folder->kind = KIND_GONE;
            }
            else
                folder->flags |= F_HIDDEN;
            changed = true;
        }
        int kept = 0;
        for (int i = 0; i < count; i++)
        {
            if (entries[i].kind != KIND_GONE)
                entries[kept++] = entries[i];
        }
        count = kept;
        if (!changed)
            break;
    }
}

void eq_files_read(void)
{
    count = 0;
    next_id = 0;
    pending = 0;
    folders_read = 0;
    bool room = read_folder(EQ_TOP, EQ_TOP, 0, "", "");
    /* then each folder noted, the oldest first: one open at a time */
    while (room && pending && folders_read < FOLDERS_READ_MAX)
    {
        const struct eq_entry f = next_folder();
        size_t len;
        if (next_id >= EQ_TOP || !folder_path(f.folder, other, EQ_PATH_MAX)
            || (len = strlen(other)) + strlen(f.name) + 1 >= EQ_PATH_MAX)
            continue;
        snprintf(other + len, EQ_PATH_MAX - len, "%s/", f.name);
        room = read_folder(next_id, f.folder, f.id, f.name, other);
    }
    pending = 0;
    prune();
    promote();
    sorted_first = sortkey_get_first();
}

void eq_files_sort(void)
{
    if (sorted_first == sortkey_get_first())
        return;
    sorted_first = sortkey_get_first();
    for (int i = 1; i < count; i++)
    {
        struct eq_entry e = entries[i];
        int j = i;
        for (; j > 0 && compare(e.name, entries[j - 1].name) < 0; j--)
            entries[j] = entries[j - 1];
        entries[j] = e;
    }
}

/* a row of the folder's, of the kind: listed */
static bool is_row(const struct eq_entry *e, int folder, bool folders)
{
    return e->folder == folder && (e->kind == KIND_FOLDER) == folders
           && !(e->flags & F_HIDDEN);
}

int eq_files_children(int folder, bool folders)
{
    int n = 0;
    for (int i = 0; i < count; i++)
        n += is_row(&entries[i], folder, folders);
    return n;
}

int eq_files_child(int folder, bool folders, int n)
{
    for (int i = 0; n >= 0 && i < count; i++)
    {
        if (is_row(&entries[i], folder, folders) && n-- == 0)
            return i;
    }
    return -1;
}

int eq_files_child_pos(int entry)
{
    const struct eq_entry *e = &entries[entry];
    int n = 0;
    for (int i = 0; i < entry; i++)
        n += is_row(&entries[i], e->folder, e->kind == KIND_FOLDER);
    return n;
}

const char *eq_files_name(int entry)
{
    return entry >= 0 && entry < count ? entries[entry].name : "";
}

int eq_files_folder_id(int entry)
{
    return entries[entry].id;
}

int eq_files_folder_of(int entry)
{
    return entries[entry].folder;
}

int eq_files_ancestor(int entry, int depth)
{
    if (entry < 0 || entry >= count)
        return -1;
    const int own = depth_of(entry);
    if (depth > own)
        return -1;
    int e = entry;
    for (int k = own; k > depth; k--)
        e = eq_files_folder_entry(entries[e].folder);
    return e;
}

int eq_files_find(const char *rel)
{
    for (int i = 0; rel && rel[0] && i < count; i++)
    {
        if (entries[i].kind != KIND_FOLDER && eq_files_path(i, path, sizeof(path))
            && !strcasecmp(path, rel))
            return i;
    }
    return -1;
}

void eq_files_shown_name(const char *rel, char *buf, size_t size)
{
    const char *name = strrchr(rel, '/');
    name = name ? name + 1 : rel;
    size_t len = strlen(name);
    const bool txt = len > 4 && !strcasecmp(name + len - 4, ".txt");
    if (len > 4 && (txt || !strcasecmp(name + len - 4, ".cfg")))
        len -= 4;
    const size_t suffix = sizeof(APO_SUFFIX) - 1;
    if (txt && len > suffix && !strncasecmp(name + len - suffix, APO_SUFFIX, suffix))
        len -= suffix;
    strmemccpy(buf, name, MIN(size, len + 1));
}

/** reading a preset **/

/* a whole number with its sign, the spaces round it skipped */
static bool take_int(char **p, int *v)
{
    char *s = skip_whitespace(*p);
    const bool neg = *s == '-';
    if (*s == '-' || *s == '+')
        s++;
    if (!isdigit(*s))
        return false;
    int n = 0;
    while (isdigit(*s))
    {
        n = n * 10 + (*s++ - '0');
        if (n > 100000)
            return false;
    }
    *p = skip_whitespace(s);
    *v = neg ? -n : n;
    return true;
}

/* "cutoff, q, gain", with ", TYPE" after it if with_type */
static bool take_band(char *value, struct eq_band_setting *b, bool with_type)
{
    int cutoff, q, gain;
    if (!take_int(&value, &cutoff) || *value++ != ','
        || !take_int(&value, &q) || *value++ != ','
        || !take_int(&value, &gain))
        return false;
    b->cutoff = cutoff;
    b->q = q;
    b->gain = gain;
    if (!with_type)
        return *value == '\0';
    if (*value++ != ',')
        return false;
    value = skip_whitespace(value);
    if (!strcasecmp(value, "LOW_SHELF"))
        b->type = EQ_FILTER_LOW_SHELF;
    else if (!strcasecmp(value, "PEAK"))
        b->type = EQ_FILTER_PEAK;
    else if (!strcasecmp(value, "HIGH_SHELF"))
        b->type = EQ_FILTER_HIGH_SHELF;
    else
        return false;
    return true;
}

/* the band a name ends with, "3" of "eq filter 3", from first to last; -1 */
static int band_number(const char *s, int first, int last, const char **after)
{
    if (!isdigit(*s))
        return -1;
    int n = 0;
    while (isdigit(*s) && n < 100)
        n = n * 10 + (*s++ - '0');
    if (after)
        *after = s;
    return n >= first && n <= last ? n : -1;
}

/* the bands of the "eq band N" lines a file has, a bit each, and whether it has
   the later forms too */
static unsigned old_bands;
static bool new_forms;

/* One line of a .cfg: 1 for a band's, 0 for any other, which changes nothing,
   -1 for one of the EQ's that is wrong. The forms: "eq filter N: cutoff, q,
   gain, TYPE", Rockbox's since July 2026 and RockWheel's; "eq low shelf
   filter", "eq peak filter 1" to 8 and "eq high shelf filter": "cutoff, q,
   gain", Rockbox's from 3.13 to then; and "eq band N cutoff", "eq band N q"
   and "eq band N gain", each with its number, Rockbox's before 3.13: of five
   bands, the last a high shelf, until January 2013, then of ten. The precut is
   the EQ's own (eq_curve_precut()), as every preset's, so the file's isn't read */
static int take_line(char *name, char *value)
{
    char *end = name + strlen(name);
    while (end > name && isspace(end[-1]))
        *--end = '\0';
    if (strncasecmp(name, "eq ", 3))
        return 0;
    const char *rest;
    int band;
    if (!strncasecmp(name, "eq filter ", 10))
    {
        band = band_number(name + 10, 0, EQ_NUM_BANDS - 1, &rest);
        if (band < 0 || *rest)
            return -1;
        new_forms = true;
        return take_band(value, &reading[band], true) ? 1 : -1;
    }
    if (!strcasecmp(name, "eq low shelf filter"))
    {
        new_forms = true;
        reading[0].type = EQ_FILTER_LOW_SHELF;
        return take_band(value, &reading[0], false) ? 1 : -1;
    }
    if (!strcasecmp(name, "eq high shelf filter"))
    {
        new_forms = true;
        reading[EQ_NUM_BANDS - 1].type = EQ_FILTER_HIGH_SHELF;
        return take_band(value, &reading[EQ_NUM_BANDS - 1], false) ? 1 : -1;
    }
    if (!strncasecmp(name, "eq peak filter ", 15))
    {
        band = band_number(name + 15, 1, EQ_NUM_BANDS - 2, &rest);
        if (band < 0 || *rest)
            return -1;
        new_forms = true;
        reading[band].type = EQ_FILTER_PEAK;
        return take_band(value, &reading[band], false) ? 1 : -1;
    }
    if (!strncasecmp(name, "eq band ", 8))
    {
        band = band_number(name + 8, 0, EQ_NUM_BANDS - 1, &rest);
        int v;
        if (band < 0 || *rest != ' ' || !take_int(&value, &v) || *value)
            return -1;
        rest++;
        if (!strcasecmp(rest, "cutoff"))
            reading[band].cutoff = v;
        else if (!strcasecmp(rest, "q"))
            reading[band].q = v;
        else if (!strcasecmp(rest, "gain"))
            reading[band].gain = v;
        else
            return -1;
        old_bands |= 1u << band;
        return 1;
    }
    return 0;
}

/* a number with a fraction, as "-6.25", scaled by scale and to the nearest */
static bool take_fixed(char **p, int scale, int *v)
{
    char *s = skip_whitespace(*p);
    const bool neg = *s == '-';
    if (*s == '-' || *s == '+')
        s++;
    if (!isdigit(*s) && !(*s == '.' && isdigit(s[1])))
        return false;
    long n = 0, frac = 0, div = 1;
    while (isdigit(*s))
    {
        n = n * 10 + (*s++ - '0');
        if (n > 100000)
            return false;
    }
    if (*s == '.')
    {
        s++;
        while (isdigit(*s))
        {
            if (div < 100000)
            {
                frac = frac * 10 + (*s - '0');
                div *= 10;
            }
            s++;
        }
    }
    const long scaled = n * scale + (frac * scale * 2 + div) / (2 * div);
    *v = neg ? -scaled : scaled;
    *p = s;
    return true;
}

/* the next word of p, as "Gain"; false if there's none */
static bool take_word(char **p, const char *word)
{
    char *s = skip_whitespace(*p);
    const size_t len = strlen(word);
    if (strncasecmp(s, word, len) || (s[len] && !isspace(s[len])))
        return false;
    *p = s + len;
    return true;
}

static int apo_bands;

/* One line of an Equalizer APO file, as AutoEq writes them for each model:
   "Preamp: -6.2 dB", "Filter 1: ON PK Fc 105 Hz Gain 6.0 dB Q 0.70". 1 for a
   filter taken, 0 for a line that changes nothing, -1 for any other: a filter
   of another type or form, a channel's, a device's or another file's lines
   would make the sound another than the curve's */
static int take_apo_line(char *s)
{
    s = skip_whitespace(s);
    if (!*s || *s == '#')
        return 0;
    if (!strncasecmp(s, "Preamp:", 7))
        return 0;   /* the precut is the EQ's own */
    if (strncasecmp(s, "Filter", 6))
        return -1;
    s += 6;
    while (isdigit(*s) || *s == ' ')
        s++;
    if (*s++ != ':')
        return -1;
    if (take_word(&s, "OFF"))
        return 0;
    if (!take_word(&s, "ON"))
        return -1;
    struct eq_band_setting b;
    if (take_word(&s, "PK"))
        b.type = EQ_FILTER_PEAK;
    else if (take_word(&s, "LSC"))
        b.type = EQ_FILTER_LOW_SHELF;
    else if (take_word(&s, "HSC"))
        b.type = EQ_FILTER_HIGH_SHELF;
    else
        return -1;
    int fc, gain, q;
    if (!take_word(&s, "Fc") || !take_fixed(&s, 1, &fc) || !take_word(&s, "Hz")
        || !take_word(&s, "Gain") || !take_fixed(&s, 10, &gain) || !take_word(&s, "dB")
        || !take_word(&s, "Q") || !take_fixed(&s, 10, &q) || *skip_whitespace(s))
        return -1;
    if (apo_bands >= EQ_NUM_BANDS)
        return -1;
    /* a frequency or a Q past the settings' ranges, as an optimizer may write
       one, at their edge; a gain past them is no preset of this EQ */
    b.cutoff = MAX(CUTOFF_MIN, MIN(fc, CUTOFF_MAX));
    b.gain = gain;
    b.q = MAX(Q_MIN, MIN(q, Q_MAX));
    reading[apo_bands++] = b;
    return 1;
}

bool eq_files_band_ok(const struct eq_band_setting *b)
{
    return b->cutoff >= CUTOFF_MIN && b->cutoff <= CUTOFF_MAX
        && b->q >= Q_MIN && b->q <= Q_MAX
        && b->gain >= -GAIN_MAX && b->gain <= GAIN_MAX
        /* the first is 0, and the enum unsigned on ARM */
        && (unsigned)b->type <= (unsigned)EQ_FILTER_HIGH_SHELF
        && eq_curve_band_fits(b);
}

/* a file's bands into reading: false for one that is no preset or a wrong one,
   which then sets nothing */
static bool read_file(const char *file, bool apo)
{
    memcpy(reading, eq_defaults, sizeof(reading));
    old_bands = 0;
    new_forms = false;
    apo_bands = 0;
    const int fd = open(file, O_RDONLY);
    if (fd < 0)
        return false;
    bool bands = false, ok = ffilesize(fd) <= EQ_FILE_MAX_BYTES, first = true;
    while (ok && read_line(fd, line, sizeof(line)) > 0)
    {
        char *s = line;
        /* a text editor's byte order mark */
        if (first && !strncmp(s, "\xef\xbb\xbf", 3))
            s += 3;
        first = false;
        int took;
        if (apo)
            took = take_apo_line(s);
        else
        {
            char *name, *value;
            took = settings_parseline(s, &name, &value) ? take_line(name, value) : 0;
        }
        bands |= took > 0;
        ok = took >= 0;
    }
    close(fd);
    if (!ok || !bands)
        return false;
    /* five bands of Rockbox before 2013: the last is the high shelf */
    if (old_bands && !new_forms && !(old_bands & ~0x1fu))
        reading[4].type = EQ_FILTER_HIGH_SHELF;
    for (int i = 0; i < EQ_NUM_BANDS; i++)
    {
        if (!eq_files_band_ok(&reading[i]))
            return false;
    }
    return true;
}

bool eq_files_exists(const char *rel)
{
    return (size_t)snprintf(path, sizeof(path), EQS_DIR "/%s", rel) < sizeof(path)
           && file_exists(path);
}

bool eq_files_load(const char *rel, struct eq_band_setting *bands, int *precut)
{
    const size_t len = strlen(rel);
    if (len <= 4 || (size_t)snprintf(path, sizeof(path), EQS_DIR "/%s", rel) >= sizeof(path))
        return false;
    /* the file's lines and the curve's maths at once, as the list waits */
    cpu_boost(true);
    const bool ok = read_file(path, !strcasecmp(rel + len - 4, ".txt"));
    if (ok)
    {
        memcpy(bands, reading, sizeof(reading));
        *precut = eq_curve_precut(eq_curve_peak_of(reading));
    }
    cpu_boost(false);
    return ok;
}

/** writing **/

/* a preset's file in one of its forms */
enum { FORM_CFG, FORM_TXT, FORM_APO, FORM_FIXED };

static int form_of(const struct eq_entry *e)
{
    return e->kind == KIND_CFG ? FORM_CFG : (e->flags & F_APO) ? FORM_APO : FORM_TXT;
}

/* the file of the name in that form, in the folder dir ("…/A/B/") */
static bool form_path(char *buf, const char *dir, const char *name, int form)
{
    return (size_t)snprintf(buf, MAX_PATH, "%s%s%s%s", dir, name,
                            form == FORM_APO ? APO_SUFFIX
                            : form == FORM_FIXED ? FIXED_SUFFIX : "",
                            form == FORM_CFG ? ".cfg" : ".txt") < MAX_PATH;
}

/* where the preset's files are, full, as "/.rockbox/eqs/A/B/", under that
   name: its own folder, for F_OWN_DIR, of that name */
static bool real_dir(const struct eq_entry *e, const char *name, char *buf)
{
    struct eq_entry as = *e;
    strmemccpy(as.name, name, EQ_NAME_MAX);
    strcpy(buf, EQS_DIR "/");
    const size_t len = strlen(buf);
    return dir_path(&as, buf + len, MAX_PATH - len);
}

/* a file of the name in the folder dir, in any form a preset's has: one not
   listed too, as the table had no room for it; its FixedBandEQ if fixed */
static bool name_in(const char *dir, const char *name, bool fixed)
{
    static char file[MAX_PATH];
    for (int form = FORM_CFG; form <= (fixed ? FORM_FIXED : FORM_APO); form++)
    {
        if (form_path(file, dir, name, form) && file_exists(file))
            return true;
    }
    return false;
}

static bool ends_with(const char *name, const char *end)
{
    const size_t len = strlen(name), end_len = strlen(end);
    return len > end_len && !strcasecmp(name + len - end_len, end);
}

/* whether the preset's files under the new name have paths the reading
   takes (EQ_PATH_MAX): its folders, its own one of the new name, and the
   longest of its files' names, a ParametricEQ's when it has one */
static bool fits_path(const struct eq_entry *e, const char *name)
{
    static char rel[MAX_PATH];
    struct eq_entry as = *e;
    strmemccpy(as.name, name, EQ_NAME_MAX);
    if (!dir_path(&as, rel, sizeof(rel)))
        return false;
    size_t longest = strlen(name) + 4; /* ".cfg", ".txt" */
    if (e->flags & (F_APO | F_APO_SEEN))
        longest += sizeof(APO_SUFFIX) - 1; /* the longer of the two */
    return strlen(rel) + longest + 1 < EQ_PATH_MAX;
}

bool eq_files_taken(int folder, const char *name, int except)
{
    static char dir[MAX_PATH];
    if (sibling(folder, name, except) >= 0)
        return true;
    strcpy(dir, EQS_DIR "/");
    if (!folder_path(folder, dir + strlen(dir), MAX_PATH - strlen(dir)))
        return true;
    if (except < 0)
        return name_in(dir, name, false);
    const struct eq_entry *e = &entries[except];
    if (!fits_path(e, name))
        return true;
    /* the same files under another case of its name are no others, whatever
       its name ends with */
    if (!strcasecmp(e->name, name))
        return false;
    /* renamed: a name the next reading would skip (" GraphicEQ") or might
       keep hidden (" FixedBandEQ") is no name for it */
    if (ends_with(name, GRAPHIC_SUFFIX) || ends_with(name, FIXED_SUFFIX))
        return true;
    /* none of its files to go over another, where they are: beside the rows
       it's listed with, and its own folder or a hidden one, and for its own
       folder, a folder of the new name beside that */
    if (name_in(dir, name, true))
        return true;
    if (e->dir == e->folder && !(e->flags & F_OWN_DIR))
        return false;
    if (e->flags & F_OWN_DIR)
    {
        if (!real_dir(e, name, dir))
            return true;
        dir[strlen(dir) - 1] = '\0';
        if (dir_exists(dir))
            return true;
    }
    return !real_dir(e, e->name, dir) || name_in(dir, name, true);
}

static const char *type_name(int type)
{
    return type == EQ_FILTER_LOW_SHELF ? "LOW_SHELF"
         : type == EQ_FILTER_HIGH_SHELF ? "HIGH_SHELF" : "PEAK";
}

/* the forms of a preset's files, in dir: its own first, then those it hides
   beside it, a ParametricEQ that a .cfg of its name shadows, a plain .txt
   that a .cfg or a ParametricEQ shadows, and AutoEq's FixedBandEQ of the
   model, those that are there; how many, FORMS at most */
#define FORMS 4
static int forms_of(const struct eq_entry *e, const char *dir, int *forms)
{
    static char file[MAX_PATH];
    int n = 0;
    forms[n++] = form_of(e);
    if ((e->flags & F_APO_SEEN) && form_path(file, dir, e->name, FORM_APO)
        && file_exists(file))
        forms[n++] = FORM_APO;
    if ((e->flags & F_TXT_SEEN) && form_path(file, dir, e->name, FORM_TXT)
        && file_exists(file))
        forms[n++] = FORM_TXT;
    if ((e->flags & (F_APO | F_APO_SEEN)) && form_path(file, dir, e->name, FORM_FIXED)
        && file_exists(file))
        forms[n++] = FORM_FIXED;
    return n;
}

/* A preset's files off the disk, those it hides first, as it is one row and
   goes whole: false at the first that can't go, its row and file still there */
static bool remove_files(const struct eq_entry *e)
{
    static char dir[MAX_PATH];
    int forms[FORMS];
    if (!real_dir(e, e->name, dir))
        return false;
    for (int k = forms_of(e, dir, forms) - 1; k >= 0; k--)
    {
        if (!form_path(path, dir, e->name, forms[k])
            || (remove(path) < 0 && file_exists(path)))
            return false;
    }
    return true;
}

/* A preset's .cfg of the name in the folder dir ("…/A/B/"), in RockWheel's
   format: the whole file at once, beside the old one until it is written,
   which rename() then replaces, so that a full disk or a reset leaves the
   old preset. False if it couldn't be written, errno then the failure's */
static bool write_cfg(const char *dir, const char *name,
                      const struct eq_band_setting *bands, int precut)
{
    size_t len = snprintf(text, sizeof(text),
                          "# RockWheel EQ preset\neq enabled: on\neq precut: %d\n",
                          precut);
    for (int i = 0; i < EQ_NUM_BANDS && len < sizeof(text); i++)
        len += snprintf(text + len, sizeof(text) - len, "eq filter %d: %d, %d, %d, %s\n",
                        i, bands[i].cutoff, bands[i].q, bands[i].gain,
                        type_name(bands[i].type));
    if (len >= sizeof(text)
        || !form_path(other, dir, name, FORM_CFG)
        || (size_t)snprintf(path, sizeof(path), "%s%s.new", dir, name) >= sizeof(path))
        return false;
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    const bool written = write(fd, text, len) == (ssize_t)len;
    if (close(fd) < 0 || !written || rename(path, other) < 0)
    {
        const int err = errno;
        remove(path);
        errno = err;
        return false;
    }
    return true;
}

bool eq_files_save(const char *name, const struct eq_band_setting *bands,
                   int precut)
{
    /* with no room for its row, not written: it would be there but not listed */
    if (sibling(EQ_TOP, name, -1) < 0 && count >= EQ_FILES_MAX)
        return false;
    if ((!dir_exists(EQS_DIR) && mkdir(EQS_DIR) < 0)
        || !write_cfg(EQS_DIR "/", name, bands, precut))
        return false;
    /* the preset of the name it replaces, when that was other files (a
       ParametricEQ.txt with its FixedBandEQ, or one in a folder of its own):
       the .cfg is it now */
    const int at = sibling(EQ_TOP, name, -1);
    struct eq_entry e;
    memset(&e, 0, sizeof(e));
    if (at >= 0 && (entries[at].kind != KIND_CFG || entries[at].dir != EQ_TOP
                    || (entries[at].flags & F_OWN_DIR)))
        remove_files(&entries[at]);
    else if (at >= 0)
        /* the .cfg rewritten where it was: still over what it shadowed */
        e.flags = entries[at].flags & (F_APO_SEEN | F_TXT_SEEN);
    if (at >= 0)
        remove_at(at);
    strmemccpy(e.name, name, EQ_NAME_MAX);
    e.folder = EQ_TOP;
    e.dir = EQ_TOP;
    e.kind = KIND_CFG;
    insert(&e);
    return true;
}

int INIT_ATTR eq_files_write_new(const char *folder, const char *name,
                                 const struct eq_band_setting *bands)
{
    char dir[MAX_PATH];
    if (!dir_exists(EQS_DIR) && mkdir(EQS_DIR) < 0)
        return EQ_NEW_FAILED;
    /* its folder made, unless a file has its name */
    if ((size_t)snprintf(dir, sizeof(dir), EQS_DIR "/%s", folder) >= sizeof(dir) - 1)
        return EQ_NEW_FAILED;
    if (folder[0] && !dir_exists(dir))
    {
        if (file_exists(dir))
            return EQ_NEW_TAKEN;
        if (mkdir(dir) < 0)
            return EQ_NEW_FAILED;
    }
    if (folder[0])
        strcat(dir, "/");
    /* none of a preset's files of the name beside it, nor a folder, which
       would be listed under it (an AutoEq model's) */
    if (name_in(dir, name, false))
        return EQ_NEW_TAKEN;
    if ((size_t)snprintf(path, sizeof(path), "%s%s", dir, name) >= sizeof(path))
        return EQ_NEW_FAILED;
    if (file_exists(path)) /* of any type */
        return EQ_NEW_TAKEN;
    /* the precut it is played with here, from its bands as every preset's
       (eq_files_load()), for a firmware that reads the file's */
    const int precut = eq_curve_precut(eq_curve_peak_of(bands));
    return write_cfg(dir, name, bands, precut) ? EQ_NEW_WRITTEN : EQ_NEW_FAILED;
}

/* a file of the preset's, in the folder dir, to the new name: false if not */
static bool rename_form(const char *dir, const char *from, const char *to, int form)
{
    static char target[MAX_PATH];
    return form_path(path, dir, from, form) && form_path(target, dir, to, form)
           && rename(path, target) >= 0;
}

/* a folder renamed, or back: its full paths, their slashes left as they are */
static bool rename_dir(char *from, char *to)
{
    from[strlen(from) - 1] = '\0';
    to[strlen(to) - 1] = '\0';
    const bool ok = rename(from, to) >= 0;
    strcat(from, "/");
    strcat(to, "/");
    return ok;
}

int eq_files_rename(int i, const char *name)
{
    struct eq_entry e = entries[i];
    static char from_dir[MAX_PATH], to_dir[MAX_PATH];
    if (strlen(name) >= EQ_NAME_MAX || eq_files_taken(e.folder, name, i)
        || !real_dir(&e, e.name, from_dir) || !real_dir(&e, name, to_dir))
        return -1;
    /* the files it hides go with it, so that they stay hidden: AutoEq's
       FixedBandEQ of the model ("Y/Y ParametricEQ.txt", "Y FixedBandEQ.txt"),
       the ParametricEQ that a .cfg of its name shadows, and the plain .txt
       that a .cfg or a ParametricEQ of its name shadows (forms_of()) */
    int forms[FORMS];
    const int n = forms_of(&e, from_dir, forms);
    const bool own = e.flags & F_OWN_DIR;
    /* its own folder first, named as it is, then its files in it; each step
       taken back if a later one fails */
    if (own && !rename_dir(from_dir, to_dir))
        return -1;
    const char *dir = own ? to_dir : from_dir;
    int done = 0;
    while (done < n && rename_form(dir, e.name, name, forms[done]))
        done++;
    if (done < n)
    {
        while (done-- > 0)
            rename_form(dir, name, e.name, forms[done]);
        if (own)
            rename_dir(to_dir, from_dir);
        return -1;
    }
    strmemccpy(e.name, name, EQ_NAME_MAX);
    remove_at(i);
    return insert(&e);
}

bool eq_files_delete(int i)
{
    if (!remove_files(&entries[i]))
        return false;
    remove_at(i);
    prune();
    return true;
}
