/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 Björn Stenberg
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
#ifndef _WPS_H_
#define _WPS_H_

#include <stdbool.h>

struct mp3entry;

/* Please don't add anything else to here... */
/* What the centre button cycles through on the WPS, as on a stock iPod.
   Skins see the current mode through %mw. */
enum wps_display_mode
{
    WPS_MODE_VOLUME = 0, /* normal view, the wheel sets the volume */
    WPS_MODE_SCRUB,      /* the wheel moves through the track */
    WPS_MODE_ALBUMART,   /* full screen album art */
    WPS_MODE_LYRICS,     /* the wheel scrolls the lyrics */
    WPS_MODE_RATING,     /* the wheel sets the rating */
    WPS_MODE_COUNT
};

struct wps_state
{
    struct mp3entry *id3;
    struct mp3entry *nid3;
    int ff_rewind_count;
    bool paused;
    enum wps_display_mode display_mode;
};

long gui_wps_show(void);

enum wps_do_action_type
{
    WPS_PLAY,
    WPS_PAUSE,
    WPS_PLAYPAUSE, /* toggle */
};

void wps_do_action(enum wps_do_action_type, bool updatewps);
/* fade (if enabled) and pause the audio, optionally rewind a little */
#define pause_action(update) wps_do_action(WPS_PAUSE, update)
#define unpause_action(update) wps_do_action(WPS_PLAY, update)
#define wps_do_playpause(update) wps_do_action(WPS_PLAYPAUSE, update)

struct wps_state *get_wps_state(void);

/* in milliseconds */
#define DEFAULT_SKIP_THRESH          3000l

#endif /* _WPS_H_ */
