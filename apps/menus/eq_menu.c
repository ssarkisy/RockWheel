/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006 Dan Everton
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
#include <stdbool.h>
#include <string.h>
#include "eq_menu.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "list.h"
#include "menu.h"
#include "action.h"
#include "settings.h"
#include "rbpaths.h"
#include "screens.h"
#include "icons.h"
#include "font.h"
#include "lang.h"
#include "talk.h"
#include "misc.h"
#include "sound.h"
#include "dsp_proc_settings.h"
#include "tree.h"
#include "screen_access.h"
#include "keyboard.h"
#include "gui/scrollbar.h"
#include "menu_common.h"
#include "viewport.h"
#include "exported_menus.h"
#include "pcmbuf.h"
#include "option_select.h"
#include "string-extra.h"

static void eq_apply(void);

/*
 * Utility functions
 */

const char* eq_q_format(char* buffer, size_t buffer_size, int value, const char* unit)
{
    snprintf(buffer, buffer_size, "%d.%d %s", value / EQ_USER_DIVISOR,
         value % EQ_USER_DIVISOR, unit);
    return buffer;
}

const char* eq_precut_format(char* buffer, size_t buffer_size, int value, const char* unit)
{
    snprintf(buffer, buffer_size, "%s%d.%d %s", value == 0 ? " " : "-",
        value / EQ_USER_DIVISOR, value % EQ_USER_DIVISOR, unit);
    return buffer;
}

void eq_enabled_option_callback(bool enabled)
{
    (void)enabled;
    eq_apply();
}

/*
 * Settings functions
 */
static void eq_apply(void)
{
    dsp_eq_enable(global_settings.eq_enabled);
    dsp_set_eq_precut(global_settings.eq_precut);
    /* Update all bands */
    for(int i = 0; i < EQ_NUM_BANDS; i++) {
        dsp_set_eq_coefs(i, &global_settings.eq_band_settings[i]);
    }
}

static int eq_setting_callback(int action,
                               const struct menu_item_ex *this_item,
                               struct gui_synclist *this_list)
{
    (void)this_list;
    switch (action)
    {
        case ACTION_ENTER_MENUITEM:
            action = lowlatency_callback(action, this_item, NULL);
            break;
        case ACTION_EXIT_MENUITEM:
            eq_apply();
            action = lowlatency_callback(action, this_item, NULL);
            break;
    }

    return action;
}
MENUITEM_SETTING(eq_enable, &global_settings.eq_enabled, eq_setting_callback);
MENUITEM_SETTING(eq_precut, &global_settings.eq_precut, eq_setting_callback);

static char* gainitem_get_name(int selected_item, void *data, char *buffer, size_t len)
{
    (void)data;
    snprintf(buffer, len, str(LANG_EQUALIZER_GAIN_ITEM),
            global_settings.eq_band_settings[selected_item].cutoff);

    return buffer;
}

static int gainitem_speak_item(int selected_item, void *data)
{
    (void)data;
    talk_number(global_settings.eq_band_settings[selected_item].cutoff, false);
    talk_id(LANG_EQUALIZER_GAIN_ITEM, true);
    return 0;
}

static enum themable_icons gainitem_get_icon(int selected_item, void * data)
{
    (void)selected_item;
    (void)data;

    return Icon_Menu_functioncall;
}

static const char* db_format(char* buffer, size_t buffer_size, int value,
                      const char* unit)
{
    int v = abs(value);

    snprintf(buffer, buffer_size, "%s%d.%d %s", value < 0 ? "-" : "",
             v / 10, v % 10, unit);
    return buffer;
}

static int32_t get_dec_talkid(int value, int unit)
{
    return TALK_ID_DECIMAL(value, 1, unit);
}

static const struct int_setting gain_int_setting = {
    .option_callback = NULL,
    .unit = UNIT_DB,
    .step = EQ_GAIN_STEP,
    .min = EQ_GAIN_MIN,
    .max = EQ_GAIN_MAX,
    .formatter = db_format,
    .get_talk_id = get_dec_talkid,
};

static const struct int_setting q_int_setting = {
    .option_callback = NULL,
    .unit = UNIT_INT,
    .step = EQ_Q_STEP,
    .min = EQ_Q_MIN,
    .max = EQ_Q_MAX,
    .formatter = eq_q_format,
    .get_talk_id = get_dec_talkid,
};

static const struct int_setting cutoff_int_setting = {
    .option_callback = NULL,
    .unit = UNIT_HERTZ,
    .step = EQ_CUTOFF_STEP,
    .min = EQ_CUTOFF_MIN,
    .max = EQ_CUTOFF_MAX,
    .formatter = NULL,
    .get_talk_id = NULL,
};

static const struct choice_setting type_choice_setting = {
    .option_callback = NULL,
    .count = 3,
    .cfg_vals = "low_shelf,peaking,high_shelf",
    {.desc = (const unsigned char*[]) {
        ID2P(LANG_EQUALIZER_BAND_LOW_SHELF),
        ID2P(LANG_EQUALIZER_BAND_PEAK),
        ID2P(LANG_EQUALIZER_BAND_HIGH_SHELF),
    }}
};

static int simplelist_action_callback(int action, struct gui_synclist *lists)
{
    (void)lists;
    if (action == ACTION_STD_OK)
        return ACTION_STD_CANCEL;
    return action;
}

static int eq_do_simple_menu(void * param)
{
    (void)param;
    struct simplelist_info info;
    struct settings_list setting;
    char title[MAX_PATH];

    simplelist_info_init(&info, str(LANG_EQUALIZER_GAIN), EQ_NUM_BANDS, NULL);
    info.get_name = (list_get_name*)gainitem_get_name;
    if(global_settings.talk_menu)
        info.get_talk = gainitem_speak_item;
    info.get_icon = gainitem_get_icon;
    info.action_callback = simplelist_action_callback;
    info.selection = -1;
    info.title_icon = Icon_Submenu;
    setting.flags = F_BANFROMQS|F_INT_SETTING|F_T_INT|F_NO_WRAP;
    setting.lang_id = LANG_GAIN;
    setting.default_val.int_ = 0;
    setting.int_setting = &gain_int_setting;

    while (true)
    {
        simplelist_show_list(&info);
        if (info.selection < 0)
            break;
        pcmbuf_set_low_latency(true);
        setting.setting = &global_settings.eq_band_settings[info.selection].gain;
        option_screen(&setting, NULL, false,
                gainitem_get_name(info.selection, NULL, title, MAX_PATH));
        eq_apply();
        pcmbuf_set_low_latency(false);
    }
    return 0;
}
MENUITEM_FUNCTION(gain_menu, 0, ID2P(LANG_EQUALIZER_GAIN),
	              eq_do_simple_menu, NULL, Icon_Submenu);

static void selection_to_banditem(int selection, int expanded_band, int *band, int *item)
{
    int diff = selection - expanded_band;

    if (expanded_band < 0 || diff < 0)
    {
        *item = 0;
        *band = selection;
    }
    else if (diff < 5)
    {
        *item = selection - expanded_band;
        *band = expanded_band;
    }
    else
    {
        *item = 0;
        *band = expanded_band + diff - 4;
    }
}

static char *advancedmenu_item_get_name(int selected_item, void *data, char *buffer, size_t len)
{
    int band;
    int item;
    int lang = -1;

    selection_to_banditem(selected_item, *(intptr_t*)data, &band, &item);

    switch (item)
    {
    case 0: /* Band title */
        snprintf(buffer, len, str(LANG_EQUALIZER_BAND), band);
        return buffer;
    case 1: /* cutoff */
        if (global_settings.eq_band_settings[band].type == EQ_FILTER_PEAK)
            lang = LANG_EQUALIZER_BAND_CENTER;
        else
            lang = LANG_EQUALIZER_BAND_CUTOFF;
        break;
    case 2: /* Q */
        lang = LANG_EQUALIZER_BAND_Q;
        break;
    case 3: /* Gain */
        lang = LANG_GAIN;
        break;
    case 4: /* Type */
        lang = LANG_EQUALIZER_FILTER_TYPE;
        break;
    }

    if(lang < 0)
        buffer[0] = 0;
    else {
        buffer[0] = '\t';
        strmemccpy(&buffer[1], str(lang), len - 1);
    }

    return buffer;
}

static int advancedmenu_speak_item(int selected_item, void *data)
{
    (void)data;
    int band;
    int item;
    int lang = -1;

    selection_to_banditem(selected_item, *(intptr_t*)data, &band, &item);

    switch (item)
    {
    case 0: /* Band title */
        talk_id(LANG_EQUALIZER_BAND, false);
        talk_number(band, true);
        return -1;
    case 1: /* cutoff */
        if (global_settings.eq_band_settings[band].type == EQ_FILTER_PEAK)
            lang = LANG_EQUALIZER_BAND_CENTER;
        else
            lang = LANG_EQUALIZER_BAND_CUTOFF;
        break;
    case 2: /* Q */
        lang = LANG_EQUALIZER_BAND_Q;
        break;
    case 3: /* Gain */
        lang = LANG_GAIN;
        break;
    }
    talk_id(lang, true);
    return -1;
}

static enum themable_icons advancedmenu_get_icon(int selected_item, void * data)
{
    (void)data;
    int band;
    int item;

    selection_to_banditem(selected_item, *(intptr_t*)data, &band, &item);

    if (item == 0)
        return Icon_Submenu;
    else
        return Icon_Menu_setting;
}
extern struct eq_band_setting eq_defaults[EQ_NUM_BANDS];

static int eq_do_advanced_menu(void * param)
{
    (void)param;
    struct simplelist_info info;
    struct settings_list setting;
    char title[MAX_PATH];
    int band, item;
    intptr_t selected_band = -1;

    simplelist_info_init(&info, str(LANG_EQUALIZER_ADVANCED),
            EQ_NUM_BANDS, &selected_band);
    info.get_name = (list_get_name*)advancedmenu_item_get_name;
    if(global_settings.talk_menu)
        info.get_talk = advancedmenu_speak_item;
    info.get_icon = advancedmenu_get_icon;
    info.action_callback = simplelist_action_callback;
    info.selection = -1;
    info.title_icon = Icon_EQ;

    while (true)
    {
        simplelist_show_list(&info);
        if (info.selection < 0)
            break;
        selection_to_banditem(info.selection, selected_band, &band, &item);
        switch (item)
        {
            case 0: /* title, do nothing */
            {
                int extra;
                if (selected_band == band)
                {
                    extra = 0;
                    selected_band = -1;
                }
                else
                {
                    extra = 4;
                    selected_band = band;
                }
                info.selection = band;
                info.count = EQ_NUM_BANDS + extra;
                continue;
            }
            case 1: /* cutoff */
                setting.flags = F_BANFROMQS|F_INT_SETTING|F_T_INT|F_NO_WRAP;
                if (global_settings.eq_band_settings[band].type != EQ_FILTER_PEAK)
                    setting.lang_id = LANG_EQUALIZER_BAND_CUTOFF;
                else
                    setting.lang_id = LANG_EQUALIZER_BAND_CENTER;
                setting.default_val.int_ = eq_defaults[band].cutoff;
                setting.int_setting = &cutoff_int_setting;
                setting.setting = &global_settings.eq_band_settings[band].cutoff;
                break;
            case 2: /* Q */
                setting.flags = F_BANFROMQS|F_INT_SETTING|F_T_INT|F_NO_WRAP;
                setting.lang_id = LANG_EQUALIZER_BAND_Q;
                setting.default_val.int_ = eq_defaults[band].q;
                setting.int_setting = &q_int_setting;
                setting.setting = &global_settings.eq_band_settings[band].q;
                break;
            case 3: /* Gain */
                setting.flags = F_BANFROMQS|F_INT_SETTING|F_T_INT|F_NO_WRAP;
                setting.lang_id = LANG_GAIN;
                setting.default_val.int_ = eq_defaults[band].gain;
                setting.int_setting = &gain_int_setting;
                setting.setting = &global_settings.eq_band_settings[band].gain;
                break;
            case 4: /* Type */
                setting.flags = F_BANFROMQS|F_CHOICE_SETTING|F_T_INT;
                setting.lang_id = LANG_EQUALIZER_FILTER_TYPE;
                setting.default_val.int_ = eq_defaults[band].type;
                setting.choice_setting = &type_choice_setting;
                setting.setting = &global_settings.eq_band_settings[band].type;
                break;
        }
        pcmbuf_set_low_latency(true);
        advancedmenu_item_get_name(info.selection, &selected_band, title, MAX_PATH);

        option_screen(&setting, NULL, false, title[0] == '\t' ? &title[1] : title);
        eq_apply();
        pcmbuf_set_low_latency(false);
    }
    return 0;
}
MENUITEM_FUNCTION(advanced_menu, 0, ID2P(LANG_EQUALIZER_ADVANCED),
                  eq_do_advanced_menu, NULL, Icon_EQ);

enum eq_slider_mode {
    TYPE,
    GAIN,
    CUTOFF,
    Q,
};

/* Draw the UI for a whole EQ band */
static int draw_eq_slider(struct screen * screen, int x, int y,
    int width, int cutoff, int q, int gain, bool selected,
    enum eq_slider_mode mode, int band, int scrollbar_size,
    enum eq_filter_type type)
{
    char buf[26];
    int steps, min_item, max_item;
    int abs_gain = abs(gain);
    int x1, x2, y1, total_height;
    int w, h;

    switch(mode) {
    case TYPE:
        steps = 5;
        min_item = (type * 2);
        max_item = (type * 2) + 1;
        break;
    case Q:
        steps = EQ_Q_MAX - EQ_Q_MIN;
        min_item = q - EQ_Q_STEP - EQ_Q_MIN;
        max_item = q + EQ_Q_STEP - EQ_Q_MIN;
        break;
    case CUTOFF:
        steps = EQ_CUTOFF_MAX - EQ_CUTOFF_MIN;
        min_item = cutoff - EQ_CUTOFF_FAST_STEP * 2;
        max_item = cutoff + EQ_CUTOFF_FAST_STEP * 2;
        break;
    case GAIN:
    default:
        steps = EQ_GAIN_MAX - EQ_GAIN_MIN;
        min_item = abs(EQ_GAIN_MIN) + gain - EQ_GAIN_STEP * 5;
        max_item = abs(EQ_GAIN_MIN) + gain + EQ_GAIN_STEP * 5;
        break;
    }

    /* Start two pixels in, one for border, one for margin */
    x1 = x + 2;
    y1 = y + 2;

    /* Print out the band label */
    if (mode == TYPE && selected)
        screen->set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
    else
        screen->set_drawmode(DRMODE_SOLID);
    if (type == EQ_FILTER_LOW_SHELF) {
        snprintf(buf, sizeof(buf),  "LS%d:", band);
        screen->putsxy(x1, y1, buf);
        /*screen->getstringsize("LS:", &w, &h); UNUSED*/
    } else if (type == EQ_FILTER_HIGH_SHELF) {
        snprintf(buf, sizeof(buf),  "HS%d:", band);
        screen->putsxy(x1, y1, buf);
        /*screen->getstringsize("HS:", &w, &h); UNUSED*/
    } else {
        snprintf(buf, sizeof(buf),  "PK%d:", band);
        screen->putsxy(x1, y1, buf);
        /*screen->getstringsize(buf, &w, &h); UNUSED*/
    }

    w = screen->getstringsize("A", NULL, &h);

    x1 += 5*w; /* 4 chars for label + 1 space = 5 */

    /* Print out gain part of status line (left justify after label) */
    if (mode == GAIN && selected)
        screen->set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
    else
        screen->set_drawmode(DRMODE_SOLID);

    snprintf(buf, sizeof(buf), "%s%2d.%d%s", gain < 0 ? "-" : " ",
        abs_gain / EQ_USER_DIVISOR, abs_gain % EQ_USER_DIVISOR,
        screen->lcdwidth >= 160 ? "dB" : "");
    screen->putsxy(x1, y1, buf);
    w = screen->getstringsize(buf, NULL, NULL);
    x1 += w;

    /* Print out Q part of status line (right justify) */
    if (mode == Q && selected)
        screen->set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
    else
        screen->set_drawmode(DRMODE_SOLID);

    snprintf(buf, sizeof(buf), "%d.%d%s", q / EQ_USER_DIVISOR,
             q % EQ_USER_DIVISOR, screen->lcdwidth >= 160 ? " Q" : "");
    w = screen->getstringsize(buf, NULL, NULL);
    x2 = x + width - w - 2;
    screen->putsxy(x2, y1, buf);

    /* Print out cutoff part of status line (center between gain & Q) */
    if (mode == CUTOFF && selected)
        screen->set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
    else
        screen->set_drawmode(DRMODE_SOLID);

    snprintf(buf, sizeof(buf),  "%5d%s", cutoff,
             screen->lcdwidth >= 160 ? "Hz" : "");
    w = screen->getstringsize(buf, NULL, NULL);
    x1 = x1 + (x2 - x1 - w)/2;
    screen->putsxy(x1, y1, buf);

    /* Draw selection box */
    total_height = 3 + h + 1 + scrollbar_size + 3;
    screen->set_drawmode(DRMODE_SOLID);
    if (selected) {
        screen->drawrect(x, y, width, total_height);
    }

    /* Draw horizontal slider. Reuse scrollbar for this */
    gui_scrollbar_draw(screen, x + 3, y1 + h + 1, width - 6, scrollbar_size,
                       steps, min_item, max_item, HORIZONTAL);

    return total_height;
}

/* Draw's all the EQ sliders. Returns the total height of the sliders drawn */
static void draw_eq_sliders(struct screen * screen, int x, int y,
                            int nb_eq_sliders, int start_item,
                            int current_band, enum eq_slider_mode mode, int scrollbar_size)
{
    int height = y;

    start_item = MIN(start_item, EQ_NUM_BANDS - nb_eq_sliders);

    for (int i = 0; i < EQ_NUM_BANDS; i++) {
        struct eq_band_setting *setting = &global_settings.eq_band_settings[i];
        int cutoff = setting->cutoff;
        int q      = setting->q;
        int gain   = setting->gain;
        enum eq_filter_type type = setting->type;

        if (i == start_item + nb_eq_sliders)
            break;

        if (i >= start_item) {
            height += draw_eq_slider(screen, x, height, screen->lcdwidth - x - 1,
                                     cutoff, q, gain, i == current_band, mode,
                                     i, scrollbar_size, type);
            /* add a margin */
            height++;
        }
    }

    if (nb_eq_sliders != EQ_NUM_BANDS)
        gui_scrollbar_draw(screen, 0, y, scrollbar_size - 1,
                           screen->lcdheight - y, EQ_NUM_BANDS,
                           start_item, start_item + nb_eq_sliders,
                           VERTICAL);
    return;
}

/* Provides a graphical means of editing the EQ settings */
int eq_menu_graphical(void)
{
    bool exit_request = false;
    bool result = true;
    bool has_changed = false;
    int button;
    int *setting;
    int current_band, x, y, step, fast_step, min, max;
    enum eq_slider_mode mode;
    int h, height, start_item, nb_eq_sliders[NB_SCREENS];
    int scrollbar_size[NB_SCREENS];

    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, false, NULL);

    FOR_NB_SCREENS(i) {
        screens[i].set_viewport(NULL);
        screens[i].clear_display();

        /* Figure out how many sliders can be drawn on the screen */
        h = screens[i].getcharheight();
        scrollbar_size[i] = h * 3 / 4;
        if (scrollbar_size[i] < 6)
            scrollbar_size[i] = 6;

        /* Total height includes margins (1), text, slider, and line selector (1) */
        height = 3 + h + 1 + scrollbar_size[i] + 3;
        nb_eq_sliders[i] = screens[i].lcdheight / height;

        /* Make sure the "Edit Mode" text fits too */
        height = nb_eq_sliders[i]*height + h + 2;
        if (height > screens[i].lcdheight)
            nb_eq_sliders[i]--;

        if (nb_eq_sliders[i] > EQ_NUM_BANDS)
            nb_eq_sliders[i] = EQ_NUM_BANDS;
    }

    y = h + 1;

    /* Start off editing gain on the first band */
    mode = GAIN;
    current_band = 0;

    while (!exit_request) {
        FOR_NB_SCREENS(i)
        {
            screens[i].clear_display();

            /* Set pointer to the band data currently editable */
            if (mode == TYPE) {
                /* type */
                setting = (int*) &global_settings.eq_band_settings[current_band].type;

                step = 1;
                fast_step = 1;
                min = EQ_FILTER_LOW_SHELF;
                max = EQ_FILTER_HIGH_SHELF;

                screens[i].putsxyf(0, 0, str(LANG_SYSFONT_EQUALIZER_EDIT_MODE),
                         str(LANG_EQUALIZER_FILTER_TYPE), "");
            } else if (mode == GAIN) {
                /* gain */
                setting = &global_settings.eq_band_settings[current_band].gain;

                step = EQ_GAIN_STEP;
                fast_step = EQ_GAIN_FAST_STEP;
                min = EQ_GAIN_MIN;
                max = EQ_GAIN_MAX;

                screens[i].putsxyf(0, 0, str(LANG_SYSFONT_EQUALIZER_EDIT_MODE),
                         str(LANG_GAIN), "(dB)");
            } else if (mode == CUTOFF) {
                /* cutoff */
                setting = &global_settings.eq_band_settings[current_band].cutoff;

                step = EQ_CUTOFF_STEP;
                fast_step = EQ_CUTOFF_FAST_STEP;
                min = EQ_CUTOFF_MIN;
                max = EQ_CUTOFF_MAX;

                screens[i].putsxyf(0, 0, str(LANG_SYSFONT_EQUALIZER_EDIT_MODE),
                         str(LANG_SYSFONT_EQUALIZER_BAND_CUTOFF), "(Hz)");
            } else {
                /* Q */
                setting = &global_settings.eq_band_settings[current_band].q;

                step = EQ_Q_STEP;
                fast_step = EQ_Q_FAST_STEP;
                min = EQ_Q_MIN;
                max = EQ_Q_MAX;

                screens[i].putsxyf(0, 0, str(LANG_SYSFONT_EQUALIZER_EDIT_MODE),
                         str(LANG_EQUALIZER_BAND_Q), "");
            }

            /* Draw scrollbar if needed */
            if (nb_eq_sliders[i] != EQ_NUM_BANDS)
            {
                if (current_band == 0) {
                    start_item = 0;
                } else if (current_band == EQ_NUM_BANDS - 1) {
                    start_item = EQ_NUM_BANDS - nb_eq_sliders[i];
                } else {
                    start_item = current_band - 1;
                }
                x = scrollbar_size[i];
            } else {
                x = 1;
                start_item = 0;
            }
            /* Draw equalizer band details */
            draw_eq_sliders(&screens[i], x, y, nb_eq_sliders[i], start_item,
                            current_band, mode, scrollbar_size[i]);

            screens[i].update();
        }

        button = get_action(CONTEXT_SETTINGS_EQ,TIMEOUT_BLOCK);

        switch (button) {
        case ACTION_SETTINGS_DEC:
        case ACTION_SETTINGS_DECREPEAT:
            *(setting) -= step;
            has_changed = true;
            if (*(setting) < min)
                *(setting) = min;
            break;

        case ACTION_SETTINGS_INC:
        case ACTION_SETTINGS_INCREPEAT:
            *(setting) += step;
            has_changed = true;
            if (*(setting) > max)
                *(setting) = max;
            break;

        case ACTION_SETTINGS_INCBIGSTEP:
            *(setting) += fast_step;
            has_changed = true;
            if (*(setting) > max)
                *(setting) = max;
            break;

        case ACTION_SETTINGS_DECBIGSTEP:
            *(setting) -= fast_step;
            has_changed = true;
            if (*(setting) < min)
                *(setting) = min;
            break;

        case ACTION_STD_PREV:
        case ACTION_STD_PREVREPEAT:
            current_band--;
            if (current_band < 0)
                current_band = EQ_NUM_BANDS - 1; /* wrap around */
            break;

        case ACTION_STD_NEXT:
        case ACTION_STD_NEXTREPEAT:
            current_band = (current_band + 1) % EQ_NUM_BANDS;
            break;

        case ACTION_STD_OK:
            mode++;
            if (mode > Q)
                mode = TYPE; /* wrap around */
            break;

        case ACTION_STD_CANCEL:
            exit_request = true;
            result = false;
            break;
        default:
            if(default_event_handler(button) == SYS_USB_CONNECTED) {
                exit_request = true;
                result = true;
            }
            break;
        }

        /* Update the filter if the user changed something */
        if (has_changed) {
            dsp_set_eq_coefs(current_band,
                &global_settings.eq_band_settings[current_band]);
            has_changed = false;
        }
    }

    /* Reset screen settings */
    FOR_NB_SCREENS(i)
    {
        screens[i].clear_display();
        screens[i].set_viewport(NULL);
        viewportmanager_theme_undo(i, false);
    }
    return (result) ? 1 : 0;
}

static int eq_save_preset(void)
{
    /* make sure that the eq is enabled for setting saving */
    bool enabled = global_settings.eq_enabled;
    global_settings.eq_enabled = true;

    bool result = settings_save_config(SETTINGS_SAVE_EQPRESET);

    global_settings.eq_enabled = enabled;

    return (result) ? 1 : 0;
}
/* Allows browsing of preset files */
static struct browse_folder_info eqs = { EQS_DIR, SHOW_CFG };

static void eq_reset_defaults(void)
{
    for (int i = 0; i < EQ_NUM_BANDS; i++) {
        global_settings.eq_band_settings[i].type = eq_defaults[i].type;
        global_settings.eq_band_settings[i].cutoff = eq_defaults[i].cutoff;
        global_settings.eq_band_settings[i].q = eq_defaults[i].q;
        global_settings.eq_band_settings[i].gain = eq_defaults[i].gain;
    }
    eq_apply();
}

MENUITEM_FUNCTION(eq_graphical, 0, ID2P(LANG_EQUALIZER_GRAPHICAL),
                  eq_menu_graphical, lowlatency_callback, Icon_EQ);
MENUITEM_FUNCTION(eq_save, 0, ID2P(LANG_EQUALIZER_SAVE),
                  eq_save_preset, NULL, Icon_NOICON);
MENUITEM_FUNCTION(eq_reset, 0, ID2P(LANG_RESET_EQUALIZER),
                  eq_reset_defaults, NULL, Icon_NOICON);
MENUITEM_FUNCTION_W_PARAM(eq_browse, 0, ID2P(LANG_EQUALIZER_BROWSE),
                          browse_folder, (void*)&eqs,
                          lowlatency_callback, Icon_NOICON);

MAKE_MENU(equalizer_menu, ID2P(LANG_EQUALIZER), NULL, Icon_EQ,
        &eq_enable, &eq_graphical, &eq_precut, &gain_menu,
        &advanced_menu, &eq_save, &eq_browse, &eq_reset);

/* The equalizer of the iPod settings, as on a stock iPod: one list of Off
   and iTunes' presets, each set by selecting it. The gains are iTunes' own,
   in hundredths of a dB, for bands at 32 Hz to 16 kHz like the default
   bands here. */
static const struct
{
    int lang_id;
    short gain[EQ_NUM_BANDS];
} eq_presets[] =
{
    { LANG_EQ_ACOUSTIC,
      { 500, 490, 395, 105, 215, 175, 350, 410, 355, 215 } },
    { LANG_EQ_BASS_BOOSTER,
      { 550, 425, 350, 250, 125, 0, 0, 0, 0, 0 } },
    { LANG_EQ_BASS_REDUCER,
      { -550, -425, -350, -250, -125, 0, 0, 0, 0, 0 } },
    { LANG_EQ_CLASSICAL,
      { 475, 375, 300, 250, -150, -150, 0, 225, 325, 375 } },
    { LANG_EQ_DANCE,
      { 357, 655, 499, 0, 192, 365, 515, 454, 359, 0 } },
    { LANG_EQ_DEEP,
      { 495, 355, 175, 100, 285, 250, 145, -215, -355, -460 } },
    { LANG_EQ_ELECTRONIC,
      { 425, 380, 120, 0, -215, 225, 85, 125, 395, 480 } },
    { LANG_EQ_FLAT,
      { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
    { LANG_EQ_HIP_HOP,
      { 500, 425, 150, 300, -100, -100, 150, -50, 200, 300 } },
    { LANG_EQ_JAZZ,
      { 400, 300, 150, 225, -150, -150, 0, 150, 300, 375 } },
    { LANG_EQ_LATIN,
      { 450, 300, 0, 0, -150, -150, -150, 0, 300, 450 } },
    { LANG_EQ_LOUDNESS,
      { 600, 400, 0, 0, -200, 0, -100, -500, 500, 100 } },
    { LANG_EQ_LOUNGE,
      { -300, -150, -50, 150, 400, 250, 0, -150, 200, 100 } },
    { LANG_EQ_PIANO,
      { 300, 200, 0, 250, 300, 150, 350, 450, 300, 350 } },
    { LANG_EQ_POP,
      { -150, -100, 0, 200, 400, 400, 200, 0, -100, -150 } },
    { LANG_EQ_RNB,
      { 262, 692, 565, 133, -219, -150, 232, 265, 300, 375 } },
    { LANG_EQ_ROCK,
      { 500, 400, 300, 150, -50, -100, 50, 250, 350, 450 } },
    { LANG_EQ_SMALL_SPEAKERS,
      { 550, 425, 350, 250, 125, 0, -125, -250, -350, -425 } },
    { LANG_EQ_SPOKEN_WORD,
      { -346, -47, 0, 69, 346, 461, 484, 428, 254, 0 } },
    { LANG_EQ_TREBLE_BOOSTER,
      { 0, 0, 0, 0, 0, 125, 250, 350, 425, 550 } },
    { LANG_EQ_TREBLE_REDUCER,
      { 0, 0, 0, 0, 0, -125, -250, -350, -425, -550 } },
    { LANG_EQ_VOCAL_BOOSTER,
      { -150, -300, -300, 150, 375, 375, 300, 150, 0, -150 } },
};

/* For particular earphones, first in the list. From measurements of 4 of them by 3
   people, against the Harman in-ear target. Few bands, at their own frequencies and
   the rest off, as each one takes time from the UI. Bands are numbered as the
   defaults, and q and gain are in tenths. The precut is the largest boost after the
   bands add up, in tenths of a dB. */
#define EQ_PHONES_BANDS 5
static const struct
{
    int lang_id;
    int precut;
    struct
    {
        int band;
        struct eq_band_setting setting;
    } bands[EQ_PHONES_BANDS];
} eq_phones[] =
{
    /* 7Hz Zero 2: 2 dB too much at 100-250 Hz, and 2 to 5 dB too little at 2.5-6.3 kHz */
    { LANG_EQ_ZERO2, 40,
      { { 2, { EQ_FILTER_PEAK, 170, 10, -25 } },
        { 7, { EQ_FILTER_PEAK, 5000, 10, 40 } } } },
    /* a little ahead of the target at 1.6-4 kHz, for voices */
    { LANG_EQ_ZERO2_VOICE, 37,
      { { 2, { EQ_FILTER_PEAK, 170, 10, -30 } },
        { 6, { EQ_FILTER_PEAK, 3000, 12, 25 } },
        { 7, { EQ_FILTER_PEAK, 5500, 12, 30 } } } },
    /* for strings, quiet sounds and cymbals: all of the bass hump gone but not the
       lowest bass, 2-5 kHz a little ahead, and the dip around 10 kHz filled in.
       Their peak at 8-8.5 kHz is cut, right after a dip it stands 6 to 10 dB over:
       with it these made a lot of sibilance. So is some of the one at 12.5-13 kHz,
       5 dB over the target. */
    { LANG_EQ_ZERO2_DETAIL, 38,
      { { 2, { EQ_FILTER_PEAK, 170, 8, -35 } },
        { 5, { EQ_FILTER_PEAK, 4000, 8, 40 } },
        { 6, { EQ_FILTER_PEAK, 8300, 30, -50 } },
        { 7, { EQ_FILTER_PEAK, 10500, 40, 20 } },
        { 8, { EQ_FILTER_PEAK, 12800, 30, -30 } } } },
    /* the same way, less far, for treble that's too much with the first */
    { LANG_EQ_ZERO2_SOFT, 25,
      { { 2, { EQ_FILTER_PEAK, 170, 10, -20 } },
        { 7, { EQ_FILTER_PEAK, 5000, 10, 25 } } } },
};

/* Preset numbers: 0 is off, then the earphones', then iTunes' */
static struct eq_band_setting eq_preset_band(int preset, int band)
{
    struct eq_band_setting setting = eq_defaults[band];
    preset--;
    if (preset < (int)ARRAYLEN(eq_phones))
    {
        for (int i = 0; i < EQ_PHONES_BANDS; i++)
        {
            /* unused entries are band 0 with gain 0, which is the default anyway */
            if (eq_phones[preset].bands[i].setting.gain != 0
                && eq_phones[preset].bands[i].band == band)
                setting = eq_phones[preset].bands[i].setting;
        }
    }
    else
    {
        /* to the tenths of a dB the bands are set in */
        int gain = eq_presets[preset - ARRAYLEN(eq_phones)].gain[band];
        setting.gain = (gain + (gain < 0 ? -5 : 5)) / 10;
    }
    return setting;
}

static int eq_get_preset(void);

static void eq_set_preset(int preset)
{
    const bool changed = preset != eq_get_preset();
    global_settings.eq_enabled = preset > 0;
    if (preset > 0)
    {
        int precut = 0;
        for (int i = 0; i < EQ_NUM_BANDS; i++)
        {
            global_settings.eq_band_settings[i] = eq_preset_band(preset, i);
            precut = MAX(precut, global_settings.eq_band_settings[i].gain);
        }
        if (preset <= (int)ARRAYLEN(eq_phones))
            precut = eq_phones[preset - 1].precut;
        /* down by the largest boost, so that loud music doesn't clip */
        global_settings.eq_precut = precut;
    }
    eq_apply();
    if (changed)
        dsp_change_heard_now();
}

/* the preset the equalizer is set to, or Off (0) if none is */
static int eq_get_preset(void)
{
    if (!global_settings.eq_enabled)
        return 0;
    const int presets = ARRAYLEN(eq_phones) + ARRAYLEN(eq_presets);
    for (int preset = 1; preset <= presets; preset++)
    {
        int i = 0;
        for (; i < EQ_NUM_BANDS; i++)
        {
            const struct eq_band_setting *band =
                &global_settings.eq_band_settings[i];
            const struct eq_band_setting wanted = eq_preset_band(preset, i);
            if (band->type != wanted.type
                || band->cutoff != wanted.cutoff
                || band->q != wanted.q
                || band->gain != wanted.gain)
                break;
        }
        if (i == EQ_NUM_BANDS)
            return preset;
    }
    return 0;
}

static int eq_choose_preset(void)
{
    static const unsigned char *names[1 + ARRAYLEN(eq_phones) + ARRAYLEN(eq_presets)];
    names[0] = ID2P(LANG_OFF);
    for (int i = 0; i < (int)ARRAYLEN(eq_phones); i++)
        names[1 + i] = ID2P(eq_phones[i].lang_id);
    for (int i = 0; i < (int)ARRAYLEN(eq_presets); i++)
        names[1 + ARRAYLEN(eq_phones) + i] = ID2P(eq_presets[i].lang_id);

    static const struct choice_setting choice =
        { eq_set_preset, ARRAYLEN(names), NULL, { .desc = names } };
    static int preset;
    preset = eq_get_preset();

    /* the preset is set when it is selected, and Menu leaves the EQ as it
       was */
    struct settings_list setting = {
        .flags = F_CHOICE_SETTING|F_T_INT|F_CB_ON_SELECT_ONLY|F_BANFROMQS,
        .setting = &preset,
        .lang_id = LANG_EQUALIZER,
        .default_val.int_ = 0,
        .choice_setting = &choice,
    };
    return option_screen(&setting, NULL, false, ID2P(LANG_EQUALIZER));
}

MENUITEM_FUNCTION(ipod_eq_menu, 0, ID2P(LANG_EQUALIZER),
                  eq_choose_preset, lowlatency_callback, Icon_EQ);

/* in the settings list: with the preset, or nothing if on and set to none */
static char *eq_row_name(int selected_item, void *data,
                         char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    const int phones = ARRAYLEN(eq_phones);
    const int preset = eq_get_preset();
    if (!preset && global_settings.eq_enabled)
        return str(LANG_EQUALIZER);
    snprintf(buffer, buffer_len, "%s\t%s", str(LANG_EQUALIZER),
             str(!preset ? LANG_OFF : preset <= phones ?
                 eq_phones[preset - 1].lang_id : eq_presets[preset - phones - 1].lang_id));
    return buffer;
}

MENUITEM_FUNCTION_DYNTEXT(ipod_eq_row, 0, eq_choose_preset, eq_row_name,
                          NULL, NULL, lowlatency_callback, Icon_EQ);
