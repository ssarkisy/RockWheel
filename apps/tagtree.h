/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 by Miika Pekkarinen
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
#ifdef HAVE_TAGCACHE
#ifndef _TAGTREE_H
#define _TAGTREE_H

#include "config.h"
#include "tagcache.h"
#include "tree.h"

#define TAGNAVI_VERSION    "#! rockbox/tagbrowser/2.0"
#define TAGMENU_MAX_ITEMS  64
#define TAGMENU_MAX_MENUS  32
#define TAGMENU_MAX_FMTS   32

int tagtree_export(void);
int tagtree_import(void);
void tagtree_init(void) INIT_ATTR;
bool tagtree_ready(void);
void tagtree_save_place(int dirlevel, int selected_item);
void tagtree_restore_place(int *dirlevel, int *selected_item);
bool tagtree_go_to_playing(bool artist, int *dirlevel, int *selected_item);
/* as that, at the albums of an artist of the Artists list (tag_virt_canonicalartist)
   or the songs of an album (tag_album), by name */
bool tagtree_go_to_name(int tag, const char *name, int *dirlevel, int *selected_item);
/* A section of Music by its phrase, as a screen of the main menu: its list at the
   row it was left at, where Menu leaves the lists, until tagtree_end_go_to() */
bool tagtree_has_section(int lang);
bool tagtree_go_to_section(int lang, int *dirlevel, int *selected_item);
/* the same at a place in it restored, from Now Playing to where music started */
void tagtree_resume_section(int lang, int *dirlevel, int *selected_item);
int tagtree_go_to_level(void);
void tagtree_end_go_to(bool go_back, int *dirlevel, int *selected_item);
int tagtree_enter(struct tree_context* c, bool is_visible);
void tagtree_exit(struct tree_context* c, bool is_visible);
/* Music's own menu, at the row of it that was gone into */
void tagtree_to_top(struct tree_context* c);
/* the phrases of Music's sections, in the order of the Music Menu settings */
extern const int tagtree_section_lang[MUSIC_SECTIONS];
/* Music Menu shows none of them: Music isn't in the main menu then */
bool tagtree_music_empty(void);
/* the callback of each Music Menu setting */
void tagtree_music_menu_changed(bool shown);
int tagtree_load(struct tree_context* c);
char* tagtree_get_entry_name(struct tree_context *c, int id,
                                    char* buf, size_t bufsize);
bool tagtree_current_playlist_insert(int position, bool queue);
bool tagtree_shuffle_songs(void);
void tagtree_prepare_songs(void);
bool tagtree_play_song_in_library(const char *path);
int tagtree_add_to_playlist(const char* playlist, bool new_playlist);
char *tagtree_get_title(struct tree_context* c);
int tagtree_get_attr(struct tree_context* c);
int tagtree_get_icon(struct tree_context* c);
int tagtree_get_filename(struct tree_context* c, char *buf, int buflen);
int tagtree_get_custom_action(struct tree_context* c);
bool tagtree_get_subentry_filename(char *buf, size_t bufsize);
bool tagtree_entries_iterate(struct tagcache_search *tcs,
                             bool (*action_cb)(const char *file_name),
                             char *buf, size_t buf_sz);

#endif
#endif
