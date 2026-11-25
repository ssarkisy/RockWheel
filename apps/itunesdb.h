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
 ****************************************************************************/
#ifndef _ITUNESDB_H_
#define _ITUNESDB_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "config.h"

/* the database Finder writes when it syncs the iPod */
#define ITUNESDB_DIR  "/iPod_Control/iTunes"
#define ITUNESDB_FILE ITUNESDB_DIR "/iTunesDB"

/* true when Finder has synced since the last import */
bool itunesdb_changed(void);
/* The pass of the import after a sync is to be made over the iTunesDB that is
   there, though it hasn't changed: one was cut short, or the last was an
   older firmware's, which left the dates of the songs Finder wrote again to
   the scan (itunesdb_import()). Once: the pass notes itself. */
bool itunesdb_pass_due(void);
bool itunesdb_exists(void);
/* iTunesDB's size and time, which each sync changes */
bool itunesdb_stamp(uint32_t *size, uint32_t *mtime);
/* those of the iTunesDB the last import was made from, as itunesdb.dat of any
   version has them, or during a rebuild the one set aside: false without one */
bool itunesdb_imported_stamp(uint32_t *size, uint32_t *mtime);
/* the name given to the iPod in Finder, or else "iPod" */
const char *itunesdb_device_name(void);
/* read again now, as Updating Library ends, while the disk spins */
void itunesdb_device_name_refresh(void);
/* when Finder last wrote the database */
struct tm;
bool itunesdb_sync_date(struct tm *tm);
bool itunesdb_synced_path(const char *path);

/* Brings the ratings and play counts in line with what
   itunesdb_import_tracks() read after the sync. Runs on the tagcache thread
   once the database has been updated from the files */
void itunesdb_import(void);
void itunesdb_forget_state(void);
/* At boot, before anything reads them: the files of ours that can't be made
   again (file_write.h, FW_KEEP) as a cut left them, put right: the import's
   counts and plays, the marks, notes, ids and moved list, On-The-Go, and the
   playlists in the catalogue's folder (lp_files_boot()). */
void itunesdb_files_boot(void);
/* A database of another format is built again: its state kept as this for the
   import after, which counts its unsynced plays from it */
#define ITUNESDB_OLD_STATE ROCKBOX_DIR "/itunesdb_old.dat"
void itunesdb_state_aside(void);

/* Writes iPod_Control/iTunes/Play Counts, for Finder to take what was played
   and rated on the iPod into the library: the plays it hasn't had and the
   ratings, in iTunesDB's order */
void itunesdb_write_playcounts(void);

/* The first lines of each playlist file the import writes, by which it knows
   its own: written so again from the iPod's own changes to them */
#define ITUNESDB_PLAYLIST_HEAD "#EXTM3U\n"
#define ITUNESDB_PLAYLIST_MARK "# From the iPod's library, written again after each sync"
/* and after them, the column it's sorted by on the Mac, as iTunesDB numbers
   them, if not its own order */
#define ITUNESDB_PLAYLIST_SORT "# Sorted by "
#define ITUNESDB_SORT_MANUAL   1
#define ITUNESDB_SORT_TITLE    3
#define ITUNESDB_SORT_ALBUM    4
#define ITUNESDB_SORT_ARTIST   5
#define ITUNESDB_SORT_GENRE    7
#define ITUNESDB_SORT_COMPOSER 18
/* one of the import's, by its second line */
bool itunesdb_own_playlist(const char *file);
/* a folder holding, in it or in one in it, a file that isn't a playlist (a
   song, a picture), dot files and bookmarks aside, or a folder too deep, or
   with too long a path, to look into: never the import's */
bool itunesdb_holds_foreign(const char *folder);
/* The playlists the last import wrote and the folders it made, a line each:
   "<hash> <size> <time> <file>", and "dir <folder>"; after a smart
   playlist's, "smart <file>"; and first "version <n>" (neither before
   1.4.1), then the iTunesDB it was written from, "itunesdb <size> <time>"
   as itunesdb_stamp() has them (not before 2.1.0). A playlist kept from an
   older list has "<hash> <size> <file>", and 1.4.0's lines were "<file>". */
#define ITUNESDB_PLAYLISTS_LIST ROCKBOX_DIR "/itunesdb_playlists.txt"
#define ITUNESDB_PLAYLISTS_STAMP "itunesdb "
/* Songs a sync wrote again under another path, as their dbid has it, by
   their old path's hash, sorted by it: for the import, and for the playlists
   made on the iPod, which keep them, at Updating Library, which then removes
   the list once the import is done */
#define ITUNESDB_MOVED_LIST ROCKBOX_DIR "/itunesdb_moved.dat"
struct itunesdb_moved {
    uint64_t old_hash;
    char path[56];      /* "/iPod_Control/Music/F07/CWZV.m4a" and the like */
};
/* the path now of a song moved from the path of old_hash */
bool itunesdb_moved_to(uint64_t old_hash, char *path, size_t size);

/* the hash that a database entry and an iTunesDB track are matched by */
uint64_t itunesdb_path_hash(const char *path);
/* and of name in the folder dir, as of "<dir>/<name>", without making it */
uint64_t itunesdb_path_hash_in(const char *dir, const char *name);

/* What iTunesDB says a track does differently, as the stock firmware
   plays it */
#define ITUNESDB_REMEMBERS_PLACE 0x01 /* podcasts, audiobooks */
#define ITUNESDB_AUDIOBOOK       0x02 /* plays at the audiobook speed */
#define ITUNESDB_SKIP_SHUFFLE    0x04 /* not in Shuffle Songs */
#define ITUNESDB_PODCAST         0x08
/* iTunes' "part of a compilation" (mhit+30): Music > Compilations lists it,
   and Artists leaves it out */
#define ITUNESDB_COMPILATION     0x80
/* Of a podcast or an audiobook, in the flags' file alone, not in the counts'
   (itunesdb.dat). The Mac's mark at the last import, "not played"; the mark
   as it is now, the Mac's until an episode ends or is marked on the iPod,
   and the Mac's again when the Mac changes its own; and the Mac's place is
   another than at the import before, for the import's pass to take */
#define ITUNESDB_MAC_UNPLAYED    0x10
#define ITUNESDB_UNPLAYED        0x20
#define ITUNESDB_PLACE_FROM_MAC  0x40

/* Notes what playback needs from iTunesDB: the tracks with flags and where
   each cover is. Until then no track has flags and the covers come from the
   files. At boot after a sync (synced), it also writes Finder's playlists
   as .m3u8 files and keeps the tracks' counts for itunesdb_import(), so
   that iTunesDB is read only once. Returns the number of tracks read; done
   and total, which may be NULL, say how far it is meanwhile. */
int itunesdb_import_tracks(bool synced, volatile int *done, volatile int *total);

/* Reads what itunesdb_import_tracks() noted into memory, at boot before
   music plays. False when that was noted from another iTunesDB, or by a
   firmware before this: the counts are then still to be kept, as they hold
   plays Finder hasn't had, but the tracks are to be imported again. */
bool itunesdb_load_tracks(void);

/* The playlists the last import wrote, listed by a firmware before this,
   which noted less of them (the smart ones' marks), or not listed at all, the
   list lost with files of the import's left: true when they're to be made
   again at boot under Updating Library, from the iTunesDB the last import
   took in, until a list of this version is written; but not over an
   iTunesDB whose playlists couldn't be read, or whose songs found no memory
   through a boot's tries. A file the list has as iTunesDB has it stays,
   though the iPod may have written it since, as a live smart one; those that
   move, as into a folder of their own, are written. */
bool itunesdb_playlists_due(void);
void itunesdb_import_playlists(volatile int *done, volatile int *total);

/* What iTunesDB says of a file Finder added since the last import, or of one
   whose tags changed there (itunesdb_has_record()), as its tags would, its
   play count and rating, and its dates of release and when it was added, as
   tag_date and tag_dateadded have them: false when it doesn't know the file.
   Kept from itunesdb_import_tracks() until itunesdb_forget_metadata(), for
   the scan. */
struct mp3entry;
struct tagcache_runtime;
bool itunesdb_get_metadata(const char *path, struct mp3entry *id3,
                           struct tagcache_runtime *rt, long *date,
                           long *added);
void itunesdb_forget_metadata(void);
/* There is a record of the file at path: of a track new to the last
   import, or whose tags in iTunesDB changed since or aren't known as the
   database has them. A file the database has, the same on the disk, the scan
   compares with it, and adds again with the record's tags if they differ,
   its counts and rating the entry's, not the record's. */
bool itunesdb_has_record(const char *path);
/* The Finder song at path, which the database had, added again from its
   file's tags as the file changed, and iTunesDB has no record of it: its
   tags then compared with iTunesDB's at a later import */
void itunesdb_tags_forget(const char *path);
/* This import's digests not to go in: a commit couldn't delete the strings
   of the songs it added again, which a name of another case would take, and
   the tracks the last import's digests tell changed are compared again */
void itunesdb_tags_hold(void);
/* Whether the Finder song at path is the one the last sync had there, by
   iTunesDB's dbid: 1 the same, 0 another, the last sync's having gone or
   moved, -1 not known, as for a path new to this sync or without the last
   sync's ids.
   Kept from itunesdb_import_tracks() until itunesdb_forget_metadata(). */
int itunesdb_same_song(const char *path);

/* the flags of the file at path, or with this itunesdb_path_hash() */
unsigned itunesdb_track_flags(const char *path);
unsigned itunesdb_hash_flags(uint64_t hash);

/* The mark of the podcast or audiobook at path, not played or played, as a
   listener or an episode's end makes it: in memory, and in the flags' file
   once the disk is next idle (itunesdb_save_flags()). False for a file
   without flags, as one outside Finder's folder */
bool itunesdb_set_unplayed(const char *path, bool unplayed);
/* the marks changed since the flags' file was written, written: as the disk
   is idle, at shutdown and before USB. Nothing is allocated */
void itunesdb_save_flags(void);
/* and at USB and shutdown, after a save that is under way */
void itunesdb_flush_flags(void);

/* The notes of the podcast or audiobook at path, its description in
   iTunesDB, as UTF-8 into buf, without an end: their length, 0 for none.
   buf is also the room the file's index is searched in, a few hundred bytes
   at least. Reads the disk, three reads: for when the view opens. */
int itunesdb_read_notes(const char *path, char *buf, size_t size);
/* and they are noted from this iTunesDB, or there is none */
bool itunesdb_notes_ready(void);

/* the flags are noted by this firmware, or there is no iTunesDB */
bool itunesdb_flags_ready(void);
/* Sound Check's gains are noted from this iTunesDB, or there is none */
bool itunesdb_gains_ready(void);
/* and the Sort fields of its tracks (tagcache.h), which the database sorts
   by */
bool itunesdb_sort_ready(void);

/* Puts the gain Finder worked out for this file into its entry, for Sound
   Check to use over any ReplayGain tags it has. Reads the disk: for when a
   track loads. */
void itunesdb_set_track_gain(struct mp3entry *id3);

/* true when the file at path is to play from where it was left: a podcast
   or an audiobook, or any file with Rockbox's automatic resume on */
bool itunesdb_resumes(const char *path);

/* whether the last import kept the covers' index and slides as they were,
   and the database */
bool itunesdb_import_kept_art(void);
/* Cover Flow's album list, which Updating Library checks is of the database */
#define ITUNESDB_ALBUM_INDEX PLUGIN_DEMOS_DATA_DIR "/pictureflow/pictureflow_album.idx"
/* Its header, as pictureflow.c writes it and both it and root_menu.c check
   it: this magic, the database's commit id, its entries and the size of its
   albums' file. U: with that size; V: made again for the first tracks, which a
   list made by searches lacks, so that each cover was a search of every track:
   one made at boot before the database was in memory had none; W: one entry
   for each album of each artist, a list of the pass before made again once,
   at the next update's album list or Cover Flow. */
#define ITUNESDB_ALBUM_INDEX_MAGIC "PFIW"
/* A list is stamped as music alone when it is made with the tracks' kinds
   known (the database in memory: MUSIC_ONLY, kind_if_known being
   tagcache_kind_if_known(), the plugin's through its API), and its stamp has
   the entries' complement then (ENTRIES). A list whose stamp differs from
   that of one made now, by MUSIC_ONLY now, is made again. */
#define ITUNESDB_ALBUM_INDEX_MUSIC_ONLY(kind_if_known) ((kind_if_known)(0) >= 0)
#define ITUNESDB_ALBUM_INDEX_ENTRIES(entries, music_alone) \
    ((music_alone) ? ~(entries) : (entries))

/* the covers Finder makes for the stock firmware are RGB565 frames */
#if defined(HAVE_ALBUMART) && (LCD_DEPTH == 16) && (LCD_PIXELFORMAT == RGB565)
#define ITUNESDB_HAVE_ART

/* the parameter of Cover Flow's plugin for making its album list and leaving,
   at the end of an import */
#define ITUNESDB_ALBUM_LIST "--album-list"
/* and for opening it again at the album playing, from Now Playing's Menu, as
   it was left for Now Playing: its tracks, at the one playing, if it was left
   from them; the music as it is, not paused by Cover Flow Pauses Music, as
   going back changes nothing of it */
#define ITUNESDB_COVER_FLOW_BACK "--back"

struct bitmap;

/* where a track's cover is */
struct itunesdb_art {
    char path[64];      /* its ithmb file */
    uint32_t offset;
    int width, height;  /* of the frame */
    int slide;          /* its album's slide for Cover Flow, 0 for none */
    uint32_t colour;    /* its album's colour: see cover_palette.h */
    int reason;         /* none found: why the picture there isn't shown
                           (enum itunesdb_cover_reason), 0 for no picture */
};

/* Why a song with a picture has no cover: one the core's decoder can't read,
   known as Updating Library prepared it */
enum itunesdb_cover_reason {
    ITUNESDB_COVER_SHOWN = 0,   /* or no picture */
    ITUNESDB_COVER_PNG,         /* a PNG, in the song or as its folder's image */
    ITUNESDB_COVER_PROGRESSIVE, /* a progressive JPEG */
    ITUNESDB_COVER_UNREADABLE,  /* another kind, damaged, or one that crashed
                                   or hung the decoder */
};

/* Cover Flow's slides, one an album, made after an import: covers this
   many pixels a side at most, as PictureFlow shows the one in the middle. That
   is as on a stock iPod, with the room of the status bar it has given to it:
   136 on a screen 240 high */
/* what itunesdb_load_slide() needs in its buffer beyond the slide: its header,
   the way to an aligned address, and the rest of its slot, which it reads whole */
#define ITUNESDB_SLIDE_ROOM 2048
#define ITUNESDB_SLIDE_SIZE MIN(MAX(LCD_HEIGHT * 17 / 30, LCD_WIDTH * 2 / 5), \
                                LCD_HEIGHT * 2 / 3)

/* the side of the larger picture, Finder's large frame's (the Video's 1024,
   the photo's 1013) and the one Updating Library makes, as the menus' pane
   shows it */
#define ITUNESDB_LARGE_SIDE LCD_HEIGHT

/* the cover Finder made for the file at path */
bool itunesdb_find_art(const char *path, struct itunesdb_art *art);

/* Finder's larger frame of that cover, if it wrote one (a format the iPod's
   description must list): the picture, width by height, starts x pixels
   into row y of a frame pitch pixels a row. find() reads its record from
   the disk; load() the picture's rows into pixels, through the covers' own
   room; the bytes, or -1. */
struct itunesdb_large_art {
    char path[64];      /* its ithmb file */
    uint32_t offset;
    int pitch, x, y, width, height;
    bool turned;        /* stored a quarter turn anticlockwise, as Finder
                           stores the iPod photo's format 1013: load() turns
                           it upright (square only) */
};
bool itunesdb_find_large_art(const char *path, struct itunesdb_large_art *art);
/* That frame of the cover at art, whichever's it is, Finder's or one made on
   Updating Library (240 px a side at most, its shape as art's frame's):
   find() reads its record from the disk, has() tells without the disk if
   there may be one */
bool itunesdb_find_large_of(const char *path, const struct itunesdb_art *art,
                            struct itunesdb_large_art *large);
bool itunesdb_has_large_of(const char *path, const struct itunesdb_art *art);
/* the songs with a cover of the computer's, by the index in memory */
uint32_t itunesdb_art_count(void);
/* the artwork was put back in place: its index is made at the next boot's
   import, unless one is before */
void itunesdb_art_due(void);
/* false if the cover index on the disk has no file of them made for it, as
   one an older firmware wrote: the import makes it, once */
bool itunesdb_large_art_ready(void);
/* the cover index on the disk is of artwork that is gone: the import, which
   removes it */
bool itunesdb_art_orphaned(void);
int itunesdb_load_large_art(const struct itunesdb_large_art *art,
                            void *pixels);

/* The covers of songs Finder made none for, made on Updating Library in
   Finder's forms: the 200 px frame, its slide and the 240 px picture of each
   song's own picture, found by the song's path, one for each picture however
   many songs have it; and one an album, for Cover Flow, the lists and the
   album's header, found by the album. An album's key: its name and album
   artist, as the database has them (the track's artist without one), Latin
   letters in either case as one; or else the folder of path. */
struct mp3entry;
uint64_t itunesdb_album_key(const char *album, const char *artist,
                            const char *path);
/* The cover of the track at path with these tags: Finder's, or else the one
   made of its own picture, or else its album's. Without the disk. A song
   whose file has changed since its own was made (by id3's size and picture,
   when id3 is of path and read from the file) has none, rather than the old
   one. False with
   art->reason set for a picture that isn't shown. */
bool itunesdb_find_track_art(const struct mp3entry *id3, const char *path,
                             struct itunesdb_art *art);
/* an album's of ours, by what itunesdb_album_key() takes */
bool itunesdb_find_album_art(const char *album, const char *artist,
                             const char *path, struct itunesdb_art *art);
/* not made yet for the database as it is now */
bool itunesdb_covers_stale(void);
/* Making them, by Cover Flow's plugin, on Updating Library: begin() with
   room for this many albums and songs, and whether the images outside
   Finder's folder changed since the last making. Then the songs: for each
   song Finder has no cover of, song_known(); for one to be made, its
   picture's sums to song_picture(), and, if that isn't known, song_making()
   before its decode and song_put() with the picture, up to 240 by 240, or
   none and why. Then the albums: for one with a song of Finder's,
   cover_known() and cover_put() with Finder's frame, up to 200 by 200; for
   the others, cover_of_song() of the song whose cover it takes. Then
   end(). */
bool itunesdb_covers_begin(int albums, int songs, bool images_changed);
/* Finder's artwork is set aside for the next sync (Rebuild Synced Covers),
   which writes every cover again: until then a song of Finder's folder has
   none made of its own picture, and its record of one made before is kept
   as it is (song_keep()) */
bool itunesdb_covers_aside(void);
/* none to be made for this database, as without the room for it */
void itunesdb_covers_skip(void);
int itunesdb_cover_known(uint64_t key, uint32_t stamp);

/* A song as the making takes it: its path's itunesdb_path_hash(), its file's
   size and time, covers_file_check() of its tags, where its picture is (in
   the file, or the folder's image of that stamp, 0 for the song's own), and
   that picture's sums; and its album's key and the stamp a making of the
   album would have from it, for a cover of the album made before from this
   song, which its own is then made into */
struct itunesdb_song_cover {
    uint64_t path;
    uint32_t size, mtime, check;
    uint32_t pic_pos, pic_size, source;
    uint32_t crc, adler;
    uint64_t album;
    uint32_t album_stamp;
};
/* The song known as it is: 1 kept as it was; 0 to be made; 2 to be asked
   again with source, the stamp of its folder's image now (0 for none), when
   the images changed and its cover may come of one; source ~0 when not
   known yet; 3 one whose picture wasn't shown, to be looked for in the
   computer's covers (below) and not decoded */
int itunesdb_song_known(uint64_t path, uint32_t mtime, uint32_t source);
/* the song's record kept as it is, whatever song_known() said of it, for a
   song not made now (itunesdb_covers_aside()) */
void itunesdb_song_keep(uint64_t path);
/* 1 its picture is one made already, and the song has it now; 0 it is to be
   decoded; -1 a decode of it was cut short twice, by a crash or a hang, and
   the song has none, as unreadable */
int itunesdb_song_picture(const struct itunesdb_song_cover *song);
/* said just before the song's picture is decoded */
void itunesdb_song_making(const struct itunesdb_song_cover *song);
/* the song's picture decoded into bm, up to 240 by 240; or NULL with why
   there is none (enum itunesdb_cover_reason, 0 for no picture). 1 made, 0
   not; -1 with nothing put, for no room left on the disk, no place left or
   a write failed, which ends the making: the rest at the next one. */
int itunesdb_song_put(const struct itunesdb_song_cover *song,
                      const struct bitmap *bm, int reason);
/* An album without a song of Finder's takes the cover of its first song
   whose picture is shown, which song_usable() tells after the songs are
   made; with none, its first song's, which is none or why its picture
   isn't shown. cover_of_song() gives it that song's: 1 made or kept, 2 one
   of a picture not shown that wasn't before, 0 none. */
bool itunesdb_song_usable(uint64_t song);
int itunesdb_cover_of_song(uint64_t key, uint32_t stamp, uint64_t song);
/* The covers a computer prepared (covers_table.h), taken by the making of
   the songs rather than decoded. inbox() says the file waits, for Updating
   Library to make the covers; offered() that the making has it. find() the
   picture of song->pic_size bytes with that quick key: its sums into song,
   1 with its pictures, 2 without them, as one made here; 0 none, as for a
   picture whose file's time, song->mtime, isn't trusted (below). take() it
   for the song, its sums as find() gave them or as read here, through buf:
   1 put; 0 not, as it has no blocks, they were unsound, or
   itunesdb_song_put() put nothing, to be made here; -1 as
   itunesdb_song_put(). row() a list row's cover, side pixels square, of a
   picture it took, into pixels, for the lists' covers made after the
   making. done() as Updating Library ends, the file removed once a making
   is over with it; drop() at once, as without room for the covers it'd
   take. */
bool itunesdb_covers_inbox(void);
bool itunesdb_inbox_offered(void);
/* whether a picture in a file of that time, as the disk has it, is known by
   its quick key: not in one later than the inbox's file, nor of no known
   time (0); one that isn't is taken only by its sums, read on the iPod */
bool itunesdb_inbox_trusted(uint32_t mtime);
int itunesdb_inbox_find(struct itunesdb_song_cover *song, uint32_t quick);
int itunesdb_inbox_take(const struct itunesdb_song_cover *song, void *buf,
                        size_t size);
bool itunesdb_inbox_row(const struct itunesdb_art *art, void *pixels,
                        int side);
void itunesdb_covers_inbox_done(void);
void itunesdb_covers_inbox_drop(void);
/* Where the picture of a song the database's scan found new or changed is,
   as its tags were read then: for the covers' making after it, in the same
   Updating Library, which then needn't read them again. song() from the
   scan, never while music plays; tags() for the making, by the song's path
   and the time the scan saw; forget() as Updating Library ends. */
struct itunesdb_scan_tags {
    uint32_t filesize;
    uint32_t pic_pos, pic_size;
    int32_t pic_type;   /* albumart.type, -1 for no picture */
};
void itunesdb_scan_song(const char *path, uint32_t mtime,
                        const struct mp3entry *id3);
bool itunesdb_scan_tags(uint64_t path, uint32_t mtime,
                        struct itunesdb_scan_tags *tags);
void itunesdb_scan_forget(void);
/* the images outside Finder's folder changed since the covers were made, by
   the check after USB: they're to be looked at again */
bool itunesdb_covers_images_changed(void);
/* room for a cover more, the next import's kept; else the message (misc.h) */
bool itunesdb_covers_room(void);
/* 1 made, 0 not; -1 for no room left on the disk, no place left or a write
   failed, which ends the making, the album made again at the next */
int itunesdb_cover_put(uint64_t key, uint32_t stamp, const struct bitmap *bm);
/* how they end: every album and song looked at, those not being gone; cut
   short by Menu or USB, the rest made the next time; or not all of them
   known, those not seen kept */
#define COVERS_CUT    0
#define COVERS_WHOLE  1
#define COVERS_PARTLY 2
void itunesdb_covers_end(int how);

#ifdef IPOD_COLOR
/* The iPod photo's pane takes Finder's 1013 frame of a song's cover, which
   Finder writes as the description lists that format (itunesdb.c,
   read_image()). A song without one, as of a cover Finder wrote before the
   format was listed or one that isn't square, would show Finder's 140 px
   frame scaled up. So Updating Library makes the pane's picture of each such
   song, of the song's own picture when that is Finder's cover, into
   COVERS_LARGE; covers_finder.dat keeps which, for itunesdb_find_large_of().
   In the covers' making, between itunesdb_covers_begin() and _end(). */
#define ITUNESDB_FINDER_LARGE
struct itunesdb_finder_song {
    uint64_t path;              /* itunesdb_path_hash() */
    uint32_t mtime, check;      /* as itunesdb_song_cover's */
    uint32_t pic_size, crc, adler;
};
/* The song of Finder's cover art (itunesdb_find_art()): 1 if none is to be
   made, as it has Finder's 1013 frame that the pane takes, its picture is
   known as it is, or there is no memory for the making; 0 to be made.
   Decided for each song. With Finder's artwork changed since, its index or
   its files, a known one's frame is read and summed again. */
int itunesdb_finder_large_known(uint64_t path, uint32_t mtime,
                                const struct itunesdb_art *art);
/* its picture, given by its sums, one made or found unlike Finder's frame
   already: 1 put so, without a decode; 0 to be decoded; -1 a decode of it
   was cut short twice, and the song has none */
int itunesdb_finder_large_picture(const struct itunesdb_finder_song *song,
                                  const struct itunesdb_art *art);
/* The song's picture decoded into bm (up to ITUNESDB_LARGE_SIDE a side), or
   NULL for none: its pane's picture made if it is Finder's cover, by Finder's
   frame. 1 put, 0 not, -1 with nothing put, for no room left on the disk, no
   place left or a write failed, which ends the making. bm's pixels are
   changed. */
int itunesdb_finder_large_put(const struct itunesdb_finder_song *song,
                              const struct itunesdb_art *art,
                              struct bitmap *bm);
#endif

/* reads the cover into bm, scaled up or down to fit its width and height;
   returns the size of the pixels or -1. It opens the ithmb file itself, and
   only when the frame isn't the one read last. */
int itunesdb_load_art(const struct itunesdb_art *art, struct bitmap *bm,
                      int max_size);

/* the same from a cover already in memory, such as one in the buffer */
int itunesdb_scale_art(const struct bitmap *src, struct bitmap *bm, int max_size);

/* both, for a cover shown small, as in an album's header: shrunk to half its
   size or less, each pixel the mean of those of the frame it covers, rather
   than bilinear's four of them */
int itunesdb_load_small_art(const struct itunesdb_art *art, struct bitmap *bm,
                            int max_size);
/* the same filling bm whole, at its width and height: the frame's middle as
   wide or as high as bm's shape takes, as a cell of a playlist's collage of
   covers has half a cover */
int itunesdb_load_small_art_filled(const struct itunesdb_art *art,
                                   struct bitmap *bm, int max_size);
int itunesdb_scale_small_art(const struct bitmap *src, struct bitmap *bm,
                             int max_size);

/* reads the slide of the cover's album into buf, ITUNESDB_SLIDE_SIZE
   squared pixels at most, column after column, and its size into width and
   height; returns the size of the pixels, or -1 when it isn't made (yet) */
int itunesdb_load_slide(const struct itunesdb_art *art, void *buf,
                        int *width, int *height);
/* the slides file closed, which itunesdb_load_slide() keeps open: as Cover
   Flow ends, and before the disk goes to USB */
void itunesdb_slides_done(void);

/* true when an import left slides to make, or the index of covers is of
   an older version, whose import at boot will, or the slides' colours aren't
   known yet */
bool itunesdb_slides_todo(void);

/* makes them, and notes the colour of each, counting done of total, until
   stop() says to stop: the next call goes on from there. On the tagcache
   thread. */
void itunesdb_make_slides(volatile int *done, volatile int *total,
                          bool (*stop)(void));
#endif

#endif /* _ITUNESDB_H_ */
