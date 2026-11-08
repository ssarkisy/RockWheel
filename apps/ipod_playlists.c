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
#include "system.h"
#include "file.h"
#include "core_alloc.h"
#include "misc.h"
#include "dir.h"
#include "lang.h"
#include "settings.h"
#include "splash.h"
#include "talk.h"
#include "string-extra.h"
#include "itunesdb.h"
#include "library_playlists.h"
#include "music_search.h"
#include "on_the_go.h"
#include "tree.h"
#include "filetree.h"
#include "playlist_catalog.h"
#include "root_menu.h"
#include "ipod_playlists.h"

/* The hashes of the paths in the import's list, sorted: a playlist's mark as
   the catalogue draws it, without opening the file. 300 playlists, 2.4 KB. */
static int synced_handle;
static int synced_count;

static int cmp_hash(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* the path of a line of the list, past its hash, size and time, or "dir " */
static const char *list_path(char *line)
{
    if (!strncmp(line, "dir ", 4))
        return line + 4;
    for (int field = 0; field < 3; field++)
    {
        line = strchr(line, ' ');
        if (!line)
            return NULL;
        line++;
    }
    return line;
}

void ip_synced_load(void)
{
    if (synced_handle > 0)
        synced_handle = core_free(synced_handle);
    synced_count = 0;
    int fd = open(ITUNESDB_PLAYLISTS_LIST, O_RDONLY);
    if (fd < 0)
        return;
    /* a line a playlist or folder: room for each */
    char buf[512];
    int lines = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
    {
        for (ssize_t i = 0; i < n; i++)
            lines += buf[i] == '\n';
    }
    if (lines == 0 || lseek(fd, 0, SEEK_SET) != 0)
    {
        close(fd);
        return;
    }
    synced_handle = core_alloc((size_t)lines * sizeof(uint64_t));
    if (synced_handle <= 0)
    {
        synced_handle = 0;
        close(fd);
        return;
    }
    char line[MAX_PATH + 64];
    while (synced_count < lines && read_line(fd, line, sizeof(line)) > 0)
    {
        const char *path = list_path(line);
        if (path && path[0] == '/')
        {
            uint64_t *hashes = core_get_data(synced_handle);
            hashes[synced_count++] = itunesdb_path_hash(path);
        }
    }
    close(fd);
    qsort(core_get_data(synced_handle), synced_count, sizeof(uint64_t),
          cmp_hash);
}

bool ip_is_synced(const char *path)
{
    if (synced_count == 0)
        return false;
    const uint64_t hash = itunesdb_path_hash(path);
    const uint64_t *hashes = core_get_data(synced_handle);
    int lo = 0, hi = synced_count - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (hashes[mid] == hash)
            return true;
        if (hashes[mid] < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}

static bool is_playlist_name(const char *name)
{
    const char *ext = strrchr(name, '.');
    return ext && (!strcasecmp(ext, ".m3u8") || !strcasecmp(ext, ".m3u"));
}

bool ip_editable(const char *path)
{
    return path && path[0] && !ip_is_synced(path)
           && strcasecmp(path, OTG_FILE) && !itunesdb_own_playlist(path);
}

/* Each playlist in a folder and those in it, as a path relative to it, to
   each(); false if each() said to stop */
static bool walk(char *path, size_t len, int depth,
                 bool (*each)(const char *path, size_t root, bool dir, void *data),
                 size_t root, void *data)
{
    DIR *dir = opendir(path);
    if (!dir)
        return true;
    bool go_on = true;
    struct dirent *e;
    while (go_on && (e = readdir(dir)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        const bool is_dir = dir_get_info(dir, e).attribute & ATTR_DIRECTORY;
        if ((size_t)snprintf(path + len, MAX_PATH - len, "/%s", e->d_name)
            >= MAX_PATH - len)
        {
            path[len] = '\0';
            continue;
        }
        if (is_dir && depth < 8)
            go_on = walk(path, strlen(path), depth + 1, each, root, data);
        if (go_on)
            go_on = each(path, root, is_dir, data);
        path[len] = '\0';
    }
    closedir(dir);
    return go_on;
}

static bool synced_in(const char *path, size_t root, bool dir, void *data)
{
    (void)root;
    /* by the list, and by its mark, if the list is gone */
    *(bool *)data = ip_is_synced(path)
                    || (!dir && is_playlist_name(path) && itunesdb_own_playlist(path));
    return !*(bool *)data;
}

bool ip_folder_editable(const char *path)
{
    if (!path || !path[0] || ip_is_synced(path))
        return false;
    char p[MAX_PATH];
    strmemccpy(p, path, sizeof(p));
    bool synced = false;
    walk(p, strlen(p), 0, synced_in, 0, &synced);
    return !synced;
}

/* The folder a name is typed for, its name as it is if renamed, and whether
   it's of a folder: taken by a playlist or folder of that name there */
static struct {
    char dir[MAX_PATH];
    char was[MAX_PATH];
} naming;

static bool name_taken(const char *name)
{
    if (naming.was[0] && !strcasecmp(name, naming.was))
        return false;
    char path[2 * MAX_PATH + 8];
    snprintf(path, sizeof(path), "%s/%s", naming.dir, name);
    if (dir_exists(path))
        return true;
    snprintf(path, sizeof(path), "%s/%s.m3u8", naming.dir, name);
    if (file_exists(path))
        return true;
    snprintf(path, sizeof(path), "%s/%s.m3u", naming.dir, name);
    return file_exists(path);
}

/* the room a name has in dir, for a playlist's file with its extension */
static size_t name_room(const char *dir)
{
    const size_t used = strlen(dir) + 1 + sizeof(".m3u8");
    return used < MAX_PATH ? MAX_PATH - used : 0;
}

/* a name typed for something in dir: 1, 0 if given up, -1 for USB */
static int ask_name(const char *title, const char *dir, const char *was,
                    char *name, size_t size)
{
    strmemccpy(naming.dir, dir[0] ? dir : "/", sizeof(naming.dir));
    if (!strcmp(naming.dir, "/"))
        naming.dir[0] = '\0';
    strmemccpy(naming.was, was ? was : "", sizeof(naming.was));
    strmemccpy(name, naming.was, size);
    return music_search_name(title, name, size, name_room(dir), name_taken);
}

int ip_new_playlist(const char *dir, char *made, size_t size)
{
    char name[MAX_PATH];
    const int ret = ask_name(str(LANG_NEW_PLAYLIST), dir, NULL, name,
                             sizeof(name));
    if (ret <= 0)
        return ret;
    snprintf(made, size, "%s/%s.m3u8", naming.dir, name);
    const int fd = open(made, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
        return 0;
    const bool ok = write(fd, "#EXTM3U\n", 8) == 8;
    if (close(fd) < 0 || !ok)
    {
        remove(made);
        return 0;
    }
    return 1;
}

int ip_new_folder(const char *dir, char *made, size_t size)
{
    char name[MAX_PATH];
    const int ret = ask_name(str(LANG_NEW_FOLDER), dir, NULL, name,
                             sizeof(name));
    if (ret <= 0)
        return ret;
    snprintf(made, size, "%s/%s", naming.dir, name);
    return mkdir(made) == 0 ? 1 : 0;
}

/* A folder renamed or moved: the index of each playlist in it with it */
struct moving {
    const char *from;
    size_t from_len;
};

static bool index_moved(const char *path, size_t root, bool dir, void *data)
{
    const struct moving *m = data;
    if (!dir && is_playlist_name(path))
    {
        char old[MAX_PATH];
        snprintf(old, sizeof(old), "%.*s%s", (int)m->from_len, m->from,
                 path + root);
        lp_index_moved(old, path);
    }
    return true;
}

/* the move of a playlist or folder, done: what goes with it */
static void moved(const char *from, const char *to, bool dir)
{
    if (dir)
    {
        char p[MAX_PATH];
        strmemccpy(p, to, sizeof(p));
        struct moving m = { from, strlen(from) };
        walk(p, strlen(p), 0, index_moved, strlen(to), &m);
    }
    else
        lp_index_moved(from, to);
    lp_source_moved(from, to);
}

int ip_rename(const char *path, char *renamed, size_t size)
{
    char dir[MAX_PATH], was[MAX_PATH], name[MAX_PATH];
    const bool is_dir = dir_exists(path);
    strmemccpy(dir, path, sizeof(dir));
    char *base = strrchr(dir, '/');
    if (!base)
        return 0;
    *base++ = '\0';
    strmemccpy(was, base, sizeof(was));
    const char *ext = is_dir ? NULL : strrchr(was, '.');
    char ext_kept[8] = "";
    if (ext)
    {
        strmemccpy(ext_kept, ext, sizeof(ext_kept));
        was[ext - was] = '\0';
    }
    const int ret = ask_name(str(LANG_RENAME), dir, was, name, sizeof(name));
    if (ret <= 0 || !strcmp(name, was))
        return ret < 0 ? ret : 0;
    /* its extension as it was */
    snprintf(renamed, size, "%s/%s%s", naming.dir, name, is_dir ? "" : ext_kept);
    if (rename(path, renamed) < 0)
        return 0;
    moved(path, renamed, is_dir);
    return 1;
}

int ip_move(const char *path, const char *to_dir, char *moved_to, size_t size)
{
    const char *base = strrchr(path, '/');
    if (!base)
        return 0;
    if ((size_t)snprintf(moved_to, size, "%s%s", to_dir, base) >= size)
        return 0;
    if (!strcasecmp(moved_to, path))
        return 0;
    if (file_exists(moved_to) || dir_exists(moved_to))
    {
        splash(HZ, ID2P(LANG_NAME_TAKEN));
        return 0;
    }
    const bool is_dir = dir_exists(path);
    if (rename(path, moved_to) < 0)
        return 0;
    moved(path, moved_to, is_dir);
    return 1;
}

static bool count_playlists(const char *path, size_t root, bool dir, void *data)
{
    (void)root;
    if (!dir && is_playlist_name(path))
        (*(int *)data)++;
    return true;
}

/* each playlist, a Mac's ._ file and folder left empty: the others stay,
   and with them their folder */
static bool remove_each(const char *path, size_t root, bool dir, void *data)
{
    (void)root; (void)data;
    const char *base = strrchr(path, '/');
    if (dir)
        rmdir(path);
    else if ((is_playlist_name(path) && !itunesdb_own_playlist(path))
             || (base && !strncmp(base + 1, "._", 2)))
    {
        if (remove(path) == 0)
        {
            lp_index_forget(path);
            lp_source_moved(path, NULL);
        }
    }
    return true;
}

int ip_delete(const char *path, const char *title)
{
    const bool is_dir = dir_exists(path);
    int count = 0;
    char p[MAX_PATH];
    if (is_dir)
    {
        strmemccpy(p, path, sizeof(p));
        walk(p, strlen(p), 0, count_playlists, 0, &count);
    }
    /* as Clear Playlist asks, Cancel first */
    char what[128];
    if (count > 0)
        snprintf(what, sizeof(what), str(LANG_DELETE_FOLDER_WITH), count);
    else
        strmemccpy(what, str(LANG_DELETE), sizeof(what));
    const struct opt_items items[] = {
        { (const unsigned char *)what, -1 }, { STR(LANG_CANCEL_0) },
    };
    int choice = 1;
    if (set_option(title, &choice, RB_INT, items, ARRAYLEN(items), NULL))
        return -1;
    if (choice != 0)
        return 0;
    if (is_dir)
    {
        strmemccpy(p, path, sizeof(p));
        walk(p, strlen(p), 0, remove_each, 0, NULL);
        rmdir(path);
    }
    else if (remove(path) == 0)
    {
        lp_index_forget(path);
        lp_source_moved(path, NULL);
    }
    return 1;
}

static long size_of(const char *file)
{
    const int fd = open(file, O_RDONLY);
    if (fd < 0)
        return 0;
    const long size = ffilesize(fd);
    close(fd);
    return size;
}

/* not one of the import's, whatever the list of them says */
static bool can_add_to(const char *target)
{
    return !strcasecmp(target, OTG_FILE) || !itunesdb_own_playlist(target);
}

bool ip_add_file(const char *target, const char *path)
{
    if (!can_add_to(target))
        return false;
    const long before = size_of(target);
    if (!playlist_file_append(target, path))
        return false;
    lp_index_appended(target, before);
    return true;
}

bool ip_add_playlist(const char *target, const char *playlist)
{
    if (!can_add_to(target))
        return false;
    const long before = size_of(target);
    if (!playlist_file_append_playlist(target, playlist))
        return false;
    lp_index_appended(target, before);
    return true;
}

bool ip_add_selection(const char *target)
{
    if (!can_add_to(target))
        return false;
    const long before = size_of(target);
    if (!playlist_file_append_selection(target))
        return false;
    lp_index_appended(target, before);
    return true;
}

/* the iPod's own and the folders, of the catalogue picked from */
static bool own_only(char *name, int attr, struct tree_context *tc)
{
    if (attr & ATTR_DIRECTORY)
        return true;
    const char *ext = strrchr(name, '.');
    if (ext && !strcasecmp(ext, ".m3u"))
        return false;
    char path[MAX_PATH];
    ft_assemble_path(path, sizeof(path), tc->currdir, name);
    return !ip_is_synced(path);
}

int ip_pick_playlist(char *picked, size_t size)
{
    char root[MAX_PATH];
    if (!catalog_directory_name(root, sizeof(root)))
        return 0;
    picked[0] = '\0';
    struct browse_context browse = {
        .dirfilter = SHOW_M3U,
        .flags = BROWSE_SELECTONLY | BROWSE_NO_CONTEXT_MENU | BROWSE_PICK_PLAYLIST,
        .callback_show_item = own_only,
        .title = str(LANG_PLAYLISTS),
        .icon = Icon_Playlist,
        .root = root,
        .buf = picked,
        .bufsize = size,
    };
    if (rockbox_browse(&browse) == GO_TO_ROOT)
        return -1;
    return (browse.flags & BROWSE_SELECTED) && picked[0] ? 1 : 0;
}
