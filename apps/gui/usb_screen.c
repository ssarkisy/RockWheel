/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 Björn Stenberg
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

#include <stdio.h>
#include <stdbool.h>
#include "action.h"
#include "font.h"
#ifdef HAVE_REMOTE_LCD
#include "lcd-remote.h"
#endif
#include "lang.h"
#include "usb.h"
#if defined(HAVE_USBSTACK)
#include "usb_core.h"
#ifdef USB_ENABLE_HID
#include "usb_keymaps.h"
#endif
#endif
#include "settings.h"
#include "led.h"
#include "appevents.h"
#include "usb_screen.h"
#include "skin_engine/skin_engine.h"
#include "playlist.h"
#include "misc.h"
#include "icons.h"
#ifdef HAVE_TAGCACHE
#include "tagcache.h"
#include "root_menu.h"
#endif

#ifdef USB_IPOD_IDENTITY
#include "bitmaps/nodisconnect.h"
#include "bitmaps/usbok.h"
#include "bitmaps/usbbattery.h"
#include "power.h"
#include "itunesdb.h"
#else
#include "bitmaps/usblogo.h"
#endif

#ifdef HAVE_REMOTE_LCD
#include "bitmaps/remote_usblogo.h"
#endif

#if (CONFIG_STORAGE & STORAGE_MMC)
#include "ata_mmc.h"
#endif

struct usb_screen_vps_t
{
    struct viewport parent;
    struct viewport logo;
#ifndef USB_ENABLE_HID
};
#else
    struct viewport title;
};

int usb_keypad_mode;
static void draw_usb_keypad_mode(struct viewport *title)
{
    struct screen *screen = &screens[SCREEN_MAIN];
    struct viewport *last_vp = screen->set_viewport(title);
    screen->clear_viewport();
    title->flags |= VP_FLAG_ALIGN_CENTER;
    if (title->width > 1)
        screen->puts_scroll(0, 0, str(keypad_mode_name_get(usb_keypad_mode)));
    screen->set_viewport(last_vp);
}
#endif /* USB_ENABLE_HID */

#ifdef USB_IPOD_IDENTITY
static void ipod_usb_update(void);
#endif

static void handle_usb_events(struct viewport *title)
{
#if (CONFIG_STORAGE & STORAGE_MMC) && !defined(SIMULATOR)
    int next_update = 0;
#endif /* STORAGE_MMC */
    int button;

    /* Don't return until we get SYS_USB_DISCONNECTED */
    while(1)
    {
#ifndef USB_ENABLE_HID
        (void)title;
#else
        if (global_settings.usb_hid)
        {
            button = get_hid_usb_action();
            if (button == ACTION_USB_HID_MODE_SWITCH_NEXT ||
                button == ACTION_USB_HID_MODE_SWITCH_PREV)
                draw_usb_keypad_mode(title);
        }
        else
#endif
        {
            /* hid emits the event in get_action */
            send_event(GUI_EVENT_ACTIONUPDATE, NULL);
            button = button_get_w_tmo(HZ/2);
        }
        if (button == SYS_USB_DISCONNECTED)
            return;
        if (button == SYS_CHARGER_DISCONNECTED)
            reset_runtime();
#ifdef USB_IPOD_IDENTITY
        ipod_usb_update();
#endif

/* USB-MMC bridge can report activity */
#if (CONFIG_STORAGE & STORAGE_MMC) && !defined(SIMULATOR)
        if(TIME_AFTER(current_tick,next_update))
        {
            if(usb_inserted()) {
                led(mmc_usb_active(HZ));
            }
            next_update=current_tick+HZ/2;
        }
#endif /* STORAGE_MMC */
    }
}

#ifdef USB_IPOD_IDENTITY
/* The screen of a stock iPod in this mode. It's all drawn here and not by the
   theme, which has the screen split in two, and can't read its files by then. */
#define IPOD_USB_SIGN_X     ((LCD_WIDTH - BMPWIDTH_nodisconnect) / 2)
#define IPOD_USB_SIGN_Y     71
#define IPOD_USB_TEXT_Y     186 /* the middle of it */
#define IPOD_USB_BATTERY_X  288
#define IPOD_USB_BATTERY_Y  6
#define IPOD_USB_BATTERY_H  (BMPHEIGHT_usbbattery / 2)

/* Colours of the title bar, for each row */
static const unsigned long ipod_usb_title_bar[] = {
    0xe6e7e5, 0xe4e5e3, 0xe2e3e3, 0xdfe1e3, 0xdde1e4, 0xdadfe2, 0xd7dce0, 0xd4dade,
    0xd5dde0, 0xd5dde0, 0xd4dcdf, 0xd2dadd, 0xced7da, 0xc7d1d5, 0xc3ced4, 0xc4d0d7,
    0xc2cfd8, 0xbbc9d4, 0xb6c4d0, 0xb3c2cf, 0xb1c1cd, 0xacbcc7, 0xa7b7c0, 0x8d9b9e,
};
#define IPOD_USB_TITLE_H ((int)ARRAYLEN(ipod_usb_title_bar))

static const char *ipod_usb_name = "iPod";
static bool ipod_usb_charging;
static bool ipod_usb_ejected;
static struct viewport *ipod_usb_parent;

/* There's no ejecting in the simulator, which has no USB storage */
#ifdef SIMULATOR
#define ipod_usb_is_ejected() false
#else
#define ipod_usb_is_ejected() usb_storage_ejected()
#endif

static inline unsigned ipod_usb_colour(unsigned long rgb)
{
    return LCD_RGBPACK((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

/* While files can still be read: what's measured stays in the font's cache */
static void ipod_usb_prepare(void)
{
#ifdef HAVE_TAGCACHE
    ipod_usb_name = itunesdb_device_name();
#endif
    font_getstringsize(ipod_usb_name, NULL, NULL, FONT_UI);
    font_getstringsize(str(LANG_USB_DO_NOT_DISCONNECT), NULL, NULL, FONT_UI);
    font_getstringsize(str(LANG_USB_OK_TO_DISCONNECT), NULL, NULL, FONT_UI);
}

/* The two frames of the bitmap: charging, and on power but not charging */
static void ipod_usb_draw_battery(struct screen *screen)
{
    ipod_usb_charging = charging_state();

    for (int y = IPOD_USB_BATTERY_Y;
         y < IPOD_USB_BATTERY_Y + IPOD_USB_BATTERY_H; y++)
    {
        screen->set_foreground(ipod_usb_colour(ipod_usb_title_bar[y]));
        screen->hline(IPOD_USB_BATTERY_X,
                      IPOD_USB_BATTERY_X + BMPWIDTH_usbbattery - 1, y);
    }

    screen->transparent_bitmap_part((const fb_data *)bm_usbbattery.data, 0,
                                    ipod_usb_charging ? 0 : IPOD_USB_BATTERY_H,
                                    STRIDE(screen->screen_type,
                                           BMPWIDTH_usbbattery,
                                           BMPHEIGHT_usbbattery),
                                    IPOD_USB_BATTERY_X, IPOD_USB_BATTERY_Y,
                                    BMPWIDTH_usbbattery, IPOD_USB_BATTERY_H);
}

static void ipod_usb_draw_centred(struct screen *screen, const char *text,
                                  int middle)
{
    int width, height;
    screen->getstringsize(text, &width, &height);
    screen->putsxy((LCD_WIDTH - width) / 2, middle - height / 2, text);
}

static void ipod_usb_draw(struct screen *screen, struct viewport *parent)
{
    struct viewport *last_vp = screen->set_viewport(parent);

    ipod_usb_parent = parent;
    ipod_usb_ejected = ipod_usb_is_ejected();
    screen->backlight_on();
    screen->set_drawmode(DRMODE_SOLID);

    for (int y = 0; y < IPOD_USB_TITLE_H; y++)
    {
        screen->set_foreground(ipod_usb_colour(ipod_usb_title_bar[y]));
        screen->hline(0, LCD_WIDTH - 1, y);
    }
    screen->set_foreground(LCD_WHITE);
    screen->fillrect(0, IPOD_USB_TITLE_H, LCD_WIDTH,
                     LCD_HEIGHT - IPOD_USB_TITLE_H);

    screen->bmp(ipod_usb_ejected ? &bm_usbok : &bm_nodisconnect,
                IPOD_USB_SIGN_X, IPOD_USB_SIGN_Y);
    ipod_usb_draw_battery(screen);

    /* The letters blend with what's under them, here the title bar */
    screen->setfont(FONT_UI);
    screen->set_foreground(LCD_BLACK);
    screen->set_drawmode(DRMODE_FG);
    /* in the middle if it's clear of the battery, and otherwise from the left */
    int name_width, name_height;
    screen->getstringsize(ipod_usb_name, &name_width, &name_height);
    if (name_width <= 2 * (IPOD_USB_BATTERY_X - 4) - LCD_WIDTH)
    {
        ipod_usb_draw_centred(screen, ipod_usb_name, IPOD_USB_TITLE_H / 2);
    }
    else
    {
        struct viewport title_vp = *parent;
        title_vp.width = IPOD_USB_BATTERY_X - 4;
        screen->set_viewport(&title_vp);
        screen->set_drawmode(DRMODE_FG);
        screen->putsxy(6, IPOD_USB_TITLE_H / 2 - name_height / 2, ipod_usb_name);
        screen->set_viewport(parent);
    }
    ipod_usb_draw_centred(screen, str(ipod_usb_ejected ?
                                      LANG_USB_OK_TO_DISCONNECT :
                                      LANG_USB_DO_NOT_DISCONNECT),
                          IPOD_USB_TEXT_Y);
    screen->set_drawmode(DRMODE_SOLID);

    screen->update();
    screen->set_viewport(last_vp);
}

static void ipod_usb_update(void)
{
    struct screen *screen = &screens[SCREEN_MAIN];

    if (ipod_usb_is_ejected() != ipod_usb_ejected)
    {
        ipod_usb_draw(screen, ipod_usb_parent);
        return;
    }

    if (charging_state() == ipod_usb_charging)
        return;

    struct viewport *last_vp = screen->set_viewport(ipod_usb_parent);
    screen->set_drawmode(DRMODE_SOLID);
    ipod_usb_draw_battery(screen);
    screen->update_rect(IPOD_USB_BATTERY_X, IPOD_USB_BATTERY_Y,
                        BMPWIDTH_usbbattery, IPOD_USB_BATTERY_H);
    screen->set_viewport(last_vp);
}
#endif /* USB_IPOD_IDENTITY */

static void usb_screen_fix_viewports(struct screen *screen,
        struct usb_screen_vps_t *usb_screen_vps)
{
#ifdef USB_IPOD_IDENTITY
    /* The whole of the screen, without the theme */
    viewportmanager_theme_enable(screen->screen_type, false,
                                 &usb_screen_vps->parent);
    ipod_usb_prepare();
#ifdef USB_ENABLE_HID
    if (global_settings.usb_hid)
    {
        /* Below the text, if the keypad mode has been turned on */
        struct viewport *title = &usb_screen_vps->title;
        *title = usb_screen_vps->parent;
        int char_height = font_get(title->font)->height;
        title->y = LCD_HEIGHT - char_height;
        title->height = char_height;

        int i = 0, langid = LANG_USB_KEYPAD_MODE;
        while (langid >= 0) /* ensure the USB mode strings get cached */
        {
            font_getstringsize(str(langid), NULL, NULL, title->font);
            langid = keypad_mode_name_get(i++);
        }
    }
#endif
#else
    int logo_width, logo_height;
    struct viewport *parent = &usb_screen_vps->parent;
    struct viewport *logo = &usb_screen_vps->logo;

#ifdef HAVE_REMOTE_LCD
    if (screen->screen_type == SCREEN_REMOTE)
    {
        logo_width = BMPWIDTH_remote_usblogo;
        logo_height = BMPHEIGHT_remote_usblogo;
    }
    else
#endif
    {
        logo_width = BMPWIDTH_usblogo;
        logo_height = BMPHEIGHT_usblogo;
    }

    viewportmanager_theme_enable(screen->screen_type, true, parent);

    if (logo_width  > parent->width)
        logo_width  = parent->width;
    if (logo_height > parent->height)
        logo_height = parent->height;

    *logo = *parent;
    logo->x = parent->x + parent->width - logo_width;
#ifdef HAVE_LCD_SPLIT
    switch (statusbar_position(screen))
    {
         /* start beyond split */
         case STATUSBAR_OFF:
             logo->y = parent->y + LCD_SPLIT_POS;
             break;
         case STATUSBAR_TOP:
             logo->y = parent->y + LCD_SPLIT_POS - STATUSBAR_HEIGHT;
             break;
         /* start at the top for maximum space */
         default:
             logo->y = parent->y;
             break;
    }
#else
    logo->y = parent->y + (parent->height - logo_height) / 2;
#endif
    logo->width = logo_width;
    logo->height = logo_height;

#ifdef USB_ENABLE_HID
    if (global_settings.usb_hid)
    {
        struct viewport *title = &usb_screen_vps->title;
        int char_height = font_get(parent->font)->height;
        *title = *parent;
        title->y = logo->y + logo->height + char_height;
        title->height = char_height;
        /* try to fit logo and title to parent */
        if (parent->y + parent->height < title->y + title->height)
        {
            logo->y = parent->y;
            title->y = parent->y + logo->height;
        }

        int i =0, langid = LANG_USB_KEYPAD_MODE;
        while (langid >= 0) /* ensure the USB mode strings get cached */
        {
            font_getstringsize(str(langid), NULL, NULL, title->font);
            langid = keypad_mode_name_get(i++);
        }
    }
#endif
#endif /* USB_IPOD_IDENTITY */
}

static void usb_screens_draw(struct usb_screen_vps_t *usb_screen_vps_ar)
{
#ifdef USB_IPOD_IDENTITY
    ipod_usb_draw(&screens[SCREEN_MAIN], &usb_screen_vps_ar[SCREEN_MAIN].parent);
#else
    struct viewport *last_vp;
    static const struct bitmap* logos[NB_SCREENS] = {
        &bm_usblogo,
#ifdef HAVE_REMOTE_LCD
        &bm_remote_usblogo,
#endif
    };
    FOR_NB_SCREENS(i)
    {
        struct screen *screen = &screens[i];
        struct usb_screen_vps_t *usb_screen_vps = &usb_screen_vps_ar[i];
        struct viewport *parent = &usb_screen_vps->parent;
        struct viewport *logo = &usb_screen_vps->logo;

        last_vp = screen->set_viewport(parent);
        screen->clear_viewport();
        screen->backlight_on();
        screen->set_viewport(logo);
        screen->bmp(logos[i], 0, 0);
        screen->set_viewport(last_vp);
    }
#endif /* USB_IPOD_IDENTITY */
#ifdef USB_ENABLE_HID
    if (global_settings.usb_hid)
        draw_usb_keypad_mode(&usb_screen_vps_ar[SCREEN_MAIN].title);
#endif
}

void gui_usb_screen_run(bool early_usb, intptr_t seqnum)
{
#ifdef SIMULATOR /* the sim allows toggling USB fast enough to overflow viewportmanagers stack */
    static bool in_usb_screen = false;
    if (in_usb_screen)
        return;
    in_usb_screen = true;
#endif

    struct usb_screen_vps_t usb_screen_vps_ar[NB_SCREENS];
    struct viewport *title = NULL;
#if defined HAVE_TOUCHSCREEN
    enum touchscreen_mode old_mode = touchscreen_get_mode();

    /* TODO: Paint buttons on screens OR switch to point mode and use
     * touchscreen as a touchpad to move the host's mouse cursor */
    touchscreen_set_mode(TOUCHSCREEN_BUTTON);
#endif

    push_current_activity(ACTIVITY_USBSCREEN);

#ifdef USB_ENABLE_HID
    usb_keypad_mode = global_settings.usb_keypad_mode;
    title = &usb_screen_vps_ar[SCREEN_MAIN].title;
#else
    title = NULL;
#endif

    FOR_NB_SCREENS(i)
    {
        struct screen *screen = &screens[i];
        /* we might be coming from anywhere, and the originating screen
         * can't be practically expected to cleanup the UI because
         * we're invoked via default_event_handler(), therefore we make a
         * generic cleanup here */
        screen->set_viewport(NULL);
        screen->scroll_stop();
        usb_screen_fix_viewports(screen, &usb_screen_vps_ar[i]);
    }

#if 0 /* handled in usb_screen_fix_viewports() */
    /* update the UI before disabling fonts, this maximizes the propability
     * that font cache lookups succeed during USB */
    send_event(GUI_EVENT_ACTIONUPDATE, NULL);
#endif

    if(!early_usb)
    {
        /* The font system leaves the .fnt fd's open, so we need for force close them all */
        font_disable_all();
    }
    usb_acknowledge(SYS_USB_CONNECTED_ACK, seqnum);
    usb_screens_draw(usb_screen_vps_ar);
    handle_usb_events(title);

#if defined(USB_ENABLE_HID)
    if (global_settings.usb_hid)
        screens[SCREEN_MAIN].scroll_stop_viewport(title);
    if (global_settings.usb_keypad_mode != usb_keypad_mode)
    {
        global_settings.usb_keypad_mode = usb_keypad_mode;
        settings_save();
    }
#endif

#ifdef HAVE_TOUCHSCREEN
    touchscreen_set_mode(old_mode);
#endif

    if(!early_usb)
    {
        font_enable_all();
        /* Not pretty, reload all settings so fonts are loaded again correctly */
        settings_apply(true);
        /* Reload playlist */
        playlist_resume();
    }

    FOR_NB_SCREENS(i)
    {
        screens[i].backlight_on();
        viewportmanager_theme_undo(i, false);
    }

    pop_current_activity();
#ifdef SIMULATOR
    in_usb_screen = false;
#endif
#ifdef HAVE_TAGCACHE
    /* after a sync, the new songs before anything else, as after a boot */
    if(!early_usb && tagcache_check_sync())
        show_library_update();
#endif
}
