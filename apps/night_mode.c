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
#include <string.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "action.h"
#include "audio.h"
#include "backlight.h"
#include "lcd.h"
#include "misc.h"
#include "splash.h"
#include "status.h"
#include "lang.h"
#include "screens.h"
#include "settings.h"
#include "sound.h"
#include "timefuncs.h"
#include "viewport.h"
#include "gui/skin_engine/skin_engine.h"
#include "gui/skin_engine/cover_palette.h"
#include "night_mode.h"

/* What the mode has changed, and so what it has the old value of */
#define NIGHT_CHANGED_DARK   0x01
#define NIGHT_CHANGED_WHEEL  0x02
#define NIGHT_CHANGED_DIM    0x04
#define NIGHT_CHANGED_LIMIT  0x08

/* How often the clock is looked at */
#define POLL_TIME (5 * HZ)
/* Off for this long, what the mode was left at no longer says anything */
#define LONG_OFF_MINUTES (12 * 60)
/* And how closely that is kept */
#define SEEN_STEP_MINUTES 60

static bool theme_changed;
/* Loading the settings calls their callbacks as well: night_mode_boot() sorts it all
   out afterwards */
static bool settings_loaded;

#define THEME_LIGHT_NAME "iClassic-Square"
#define THEME_DARK_NAME THEME_LIGHT_NAME "-Dark"

/* The theme with a dark version. A theme of someone's own stays as it is. */
static bool theme_has_dark(void)
{
    return !strcmp(global_settings.wps_file, THEME_LIGHT_NAME)
        || !strcmp(global_settings.wps_file, THEME_DARK_NAME);
}

static void theme_set_names(bool dark)
{
    const char *const name = dark ? THEME_DARK_NAME : THEME_LIGHT_NAME;
    strmemccpy(global_settings.wps_file, name, sizeof(global_settings.wps_file));
    strmemccpy(global_settings.sbs_file, name, sizeof(global_settings.sbs_file));
    strmemccpy(global_settings.fms_file, name, sizeof(global_settings.fms_file));
    strmemccpy(global_settings.backdrop_file,
               dark ? BACKDROP_DIR "/iClassic_bd_dark.bmp" : BACKDROP_DIR "/iClassic_bd.bmp",
               sizeof(global_settings.backdrop_file));
    global_settings.fg_color = dark ? LCD_RGBPACK(0xf2, 0xf2, 0xf6) : LCD_RGBPACK(0, 0, 0);
    global_settings.bg_color = dark ? LCD_RGBPACK(THEME_DARK_BG_R, THEME_DARK_BG_G,
                                                  THEME_DARK_BG_B)
                                    : LCD_RGBPACK(0xff, 0xff, 0xff);
}

/* Music goes on: only the images change, and each has the size it had */
static void theme_apply(void)
{
    if (!theme_has_dark())
        return;
    /* whatever the screen shows is drawn anew when it's done */
    splash(0, ID2P(LANG_THEME_APPLYING));
    cover_palette_theme_changed();
    theme_set_names(global_settings.dark_theme);
    screens[SCREEN_MAIN].set_foreground(global_settings.fg_color);
    screens[SCREEN_MAIN].set_background(global_settings.bg_color);
    skin_reload_while_playing();
    theme_changed = true;
}

bool night_mode_theme_changed(void)
{
    const bool changed = theme_changed;
    theme_changed = false;
    return changed;
}

/* Puts what the mode asks for in the settings, and keeps what they were */
static void night_settings_on(void)
{
    struct user_settings *const s = &global_settings;

    s->night_changed = 0;
    if (s->night_dark && !s->dark_theme)
    {
        s->night_changed |= NIGHT_CHANGED_DARK;
        s->dark_theme = true;
    }
    if (s->night_silent)
    {
        s->night_changed |= NIGHT_CHANGED_WHEEL;
        s->night_old_keyclick = s->keyclick;
        s->keyclick = 0;
#ifdef HAVE_HARDWARE_CLICK
        s->night_old_keyclick_hw = s->keyclick_hardware;
        s->keyclick_hardware = false;
#endif
    }
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    if (s->night_dim)
    {
        s->night_changed |= NIGHT_CHANGED_DIM;
        s->night_old_brightness = s->brightness;
        s->brightness = s->night_brightness;
    }
#endif
    if (s->night_limit)
    {
        s->night_changed |= NIGHT_CHANGED_LIMIT;
        s->night_old_limit = s->volume_limit;
        s->night_old_volume = global_status.volume;
        s->volume_limit = MIN(s->volume_limit, s->night_volume);
    }
}

/* Puts back what it has changed. The volume goes back too, if the mode brought it down,
   but for at the end of the schedule while music plays: someone may be asleep to it. */
static void night_settings_off(bool by_schedule)
{
    struct user_settings *const s = &global_settings;

    if (s->night_changed & NIGHT_CHANGED_DARK)
        s->dark_theme = false;
    if (s->night_changed & NIGHT_CHANGED_WHEEL)
    {
        s->keyclick = s->night_old_keyclick;
#ifdef HAVE_HARDWARE_CLICK
        s->keyclick_hardware = s->night_old_keyclick_hw;
#endif
    }
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    if (s->night_changed & NIGHT_CHANGED_DIM)
        s->brightness = s->night_old_brightness;
#endif
    if (s->night_changed & NIGHT_CHANGED_LIMIT)
    {
        const int limit = s->volume_limit;
        s->volume_limit = s->night_old_limit;
        if (global_status.volume == limit && s->night_old_volume > limit
            && !(by_schedule && (audio_status() & AUDIO_STATUS_PLAY)))
            global_status.volume = MIN(s->night_old_volume, s->volume_limit);
    }
    s->night_changed = 0;
}

/* What they now say, on the screen and in the sound */
static void night_apply_settings(bool old_dark, int old_volume)
{
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    backlight_set_brightness(global_settings.brightness);
#endif
    /* only if the sound changes: it brings up the volume on Now Playing */
    if (global_status.volume != old_volume
        || global_status.volume > global_settings.volume_limit)
        setvol();
    if (global_settings.dark_theme != old_dark)
        theme_apply();
    settings_save();
}

static void night_set(bool on, bool by_schedule)
{
    const bool old_dark = global_settings.dark_theme;
    const int old_volume = global_status.volume;

    global_settings.night_mode = on;
    night_settings_off(by_schedule);
    if (on)
        night_settings_on();
    night_apply_settings(old_dark, old_volume);
}

#if CONFIG_RTC
static bool in_schedule(const struct tm *tm)
{
    const int now = tm->tm_hour * 60 + tm->tm_min;
    const int start = global_settings.night_start, end = global_settings.night_end;

    if (start <= end)
        return start <= now && now < end;
    /* over midnight */
    return now >= start || now < end;
}

/* The minute it is, from some time ago */
static int minutes_now(const struct tm *tm)
{
    struct tm copy = *tm;
    return (int)(mktime(&copy) / 60);
}
#endif

void night_mode_boot(void)
{
    struct user_settings *const s = &global_settings;

#if CONFIG_RTC
    /* over midnight or not, and whether it has gone by while it was off */
    struct tm *tm = get_time();
    if (s->night_schedule && valid_time(tm))
    {
        const bool in = in_schedule(tm);
        const int now = minutes_now(tm);
        if (in != s->night_was_in || now - s->night_seen >= LONG_OFF_MINUTES
            || now < s->night_seen)
            s->night_mode = in;
        s->night_was_in = in;
        s->night_seen = now;
    }
#endif
    /* the settings are applied after this, so only they are changed */
    if (s->night_mode && !s->night_changed)
        night_settings_on();
    else if (!s->night_mode && s->night_changed)
        night_settings_off(true);
    if (theme_has_dark())
        theme_set_names(s->dark_theme);
    settings_loaded = true;
}

/* Only on the screens that draw again on ACTION_REDRAW, and at their top level: the
   skins are read into the plugin buffer, which a plugin, the playlist viewer, a list
   of bookmarks and a list being written all use, and the UI's stack is small. A change
   of schedule can wait for them to be left. */
static bool theme_can_change_here(int context)
{
    const enum current_activity activity = get_current_activity();

    switch (context)
    {
        case CONTEXT_WPS:
            return activity == ACTIVITY_WPS && !status_get_ffmode();
        case CONTEXT_TREE:
            return activity == ACTIVITY_DATABASEBROWSER
                || activity == ACTIVITY_FILEBROWSER;
        case CONTEXT_MAINMENU:
            return activity == ACTIVITY_MAINMENU || activity == ACTIVITY_SETTINGS;
        default:
            return false;
    }
}

bool night_mode_poll(int context)
{
#if CONFIG_RTC
    static long next_poll;

    if (!global_settings.night_schedule || TIME_BEFORE(current_tick, next_poll))
        return false;
    if (!theme_can_change_here(context & ~ALLOW_SOFTLOCK))
        return false;
    next_poll = current_tick + POLL_TIME;

    struct tm *tm = get_time();
    if (!valid_time(tm))
        return false;
    const bool in = in_schedule(tm);
    /* not at every minute: it's a setting, and would make every save write the file */
    const int now = minutes_now(tm);
    if (now - global_settings.night_seen >= SEEN_STEP_MINUTES
        || now < global_settings.night_seen)
        global_settings.night_seen = now;
    if (in == global_settings.night_was_in)
        return false;
    global_settings.night_was_in = in;
    if (in == global_settings.night_mode)
    {
        settings_save();
        return false;
    }
    night_set(in, true);
    return true;
#else
    (void)context;
    return false;
#endif
}

void night_mode_settings_loading(void)
{
    settings_loaded = false;
}

void dark_theme_changed(bool dark)
{
    (void)dark;
    if (settings_loaded)
        theme_apply();
}

void night_mode_changed(bool on)
{
    if (settings_loaded)
        night_set(on, false);
}

void dark_theme_toggle(void)
{
    global_settings.dark_theme = !global_settings.dark_theme;
    theme_apply();
    settings_save();
}

void night_mode_toggle(void)
{
    night_set(!global_settings.night_mode, false);
}

/* What it does has changed while it's on: from what there was before it, again. A change
   made meanwhile to what it had already changed lasts until then, and no longer. */
void night_settings_chosen(void)
{
    if (global_settings.night_mode)
        night_set(true, false);
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    else
        backlight_set_brightness(global_settings.brightness); /* as it was tried out */
#endif
}

void night_action_changed(bool on)
{
    (void)on;
    if (settings_loaded)
        night_settings_chosen();
}

void night_brightness_changed(int value)
{
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    if (settings_loaded)
        backlight_set_brightness(value);
#else
    (void)value;
#endif
}

/* Turned on, or its times changed, it goes by the time it is now */
void night_schedule_changed(bool on)
{
#if CONFIG_RTC
    struct tm *tm = get_time();
    if (settings_loaded && on && valid_time(tm))
    {
        const bool in = in_schedule(tm);
        global_settings.night_was_in = in;
        global_settings.night_seen = minutes_now(tm);
        if (in != global_settings.night_mode)
            night_set(in, false);
    }
#else
    (void)on;
#endif
}

const char *night_time_format(char *buffer, size_t buffer_size, int value,
                              const char *unit)
{
    (void)unit;
    const int hour = value / 60, minute = value % 60;
    if (global_settings.timeformat)
        snprintf(buffer, buffer_size, "%d:%02d %s", hour % 12 ? hour % 12 : 12, minute,
                 hour < 12 ? "AM" : "PM");
    else
        snprintf(buffer, buffer_size, "%02d:%02d", hour, minute);
    return buffer;
}
