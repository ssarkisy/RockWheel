/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Felix Arends
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

#include <math.h>
#include <stdlib.h>         /* EXIT_SUCCESS */
#include <stdio.h>
#include "sim-ui-defines.h"
#ifdef HAVE_REMOTE_LCD
#include "lcd-remote.h"
#endif
#include "config.h"
#include "button.h"
#include "kernel.h"
#include "backlight.h"
#include "system.h"
#include "button-sdl.h"
#if SDL_MAJOR_VERSION > 1
#include "window-sdl.h"
#endif
#include "sim_tasks.h"
#include "buttonmap.h"
#include "debug.h"
#include "powermgmt.h"
#include "storage.h"

#ifdef HAVE_TOUCHSCREEN
#include "touchscreen.h"
static int mouse_coords = 0;
#endif
/* how long until repeat kicks in */
#define REPEAT_START      6

/* the speed repeat starts at */
#define REPEAT_INTERVAL_START   4

/* speed repeat finishes at */
#define REPEAT_INTERVAL_FINISH  2

#ifdef HAVE_TOUCHSCREEN
#define USB_KEY SDLK_c /* SDLK_u is taken by BUTTON_MIDLEFT */
#else
#define USB_KEY SDLK_u
#endif
#define EXT_KEY SDLK_e

#if defined(IRIVER_H100_SERIES) || defined (IRIVER_H300_SERIES)
int _remote_type=REMOTETYPE_H100_LCD;

int remote_type(void)
{
    return _remote_type;
}
#endif

static int btn = 0;    /* Hopefully keeps track of currently pressed keys... */

#ifdef SIMULATOR
#include <string.h>
#include "strlcat.h"
#include "screendump.h"
#include "thread-sdl.h"
/* root_menu() came, past the boot's waits that take keys (RBSIM_SCRIPT) */
volatile bool sim_boot_done = false;
static bool cursor_isfocus = false;
SDL_Cursor *sdl_focus_cursor = NULL;
SDL_Cursor *sdl_arrow_cursor = NULL;
#endif

int sdl_app_has_input_focus = 1;

#ifdef HAS_BUTTON_HOLD
bool hold_button_state = false;
bool button_hold(void) {
    return hold_button_state;
}
#endif

#ifdef HAS_REMOTE_BUTTON_HOLD
bool remote_hold_button_state = false;
bool remote_button_hold(void) {
    return remote_hold_button_state;
}
#endif
static void button_event(int key, bool pressed);
extern bool debug_wps;
extern bool mapping;

#ifdef HAVE_TOUCHSCREEN
static void touchscreen_event(int x, int y)
{
    if(background) {
        x -= UI_LCD_POSX;
        y -= UI_LCD_POSY;
    }

    if(x >= 0 && y >= 0 && x < SIM_LCD_WIDTH && y < SIM_LCD_HEIGHT) {
        mouse_coords = (x << 16) | y;
        button_event(BUTTON_TOUCHSCREEN, true);
        if (debug_wps)
            printf("Mouse at 1: (%d, %d)\n", x, y);
    }
}
#endif

#if defined(HAVE_SCROLLWHEEL) || ((defined(BUTTON_SCROLL_FWD) && defined(BUTTON_SCROLL_BACK)))
static void scrollwheel_event(int x, int y)
{
    int new_btn = 0;
    if (y > 0)
        new_btn = BUTTON_SCROLL_BACK;
    else if (y < 0)
        new_btn = BUTTON_SCROLL_FWD;
    else
        return;

#ifdef HAVE_BACKLIGHT
    backlight_on();
#endif
#ifdef HAVE_BUTTON_LIGHT
    buttonlight_on();
#endif
    reset_poweroff_timer();
    if (new_btn && !button_queue_full())
        button_queue_post(new_btn, 1<<24);

    (void)x;
}
#endif
static void mouse_event(SDL_MouseButtonEvent *event, bool button_up)
{
#define SQUARE(x) ((x)*(x))
    static int x,y;
#ifdef SIMULATOR
    static int xybutton = 0;
#endif

    if(button_up) {
        switch ( event->button )
        {
        case SDL_BUTTON_MIDDLE:
        case SDL_BUTTON_RIGHT:
            button_event( event->button, false );
            break;
        /* The scrollwheel button up events are ignored as they are queued immediately */
        case SDL_BUTTON_LEFT:
            if ( mapping && background ) {
                printf("    { SDLK_,     %d, %d, %d, \"\" },\n", x, y,
                (int)sqrt( SQUARE(x-(int)event->x) + SQUARE(y-(int)event->y))
                );
            }
#ifdef SIMULATOR
            if ( background && xybutton ) {
                    button_event( xybutton, false );
                    xybutton = 0;
                }
#endif
#ifdef HAVE_TOUCHSCREEN
                else
                    button_event(BUTTON_TOUCHSCREEN, false);
#endif
            break;
        }
    } else {   /* button down */
        switch ( event->button )
        {
        case SDL_BUTTON_MIDDLE:
        case SDL_BUTTON_RIGHT:
            button_event( event->button, true );
            break;
        case SDL_BUTTON_LEFT:
            if ( mapping && background ) {
                x = event->x;
                y = event->y;
            }
#ifdef SIMULATOR
            if ( background ) {
                xybutton = xy2button( event->x, event->y );
                if( xybutton ) {
                    button_event( xybutton, true );
                    break;
                }
            }
#endif
#ifdef HAVE_TOUCHSCREEN
            touchscreen_event(event->x, event->y);
#endif
            break;
        }

        if (debug_wps && event->button == SDL_BUTTON_LEFT)
        {
            int m_x, m_y;

            if ( background )
            {
                m_x = event->x - 1;
                m_y = event->y - 1;
#ifdef HAVE_REMOTE
                if ( event->y >= UI_REMOTE_POSY ) /* Remote Screen */
                {
                    m_x -= UI_REMOTE_POSX;
                    m_y -= UI_REMOTE_POSY;
                }
                else
#endif
                {
                    m_x -= UI_LCD_POSX;
                    m_y -= UI_LCD_POSY;
                }
            }
            else
            {
                m_x = event->x;
                m_y = event->y;
#ifdef HAVE_REMOTE
                if ( m_y >= LCD_HEIGHT ) /* Remote Screen */
                    m_y -= LCD_HEIGHT;
#endif
            }

            printf("Mouse at 2: (%d, %d)\n", m_x, m_y);
        }
    }
#undef SQUARE
}

static bool event_handler(SDL_Event *event)
{
#if SDL_MAJOR_VERSION > 1
    SDL_Keycode ev_key;
#else
    SDLKey ev_key;
#endif

    switch(event->type)
    {
#if SDL_MAJOR_VERSION > 1
    case SDL_WINDOWEVENT:
        if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
            sdl_app_has_input_focus = 1;
        else if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            sdl_app_has_input_focus = 0;
        else if(event->window.event == SDL_WINDOWEVENT_RESIZED)
        {
            SDL_LockMutex(window_mutex);
            sdl_window_adjustment_needed(false);
            SDL_UnlockMutex(window_mutex);
#if !defined (__APPLE__) && !defined(__WIN32)
            static unsigned long last_tick;
            if (TIME_AFTER(current_tick, last_tick + HZ/20) && !button_queue_full())
                  button_queue_post(SDLK_UNKNOWN, 0); /* update window on main thread */
            else
                  last_tick = current_tick;
#endif
        }
#endif /* SDL_MAJOR_VERSION */
#ifdef SIMULATOR
        if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST
            || event->window.event == SDL_WINDOWEVENT_LEAVE
            || event->window.event == SDL_WINDOWEVENT_RESIZED
            || event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
        {
            if (cursor_isfocus)
            {
                cursor_isfocus = false;
                SDL_SetCursor(sdl_arrow_cursor);
            }
        }
#endif
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        ev_key = event->key.keysym.sym;
        button_event(ev_key, event->type == SDL_KEYDOWN);
        break;



    case SDL_MOUSEMOTION:
    {
#ifdef SIMULATOR
        static uint32_t next_check = 0;
        if (background && sdl_app_has_input_focus
            && (TIME_AFTER(event->motion.timestamp, next_check)))
        {
            int x = event->motion.x;
            int y = event->motion.y;

            extern struct button_map bm[];
            int i;
            for (i = 0; bm[i].button; i++)
            {
                int xd = (x-bm[i].x)*(x-bm[i].x);
                int yd = (y-bm[i].y)*(y-bm[i].y);
                /* check distance from center of button < radius */
                if ( xd + yd < bm[i].radius*bm[i].radius ) {
                    break;
                }
            }

            if (bm[i].button)
            {
                if (!cursor_isfocus)
                {
                    cursor_isfocus = true;
                    SDL_SetCursor(sdl_focus_cursor);
                }
            }
            else if (cursor_isfocus)
            {
                cursor_isfocus = false;
                SDL_SetCursor(sdl_arrow_cursor);
            }

            next_check = event->motion.timestamp + 10; /* ms */
        }
#endif
#ifdef HAVE_TOUCHSCREEN
        if (event->motion.state & SDL_BUTTON(1))
        {
            int x = event->motion.x;
            int y = event->motion.y;
            touchscreen_event(x, y);
        }
#endif
        break;

    }
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEBUTTONDOWN:
    {
        SDL_MouseButtonEvent *mev = &event->button;
        mouse_event(mev, event->type == SDL_MOUSEBUTTONUP);
        break;
    }
#ifdef HAVE_SCROLLWHEEL
    case SDL_MOUSEWHEEL:
    {
        scrollwheel_event(event->wheel.x, event->wheel.y);
        break;
    }
#endif
    case SDL_QUIT:
        /* Will post SDL_USEREVENT in shutdown_hw() if successful. */
        sdl_sys_quit();
        break;
    case SDL_USEREVENT:
        return true;
        break;
    }

    return false;
}

#ifdef __APPLE__
int sdl_event_filter(void *userdata, SDL_Event * event)
{
    (void) userdata;
    event_handler(event);
    return 0;
}
#endif

void gui_message_loop(void)
{
    SDL_Event event;
    bool quit;

    do {
        /* wait for the next event */
        if(SDL_WaitEvent(&event) == 0) {
            printf("SDL_WaitEvent(): %s\n", SDL_GetError());
            return; /* error, out of here */
        }

        sim_enter_irq_handler();
        quit = event_handler(&event);
        sim_exit_irq_handler();

    } while(!quit);
}

#if defined(SIMULATOR)
static void show_sim_help(void)
{
    extern SDL_Window *sdlWindow;
    extern struct button_map bm[];

    uint32_t flags = SDL_MESSAGEBOX_INFORMATION;
    static char helptext[4096] = {'\0'};

    if (helptext[0] != '\0')
    {
        SDL_ShowSimpleMessageBox(flags, UI_TITLE, helptext, sdlWindow);
        return;
    }

    const char *keyname = SDL_GetKeyName(SDLK_HELP);
    strlcat(helptext, "Rockbox simulator\n[Key] Description\n\n[", sizeof(helptext)-1);
    /* simulator keys */
    #define HELPTXT(SDLK,litdesc)                       \
        keyname = SDL_GetKeyName(SDLK);                 \
        if (keyname[0]) {                               \
        strlcat(helptext, keyname, sizeof(helptext)-1); \
        strlcat(helptext, "] "litdesc"\n[", sizeof(helptext)-1);}
    /*HELPTXT(, "");*/
    HELPTXT(SDLK_HELP, "display this window");
    HELPTXT(SDLK_KP_0, "screendump");
    HELPTXT(SDLK_F5, "screendump");
    HELPTXT(SDLK_TAB, "hide device background");
    HELPTXT(SDLK_0, "zoom display level 1/2");
    HELPTXT(SDLK_1, "zoom display level 1");
    HELPTXT(SDLK_2, "zoom display level 2");
    HELPTXT(SDLK_3, "zoom display level 3");
    HELPTXT(SDLK_4, "toggle display quality");
    HELPTXT(USB_KEY, "toggle USB");

#ifdef HAVE_HEADPHONE_DETECTION
    HELPTXT(SDLK_p, "toggle headphone");
#endif
#ifdef HAVE_LINEOUT_DETECTION
    HELPTXT(SDLK_p, "toggle lineout");
#endif
#ifdef HAVE_HOTSWAP
    HELPTXT(EXT_KEY, "toggle external drive");
#endif
#ifdef HAS_BUTTON_HOLD
    HELPTXT(SDLK_h, "toggle hold button");
#endif
#ifdef HAS_REMOTE_BUTTON_HOLD
    HELPTXT(SDLK_j, "toggle remote hold button");
#endif
#if defined(IRIVER_H100_SERIES) || defined (IRIVER_H300_SERIES)
    HELPTXT(SDLK_t, "toggle remote type");
#endif
#ifdef HAVE_TOUCHSCREEN
    HELPTXT(SDLK_F4, "toggle touch mode");
#endif

    /* device specific keymap */
    for (int i = 0; bm[i].button; i++)
    {
        keyname = SDL_GetKeyName(bm[i].button);
        strlcat(helptext, keyname, sizeof(helptext)-1);
        strlcat(helptext, "] ", sizeof(helptext)-1);
        strlcat(helptext, bm[i].description, sizeof(helptext)-1);
        strlcat(helptext, "\n[", sizeof(helptext)-1);
    }

    /* last one doesn't use the macro */
    keyname = SDL_GetKeyName(SDLK_F1);
    strlcat(helptext, keyname, sizeof(helptext)-1);
    strlcat(helptext, "] display this window\n\n", sizeof(helptext)-1);

strlcat(helptext, "Note: If you don't have a keypad\n" \
                  "alternate keys are in uisimulator/buttonmap\n\n", sizeof(helptext)-1);

    DEBUGF("[%u] characters\n%s", (unsigned int)strlen(helptext), helptext);
    SDL_ShowSimpleMessageBox(flags, UI_TITLE, helptext, sdlWindow);
}
#endif

static void button_event(int key, bool pressed)
{
    int new_btn = 0;
    switch (key)
    {
#ifdef SIMULATOR
    case SDLK_HELP:
    case SDLK_F1:
        if (!pressed)
            show_sim_help();
        return;
    case SDLK_TAB:
        if (!pressed)
        {
            SDL_LockMutex(window_mutex);
            background = !background;
            sdl_window_adjustment_needed(true);
            SDL_UnlockMutex(window_mutex);
#if !defined(__WIN32) && !defined (__APPLE__)
            button_queue_post(SDLK_UNKNOWN, 0); /* update window on main thread */
#endif
        }
        return;
    case SDLK_0:
        display_zoom = 0.5;
    case SDLK_1:
        display_zoom = display_zoom ?: 1;
    case SDLK_2:
        display_zoom = display_zoom ?: 2;
    case SDLK_3:
        display_zoom = display_zoom ?: 3;
    case SDLK_4:
        if (pressed)
        {
            display_zoom = 0;
            return;
        }
        SDL_LockMutex(window_mutex);
        if (!display_zoom)
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
                        strcmp(SDL_GetHint(SDL_HINT_RENDER_SCALE_QUALITY) ?:
                        "best", "best") ? "best": "nearest");

        sdl_window_adjustment_needed(!display_zoom);
        SDL_UnlockMutex(window_mutex);
#if !defined(__WIN32) && !defined (__APPLE__)
        button_queue_post(SDLK_UNKNOWN, 0); /* update window on main thread */
#endif
        return;
    case USB_KEY:
        if (!pressed)
        {
            static bool usb_connected = false;
            usb_connected = !usb_connected;
            sim_trigger_usb(usb_connected);
        }
        return;
#ifdef HAVE_HEADPHONE_DETECTION
    case SDLK_p:
        if (!pressed)
        {
            static bool hp_connected = true;
            hp_connected = !hp_connected;
            sim_trigger_hp(hp_connected);
        }
        return;
#endif
#ifdef HAVE_LINEOUT_DETECTION
    case SDLK_l:
        if (!pressed)
        {
            static bool lo_connected = false;
            lo_connected = !lo_connected;
            sim_trigger_lo(lo_connected);
        }
        return;
#endif
#ifdef HAVE_HOTSWAP
    case EXT_KEY:
        if (!pressed)
            sim_trigger_external(!storage_present(1));
        return;
#endif
#endif
#ifdef RG_NANO
    case SDLK_q:
        /* Use reboot to exit without shutting down */
        sys_reboot();
        break;
#endif
#ifdef HAS_BUTTON_HOLD
    case SDLK_h:
        if(pressed)
        {
            hold_button_state = !hold_button_state;
            DEBUGF("Hold button is %s\n", hold_button_state?"ON":"OFF");
        }
        return;
#endif

#ifdef HAS_REMOTE_BUTTON_HOLD
    case SDLK_j:
        if(pressed)
        {
            remote_hold_button_state = !remote_hold_button_state;
            DEBUGF("Remote hold button is %s\n",
                   remote_hold_button_state?"ON":"OFF");
        }
        return;
#endif

#if defined(IRIVER_H100_SERIES) || defined (IRIVER_H300_SERIES)
    case SDLK_t:
        if(pressed)
            switch(_remote_type)
            {
                case REMOTETYPE_UNPLUGGED:
                    _remote_type=REMOTETYPE_H100_LCD;
                    DEBUGF("Changed remote type to H100\n");
                    break;
                case REMOTETYPE_H100_LCD:
                    _remote_type=REMOTETYPE_H300_LCD;
                    DEBUGF("Changed remote type to H300\n");
                    break;
                case REMOTETYPE_H300_LCD:
                    _remote_type=REMOTETYPE_H300_NONLCD;
                    DEBUGF("Changed remote type to H300 NON-LCD\n");
                    break;
                case REMOTETYPE_H300_NONLCD:
                    _remote_type=REMOTETYPE_UNPLUGGED;
                    DEBUGF("Changed remote type to none\n");
                    break;
            }
        break;
#endif
#ifndef APPLICATION
    case SDLK_KP_0:
    case SDLK_F5:
        if(pressed)
        {
            sim_trigger_screendump();
            return;
        }
        break;
#endif
#ifdef HAVE_TOUCHSCREEN
    case SDLK_F4:
        if(pressed)
        {
            touchscreen_set_mode(touchscreen_get_mode() == TOUCHSCREEN_POINT ? TOUCHSCREEN_BUTTON : TOUCHSCREEN_POINT);
            printf("Touchscreen mode: %s\n", touchscreen_get_mode() == TOUCHSCREEN_POINT ? "TOUCHSCREEN_POINT" : "TOUCHSCREEN_BUTTON");
        }
#endif
    default:
#ifdef HAVE_TOUCHSCREEN
# ifndef HAS_BUTTON_HOLD
        if(touchscreen_is_enabled())
# endif
            new_btn = key_to_touch(key, mouse_coords);
        if (!new_btn)
#endif
            new_btn = key_to_button(key);
#ifdef HAVE_TOUCHPAD
        new_btn = touchpad_filter(new_btn);
#endif
        break;
    }

#if defined(BUTTON_SCROLL_FWD) && defined(BUTTON_SCROLL_BACK)
    if((new_btn == BUTTON_SCROLL_FWD || new_btn == BUTTON_SCROLL_BACK) &&
        pressed)
    {
        scrollwheel_event(0, new_btn == BUTTON_SCROLL_FWD ? -1 : 1);
        new_btn &= ~(BUTTON_SCROLL_FWD | BUTTON_SCROLL_BACK);
    }
#endif

    /* Update global button press state */
    if (pressed)
        btn |= new_btn;
    else
        btn &= ~new_btn;
}
#if defined(SIMULATOR) && SDL_MAJOR_VERSION > 1
/* Scripted input for automated UI testing. RBSIM_SCRIPT=<file> holds one
 * command a line, '#' starting a comment. Times are the device's ms, which
 * RBSIM_SPEED makes pass faster than the host's.
 *
 *   <key> [<hold>]   an SDL key name: "Up" and "Down", a step of the wheel each,
 *                    "Return", "Escape", "Left", "Right", "Space"; or the
 *                    simulator's own, "U" (USB), "h" (hold)... A button is held
 *                    for RBSIM_KEYGAP ms, or <hold> (300 and longer is a long
 *                    press), then released for as long. A button or a step
 *                    first waits for the boot to reach the menu, and then, up
 *                    to 10 s of the host's, for the UI to wait for a key, the
 *                    keys before taken: none is lost to the boot's waits, nor
 *                    to a screen that loads or clears the queue as it opens,
 *                    however short the gap. "!Return" doesn't wait.
 *   <cmd>*<n> ...    a command n times: "Down*421"
 *   wheel <steps> <stage>
 *                    the click wheel turned <steps> steps, back if negative,
 *                    at its acceleration stage 0-6: each step goes 2^stage rows,
 *                    as far as the list lets it (apps/gui/list.c), as the iPod's
 *                    driver posts them (button-clickwheel.c)
 *   wait <ms>
 *   waitidle <ms> [<max>]
 *                    until the LCD had no update for <ms>, a scrolling line's
 *                    aside; <max> at most, 60000 if not given
 *   dump             a screendump as "dump 001.bmp", "dump 002.bmp"... in the
 *                    simdisk's root; after the boot, once the UI took the keys
 *                    before it ("!dump" at once)
 *   quit
 *
 * RBSIM_KEYGAP=<ms>  a button's hold, and the gap after a key: 20 if not
 *                    given. A button's are 20 at least, as the driver takes a
 *                    change seen at two ticks in a row (button.c button_read);
 *                    a step of the wheel, posted at once, has the gap only.
 * RBSIM_SPEED=<n>    the device's clock n times as fast as the host's, for
 *                    ticks, sleeps, timeouts and the music, which is silent
 *                    (kernel-sdl.c)
 */

/* The script's press, read by the ticks after script_press_tick, as many as
   script_press_ticks: however late its thread runs, a press lasts that long */
static int script_press_btn;
static long script_press_tick;
static int script_press_ticks;

static int script_pressed(void)
{
    if (script_press_btn && TIME_AFTER(current_tick, script_press_tick)
        && !TIME_AFTER(current_tick, script_press_tick + script_press_ticks))
        return script_press_btn;
    return 0;
}
#else
#define script_pressed() 0
#endif

#if defined(HAVE_BUTTON_DATA)
int button_read_device(int* data)
{
#if defined(HAVE_TOUCHSCREEN)
    *data = mouse_coords;
#else
    (void) *data;   /* suppress compiler warnings */
#endif
#else
int button_read_device(void)
{
#endif
#ifdef HAS_BUTTON_HOLD
    int hold_button = button_hold();

#ifdef HAVE_BACKLIGHT
    /* light handling */
    static int hold_button_old = false;
    if (hold_button != hold_button_old)
    {
        hold_button_old = hold_button;
        backlight_hold_changed(hold_button);
    }
#endif

    if (hold_button)
        return BUTTON_NONE;
    else
#endif

    return btn | script_pressed();
}

#if defined(SIMULATOR) && SDL_MAJOR_VERSION > 1
static char *script_buf;
static long script_stack[DEFAULT_STACK_SIZE/sizeof(long)];
static int script_keygap = 20; /* ms */
static int script_dumps;

extern bool sim_button_queue_waited(void);     /* button_queue.c */
extern volatile long sim_lcd_update_tick;      /* lcd-bitmap.c */

#define SCRIPT_TICKS(ms) ((ms) / (1000/HZ))

static void script_sleep_until(long tick)
{
    while (TIME_BEFORE(current_tick, tick))
        sleep(tick - current_tick);
}

/* a host's ms off the kernel, for what's polled: a tick is 10 ms at 1x */
static void script_poll(void)
{
    void *me = sim_thread_unlock();
    SDL_Delay(1);
    sim_thread_lock(me);
}

/* Until the boot is done and the UI waits for a key, the keys before taken.
   Ten seconds of the host's at most, as the UI may be busy that long on its
   CPU, which RBSIM_SPEED doesn't hasten. */
static void script_wait_ui(void)
{
    while (!sim_boot_done)
        script_poll();
    const Uint32 start = SDL_GetTicks();
    while (!sim_button_queue_waited())
    {
        if (SDL_GetTicks() - start > 10000)
        {
            printf("RBSIM_SCRIPT: the UI took no key for 10 s, going on\n");
            return;
        }
        script_poll();
    }
}

static void script_key(SDL_Keycode key, int hold_ms, bool now)
{
    const int b = key_to_button(key);
    if (b && !now)
        script_wait_ui();

    if (b & (BUTTON_SCROLL_FWD | BUTTON_SCROLL_BACK))
    {
        /* a step of the wheel, posted at once */
        button_event(key, true);
        button_event(key, false);
        script_sleep_until(current_tick + SCRIPT_TICKS(script_keygap));
    }
    else if (b)
    {
        /* seen at two ticks in a row, a change is taken (button.c) */
        const int hold = MAX(SCRIPT_TICKS(hold_ms), 2);
        const int gap = MAX(SCRIPT_TICKS(script_keygap), 2);
        /* no tick reads it half set */
        const int oldlevel = disable_irq_save();
        script_press_tick = current_tick;
        script_press_ticks = hold;
        script_press_btn = b;
        restore_irq(oldlevel);
        script_sleep_until(script_press_tick + hold + gap);
    }
    else
    {
        /* the simulator's own, as USB or hold: on the press or the release */
        button_event(key, true);
        script_sleep_until(current_tick + MAX(SCRIPT_TICKS(hold_ms), 1));
        button_event(key, false);
        script_sleep_until(current_tick + MAX(SCRIPT_TICKS(script_keygap), 1));
    }
}

#ifdef HAVE_WHEEL_ACCELERATION
/* steps of the click wheel as its driver posts them: the first of a turn
   without BUTTON_REPEAT, and each at the stage's rows */
static void script_wheel(int steps, int stage, bool now)
{
    const int b = steps < 0 ? BUTTON_SCROLL_BACK : BUTTON_SCROLL_FWD;
    stage = MAX(0, MIN(stage, 6));
    for (int i = 0; i < abs(steps); i++)
    {
        if (!now)
            script_wait_ui();
#ifdef HAVE_BACKLIGHT
        backlight_on();
#endif
        reset_poweroff_timer();
        button_queue_post(b | (i ? BUTTON_REPEAT : 0),
                          (1u << 31) | (1u << 24) | WHEEL_DATA_STAGED
                          | (1u << stage));
        script_sleep_until(current_tick + SCRIPT_TICKS(script_keygap));
    }
}
#endif

/* until the LCD had no update for so long, a scrolling line's aside */
static void script_wait_idle(int ms, int max_ms)
{
    const long quiet = MAX(SCRIPT_TICKS(ms), 1);
    const long until = current_tick + SCRIPT_TICKS(max_ms);
    while (TIME_BEFORE(current_tick, sim_lcd_update_tick + quiet))
    {
        if (!TIME_BEFORE(current_tick, until))
        {
            printf("RBSIM_SCRIPT: waitidle %d: the LCD still updates after %d ms\n",
                   ms, max_ms);
            return;
        }
        script_poll();
    }
}

/* one command; false to end the script */
static bool script_command(const char *cmd, int argc, int a, int b)
{
    /* "!Escape": at once, the UI ready or not */
    const bool now = cmd[0] == '!';
    if (now)
        cmd++;

    if (!strcmp(cmd, "wait"))
        sleep(SCRIPT_TICKS(a));
    else if (!strcmp(cmd, "waitidle"))
        script_wait_idle(a, argc >= 3 ? b : 60000);
    else if (!strcmp(cmd, "dump"))
    {
        /* the boot's screens as they come */
        if (sim_boot_done && !now)
            script_wait_ui();
        sim_screen_dump_numbered(++script_dumps);
    }
#ifdef HAVE_WHEEL_ACCELERATION
    else if (!strcmp(cmd, "wheel"))
        script_wheel(a, argc >= 3 ? b : 0, now);
#endif
    else if (!strcmp(cmd, "quit"))
    {
        SDL_Event ev = { .type = SDL_QUIT };
        SDL_PushEvent(&ev);
        return false;
    }
    else
    {
        SDL_Keycode key = SDL_GetKeyFromName(cmd);
        if (key == SDLK_UNKNOWN)
        {
            printf("RBSIM_SCRIPT: unknown key '%s'\n", cmd);
            return true;
        }
        script_key(key, argc >= 2 ? a : script_keygap, now);
    }
    return true;
}

static void script_thread(void)
{
    char *save;
    for (char *line = strtok_r(script_buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save))
    {
        char cmd[64];
        int a = 0, b = 0;
        const int argc = sscanf(line, "%63s %d %d", cmd, &a, &b);
        if (argc < 1 || cmd[0] == '#')
            continue;

        /* "Down*421": that many times */
        int times = 1;
        char *star = strchr(cmd, '*');
        if (star)
        {
            *star = '\0';
            times = atoi(star + 1);
        }
        for (int i = 0; i < times; i++)
            if (!script_command(cmd, argc, a, b))
                return;
    }
}

static void script_init(void)
{
    const char *path = getenv("RBSIM_SCRIPT");
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f)
        return;

    size_t size = 0, room = 4096;
    script_buf = malloc(room);
    while (script_buf)
    {
        size += fread(script_buf + size, 1, room - 1 - size, f);
        if (size < room - 1)
            break;
        char *bigger = realloc(script_buf, room *= 2);
        if (!bigger)
            free(script_buf);
        script_buf = bigger;
    }
    fclose(f);
    if (!script_buf)
        return;
    script_buf[size] = '\0';

    const char *gap = getenv("RBSIM_KEYGAP");
    if (gap && *gap)
        script_keygap = MAX(atoi(gap), 0);
    printf("RBSIM_SCRIPT: %s, keys %d ms apart\n", path, script_keygap);

    create_thread(script_thread, script_stack, sizeof(script_stack), 0,
                  "sim script" IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU));
}
#endif

void button_init_device(void)
{
#ifdef SIMULATOR
    if (!sdl_focus_cursor)
        sdl_focus_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    if (!sdl_arrow_cursor)
        sdl_arrow_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
#if SDL_MAJOR_VERSION > 1
    script_init();
#endif
#endif
}
