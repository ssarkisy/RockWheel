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

/* The HANG screen's text, drawn into hang_text (hang.h) as the language and
 * the theme are set: at the hang, interrupts off and maybe the disk held,
 * no glyph can be read from a font file. The UI font draws it as it draws
 * any text (lcd_glyph_sink): the language's own letters, fallback fonts,
 * either direction. One bit a pixel, a glyph's ink where it is at least
 * half. */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "language.h"
#include "panic.h"
#include "hang.h"
#include "settings.h"
#include "crc32.h"
#include "gui/ui_theme.h"
#include "hang_text.h"

#ifdef HAVE_HANG_SCREEN

#if LCD_WIDTH >= 320
#define MARGIN 12
#else
#define MARGIN 8
#endif
#define TEXT_WIDTH (LCD_WIDTH - 2 * MARGIN)

/* the line being drawn: its bits in hang_text, its size; or measured only,
   how far right it draws */
static unsigned char *line_bits;
static int line_width, line_height;
static bool measuring;
static int drawn_right;

static void sink(const unsigned char *bits, bool alpha, int src_x, int stride,
                 int x, int y, int width, int height)
{
    if (measuring)
    {
        drawn_right = MAX(drawn_right, x + width);
        return;
    }
    for (int r = 0; r < height; r++)
    {
        const int py = y + r;
        if (py < 0 || py >= line_height)
            continue;
        for (int c = 0; c < width; c++)
        {
            const int px = x + c, sx = src_x + c;
            if (px < 0 || px >= line_width)
                continue;
            bool ink;
            if (alpha) /* 0 all ink, 15 none: the lower nibble first */
            {
                const int i = r * stride + sx;
                ink = ((bits[i >> 1] >> ((i & 1) * 4)) & 0xf) < 8;
            }
            else
                ink = (bits[(r >> 3) * stride + sx] >> (r & 7)) & 1;
            if (ink)
                line_bits[(py >> 3) * line_width + px] |= 1 << (py & 7);
        }
    }
}

/* the first n bytes of str drawn into the sink at 0, 0 of a line */
static void draw(const char *str, int n)
{
    char line[320]; /* a whole paragraph at most (hang_text_render()) */
    n = MIN(n, (int)sizeof(line) - 1);
    memcpy(line, str, n);
    line[n] = '\0';
    struct viewport vp;
    memset(&vp, 0, sizeof(vp));
    vp.width = LCD_WIDTH;
    vp.height = line_height;
    vp.font = FONT_UI;
    vp.drawmode = DRMODE_SOLID;
    struct viewport *old = lcd_set_viewport(&vp);
    lcd_glyph_sink_thread = thread_self();
    lcd_glyph_sink = sink;
    lcd_putsxy(0, 0, (const unsigned char *)line);
    lcd_glyph_sink = NULL;
    lcd_set_viewport(old);
}

/* how wide the first n bytes of str draw: as drawn, not as the font's sizes
   add up, which a script whose marks the drawing sets over its letters
   (Devanagari) makes narrower */
static int drawn_width(const char *str, int n)
{
    measuring = true;
    drawn_right = 0;
    draw(str, n);
    measuring = false;
    return drawn_right;
}

/* a letter's bytes back from str + n, to the one before */
static int letter_before(const char *str, int n)
{
    do
        n--;
    while (n > 0 && ((unsigned char)str[n] & 0xc0) == 0x80);
    return n;
}

/* the bytes of str's first line in TEXT_WIDTH: to its last space that
   fits, or as many of its letters as fit when no space does */
static int line_length(const char *str, int len)
{
    int fits = 0;
    for (int i = 1; i <= len; i++)
    {
        if (i < len && str[i] != ' ')
            continue;
        if (drawn_width(str, i) > TEXT_WIDTH)
            break;
        fits = i;
    }
    if (!fits)
    {
        int w;
        fits = font_measurestring((const unsigned char *)str, len, TEXT_WIDTH,
                                  &w, NULL, FONT_UI);
        fits = MAX(fits, 1);
        while (fits > 1 && drawn_width(str, fits) > TEXT_WIDTH)
            fits = letter_before(str, fits);
        /* one letter wider than the screen: it all the same */
        while (fits < len && ((unsigned char)str[fits] & 0xc0) == 0x80)
            fits++;
    }
    return fits;
}

/* paragraph's lines into hang_text from *used bytes and *y; false if they
   don't fit */
static bool add_paragraph(const char *text, int colour, size_t *used, int *y)
{
    struct hang_text *t = &hang_text;
    const unsigned int column = (line_height + 7) / 8;
    int len = strlen(text);
    while (len > 0)
    {
        while (len > 0 && *text == ' ')
            text++, len--;
        if (!len)
            break;
        const int n = line_length(text, len);
        const int w = MIN(drawn_width(text, n), LCD_WIDTH);
        if (t->lines >= HANG_TEXT_LINES || w <= 0
            || *used + (size_t)w * column > sizeof(t->bits))
            return false;
        line_bits = t->bits + *used;
        line_width = w;
        memset(line_bits, 0, w * column);
        draw(text, n);

        const int i = t->lines++;
        t->x[i] = (LCD_WIDTH - w) / 2;
        t->y[i] = *y;
        t->width[i] = w;
        t->colour[i] = colour;
        *used += (size_t)w * column;
        *y += line_height;
        text += n;
        len -= n;
    }
    return true;
}

/* What the text was last drawn from: its phrases, the UI font as its file's
   sizes tell one from another, the theme and Dark Theme. It's asked for at
   each settings_apply(true) as at each settings_apply_skins(), which may
   follow at once, and is drawn only when one of these has changed. */
static uint32_t drawn_from;
static bool drawn;

static uint32_t drawn_from_now(const char *send)
{
    const struct font *pf = font_get(FONT_UI);
    const char *const texts[] = {
        str(LANG_HANG_TITLE), str(LANG_HANG_RESTART), send
    };
    uint32_t crc = 0xffffffff;
    for (unsigned int i = 0; i < ARRAYLEN(texts); i++)
        crc = crc_32(texts[i], strlen(texts[i]) + 1, crc);
    const int32_t sizes[] = {
        pf->maxwidth, pf->height, pf->ascent, pf->firstchar, pf->size,
        pf->depth, pf->bits_size, global_settings.ui_theme,
        global_settings.dark_theme
    };
    return crc_32(sizes, sizeof(sizes), crc);
}

void hang_text_render(void)
{
    struct hang_text *t = &hang_text;
    static char send[320];
    snprintf(send, sizeof(send), str(LANG_HANG_SEND), CRASH_LOG_SHOWN,
             CRASH_REPORT_ADDRESS);
    const uint32_t from = drawn_from_now(send);
    if (drawn && from == drawn_from)
        return;
    drawn = true;
    drawn_from = from;

    t->ready = false; /* the English one meanwhile, should it come now */
    t->lines = 0;
    line_height = font_get(FONT_UI)->height;
    t->height = line_height;
    /* the theme's own, not a cover's tone of them, which changes with the
       song: Dark Theme's change draws this again */
    const struct ui_theme_look *look = ui_theme_look(global_settings.dark_theme);
    t->fg[0] = look->colour[UI_COLOUR_TEXT];
    t->fg[1] = look->colour[UI_COLOUR_VALUE];
    t->bg = look->colour[UI_COLOUR_GROUND];
    /* the title, what to do, and the file: half a line between them */
    const struct { const char *text; int colour; } paragraphs[] = {
        { str(LANG_HANG_TITLE), 0 },
        { str(LANG_HANG_RESTART), 0 },
        { send, 1 },
    };
    size_t used = 0;
    int y = 0;
    for (unsigned int p = 0; p < ARRAYLEN(paragraphs); p++)
    {
        if (p)
            y += line_height / 2;
        if (!add_paragraph(paragraphs[p].text, paragraphs[p].colour, &used,
                           &y))
            return;
    }
    if (y > LCD_HEIGHT - 2 * MARGIN)
        return;
    /* the whole in the screen's middle */
    const int top = (LCD_HEIGHT - y) / 2;
    for (int i = 0; i < t->lines; i++)
        t->y[i] += top;
    t->ready = true;
}

#endif /* HAVE_HANG_SCREEN */
