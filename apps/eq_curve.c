/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * The response maths after podbox's apps-ipod/screens/settings/eq_curve.c
 * (Anthony Fletcher, https://github.com/anthonyfletcher/podbox), GPLv2 or
 * later.
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
#include <stdint.h>
#include <stdbool.h>
#ifdef ROCKBOX
#include "config.h"
#endif
#include "fixedpoint.h"
#include "eq_curve.h"

/* The response comes from each band's settings through the cookbook biquads the
   DSP makes of them (dsp_filter.c), not from its coefficients. With phi =
   sin^2(w/2) at the point and sigma = sin^2(w0/2) at the band, both divided by
   the larger of the two (sn, pn; their ratio is t), each filter's squared
   magnitude is a ratio of terms none of which cancels, so that 32 Hz is as
   exact as 8 kHz:

     peak        [(1-t)^2 + W A^2/Q^2] / [(1-t)^2 + W/(A^2 Q^2)]
     low shelf   A^2 [(A sn - u pn)^2 + W A/Q^2] / [(sn - v pn)^2 + W A/Q^2]
     high shelf  A^4 times the low shelf at 1/A

   where W = (1-sigma)(1-phi) t, u = 1 + (A-1) sigma, v = A - (A-1) sigma and
   A = 10^(gain/40). Numbers are s.30 in 64 bits unless named otherwise. */

#define EQ_CURVE_FS 44100
#define ONE30       (1LL << 30)
#define LN1000_16   452709          /* ln(1000), s15.16 */
#define PRECUT_MAX  240             /* the precut setting's top, tenths */

/* |a| < 2^33 and |b| <= 2^30 */
static int64_t mul30(int64_t a, int64_t b)
{
    return (a * b) >> 30;
}

/* a >= 0 of any size, 0 <= b <= 2^30 */
static int64_t mul30_wide(int64_t a, int64_t b)
{
    return (a >> 30) * b + (((a & (ONE30 - 1)) * b) >> 30);
}

/* |x| < 2^33 */
static int64_t sq30(int64_t x)
{
    x >>= 2;
    return (x * x) >> 26;
}

/* sin(w/2) of a frequency in Hz, s15.16, as s0.31 */
static int32_t sin_half(int64_t hz16)
{
    long cos;
    return fp_sincos((unsigned long)((hz16 << 15) / EQ_CURVE_FS), &cos);
}

/* 10^(gain/40) of a gain in tenths of a dB */
static int64_t gain_factor30(int gain)
{
    return (int64_t)fp_factor(((long)gain << 16) / 20, 16) << 14;
}

/* the DSP's filters go no higher than this */
static int hz_limit(int hz)
{
    const int top = EQ_CURVE_FS * 49 / 100;
    return hz < 1 ? 1 : hz > top ? top : hz;
}

/* a band made ready to be evaluated at many points */
struct band_pre
{
    bool flat;
    bool peak;
    int32_t ssig;               /* sin(w0/2), s0.31 */
    int64_t one_m_sig;          /* 1 - sigma */
    int64_t a;                  /* A; 1/A for a high shelf */
    int64_t g, h;               /* peak: A^2/Q^2 and 1/(A^2 Q^2) */
    int64_t k, u, v;            /* shelf: A/Q^2, u, v */
    int half_cdb;               /* shelf: gain/2, hundredths of a dB */
};

static void band_prepare(struct band_pre *p, const struct eq_band_setting *s)
{
    p->flat = s->gain == 0;
    if (p->flat)
        return;

    const int q = s->q ? s->q : 1;    /* as the DSP reads it */
    const int64_t invq2 = (100LL << 16) / (q * q);      /* 1/Q^2, s.16 */

    p->ssig = sin_half((int64_t)hz_limit(s->cutoff) << 16);
    const int64_t sig = ((int64_t)p->ssig * p->ssig) >> 32;
    p->one_m_sig = ONE30 - sig;
    p->peak = s->type == EQ_FILTER_PEAK;
    p->a = gain_factor30(s->gain);
    p->half_cdb = s->gain * 5;

    if (p->peak)
    {
        const int64_t a2 = sq30(p->a);
        p->g = (a2 * invq2) >> 16;
        p->h = (((ONE30 << 30) / a2) * invq2) >> 16;
    }
    else
    {
        if (s->type == EQ_FILTER_HIGH_SHELF)
            p->a = (ONE30 << 30) / p->a;
        p->k = (p->a * invq2) >> 16;
        p->u = ONE30 + mul30(p->a - ONE30, sig);
        p->v = p->a - mul30(p->a - ONE30, sig);
    }
}

/* the band at a point of that sin(w/2), hundredths of a dB */
static int band_eval(const struct band_pre *p, int32_t sphi)
{
    if (p->flat)
        return 0;

    const int32_t lo = sphi < p->ssig ? sphi : p->ssig;
    const int32_t hi = sphi < p->ssig ? p->ssig : sphi;
    const int64_t phi = ((int64_t)sphi * sphi) >> 32;
    const int64_t r = ((int64_t)lo << 30) / (hi > 0 ? hi : 1);
    const int64_t t = (r * r) >> 30;
    const int64_t w = mul30(mul30(p->one_m_sig, ONE30 - phi), t);
    int64_t num, den;

    if (p->peak)
    {
        const int64_t base = sq30(ONE30 - t);
        num = base + mul30_wide(p->g, w);
        den = base + mul30_wide(p->h, w);
    }
    else
    {
        const int64_t sn = sphi >= p->ssig ? t : ONE30;
        const int64_t pn = sphi >= p->ssig ? ONE30 : t;
        const int64_t kw = mul30_wide(p->k, w);
        num = sq30(mul30(p->a, sn) - mul30(p->u, pn)) + kw;
        den = sq30(sn - mul30(p->v, pn)) + kw;
    }

    int64_t ratio = (num << 16) / (den > 0 ? den : 1);
    if (ratio < 1)
        ratio = 1;
    if (ratio > 0x7fffffff)
        ratio = 0x7fffffff;
    /* 10 log10(x) = 4.3429 ln(x); 27795 / 2^22 is 4.3429 / 65536 * 100 */
    const int cdb = (int)(((int64_t)fp16_log((int)ratio) * 27795 + (1 << 21)) >> 22);
    return p->peak ? cdb : p->half_cdb + cdb;
}

/* sin(w/2) at each point */
static int32_t point_sin[EQ_CURVE_POINTS];

static int64_t point_hz16(int point)
{
    return 20LL * fp16_exp(point * LN1000_16 / (EQ_CURVE_POINTS - 1));
}

void eq_curve_init(void)
{
    if (point_sin[EQ_CURVE_POINTS - 1])
        return;
    const int64_t top = (int64_t)hz_limit(EQ_CURVE_FS) << 16;
    for (int i = 0; i < EQ_CURVE_POINTS; i++)
    {
        const int64_t hz16 = point_hz16(i);
        point_sin[i] = sin_half(hz16 < top ? hz16 : top);
    }
}

int eq_curve_position(int hz)
{
    if (hz <= 20)
        return 0;
    const int64_t pos = (int64_t)fp16_log(hz * 65536 / 20) * 65536 / LN1000_16;
    return pos > 65536 ? 65536 : (int)pos;
}

void eq_curve_set_band(struct eq_curve *c, int band,
                       const struct eq_band_setting *s)
{
    struct band_pre p;
    eq_curve_init();
    band_prepare(&p, s);
    for (int i = 0; i < EQ_CURVE_POINTS; i++)
    {
        const int v = band_eval(&p, point_sin[i]);
        c->cdb[band][i] = v > 32767 ? 32767 : v < -32767 ? -32767 : v;
    }
}

int eq_curve_total(const struct eq_curve *c, int point)
{
    int sum = 0;
    for (int b = 0; b < EQ_NUM_BANDS; b++)
        sum += c->cdb[b][point];
    return sum;
}

/* the bands made ready, out of the stack: only the UI's thread works these out */
static struct band_pre pre[EQ_NUM_BANDS];

/* the bands together at each band's own frequency, and at the points too with
   all_points, from peak on */
static int peak_over(const struct eq_band_setting *bands, bool all_points, int peak)
{
    for (int b = 0; b < EQ_NUM_BANDS; b++)
        band_prepare(&pre[b], &bands[b]);
    const int first = all_points ? 0 : EQ_CURVE_POINTS;
    for (int i = first; i < EQ_CURVE_POINTS + EQ_NUM_BANDS; i++)
    {
        int32_t sphi;
        if (i < EQ_CURVE_POINTS)
            sphi = point_sin[i];
        else if (bands[i - EQ_CURVE_POINTS].gain == 0)
            continue;
        else
            sphi = sin_half((int64_t)hz_limit(bands[i - EQ_CURVE_POINTS].cutoff) << 16);
        int sum = 0;
        for (int b = 0; b < EQ_NUM_BANDS; b++)
            sum += band_eval(&pre[b], sphi);
        if (sum > peak)
            peak = sum;
    }
    return peak;
}

int eq_curve_peak(const struct eq_curve *c, const struct eq_band_setting *bands)
{
    int peak = -32767;
    for (int i = 0; i < EQ_CURVE_POINTS; i++)
    {
        const int sum = eq_curve_total(c, i);
        if (sum > peak)
            peak = sum;
    }
    return peak_over(bands, false, peak);
}

int eq_curve_peak_of(const struct eq_band_setting *bands)
{
    eq_curve_init();
    return peak_over(bands, true, -32767);
}

/* filter_pk_coefs() holds alpha = sin(w0)/2Q as s1.30, and b0 = 1 + alpha A and
   a0 = 1 + alpha/A as s3.28, all before it divides by a0; the shelves share the
   alpha. Past those the DSP plays another filter than the curve shows: at Q 0.1
   and 0.2 above a few kHz, off by up to 60 dB, where every band that passes this
   test is within 0.5 dB of the cookbook's response (checked on the editor's grid of
   frequencies, gains and Qs at 44.1 and 48 kHz) */
static bool band_fits_at(const struct eq_band_setting *s, unsigned int fs)
{
    long cos;
    const int q = s->q ? s->q : 1;
    const int32_t sw0 = fp_sincos((unsigned long)(((int64_t)s->cutoff << 32) / fs), &cos);
    if (sw0 < 0)
        return false;
    const int64_t alpha = ((int64_t)sw0 * 10 / (2 * q)) >> 1;
    if (alpha >= 2 * ONE30)
        return false;
    if (s->type != EQ_FILTER_PEAK)
        return true;
    const int64_t a = gain_factor30(s->gain);
    const int64_t amax = a >= ONE30 ? a : (ONE30 << 30) / a;
    return ONE30 + (((alpha >> 2) * (amax >> 2)) >> 26) < 8 * ONE30;
}

bool eq_curve_band_fits(const struct eq_band_setting *s)
{
    if (s->gain == 0)
        return true;
    /* the rates the DSP's output can have here */
    return band_fits_at(s, 44100)
#if defined(HAVE_PLAY_FREQ) || !defined(ROCKBOX)
        && band_fits_at(s, 48000)
#endif
        ;
}

int eq_curve_precut(int peak_cdb)
{
    if (peak_cdb <= 0)
        return 0;
    int db = peak_cdb / 100;
    if (peak_cdb % 100 >= 10)
        db++;
    return db * 10 < PRECUT_MAX ? db * 10 : PRECUT_MAX;
}
