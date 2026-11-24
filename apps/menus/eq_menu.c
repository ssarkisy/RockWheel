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
#include <errno.h>
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
#include "root_menu.h"
#include "string-extra.h"
#include "transition.h"
#include "splash.h"
#include "eq_files.h"
#include "eq_curve.h"
#include "eq_editor.h"
#include "health_log.h"
#include "file.h"
#ifdef HAVE_TAGCACHE
#include "music_search.h"
#endif

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
    eq_makeup_now(global_settings.eq_enabled ? global_settings.eq_precut : 0);
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

/* The equalizer of the iPod settings: Off and iTunes' presets, as on a stock iPod,
   each set by selecting it, and the user's own. The gains are iTunes' own, in
   hundredths of a dB, for bands at 32 Hz to 16 kHz like the default bands here.

   Each preset's precut, in tenths of a dB, is its largest boost after the bands add
   up, with the DSP's filters, which is more than any one band where they overlap. It's
   in whole dB, up from that boost, as the volume makes up for it in steps of one: the
   two are then the same, and a preset is as loud as no EQ while the amplifier has room
   for it. Less than 0.1 dB over a whole dB is let pass. */
static const struct
{
    int lang_id;
    int precut;
    short gain[EQ_NUM_BANDS];
} eq_presets[] =
{
    { LANG_EQ_ACOUSTIC, 70, /* 6.7 dB at 69 Hz */
      { 500, 490, 395, 105, 215, 175, 350, 410, 355, 215 } },
    { LANG_EQ_BASS_BOOSTER, 60, /* 6.1 at 69 Hz */
      { 550, 425, 350, 250, 125, 0, 0, 0, 0, 0 } },
    { LANG_EQ_BASS_REDUCER, 0,
      { -550, -425, -350, -250, -125, 0, 0, 0, 0, 0 } },
    { LANG_EQ_CLASSICAL, 60, /* 5.3 at 69 Hz */
      { 475, 375, 300, 250, -150, -150, 0, 225, 325, 375 } },
    { LANG_EQ_DANCE, 90, /* 8.6 at 68 Hz */
      { 357, 655, 499, 0, 192, 365, 515, 454, 359, 0 } },
    { LANG_EQ_DEEP, 50, /* 5.05 below 32 Hz */
      { 495, 355, 175, 100, 285, 250, 145, -215, -355, -460 } },
    { LANG_EQ_ELECTRONIC, 50, /* 4.8 at the top */
      { 425, 380, 120, 0, -215, 225, 85, 125, 395, 480 } },
    { LANG_EQ_FLAT, 0,
      { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
    { LANG_EQ_HIP_HOP, 60, /* 5.3 at 65 Hz */
      { 500, 425, 150, 300, -100, -100, 150, -50, 200, 300 } },
    { LANG_EQ_JAZZ, 40, /* 4.04 below 32 Hz */
      { 400, 300, 150, 225, -150, -150, 0, 150, 300, 375 } },
    { LANG_EQ_LATIN, 50, /* 4.5 at both ends */
      { 450, 300, 0, 0, -150, -150, -150, 0, 300, 450 } },
    { LANG_EQ_LOUDNESS, 60, /* 6.03 below 32 Hz */
      { 600, 400, 0, 0, -200, 0, -100, -500, 500, 100 } },
    { LANG_EQ_LOUNGE, 60, /* 5.2 at 515 Hz */
      { -300, -150, -50, 150, 400, 250, 0, -150, 200, 100 } },
    { LANG_EQ_PIANO, 70, /* 6.5 at 3.9 kHz */
      { 300, 200, 0, 250, 300, 150, 350, 450, 300, 350 } },
    { LANG_EQ_POP, 60, /* 6.03 at 543 Hz */
      { -150, -100, 0, 200, 400, 400, 200, 0, -100, -150 } },
    { LANG_EQ_RNB, 100, /* 9.1 at 69 Hz */
      { 262, 692, 565, 133, -219, -150, 232, 265, 300, 375 } },
    { LANG_EQ_ROCK, 60, /* 5.4 at 68 Hz */
      { 500, 400, 300, 150, -50, -100, 50, 250, 350, 450 } },
    { LANG_EQ_SMALL_SPEAKERS, 60, /* 6.07 at 69 Hz */
      { 550, 425, 350, 250, 125, 0, -125, -250, -350, -425 } },
    { LANG_EQ_SPOKEN_WORD, 80, /* 7.9 at 2 kHz */
      { -346, -47, 0, 69, 346, 461, 484, 428, 254, 0 } },
    { LANG_EQ_TREBLE_BOOSTER, 60, /* 5.5 at the top */
      { 0, 0, 0, 0, 0, 125, 250, 350, 425, 550 } },
    { LANG_EQ_TREBLE_REDUCER, 0,
      { 0, 0, 0, 0, 0, -125, -250, -350, -425, -550 } },
    { LANG_EQ_VOCAL_BOOSTER, 60, /* 6.03 at 977 Hz */
      { -150, -300, -300, 150, 375, 375, 300, 150, 0, -150 } },
};

/* The presets for particular earphones that were built in until 2.1.1, ids 1 to
   6 of the settings' eq_preset: now presets of the user's, written once into
   /.rockbox/eqs (eq_phones_boot()) and listed, renamed and deleted as any other,
   a model's several in a folder of its name. From measurements of several of
   them, against the Harman target of their kind. Few bands, at their own
   frequencies and the rest off, as each one takes time from the UI. Bands are
   numbered as the defaults, and q and gain are in tenths. The precut is the
   files' own, from the curve's peak (eq_files_load()), as for every preset */
#define EQ_PHONES_BANDS 6
static const struct
{
    int id;             /* eq_preset's, in a setting from before */
    const char *folder; /* in /.rockbox/eqs, "" for the top */
    const char *name;   /* the file's, without ".cfg", in English in every language */
    struct
    {
        int band;
        struct eq_band_setting setting;
    } bands[EQ_PHONES_BANDS];
} eq_phones[] =
{
    /* 7Hz Zero 2: 2 dB too much at 100-250 Hz, and 2 to 5 dB too little at 2.5-6.3 kHz */
    { 1, "7Hz Zero 2", "7Hz Zero 2",
      { { 2, { EQ_FILTER_PEAK, 170, 10, -25 } },
        { 7, { EQ_FILTER_PEAK, 5000, 10, 40 } } } },
    /* a little ahead of the target at 1.6-4 kHz, for voices */
    { 2, "7Hz Zero 2", "7Hz Zero 2 Voice",
      { { 2, { EQ_FILTER_PEAK, 170, 10, -30 } },
        { 6, { EQ_FILTER_PEAK, 3000, 12, 25 } },
        { 7, { EQ_FILTER_PEAK, 5500, 12, 30 } } } },
    /* for strings, quiet sounds and cymbals: all of the bass hump gone but not the
       lowest bass, 2-5 kHz a little ahead, and the dip around 10 kHz filled in.
       Their peak at 8-8.5 kHz is cut, right after a dip it stands 6 to 10 dB over:
       with it these made a lot of sibilance. So is some of the one at 12.5-13 kHz,
       5 dB over the target. */
    { 3, "7Hz Zero 2", "7Hz Zero 2 Detail",
      { { 2, { EQ_FILTER_PEAK, 170, 8, -35 } },
        { 5, { EQ_FILTER_PEAK, 4000, 8, 40 } },
        { 6, { EQ_FILTER_PEAK, 8300, 30, -50 } },
        { 7, { EQ_FILTER_PEAK, 10500, 40, 20 } },
        { 8, { EQ_FILTER_PEAK, 12800, 30, -30 } } } },
    /* the same way, less far, for treble that's too much with the first */
    { 4, "7Hz Zero 2", "7Hz Zero 2 Soft",
      { { 2, { EQ_FILTER_PEAK, 170, 10, -20 } },
        { 7, { EQ_FILTER_PEAK, 5000, 10, 25 } } } },
    /* Koss KPH30i, from 3 measurements against the over-ear target and then by ear: 10 dB
       too little at 40 Hz, of which more is made up than the target asks, and 3 dB too
       much at 125-250 Hz, of which half is cut, as all of it sounded dry. A notch of 8 to
       12 dB at 4-4.5 kHz comes right before a peak at 5 kHz that a single wide band would
       raise, then dips at 6.3 and 10 kHz. Voices wanted a little more around 3 kHz. */
    { 5, "", "Koss KPH30i",
      { { 0, { EQ_FILTER_LOW_SHELF, 70, 10, 75 } },
        { 2, { EQ_FILTER_PEAK, 190, 11, -15 } },
        { 4, { EQ_FILTER_PEAK, 3000, 12, 15 } },
        { 5, { EQ_FILTER_PEAK, 4200, 40, 50 } },
        { 6, { EQ_FILTER_PEAK, 6300, 20, 35 } },
        { 7, { EQ_FILTER_PEAK, 10400, 40, 40 } } } },
    /* Sony WH-1000XM4C by cable, switched on, with noise cancelling, from one measurement
       against the target of its rig and then by ear: 3 to 5 dB too much from 20 to
       250 Hz, cut further at 125-200 Hz, where it booms, than at 50-100 Hz, so that the
       bass still punches. 3 dB too little at 2-2.5 kHz, filled for voices and guitars.
       Sibilance, loudest at 8 kHz, and a peak at 10 kHz are cut only in part: more,
       or anything off at 4 kHz or above 12 kHz, made the sound closed or dull. */
    { 6, "", "Sony WH-1000XM4C",
      { { 0, { EQ_FILTER_LOW_SHELF, 100, 6, -50 } },
        { 2, { EQ_FILTER_PEAK, 160, 10, -55 } },
        { 6, { EQ_FILTER_PEAK, 2300, 20, 30 } },
        { 7, { EQ_FILTER_PEAK, 8000, 40, -30 } },
        { 8, { EQ_FILTER_PEAK, 10000, 50, -33 } } } },
};

/* Preset numbers: 0 is off, then iTunes' */
static struct eq_band_setting eq_preset_band(int preset, int band)
{
    struct eq_band_setting setting = eq_defaults[band];
    /* to the tenths of a dB the bands are set in */
    int gain = eq_presets[preset - 1].gain[band];
    setting.gain = (gain + (gain < 0 ? -5 : 5)) / 10;
    return setting;
}

static int eq_preset_precut(int preset)
{
    return eq_presets[preset - 1].precut;
}

/* the id the settings keep for a preset: 101 on for iTunes', which never
   change, as 1 to 6 were the earphones' (eq_phones) */
#define EQ_ITUNES_ID 101
static int eq_preset_id(int preset)
{
    return EQ_ITUNES_ID + preset - 1;
}

#define EQ_PRESETS ((int)ARRAYLEN(eq_presets))

/* the preset whose bands the EQ's are, or 0 */
static int eq_preset_of_bands(void)
{
    for (int preset = 1; preset <= EQ_PRESETS; preset++)
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

/* Beyond the built-in presets' ids, those the settings keep for the EQ's own:
   Custom (global_settings.eq_custom), and the user's preset at eq_preset_file.
   An older firmware finds no preset of these and keeps the bands config.cfg
   has, which are the EQ's */
#define EQ_ID_CUSTOM 900
#define EQ_ID_FILE   901
/* and their numbers here, after the built-in presets' */
#define EQ_CUSTOM (EQ_PRESETS + 1)
#define EQ_FILE   (EQ_PRESETS + 2)

/* the preset the equalizer is set to, or Off (0) if none is: the one kept, or for
   settings from before that, the one with its bands */
static int eq_get_preset(void)
{
    if (!global_settings.eq_enabled)
        return 0;
    if (global_settings.eq_preset == EQ_ID_CUSTOM)
        return EQ_CUSTOM;
    if (global_settings.eq_preset == EQ_ID_FILE)
        return EQ_FILE;
    for (int preset = 1; preset <= EQ_PRESETS; preset++)
    {
        if (eq_preset_id(preset) == global_settings.eq_preset)
            return preset;
    }
    return eq_preset_of_bands();
}

/* whether eq_take() has had the music decoded again, since it was cleared */
static bool eq_sought;

/* The EQ set to these bands and precut, by the id the settings keep and the
   user's preset's path: heard at once if that changes the sound, and saved if
   it changes anything */
static void eq_take(const struct eq_band_setting *bands, int precut, int id,
                    const char *file)
{
    if (!file)
        file = "";
    const bool heard = !global_settings.eq_enabled
        || (int)global_settings.eq_precut != precut
        || memcmp(global_settings.eq_band_settings, bands,
                  sizeof(global_settings.eq_band_settings));
    const bool changed = heard || global_settings.eq_preset != id
        || strcmp(global_settings.eq_preset_file, file);
    global_settings.eq_enabled = true;
    global_settings.eq_preset = id;
    if (file != global_settings.eq_preset_file)
        strmemccpy(global_settings.eq_preset_file, file,
                   sizeof(global_settings.eq_preset_file));
    memmove(global_settings.eq_band_settings, bands,
            sizeof(global_settings.eq_band_settings));
    global_settings.eq_precut = precut;
    if (heard)
        dsp_change_heard_now(eq_apply);
    else
        eq_apply();
    eq_sought |= heard;
    if (changed)
        settings_save();
}

/* Off or a built-in preset */
static void eq_set_preset(int preset)
{
    if (preset > 0)
    {
        struct eq_band_setting bands[EQ_NUM_BANDS];
        for (int i = 0; i < EQ_NUM_BANDS; i++)
            bands[i] = eq_preset_band(preset, i);
        /* down by the largest boost, so that loud music doesn't clip */
        eq_take(bands, eq_preset_precut(preset), eq_preset_id(preset), NULL);
        return;
    }
    /* on and set to none is a setting of its own, which Off changes too */
    const bool changed = global_settings.eq_enabled || global_settings.eq_preset != 0;
    global_settings.eq_enabled = false;
    global_settings.eq_preset = 0;
    global_settings.eq_preset_file[0] = '\0';
    if (changed)
    {
        dsp_change_heard_now(eq_apply);
        settings_save();
    }
    else
        eq_apply();
}

void eq_set_custom(void)
{
    eq_take(global_settings.eq_custom, global_settings.eq_custom_precut,
            EQ_ID_CUSTOM, NULL);
}

/* Custom made a copy of these, and the EQ set to it */
static void eq_custom_from(const struct eq_band_setting *bands, int precut)
{
    memmove(global_settings.eq_custom, bands, sizeof(global_settings.eq_custom));
    global_settings.eq_custom_precut = precut;
    eq_set_custom();
}

static void eq_custom_from_preset(int preset)
{
    struct eq_band_setting bands[EQ_NUM_BANDS];
    for (int i = 0; i < EQ_NUM_BANDS; i++)
        bands[i] = eq_preset_band(preset, i);
    eq_custom_from(bands, eq_preset_precut(preset));
}

/* Bands as config.cfg gave them, which anything may have written: a band
   outside the settings' ranges or that the DSP plays otherwise than the curve
   shows is the default band, as a preset's file of it would not be taken; the
   curve's maths overflow far outside them. Whether one was */
static bool eq_bands_check(struct eq_band_setting *bands)
{
    bool reset = false;
    for (int i = 0; i < EQ_NUM_BANDS; i++)
    {
        if (!eq_files_band_ok(&bands[i]))
        {
            bands[i] = eq_defaults[i];
            reset = true;
        }
    }
    return reset;
}

/* Custom's, and the EQ's own: those an older firmware or a config.cfg of
   bands alone set, which no preset takes the place of (eq_get_preset()'s 0),
   were played as they were */
static void eq_config_check(void)
{
    if (eq_bands_check(global_settings.eq_custom)
        || global_settings.eq_custom_precut < 0
        || global_settings.eq_custom_precut > 240)
        global_settings.eq_custom_precut =
            eq_curve_precut(eq_curve_peak_of(global_settings.eq_custom));
    if (eq_bands_check(global_settings.eq_band_settings)
        || global_settings.eq_precut > 240)
        global_settings.eq_precut =
            eq_curve_precut(eq_curve_peak_of(global_settings.eq_band_settings));
}

/** the earphones' presets, from before **/

/* There once eq_phones_boot() has written them all, or found a file or folder
   of a name there instead: until then each boot tries again, so that one
   deleted meanwhile may come back. A file of its own in /.rockbox, not in eqs
   or the settings: neither a settings reset nor deleting eqs brings them
   back, and an update, whose zip has no such file, leaves it */
#define EQ_PHONES_MARK ROCKBOX_DIR "/eq-headphones-written"
static bool eq_phones_done;
/* this boot's eq_phones_boot() left one unwritten: told once, by the first
   eq_phones_move() after it */
static bool eq_phones_unwritten;

static void eq_phones_bands(int k, struct eq_band_setting *bands)
{
    memcpy(bands, eq_defaults, sizeof(eq_defaults));
    for (int i = 0; i < EQ_PHONES_BANDS; i++)
    {
        /* unused entries are band 0 with gain 0, which is the default anyway */
        if (eq_phones[k].bands[i].setting.gain != 0)
            bands[eq_phones[k].bands[i].band] = eq_phones[k].bands[i].setting;
    }
}

/* member by member: the enum is a byte on ARM, and the padding after it
   whatever a struct copy left there */
static bool eq_bands_equal(const struct eq_band_setting *a,
                           const struct eq_band_setting *b)
{
    for (int i = 0; i < EQ_NUM_BANDS; i++)
    {
        if (a[i].type != b[i].type || a[i].cutoff != b[i].cutoff
            || a[i].q != b[i].q || a[i].gain != b[i].gain)
            return false;
    }
    return true;
}

/* What its .cfg is, its path from /.rockbox/eqs into rel (EQ_PATH_MAX):
   EQ_PHONES_OWN, a preset of its bands, as a boot wrote it; EQ_PHONES_OTHER,
   another preset, as one of the user's; EQ_PHONES_BAD, a file that is no
   preset; or EQ_PHONES_NONE */
enum { EQ_PHONES_NONE, EQ_PHONES_BAD, EQ_PHONES_OTHER, EQ_PHONES_OWN };
static int eq_phones_file(int k, char *rel)
{
    struct eq_band_setting bands[EQ_NUM_BANDS], in_file[EQ_NUM_BANDS];
    int precut;
    snprintf(rel, EQ_PATH_MAX, "%s%s%s.cfg", eq_phones[k].folder,
             eq_phones[k].folder[0] ? "/" : "", eq_phones[k].name);
    if (!eq_files_load(rel, in_file, &precut))
        return eq_files_exists(rel) ? EQ_PHONES_BAD : EQ_PHONES_NONE;
    eq_phones_bands(k, bands);
    return eq_bands_equal(in_file, bands) ? EQ_PHONES_OWN : EQ_PHONES_OTHER;
}

void INIT_ATTR eq_phones_boot(void)
{
    eq_phones_done = file_exists(EQ_PHONES_MARK);
    if (eq_phones_done)
        return;
    struct eq_band_setting bands[EQ_NUM_BANDS];
    char rel[EQ_PATH_MAX];
    /* RockWheel 1.0 to 1.2.1 kept no eq_preset: the EQ on, at the bands of
       one of them, is that one, as 2.1.0's eq_preset_of_bands() named it */
    for (int k = 0; global_settings.eq_enabled && !global_settings.eq_preset
                    && k < (int)ARRAYLEN(eq_phones); k++)
    {
        eq_phones_bands(k, bands);
        if (eq_bands_equal(bands, global_settings.eq_band_settings))
            global_settings.eq_preset = eq_phones[k].id;
    }
    int failed = 0, err = 0;
    for (int k = 0; k < (int)ARRAYLEN(eq_phones); k++)
    {
        eq_phones_bands(k, bands);
        switch (eq_files_write_new(eq_phones[k].folder, eq_phones[k].name, bands))
        {
            case EQ_NEW_TAKEN:
                /* unless it's its own, from a boot that didn't get to the mark */
                if (eq_phones_file(k, rel) != EQ_PHONES_OWN)
                    health_note("eq: headphone preset %d not written, a file or "
                                "folder of its name is there", eq_phones[k].id);
                break;
            case EQ_NEW_FAILED:
                failed++;
                err = errno;
                break;
        }
    }
    if (failed)
    {
        eq_phones_unwritten = true;
        health_note("eq: %d of %d headphone presets not written, errno %d: tried "
                    "again at the next boot", failed, (int)ARRAYLEN(eq_phones), err);
        return;
    }
    const int fd = open(EQ_PHONES_MARK, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    eq_phones_done = fd >= 0 && close(fd) >= 0;
    if (!eq_phones_done)
        health_note("eq: headphone presets' mark not written, errno %d", errno);
}

/* The EQ on one of them by a setting from before: set to its file, if that
   has its bands, and on. Else Off: for good, eq_preset 0, once the file is
   another preset or no preset, or they are all in and its file gone; else
   until a boot has written it, eq_preset kept, and the boot that has sets it
   to the file and on again. Off with eq_preset 1 to 6 is that wait alone, as
   2.1.0's lists cleared eq_preset with the EQ (eq_set_preset()): any choice in
   the lists, Off too, or a settings reset ends the wait, the EQ staying as
   chosen */
static void eq_phones_move(void)
{
    const int id = global_settings.eq_preset;
    int k = 0;
    while (k < (int)ARRAYLEN(eq_phones) && eq_phones[k].id != id)
        k++;
    if (k == (int)ARRAYLEN(eq_phones))
        return;
    /* its path, which the EQ keeps only if it goes over to the file */
    const int file = eq_phones_file(k, global_settings.eq_preset_file);
    if (file == EQ_PHONES_OWN)
    {
        global_settings.eq_enabled = true;
        global_settings.eq_preset = EQ_ID_FILE;
        settings_save();
        return;
    }
    global_settings.eq_preset_file[0] = '\0';
    const bool waits = file == EQ_PHONES_NONE && !eq_phones_done;
    if (!waits)
        health_note("eq: the setting on headphone preset %d turned the EQ off: its "
                    "file %s", id, file == EQ_PHONES_OTHER ? "is another preset"
                    : file == EQ_PHONES_BAD ? "is no preset" : "isn't there");
    else if (eq_phones_unwritten)
        health_note("eq: the setting on headphone preset %d turned the EQ off until "
                    "a boot writes its file", id);
    eq_phones_unwritten = false;
    const bool changed = global_settings.eq_enabled || !waits;
    global_settings.eq_enabled = false;
    if (!waits)
        global_settings.eq_preset = 0;
    if (changed)
        settings_save();
}

void eq_update_preset(bool read_files)
{
    if (read_files)
    {
        eq_files_read();
        eq_phones_move();
    }
    eq_config_check();
    const int preset = eq_get_preset();
    if (preset <= 0)
        return;
    if (preset == EQ_CUSTOM)
    {
        memcpy(global_settings.eq_band_settings, global_settings.eq_custom,
               sizeof(global_settings.eq_band_settings));
        global_settings.eq_precut = global_settings.eq_custom_precut;
        return;
    }
    if (preset == EQ_FILE)
    {
        /* as it is in the file now, at boot and after USB, and otherwise as it
           was chosen; one gone, or no longer a preset, gives Off */
        int precut;
        if (!read_files)
            return;
        if (eq_files_load(global_settings.eq_preset_file,
                          global_settings.eq_band_settings, &precut))
        {
            global_settings.eq_precut = precut;
            /* its name as it's listed, should it now be in other capitals */
            const int entry = eq_files_find(global_settings.eq_preset_file);
            if (entry >= 0)
                eq_files_path(entry, global_settings.eq_preset_file,
                              sizeof(global_settings.eq_preset_file));
            return;
        }
        global_settings.eq_enabled = false;
        global_settings.eq_preset = 0;
        global_settings.eq_preset_file[0] = '\0';
        settings_save();
        return;
    }
    global_settings.eq_preset = eq_preset_id(preset);
    for (int i = 0; i < EQ_NUM_BANDS; i++)
        global_settings.eq_band_settings[i] = eq_preset_band(preset, i);
    global_settings.eq_precut = eq_preset_precut(preset);
}

/* The first list: Off, iTunes' presets as one row, then the user's own, Custom
   first. Start From in the editor's menu has the same rows without Off and
   Custom, and copies the one chosen into Custom. The user's presets: the folders
   of /.rockbox/eqs first, each a list of its own, then the presets, each by
   name, as Playlists and the files' lists have their folders first */
#define EQ_ITUNES_FIRST 1
#define EQ_ROW_ITUNES 1
#define EQ_ROW_CUSTOM 3
#define EQ_ROW_FIRST_MINE 4

static int eq_current; /* the preset the first list opened with, and chose since */
/* the user's preset it is, an entry of the table, or -1 (eq_level_build()) */
static int eq_active = -1;

/* what the rows do: choose the EQ, or copy into Custom (Start From) */
static enum { EQ_LIST_CHOOSE, EQ_LIST_START } eq_list_mode;

/* A row's name, with a check mark at its end (MENU_CHECK) for the EQ's active
   preset, in the lists that choose it; Start From's, which copy into Custom,
   have none */
static char *eq_marked(const char *name, bool active, char *buffer, size_t len)
{
    if (!active || eq_list_mode != EQ_LIST_CHOOSE)
        return (char *)name;
    snprintf(buffer, len, "%s\t%c", name, MENU_CHECK);
    return buffer;
}

/* A row's function's return after a rename or a delete of one of the user's
   presets: its list closes and opens again on eq_list_again, the row of the one
   renamed or the nearest to the one deleted, as do_menu() would build it again
   on the row it was on, now another's */
#define EQ_LIST_AGAIN 2
static int eq_list_again = -1;

/* Select held on a row of the user's presets. Only those have a menu of their
   own: the list's callback turns the hold into Select with this set, so that the
   row's function runs, which takes it first, and the list is built again after
   it, as a rename or a delete changes its rows */
static bool eq_held;

static bool eq_take_held(void)
{
    const bool held = eq_held;
    eq_held = false;
    return held;
}

/* A preset chosen in the lists: the EQ from now on, heard at once, and the
   list stays where it is, the cursor on its row, so that presets can be heard
   one after another; Menu goes back (0, as the menu takes it). From Start
   From, copied into Custom, back to the editor (1) */
static int eq_chosen(const struct eq_band_setting *bands, int precut, int id,
                     const char *file)
{
    if (eq_list_mode == EQ_LIST_START)
    {
        eq_custom_from(bands, precut);
        return 1;
    }
    eq_take(bands, precut, id, file);
    eq_current = eq_get_preset();
    eq_active = eq_files_find(global_settings.eq_preset_file);
    return 0;
}

/* the same for Off or a built-in preset */
static int eq_chosen_preset(int preset)
{
    if (eq_list_mode == EQ_LIST_START)
    {
        eq_custom_from_preset(preset);
        return 1;
    }
    eq_set_preset(preset);
    eq_current = eq_get_preset();
    eq_active = -1;
    return 0;
}

/** the user's presets in lists of folders **/

/* A level of the lists: the first list's or Start From's rows of the top
   folder, at base, or a folder's list of its own */
struct eq_level {
    int folder;          /* its id, EQ_TOP */
    int folders, presets;
    int base;            /* the row of its first folder in its list */
};
/* each of the two lists' own, as Start From opens over the first one */
static struct eq_nav {
    struct eq_level level[EQ_DEPTH_MAX + 1];
    int depth;           /* the level shown */
    char title[EQ_DEPTH_MAX + 1][EQ_NAME_MAX];
    struct menu_item_ex menu[EQ_DEPTH_MAX + 1];
    struct menu_callback_with_desc desc[EQ_DEPTH_MAX + 1];
    const struct menu_item_ex *top[EQ_ROW_FIRST_MINE + EQ_FILES_MAX];
} eq_navs[2];
#define NAV (&eq_navs[eq_list_mode])
_Static_assert(ARRAYLEN(eq_navs[0].top) <= MENU_MAX_ROWS,
               "the first list's rows, which do_menu() would cut");

/* the row that ran, as the list had it selected: every folder's row is one
   item, and every preset's another, as many times as there are rows */
static int eq_row;
static int eq_entry_callback(int action, const struct menu_item_ex *this_item,
                             struct gui_synclist *this_list)
{
    (void)this_item;
    if (action == ACTION_ENTER_MENUITEM && this_list)
        eq_row = gui_synclist_get_sel_pos(this_list);
    return action;
}

/* the entry a row of the level shown is, -1 for none */
static int eq_row_entry(int row, bool *folder)
{
    const struct eq_level *l = &NAV->level[NAV->depth];
    row -= l->base;
    *folder = row < l->folders;
    if (row < 0 || row >= l->folders + l->presets)
        return -1;
    return eq_files_child(l->folder, *folder, *folder ? row : row - l->folders);
}

static char *eq_entry_name(int selected_item, void *data, char *buffer, size_t len)
{
    (void)data;
    bool folder;
    const int entry = eq_row_entry(selected_item, &folder);
    /* a folder holding the active preset shows nothing more */
    return eq_marked(eq_files_name(entry), !folder && entry >= 0 && entry == eq_active,
                     buffer, len);
}

static int eq_open_folder(void);
static int eq_choose_entry(void);
/* a folder's leads on; a preset's sets it, and has no arrow */
MENUITEM_FUNCTION_DYNTEXT(eq_folder_row, MENU_FUNC_CHECK_RETVAL, eq_open_folder,
                          eq_entry_name, NULL, NULL, eq_entry_callback, Icon_NOICON);
MENUITEM_FUNCTION_DYNTEXT(eq_entry_row, MENU_FUNC_CHECK_RETVAL|MENU_EXITAFTERTHISMENU,
                          eq_choose_entry, eq_entry_name, NULL, NULL, eq_entry_callback,
                          Icon_NOICON);
/* a level's rows: its folders' from the end of the first half, then its
   presets' */
static const struct menu_item_ex *eq_level_rows[2 * EQ_FILES_MAX];

/* the lists of the user's presets: Select held on a preset's row is its menu */
static int eq_level_callback(int action, const struct menu_item_ex *this_item,
                             struct gui_synclist *this_list)
{
    (void)this_item;
    if (action == ACTION_STD_CONTEXT && eq_list_mode == EQ_LIST_CHOOSE && this_list)
    {
        bool folder;
        if (eq_row_entry(gui_synclist_get_sel_pos(this_list), &folder) >= 0 && !folder)
        {
            eq_held = true;
            return ACTION_STD_OK;
        }
    }
    return action;
}

static void eq_level_count(struct eq_level *l, int folder, int base)
{
    l->folder = folder;
    l->folders = eq_files_children(folder, true);
    l->presets = eq_files_children(folder, false);
    l->base = base;
}

/* a folder's list as the table has it now: false if nothing's left in it */
static bool eq_level_build(struct eq_nav *nav, int depth)
{
    /* as the table may have changed since: a rename, a delete */
    eq_active = eq_current == EQ_FILE
                ? eq_files_find(global_settings.eq_preset_file) : -1;
    if (!eq_level_rows[0])
    {
        for (int i = 0; i < EQ_FILES_MAX; i++)
        {
            eq_level_rows[i] = &eq_folder_row;
            eq_level_rows[EQ_FILES_MAX + i] = &eq_entry_row;
        }
    }
    struct eq_level *l = &nav->level[depth];
    eq_level_count(l, l->folder, 0);
    const int rows = l->folders + l->presets;
    nav->desc[depth] = (struct menu_callback_with_desc){
        eq_level_callback, (unsigned char *)nav->title[depth], Icon_NOICON };
    struct menu_item_ex *m = &nav->menu[depth];
    m->flags = MT_MENU | MENU_HAS_DESC | MENU_ITEM_COUNT(rows);
    m->submenus = &eq_level_rows[EQ_FILES_MAX - l->folders];
    m->callback_and_desc = &nav->desc[depth];
    return rows > 0;
}

static int eq_choose_off(void);
static int eq_choose_itunes(void);
static int eq_choose_custom(void);
static char *eq_itunes_row_name(int selected_item, void *data,
                                char *buffer, size_t buffer_len);

static char *eq_off_row_name(int selected_item, void *data, char *buffer,
                             size_t len)
{
    (void)selected_item; (void)data;
    return eq_marked(str(LANG_OFF), !global_settings.eq_enabled, buffer, len);
}
static char *eq_custom_row_name(int selected_item, void *data, char *buffer,
                                size_t len)
{
    (void)selected_item; (void)data;
    return eq_marked(str(LANG_EQ_CUSTOM), eq_current == EQ_CUSTOM, buffer, len);
}

MENUITEM_FUNCTION_DYNTEXT(eq_off_row, MENU_FUNC_CHECK_RETVAL|MENU_EXITAFTERTHISMENU,
                          eq_choose_off, eq_off_row_name, NULL, NULL, NULL, Icon_NOICON);
MENUITEM_FUNCTION_DYNTEXT(eq_itunes_row, MENU_FUNC_CHECK_RETVAL, eq_choose_itunes,
                          eq_itunes_row_name, NULL, NULL, NULL, Icon_NOICON);
MENUITEM_GROUP(eq_mine_group, ID2P(LANG_EQ_MY_PRESETS));
/* opens the editor: a row that leads on */
MENUITEM_FUNCTION_DYNTEXT(eq_custom_row, MENU_FUNC_CHECK_RETVAL, eq_choose_custom,
                          eq_custom_row_name, NULL, NULL, NULL, Icon_NOICON);

/* the first list's or Start From's rows, with the top folder's as it is now */
static void eq_top_build(struct eq_nav *nav, bool start)
{
    eq_level_build(nav, 0);   /* the rows' items made */
    const struct menu_item_ex **r = nav->top;
    int n = 0;
    if (!start)
        r[n++] = &eq_off_row;
    r[n++] = &eq_itunes_row;
    struct eq_level *l = &nav->level[0];
    const int folders = eq_files_children(EQ_TOP, true);
    const int presets = eq_files_children(EQ_TOP, false);
    /* My Presets: Custom always in the first list; in Start From only the
       user's own, and none without them */
    if (!start || folders + presets)
        r[n++] = &eq_mine_group;
    if (!start)
        r[n++] = &eq_custom_row;
    eq_level_count(l, EQ_TOP, n);
    for (int i = 0; i < folders; i++)
        r[n++] = &eq_folder_row;
    for (int i = 0; i < presets; i++)
        r[n++] = &eq_entry_row;
    nav->desc[0] = (struct menu_callback_with_desc){
        eq_level_callback, start ? ID2P(LANG_EQ_START_FROM) : ID2P(LANG_EQUALIZER), Icon_EQ };
    struct menu_item_ex *m = &nav->menu[0];
    m->flags = MT_MENU | MENU_HAS_DESC | MENU_ITEM_COUNT(n);
    m->submenus = nav->top;
    m->callback_and_desc = &nav->desc[0];
}

/* the row of the level that leads to the chosen preset, or is it; -1 */
static int eq_chosen_row(const struct eq_nav *nav, int depth)
{
    if (eq_current != EQ_FILE)
        return -1;
    const int entry = eq_files_ancestor(eq_files_find(global_settings.eq_preset_file),
                                        depth);
    const struct eq_level *l = &nav->level[depth];
    if (entry < 0 || eq_files_folder_of(entry) != l->folder)
        return -1;
    bool folder = eq_files_children(l->folder, true)
                  && eq_files_child(l->folder, true, eq_files_child_pos(entry)) == entry;
    return l->base + (folder ? 0 : l->folders) + eq_files_child_pos(entry);
}

/* after a change in the level's rows: the nearest row to one at pos among its
   presets, a folder's if none is left */
static int eq_nearest_row(const struct eq_level *l, int pos)
{
    if (l->presets)
        return l->base + l->folders + MIN(pos, l->presets - 1);
    if (l->folders)
        return l->base + l->folders - 1;
    return l == &eq_navs[EQ_LIST_CHOOSE].level[0] ? EQ_ROW_CUSTOM : l->base;
}

/* the level of the depth, its list shown again on the row of the entry at
   that path or the nearest to pos */
static void eq_again_on(int depth, const char *rel, int pos)
{
    struct eq_nav *nav = NAV;
    struct eq_level *l = &nav->level[depth];
    eq_level_count(l, l->folder, l->base);
    const int entry = rel ? eq_files_find(rel) : -1;
    eq_list_again = entry >= 0 && eq_files_folder_of(entry) == l->folder
        ? l->base + l->folders + eq_files_child_pos(entry) : eq_nearest_row(l, pos);
}

static int eq_open_folder(void)
{
    struct eq_nav *nav = NAV;
    bool folder;
    const int entry = eq_row_entry(eq_row, &folder);
    if (eq_take_held() || entry < 0 || !folder || nav->depth >= EQ_DEPTH_MAX)
        return 0;
    const int d = ++nav->depth;
    nav->level[d].folder = eq_files_folder_id(entry);
    strmemccpy(nav->title[d], eq_files_name(entry), EQ_NAME_MAX);
    const int pos = eq_files_child_pos(entry);
    int ret = 0;
    bool emptied = false;
    eq_level_build(nav, d);
    int selected = MAX(eq_chosen_row(nav, d), 0);
    transition_begin(TRANSITION_FORWARD);
    while (true)
    {
        if (!eq_level_build(nav, d))
        {
            emptied = true;
            break;
        }
        ret = do_menu(&nav->menu[d], &selected, NULL, false);
        if (ret != EQ_LIST_AGAIN)
            break;
        selected = eq_list_again;
        eq_list_again = -1;
    }
    nav->depth--;
    /* its list's rows again, as anything opened from here may have changed
       them: a preset saved at the top from the editor, this folder gone */
    if (d == 1)
        eq_top_build(nav, eq_list_mode == EQ_LIST_START);
    else
        eq_level_build(nav, d - 1);
    if (emptied)
    {
        struct eq_level *l = &nav->level[d - 1];
        eq_list_again = l->folders ? l->base + MIN(pos, l->folders - 1)
                                   : eq_nearest_row(l, 0);
        return EQ_LIST_AGAIN;
    }
    return ret == GO_TO_PREVIOUS ? 0 : ret;
}

/** a preset's menu, Select held on its row **/

/* the one whose menu is open, and its name as the menu's title */
static int eq_file_at;
static char eq_file_title[EQ_NAME_MAX];
/* a name being typed, the folder it's for and the entry it was */
static char eq_name_buf[EQ_NAME_MAX];
static int eq_name_folder = EQ_TOP, eq_name_except = -1;
/* a path, out of the stack */
static char eq_path[EQ_PATH_MAX];

static void eq_failed(void)
{
    splash(HZ * 2, ID2P(LANG_FAILED));
}

/* without the spaces and dots at its start, which would hide its file */
static const char *eq_name_start(const char *name)
{
    while (*name == ' ' || *name == '.')
        name++;
    return name;
}

/* as it will be saved */
static bool eq_name_taken(const char *name)
{
    return eq_files_taken(eq_name_folder, eq_name_start(name), eq_name_except);
}

/* A name typed with Search's ring, as a playlist's is: 1, 0 if given up, -1 for
   USB. Taken is asked of it as it will be saved, from its first letter or digit */
static int eq_type_name(const char *title, bool (*taken)(const char *name))
{
    int ret;
    do
    {
#ifdef HAVE_TAGCACHE
        ret = music_search_name(title, eq_name_buf, sizeof(eq_name_buf),
                                sizeof(eq_name_buf) - 1, taken);
#else
        (void)title; (void)taken;
        ret = kbd_input(eq_name_buf, sizeof(eq_name_buf), NULL) < 0 ? 0 : 1;
#endif
        const char *start = eq_name_start(eq_name_buf);
        memmove(eq_name_buf, start, strlen(start) + 1);
    } while (ret == 1 && !eq_name_buf[0]);
    return ret;
}

/* a function of a menu, as the menu takes it: closed and back with the slide */
static int eq_menu_result(int ret)
{
    if (ret < 0)
        return ret == GO_TO_ROOT || ret == MENU_ATTACHED_USB
               || ret == GO_TO_PREVIOUS_MUSIC ? ret : 0;
    if (ret > 0)
        transition_begin(TRANSITION_BACK);
    return ret;
}

/* the preset Edit opened, kept apart from eq_path, which Start From in the
   editor's menu uses as it reads a preset */
static char eq_edited[EQ_PATH_MAX];

static int eq_file_edit(void)
{
    struct eq_band_setting bands[EQ_NUM_BANDS];
    int precut;
    if (!eq_files_path(eq_file_at, eq_edited, sizeof(eq_edited))
        || !eq_files_load(eq_edited, bands, &precut))
    {
        eq_failed();
        return 0;
    }
    eq_sought = false;
    eq_custom_from(bands, precut);
    eq_current = EQ_CUSTOM;
    transition_begin(TRANSITION_FORWARD);
    const int ret = eq_editor(eq_sought);
    /* on its row again: Save Preset may have added one before it */
    eq_again_on(NAV->depth, eq_edited, 0);
    return ret ? ret : 1;
}

static int eq_file_rename(void)
{
    const int at = eq_file_at;
    strmemccpy(eq_name_buf, eq_files_name(at), sizeof(eq_name_buf));
    eq_name_folder = eq_files_folder_of(at);
    eq_name_except = at;
    const int typed = eq_type_name(str(LANG_RENAME), eq_name_taken);
    eq_name_except = -1;
    if (typed <= 0)
        return typed < 0 ? MENU_ATTACHED_USB : 0;
    if (!strcmp(eq_name_buf, eq_files_name(at)))
        return eq_menu_result(1);
    if (!eq_files_path(at, eq_path, sizeof(eq_path)))
        return eq_menu_result(1);
    const bool chosen = eq_get_preset() == EQ_FILE
        && !strcasecmp(global_settings.eq_preset_file, eq_path);
    /* asked again just before the rename, of the name as it's saved, with the
       spaces and dots at its start gone */
    if (eq_files_taken(eq_name_folder, eq_name_buf, at))
    {
        splash(HZ, ID2P(LANG_NAME_TAKEN));
        return 0;
    }
    const int now = eq_files_rename(at, eq_name_buf);
    if (now < 0)
    {
        eq_failed();
        return eq_menu_result(1);
    }
    if (eq_files_path(now, eq_path, sizeof(eq_path)))
    {
        if (chosen)
        {
            strmemccpy(global_settings.eq_preset_file, eq_path,
                       sizeof(global_settings.eq_preset_file));
            settings_save();
        }
        /* where its new name sorts it */
        eq_again_on(NAV->depth, eq_path, 0);
    }
    return eq_menu_result(1);
}

static int eq_file_delete(void)
{
    const int at = eq_file_at;
    /* as Playlists' Delete asks, Cancel first */
    const struct opt_items items[] = {
        { STR(LANG_DELETE) }, { STR(LANG_CANCEL_0) },
    };
    int choice = 1;
    if (set_option(eq_files_name(at), &choice, RB_INT, items, ARRAYLEN(items), NULL))
        return MENU_ATTACHED_USB;
    if (choice != 0)
        return 0;
    const bool chosen = eq_get_preset() == EQ_FILE
        && eq_files_path(at, eq_path, sizeof(eq_path))
        && !strcasecmp(global_settings.eq_preset_file, eq_path);
    const int pos = eq_files_child_pos(at);
    if (!eq_files_delete(at))
    {
        eq_failed();
        return eq_menu_result(1);
    }
    /* the EQ of one gone is Off, as after a boot without its file */
    if (chosen)
        eq_set_preset(0);
    /* the one after it, which took its place, or the one before */
    eq_again_on(NAV->depth, NULL, pos);
    return eq_menu_result(1);
}

MENUITEM_FUNCTION(eq_file_edit_row, MENU_FUNC_CHECK_RETVAL, ID2P(LANG_EDIT),
                  eq_file_edit, NULL, Icon_NOICON);
MENUITEM_FUNCTION(eq_file_rename_row, MENU_FUNC_CHECK_RETVAL|MENU_EXITAFTERTHISMENU,
                  ID2P(LANG_RENAME), eq_file_rename, NULL, Icon_NOICON);
MENUITEM_FUNCTION(eq_file_delete_row, MENU_FUNC_CHECK_RETVAL|MENU_EXITAFTERTHISMENU,
                  ID2P(LANG_DELETE), eq_file_delete, NULL, Icon_NOICON);
MAKE_MENU(eq_file_menu, eq_file_title, NULL, Icon_NOICON,
          &eq_file_edit_row, &eq_file_rename_row, &eq_file_delete_row);

/* a preset's row: set it, its menu for Select held, copied into Custom from
   Start From. Its bands are read from its file now */
static int eq_choose_entry(void)
{
    bool folder;
    const int entry = eq_row_entry(eq_row, &folder);
    if (entry < 0 || folder)
    {
        eq_take_held();
        return 0;
    }
    if (eq_take_held())
    {
        eq_file_at = entry;
        strmemccpy(eq_file_title, eq_files_name(entry), sizeof(eq_file_title));
        eq_list_again = -1;
        transition_begin(TRANSITION_FORWARD);
        const int ret = do_menu(&eq_file_menu, NULL, NULL, false);
        if (ret == GO_TO_ROOT || ret == MENU_ATTACHED_USB || ret == GO_TO_PREVIOUS_MUSIC)
            return ret;
        return eq_list_again >= 0 ? EQ_LIST_AGAIN : 0;
    }
    struct eq_band_setting bands[EQ_NUM_BANDS];
    int precut;
    if (!eq_files_path(entry, eq_path, sizeof(eq_path))
        || !eq_files_load(eq_path, bands, &precut))
    {
        eq_failed();
        return 0;
    }
    return eq_chosen(bands, precut, EQ_ID_FILE, eq_path);
}

/** the other rows **/

/* the list of some presets that's on the screen */
static int eq_list_first;
static const unsigned char **eq_list_names;
static bool eq_list_chosen;

static const char *eq_list_name(int selected_item, void *data, char *buffer,
                                size_t len)
{
    (void)data;
    return eq_marked(P2STR(eq_list_names[selected_item]),
                     eq_current == eq_list_first + selected_item, buffer, len);
}

/* Select, or held, chooses as in the first list (eq_chosen_preset()): the
   list stays, on its row, or from Start From it closes, back to the editor.
   Play held goes on to Now Playing, as in the first list (do_menu()) */
static int eq_list_action(int action, struct gui_synclist *lists)
{
    /* its rows drawn as a menu's, for the check mark (simplelist_show_list()
       asks this first, before it draws) */
    lists->callback_draw_item = menu_draw_value_line;
    if (action == ACTION_STD_OK || action == ACTION_STD_CONTEXT)
    {
        if (eq_chosen_preset(eq_list_first + gui_synclist_get_sel_pos(lists)))
        {
            eq_list_chosen = true;
            return ACTION_STD_CANCEL;
        }
        return ACTION_REDRAW;
    }
    if (action == ACTION_TREE_STOP && now_playing_available())
        return ACTION_TREE_WPS;
    return action;
}

/* A list of some presets, its own: 1 if one was copied into Custom from Start
   From, GO_TO_PREVIOUS_MUSIC for Now Playing (Play held, or Play with no music
   loaded, which otherwise pauses or plays on), MENU_ATTACHED_USB, else 0 for
   Menu, back to the first list with the EQ as it was last chosen */
static int eq_choose_from(int lang_id, int first, int count, const unsigned char **names)
{
    struct simplelist_info info;
    simplelist_info_init(&info, str(lang_id), count, NULL);
    info.get_name = eq_list_name;
    info.action_callback = eq_list_action;
    info.title_icon = Icon_Questionmark;
    info.timeout = HZ; /* as option_screen() and do_menu() wait, not 10 times a second */
    info.selection = (eq_current >= first && eq_current < first + count)
                     ? eq_current - first : 0;
    eq_list_first = first;
    eq_list_names = names;
    eq_list_chosen = false;
    if (simplelist_show_list(&info))
        return MENU_ATTACHED_USB;
    if (eq_list_chosen)
        return 1;
    return info.selection == -2 ? GO_TO_PREVIOUS_MUSIC : 0;
}

static int eq_choose_off(void)
{
    if (eq_take_held())
        return 0;
    return eq_chosen_preset(0);
}

static int eq_choose_itunes(void)
{
    if (eq_take_held())
        return 0;
    static const unsigned char *names[ARRAYLEN(eq_presets)];
    for (int i = 0; i < (int)ARRAYLEN(eq_presets); i++)
        names[i] = ID2P(eq_presets[i].lang_id);
    return eq_choose_from(LANG_EQ_GENERAL, EQ_ITUNES_FIRST, ARRAYLEN(names), names);
}

/* Custom, the EQ from now on, in the editor; back to the list after it */
static int eq_choose_custom(void)
{
    if (eq_take_held())
        return 0;
    eq_sought = false;
    eq_set_custom();
    eq_current = EQ_CUSTOM;
    transition_begin(TRANSITION_FORWARD);
    const int ret = eq_editor(eq_sought);
    /* the top's rows again: Save Preset may have added one */
    eq_top_build(&eq_navs[EQ_LIST_CHOOSE], false);
    return ret;
}

static char *eq_itunes_row_name(int selected_item, void *data,
                                char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    if (eq_current < EQ_ITUNES_FIRST || eq_current > EQ_PRESETS)
        return str(LANG_EQ_GENERAL);
    snprintf(buffer, buffer_len, "%s\t%s", str(LANG_EQ_GENERAL),
             str(eq_presets[eq_current - EQ_ITUNES_FIRST].lang_id));
    return buffer;
}

/** the editor's menu, Select held in it **/

/* Custom kept as a preset of the user's at the top: a free "Preset N" offered;
   a name taken asks whether to replace that one */
static int eq_save_custom(void)
{
    eq_name_folder = EQ_TOP;
    eq_name_except = -1;
    for (int n = 1; n < 1000; n++)
    {
        snprintf(eq_name_buf, sizeof(eq_name_buf), str(LANG_EQ_PRESET_N), n);
        if (!eq_files_taken(EQ_TOP, eq_name_buf, -1))
            break;
    }
    while (true)
    {
        const int typed = eq_type_name(str(LANG_EQ_SAVE_PRESET), NULL);
        if (typed <= 0)
            return typed < 0 ? MENU_ATTACHED_USB : 0;
        if (eq_files_taken(EQ_TOP, eq_name_buf, -1))
        {
            /* as Delete asks, under the name, Cancel first; Cancel or Menu
               goes back to the name */
            const struct opt_items items[] = {
                { STR(LANG_EQ_REPLACE) }, { STR(LANG_CANCEL_0) },
            };
            int choice = 1;
            if (set_option(eq_name_buf, &choice, RB_INT, items, ARRAYLEN(items), NULL))
                return MENU_ATTACHED_USB;
            if (choice != 0)
                continue;
        }
        if (!eq_files_save(eq_name_buf, global_settings.eq_custom,
                           global_settings.eq_custom_precut))
            eq_failed();
        return eq_menu_result(1);
    }
}

static int eq_reset_custom(void)
{
    eq_custom_from(eq_defaults, 0);
    return eq_menu_result(1);
}

MENUITEM_FUNCTION(eq_save_row, MENU_FUNC_CHECK_RETVAL|MENU_EXITAFTERTHISMENU,
                  ID2P(LANG_EQ_SAVE_PRESET_DOTS), eq_save_custom, NULL, Icon_NOICON);
MENUITEM_FUNCTION(eq_flat_row, MENU_FUNC_CHECK_RETVAL|MENU_EXITAFTERTHISMENU,
                  ID2P(LANG_EQ_RESET_FLAT), eq_reset_custom, NULL, Icon_NOICON);
/* Start From: Start From's list of the user's presets, built as it opens */
MAKE_MENU(eq_editor_menu_list, ID2P(LANG_EQ_CUSTOM), NULL, Icon_EQ,
          &eq_save_row, &eq_navs[EQ_LIST_START].menu[0], &eq_flat_row);

int eq_editor_menu(void)
{
    const int was = eq_current;
    eq_files_sort();
    eq_list_mode = EQ_LIST_START;
    eq_current = EQ_CUSTOM;
    eq_navs[EQ_LIST_START].depth = 0;
    eq_top_build(&eq_navs[EQ_LIST_START], true);
    transition_begin(TRANSITION_FORWARD);
    int ret = do_menu(&eq_editor_menu_list, NULL, NULL, false);
    eq_list_mode = EQ_LIST_CHOOSE;
    eq_current = was;
    if (ret > 0)
        transition_begin(TRANSITION_BACK);
    return ret == GO_TO_ROOT || ret == MENU_ATTACHED_USB
           || ret == GO_TO_PREVIOUS_MUSIC ? ret : 0;
}

static int eq_choose_preset(void)
{
    struct eq_nav *nav = &eq_navs[EQ_LIST_CHOOSE];
    eq_files_sort();
    eq_list_mode = EQ_LIST_CHOOSE;
    eq_held = false;
    eq_list_again = -1;
    nav->depth = 0;
    eq_current = eq_get_preset();
    eq_top_build(nav, false);
    int selected = 0;
    if (eq_current == EQ_CUSTOM)
        selected = EQ_ROW_CUSTOM;
    else if (eq_current == EQ_FILE)
    {
        selected = eq_chosen_row(nav, 0);
        if (selected < 0)
            selected = EQ_ROW_CUSTOM;
    }
    else if (eq_current >= EQ_ITUNES_FIRST)
        selected = EQ_ROW_ITUNES;
    int ret;
    while ((ret = do_menu(&nav->menu[0], &selected, NULL, false)) == EQ_LIST_AGAIN)
    {
        selected = eq_list_again;
        eq_list_again = -1;
        eq_current = eq_get_preset();
        eq_top_build(nav, false);
    }
    /* Menu held goes on to the main menu, past the menu this one was opened
       from; Play held to Now Playing, from the editor's menu too */
    if (ret == GO_TO_ROOT)
        menu_request_root();
    return ret == GO_TO_PREVIOUS_MUSIC ? ret : 0;
}

/* the name of the preset, Off, or NULL if on and set to none */
static char *eq_setting_name(void)
{
    static char shown[EQ_NAME_MAX];
    const int preset = eq_get_preset();
    if (!preset && global_settings.eq_enabled)
        return NULL;
    if (preset == EQ_CUSTOM)
        return str(LANG_EQ_CUSTOM);
    if (preset == EQ_FILE)
    {
        eq_files_shown_name(global_settings.eq_preset_file, shown, sizeof(shown));
        return shown;
    }
    return str(!preset ? LANG_OFF : eq_presets[preset - 1].lang_id);
}

/* in the settings list, and in the menu over Now Playing */
static char *eq_row_name(int selected_item, void *data,
                         char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    char *name = eq_setting_name();
    if (!name)
        return str(LANG_EQUALIZER);
    snprintf(buffer, buffer_len, "%s\t%s", str(LANG_EQUALIZER), name);
    return buffer;
}

MENUITEM_FUNCTION_DYNTEXT(ipod_eq_menu, MENU_EXITAFTERTHISMENU|MENU_FUNC_OPENS_SCREEN,
                          eq_choose_preset, eq_row_name,
                          NULL, NULL, lowlatency_callback, Icon_EQ);

/* Play held in its lists goes on to Now Playing (eq_choose_preset()) */
MENUITEM_FUNCTION_DYNTEXT(ipod_eq_row, MENU_FUNC_CHECK_RETVAL, eq_choose_preset,
                          eq_row_name, NULL, NULL, lowlatency_callback, Icon_EQ);
