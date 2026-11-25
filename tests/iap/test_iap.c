/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 David Codesido
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "misc.h"
#include "libiap/constants.h"
#include "libiap/endian.h"
#include "libiap/iap.h"
#include "libiap/platform.h"
#include "libiap/spec/hid.h"
#include "libiap/spec/iap.h"
#include "libiap/spec/lingoes/general/set-internal-battery-charging-state.h"

#include "../lib/check.h"

unsigned long iap_debug_timestamp(void) {
    return 0;
}

/* Mock platform state & recorded events */
struct mock_packet {
    uint8_t  lingo;
    uint16_t command;
    uint16_t trans_id;
    uint8_t  payload[512];
    size_t   payload_len;
};

struct mock_platform {
    struct IAPContext* ctx;

    uint8_t            tx_stream[1024];
    size_t             tx_len;
    bool               tx_in_progress;
    struct mock_packet tx_packets[32];
    size_t             tx_count;

    enum IAPPlatformControl last_controls[16];
    size_t                  control_count;

    uint8_t                          db_hierarchy;
    int32_t                          sel_playlist;
    int32_t                          sel_artist;
    int32_t                          sel_album;
    int32_t                          play_sel_called;
    uint32_t                         play_sel_track;
    struct IAPPlatformPendingControl play_sel_pending;
    bool                             defer_play_sel_ack;

    uint32_t sampr_calls;
    uint32_t samprs[8];

    const char* record_override; /* returned for every DB record if set */
};

static const char* mock_playlists[]  = {"Favoritas", "Rock", "Viaje"};
static const char* mock_artists[]    = {"Artist A", "Artist B"};
static const char* mock_albums_a[]   = {"Album A1", "Album A2"};
static const char* mock_albums_b[]   = {"Album B1"};
static const char* mock_tracks_a1[]  = {"Track 1", "Track 2", "Track 3",
                                        "Track 4"};
static const char* mock_tracks_all[] = {"All 1", "All 2", "All 3", "All 4",
                                        "All 5"};

void* iap_platform_malloc(struct IAPContext* iap_ctx, size_t size, int flags) {
    (void)iap_ctx;
    (void)flags;
    return calloc(1, size);
}

void iap_platform_free(struct IAPContext* iap_ctx, void* ptr) {
    (void)iap_ctx;
    free(ptr);
}

static void decode_tx_packet(struct mock_platform* mp, bool has_trans_id) {
    CHECK(mp->tx_len >= 5);
    CHECK_EQ(mp->tx_stream[0], IAP_SOF_BYTE);

    size_t pos     = 1;
    size_t pkt_len = mp->tx_stream[pos++];
    if(pkt_len == 0) {
        CHECK(mp->tx_len >= 7);
        pkt_len = ((size_t)mp->tx_stream[pos] << 8) | mp->tx_stream[pos + 1];
        pos += 2;
    }
    const size_t packet_end = pos + pkt_len;
    CHECK(mp->tx_len >= packet_end + 1);

    uint8_t sum = 0;
    for(size_t i = 1; i < packet_end; i++) {
        sum = (uint8_t)(sum + mp->tx_stream[i]);
    }
    CHECK_EQ((uint8_t)(sum + mp->tx_stream[packet_end]), 0);

    CHECK(mp->tx_count < ARRAYLEN(mp->tx_packets));
    struct mock_packet* out = &mp->tx_packets[mp->tx_count++];
    memset(out, 0, sizeof(*out));

    size_t p   = pos;
    out->lingo = mp->tx_stream[p++];
    if(out->lingo == IAPLingoID_ExtendedInterface) {
        out->command = ((uint16_t)mp->tx_stream[p] << 8) | mp->tx_stream[p + 1];
        p += 2;
    } else {
        out->command = mp->tx_stream[p++];
    }
    if(has_trans_id) {
        out->trans_id =
            ((uint16_t)mp->tx_stream[p] << 8) | mp->tx_stream[p + 1];
        p += 2;
    }
    out->payload_len = packet_end - p;
    CHECK(out->payload_len <= sizeof(out->payload));
    if(out->payload_len > 0) {
        memcpy(out->payload, &mp->tx_stream[p], out->payload_len);
    }
}

int iap_platform_send_hid_report(struct IAPContext* iap_ctx, const void* ptr,
                                 size_t size) {
    struct mock_platform* mp = iap_ctx->platform;
    CHECK(size >= 2);
    const struct IAPHIDReport* rep      = ptr;
    const size_t               data_len = size - 2;

    if(!(rep->link_control & IAPHIDReportLinkControlBits_Continue)) {
        mp->tx_len         = 0;
        mp->tx_in_progress = true;
    } else {
        CHECK(mp->tx_in_progress);
    }
    CHECK(mp->tx_len + data_len <= sizeof(mp->tx_stream));
    memcpy(mp->tx_stream + mp->tx_len, rep->data, data_len);
    mp->tx_len += data_len;

    if(!(rep->link_control & IAPHIDReportLinkControlBits_MoreToFollow)) {
        mp->tx_in_progress = false;
        decode_tx_packet(mp, iap_ctx->trans_id_support == 1);
    }
    return (int)size;
}

IAPBool iap_platform_get_ipod_serial_num(struct IAPContext* iap_ctx,
                                         struct IAPSpan*    serial) {
    (void)iap_ctx;
    static const char str[] = "RW1234567890";
    return iap_span_append(serial, str, sizeof(str));
}

IAPBool iap_platform_get_play_status(struct IAPContext*            iap_ctx,
                                     struct IAPPlatformPlayStatus* status) {
    (void)iap_ctx;
    status->state          = IAPIPodStatePlayStatus_Playing;
    status->track_total_ms = 210000;
    status->track_pos_ms   = 15000;
    status->track_index    = 1;
    status->track_count    = ARRAYLEN(mock_tracks_all);
    return iap_true;
}

void iap_platform_control(struct IAPContext*               iap_ctx,
                          enum IAPPlatformControl          control,
                          struct IAPPlatformPendingControl pending) {
    struct mock_platform* mp = iap_ctx->platform;
    if(mp->control_count < ARRAYLEN(mp->last_controls)) {
        mp->last_controls[mp->control_count++] = control;
    }
    CHECK(iap_control_response(iap_ctx, pending, iap_true));
}

IAPBool iap_platform_get_volume(struct IAPContext*              iap_ctx,
                                struct IAPPlatformVolumeStatus* status) {
    (void)iap_ctx;
    status->volume = 128;
    status->muted  = iap_false;
    return iap_true;
}

IAPBool iap_platform_get_power_status(struct IAPContext*             iap_ctx,
                                      struct IAPPlatformPowerStatus* status) {
    (void)iap_ctx;
    status->state         = IAPIPodStatePowerState_ExternalCharged;
    status->battery_level = 255;
    return iap_true;
}

IAPBool iap_platform_get_shuffle_setting(struct IAPContext* iap_ctx,
                                         uint8_t*           status) {
    (void)iap_ctx;
    *status = IAPIPodStateShuffleSettingState_Off;
    return iap_true;
}

IAPBool iap_platform_set_shuffle_setting(struct IAPContext* iap_ctx,
                                         uint8_t            status,
                                         IAPBool            restore_on_exit) {
    (void)iap_ctx;
    (void)status;
    (void)restore_on_exit;
    return iap_true;
}

IAPBool iap_platform_get_repeat_setting(struct IAPContext* iap_ctx,
                                        uint8_t*           status) {
    (void)iap_ctx;
    *status = IAPIPodStateRepeatSettingState_Off;
    return iap_true;
}

static int     last_repeat_status = -1;
static IAPBool last_repeat_roe;

IAPBool iap_platform_set_repeat_setting(struct IAPContext* iap_ctx,
                                        uint8_t            status,
                                        IAPBool            restore_on_exit) {
    (void)iap_ctx;
    last_repeat_status = status;
    last_repeat_roe    = restore_on_exit;
    return iap_true;
}

IAPBool iap_platform_get_date_time(struct IAPContext*  iap_ctx,
                                   struct IAPDateTime* time) {
    (void)iap_ctx;
    memset(time, 0, sizeof(*time));
    time->year  = 2026;
    time->month = 11;
    time->day   = 24;
    return iap_true;
}

IAPBool iap_platform_get_backlight_level(struct IAPContext* iap_ctx,
                                         uint8_t*           level) {
    (void)iap_ctx;
    *level = 128;
    return iap_true;
}

IAPBool iap_platform_get_hold_switch_state(struct IAPContext* iap_ctx,
                                           IAPBool*           state) {
    (void)iap_ctx;
    *state = iap_false;
    return iap_true;
}

IAPBool iap_platform_get_indexed_track_info(struct IAPContext* iap_ctx,
                                            uint32_t           index,
                                            struct IAPPlatformTrackInfo* info) {
    (void)iap_ctx;
    if(index >= ARRAYLEN(mock_tracks_all)) {
        return iap_false;
    }
    if(info->total_ms)
        *info->total_ms = 210000;
    if(info->caps)
        *info->caps = (index == 0) ? IAPIPodStateTrackCapBits_HasAlbumArts : 0;
    if(info->artist) {
        static const char s[] = "Mock Artist";
        return iap_span_append(info->artist, s, sizeof(s));
    }
    if(info->album) {
        static const char s[] = "Mock Album";
        return iap_span_append(info->album, s, sizeof(s));
    }
    if(info->title) {
        return iap_span_append(info->title, mock_tracks_all[index],
                               strlen(mock_tracks_all[index]) + 1);
    }
    return iap_true;
}

IAPBool iap_platform_set_playing_track(struct IAPContext* iap_ctx,
                                       uint32_t           index) {
    (void)iap_ctx;
    return index < ARRAYLEN(mock_tracks_all) ? iap_true : iap_false;
}

IAPBool iap_platform_open_artwork(struct IAPContext* iap_ctx, uint32_t index,
                                  struct IAPPlatformArtwork* artwork) {
    (void)iap_ctx;
    (void)index;
    (void)artwork;
    return iap_false;
}

IAPBool iap_platform_get_artwork_ptr(struct IAPContext*         iap_ctx,
                                     struct IAPPlatformArtwork* artwork,
                                     struct IAPSpan*            span) {
    (void)iap_ctx;
    (void)artwork;
    (void)span;
    return iap_false;
}

IAPBool iap_platform_close_artwork(struct IAPContext*         iap_ctx,
                                   struct IAPPlatformArtwork* artwork) {
    (void)iap_ctx;
    (void)artwork;
    return iap_true;
}

IAPBool iap_platform_db_reset(struct IAPContext* iap_ctx,
                              uint8_t            selection_hierarchy) {
    struct mock_platform* mp = iap_ctx->platform;
    if(selection_hierarchy != 0 &&
       selection_hierarchy != IAPResetDBSelectionHierarchySelection_Audio &&
       selection_hierarchy != IAPResetDBSelectionHierarchySelection_Video) {
        return iap_false;
    }
    mp->db_hierarchy = selection_hierarchy;
    mp->sel_playlist = -1;
    mp->sel_artist   = -1;
    mp->sel_album    = -1;
    return iap_true;
}

IAPBool iap_platform_db_select(struct IAPContext* iap_ctx, uint8_t type,
                               uint32_t index) {
    struct mock_platform* mp    = iap_ctx->platform;
    uint32_t              count = 0;
    if(!iap_platform_db_get_count(iap_ctx, type, &count) || index >= count) {
        return iap_false;
    }
    if(type == IAPDatabaseType_Playlist) {
        mp->sel_playlist = (int32_t)index;
        mp->sel_artist   = -1;
        mp->sel_album    = -1;
    } else if(type == IAPDatabaseType_Artist) {
        mp->sel_playlist = -1;
        mp->sel_artist   = (int32_t)index;
        mp->sel_album    = -1;
    } else if(type == IAPDatabaseType_Album) {
        mp->sel_playlist = -1;
        mp->sel_album    = (int32_t)index;
    }
    return iap_true;
}

IAPBool iap_platform_db_get_count(struct IAPContext* iap_ctx, uint8_t type,
                                  uint32_t* count) {
    struct mock_platform* mp = iap_ctx->platform;
    if(mp->db_hierarchy == IAPResetDBSelectionHierarchySelection_Video) {
        *count = 0;
        return iap_true;
    }
    switch(type) {
    case IAPDatabaseType_Playlist:
        *count = ARRAYLEN(mock_playlists);
        return iap_true;
    case IAPDatabaseType_Artist:
        *count = ARRAYLEN(mock_artists);
        return iap_true;
    case IAPDatabaseType_Album:
        *count = (mp->sel_artist == 1) ? ARRAYLEN(mock_albums_b)
                                       : ARRAYLEN(mock_albums_a);
        return iap_true;
    case IAPDatabaseType_Track:
        if(mp->sel_artist == 0 && mp->sel_album == 0) {
            *count = ARRAYLEN(mock_tracks_a1);
        } else {
            *count = ARRAYLEN(mock_tracks_all);
        }
        return iap_true;
    case IAPDatabaseType_Audiobook:
        *count = 2;
        return iap_true;
    default:
        *count = 0;
        return iap_true;
    }
}

IAPBool iap_platform_db_get_record(struct IAPContext* iap_ctx, uint8_t type,
                                   uint32_t index, char* buf, size_t buf_size) {
    struct mock_platform* mp    = iap_ctx->platform;
    uint32_t              count = 0;
    if(!iap_platform_db_get_count(iap_ctx, type, &count) || index >= count) {
        return iap_false;
    }
    const char* src = "";
    switch(type) {
    case IAPDatabaseType_Playlist:
        src = mock_playlists[index];
        break;
    case IAPDatabaseType_Artist:
        src = mock_artists[index];
        break;
    case IAPDatabaseType_Album:
        src =
            (mp->sel_artist == 1) ? mock_albums_b[index] : mock_albums_a[index];
        break;
    case IAPDatabaseType_Track:
        src = (mp->sel_artist == 0 && mp->sel_album == 0)
                  ? mock_tracks_a1[index]
                  : mock_tracks_all[index];
        break;
    default:
        return iap_false;
    }
    if(mp->record_override != NULL) {
        src = mp->record_override;
    }
    snprintf(buf, buf_size, "%s", src);
    return iap_true;
}

void iap_platform_db_play_selection(struct IAPContext* iap_ctx,
                                    uint32_t           track_index,
                                    struct IAPPlatformPendingControl pending) {
    struct mock_platform* mp = iap_ctx->platform;
    mp->play_sel_called++;
    mp->play_sel_track   = track_index;
    mp->play_sel_pending = pending;
    if(!mp->defer_play_sel_ack) {
        CHECK(iap_control_response(iap_ctx, pending, iap_true));
    }
}

IAPBool iap_platform_on_acc_samprs_received(struct IAPContext* iap_ctx,
                                            struct IAPSpan*    samprs) {
    struct mock_platform* mp = iap_ctx->platform;
    mp->sampr_calls          = 0;
    while(samprs->size > 0) {
        uint32_t rate;
        if(!iap_span_read_32(samprs, &rate)) {
            return iap_false;
        }
        if(mp->sampr_calls < ARRAYLEN(mp->samprs)) {
            mp->samprs[mp->sampr_calls++] = rate;
        }
    }
    return mp->sampr_calls > 0 ? iap_true : iap_false;
}

void iap_platform_dump_hex(const void* ptr, size_t size) {
    (void)ptr;
    (void)size;
}

/* Test helpers */
static void send_pkt(struct IAPContext* ctx, struct mock_platform* mp,
                     uint8_t lingo, uint16_t cmd, bool has_trans_id,
                     uint16_t trans_id, const void* payload,
                     size_t payload_len) {
    uint8_t buf[512];
    size_t  pos = 0;
    buf[pos++]  = IAP_SOF_BYTE;
    size_t body = 1 + (lingo == IAPLingoID_ExtendedInterface ? 2 : 1) +
                  (has_trans_id ? 2 : 0) + payload_len;
    CHECK(body <= 252);
    buf[pos++] = (uint8_t)body;
    buf[pos++] = lingo;
    if(lingo == IAPLingoID_ExtendedInterface) {
        buf[pos++] = (uint8_t)(cmd >> 8);
        buf[pos++] = (uint8_t)(cmd & 0xFF);
    } else {
        buf[pos++] = (uint8_t)cmd;
    }
    if(has_trans_id) {
        buf[pos++] = (uint8_t)(trans_id >> 8);
        buf[pos++] = (uint8_t)(trans_id & 0xFF);
    }
    if(payload_len > 0) {
        memcpy(&buf[pos], payload, payload_len);
        pos += payload_len;
    }
    uint8_t sum = 0;
    for(size_t i = 1; i < pos; i++) {
        sum = (uint8_t)(sum + buf[i]);
    }
    buf[pos++] = (uint8_t)(0x100 - sum);

    mp->tx_count = 0;
    CHECK(_iap_feed_packet(ctx, buf, pos));
}

static void complete_pending_tx(struct IAPContext* ctx) {
    CHECK(iap_notify_send_complete(ctx));
}

static void init_test_ctx_speed(struct IAPContext*    ctx,
                                struct mock_platform* mp, IAPBool highspeed) {
    memset(mp, 0, sizeof(*mp));
    mp->ctx          = ctx;
    mp->sel_playlist = -1;
    mp->sel_artist   = -1;
    mp->sel_album    = -1;

    struct IAPOpts opts = {
        .usb_highspeed        = highspeed,
        .ignore_hid_report_id = iap_true,
    };
    CHECK(iap_init_ctx(ctx, opts, mp));
}

static void init_test_ctx(struct IAPContext* ctx, struct mock_platform* mp) {
    init_test_ctx_speed(ctx, mp, iap_true);
}

static void test_identify_lingoes_auth_modes(void) {
    check_case = "identify_lingoes_auth_modes";
    struct IAPContext    ctx;
    struct mock_platform mp;

    /* 1. NoAuth without StartIDPS (no trans_id) */
    init_test_ctx(&ctx, &mp);
    struct IAPIdentifyDeviceLingoesPayload id_noauth = {
        .lingoes_bits =
            swap_32(IAPIdentifyDeviceLingoesLingoBits_General |
                    IAPIdentifyDeviceLingoesLingoBits_ExtendedInterface),
        .options   = swap_32(IAPIdentifyDeviceLingoesOptions_NoAuth),
        .device_id = 0,
    };
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_IdentifyDeviceLingoes, false, 0, &id_noauth,
             sizeof(id_noauth));
    CHECK_EQ(mp.tx_count, 1);
    CHECK_EQ(mp.tx_packets[0].command, IAPGeneralCommandID_IPodAck);
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPAckStatus_Success);
    CHECK_EQ(ctx.phase, IAPPhase_Authed);
    complete_pending_tx(&ctx);

    /* Extended Interface command immediately succeeds after NoAuth */
    uint8_t pl_type = IAPDatabaseType_Playlist;
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_GetNumberCategorizedDBRecords, false,
             0, &pl_type, 1);
    CHECK_EQ(mp.tx_count, 1);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPExtendedInterfaceCommandID_ReturnNumberCategorizedDBRecords);
    CHECK_EQ(swap_32(*(uint32_t*)mp.tx_packets[0].payload),
             ARRAYLEN(mock_playlists));
    complete_pending_tx(&ctx);
    iap_deinit_ctx(&ctx);

    /* 2. DeferAuth WITH trans_id */
    init_test_ctx(&ctx, &mp);
    struct IAPIdentifyDeviceLingoesPayload id_defer = id_noauth;
    id_defer.options = swap_32(IAPIdentifyDeviceLingoesOptions_DeferAuth);
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_IdentifyDeviceLingoes, true, 0x0042, &id_defer,
             sizeof(id_defer));
    CHECK_EQ(mp.tx_count, 1);
    CHECK_EQ(mp.tx_packets[0].trans_id, 0x0042);
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPAckStatus_Success);
    CHECK_EQ(ctx.phase, IAPPhase_Authed);
    complete_pending_tx(&ctx);
    iap_deinit_ctx(&ctx);

    /* 3. Legacy Identify (0x01) */
    init_test_ctx(&ctx, &mp);
    uint8_t legacy_lingo = IAPLingoID_ExtendedInterface;
    send_pkt(&ctx, &mp, IAPLingoID_General, IAPGeneralCommandID_Identify, false,
             0, &legacy_lingo, 1);
    CHECK_EQ(mp.tx_count, 1);
    CHECK_EQ(mp.tx_packets[0].command, IAPGeneralCommandID_IPodAck);
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPAckStatus_Success);
    CHECK_EQ(ctx.phase, IAPPhase_Authed);
    complete_pending_tx(&ctx);
    iap_deinit_ctx(&ctx);
}

static void test_pre_idps_queries_and_ui_mode(void) {
    check_case = "pre_idps_queries_and_ui_mode";
    struct IAPContext    ctx;
    struct mock_platform mp;
    init_test_ctx(&ctx, &mp);

    /* Car asks Lingo Protocol Version before StartIDPS (with trans_id) */
    uint8_t ext_lingo = IAPLingoID_ExtendedInterface;
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_RequestLingoProtocolVersion, true, 0x0001,
             &ext_lingo, 1);
    CHECK_EQ(mp.tx_count, 1);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPGeneralCommandID_ReturnLingoProtocolVersion);
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPLingoID_ExtendedInterface);
    complete_pending_tx(&ctx);

    /* Now StartIDPS */
    send_pkt(&ctx, &mp, IAPLingoID_General, IAPGeneralCommandID_StartIDPS, true,
             0x0002, NULL, 0);
    CHECK_EQ(ctx.phase, IAPPhase_IDPS);
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPAckStatus_Success);
    complete_pending_tx(&ctx);

    /* GetUIMode / ExitExtendedInterfaceMode / EnterExtendedInterfaceMode */
    send_pkt(&ctx, &mp, IAPLingoID_General, IAPGeneralCommandID_GetUIMode, true,
             0x0003, NULL, 0);
    CHECK_EQ(mp.tx_packets[0].command, IAPGeneralCommandID_RetUIMode);
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPUIMode_Extended);
    complete_pending_tx(&ctx);

    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_ExitExtendedInterfaceMode, true, 0x0004, NULL,
             0);
    complete_pending_tx(&ctx);
    send_pkt(&ctx, &mp, IAPLingoID_General, IAPGeneralCommandID_GetUIMode, true,
             0x0005, NULL, 0);
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPUIMode_Standard);
    complete_pending_tx(&ctx);

    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_EnterExtendedInterfaceMode, true, 0x0006, NULL,
             0);
    complete_pending_tx(&ctx);
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_RequestExtendedInterfaceMode, true, 0x0007,
             NULL, 0);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPGeneralCommandID_ReturnExtendedInterfaceMode);
    CHECK_EQ(mp.tx_packets[0].payload[0], 1);
    complete_pending_tx(&ctx);

    iap_deinit_ctx(&ctx);
}

static void test_idps_fid_tokens_and_deferred_auth(void) {
    check_case = "idps_fid_tokens_and_deferred_auth";
    struct IAPContext    ctx;
    struct mock_platform mp;
    init_test_ctx(&ctx, &mp);

    send_pkt(&ctx, &mp, IAPLingoID_General, IAPGeneralCommandID_StartIDPS, true,
             0x0010, NULL, 0);
    complete_pending_tx(&ctx);

    /* SetFIDTokenValues with DeferAuth should return Accepted (0x00), never
     * RequiredFailed */
    uint8_t fid_buf[64];
    size_t  fpos    = 0;
    fid_buf[fpos++] = 1;    /* num_token_values */
    fid_buf[fpos++] = 12;   /* length of token bytes after length byte */
    fid_buf[fpos++] = 0x00; /* Identify type */
    fid_buf[fpos++] = 0x00; /* Identify subtype */
    fid_buf[fpos++] = 1;    /* num_lingoes */
    fid_buf[fpos++] = IAPLingoID_ExtendedInterface;
    struct IAPFIDTokenValuesIdentifyTokenTail tail = {
        .device_option = swap_32(IAPIdentifyDeviceLingoesOptions_DeferAuth),
        .device_id     = swap_32(0x12345678),
    };
    memcpy(&fid_buf[fpos], &tail, sizeof(tail));
    fpos += sizeof(tail);

    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_SetFIDTokenValues, true, 0x0011, fid_buf,
             fpos);
    CHECK_EQ(mp.tx_count, 1);
    CHECK_EQ(mp.tx_packets[0].command, IAPGeneralCommandID_AckFIDTokenValues);
    const struct IAPFIDTokenValuesIdentifyAck* ack =
        (const struct IAPFIDTokenValuesIdentifyAck*)&mp.tx_packets[0]
            .payload[1];
    CHECK_EQ(ack->status, IAPFIDTokenValuesIdentifyAckStatus_Accepted);
    complete_pending_tx(&ctx);

    uint8_t end_status = 0x00; /* continue */
    send_pkt(&ctx, &mp, IAPLingoID_General, IAPGeneralCommandID_EndIDPS, true,
             0x0012, &end_status, 1);
    complete_pending_tx(&ctx);
    CHECK_EQ(ctx.phase, IAPPhase_Auth);
    /* transition_idps_to_auth_cb queued GetAccessoryAuthenticationInfo; finish
     * its TX */
    complete_pending_tx(&ctx);

    /* Car using DeferAuth sends ExtendedInterface ResetDBSelection before
     * finishing auth cert */
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_ResetDBSelection, true, 0x0013, NULL,
             0);
    CHECK_EQ(mp.tx_count, 1);
    CHECK_EQ(mp.tx_packets[0].command, IAPExtendedInterfaceCommandID_IPodAck);
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPAckStatus_Success);
    complete_pending_tx(&ctx);

    /* Later completing deferred auth transitions to IAPPhase_Authed and
     * requests Digital Audio sample rates */
    struct IAPRetAccAuthInfoPayload2p0 cert = {
        .protocol_major             = 2,
        .protocol_minor             = 0,
        .cert_current_section_index = 0,
        .cert_max_section_index     = 0,
    };
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_RetAccessoryAuthenticationInfo, true, 0x0014,
             &cert, sizeof(cert));
    CHECK_EQ(mp.tx_packets[0].command,
             IAPGeneralCommandID_AckAccessoryAuthenticationInfo);
    complete_pending_tx(&ctx);
    complete_pending_tx(&ctx); /* send_auth_challenge_sig_cb */

    uint8_t fake_sig[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_RetAccessoryAuthenticationSignature, true,
             0x0015, fake_sig, sizeof(fake_sig));
    CHECK_EQ(mp.tx_packets[0].command,
             IAPGeneralCommandID_AckAccessoryAuthenticationStatus);
    CHECK_EQ(ctx.phase, IAPPhase_Authed);
    mp.tx_count = 0;
    complete_pending_tx(&ctx);
    CHECK_EQ(mp.tx_count, 1);
    CHECK_EQ(mp.tx_packets[0].lingo, IAPLingoID_DigitalAudio);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPDigitalAudioCommandID_GetAccessorySampleRateCaps);
    complete_pending_tx(&ctx);

    /* Verify Digital Audio SampleRateCaps supports 44.1kHz-only car */
    uint8_t report[16];
    report[0]       = 0x0C;
    report[1]       = 0x00;
    uint32_t rate44 = swap_32(44100);
    send_pkt(&ctx, &mp, IAPLingoID_DigitalAudio,
             IAPDigitalAudioCommandID_RetAccessorySampleRateCaps, true, 0x0014,
             &rate44, sizeof(rate44));
    CHECK_EQ(mp.sampr_calls, 1);
    CHECK_EQ(mp.samprs[0], 44100);
    (void)report;

    iap_deinit_ctx(&ctx);

    /* IDPS with unknown FID token + AccDigitalAudioSampleRates + Auth 1.0 */
    init_test_ctx(&ctx, &mp);
    send_pkt(&ctx, &mp, IAPLingoID_General, IAPGeneralCommandID_StartIDPS, true,
             0x0020, NULL, 0);
    complete_pending_tx(&ctx);

    fpos            = 0;
    fid_buf[fpos++] = 3; /* num_token_values */
    /* Token 1: Identify (ImmediateAuth) */
    fid_buf[fpos++]    = 12;
    fid_buf[fpos++]    = 0x00;
    fid_buf[fpos++]    = 0x00;
    fid_buf[fpos++]    = 1;
    fid_buf[fpos++]    = IAPLingoID_DigitalAudio;
    tail.device_option = swap_32(IAPIdentifyDeviceLingoesOptions_ImmediateAuth);
    memcpy(&fid_buf[fpos], &tail, sizeof(tail));
    fpos += sizeof(tail);
    /* Token 2: AccDigitalAudioSampleRates (44100 Hz) */
    fid_buf[fpos++] = 6;
    fid_buf[fpos++] = 0x00;
    fid_buf[fpos++] = 0x0E;
    memcpy(&fid_buf[fpos], &rate44, sizeof(rate44));
    fpos += sizeof(rate44);
    /* Token 3: Unknown FID token (0x0009) */
    fid_buf[fpos++] = 3;
    fid_buf[fpos++] = 0x00;
    fid_buf[fpos++] = 0x09;
    fid_buf[fpos++] = 0x01;

    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_SetFIDTokenValues, true, 0x0021, fid_buf,
             fpos);
    CHECK_EQ(mp.tx_count, 1);
    CHECK_EQ(mp.tx_packets[0].command, IAPGeneralCommandID_AckFIDTokenValues);
    CHECK_EQ(mp.tx_packets[0].payload[0], 3);
    complete_pending_tx(&ctx);

    send_pkt(&ctx, &mp, IAPLingoID_General, IAPGeneralCommandID_EndIDPS, true,
             0x0022, &end_status, 1);
    complete_pending_tx(&ctx);
    complete_pending_tx(&ctx);

    struct IAPRetAccAuthInfoPayload auth1 = {.protocol_major = 1,
                                             .protocol_minor = 0};
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_RetAccessoryAuthenticationInfo, true, 0x0023,
             &auth1, sizeof(auth1));
    CHECK_EQ(mp.tx_packets[0].command,
             IAPGeneralCommandID_AckAccessoryAuthenticationInfo);
    complete_pending_tx(&ctx);
    complete_pending_tx(&ctx); /* send_auth_challenge_sig_cb */

    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_RetAccessoryAuthenticationSignature, true,
             0x0024, fake_sig, sizeof(fake_sig));
    CHECK_EQ(ctx.phase, IAPPhase_Authed);
    mp.sampr_calls = 0;
    complete_pending_tx(&ctx); /* triggers apply_idps_samprs_cb */
    CHECK_EQ(mp.sampr_calls, 1);
    CHECK_EQ(mp.samprs[0], 44100);

    iap_deinit_ctx(&ctx);
}

static void enter_authed_session(struct IAPContext*    ctx,
                                 struct mock_platform* mp) {
    init_test_ctx(ctx, mp);
    struct IAPIdentifyDeviceLingoesPayload id = {
        .lingoes_bits =
            swap_32(IAPIdentifyDeviceLingoesLingoBits_General |
                    IAPIdentifyDeviceLingoesLingoBits_SimpleRemote |
                    IAPIdentifyDeviceLingoesLingoBits_ExtendedInterface),
        .options   = swap_32(IAPIdentifyDeviceLingoesOptions_NoAuth),
        .device_id = 0,
    };
    send_pkt(ctx, mp, IAPLingoID_General,
             IAPGeneralCommandID_IdentifyDeviceLingoes, true, 0x0001, &id,
             sizeof(id));
    complete_pending_tx(ctx);
}

static void test_database_browsing_and_streaming(void) {
    check_case = "database_browsing_and_streaming";
    struct IAPContext    ctx;
    struct mock_platform mp;
    enter_authed_session(&ctx, &mp);

    /* Legacy ExtendedInterface ProtocolVersion & IPodName */
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_RequestProtocolVersion, true, 0x0020,
             NULL, 0);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPExtendedInterfaceCommandID_ReturnProtocolVersion);
    CHECK_EQ(mp.tx_packets[0].payload[0], 1);
    CHECK_EQ(mp.tx_packets[0].payload[1], 14);
    complete_pending_tx(&ctx);

    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_RequestIPodName, true, 0x0021, NULL,
             0);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPExtendedInterfaceCommandID_ReturnIPodName);
    CHECK_EQ(strcmp((const char*)mp.tx_packets[0].payload, "Rockbox"), 0);
    complete_pending_tx(&ctx);

    /* Select Artist 0 -> Album 0 -> Track count should be 4 */
    struct IAPSelectDBRecord sel_artist = {.type  = IAPDatabaseType_Artist,
                                           .index = swap_32(0)};
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_SelectDBRecord, true, 0x0022,
             &sel_artist, sizeof(sel_artist));
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPAckStatus_Success);
    complete_pending_tx(&ctx);

    struct IAPSelectDBRecord sel_album = {.type  = IAPDatabaseType_Album,
                                          .index = swap_32(0)};
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_SelectDBRecord, true, 0x0023,
             &sel_album, sizeof(sel_album));
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPAckStatus_Success);
    complete_pending_tx(&ctx);

    uint8_t track_type = IAPDatabaseType_Track;
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_GetNumberCategorizedDBRecords, true,
             0x0024, &track_type, 1);
    CHECK_EQ(swap_32(*(uint32_t*)mp.tx_packets[0].payload),
             ARRAYLEN(mock_tracks_a1));
    complete_pending_tx(&ctx);

    /* Stream records 1..3 (3 packets!) */
    struct IAPRetrieveCategorizedDatabaseRecordsPayload ret_req = {
        .type  = IAPDatabaseType_Track,
        .index = swap_32(1),
        .count = swap_32(3),
    };
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_RetrieveCategorizedDatabaseRecords,
             true, 0x0025, &ret_req, sizeof(ret_req));
    CHECK_EQ(mp.tx_count, 1);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPExtendedInterfaceCommandID_ReturnCategorizedDatabaseRecords);
    CHECK_EQ(mp.tx_packets[0].trans_id, 0x0025);
    CHECK_EQ(swap_32(*(uint32_t*)mp.tx_packets[0].payload), 1);
    CHECK_EQ(strcmp((const char*)&mp.tx_packets[0].payload[4], "Track 2"), 0);

    mp.tx_count = 0;
    complete_pending_tx(&ctx);
    CHECK_EQ(mp.tx_count, 1);
    CHECK_EQ(mp.tx_packets[0].trans_id, 0x0025);
    CHECK_EQ(swap_32(*(uint32_t*)mp.tx_packets[0].payload), 2);
    CHECK_EQ(strcmp((const char*)&mp.tx_packets[0].payload[4], "Track 3"), 0);

    mp.tx_count = 0;
    complete_pending_tx(&ctx);
    CHECK_EQ(mp.tx_count, 1);
    CHECK_EQ(swap_32(*(uint32_t*)mp.tx_packets[0].payload), 3);
    CHECK_EQ(strcmp((const char*)&mp.tx_packets[0].payload[4], "Track 4"), 0);
    complete_pending_tx(&ctx);

    /* Ensure no extra packets after stream finishes */
    mp.tx_count = 0;
    complete_pending_tx(&ctx);
    CHECK_EQ(mp.tx_count, 0);

    /* Start stream count = -1 (0xFFFFFFFF) and cancel via CancelCommand */
    ret_req.index = swap_32(0);
    ret_req.count = swap_32(0xFFFFFFFFu);
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_RetrieveCategorizedDatabaseRecords,
             true, 0x0026, &ret_req, sizeof(ret_req));
    CHECK_EQ(mp.tx_count, 1);
    CHECK_EQ(swap_32(*(uint32_t*)mp.tx_packets[0].payload), 0);
    complete_pending_tx(&ctx); /* sends record 1 */
    CHECK_EQ(mp.tx_count, 2);
    complete_pending_tx(&ctx); /* sends record 2 */
    complete_pending_tx(&ctx); /* mark idle before next outgoing packet */

    struct IAPCancelCommandPayload cancel = {
        .lingo_id   = IAPLingoID_ExtendedInterface,
        .command_id = swap_16(
            IAPExtendedInterfaceCommandID_RetrieveCategorizedDatabaseRecords),
        .transaction_id = swap_16(0x0026)};
    send_pkt(&ctx, &mp, IAPLingoID_General, IAPGeneralCommandID_CancelCommand,
             true, 0x0027, &cancel, sizeof(cancel));
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPAckStatus_Success);
    complete_pending_tx(&ctx);
    mp.tx_count = 0;
    complete_pending_tx(&ctx);
    CHECK_EQ(mp.tx_count, 0);

    /* PlayCurrentSelection passes track_index to
     * iap_platform_db_play_selection */
    struct IAPPlayCurrentSelectionPayload play_sel = {.track_index =
                                                          swap_32(2)};
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_PlayCurrentSelection, true, 0x0028,
             &play_sel, sizeof(play_sel));
    CHECK_EQ(mp.play_sel_called, 1);
    CHECK_EQ(mp.play_sel_track, 2);
    complete_pending_tx(&ctx);

    /* GetDBITunesInfo UID & AudioTrackCount */
    uint8_t meta_uid = IAPITunesMetadataType_UID;
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_GetDBITunesInfo, true, 0x0029,
             &meta_uid, 1);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPExtendedInterfaceCommandID_RetDBITunesInfo);
    CHECK_EQ(memcmp(&mp.tx_packets[0].payload[1], "ROCKWHEL", 8), 0);
    complete_pending_tx(&ctx);

    uint8_t meta_audio = IAPITunesMetadataType_AudioTrackCount;
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_GetDBITunesInfo, true, 0x002A,
             &meta_audio, 1);
    CHECK_EQ(swap_32(*(uint32_t*)&mp.tx_packets[0].payload[1]),
             ARRAYLEN(mock_tracks_a1));
    complete_pending_tx(&ctx);

    /* Out-of-bounds DB selection returns EBadParameter */
    struct IAPSelectDBRecord bad_sel = {.type  = IAPDatabaseType_Artist,
                                        .index = swap_32(99)};
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_SelectDBRecord, true, 0x002B,
             &bad_sel, sizeof(bad_sel));
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPAckStatus_EBadParameter);
    complete_pending_tx(&ctx);

    /* ResetDBSelectionHierarchy Video leaves 0 records */
    uint8_t video_sel = IAPResetDBSelectionHierarchySelection_Video;
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_ResetDBSelectionHierarchy, true,
             0x002C, &video_sel, 1);
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPAckStatus_Success);
    complete_pending_tx(&ctx);
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_GetNumberCategorizedDBRecords, true,
             0x002D, &track_type, 1);
    CHECK_EQ(swap_32(*(uint32_t*)mp.tx_packets[0].payload), 0);
    complete_pending_tx(&ctx);

    iap_deinit_ctx(&ctx);
}

static void press_and_release_ctx_btn(struct IAPContext*    ctx,
                                      struct mock_platform* mp,
                                      uint16_t* trans_id, uint8_t b1,
                                      uint8_t b2) {
    struct IAPButtonStatusPayload press = {
        .bits1 = b1, .bits2 = b2, .bits3 = 0, .bits4 = 0};
    send_pkt(ctx, mp, IAPLingoID_SimpleRemote,
             IAPSimpleRemoteCommandID_ContextButtonStatus, true, (*trans_id)++,
             &press, sizeof(press));
    complete_pending_tx(ctx);
    struct IAPButtonStatusPayload rel = {0, 0, 0, 0};
    send_pkt(ctx, mp, IAPLingoID_SimpleRemote,
             IAPSimpleRemoteCommandID_ContextButtonStatus, true, (*trans_id)++,
             &rel, sizeof(rel));
    complete_pending_tx(ctx);
}

static void test_album_and_playlist_skip_controls(void) {
    check_case = "album_and_playlist_skip_controls";
    struct IAPContext    ctx;
    struct mock_platform mp;
    enter_authed_session(&ctx, &mp);

    uint16_t tid = 0x0100;
    press_and_release_ctx_btn(&ctx, &mp, &tid,
                              IAPContextButtonStatusButtons_NextTrack, 0);
    press_and_release_ctx_btn(&ctx, &mp, &tid,
                              IAPContextButtonStatusButtons_PrevTrack, 0);
    press_and_release_ctx_btn(&ctx, &mp, &tid,
                              IAPContextButtonStatusButtons_NextAlbum, 0);
    press_and_release_ctx_btn(&ctx, &mp, &tid,
                              IAPContextButtonStatusButtons_PrevAlbum, 0);
    press_and_release_ctx_btn(&ctx, &mp, &tid, 0,
                              IAPContextButtonStatusButtons_NextPlaylist);
    press_and_release_ctx_btn(&ctx, &mp, &tid, 0,
                              IAPContextButtonStatusButtons_PrevPlaylist);

    uint8_t next_ch = IAPPlayControlCode_NextChapter;
    uint8_t prev_ch = IAPPlayControlCode_PrevChapter;
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_PlayControl, true, tid++, &next_ch,
             1);
    complete_pending_tx(&ctx);
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_PlayControl, true, tid++, &prev_ch,
             1);
    complete_pending_tx(&ctx);

    CHECK_EQ(mp.control_count, 8);
    CHECK_EQ(mp.last_controls[0], IAPPlatformControl_Next);
    CHECK_EQ(mp.last_controls[1], IAPPlatformControl_Prev);
    CHECK_EQ(mp.last_controls[2], IAPPlatformControl_NextAlbum);
    CHECK_EQ(mp.last_controls[3], IAPPlatformControl_PrevAlbum);
    CHECK_EQ(mp.last_controls[4], IAPPlatformControl_NextPlaylist);
    CHECK_EQ(mp.last_controls[5], IAPPlatformControl_PrevPlaylist);
    CHECK_EQ(mp.last_controls[6], IAPPlatformControl_NextAlbum);
    CHECK_EQ(mp.last_controls[7], IAPPlatformControl_PrevAlbum);

    iap_deinit_ctx(&ctx);
}

static uint64_t read_u64_be(const uint8_t* p) {
    uint64_t raw = 0;
    memcpy(&raw, p, sizeof(raw));
    return swap_64(raw);
}

static uint32_t read_u32_be(const uint8_t* p) {
    uint32_t raw = 0;
    memcpy(&raw, p, sizeof(raw));
    return swap_32(raw);
}

static uint16_t read_u16_be(const uint8_t* p) {
    uint16_t raw = 0;
    memcpy(&raw, p, sizeof(raw));
    return swap_16(raw);
}

static void test_vehicle_protocol_completeness(void) {
    check_case = "vehicle_protocol_completeness";
    /* Verify swap_64 preserves all 8 bytes */
    CHECK_EQ(swap_64(0x0123456789ABCDEFULL), 0xEFCDAB8967452301ULL);
    CHECK_EQ(swap_64(swap_64(0x0123456789ABCDEFULL)), 0x0123456789ABCDEFULL);

    struct IAPContext    ctx;
    struct mock_platform mp;
    enter_authed_session(&ctx, &mp);
    uint16_t tid = 0x0200;

    /* General: RequestIPodSoftwareVersion -> 2.0.4 */
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_RequestIPodSoftwareVersion, true, tid++, NULL,
             0);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPGeneralCommandID_ReturnIPodSoftwareVersion);
    CHECK_EQ(mp.tx_packets[0].payload[0], 2);
    CHECK_EQ(mp.tx_packets[0].payload[1], 0);
    CHECK_EQ(mp.tx_packets[0].payload[2], 4);
    complete_pending_tx(&ctx);

    /* General: RequestIPodModelNum -> 4-byte 0x00130200 + "MC293" */
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_RequestIPodModelNum, true, tid++, NULL, 0);
    CHECK_EQ(mp.tx_packets[0].command, IAPGeneralCommandID_ReturnIPodModelNum);
    CHECK_EQ(read_u32_be(mp.tx_packets[0].payload), 0x00130200U);
    CHECK_EQ(strcmp((const char*)&mp.tx_packets[0].payload[4], "MC293"), 0);
    complete_pending_tx(&ctx);

    /* General: GetIPodOptions & GetIPodPreferences */
    send_pkt(&ctx, &mp, IAPLingoID_General, IAPGeneralCommandID_GetIPodOptions,
             true, tid++, NULL, 0);
    CHECK_EQ(mp.tx_packets[0].command, IAPGeneralCommandID_RetIPodOptions);
    CHECK_EQ(read_u64_be(mp.tx_packets[0].payload),
             (uint64_t)IAPRetIPodOptionsState_SetiPodPreferencesSupport);
    complete_pending_tx(&ctx);

    uint8_t pref_class = IAPIPodPereferenceClassID_LineOutUsage;
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_GetIPodPreferences, true, tid++, &pref_class,
             1);
    CHECK_EQ(mp.tx_packets[0].command, IAPGeneralCommandID_RetIPodPreferences);
    CHECK_EQ(mp.tx_packets[0].payload[0],
             IAPIPodPereferenceClassID_LineOutUsage);
    CHECK_EQ(mp.tx_packets[0].payload[1],
             IAPIPodPreferenceLineOutUsageSettingID_Used);
    complete_pending_tx(&ctx);

    /* General: GetIPodOptionsForLingo capability bits */
    uint8_t q_lingo = IAPLingoID_General;
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_GetIPodOptionsForLingo, true, tid++, &q_lingo,
             1);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPGeneralCommandID_RetIPodOptionsForLingo);
    CHECK_EQ(
        read_u64_be(&mp.tx_packets[0].payload[1]),
        (uint64_t)IAPRetIPodOptionsForLingoGeneralBits_LineOutUsage |
            (uint64_t)IAPRetIPodOptionsForLingoGeneralBits_DeviceNotification |
            (uint64_t)
                IAPRetIPodOptionsForLingoGeneralBits_PauseOnRemovalControl);
    complete_pending_tx(&ctx);

    q_lingo = IAPLingoID_DisplayRemote;
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_GetIPodOptionsForLingo, true, tid++, &q_lingo,
             1);
    const uint64_t dr_options =
        IAPRetIPodOptionsForLingoDisplayRemoteBits_UIVolumeControl |
        IAPRetIPodOptionsForLingoDisplayRemoteBits_AbsoluteVolumeControl;
    CHECK_EQ(read_u64_be(&mp.tx_packets[0].payload[1]), dr_options);
    complete_pending_tx(&ctx);

    q_lingo = IAPLingoID_ExtendedInterface;
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_GetIPodOptionsForLingo, true, tid++, &q_lingo,
             1);
    const uint64_t ei_options =
        IAPRetIPodOptionsForLingoExtendedInterfaceBits_DisplayImages |
        IAPRetIPodOptionsForLingoExtendedInterfaceBits_CategoryListAccess |
        IAPRetIPodOptionsForLingoExtendedInterfaceBits_PlayControl;
    CHECK_EQ(read_u64_be(&mp.tx_packets[0].payload[1]), ei_options);
    complete_pending_tx(&ctx);

    q_lingo = IAPLingoID_DigitalAudio;
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_GetIPodOptionsForLingo, true, tid++, &q_lingo,
             1);
    CHECK_EQ(
        read_u64_be(&mp.tx_packets[0].payload[1]),
        (uint64_t)
            IAPRetIPodOptionsForLingoDigitalAudioBits_SampleRateCapsFIDToken);
    complete_pending_tx(&ctx);

    /* General: SetEventNotification / GetEventNotification / AppBundle /
     * Localization */
    uint64_t ev_mask = swap_64(0x0000123400000004ULL);
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_SetEventNotification, true, tid++, &ev_mask,
             sizeof(ev_mask));
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPAckStatus_Success);
    complete_pending_tx(&ctx);
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_GetEventNotification, true, tid++, NULL, 0);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPGeneralCommandID_RetEventNotification);
    CHECK_EQ(read_u64_be(mp.tx_packets[0].payload), 0x0000123400000004ULL);
    complete_pending_tx(&ctx);

    uint8_t chg_state = IAPInternalBatteryChargingState_MayCharge;
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_SetInternalBatteryChargingState, true, tid++,
             &chg_state, 1);
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPAckStatus_Success);
    complete_pending_tx(&ctx);

    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_GetNowPlayingApplicationBundleName, true,
             tid++, NULL, 0);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPGeneralCommandID_RetNowPlayingApplicationBundleName);
    CHECK_EQ(
        strcmp((const char*)mp.tx_packets[0].payload, "com.apple.mobileipod"),
        0);
    complete_pending_tx(&ctx);

    uint8_t loc_type = IAPLocalicationInfoType_Language;
    send_pkt(&ctx, &mp, IAPLingoID_General,
             IAPGeneralCommandID_GetLocalizationInfo, true, tid++, &loc_type,
             1);
    CHECK_EQ(mp.tx_packets[0].command, IAPGeneralCommandID_RetLocalizationInfo);
    CHECK_EQ(strcmp((const char*)&mp.tx_packets[0].payload[1], "en"), 0);
    complete_pending_tx(&ctx);

    /* DisplayRemote: EQ, RemoteEventStatus, PlaybackEngineContents,
     * SetIPodStateInfo, SoundCheck */
    send_pkt(&ctx, &mp, IAPLingoID_DisplayRemote,
             IAPDisplayRemoteCommandID_GetCurrentEQProfileIndex, true, tid++,
             NULL, 0);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPDisplayRemoteCommandID_RetCurrentEQProfileIndex);
    CHECK_EQ(read_u32_be(mp.tx_packets[0].payload), 0U);
    complete_pending_tx(&ctx);

    send_pkt(&ctx, &mp, IAPLingoID_DisplayRemote,
             IAPDisplayRemoteCommandID_GetNumEQProfiles, true, tid++, NULL, 0);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPDisplayRemoteCommandID_RetNumEQProfiles);
    CHECK_EQ(read_u32_be(mp.tx_packets[0].payload), 1U);
    complete_pending_tx(&ctx);

    uint32_t eq_idx = swap_32(0);
    send_pkt(&ctx, &mp, IAPLingoID_DisplayRemote,
             IAPDisplayRemoteCommandID_GetIndexedEQProfileName, true, tid++,
             &eq_idx, sizeof(eq_idx));
    CHECK_EQ(mp.tx_packets[0].command,
             IAPDisplayRemoteCommandID_RetIndexedEQProfileName);
    CHECK_EQ(strcmp((const char*)mp.tx_packets[0].payload, "Flat"), 0);
    complete_pending_tx(&ctx);

    send_pkt(&ctx, &mp, IAPLingoID_DisplayRemote,
             IAPDisplayRemoteCommandID_GetRemoteEventStatus, true, tid++, NULL,
             0);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPDisplayRemoteCommandID_RetRemoteEventStatus);
    complete_pending_tx(&ctx);

    uint8_t pb_contents = IAPIPodStateType_PlaybackEngineContents;
    send_pkt(&ctx, &mp, IAPLingoID_DisplayRemote,
             IAPDisplayRemoteCommandID_GetIPodStateInfo, true, tid++,
             &pb_contents, 1);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPDisplayRemoteCommandID_RetIPodStateInfo);
    CHECK_EQ(read_u32_be(&mp.tx_packets[0].payload[1]),
             ARRAYLEN(mock_tracks_all));
    complete_pending_tx(&ctx);

    uint8_t set_state[5] = {IAPIPodStateType_TrackPlaybackIndex, 0, 0, 0, 2};
    send_pkt(&ctx, &mp, IAPLingoID_DisplayRemote,
             IAPDisplayRemoteCommandID_SetIPodStateInfo, true, tid++, set_state,
             sizeof(set_state));
    CHECK_EQ(mp.tx_packets[0].command, IAPDisplayRemoteCommandID_IPodAck);
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPAckStatus_Success);
    complete_pending_tx(&ctx);

    send_pkt(&ctx, &mp, IAPLingoID_DisplayRemote,
             IAPDisplayRemoteCommandID_GetSoundCheckState, true, tid++, NULL,
             0);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPDisplayRemoteCommandID_RetSoundCheckState);
    CHECK_EQ(mp.tx_packets[0].payload[0], 0);
    complete_pending_tx(&ctx);

    send_pkt(&ctx, &mp, IAPLingoID_DisplayRemote,
             IAPDisplayRemoteCommandID_GetArtworkFormats, true, tid++, NULL, 0);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPDisplayRemoteCommandID_RetArtworkFormats);
    CHECK_EQ(mp.tx_packets[0].payload_len, sizeof(struct IAPArtworkFormat));
    CHECK_EQ(read_u16_be(&mp.tx_packets[0].payload[0]), 0U);
    CHECK_EQ(mp.tx_packets[0].payload[2], IAPArtworkPixelFormats_RGB565LE);
    CHECK_EQ(read_u16_be(&mp.tx_packets[0].payload[3]), 128U);
    CHECK_EQ(read_u16_be(&mp.tx_packets[0].payload[5]), 128U);
    complete_pending_tx(&ctx);

    struct IAPGetIndexedPlayingTrackInfoPayload dr_art = {
        .type          = IAPIndexedPlayingTrackInfoType_ArtworkCount,
        .track_index   = swap_32(1),
        .chapter_index = 0,
    };
    send_pkt(&ctx, &mp, IAPLingoID_DisplayRemote,
             IAPDisplayRemoteCommandID_GetIndexedPlayingTrackInfo, true, tid++,
             &dr_art, sizeof(dr_art));
    CHECK_EQ(mp.tx_packets[0].command,
             IAPDisplayRemoteCommandID_RetIndexedPlayingTrackInfo);
    CHECK_EQ(mp.tx_packets[0].payload[0],
             IAPIndexedPlayingTrackInfoType_ArtworkCount);
    CHECK_EQ(read_u16_be(&mp.tx_packets[0].payload[3]), 0U);
    complete_pending_tx(&ctx);

    /* ExtendedInterface: 320x240 limits, ArtworkFormats, ArtworkCount,
     * SelectSortDBRecord, GetArtworkTimes */
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_GetArtworkFormats, true, tid++, NULL,
             0);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPExtendedInterfaceCommandID_RetArtworkFormats);
    CHECK_EQ(mp.tx_packets[0].payload_len, sizeof(struct IAPArtworkFormat));
    CHECK_EQ(read_u16_be(&mp.tx_packets[0].payload[0]), 0U);
    CHECK_EQ(mp.tx_packets[0].payload[2], IAPArtworkPixelFormats_RGB565LE);
    CHECK_EQ(read_u16_be(&mp.tx_packets[0].payload[3]), 128U);
    CHECK_EQ(read_u16_be(&mp.tx_packets[0].payload[5]), 128U);
    complete_pending_tx(&ctx);
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_GetColorDisplayImageLimits, true,
             tid++, NULL, 0);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPExtendedInterfaceCommandID_ReturnColorDisplayImageLimits);
    CHECK_EQ(read_u16_be(&mp.tx_packets[0].payload[0]), 320U);
    CHECK_EQ(read_u16_be(&mp.tx_packets[0].payload[2]), 240U);
    complete_pending_tx(&ctx);

    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_GetMonoDisplayImageLimits, true,
             tid++, NULL, 0);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPExtendedInterfaceCommandID_ReturnMonoDisplayImageLimits);
    CHECK_EQ(read_u16_be(&mp.tx_packets[0].payload[0]), 320U);
    CHECK_EQ(read_u16_be(&mp.tx_packets[0].payload[2]), 240U);
    complete_pending_tx(&ctx);

    struct IAPExtendedGetIndexedPlayingTrackInfoPayload ext_info = {
        .type        = IAPExtendedIndexedPlayingTrackInfoType_TrackArtworkCount,
        .track_index = 0,
        .chapter_index = 0,
    };
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_GetIndexedPlayingTrackInfo, true,
             tid++, &ext_info, sizeof(ext_info));
    CHECK_EQ(mp.tx_packets[0].command,
             IAPExtendedInterfaceCommandID_ReturnIndexedPlayingTrackInfo);
    CHECK_EQ(mp.tx_packets[0].payload[0],
             IAPExtendedIndexedPlayingTrackInfoType_TrackArtworkCount);
    CHECK_EQ(read_u16_be(&mp.tx_packets[0].payload[3]), 1U);
    complete_pending_tx(&ctx);

    uint8_t sort_sel[6] = {IAPDatabaseType_Artist, 0, 0, 0, 1, 0xFF};
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_SelectSortDBRecord, true, tid++,
             sort_sel, sizeof(sort_sel));
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPAckStatus_Success);
    CHECK_EQ(mp.sel_artist, 1);
    complete_pending_tx(&ctx);

    struct IAPGetArtworkTimesIndexPayload art_t = {
        .id_type       = IAPGetArtworkTrackIDType_PlaybackListIndex,
        .index         = swap_32(1),
        .format_id     = 0,
        .artwork_index = 0,
        .artwork_count = swap_16(1),
    };
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_GetArtworkTimes, true, tid++, &art_t,
             sizeof(art_t));
    CHECK_EQ(mp.tx_packets[0].command,
             IAPExtendedInterfaceCommandID_RetArtworkTimes);
    CHECK_EQ(mp.tx_packets[0].payload_len,
             sizeof(struct IAPRetArtworkTimesIndexPayload) + 4);
    complete_pending_tx(&ctx);

    /* ExtendedInterface: GetTrackArtworkTimes with count 0, 1 and the
     * wildcard 0xFFFF a Volvo head unit sends (clamped to one zero offset).
     * The response offsets are written byte-wise: the span is unaligned on
     * target and a uint32_t store there data-aborted the usb thread. */
    {
        static const uint16_t counts[]   = {0, 1, 0xFFFF};
        static const size_t   expected[] = {0, 4, 4};
        for(size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
            struct IAPGetTrackArtworkTimesPayload tat = {
                .track_index   = swap_32(0),
                .format_id     = 0,
                .artwork_index = 0,
                .artwork_count = swap_16(counts[i]),
            };
            send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
                     IAPExtendedInterfaceCommandID_GetTrackArtworkTimes, true,
                     tid++, &tat, sizeof(tat));
            CHECK_EQ(mp.tx_packets[0].command,
                     IAPExtendedInterfaceCommandID_RetTrackArtworkTimes);
            CHECK_EQ(mp.tx_packets[0].payload_len, expected[i]);
            if(expected[i] == 4) {
                CHECK_EQ(read_u32_be(mp.tx_packets[0].payload), 0U);
            }
            complete_pending_tx(&ctx);
        }
    }

    /* ExtendedInterface: SetRepeat keeps the restore-on-exit flag a Volvo
     * head unit sends, so the change stays to the accessory session */
    {
        static const uint8_t roes[] = {1, 0};
        for(size_t i = 0; i < sizeof(roes) / sizeof(roes[0]); i++) {
            struct IAPSetRepeatPayload rep = {
                .mode            = IAPIPodStateRepeatSettingState_All,
                .restore_on_exit = roes[i],
            };
            last_repeat_status = -1;
            send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
                     IAPExtendedInterfaceCommandID_SetRepeat, true, tid++, &rep,
                     sizeof(rep));
            CHECK_EQ(mp.tx_packets[0].payload[0], IAPAckStatus_Success);
            CHECK_EQ(last_repeat_status, IAPIPodStateRepeatSettingState_All);
            CHECK_EQ(last_repeat_roe, roes[i] ? iap_true : iap_false);
            complete_pending_tx(&ctx);
        }
    }

    /* DigitalAudio: SetVideoDelay */
    uint32_t vdelay = 0;
    send_pkt(&ctx, &mp, IAPLingoID_DigitalAudio,
             IAPDigitalAudioCommandID_SetVideoDelay, true, tid++, &vdelay,
             sizeof(vdelay));
    CHECK_EQ(mp.tx_packets[0].command, IAPDigitalAudioCommandID_IPodAck);
    CHECK_EQ(mp.tx_packets[0].payload[0], IAPAckStatus_Success);
    complete_pending_tx(&ctx);

    /* Storage: GetIPodCaps & GetIPodFreeSpace & FeatureFile */
    send_pkt(&ctx, &mp, IAPLingoID_Storage, IAPStorageCommandID_GetIPodCaps,
             true, tid++, NULL, 0);
    CHECK_EQ(mp.tx_packets[0].command, IAPStorageCommandID_RetIPodCaps);
    CHECK_EQ(read_u64_be(mp.tx_packets[0].payload),
             64ULL * 1024ULL * 1024ULL * 1024ULL);
    complete_pending_tx(&ctx);

    send_pkt(&ctx, &mp, IAPLingoID_Storage,
             IAPStorageCommandID_GetIPodFreeSpace, true, tid++, NULL, 0);
    CHECK_EQ(mp.tx_packets[0].command, IAPStorageCommandID_RetIPodFreeSpace);
    CHECK_EQ(read_u64_be(mp.tx_packets[0].payload),
             16ULL * 1024ULL * 1024ULL * 1024ULL);
    complete_pending_tx(&ctx);

    iap_deinit_ctx(&ctx);
}

/* At full speed a response longer than one 63-byte report goes out in
 * several. A request arriving meanwhile (a Volvo XC40 does not wait for the
 * records it asked for) must not overwrite it; it is answered after it. */
static void test_request_during_multi_report_response(void) {
    check_case = "request_during_multi_report_response";
    struct IAPContext    ctx;
    struct mock_platform mp;
    init_test_ctx_speed(&ctx, &mp, iap_false);

    static const char long_name[] =
        "A playlist name long enough to need several full-speed HID reports";
    mp.record_override = long_name;

    /* opens the session and settles transaction ID support */
    send_pkt(&ctx, &mp, IAPLingoID_General, IAPGeneralCommandID_StartIDPS, true,
             1, NULL, 0);
    CHECK_EQ(mp.tx_count, 1);
    complete_pending_tx(&ctx);

    struct IAPRetrieveCategorizedDatabaseRecordsPayload ret_req = {
        .type  = IAPDatabaseType_Playlist,
        .index = swap_32(0),
        .count = swap_32(1),
    };
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_RetrieveCategorizedDatabaseRecords,
             true, 2, &ret_req, sizeof(ret_req));
    CHECK_EQ(mp.tx_count, 0);
    CHECK(mp.tx_in_progress);

    /* arrives after the first report of the response */
    send_pkt(&ctx, &mp, IAPLingoID_ExtendedInterface,
             IAPExtendedInterfaceCommandID_GetNumPlayingTracks, true, 3, NULL,
             0);
    CHECK_EQ(mp.tx_count, 0);

    for(int i = 0; i < 16 && mp.tx_count < 2; i++) {
        complete_pending_tx(&ctx);
    }
    CHECK_EQ(mp.tx_count, 2);
    CHECK_EQ(mp.tx_packets[0].command,
             IAPExtendedInterfaceCommandID_ReturnCategorizedDatabaseRecords);
    CHECK_EQ(mp.tx_packets[0].trans_id, 2);
    CHECK_EQ(swap_32(*(uint32_t*)mp.tx_packets[0].payload), 0);
    CHECK_EQ(strcmp((const char*)&mp.tx_packets[0].payload[4], long_name), 0);
    CHECK_EQ(mp.tx_packets[1].command,
             IAPExtendedInterfaceCommandID_ReturnNumPlayingTracks);
    CHECK_EQ(mp.tx_packets[1].trans_id, 3);
    CHECK_EQ(swap_32(*(uint32_t*)mp.tx_packets[1].payload),
             ARRAYLEN(mock_tracks_all));
    complete_pending_tx(&ctx);

    iap_deinit_ctx(&ctx);
}

int main(void) {
    check_time_limit(30);
    test_identify_lingoes_auth_modes();
    test_pre_idps_queries_and_ui_mode();
    test_idps_fid_tokens_and_deferred_auth();
    test_database_browsing_and_streaming();
    test_album_and_playlist_skip_controls();
    test_vehicle_protocol_completeness();
    test_request_during_multi_report_response();
    return check_done("iap");
}
