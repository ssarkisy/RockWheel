/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 Robert E. Hak
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
/*
2005 Kevin Ferrare :
 - Multi screen support
 - Rewrote/removed a lot of code now useless with the new gui API
*/
#include <stdbool.h>
#include <stdlib.h>
#include "config.h"
#include "system.h"

#include "appevents.h"
#include "lcd.h"
#include "font.h"
#include "file.h"
#include "menu.h"
#include "onplay.h"
#include "transition.h"
#include "button.h"
#include "kernel.h"
#include "debug.h"
#include "usb.h"
#include "panic.h"
#include "settings.h"
#include "gui/ui_theme.h"
#include "settings_list.h"
#include "option_select.h"
#include "screens.h"
#include "talk.h"
#include "lang.h"
#include "misc.h"
#include "action.h"
#include "gui/skin_engine/skin_engine.h"
#include "gui/skin_engine/menu_cover.h"
#include "menus/exported_menus.h"
#include "string-extra.h"
#include "root_menu.h"
#include "audio.h"
#include "sound.h"
#include "viewport.h"
#include "quickscreen.h"
#include "shortcuts.h"
#include "statusbar-skinned.h"

#include "icons.h"

/* gui api */
#include "list.h"

#define MAX_MENUS 8
/* used to allow for dynamic menus */
#define MAX_MENU_SUBITEMS 64
static struct menu_item_ex *current_submenus_menu;
static int current_subitems[MAX_MENU_SUBITEMS];
static int current_subitems_count = 0;
static int talk_menu_item(int selected_item, void *data);

struct menu_data_t
{
    const struct menu_item_ex *menu;
    int selected;
};

static int empty_menu_callback(int action, const struct menu_item_ex *this_item, struct gui_synclist *this_list)
{
    return action;
    (void)this_item;
    (void)this_list;
}

static void get_menu_callback(const struct menu_item_ex *m,
                        menu_callback_type *menu_callback)
{
    if (m->flags&(MENU_HAS_DESC|MENU_DYNAMIC_DESC))
        *menu_callback= m->callback_and_desc->menu_callback;
    else
        *menu_callback = m->menu_callback;

    if (!*menu_callback)
        *menu_callback = &empty_menu_callback;
}

static bool query_audio_status(int *old_audio_status)
{
    bool redraw_list = false;
    /* query audio status to see if it changed */
    int new_audio_status = audio_status();
    if (*old_audio_status != new_audio_status)
    {  /* force a redraw if anything changed the audio status from outside */
        *old_audio_status = new_audio_status;
        redraw_list = true;
    }
    return redraw_list;
}

static int get_menu_selection(int selected_item, const struct menu_item_ex *menu)
{
    int type = (menu->flags&MENU_TYPE_MASK);
    if ((type == MT_MENU || type == MT_RETURN_ID)
        && (selected_item<current_subitems_count))
        return current_subitems[selected_item];
    return selected_item;
}

/* The settings list shows what each setting is set to, and one with few values
   changes right there when selected, as on a stock iPod; the rest open their
   screen */
#define IN_PLACE_CHOICES 4
#define HASFLAG(setting, flag) (((setting)->flags & (flag)) == (flag))

extern const struct menu_item_ex main_menu_, settings_sound, settings_playback,
                                 settings_general;

/* Settings and what opens from it, and any list as wide: the menu over Now Playing */
static bool menu_shows_values(const struct menu_item_ex *menu)
{
    return menu_wide() || menu == &main_menu_ || menu == &ipod_keyclick_menu
           || menu == &night_menu || menu == &appearance_menu
           || menu == &settings_sound || menu == &settings_playback
           || menu == &settings_general
           || menu == &wps_menu_items
#ifdef HAVE_TAGCACHE
           || menu == &music_menu_items || menu == &main_menu_items
#endif
           ;
}

/* The hints under Settings' folders: of the settings not plain from their
   names, by their variables. Only in the wide lists, where three lines fit. */
static const struct {
    const void *variable;
    int lang;
} hints[] = {
    { &global_settings.resume_rewind, LANG_HINT_RESUME_REWIND },
#ifdef HAVE_HEADPHONE_DETECTION
    { &global_settings.unplug_mode, LANG_HINT_HEADPHONE_UNPLUG },
#endif
    { &global_settings.cuesheet, LANG_HINT_CUESHEET },
    { &global_settings.crossfeed, LANG_HINT_CROSSFEED },
    { &global_settings.playlist_shuffle, LANG_HINT_SHUFFLE },
#ifdef HAVE_CROSSFADE
    { &global_settings.crossfade, LANG_HINT_CROSSFADE },
#endif
#ifdef HAVE_PITCHCONTROL
    { &global_settings.podcast_speed, LANG_HINT_PODCAST_SPEED },
    { &global_settings.audiobook_speed, LANG_HINT_AUDIOBOOK_SPEED },
#endif
#ifdef HAVE_TAGCACHE
    { &global_settings.album_artists, LANG_HINT_ARTISTS_IN_ALBUMS },
#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
    { &global_settings.list_covers, LANG_HINT_COVERS_IN_LISTS },
#endif
#endif
#if defined(HAVE_ALBUMART) && (LCD_DEPTH >= 16)
    { &global_settings.cover_colours, LANG_HINT_COVER_COLOURS },
    { &global_settings.status_icons, LANG_HINT_STATUS_ICONS },
    { &global_settings.angled_cover, LANG_HINT_ANGLED_COVER },
#endif
#ifdef HAVE_MENU_COVER
    { &global_settings.moving_cover, LANG_HINT_MOVING_COVER },
    { &global_settings.theme_colour, LANG_HINT_THEME_COLOUR },
#endif
#ifdef HAVE_BACKLIGHT
    { &global_settings.wake_screen, LANG_HINT_WAKE_SCREEN },
    { &global_settings.return_to_wps, LANG_HINT_RETURN_TO_WPS },
#endif
    { &global_settings.coverflow_pause, LANG_HINT_COVER_FLOW_PAUSE },
#ifdef HAVE_TAGCACHE
    { &global_settings.smart_search, LANG_HINT_SMART_SEARCH },
#endif
};

static const char *item_hint(const struct menu_item_ex *item)
{
    if ((item->flags & MENU_TYPE_MASK) != MT_SETTING)
        return NULL;
    for (unsigned i = 0; i < ARRAYLEN(hints); i++)
    {
        if (hints[i].variable == item->variable)
            return str(hints[i].lang);
    }
    return NULL;
}

static const char *menu_hint(int selected, void *data)
{
    const struct menu_item_ex *menu = data;
    if (selected < 0 || selected >= current_subitems_count)
        return NULL;
    return item_hint(menu->submenus[get_menu_selection(selected, menu)]);
}

/* a menu with any: its band then, for all its rows, which don't move */
static bool menu_has_hints(const struct menu_item_ex *menu)
{
    if ((menu->flags & MENU_TYPE_MASK) != MT_MENU)
        return false;
    for (int i = 0; i < (int)MENU_GET_COUNT(menu->flags); i++)
    {
        if (item_hint(menu->submenus[i]))
            return true;
    }
    return false;
}

static bool setting_changes_in_place(const struct settings_list *setting)
{
    return setting && (HASFLAG(setting, F_BOOL_SETTING)
                       || (HASFLAG(setting, F_CHOICE_SETTING)
                           && setting->choice_setting->count <= IN_PLACE_CHOICES));
}

static const char *setting_value(const struct settings_list *setting,
                                 char *buffer, size_t buffer_len)
{
    if (HASFLAG(setting, F_BOOL_SETTING))
        return str(*(bool *)setting->setting ? LANG_ON : LANG_OFF);
    if (HASFLAG(setting, F_CUSTOM_SETTING)) /* the volume limit */
        return format_volume_step(buffer, buffer_len, *(int *)setting->setting);
    return option_get_valuestring(setting, buffer, buffer_len,
                                  option_value_as_int(setting));
}

/* A setting that is one of On and Off, changed in place: whether it is on,
   -1 for another kind, as a setting of two other words (Inverse, Normal). A
   choice of those two words is one too, as Sound Check */
static int setting_switch(const struct settings_list *setting)
{
    if (HASFLAG(setting, F_BOOL_SETTING))
        return setting->bool_setting->lang_yes == LANG_SET_BOOL_YES
               && setting->bool_setting->lang_no == LANG_SET_BOOL_NO ?
               *(bool *)setting->setting : -1;
    if (HASFLAG(setting, F_CHOICE_SETTING) && !HASFLAG(setting, F_CHOICETALKS)
        && setting->choice_setting->count == 2)
    {
        const unsigned char **desc = setting->choice_setting->desc;
        const int first = P2ID(desc[0]), second = P2ID(desc[1]);
        const int value = option_value_as_int(setting);
        if (value >= 0 && value < 2
            && ((first == LANG_ON && second == LANG_OFF)
                || (first == LANG_OFF && second == LANG_ON)))
            return P2ID(desc[value]) == LANG_ON;
    }
    return -1;
}

/* Submenus, screens and actions lead to more, settings don't unless they open a
   screen; nor does Shuffle Songs, as on a stock iPod */
static bool menu_has_children(int selected_item, void * data)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    if ((menu->flags&MENU_TYPE_MASK) == MT_RETURN_ID)
        return false;
    const struct menu_item_ex *item =
                    menu->submenus[get_menu_selection(selected_item, menu)];
    switch (item->flags&MENU_TYPE_MASK)
    {
        case MT_MENU:
            return true;
        case MT_FUNCTION_CALL:
            /* what changes right here, in a menu that stays open, opens nothing */
            return !(item->flags & MENU_EXITAFTERTHISMENU)
                || (item->flags & MENU_FUNC_OPENS_SCREEN);
        case MT_RETURN_VALUE:
#ifdef HAVE_TAGCACHE
            /* nor, in a theme whose actions have none, the row that plays:
               Now Playing or Resume Playback */
            return item->value != GO_TO_SHUFFLE_SONGS
                   && item->value != GO_TO_SHUFFLE_ALBUMS
                   && (item->value != GO_TO_WPS || ui_metric(UI_METRIC_CHEVRON_ON_PLAY));
#else
            return true;
#endif
        case MT_SETTING:
        case MT_SETTING_W_TEXT:
            return menu_shows_values(menu)
                && !setting_changes_in_place(find_setting(item->variable));
        default:
            return false;
    }
}

/* whether a row of the list built is a group's bar */
static bool is_group_row(const struct menu_item_ex *menu, int row)
{
    return (menu->submenus[current_subitems[row]]->flags & MENU_TYPE_MASK)
           == MT_GROUP;
}

static int find_menu_selection(int selected)
{
    int i;
    for (i=0; i< current_subitems_count; i++)
        if (current_subitems[i] == selected)
            return i;
    return 0;
}
static const char* get_menu_item_name(int selected_item,
                                      void * data,
                                      char *buffer,
                                      size_t buffer_len)
{
    const char *name;
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int type = (menu->flags&MENU_TYPE_MASK);
    selected_item = get_menu_selection(selected_item, menu);

    /* only MT_MENU or MT_RETURN_ID is allowed in here */
    if (type == MT_RETURN_ID)
    {
        if (menu->flags&MENU_DYNAMIC_DESC)
        {
            name = menu->menu_get_name_and_icon->list_get_name(selected_item,
            menu->menu_get_name_and_icon->list_get_name_data, buffer, buffer_len);
        }
        else
            name = menu->strings[selected_item];

        if (P2ID((unsigned char *)name) > VOICEONLY_DELIMITER)
            name = "";

        return name;
    }
    if (type == MT_MENU)
        menu = menu->submenus[selected_item];

    if ((menu->flags&MENU_DYNAMIC_DESC) && (type != MT_SETTING_W_TEXT))
        return menu->menu_get_name_and_icon->list_get_name(selected_item,
            menu->menu_get_name_and_icon->list_get_name_data, buffer, buffer_len);

    type = (menu->flags&MENU_TYPE_MASK);
    if ((type == MT_SETTING) || (type == MT_SETTING_W_TEXT))
    {
        const struct settings_list *v = find_setting(menu->variable);
        if (!v)
            return "Not Done yet!";
        if (!menu_shows_values((const struct menu_item_ex *)data))
            return str(v->lang_id);
        char value[64];
        const int on = setting_switch(v);
        if (on >= 0)
            snprintf(buffer, buffer_len, "%s\t%c%s", str(v->lang_id),
                     MENU_SWITCH(on), setting_value(v, value, sizeof(value)));
        else
            snprintf(buffer, buffer_len, "%s\t%s", str(v->lang_id),
                     setting_value(v, value, sizeof(value)));
        return buffer;
    }
    return P2STR(menu->callback_and_desc->desc);
}

/* a list of strings some of which have a value, as Cover Flow's Sort By:
   drawn as a setting's row */
static bool strings_have_values(const struct menu_item_ex *menu)
{
    if ((menu->flags & MENU_TYPE_MASK) != MT_RETURN_ID)
        return false;
    for (int i = 0; i < (int)MENU_GET_COUNT(menu->flags); i++)
    {
        /* a phrase's id, voice ones among them, has no tab */
        const unsigned char *s = (const unsigned char *)menu->strings[i];
        if (P2ID(s) < 0 && strchr((const char *)s, '\t'))
            return true;
    }
    return false;
}

static bool menu_has_groups(const struct menu_item_ex *menu)
{
    if ((menu->flags&MENU_TYPE_MASK) != MT_MENU)
        return false;
    for (int i = 0; i < (int)MENU_GET_COUNT(menu->flags); i++)
    {
        if ((menu->submenus[i]->flags&MENU_TYPE_MASK) == MT_GROUP)
            return true;
    }
    return false;
}

static bool menu_is_group(int item, void *data)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    return (menu->flags&MENU_TYPE_MASK) == MT_MENU
        && (menu->submenus[get_menu_selection(item, menu)]->flags&MENU_TYPE_MASK)
           == MT_GROUP;
}

/* A line of "name\tvalue": the name as usual, then the value at the right end,
   grey. It goes left of where a chevron can come in a list that has them, and
   only if it fits beside the name. The name of a group has a bar of its own. */
#define VALUE_COLOUR LIST_VALUE_COLOUR
#define GROUP_COLOUR LIST_GROUP_COLOUR
#define GROUP_BAR_COLOUR LIST_GROUP_BAR_COLOUR
#define VALUE_MARGIN ui_metric(UI_METRIC_VALUE_MARGIN)
#define VALUE_MARGIN_CHEVRON ui_metric(UI_METRIC_VALUE_MARGIN_CHEVRON)
#define VALUE_GAP ui_metric(UI_METRIC_VALUE_GAP)
#define LIGHT_LEAST 40 /* the room a lighter text after a name needs at least */
void menu_draw_value_line(struct list_putlineinfo_t *info)
{
    struct screen *display = info->display;
    const char *text = info->dsp_text;
    const char *tab = strchr(text, '\t');
    char name[64];

    if (info->list->callback_is_group
        && info->list->callback_is_group(info->line, info->list->data))
    {
        struct line_desc linedes = *info->linedes;
        const int group_style = ui_metric(UI_METRIC_GROUP_BAR);
        if (!group_style || group_style == 3)
        {
            /* no bar: the name small and quiet, low in its row, over the
               rows it names; or (3) a heading in the theme's accent, its
               font's cell two rows over the row's end */
            const int name_font = info->vp->font;
            const int font = font_find(ui_font_file(UI_FONT_GROUP));
            linedes.style = STYLE_DEFAULT;
            display->put_line(info->x, info->y, &linedes, "");
            const unsigned old_fg = display->get_foreground();
            display->set_foreground(group_style ? ui_colour(UI_COLOUR_ACCENT)
                                                : GROUP_COLOUR);
            display->set_drawmode(DRMODE_FG);
            if (font >= 0)
                info->vp->font = font;
            /* a heading too long for the row is cut with an ellipsis, at
               the values' margin; the small name is left to the row's end */
            char fitted[MAX_PATH];
            if (group_style)
                text = line_fit(text, info->vp->font,
                                info->vp->width - info->x - info->item_indent
                                - VALUE_MARGIN, fitted, sizeof(fitted));
            if (text)
                display->putsxy(info->x + info->item_indent,
                                info->y + linedes.height - (group_style ? 2 : 6)
                                - (int)font_get(info->vp->font)->height, text);
            info->vp->font = name_font;
            display->set_drawmode(DRMODE_SOLID);
            display->set_foreground(old_fg);
            return;
        }
        linedes.style = STYLE_COLORBAR;
        linedes.text_color = GROUP_COLOUR;
        linedes.line_color = GROUP_BAR_COLOUR;
#ifdef HAVE_LCD_COLOR
        if (group_style == 2 && display->screen_type == SCREEN_MAIN)
        {
            /* a band as high as the row, a hairline on its first and last
               rows; the name in the theme's font for it and the rows' own
               ink (a grey read poorly at that size), in the band's middle,
               where the rows' text starts */
            const int name_font = info->vp->font;
            const int font = font_find(ui_font_file(UI_FONT_GROUP));
            display->put_line(info->x, info->y, &linedes, "");
            const unsigned old_fg = display->get_foreground();
            display->set_drawmode(DRMODE_SOLID);
            display->set_foreground(ui_colour(UI_COLOUR_GROUP_RULE));
            display->hline(0, info->vp->width - 1, info->y);
            display->hline(0, info->vp->width - 1, info->y + linedes.height - 1);
            display->set_foreground(ui_colour(UI_COLOUR_TEXT));
            display->set_drawmode(DRMODE_FG);
            if (font >= 0)
                info->vp->font = font;
            /* a name too long for the band is cut with an ellipsis, at the
               values' margin */
            char fitted[MAX_PATH];
            text = line_fit(text, info->vp->font,
                            info->vp->width - info->x - info->item_indent
                            - VALUE_MARGIN, fitted, sizeof(fitted));
            if (text)
                display->putsxy(info->x + info->item_indent,
                                info->y + (linedes.height
                                           - (int)font_get(info->vp->font)->height) / 2,
                                text);
            info->vp->font = name_font;
            display->set_drawmode(DRMODE_SOLID);
            display->set_foreground(old_fg);
            return;
        }
#endif
        display->put_line(info->x, info->y, &linedes, "$*s$*t", info->item_indent,
                          info->item_offset, text);
        return;
    }

    /* "name\x1flight": a lighter text right after the name, as an album's
       artist in Search, in the font of a row's second line, on the name's
       baseline; cut short before the chevron, and none where it would be
       only a letter or two */
    const char *light = strchr(text, MENU_LIGHT_SEP);
    if (light)
    {
        char whole[MAX_PATH], fitted[MAX_PATH];
        strlcpy(whole, text, MIN((size_t)(light - text) + 1, sizeof(whole)));
        display->put_line(info->x, info->y, info->linedes, "$*s$*t",
                          info->item_indent, info->item_offset, whole);
        int name_width, height;
        display->getstringsize(whole, &name_width, &height);
        const int x = info->x + info->item_indent + name_width + VALUE_GAP;
        const int right = info->vp->width - (info->list->callback_has_children ?
                                             VALUE_MARGIN_CHEVRON : VALUE_MARGIN);
        const int name_font = info->vp->font;
        const int font = list_subline_font(name_font);
        /* cut short in the form of the whole */
        font_set_text_form(font_text_form((const unsigned char *)light + 1, -1));
        const char *fit = right - x >= LIGHT_LEAST ?
            line_fit(light + 1, font, right - x, fitted, sizeof(fitted)) : NULL;
        if (!fit)
        {
            font_set_text_form(-1);
            return;
        }
        const int y = info->y + info->linedes->height / 2 - height / 2
                      + font_get(name_font)->ascent - font_get(font)->ascent;
        const unsigned old_fg = display->get_foreground();
        display->set_foreground(info->is_selected ? ui_colour(UI_COLOUR_VALUE_SELECTED)
                                                  : VALUE_COLOUR);
        display->set_drawmode(DRMODE_FG);
        info->vp->font = font;
        display->putsxy(x, y, fit);
        font_set_text_form(-1);
        info->vp->font = name_font;
        display->set_drawmode(DRMODE_SOLID);
        display->set_foreground(old_fg);
        return;
    }

    /* a row without a value, as an album's, as long as it is */
    if (!tab)
    {
        display->put_line(info->x, info->y, info->linedes, "$*s$*t",
                          info->item_indent, info->item_offset, text);
        return;
    }
    strlcpy(name, text, MIN((size_t)(tab - text) + 1, sizeof(name)));
    display->put_line(info->x, info->y, info->linedes, "$*s$*t", info->item_indent,
                      info->item_offset, name);

    int name_width, width, height;
    display->getstringsize(name, &name_width, &height);
    const int name_end = info->x + info->item_indent + name_width + VALUE_GAP;
    /* On or Off that changes in place: the theme's switch at the row's end
       if it fits beside the name, or the word */
    const char *value = tab + 1;
    const bool is_switch = *value == MENU_SWITCH_ON || *value == MENU_SWITCH_OFF;
    if (is_switch)
    {
        const bool on = *value++ == MENU_SWITCH_ON;
#ifdef HAVE_LCD_COLOR
        if (ui_metric(UI_METRIC_SWITCHES) && display->screen_type == SCREEN_MAIN
            && name_end <= info->vp->width - VALUE_MARGIN - 27)
        {
            /* a capsule of 27 by 16, its knob 12 px, white; at the other
               end right to left */
            const int x = VP_IS_RTL(info->vp) ? VALUE_MARGIN
                                              : info->vp->width - VALUE_MARGIN - 27;
            const int y = info->y + (info->linedes->height - 16) / 2;
            const unsigned old_fg = display->get_foreground();
            const unsigned old_bg = display->get_background();
            const unsigned colour = ui_colour(on ? UI_COLOUR_SWITCH_ON
                                                 : UI_COLOUR_SWITCH_OFF);
            if (info->is_selected)
                display->set_background(global_settings.lss_color);
            /* on a bar of one colour, a ring in the bar's text's colour
               round it: the green stood on the accent without an edge */
            if (info->is_selected && ui_metric(UI_METRIC_SELECTION_INK))
            {
                line_fill_rounded(display, x - 1, y - 1, 29, 18, 9,
                                  global_settings.lst_color);
                display->set_background(global_settings.lst_color);
            }
            line_fill_rounded(display, x, y, 27, 16, 8, colour);
            display->set_background(colour);
            line_fill_rounded(display, x + (on ? 13 : 2), y + 2, 12, 12, 6,
                              LCD_RGBPACK(0xff, 0xff, 0xff));
            display->set_background(old_bg);
            display->set_foreground(old_fg);
            display->set_drawmode(DRMODE_SOLID);
            return;
        }
#endif
    }
    /* the value in the theme's font for it, on the name's baseline */
    const int name_font = info->vp->font;
    int font = font_find(ui_font_file(UI_FONT_MENU_VALUE));
    if (font < 0)
        font = name_font;
    font_getstringsize(value, &width, NULL, font);
    /* left of where a chevron can come in the list; or, where the theme
       lays the row's end out itself, of this row's own, selected or not.
       Right to left no row has a chevron: where every row that leads on
       would have one, their values end with the others' */
    const bool chevron = info->list->callback_has_children
        && (!ui_metric(UI_METRIC_ROW_RIGHT)
            || info->list->callback_has_children(info->line, info->list->data))
        && !(ui_metric(UI_METRIC_CHEVRON_ALL) && VP_IS_RTL(info->vp));
    const int right = info->vp->width -
        (chevron ? VALUE_MARGIN_CHEVRON : VALUE_MARGIN);
    const int least = name_end;
    char fitted[MAX_PATH];
    int x = right - width;
    if (x < least)
    {
        /* no room after the name: none, or by the theme the value cut
           short with an ellipsis, if that leaves enough of it to read */
        if (!ui_metric(UI_METRIC_VALUE_CUT) || right - least < LIGHT_LEAST)
            return;
        /* cut short in the form of the whole, and drawn in it */
        font_set_text_form(font_text_form((const unsigned char *)value, -1));
        value = line_fit(value, font, right - least, fitted, sizeof(fitted));
        if (!value)
        {
            font_set_text_form(-1);
            return;
        }
        font_getstringsize(value, &width, NULL, font);
        x = right - width;
    }

    const unsigned old_fg = display->get_foreground();
    display->set_foreground(info->is_selected ? ui_colour(UI_COLOUR_VALUE_SELECTED)
                                                  : VALUE_COLOUR);
    display->set_drawmode(DRMODE_FG);
    info->vp->font = font;
    display->putsxy(x, info->y + (info->linedes->height - height) / 2
                    + font_get(name_font)->ascent - font_get(font)->ascent, value);
    font_set_text_form(-1);
    info->vp->font = name_font;
    display->set_drawmode(DRMODE_SOLID);
    display->set_foreground(old_fg);
}

static enum themable_icons  menu_get_icon(int selected_item, void * data)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int menu_icon = Icon_NOICON;
    int type = (menu->flags&MENU_TYPE_MASK);
    selected_item = get_menu_selection(selected_item, menu);

    if (type == MT_RETURN_ID)
    {
        return Icon_Menu_functioncall;
    }
    if (type == MT_MENU)
        menu = menu->submenus[selected_item];

    if (menu->flags&MENU_HAS_DESC)
        menu_icon = menu->callback_and_desc->icon_id;
    else if (menu->flags&MENU_DYNAMIC_DESC)
        menu_icon = menu->menu_get_name_and_icon->icon_id;

    if (menu_icon == Icon_NOICON)
    {
        unsigned int flags = (menu->flags&MENU_TYPE_MASK);
        if(flags == MT_MENU)
            menu_icon = Icon_Submenu;
        else if (flags == MT_SETTING || flags == MT_SETTING_W_TEXT)
             menu_icon = Icon_Menu_setting;
        else if (flags == MT_FUNCTION_CALL || flags == MT_RETURN_VALUE)
             menu_icon = Icon_Menu_functioncall;
    }
    return menu_icon;
}

static char* init_title(const struct menu_item_ex *menu, int *icon,
                        char* buf, size_t buf_sz)
{
    char *title;

    if (menu->flags&MENU_HAS_DESC)
    {
        *icon = menu->callback_and_desc->icon_id;
        title = P2STR(menu->callback_and_desc->desc);
    }
    else if (menu->flags&MENU_DYNAMIC_DESC)
    {
        *icon = menu->menu_get_name_and_icon->icon_id;
        title = menu->menu_get_name_and_icon->
                      list_get_name(-1, menu->menu_get_name_and_icon->
                                    list_get_name_data, buf, buf_sz);
    }
    else
    {
        *icon = Icon_NOICON;
        title = "";
    }

    if (*icon == Icon_NOICON)
        *icon = Icon_Submenu_Entered;

    return title;
}

static void panel_before_draw(struct gui_synclist *lists);

static int init_menu_lists(const struct menu_item_ex *menu,
                     struct gui_synclist *lists, int selected, bool callback,
                     struct viewport parent[NB_SCREENS], char* buf, size_t buf_sz)
{
    if (!menu || !lists)
    {
        panicf("init_menu_lists, NULL pointer");
        return 0;
    }

    int i;
    int start_action = ACTION_ENTER_MENUITEM;
    int count = MIN(MENU_GET_COUNT(menu->flags), MAX_MENU_SUBITEMS);
    int type = (menu->flags&MENU_TYPE_MASK);
    menu_callback_type menu_callback = &empty_menu_callback;
    int icon;
    char * title;
    current_subitems_count = 0;

    if (type == MT_RETURN_ID)
        get_menu_callback(menu, &menu_callback);

    for (i=0; i<count; i++)
    {
        if (type != MT_RETURN_ID)
            get_menu_callback(menu->submenus[i],&menu_callback);

        if (menu_callback(ACTION_REQUEST_MENUITEM,
            type==MT_RETURN_ID ? (void*)(intptr_t)i: menu->submenus[i], lists)
                != ACTION_EXIT_MENUITEM
            && (type == MT_RETURN_ID || !onplay_row_hidden(menu, i)))
        {
            current_subitems[current_subitems_count] = i;
            current_subitems_count++;
        }
    }

    /* a group's bar with none of its rows shown after it, as when they're
       hidden: gone too */
    if (type == MT_MENU)
    {
        int kept = 0;
        for (i = 0; i < current_subitems_count; i++)
        {
            if (is_group_row(menu, i)
                && (i + 1 == current_subitems_count || is_group_row(menu, i + 1)))
                continue;
            current_subitems[kept++] = current_subitems[i];
        }
        current_subitems_count = kept;
    }

    current_submenus_menu = (struct menu_item_ex *)menu;

    gui_synclist_init(lists,get_menu_item_name,(void*)menu,false,1, parent);
    gui_synclist_set_before_draw(lists, panel_before_draw);
    if (menu_wide() && menu_has_hints(menu))
        gui_synclist_set_hint_callback(lists, menu_hint);
    if (menu_shows_values(menu) || menu_has_groups(menu)
        || strings_have_values(menu))
    {
        lists->callback_draw_item = menu_draw_value_line;
        lists->callback_is_group = menu_is_group;
    }
    title = init_title(menu, &icon, buf, buf_sz);
    gui_synclist_set_title(lists, title, icon);
    gui_synclist_set_icon_callback(lists, global_settings.show_icons?menu_get_icon:NULL);
    gui_synclist_set_has_children_callback(lists, menu_has_children);
    if(global_settings.talk_menu)
        gui_synclist_set_voice_callback(lists, talk_menu_item);
    gui_synclist_set_nb_items(lists,current_subitems_count);
    /* not on a group's bar, as when the rows before it are hidden */
    int row = find_menu_selection(selected);
    while (type == MT_MENU && row + 1 < current_subitems_count
           && is_group_row(menu, row))
        row++;
    gui_synclist_select_item(lists, row);

    get_menu_callback(menu,&menu_callback);
    if (callback)
        start_action = menu_callback(start_action, menu, lists);

    return start_action;
}

static int talk_menu_item(int selected_item, void *data)
{
    const struct menu_item_ex *menu = (const struct menu_item_ex *)data;
    int id = -1;
    int type;
    unsigned char *str;
    int sel = get_menu_selection(selected_item, menu);

        if ((menu->flags&MENU_TYPE_MASK) == MT_MENU)
        {
            type = menu->submenus[sel]->flags&MENU_TYPE_MASK;
            if ((type == MT_SETTING) || (type == MT_SETTING_W_TEXT))
                talk_setting(menu->submenus[sel]->variable);
            else
            {
                if (menu->submenus[sel]->flags&(MENU_DYNAMIC_DESC))
                {
                    int (*list_speak_item)(int selected_item, void * data)
                        = menu->submenus[sel]->menu_get_name_and_icon->
                        list_speak_item;
                    if(list_speak_item)
                        list_speak_item(sel, menu->submenus[sel]->
                                        menu_get_name_and_icon->
                                        list_get_name_data);
                    else
                    {
                        char buffer[80];
                        str = menu->submenus[sel]->menu_get_name_and_icon->
                            list_get_name(sel, menu->submenus[sel]->
                                    menu_get_name_and_icon->
                                    list_get_name_data, buffer, sizeof(buffer));
                        id = P2ID(str);
                    }
                }
                else
                    id = P2ID(menu->submenus[sel]->callback_and_desc->desc);
                if (id != -1)
                    talk_id(id,false);
            }
        }
        else if(((menu->flags&MENU_TYPE_MASK) == MT_RETURN_ID))
        {
            if ((menu->flags&MENU_DYNAMIC_DESC) == 0)
            {
                unsigned char *s = (unsigned char *)menu->strings[sel];
                /* string list, try to talk it if ID2P was used */
                id = P2ID(s);
                if (id != -1)
                    talk_id(id,false);
            }
        }
        return 0;
}

void do_setting_screen(const struct settings_list *setting, const char * title,
                        struct viewport parent[NB_SCREENS])
{
    char padded_title[MAX_PATH];
    /* Pad the title string by repeating it. This is needed
       so the scroll settings title can actually be used to
       test the setting */
    if (setting->flags&F_PADTITLE)
    {
        int i = 0, len;
        title = P2STR((unsigned char*)title);
        len = strlen(title);
        while (i < MAX_PATH-1)
        {
            int padlen = MIN(len, MAX_PATH-1-i);
            memcpy(&padded_title[i], title, padlen);
            i += padlen;
            if (i<MAX_PATH-1)
                padded_title[i++] = ' ';
        }
        padded_title[i] = '\0';
        title = padded_title;
    }

    option_screen((struct settings_list *)setting, parent,
                  setting->flags&F_TEMPVAR, (char*)title);
}


void do_setting_from_menu(const struct menu_item_ex *temp,
                          struct viewport parent[NB_SCREENS])
{
    char *title;
    if (!temp)
    {
        panicf("do_setting_from_menu, NULL pointer");
        return;
    }
    const struct settings_list *setting = find_setting(temp->variable);

    if ((temp->flags&MENU_TYPE_MASK) == MT_SETTING_W_TEXT)
        title = temp->callback_and_desc->desc;
    else
        title = ID2P(setting->lang_id);

    do_setting_screen(setting, title, parent);
}

/* display a menu */
/* Settings is selected in the main menu, for the theme to show the iPod beside it */
static enum menu_panel selected_panel = MENU_PANEL_NONE;

enum menu_panel menu_selected_panel(void)
{
    return selected_panel;
}

static bool wide = false;
bool menu_wide(void)
{
    return wide;
}

void menu_set_wide(bool on)
{
    wide = on;
    /* the theme chooses the size of what's drawn in it */
    FOR_NB_SCREENS(i)
    {
        skin_request_full_update(CUSTOM_STATUSBAR);
        skin_update(CUSTOM_STATUSBAR, i, SKIN_REFRESH_ALL);
    }
}

void menu_set_wide_later(bool on)
{
    wide = on;
    skin_request_full_update(CUSTOM_STATUSBAR);
}

static bool root_requested = false;
void menu_request_root(void)
{
    root_requested = true;
}

/* The theme drawn again for it, over a part of the list, which is drawn next:
   the two go on the screen together then. Shown at once, the album art's strip
   over the list's right end took off that of the selection for the time the
   list took to draw */
static void set_selected_panel(enum menu_panel panel)
{
    if (panel == selected_panel)
        return;
    selected_panel = panel;
    viewportmanager_redraw_unshown();
}

static enum menu_panel item_panel(const struct menu_item_ex *menu, int selected)
{
    if (menu != &root_menu_)
        return MENU_PANEL_NONE;
    const struct menu_item_ex *item = menu->submenus[selected];
    if ((item->flags & MENU_TYPE_MASK) != MT_RETURN_VALUE)
        return MENU_PANEL_NONE;
    switch (item->value)
    {
        case GO_TO_MAINMENU:
            return MENU_PANEL_SETTINGS;
        case GO_TO_EXTRAS:
            return MENU_PANEL_EXTRAS;
        case GO_TO_PHOTOS:
            return MENU_PANEL_PHOTOS;
        default:
            return MENU_PANEL_NONE;
    }
}

/* Before a menu's list is drawn, its panel for the selection, so that the two
   go on the screen together: drawn after it, as the list had been, the theme's
   album art strip over the list's right end showed for a moment, and the list
   was drawn twice */
static void panel_before_draw(struct gui_synclist *lists)
{
    static bool drawing;
    if (drawing)
        return;
    const struct menu_item_ex *menu = lists->data;
    drawing = true;
    set_selected_panel(item_panel(menu,
        get_menu_selection(gui_synclist_get_sel_pos(lists), menu)));
    drawing = false;
}

int do_menu(const struct menu_item_ex *start_menu, int *start_selected,
            struct viewport parent[NB_SCREENS], bool hide_theme)
{
    int selected = start_selected? *start_selected : 0;
    int ret = 0;
    int action;
    int start_action;
    int icon;
    char buf[80], *title;
    struct gui_synclist lists;
    const struct menu_item_ex *temp = NULL;
    const struct menu_item_ex *menu = start_menu;

    bool in_stringlist, done = false;
    bool redraw_lists;
    /* a menu that came in with a slide goes with one, and one opened by an
       item's function, which came at once, goes at once too */
    const bool slid_in = transition_pending();

    int old_audio_status = audio_status();

#ifdef HAVE_TOUCHSCREEN
    /* plugins possibly have grid mode active. force global settings in lists */
    enum touchscreen_mode tsm = touchscreen_get_mode();
    enum touchscreen_mode old_global_mode = global_settings.touch_mode;
    touchscreen_set_mode(global_settings.touch_mode);
#endif

    title = init_title(menu, &icon, buf, sizeof buf);
    FOR_NB_SCREENS(i)
    {
        sb_set_persistent_title(title, icon, i);
        viewportmanager_theme_enable(i, !hide_theme, NULL);
    }
    struct menu_data_t mstack[MAX_MENUS]; /* menu, selected */
    int stack_top = 0;

    struct viewport *vps = NULL;
    menu_callback_type menu_callback = &empty_menu_callback;

    /* if hide_theme is true, assume parent has been fixed before passed into
     * this function, e.g. with viewport_set_defaults(parent, screen)
     * start_action allows an action to be processed
     * by menu logic by bypassing get_action on the initial run */
    start_action = init_menu_lists(menu, &lists, selected, true, parent,
                                   buf, sizeof buf);
    vps = *(lists.parent);
    in_stringlist = ((menu->flags&MENU_TYPE_MASK) == MT_RETURN_ID);
    /* load the callback, and only reload it if menu changes */
    get_menu_callback(menu, &menu_callback);

    gui_synclist_draw(&lists);
    gui_synclist_speak_item(&lists);

    while (!done)
    {
        keyclick_set_callback(gui_synclist_keyclick_callback, &lists);

        if (UNLIKELY(start_action != ACTION_ENTER_MENUITEM))
        {
            action = start_action;
            start_action = ACTION_ENTER_MENUITEM;
        }
        else
        {
            action = get_action(CONTEXT_MAINMENU|ALLOW_SOFTLOCK,
                                list_do_action_timeout(&lists, HZ));
        }
            /* HZ so the status bar redraws corectly */

        /* query audio status to see if it changed */
        redraw_lists = query_audio_status(&old_audio_status);
        menu_cover_memory();

#ifdef HAVE_TOUCHSCREEN
        /* need to translate touch actions *first* so the menu callback has
         * a chance to intercept before it hits the list's do_button. */
        if (action == ACTION_TOUCHSCREEN)
            action = gui_synclist_do_touchscreen(&lists);
#endif

        int new_action = menu_callback(action, menu, &lists);
        if (new_action == ACTION_EXIT_AFTER_THIS_MENUITEM)
            ret = MENU_SELECTED_EXIT; /* exit after return from selection */
        else if (new_action == ACTION_REDRAW)
            redraw_lists = true;
        else
            action = new_action;

        if (LIKELY(gui_synclist_do_button(&lists, &action)))
            continue;
#ifdef HAVE_QUICKSCREEN
        else if (action == ACTION_STD_QUICKSCREEN)
        {
            if (global_settings.shortcuts_replaces_qs ||
                quickscreen_show(action) == QUICKSCREEN_GOTO_SHORTCUTS_MENU)
            {
                int last_screen = global_status.last_screen;
                global_status.last_screen = GO_TO_SHORTCUTMENU;
                int shortcut_ret = do_shortcut_menu(NULL);
                if (shortcut_ret == GO_TO_PREVIOUS)
                    global_status.last_screen = last_screen;
                else
                {
                    ret = shortcut_ret;
                    done = true;
                }
            }
            if (!done)
                init_menu_lists(menu, &lists, lists.selected_item, false, vps,
                                buf, sizeof buf);
            redraw_lists = true;
        }
#endif
#ifdef HAVE_RECORDING
        else if (action == ACTION_STD_REC)
        {
            ret = GO_TO_RECSCREEN;
            done = true;
        }
#endif
        else if (action == ACTION_TREE_WPS)
        {
            ret = GO_TO_PREVIOUS_MUSIC;
            done = true;
        }
        else if (action == ACTION_TREE_STOP)
        {
            /* holding Play goes to the track playing, if there is one */
            if (audio_status())
            {
                ret = GO_TO_PREVIOUS_MUSIC;
                done = true;
            }
        }
        else if (action == ACTION_STD_CONTEXT)
        {
            if (menu == &root_menu_)
            {
                ret = GO_TO_ROOTITEM_CONTEXT;
                done = true;
            }
            else if (!in_stringlist)
            {
                int type = (menu->flags&MENU_TYPE_MASK);
                selected = get_menu_selection(gui_synclist_get_sel_pos(&lists),menu);
                if (type == MT_MENU)
                {
                    temp = menu->submenus[selected];
                    type = (temp->flags&MENU_TYPE_MASK);
                }
                else
                    type = -1;

                if ((type == MT_SETTING_W_TEXT || type == MT_SETTING)
                    && !menu_shows_values(menu))
                {
                    const struct menu_item_ex *context_menu;
                    const struct settings_list *setting =
                            find_setting(temp->variable);

                    MENUITEM_STRINGLIST(settings_op_menu,
                                        ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                                        ID2P(LANG_RESET_SETTING),
#ifndef HAVE_QUICKSCREEN
                                       );
                    context_menu = &settings_op_menu;
#else
                                        ID2P(LANG_TOP_QS_ITEM),
                                        ID2P(LANG_LEFT_QS_ITEM),
                                        ID2P(LANG_BOTTOM_QS_ITEM),
                                        ID2P(LANG_RIGHT_QS_ITEM),
                                        ID2P(LANG_ADD_CURRENT_TO_FAVES),
                                        ID2P(LANG_ADD_TO_FAVES));

                    /* re-use the strings and desc from the settings_op_menu */
                    static const struct menu_item_ex non_quickscreen_op_menu =
                    {MT_RETURN_ID|MENU_HAS_DESC|MENU_ITEM_COUNT(1),
                    { .strings = settings_op_menu_},
                    {.callback_and_desc = &settings_op_menu__}};

                    if (is_setting_quickscreenable(setting))
                        context_menu = &settings_op_menu;
                    else
                    {
                        context_menu = &non_quickscreen_op_menu;
                    }
#endif
                    int msel = do_menu(context_menu, NULL, NULL, false);

                    switch (msel)
                    {
                        case GO_TO_PREVIOUS:
                            break;
                        case 0: /* reset setting */
                            reset_setting(setting, setting->setting);
                            settings_save();
                            settings_apply(false);
                            break;
#ifdef HAVE_QUICKSCREEN
                        case 1: /* set as top QS item */
                            global_settings.qs_items[QUICKSCREEN_TOP] = setting;
                            break;
                        case 2: /* set as left QS item */
                            global_settings.qs_items[QUICKSCREEN_LEFT] = setting;
                            break;
                        case 3: /* set as bottom QS item */
                            global_settings.qs_items[QUICKSCREEN_BOTTOM] = setting;
                            break;
                        case 4: /* set as right QS item */
                            global_settings.qs_items[QUICKSCREEN_RIGHT] = setting;
                            break;
                        case 5: /* Add current value of setting to faves.
                                   Same limitation on which can be
                                   added to the shortcuts menu as the quickscreen */
                            shortcuts_add(SHORTCUT_SETTING_APPLY, (void*)setting);
                            break;
                        case 6: /* Add to faves. Same limitation on which can be
                                  added to the shortcuts menu as the quickscreen */
                            shortcuts_add(SHORTCUT_SETTING, (void*)setting);
                            break;
#endif
                    } /* switch(do_menu()) */
                    if (menu->flags & MENU_EXITAFTERTHISMENU)
                        done = true; /* in case onplay menu contains setting */
                    redraw_lists = true;
                }
            } /* else if (!in_stringlist) */
        }
        else if (action == ACTION_STD_MENU)
        {
            /* Menu held; it's already there in the main menu itself. Also for a
               menu around this one, if this was opened by a function of it. */
            if (menu != &root_menu_)
            {
                ret = GO_TO_ROOT;
                done = true;
                root_requested = true;
            }
        }
        else if (action == ACTION_STD_CANCEL)
        {
            /* might be leaving list, so stop scrolling */
            gui_synclist_scroll_stop(&lists);

            bool exiting_menu = false;
            in_stringlist = false;

            menu_callback(ACTION_EXIT_MENUITEM, menu, &lists);

            /* one opened from another goes back to it, as anywhere else */
            if ((menu->flags&MENU_EXITAFTERTHISMENU) && stack_top == 0)
                done = true;
            else if ((menu->flags&MENU_TYPE_MASK) == MT_MENU)
                exiting_menu = true;

            if (stack_top > 0)
            {
                stack_top--;
                menu = mstack[stack_top].menu;
                int msel = mstack[stack_top].selected;
                if (!exiting_menu && (menu->flags&MENU_EXITAFTERTHISMENU))
                    done = true;
                else
                {
                    transition_begin(TRANSITION_BACK);
                    init_menu_lists(menu, &lists, msel, false, vps, buf, sizeof buf);
                }
                redraw_lists = true;
                /* new menu, so reload the callback */
                get_menu_callback(menu, &menu_callback);
            }
            else if (menu != &root_menu_)
            {
                if (slid_in)
                    transition_begin(TRANSITION_BACK);
                ret = GO_TO_PREVIOUS;
                done = true;
            }
        }
        else if (action == ACTION_STD_OK)
        {
            /* entering an item that may not be a list, so stop scrolling */
            gui_synclist_scroll_stop(&lists);
            redraw_lists = true;

            int type = (menu->flags&MENU_TYPE_MASK);
            selected = get_menu_selection(gui_synclist_get_sel_pos(&lists), menu);
            if (type == MT_MENU)
                temp = menu->submenus[selected];
            else if (!in_stringlist)
                type = -1;

            if (!in_stringlist && temp)
            {
                type = (temp->flags&MENU_TYPE_MASK);
                get_menu_callback(temp, &menu_callback);
                action = menu_callback(ACTION_ENTER_MENUITEM, temp, &lists);
                if (action == ACTION_EXIT_MENUITEM)
                    break;
            }
            root_requested = false;
            switch (type)
            {
                case MT_MENU:
                    if (stack_top < MAX_MENUS)
                    {
                        mstack[stack_top].menu = menu;
                        mstack[stack_top].selected = selected;
                        stack_top++;
                        menu = temp;
                        transition_begin(TRANSITION_FORWARD);
                        init_menu_lists(menu, &lists, 0, true, vps, buf, sizeof buf);
                    }
                    break;
                case MT_FUNCTION_CALL_W_PARAM:
                case MT_FUNCTION_CALL:
                {
                    int return_value;
                    if (type == MT_FUNCTION_CALL_W_PARAM)
                    {
                        return_value = temp->function_param->function_w_param(
                                    temp->function_param->param);
                    }
                    else
                    {
                        return_value = temp->function->function();
                    }
                    if (!(menu->flags&MENU_EXITAFTERTHISMENU) ||
                            (temp->flags&MENU_EXITAFTERTHISMENU))
                    {
                        /* Reload menu but don't run the calback again FS#8117 */
                        init_menu_lists(menu, &lists, selected, false, vps,
                                        buf, sizeof buf);
                    }
                    if (temp->flags&MENU_FUNC_CHECK_RETVAL)
                    {
                        if (return_value != 0)
                        {
                            done = true;
                            ret =  return_value;
                        }
                    }
                    break;
                }
                case MT_SETTING:
                case MT_SETTING_W_TEXT:
                {
                    const struct settings_list *setting =
                                                find_setting(temp->variable);
                    if (menu_shows_values(menu)
                        && setting_changes_in_place(setting))
                    {
                        option_select_next_val(setting, false, true);
                        settings_save();
                        /* the rows shown again, as one may show only with
                           some values of it: Status Icons with Cover Colours;
                           where the list was scrolled to as well */
                        int top[NB_SCREENS];
                        FOR_NB_SCREENS(i)
                            top[i] = lists.start_item[i];
                        init_menu_lists(menu, &lists, selected, false, vps,
                                        buf, sizeof buf);
                        /* no further down than the last row's screen: a
                           row gone left a line empty at the bottom */
                        FOR_NB_SCREENS(i)
                            lists.start_item[i] = MAX(MIN(top[i],
                                lists.nb_items - list_get_nb_lines(&lists, i)), 0);
                        redraw_lists = true;
                        break;
                    }
                    transition_begin(TRANSITION_FORWARD);
                    do_setting_from_menu(temp, vps);
                    transition_begin(TRANSITION_BACK);
                    init_menu_lists(menu, &lists, selected, false, vps, buf, sizeof buf);
                    redraw_lists = true;

                    break;
                }
                case MT_RETURN_ID:
                    if (in_stringlist)
                    {
                        done = true;
                        ret =  selected;
                    }
                    else if (stack_top < MAX_MENUS)
                    {
                        mstack[stack_top].menu = menu;
                        mstack[stack_top].selected = selected;
                        stack_top++;
                        menu = temp;
                        transition_begin(TRANSITION_FORWARD);
                        init_menu_lists(menu, &lists, 0, false, vps, buf, sizeof buf);
                        in_stringlist = true;
                    }
                    break;
                case MT_RETURN_VALUE:
                    if (menu == &root_menu_)
                        transition_begin(TRANSITION_FORWARD);
                    ret = temp->value;
                    done = true;
                    break;

                default:
                    ret = GO_TO_PREVIOUS;
                    done = true;
                    break;
            }
            if (root_requested)
            {
                root_requested = false;
                ret = GO_TO_ROOT;
                done = true;
            }
            if (type != MT_MENU)
            {
                menu_callback(ACTION_EXIT_MENUITEM, temp, &lists);
            }
            if (current_submenus_menu != menu)
                init_menu_lists(menu, &lists,selected, true, vps, buf, sizeof buf);
            /* callback was changed, so reload the menu's callback */
            get_menu_callback(menu, &menu_callback);
            if ((menu->flags&MENU_EXITAFTERTHISMENU) &&
                !(temp->flags&MENU_EXITAFTERTHISMENU))
            {
                done = true;
                break;
            }
        }
        else
        {
            if (action == SYS_USB_CONNECTED)
                gui_synclist_scroll_stop(&lists);

            switch(default_event_handler(action))
            {
                case SYS_USB_CONNECTED:
                    ret = MENU_ATTACHED_USB;
                    done = true;
                    break;
                case SYS_CALL_HUNG_UP:
                case BUTTON_MULTIMEDIA_PLAYPAUSE:
                /* remove splash from playlist_resume() */
                    redraw_lists = true;
                    break;
            }
        }

        if (redraw_lists && !done)
        {
            if (menu_callback(ACTION_REDRAW, menu, &lists) != ACTION_REDRAW)
                continue;


            gui_synclist_set_title(&lists, lists.title, lists.title_icon);
            gui_synclist_draw(&lists);
            gui_synclist_speak_item(&lists);
        }
    }

    if (start_selected)
    {
        /* make sure the start_selected variable is set to
           the selected item from the menu do_menu() was called from */
        if (stack_top > 0)
        {
            menu = mstack[0].menu;
            init_menu_lists(menu, &lists, mstack[0].selected, true, vps,
                            buf, sizeof buf);
        }
        *start_selected = get_menu_selection(
                            gui_synclist_get_sel_pos(&lists), menu);
    }

    FOR_NB_SCREENS(i)
    {
        sb_set_persistent_title(lists.title, lists.title_icon, i);
        viewportmanager_theme_undo(i, false);
        skinlist_set_cfg(i, NULL); /* Bugfix dangling reference in skin_draw() */
    }
#ifdef HAVE_TOUCHSCREEN
    /* This is needed because this function runs the settings menu and we do
     * not want to switch back to the old mode if the user intentionally went
     * to a different one. This is a very hacky way to do this... */
    if(!(global_settings.touch_mode != (int)old_global_mode &&
         tsm == old_global_mode))
        touchscreen_set_mode(tsm);
#endif
    /* the next screen draws it: drawn here it'd show for a moment before that */
    selected_panel = MENU_PANEL_NONE;
    return ret;
}
