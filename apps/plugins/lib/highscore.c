/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 Linus Nielsen Feltzing
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
#include "plugin.h"
#include "highscore.h"

static bool highscore_updated = false;

int highscore_save(char *filename, struct highscore *scores, int num_scores)
{
    int i;
    int fd;
    int rc;
    char buf[80];

    if(!highscore_updated)
        return 1;

    fd = rb->open(filename, O_WRONLY|O_CREAT, 0666);
    if(fd < 0)
        return -1;
    
    for(i = 0;i < num_scores;i++)
    {
        rb->snprintf(buf, sizeof(buf), "%d:%d:%s\n",
                     scores[i].score, scores[i].level, scores[i].name);
        rc = rb->write(fd, buf, rb->strlen(buf));
        if(rc < 0)
        {
            rb->close(fd);
            return -2;
        }
    }
    rb->close(fd);
    highscore_updated = false;
    return 0;
}

int highscore_load(char *filename, struct highscore *scores, int num_scores)
{
    int i;
    int fd;
    char buf[80];
    char *score, *level, *name;

    rb->memset(scores, 0, sizeof(struct highscore)*num_scores);

    fd = rb->open(filename, O_RDONLY);
    if(fd < 0)
        return -1;

    i = 0;
    while(rb->read_line(fd, buf, sizeof(buf)) > 0 && i < num_scores)
    {
        DEBUGF("%s\n", buf);

        if ( !rb->settings_parseline(buf, &score, &level) )
            continue;
        if ( !rb->settings_parseline(level, &level, &name) )
            continue;

        scores[i].score = rb->atoi(score);
        scores[i].level = rb->atoi(level);
        rb->strlcpy(scores[i].name, name, sizeof(scores[i].name));
        i++;
    }
    rb->close(fd);
    highscore_updated = false;
    return 0;
}

int highscore_update(int score, int level, const char *name,
                     struct highscore *scores, int num_scores)
{
    int pos;
    struct highscore *entry;
    
    if (!highscore_would_update(score, scores, num_scores))
        return -1;

    pos = num_scores-1;
    while (pos > 0 && score > scores[pos-1].score)
    {
        /* move down one */
        rb->memcpy((void *)&scores[pos], (void *)&scores[pos-1],
                   sizeof(struct highscore));
        pos--;
    }
    
    entry = scores + pos;
    entry->score = score;
    entry->level = level;
    rb->strlcpy(entry->name, name, sizeof(entry->name));

    highscore_updated = true;
    return pos;
}

bool highscore_would_update(int score, struct highscore *scores,
                            int num_scores)
{
    return (num_scores > 0) && (score > scores[num_scores-1].score);
}

#define MARGIN 5
void highscore_show(int position, struct highscore *scores, int num_scores,
                    bool show_level)
{
    const unsigned char *title = rb->str(LANG_GAME_HIGH_SCORES);
    const unsigned char *score_title = rb->str(LANG_GAME_SCORE);
    const unsigned char *level_title = rb->str(LANG_GAME_LEVEL);
    char rank[8];
    int i, w, h, row_h, title_font, rows_font;
    int rank_w, score_w, level_w, score_x, level_x;
#if defined(HAVE_LCD_COLOR) || LCD_DEPTH >= 2
    unsigned bgcolor = rb->lcd_get_background();
    unsigned fgcolor = rb->lcd_get_foreground();
#ifdef HAVE_LCD_COLOR
    rb->lcd_set_background(LCD_BLACK);
    rb->lcd_set_foreground(LCD_WHITE);
#else
    rb->lcd_set_background(LCD_WHITE);
    rb->lcd_set_foreground(LCD_BLACK);
#endif
#endif
    rb->lcd_clear_display();

    /* The titles are in the UI font, the one with every language's letters.
     * The rows hold only digits: if they don't fit in it, they take the
     * small built-in font, and only a screen too small for even that puts
     * the titles in it too (its letters are Latin only). */
    title_font = rows_font = FONT_UI;
    h = row_h = rb->font_get(FONT_UI)->height;
    if ((3*h + row_h*num_scores + MARGIN) > LCD_HEIGHT) {
        rows_font = FONT_SYSFIXED;
        row_h = rb->font_get(FONT_SYSFIXED)->height;
        if ((3*h + row_h*num_scores + MARGIN) > LCD_HEIGHT) {
            title_font = FONT_SYSFIXED;
            h = row_h;
        }
    }

    /* columns placed by the widths of what's in them, not by the English
     * words', so a longer translation pushes them apart; the title is
     * centred and starts at the left edge when it's wider than the screen */
    rb->snprintf(rank, sizeof(rank), "%d)", num_scores);
    rb->lcd_setfont(rows_font);
    rb->lcd_getstringsize(rank, &rank_w, NULL);
    rb->lcd_setfont(title_font);
    rb->lcd_getstringsize(title, &w, NULL);
    rb->lcd_getstringsize(score_title, &score_w, NULL);
    rb->lcd_getstringsize(level_title, &level_w, NULL);

    rb->lcd_putsxy(MAX(0, LCD_WIDTH/2-w/2), MARGIN, title);

    score_x = MAX(LCD_WIDTH/6, MARGIN + rank_w + MARGIN);
    level_x = MAX(LCD_WIDTH*5/8, score_x + score_w + 2*MARGIN);
    if (level_x + level_w > LCD_WIDTH - MARGIN)
        level_x = MAX(LCD_WIDTH - MARGIN - level_w,
                      score_x + score_w + 2*MARGIN);

    rb->lcd_putsxy(score_x, 2*h, score_title);
    if(show_level) {
        rb->lcd_putsxy(level_x, 2*h, level_title);
    }

    rb->lcd_setfont(rows_font);
    for (i = 0; i<num_scores; i++)
    {
#ifdef HAVE_LCD_COLOR
        if (i == position) {
            rb->lcd_set_foreground(LCD_RGBPACK(245,0,0));
        }
#endif
        rb->lcd_putsxyf (MARGIN, 3*h + row_h*i, "%d)", i+1);
        rb->lcd_putsxyf (score_x, 3*h + row_h*i, "%d", scores[i].score);

        if(show_level) {
            rb->lcd_putsxyf (level_x, 3*h + row_h*i, "%d", scores[i].level);
        }

        if(i == position) {
#ifdef HAVE_LCD_COLOR
            rb->lcd_set_foreground(LCD_WHITE);
#else
            rb->lcd_hline(MARGIN, LCD_WIDTH-MARGIN*2, 3*h + row_h*(i+1) - 1);
#endif
        }
    }
    rb->lcd_update();
    rb->sleep(HZ/2);
    rb->button_clear_queue();
    rb->button_get(true);
    rb->lcd_setfont(FONT_SYSFIXED);
#if defined(HAVE_LCD_COLOR) || LCD_DEPTH >= 2
    rb->lcd_set_background(bgcolor);
    rb->lcd_set_foreground(fgcolor);
#endif
}
