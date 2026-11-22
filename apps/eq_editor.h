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
#ifndef _EQ_EDITOR_H_
#define _EQ_EDITOR_H_

#include <stdbool.h>

/* The editor of the EQ's Custom, which the caller has made the EQ
   (eq_set_custom()): its ten bands with their curve, and one band's settings.
   Each change is heard at once and kept in the settings. sought: the caller
   has just had the music decoded again (eq_take()), which the editor then
   doesn't. GO_TO_ROOT for Menu held, MENU_ATTACHED_USB after USB, else 0 for
   Menu, with the slide back begun */
int eq_editor(bool sought);

/* The EQ's makeup on the volume now, with the precut set in step with it, as
   the EQ's settings are applied (eq_apply(), settings_apply()): what the
   editor left waiting for the music queued then goes */
void eq_makeup_now(int tenth_db);

#endif /* _EQ_EDITOR_H_ */
