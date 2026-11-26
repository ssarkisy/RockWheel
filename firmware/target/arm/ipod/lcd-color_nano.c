/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Rockbox driver for iPod LCDs
 *
 * Based on code from the ipodlinux project - http://ipodlinux.org/
 * Adapted for Rockbox in November 2005
 *
 * Original file: linux/arch/armnommu/mach-ipod/fb.c
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
#include "config.h"
#include "cpu.h"
#include "lcd.h"
#include "kernel.h"
#include "system.h"
#include "hwcompat.h"
#include "backlight-target.h"
#include "backlight.h"
#include "hang.h"

/* LCD command codes for HD66789R */
#define LCD_CNTL_RAM_ADDR_SET           0x21
#define LCD_CNTL_WRITE_TO_GRAM          0x22
#define LCD_CNTL_HORIZ_RAM_ADDR_POS     0x44
#define LCD_CNTL_VERT_RAM_ADDR_POS      0x45

#if CONFIG_LCD == LCD_IPODCOLOR
/* lcd_type 0 and 2: the panel is portrait, 176 by 220. It refreshes its
   lines one after another, each one of our columns, from our right edge
   leftwards. Register 0x18 sets the order it takes pixels in:
   - ROWS, as lcd_init_device() sets it: along our rows. Each row crossed the
     refresh, so a picture changing showed in diagonal pieces for a frame;
   - COLUMNS, as Apple's firmware sets it for each update: along the panel's
     lines, each of our columns top to bottom, from the right one leftwards.
     The refresh then shows every column whole, the old one or the new. */
#define LCD_TYPE0_ENTRY_MODE            0x18
#define LCD_TYPE0_ENTRY_ROWS            0x6
#define LCD_TYPE0_ENTRY_COLUMNS         0x0
#endif

/*** globals ***/
int lcd_type = 1; /* 0,2 = "old" Color/Photo; 1,3 = similar to HD66789R */

#ifdef HAVE_LCD_SLEEP
/* lcd_type 0's panel off: by lcd_sleep(), as the backlight thread asks while
   the iPod sleeps; on by lcd_awake(). Nothing is sent to it while it's off.
   lcd_shutdown() sets it for type 2 too; types 1 and 3 never have it. Read
   on either core: uncached */
static bool lcd_off SHAREDBSS_ATTR;
/* the updates sent, for lcd_awake_redrawn()'s wait; under lcd_cl */
static unsigned int lcd_updates SHAREDBSS_ATTR;
#if NUM_CORES > 1
/* the panel's power changes between the commands of no update, from either
   core: mpegplayer's video thread updates from the COP */
static struct corelock lcd_cl SHAREDBSS_ATTR;
#endif

#if NUM_CORES > 1 && !defined(BOOTLOADER)
/* With interrupts off, as panicf, UIE and the HANG screen draw (on the CPU),
   the other core's update is waited for 50 ms at most, then the panel drawn
   all the same, as the Video's BCM is taken (lcd_take_bcm()): a COP stuck
   in an update would hold the lock for good. Only the CPU's crash screen
   sets this, and only an unlock there acts on it: any other took the lock
   as always and gives it back. */
static bool lcd_lock_skipped;

static inline bool lcd_crash_drawing(void)
{
    return !irq_enabled() && CURRENT_CORE == CPU;
}
#endif

static inline void lcd_lock(void)
{
#if NUM_CORES > 1
#ifndef BOOTLOADER
    if (lcd_crash_drawing())
    {
        const unsigned long start = USEC_TIMER;
        while (!corelock_try_lock(&lcd_cl))
        {
            if (USEC_TIMER - start > 50000)
            {
                lcd_lock_skipped = true;
                return;
            }
        }
        return;
    }
#endif
    corelock_lock(&lcd_cl);
#endif
}

static inline void lcd_unlock(void)
{
#if NUM_CORES > 1
#ifndef BOOTLOADER
    if (lcd_lock_skipped && lcd_crash_drawing())
    {
        lcd_lock_skipped = false;
        return;
    }
#endif
    corelock_unlock(&lcd_cl);
#endif
}
#endif /* HAVE_LCD_SLEEP */

static inline void lcd_wait_write(void)
{
    while (LCD2_PORT & LCD2_BUSY_MASK);
}

static void lcd_cmd_data(unsigned cmd, unsigned data)
{
    if ((lcd_type&1) == 0) {  /* 16 bit transfers */
        lcd_wait_write();
        LCD2_PORT = LCD2_CMD_MASK | cmd;
        lcd_wait_write();
        LCD2_PORT = LCD2_CMD_MASK | data;
    } else {
        lcd_wait_write();
        LCD2_PORT = LCD2_CMD_MASK;
        LCD2_PORT = LCD2_CMD_MASK | cmd;
        lcd_wait_write();
        LCD2_PORT = LCD2_DATA_MASK | (data >> 8);
        LCD2_PORT = LCD2_DATA_MASK | (data & 0xff);
    }
}

/*** hardware configuration ***/

#ifdef HAVE_LCD_CONTRAST
void lcd_set_contrast(int val)
{
  /* TODO: Implement lcd_set_contrast() */
  (void)val;
}
#endif

#ifdef HAVE_LCD_INVERT
void lcd_set_invert_display(bool yesno)
{
#ifdef IPOD_NANO    /* this has only been tested on the ipod nano */
    lcd_cmd_data(0x07, 0x73 | (yesno ? 0 : (1<<2)));
#endif
}
#endif

#ifdef HAVE_LCD_FLIP
/* turn the display upside down (call lcd_update() afterwards) */
void lcd_set_flip(bool yesno)
{
  /* TODO: Implement lcd_set_flip() */
  (void)yesno;
}
#endif

#if defined(HAVE_LCD_SHUTDOWN) || defined(HAVE_LCD_SLEEP)
/* lcd_type 0 and 2: the panel off, as Apple's firmware turns it off (in its
   1.x osos at 0x169b68, the same sequence) */
static void lcd_type0_off(void)
{
    lcd_cmd_data(0x00EF, 0x0000);
    lcd_cmd_data(0x0080, 0x0000); udelay(1000);
    lcd_cmd_data(0x0001, 0x0001);
}
#endif

/* LCD init */
void lcd_init_device(void)
{
#if defined(HAVE_LCD_SLEEP) && NUM_CORES > 1
    corelock_init(&lcd_cl);
#endif
#if CONFIG_LCD == LCD_IPODCOLOR
    if (IPOD_HW_REVISION == 0x60000) {
        lcd_type = 0;
    } else {
        lcd_type = (GPIOA_INPUT_VAL & 0x2) | ((GPIOA_INPUT_VAL & 0x10) >> 4);
    }
    if ((lcd_type&1) == 0) {
        lcd_cmd_data(0xef, 0x0);
        lcd_cmd_data(0x01, 0x0);
        lcd_cmd_data(0x80, 0x1);
        lcd_cmd_data(0x10, 0xc);
        lcd_cmd_data(0x18, 0x6);
        lcd_cmd_data(0x7e, 0x4);
        lcd_cmd_data(0x7e, 0x5);
        lcd_cmd_data(0x7f, 0x1);
    }
#elif CONFIG_LCD == LCD_IPODNANO
    /* iPodLinux doesn't appear have any LCD init code for the Nano */
#endif
}

#ifdef HAVE_LCD_SHUTDOWN
void lcd_shutdown(void) {
    /* Immediately switch off the backlight to avoid flashing. */
#if defined(IPOD_NANO)
    _backlight_hw_enable(false);
#elif defined(IPOD_COLOR)
    backlight_hw_off();
#endif

    if ((lcd_type&1) == 0) {
        /* lcd_type 0 and 2 */
#ifdef HAVE_LCD_SLEEP
        /* once, if lcd_sleep() hasn't already; and no update after */
        lcd_lock();
        if (!lcd_off)
            lcd_type0_off();
        lcd_off = true;
        lcd_unlock();
#else
        lcd_type0_off();
#endif
    } else if (lcd_type == 1) {
        /* lcd_type 1 */
        lcd_cmd_data(0x0007, 0x0236); sleep( 40*HZ/1000);
        lcd_cmd_data(0x0007, 0x0226); sleep( 40*HZ/1000);
        lcd_cmd_data(0x0007, 0x0204);
        lcd_cmd_data(0x0010, 0x7574); sleep(200*HZ/1000);
        lcd_cmd_data(0x0010, 0x7504); sleep( 50*HZ/1000);
        lcd_cmd_data(0x0010, 0x0501);
    } else {
        /* lcd_type 3 */
        lcd_cmd_data(0x0007, 0x4016); sleep( 20*HZ/1000);
        lcd_cmd_data(0x0059, 0x0011); sleep( 20*HZ/1000);
        lcd_cmd_data(0x0059, 0x0003); sleep( 20*HZ/1000);
        lcd_cmd_data(0x0059, 0x0002); sleep( 20*HZ/1000);
        lcd_cmd_data(0x0010, 0x6360); sleep(200*HZ/1000);
        lcd_cmd_data(0x0010, 0x6300); sleep( 50*HZ/1000);
        lcd_cmd_data(0x0010, 0x0300);
        lcd_cmd_data(0x0059, 0x0000);
        lcd_cmd_data(0x0007, 0x4004);
        lcd_cmd_data(0x0010, 0x0301);
    }
}
#endif

/* Helper function to set up drawing region and start drawing */
static void lcd_setup_drawing_region(int x, int y, int width, int height)
{
    int y0, x0, y1, x1;
    
    /* calculate the drawing region */
#if CONFIG_LCD == LCD_IPODNANO
    y0 = x;                         /* start horiz */
    y1 = (x + width) - 1;           /* max horiz */
    x0 = y;                         /* start vert */
    x1 = (y + height) - 1;          /* max vert */
#elif CONFIG_LCD == LCD_IPODCOLOR
    y0 = y;                         /* start vert */
    y1 = (y + height) - 1;          /* end vert */
    x1 = (LCD_WIDTH - 1) - x;       /* end horiz */
    x0 = (x1 - width) + 1;          /* start horiz */
#endif

    /* setup the drawing region */
    if ((lcd_type&1) == 0) {
#if CONFIG_LCD == LCD_IPODCOLOR
        /* lcd_update_rect() leaves the panel taking columns */
        lcd_cmd_data(LCD_TYPE0_ENTRY_MODE, LCD_TYPE0_ENTRY_ROWS);
#endif
        /* x0 and x1 need to be swapped until 
         * proper direction setup is added */
        lcd_cmd_data(0x12, y0);      /* start vert */
        lcd_cmd_data(0x13, x1);      /* start horiz */
        lcd_cmd_data(0x15, y1);      /* end vert */
        lcd_cmd_data(0x16, x0);      /* end horiz */
    } else {
        /* max horiz << 8 | start horiz */
        lcd_cmd_data(LCD_CNTL_HORIZ_RAM_ADDR_POS, (y1 << 8) | y0);
        /* max vert << 8 | start vert */
        lcd_cmd_data(LCD_CNTL_VERT_RAM_ADDR_POS, (x1 << 8) | x0);

        /* start vert = max vert */
#if CONFIG_LCD == LCD_IPODCOLOR
        x0 = x1;
#endif

        /* position cursor (set AD0-AD15) */
        /* start vert << 8 | start horiz */
        lcd_cmd_data(LCD_CNTL_RAM_ADDR_SET, ((x0 << 8) | y0));

        /* start drawing */
        lcd_wait_write();
        LCD2_PORT = LCD2_CMD_MASK;
        LCD2_PORT = (LCD2_CMD_MASK|LCD_CNTL_WRITE_TO_GRAM);
    }
}

/* Line write helper function for lcd_yuv_blit. Writes two lines of yuv420. */
extern void lcd_write_yuv420_lines(unsigned char const * const src[3],
                                   const unsigned int lcd_baseadress,
                                   int width,
                                   int stride);

/* Performance function to blit a YUV bitmap directly to the LCD */
void lcd_blit_yuv(unsigned char * const src[3],
                  int src_x, int src_y, int stride,
                  int x, int y, int width, int height)
{
    int z;
    unsigned char const * yuv_src[3];

    width  = (width  + 1) & ~1; /* ensure width is even  */
    height = (height + 1) & ~1; /* ensure height is even */

#ifdef HAVE_LCD_SLEEP
    lcd_lock();
    if (lcd_off)
    {
        lcd_unlock();
        return;
    }
#endif

    lcd_setup_drawing_region(x, y, width, height);

    z = stride * src_y;
    yuv_src[0] = src[0] + z + src_x;
    yuv_src[1] = src[1] + (z >> 2) + (src_x >> 1);
    yuv_src[2] = src[2] + (yuv_src[1] - src[1]);

    while (height > 0) {
        int r, h, pixels_to_write;

        pixels_to_write = (width * height) * 2;
        h = height;

        /* calculate how much we can do in one go */
        if (pixels_to_write > 0x10000) {
            h = ((0x10000/2) / width) & ~1; /* ensure h is even */
            pixels_to_write = (width * h) * 2;
        }
        
        LCD2_BLOCK_CTRL   = 0x10000080;
        LCD2_BLOCK_CONFIG = 0xc0010000 | (pixels_to_write - 1);
        LCD2_BLOCK_CTRL   = 0x34000000;

        r = h>>1; /* lcd_write_yuv420_lines writes two lines at once */
        do {
            lcd_write_yuv420_lines(yuv_src, LCD2_BASE, width, stride);
            yuv_src[0] += stride << 1;
            yuv_src[1] += stride >> 1;
            yuv_src[2] += stride >> 1;
        } while (--r > 0);
        
        /* transfer of pixels_to_write bytes finished */
        while (!(LCD2_BLOCK_CTRL & LCD2_BLOCK_READY));
        LCD2_BLOCK_CONFIG = 0;

        height -= h;
    }
#ifdef HAVE_LCD_SLEEP
    lcd_unlock();
#endif
}

/* Helper function writes 'count' consecutive pixels from src to LCD IF */
static void lcd_write_line(int count, unsigned long *src)
{
    do {
#if LCD_PIXELFORMAT == RGB565
        /* the panel takes each pixel's bytes the other way round than the
           framebuffer, which is RGB565 as on the iPod Video (ipodcolor.h) */
        unsigned long p = *src++;
        p = ((p & 0x00ff00ff) << 8) | ((p >> 8) & 0x00ff00ff);
        while (!(LCD2_BLOCK_CTRL & LCD2_BLOCK_TXOK)); /* FIFO wait */
        LCD2_BLOCK_DATA = p;                          /* output 2 pixels */
#else
        while (!(LCD2_BLOCK_CTRL & LCD2_BLOCK_TXOK)); /* FIFO wait */
        LCD2_BLOCK_DATA = *src++;                     /* output 2 pixels */
#endif
        count -= 2;
    } while (count > 0);
}

/* The rectangle within the screen; false if nothing's left of it. Out of the
   screen, the panel's region and the count of pixels sent wouldn't agree */
static bool clip_rect(int *x, int *y, int *width, int *height)
{
    if (*x < 0)
    {
        *width += *x;
        *x = 0;
    }
    if (*y < 0)
    {
        *height += *y;
        *y = 0;
    }
    if (*x + *width > LCD_WIDTH)
        *width = LCD_WIDTH - *x;
    if (*y + *height > LCD_HEIGHT)
        *height = LCD_HEIGHT - *y;

    if ((*width <= 0) || (*height <= 0))
        return false; /* Nothing left to do. */

    return true;
}

#if CONFIG_LCD == LCD_IPODCOLOR
/* Writes 'pairs' pairs of pixels down a column, from 'src', to LCD IF. Not
   inlined: its loop stays in registers then */
static NO_INLINE void lcd_write_column(const fb_data *src, int pairs)
{
    do {
        unsigned long p = src[0] | ((unsigned long)src[LCD_WIDTH] << 16);
        src += 2*LCD_WIDTH;
#if LCD_PIXELFORMAT == RGB565
        p = ((p & 0x00ff00ff) << 8) | ((p >> 8) & 0x00ff00ff); /* as above */
#endif
        while (!(LCD2_BLOCK_CTRL & LCD2_BLOCK_TXOK)); /* FIFO wait */
        LCD2_BLOCK_DATA = p;                          /* output 2 pixels */
    } while (--pairs);
}

/* lcd_type 0 and 2: the panel's region for a rectangle sent column by
   column, in the order the panel refreshes them (LCD_TYPE0_ENTRY_COLUMNS) */
static void lcd_columns_region(int x, int y, int width, int height)
{
    lcd_cmd_data(LCD_TYPE0_ENTRY_MODE, LCD_TYPE0_ENTRY_COLUMNS);
    lcd_cmd_data(0x12, y);                                 /* start vert */
    lcd_cmd_data(0x13, (LCD_WIDTH - 1) - (x + width - 1)); /* start horiz */
    lcd_cmd_data(0x15, y + height - 1);                    /* end vert */
    lcd_cmd_data(0x16, (LCD_WIDTH - 1) - x);               /* end horiz */
}

/* The panel's frame time. A write that crosses the screen as fast as the
   refresh does can't meet it, wherever each starts, so each column gets
   1/LCD_WIDTH of a frame. Faster or slower, the two would meet in about
   |1 - write/frame| of whole-screen updates, fewer of narrower ones, and
   that frame would show the picture cut along a column. On the iPod Photo
   M9829 the refresh is 60 Hz: 16.667 ms per screen showed no tearing,
   16.0 and 16.333 a rare cut, 15 and 17 more */
#define LCD_FRAME_US        16667
/* Paced from this height: text lines, 12 px and up in the 220x176 themes,
   and list rows, 22 and up, are. Thinner bars aren't: Updating Library's,
   6 px, Modern's and Neo's, 9 and 11. From one of their steps to the next
   only a bar's end changes, so a cut through it can't show */
#define LCD_PACE_HEIGHT     12
/* The LCD bus runs at the CPU's clock, a pixel in about 25 of its cycles:
   a screen takes 12 ms at 80 MHz, 31 at 30 */
#define LCD_BUS_CYCLES_PX   26

/* lcd_type 0 and 2: the rectangle column by column, in the order the panel
   refreshes them, at the panel's pace */
static void lcd_update_rect_columns(int x, int y, int width, int height)
{
    /* paced where a cut could be seen: not in a bar, nor with the backlight
       off, unless boosted for a whole screen, as for a button's redraw, which
       may come before the backlight thread lights the screen */
    bool pace = height >= LCD_PACE_HEIGHT;
#if defined(HAVE_BACKLIGHT) && !defined(BOOTLOADER)
    if (pace && !is_backlight_on(true))
        pace = cpu_frequency == CPUFREQ_MAX &&
               width == LCD_WIDTH && height == LCD_HEIGHT;
#endif

    /* the pixels go in pairs down each column */
    height += y & 1;
    y &= ~1;
    height = (height + 1) & ~1;

    /* a column's slot, in 1/16 us */
    const unsigned long step = (LCD_FRAME_US << 4) / LCD_WIDTH;

#if defined(HAVE_ADJUSTABLE_CPU_FREQ) && !defined(BOOTLOADER)
    /* boosted if its columns don't fit their slots at this clock, as a
       whole screen's don't at 30 MHz. Not with interrupts off (a panic's
       screen); nor on the COP, by choice: nothing there needs the pace */
    const bool boost = pace && cpu_frequency != CPUFREQ_MAX &&
                       irq_enabled() && CURRENT_CORE == CPU &&
                       (unsigned long)height * (LCD_BUS_CYCLES_PX << 4) >
                       step * (cpu_frequency / 1000000);
    if (boost)
        cpu_boost(true);
#endif

    lcd_columns_region(x, y, width, height);

    /* horiz runs against our x: the rightmost column goes first */
    const fb_data *column = FBADDR(x + width - 1, y);
    const int column_bytes = height * sizeof(fb_data);
    unsigned long slot = USEC_TIMER << 4;

    while (width > 0) {
        int c, columns = width;

        /* calculate how much we can do in one go */
        if (columns * column_bytes > 0x10000)
            columns = 0x10000 / column_bytes;

        LCD2_BLOCK_CTRL   = 0x10000080;
        LCD2_BLOCK_CONFIG = 0xc0010000 | (columns * column_bytes - 1);
        LCD2_BLOCK_CTRL   = 0x34000000;

        for (c = 0; c < columns; c++, column--) {
            lcd_write_column(column, height / 2);

            /* to the next column's slot, if there's one */
            slot += step;
            if (pace && (c < columns - 1 || width > columns))
                while ((long)(slot - (USEC_TIMER << 4)) > 0);
        }

        /* transfer of the block finished */
        while (!(LCD2_BLOCK_CTRL & LCD2_BLOCK_READY));
        LCD2_BLOCK_CONFIG = 0;

        width -= columns;
    }

#if defined(HAVE_ADJUSTABLE_CPU_FREQ) && !defined(BOOTLOADER)
    if (boost)
        cpu_boost(false);
#endif
}
#endif /* CONFIG_LCD == LCD_IPODCOLOR */

/* The framebuffer's rectangle, within the screen, to the panel */
static void write_rect(int x, int y, int width, int height)
{
    unsigned long *addr;

#if CONFIG_LCD == LCD_IPODCOLOR
    if ((lcd_type&1) == 0) {
        lcd_update_rect_columns(x, y, width, height);
        return;
    }
#endif

    /* x and width even, to read the framebuffer 32 bits at a time */
    width = (width + (x & 1) + 1) & ~1;
    x &= ~1;

    lcd_setup_drawing_region(x, y, width, height);

    addr = (unsigned long*)FBADDR(x, y);

    while (height > 0) {
        int r, h, pixels_to_write;

        pixels_to_write = (width * height) * 2;
        h = height;

        /* calculate how much we can do in one go */
        if (pixels_to_write > 0x10000) {
            h = ((0x10000/2) / width) & ~1; /* ensure h is even */
            pixels_to_write = (width * h) * 2;
        }

        LCD2_BLOCK_CTRL   = 0x10000080;
        LCD2_BLOCK_CONFIG = 0xc0010000 | (pixels_to_write - 1);
        LCD2_BLOCK_CTRL   = 0x34000000;

        if (LCD_WIDTH == width) {
            /* for each row and column in a single call */
            lcd_write_line(h*width, addr);
            addr += LCD_WIDTH/2*h;
        } else {
            /* for each row */
            for (r = 0; r < h; r++) {
                lcd_write_line(width, addr);
                addr += LCD_WIDTH/2;
            }
        }

        /* transfer of pixels_to_write bytes finished */
        while (!(LCD2_BLOCK_CTRL & LCD2_BLOCK_READY));
        LCD2_BLOCK_CONFIG = 0;

        height -= h;
    }
}

/* Update a fraction of the display. */
void lcd_update_rect(int x, int y, int width, int height)
{
    lcd_update_filter_rect(x, y, width, height);
    /* a screen drawn in full before it shows, as for the menus' slide: the
       driver holds its updates, as the iPod Video's does */
    if (lcd_update_held(&x, &y, &width, &height))
        return;

    if (!clip_rect(&x, &y, &width, &height))
        return;

#ifdef HAVE_LCD_SLEEP
    lcd_lock();
    if (!lcd_off)
    {
        write_rect(x, y, width, height);
        lcd_updates++;
    }
    lcd_unlock();
#else
    write_rect(x, y, width, height);
#endif
}

/* Update the display.
   This must be called after all other LCD functions that change the display. */
void lcd_update(void)
{
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

#ifdef HAVE_LCD_SLEEP
/* From the backlight thread, the light off */
void lcd_sleep(void)
{
    /* lcd_type 2 untested; 1 and 3 have no sequence to power on again */
    if (lcd_type != 0)
        return;

    lcd_lock();
    if (!lcd_off)
    {
        lcd_type0_off();
        lcd_off = true;
    }
    lcd_unlock();
}

/* under lcd_cl, the panel off */
static void lcd_power_on(void)
{
    /* as Apple's firmware powers it on (in its 1.x osos at 0x169b68), which
       sets none of lcd_init_device()'s other registers again */
    lcd_cmd_data(0xef, 0x0);
    lcd_cmd_data(0x01, 0x0);
    lcd_cmd_data(0x80, 0x1); udelay(1000);
    lcd_cmd_data(0x7e, 0x5);
    lcd_cmd_data(0x7f, 0x1);
    lcd_off = false;
}

#ifdef HAVE_HANG_CATCHER
/* lcd_awake() for the HANG screen (panic.c), interrupts off: without the
   activation's event, whose handlers would draw, and with its lock bounded
   (lcd_lock()); the screen is drawn whole right after */
void lcd_awake_crash(void)
{
    if (lcd_type != 0 || !lcd_off)
        return;
    lcd_lock();
    if (lcd_off)
        lcd_power_on();
    lcd_unlock();
}
#endif

/* From the backlight thread: before the light comes on (lcd_awake_redrawn()),
   as the iPod's sleep ends, the light off (backlight.c), and at a shutdown */
void lcd_awake(void)
{
    /* without the lock while it's on, as it is at every light: only the CPU's
       threads change it, which switch only as they yield */
    if (lcd_type != 0 || !lcd_off)
        return;

    lcd_lock();
    if (!lcd_off)
    {
        lcd_unlock();
        return;
    }

    lcd_power_on();

    /* the framebuffer's whole picture before the light shows the panel,
       whatever the panel kept, and whatever lcd_update_held() would hold:
       none of the updates since it went off reached it */
    write_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
    lcd_unlock();

    /* what wasn't drawn for a screen off (lcd_active()), drawn now */
    send_event(LCD_EVENT_ACTIVATION, NULL);
}

/* As the light comes on (backlight-4g_color.c): the panel on, if it was off,
   and the light kept back until the screens that stopped drawing while it
   was off (lcd_active()) have drawn again, the status bar's clock first:
   their first update and those right after it, LCD_REDRAW_TICKS at most.
   Lit at once, the picture of the moment the panel went off showed, a night's
   old clock. From the backlight thread, which sleeps meanwhile. */
#define LCD_REDRAW_TICKS (HZ / 5)

void lcd_awake_redrawn(void)
{
    if (lcd_type != 0 || !lcd_off)
        return;
    lcd_awake();

    /* the activation's redraw is the UI thread's: posted, not drawn yet.
       Boosted meanwhile: the clock may still be the sleep's 24 MHz, which
       the power thread raises only after the UI has run */
    cpu_boost(true);
    unsigned int seen = lcd_updates;
    bool drawn = false;
    const long until = current_tick + LCD_REDRAW_TICKS;
    while (TIME_BEFORE(current_tick, until))
    {
        sleep(1);
        if (lcd_updates != seen)
        {
            seen = lcd_updates;
            drawn = true;
        }
        else if (drawn)
            break; /* a tick without one, after it drew */
    }
    cpu_boost(false);
}

bool lcd_active(void)
{
    return !lcd_off;
}
#endif /* HAVE_LCD_SLEEP */
