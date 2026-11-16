/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Sergei Sarkisian
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * test_lines.c's disk, a folder of the Mac's, by the host's own functions:
 * the test's file is built with the firmware's file.h, whose macros take
 * their names.
 *
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

static char root[1024];

void host_root(const char *dir)
{
    snprintf(root, sizeof(root), "%s", dir);
    mkdir(root, 0777);
}

static void remove_under(const char *path)
{
    struct dirent **names;
    const int n = scandir(path, &names, NULL, alphasort);
    for (int i = 0; i < n; i++)
    {
        const char *name = names[i]->d_name;
        if (strcmp(name, ".") && strcmp(name, ".."))
        {
            char full[4096];
            snprintf(full, sizeof(full), "%s/%s", path, name);
            struct stat st;
            if (!lstat(full, &st) && S_ISDIR(st.st_mode))
            {
                remove_under(full);
                rmdir(full);
            }
            else
                unlink(full);
        }
        free(names[i]);
    }
    if (n >= 0)
        free(names);
}

/* the iPod's folders the cases write in, nothing in them */
void host_fresh(void)
{
    remove_under(root);
    static const char *const dirs[] = { "/.rockbox", "/Playlists",
                                        "/Playlists/B" };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(*dirs); i++)
    {
        char path[2048];
        snprintf(path, sizeof(path), "%s%s", root, dirs[i]);
        mkdir(path, 0777);
    }
}

void host_put(const char *path, const void *data, size_t n)
{
    char full[2048];
    snprintf(full, sizeof(full), "%s%s", root, path);
    FILE *f = fopen(full, "wb");
    if (!f || fwrite(data, 1, n, f) != n)
    {
        perror(full);
        exit(2);
    }
    fclose(f);
}

static void each_under(const char *dir,
                       void (*f)(const char *path, const void *data, size_t n))
{
    char path[2048];
    snprintf(path, sizeof(path), "%s%s", root, dir);
    struct dirent **names;
    const int n = scandir(path, &names, NULL, alphasort);
    for (int i = 0; i < n; i++)
    {
        const char *name = names[i]->d_name;
        if (strcmp(name, ".") && strcmp(name, ".."))
        {
            char sub[2048], full[4096];
            snprintf(sub, sizeof(sub), "%s/%s", dir, name);
            snprintf(full, sizeof(full), "%s%s", root, sub);
            struct stat st;
            if (!stat(full, &st) && S_ISDIR(st.st_mode))
                each_under(sub, f);
            else
            {
                static char data[1 << 20];
                FILE *file = fopen(full, "rb");
                const size_t got =
                    file ? fread(data, 1, sizeof(data), file) : 0;
                if (file)
                    fclose(file);
                f(sub, data, got);
            }
        }
        free(names[i]);
    }
    if (n >= 0)
        free(names);
}

/* every file under the folder, in the order of their paths, with its bytes */
void host_each_file(void (*f)(const char *path, const void *data, size_t n))
{
    each_under("", f);
}

void host_remove_root(void)
{
    remove_under(root);
    rmdir(root);
}
