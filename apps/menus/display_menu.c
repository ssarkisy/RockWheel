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
#include "appevents.h"
#include "lang.h"
#include "action.h"
#include "settings.h"
#include "gui/ui_theme.h"
#include "menu.h"
#include "tree.h"
#include "list.h"
#include "peakmeter.h"
#include "talk.h"
#include "lcd.h"
#ifdef HAVE_REMOTE_LCD
#include "lcd-remote.h"
#endif
#ifdef HAVE_BACKLIGHT
#include "mask_select.h"
#include "splash.h"
#endif
#ifdef HAVE_TOUCHSCREEN
#include "screens.h"
#endif
#include "viewport.h"
#include "statusbar.h" /* statusbar_vals enum*/
#include "rbunicode.h"
#include "night_mode.h"
#include "string-extra.h"
#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
#include "skin_engine/cover_palette.h"
#include "skin_engine/menu_cover.h"
#endif

#ifdef HAVE_BACKLIGHT
static int selectivebacklight_callback(int action,
                                       const struct menu_item_ex *this_item,
                                       struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    switch (action)
    {
        case ACTION_EXIT_MENUITEM:
        case ACTION_STD_MENU:
        case ACTION_STD_CANCEL:
            set_selective_backlight_actions(
                                       global_settings.bl_selective_actions,
                                       global_settings.bl_selective_actions_mask,
                                       global_settings.bl_filter_first_keypress);
            break;
    }

    return action;
}

static int filterfirstkeypress_callback(int action,
                                        const struct menu_item_ex *this_item,
                                        struct gui_synclist *this_list)
{
    /*(void)this_item;REMOVED*/
    switch (action)
    {
        case ACTION_EXIT_MENUITEM:
            set_backlight_filter_keypress(global_settings.bl_filter_first_keypress);
#ifdef HAVE_REMOTE_LCD
            set_remote_backlight_filter_keypress(
                                global_settings.remote_bl_filter_first_keypress);
#endif /* HAVE_REMOTE_LCD */
            selectivebacklight_callback(action,this_item, this_list);/*uses Filter First KP*/
            break;
    }

    return action;
}

static int selectivebacklight_set_mask(void* param)
{
    (void)param;
     int mask = global_settings.bl_selective_actions_mask;
            struct s_mask_items maskitems[]={
                                       {ID2P(LANG_ACTION_VOLUME)   , SEL_ACTION_VOL},
                                       {ID2P(LANG_ACTION_PLAY), SEL_ACTION_PLAY},
                                       {ID2P(LANG_ACTION_SEEK), SEL_ACTION_SEEK},
                                       {ID2P(LANG_ACTION_SKIP), SEL_ACTION_SKIP},
                     {ID2P(LANG_ACTION_DISABLE_UNMAPPED), SEL_ACTION_NOUNMAPPED}
#if CONFIG_CHARGING
                        ,{ID2P(LANG_ACTION_DISABLE_EXT_POWER), SEL_ACTION_NOEXT}
#endif
                                            };

            mask = mask_select(mask, ID2P(LANG_BACKLIGHT_SELECTIVE)
                                   , maskitems, ARRAYLEN(maskitems));

            if (mask == SEL_ACTION_NONE ||  mask == SEL_ACTION_NOEXT)
                global_settings.bl_selective_actions = false;
            else if (global_settings.bl_selective_actions_mask != mask)
                global_settings.bl_selective_actions = true;

            global_settings.bl_selective_actions_mask = mask;

    return true;
}

#endif /* HAVE_BACKLIGHT */
#ifdef HAVE_LCD_FLIP
static int flipdisplay_callback(int action,
                                const struct menu_item_ex *this_item,
                                struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    switch (action)
    {
        case ACTION_EXIT_MENUITEM:
            button_set_flip(global_settings.flip_display);
            lcd_set_flip(global_settings.flip_display);
            lcd_update();
#ifdef HAVE_REMOTE_LCD
            lcd_remote_set_flip(global_settings.remote_flip_display);
            lcd_remote_update();
#endif
            break;
    }
    return action;
}
#endif

/***********************************/
/*    LCD MENU                     */
#ifdef HAVE_BACKLIGHT
MENUITEM_SETTING_SHARED(backlight_timeout, &global_settings.backlight_timeout, NULL);
MENUITEM_SETTING_SHARED(wake_screen_item, &global_settings.wake_screen, NULL);
MENUITEM_SETTING_SHARED(return_to_wps_item, &global_settings.return_to_wps, NULL);
#if CONFIG_CHARGING
MENUITEM_SETTING(backlight_timeout_plugged,
                &global_settings.backlight_timeout_plugged, NULL);
#endif

MENUITEM_SETTING(backlight_on_button_hold,
                &global_settings.backlight_on_button_hold, NULL);

MENUITEM_SETTING(caption_backlight, &global_settings.caption_backlight, NULL);
#if    defined(HAVE_BACKLIGHT_FADING_INT_SETTING) \
    || defined(HAVE_BACKLIGHT_FADING_BOOL_SETTING)
MENUITEM_SETTING(backlight_fade_in, &global_settings.backlight_fade_in, NULL);
MENUITEM_SETTING(backlight_fade_out, &global_settings.backlight_fade_out, NULL);
#endif
MENUITEM_SETTING(bl_filter_first_keypress,
                    &global_settings.bl_filter_first_keypress,
                    filterfirstkeypress_callback);

/* The backlight settings of Settings > Advanced, the timeout and
   brightness are in Settings itself */
MAKE_MENU(ipod_backlight_menu, ID2P(LANG_BACKLIGHT), 0, Icon_NOICON,
#if CONFIG_CHARGING
          &backlight_timeout_plugged,
#endif
          &backlight_on_button_hold, &bl_filter_first_keypress);

MENUITEM_SETTING(bl_selective_actions,
                 &global_settings.bl_selective_actions,
                                                    selectivebacklight_callback);

MENUITEM_FUNCTION(sel_backlight_mask, 0, ID2P(LANG_SETTINGS),
                  selectivebacklight_set_mask, selectivebacklight_callback,
                  Icon_Menu_setting);

MAKE_MENU(sel_backlight, ID2P(LANG_BACKLIGHT_SELECTIVE),
          NULL, Icon_Menu_setting, &bl_selective_actions, &sel_backlight_mask);

#ifdef HAVE_LCD_SLEEP_SETTING
MENUITEM_SETTING(lcd_sleep_after_backlight_off,
                &global_settings.lcd_sleep_after_backlight_off, NULL);
#endif
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
MENUITEM_SETTING_SHARED(brightness_item, &global_settings.brightness, NULL);
#endif
#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
/* a theme that takes nothing from the cover has neither row: the setting is
   as Off there */
static bool theme_has_cover_colours(void)
{
    return ui_theme_current()->cover_colours || ui_theme_current()->cover_tone;
}

static int cover_colours_callback(int action,
                                  const struct menu_item_ex *this_item,
                                  struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM && !theme_has_cover_colours())
        return ACTION_EXIT_MENUITEM;
    return action;
}
MENUITEM_SETTING_SHARED(cover_colours_item, &global_settings.cover_colours,
                        cover_colours_callback);
/* only the modes that colour the screen's images colour the icons; a theme
   that takes the cover's tone leaves its status glyphs as they are */
static int status_icons_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM
        && (global_settings.cover_colours < COVER_COLOURS_SCREEN
            || ui_theme_current()->cover_tone || !theme_has_cover_colours()))
        return ACTION_EXIT_MENUITEM;
    return action;
}
MENUITEM_SETTING_SHARED(status_icons_item, &global_settings.status_icons,
                        status_icons_callback);
/* not in a theme whose skin never turns the cover */
static int angled_cover_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM && ui_theme_current()->flat_cover)
        return ACTION_EXIT_MENUITEM;
    return action;
}
MENUITEM_SETTING_SHARED(angled_cover_item, &global_settings.angled_cover,
                        angled_cover_callback);
#endif
MENUITEM_SETTING(np_rating_item, &global_settings.np_rating, NULL);
MENUITEM_SETTING(np_format_item, &global_settings.np_format, NULL);

MENUITEM_SETTING(ui_theme_item, &global_settings.ui_theme, NULL);
MENUITEM_SETTING_SHARED(dark_theme_item, &global_settings.dark_theme, NULL);
#ifdef HAVE_TAGCACHE
/* the look of Music's lists of albums */
MENUITEM_SETTING(album_artists_item, &global_settings.album_artists, NULL);
#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
/* none with Album Art off, as there's no cover anywhere then */
static int list_covers_callback(int action,
                                const struct menu_item_ex *this_item,
                                struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM && global_settings.album_art == AA_OFF)
        return ACTION_EXIT_MENUITEM;
    return action;
}
MENUITEM_SETTING(list_covers_item, &global_settings.list_covers,
                 list_covers_callback);
#endif
#endif
MENUITEM_SETTING(list_scrollbar_item, &global_settings.list_scrollbar, NULL);
MENUITEM_SETTING(jump_by_letter_item, &global_settings.jump_by_letter, NULL);
/* Colour and Tinted Background: only in a theme that has a tone */
static int theme_colour_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM && !ui_theme_current()->cover_tone)
        return ACTION_EXIT_MENUITEM;
    return action;
}
MENUITEM_SETTING(theme_colour_item, &global_settings.theme_colour,
                 theme_colour_callback);
MENUITEM_SETTING(tinted_background_item, &global_settings.tinted_background,
                 theme_colour_callback);
#ifdef HAVE_MENU_COVER
/* none with Album Art off, as there's no cover beside the menus then */
static int moving_cover_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM && global_settings.album_art == AA_OFF)
        return ACTION_EXIT_MENUITEM;
    return action;
}
MENUITEM_SETTING(moving_cover_item, &global_settings.moving_cover,
                 moving_cover_callback);
#endif

/* Appearance, a folder of Settings: all that changes the look, the theme
   first, without a heading of its own, then a section each for the lists, the
   main menu and Now Playing. A section whose rows are all hidden, by the
   theme or with Album Art off, goes with them (init_menu_lists()) */
MENUITEM_GROUP(appearance_group_lists, ID2P(LANG_APPEARANCE_LISTS));
#ifdef HAVE_MENU_COVER
MENUITEM_GROUP(appearance_group_main_menu, ID2P(LANG_MAIN_MENU));
#endif
MENUITEM_GROUP(appearance_group_now_playing, ID2P(LANG_NOW_PLAYING));
MAKE_MENU(appearance_menu, ID2P(LANG_APPEARANCE), NULL, Icon_NOICON,
          &ui_theme_item, &dark_theme_item,
          &theme_colour_item, &tinted_background_item,
          &appearance_group_lists,
#ifdef HAVE_TAGCACHE
          &album_artists_item,
#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
          &list_covers_item,
#endif
#endif
          &list_scrollbar_item, &jump_by_letter_item,
#ifdef HAVE_MENU_COVER
          &appearance_group_main_menu,
          &moving_cover_item,
#endif
          &appearance_group_now_playing,
#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
          &cover_colours_item, &status_icons_item, &angled_cover_item,
#endif
          &np_rating_item, &np_format_item,
          );

/* after the screen of a value of the mode */
static int night_value_callback(int action, const struct menu_item_ex *this_item,
                                struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_EXIT_MENUITEM)
        night_settings_chosen();
    return action;
}

static int night_time_callback(int action, const struct menu_item_ex *this_item,
                               struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_EXIT_MENUITEM)
        night_schedule_changed(global_settings.night_schedule);
    return action;
}

MENUITEM_SETTING(night_mode_item, &global_settings.night_mode, NULL);
MENUITEM_GROUP(night_group_actions, ID2P(LANG_NIGHT_WHEN_ON));
MENUITEM_SETTING(night_dark_item, &global_settings.night_dark, NULL);
MENUITEM_SETTING(night_silent_item, &global_settings.night_silent, NULL);
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
MENUITEM_SETTING(night_dim_item, &global_settings.night_dim, NULL);
MENUITEM_SETTING(night_brightness_item, &global_settings.night_brightness,
                 night_value_callback);
#endif
MENUITEM_SETTING(night_limit_item, &global_settings.night_limit, NULL);
MENUITEM_SETTING(night_volume_item, &global_settings.night_volume, night_value_callback);
MENUITEM_GROUP(night_group_schedule, ID2P(LANG_NIGHT_SCHEDULE));
MENUITEM_SETTING(night_schedule_item, &global_settings.night_schedule, NULL);
MENUITEM_SETTING(night_start_item, &global_settings.night_start, night_time_callback);
MENUITEM_SETTING(night_end_item, &global_settings.night_end, night_time_callback);

MAKE_MENU(night_menu, ID2P(LANG_NIGHT_MODE), NULL, Icon_NOICON,
          &night_mode_item,
          &night_group_actions,
          &night_dark_item, &night_silent_item,
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
          &night_dim_item, &night_brightness_item,
#endif
          &night_limit_item, &night_volume_item,
          &night_group_schedule,
          &night_schedule_item, &night_start_item, &night_end_item);

/* its row in Settings, which says whether it's on, as the EQ's says its preset */
static int night_menu_open(void)
{
    /* Menu held goes on to the main menu, past the menu this one was opened
       from, and Play held to Now Playing */
    const int ret = do_menu(&night_menu, NULL, NULL, false);
    if (ret == GO_TO_ROOT)
        menu_request_root();
    return ret == GO_TO_PREVIOUS_MUSIC ? ret : 0;
}

static char *night_row_name(int selected_item, void *data,
                            char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    snprintf(buffer, buffer_len, "%s\t%s", str(LANG_NIGHT_MODE),
             str(global_settings.night_mode ? LANG_ON : LANG_OFF));
    return buffer;
}

MENUITEM_FUNCTION_DYNTEXT(night_row, MENU_FUNC_CHECK_RETVAL, night_menu_open,
                          night_row_name, NULL, NULL, NULL, Icon_NOICON);
#ifdef HAVE_COMPOSITE_VIDEO_OUT
MENUITEM_SETTING(composite_video_output,
                 &global_settings.composite_video_output, NULL);
#endif
#endif /* HAVE_BACKLIGHT */
#ifdef HAVE_LCD_CONTRAST
MENUITEM_SETTING(contrast, &global_settings.contrast, NULL);
#endif
#ifdef HAVE_LCD_INVERT
MENUITEM_SETTING(invert, &global_settings.invert, NULL);
#endif
#ifdef HAVE_LCD_FLIP
MENUITEM_SETTING(flip_display, &global_settings.flip_display, flipdisplay_callback);
#endif

/* now the actual menu */
MAKE_MENU(lcd_settings,ID2P(LANG_LCD_MENU),
            NULL, Icon_Display_menu
#ifdef HAVE_BACKLIGHT
            ,&backlight_timeout
# if CONFIG_CHARGING
            ,&backlight_timeout_plugged
# endif
            ,&backlight_on_button_hold
            ,&caption_backlight
#if    defined(HAVE_BACKLIGHT_FADING_INT_SETTING) \
    || defined(HAVE_BACKLIGHT_FADING_BOOL_SETTING)
            ,&backlight_fade_in, &backlight_fade_out
#endif
            ,&bl_filter_first_keypress
            ,&sel_backlight
# ifdef HAVE_LCD_SLEEP_SETTING
            ,&lcd_sleep_after_backlight_off
# endif
# ifdef HAVE_BACKLIGHT_BRIGHTNESS
            ,&brightness_item
# endif
#endif /* HAVE_BACKLIGHT */
#ifdef HAVE_COMPOSITE_VIDEO_OUT
            ,&composite_video_output
#endif
#ifdef HAVE_LCD_CONTRAST
            ,&contrast
#endif
# ifdef HAVE_LCD_INVERT
            ,&invert
# endif
# ifdef HAVE_LCD_FLIP
            ,&flip_display
# endif
         );
/*    LCD MENU                    */
/***********************************/


/********************************/
/* Remote LCD settings menu     */
#ifdef HAVE_REMOTE_LCD
MENUITEM_SETTING(remote_backlight_timeout,
                    &global_settings.remote_backlight_timeout, NULL);

#if CONFIG_CHARGING
MENUITEM_SETTING(remote_backlight_timeout_plugged,
                    &global_settings.remote_backlight_timeout_plugged, NULL);
#endif

#ifdef HAS_REMOTE_BUTTON_HOLD
MENUITEM_SETTING(remote_backlight_on_button_hold,
                    &global_settings.remote_backlight_on_button_hold, NULL);
#endif

MENUITEM_SETTING(remote_caption_backlight,
                    &global_settings.remote_caption_backlight, NULL);
MENUITEM_SETTING(remote_bl_filter_first_keypress,
                    &global_settings.remote_bl_filter_first_keypress,
                    filterfirstkeypress_callback);
MENUITEM_SETTING(remote_contrast,
                    &global_settings.remote_contrast, NULL);
MENUITEM_SETTING(remote_invert,
                    &global_settings.remote_invert, NULL);

#ifdef HAVE_LCD_FLIP
MENUITEM_SETTING(remote_flip_display,
                    &global_settings.remote_flip_display, flipdisplay_callback);
#endif

#ifdef HAVE_REMOTE_LCD_TICKING
static int ticking_callback(int action,
                            const struct menu_item_ex *this_item,
                            struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    switch (action)
    {
        case ACTION_EXIT_MENUITEM:
            lcd_remote_emireduce(global_settings.remote_reduce_ticking);
            break;
    }
    return action;
}
MENUITEM_SETTING(remote_reduce_ticking,
                    &global_settings.remote_reduce_ticking, ticking_callback);
#endif

MAKE_MENU(lcd_remote_settings, ID2P(LANG_LCD_REMOTE_MENU),
            NULL, Icon_Remote_Display_menu,
            &remote_backlight_timeout,
#if CONFIG_CHARGING
            &remote_backlight_timeout_plugged,
#endif
#ifdef HAS_REMOTE_BUTTON_HOLD
            &remote_backlight_on_button_hold,
#endif
            &remote_caption_backlight, &remote_bl_filter_first_keypress,
            &remote_contrast, &remote_invert

#ifdef HAVE_LCD_FLIP
            ,&remote_flip_display
#endif
#ifdef HAVE_REMOTE_LCD_TICKING
            ,&remote_reduce_ticking
#endif
         );

#endif /* HAVE_REMOTE_LCD */
/* Remote LCD settings menu     */
/********************************/

/***********************************/
/*    SCROLL MENU                  */
MENUITEM_SETTING_W_TEXT(scroll_speed, &global_settings.scroll_speed,
                         ID2P(LANG_SCROLL), NULL);
MENUITEM_SETTING(scroll_delay, &global_settings.scroll_delay, NULL);
MENUITEM_SETTING_W_TEXT(scroll_step, &global_settings.scroll_step,
                        ID2P(LANG_SCROLL_STEP_EXAMPLE), NULL);
MENUITEM_SETTING(bidir_limit, &global_settings.bidir_limit, NULL);
#ifdef HAVE_REMOTE_LCD
MENUITEM_SETTING_W_TEXT(remote_scroll_speed, &global_settings.remote_scroll_speed,
                         ID2P(LANG_SCROLL), NULL);
MENUITEM_SETTING(remote_scroll_delay, &global_settings.remote_scroll_delay, NULL);
MENUITEM_SETTING_W_TEXT(remote_scroll_step, &global_settings.remote_scroll_step,
                        ID2P(LANG_SCROLL_STEP_EXAMPLE), NULL);
MENUITEM_SETTING(remote_bidir_limit, &global_settings.remote_bidir_limit, NULL);

MAKE_MENU(remote_scroll_sets, ID2P(LANG_REMOTE_SCROLL_SETS), 0, Icon_NOICON,
          &remote_scroll_speed, &remote_scroll_delay,
          &remote_scroll_step, &remote_bidir_limit);
#endif /* HAVE_REMOTE_LCD */

/* list acceleration */
#ifndef HAVE_WHEEL_ACCELERATION
MENUITEM_SETTING(list_accel_start_delay,
                 &global_settings.list_accel_start_delay, NULL);
MENUITEM_SETTING(list_accel_wait, &global_settings.list_accel_wait, NULL);
#endif /* HAVE_WHEEL_ACCELERATION */
MENUITEM_SETTING(offset_out_of_view, &global_settings.offset_out_of_view, NULL);
MENUITEM_SETTING(disable_mainmenu_scrolling, &global_settings.disable_mainmenu_scrolling, NULL);
MENUITEM_SETTING(screen_scroll_step, &global_settings.screen_scroll_step, NULL);
MENUITEM_SETTING(scroll_paginated, &global_settings.scroll_paginated, NULL);
MENUITEM_SETTING(list_wraparound, &global_settings.list_wraparound, NULL);
MENUITEM_SETTING(list_order, &global_settings.list_order, NULL);

MAKE_MENU(scroll_settings_menu, ID2P(LANG_SCROLL_MENU), 0, Icon_NOICON,
          &scroll_speed, &scroll_delay,
          &scroll_step,
          &bidir_limit,
#ifdef HAVE_REMOTE_LCD
          &remote_scroll_sets,
#endif
          &offset_out_of_view,
          &disable_mainmenu_scrolling,
          &screen_scroll_step,
          &scroll_paginated,
          &list_wraparound,
          &list_order,
#ifndef HAVE_WHEEL_ACCELERATION
          &list_accel_start_delay, &list_accel_wait
#endif
          );
/*    SCROLL MENU                  */
/***********************************/

/***********************************/
/*    PEAK METER MENU              */

static int peakmeter_callback(int action,
                              const struct menu_item_ex *this_item,
                              struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    switch (action)
    {
        case ACTION_EXIT_MENUITEM:
            peak_meter_init_times(global_settings.peak_meter_release,
                                    global_settings.peak_meter_hold,
                                    global_settings.peak_meter_clip_hold);
            break;
    }
    return action;
}
MENUITEM_SETTING(peak_meter_hold,
                 &global_settings.peak_meter_hold, peakmeter_callback);
MENUITEM_SETTING(peak_meter_clip_hold,
                 &global_settings.peak_meter_clip_hold, peakmeter_callback);
#ifdef HAVE_RECORDING
MENUITEM_SETTING(peak_meter_clipcounter,
                 &global_settings.peak_meter_clipcounter, NULL);
#endif
MENUITEM_SETTING(peak_meter_release,
                 &global_settings.peak_meter_release, peakmeter_callback);
/**
 * Menu to select wether the scale of the meter
 * displays dBfs of linear values.
 */
static int peak_meter_scale(void) {
    bool retval = false;
    bool use_dbfs = global_settings.peak_meter_dbfs;
    retval = set_bool_options(str(LANG_PM_SCALE),
        &use_dbfs,
        STR(LANG_PM_DBFS), STR(LANG_PM_LINEAR),
        NULL);

    /* has the user really changed the scale? */
    if (use_dbfs != global_settings.peak_meter_dbfs) {

        /* store the change */
        global_settings.peak_meter_dbfs = use_dbfs;
        peak_meter_set_use_dbfs(use_dbfs);

        /* If the user changed the scale mode the meaning of
           peak_meter_min (peak_meter_max) has changed. Thus we have
           to convert the values stored in global_settings. */
        if (use_dbfs) {

            /* we only store -dBfs */
            global_settings.peak_meter_min = -peak_meter_get_min() / 100;
            global_settings.peak_meter_max = -peak_meter_get_max() / 100;

            /* limit the returned value to the allowed range */
            if(global_settings.peak_meter_min > 89)
                global_settings.peak_meter_min = 89;
        } else {
            int max;

            /* linear percent */
            global_settings.peak_meter_min = peak_meter_get_min();

            /* converting dBfs -> percent results in a precision loss.
               I assume that the user doesn't bother that conversion
               dBfs <-> percent isn't symmetrical for odd values but that
               he wants 0 dBfs == 100%. Thus I 'correct' the percent value
               resulting from dBfs -> percent manually here */
            max = peak_meter_get_max();
            global_settings.peak_meter_max = max < 99 ? max : 100;
        }
        settings_apply_pm_range();
    }
    return retval;
}

/**
 * Adjust the min value of the value range that
 * the peak meter shall visualize.
 */
static int peak_meter_min(void) {
    bool retval = false;
    if (global_settings.peak_meter_dbfs) {

        /* for dBfs scale */
        int range_max = -global_settings.peak_meter_max;
        int min = -global_settings.peak_meter_min;

        retval =  set_int(str(LANG_PM_MIN), str(LANG_PM_DBFS), UNIT_DB,
            &min, NULL, 1, -89, range_max, NULL);

        global_settings.peak_meter_min = - min;
    }

    /* for linear scale */
    else {
        int min = global_settings.peak_meter_min;

        retval =  set_int(str(LANG_PM_MIN), "%", UNIT_PERCENT,
            &min, NULL,
            1, 0, global_settings.peak_meter_max - 1, NULL);

        global_settings.peak_meter_min = (unsigned char)min;
    }

    settings_apply_pm_range();
    return retval;
}


/**
 * Adjust the max value of the value range that
 * the peak meter shall visualize.
 */
static int peak_meter_max(void) {
    bool retval = false;
    if (global_settings.peak_meter_dbfs) {

        /* for dBfs scale */
        int range_min = -global_settings.peak_meter_min;
        int max = -global_settings.peak_meter_max;

        retval =  set_int(str(LANG_PM_MAX), str(LANG_PM_DBFS), UNIT_DB,
            &max, NULL, 1, range_min, 0, NULL);

        global_settings.peak_meter_max = - max;

    }

    /* for linear scale */
    else {
        int max = global_settings.peak_meter_max;

        retval =  set_int(str(LANG_PM_MAX), "%", UNIT_PERCENT,
            &max, NULL,
            1, global_settings.peak_meter_min + 1, 100, NULL);

        global_settings.peak_meter_max = (unsigned char)max;
    }

    settings_apply_pm_range();
    return retval;
}

#if defined(HAVE_HISTOGRAM)
static bool history_interval(void)
{
    static const struct opt_items names[] = {
        { "0s", TALK_ID(0, UNIT_SEC) },
        { "1s", TALK_ID(1, UNIT_SEC) },
        { "2s", TALK_ID(2, UNIT_SEC) },
        { "4s", TALK_ID(4, UNIT_SEC) }
    };

    /* reconfigure histogram settings here */

    return set_option(str(LANG_HISTOGRAM_INTERVAL),
                          &global_settings.histogram_interval,
                          RB_INT, names, 4, NULL );
}

MENUITEM_FUNCTION(histogram, 0, ID2P(LANG_HISTOGRAM_INTERVAL),
                  history_interval, NULL, Icon_Menu_setting);

#endif

MENUITEM_FUNCTION(peak_meter_scale_item, 0, ID2P(LANG_PM_SCALE),
                  peak_meter_scale, NULL, Icon_NOICON);
MENUITEM_FUNCTION(peak_meter_min_item, 0, ID2P(LANG_PM_MIN),
                  peak_meter_min, NULL, Icon_NOICON);
MENUITEM_FUNCTION(peak_meter_max_item, 0, ID2P(LANG_PM_MAX),
                  peak_meter_max, NULL, Icon_NOICON);
MAKE_MENU(peak_meter_menu, ID2P(LANG_PM_MENU), NULL, Icon_NOICON,
          &peak_meter_release, &peak_meter_hold,
          &peak_meter_clip_hold,
#ifdef HAVE_RECORDING
          &peak_meter_clipcounter,
#endif
#ifdef HAVE_HISTOGRAM
          &histogram,
#endif
          &peak_meter_scale_item, &peak_meter_min_item, &peak_meter_max_item);
/*    PEAK METER MENU              */
/***********************************/


#ifdef HAVE_TOUCHSCREEN
static int touch_mode_callback(int action,
                               const struct menu_item_ex *this_item,
                               struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    switch (action)
    {
        case ACTION_EXIT_MENUITEM: /* on exit */
            touchscreen_set_mode(global_settings.touch_mode);
            break;
    }
    return action;
}

static int line_padding_callback(int action,
                                 const struct menu_item_ex *this_item,
                                 struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    if (action == ACTION_EXIT_MENUITEM)
        viewportmanager_theme_changed(THEME_LISTS);
    return action;
}

MENUITEM_SETTING(touch_mode, &global_settings.touch_mode, touch_mode_callback);
MENUITEM_SETTING(touch_enable_flick_shortcuts,
                 &global_settings.touch_enable_flick_shortcuts, NULL);

MENUITEM_FUNCTION(touchscreen_menu_calibrate, 0,
	              ID2P(LANG_TOUCHSCREEN_CALIBRATE), calibrate, NULL, Icon_NOICON);
MENUITEM_FUNCTION(touchscreen_menu_reset_calibration, 0,
	              ID2P(LANG_TOUCHSCREEN_RESET_CALIBRATION),
	              reset_mapping, NULL, Icon_NOICON);
MENUITEM_SETTING(list_line_padding, &global_settings.list_line_padding, line_padding_callback);

MAKE_MENU(touchscreen_menu, ID2P(LANG_TOUCHSCREEN_SETTINGS), NULL, Icon_NOICON,
          &touch_mode,
          &touch_enable_flick_shortcuts,
          &list_line_padding,
          &touchscreen_menu_calibrate,
          &touchscreen_menu_reset_calibration);
#endif

static int codepage_callback(int action,
                             const struct menu_item_ex *this_item,
                             struct gui_synclist *this_list)
{
    (void)this_item;
    (void)this_list;
    static int old_codepage;
    int new_codepage = global_settings.default_codepage;
    switch (action)
    {
        case ACTION_ENTER_MENUITEM:
            old_codepage = new_codepage;
            break;
        case ACTION_EXIT_MENUITEM:
            if (new_codepage != old_codepage)
                set_codepage(new_codepage);
            break;
    }
    return action;
}

MENUITEM_SETTING_SHARED(codepage_setting, &global_settings.default_codepage, codepage_callback);


MAKE_MENU(display_menu, ID2P(LANG_DISPLAY),
            NULL, Icon_Display_menu,
            &lcd_settings,
#ifdef HAVE_REMOTE_LCD
            &lcd_remote_settings,
#endif
            &scroll_settings_menu,
            &peak_meter_menu,
            &codepage_setting,
#ifdef HAVE_TOUCHSCREEN
            &touchscreen_menu,
#endif
            );
