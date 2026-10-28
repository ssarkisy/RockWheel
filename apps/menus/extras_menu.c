/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Sergei Sarkisian
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

/* Extras, as in the main menu of a stock iPod: the plugins closest to its
   clock, games, calendar and stopwatch */

#include <stdbool.h>
#include "config.h"
#include "lang.h"
#include "menu.h"
#include "plugin.h"
#include "rbpaths.h"
#include "root_menu.h"
#include "exported_menus.h"

static int run_plugin(void *param)
{
    switch (plugin_load((const char *)param, NULL))
    {
        case PLUGIN_GOTO_WPS:
            return GO_TO_WPS;
        case PLUGIN_GOTO_PLUGIN:
            return GO_TO_PLUGIN;
        case PLUGIN_USB_CONNECTED:
        case PLUGIN_GOTO_ROOT:
            return GO_TO_ROOT;
        default:
            return 0; /* back to the menu */
    }
}

#define EXTRA(name, lang, path) \
    MENUITEM_FUNCTION_W_PARAM(name, MENU_FUNC_CHECK_RETVAL, ID2P(lang), \
                              run_plugin, (void *)(path), NULL, Icon_NOICON)

EXTRA(clock_item,     LANG_CLOCK,     PLUGIN_APPS_DIR  "/clock.rock");
EXTRA(brick_item,     LANG_BRICK,     PLUGIN_GAMES_DIR "/brickmania.rock");
EXTRA(solitaire_item, LANG_SOLITAIRE, PLUGIN_GAMES_DIR "/solitaire.rock");
EXTRA(calendar_item,  LANG_CALENDAR,  PLUGIN_APPS_DIR  "/calendar.rock");
EXTRA(stopwatch_item, LANG_STOPWATCH, PLUGIN_APPS_DIR  "/stopwatch.rock");

MAKE_MENU(games_menu, ID2P(LANG_PLUGIN_GAMES), NULL, Icon_NOICON,
          &brick_item, &solitaire_item);

MAKE_MENU(extras_menu, ID2P(LANG_EXTRAS), NULL, Icon_NOICON,
          &clock_item, &games_menu, &calendar_item, &stopwatch_item);
