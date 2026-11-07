/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2007 Copyright Kévin Ferrare based on Zakk Roberts's work
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
#include "clock_draw.h"
#include "clock_settings.h"

static int max_skin[]={
    [ANALOG]=2,
    [BINARY]=3,
    [DIGITAL]=2,
};

enum settings_file_status{
    LOADED, ERRLOAD,
    SAVED, ERRSAVE
};

struct clock_settings clock_settings;
bool show_counter;

/* The settings as they exist on the hard disk, so that
 * we can know at saving time if changes have been made */
struct clock_settings hdd_clock_settings;

static bool settings_needs_saving(struct clock_settings* settings){
    return(rb->memcmp(settings, &hdd_clock_settings, sizeof(*settings)));
}

void clock_settings_reset(struct clock_settings* settings){
    settings->mode = ANALOG;
    settings->general.date_format = EUROPEAN;
    settings->skin[ANALOG] = 1; /* round */
    settings->skin[DIGITAL] = 1; /* LCD-style */
    settings->skin[BINARY] = 2; /* LCD-style */
    for (const char *ptr = rb->str(LANG_VOICED_DATE_FORMAT) ; *ptr; ptr++)
    {
        if (*ptr == 'd')
            break;
        else if (*ptr == 'Y')
        {
            settings->general.date_format = JAPANESE;
            break;
        }
        else if (*ptr == 'A' || *ptr == 'm')
        {
            settings->general.date_format = ENGLISH;
            break;
        }
    }
    settings->general.save_settings = true;
    settings->general.idle_poweroff=true;
    settings->general.backlight = ROCKBOX_SETTING;

    settings->analog.show_date = false;
    settings->analog.show_seconds = true;
    settings->analog.show_border = true;

    settings->digital.show_seconds = true;
    settings->digital.blinkcolon = false;
    apply_backlight_setting(settings->general.backlight);
}

void apply_backlight_setting(int backlight_setting)
{
    if(backlight_setting == ALWAS_OFF)
        rb->backlight_set_timeout(-1);
    else if(backlight_setting == ROCKBOX_SETTING)
        rb->backlight_set_timeout(rb->global_settings->backlight_timeout);
    else if(backlight_setting == ALWAYS_ON)
        rb->backlight_set_timeout(0);
}

void clock_settings_skin_next(struct clock_settings* settings){
    settings->skin[settings->mode]++;
    if(settings->skin[settings->mode]>=max_skin[settings->mode])
        settings->skin[settings->mode]=0;
}

void clock_settings_skin_previous(struct clock_settings* settings){
    settings->skin[settings->mode]--;
    if(settings->skin[settings->mode]<0)
        settings->skin[settings->mode]=max_skin[settings->mode]-1;
}

static enum settings_file_status clock_settings_load(
        struct clock_settings* settings,char* filename){
    int fd = rb->open(filename, O_RDONLY);
    if(fd >= 0)
    {
        /* basic consistency check */
        if(rb->ffilesize(fd) != sizeof(*settings))
            rb->close(fd);
        else
        {
            rb->read(fd, settings, sizeof(*settings));
            rb->close(fd);
            apply_backlight_setting(settings->general.backlight);
            rb->memcpy(&hdd_clock_settings, settings, sizeof(*settings));
            return(LOADED);
        }
    }
    /* Initializes the settings with default values at least */
    clock_settings_reset(settings);
    return(ERRLOAD);
}

static enum settings_file_status clock_settings_save(
        struct clock_settings* settings, char* filename){
    int fd = rb->creat(filename, 0666);
    if(fd >= 0){ /* does file exist? */
        rb->write (fd, settings, sizeof(*settings));
        rb->close(fd);
        return(SAVED);
    }
    return(ERRSAVE);
}

/* A settings file that won't load or save passes in silence: the clock goes on with
   its defaults, as nothing on the screen could help. The English pictures that said
   so ("Error loading settings!" over a "clock" logo) are gone with it. */
void load_settings(void)
{
    if (rb->file_exists(settings_filename))
        clock_settings_load(&clock_settings, settings_filename);
    else
        clock_settings_reset(&clock_settings);
    rb->storage_sleep();
}

void save_settings(void)
{
    if (settings_needs_saving(&clock_settings))
        clock_settings_save(&clock_settings, settings_filename);
}

void save_settings_wo_gui(void){
    clock_settings_save(&clock_settings, settings_filename);
}
