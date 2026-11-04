/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006-2007 Thom Johansen
 * Copyright (C) 2012 Michael Sevakis
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
#include "platform.h"
#include "fixedpoint.h"
#include "fracmul.h"
#include "dsp_filter.h"
#include "dsp_proc_entry.h"
#include "dsp_core.h"
#include "dsp_misc.h"
#include "eq.h"
#include "pga.h"
#include "replaygain.h"
#include <string.h>

/**
 * Current setup is one lowshelf filters eight peaking filters and one
 *  highshelf filter. Varying the number of shelving filters make no sense,
 *  but adding peaking filters is possible. Check EQ_NUM_BANDS to have
 *  2 shelving filters and EQ_NUM_BANDS-2 peaking filters.
 */

#if EQ_NUM_BANDS < 3
/* No good. Expect at least 1 peaking and low/high shelving filters */
#error Band count must be greater than or equal to 3
#endif

/* Cached band settings */
static struct eq_band_setting settings[EQ_NUM_BANDS];

static struct eq_state
{
    uint32_t enabled;                        /* Mask of enabled bands */
    uint8_t bands[EQ_NUM_BANDS+1];           /* Indexes of enabled bands */
    struct dsp_filter filters[EQ_NUM_BANDS]; /* Data for each filter */
} eq_data IBSS_ATTR;

#define FOR_EACH_ENB_BAND(b) \
    for (uint8_t *b = eq_data.bands; *b < EQ_NUM_BANDS; b++)

/* While the UI animates, as Cover Flow's covers roll, what's decoded then goes
   without the bands, the precut kept: the decoder keeps up on a fraction of the
   CPU it took. Into that and out of it, a crossfade of EQ_FADE samples, 46 ms,
   over as many passes of the DSP as it takes, each EQ_PIECE at most at a time. */
#define EQ_FADE_SHIFT 11
#define EQ_FADE (1 << EQ_FADE_SHIFT)
#define EQ_PIECE 192
static bool eq_light;          /* asked for */
static int eq_wet = EQ_FADE;   /* how much of the filtered there is, of EQ_FADE */
static int32_t eq_dry[2][EQ_PIECE];
static int32_t eq_last[2][2]; /* the last two samples that came in, of each channel */

/* The filters as if they had let the sound through as it is, rather than
   from nothing: only what they change comes in then, at their own pace. From
   nothing, a narrow band in the bass took 50 ms to settle, as long as the
   crossfade, and thumped. */
static void eq_warm(unsigned int channels)
{
    FOR_EACH_ENB_BAND(b)
    {
        struct dsp_filter *f = &eq_data.filters[*b];
        filter_flush(f);
        for (unsigned int ch = 0; ch < channels && ch < 2; ch++)
        {
#if defined(CPU_ARM) && !defined(CPU_ARM_MICRO)
            /* the assembly keeps the channels' the other way round, and a
               peak's as its correction, which is none */
            int32_t *h = f->history[channels - 1 - ch];
            if (f->peak)
            {
                h[0] = eq_last[ch][0]; /* x[i - 2] */
                h[1] = eq_last[ch][1]; /* x[i - 1] */
                continue;
            }
#else
            int32_t *h = f->history[ch];
#endif
            h[0] = h[2] = eq_last[ch][1]; /* x[i - 1], y[i - 1] */
            h[1] = h[3] = eq_last[ch][0]; /* x[i - 2], y[i - 2] */
        }
    }
}

/* the last two samples of what comes in, as it comes */
static void eq_keep_last(struct dsp_buffer *buf, int count, unsigned int channels)
{
    for (unsigned int ch = 0; ch < channels && ch < 2; ch++)
    {
        const int32_t *d = buf->p32[ch];
        eq_last[ch][0] = count > 1 ? d[count - 2] : eq_last[ch][1];
        eq_last[ch][1] = d[count - 1];
    }
}

/* Clear histories of all enabled bands */
static void eq_flush(void)
{
    if (eq_data.enabled == 0)
        return; /* Not initialized yet/no bands on */

    FOR_EACH_ENB_BAND(b)
        filter_flush(&eq_data.filters[*b]);
}

static void update_band_filter(int band, unsigned int fout)
{
    /* Convert user settings to format required by coef generator
       functions */
    typeof (filter_pk_coefs) *coef_gen = filter_pk_coefs;

    const struct eq_band_setting *setting = &settings[band];
    struct dsp_filter *filter = &eq_data.filters[band];

    switch (setting->type)
    {
    case EQ_FILTER_LOW_SHELF:
        coef_gen = filter_ls_coefs;
        break;
    case EQ_FILTER_PEAK:
        coef_gen = filter_pk_coefs;
        break;
    case EQ_FILTER_HIGH_SHELF:
        coef_gen = filter_hs_coefs;
        break;
    }

    const uint8_t was_peak = filter->peak;
    coef_gen(fp_div(setting->cutoff, fout, 32), setting->q ?: 1,
             setting->gain, filter);
    /* a peak's history is another's to the ARM filter: from nothing then */
    if (filter->peak != was_peak)
        filter_flush(filter);
}

/* Resync all bands to a new DSP output frequency */
static void update_samplerate(unsigned int fout)
{
    if (eq_data.enabled == 0)
        return; /* Not initialized yet/no bands on */

    FOR_EACH_ENB_BAND(b)
        update_band_filter(*b, fout);
}

/** DSP interface **/

/* Set the precut gain value */
void dsp_set_eq_precut(int precut)
{
    pga_set_gain(PGA_EQ_PRECUT, get_replaygain_int(precut * -10));
}

/* Update the filter configuration for the band */
void dsp_set_eq_coefs(int band, const struct eq_band_setting *setting)
{
    if (band < 0 || band >= EQ_NUM_BANDS)
        return;

    settings[band] = *setting; /* cache setting */

    struct dsp_config *dsp = dsp_get_config(CODEC_IDX_AUDIO);

    /* NOTE: The coef functions assume the EMAC unit is in fractional mode,
       which it should be, since we're executed from the main thread. */

    uint32_t mask = eq_data.enabled;

    /* Assume a band is disabled if the gain is zero */
    mask &= ~BIT_N(band);

    if (setting->gain != 0)
    {
        mask |= BIT_N(band);
        update_band_filter(band, dsp_get_output_frequency(dsp));
    }

    if (mask == eq_data.enabled)
        return; /* No change in band-enable state */

    if (mask & BIT_N(band))
        filter_flush(&eq_data.filters[band]); /* Coming online */

    eq_data.enabled = mask;

    /* Only be active if there are bands to process - if EQ is off, then
       this call has no effect */
    dsp_proc_activate(dsp, DSP_PROC_EQUALIZER, mask != 0);
  
    /* Prepare list of enabled bands for efficient iteration */
    for (band = 0; mask != 0; mask &= mask - 1, band++)
        eq_data.bands[band] = (uint8_t)find_first_set_bit(mask);

    eq_data.bands[band] = EQ_NUM_BANDS;
}

/* Enable or disable the equalizer */
void dsp_eq_enable(bool enable)
{
    struct dsp_config *dsp = dsp_get_config(CODEC_IDX_AUDIO);
    bool enabled = dsp_proc_enabled(dsp, DSP_PROC_EQUALIZER);

    if (enable == enabled)
        return;

    dsp_proc_enable(dsp, DSP_PROC_EQUALIZER, enable);

    if (enable && eq_data.enabled != 0)
        dsp_proc_activate(dsp, DSP_PROC_EQUALIZER, true);
}

void dsp_eq_set_light(bool light)
{
    eq_light = light;
}

/* After a seek or a change of track, the sound starts again: it's as asked at
   once, the filters from nothing */
static void eq_light_reset(void)
{
    eq_wet = eq_light ? 0 : EQ_FADE;
    memset(eq_last, 0, sizeof(eq_last));
}

static void eq_filter(int32_t * const p[2], int count, unsigned int channels)
{
    FOR_EACH_ENB_BAND(b)
        filter_process(&eq_data.filters[*b], p, count, channels);
}

/* n samples of the crossfade, from the filtered to the dry or back, a sample of
   EQ_FADE further each */
static void eq_crossfade(int32_t * const p[2], int n, unsigned int channels, int step)
{
    for (unsigned int ch = 0; ch < channels && ch < 2; ch++)
    {
        int32_t *d = p[ch];
        int wet = eq_wet;
        for (int i = 0; i < n; i++)
        {
            wet += step;
            d[i] = eq_dry[ch][i] +
                   (int32_t)((((int64_t)d[i] - eq_dry[ch][i]) * wet) >> EQ_FADE_SHIFT);
        }
    }
    eq_wet += step * n;
}

/* Apply EQ filters to those bands that have got it switched on. */
static void eq_process(struct dsp_proc_entry *this,
                       struct dsp_buffer **buf_p)
{
    struct dsp_buffer *buf = *buf_p;
    int count = buf->remcount;
    unsigned int channels = buf->format.num_channels;
    const int target = eq_light ? 0 : EQ_FADE;

    if (eq_wet == EQ_FADE && target == EQ_FADE)
    {
        eq_filter(buf->p32, count, channels);
        return;
    }
    if (count <= 0)
        return;
    if (eq_wet == 0 && target != 0)
        eq_warm(channels); /* under the dry, from what came before */
    eq_keep_last(buf, count, channels);

    /* the crossfade, as much of it as there is here */
    int done = 0;
    while (done < count && eq_wet != target)
    {
        const int step = target > eq_wet ? 1 : -1;
        const int n = MIN(MIN(count - done, EQ_PIECE), (target - eq_wet) * step);
        int32_t * const p[2] = { buf->p32[0] + done,
                                 channels > 1 ? buf->p32[1] + done : NULL };
        for (unsigned int ch = 0; ch < channels && ch < 2; ch++)
            memcpy(eq_dry[ch], p[ch], n * sizeof (int32_t));
        eq_filter(p, n, channels);
        eq_crossfade(p, n, channels, step);
        done += n;
    }
    /* and after it, the filtered or the dry */
    if (done < count && eq_wet == EQ_FADE)
    {
        int32_t * const p[2] = { buf->p32[0] + done,
                                 channels > 1 ? buf->p32[1] + done : NULL };
        eq_filter(p, count - done, channels);
    }

    (void)this;
}

/* DSP message hook */
static intptr_t eq_configure(struct dsp_proc_entry *this,
                             struct dsp_config *dsp,
                             unsigned int setting,
                             intptr_t value)
{
    switch (setting)
    {
    case DSP_PROC_INIT:
        this->process = eq_process;
        /* Wouldn't have been getting frequency updates */
        update_samplerate(dsp_get_output_frequency(dsp));
        /* Fall-through */
    case DSP_PROC_CLOSE:
        pga_enable_gain(PGA_EQ_PRECUT, setting == DSP_PROC_INIT);
        eq_light_reset();
        break;

    case DSP_FLUSH:
        eq_flush();
        eq_light_reset();
        break;

    case DSP_SET_OUT_FREQUENCY:
        update_samplerate(value);
        break;
    }

    return 0;
    (void)dsp;
}

/* Database entry */
DSP_PROC_DB_ENTRY(EQUALIZER,
                  eq_configure);
