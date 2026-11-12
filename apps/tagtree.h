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
#ifdef HAVE_TC_RAMCACHE
/* the database not in memory after 20 s before the menu: the lists don't
   wait for it too, but are made from the disk until it is */
void tagtree_ram_waited_out(void);
#endif
void tagtree_save_place(int dirlevel, int selected_item);
void tagtree_restore_place(int *dirlevel, int *selected_item);
/* That place, a list of Music's, is known for the music playing: Now Playing's
   Playing From opens it then, as the go-tos below, at the song playing */
bool tagtree_playing_from_known(void);
/* a playlist file the lists of Music play from, not a playlist of one's own */
bool tagtree_is_list_file(const char *path);
bool tagtree_go_to_from(int *dirlevel, int *selected_item);
/* it kept over a boot, as the iPod shuts down or USB is connected */
void tagtree_keep_place(void);
bool tagtree_go_to_playing(bool artist, int *dirlevel, int *selected_item);
/* as that, at a song by its file: from Search, a playlist, Cover Flow */
bool tagtree_go_to_track(const char *path, bool artist, int *dirlevel,
                         int *selected_item);
/* Go to Artist and Go to Album in the menu of the row selected in Music's lists,
   which goes there in place: whether it has the one (an album has Go to Artist,
   a song both, but Go to Album in its album's own list); and going there. Menu
   in the list opened, at tagtree_browse_level(), goes back to the row with
   tagtree_end_browse(), true if Cover Flow is to open again at the song in buf
   (tagtree_browse_from_cover_flow()). Forgotten as the lists are left. */
bool tagtree_row_goes_to(bool album);
/* The file at next is a played episode that the music, going on from the
   episode at playing, passes over: both Finder's podcasts of one show, the
   queue that show's own list in Podcasts, as started there or resumed, and
   Repeat off. From memory only; for the playback's thread as it loads the
   next track, which leaves out what the listener put in the queue by hand */
bool tagtree_passes_over(const char *playing, const char *next);
/* the music playing is a show's or a book's own list, of Podcasts or
   Audiobooks, which Shuffle leaves in its order. On the UI's thread */
bool tagtree_queue_in_order(void);
/* The row selected is an episode of the show listed, in Podcasts: its state
   (EPISODE_..., tagcache.h), -1 if it isn't one. *marked: Finder's, with a
   mark to set either way; *playing: the one playing, begun as it plays */
int tagtree_row_episode(bool *marked, bool *playing);
/* It marked played or, Finder's only, not played, its place forgotten either
   way: Finder's by its mark alone, another by a play if it had none. False
   if nothing was changed */
bool tagtree_row_mark(bool played);
/* and played from its start, its place forgotten: as Select, which goes on
   from the place. False if it didn't start */
bool tagtree_row_start_over(void);
bool tagtree_browse_from_row(bool artist);
bool tagtree_browse_from_cover_flow(const char *path);
int tagtree_browse_level(void);
bool tagtree_end_browse(int *dirlevel, int *selected_item, char *buf, size_t size);
void tagtree_forget_browse(void);
/* as that, at the albums of an artist (tag_virt_canonicalartist), of the songs or
   of the albums, by its row in Artists or else in a list of the other artists in
   its place; or at the songs of an album (tag_album), by name */
bool tagtree_go_to_name(int tag, const char *name, int *dirlevel, int *selected_item);
/* and at an album's songs by its name and its album artist's seek, as Albums
   has the albums of a name apart; by its name alone with -1 */
bool tagtree_go_to_album(const char *name, int32_t artist, int *dirlevel,
                         int *selected_item);
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
/* a list's Shuffle Songs row, and its songs played in a new order: false if
   none started */
bool tagtree_row_is_shuffle(struct tree_context *c, int row);
bool tagtree_shuffle_list(struct tree_context *c);
int tagtree_load(struct tree_context* c);
char* tagtree_get_entry_name(struct tree_context *c, int id,
                                    char* buf, size_t bufsize);
bool tagtree_current_playlist_insert(int position, bool queue);
bool tagtree_shuffle_songs(void);
void tagtree_prepare_songs(void);
/* whether Songs' list is made for the database as it is */
bool tagtree_songs_prepared(void);
/* Songs' playlist in the order chosen, or the file of its orders, isn't
   written for the library as it is, and has its room */
bool tagtree_songs_stale(void);
bool tagtree_play_song_in_library(const char *path);
int tagtree_add_to_playlist(const char* playlist, bool new_playlist);
bool tagtree_add_to_file(const char *file);
void tagtree_reload_after_browse(void);
bool tagtree_at_top(void);
char *tagtree_get_title(struct tree_context* c);
int tagtree_get_attr(struct tree_context* c);
bool tagtree_row_is_playing(struct tree_context *c, int row, long playing);
/* the column before the rows' names: the digits of its widest text, 0 for a
   list without it; and a row's text there, as gui_synclist's lead has it */
int tagtree_lead_digits(struct tree_context *c);
const char *tagtree_entry_lead(struct tree_context *c, int row, char *buf,
                               size_t size);
/* The rows of albums in two lines, each album's name over its year and its
   album artist, as Albums has them with Artists in Albums, and with their
   covers, with Covers in Lists (list_covers.h) in any list of albums: the
   list made last has them. A row's second line written to buf, or NULL for a
   row of one line, as Sort By; and its album by its number in the table of
   years (album_years.h), whose cover it shows: -1 for the note, as Unknown
   Album, -2 for a row without a cover, as Sort By */
bool tagtree_rows_two_lines(struct tree_context *c);
/* what the list says when it has no rows, "No Composers": a phrase, or NULL */
const unsigned char *tagtree_empty_text(struct tree_context *c);
bool tagtree_rows_covers(struct tree_context *c);
const char *tagtree_entry_subline(struct tree_context *c, int row, char *buf,
                                  size_t size);
/* "2011 · Juniper Coast": an album's year, if it's given one, and with artist
   its album artist, as the album's number has it, unless that's except, as
   an artist's albums have it for theirs. NULL for neither */
const char *tagtree_album_subline(int no, int year, bool artist,
                                  const char *except, char *buf, size_t size);
int tagtree_entry_album(struct tree_context *c, int row);
/* Artists in Albums switched, by its setting or as settings are applied:
   Albums' rows are others, and the places in it at their first */
void tagtree_album_rows_changed(bool on);
void tagtree_album_rows_check(void);
/* Quick Scroll's letter of a row, as the list goes by them, or its year in a
   list by years, written to buf; NULL for a row without one, as at the top,
   and for each of a list in another order */
const char *tagtree_entry_initial(struct tree_context *c, int row, char *buf,
                                  size_t size);
/* the list has a Sort By, Albums', All Songs' or Songs', which is drawn as a
   setting, with its value */
bool tagtree_has_sort_by(struct tree_context *c);
/* the row opens a list: not a song, nor Sort By, which switches in place */
bool tagtree_row_has_children(struct tree_context *c, int row);
/* the list's Sort By chosen: its other order, saved, as the list is made
   again */
void tagtree_sort_by_switched(struct tree_context *c);
/* The bars of groups among the list's songs, which aren't its entries: how
   many, the entry each is over, in their order, and the text of one */
int tagtree_group_count(struct tree_context *c);
int tagtree_group_at(struct tree_context *c, int group);
const char *tagtree_group_name(struct tree_context *c, int group, char *buf,
                               size_t size);
/* The list made last as an album's screen shows it, under a header with its
   cover, name, album artist, year and songs (album_header.c): the songs of
   one album, all of them, in the order of its discs and tracks, and music.
   False for another list: a part of one, the songs without an album, those
   of two albums of a name in one row, as without Artists in Albums, a
   book's, or any without the database in memory, which the rest is read
   from. */
struct tagtree_album {
    int songs;
    int year;                   /* 0 for none */
    long longest;               /* the longest song's length, in ms */
    int32_t first;              /* its first song in the database */
    int32_t album, albumartist; /* their names' seeks, which are the album's */
};
bool tagtree_album(struct tree_context *c, struct tagtree_album *album);
/* It is a show's, in Podcasts, its songs the show's episodes, the newest
   first, each in two lines: its header has how many they are in place of a
   year and songs, and albumartist is the author they all have, -1 if they
   differ; year and longest are 0 */
bool tagtree_album_is_show(struct tree_context *c);
/* Or a book's, in Audiobooks, its songs the book's parts in their order. The
   header's last line of a show or a book, "3 episodes", "8 parts \xc2\xb7 9 h 5 min",
   NULL for an album; the widest of a book's values at its rows' right, NULL
   for an album; and the row Resume or Listen Again plays from, -1 if none. */
bool tagtree_album_is_book(struct tree_context *c);
const char *tagtree_album_counts(struct tree_context *c, char *buf, size_t size);
const char *tagtree_value_widest(struct tree_context *c, char *buf, size_t size);
int tagtree_book_resume_row(struct tree_context *c);
/* The row selected is a part of the book listed: 0 if the book is new, 1 if
   begun, 2 if heard to its end, -1 if it is none; *playing if a part of it
   plays. Start Over, after a yes/no question, forgets the book's place and
   plays it from its first part; Mark as Finished forgets it and counts every
   part as heard: Finder's by its mark, without a play, another by a play if
   it has none. False if nothing was done. */
int tagtree_row_book(bool *playing);
bool tagtree_book_start_over(void);
bool tagtree_book_finish(void);
/* A track's texts as Podcasts' rows have them: its date, "20 May 2023", with its
   length after it if with_length, and what is left of it, "28 min left"; or NULL. */
const char *tagtree_track_date(int32_t idx, bool with_length, char *buf,
                               size_t size);
const char *tagtree_time_left(unsigned long elapsed, unsigned long length,
                              char *buf, size_t size);
/* The list made last has its rows' states, a show's episodes: each row's
   mark in a column before its name (enum line_mark): a dot for a new
   episode, a half dot for one begun, LINE_MARK_EMPTY for the rest;
   LINE_MARK_NONE for a row at its top */
bool tagtree_rows_states(struct tree_context *c);
int tagtree_entry_mark(struct tree_context *c, int row);
/* of its first song, a tag of names' text as the database has it, UNTAGGED
   for none: from its memory only, false without it. Not tag_filename, whose
   path may be read from the disk */
bool tagtree_album_text(struct tree_context *c, int tag, char *buf, size_t size);
/* a song's length at the right of its row there, or NULL */
const char *tagtree_entry_length(struct tree_context *c, int row, char *buf,
                                 size_t size);
/* the title of the list the list was opened from, as an album's screen has
   it while its header shows: Albums, the artist, the genre */
const char *tagtree_get_title_above(struct tree_context *c);
/* the albums' years made again */
void tagtree_albums_reordered(void);
/* Artists and Genres as Group by Album Artist has them, if they aren't: they
   list the other artists, and the lists left in them, at the place given, go
   back up to those */
void tagtree_artists_grouping_changed(int *dirlevel, int *selected_item);
int tagtree_get_icon(struct tree_context* c);
int tagtree_get_filename(struct tree_context* c, char *buf, int buflen);
int tagtree_get_custom_action(struct tree_context* c);
bool tagtree_get_subentry_filename(char *buf, size_t bufsize);
bool tagtree_entries_iterate(struct tagcache_search *tcs,
                             bool (*action_cb)(const char *file_name),
                             char *buf, size_t buf_sz);

#endif
#endif
