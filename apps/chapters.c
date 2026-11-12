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
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "audio.h"
#include "action.h"
#include "lang.h"
#include "misc.h"
#include "kernel.h"
#include "tagcache.h"
#include "playback.h"
#include "root_menu.h"
#include "string-extra.h"
#include "gui/list.h"
#include "gui/transition.h"
#include "gui/wps.h"
#include "chapters.h"

static char *names_of(const struct chapters *c)
{
    return (char *)&c->ch[c->slots];
}

/* the bytes of a title kept, cut between characters */
static size_t name_len(const char *title)
{
    size_t len = strlen(title);
    if (len > CHAPTER_NAME)
    {
        len = CHAPTER_NAME;
        while (len > 0 && (title[len] & 0xc0) == 0x80)
            len--;
    }
    return len;
}

static void read_file(int fd, const struct mp3entry *id3, chapter_add_fn *add,
                      void *ctx)
{
    const struct embedded_chapters *e = &id3->embedded_chapters;
    if (lseek(fd, e->pos, SEEK_SET) < 0)
        return;
    switch (e->source)
    {
        case CHAPTERS_ID3V23:
        case CHAPTERS_ID3V24:
            id3_read_chapters(fd, e->size, e->source == CHAPTERS_ID3V24,
                              add, ctx);
            break;
        case CHAPTERS_MP4_CHPL:
        case CHAPTERS_MP4_TRACK:
            mp4_read_chapters(fd, e->size, e->source == CHAPTERS_MP4_CHPL,
                              add, ctx);
            break;
    }
}

struct measure {
    unsigned int count, names;
};

static void measure_add(void *ctx, unsigned long ms, const char *title)
{
    struct measure *m = ctx;
    m->count++;
    m->names += name_len(title);
    (void)ms;
}

int chapters_measure(int fd, const struct mp3entry *id3, size_t *size)
{
    struct measure m = { 0, 0 };
    read_file(fd, id3, measure_add, &m);
    if (m.count < 2)
        return 0;
    const int slots = MIN(m.count, CHAPTERS_MAX);
    const size_t fixed = sizeof(struct chapters) + slots * sizeof(struct chapter);
    *size = fixed + MIN(m.names, CHAPTERS_SIZE - fixed);
    return slots;
}

struct fill {
    struct chapters *c;
    size_t room;    /* for the names */
};

/* In the order of their starts, as files need not have them so; of more than
   there are slots for, the earliest. A name there is no room left for is
   left out. */
static void fill_add(void *ctx, unsigned long ms, const char *title)
{
    struct fill *f = ctx;
    struct chapters *c = f->c;
    const uint32_t start = ms;  /* as it is kept, where long is wider */
    int n = c->count;
    if (n >= c->slots)
    {
        if (!n || start >= c->ch[n - 1].start)
            return;
        n--;
    }
    int i = n;
    for (; i > 0 && c->ch[i - 1].start > start; i--)
        c->ch[i] = c->ch[i - 1];
    size_t len = name_len(title);
    if (len > f->room - c->names)
        len = 0;
    memcpy(names_of(c) + c->names, title, len);
    c->ch[i] = (struct chapter){ .start = start, .name = c->names, .len = len };
    c->names += len;
    c->count = n + 1;
}

bool chapters_read(int fd, const struct mp3entry *id3, struct chapters *c,
                   int slots, size_t size)
{
    const size_t fixed = sizeof(struct chapters) + slots * sizeof(struct chapter);
    if (slots < 2 || slots > CHAPTERS_MAX || size < fixed || size > CHAPTERS_SIZE)
        return false;
    memset(c, 0, fixed);
    c->slots = slots;
    c->current = c->jumped = -1;
    c->passed = 0;
    c->again = -1;
    struct fill f = { c, size - fixed };
    read_file(fd, id3, fill_add, &f);

    /* Those past the track's end go, and of two with one start the first,
       which Next would stand still on; its name is kept if the other has
       none */
    int kept = 0;
    for (int i = 0; i < c->count; i++)
    {
        struct chapter ch = c->ch[i];
        if (id3->length && ch.start >= id3->length)
            break;
        if (kept && c->ch[kept - 1].start == ch.start)
        {
            kept--;
            if (!ch.len)
            {
                ch.name = c->ch[kept].name;
                ch.len = c->ch[kept].len;
            }
        }
        c->ch[kept++] = ch;
    }
    c->count = kept;
    return kept >= 2;
}

/* A song has chapters as a long recording has, a mix or a concert: one
   shorter than this, or of unknown length, is a track without them */
#define CHAPTERS_SONG_MS (10 * 60 * 1000)

/* Whatever the scratch holds, nothing is read outside its CHAPTERS_SIZE */
static struct chapters *chapters_of(const struct mp3entry *id3)
{
    struct chapters *c = id3 ? id3->chapters : NULL;
    if (!c || c->count < 2 || c->count > c->slots
        || sizeof(*c) + c->slots * sizeof(struct chapter) + c->names
           > CHAPTERS_SIZE)
        return NULL;
#ifdef HAVE_TAGCACHE
    if (id3->length < CHAPTERS_SONG_MS
        && !tagcache_track_kind(id3->tagcache_idx - 1))
        return NULL;
#endif
    return c;
}

static unsigned int jump_seek;  /* the jump's seek: audio_seeks_posted() after it */

/* The chapter jumped to is where the track is: no seek was asked for since
   the jump's own, and that one is not taken up yet (the position is the old
   one still) or landed at or before the chapter's start, where it stays
   while paused */
static bool jump_holds(const struct chapters *c, const struct mp3entry *id3)
{
    return c->jumped >= 0 && c->jumped < c->count
        && audio_seeks_posted() == jump_seek
        && ((int)(audio_seeks_taken() - jump_seek) < 0
            || id3->elapsed <= c->ch[c->jumped].start);
}

int chapters_count(const struct mp3entry *id3)
{
    const struct chapters *c = chapters_of(id3);
    return c ? c->count : 0;
}

int chapters_at(const struct mp3entry *id3, unsigned long elapsed)
{
    const struct chapters *c = chapters_of(id3);
    int at = -1;
    if (c)
    {
        /* for the position itself, not for another time asked about, as
           under the scrubber's knob */
        if (elapsed == id3->elapsed && jump_holds(c, id3))
            return c->jumped;
        while (at + 1 < c->count && c->ch[at + 1].start <= elapsed)
            at++;
    }
    return at;
}

unsigned long chapters_start(const struct mp3entry *id3, int chapter)
{
    const struct chapters *c = chapters_of(id3);
    return c && chapter >= 0 && chapter < c->count ? c->ch[chapter].start : 0;
}

const char *chapters_name(const struct mp3entry *id3, int chapter,
                          char *buf, size_t size)
{
    const struct chapters *c = chapters_of(id3);
    if (!c || chapter < 0 || chapter >= c->count || !size)
        return NULL;
    const struct chapter *ch = &c->ch[chapter];
    if (!ch->len || ch->name + ch->len > c->names)
        return NULL;
    const size_t len = MIN(ch->len, size - 1);
    memcpy(buf, names_of(c) + ch->name, len);
    buf[len] = '\0';
    return buf;
}

bool chapters_changed(struct mp3entry *id3)
{
    struct chapters *c = chapters_of(id3);
    if (!c)
        return false;
    if (!jump_holds(c, id3))
        c->jumped = -1;
    const int now = chapters_at(id3, id3->elapsed);
    if (now == c->current)
        return false;
    c->current = now;
    return true;
}

unsigned long chapters_heard(const struct mp3entry *id3, bool again)
{
    struct chapters *c = chapters_of(id3);
    if (!c)
        return id3->elapsed;
    long heard = (long)id3->elapsed - c->passed;
    if (again)
    {
        /* a jump's own: jumps asked for after it are in "passed" already */
        if (c->again >= 0)
            heard = c->again;
        else
            c->passed = 0;
        c->again = -1;
    }
    return MAX(heard, 0);
}

static void jump_to(struct chapters *c, const struct mp3entry *id3, int chapter)
{
    const uint32_t start = c->ch[chapter].start;
    /* from the chapter a jump just went to, where the position may not be yet */
    const uint32_t from = jump_holds(c, id3) ? c->ch[c->jumped].start
                                             : id3->elapsed;
    if (start)
        c->passed += (int32_t)(start - from);
    else if (c->again < 0)
    {
        /* the track begins again: its finish event comes as the seek is taken */
        c->again = MAX((long)from - c->passed, 0);
        c->passed = 0;
    }
    c->jumped = chapter;
    audio_pre_ff_rewind();
    audio_ff_rewind(c->ch[chapter].start);
    jump_seek = audio_seeks_posted();
}

bool chapters_skip(const struct mp3entry *id3, int direction)
{
    struct chapters *c = chapters_of(id3);
    if (!c || !(audio_status() & AUDIO_STATUS_PLAY))
        return false;
    if (!jump_holds(c, id3))
        c->jumped = -1;
    int to = chapters_at(id3, id3->elapsed);
    if (direction > 0)
        to++;
    /* a chapter just jumped to is in its first seconds, wherever it landed */
    else if (to >= 0 && (to == c->jumped || id3->elapsed - c->ch[to].start
                                            < (unsigned long)DEFAULT_SKIP_THRESH))
        to--;
    if (to < 0 || to >= c->count)
        return false;
    jump_to(c, id3, to);
    return true;
}

static const struct mp3entry *list_id3;

static const char *list_name(int item, void *data, char *buf, size_t size)
{
    (void)data;
    if (!chapters_name(list_id3, item, buf, size))
        snprintf(buf, size, str(LANG_CHAPTER_NUMBER), item + 1);
    return buf;
}

static const char *list_start(int item, void *data, char *buf, size_t size)
{
    (void)data;
    format_time(buf, size, chapters_start(list_id3, item));
    return buf;
}

/* The speaker is the chapter playing: Select there goes back to it as it
   plays, as on the song playing in a list of songs */
static bool list_playing(int item, void *data)
{
    (void)data;
    return item == chapters_at(list_id3, list_id3->elapsed);
}

/* still the track the list is of: another one's chapters would be in its
   rows, and under Select */
static bool list_track(const char *path, int count)
{
    list_id3 = audio_current_track();
    return (audio_status() & AUDIO_STATUS_PLAY) && list_id3
        && chapters_count(list_id3) == count && !strcmp(path, list_id3->path);
}

int chapters_screen(void)
{
    char path[MAX_PATH];
    list_id3 = audio_current_track();
    const int count = chapters_count(list_id3);
    if (!count)
        return 0;
    strmemccpy(path, list_id3->path, sizeof(path));

    if (!transition_pending())
        transition_begin(TRANSITION_FORWARD);
    struct gui_synclist list;
    char widest[16];
    gui_synclist_init(&list, list_name, NULL, false, 1, NULL);
    gui_synclist_set_title(&list, str(LANG_CHAPTERS), NOICON);
    gui_synclist_set_icon_callback(&list, NULL);
    gui_synclist_set_playing_callback(&list, list_playing);
    format_time(widest, sizeof(widest), chapters_start(list_id3, count - 1));
    gui_synclist_set_value_callback(&list, list_start, widest);
    gui_synclist_set_nb_items(&list, count);
    int playing = chapters_at(list_id3, list_id3->elapsed);
    gui_synclist_select_item(&list, MAX(playing, 0));

    int ret = -2;
    bool redraw = true;
    while (ret == -2)
    {
        if (!list_track(path, count))
        {
            transition_begin(TRANSITION_BACK);
            ret = 0;
            break;
        }
        const int now = chapters_at(list_id3, list_id3->elapsed);
        if (redraw || now != playing)
            gui_synclist_draw(&list);
        playing = now;
        redraw = false;
        int action;
        if (list_do_action(CONTEXT_TREE | ALLOW_SOFTLOCK, HZ / 2, &list, &action))
            continue;
        switch (action)
        {
            case ACTION_STD_OK:
            {
                const int row = gui_synclist_get_sel_pos(&list);
                struct chapters *c = list_track(path, count) ?
                                     chapters_of(list_id3) : NULL;
                if (!c || row >= c->count)
                {
                    transition_begin(TRANSITION_BACK);
                    ret = 0;
                    break;
                }
                transition_begin(TRANSITION_FORWARD);
                if (row != chapters_at(list_id3, list_id3->elapsed))
                    jump_to(c, list_id3, row);
                ret = GO_TO_WPS;
                break;
            }
            case ACTION_STD_CANCEL:
                transition_begin(TRANSITION_BACK);
                ret = 0;
                break;
            case ACTION_TREE_STOP:
            case ACTION_TREE_WPS:
                ret = GO_TO_WPS;
                break;
            case ACTION_STD_MENU:
                ret = GO_TO_ROOT;
                break;
            default:
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                    ret = GO_TO_ROOT;
                break;
        }
    }
    gui_synclist_scroll_stop(&list);
    return ret;
}
