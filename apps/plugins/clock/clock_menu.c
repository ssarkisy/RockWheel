/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2003 Zakk Roberts
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

#include "clock.h"
#include "clock_bitmaps.h"
#include "clock_settings.h"
#include "clock_menu.h"
#include "lib/playback_control.h"

/* Option structs (possible selections per each option) */
static const struct opt_items noyes_text[] = {
    { STR(LANG_SET_BOOL_NO) },
    { STR(LANG_SET_BOOL_YES) }
};

static const struct opt_items backlight_settings_text[] = {
    { STR(LANG_CLOCK_ALWAYS_OFF) },
    { STR(LANG_CLOCK_USE_SYSTEM_SETTING) },
    { STR(LANG_ALWAYS_ON) }
};

static const struct opt_items idle_poweroff_text[] = {
    { STR(LANG_OFF) },
    { STR(LANG_ON) }
};

static const struct opt_items date_format_text[] = {
    { STR(LANG_CLOCK_NO_DATE) },
    { STR(LANG_CLOCK_MONTH_DAY_YEAR) },
    { STR(LANG_CLOCK_DAY_MONTH_YEAR) },
    { STR(LANG_CLOCK_YEAR_MONTH_DAY) },
};

/***************
 * Select a mode, returs true when the mode has been selected
 * (we go back to clock display then)
 **************/
static bool menu_mode_selector(void){
    MENUITEM_STRINGLIST(menu, ID2P(LANG_GAME_MODE), NULL,
                        ID2P(LANG_CLOCK_ANALOG), ID2P(LANG_CLOCK_DIGITAL),
                        ID2P(LANG_CLOCK_BINARY));
    if(rb->do_menu(&menu, &clock_settings.mode, NULL, false) >=0)
        return(true);
    return(false);
}

/**********************
 * Analog settings menu
 *********************/
static void menu_analog_settings(void)
{
    int selection=0, result=0;

    MENUITEM_STRINGLIST(menu, ID2P(LANG_CLOCK_ANALOG), NULL,
                        ID2P(LANG_CLOCK_SHOW_DATE),
                        ID2P(LANG_CLOCK_SHOW_SECOND_HAND),
                        ID2P(LANG_CLOCK_SHOW_HOUR_MARKS));

    while(result>=0){
        result=rb->do_menu(&menu, &selection, NULL, false);
        switch(result){
            case 0:
                rb->set_option(rb->str(LANG_CLOCK_SHOW_DATE),
                               &clock_settings.analog.show_date,
                               RB_BOOL, noyes_text, 2, NULL);
                break;
            case 1:
                rb->set_option(rb->str(LANG_CLOCK_SHOW_SECOND_HAND),
                               &clock_settings.analog.show_seconds,
                               RB_BOOL, noyes_text, 2, NULL);
                break;
            case 2:
                rb->set_option(rb->str(LANG_CLOCK_SHOW_HOUR_MARKS),
                               &clock_settings.analog.show_border,
                               RB_BOOL, noyes_text, 2, NULL);
                break;
        }
    }
}

/***********************
 * Digital settings menu
 **********************/
static void menu_digital_settings(void){
    int selection=0, result=0;

    MENUITEM_STRINGLIST(menu, ID2P(LANG_CLOCK_DIGITAL), NULL,
                        ID2P(LANG_CLOCK_SHOW_SECONDS),
                        ID2P(LANG_CLOCK_BLINKING_COLON));

    while(result>=0){
        result=rb->do_menu(&menu, &selection, NULL, false);
        switch(result){
            case 0:
                rb->set_option(rb->str(LANG_CLOCK_SHOW_SECONDS),
                               &clock_settings.digital.show_seconds,
                               RB_BOOL, noyes_text, 2, NULL);
                break;
            case 1:
                rb->set_option(rb->str(LANG_CLOCK_BLINKING_COLON),
                               &clock_settings.digital.blinkcolon,
                               RB_BOOL, noyes_text, 2, NULL);
                break;
        }
    }
}

/************************************
 * General settings. Reset, save, etc
 ***********************************/
static int menu_general_settings(void){
    int selection=0, result=0;

    /* Under the name of the row that opens it, Settings */
    MENUITEM_STRINGLIST(menu, ID2P(LANG_SETTINGS), NULL,
                        ID2P(LANG_CLOCK_ANALOG), ID2P(LANG_CLOCK_DIGITAL),
                        ID2P(LANG_CLOCK_DATE_FORMAT), ID2P(LANG_BACKLIGHT),
                        ID2P(LANG_POWEROFF_IDLE),
                        ID2P(LANG_CLOCK_SAVE_ON_EXIT), ID2P(LANG_SAVE),
                        ID2P(LANG_RESET));

    while(result>=0){
        result=rb->do_menu(&menu, &selection, NULL, false);
        switch(result){
            case 0:
                menu_analog_settings();
                break;
            case 1:
                menu_digital_settings();
                break;
            case 2:
                rb->set_option(rb->str(LANG_CLOCK_DATE_FORMAT),
                               &clock_settings.general.date_format,
                               RB_INT, date_format_text, 4, NULL);
                break;
            case 3:
                rb->set_option(rb->str(LANG_BACKLIGHT),
                               &clock_settings.general.backlight,
                               RB_INT, backlight_settings_text, 3, NULL);
                apply_backlight_setting(clock_settings.general.backlight);
                break;

            case 4:
                rb->set_option(rb->str(LANG_POWEROFF_IDLE),
                               &clock_settings.general.idle_poweroff,
                               RB_BOOL, idle_poweroff_text, 2, NULL);
                break;
            case 5:
                rb->set_option(rb->str(LANG_CLOCK_SAVE_ON_EXIT),
                               &clock_settings.general.save_settings,
                               RB_BOOL, noyes_text, 2, NULL);

                /* if we no longer save on exit,
                   we better save now to remember that */
                if(!clock_settings.general.save_settings)
                    save_settings_wo_gui();
                break;
            case 6:
                save_settings_wo_gui();
                rb->splash(HZ, ID2P(LANG_SETTINGS_SAVED));
                break;
            case 7:
                if (rb->yesno_pop_confirm(ID2P(LANG_RESET)))
                {
                    clock_settings_reset(&clock_settings);
                    return 1;
                }
                break;
        }
    }
    return 0;
}

/***********
 * Main menu
 **********/
bool main_menu(void)
{
    int selection = 0;
    bool done = false;
    bool exit_clock=false;

    MENUITEM_STRINGLIST(menu, ID2P(LANG_CLOCK), NULL, ID2P(LANG_GAME_MODE),
                        ID2P(LANG_SETTINGS), ID2P(LANG_PLAYBACK_CONTROL),
                        ID2P(LANG_MENU_QUIT));
    while (!done)
    {
        switch(rb->do_menu(&menu, &selection, NULL, false))
        {
            case 0:
                done=menu_mode_selector();
                break;
            case 1:
                if (menu_general_settings())
                    done = true;
                break;
            case 2:
                playback_control(NULL);
                break;
            case 3:
                exit_clock = true;
                done = true;
                break;
            default:
                done=true;
                break;
        }
    }
    return(exit_clock);
}
