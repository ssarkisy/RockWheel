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
#include "action.h"
#include "file.h"
#include "lang.h"
#include "menu.h"
#include "plugin.h"
#include "rbpaths.h"
#include "root_menu.h"
#include "settings.h"
#include "strnatcmp.h"
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

/* an item whose plugin the build left out isn't shown */
static int plugin_there(int action, const struct menu_item_ex *this_item,
                        struct gui_synclist *this_list)
{
    (void)this_list;
    if (action == ACTION_REQUEST_MENUITEM
        && !file_exists((const char *)this_item->function_param->param))
        return ACTION_EXIT_MENUITEM;
    return action;
}

#define EXTRA(name, lang, path) \
    MENUITEM_FUNCTION_W_PARAM(name, MENU_FUNC_CHECK_RETVAL, ID2P(lang), \
                              run_plugin, (void *)(path), plugin_there, \
                              Icon_NOICON)

EXTRA(clock_item,     LANG_CLOCK,     PLUGIN_APPS_DIR  "/clock.rock");
EXTRA(blackjack_item, LANG_BLACKJACK, PLUGIN_GAMES_DIR "/blackjack.rock");
EXTRA(brick_item,     LANG_BRICK,     PLUGIN_GAMES_DIR "/brickmania.rock");
EXTRA(solitaire_item, LANG_SOLITAIRE, PLUGIN_GAMES_DIR "/solitaire.rock");
EXTRA(jewels_item,    LANG_JEWELS,    PLUGIN_GAMES_DIR "/jewels.rock");
EXTRA(mines_item,     LANG_MINESWEEPER, PLUGIN_GAMES_DIR "/minesweeper.rock");
EXTRA(calendar_item,  LANG_CALENDAR,  PLUGIN_APPS_DIR  "/calendar.rock");
EXTRA(stopwatch_item, LANG_STOPWATCH, PLUGIN_APPS_DIR  "/stopwatch.rock");

/* Rockbox's games of the second archive, rockbox-games.zip, which the
   installer asks about: apps/plugins/ROCKWHEEL_GAMES */
EXTRA(rockblox_item,   LANG_ROCKBLOX,   PLUGIN_GAMES_DIR "/rockblox.rock");
EXTRA(g2048_item,      LANG_2048,       PLUGIN_GAMES_DIR "/2048.rock");
EXTRA(sudoku_item,     LANG_SUDOKU,     PLUGIN_GAMES_DIR "/sudoku.rock");
EXTRA(sokoban_item,    LANG_SOKOBAN,    PLUGIN_GAMES_DIR "/sokoban.rock");
EXTRA(reversi_item,    LANG_REVERSI,    PLUGIN_GAMES_DIR "/reversi.rock");
EXTRA(spacerocks_item, LANG_SPACEROCKS, PLUGIN_GAMES_DIR "/spacerocks.rock");
EXTRA(bubbles_item,    LANG_BUBBLES,    PLUGIN_GAMES_DIR "/bubbles.rock");
EXTRA(clix_item,       LANG_CLIX,       PLUGIN_GAMES_DIR "/clix.rock");

static const char *item_name(const struct menu_item_ex *item)
{
    return P2STR((unsigned char *)item->callback_and_desc->desc);
}

/* Games, by the alphabet of the menus' language as the stock iPod has them,
   sorted as they open, and not shown with none of them on the disk. The
   second archive of the games may not have been put on the iPod. */
static int games_callback(int action, const struct menu_item_ex *this_item,
                          struct gui_synclist *this_list)
{
    (void)this_list;
    const struct menu_item_ex **games = this_item->submenus;
    const int count = MENU_GET_COUNT(this_item->flags);
    if (action == ACTION_REQUEST_MENUITEM)
    {
        for (int i = 0; i < count; i++)
        {
            if (file_exists((const char *)games[i]->function_param->param))
                return action;
        }
        return ACTION_EXIT_MENUITEM;
    }
    if (action == ACTION_ENTER_MENUITEM)
    {
        for (int i = 1; i < count; i++)
        {
            const struct menu_item_ex *game = games[i];
            int j = i;
            for (; j > 0 && strnatcasecmp(item_name(games[j - 1]), item_name(game)) > 0; j--)
                games[j] = games[j - 1];
            games[j] = game;
        }
    }
    return action;
}

MAKE_MENU(games_menu, ID2P(LANG_PLUGIN_GAMES), games_callback, Icon_NOICON,
          &blackjack_item, &brick_item, &solitaire_item, &jewels_item,
          &mines_item, &rockblox_item, &g2048_item, &sudoku_item, &sokoban_item,
          &reversi_item, &spacerocks_item, &bubbles_item, &clix_item);

MAKE_MENU(extras_menu, ID2P(LANG_EXTRAS), NULL, Icon_NOICON,
          &clock_item, &games_menu, &calendar_item, &stopwatch_item);
