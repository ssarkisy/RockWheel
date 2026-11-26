#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* Define to record every iAP accessory session (USB setup traffic, iAP
 * packets and decisions) to ROCKBOX_DIR "/iap_trace.log", for diagnosing a
 * head unit. The file is appended to and never trimmed. */
/* #define IAP_TRACE */

#ifdef USB_ENABLE_IAP
/* usbstack/iap/notification.c */
void iap_on_track_time_position(uint32_t pos_ms);
void iap_on_track_playback_index(uint32_t index, bool track_ready);
void iap_on_tracks_count(uint32_t count);
void iap_on_play_status(int status /* AUDIO_STATUS_* */);
void iap_on_volume(int volume);
void iap_on_shuffle_state(int state); /* SHUFFLE_OFF, _SONGS, _ALBUMS */
void iap_on_repeat_state(int state);
#else
static inline void iap_on_track_time_position(uint32_t pos_ms) {
    (void)pos_ms;
}
static inline void iap_on_track_playback_index(uint32_t index, bool track_ready) {
    (void)index;
    (void)track_ready;
}
static inline void iap_on_tracks_count(uint32_t count) {
    (void)count;
}
static inline void iap_on_play_status(int status) {
    (void)status;
}
static inline void iap_on_volume(int volume) {
    (void)volume;
}
static inline void iap_on_shuffle_state(int state) {
    (void)state;
}
static inline void iap_on_repeat_state(int state) {
    (void)state;
}
#endif

#if defined(USB_ENABLE_IAP) && defined(IAP_TRACE)
/* usbstack/iap/debug.c */
void iap_trace_logf(const char* fmt, ...);
void iap_trace_hex(const char* prefix, const void* data, size_t len);
void iap_trace_flush(void);
bool iap_trace_needs_flush(void);
#else
#define iap_trace_logf(...)        do { } while(0)
#define iap_trace_hex(...)         do { } while(0)
#define iap_trace_flush()          do { } while(0)
#define iap_trace_needs_flush()    false
#endif
