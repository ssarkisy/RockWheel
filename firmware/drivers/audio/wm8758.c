/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Driver for WM8758 audio codec - based on datasheet for WM8983
 *
 * Based on code from the ipodlinux project - http://ipodlinux.org/
 * Adapted for Rockbox in December 2005
 *
 * Original file: linux/arch/armnommu/mach-ipod/audio.c
 *
 * Copyright (c) 2003-2005 Bernard Leach (leachbj@bouncycastle.org)
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

#include "system.h"
#include "kernel.h"
#include "string.h"
#include "audio.h"

#include "wmcodec.h"
#include "audiohw.h"
#include "sound.h"
#include "dac_standby.h"
#ifdef HAVE_DAC_STANDBY
#include "pcm.h"
#include "pcm-internal.h"
#endif

#ifdef HAVE_DAC_STANDBY
/* Standby, its order and the check again under the lock after ProkBox5g's
   (prokopto-dev), written anew: the soft mute, the outputs, the mixers and
   the DAC, then the PLL go off; bias and VMID stay, and an output that is off
   is held at VMID through 30k (OUTCTRL_VROI), so that it doesn't move. */

/* PWRMGMT1-3 as asked for at [1]-[3], DACCTRL at [0]: what standby turns off
   is left off in what is written meanwhile, and put back from these */
static unsigned short pwr_reg[4];
#define DACCTRL_SHADOW 0

#define STANDBY_PWR1_OFF  PWRMGMT1_PLLEN
#define STANDBY_PWR2_OFF  (PWRMGMT2_LOUT1EN | PWRMGMT2_ROUT1EN)
#define STANDBY_PWR3_OUT2 (PWRMGMT3_LOUT2EN | PWRMGMT3_ROUT2EN)
#define STANDBY_PWR3_OFF  (STANDBY_PWR3_OUT2 | PWRMGMT3_LMIXEN | PWRMGMT3_RMIXEN \
                           | PWRMGMT3_DACENL | PWRMGMT3_DACENR)

/* after PCM stopped, which the mixer does after 3 s of its own silence */
#define DAC_IDLE_TICKS (5 * HZ)
/* for the PLL to lock, as ProkBox waits */
#define DAC_PLL_LOCK_USEC 2000

static struct mutex dac_mtx;
/* from audiohw_postinit(): the power thread runs before the codec is set up,
   and its mutex made */
static bool dac_ready;
static bool standby;
static bool analog_in; /* line-in or the remote's radio, through the mixers */
static bool kept_awake; /* dac_standby_keep_awake(): the USB DAC */
static long busy_tick; /* PCM last seen playing, or a start on its way */
/* in the sleep: 7 mA less there, measured on an iPod Video, and no click
   heard as it wakes */
#ifndef DAC_STANDBY_DEFAULT
#define DAC_STANDBY_DEFAULT DAC_STANDBY_SLEEP
#endif
static enum dac_standby_mode standby_mode = DAC_STANDBY_DEFAULT;

static unsigned short standby_value(int reg, unsigned short val)
{
    switch (reg)
    {
        case PWRMGMT1:
            return val & ~STANDBY_PWR1_OFF;
        case PWRMGMT2:
            return val & ~STANDBY_PWR2_OFF;
        case PWRMGMT3:
            return val & ~STANDBY_PWR3_OFF;
        default: /* DACCTRL */
            return val | DACCTRL_SOFTMUTE;
    }
}

/* PWRMGMT1-3 and DACCTRL are written here only */
static void pwr_write(int reg, unsigned short val)
{
    mutex_lock(&dac_mtx);
    pwr_reg[reg == DACCTRL ? DACCTRL_SHADOW : reg] = val;
    wmcodec_write(reg, standby ? standby_value(reg, val) : val);
    mutex_unlock(&dac_mtx);
}

/* under dac_mtx */
static void standby_enter(void)
{
    standby = true;
    wmcodec_write(DACCTRL, pwr_reg[DACCTRL_SHADOW] | DACCTRL_SOFTMUTE);
    wmcodec_write(PWRMGMT2, pwr_reg[2] & ~STANDBY_PWR2_OFF);
    wmcodec_write(PWRMGMT3, pwr_reg[3] & ~STANDBY_PWR3_OUT2);
    wmcodec_write(PWRMGMT3, pwr_reg[3] & ~STANDBY_PWR3_OFF);
    wmcodec_write(PWRMGMT1, pwr_reg[1] & ~STANDBY_PWR1_OFF);
}

/* under dac_mtx: in the reverse order */
static void standby_exit(void)
{
    wmcodec_write(PWRMGMT1, pwr_reg[1]);
    udelay(DAC_PLL_LOCK_USEC);
    wmcodec_write(PWRMGMT3, pwr_reg[3] & ~STANDBY_PWR3_OUT2);
    wmcodec_write(PWRMGMT3, pwr_reg[3]);
    wmcodec_write(PWRMGMT2, pwr_reg[2]);
    wmcodec_write(DACCTRL, pwr_reg[DACCTRL_SHADOW]);
    standby = false;
}

void dac_standby_set_mode(enum dac_standby_mode mode)
{
    standby_mode = mode;
    if (mode == DAC_STANDBY_OFF)
        dac_wake();
}

bool dac_in_standby(void)
{
    return standby;
}

static bool dac_busy(void)
{
    return pcm_is_playing() || analog_in
#ifdef HAVE_RECORDING
        || pcm_is_recording()
#endif
        || kept_awake;
}

void dac_standby_keep_awake(bool on)
{
    kept_awake = on;
    if (on)
        dac_wake();
}

void dac_idle_poll(bool sleeping)
{
    if (!dac_ready)
        return;
    if (dac_busy())
        busy_tick = current_tick;
    if (standby || standby_mode == DAC_STANDBY_OFF
        || (standby_mode == DAC_STANDBY_SLEEP && !sleeping)
        || TIME_BEFORE(current_tick, busy_tick + DAC_IDLE_TICKS))
        return;

    mutex_lock(&dac_mtx);
    /* again, as a wake may have come while the lock was waited for */
    if (!standby && !dac_busy()
        && !TIME_BEFORE(current_tick, busy_tick + DAC_IDLE_TICKS))
        standby_enter();
    mutex_unlock(&dac_mtx);
}

void dac_wake(void)
{
    if (!dac_ready)
        return;
    /* the start on its way: pcm_play_data() waits on I2C before it sets
       pcm_playing, and the power thread may look meanwhile */
    busy_tick = current_tick;
    /* Awake, as it always is with standby off: no lock, which the USB DAC's
       start from its interrupt couldn't take. Threads take turns only where
       one yields: the power thread checks busy_tick again under the lock and
       sets standby before its first I2C write, so this sees standby set or
       has kept it off. */
    if (!standby)
        return;
    /* asleep, from an interrupt: can't be (the USB DAC keeps it awake from
       its thread first: dac_standby_keep_awake()), nor waited on I2C for */
    if (!is_thread_context())
        return;
    mutex_lock(&dac_mtx);
    if (standby)
        standby_exit();
    mutex_unlock(&dac_mtx);
}
#else /* !HAVE_DAC_STANDBY */
#define pwr_write(reg, val) wmcodec_write(reg, val)
#endif /* HAVE_DAC_STANDBY */

/* shadow registers */
static unsigned short eq1_reg = EQ1_EQ3DMODE | EQ_GAIN_VALUE(0);
static unsigned short eq5_reg = EQ_GAIN_VALUE(0);

/* convert tenth of dB volume (-89..6) to master volume register value */
static int vol_tenthdb2hw(int db)
{
    /*   att  DAC  AMP  result
        +6dB    0   +6     96
         0dB    0    0     90
       -57dB    0  -57     33
       -58dB   -1  -57     32
       -89dB  -32  -57      1
       -90dB  -oo  -oo      0 */
    if (db <= -900) {
        return 0;
    } else {
        return db / 10 - -90;
    }
}

/* helper function that calculates the register setting for amplifier and
   DAC volume out of the input from tenthdb2master() */
static void get_volume_params(int db, int *dac, int *amp)
{
    /* should never happen, set max volume for amp and dac */
    if      (db > 96) {
        *dac = 255;
        *amp = 63;
    }
    /* set dac to max and set volume for amp (better snr) */
    else if (db > 32) {
        *dac = 255;
        *amp = (db-90)+57;
    }
    /* set amp to min and reduce dac output */
    else if (db >  0) {
        *dac = (db-33)*2 + 255;
        *amp = 0;
    }
    /* mute all */
    else {
        *dac = 0x00;
        *amp = 0x40;
    }
}

static void audiohw_mute(bool mute)
{
    if (mute) {
        pwr_write(DACCTRL, DACCTRL_SOFTMUTE);
    } else {
        pwr_write(DACCTRL, DACCTRL_DACOSR128);
    }
}

/* A pair of the codec's volume registers, written only if one of them changes: each
   write waits on the I2C bus, giving the processor away, and a step of the volume
   wrote six. The right one, with its update bit, puts both in effect. */
struct volume_pair
{
    int left, right;
};

static void write_volume_pair(struct volume_pair *last, int reg_l, int val_l,
                              int reg_r, int val_r)
{
    if (last->left == val_l && last->right == val_r)
        return;
    wmcodec_write(reg_l, val_l);
    wmcodec_write(reg_r, val_r);
    last->left = val_l;
    last->right = val_r;
}

static struct volume_pair dac_last = { -1, -1 };
static struct volume_pair out1_last = { -1, -1 };
static struct volume_pair out2_last = { -1, -1 };

void audiohw_preinit(void)
{
#ifdef HAVE_DAC_STANDBY
    mutex_init(&dac_mtx);
#endif
    /* Set low bias mode */
    wmcodec_write(BIASCTRL, BIASCTRL_BIASCUT);
    /* Enable HPCOM, LINECOM */
    wmcodec_write(OUTCTRL, OUTCTRL_HP_COM | OUTCTRL_LINE_COM
                         | OUTCTRL_TSOPCTRL | OUTCTRL_TSDEN | OUTCTRL_VROI);
    /* Mute all Outputs and set PGAs minimum gain */
    wmcodec_write(LOUT1VOL, 0x140);
    wmcodec_write(ROUT1VOL, 0x140);
    wmcodec_write(LOUT2VOL, 0x140);
    wmcodec_write(ROUT2VOL, 0x140);
    /* what the volume pairs now hold, the DAC's unknown */
    out1_last = out2_last = (struct volume_pair){ 0x140, 0x140 };
    dac_last = (struct volume_pair){ -1, -1 };
    wmcodec_write(OUT3MIX,  0x40);
    wmcodec_write(OUT4MIX,  0x40);
    /* Enable L/ROUT1 */
    pwr_write(PWRMGMT2, PWRMGMT2_ROUT1EN | PWRMGMT2_LOUT1EN);
    /* Enable VMID independent current bias */
    wmcodec_write(OUT4TOADC, OUT4TOADC_POBCTRL);
    /* Enable required DACs and mixers */
    pwr_write(PWRMGMT3, PWRMGMT3_RMIXEN | PWRMGMT3_LMIXEN
                      | PWRMGMT3_DACENR | PWRMGMT3_DACENL);
    /* Enable VMIDSEL, BIASEN, BUFIOEN */
    pwr_write(PWRMGMT1, PWRMGMT1_PLLEN | PWRMGMT1_BIASEN
                      | PWRMGMT1_BUFIOEN | PWRMGMT1_VMIDSEL_10K);
    /* Setup digital interface, input amplifiers, PLL, ADCs and DACs */
    wmcodec_write(AINTFCE, AINTFCE_IWL_16BIT | AINTFCE_FORMAT_I2S);
    wmcodec_write(CLKCTRL, CLKCTRL_MS); /* WM8758 is clock master */

    audiohw_set_frequency(HW_FREQ_44);

    wmcodec_write(LOUTMIX, LOUTMIX_DACL2LMIX);
    wmcodec_write(ROUTMIX, ROUTMIX_DACR2RMIX);
    /* Disable VMID independent current bias */
    wmcodec_write(OUT4TOADC, 0);
}

void audiohw_postinit(void)
{
    pwr_write(PWRMGMT1, PWRMGMT1_PLLEN | PWRMGMT1_BIASEN
                      | PWRMGMT1_BUFIOEN | PWRMGMT1_VMIDSEL_500K);
                          /* lower the VMID power consumption */
    wmcodec_write(BIASCTRL, 0);
    audiohw_mute(false);
#ifdef HAVE_DAC_STANDBY
    dac_ready = true;
#endif
}

void audiohw_set_volume(int vol_l, int vol_r)
{
    int dac_l, amp_l, dac_r, amp_r;

    vol_l = vol_tenthdb2hw(vol_l);
    vol_r = vol_tenthdb2hw(vol_r);

    get_volume_params(vol_l, &dac_l, &amp_l);
    get_volume_params(vol_r, &dac_r, &amp_r);

    /* set DAC
       Important: DAC is global and will also affect lineout */
    write_volume_pair(&dac_last, LDACVOL, dac_l, RDACVOL, dac_r | RDACVOL_DACVU);

    /* set headphone amp OUT1 */
    write_volume_pair(&out1_last, LOUT1VOL, amp_l | LOUT1VOL_LOUT1ZC,
                      ROUT1VOL, amp_r | ROUT1VOL_ROUT1ZC | ROUT1VOL_OUT1VU);
}

void audiohw_set_lineout_volume(int vol_l, int vol_r)
{
    int dac_l, amp_l, dac_r, amp_r;

    vol_l = vol_tenthdb2hw(vol_l);
    vol_r = vol_tenthdb2hw(vol_r);

    get_volume_params(vol_l, &dac_l, &amp_l);
    get_volume_params(vol_r, &dac_r, &amp_r);

    /* set lineout amp OUT2 */
    write_volume_pair(&out2_last, LOUT2VOL, amp_l | LOUT2VOL_LOUT2ZC,
                      ROUT2VOL, amp_r | ROUT2VOL_ROUT2ZC | ROUT2VOL_OUT2VU);
}

void audiohw_enable_lineout(bool enable)
{
    /* Initialize data without lineout enabling. */
    int pwrmgmt3_data  = PWRMGMT3_RMIXEN  | PWRMGMT3_LMIXEN
                       | PWRMGMT3_DACENR  | PWRMGMT3_DACENL;
    /* Set lineout (OUT2), if enabled. */
    if (enable)
        pwrmgmt3_data |= PWRMGMT3_LOUT2EN | PWRMGMT3_ROUT2EN;

    /* Set register. */
    pwr_write(PWRMGMT3, pwrmgmt3_data);
}

void audiohw_set_bass(int value)
{
    eq1_reg = (eq1_reg & ~EQ_GAIN_MASK) | EQ_GAIN_VALUE(value);
    wmcodec_write(EQ1, eq1_reg);
}

void audiohw_set_bass_cutoff(int value)
{
    eq1_reg = (eq1_reg & ~EQ_CUTOFF_MASK) | EQ_CUTOFF_VALUE(value);
    wmcodec_write(EQ1, eq1_reg);
}

void audiohw_set_treble(int value)
{
    eq5_reg = (eq5_reg & ~EQ_GAIN_MASK) | EQ_GAIN_VALUE(value);
    wmcodec_write(EQ5, eq5_reg);
}

void audiohw_set_treble_cutoff(int value)
{
    eq5_reg = (eq5_reg & ~EQ_CUTOFF_MASK) | EQ_CUTOFF_VALUE(value);
    wmcodec_write(EQ5, eq5_reg);
}

/* Nice shutdown of WM8758 codec */
void audiohw_close(void)
{
#ifdef HAVE_DAC_STANDBY
    /* from standby too, the way it always went: its outputs on as VMID
       falls, and no standby again meanwhile */
    standby_mode = DAC_STANDBY_OFF;
    dac_wake();
#endif
    audiohw_mute(true);

    /* Disable Thermal shutdown */
    wmcodec_write(OUTCTRL, OUTCTRL_HP_COM | OUTCTRL_VROI);
    /* Enable VMIDTOG */
    wmcodec_write(OUT4TOADC, OUT4TOADC_VMIDTOG);
    /* Disable VMIDSEL and BUFIOEN */
    pwr_write(PWRMGMT1, PWRMGMT1_PLLEN | PWRMGMT1_BIASEN
                      | PWRMGMT1_VMIDSEL_OFF);
    /* Wait for VMID to discharge */
    sleep(3*HZ/10);
    /* Power off registers */
    pwr_write(PWRMGMT2, 0);
    pwr_write(PWRMGMT3, 0);
    pwr_write(PWRMGMT1, 0);
}

/* Note: Disable output before calling this function */
void audiohw_set_frequency(int fsel)
{
    /* CLKCTRL_MCLKDIV_MASK and ADDCTRL_SR_MASK don't overlap,
       so they can both fit in one byte.  Bit 0 selects PLL
       configuration via pll_setups.
     */
    static const unsigned char freq_setups[HW_NUM_FREQ] =
    {
        [HW_FREQ_48] = CLKCTRL_MCLKDIV_2 | ADDCTRL_SR_48kHz | 1,
        [HW_FREQ_44] = CLKCTRL_MCLKDIV_2 | ADDCTRL_SR_48kHz,
        [HW_FREQ_32] = CLKCTRL_MCLKDIV_3 | ADDCTRL_SR_32kHz | 1,
        [HW_FREQ_24] = CLKCTRL_MCLKDIV_4 | ADDCTRL_SR_24kHz | 1,
        [HW_FREQ_22] = CLKCTRL_MCLKDIV_4 | ADDCTRL_SR_24kHz,
        [HW_FREQ_16] = CLKCTRL_MCLKDIV_6 | ADDCTRL_SR_16kHz | 1,
        [HW_FREQ_12] = CLKCTRL_MCLKDIV_8 | ADDCTRL_SR_12kHz | 1,
        [HW_FREQ_11] = CLKCTRL_MCLKDIV_8 | ADDCTRL_SR_12kHz,
        [HW_FREQ_8] = CLKCTRL_MCLKDIV_12 | ADDCTRL_SR_8kHz | 1
    };

    /* Each PLL configuration is an array consisting of
       { PLLN, PLLK1, PLLK2, PLLK3 }.  The WM8983 datasheet requires
       5 < PLLN < 13, and states optimum is PLLN = 8, f2 = 90 MHz
     */
    static const unsigned short pll_setups[2][4] =
    {
        /* f1 = 12 MHz, R = 7.5264, f2 = 90.3168 MHz, fPLLOUT = 22.5792 MHz */
        { PLLN_PLLPRESCALE | 0x7, 0x21, 0x161, 0x26 },
        /* f1 = 12 MHz, R = 8.192, f2 = 98.304 MHz, fPLLOUT = 24.576 MHz */
        { PLLN_PLLPRESCALE | 0x8, 0xC, 0x93, 0xE9 }
    };

    int i;

    /* PLLN, PLLK1, PLLK2, PLLK3 are contiguous (at 0x24 to 0x27) */
    for (i = 0; i < 4; i++)
        wmcodec_write(PLLN + i, pll_setups[freq_setups[fsel] & 1][i]);

    /* CLKCTRL_MCLKDIV divides fPLLOUT to get SYSCLK (256 * sample rate) */
    wmcodec_write(CLKCTRL, CLKCTRL_CLKSEL
                         | (freq_setups[fsel] & CLKCTRL_MCLKDIV_MASK)
                         | CLKCTRL_BCLKDIV_2 | CLKCTRL_MS);

    /* set ADC and DAC filter characteristics according to sample rate */
    wmcodec_write(ADDCTRL, (freq_setups[fsel] & ADDCTRL_SR_MASK)
                         | ADDCTRL_SLOWCLKEN);
    /* SLOWCLK enabled for zero cross timeout to work */
}

void audiohw_enable_recording(bool source_mic)
{
    (void)source_mic; /* We only have a line-in (I think) */

#ifdef HAVE_DAC_STANDBY
    /* the input goes through the mixers without PCM: no standby meanwhile */
    analog_in = true;
    dac_wake();
#endif
    pwr_write(PWRMGMT2, PWRMGMT2_ROUT1EN | PWRMGMT2_LOUT1EN
                      | PWRMGMT2_INPGAENR | PWRMGMT2_INPGAENL
                      | PWRMGMT2_ADCENR | PWRMGMT2_ADCENL);

    wmcodec_write(INCTRL, INCTRL_R2_2INPGA | INCTRL_L2_2INPGA);

    wmcodec_write(LADCBOOST, LADCBOOST_L2_2BOOST(5));
    wmcodec_write(RADCBOOST, RADCBOOST_R2_2BOOST(5));

    /* Enable monitoring */
    wmcodec_write(LOUTMIX, LOUTMIX_BYP2LMIXVOL(5)
                         | LOUTMIX_BYPL2LMIX | LOUTMIX_DACL2LMIX);
    wmcodec_write(ROUTMIX, ROUTMIX_BYP2RMIXVOL(5)
                         | ROUTMIX_BYPR2RMIX | ROUTMIX_DACR2RMIX);
}

void audiohw_disable_recording(void)
{
    wmcodec_write(LOUTMIX, LOUTMIX_DACL2LMIX);
    wmcodec_write(ROUTMIX, ROUTMIX_DACR2RMIX);

    pwr_write(PWRMGMT2, PWRMGMT2_ROUT1EN | PWRMGMT2_LOUT1EN);
#ifdef HAVE_DAC_STANDBY
    analog_in = false;
#endif
}

/* volume in 0 .. 63, corresponds to -12dB .. +35.25dB in 0.75dB steps */
void audiohw_set_recvol(int left, int right, int type)
{
    switch (type)
    {
    case AUDIO_GAIN_MIC:
        right = left;
        /* fall through */
    case AUDIO_GAIN_LINEIN:
        wmcodec_write(LINPGAVOL, LINPGAVOL_INPGAZCL
                               | (left & LINPGAVOL_INPGAVOL_MASK));
        wmcodec_write(RINPGAVOL, RINPGAVOL_INPGAVU | RINPGAVOL_INPGAZCR
                               | (right & RINPGAVOL_INPGAVOL_MASK));
        break;
    default:
        return;
    }
}

void audiohw_set_monitor(bool enable)
{
    (void)enable;
}
