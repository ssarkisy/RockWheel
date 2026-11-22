/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 Miika Pekkarinen
 * Copyright (C) 2005 Magnus Holmgren
 * Copyright (C) 2007 Thom Johansen
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
#ifndef DSP_MISC_H
#define DSP_MISC_H

/* Set the tri-pdf dithered output */
void dsp_dither_enable(bool enable); /* in dsp_sample_output.c */
void dsp_dither_set_light(bool light); /* in dsp_sample_output.c */

/* Lighter while the UI animates: no EQ bands and no dither in what's decoded
   then, which the decoder keeps up with on a fraction of the CPU */
void dsp_set_light(bool light);

/* Sound Check's On is TRACK, Off OFF, Albums SHUFFLE (apps/settings_list.c),
   in this order since On and Off were the setting's two values: a new order
   is the plugins' too (PLUGIN_API_REVISION) */
enum replaygain_types
{
    REPLAYGAIN_TRACK = 0, /* the track's gain, the album's where it has none */
    REPLAYGAIN_OFF,
    REPLAYGAIN_ALBUM,     /* the album's gain, the track's where it has none */
    REPLAYGAIN_SHUFFLE,   /* ALBUM, or TRACK while songs are shuffled one by
                             one (apps/misc.c) */
};

struct replaygain_settings
{
    bool noclip; /* scale to prevent clips */
    int type;    /* enum replaygain_types: the setting's SHUFFLE is given
                    to the DSP as TRACK or ALBUM (replaygain_update()) */
    int preamp;  /* scale replaygained tracks by this */
};

/* Structure used with REPLAYGAIN_SET_GAINS message */
#define REPLAYGAIN_SET_GAINS (DSP_PROC_SETTING+DSP_PROC_MISC_HANDLER)
struct dsp_replay_gains
{
    long track_gain;
    long album_gain;
    long track_peak;
    long album_peak;
};

void dsp_replaygain_set_settings(const struct replaygain_settings *settings);

#ifdef HAVE_PITCHCONTROL
void dsp_set_pitch(int32_t pitch);
int32_t dsp_get_pitch(void);
#endif /* HAVE_PITCHCONTROL */

/* Set output samplerate for all DSPs */
void dsp_set_all_output_frequency(unsigned int samplerate);

/* Return DSP's output samplerate */
struct dsp_config;
unsigned int dsp_get_output_frequency(struct dsp_config *dsp);

#endif /* DSP_MISC_H */
