/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2007 Jonathan Gordon
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
#include <stddef.h>
#include <limits.h>
#include "config.h"
#include "string.h"
#include "lang.h"
#include "action.h"
#include "settings.h"
#include "power.h"
#include "powermgmt.h"
#include "menu.h"
#include "misc.h"
#include "exported_menus.h"
#include "tree.h"
#include "storage.h"
#ifdef HAVE_RECORDING
#include "recording.h"
#endif
#include "yesno.h"
#include "keyboard.h"
#include "screens.h"
#include "plugin.h"
#include "talk.h"
#include "splash.h"
#include "debug_menu.h"
#include "version.h"
#include "time.h"
#include "wps.h"
#include "skin_buffer.h"
#include "disk.h"
#include "tagcache.h"
#include "itunesdb.h"
#include "core_alloc.h"
#include "file.h"

static const struct browse_folder_info config = {ROCKBOX_DIR, SHOW_CFG};
static int show_info(void);
/***********************************/
/*    MANAGE SETTINGS MENU        */

static int reset_settings(void)
{
    static const char *lines[]={ID2P(LANG_ARE_YOU_SURE), ID2P(LANG_RESET)};
    static const char *yes_lines[]={
        ID2P(LANG_SETTINGS),
        ID2P(LANG_RESET_DONE_CLEAR)
    };
    static const char *no_lines[]={
        ID2P(LANG_SETTINGS),
        ID2P(LANG_CANCEL)
    };
    static const struct text_message message={lines, 2};
    static const struct text_message yes_message={yes_lines, 2};
    static const struct text_message no_message={no_lines, 2};

    switch(gui_syncyesno_run(&message, &yes_message, &no_message))
    {
        case YESNO_YES:
            settings_reset();
            settings_save();
            settings_apply(true);
            settings_apply_skins();
            break;
        case YESNO_TMO:
        case YESNO_NO:
            break;
        case YESNO_USB:
            return 1;
    }
    return 0;
}
static int write_settings_file(void* param)
{
    return settings_save_config((intptr_t)param);
}

MENUITEM_FUNCTION_W_PARAM(browse_configs, 0, ID2P(LANG_CUSTOM_CFG),
                          browse_folder, (void*)&config, NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(save_settings_item, 0, ID2P(LANG_SAVE_SETTINGS),
                          write_settings_file, (void*)SETTINGS_SAVE_ALL,
                          NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(save_theme_item, 0, ID2P(LANG_SAVE_THEME),
                          write_settings_file, (void*)SETTINGS_SAVE_THEME,
                          NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(save_sound_item, 0, ID2P(LANG_SAVE_SOUND),
                          write_settings_file, (void*)SETTINGS_SAVE_SOUND,
                          NULL, Icon_NOICON);
MENUITEM_FUNCTION(reset_settings_item, 0, ID2P(LANG_RESET),
                  reset_settings, NULL, Icon_NOICON);

MAKE_MENU(manage_settings, ID2P(LANG_MANAGE_MENU), NULL, Icon_Config,
          &browse_configs, &reset_settings_item,
          &save_settings_item, &save_sound_item, &save_theme_item);
/*    MANAGE SETTINGS MENU        */
/**********************************/

/***********************************/
/*      INFO MENU                  */

static int show_credits(void)
{
    if (plugin_load(VIEWERS_DIR "/credits.rock", NULL) != PLUGIN_OK)
        show_info();
    return 0;
}

/* The notice first, then the licences as the package has them */
static int show_legal(void)
{
    static const char notice[] =
        "RockWheel is built on Rockbox, free software under the GNU General Public "
        "License, version 2 or later, by the Rockbox contributors.\n\n"
        "Theme: modified/derived from iClassic Square theme for iPod Video "
        "(CC-BY-SA 3.0) by Humberto Santana, with elements from the iLike theme by "
        "Alan Brito and H. Pearce.\n\n"
        "Font: Inter, under the SIL Open Font License.\n\n"
        "Apple, iPod, iTunes and Finder are trademarks of Apple Inc. RockWheel has "
        "nothing to do with Apple.\n\n";
    static const char * const files[] =
        { ROCKBOX_DIR "/docs/COPYING.txt", ROCKBOX_DIR "/docs/LICENSES.txt" };

    size_t size = sizeof(notice);
    for (size_t i = 0; i < ARRAYLEN(files); i++)
    {
        const int fd = open(files[i], O_RDONLY);
        if (fd >= 0)
        {
            size += lseek(fd, 0, SEEK_END) + 2;
            close(fd);
        }
    }

    const int handle = core_alloc(size);
    if (handle <= 0)
    {
        splash(HZ, ID2P(LANG_FAILED));
        return 0;
    }
    core_pin(handle);
    char *text = core_get_data(handle);
    size_t length = strlcpy(text, notice, size);
    for (size_t i = 0; i < ARRAYLEN(files); i++)
    {
        const int fd = open(files[i], O_RDONLY);
        if (fd < 0)
            continue;
        const ssize_t got = read(fd, text + length, size - length - 3);
        close(fd);
        if (got > 0)
            length += got;
        text[length++] = '\n';
        text[length++] = '\n';
    }
    text[length] = '\0';

    const char *title_and_text[2] = { str(LANG_LEGAL_NOTICES), text };
    plugin_load(VIEWERS_DIR "/view_text.rock", title_and_text);
    core_unpin(handle);
    core_free(handle);
    return 0;
}

#define SIZE_FMT "%s %s"
struct info_data
{
    sector_t size[NUM_VOLUMES];
    sector_t free[NUM_VOLUMES];
    unsigned long name[NUM_VOLUMES];
    bool new_data;
};

enum infoscreenorder
{
    INFO_BATTERY = 0,
    INFO_BUFFER,
#ifdef HAVE_RECORDING
    INFO_REC_DIR_TITLE,
    INFO_REC_DIR,
#endif
    INFO_ROOT_DIR_TITLE,
    INFO_ROOT_DIR,
    INFO_VERSION_TITLE,
    INFO_VERSION,
#if CONFIG_RTC
    INFO_DATE,
    INFO_TIME,
#endif
    INFO_DISK1, /* capacity/free on internal */
    INFO_COUNT
};

/* NOTE:  This code is not quite right for
   (HAVE_MULTIVOLUME && !HAVE_MULTIDRIVE), but there are no
   in-tree matches so.. eh.
*/
static int refresh_data(struct info_data *info)
{
    int i = 0;
#ifdef HAVE_MULTIVOLUME
#ifdef HAVE_MULTIDRIVE
    int max = -1;
    int special = 0;
#endif
    int drive = 0;
    for (i = 0 ; i < NUM_VOLUMES ; i++)
#endif
    {
	volume_size(IF_MV(i,) &info->size[i], &info->free[i]);
#ifdef HAVE_MULTIVOLUME
#ifdef HAVE_MULTIDRIVE
	drive = volume_drive(i);
#endif
	if (drive > 0)
	    info->name[i] = LANG_DISK_NAME_MMC;
	else
#endif
	    info->name[i] = LANG_DISK_NAME_INTERNAL;
#ifdef HAVE_MULTIDRIVE
	if (drive > max) {
	    max = drive;
        } else if (drive < max) {
            if (max == 0) {
                /* Special case, make sure we display a single entry for
                   secondary drive too */
                special = 1;
            }
#endif
#if defined(HAVE_MULTIVOLUME) && (defined(HAVE_HOTSWAP) || defined(HAVE_DIRCACHE) || defined(HAVE_BOOTDATA)) // Is this #ifdef necessary?
            if (volume_partition(i) == -1)
#endif
	    {
#ifdef HAVE_MULTIDRIVE
                if (special)
                    info->name[i] = LANG_DISK_NAME_MMC;
                else
#endif
                    info->name[i] = 0;
#ifdef HAVE_MULTIVOLUME
                break; /* ie stop when we run out of valid partitions */
#endif
            }
#ifdef HAVE_MULTIDRIVE
        }
#endif
    }

    info->new_data = false;
    return i;
}

static int info_speak_item(int selected_item, void * data)
{
    struct info_data *info = (struct info_data*)data;

#if CONFIG_RTC
    struct tm *tm;
#endif
    int i;

    if (info->new_data)
        refresh_data(info);

    switch (selected_item)
    {
        case INFO_VERSION: /* version */
            talk_id(LANG_VERSION, false);
            talk_spell(rbversion, true);
        case INFO_VERSION_TITLE:  /*fallthrough*/
            break;

#if CONFIG_RTC
        case INFO_TIME:
            talk_id(LANG_CURRENT_TIME, false);
            /* fallthrough */
        case INFO_DATE:
            tm = get_time();

            if (!valid_time(tm))
            {
                talk_id(LANG_UNKNOWN, true);
                break;
            }
            if (selected_item == INFO_DATE)
                talk_date(tm, true);
            else
                talk_time(tm, true);

            break;
#endif

#ifdef HAVE_RECORDING
        case INFO_REC_DIR:
            talk_id(LANG_REC_DIR, false);
            if (global_settings.rec_directory[0])
            {
                talk_fullpath(global_settings.rec_directory, true);
            }
        case INFO_REC_DIR_TITLE:  /*fallthrough*/
            break;
#endif
        case INFO_ROOT_DIR:
            talk_id(LANG_DISPLAY_FULL_PATH, false);
            talk_fullpath(root_realpath(), true);
        case INFO_ROOT_DIR_TITLE: /*fallthrough*/
            break;
        case INFO_BUFFER: /* buffer */
        {
            talk_id(LANG_BUFFER_STAT, false);
            long kib = audio_buffer_size() >> 10; /* to KiB */
            output_dyn_value(NULL, 0, kib, kibyte_units, 3, true);
            break;
        }
        case INFO_BATTERY: /* battery */
#if CONFIG_CHARGING == CHARGING_SIMPLE
            /* Only know if plugged */
            if (charger_inserted())
            {
                talk_id(LANG_BATTERY_CHARGE, true);
                if (battery_level() >= 0)
                    talk_value(battery_level(), UNIT_PERCENT, true);
            }
            else
#elif CONFIG_CHARGING >= CHARGING_MONITOR
            /* Go by what power management reports */
            if (charging_state())
            {
                talk_id(LANG_BATTERY_CHARGE, true);
                if (battery_level() >= 0)
                    talk_value(battery_level(), UNIT_PERCENT, true);
            }
            else
#endif /* CONFIG_CHARGING = */
            if (battery_level() >= 0)
            {
                int time_left = battery_time();
                talk_id(LANG_BATTERY_TIME, false);
                talk_value(battery_level(), UNIT_PERCENT, true);
                if (time_left >= 0)
                    talk_value(time_left * 60, UNIT_TIME, true);
            }
            else talk_id(VOICE_BLANK, false);
            break;
        case INFO_DISK1: /* disk 1 */
        default:
            i = selected_item - INFO_DISK1;
            if (info->size[i]) {
                talk_ids(false, info->name[i], LANG_DISK_FREE_INFO);
                output_dyn_value(NULL, 0, info->free[i], kibyte_units, 3, true);
                talk_id(LANG_DISK_SIZE_INFO, true);
                output_dyn_value(NULL, 0, info->size[i], kibyte_units, 3, true);
#ifdef HAVE_MULTIVOLUME
            } else if (info->name[i]) {
                talk_ids(true, info->name[i], LANG_NOT_PRESENT);
#endif
            }
            break;
    }
    return 0;
}

static int info_action_callback(int action, struct gui_synclist *lists)
{
    struct info_data *info = (struct info_data *)lists->data;
    if (info->new_data)
        refresh_data(info);

    char s1[32];
    char s2[32];
#if CONFIG_RTC
    struct tm *tm;
#endif
    if (action == ACTION_STD_CANCEL)
    {
        return action;
    }
    else if (action == ACTION_STD_OK
#ifdef HAVE_HOTSWAP
        || action == SYS_FS_CHANGED
#endif
        )
    {
        action = ACTION_REDRAW;
        info->new_data = true;
        splash(0, ID2P(LANG_SCANNING_DISK));
#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
        for (int i = 0; i < NUM_VOLUMES; i++)
            volume_recalc_free(IF_MV(i));
#endif
        gui_synclist_speak_item(lists);
    }
#if CONFIG_RTC
    else if (action == ACTION_NONE)
    {
        static int last_redraw = 0;
        if (TIME_AFTER(current_tick, last_redraw + HZ*5))
        {
            last_redraw = current_tick;
        }
        else
            return action;
    }
#endif
/* Note: these need to be in the same order as enum infoscreenorder */
    simplelist_reset_lines();
/* INFO_BATTERY */
#if CONFIG_CHARGING == CHARGING_SIMPLE
    /* Only know if plugged */
    if (charger_inserted())
        simplelist_setline(str(LANG_BATTERY_CHARGE));
    else
#elif CONFIG_CHARGING >= CHARGING_MONITOR
    /* Go by what power management reports */
    if (charging_state())
        simplelist_setline(str(LANG_BATTERY_CHARGE));
    else
#endif /* CONFIG_CHARGING = */
    if (battery_level() >= 0)
        simplelist_addline(str(LANG_BATTERY_TIME),
        battery_level(), battery_time() / 60, battery_time() % 60);
    else
        simplelist_setline("Battery n/a"); /* translating worth it? */

/* INFO_BUFFER */
    long kib = audio_buffer_size() >> 10; /* to KiB */
    output_dyn_value(s1, sizeof(s1), kib, kibyte_units, 3, true);
    simplelist_addline("%s %s", str(LANG_BUFFER_STAT), s1);

#ifdef HAVE_RECORDING
/* INFO_REC_DIR_TITLE*/
    simplelist_setline(str(LANG_REC_DIR));
/* INFO_REC_DIR */
    simplelist_setline(global_settings.rec_directory);
#endif

/* INFO_ROOT_DIR_TITLE */
    simplelist_setline(str(LANG_DISPLAY_FULL_PATH));
/* INFO_ROOT_DIR */
    simplelist_setline(root_realpath());
/* INFO_VERSION_TITLE */
    simplelist_setline(str(LANG_VERSION));
/* INFO_VERSION */
    simplelist_setline(rbversion);
#if CONFIG_RTC

    tm = get_time();
    if (valid_time(tm))
    {
/* INFO_DATE */
        simplelist_addline("%s %d %d",
            str(LANG_MONTH_JANUARY + tm->tm_mon),
            tm->tm_mday,
            tm->tm_year+1900);
/* INFO_TIME */
        simplelist_addline("%02d:%02d:%02d %s",
            global_settings.timeformat == 0 ? tm->tm_hour :
                 ((tm->tm_hour + 11) % 12) + 1,
                 tm->tm_min,
                 tm->tm_sec,
                 global_settings.timeformat == 0 ? "" :
                 tm->tm_hour>11 ? "P" : "A");
    }
    else
    {
/* INFO_DATE */
        simplelist_setline(str(LANG_UNKNOWN));
/* INFO_TIME */
        simplelist_setline("--:--:--");
    }
#endif
/* INFO_DISK, capacity/free on internal */
    for (int i = 0; i < NUM_VOLUMES ; i++) {
        if (info->size[i]) {
            output_dyn_value(s1, sizeof s1, info->free[i], kibyte_units, 3, true);
            output_dyn_value(s2, sizeof s2, info->size[i], kibyte_units, 3, true);
            simplelist_addline("%s %s/%s", str(info->name[i]), s1, s2);
#ifdef HAVE_MULTIVOLUME
        } else if (info->name[i]) {
            simplelist_addline("%s %s", str(info->name[i]),
                str(LANG_NOT_PRESENT));
#endif
        }
    }

    return action;
}

static int show_info(void)
{
    struct info_data data = {.new_data = true };
    struct simplelist_info info;
    refresh_data(&data);
    simplelist_info_init(&info, str(LANG_ROCKBOX_INFO), 0, (void*)&data);
    simplelist_setline(str(LANG_WAIT)); /* set a line so list is not empty on load */
    if(global_settings.talk_menu)
         info.get_talk = info_speak_item;
    info.action_callback = info_action_callback;
    return (simplelist_show_list(&info)) ? 1 : 0;
}


/* What this iPod holds and is, one line for each, its name and then its value, as on
   a stock iPod. The Rockbox this is built on, from git describe of its upstream. */
#define ABOUT_ROCKBOX   "4.0+ f3ffe789ef"
#define ABOUT_THEME     "iClassic Square"
#define ABOUT_LINES     14
#define ABOUT_LINE_SIZE 64
static char about_lines[ABOUT_LINES][ABOUT_LINE_SIZE];
static int about_count;

static void about_add(const char *name, const char *value)
{
    if (about_count < ABOUT_LINES)
        snprintf(about_lines[about_count++], ABOUT_LINE_SIZE, "%s\t%s", name, value);
}

static void about_add_number(int lang_id, int number)
{
    char value[16];
    if (number < 0)
        return;
    snprintf(value, sizeof(value), "%d", number);
    about_add(str(lang_id), value);
}

/* In units of 1000, as the Mac shows the same disk */
static void about_add_size(int lang_id, sector_t kib)
{
    char value[16];
    const uint64_t bytes = (uint64_t)kib * 1024;
    if (bytes >= 1000000000)
        snprintf(value, sizeof(value), str(LANG_ABOUT_GB), (int)(bytes / 1000000000));
    else
        snprintf(value, sizeof(value), str(LANG_ABOUT_MB), (int)(bytes / 1000000));
    about_add(str(lang_id), value);
}

static uint32_t about_le32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Reads no more of Photo Database than the count in the header of its list of images */
static int about_photo_count(void)
{
    unsigned char h[24];
    int count = 0;
    const int fd = open("/Photos/Photo Database", O_RDONLY);
    if (fd < 0)
        return 0;
    if (read(fd, h, sizeof(h)) == sizeof(h) && !memcmp(h, "mhfd", 4))
    {
        off_t pos = about_le32(h + 4);
        const int sections = about_le32(h + 20);
        for (int i = 0; i < sections; i++)
        {
            if (lseek(fd, pos, SEEK_SET) != pos || read(fd, h, 14) != 14
                || memcmp(h, "mhsd", 4))
                break;
            const uint32_t header = about_le32(h + 4), total = about_le32(h + 8);
            if (h[12] == 1)
            {
                if (lseek(fd, pos + header, SEEK_SET) == pos + (off_t)header
                    && read(fd, h, 12) == 12 && !memcmp(h, "mhli", 4))
                    count = about_le32(h + 8);
                break;
            }
            if (total == 0)
                break;
            pos += total;
        }
    }
    close(fd);
    return count;
}

static void about_prepare(void)
{
    char value[32];
    about_count = 0;

    const char *device = itunesdb_device_name();
    if (device && device[0])
        strlcpy(about_lines[about_count++], device, ABOUT_LINE_SIZE);

    about_add_number(LANG_SONGS, tagcache_get_song_count());
    about_add_number(LANG_ALBUMS, tagcache_get_tag_count(tag_album));
    about_add_number(LANG_ARTISTS, tagcache_get_tag_count(tag_artist));
    about_add_number(LANG_PHOTOS, about_photo_count());

    sector_t size = 0, free = 0;
    volume_size(IF_MV(0,) &size, &free);
    about_add_size(LANG_ABOUT_CAPACITY, size);
    about_add_size(LANG_ABOUT_USED, size - free);
    about_add_size(LANG_ABOUT_AVAILABLE, free);

    if (battery_level() >= 0)
    {
        snprintf(value, sizeof(value), "%d %%", battery_level());
        about_add(str(LANG_BATTERY_MENU), value);
    }
    if (!charger_inserted())
    {
        update_runtime();
        snprintf(value, sizeof(value), "%d:%02d", global_status.runtime / 3600,
                 global_status.runtime / 60 % 60);
        about_add(str(LANG_ABOUT_ON_BATTERY), value);
    }

    struct tm tm;
    if (itunesdb_sync_date(&tm))
    {
        snprintf(value, sizeof(value), "%02d.%02d.%04d", tm.tm_mday, tm.tm_mon + 1,
                 tm.tm_year + 1900);
        about_add(str(LANG_ABOUT_LAST_SYNC), value);
    }

    about_add("RockWheel", rbversion);
    about_add("Rockbox", ABOUT_ROCKBOX);
    about_add(str(LANG_ABOUT_THEME), ABOUT_THEME);
}

static const char *about_get_name(int item, void *data, char *buffer, size_t size)
{
    (void)data; (void)buffer; (void)size;
    return about_lines[item];
}

static int about_action(int action, struct gui_synclist *lists)
{
    lists->callback_draw_item = menu_draw_value_line;
    return action == ACTION_STD_OK ? ACTION_NONE : action;
}

static int show_about(void)
{
    struct simplelist_info info;
    about_prepare();
    simplelist_info_init(&info, str(LANG_ABOUT), about_count, NULL);
    info.get_name = about_get_name;
    info.action_callback = about_action;
    return simplelist_show_list(&info) ? 1 : 0;
}

MENUITEM_FUNCTION(show_info_item, 0, ID2P(LANG_ABOUT),
                  show_about, NULL, Icon_NOICON);

#if CONFIG_RTC
int time_screen(void* ignored);
MENUITEM_FUNCTION(timedate_item, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_TIME_MENU),
                  time_screen,  NULL, Icon_Menu_setting );
#endif

MENUITEM_FUNCTION(show_credits_item, 0, ID2P(LANG_CREDITS),
                  show_credits, NULL, Icon_NOICON);

MENUITEM_FUNCTION(show_runtime_item, 0, ID2P(LANG_RUNNING_TIME),
                  view_runtime, NULL, Icon_NOICON);

MENUITEM_FUNCTION(debug_menu_item, 0, ID2P(LANG_DEBUG),
                  debug_menu, NULL, Icon_NOICON);

MENUITEM_FUNCTION(show_legal_item, 0, ID2P(LANG_LEGAL_NOTICES),
                  show_legal, NULL, Icon_NOICON);

MAKE_MENU(info_menu, ID2P(LANG_SYSTEM), 0, Icon_System_menu,
          &show_info_item, &show_credits_item,
          &show_runtime_item, &show_legal_item, &debug_menu_item);
/*      INFO MENU                  */
/***********************************/

static int main_menu_config(void)
{
    plugin_load(PLUGIN_APPS_DIR "/main_menu_config.rock", NULL);
    return 0;
}

MENUITEM_FUNCTION(main_menu_config_item, 0, ID2P(LANG_MAIN_MENU),
                  main_menu_config, NULL, Icon_Rockbox);

/***********************************/
/*    MAIN MENU                    */

/* All the settings there are, by what they are about. Rockbox's full settings
   menus are not shown. */
MENUITEM_GROUP(group_playback, ID2P(LANG_PLAYBACK));
MENUITEM_GROUP(group_sound, ID2P(LANG_SOUND_SETTINGS));
#if defined(HAVE_BACKLIGHT) || defined(HAVE_BACKLIGHT_BRIGHTNESS)
MENUITEM_GROUP(group_display, ID2P(LANG_DISPLAY));
#endif
MENUITEM_GROUP(group_general, ID2P(LANG_GENERAL_SETTINGS));

MAKE_MENU(main_menu_, ID2P(LANG_SETTINGS), NULL,
        Icon_Submenu_Entered,
        &show_info_item,
        &group_playback,
        &shuffle_item, &repeat_mode,
#ifdef HAVE_CROSSFADE
        &crossfade,
#endif
#ifdef HAVE_PITCHCONTROL
        &audiobook_speed,
#endif
        &resume_rewind,
#ifdef HAVE_HEADPHONE_DETECTION
        &unplug_mode,
#endif
        &cuesheet,
        &group_sound,
        &ipod_eq_row, &replaygain_type, &volume_limit, &balance, &crossfeed,
        &afr_enabled,
#if defined(HAVE_BACKLIGHT) || defined(HAVE_BACKLIGHT_BRIGHTNESS)
        &group_display,
#endif
#ifdef HAVE_BACKLIGHT
        &backlight_timeout,
#endif
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
        &brightness_item,
#endif
        &group_general,
        &ipod_keyclick_menu, &sleep_timer_item,
#if CONFIG_RTC
        &timedate_item,
#endif
        &language_item,
        &show_legal_item, &reset_settings_item,
        );
/*    MAIN MENU                    */
/***********************************/
