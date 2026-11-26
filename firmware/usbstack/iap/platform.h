/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025 by Sho Tanimoto
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
#pragma once
#include <stdint.h>

#include "libiap/datetime.h"
#include "libiap/platform.h"
#include "powermgmt.h"
#include "time.h"

#include "buffer.h"

/* usb_iap.c */
struct IAPContext* _iap_acquire_ctx(bool lock);
void               _iap_release_ctx(void);

enum {
    IAP_SAMPR_BIT_32 = 1u << 0,
    IAP_SAMPR_BIT_44 = 1u << 1,
    IAP_SAMPR_BIT_48 = 1u << 2,
};

struct Platform {
    struct IAPAllocResult            malloc_results[5]; /* libiap makes 4 */
    struct IAPPlatformPendingControl pending_control;
    bool                             control_pending;
    bool                             autoplay_done; /* resume once per accessory session */

    int      aa_slot;
    uint8_t  acc_sampr_mask;

    /* extended interface DB selection state */
    bool     db_initialized;
    uint8_t  db_hierarchy;
    bool     db_has_playlist;
    uint32_t db_playlist_idx;
    int32_t  db_artist_seek;
    int32_t  db_album_seek;
    int32_t  db_genre_seek;
    int32_t  db_composer_seek;
    int32_t  db_kind;
    bool     db_has_track;
    uint32_t db_track_idx;
    uint16_t db_browsed_mask;
    uint16_t db_listed_mask; /* types whose records were retrieved (not just counted) */
    bool     db_selection_pending;

    /* user settings to put back when the accessory detaches, for changes it
     * asked to be restored on exit (-1: nothing to restore) */
    int8_t saved_repeat_mode;
    int8_t saved_shuffle;
};

void iap_audio_configure_samprs(uint8_t sampr_mask);
void iap_platform_autoplay(struct IAPContext* iap_ctx);
/* put back settings the accessory changed for its session only */
void iap_platform_restore_on_exit(struct Platform* plt);

/* helper functions */
uint8_t _iap_convert_play_status(int rb_audio_status);
uint8_t _iap_convert_volume(int rb_volume);
uint8_t _iap_convert_shuffle_state(int rb_state);
uint8_t _iap_convert_repeat_state(int rb_state);
uint8_t _iap_convert_battery_level(int rb_battery_level);
uint8_t _iap_convert_charge_status(enum charge_state_type rb_charge_state);
void    _iap_convert_datetime(struct tm* rb_time, struct IAPDateTime* time);
