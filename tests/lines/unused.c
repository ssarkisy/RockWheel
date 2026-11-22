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
 * The firmware library_playlists.c and on_the_go.c call from what
 * test_lines.c doesn't run: the screens, the queue, Updating Library. Each
 * ends the test if it's called after all. Built without the firmware's
 * headers, so that their types don't matter.
 *
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>

unsigned char *language_strings[1];

static void unused(const char *name)
{
    fprintf(stderr, "test_lines: %s() called, which the test doesn't have\n",
            name);
    abort();
}

#define UNUSED(name) void name(void); void name(void) { unused(#name); }
UNUSED(album_header_cover_came)
UNUSED(album_header_cover_reading)
UNUSED(album_header_draw)
UNUSED(album_header_open_playlist)
UNUSED(album_header_song_cover)
UNUSED(album_years_hash)
UNUSED(album_years_ready)
UNUSED(catalog_select_playlist)
UNUSED(default_event_handler)
UNUSED(fdprintf)
UNUSED(get_action)
UNUSED(gui_synclist_do_button)
UNUSED(gui_synclist_draw)
UNUSED(gui_synclist_get_sel_pos)
UNUSED(gui_synclist_init)
UNUSED(gui_synclist_keyclick_callback)
UNUSED(gui_synclist_playing)
UNUSED(gui_synclist_select_item)
UNUSED(gui_synclist_set_before_draw)
UNUSED(gui_synclist_set_block)
UNUSED(gui_synclist_set_cover_callback)
UNUSED(gui_synclist_set_icon_callback)
UNUSED(gui_synclist_set_initial_callback)
UNUSED(gui_synclist_set_nb_items)
UNUSED(gui_synclist_set_playing_callback)
UNUSED(gui_synclist_set_subline_callback)
UNUSED(gui_synclist_set_title)
UNUSED(gui_synclist_song_playing)
UNUSED(health_note)
UNUSED(ip_is_synced)
UNUSED(itunesdb_changed)
UNUSED(keyclick_set_callback)
UNUSED(list_covers_ahead)
UNUSED(list_covers_apply)
UNUSED(list_covers_draw_soon)
UNUSED(list_covers_frame)
UNUSED(list_covers_get)
UNUSED(list_covers_input)
UNUSED(list_covers_on)
UNUSED(list_covers_opened)
UNUSED(list_covers_poll)
UNUSED(list_covers_taken)
UNUSED(list_covers_timeout)
UNUSED(list_covers_wanted)
UNUSED(list_do_action_timeout)
UNUSED(list_get_nb_lines)
UNUSED(menu_set_wide)
UNUSED(menu_wide)
UNUSED(now_playing_available)
UNUSED(onplay)
UNUSED(onplay_set_removable)
UNUSED(onplay_set_title)
UNUSED(playlist_amount)
UNUSED(playlist_amount_ex)
UNUSED(playlist_create)
UNUSED(playlist_delete)
UNUSED(playlist_get_current)
UNUSED(playlist_get_track_info)
UNUSED(playlist_shuffle)
UNUSED(playlist_start)
UNUSED(playlist_viewer_ex)
UNUSED(plugin_buffer_gen)
UNUSED(plural_str)
UNUSED(pop_current_activity_without_refresh)
UNUSED(push_current_activity)
UNUSED(register_storage_idle_func)
UNUSED(set_option)
UNUSED(smart_prepare)
UNUSED(splashf)
UNUSED(tagcache_entry_deleted)
UNUSED(tagcache_initial)
UNUSED(tagcache_tag_string)
UNUSED(tagtree_add_to_file)
UNUSED(tagtree_is_list_file)
UNUSED(tagtree_minutes_text)
UNUSED(tagtree_song_album)
UNUSED(tagtree_song_subline)
UNUSED(track_protected_splash)
UNUSED(transition_begin)
UNUSED(transition_cancel)
UNUSED(transition_pending)
UNUSED(transition_wait)
UNUSED(unregister_storage_idle_func)
UNUSED(warn_on_pl_erase)
UNUSED(debugf)
UNUSED(panicf)
