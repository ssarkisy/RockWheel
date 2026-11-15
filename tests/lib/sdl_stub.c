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

/* The SDL that warble plays through, for the tests' warble, which only
   writes WAV files: without the library, which on macOS (sdl2-compat) can
   stop a process at its start with a dialog. Playing fails at SDL_Init(). */

#include <stdint.h>

int SDL_Init(uint32_t flags)
{
    (void)flags;
    return -1;
}

const char *SDL_GetError(void)
{
    return "no audio in the host tests";
}

void SDL_Quit(void)
{
}

void *SDL_CreateSemaphore(uint32_t value)
{
    (void)value;
    return 0;
}

int SDL_SemPost(void *sem)
{
    (void)sem;
    return -1;
}

int SDL_SemWait(void *sem)
{
    (void)sem;
    return -1;
}

int SDL_OpenAudio(void *desired, void *obtained)
{
    (void)desired;
    (void)obtained;
    return -1;
}

void SDL_PauseAudio(int pause_on)
{
    (void)pause_on;
}
