/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Sergei Sarkisian
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

/* Photos, as on a stock iPod Video: the albums Finder syncs, a grid of
   thumbnails, full screen photos and a slideshow. Reads Photos/Photo Database
   and the thumbnails Finder scales for the iPod into Photos/Thumbs/F*.ithmb,
   which are RGB565 frames ready for the screen. The record layouts are the
   ones libgpod documents. */

#include "plugin.h"
#include "lib/helper.h"
#include "lib/configfile.h"

#define PHOTOS_DIR  "/Photos"
#define PHOTO_DB    PHOTOS_DIR "/Photo Database"
#define CONFIG_FILE "photos.cfg"
#define CONFIG_VERSION 1

/* the two of the iPod Video formats shown here */
#define GRID_FORMAT   1036 /* 50x41 thumbnails */
#define GRID_W        50
#define GRID_H        41
#define SCREEN_FORMAT 1024 /* full screen */

/* the grid as on the stock iPod: thumbnails in thin lines, the selected one
   framed in yellow, and a scrollbar */
#define GRID_COLS   6
#define GRID_ROWS   5
#define CELL_W      (GRID_W + 1) /* a thumbnail and the line left of it */
#define CELL_H      (GRID_H + 1)
#define SCROLLBAR_W 6
#define GRID_LINE   LCD_RGBPACK(0xc8, 0xc8, 0xc8)
#define GRID_EMPTY  LCD_RGBPACK(0xe8, 0xe8, 0xe8)
#define GRID_SELECT LCD_RGBPACK(0xf0, 0xb8, 0x20)

#define TITLE_FONT FONT_DIR "/14-Inter-Bold.fnt"
#define TEXT(id)   ((const char *)rb->str(id))
#define BATTERY_W  25
#define BATTERY_H  11

/* a photo and where its frames are */
struct photo {
    uint32_t id;
    uint32_t grid_offset;
    uint32_t screen_offset;
    uint8_t grid_file;    /* N of F1036_N.ithmb, 0 when missing */
    uint8_t screen_file;  /* N of F1024_N.ithmb, 0 when missing */
    uint8_t grid_x, grid_y, grid_w, grid_h; /* the picture in its thumbnail */
};

struct album {
    char name[96];
    bool library;
    int count;
    uint32_t *items; /* image ids while parsing, then indexes in photos[] */
};

static struct photo *photos;
static int photo_count;
static struct album *albums;
static int album_count;
static int grid_stride = GRID_W;

static unsigned char *mem;
static size_t mem_left;

static fb_data *screen_buf;
static fb_data grid_buf[GRID_W * GRID_H * 2];
static fb_data *title_bar;
static int title_h;
static int title_font = FONT_UI;
static int grid_x0, grid_y0;

/* slideshow settings, as on the stock iPod */
static const int slide_times[] = { 0, 2, 3, 5, 10, 20 }; /* 0 is manual */
static int slide_time = 2;
static bool slide_repeat = false;
static bool slide_shuffle = false;

static struct configdata config[] = {
    { TYPE_INT, 0, ARRAYLEN(slide_times) - 1, { .int_p = &slide_time },
      "time per slide", NULL },
    { TYPE_BOOL, 0, 1, { .bool_p = &slide_repeat }, "repeat", NULL },
    { TYPE_BOOL, 0, 1, { .bool_p = &slide_shuffle }, "shuffle", NULL },
};

#define ACTION_PHOTOS_PLAY ACTION_WPS_PLAY

static const struct button_mapping photos_keys[] = {
    { ACTION_STD_PREV,       BUTTON_SCROLL_BACK,               BUTTON_NONE },
    { ACTION_STD_PREVREPEAT, BUTTON_SCROLL_BACK|BUTTON_REPEAT, BUTTON_NONE },
    { ACTION_STD_NEXT,       BUTTON_SCROLL_FWD,                BUTTON_NONE },
    { ACTION_STD_NEXTREPEAT, BUTTON_SCROLL_FWD|BUTTON_REPEAT,  BUTTON_NONE },
    { ACTION_STD_PREV,       BUTTON_LEFT|BUTTON_REL,           BUTTON_LEFT },
    { ACTION_STD_NEXT,       BUTTON_RIGHT|BUTTON_REL,          BUTTON_RIGHT },
    { ACTION_STD_OK,         BUTTON_SELECT|BUTTON_REL,         BUTTON_SELECT },
    { ACTION_STD_CANCEL,     BUTTON_MENU|BUTTON_REL,           BUTTON_MENU },
    { ACTION_PHOTOS_PLAY,    BUTTON_PLAY|BUTTON_REL,           BUTTON_PLAY },
    LAST_ITEM_IN_LIST
};

static const struct button_mapping *get_keys(int context)
{
    (void)context;
    return photos_keys;
}

static int get_photos_action(int timeout)
{
    return rb->get_custom_action(CONTEXT_PLUGIN, timeout, get_keys);
}

static void *alloc(size_t size)
{
    size = (size + 3) & ~3;
    if (size > mem_left)
        return NULL;
    void *p = mem;
    mem += size;
    mem_left -= size;
    return p;
}

/* Photo Database parsing, through a small read cache */

static int db_fd = -1;
static unsigned char db_cache[4096];
static off_t db_cache_pos;
static int db_cache_len;

static const unsigned char *db_get(off_t pos, size_t len)
{
    if (len > sizeof(db_cache) || pos < 0)
        return NULL;
    if (pos < db_cache_pos || pos + (off_t)len > db_cache_pos + db_cache_len)
    {
        if (rb->lseek(db_fd, pos, SEEK_SET) != pos)
            return NULL;
        db_cache_pos = pos;
        db_cache_len = rb->read(db_fd, db_cache, sizeof(db_cache));
        if (db_cache_len < (int)len)
        {
            db_cache_len = 0;
            return NULL;
        }
    }
    return db_cache + (pos - db_cache_pos);
}

static uint32_t le32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const unsigned char *p)
{
    return p[0] | p[1] << 8;
}

/* a record with this tag at pos, at least len bytes of it */
static const unsigned char *db_record(off_t pos, const char *tag, size_t len)
{
    const unsigned char *p = db_get(pos, len);
    if (p && !rb->memcmp(p, tag, 4))
        return p;
    return NULL;
}

/* the text of a string mhod as UTF-8 */
static void mhod_text(off_t pos, char *buf, size_t size)
{
    buf[0] = '\0';
    const unsigned char *p = db_record(pos, "mhod", 36);
    if (!p)
        return;
    size_t len = le32(p + 24);
    int encoding = p[28];
    if (encoding == 2)
    {
        /* UTF-16 takes at most 3 UTF-8 bytes a unit */
        len = MIN(len / 2, (size - 1) / 3);
        p = db_get(pos + 36, len * 2);
        if (p)
            *rb->utf16LEdecode(p, (unsigned char *)buf, len) = '\0';
    }
    else
    {
        bool cut = len > size - 1;
        len = MIN(len, size - 1);
        p = db_get(pos + 36, len);
        if (!p)
            return;
        rb->memcpy(buf, p, len);
        /* don't end on half a character */
        if (cut)
        {
            /* the last character's lead byte, and how long it should be */
            size_t lead = len;
            while (lead > 0 && ((unsigned char)buf[lead - 1] & 0xc0) == 0x80)
                lead--;
            if (lead > 0)
            {
                unsigned char c = buf[lead - 1];
                size_t need = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
                if (len - (lead - 1) < need)
                    len = lead - 1;
            }
        }
        buf[len] = '\0';
    }
}

/* N of ":Thumbs:F1024_N.ithmb" */
static int ithmb_number(const char *name)
{
    const char *p = rb->strrchr(name, '_');
    int n = p ? rb->atoi(p + 1) : 0;
    return (n > 0 && n < 256) ? n : 0;
}

static void parse_mhni(off_t pos, struct photo *photo)
{
    const unsigned char *p = db_record(pos, "mhni", 36);
    if (!p)
        return;
    uint32_t header_len = le32(p + 4);
    int format = le32(p + 16);
    uint32_t offset = le32(p + 20);
    uint32_t size = le32(p + 24);
    int vpad = (int16_t)le16(p + 28);
    int hpad = (int16_t)le16(p + 30);
    int height = (int16_t)le16(p + 32);
    int width = (int16_t)le16(p + 34);

    char name[96];
    mhod_text(pos + header_len, name, sizeof(name));
    int number = ithmb_number(name);

    if (format == GRID_FORMAT && size % (GRID_H * 2) == 0
        && size / (GRID_H * 2) >= GRID_W && size / (GRID_H * 2) <= 2 * GRID_W)
    {
        grid_stride = size / (GRID_H * 2);
        photo->grid_file = number;
        photo->grid_offset = offset;
        /* the picture, without the bars around it */
        if (hpad < 0 || vpad < 0 || width <= 0 || height <= 0
            || hpad + width > GRID_W || vpad + height > GRID_H)
        {
            hpad = vpad = 0;
            width = GRID_W;
            height = GRID_H;
        }
        photo->grid_x = hpad;
        photo->grid_y = vpad;
        photo->grid_w = width;
        photo->grid_h = height;
    }
    else if (format == SCREEN_FORMAT && size == LCD_WIDTH * LCD_HEIGHT * 2)
    {
        photo->screen_file = number;
        photo->screen_offset = offset;
    }
}

static void parse_mhii(off_t pos, struct photo *photo)
{
    const unsigned char *p = db_record(pos, "mhii", 20);
    rb->memset(photo, 0, sizeof(*photo));
    if (!p)
        return;
    uint32_t header_len = le32(p + 4);
    int children = le32(p + 12);
    photo->id = le32(p + 16);

    off_t child = pos + header_len;
    for (int i = 0; i < children; i++)
    {
        p = db_record(child, "mhod", 14);
        if (!p)
            break;
        uint32_t len = le32(p + 4), total = le32(p + 8);
        if (le16(p + 12) == 2) /* a thumbnail */
            parse_mhni(child + len, photo);
        if (total == 0)
            break;
        child += total;
    }
}

static void parse_mhba(off_t pos, struct album *album)
{
    const unsigned char *p = db_record(pos, "mhba", 31);
    rb->memset(album, 0, sizeof(*album));
    if (!p)
        return;
    uint32_t header_len = le32(p + 4);
    int mhods = le32(p + 12);
    int mhias = le32(p + 16);
    album->library = (p[30] == 1);

    off_t child = pos + header_len;
    for (int i = 0; i < mhods; i++)
    {
        p = db_record(child, "mhod", 14);
        if (!p)
            return;
        uint32_t total = le32(p + 8);
        if (le16(p + 12) == 1) /* the album name */
            mhod_text(child, album->name, sizeof(album->name));
        if (total == 0)
            return;
        child += total;
    }

    album->items = alloc(mhias * sizeof(uint32_t));
    if (!album->items)
        return;
    for (int i = 0; i < mhias; i++)
    {
        p = db_record(child, "mhia", 20);
        if (!p)
            break;
        album->items[album->count++] = le32(p + 16);
        child += le32(p + 8);
    }
}

/* the items of the image (mhli) or album (mhla) list at pos */
static bool parse_list(off_t pos, bool images)
{
    const unsigned char *p = db_record(pos, images ? "mhli" : "mhla", 12);
    if (!p)
        return false;
    uint32_t header_len = le32(p + 4);
    int count = le32(p + 8);
    if (count < 0)
        return false;

    if (images)
    {
        /* as many as fit with half the memory, the rest is for albums */
        int fit = mem_left / 2 / sizeof(struct photo);
        if (count > fit)
            count = fit;
        photos = alloc(count * sizeof(struct photo));
        if (!photos)
            return false;
    }
    else
    {
        albums = alloc(count * sizeof(struct album));
        if (!albums)
            return false;
    }

    off_t item = pos + header_len;
    for (int i = 0; i < count; i++)
    {
        p = db_get(item, 12);
        if (!p)
            break;
        uint32_t total = le32(p + 8); /* before parsing reloads the cache */
        if (images)
            parse_mhii(item, &photos[photo_count++]);
        else
            parse_mhba(item, &albums[album_count++]);
        if (total == 0)
            break;
        item += total;
    }
    return true;
}

static int photo_cmp(const void *a, const void *b)
{
    uint32_t x = ((const struct photo *)a)->id;
    uint32_t y = ((const struct photo *)b)->id;
    return (x > y) - (x < y);
}

static int find_photo(uint32_t id)
{
    int lo = 0, hi = photo_count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (photos[mid].id == id)
            return mid;
        if (photos[mid].id < id)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static bool load_database(void)
{
    db_fd = rb->open(PHOTO_DB, O_RDONLY);
    if (db_fd < 0)
        return false;
    db_cache_pos = db_cache_len = 0;

    const unsigned char *p = db_record(0, "mhfd", 24);
    if (p)
    {
        uint32_t header_len = le32(p + 4);
        int sections = le32(p + 20);
        off_t pos = header_len;
        for (int i = 0; i < sections; i++)
        {
            p = db_record(pos, "mhsd", 14);
            if (!p)
                break;
            uint32_t len = le32(p + 4), total = le32(p + 8);
            int index = le16(p + 12);
            if (index == 1 && !photos)
                parse_list(pos + len, true);
            else if (index == 2 && !albums)
                parse_list(pos + len, false);
            if (total == 0)
                break;
            pos += total;
        }
    }
    rb->close(db_fd);
    db_fd = -1;

    /* album items as indexes in photos[] */
    rb->qsort(photos, photo_count, sizeof(struct photo), photo_cmp);
    for (int i = 0; i < album_count; i++)
    {
        struct album *album = &albums[i];
        int n = 0;
        for (int j = 0; j < album->count; j++)
        {
            int index = find_photo(album->items[j]);
            if (index >= 0)
                album->items[n++] = index;
        }
        album->count = n;
    }
    /* the Photo Library first, the albums in their order */
    int library = -1;
    for (int i = 0; i < album_count && library < 0; i++)
        if (albums[i].library)
            library = i;
    if (library > 0)
    {
        struct album album = albums[library];
        rb->memmove(&albums[1], &albums[0], library * sizeof(struct album));
        albums[0] = album;
    }
    else if (library < 0 && photo_count > 0)
    {
        /* no library in the database: one with all the photos */
        struct album *all = alloc((album_count + 1) * sizeof(struct album));
        uint32_t *items = alloc(photo_count * sizeof(uint32_t));
        if (all && items)
        {
            rb->memset(&all[0], 0, sizeof(struct album));
            all[0].library = true;
            all[0].count = photo_count;
            all[0].items = items;
            for (int i = 0; i < photo_count; i++)
                items[i] = i;
            if (album_count > 0)
                rb->memcpy(&all[1], albums, album_count * sizeof(struct album));
            albums = all;
            album_count++;
        }
    }
    return photo_count > 0;
}

/* thumbnails and photos from the ithmb files */

struct ithmb {
    int format;
    int number;
    int fd;
};
static struct ithmb grid_ithmb = { GRID_FORMAT, 0, -1 };
static struct ithmb screen_ithmb = { SCREEN_FORMAT, 0, -1 };

static bool read_frame(struct ithmb *file, int number, uint32_t offset,
                       void *buf, size_t size)
{
    if (number == 0)
        return false;
    if (file->fd < 0 || file->number != number)
    {
        char path[MAX_PATH];
        if (file->fd >= 0)
            rb->close(file->fd);
        rb->snprintf(path, sizeof(path), PHOTOS_DIR "/Thumbs/F%d_%d.ithmb",
                     file->format, number);
        file->fd = rb->open(path, O_RDONLY);
        file->number = number;
        if (file->fd < 0)
            return false;
    }
    return rb->lseek(file->fd, offset, SEEK_SET) == (off_t)offset
        && rb->read(file->fd, buf, size) == (ssize_t)size;
}

static void close_frames(void)
{
    if (grid_ithmb.fd >= 0)
        rb->close(grid_ithmb.fd);
    if (screen_ithmb.fd >= 0)
        rb->close(screen_ithmb.fd);
    grid_ithmb.fd = screen_ithmb.fd = -1;
}

/* the title bar of the grid, full width, made of the theme's backdrop */

static void init_title_bar(void)
{
    struct viewport vp;
    rb->viewport_set_defaults(&vp, SCREEN_MAIN);
    title_h = vp.y;

    int font = rb->font_load(TITLE_FONT);
    if (font >= 0)
        title_font = font;
    int font_h = rb->font_get(title_font)->height;
    if (title_h < font_h + 4)
        title_h = font_h + 4;

    grid_x0 = (LCD_WIDTH - (GRID_COLS * CELL_W + 1) - (SCROLLBAR_W + 2)) / 2;
    grid_y0 = title_h + (LCD_HEIGHT - title_h - (GRID_ROWS * CELL_H + 1)) / 2;

    title_bar = alloc(LCD_WIDTH * title_h * sizeof(fb_data));
    fb_data *backdrop = rb->lcd_get_backdrop();
    if (!title_bar)
        return;
    for (int y = 0; y < title_h; y++)
    {
        fb_data colour = backdrop ? backdrop[y * LCD_WIDTH + 1]
                                  : FB_SCALARPACK_LCD(rb->global_settings->lss_color);
        for (int x = 0; x < LCD_WIDTH; x++)
            title_bar[y * LCD_WIDTH + x] = colour;
    }
}

static void draw_battery(int x, int y)
{
    int level = rb->battery_level();
    rb->lcd_set_foreground(LCD_RGBPACK(0x60, 0x60, 0x60));
    rb->lcd_drawrect(x, y, BATTERY_W - 2, BATTERY_H);
    rb->lcd_fillrect(x + BATTERY_W - 2, y + 3, 2, BATTERY_H - 6);
    if (level > 0)
    {
        rb->lcd_set_foreground(level <= 10 ? LCD_RGBPACK(0xe0, 0x30, 0x20)
                                           : LCD_RGBPACK(0x4c, 0xc2, 0x2c));
        rb->lcd_fillrect(x + 2, y + 2, (BATTERY_W - 6) * level / 100,
                         BATTERY_H - 4);
    }
}

static void draw_title(const char *title)
{
    if (title_bar)
        rb->lcd_bitmap(title_bar, 0, 0, LCD_WIDTH, title_h);
    rb->lcd_setfont(title_font);
    rb->lcd_set_drawmode(DRMODE_FG);
    rb->lcd_set_foreground(rb->global_settings->fg_color);
    int w, h;
    rb->lcd_getstringsize(title, &w, &h);
    rb->lcd_putsxy(6, (title_h - h) / 2, title);
    rb->lcd_set_drawmode(DRMODE_SOLID);
    draw_battery(LCD_WIDTH - 6 - BATTERY_W, (title_h - BATTERY_H) / 2);
    rb->lcd_setfont(FONT_UI);
}

/* the grid */

static const char *album_name(const struct album *album)
{
    return album->library ? TEXT(LANG_PHOTO_LIBRARY) : album->name;
}

/* the top left of a cell, its lines included */
static void cell_pos(int cell, int *x, int *y)
{
    *x = grid_x0 + (cell % GRID_COLS) * CELL_W;
    *y = grid_y0 + (cell / GRID_COLS) * CELL_H;
}

static void update_cell(int cell)
{
    int x, y;
    cell_pos(cell, &x, &y);
    rb->lcd_update_rect(x, y, CELL_W + 1, CELL_H + 1);
}

static void draw_cell(const struct album *album, int top, int index,
                      bool selected)
{
    if (index >= album->count)
        return;
    int x, y;
    cell_pos(index - top, &x, &y);
    /* the lines around it, shared with the next cells */
    rb->lcd_set_foreground(selected ? GRID_SELECT : GRID_LINE);
    rb->lcd_drawrect(x, y, CELL_W + 1, CELL_H + 1);
    x++;
    y++;

    const struct photo *photo = &photos[album->items[index]];
    if (read_frame(&grid_ithmb, photo->grid_file, photo->grid_offset,
                   grid_buf, grid_stride * GRID_H * sizeof(fb_data)))
    {
        if (photo->grid_w < GRID_W || photo->grid_h < GRID_H)
        {
            rb->lcd_set_foreground(rb->global_settings->bg_color);
            rb->lcd_fillrect(x, y, GRID_W, GRID_H);
        }
        rb->lcd_bitmap_part(grid_buf, photo->grid_x, photo->grid_y,
                            grid_stride, x + (GRID_W - photo->grid_w) / 2,
                            y + (GRID_H - photo->grid_h) / 2,
                            photo->grid_w, photo->grid_h);
    }
    else
    {
        /* a grey square with a dot, as the stock iPod shows */
        rb->lcd_set_foreground(GRID_EMPTY);
        rb->lcd_fillrect(x, y, GRID_W, GRID_H);
        rb->lcd_set_foreground(LCD_RGBPACK(0x90, 0x90, 0x90));
        rb->lcd_fillrect(x + GRID_W / 2 - 3, y + GRID_H / 2 - 3, 6, 6);
    }
    if (selected)
    {
        /* the frame is two pixels wide */
        rb->lcd_set_foreground(GRID_SELECT);
        rb->lcd_drawrect(x, y, GRID_W, GRID_H);
    }
}

static void draw_scrollbar(const struct album *album, int top)
{
    int rows = (album->count + GRID_COLS - 1) / GRID_COLS;
    if (rows <= GRID_ROWS)
        return;
    int x = grid_x0 + GRID_COLS * CELL_W + 3;
    int h = GRID_ROWS * CELL_H + 1;
    rb->lcd_set_foreground(rb->global_settings->bg_color);
    rb->lcd_fillrect(x, grid_y0, SCROLLBAR_W, h);
    rb->lcd_set_foreground(GRID_LINE);
    rb->lcd_drawrect(x, grid_y0, SCROLLBAR_W, h);
    int size = MAX((h - 2) * GRID_ROWS / rows, 6);
    int pos = (h - 2 - size) * (top / GRID_COLS) / (rows - GRID_ROWS);
    rb->lcd_set_foreground(rb->global_settings->lse_color);
    rb->lcd_fillrect(x + 1, grid_y0 + 1 + pos, SCROLLBAR_W - 2, size);
}

static void draw_grid(const struct album *album, int top, int selected)
{
    rb->lcd_set_viewport(NULL);
    draw_title(album_name(album));
    rb->lcd_set_foreground(rb->global_settings->bg_color);
    rb->lcd_fillrect(0, title_h, LCD_WIDTH, LCD_HEIGHT - title_h);
    for (int i = 0; i < GRID_COLS * GRID_ROWS; i++)
        if (top + i != selected)
            draw_cell(album, top, top + i, false);
    /* last, so that its frame stays on the shared lines */
    draw_cell(album, top, selected, true);
    draw_scrollbar(album, top);
    rb->lcd_update();
}

/* full screen and the slideshow */

static void show_photo(const struct photo *photo)
{
    if (read_frame(&screen_ithmb, photo->screen_file, photo->screen_offset,
                   screen_buf, LCD_WIDTH * LCD_HEIGHT * sizeof(fb_data)))
        rb->lcd_bitmap(screen_buf, 0, 0, LCD_WIDTH, LCD_HEIGHT);
    else
    {
        rb->lcd_set_foreground(LCD_BLACK);
        rb->lcd_fillrect(0, 0, LCD_WIDTH, LCD_HEIGHT);
    }
    rb->lcd_update();
}

/* Shows the album from photo index; returns the photo it ended on, or -1 on
   USB. Play starts and pauses the slideshow, Menu goes back. */
static int view_photos(const struct album *album, int index, bool slideshow)
{
    int count = album->count;
    int *order = NULL;
    unsigned char *mem_mark = mem;
    size_t mem_left_mark = mem_left;

    if (slideshow && slide_shuffle && count > 1)
    {
        order = alloc(count * sizeof(int));
        if (order)
        {
            /* the chosen photo first, then the others in random order */
            for (int i = 0; i < count; i++)
                order[i] = i;
            order[0] = index;
            order[index] = 0;
            rb->srand(*rb->current_tick);
            for (int i = count - 1; i > 1; i--)
            {
                int j = 1 + rb->rand() % i;
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
            index = 0;
        }
    }
#define PHOTO_AT(i) (order ? order[i] : (i))

    if (slideshow)
        backlight_ignore_timeout();
    int result = -2;
    show_photo(&photos[album->items[PHOTO_AT(index)]]);
    while (result == -2)
    {
        int seconds = slide_times[slide_time];
        int timeout = (slideshow && seconds) ? seconds * HZ : TIMEOUT_BLOCK;
        int action = get_photos_action(timeout);
        int next = index;

        switch (action)
        {
            case ACTION_NONE:
                if (!slideshow || !seconds)
                    break;
                if (index + 1 < count)
                    next = index + 1;
                else if (slide_repeat)
                    next = 0;
                else
                    result = PHOTO_AT(index);
                rb->reset_poweroff_timer();
                break;
            case ACTION_STD_NEXT:
            case ACTION_STD_NEXTREPEAT:
                if (index + 1 < count)
                    next = index + 1;
                else if (slideshow && slide_repeat)
                    next = 0;
                break;
            case ACTION_STD_PREV:
            case ACTION_STD_PREVREPEAT:
                if (index > 0)
                    next = index - 1;
                else if (slideshow && slide_repeat)
                    next = count - 1;
                break;
            case ACTION_PHOTOS_PLAY:
                slideshow = !slideshow;
                if (slideshow)
                    backlight_ignore_timeout();
                else
                    backlight_use_settings();
                break;
            case ACTION_STD_CANCEL:
                result = PHOTO_AT(index);
                break;
            default:
                if (rb->default_event_handler(action) == SYS_USB_CONNECTED)
                    result = -1;
                break;
        }
        if (next != index && result == -2)
        {
            index = next;
            show_photo(&photos[album->items[PHOTO_AT(index)]]);
        }
    }
#undef PHOTO_AT
    backlight_use_settings();
    mem = mem_mark;
    mem_left = mem_left_mark;
    return result;
}

/* the grid of an album; false on USB */
static bool browse_album(const struct album *album)
{
    if (album->count == 0)
    {
        rb->splash(HZ * 2, TEXT(LANG_NO_PHOTOS));
        return true;
    }

    rb->viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    const int page = GRID_COLS * GRID_ROWS;
    int selected = 0, top = 0;
    bool usb = false, done = false;

    draw_grid(album, top, selected);
    while (!done)
    {
        int action = get_photos_action(HZ);
        int next = selected;
        switch (action)
        {
            case ACTION_STD_NEXT:
            case ACTION_STD_NEXTREPEAT:
                if (selected + 1 < album->count)
                    next = selected + 1;
                break;
            case ACTION_STD_PREV:
            case ACTION_STD_PREVREPEAT:
                if (selected > 0)
                    next = selected - 1;
                break;
            case ACTION_STD_OK:
            case ACTION_PHOTOS_PLAY:
            {
                int shown = view_photos(album, selected,
                                        action == ACTION_PHOTOS_PLAY);
                if (shown < 0)
                {
                    usb = done = true;
                    break;
                }
                next = selected = shown;
                top = -1; /* redraw it all */
                break;
            }
            case ACTION_STD_CANCEL:
                done = true;
                break;
            case ACTION_NONE:
                /* keep the battery current */
                draw_title(album_name(album));
                rb->lcd_update_rect(0, 0, LCD_WIDTH, title_h);
                break;
            default:
                if (rb->default_event_handler(action) == SYS_USB_CONNECTED)
                    usb = done = true;
                break;
        }
        if (done)
            break;

        /* scroll a row at a time */
        int new_top = top < 0 ? 0 : top;
        if (next < new_top)
            new_top = next / GRID_COLS * GRID_COLS;
        else if (next >= new_top + page)
            new_top = (next / GRID_COLS - (GRID_ROWS - 1)) * GRID_COLS;

        if (new_top != top)
        {
            top = new_top;
            selected = next;
            draw_grid(album, top, selected);
        }
        else if (next != selected)
        {
            draw_cell(album, top, selected, false);
            update_cell(selected - top);
            selected = next;
            draw_cell(album, top, selected, true);
            update_cell(selected - top);
        }
    }
    close_frames();
    rb->viewportmanager_theme_undo(SCREEN_MAIN, !usb);
    return !usb;
}

/* the lists, which look like the rest of the menus */

static bool has_children(int item, void *data)
{
    (void)item;
    (void)data;
    return true;
}

static const char *settings_name(int item, void *data, char *buf, size_t len)
{
    (void)data;
    (void)buf;
    (void)len;
    static const int names[] = {
        LANG_TIME_PER_SLIDE, LANG_REPEAT, LANG_SHUFFLE_PHOTOS
    };
    return TEXT(names[item]);
}

static void set_slide_time(void)
{
    static char texts[ARRAYLEN(slide_times)][24];
    struct opt_items items[ARRAYLEN(slide_times)];
    for (unsigned i = 0; i < ARRAYLEN(slide_times); i++)
    {
        if (slide_times[i] == 0)
            rb->strlcpy(texts[i], TEXT(LANG_SLIDESHOW_MANUAL),
                        sizeof(texts[i]));
        else
            rb->snprintf(texts[i], sizeof(texts[i]),
                         TEXT(LANG_SLIDE_SECONDS), slide_times[i]);
        items[i].string = texts[i];
        items[i].voice_id = -1;
    }
    rb->set_option(TEXT(LANG_TIME_PER_SLIDE), &slide_time, RB_INT, items,
                   ARRAYLEN(slide_times), NULL);
}

static bool slideshow_settings(void)
{
    struct gui_synclist list;
    rb->gui_synclist_init(&list, settings_name, NULL, false, 1, NULL);
    list.callback_has_children = has_children;
    rb->gui_synclist_set_nb_items(&list, 3);
    rb->gui_synclist_set_title(&list, TEXT(LANG_SLIDESHOW_SETTINGS),
                               Icon_NOICON);
    rb->gui_synclist_draw(&list);
    while (true)
    {
        int action = rb->get_action(CONTEXT_LIST, HZ / 2);
        if (rb->gui_synclist_do_button(&list, &action))
            continue;
        switch (action)
        {
            case ACTION_STD_OK:
                switch (rb->gui_synclist_get_sel_pos(&list))
                {
                    case 0:
                        set_slide_time();
                        break;
                    case 1:
                        rb->set_bool_options(TEXT(LANG_REPEAT),
                                             &slide_repeat,
                                             TEXT(LANG_ON), LANG_ON,
                                             TEXT(LANG_OFF), LANG_OFF,
                                             NULL);
                        break;
                    case 2:
                        rb->set_bool_options(TEXT(LANG_SHUFFLE_PHOTOS),
                                             &slide_shuffle,
                                             TEXT(LANG_ON), LANG_ON,
                                             TEXT(LANG_OFF), LANG_OFF,
                                             NULL);
                        break;
                }
                rb->gui_synclist_set_title(&list,
                                           TEXT(LANG_SLIDESHOW_SETTINGS),
                                           Icon_NOICON);
                rb->gui_synclist_draw(&list);
                break;
            case ACTION_STD_CANCEL:
                return true;
            default:
                if (rb->default_event_handler(action) == SYS_USB_CONNECTED)
                    return false;
                break;
        }
    }
}

static const char *photos_name(int item, void *data, char *buf, size_t len)
{
    (void)data;
    (void)buf;
    (void)len;
    if (item == 0)
        return TEXT(LANG_SLIDESHOW_SETTINGS);
    return album_name(&albums[item - 1]);
}

static enum plugin_status photos_menu(void)
{
    struct gui_synclist list;
    rb->gui_synclist_init(&list, photos_name, NULL, false, 1, NULL);
    list.callback_has_children = has_children;
    rb->gui_synclist_set_nb_items(&list, album_count + 1);
    rb->gui_synclist_set_title(&list, TEXT(LANG_PHOTOS), Icon_NOICON);
    rb->gui_synclist_select_item(&list, album_count > 0 ? 1 : 0);
    rb->gui_synclist_draw(&list);
    while (true)
    {
        int action = rb->get_action(CONTEXT_LIST, HZ / 2);
        if (rb->gui_synclist_do_button(&list, &action))
            continue;
        switch (action)
        {
            case ACTION_STD_OK:
            {
                int item = rb->gui_synclist_get_sel_pos(&list);
                bool ok = (item == 0) ? slideshow_settings()
                                      : browse_album(&albums[item - 1]);
                if (!ok)
                    return PLUGIN_USB_CONNECTED;
                rb->gui_synclist_set_title(&list, TEXT(LANG_PHOTOS),
                                           Icon_NOICON);
                rb->gui_synclist_draw(&list);
                break;
            }
            case ACTION_STD_CANCEL:
                return PLUGIN_OK;
            default:
                if (rb->default_event_handler(action) == SYS_USB_CONNECTED)
                    return PLUGIN_USB_CONNECTED;
                break;
        }
    }
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    mem = rb->plugin_get_buffer(&mem_left);

    screen_buf = alloc(LCD_WIDTH * LCD_HEIGHT * sizeof(fb_data));
    if (!screen_buf || !load_database())
    {
        rb->splash(HZ * 2, TEXT(LANG_NO_PHOTOS));
        return PLUGIN_OK;
    }
    init_title_bar();

    configfile_load(CONFIG_FILE, config, ARRAYLEN(config), CONFIG_VERSION);
    int old_time = slide_time;
    bool old_repeat = slide_repeat, old_shuffle = slide_shuffle;

    enum plugin_status status = photos_menu();

    if (slide_time != old_time || slide_repeat != old_repeat
        || slide_shuffle != old_shuffle)
        configfile_save(CONFIG_FILE, config, ARRAYLEN(config), CONFIG_VERSION);
    close_frames();
    if (title_font != FONT_UI)
        rb->font_unload(title_font);
    return status;
}
