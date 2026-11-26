/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025 by Sho Tanimoto
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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "debug.h"
#include "file.h"
#include "logf.h"
#include "rbpaths.h"
#include "system.h"
#include "tick.h"

#include "font.h"
#include "lcd.h"

#define MAX_COLS 64

static int      rows;
static int      columns;
static unsigned count;

static void update_color(void) {
    unsigned avail = rows - 1;
    if((count % (avail * 2)) > avail) {
        lcd_set_drawinfo(DRMODE_SOLID, LCD_BLACK, LCD_WHITE);
    } else {
        lcd_set_drawinfo(DRMODE_SOLID, LCD_WHITE, LCD_BLACK);
    }
}

void iap_lcd_scatter(const char* fmt, ...) {
    if(rows == 0) {
        int w, h;
        font_getstringsize((unsigned char*)"A", &w, &h, FONT_SYSFIXED);
        columns = MIN(LCD_WIDTH / w, MAX_COLS);
        rows    = LCD_HEIGHT / h;
    }

    va_list ap;
    char    buf[256];
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), (char*)fmt, ap);
    va_end(ap);

    logf("%s", buf);

    lcd_set_backdrop(NULL);
    lcd_setfont(FONT_SYSFIXED);

    update_color();
    lcd_putsf(0, 0, (unsigned char*)"count %u", count);
    for(int i = 0; i < len;) {
        char line[MAX_COLS];
        int  copy = MIN(len - i, columns - 1);
        memcpy(line, buf + i, copy);
        memset(line + copy, ' ', columns - 1 - copy);
        line[columns - 1] = '\0';
        update_color();
        lcd_puts(0, 1 + count % (rows - 1), (unsigned char*)line);
        count += 1;
        i += copy;
    }
    lcd_update();
}

static unsigned long timestamp_epoch;

unsigned long iap_debug_timestamp(void) {
    return current_tick - timestamp_epoch;
}

void iap_debug_reset_timestamp(void) {
    timestamp_epoch = current_tick;
}

#ifdef IAP_TRACE
#define IAP_TRACE_BUF_SIZE 65536
static char   iap_trace_buf[IAP_TRACE_BUF_SIZE];
static size_t iap_trace_len;

static void iap_trace_append_str(const char* s, size_t n) {
    int oldlevel = disable_irq_save();
    if(n > 0 && iap_trace_len < IAP_TRACE_BUF_SIZE) {
        size_t space = IAP_TRACE_BUF_SIZE - iap_trace_len;
        size_t copy  = n < space ? n : space;
        memcpy(iap_trace_buf + iap_trace_len, s, copy);
        iap_trace_len += copy;
    }
    restore_irq(oldlevel);
}

void iap_trace_logf(const char* fmt, ...) {
    char    tmp[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp) - 1, fmt, ap);
    va_end(ap);
    if(n <= 0) {
        return;
    }
    size_t len = (size_t)n;
    if(len >= sizeof(tmp) - 1) {
        len = sizeof(tmp) - 2;
    }
    if(tmp[len - 1] != '\n') {
        tmp[len++] = '\n';
    }
    iap_trace_append_str(tmp, len);
}

void iap_trace_hex(const char* prefix, const void* data, size_t len) {
    static const char hex[] = "0123456789ABCDEF";
    char              line[192];
    const uint8_t*    bytes = (const uint8_t*)data;
    size_t            pos   = (size_t)snprintf(line, sizeof(line),
                                           "%lu %s (%zu):",
                                           iap_debug_timestamp(), prefix, len);
    size_t            limit = len < 16 ? len : 16;
    for(size_t i = 0; i < limit && pos + 3 < sizeof(line) - 4; i++) {
        line[pos++] = ' ';
        line[pos++] = hex[bytes[i] >> 4];
        line[pos++] = hex[bytes[i] & 0xF];
    }
    if(len > limit && pos + 4 < sizeof(line)) {
        line[pos++] = '.';
        line[pos++] = '.';
        line[pos++] = '.';
    }
    if(pos < sizeof(line)) {
        line[pos++] = '\n';
    }
    iap_trace_append_str(line, pos);
}

bool iap_trace_needs_flush(void) {
    return iap_trace_len >= (IAP_TRACE_BUF_SIZE / 2);
}

void iap_trace_flush(void) {
    int    oldlevel = disable_irq_save();
    size_t len      = iap_trace_len;
    restore_irq(oldlevel);
    if(len == 0) {
        return;
    }

    int fd = open(ROCKBOX_DIR "/iap_trace.log",
                  O_WRONLY | O_CREAT | O_APPEND, 0666);
    if(fd < 0) {
        return;
    }

    size_t off = 0;
    while(off < len) {
        char   chunk[256];
        size_t take = (len - off) < sizeof(chunk) ? (len - off) : sizeof(chunk);
        oldlevel    = disable_irq_save();
        memcpy(chunk, iap_trace_buf + off, take);
        restore_irq(oldlevel);
        if(write(fd, chunk, take) != (ssize_t)take) {
            break;
        }
        off += take;
    }
    close(fd);

    oldlevel = disable_irq_save();
    if(off > 0 && off <= iap_trace_len) {
        size_t rem = iap_trace_len - off;
        if(rem > 0) {
            memmove(iap_trace_buf, iap_trace_buf + off, rem);
        }
        iap_trace_len = rem;
    }
    restore_irq(oldlevel);
}
#endif /* IAP_TRACE */
