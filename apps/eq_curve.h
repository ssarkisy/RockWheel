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
#ifndef _EQ_CURVE_H_
#define _EQ_CURVE_H_

#include <stdint.h>
#include <stdbool.h>
#include "eq.h"

/* The software EQ's response, as the DSP's biquads have it at 44.1 kHz, worked
   out from the bands' settings, in hundredths of a dB: for the editor's curve
   and for the precut of every EQ. Fixed point only, so it also runs on a host. */

/* The points, log-spaced from 20 Hz to 20 kHz: the Video's plot's columns one to
   one; the precut is found over them whatever the screen */
#define EQ_CURVE_POINTS 300

/* the response of each band at each point */
struct eq_curve {
    int16_t cdb[EQ_NUM_BANDS][EQ_CURVE_POINTS];
};

/* the points' frequencies, worked out once */
void eq_curve_init(void);
/* a band's row of c from its settings; flat for a gain of 0, as the DSP leaves it out */
void eq_curve_set_band(struct eq_curve *c, int band,
                       const struct eq_band_setting *s);
/* all the bands at a point */
int eq_curve_total(const struct eq_curve *c, int point);
/* the largest the bands of c reach together: at the points, and at each band's own
   frequency, where a narrow one peaks between two points */
int eq_curve_peak(const struct eq_curve *c, const struct eq_band_setting *bands);
/* the same for bands without a curve made of them: a preset chosen from a list */
int eq_curve_peak_of(const struct eq_band_setting *bands);
/* The precut, in tenths of a dB, that keeps that peak from clipping, as the iTunes
   presets' table in eq_menu.c has it: whole dB up from the peak, as the volume makes
   up for it in steps of one, and less than 0.1 dB over a whole dB let pass */
int eq_curve_precut(int peak_cdb);

/* whether the DSP plays the band as the curve shows it: false where its fixed-point
   coefficients would overflow, which a very low Q at a high frequency does */
bool eq_curve_band_fits(const struct eq_band_setting *s);

/* where a frequency is along the points, 0 (20 Hz) to 65536 (20 kHz) */
int eq_curve_position(int hz);

#endif /* _EQ_CURVE_H_ */
