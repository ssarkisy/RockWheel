/***************************************************************************
*             __________               __   ___.
*   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
*   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
*   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
*   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
*                     \/            \/     \/    \/            \/
*
* Copyright (C) 2007 Jonas Hurrelmann (j@outpo.st)
* Copyright (C) 2007 Nicolas Pennequin
* Copyright (C) 2007 Ariya Hidayat (ariya@kde.org) (original Qt Version)
*
* Original code: http://code.google.com/p/pictureflow/
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
#include "covers_table.h"
#include "album_years.h"
#include "albumart.h"
#include "lib/read_image.h"
#include "lib/pluginlib_actions.h"
#include "lib/pluginlib_exit.h"
#include "lib/helper.h"
#include "lib/configfile.h"
#include "lib/grey.h"
#include "lib/mylcd.h"
#include "lib/feature_wrappers.h"
#include "lib/simple_viewer.h"

/******************************* Globals ***********************************/
static fb_data *lcd_fb;

/*
 *  Targets which use plugin_get_audio_buffer() can't have playback from
 * within pictureflow itself, as the whole core audio buffer is occupied */
#define PF_PLAYBACK_CAPABLE (PLUGIN_BUFFER_SIZE > 0x10000)

#if PF_PLAYBACK_CAPABLE
#include "lib/playback_control.h"
#include "lib/mul_id3.h"
#include "on_the_go.h"
#endif

#define PF_PREV ACTION_STD_PREV
#define PF_PREV_REPEAT ACTION_STD_PREVREPEAT
#define PF_NEXT ACTION_STD_NEXT
#define PF_NEXT_REPEAT ACTION_STD_NEXTREPEAT
#define PF_SELECT ACTION_STD_OK
#define PF_CONTEXT ACTION_STD_CONTEXT
#define PF_BACK ACTION_STD_CANCEL
#define PF_MENU ACTION_STD_MENU
#define PF_WPS ACTION_TREE_WPS
#define PF_ALBUM_NEXT ACTION_LISTTREE_PGDOWN
#define PF_ALBUM_PREV ACTION_LISTTREE_PGUP

#define PF_QUIT (LAST_ACTION_PLACEHOLDER + 1)
#define PF_TRACKLIST (LAST_ACTION_PLACEHOLDER + 2)
#define PF_SORTING_NEXT (LAST_ACTION_PLACEHOLDER + 3)
#define PF_SORTING_PREV (LAST_ACTION_PLACEHOLDER + 4)
#define PF_ALBUM_FIRST (LAST_ACTION_PLACEHOLDER + 5)
#define PF_ALBUM_LAST (LAST_ACTION_PLACEHOLDER + 6)

#if defined(HAVE_SCROLLWHEEL) || CONFIG_KEYPAD == IRIVER_H10_PAD || \
    CONFIG_KEYPAD == MPIO_HD300_PAD
#if (CONFIG_KEYPAD != IPOD_1G2G_PAD) \
    && (CONFIG_KEYPAD != IPOD_3G_PAD) \
    && (CONFIG_KEYPAD != IPOD_4G_PAD) \
    && (CONFIG_KEYPAD != FIIO_M3K_PAD)
#define USE_CORE_PREVNEXT
#endif
#endif

#ifndef USE_CORE_PREVNEXT
    /* scrollwheel targets use the wheel, just as they do in lists,
     * so there's no need for a special context,
     * others use left/right here too (as oppsed to up/down in lists) */
const struct button_mapping pf_context_album_scroll[] =
{
#ifdef HAVE_TOUCHSCREEN
    {PF_PREV,         BUTTON_MIDLEFT,                 BUTTON_NONE},
    {PF_PREV_REPEAT,  BUTTON_MIDLEFT|BUTTON_REPEAT,   BUTTON_NONE},
    {PF_NEXT,         BUTTON_MIDRIGHT,                BUTTON_NONE},
    {PF_NEXT_REPEAT,  BUTTON_MIDRIGHT|BUTTON_REPEAT,  BUTTON_NONE},
#endif
#if (CONFIG_KEYPAD == IAUDIO_M3_PAD || CONFIG_KEYPAD == MROBE500_PAD)
    {PF_PREV,         BUTTON_RC_REW,              BUTTON_NONE},
    {PF_PREV_REPEAT,  BUTTON_RC_REW|BUTTON_REPEAT,BUTTON_NONE},
    {PF_NEXT,         BUTTON_RC_FF,               BUTTON_NONE},
    {PF_NEXT_REPEAT,  BUTTON_RC_FF|BUTTON_REPEAT, BUTTON_NONE},
#elif (CONFIG_KEYPAD == IPOD_1G2G_PAD) \
    || (CONFIG_KEYPAD == IPOD_3G_PAD) \
    || (CONFIG_KEYPAD == IPOD_4G_PAD) \
    || (CONFIG_KEYPAD == FIIO_M3K_PAD)
    {PF_ALBUM_PREV,     BUTTON_LEFT,                BUTTON_NONE},
#if defined(ITUNESDB_HAVE_ART) && CONFIG_KEYPAD != FIIO_M3K_PAD
    /* held, to the first album or the last */
    {PF_ALBUM_FIRST,    BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_NONE},
#else
    {PF_ALBUM_PREV,     BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_NONE},
#endif
    {PF_ALBUM_NEXT,          BUTTON_RIGHT,               BUTTON_NONE},
#if defined(ITUNESDB_HAVE_ART) && CONFIG_KEYPAD != FIIO_M3K_PAD
    {PF_ALBUM_LAST,     BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_NONE},
#else
    {PF_ALBUM_NEXT,          BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_NONE},
#endif
    {ACTION_NONE,     BUTTON_LEFT|BUTTON_REL,     BUTTON_LEFT},
    {ACTION_NONE,     BUTTON_RIGHT|BUTTON_REL,    BUTTON_RIGHT},
    {ACTION_NONE,     BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_LEFT},
    {ACTION_NONE,     BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_RIGHT},
#elif defined(BUTTON_LEFT) && defined(BUTTON_RIGHT)
    {PF_PREV,         BUTTON_LEFT,                BUTTON_NONE},
    {PF_PREV_REPEAT,  BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_NONE},
    {PF_NEXT,         BUTTON_RIGHT,               BUTTON_NONE},
    {PF_NEXT_REPEAT,  BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_NONE},
    {ACTION_NONE,     BUTTON_LEFT|BUTTON_REL,     BUTTON_LEFT},
    {ACTION_NONE,     BUTTON_RIGHT|BUTTON_REL,    BUTTON_RIGHT},
    {ACTION_NONE,     BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_LEFT},
    {ACTION_NONE,     BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_RIGHT},
#else
#warning "LEFT/RIGHT not defined!"
#endif
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_PLUGIN|1)
};
#endif /* !USE_CORE_PREVNEXT */

const struct button_mapping pf_context_buttons[] =
{
#ifdef HAVE_TOUCHSCREEN
    {PF_SELECT,       BUTTON_CENTER,              BUTTON_NONE},
    {PF_BACK,         BUTTON_BOTTOMRIGHT,         BUTTON_NONE},
#endif
#if CONFIG_KEYPAD == PHILIPS_HDD1630_PAD || \
    CONFIG_KEYPAD == GIGABEAT_PAD || CONFIG_KEYPAD == GIGABEAT_S_PAD || \
    CONFIG_KEYPAD == MROBE100_PAD || CONFIG_KEYPAD == MROBE500_PAD || \
    CONFIG_KEYPAD == PHILIPS_SA9200_PAD || CONFIG_KEYPAD == SANSA_CLIP_PAD || \
    CONFIG_KEYPAD == SANSA_FUZEPLUS_PAD || CONFIG_KEYPAD == CREATIVE_ZENXFI3_PAD || \
    CONFIG_KEYPAD == XDUOO_X3_PAD
    {PF_QUIT,         BUTTON_POWER,               BUTTON_NONE},
#if CONFIG_KEYPAD == SANSA_FUZEPLUS_PAD
    {PF_MENU,         BUTTON_SELECT|BUTTON_REPEAT,  BUTTON_SELECT},
#endif
#elif CONFIG_KEYPAD == SANSA_FUZE_PAD
    {PF_QUIT,         BUTTON_HOME|BUTTON_REPEAT,  BUTTON_NONE},
    {PF_TRACKLIST,    BUTTON_RIGHT,  BUTTON_NONE},
/* These all use short press of BUTTON_POWER for menu, map long POWER to quit
*/
#elif CONFIG_KEYPAD == SANSA_C200_PAD || CONFIG_KEYPAD == SANSA_M200_PAD || \
    CONFIG_KEYPAD == IRIVER_H10_PAD || CONFIG_KEYPAD == COWON_D2_PAD
    {PF_QUIT,         BUTTON_POWER|BUTTON_REPEAT, BUTTON_POWER},
#if CONFIG_KEYPAD == COWON_D2_PAD
    {PF_BACK,         BUTTON_POWER|BUTTON_REL,    BUTTON_POWER},
    {ACTION_NONE,     BUTTON_POWER,               BUTTON_NONE},
#endif
#elif CONFIG_KEYPAD == SANSA_E200_PAD
    {PF_QUIT,         BUTTON_POWER,               BUTTON_NONE},
#elif (CONFIG_KEYPAD == IPOD_1G2G_PAD) \
    || (CONFIG_KEYPAD == IPOD_3G_PAD) \
    || (CONFIG_KEYPAD == IPOD_4G_PAD)
    {PF_MENU,         BUTTON_MENU|BUTTON_REPEAT,  BUTTON_MENU},
    {PF_QUIT,         BUTTON_MENU|BUTTON_REL,     BUTTON_MENU},
#ifdef ITUNESDB_HAVE_ART
    /* holding Select opens the menu that Music's lists have so, Add to
       On-The-Go first, for the album or, on its songs, the song. Select with
       Menu, which restarts an iPod that hangs, and Select with Play sort the
       albums otherwise, for good: they do nothing. */
    {PF_CONTEXT,      BUTTON_SELECT|BUTTON_REPEAT, BUTTON_SELECT},
    {ACTION_NONE,     BUTTON_SELECT|BUTTON_MENU,  BUTTON_NONE},
    {ACTION_NONE,     BUTTON_SELECT|BUTTON_PLAY,  BUTTON_NONE},
#else
    {PF_SORTING_NEXT, BUTTON_SELECT|BUTTON_MENU,  BUTTON_NONE},
    {PF_SORTING_PREV, BUTTON_SELECT|BUTTON_PLAY,  BUTTON_NONE},
#endif
#elif CONFIG_KEYPAD == MPIO_HD300_PAD
    {PF_QUIT,         BUTTON_MENU|BUTTON_REPEAT,  BUTTON_MENU},
#elif CONFIG_KEYPAD == IAUDIO_M3_PAD
    {PF_QUIT,         BUTTON_RC_REC,              BUTTON_NONE},
#elif CONFIG_KEYPAD == IRIVER_H100_PAD || CONFIG_KEYPAD == IRIVER_H300_PAD
    {PF_QUIT,         BUTTON_OFF,                 BUTTON_NONE},
#elif CONFIG_KEYPAD == PBELL_VIBE500_PAD
    {PF_QUIT,         BUTTON_REC,                 BUTTON_NONE},
#elif CONFIG_KEYPAD == SAMSUNG_YH820_PAD || CONFIG_KEYPAD == SAMSUNG_YH92X_PAD
    {PF_QUIT,         BUTTON_REW|BUTTON_REPEAT,   BUTTON_REW},
    {PF_MENU,         BUTTON_REW|BUTTON_REL,      BUTTON_REW},
    {PF_SELECT,       BUTTON_PLAY|BUTTON_REL,     BUTTON_PLAY},
    {PF_CONTEXT,      BUTTON_FFWD|BUTTON_REPEAT,  BUTTON_FFWD},
    {PF_TRACKLIST,    BUTTON_FFWD|BUTTON_REL,     BUTTON_FFWD},
    {PF_WPS,          BUTTON_PLAY|BUTTON_REPEAT,  BUTTON_PLAY},
#elif CONFIG_KEYPAD == FIIO_M3K_PAD
    {PF_ALBUM_PREV,     BUTTON_LEFT,                BUTTON_NONE},
    {PF_ALBUM_PREV,     BUTTON_LEFT|BUTTON_REPEAT,  BUTTON_NONE},
    {PF_ALBUM_NEXT,          BUTTON_RIGHT,               BUTTON_NONE},
    {PF_ALBUM_NEXT,          BUTTON_RIGHT|BUTTON_REPEAT, BUTTON_NONE},
    {PF_MENU,         BUTTON_POWER|BUTTON_REL,    BUTTON_POWER},
    {PF_SORTING_NEXT, BUTTON_VOL_UP,              BUTTON_NONE},
    {PF_SORTING_PREV, BUTTON_VOL_DOWN,            BUTTON_NONE},
    {PF_QUIT,         BUTTON_POWER|BUTTON_REPEAT, BUTTON_POWER},
    {PF_CONTEXT,      BUTTON_MENU|BUTTON_REL,     BUTTON_MENU},
    {PF_TRACKLIST,    BUTTON_MENU|BUTTON_REPEAT,  BUTTON_MENU},
#endif
#if CONFIG_KEYPAD == IAUDIO_M3_PAD
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD|CONTEXT_REMOTE)
#else
    LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_TREE)
#endif
};
const struct button_mapping *pf_contexts[] =
{
#ifndef USE_CORE_PREVNEXT
    pf_context_album_scroll,
#endif
    pf_context_buttons
};

#if LCD_DEPTH < 8
#if LCD_DEPTH > 1
#define N_BRIGHT(y) LCD_BRIGHTNESS(y)
#else   /* LCD_DEPTH <= 1 */
#define N_BRIGHT(y) ((y > 127) ? 0 : 1)
#ifdef HAVE_NEGATIVE_LCD  /* m:robe 100, Clip */
#define PICTUREFLOW_DRMODE DRMODE_SOLID
#else
#define PICTUREFLOW_DRMODE (DRMODE_SOLID|DRMODE_INVERSEVID)
#endif
#endif  /* LCD_DEPTH <= 1 */
#define USEGSLIB
GREY_INFO_STRUCT
#define LCD_BUF _grey_info.buffer
#define G_PIX(r,g,b) \
    (77 * (unsigned)(r) + 150 * (unsigned)(g) + 29 * (unsigned)(b)) / 256
#define N_PIX(r,g,b) N_BRIGHT(G_PIX(r,g,b))
#define G_BRIGHT(y) (y)
#define BUFFER_WIDTH _grey_info.width
#define BUFFER_HEIGHT _grey_info.height
typedef unsigned char pix_t;
#else   /* LCD_DEPTH >= 8 */
#define LCD_BUF lcd_fb
#define G_PIX LCD_RGBPACK
#define N_PIX LCD_RGBPACK
#define G_BRIGHT(y) LCD_RGBPACK(y,y,y)
#define N_BRIGHT(y) LCD_RGBPACK(y,y,y)
#define BUFFER_WIDTH LCD_WIDTH
#define BUFFER_HEIGHT LCD_HEIGHT
typedef fb_data pix_t;
#endif  /* LCD_DEPTH >= 8 */

/* for fixed-point arithmetic, we need minimum 32-bit long
   long long (64-bit) might be useful for multiplication and division.
   32 bits exactly, so that a simulator on a 64-bit host overflows where
   the targets do. */
#define PFreal int32_t
#define PFREAL_SHIFT 10
#define PFREAL_FACTOR (1 << PFREAL_SHIFT)
#define PFREAL_ONE (1 << PFREAL_SHIFT)
#define PFREAL_HALF (PFREAL_ONE >> 1)

#define IANGLE_MAX 1024
#define IANGLE_MASK 1023

#define REFLECT_TOP (LCD_HEIGHT * 2 / 3)
#define REFLECT_HEIGHT (LCD_HEIGHT - REFLECT_TOP)
#define DISPLAY_HEIGHT REFLECT_TOP
#ifdef ITUNESDB_HAVE_ART
/* The middle cover as large as the slides the core makes, its top this far down:
   a stock iPod's, with the room of the status bar it has given to it */
#define DISPLAY_WIDTH ITUNESDB_SLIDE_SIZE
#define COVER_FLOW_TOP (LCD_HEIGHT * 11 / 120)
/* the album and its artist under it this far apart: 22 on a screen 240 high */
#define COVER_FLOW_LINES (LCD_HEIGHT * 11 / 120)
#define DISPLAY_OFFS (DISPLAY_WIDTH - LCD_HEIGHT / 2 + COVER_FLOW_TOP)
#else
#define DISPLAY_WIDTH MAX((LCD_HEIGHT * LCD_PIXEL_ASPECT_HEIGHT / \
    LCD_PIXEL_ASPECT_WIDTH / 2), (LCD_WIDTH * 2 / 5))
#define DISPLAY_OFFS ((LCD_HEIGHT / 2) - REFLECT_HEIGHT)
#endif
#define REFLECT_SC ((0x10000U * 3 + (REFLECT_HEIGHT * 5 - 1)) / \
    (REFLECT_HEIGHT * 5))
#ifdef ITUNESDB_HAVE_ART
/* further away, as a stock iPod's: the far edges of the covers aside are less
   smaller than their near ones */
#define CAM_DIST (LCD_HEIGHT * 5 / 3)
#else
#define CAM_DIST MAX(MIN(LCD_HEIGHT,LCD_WIDTH),120)
#endif
#define CAM_DIST_R (CAM_DIST << PFREAL_SHIFT)
#define DISPLAY_LEFT_R (PFREAL_HALF - LCD_WIDTH * PFREAL_HALF)
#define MAXSLIDE_LEFT_R (PFREAL_HALF - DISPLAY_WIDTH * PFREAL_HALF)

#define SLIDE_CACHE_SIZE 64 /* probably more than can be loaded */

#define MAX_SLIDES_COUNT 10

#define THREAD_STACK_SIZE (DEFAULT_STACK_SIZE * 8)
#define CACHE_PREFIX PLUGIN_DEMOS_DATA_DIR "/pictureflow"
#define ALBUM_INDEX ITUNESDB_ALBUM_INDEX

#define EV_EXIT 9999
#define EV_WAKEUP 1337

/* named anew when its picture changes, as it's made again only if missing */
/* 2: of the size of the larger covers */
#define EMPTY_SLIDE CACHE_PREFIX "/emptynote2.pfraw"
/* an album whose picture isn't shown: the note's slide with a "?" in its
   place (Missing Covers Caution) */
#define UNSHOWN_SLIDE CACHE_PREFIX "/unshown1.pfraw"
#define UNSHOWN_SLIDE_BMP PLUGIN_DEMOS_DIR "/pictureflow_unshownslide.bmp"
#define EMPTY_SLIDE_BMP PLUGIN_DEMOS_DIR "/pictureflow_emptyslide.bmp"
#define SPLASH_BMP PLUGIN_DEMOS_DIR "/pictureflow_splash.bmp"

/* some magic numbers for cache_version. */
#define CACHE_REBUILD   0

/* Error return values */
#define SUCCESS              0
#define ERROR_NO_ALBUMS     -1
#define ERROR_BUFFER_FULL   -2
#define ERROR_NO_ARTISTS    -3
#define ERROR_USER_ABORT    -4

/* current version for cover cache: 5, of Cover Flow's larger covers */
#define CACHE_VERSION 5
#define CONFIG_VERSION 1

#ifdef ITUNESDB_HAVE_ART
/* As on a stock iPod: the covers on either side close together, at a steep
   angle, the nearest ones tucked behind the middle one, of which a strip of a
   third shows. There's no menu for these here, and an old config file mustn't
   keep the old look. */
#define COVER_FLOW_SPACING 27
#define COVER_FLOW_MARGIN  -18
#define COVER_FLOW_ZOOM    100
#define COVER_FLOW_TILT    72
#define COVER_FLOW_SLIDES  5
#endif
#define CONFIG_FILE "pictureflow.cfg"
/* U: with the size of the database too; V: made again for the first tracks,
   which a list made by searches lacks, so that each cover was a search of every
   track: one made at boot before the database was in memory had none */
#define INDEX_HDR "PFIV"

/** structs we use */
struct pf_config_t
{
     /* config values */
     int slide_spacing;
     int center_margin;

     int num_slides;
     int zoom;

     int auto_wps;
     int last_album;

     int backlight_mode;
     int cache_version;

     int show_album_name;
     int sort_albums_by;
     int year_sort_order;
     bool show_year;

     bool resize;
     bool show_fps;

     bool update_albumart;
};

struct pf_index_t {
    uint32_t            header; /*INDEX_HDR*/
    int32_t             commitid; /* of the database the index was made of */
    /* and its tracks and the size of its albums' file: a new database
       counts its commits from the start again, and its number can be one an
       older library's index has */
    int32_t             entries;
    int32_t             albums_size;
    uint16_t            artist_ct;
    uint16_t            album_ct;

    char               *artist_names;
    struct artist_data *artist_index;
    size_t              artist_len;

    unsigned int        album_untagged_idx;
    char               *album_names;
    struct album_data  *album_index;
    size_t              album_len;
    long                album_untagged_seek;

    void * buf;
    size_t buf_sz;
};

struct pf_track_t {
    int    count;
    int    cur_idx;
    int    sel;
    int    sel_pulse;
    int    last_sel;
    int    list_start;
    int    list_visible;
    int    list_y;
    int    list_h;
    size_t borrowed;
    size_t used;
    struct track_data *index;
    char  *names;
};

struct albumart_t {
    struct bitmap input_bmp;
    char pfraw_file[MAX_PATH];
    char file[MAX_PATH];
#ifdef HAVE_ALBUMART
    struct mp3_albumart embedded; /* type is AA_TYPE_UNKNOWN unless file
                                     is a track with the art embedded */
#endif
#ifdef ITUNESDB_HAVE_ART
    struct itunesdb_art ipod_art; /* width 0 unless Finder made a cover */
#endif
    int idx;
    int slides;
    int inspected;
    void * buf;
    size_t buf_sz;
};

struct slide_data {
    int slide_index;
    int angle;
    PFreal cx;
    PFreal cy;
    PFreal distance;
};

struct slide_cache {
    int index;      /* index of the cached slide */
    int hid;        /* handle ID of the cached slide */
    short next; /* "next" slide, with LRU last */
    short prev; /* "previous" slide */
};

struct album_data {
    int name_idx;    /* offset to the album name */
    int artist_idx;  /* offset to the artist name */
    int year;        /* album year; its date once take_album_dates() has run:
                        yyyymmdd, yyyymm00 or the year alone */
    long artist_seek; /* artist taglist position */
    long seek;        /* album taglist position */
    int first_track;  /* its first track in the database, -1 if not known */
};

struct artist_data {
    int name_idx; /* offset to the artist name */
    long seek;    /* artist taglist position */
};

struct track_data {
    uint32_t sort;
    int name_idx;       /* offset to the track name */
    long seek;
    int length;         /* in seconds */
#if PF_PLAYBACK_CAPABLE
    /* offset to the filename in the string, needed for playlist generation */
    int filename_idx;
#endif
};

struct rect {
    int left;
    int right;
    int top;
    int bottom;
};

struct load_slide_event_data {
    int slide_index;
    int cache_index;
};

struct pf_slide_cache
{
    struct slide_cache cache[SLIDE_CACHE_SIZE];
    int free;
    int used;
    int left_idx;
    int right_idx;
    int center_idx;
};

enum pf_scroll_line_type {
    PF_SCROLL_TRACK = 0,
    PF_SCROLL_ALBUM,
    PF_SCROLL_ARTIST,
    PF_MAX_SCROLL_LINES
};

struct pf_scroll_line_info {
    long ticks;         /* number of ticks between each move */
    long delay;         /* number of ticks to delay starting scrolling */
    int step;           /* pixels to move */
    long next_scroll;   /* tick of the next move */
};

struct pf_scroll_line {
    int width;          /* width of the string */
    int room;           /* of the place it's in */
    int offset;         /* x coordinate of the string */
    int step;           /* 0 if scroll is disabled. otherwise, pixels to move */
    long start_tick;    /* tick when to start scrolling */
};

struct pfraw_header {
    int32_t width;          /* bmap width in pixels */
    int32_t height;         /* bmap height in pixels */
};

enum show_album_name_values {
    ALBUM_NAME_HIDE = 0,
    ALBUM_NAME_BOTTOM,
    ALBUM_NAME_TOP,
    ALBUM_AND_ARTIST_TOP,
    ALBUM_AND_ARTIST_BOTTOM
};
static char* show_album_name_conf[] =
{
    "hide",
    "bottom",
    "top",
    "both top",
    "both bottom",
};

enum sort_albums_by_values {
    SORT_BY_ARTIST_AND_NAME = 0,
    SORT_BY_ARTIST_AND_YEAR,
    SORT_BY_YEAR,
    SORT_BY_NAME,

    SORT_VALUES_SIZE
};
static char* sort_albums_by_conf[] =
{
    "artist + name",
    "artist + year",
    "year",
    "name"
};
enum year_sort_order_values {
    ASCENDING = 0,
    DESCENDING
};
static char* year_sort_order_conf[] =
{
    "ascending",
    "descending"
};

#define MAX_SPACING 40
#define MAX_MARGIN 80

static struct albumart_t aa_cache;
static struct pf_config_t pf_cfg;

static struct configdata config[] =
{
    { TYPE_INT, 0, MAX_SPACING, { .int_p = &pf_cfg.slide_spacing }, "slide spacing",
      NULL },
    { TYPE_INT, 0, MAX_MARGIN, { .int_p = &pf_cfg.center_margin }, "center margin",
      NULL },
    { TYPE_INT, 0, MAX_SLIDES_COUNT, { .int_p = &pf_cfg.num_slides }, "slides count",
      NULL },
    { TYPE_INT, 0, 300, { .int_p = &pf_cfg.zoom }, "zoom", NULL },
    { TYPE_BOOL, 0, 1, { .bool_p = &pf_cfg.show_fps }, "show fps", NULL },
    { TYPE_BOOL, 0, 1, { .bool_p = &pf_cfg.resize }, "resize", NULL },
    { TYPE_INT, 0, 100, { .int_p = &pf_cfg.cache_version }, "cache version", NULL },
    { TYPE_ENUM, 0, 5, { .int_p = &pf_cfg.show_album_name }, "show album name",
      show_album_name_conf },
    { TYPE_INT, 0, 2, { .int_p = &pf_cfg.auto_wps }, "auto wps", NULL },
    { TYPE_INT, 0, 999999, { .int_p = &pf_cfg.last_album }, "last album", NULL },
    { TYPE_INT, 0, 1, { .int_p = &pf_cfg.backlight_mode }, "backlight", NULL },
    { TYPE_INT, 0, 999999, { .int_p = &aa_cache.idx }, "art cache pos", NULL },
    { TYPE_INT, 0, 999999, { .int_p = &aa_cache.inspected }, "art cache inspected", NULL },
    { TYPE_ENUM, 0, 4, { .int_p = &pf_cfg.sort_albums_by }, "sort albums by",
      sort_albums_by_conf },
    { TYPE_ENUM, 0, 2, { .int_p = &pf_cfg.year_sort_order }, "year order",
      year_sort_order_conf },
    { TYPE_BOOL, 0, 1, { .bool_p = &pf_cfg.show_year }, "show year", NULL },
    { TYPE_BOOL, 0, 1, { .bool_p = &pf_cfg.update_albumart }, "update albumart", NULL }
};

#define CONFIG_NUM_ITEMS (sizeof(config) / sizeof(struct configdata))

/** below we allocate the memory we want to use **/

static pix_t *buffer; /* for now it always points to the lcd framebuffer */
static uint8_t reflect_table[REFLECT_HEIGHT];
static struct slide_data center_slide;
static struct slide_data left_slides[MAX_SLIDES_COUNT];
static struct slide_data right_slides[MAX_SLIDES_COUNT];
static int slide_frame;
static int step;
static int target;
/* With a wheel that turns fast the covers roll: they go at a speed that follows
   the wheel's, and when it's let go, what they follow runs down. In 1/65536 of a
   slide a frame; roll_speed is 0 when they aren't rolling. */
static int roll_speed;
static int roll_aim;
static bool roll_free; /* the wheel has been let go */
static int roll_carry; /* what they had when a finger came back on the wheel */
static bool roll_settling; /* slowing down for good, onto a cover already chosen */
static int roll_settle_time; /* in 1/16 frames: how fast it slows */
#ifdef HAVE_WHEEL_POSITION
static bool wheel_timer_set;
static bool wheel_events = true; /* the driver's, off over the covers */
static bool wheel_waking; /* the touch that lit the dark screen goes on */
static void wheel_start(void);
static void wheel_clicks_taken(void);
#else
static inline void wheel_start(void) {}
static inline void wheel_clicks_taken(void) {}
#endif
static int anim_speed; /* what they last moved by */
static bool anim_turning; /* just turned back, in the same place */
/* << or >> held: on to the first album or the last, quicker frame by frame,
   slowing onto it; what a frame may move so far, in 1/65536 of a slide */
static bool seeking;
static int seek_cap;
/* the wheel's clicks a slide: 16 slides a turn, slower than the lists' 24, as
   users found the covers too quick even turned slowly */
#define WHEEL_SLIDE_CLICKS  6
/* of a click a second, at 25 frames a second: at the speed of the wheel */
#define ROLL_PER_CLICK      (65536 / (WHEEL_SLIDE_CLICKS * 25))
#define ROLL_SPEED_MAX      81920 /* a slide and a quarter, from one turn */
#define ROLL_SPEED_TOP      131072 /* two slides, from turns one after the other */
#define ROLL_SPEED_MIN      18000 /* about what the stop on the next slide starts with */
#define ROLL_LAST_SLIDES    6554  /* the most for each slide left, with what's below */
#define ROLL_LAST_SPEED     22000
#define ROLL_FOLLOW         3     /* 1/n of the difference is made up in a frame */
#define ROLL_FRICTION       24    /* 1/n of what they follow is lost in a frame */
#define ROLL_CLICKS_MAX     288   /* a second, 3 turns: over a few moments it's no surer */
#define ROLL_TRAVEL_FULL    64    /* clicks; with less, it's that much of the wheel's speed */
/* The end of a roll: what's left of the way is lost at a steady rate, so that the speed
   falls by the same share every frame, as it does on an iPod touch, and they glide on
   for about three covers. The cover they land on is chosen so that the speed doesn't
   change where this begins. */
#define ROLL_SETTLE_FRAMES  11
#define ROLL_SETTLE_MIN     900   /* the last of it, or it would never get there */
/* The animations go by the time and not by the frames drawn, so that the covers
   are as quick with music playing as without, and the wheel's speed is theirs: in
   the frames of 1/25 s that all of the above is in, ANIM_ONE to a frame */
#define ANIM_FRAME_US       (1000000 / 25)
#define ANIM_ONE            256
#define ANIM_MOST           (3 * ANIM_ONE) /* after a stall, no more than that at once */
#if CONFIG_CPU == PP5020 || CONFIG_CPU == PP5022 || CONFIG_CPU == PP5024
/* the SoC's microsecond timer, USEC_TIMER, whose header plugins don't see */
#define PF_USEC_TIMER (*(volatile unsigned long *)0x60005010)
#endif
static unsigned long anim_clock;
static bool anim_clock_running;
static int anim_part;  /* the time of this frame */
static int anim_keys;  /* toward the next key frame of the zoom */
static int fade;
static int center_index = 0; /* index of the slide that is in the center */
static int itilt;
static PFreal offsetX;
static PFreal offsetY;
static int number_of_slides;
static bool is_initial_slide = true;
static bool show_tracks_while_browsing = false;

static struct pf_slide_cache pf_sldcache;

/* use long for aligning */
unsigned long thread_stack[THREAD_STACK_SIZE / sizeof(long)];
/* queue (as array) for scheduling load_surface */

static int empty_slide_hid;
static int unshown_slide_hid = -1; /* -1 without it: the note's then */

unsigned int thread_id;
struct event_queue thread_q;

static struct tagcache_search tcs;

static struct buflib_context buf_ctx;

static struct pf_index_t pf_idx;

static struct pf_track_t pf_tracks;

static struct mp3entry id3;
static bool id3_from_ramcache; /* tags only, no embedded album art */

void reset_track_list(void);

static bool thread_is_running;
static bool wants_to_quit;
/* the slides the thread has loaded: still covers are drawn again only for one */
static volatile unsigned slides_arrived;

/*
    Prevent picture loading thread from allocating
    buflib memory while the main thread may be
    performing buffer-shifting operations.
*/
static struct mutex buf_ctx_mutex;
/* The main thread has buf_ctx from create_track_index() or Select until the
   album's closing ends, so that no slide loads while it turns, or until the
   tracks made ahead are done (prepare_ahead()); leaving must let it go or the
   slide thread never ends. Only that says so here: unlocking hands the mutex
   over at once to a thread of higher priority, so a flag of all holders was
   cleared after the next holder had set it. The tracks' room, borrowed below
   the slides', stays theirs without it (tracks_unlock()). */
static bool buf_ctx_locked;
/* the slide thread has slides to load, from its wakeup until a load finds none */
static volatile bool thread_loading;

static int cover_animation_keyframe;
#if defined(ITUNESDB_HAVE_ART) && defined(HAVE_LCD_COLOR)
/* The panel with its edge as a picture that turns as a cover does, column after
   column: the other side of the cover as it opens, which becomes the panel, as
   on a stock iPod. Square, as it's turned where it's drawn. Turned to its edge,
   the cover goes on turning as the card, which grows into the panel: at the
   cover's pace, from where the cover was, as tall as its edge there. Its depth
   to start with, and its rows below the horizon. */
#define CARD_FRAME_COUNT    11
#define CARD_FRAME_DIST     158
#define CARD_FRAME_OFFS     52
#define CARD_OFFS           (PANEL_Y + PANEL_H + 1 - LCD_HEIGHT / 2)
#define CARD_W              (PANEL_W + 2)
#define CARD_H              (PANEL_H + 2)
#define CARD_SQUARE         (CARD_W == CARD_H)
static struct dim *card;            /* NULL without room for it */
static bool card_drawn;             /* of the album and tracks as they are now */
static int card_album;              /* which */
static struct slide_data card_slide;
#else
#define CARD_FRAME_COUNT    0
#endif
static int card_frame; /* 0 while the cover shows, else how far the card has come */
static int card_offs(void);
static int extra_fade;
/* Light behind the covers with the light theme, as on a stock iPod, which their
   reflections and the far ones fade into; dark with the dark theme */
static bool pf_light;
static pix_t pf_back;
#ifdef ITUNESDB_HAVE_ART
/* Each album's slide as found once, so that a load is only a read: else the
   slide's number in the art index */
static int32_t *album_slides;
#define SLIDE_UNKNOWN   0   /* not looked for yet */
#define SLIDE_NONE      (-1) /* no cover */
#define SLIDE_UNSHOWN   (-3) /* none, its picture in a format not shown */
#define SLIDE_UNREAD    (-2) /* a cover without a slide that reads: made of the
                                cover each time, found again for it */
#endif

static struct pf_scroll_line_info scroll_line_info;
static struct pf_scroll_line scroll_lines[PF_MAX_SCROLL_LINES];

enum ePFS{ePFS_ARTIST = 0, ePFS_ALBUM};
/*
    Proposals for transitions:

    pf_idle -> pf_scrolling : NEXT_ALBUM/PREV_ALBUM pressed
            -> pf_cover_in -> pf_show_tracks : SELECT_ALBUM clicked

    pf_scrolling -> pf_idle : NEXT_ALBUM/PREV_ALBUM released

    pf_show_tracks -> pf_cover_out -> pf_idle : SELECT_ALBUM pressed

    TODO:
    pf_show_tracks -> pf_cover_out -> pf_idle : MENU_PRESSED pressed
    pf_show_tracks -> play_track() -> exit() : SELECT_ALBUM pressed

    pf_idle, pf_scrolling -> show_menu(): MENU_PRESSED
*/
enum pf_states {
    pf_idle = 0,
    pf_scrolling,
    pf_cover_in,
    pf_show_tracks,
    pf_cover_out
};

static int pf_state;

#if PF_PLAYBACK_CAPABLE
static bool insert_whole_album;
static bool old_shuffle = false;
static int old_playlist = -1;
#endif

/** code */
static bool free_slide_prio(int prio);
bool load_new_slide(void);
int load_surface(int);
static void draw_progressbar(int step, int count, char *msg);
static void draw_splashscreen(unsigned char * buf_tmp, size_t buf_tmp_size);
#ifdef ITUNESDB_HAVE_ART
static bool preparing; /* only the album list, on the boot screen */
#endif
static void free_all_slide_prio(int prio);

static inline void buf_ctx_lock(void)
{
    rb->mutex_lock(&buf_ctx_mutex);
}

static inline void buf_ctx_unlock(void)
{
    rb->mutex_unlock(&buf_ctx_mutex);
}

static void tracks_lock(void)
{
    if (!buf_ctx_locked)
    {
        buf_ctx_lock();
        buf_ctx_locked = true;
    }
}

/* buf_ctx back to the slide thread, the tracks kept: their room is below the
   slides', which only borrowing and giving back move, with buf_ctx */
static void tracks_unlock(void)
{
    if (buf_ctx_locked)
    {
        buf_ctx_locked = false;
        buf_ctx_unlock();
    }
}

/* What the health log is told as Cover Flow quits (cf_marks_note()): ticks,
   and microseconds for the work at Select and the frames */
static struct {
    long opened_at;         /* 0 when not counting: Updating Library's run */
    long boosted;           /* Cover Flow's own 80 MHz */
    long boosted_at;
    int opened, ready;      /* albums opened, and of them made ahead */
    unsigned long made_most, made_all; /* the making at Select, when not */
    int prepared;           /* steps made ahead at rest */
    unsigned long prepared_most;
    unsigned long frame_in, frame_out; /* the longest frame opening, closing */
    int frame_in_key;       /* the key frame that one reached */
    int darks, sleeps;      /* the screen went dark, and the iPod to sleep */
    long dark_after, sleep_after; /* the last of each, after the last use */
} cf_marks;

/* Cover Flow's hold of the 80 MHz, one at most: up while the covers move, the
   slides load or a card is made, down at rest. The boost is counted, so each
   up has its one down, as it quits too (cleanup()). */
static bool pf_boosted;
static void pf_boost(bool on)
{
    if (on == pf_boosted)
        return;
    pf_boosted = on;
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(on);
#endif
    if (on)
        cf_marks.boosted_at = *rb->current_tick;
    else
        cf_marks.boosted += *rb->current_tick - cf_marks.boosted_at;
}

static bool check_database(void)
{
    bool needwarn = true;
    int spin = 5;

    struct tagcache_stat *stat = rb->tagcache_get_stat();

    while ( !(stat->initialized && stat->ready) )
    {
        if (--spin > 0)
        {
            rb->sleep(HZ/5);
        }
        else if (needwarn)
        {
            needwarn = false;
            rb->splash(0, ID2P(LANG_TAGCACHE_BUSY));
        }
        else
            return false;

        rb->yield();
        stat = rb->tagcache_get_stat();
    }
    return true;
}

static bool progress_cancel(int step, int count, char *msg)
{
    const struct text_message prompt = {
            (const char*[]) {"Quit?", "Progress will be lost"}, 2};

    int action = rb->get_action(CONTEXT_STD,TIMEOUT_NOBLOCK);
#ifdef ITUNESDB_HAVE_ART
    if (preparing && (action == SYS_USB_CONNECTED || action == SYS_POWEROFF
                      || action == SYS_REBOOT))
    {
        /* answered once Updating Library is over, as during the covers:
           dropped here, USB waited for a reconnect */
        rb->library_update_hold(action);
        return true;
    }
    else if (preparing && IS_SYSEVENT(action))
        rb->default_event_handler(action);
#endif
    if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
    {
#ifdef ITUNESDB_HAVE_ART
        if (preparing)
            return true; /* Cover Flow makes it when it opens */
#endif
        if (rb->gui_syncyesno_run(&prompt, NULL, NULL) == YESNO_YES)
            return true;
        rb->lcd_clear_display();
    }
    else
        msg = NULL;

    if (count)
        draw_progressbar(step, count, msg);
#ifdef ITUNESDB_HAVE_ART
    else if (preparing)
        rb->library_update_progress(0, 0); /* it still works */
#endif

    return false;
}

static void config_save(int cache_version, bool update_albumart)
{
    pf_cfg.cache_version = cache_version;
    pf_cfg.update_albumart = update_albumart;
    configfile_save(CONFIG_FILE, config, CONFIG_NUM_ITEMS, CONFIG_VERSION);
}

static void config_set_defaults(struct pf_config_t *cfg)
{
     cfg->slide_spacing = DISPLAY_WIDTH / 4;
     cfg->center_margin = (LCD_WIDTH - DISPLAY_WIDTH) / 12;
     cfg->num_slides = 4;
     cfg->zoom = 100;
     cfg->show_fps = false;
     cfg->auto_wps = 0;
     cfg->last_album = 0;
     cfg->backlight_mode = 0;
     cfg->resize = true;
     cfg->cache_version = CACHE_REBUILD;
     cfg->show_album_name = (LCD_HEIGHT > 100)
        ? ALBUM_AND_ARTIST_BOTTOM : ALBUM_NAME_BOTTOM;
     cfg->sort_albums_by = SORT_BY_ARTIST_AND_NAME;
     cfg->year_sort_order = ASCENDING;
     cfg->show_year = false;
     cfg->update_albumart = false;
}

static inline PFreal fmul(PFreal a, PFreal b)
{
    return (a*b) >> PFREAL_SHIFT;
}

/**
 * This version preshifts each operand, which is useful when we know how many
 * of the least significant bits will be empty, or are worried about overflow
 * in a particular calculation
 */
static inline PFreal fmuln(PFreal a, PFreal b, int ps1, int ps2)
{
    return ((a >> ps1) * (b >> ps2)) >> (PFREAL_SHIFT - ps1 - ps2);
}

/* ARMv5+ has a clz instruction equivalent to our function.
 */
#if (defined(CPU_ARM) && (ARM_ARCH > 4))
static inline int clz(uint32_t v)
{
    return __builtin_clz(v);
}

/* Otherwise, use our clz, which can be inlined */
#elif defined(CPU_COLDFIRE)
/* This clz is based on the log2(n) implementation at
 * http://graphics.stanford.edu/~seander/bithacks.html#IntegerLog
 * A clz benchmark plugin showed this to be about 14% faster on coldfire
 * than the LUT-based version.
 */
static inline int clz(uint32_t v)
{
    int r = 32;
    if (v >= 0x10000)
    {
        v >>= 16;
        r -= 16;
    }
    if (v & 0xff00)
    {
        v >>= 8;
        r -= 8;
    }
    if (v & 0xf0)
    {
        v >>= 4;
        r -= 4;
    }
    if (v & 0xc)
    {
        v >>= 2;
        r -= 2;
    }
    if (v & 2)
    {
        v >>= 1;
        r -= 1;
    }
    r -= v;
    return r;
}
#else
static const char clz_lut[16] = { 4, 3, 2, 2, 1, 1, 1, 1,
                                  0, 0, 0, 0, 0, 0, 0, 0 };
/* This clz is based on the log2(n) implementation at
 * http://graphics.stanford.edu/~seander/bithacks.html#IntegerLogLookup
 * It is not any faster than the one above, but trades 16B in the lookup table
 * for a savings of 12B per each inlined call.
 */
static inline int clz(uint32_t v)
{
    int r = 28;
    if (v >= 0x10000)
    {
        v >>= 16;
        r -= 16;
    }
    if (v & 0xff00)
    {
        v >>= 8;
        r -= 8;
    }
    if (v & 0xf0)
    {
        v >>= 4;
        r -= 4;
    }
    return r + clz_lut[v];
}
#endif

/* Return the maximum possible left shift for a signed int32, without
 * overflow
 */
static inline int allowed_shift(int32_t val)
{
    uint32_t uval = val ^ (val >> 31);
    return clz(uval) - 1;
}

/* Calculate num/den, with the result shifted left by PFREAL_SHIFT, by shifting
 * num and den before dividing.
 */
static inline PFreal fdiv(PFreal num, PFreal den)
{
    int shift = allowed_shift(num);
    shift = MIN(PFREAL_SHIFT, shift);
    num <<= shift;
    den >>= PFREAL_SHIFT - shift;
    return num / den;
}

#define fmin(a,b) (((a) < (b)) ? (a) : (b))
#define fmax(a,b) (((a) > (b)) ? (a) : (b))
#define fabs(a) (a < 0 ? -a : a)
#define fbound(min,val,max) (fmax((min),fmin((max),(val))))

#define MULUQ(a, b) ((a) * (b))

#if 0
#define fmul(a,b) ( ((a)*(b)) >> PFREAL_SHIFT )
#define fdiv(n,m) ( ((n)<< PFREAL_SHIFT ) / m )

#define fconv(a, q1, q2) (((q2)>(q1)) ? (a)<<((q2)-(q1)) : (a)>>((q1)-(q2)))
#define tofloat(a, q) ( (float)(a) / (float)(1<<(q)) )

static inline PFreal fmul(PFreal a, PFreal b)
{
    return (a*b) >> PFREAL_SHIFT;
}

static inline PFreal fdiv(PFreal n, PFreal m)
{
    return (n<<(PFREAL_SHIFT))/m;
}
#endif

/* warning: regenerate the table if IANGLE_MAX and PFREAL_SHIFT are changed! */
static const short sin_tab[] = {
        0,   100,   200,   297,   392,   483,   569,   650,
      724,   792,   851,   903,   946,   980,  1004,  1019,
     1024,  1019,  1004,   980,   946,   903,   851,   792,
      724,   650,   569,   483,   392,   297,   200,   100,
        0,  -100,  -200,  -297,  -392,  -483,  -569,  -650,
     -724,  -792,  -851,  -903,  -946,  -980, -1004, -1019,
    -1024, -1019, -1004,  -980,  -946,  -903,  -851,  -792,
     -724,  -650,  -569,  -483,  -392,  -297,  -200,  -100,
        0
};

static inline PFreal fsin(int iangle)
{
    iangle &= IANGLE_MASK;

    int i = (iangle >> 4);
    PFreal p = sin_tab[i];
    PFreal q = sin_tab[(i+1)];
    PFreal g = (q - p);
    return p + g * (iangle-i*16)/16;
}

static inline PFreal fcos(int iangle)
{
    return fsin(iangle + (IANGLE_MAX >> 2));
}

/* scales the 8bit subpixel value to native lcd format, indicated by bits */
static inline unsigned scale_subpixel_lcd(unsigned val, unsigned bits)
{
    (void) bits;
#if LCD_PIXELFORMAT != RGB888
    val = val * ((1 << bits) - 1);
    val = ((val >> 8) + val + 128) >> 8;
#endif
    return val;
}

static void output_row_8_transposed(uint32_t row, void * row_in,
                                       struct scaler_context *ctx)
{
    pix_t *dest = (pix_t*)ctx->bm->data + row;
    pix_t *end = dest + ctx->bm->height * ctx->bm->width;
#ifdef USEGSLIB
    uint8_t *qp = (uint8_t*)row_in;
    for (; dest < end; dest += ctx->bm->height)
        *dest = *qp++;
#else
    struct uint8_rgb *qp = (struct uint8_rgb*)row_in;
    unsigned r, g, b;
    for (; dest < end; dest += ctx->bm->height)
    {
        r = scale_subpixel_lcd(qp->red, 5);
        g = scale_subpixel_lcd(qp->green, 6);
        b = scale_subpixel_lcd((qp++)->blue, 5);
        *dest = FB_RGBPACK_LCD(r,g,b);
    }
#endif
}

/* read_image_file() is called without FORMAT_TRANSPARENT so
 * it's safe to ignore alpha channel in the next two functions */
static void output_row_32_transposed(uint32_t row, void * row_in,
                                       struct scaler_context *ctx)
{
    pix_t *dest = (pix_t*)ctx->bm->data + row;
    pix_t *end = dest + ctx->bm->height * ctx->bm->width;
#ifdef USEGSLIB
    uint32_t *qp = (uint32_t*)row_in;
    for (; dest < end; dest += ctx->bm->height)
        *dest = SC_OUT(*qp++, ctx);
#else
    struct uint32_argb *qp = (struct uint32_argb*)row_in;
    int r, g, b;
    for (; dest < end; dest += ctx->bm->height)
    {
        r = scale_subpixel_lcd(SC_OUT(qp->r, ctx), 5);
        g = scale_subpixel_lcd(SC_OUT(qp->g, ctx), 6);
        b = scale_subpixel_lcd(SC_OUT(qp->b, ctx), 5);
        qp++;
        *dest = FB_RGBPACK_LCD(r,g,b);
    }
#endif
}

#ifdef HAVE_LCD_COLOR
static void output_row_32_transposed_fromyuv(uint32_t row, void * row_in,
                                       struct scaler_context *ctx)
{
    pix_t *dest = (pix_t*)ctx->bm->data + row;
    pix_t *end = dest + ctx->bm->height * ctx->bm->width;
    struct uint32_argb *qp = (struct uint32_argb*)row_in;
    for (; dest < end; dest += ctx->bm->height)
    {
        unsigned r, g, b, y, u, v;
        y = SC_OUT(qp->b, ctx);
        u = SC_OUT(qp->g, ctx);
        v = SC_OUT(qp->r, ctx);
        qp++;
        yuv_to_rgb(y, u, v, &r, &g, &b);
        r = scale_subpixel_lcd(r, 5);
        g = scale_subpixel_lcd(g, 6);
        b = scale_subpixel_lcd(b, 5);
        *dest = FB_RGBPACK_LCD(r, g, b);
    }
}
#endif

static unsigned int get_size(struct bitmap *bm)
{
    return bm->width * bm->height * sizeof(pix_t);
}

const struct custom_format format_transposed = {
    .output_row_8 = output_row_8_transposed,
#ifdef HAVE_LCD_COLOR
    .output_row_32 = {
        output_row_32_transposed,
        output_row_32_transposed_fromyuv
    },
#else
    .output_row_32 = output_row_32_transposed,
#endif
    .get_size = get_size
};

static const struct button_mapping* get_context_map(int context)
{
    context &= ~CONTEXT_LOCKED;
    return pf_contexts[context & ~CONTEXT_PLUGIN];
}

/* scrolling */
static void init_scroll_lines(void)
{
    int i;
    static const char scroll_tick_table[16] = {
     /* Hz values:
        1, 1.25, 1.55, 2, 2.5, 3.12, 4, 5, 6.25, 8.33, 10, 12.5, 16.7, 20, 25, 33 */
        100, 80, 64, 50, 40, 32, 25, 20, 16, 12, 10, 8, 6, 5, 4, 3
    };

    scroll_line_info.ticks = scroll_tick_table[rb->global_settings->scroll_speed];
    scroll_line_info.step = rb->global_settings->scroll_step;
    scroll_line_info.delay = rb->global_settings->scroll_delay / (HZ / 10);
    scroll_line_info.next_scroll = *rb->current_tick;
    for (i = 0; i < PF_MAX_SCROLL_LINES; i++)
        scroll_lines[i].step = 0;
}

/* str in a place room wide, in the font it's drawn in: in its middle if it fits,
   and from its left if centred is false; if it doesn't, it scrolls */
static void set_scroll_line_in(const char *str, enum pf_scroll_line_type type,
                               int room, bool centred)
{
    struct pf_scroll_line *s = &scroll_lines[type];
    s->width = mylcd_getstringsize(str, NULL, NULL);
    s->room = room;
    s->step = 0;
    s->offset = 0;
    s->start_tick = *rb->current_tick + scroll_line_info.delay;
    if (room - s->width < 0)
        s->step = scroll_line_info.step;
    else if (centred)
        s->offset = (room - s->width) / 2;
}

static void set_scroll_line(const char *str, enum pf_scroll_line_type type)
{
    set_scroll_line_in(str, type, LCD_WIDTH, true);
}

/* a line set before from its start again, after the delay, as if just set:
   the chosen track's, set as the card was made ahead */
static void restart_scroll_line(enum pf_scroll_line_type type)
{
    struct pf_scroll_line *s = &scroll_lines[type];
    if (s->step)
    {
        s->step = scroll_line_info.step;
        s->offset = 0;
    }
    s->start_tick = *rb->current_tick + scroll_line_info.delay;
}

static int get_scroll_line_offset(enum pf_scroll_line_type type)
{
    return scroll_lines[type].offset;
}

/* which of them moved, a bit for each */
static unsigned update_scroll_lines(void)
{
    int i;
    unsigned moved = 0;

    if (TIME_BEFORE(*rb->current_tick, scroll_line_info.next_scroll))
        return 0;

    scroll_line_info.next_scroll = *rb->current_tick + scroll_line_info.ticks;

    for (i = 0; i < PF_MAX_SCROLL_LINES; i++)
    {
        struct pf_scroll_line *s = &scroll_lines[i];
        if (s->step && TIME_BEFORE(s->start_tick, *rb->current_tick))
        {
            s->offset -= s->step;
            moved |= 1u << i;

            if (s->offset >= 0) {
                /* at beginning of line */
                s->offset = 0;
                s->step = scroll_line_info.step;
                s->start_tick = *rb->current_tick + scroll_line_info.delay * 2;
            }
            if (s->offset <= s->room - s->width) {
                /* at end of line */
                s->offset = s->room - s->width;
                s->step = -scroll_line_info.step;
                s->start_tick = *rb->current_tick + scroll_line_info.delay * 2;
            }
        }
    }
    return moved;
}

/* Create the lookup table with the scaling values for the reflections */
static void init_reflect_table(void)
{
    int i;
    for (i = 0; i < REFLECT_HEIGHT; i++)
        reflect_table[i] =
            (768 * (REFLECT_HEIGHT - i) + (5 * REFLECT_HEIGHT / 2)) /
            (5 * REFLECT_HEIGHT);
}


static int compare_albums (const void *a_v, const void *b_v)
{
    uint32_t artist_a = ((struct album_data *)a_v)->artist_idx;
    uint32_t artist_b = ((struct album_data *)b_v)->artist_idx;

    uint32_t album_a = ((struct album_data *)a_v)->name_idx;
    uint32_t album_b = ((struct album_data *)b_v)->name_idx;

    int year_a = ((struct album_data *)a_v)->year;
    int year_b = ((struct album_data *)b_v)->year;

#ifdef ITUNESDB_HAVE_ART
    /* by album artist, each one's oldest first and those without a date
       last, then by name, as Music's lists; or by date, the oldest first,
       then by album artist and name. The songs without an album last. */
    const bool by_year = pf_cfg.sort_albums_by == SORT_BY_YEAR;
    const bool untagged_a = album_a == pf_idx.album_untagged_idx;
    const bool untagged_b = album_b == pf_idx.album_untagged_idx;
    if (untagged_a != untagged_b)
        return untagged_a ? 1 : -1;
    if (!by_year && artist_a != artist_b)
        return artist_a < artist_b ? -1 : 1;
    const int32_t order_a = rb->album_date_order(year_a);
    const int32_t order_b = rb->album_date_order(year_b);
    if (order_a != order_b)
        return !order_a ? 1 : !order_b ? -1 : order_a < order_b ? -1 : 1;
    if (artist_a != artist_b)
        return artist_a < artist_b ? -1 : 1;
    return album_a < album_b ? -1 : album_a > album_b;
#else
    switch (pf_cfg.sort_albums_by)
    {
        case SORT_BY_ARTIST_AND_NAME:
            if (artist_a - artist_b == 0)
                return (int)(album_a - album_b);
            break;
        case SORT_BY_ARTIST_AND_YEAR:
            if (artist_a - artist_b == 0)
            {
                if (pf_cfg.year_sort_order == ASCENDING)
                    return year_a - year_b;
                else
                    return year_b - year_a;
            }
            break;
        case SORT_BY_YEAR:
            if (year_a - year_b != 0)
            {
                if (pf_cfg.year_sort_order == ASCENDING)
                    return year_a - year_b;
                else
                    return year_b - year_a;
            }
            break;
        case SORT_BY_NAME:
            if (album_a - album_b != 0)
                return (int)(album_a - album_b);
            break;
    }

    return (int)(artist_a - artist_b);
#endif
}

/* the albums' dates from Music's table of albums, in place of the years
   counted here; none for the songs without an album, as Music has them */
static void take_album_dates(void)
{
#ifdef ITUNESDB_HAVE_ART
    if (!rb->album_years_ready())
        return;
    for (int i = 0; i < pf_idx.album_ct; i++)
    {
        struct album_data *a = &pf_idx.album_index[i];
        a->year = a->name_idx == (int)pf_idx.album_untagged_idx ? 0
                  : rb->album_date(a->seek, a->artist_seek);
    }
#endif
}

static int compare_album_artists (const void *a_v, const void *b_v)
{
    uint32_t a = ((struct album_data *)a_v)->artist_idx;
    uint32_t b = ((struct album_data *)b_v)->artist_idx;
    return (int)(a - b);
}

static void write_album_index(int idx, int name_idx,
                              long album_seek, int artist_idx, long artist_seek)
{
    pf_idx.album_index[idx].name_idx = name_idx;
    pf_idx.album_index[idx].seek = album_seek;
    pf_idx.album_index[idx].artist_idx = artist_idx;
    pf_idx.album_index[idx].artist_seek = artist_seek;
    pf_idx.album_index[idx].year = 0;
    pf_idx.album_index[idx].first_track = -1;
}

static inline void write_album_entry(struct tagcache_search *tcs,
                                     int name_idx, unsigned int len)
{
    write_album_index(-pf_idx.album_ct, name_idx, tcs->result_seek, 0, -1);
    pf_idx.album_len += len;
    pf_idx.album_ct++;

    if (pf_idx.album_untagged_seek == -1 && rb->strcmp(UNTAGGED, tcs->result) == 0)
    {
        pf_idx.album_untagged_idx = name_idx;
        pf_idx.album_untagged_seek = tcs->result_seek;
    }
}

static void write_artist_entry(struct tagcache_search *tcs,
                               int name_idx, unsigned int len)
{
    pf_idx.artist_index[-pf_idx.artist_ct].name_idx = name_idx;
    pf_idx.artist_index[-pf_idx.artist_ct].seek = tcs->result_seek;
    pf_idx.artist_len += len;
    pf_idx.artist_ct++;
}

/* adds tagcache_search results into artist/album index */
static int get_tcs_search_res(int type, struct tagcache_search *tcs,
                              void **buf, size_t *bufsz)
{
    char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    int ret = SUCCESS;
    unsigned int l, name_idx = 0;
    void (*writefn)(struct tagcache_search *, int, unsigned int);
    int data_size;
    if (type == ePFS_ARTIST)
    {
        writefn = &write_artist_entry;
        data_size = sizeof(struct artist_data);
    }
    else
    {
        writefn = &write_album_entry;
        data_size = sizeof(struct album_data);
    }

    while (rb->tagcache_get_next(tcs, tcs_buf, tcs_bufsz))
    {
        if (progress_cancel(0, 0, NULL))
        {
            ret = ERROR_USER_ABORT;
            break;
        }

        l = tcs->result_len;

        if ( data_size + l > *bufsz )
        {
            /* not enough memory */
            ret = ERROR_BUFFER_FULL;
            break;
        }

        *bufsz -= data_size;

        rb->strcpy(*buf, tcs->result);

        *bufsz -= l;
        *buf = l + (char *)*buf;

        writefn(tcs, name_idx, l);

        name_idx += l;
    }
    rb->tagcache_search_finish(tcs);
    return ret;
}

#define STR_STEP_INDEXING_UNTAGGED "1/5 Find " UNTAGGED
#define STR_STEP_ASSIGNING_ALBUMS "2/5 Find Albums"
#define STR_STEP_ASSIGNING_ALBUM_YEAR "3/5 Check Album Year"
#define STR_STEP_REMOVING_DUPLICATES "4/5 Remove Duplicates"
#define STR_STEP_PREPARING_ARTWORK "5/5 Prepare Artwork"

/*adds <untagged> albums/artist to existing album index */
static int create_album_untagged(struct tagcache_search *tcs,
                                 void **buf, size_t *bufsz)
{
    static char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    int ret = SUCCESS;
    int album_count = pf_idx.album_ct; /* store existing count */
    int total_count = pf_idx.album_ct + pf_idx.artist_ct * 2;
    long seek;
    int last, final, retry;
    int i, j;
    draw_splashscreen(*buf, *bufsz);
    draw_progressbar(0, total_count, STR_STEP_INDEXING_UNTAGGED);

    /* search tagcache for all <untagged> albums & save the albumartist seek pos */
    if (rb->tagcache_search(tcs, tag_albumartist))
    {
        rb->tagcache_search_add_filter(tcs, tag_album, pf_idx.album_untagged_seek);

        while (rb->tagcache_get_next(tcs, tcs_buf, tcs_bufsz))
        {
            if (progress_cancel(pf_idx.album_ct, total_count, STR_STEP_INDEXING_UNTAGGED))
            {
                rb->tagcache_search_finish(tcs);
                return ERROR_USER_ABORT;
            }

            if (tcs->result_seek ==
                pf_idx.album_index[-(pf_idx.album_ct - 1)].artist_seek)
                continue;

            if (sizeof(struct album_data) > *bufsz)
            {
                /* not enough memory */
                ret = ERROR_BUFFER_FULL;
                break;
            }

            *bufsz -= sizeof(struct album_data);
            write_album_index(-pf_idx.album_ct, pf_idx.album_untagged_idx,
                               pf_idx.album_untagged_seek, -1, tcs->result_seek);

            pf_idx.album_ct++;
        }
        rb->tagcache_search_finish(tcs);

        if (ret == SUCCESS) {
            draw_splashscreen(*buf, *bufsz);
            draw_progressbar(0, pf_idx.album_ct, STR_STEP_INDEXING_UNTAGGED);

            last = 0;
            final = pf_idx.artist_ct;
            retry = 0;

            /* map the artist_seek position to the artist name index */
            for (j = album_count; j < pf_idx.album_ct; j++)
            {
                if (progress_cancel(j, pf_idx.album_ct, STR_STEP_INDEXING_UNTAGGED))
                    return ERROR_USER_ABORT;

                seek = pf_idx.album_index[-j].artist_seek;

    retry_artist_lookup:
                retry++;
                for (i = last; i < final; i++)
                {
                    if (seek == pf_idx.artist_index[i].seek)
                    {
                        int idx = pf_idx.artist_index[i].name_idx;
                        pf_idx.album_index[-j].artist_idx = idx;
                        last = i; /* last match, start here next loop */
                        final = pf_idx.artist_ct;
                        retry = 0;
                        break;
                    }
                }
                if (retry > 0 && retry < 2)
                {
                    /* no match start back at beginning */
                    final = last;
                    last = 0;
                    goto retry_artist_lookup;
                }
            }
        }
    }

    return ret;
}

/* Create an index of all artists from the database */
static int build_artist_index(struct tagcache_search *tcs,
                                 void **buf, size_t *bufsz)
{
    int i, res = SUCCESS;
    struct artist_data* tmp_artist;

    /* artist index starts at end of buf it will be rearranged when finalized */
    pf_idx.artist_index = ((struct artist_data *)(*bufsz + (char *) *buf)) - 1;
    pf_idx.artist_ct = 0;
    pf_idx.artist_len = 0;
    /* artist names starts at beginning of buf */
    pf_idx.artist_names = *buf;

    rb->tagcache_search(tcs, tag_albumartist);
    res = get_tcs_search_res(ePFS_ARTIST, tcs, &(*buf), bufsz);
    rb->tagcache_search_finish(tcs);
    if (res < SUCCESS)
        return res;

    /* finalize the artist index */
    ALIGN_BUFFER(*buf, *bufsz, alignof(struct artist_data));
    tmp_artist = (struct artist_data*)*buf;
    for (i = pf_idx.artist_ct - 1; i >= 0; i--)
        tmp_artist[i] = pf_idx.artist_index[-i];

    pf_idx.artist_index = tmp_artist;
    /* move buf ptr to end of artist_index */
    *buf = pf_idx.artist_index + pf_idx.artist_ct;

    if (res == SUCCESS)
    {
        if (pf_idx.artist_ct > 0)
            res = pf_idx.artist_ct;
        else
            res = ERROR_NO_ALBUMS;
    }

    return res;
}

static int assign_album_year(void)
{
    char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    draw_progressbar(0, pf_idx.album_ct, STR_STEP_ASSIGNING_ALBUM_YEAR);
    for (int album_idx = 0; album_idx < pf_idx.album_ct; album_idx++)
    {
        /* Prevent idle poweroff */
        rb->reset_poweroff_timer();

        if (progress_cancel(album_idx, pf_idx.album_ct, STR_STEP_ASSIGNING_ALBUM_YEAR))
            return ERROR_USER_ABORT;

        int album_year = 0;

        if (rb->tagcache_search(&tcs, tag_year))
        {
            rb->tagcache_search_add_filter(&tcs, tag_album,
                                       pf_idx.album_index[album_idx].seek);

            if (pf_idx.album_index[album_idx].artist_idx >= 0)
                rb->tagcache_search_add_filter(&tcs, tag_albumartist,
                    pf_idx.album_index[album_idx].artist_seek);

            while (rb->tagcache_get_next(&tcs, tcs_buf, tcs_bufsz)) {
                int track_year = rb->tagcache_get_numeric(&tcs, tag_year);
                if (track_year > album_year)
                    album_year = track_year;
            }
        }
        rb->tagcache_search_finish(&tcs);

        pf_idx.album_index[album_idx].year = album_year;
    }
    return SUCCESS;
}

#ifdef HAVE_TC_RAMCACHE
/* The albums' artists and years from the database in memory, in one pass
   over the tracks, the same as the searches for each album give: an album
   has the artist of its first track, and the latest year of its tracks by
   that artist. The searches took a minute for 1000 albums of 9000 tracks. */
static struct {
    int *order;                 /* album_index by seek */
    struct artist_data *artists;/* by seek */
    unsigned char *seen;        /* the album's first track came */
    int extra, room;            /* albums added behind album_index */
} by_seek;

static int compare_album_seeks(const void *a, const void *b)
{
    long x = pf_idx.album_index[*(const int *)a].seek;
    long y = pf_idx.album_index[*(const int *)b].seek;
    return (x > y) - (x < y);
}

static int compare_artist_seeks(const void *a, const void *b)
{
    long x = ((const struct artist_data *)a)->seek;
    long y = ((const struct artist_data *)b)->seek;
    return (x > y) - (x < y);
}

static int artist_name_of(long seek)
{
    int lo = 0, hi = pf_idx.artist_ct - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (by_seek.artists[mid].seek == seek)
            return by_seek.artists[mid].name_idx;
        if (by_seek.artists[mid].seek < seek)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

/* A track of an album with the name of another artist's. The database lists
   each name once, "Greatest Hits" for all who made one, so this album is added
   behind the others: first_of_name is the place in order of an album with the
   name, past them if it has none. */
static void album_of_other_artist(int idx_id, long album, long artist, int year,
                                  int first_of_name)
{
    struct album_data *extra = pf_idx.album_index + pf_idx.album_ct;
    for (int i = 0; i < by_seek.extra; i++)
    {
        if (extra[i].seek == album && extra[i].artist_seek == artist)
        {
            if (year > extra[i].year)
                extra[i].year = year;
            return;
        }
    }
    int idx = artist_name_of(artist);
    if (by_seek.extra == by_seek.room || idx < 0
        || first_of_name >= pf_idx.album_ct)
        return;
    const struct album_data *same =
        &pf_idx.album_index[by_seek.order[first_of_name]];
    if (same->seek != album)
        return;
    struct album_data *a = &extra[by_seek.extra++];
    *a = *same;
    a->artist_idx = idx;
    a->artist_seek = artist;
    a->year = year;
    a->first_track = idx_id;
}

/* Albums of music alone, as Music's lists, when the tracks' kinds are in
   memory: a show's or a book's album, which no song of music is of, is left
   out of the index. The index's stamp says which it was made as. */
static bool music_only(void)
{
    return rb->tagcache_kind_if_known(0) >= 0;
}

static void album_of_track(int idx_id, const int32_t *tag_seek, void *data)
{
    (void)data;
    if (rb->tagcache_kind_if_known(idx_id) > 0)
        return;
    long album = tag_seek[tag_album], artist = tag_seek[tag_albumartist];
    int year = tag_seek[tag_year];

    /* the albums of this seek, more than one only for <untagged> */
    int lo = 0, hi = pf_idx.album_ct;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (pf_idx.album_index[by_seek.order[mid]].seek < album)
            lo = mid + 1;
        else
            hi = mid;
    }
    bool found = false;
    for (int k = lo; k < pf_idx.album_ct; k++)
    {
        int j = by_seek.order[k];
        struct album_data *a = &pf_idx.album_index[j];
        if (a->seek != album)
            break;
        if (!by_seek.seen[j])
        {
            by_seek.seen[j] = 1;
            int idx;
            if (a->artist_seek < 0 && (idx = artist_name_of(artist)) >= 0)
            {
                a->artist_idx = idx;
                a->artist_seek = artist;
            }
        }
        if ((a->artist_idx < 0 || a->artist_seek == artist) && year > a->year)
            a->year = year;
        /* the track a search of the album and its artist finds first */
        if (a->first_track < 0 && a->artist_seek == artist)
            a->first_track = idx_id;
        if (a->artist_seek == artist)
            found = true;
    }
    if (!found)
        album_of_other_artist(idx_id, album, artist, year, lo);
}

/* false when the database isn't in memory or there is no room */
static bool assign_albums_from_ram(void *buf, size_t buf_size)
{
    /* far more than albums share a name, 17 names of 1668 on one iPod */
    by_seek.extra = 0;
    by_seek.room = pf_idx.album_ct / 8 + 64;
    size_t room = by_seek.room * sizeof(struct album_data);
    size_t need = room + pf_idx.album_ct * (sizeof(int) + 1)
                + pf_idx.artist_ct * sizeof(struct artist_data)
                + 2 * sizeof(long);
    if (need > buf_size)
        return false;
    buf = (char *)buf + room;
    buf_size -= room;
    ALIGN_BUFFER(buf, buf_size, sizeof(long));
    by_seek.artists = buf;
    by_seek.order = (int *)(by_seek.artists + pf_idx.artist_ct);
    by_seek.seen = (unsigned char *)(by_seek.order + pf_idx.album_ct);

    rb->memcpy(by_seek.artists, pf_idx.artist_index,
               pf_idx.artist_ct * sizeof(struct artist_data));
    rb->qsort(by_seek.artists, pf_idx.artist_ct, sizeof(struct artist_data),
              compare_artist_seeks);
    for (int j = 0; j < pf_idx.album_ct; j++)
    {
        by_seek.order[j] = j;
        by_seek.seen[j] = 0;
    }
    rb->qsort(by_seek.order, pf_idx.album_ct, sizeof(int),
              compare_album_seeks);
    if (!rb->tagcache_for_each_track(album_of_track, NULL))
        return false;
    if (music_only())
    {
        /* those no song of music was found for go; the albums added for
           other artists come after them and move down */
        const struct album_data *extra = pf_idx.album_index + pf_idx.album_ct;
        int kept = 0;
        for (int j = 0; j < pf_idx.album_ct; j++)
        {
            if (by_seek.seen[j])
                pf_idx.album_index[kept++] = pf_idx.album_index[j];
        }
        rb->memmove(pf_idx.album_index + kept, extra,
                    by_seek.extra * sizeof(*extra));
        pf_idx.album_ct = kept;
    }
    pf_idx.album_ct += by_seek.extra;
    return true;
}
#endif /* HAVE_TC_RAMCACHE */

/* the albums' artists and years by a search of each album */
static int assign_albums_by_search(void *buf, size_t buf_size)
{
    static char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    int i, j, last, final, retry;

    /* Assign indices */
    draw_splashscreen(buf, buf_size);
    draw_progressbar(0, pf_idx.album_ct, STR_STEP_ASSIGNING_ALBUMS);
    for (j = 0; j < pf_idx.album_ct; j++)
    {
        /* Prevent idle poweroff */
        rb->reset_poweroff_timer();

        if (progress_cancel(j, pf_idx.album_ct, STR_STEP_ASSIGNING_ALBUMS))
            return ERROR_USER_ABORT;

        if (pf_idx.album_index[j].artist_seek >= 0) { continue; }

        rb->tagcache_search(&tcs, tag_albumartist);
        rb->tagcache_search_add_filter(&tcs, tag_album, pf_idx.album_index[j].seek);

        last = 0;
        final = pf_idx.artist_ct;
        retry = 0;
        if (rb->tagcache_get_next(&tcs, tcs_buf, tcs_bufsz))
        {

retry_artist_lookup:
            retry++;
            for (i = last; i < final; i++)
            {
                if (tcs.result_seek == pf_idx.artist_index[i].seek)
                {
                    int idx = pf_idx.artist_index[i].name_idx;
                    pf_idx.album_index[j].artist_idx = idx;
                    pf_idx.album_index[j].artist_seek = tcs.result_seek;
                    last = i; /* last match, start here next loop */
                    final = pf_idx.artist_ct;
                    retry = 0;
                    break;
                }
            }
            if (retry > 0 && retry < 2)
            {
                /* no match start back at beginning */
                final = last;
                last = 0;
                goto retry_artist_lookup;
            }
        }
        rb->tagcache_search_finish(&tcs);
    }

    draw_splashscreen(buf, buf_size);

    return assign_album_year();
}

/**
  Create an index of all artists and albums from the database.
  Also store the artists and album names so we can access them later.
 */
static int create_album_index(void)
{
    void *buf = pf_idx.buf;
    size_t buf_size = pf_idx.buf_sz;

    struct album_data* tmp_album;

    int i, j, res;

    draw_splashscreen(buf, buf_size);
    ALIGN_BUFFER(buf, buf_size, sizeof(long));

    /* Artists */
    res = build_artist_index(&tcs, &buf, &buf_size);
    if (res < SUCCESS)
        return res;

    /* Albums */
    pf_idx.album_ct = 0;
    pf_idx.album_len =0;
    pf_idx.album_untagged_idx = -1;
    pf_idx.album_untagged_seek = -1;

    /* album_index starts at end of buf it will be rearranged when finalized */
    pf_idx.album_index = ((struct album_data *)(buf_size + (char *)buf)) - 1;
    /* album_names starts at the beginning of buf */
    pf_idx.album_names = buf;

    rb->tagcache_search(&tcs, tag_album);
    res = get_tcs_search_res(ePFS_ALBUM, &tcs, &buf, &buf_size);
    rb->tagcache_search_finish(&tcs);
    if (res < SUCCESS)
        return res;

    /* Build artist list for untagged albums */
    res = create_album_untagged(&tcs, &buf, &buf_size);

    if (res < SUCCESS)
        return res;

    /* finalize the album index */
    ALIGN_BUFFER(buf, buf_size, alignof(struct album_data));
    tmp_album = (struct album_data*)buf;
    for (i = pf_idx.album_ct - 1; i >= 0; i--)
        tmp_album[i] = pf_idx.album_index[-i];

    pf_idx.album_index = tmp_album;
    /* move buf ptr to end of album_index */
    buf = pf_idx.album_index + pf_idx.album_ct;

    /* Assign indices */
#ifdef HAVE_TC_RAMCACHE
    if (assign_albums_from_ram(buf, buf_size))
    {
        /* with the albums it added */
        buf = pf_idx.album_index + pf_idx.album_ct;
        buf_size -= by_seek.extra * sizeof(struct album_data);
    }
    else
#endif
    {
        res = assign_albums_by_search(buf, buf_size);
        if (res < SUCCESS)
            return res;
    }

    /* sort list order to find duplicates */
    rb->qsort(pf_idx.album_index, pf_idx.album_ct,
              sizeof(struct album_data), compare_album_artists);

    draw_splashscreen(buf, buf_size);
    draw_progressbar(0, pf_idx.album_ct, STR_STEP_REMOVING_DUPLICATES);
    /* mark duplicate albums for deletion */
    for (i = 0; i < pf_idx.album_ct - 1; i++) /* -1 don't check last entry */
    {
        /* Prevent idle poweroff */
        rb->reset_poweroff_timer();

        if (progress_cancel(i, pf_idx.album_ct, STR_STEP_REMOVING_DUPLICATES))
            return ERROR_USER_ABORT;

        int idxi = pf_idx.album_index[i].artist_idx;
        int seeki = pf_idx.album_index[i].seek;

        for (j = i + 1; j < pf_idx.album_ct; j++)
        {
            if (idxi > 0 &&
            idxi == pf_idx.album_index[j].artist_idx &&
            seeki == pf_idx.album_index[j].seek)
            {
                pf_idx.album_index[j].artist_idx = -1;
            }
            else
            {
                i = j - 1;
                break;
            }
        }
    }

    /* now fix the album list order */
    rb->qsort(pf_idx.album_index, pf_idx.album_ct,
              sizeof(struct album_data), compare_album_artists);

    /* remove any extra untagged albums
     * extra space is orphaned till restart
     * the database sorts that name first, so its offset is 0, or -1 without:
     * this is the one entry of all albums without a name */
    pf_idx.album_index += pf_idx.album_untagged_idx + 1;
    pf_idx.album_ct -= pf_idx.album_untagged_idx + 1;

    pf_idx.buf = buf;
    pf_idx.buf_sz = buf_size;
    pf_idx.artist_index = 0;

    take_album_dates();
    rb->qsort(pf_idx.album_index, pf_idx.album_ct,
                          sizeof(struct album_data), compare_albums);

    return (pf_idx.album_ct > 0) ? 0 : ERROR_NO_ALBUMS;
}

/* what the index is made of: the database as it is now */
static void index_stamp(struct pf_index_t *stamp)
{
    struct tagcache_stat *stat = rb->tagcache_get_stat();
    char file[MAX_PATH];
    stamp->commitid = rb->tagcache_get_commitid();
    /* below zero for an index of music alone: one made with every album,
       or the other way round, is made again */
    stamp->entries = music_only() ? ~stat->total_entries : stat->total_entries;
    stamp->albums_size = -1;
    rb->snprintf(file, sizeof(file), "%s/database_%d.tcd", stat->db_path,
                 tag_album);
    int fd = rb->open(file, O_RDONLY);
    if (fd >= 0)
    {
        stamp->albums_size = rb->ffilesize(fd);
        rb->close(fd);
    }
}

/*Saves the album index into a binary file to be recovered the
 next time PictureFlow is launched*/

/* whole or not at all: one cut short had a stamp of this database */
static bool write_all(int fd, const void *p, size_t n)
{
    return rb->write(fd, p, n) == (ssize_t)n;
}

static int save_album_index(void){
    int fd = rb->whole_file_open(ALBUM_INDEX);

    struct pf_index_t data;
    memcpy(&data, &pf_idx, sizeof(struct pf_index_t));

    if(fd >= 0)
    {
        rb->memcpy(&data.header, INDEX_HDR, sizeof(pf_idx.header));
        index_stamp(&data);

        bool ok = write_all(fd, &data, sizeof(struct pf_index_t))
            && write_all(fd, data.artist_names, data.artist_len)
            && write_all(fd, data.album_names, data.album_len)
            && write_all(fd, data.album_index,
                         data.album_ct * sizeof(struct album_data));

        return rb->whole_file_close(fd, ALBUM_INDEX, ok) ? 0 : -1;
    }
    return -1;
}

/* reads data from save file to buffer */
static inline int read2buf(int fildes, void *buf, size_t nbyte){
    int read;
    read = rb->read(fildes, buf, nbyte);
    if (read < 0 || (size_t)read < nbyte)
        return 0;

    return read;
}

/*Loads the album_index information stored in the hard drive*/
static int load_album_index(void){

    int i, fr = rb->open(ALBUM_INDEX, O_RDONLY);
    struct pf_index_t data;

    void *bufstart = pf_idx.buf;
    unsigned int bufstart_sz = pf_idx.buf_sz;

    void* buf = pf_idx.buf;
    size_t buf_size = pf_idx.buf_sz;

    unsigned int name_sz, album_idx_sz;
    int album_idx, artist_idx;

    if (fr >= 0){
        const unsigned long filesize = rb->ffilesize(fr);
        if (filesize > sizeof(data))
        {
            if (rb->read(fr, &data, sizeof(data)) == sizeof(data))
            {
                /* the files of a database changed since hold the albums
                   and artists elsewhere, as after every sync; the index of
                   an older version is made again as well */
                struct pf_index_t now;
                index_stamp(&now);
                if (rb->memcmp(&(data.header), INDEX_HDR, sizeof(data.header))
                    || data.commitid != now.commitid
                    || data.entries != now.entries
                    || data.albums_size != now.albums_size)
                {
                    rb->close(fr);
                    return -1;
                }
                /* each alone first, as their sum could wrap around */
                if (data.artist_len > bufstart_sz || data.album_len > bufstart_sz)
                    goto failure;
                name_sz = data.artist_len + data.album_len;
                album_idx_sz = data.album_ct * sizeof(struct album_data);

                if (name_sz > bufstart_sz || album_idx_sz > bufstart_sz - name_sz)
                    goto failure;

                //rb->lseek(fr, sizeof(data) + 1, SEEK_SET);
                /* artist names */
                if (read2buf(fr, buf, data.artist_len) == 0)
                    goto failure;

                data.artist_names = buf;
                buf = (char *)buf + data.artist_len;
                buf_size -= data.artist_len;

                /* album names */
                if (read2buf(fr, buf, data.album_len) == 0)
                    goto failure;

                data.album_names = buf;
                buf = (char *)buf + data.album_len;
                buf_size -= data.album_len;

                /* index of album names */
                ALIGN_BUFFER(buf, buf_size, alignof(struct album_data));
                if (read2buf(fr, buf, album_idx_sz) == 0)
                    goto failure;

                data.album_index = buf;
                buf = (char *)buf + album_idx_sz;
                buf_size -= album_idx_sz;

                rb->close(fr);

                /* sanity check loaded data */
                for (i = 0; i < data.album_ct; i++)
                {
                    album_idx = data.album_index[i].name_idx;
                    artist_idx = data.album_index[i].artist_idx;
                    if (album_idx < 0 || artist_idx < 0 ||
                        album_idx >= (int) data.album_len ||
                        artist_idx >= (int) data.artist_len)
                    {
                        goto failure;
                    }
                }

                memcpy(&pf_idx, &data, sizeof(struct pf_index_t));
                pf_idx.buf = buf;
                pf_idx.buf_sz = buf_size;

                take_album_dates();
                rb->qsort(pf_idx.album_index, pf_idx.album_ct,
                          sizeof(struct album_data), compare_albums);

                return 0;
            }
        }
    }

failure:
    rb->splash(HZ/2, "Failed to load index");
    if (fr >= 0)
        rb->close(fr);

    pf_idx.buf = bufstart;
    pf_idx.buf_sz = bufstart_sz;
    pf_idx.artist_ct = 0;
    pf_idx.album_ct = 0;
    return -1;

}

/**
 Return a pointer to the album name of the given slide_index
 */
static char* get_album_name(const int slide_index)
{
    char *name = pf_idx.album_names + pf_idx.album_index[slide_index].name_idx;
    return name;
}

/**
 Return a pointer to the album name of the given slide_index
 */
static char* get_album_name_idx(const int slide_index, int *idx)
{
    *idx = pf_idx.album_index[slide_index].name_idx;
    char *name = pf_idx.album_names + pf_idx.album_index[slide_index].name_idx;
    return name;
}

/**
 Return a pointer to the album artist of the given slide_index
 */
static char* get_album_artist(const int slide_index)
{
    if (slide_index < pf_idx.album_ct && slide_index >= 0){
        int idx = pf_idx.album_index[slide_index].artist_idx;
        if (idx >= 0 && idx < (int) pf_idx.artist_len) {
            char *name = pf_idx.artist_names + idx;
            return name;
        }
    }
    return "?";
}

/* An album's year, of its date or as counted here; and its artist with it,
   "LINKIN PARK · 2000" */
static int album_shown_year(const int slide_index)
{
    if (slide_index < 0 || slide_index >= pf_idx.album_ct)
        return 0;
    return album_date_year(pf_idx.album_index[slide_index].year);
}

static const char *get_album_artist_year(const int slide_index, char *buf,
                                         size_t size)
{
    const char *artist = get_album_artist(slide_index);
    const int year = album_shown_year(slide_index);
    if (year <= 0)
        return artist;
    rb->snprintf(buf, size, "%s · %d", artist, year);
    return buf;
}


/**
 Return a pointer to the track name of the active album
 create_track_index has to be called first.
 */
static char* get_track_name(const int track_index)
{
    if (track_index >= 0 && track_index < pf_tracks.count )
        return pf_tracks.names + pf_tracks.index[track_index].name_idx;
    return 0;
}
#if PF_PLAYBACK_CAPABLE
static char* get_track_filename(const int track_index)
{
    if ( track_index < pf_tracks.count )
        return pf_tracks.names + pf_tracks.index[track_index].filename_idx;
    return 0;
}
#endif



static int id3_get_index(struct mp3entry *id3)
{
    char* current_artist = UNTAGGED;
    char* current_album  = UNTAGGED;

    if(id3)
    {
        /* we could be looking for the artist in either field */
        if(id3->albumartist && id3->albumartist[0])
            current_artist = id3->albumartist;
        else if(id3->artist)
            current_artist = id3->artist;

        if (id3->album && rb->strlen(id3->album) > 0)
            current_album = id3->album;

        //rb->splashf(1000, "%s, %s", current_album, current_artist);

        int i;
        int album_idx, artist_idx;

        for (i = 0; i < pf_idx.album_ct; i++ )
        {
            album_idx = pf_idx.album_index[i].name_idx;
            artist_idx = pf_idx.album_index[i].artist_idx;

            if(!rb->strcmp(pf_idx.album_names + album_idx, current_album) &&
                !rb->strcasecmp(pf_idx.artist_names + artist_idx, current_artist))
                return i;
        }

    }
    rb->splash(HZ/2, "Album Not Found!");
    return pf_cfg.last_album;
}

/**
  Compare two unsigned ints passed via pointers.
 */
static int compare_tracks (const void *a_v, const void *b_v)
{
    uint32_t a = ((struct track_data *)a_v)->sort;
    uint32_t b = ((struct track_data *)b_v)->sort;
    return (int)(a - b);
}



#define TRACKS_BORROW (16 * 1024)
/* kept from one album to the next up to this: a bigger album's room is given
   back once it's closed, else the slides it pushed out stay out */
#define TRACKS_KEEP   (64 * 1024)
#if CARD_FRAME_COUNT > 0
/* the album not made ahead: without tracks to show, or too many to keep */
static int prepare_failed = -1;
#endif
static bool track_buffer_avail(size_t needed)
{
    size_t total_out = 0;
    size_t out = 0;
    if (pf_tracks.borrowed == 0 && pf_tracks.used == 0)
    {
        /* what most albums take, rather than all that's free: it's kept from
           one album to the next, below the slides, as each borrowing or
           giving back moves every slide */
        out = MAX(needed, TRACKS_BORROW);
        pf_tracks.names = rb->buflib_buffer_out(&buf_ctx, &out);
        pf_tracks.borrowed = out;
    }

    if (needed <= pf_tracks.borrowed - pf_tracks.used)
        return true;

    while (needed > (pf_tracks.borrowed + total_out) - pf_tracks.used)
    {
        if (!free_slide_prio(0))
            break;
        out = 0;
        rb->buflib_buffer_out(&buf_ctx, &out);
        total_out += out;
    }
    pf_tracks.borrowed += total_out;

    // have to move already stored track_data structs
    if (pf_tracks.count)
    {
        struct track_data *new_tracks = (struct track_data *)(total_out + (uintptr_t)pf_tracks.index);
        unsigned int bytes = pf_tracks.count * sizeof(struct track_data);
        rb->memmove(new_tracks, pf_tracks.index, bytes);
    }

    if (needed > pf_tracks.borrowed - pf_tracks.used)
        return false;

    return true;
}


static int pf_tcs_retrieve_track_title(int string_index)
{
    char file_name[MAX_PATH];
    char *track_title = NULL;
    int str_len;

    if (rb->strcmp(UNTAGGED, tcs.result) == 0)
    {
        /* show filename instead of <untaggged> */
        if (!rb->tagcache_retrieve(&tcs, tcs.idx_id, tag_virt_basename,
                                file_name, MAX_PATH))
            return 0;
        track_title = file_name;
    }

    if (!track_title)
        track_title = tcs.result;

    /* the title alone, as on a stock iPod: the numbers only sort the list */
    int max_len = rb->strlen(track_title) + 1;
    if (!track_buffer_avail(max_len))
        return 0;

    str_len = rb->snprintf(pf_tracks.names + string_index, max_len,
        "%s", track_title);
    return str_len;
}

#if PF_PLAYBACK_CAPABLE
static int pf_tcs_retrieve_file_name(int fn_idx)
{
    if (!track_buffer_avail(MAX_PATH))
        return 0;

    rb->tagcache_retrieve(&tcs, tcs.idx_id, tag_filename,
            pf_tracks.names + fn_idx, MAX_PATH);

    return rb->strlen(pf_tracks.names + fn_idx);
}
#endif

/**
  Create the track index of the given slide_index.
 */
static void create_track_index(const int slide_index)
{
    char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    tracks_lock();
    if ( slide_index == pf_tracks.cur_idx )
        return;
    /* another album's in what's borrowed: over it */
    pf_tracks.cur_idx = -1;
    pf_tracks.used = 0;

    if (!rb->tagcache_search(&tcs, tag_title))
        goto fail;

    rb->tagcache_search_add_filter(&tcs, tag_album,
                                   pf_idx.album_index[slide_index].seek);

    if (pf_idx.album_index[slide_index].artist_idx >= 0)
        rb->tagcache_search_add_filter(&tcs, tag_albumartist,
            pf_idx.album_index[slide_index].artist_seek);

    int string_index = 0;
    pf_tracks.count = 0;

    while (rb->tagcache_get_next(&tcs, tcs_buf, tcs_bufsz))
    {
        int disc_num = rb->tagcache_get_numeric(&tcs, tag_discnumber);
        int track_num = rb->tagcache_get_numeric(&tcs, tag_tracknumber);
        disc_num = disc_num > 0 ? disc_num : 0;
        track_num = track_num > 0 ? track_num : 0;
        int fn_idx = 1 + pf_tcs_retrieve_track_title(string_index);
        if (fn_idx <= 1)
            goto fail;
        pf_tracks.used += fn_idx;

#if PF_PLAYBACK_CAPABLE
        int fn_len = 1 + pf_tcs_retrieve_file_name(string_index + fn_idx);
        if (fn_len <= 1)
            goto fail;
        pf_tracks.used += fn_len;
#endif
        if (!track_buffer_avail(sizeof(struct track_data)))
            goto fail;

        pf_tracks.used += sizeof(struct track_data);
        unsigned int arr_sz = (pf_tracks.count + 1) * sizeof(struct track_data);
        // Arrray descends from upper end of buflib-borrowed buffer.
        pf_tracks.index = (struct track_data*)(pf_tracks.names + pf_tracks.borrowed
                                                               - arr_sz );
        pf_tracks.index->sort = (disc_num << 24) + (track_num << 14);
        pf_tracks.index->sort += pf_tracks.count;
        pf_tracks.index->name_idx = string_index;
        pf_tracks.index->seek = tcs.result_seek;
        pf_tracks.index->length = rb->tagcache_get_numeric(&tcs, tag_length) / 1000;
#if PF_PLAYBACK_CAPABLE
        pf_tracks.index->filename_idx = fn_idx + string_index;
        string_index += (fn_idx + fn_len);
#else
        string_index += fn_idx;
#endif
        pf_tracks.count++;
    }

    rb->tagcache_search_finish(&tcs);

    /* now fix the track list order */
    rb->qsort(pf_tracks.index, pf_tracks.count,
              sizeof(struct track_data), compare_tracks);

    pf_tracks.cur_idx = slide_index;
    return;
fail:
    rb->tagcache_search_finish(&tcs);
    pf_tracks.count = 0;
    return;
}

/**
  Re-grow the buflib buffer by returning space borrowed
  for track list
*/
static inline void free_borrowed_tracks(void)
{
    if (pf_tracks.borrowed)
    {
        tracks_lock();
        rb->buflib_buffer_in(&buf_ctx, pf_tracks.borrowed);
    }
    pf_tracks.borrowed = 0;
    pf_tracks.used = 0;
    pf_tracks.cur_idx = -1;
#if CARD_FRAME_COUNT > 0
    card_drawn = false; /* of those tracks */
    prepare_failed = -1; /* as the albums may be sorted again */
#endif
    tracks_unlock();
}

/* The tracks done with: kept for the album to open again at once, as giving
   their room back moves every slide, unless they took more than TRACKS_KEEP;
   then it's given back and the slides it pushed out are loaded again */
static void tracks_done(void)
{
    if (pf_tracks.borrowed > TRACKS_KEEP)
    {
        free_borrowed_tracks();
        rb->queue_post(&thread_q, EV_WAKEUP, 0);
    }
    else
    {
        reset_track_list();
        tracks_unlock();
    }
}

/* Fills mp3entry with metadata retrieved from  RAM, if possible, or by reading from
 * the file directly.  Note that the tagcache only stores a subset of metadata and
 * will thus not return certain properties of the file, such as frequency, size, or
 * codec.
 */
bool retrieve_id3(struct mp3entry *id3, const char* file)
{
#if defined (HAVE_TAGCACHE) && defined(HAVE_TC_RAMCACHE) && defined(HAVE_DIRCACHE)
    if (rb->tagcache_fill_tags(id3, file))
    {
        rb->strlcpy(id3->path, file, sizeof(id3->path));
        id3_from_ramcache = true;
        return true;
    }
#endif

    id3_from_ramcache = false;
    return rb->get_metadata(id3, -1, file);
}

#if defined(HAVE_ALBUMART) && !defined(ITUNESDB_HAVE_ART)
/**
  Find a JPEG embedded in the track. On success the track's path is in buf
  and the position of the JPEG in *aa.
 */
static bool get_embedded_albumart(struct mp3entry *id3, char *buf, int buflen,
                                  struct mp3_albumart *aa)
{
    /* tags from the database don't say whether the track has album art,
       so read them from the track. get_metadata() clears id3, path
       included, so pass it a copy of the path. */
    rb->strlcpy(buf, id3->path, buflen);
    if (id3_from_ramcache)
    {
        if (!rb->get_metadata(id3, -1, buf))
        {
            /* it clears id3 on failure too; fill it from the database
               again so that image files can still be searched for */
            retrieve_id3(id3, buf);
            return false;
        }
        id3_from_ramcache = false;
    }

    /* only JPEG can be decoded */
    if (!id3->has_embedded_albumart ||
        (id3->albumart.type & AA_CLEAR_FLAGS_MASK) != AA_TYPE_JPG)
        return false;

    *aa = id3->albumart;
    return true;
}
#endif

#ifdef HAVE_ALBUMART
static int read_embedded_jpeg(const char *path, const struct mp3_albumart *aa,
                              struct bitmap *bm, int maxsize, int format,
                              const struct custom_format *cformat)
{
    int fd = rb->open(path, O_RDONLY);
    if (fd < 0)
        return fd;
    rb->lseek(fd, aa->pos, SEEK_SET);
    int ret = clip_jpeg_fd(fd, aa->type, aa->size, bm, maxsize, format,
                           cformat);
    rb->close(fd);
    return ret;
}
#endif /* HAVE_ALBUMART */

#ifdef ITUNESDB_HAVE_ART
#define TURN_W 16
static pix_t turn_tile[TURN_W * DISPLAY_HEIGHT] CACHEALIGN_ATTR;

/**
  The rows of a cover into the columns PictureFlow keeps, TURN_W columns at
  a time through a tile that stays in the cache: written straight down the
  columns, every pixel was a write to a far address. h is at most
  DISPLAY_HEIGHT.
 */
static void turn_rows(const fb_data *src, pix_t *dst, int w, int h)
{
    for (int x0 = 0; x0 < w; x0 += TURN_W)
    {
        int n = MIN(TURN_W, w - x0);
        for (int y = 0; y < h; y++)
        {
            const fb_data *row = &src[y * w + x0];
            for (int c = 0; c < n; c++)
                turn_tile[c * h + y] = row[c];
        }
        rb->memcpy(&dst[x0 * h], turn_tile, n * h * sizeof(pix_t));
    }
}

/* the cover Finder made for the album's song at path, or else the one made
   on Updating Library for the album, of songs copied outside Finder's
   folder */
static bool find_album_cover(const int slide_index, const char *path,
                             struct itunesdb_art *art)
{
    return rb->itunesdb_find_art(path, art)
           || rb->itunesdb_find_album_art(get_album_name(slide_index),
                                          get_album_artist(slide_index),
                                          path, art);
}

/**
  The cover Finder made for the album's first track, if there is one. Called
  with buf_ctx locked, as the other users of tcs are, or before the slide
  thread runs.
 */
static bool find_ipod_cover(const int slide_index, struct itunesdb_art *art)
{
    const struct album_data *album = &pf_idx.album_index[slide_index];
    char tcs_buf[TAGCACHE_BUFSZ];
    bool found = false;
    if (tcs.valid || !rb->tagcache_search(&tcs, tag_filename))
        return false;
    if (album->first_track >= 0)
    {
        /* known from the album list: a search goes through every track */
        if (rb->tagcache_retrieve(&tcs, album->first_track, tag_filename,
                                  tcs_buf, sizeof(tcs_buf)))
            found = find_album_cover(slide_index, tcs_buf, art);
    }
    else
    {
        rb->tagcache_search_add_filter(&tcs, tag_album, album->seek);
        rb->tagcache_search_add_filter(&tcs, tag_albumartist,
                                       album->artist_seek);
        if (rb->tagcache_get_next(&tcs, tcs_buf, sizeof(tcs_buf)))
        {
            found = find_album_cover(slide_index, tcs.result, art);
            /* a search of the filenames gives the track, which the next time
               is all it takes */
            pf_idx.album_index[slide_index].first_track = tcs.idx_id;
        }
    }
    rb->tagcache_search_finish(&tcs);
    return found;
}
#endif /* ITUNESDB_HAVE_ART */

#ifndef ITUNESDB_HAVE_ART
/**
  Find the album art of the track: an image file found by
  search_albumart_files() or a JPEG embedded in the track, in the order
  the album art setting prefers (files only when album art is switched
  off). The file name goes in buf, and if the art is embedded, its position
  in the file goes in aa->embedded.
 */
static bool find_track_albumart(struct mp3entry *id3, char *buf, int buflen,
                                struct albumart_t *aa)
{
#ifdef HAVE_ALBUMART
    int pref = rb->global_settings->album_art;
    aa->embedded.type = AA_TYPE_UNKNOWN;
    if (pref == AA_PREFER_EMBEDDED &&
        get_embedded_albumart(id3, buf, buflen, &aa->embedded))
        return true;
#else
    (void)aa;
#endif
    if (search_albumart_files(id3, ":", buf, buflen))
        return true;
#ifdef HAVE_ALBUMART
    if (pref == AA_PREFER_IMAGE_FILE &&
        get_embedded_albumart(id3, buf, buflen, &aa->embedded))
        return true;
#endif
    return false;
}
#endif /* !ITUNESDB_HAVE_ART */

/**
  Determine filename of the album art for the given slide_index and
  store the result in buf.
  The algorithm looks for the first track of the given album uses
  find_track_albumart to find the filename.
 */
static bool get_albumart_for_index_from_db(const int slide_index, char *buf,
                                    int buflen, struct albumart_t *aa)
{
    bool ret;
    char tcs_buf[TAGCACHE_BUFSZ];
    const long tcs_bufsz = sizeof(tcs_buf);
    if (tcs.valid || !rb->tagcache_search(&tcs, tag_filename))
        return false;

    /* find the first track of the album */
    rb->tagcache_search_add_filter(&tcs, tag_album,
                                   pf_idx.album_index[slide_index].seek);

    rb->tagcache_search_add_filter(&tcs, tag_albumartist,
                                   pf_idx.album_index[slide_index].artist_seek);

    ret = rb->tagcache_get_next(&tcs, tcs_buf, tcs_bufsz);
#ifdef ITUNESDB_HAVE_ART
    /* Only a cover made already: Finder's, or the one Updating Library
       made. None is decoded as Cover Flow shows: an album's embedded
       1000x1000 took the iPod seconds, and one without is made at the next
       Updating Library. */
    aa->ipod_art.width = 0;
    ret = ret && find_album_cover(slide_index, tcs.result, &aa->ipod_art);
    if (ret)
        rb->strlcpy(buf, tcs.result, buflen);
    else
        aa->ipod_art.width = 0;
#else
    ret = ret && retrieve_id3(&id3, tcs.result) &&
          find_track_albumart(&id3, buf, buflen, aa);
#endif

    rb->tagcache_search_finish(&tcs);
    return ret;
}

/**
  Draw the PictureFlow logo
 */
static void draw_splashscreen(unsigned char * buf_tmp, size_t buf_tmp_size)
{
#ifdef ITUNESDB_HAVE_ART
    if (preparing)
        return;
    /* as Cover Flow starts: plain, no logo */
    (void)buf_tmp;
    (void)buf_tmp_size;
    rb->lcd_set_background(pf_light ? LCD_WHITE : LCD_BLACK);
    rb->lcd_clear_display();
    rb->lcd_update();
    return;
#endif
    struct screen* display = rb->screens[SCREEN_MAIN];
#if FB_DATA_SZ > 1
    ALIGN_BUFFER(buf_tmp, buf_tmp_size, sizeof(fb_data));
#endif
    struct bitmap logo = {
#if LCD_WIDTH < 200
        .width = 100,
        .height = 18,
#else
        .width = 193,
        .height = 34,
#endif
        .data = buf_tmp
    };
    int ret = rb->read_bmp_file(SPLASH_BMP, &logo, buf_tmp_size,
                                FORMAT_NATIVE, NULL);
#if LCD_DEPTH > 1
    rb->lcd_set_background(N_BRIGHT(0));
    rb->lcd_set_foreground(N_BRIGHT(255));
#else
    rb->lcd_set_drawmode(PICTUREFLOW_DRMODE);
#endif
    rb->lcd_clear_display();

    if (ret > 0)
    {
#if LCD_DEPTH == 1  /* Mono LCDs need the logo inverted */
        rb->lcd_set_drawmode(PICTUREFLOW_DRMODE ^ DRMODE_INVERSEVID);
#endif
        display->bitmap(logo.data, (LCD_WIDTH - logo.width) / 2, 10,
            logo.width, logo.height);
#if LCD_DEPTH == 1  /* Mono LCDs need the logo inverted */
        rb->lcd_set_drawmode(PICTUREFLOW_DRMODE);
#endif
    }

    rb->lcd_update();
}


/**
  Draw a simple progress bar
 */
static void draw_progressbar(int step, int count, char *msg)
{
#ifdef ITUNESDB_HAVE_ART
    if (preparing)
    {
        rb->library_update_progress(step, count);
        return;
    }
    /* the thin bar of the boot screen's "Updating Library", without words */
    (void)msg;
    const int bar_w = 140, bar_h = 6;
    const int bx = (LCD_WIDTH - bar_w) / 2, by = (LCD_HEIGHT - bar_h) / 2;
    /* drawn when it grows: a screen for each of 1700 albums took 17 s */
    static int shown_fill = -1;
    const int fill = count > 0 ? bar_w * MIN(step, count) / count : 0;
    if (step > 0 && fill == shown_fill)
        return;
    shown_fill = fill;
    rb->lcd_set_foreground(pf_light ? LCD_RGBPACK(210, 210, 210) : LCD_RGBPACK(70, 70, 70));
    rb->lcd_fillrect(bx, by, bar_w, bar_h);
    rb->lcd_set_foreground(pf_light ? LCD_RGBPACK(70, 70, 70) : LCD_WHITE);
    rb->lcd_fillrect(bx, by, fill, bar_h);
    rb->lcd_update();
    rb->yield();
    return;
#endif
    static int txt_w, txt_h;
    const int bar_height = 22;
    const int w = LCD_WIDTH - 20;
    const int x = 10;
    static int y;
    if (msg != NULL)
    {
#if LCD_DEPTH > 1
        rb->lcd_set_background(N_BRIGHT(0));
        rb->lcd_set_foreground(N_BRIGHT(255));
#else
        rb->lcd_set_drawmode(PICTUREFLOW_DRMODE);
#endif
        rb->lcd_getstringsize(msg, &txt_w, &txt_h);

        y = (LCD_HEIGHT - txt_h)/2;

        rb->lcd_putsxy((LCD_WIDTH - txt_w)/2, y, msg);
        y += (txt_h + 5);
    }
#if LCD_DEPTH > 1
    rb->lcd_set_foreground(N_BRIGHT(100));
#endif
    rb->lcd_drawrect(x, y, w+2, bar_height);
#if LCD_DEPTH > 1
    rb->lcd_set_foreground(N_PIX(165, 231, 82));
#endif

    rb->lcd_fillrect(x+1, y+1, step * w / count, bar_height-2);
#if LCD_DEPTH > 1
    rb->lcd_set_foreground(N_BRIGHT(255));
#endif
    rb->lcd_update();
    rb->yield();
}

/* Calculate modified FNV hash of string
 * has good avalanche behaviour and uniform distribution
 * see http://home.comcast.net/~bretm/hash/ */
static unsigned int mfnv(char *str)
{
    const unsigned int p = 16777619;
    unsigned int hash = 0x811C9DC5; // 2166136261;

    if (!str)
        return 0;

    while(*str)
        hash = (hash ^ *str++) * p;
    hash += hash << 13;
    hash ^= hash >> 7;
    hash += hash << 3;
    hash ^= hash >> 17;
    hash += hash << 5;
    return hash;
}

/**
 Save the given bitmap as filename in the pfraw format
 */
static bool save_pfraw(char* filename, struct bitmap *bm)
{
    struct pfraw_header bmph;
    bmph.width = bm->width;
    bmph.height = bm->height;
    int fh = rb->whole_file_open(filename);
    if( fh < 0 ) return false;
    bool ok = write_all( fh, &bmph, sizeof( struct pfraw_header ) )
        && write_all( fh, bm->data , sizeof( pix_t ) * bm->width *  bm->height );
    return rb->whole_file_close( fh, filename, ok );
}

static bool incremental_albumart_cache(bool verbose)
{
    if (!aa_cache.buf)
        goto aa_failure;

    if (aa_cache.inspected >= pf_idx.album_ct)
        return false;

    /* Prevent idle poweroff */
    rb->reset_poweroff_timer();

    int idx, ret;
    unsigned int hash_artist, hash_album;
    unsigned int format = FORMAT_NATIVE;

    if (pf_cfg.resize)
        format |= FORMAT_RESIZE|FORMAT_KEEP_ASPECT;

    idx = aa_cache.idx;
    if (idx >= pf_idx.album_ct || idx < 0) { idx = 0; } /* Rollover */


    aa_cache.idx++;
    aa_cache.inspected++;
    if (aa_cache.idx >= pf_idx.album_ct) { aa_cache.idx = 0; } /* Rollover */


    hash_artist = mfnv(get_album_artist(idx));
    hash_album = mfnv(get_album_name(idx));

    rb->snprintf(aa_cache.pfraw_file, sizeof(aa_cache.pfraw_file),
                 CACHE_PREFIX "/%x%x.pfraw", hash_album, hash_artist);

    if(pf_cfg.update_albumart && rb->file_exists(aa_cache.pfraw_file)) {
        aa_cache.slides++;
        goto aa_success;
    }

    if (!get_albumart_for_index_from_db(idx, aa_cache.file,
                                        sizeof(aa_cache.file), &aa_cache))
        goto aa_failure; //rb->strcpy(aa_cache.file, EMPTY_SLIDE_BMP);

#ifdef ITUNESDB_HAVE_ART
    /* a cover Finder made is read as it is shown, with no file of it */
    if (aa_cache.ipod_art.width > 0)
    {
        aa_cache.slides++;
        goto aa_success;
    }
#endif

    aa_cache.input_bmp.data = aa_cache.buf;
    aa_cache.input_bmp.width = DISPLAY_WIDTH;
    aa_cache.input_bmp.height = DISPLAY_HEIGHT;

#ifdef HAVE_ALBUMART
    if (aa_cache.embedded.type != AA_TYPE_UNKNOWN)
        ret = read_embedded_jpeg(aa_cache.file, &aa_cache.embedded,
                                 &aa_cache.input_bmp, aa_cache.buf_sz,
                                 format, &format_transposed);
    else
#endif
        ret = read_image_file(aa_cache.file, &aa_cache.input_bmp,
                              aa_cache.buf_sz, format, &format_transposed);
    if (ret <= 0) {
        if (verbose) {
            rb->splashf(HZ, "Album art is bad: %s", get_album_name(idx));
        }

        goto aa_failure;
    }
    rb->remove(aa_cache.pfraw_file);

    if (!save_pfraw(aa_cache.pfraw_file, &aa_cache.input_bmp))
    {
        if (verbose) { rb->splash(HZ, "Could not write bmp"); }
        goto aa_failure;
    }
    aa_cache.slides++;

aa_failure:
    if (verbose)
    {
        if (aa_cache.inspected >= pf_idx.album_ct)
            configfile_save(CONFIG_FILE, config, CONFIG_NUM_ITEMS,
                            CONFIG_VERSION);
        return false;
    }

aa_success:
    if (aa_cache.inspected >= pf_idx.album_ct)
    {
        configfile_save(CONFIG_FILE, config, CONFIG_NUM_ITEMS,
                            CONFIG_VERSION);
        free_all_slide_prio(0);
        if (pf_state == pf_idle)
            rb->queue_post(&thread_q, EV_WAKEUP, 0);
    }

    if(verbose)/* direct interaction with user */
        return true;

    return false;
}

/**
 Precomupte the album art images and store them in CACHE_PREFIX.
 Use the "?" bitmap if image is not found.
 */
static bool create_albumart_cache(void)
{
    draw_splashscreen(pf_idx.buf, pf_idx.buf_sz);
    draw_progressbar(0, pf_idx.album_ct, STR_STEP_PREPARING_ARTWORK);
    aa_cache.inspected = 0;
    for (int i=0; i < pf_idx.album_ct; i++)
    {
        incremental_albumart_cache(true);
        draw_progressbar(aa_cache.inspected, pf_idx.album_ct, NULL);
        if (rb->button_get(false) > BUTTON_NONE)
            return true;
    }
    if ( aa_cache.slides == 0 ) {
        /* Warn the user that we couldn't find any albumart */
        rb->splash(2*HZ, ID2P(LANG_NO_ALBUMART_FOUND));
        return false;
    }
    return true;
}

static int read_pfraw(char* filename, int prio);

/* The slide of an album whose picture isn't shown, made of its picture the
   first time, as the note's; -1 without, the note's then */
static void load_unshown_slide(void)
{
    unshown_slide_hid = -1;
#ifdef ITUNESDB_HAVE_ART
    /* read_pfraw() gives the note's slide for a file missing or damaged,
       which this isn't */
    int hid = rb->file_exists(UNSHOWN_SLIDE) ? read_pfraw(UNSHOWN_SLIDE, 0)
                                             : -1;
    if ((hid < 0 || hid == empty_slide_hid) && aa_cache.buf
        && rb->file_exists(UNSHOWN_SLIDE_BMP))
    {
        const unsigned int format = FORMAT_NATIVE | FORMAT_RESIZE
                                    | FORMAT_KEEP_ASPECT;
        aa_cache.input_bmp.width = DISPLAY_WIDTH;
        aa_cache.input_bmp.height = DISPLAY_HEIGHT;
#if LCD_DEPTH > 1
        aa_cache.input_bmp.format = FORMAT_NATIVE;
#endif
        aa_cache.input_bmp.data = (char*)aa_cache.buf;
        hid = scaled_read_bmp_file(UNSHOWN_SLIDE_BMP, &aa_cache.input_bmp,
                                   aa_cache.buf_sz, format,
                                   &format_transposed) > 0
              && save_pfraw(UNSHOWN_SLIDE, &aa_cache.input_bmp)
              ? read_pfraw(UNSHOWN_SLIDE, 0) : -1;
    }
    unshown_slide_hid = hid >= 0 && hid != empty_slide_hid ? hid : -1;
#endif
}

/**
  Create the "?" slide, that is shown while loading
  or when no cover was found.
 */
static int create_empty_slide(bool force)
{
    const unsigned int format = FORMAT_NATIVE|FORMAT_RESIZE|FORMAT_KEEP_ASPECT;

    if (!aa_cache.buf)
        return false;

    if ( force || ! rb->file_exists( EMPTY_SLIDE ) )  {
        aa_cache.input_bmp.width = DISPLAY_WIDTH;
        aa_cache.input_bmp.height = DISPLAY_HEIGHT;
#if LCD_DEPTH > 1
        aa_cache.input_bmp.format = FORMAT_NATIVE;
#endif
        aa_cache.input_bmp.data = (char*)aa_cache.buf;

        scaled_read_bmp_file(EMPTY_SLIDE_BMP, &aa_cache.input_bmp,
                             aa_cache.buf_sz, format, &format_transposed);

        if (!save_pfraw(EMPTY_SLIDE, &aa_cache.input_bmp))
            return false;
        /* the question mark's, before the note, and the note's of the smaller
           covers */
        rb->remove(CACHE_PREFIX "/emptyslide.pfraw");
        rb->remove(CACHE_PREFIX "/emptynote.pfraw");
    }

    return true;
}

/**
 Thread used for loading and preparing bitmaps in the background
 */
static void thread(void)
{
    long sleep_time = 5 * HZ;
    struct queue_event ev;
    while (1) {
        rb->queue_wait_w_tmo(&thread_q, &ev, sleep_time);
        switch (ev.id) {
            case EV_EXIT:
                return;
            case EV_WAKEUP:
                /* we just woke up */
                break;
            case SYS_USB_CONNECTED:
                thread_loading = false;
                /* The queue gets the broadcasts, so USB waits for this
                   thread too: without the ack the computer never got the
                   disk. It is the computer's until it is let go. */
#ifdef ITUNESDB_HAVE_ART
                /* no file of the disk's open as it goes */
                rb->itunesdb_slides_done();
#endif
                rb->usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
                do
                    rb->queue_wait(&thread_q, &ev);
                while (ev.id != SYS_USB_DISCONNECTED && ev.id != EV_EXIT);
                if (ev.id == EV_EXIT)
                    return;
                continue;
        }

        if(ev.id != SYS_TIMEOUT) {
            thread_loading = true;
            while ( rb->queue_empty(&thread_q) ) {
                buf_ctx_lock();
                bool slide_loaded = load_new_slide();
                buf_ctx_unlock();
                if (!slide_loaded)
                    break;
                slides_arrived++;
                rb->yield();
            }
            /* or another wakeup, which sets it again at once */
            thread_loading = false;
        }
    }
}


/**
 End the thread by posting the EV_EXIT event
 */
static void end_pf_thread(void)
{
    if ( thread_is_running ) {
        rb->queue_post(&thread_q, EV_EXIT, 0);
        rb->thread_wait(thread_id);
        /* remove the thread's queue from the broadcast list */
        rb->queue_delete(&thread_q);
        thread_is_running = false;
    }
}


/**
 Create the thread an setup the event queue
 */
static bool create_pf_thread(void)
{
    /* put the thread's queue in the bcast list */
    rb->queue_init(&thread_q, true);
    if ((thread_id = rb->create_thread(
                           thread,
                           thread_stack,
                           sizeof(thread_stack),
                            0,
                           "Picture load thread"
                               /* as the UI: it loads while the UI draws, each
                                  wait for the disk a frame of the UI's, where
                                  below it a load waited for a frame to block */
                               IF_PRIO(, PRIORITY_USER_INTERFACE)
                               IF_COP(, CPU)
                                      )
        ) == 0) {
        return false;
    }
    thread_is_running = true;
    rb->queue_post(&thread_q, EV_WAKEUP, 0);
    return true;
}


static void initialize_slide_cache(void)
{
    int i= 0;
    for (i = 0; i < SLIDE_CACHE_SIZE; i++) {
        pf_sldcache.cache[i].hid = 0;
        pf_sldcache.cache[i].index = 0;
        pf_sldcache.cache[i].next = i + 1;
        pf_sldcache.cache[i].prev = i - 1;
    }
    pf_sldcache.cache[0].prev = i - 1;
    pf_sldcache.cache[i - 1].next = 0;

    pf_sldcache.free = 0;
    pf_sldcache.used = -1;
    pf_sldcache.left_idx = -1;
    pf_sldcache.right_idx = -1;
    pf_sldcache.center_idx = -1;
}


/*
 * The following functions implement the linked-list-in-array used to manage
 * the LRU cache of slides, and the list of free cache slots.
 */

#define _SEEK_RIGHT_WHILE(start, cond) \
({ \
    int ind_, next_ = (start); \
    int i_ = 0; \
    do { \
        ind_ = next_; \
        next_ = pf_sldcache.cache[ind_].next; \
        i_++; \
    } while (next_ != pf_sldcache.used && (cond) && i_ <= SLIDE_CACHE_SIZE); \
    if (i_ > SLIDE_CACHE_SIZE) \
    /* TODO: Not supposed to happen */ \
        ind_ = -1; \
    ind_; \
})

#define _SEEK_LEFT_WHILE(start, cond) \
({ \
    int ind_, next_ = (start); \
    int i_ = 0; \
    do { \
        ind_ = next_; \
        next_ = pf_sldcache.cache[ind_].prev; \
        i_++; \
    } while (ind_ != pf_sldcache.used && (cond) && i_ <= SLIDE_CACHE_SIZE); \
    if (i_ > SLIDE_CACHE_SIZE) \
    /* TODO: Not supposed to happen */ \
        ind_ = -1; \
    ind_; \
})

/**
 Pop the given item from the linked list starting at *head, returning the next
 item, or -1 if the list is now empty.
*/
static inline int lla_pop_item (int *head, int i)
{
    int prev = pf_sldcache.cache[i].prev;
    int next = pf_sldcache.cache[i].next;
    if (i == next)
    {
        *head = -1;
        return -1;
    }
    else if (i == *head)
        *head = next;
    pf_sldcache.cache[next].prev = prev;
    pf_sldcache.cache[prev].next = next;
    return next;
}


/**
 Pop the head item from the list starting at *head, returning the index of the
 item, or -1 if the list is already empty.
*/
static inline int lla_pop_head (int *head)
{
    int i = *head;
    if (i != -1)
        lla_pop_item(head, i);
    return i;
}

/**
 Insert the item at index i before the one at index p.
*/
static inline void lla_insert (int i, int p)
{
    int next = p;
    int prev = pf_sldcache.cache[next].prev;
    pf_sldcache.cache[next].prev = i;
    pf_sldcache.cache[prev].next = i;
    pf_sldcache.cache[i].next = next;
    pf_sldcache.cache[i].prev = prev;
}


/**
 Insert the item at index i at the end of the list starting at *head.
*/
static inline void lla_insert_tail (int *head, int i)
{
    if (*head == -1)
    {
        *head = i;
        pf_sldcache.cache[i].next = i;
        pf_sldcache.cache[i].prev = i;
    } else
        lla_insert(i, *head);
}

/**
 Insert the item at index i before the one at index p.
*/
static inline void lla_insert_after(int i, int p)
{
    p = pf_sldcache.cache[p].next;
    lla_insert(i, p);
}


/**
 Insert the item at index i before the one at index p in the list starting at
 *head
*/
static inline void lla_insert_before(int *head, int i, int p)
{
    lla_insert(i, p);
    if (*head == p)
        *head = i;
}


/**
 Free the used slide at index i, and its buffer, and move it to the free
 slides list.
*/
static inline void free_slide(int i)
{
    if (pf_sldcache.cache[i].hid != empty_slide_hid
        && pf_sldcache.cache[i].hid != unshown_slide_hid)
        rb->buflib_free(&buf_ctx, pf_sldcache.cache[i].hid);
    pf_sldcache.cache[i].index = -1;
    lla_pop_item(&pf_sldcache.used, i);
    lla_insert_tail(&pf_sldcache.free, i);
    if (pf_sldcache.used == -1)
    {
        pf_sldcache.right_idx = -1;
        pf_sldcache.left_idx = -1;
        pf_sldcache.center_idx = -1;
    }
}


/**
 Free one slide ranked above the given priority. If no such slide can be found,
 return false.
*/
static bool free_slide_prio(int prio)
{
    if (pf_sldcache.used == -1)
        return false;

    int i, prio_max;
    int l = pf_sldcache.used;
    int r = pf_sldcache.cache[pf_sldcache.used].prev;

    int prio_l = pf_sldcache.cache[l].index < center_index ?
           center_index - pf_sldcache.cache[l].index : 0;
    int prio_r = pf_sldcache.cache[r].index > center_index ?
           pf_sldcache.cache[r].index - center_index : 0;
    if (prio_l > prio_r)
    {
        i = l;
        prio_max = prio_l;
    } else {
        i = r;
        prio_max = prio_r;
    }
    if (prio_max > prio)
    {
        if (i == pf_sldcache.left_idx)
            pf_sldcache.left_idx = pf_sldcache.cache[i].next;
        if (i == pf_sldcache.right_idx)
            pf_sldcache.right_idx = pf_sldcache.cache[i].prev;
        free_slide(i);
        return true;
    } else
        return false;
}


/**
 Free all slides ranked above the given priority.
*/
static void free_all_slide_prio(int prio)
{
    while (free_slide_prio(prio))
    {;;}
}


/**
 Read the pfraw image given as filename and return the hid of the buffer
 */
static int read_pfraw(char* filename, int prio)
{
    struct pfraw_header bmph;
    int fh = rb->open(filename, O_RDONLY);
    if( fh < 0 ) {
        /* pf_cfg.cache_version = CACHE_UPDATE; -- don't invalidate on missing pfraw */
        return empty_slide_hid;
    }
    /* a damaged one is as none */
    if (rb->read(fh, &bmph, sizeof(struct pfraw_header))
            != sizeof(struct pfraw_header)
        || bmph.width <= 0 || bmph.height <= 0
        || bmph.width > MAX(DISPLAY_WIDTH, DISPLAY_HEIGHT)
        || bmph.height > MAX(DISPLAY_WIDTH, DISPLAY_HEIGHT))
    {
        rb->close(fh);
        return empty_slide_hid;
    }

    int size =  sizeof(struct dim) +
                sizeof( pix_t ) * bmph.width * bmph.height;

    int hid;
    do {
        hid = rb->buflib_alloc(&buf_ctx, size);
    } while (hid < 0 && free_slide_prio(prio));

    if (hid < 0) {
        rb->close( fh );
        return -1;
    }

    rb->yield(); /* allow audio to play when fast scrolling */
    struct dim *bm = rb->buflib_get_data(&buf_ctx, hid);

    bm->width = bmph.width;
    bm->height = bmph.height;
    pix_t *data = (pix_t*)(sizeof(struct dim) + (char *)bm);

    /* nor one cut short */
    const ssize_t want = sizeof( pix_t ) * bm->width * bm->height;
    const bool whole = rb->read( fh, data, want ) == want;
    rb->close( fh );
    if (!whole) {
        rb->buflib_free(&buf_ctx, hid);
        return empty_slide_hid;
    }
    return hid;
}

#ifdef ITUNESDB_HAVE_ART
/* no memory for the cover now, as opposed to there being none: an album taken
   for one without would keep the empty slide until it left the cache */
#define COVER_NO_ROOM (-2)
#define COVER_UNSHOWN (-3) /* its picture isn't shown (itunesdb.h's reasons) */

/**
  The slide of the cover Finder made: the one made after the sync, read
  as it is shown, or else made of the cover here. No file of it is kept by
  PictureFlow, where writing one for each of a thousand albums took
  minutes. The hid, -1 without such a cover, or COVER_NO_ROOM.
 */
static int read_ipod_cover(const int slide_index, const int prio)
{
    struct itunesdb_art art;
    /* the slide known from before, which is all a read needs */
    int32_t *const known = album_slides ? &album_slides[slide_index] : NULL;
    if (known && *known == SLIDE_NONE)
        return -1;
    if (known && *known == SLIDE_UNSHOWN)
        return COVER_UNSHOWN;
    if (known && *known > 0)
    {
        rb->memset(&art, 0, sizeof(art));
        art.slide = *known;
    }
    else
    {
        art.reason = 0;
        const bool found = find_ipod_cover(slide_index, &art);
        if (known && *known == SLIDE_UNREAD)
            art.slide = 0;
        else if (known)
        {
            if (!found)
                *known = art.reason ? SLIDE_UNSHOWN : SLIDE_NONE;
            else
                *known = art.slide > 0 ? art.slide : SLIDE_UNREAD;
        }
        if (!found)
            return art.reason ? COVER_UNSHOWN : -1;
    }

    int hid;
    if (art.slide && ITUNESDB_SLIDE_SIZE <= DISPLAY_WIDTH
        && ITUNESDB_SLIDE_SIZE <= DISPLAY_HEIGHT)
    {
        do {
            hid = rb->buflib_alloc(&buf_ctx, sizeof(struct dim)
                  + sizeof(pix_t) * ITUNESDB_SLIDE_SIZE * ITUNESDB_SLIDE_SIZE
                  + ITUNESDB_SLIDE_ROOM);
        } while (hid < 0 && free_slide_prio(prio));
        if (hid < 0)
            return COVER_NO_ROOM;
        /* nothing else allocates while buf_ctx is locked, so it stays */
        struct dim *slide = rb->buflib_get_data(&buf_ctx, hid);
        int w, h;
        if (rb->itunesdb_load_slide(&art, slide + 1, &w, &h) > 0)
        {
            slide->width = w;
            slide->height = h;
            return hid;
        }
        rb->buflib_free(&buf_ctx, hid);
        if (known)
            *known = SLIDE_UNREAD;
    }

    /* the slide didn't read: the cover itself then, whose frame is to find */
    if (art.path[0] == '\0' && !find_ipod_cover(slide_index, &art))
        return -1;
    /* its rows in aa_cache.buf, which the lock on buf_ctx keeps to us */
    struct bitmap bm = {
        .width = DISPLAY_WIDTH,
        .height = DISPLAY_HEIGHT,
        .data = aa_cache.buf
    };
    if (rb->itunesdb_load_art(&art, &bm, aa_cache.buf_sz) <= 0
        || bm.width > DISPLAY_WIDTH || bm.height > DISPLAY_HEIGHT)
        return -1;

    size_t pixels = sizeof(pix_t) * bm.width * bm.height;
    do {
        hid = rb->buflib_alloc(&buf_ctx, sizeof(struct dim) + pixels);
    } while (hid < 0 && free_slide_prio(prio));
    if (hid < 0)
        return COVER_NO_ROOM;

    rb->yield(); /* a frame between reading and turning, when it scrolls */
    struct dim *slide = rb->buflib_get_data(&buf_ctx, hid);
    slide->width = bm.width;
    slide->height = bm.height;
    turn_rows((const fb_data *)bm.data,
              (pix_t *)(sizeof(struct dim) + (char *)slide),
              bm.width, bm.height);
    return hid;
}
#endif /* ITUNESDB_HAVE_ART */


/**
  Load the surface for the given slide_index into the cache at cache_index.
 */
static inline bool load_and_prepare_surface(const int slide_index,
                                            const int cache_index,
                                            const int prio)
{
    int hid = -1;
#ifdef ITUNESDB_HAVE_ART
    /* the slides Updating Library made, of Finder's covers and of the songs
       outside its folder: no file of the plugin's own any more, as older
       versions kept of some albums, showing covers since changed */
    hid = read_ipod_cover(slide_index, prio);
    if (hid == COVER_NO_ROOM)
        return false;
    /* one whose picture isn't shown: the "?" slide, if it's to say so */
    if (hid == COVER_UNSHOWN)
        hid = rb->global_settings->cover_caution && unshown_slide_hid >= 0
              ? unshown_slide_hid : -1;
    /* none, the note's slide, as a missing .pfraw gave it: a slide not
       loaded is taken for no room, and the slides past it weren't loaded */
    if (hid < 0)
        hid = empty_slide_hid;
#else
    {
        char pfraw_file[MAX_PATH];
        unsigned int hash_artist = mfnv(get_album_artist(slide_index));
        unsigned int hash_album = mfnv(get_album_name(slide_index));

        rb->snprintf(pfraw_file, sizeof(pfraw_file),
                     CACHE_PREFIX "/%x%x.pfraw", hash_album, hash_artist);
        hid = read_pfraw(pfraw_file, prio);
    }
#endif
    if (hid < 0)
        return false;

    pf_sldcache.cache[cache_index].hid = hid;

    if ( cache_index < SLIDE_CACHE_SIZE ) {
        pf_sldcache.cache[cache_index].index = slide_index;
    }

    return true;
}


/**
 Load the "next" slide that we can load, freeing old slides if needed, provided
 that they are further from center_index than the current slide
*/
bool load_new_slide(void)
{
    if (wants_to_quit)
        return false;

    int i = -1;

    if (pf_sldcache.center_idx != -1)
    {
        int next, prev;
        if (pf_sldcache.cache[pf_sldcache.center_idx].index != center_index)
        {
            if (pf_sldcache.cache[pf_sldcache.center_idx].index < center_index)
            {
                pf_sldcache.center_idx = _SEEK_RIGHT_WHILE(pf_sldcache.center_idx,
                                pf_sldcache.cache[next_].index <= center_index);
                if (pf_sldcache.center_idx == -1)
                    goto fatal_fail;

                prev = pf_sldcache.center_idx;
                next = pf_sldcache.cache[pf_sldcache.center_idx].next;
            }
            else
            {
                pf_sldcache.center_idx = _SEEK_LEFT_WHILE(pf_sldcache.center_idx,
                                pf_sldcache.cache[next_].index >= center_index);
                if (pf_sldcache.center_idx == -1)
                    goto fatal_fail;

                next = pf_sldcache.center_idx;
                prev = pf_sldcache.cache[pf_sldcache.center_idx].prev;
            }
            if (pf_sldcache.cache[pf_sldcache.center_idx].index != center_index)
            {
                if (pf_sldcache.free == -1)
                    free_slide_prio(0);

                i = lla_pop_head(&pf_sldcache.free);
                if (!load_and_prepare_surface(center_index, i, 0))
                    goto fail_and_refree;

                if (pf_sldcache.cache[next].index == -1)
                {
                    if (pf_sldcache.cache[prev].index == -1)
                        goto insert_first_slide;
                    else
                        next = pf_sldcache.cache[prev].next;
                }
                lla_insert(i, next);
                if (pf_sldcache.cache[i].index < pf_sldcache.cache[pf_sldcache.used].index)
                    pf_sldcache.used = i;

                pf_sldcache.center_idx = i;
                pf_sldcache.left_idx = i;
                pf_sldcache.right_idx = i;
                return true;
            }
        }
        int left, center, right;
        left = pf_sldcache.cache[pf_sldcache.left_idx].index;
        center = pf_sldcache.cache[pf_sldcache.center_idx].index;
        right = pf_sldcache.cache[pf_sldcache.right_idx].index;

        if (left > center)
            pf_sldcache.left_idx = pf_sldcache.center_idx;
        if (right < center)
            pf_sldcache.right_idx = pf_sldcache.center_idx;

        pf_sldcache.left_idx = _SEEK_LEFT_WHILE(pf_sldcache.left_idx,
            pf_sldcache.cache[ind_].index - 1 == pf_sldcache.cache[next_].index);

        pf_sldcache.right_idx = _SEEK_RIGHT_WHILE(pf_sldcache.right_idx,
            pf_sldcache.cache[ind_].index + 1 == pf_sldcache.cache[next_].index);
        if (pf_sldcache.right_idx == -1 || pf_sldcache.left_idx == -1)
            goto fatal_fail;


        /* update indices */
        left = pf_sldcache.cache[pf_sldcache.left_idx].index;
        center = pf_sldcache.cache[pf_sldcache.center_idx].index;
        right = pf_sldcache.cache[pf_sldcache.right_idx].index;

        int prio_l = center - left + 1;
        int prio_r = right - center + 1;
        if ((prio_l < prio_r || right >= number_of_slides - 1) && left > 0)
        {
            if (pf_sldcache.free == -1 && !free_slide_prio(prio_l))
            {
                return false;
            }

            i = lla_pop_head(&pf_sldcache.free);
            if (load_and_prepare_surface(left - 1, i, prio_l))
            {
                lla_insert_before(&pf_sldcache.used, i, pf_sldcache.left_idx);
                pf_sldcache.left_idx = i;
                return true;
            }
        } else if(right < number_of_slides - 1)
        {
            if (pf_sldcache.free == -1 && !free_slide_prio(prio_r))
            {
                return false;
            }

            i = lla_pop_head(&pf_sldcache.free);
            if (load_and_prepare_surface(right + 1, i, prio_r))
            {
                lla_insert_after(i, pf_sldcache.right_idx);
                pf_sldcache.right_idx = i;
                return true;
            }
        }
    } else {
        i = lla_pop_head(&pf_sldcache.free);
        if (load_and_prepare_surface(center_index, i, 0))
        {
insert_first_slide:
            pf_sldcache.cache[i].next = i;
            pf_sldcache.cache[i].prev = i;
            pf_sldcache.center_idx = i;
            pf_sldcache.left_idx = i;
            pf_sldcache.right_idx = i;
            pf_sldcache.used = i;
            return true;
        }
    }
fail_and_refree:
    if (i != -1)
    {
        lla_insert_tail(&pf_sldcache.free, i);
    }
    return false;
fatal_fail:
    free_all_slide_prio(0);
    initialize_slide_cache();
    return false;
}


/**
  Get a slide from the buffer
 */
static inline struct dim *get_slide(const int hid)
{
    if (!hid)
        return NULL;

    struct dim *bmp;

    bmp = rb->buflib_get_data(&buf_ctx, hid);

    return bmp;
}


/**
 Return the requested surface
*/
static inline struct dim *surface(const int slide_index)
{
    if (slide_index < 0)
        return 0;
    if (slide_index >= number_of_slides)
        return 0;
    int i;
    if ((i = pf_sldcache.used ) != -1)
    {
        int j = 0;
        do {
            if (pf_sldcache.cache[i].index == slide_index) {
                if (is_initial_slide && slide_index == center_index)
                    is_initial_slide = false;
                return get_slide(pf_sldcache.cache[i].hid);
            }
            i = pf_sldcache.cache[i].next;
            j++;
        } while (i != pf_sldcache.used && j < SLIDE_CACHE_SIZE);
    }
    if (is_initial_slide && slide_index == center_index)
        return NULL;
    else
        return get_slide(empty_slide_hid);
}

/**
 adjust slides so that they are in "steady state" position
 */
static void reset_slides(void)
{
    center_slide.angle = 0;
    center_slide.cx = 0;
    center_slide.cy = 0;
    center_slide.distance = 0;
    center_slide.slide_index = center_index;

    int i;
    for (i = 0; i < pf_cfg.num_slides; i++) {
        struct slide_data *si = &left_slides[i];
        si->angle = itilt;
        si->cx = -(offsetX + pf_cfg.slide_spacing * i * PFREAL_ONE);
        si->cy = offsetY;
        si->slide_index = center_index - 1 - i;
        si->distance = 0;
    }

    for (i = 0; i < pf_cfg.num_slides; i++) {
        struct slide_data *si = &right_slides[i];
        si->angle = -itilt;
        si->cx = offsetX + pf_cfg.slide_spacing * i * PFREAL_ONE;
        si->cy = offsetY;
        si->slide_index = center_index + 1 + i;
        si->distance = 0;
    }
}


/**
 Updates look-up table and other stuff necessary for the rendering.
 Call this when the viewport size or slide dimension is changed.
 *
 * To calculate the offset that will provide the proper margin, we use the same
 * projection used to render the slides. The solution for xc, the slide center,
 * is:
 *                         xp * (zo + xs * sin(r))
 * xc = xp - xs * cos(r) + ───────────────────────
 *                                    z
 * TODO: support moving the side slides toward or away from the camera
 */
static void recalc_offsets(void)
{
    PFreal xs = PFREAL_HALF - DISPLAY_WIDTH * PFREAL_HALF;
    PFreal zo;
    PFreal xp = (DISPLAY_WIDTH * PFREAL_HALF - PFREAL_HALF +
                pf_cfg.center_margin * PFREAL_ONE) * pf_cfg.zoom / 100;
    PFreal cosr, sinr;

#ifdef ITUNESDB_HAVE_ART
    itilt = COVER_FLOW_TILT * IANGLE_MAX / 360;
#else
    itilt = 70 * IANGLE_MAX / 360;      /* approx. 70 degrees tilted */
#endif
    cosr = fcos(-itilt);
    sinr = fsin(-itilt);
    zo = CAM_DIST_R * 100 / pf_cfg.zoom - CAM_DIST_R +
        fmuln(MAXSLIDE_LEFT_R, sinr, PFREAL_SHIFT - 2, 0);
    offsetX = xp - fmul(xs, cosr) + fmuln(xp,
        zo + fmuln(xs, sinr, PFREAL_SHIFT - 2, 0), PFREAL_SHIFT - 2, 0)
        / CAM_DIST;
    offsetY = DISPLAY_WIDTH / 2 * (fsin(itilt) + PFREAL_ONE / 2);
}

/**
   Fade the given color by spreading the fb_data
   to an uint, multiply and compress the result back to a fb_data.
 */
static inline pix_t fade_color(pix_t c, unsigned a)
{
#if (LCD_PIXELFORMAT == RGB565SWAPPED)
    unsigned int result;
    c = swap16(c);
    a = (a + 2) & 0x1fc;
    result = ((c & 0xf81f) * a) & 0xf81f00;
    result |= ((c & 0x7e0) * a) & 0x7e000;
    result >>= 8;
    return swap16(result);

#elif LCD_PIXELFORMAT == RGB565
    unsigned int result;
    a = (a + 2) & 0x1fc;
    result = ((c & 0xf81f) * a) & 0xf81f00;
    result |= ((c & 0x7e0) * a) & 0x7e000;
    result >>= 8;
    return result;

#elif (LCD_PIXELFORMAT == RGB888 || LCD_PIXELFORMAT == XRGB8888) // FIXME: check this
    unsigned int pixel = FB_UNPACK_SCALAR_LCD(c);
    unsigned int result;
    a = (a + 2) & 0x1fc;
    result  = ((pixel & 0xff00ff) * a) & 0xff00ff00;
    result |= ((pixel & 0x00ff00) * a) & 0x00ff0000;
    result >>= 8;
    return FB_SCALARPACK(result);

#else
    unsigned val = c;
    return MULUQ(val, a) >> 8;
#endif
}

/**
 * Render the slides
 * Where xc is the slide's horizontal offset from center, xs is the horizontal
 * on the slide from its center, zo is the slide's depth offset from the plane
 * of the display, r is the angle at which the slide is tilted, and xp is the
 * point on the display corresponding to xs on the slide, the projection
 * formulas are:
 *
 *      z * (xc + xs * cos(r))
 * xp = ──────────────────────
 *       z + zo + xs * sin(r)
 *
 *      z * (xc - xp) - xp * zo
 * xs = ────────────────────────
 *      xp * sin(r) - z * cos(r)
 *
 * We use the xp projection once, to find the left edge of the slide on the
 * display. From there, we use the xs reverse projection to find the horizontal
 * offset from the slide center of each column on the screen, until we reach
 * the right edge of the slide, or the screen. The reverse projection can be
 * optimized by saving the numerator and denominator of the fraction, which can
 * then be incremented by (z + zo) and sin(r) respectively.
 *
 * A slide fills a column of the screen from the horizon up and down, so the
 * slides are drawn from the front back, each only beyond the rows those in
 * front of it filled there: every pixel is written once, and what no slide
 * covers is cleared afterwards. The columns are drawn into a strip small
 * enough to stay in the cache, one after another in it, and the strip is
 * copied into the framebuffer a row at a time: drawn down the columns of the
 * framebuffer itself, every pixel was a write to a far address, which made
 * the slides most of the time of a frame. Where there is a second core, it
 * draws the right half of the screen meanwhile. Nothing is in IRAM, which
 * plugins share with the codecs: music stops for a plugin that uses it.
 */

/* a slide as it is drawn in this frame */
struct slide_cols {
    const pix_t *src;
    int sw, sh;
    int alpha;
    bool flat;          /* faces the camera at its depth: xs steps by one */
    PFreal slide_left;
    PFreal zo, sinr;
    PFreal xsnum, xsden, xsnumi, xsdeni;  /* at column xi */
    PFreal xs;          /* at column xi, when flat */
    int xi, xend;       /* its columns */
    int offs;           /* its rows below the horizon, as the covers' DISPLAY_OFFS */
    bool reflect;       /* with its reflection under it */
    uint8_t reftab[REFLECT_HEIGHT];
};

#if LCD_STRIDEFORMAT == VERTICAL_STRIDE
/* the framebuffer has its columns one after another already */
#define STRIP_W LCD_WIDTH
#else
#define STRIP_W 8   /* a cache line of each row of the framebuffer */
#endif

/* only with rows of whole cache lines: a row of the iPod Photo's 220 pixels is
   half a line out on every other one, where both cores would write one line */
#if NUM_CORES > 1 && LCD_STRIDEFORMAT != VERTICAL_STRIDE && LCD_DEPTH == 16 \
    && (LCD_WIDTH * 2) % CACHEALIGN_SIZE == 0
/* The COP draws from this column on to start with (cop_from), always at the
   start of a cache line in every row of the framebuffer, so that the cores
   never share one there */
#define PF_COP_FROM ((LCD_WIDTH / 2) & ~7)
#define PF_CORES 2
#else
#define PF_CORES 1
#endif

/* where a core draws, in cache lines of its own */
struct strip {
#if LCD_STRIDEFORMAT != VERTICAL_STRIDE
    pix_t pixels[STRIP_W * LCD_HEIGHT];
#endif
    /* the rows of each column filled above and below the horizon */
    short filled_up[STRIP_W];
    short filled_down[STRIP_W];
} CACHEALIGN_ATTR;
static struct strip strips[PF_CORES];

#if defined(PF_COP_FROM) && defined(HAVE_LCD_UPDATE_FROM)
/* While the covers move, the COP shows the last frame as the next is drawn, in
   a frame of their own that takes turns with the screen's: showing one was a
   quarter of each frame's time, the CPU's alone. Still, they're drawn in the
   screen's and shown as before. */
#define PF_SHOW_ON_COP
static pix_t *frame_other;              /* the frame besides the screen's */
static struct frame_buffer_t frame_other_fb;
static struct viewport frame_other_vp;
static const pix_t *shown_later;        /* drawn, and shown as the next is */
static const pix_t *volatile cop_show SHAREDBSS_ATTR; /* the COP shows it first */
/* The COP decodes the music too, and then hasn't the time to show the frames as
   well: the CPU shows them, until the COP has time to spare again */
static bool cpu_shows;
static int cop_behind, cop_ahead; /* frames one after another it was so */
static volatile unsigned long cop_done_at SHAREDBSS_ATTR; /* its part, by the timer */
#define COP_SLOW_US         1000 /* the CPU waited this long: a strip less */
#define COP_FAST_US         200  /* and this little: a strip more */
#define COP_BEHIND_US       2000 /* a strip and still this long: behind */
#define COP_SHOWS_FROM      (LCD_WIDTH * 2 / 3) /* with a third of them: ahead */
#define COP_SWITCH_FRAMES   8

/* the frame left to show, as a menu or a splash comes over the screen: what was
   drawn under it isn't shown after it, and the screen's is drawn in again */
static void drop_frame(void)
{
    cop_show = NULL;
    shown_later = NULL;
    buffer = LCD_BUF;
}
#else
static inline void drop_frame(void) {}
#endif
#ifdef PF_COP_FROM
/* where the COP's part starts: showing a frame too, it takes fewer columns,
   as many as it can do in the time the CPU does the rest */
static volatile int cop_from SHAREDBSS_ATTR;
#endif

#if LCD_STRIDEFORMAT == VERTICAL_STRIDE
#define STRIP_COLUMN(st, x0, x) (&buffer[BUFFER_HEIGHT * (x)])
#define STRIP_STEP 1
#else
/* The strip a row after another, as the framebuffer has them: a row of it is
   copied out in four words, where gathering a row of columns was a pixel at a
   time. A column steps through it by its rows. */
#define STRIP_COLUMN(st, x0, x) (&(st)->pixels[(x) - (x0)])
#define STRIP_STEP STRIP_W
#endif

static struct slide_cols slide_cols[2 * MAX_SLIDES_COUNT + 1];
static int slide_cols_count;

/**
  Ready a picture to be drawn as a slide is with the given alpha, in front of
  those readied before: so many of its rows below the horizon, and its
  reflection under it if reflect
 */
static void add_picture(const struct slide_data *slide, const struct dim *bmp,
                        const int alpha, int offs, bool reflect)
{
    if (slide->angle > 255 || slide->angle < -255)
        return;

    struct slide_cols *s = &slide_cols[slide_cols_count];
    s->src = (const pix_t*)(sizeof(struct dim) + (const char *)bmp);
    s->sw = bmp->width;
    s->sh = bmp->height;
    s->alpha = alpha;
    s->offs = fbound(0, offs, s->sh);
    s->reflect = reflect;
    s->slide_left = -s->sw * PFREAL_HALF + PFREAL_HALF;

    if (alpha == 256) { /* opaque -> copy table */
        rb->memcpy(s->reftab, reflect_table, sizeof(s->reftab));
    } else {            /* precalculate faded table */
        int i, lalpha;
        for (i = 0; i < REFLECT_HEIGHT; i++) {
            lalpha = reflect_table[i];
            s->reftab[i] = (MULUQ(lalpha, alpha) + 129) >> 8;
        }
    }

    PFreal cosr = fcos(slide->angle);
    PFreal sinr = fsin(slide->angle);
    PFreal zo = PFREAL_ONE * slide->distance + CAM_DIST_R * 100 / pf_cfg.zoom
        - CAM_DIST_R - fmuln(MAXSLIDE_LEFT_R, fabs(sinr), PFREAL_SHIFT - 2, 0);
    PFreal xs = s->slide_left;
    PFreal xp = fdiv(CAM_DIST * (slide->cx + fmul(xs, cosr)),
        (CAM_DIST_R + zo + fmul(xs,sinr)));

    /* Since we're finding the screen position of the left edge of the slide,
     * we round up.
     */
    int xi = (fmax(DISPLAY_LEFT_R, xp) - DISPLAY_LEFT_R + PFREAL_ONE - 1)
        >> PFREAL_SHIFT;
    xp = DISPLAY_LEFT_R + xi * PFREAL_ONE;
    if (xi >= LCD_WIDTH) {
        return;
    }
    s->xsnum = CAM_DIST * (slide->cx - xp) - fmuln(xp, zo, PFREAL_SHIFT - 2, 0);
    s->xsden = fmuln(xp, sinr, PFREAL_SHIFT - 2, 0) - CAM_DIST * cosr;
    s->xs = fdiv(s->xsnum, s->xsden);
    s->xsnumi = -CAM_DIST_R - zo;
    s->xsdeni = sinr;
    s->zo = zo;
    s->sinr = sinr;
    s->flat = !zo && !slide->angle;
    s->xi = xi;

    /* Where it ends on the screen, found a column at a time as the drawing
       used to: past that the projection goes on to where it divides by
       zero, and gives nonsense there */
    PFreal xsnum = s->xsnum, xsden = s->xsden;
    xs = s->xs;
    int x;
    for (x = xi; x < LCD_WIDTH; x++) {
        int column = (xs - s->slide_left) / PFREAL_ONE;
        if (column < 0 || column >= s->sw)
            break;
        if (s->flat)
            xs += PFREAL_ONE;
        else
        {
            xsnum += s->xsnumi;
            xsden += s->xsdeni;
            xs = fdiv(xsnum, xsden);
        }
    }
    s->xend = x;
    slide_cols_count++;
}

/**
  Ready a slide to be drawn with the given alpha, in front of those readied before
 */
static void add_slide(struct slide_data *slide, const int alpha)
{
    const struct dim *bmp = surface(slide->slide_index);
    if (bmp)
        add_picture(slide, bmp, alpha, DISPLAY_OFFS, true);
}

/**
  One column of a slide, beyond the rows filled there already
 */
static void render_column(const struct slide_cols *s, PFreal xs, pix_t *col,
                          short *up, short *down)
{
    const int sh = s->sh;
    const int alpha = s->alpha;
    int column = (xs - s->slide_left) / PFREAL_ONE;
    int dy = PFREAL_ONE;
    if (!s->flat)
        dy = (CAM_DIST_R + s->zo + fmul(xs, s->sinr)) / CAM_DIST;
    if (dy <= 0)
        return; /* the loops below would never end */

    const pix_t *ptr = &s->src[column * sh];

    /* from the horizon up */
    int p = (sh-1-s->offs) * PFREAL_ONE;
    int plim = MAX(0, p - (LCD_HEIGHT/2-1) * dy);
    p -= *up * dy;
    pix_t *pixel = col + ((LCD_HEIGHT/2)-1 - *up) * STRIP_STEP;
    pix_t *from = pixel;

    /* Faded into what's behind: black, or white as the colour turned over,
       faded and turned back, where a pixel is a number to turn over. Each in
       loops of its own, as these are most of a frame's time: the dark theme's
       as they always were, without the turning */
#if LCD_DEPTH == 16 || defined(USEGSLIB)
    const bool light = pf_light;
#define FADED_LIGHT(c, a) ((pix_t)~fade_color((pix_t)~(c), (a)))
#else
    const bool light = false;
#define FADED_LIGHT(c, a) fade_color((c), (a))
#endif
    if (alpha == 256) {
        while (p >= plim) {
            *pixel = ptr[((unsigned)p) >> PFREAL_SHIFT];
            pixel -= STRIP_STEP;
            p -= dy;
        }
    } else if (light) {
        while (p >= plim) {
            *pixel = FADED_LIGHT(ptr[((unsigned)p) >> PFREAL_SHIFT], alpha);
            pixel -= STRIP_STEP;
            p -= dy;
        }
    } else {
        while (p >= plim) {
            *pixel = fade_color(ptr[((unsigned)p) >> PFREAL_SHIFT], alpha);
            pixel -= STRIP_STEP;
            p -= dy;
        }
    }
    *up += (from - pixel) / STRIP_STEP;

    /* from the horizon down: the rest of the slide, then its reflection */
    p = (sh-s->offs) * PFREAL_ONE;
    plim = MIN(sh * PFREAL_ONE, p + (LCD_HEIGHT/2) * dy);
    int plim2 = !s->reflect ? plim
              : MIN(MIN(sh + REFLECT_HEIGHT, sh * 2) * PFREAL_ONE,
                    p + (LCD_HEIGHT/2) * dy);
    p += *down * dy;
    /* a slide too wide and low to reach the horizon starts at its top */
    if (p < 0)
        p = 0;
    pixel = col + ((LCD_HEIGHT/2) + *down) * STRIP_STEP;
    from = pixel;

    if (alpha == 256) {
        while (p < plim) {
            *pixel = ptr[((unsigned)p) >> PFREAL_SHIFT];
            pixel += STRIP_STEP;
            p += dy;
        }
    } else if (light) {
        while (p < plim) {
            *pixel = FADED_LIGHT(ptr[((unsigned)p) >> PFREAL_SHIFT], alpha);
            pixel += STRIP_STEP;
            p += dy;
        }
    } else {
        while (p < plim) {
            *pixel = fade_color(ptr[((unsigned)p) >> PFREAL_SHIFT], alpha);
            pixel += STRIP_STEP;
            p += dy;
        }
    }
    if (light) {
        while (p < plim2) {
            int ty = (((unsigned)p) >> PFREAL_SHIFT) - sh;
            *pixel = FADED_LIGHT(ptr[sh - 1 - ty], s->reftab[ty]);
            pixel += STRIP_STEP;
            p += dy;
        }
    } else {
        while (p < plim2) {
            int ty = (((unsigned)p) >> PFREAL_SHIFT) - sh;
            *pixel = fade_color(ptr[sh - 1 - ty], s->reftab[ty]);
            pixel += STRIP_STEP;
            p += dy;
        }
    }
#undef FADED_LIGHT
    *down += (pixel - from) / STRIP_STEP;
}

/**
  The columns of a slide in the strip, which starts at x0 and ends before x1
 */
static void render_slide_columns(const struct slide_cols *s, struct strip *st,
                                 int x0, int x1)
{
    int x = MAX(s->xi, x0);
    x1 = MIN(s->xend, x1);
    if (x >= x1)
        return;

    PFreal xsnum = 0, xsden = 0, xs;
    if (s->flat)
        xs = s->xs + (x - s->xi) * PFREAL_ONE;
    else
    {
        xsnum = s->xsnum + (x - s->xi) * s->xsnumi;
        xsden = s->xsden + (x - s->xi) * s->xsdeni;
        xs = x == s->xi ? s->xs : fdiv(xsnum, xsden);
    }

    for (; x < x1; x++) {
        render_column(s, xs, STRIP_COLUMN(st, x0, x),
                      &st->filled_up[x - x0], &st->filled_down[x - x0]);

        if (s->flat)
            xs += PFREAL_ONE;
        else
        {
            xsnum += s->xsnumi;
            xsden += s->xsdeni;
            xs = fdiv(xsnum, xsden);
        }
    }
}

#if LCD_STRIDEFORMAT != VERTICAL_STRIDE
/**
  The strip into the framebuffer at column x0, a row at a time
 */
static void strip_to_buffer(const struct strip *st, int x0, int w)
{
    for (int y = 0; y < LCD_HEIGHT; y++) {
        const pix_t *src = &st->pixels[y * STRIP_W];
        pix_t *dst = &buffer[y * BUFFER_WIDTH + x0];
#if LCD_DEPTH == 16
        /* a whole row of it on a word of both, as in the middle of the screen:
           four words */
        if (w == STRIP_W && STRIP_W == 8 && !((uintptr_t)dst & 3)) {
            const uint32_t *s32 = (const uint32_t *)src;
            uint32_t *d32 = (uint32_t *)dst;
            d32[0] = s32[0];
            d32[1] = s32[1];
            d32[2] = s32[2];
            d32[3] = s32[3];
            continue;
        }
#endif
        for (int c = 0; c < w; c++)
            dst[c] = src[c];
    }
}
#endif


/**
  Jump to the given slide_index
 */
static inline void set_current_slide(const int slide_index)
{
    roll_speed = 0;
    int old_center_index = center_index;
    step = 0;
    center_index = fbound(0, slide_index, number_of_slides - 1);
    if (old_center_index != center_index)
    {
        rb->queue_remove_from_head(&thread_q, EV_WAKEUP);
        rb->queue_post(&thread_q, EV_WAKEUP, 0);
    }
    target = center_index;
    slide_frame = center_index << 16;
    reset_slides();
}


static void skip_animation_to_idle_state(void);
static void return_to_idle_state(void)
{
    free_borrowed_tracks(); /* made as an album opens, too */
    if (pf_state == pf_show_tracks ||
        pf_state == pf_cover_in ||
        pf_state == pf_cover_out)
        skip_animation_to_idle_state();
    else if (pf_state == pf_scrolling)
        set_current_slide(target);

    pf_state = pf_idle;
}


static void set_initial_slide(const char* selected_file)
{
    if (selected_file)
        set_current_slide(retrieve_id3(&id3, selected_file) ?
                            id3_get_index(&id3) :
                            pf_cfg.last_album);
    else
        set_current_slide(rb->audio_status() ?
                            id3_get_index(rb->audio_current_track()) :
                            pf_cfg.last_album);

}

static void reselect(unsigned int hash_album, unsigned int hash_artist)
{
    int i, album_idx, artist_idx;
    for (i = 0; i < pf_idx.album_ct; i++ )
    {
        album_idx = pf_idx.album_index[i].name_idx;
        artist_idx = pf_idx.album_index[i].artist_idx;

        if(hash_album == mfnv(pf_idx.album_names + album_idx) &&
           hash_artist == mfnv(pf_idx.artist_names + artist_idx))
        {
            set_current_slide(i);
            pf_cfg.last_album = i;
            return;
        }
    }
    set_initial_slide(NULL);
}

static bool sort_albums(int new_sorting, bool from_settings)
{
    unsigned int hash_album, hash_artist;
    static const char* sort_options[] = {
        ID2P(LANG_ARTIST_PLUS_NAME),
        ID2P(LANG_ARTIST_PLUS_YEAR),
        ID2P(LANG_ID3_YEAR),
        ID2P(LANG_NAME)
    };

    drop_frame();
#ifndef ITUNESDB_HAVE_ART
    /* Only change sorting once artwork has been inspected */
    if (aa_cache.inspected < pf_idx.album_ct)
    {
#ifdef USEGSLIB
        if (!from_settings)
            grey_show(false);
#endif
        rb->splash(HZ*2, rb->str(LANG_WAIT_FOR_CACHE));
#ifdef USEGSLIB
        if (!from_settings)
            grey_show(true);
#endif
        return false;
    }
#endif

    return_to_idle_state();

    pf_cfg.sort_albums_by = new_sorting;
    if (!from_settings)
    {
#ifdef USEGSLIB
        grey_show(false);
#if LCD_DEPTH > 1
        rb->lcd_set_background(N_BRIGHT(0));
        rb->lcd_set_foreground(N_BRIGHT(255));
#endif
        rb->lcd_clear_display();
        rb->lcd_update();
#endif
        rb->splash(HZ, sort_options[pf_cfg.sort_albums_by]);
    }

    hash_album = mfnv(get_album_name(center_index));
    hash_artist = mfnv(get_album_artist(center_index));

    end_pf_thread(); /* stop loading of covers  */

    rb->qsort(pf_idx.album_index, pf_idx.album_ct,
                  sizeof(struct album_data), compare_albums);
#ifdef ITUNESDB_HAVE_ART
    /* the albums elsewhere now: their slides looked for again */
    if (album_slides)
        rb->memset(album_slides, 0, pf_idx.album_ct * sizeof(int32_t));
#endif

    /* Empty cache and restart cover loading thread */
    rb->buflib_init(&buf_ctx, (void *)pf_idx.buf, pf_idx.buf_sz);
    /* -1 as it's read, which is what a failure to read it gives then; made
       again if it doesn't read, as at the start: without it a slide with no
       cover was loaded again and again */
    empty_slide_hid = -1;
    empty_slide_hid = read_pfraw(EMPTY_SLIDE, 0);
    if (empty_slide_hid < 0 && create_empty_slide(true))
        empty_slide_hid = read_pfraw(EMPTY_SLIDE, 0);
    load_unshown_slide();
    initialize_slide_cache();
    is_initial_slide = true;
    create_pf_thread();

    reselect(hash_album, hash_artist); /* splash if not found */

#ifdef USEGSLIB
    if (!from_settings)
        grey_show(true);
#endif
    return true;
}

/**
  Start the animation for changing slides
 */
static void start_animation(void)
{
    roll_speed = 0;
    step = (target < center_slide.slide_index) ? -1 : 1;
    pf_state = pf_scrolling;
}

static void update_scroll_animation(void);

/**
  Go to the previous slide
 */
static void show_previous_slide(void)
{
    seeking = false;
    if (step == 0) {
        if (center_index > 0) {
            target = center_index - 1;
            start_animation();
        }
    } else if ( step > 0 ) {
        roll_speed = 0;
        anim_turning = true;
        target = center_index;
        step = (target <= center_slide.slide_index) ? -1 : 1;
        if (step < 0)
            update_scroll_animation();
    } else {
        int ahead = fmax(0, center_index - 2);
        /* the end of a glide: a step this way goes on past the cover it was
           to stop on, where a finger held still stops it at the next */
        if (roll_settling && roll_speed) {
            roll_settling = false;
            roll_speed = 0;
            ahead = fmin(ahead, target);
        }
        target = ahead;
    }
}


/**
  Go to the next slide
 */
static void show_next_slide(void)
{
    seeking = false;
    if (step == 0) {
        if (center_index < number_of_slides - 1) {
            target = center_index + 1;
            start_animation();
        }
    } else if ( step < 0 ) {
        roll_speed = 0;
        anim_turning = true;
        target = center_index;
        step = (target < center_slide.slide_index) ? -1 : 1;
        if (step > 0)
            update_scroll_animation();
    } else {
        int ahead = fmin(center_index + 2, number_of_slides - 1);
        /* as in show_previous_slide() */
        if (roll_settling && roll_speed) {
            roll_settling = false;
            roll_speed = 0;
            ahead = fmax(ahead, target);
        }
        target = ahead;
    }
}


/**
  The columns from "from" to before "to", a strip at a time, of the slides
  readied from the back and so drawn from the front
 */
static void render_strips(struct strip *st, int from, int to)
{
    const pix_t back = pf_back;
    for (int x0 = from; x0 < to; x0 += STRIP_W) {
        int x1 = MIN(x0 + STRIP_W, to);
        rb->memset(st->filled_up, 0, sizeof(st->filled_up));
        rb->memset(st->filled_down, 0, sizeof(st->filled_down));
        for (int i = slide_cols_count - 1; i >= 0; i--)
            render_slide_columns(&slide_cols[i], st, x0, x1);

        for (int x = x0; x < x1; x++) {
            pix_t *col = STRIP_COLUMN(st, x0, x);
            pix_t *top = col + ((LCD_HEIGHT/2) - st->filled_up[x - x0]) * STRIP_STEP;
            pix_t *bottom = col + ((LCD_HEIGHT/2) + st->filled_down[x - x0]) * STRIP_STEP;
            pix_t *const end = col + LCD_HEIGHT * STRIP_STEP;
            for (; col < top; col += STRIP_STEP)
                *col = back;
            for (; bottom < end; bottom += STRIP_STEP)
                *bottom = back;
        }
#if LCD_STRIDEFORMAT != VERTICAL_STRIDE
        strip_to_buffer(st, x0, x1 - x0);
#endif
#ifdef PF_COP_FROM
        /* the MP3 codec's synthesis runs on the COP too: it comes in
           between strips, so the music isn't held up for a half a frame */
        if (st != &strips[0])
            rb->yield();
#endif
    }
}

#ifdef PF_COP_FROM
/* The COP's half of the frame, and the word that it is done. It only reads
   what the CPU readied, and writes its strip and its columns of the
   framebuffer. */
static struct semaphore cop_go SHAREDBSS_ATTR;
static volatile bool cop_busy SHAREDBSS_ATTR;
static volatile bool cop_quit SHAREDBSS_ATTR;
static unsigned int cop_thread_id;
static bool cop_draws;          /* false if it was too late once */
static long cop_stack[CACHEALIGN_UP(DEFAULT_STACK_SIZE * 2 / sizeof(long))]
    CACHEALIGN_AT_LEAST_ATTR(4);

static void cop_thread(void)
{
    while (true) {
        rb->semaphore_wait(&cop_go, TIMEOUT_BLOCK);
        if (cop_quit)
            break;
        /* nothing of the slides kept from before, nor of the frame shown */
        rb->commit_discard_dcache();
#ifdef PF_SHOW_ON_COP
        const pix_t *show = cop_show;
        if (show)
        {
            rb->lcd_update_from(show);
            cop_show = NULL;
        }
#endif
        render_strips(&strips[1], cop_from, LCD_WIDTH);
        rb->commit_dcache();
#ifdef PF_SHOW_ON_COP
        cop_done_at = PF_USEC_TIMER;
#endif
        cop_busy = false;
    }
}

static void create_cop_thread(void)
{
    cop_quit = false;
    cop_busy = false;
    cop_from = PF_COP_FROM;
#ifdef PF_SHOW_ON_COP
    cpu_shows = false;
    cop_behind = cop_ahead = 0;
#endif
    rb->semaphore_init(&cop_go, 1, 0);
    cop_thread_id = rb->create_thread(cop_thread, cop_stack, sizeof(cop_stack),
                                      0, "PictureFlow COP"
                                      IF_PRIO(, PRIORITY_USER_INTERFACE)
                                      IF_COP(, COP));
    cop_draws = cop_thread_id != 0;
}

static void end_cop_thread(void)
{
    if (!cop_thread_id)
        return;
    cop_quit = true;
    rb->semaphore_release(&cop_go);
    rb->thread_wait(cop_thread_id);
    cop_thread_id = 0;
    cop_draws = false;
}
#endif /* PF_COP_FROM */

/**
  Render the slides. Updates only the offscreen buffer.
*/
static void render_all_slides(void)
{
    mylcd_set_background(pf_back);
    slide_cols_count = 0;

    int nleft = pf_cfg.num_slides;
    int nright = pf_cfg.num_slides;

    int alpha;
    int index;
    int incoming_alpha = 0; /* of the one coming to the middle */
    /* The cover coming to the middle goes in front of the one leaving it as
       soon as it starts to, as on a stock iPod. Passing by on the way to
       another, it does once it's the nearer of the two to the middle: the one
       in the middle is in front again with each cover reached, rather than
       always under the next. */
    const int towards = step > 0 ? (slide_frame & 0xffff)
                                 : 65536 - (slide_frame & 0xffff);
    const bool passing = roll_speed || abs(target - center_index) > 1;
    const bool incoming_front = !passing || towards > 32768;
    if (step == 0) {
        /* no animation, boring plain rendering */
        for (index = nleft - 2; index >= 0; index--) {
            alpha = (index < nleft - 2) ? 256 : 128;
            alpha -= extra_fade;
            if (alpha > 0 )
                add_slide(&left_slides[index], alpha);
        }
        for (index = nright - 2; index >= 0; index--) {
            alpha = (index < nright - 2) ? 256 : 128;
            alpha -= extra_fade;
            if (alpha > 0 )
                add_slide(&right_slides[index], alpha);
        }
    } else {
        /* the first and last slide must fade in/fade out */

        /* if step<0 and nleft==1, left_slides[0] is fading in  */
        alpha = ((step > 0) ? 0 : ((nleft == 1) ? 256 : 128)) - fade / 2;
        for (index = nleft - 1; index >= 0; index--) {
            if (alpha > 0) {
                if (step < 0 && index == 0 && incoming_front)
                    incoming_alpha = alpha;
                else
                    add_slide(&left_slides[index], alpha);
            }
            alpha += 128;
            if (alpha > 256) alpha = 256;
        }
        /* if step>0 and nright==1, right_slides[0] is fading in  */
        alpha = ((step > 0) ? ((nright == 1) ? 128 : 0) : -128) + fade / 2;
        for (index = nright - 1; index >= 0; index--) {
            if (alpha > 0) {
                if (step > 0 && index == 0 && incoming_front)
                    incoming_alpha = alpha;
                else
                    add_slide(&right_slides[index], alpha);
            }
            alpha += 128;
            if (alpha > 256) alpha = 256;
        }
    }
    alpha = 256;
    if (step != 0 && pf_cfg.num_slides <= 2) /* fading out center slide */
        alpha = (step > 0) ? 256 - fade / 2 : 128 + fade / 2;
    /* turned to its edge, behind the tracks: not a line of it above and below */
#if CARD_FRAME_COUNT > 0
    if (card_frame && card_drawn)
        add_picture(&card_slide, card, 256, card_offs(), false);
    else
#endif
    if (pf_state != pf_show_tracks)
        add_slide(&center_slide, alpha);
    /* the cover coming to the middle over the one leaving it, as soon as it
       starts to, as on a stock iPod: added last, it's drawn in front */
    if (incoming_alpha > 0)
        add_slide(step > 0 ? &right_slides[0] : &left_slides[0], incoming_alpha);

#ifdef PF_COP_FROM
    if (cop_draws) {
        /* the slides out of this core's cache to the COP, and no line of
           its half of the framebuffer kept here */
        rb->commit_discard_dcache();
        const int from = cop_from;
        cop_busy = true;
        rb->semaphore_release(&cop_go);
#ifdef PF_SHOW_ON_COP
        const unsigned long cpu_start = PF_USEC_TIMER;
#endif
        render_strips(&strips[0], 0, from);
        /* Waited for without yielding, as the slide thread would move the
           slides meanwhile; a COP that doesn't come back is not used again */
        long timeout = *rb->current_tick + HZ;
#ifdef PF_SHOW_ON_COP
        const unsigned long wait_start = PF_USEC_TIMER;
        const unsigned long cpu_us = wait_start - cpu_start;
#endif
        while (cop_busy) {
            /* not every cycle: the word is in DRAM, which the COP draws
               from too */
            for (volatile int i = 0; i < 16; i++);
            if (TIME_AFTER(*rb->current_tick, timeout)) {
                /* Too late: not used again, but let finish, as it may be
                   drawing or showing a frame still, the slides held where
                   they are meanwhile */
                cop_draws = false;
                buf_ctx_lock();
                while (cop_busy)
                    rb->sleep(1);
                buf_ctx_unlock();
                break;
            }
        }
#ifdef PF_SHOW_ON_COP
        /* Strips moved to whichever core waited, of the 40 a little more than
           a millisecond's work each: as many as make up half the time it
           waited, a strip at least, so that they're even again within a few
           frames of the music's pausing or going on. A strip a frame took a
           second and more. Waiting under 0.2 ms, the COP was through first, by
           as long before the CPU as it was. */
        const unsigned long waited = PF_USEC_TIMER - wait_start;
        const long strip_us = MAX(cpu_us / MAX(from / STRIP_W, 1), 200ul);
        if (waited > COP_SLOW_US && from < LCD_WIDTH - STRIP_W)
        {
            const int strips_more = MAX((long)waited / (4 * strip_us), 1);
            cop_from = MIN(from + strips_more * STRIP_W, LCD_WIDTH - STRIP_W);
        }
        else if (waited < COP_FAST_US && from > STRIP_W)
        {
            const long ahead = (long)(wait_start - cop_done_at);
            const int strips_less = MAX(ahead / (4 * strip_us), 1);
            cop_from = MAX(from - strips_less * STRIP_W, STRIP_W);
        }
        /* Showing the frames, the COP behind with only a strip of its own,
           COP_SWITCH_FRAMES on end: the CPU shows them. Then with a third of
           them and more, as without music: the COP again. */
        if (!cpu_shows)
        {
            cop_behind = from >= LCD_WIDTH - STRIP_W && waited > COP_BEHIND_US
                         ? cop_behind + 1 : 0;
            if (cop_behind >= COP_SWITCH_FRAMES)
            {
                cpu_shows = true;
                cop_behind = 0;
            }
        }
        else
        {
            cop_ahead = from <= COP_SHOWS_FROM ? cop_ahead + 1 : 0;
            if (cop_ahead >= COP_SWITCH_FRAMES)
            {
                cpu_shows = false;
                cop_ahead = 0;
            }
        }
#endif
        return;
    }
#endif
    render_strips(&strips[0], 0, LCD_WIDTH);
}


/* The slide this many from the middle one in the direction they go, or the last */
static int slide_ahead(int slides)
{
    return fbound(0, center_index + slides * step, number_of_slides - 1);
}

/* the cover they'd get to at this speed, and the rate that gets them there */
static void roll_settle_start(void)
{
    const int want = roll_speed * ROLL_SETTLE_FRAMES;
    int slides = 1;
    int way;

    while (true) {
        target = slide_ahead(slides);
        way = abs((target << 16) - slide_frame);
        if (way >= want - 32768 || target == slide_ahead(slides + 1))
            break;
        slides++;
    }
    roll_settle_time = fmax(way * 16 / roll_speed, 16);
    roll_settling = true;
}

/**
  Updates the animation effect. Call this periodically from a timer.
*/
static void update_scroll_animation(void)
{
    if (step == 0)
        return;

    int speed = 16384;
    int i;

    /* deaccelerate when approaching the target */
    if (true) {
        const int max = 2 * 65536;

        int fi = slide_frame;
        fi -= (target << 16);
        if (fi < 0)
            fi = -fi;
        fi = fmin(fi, max);

        int ia = IANGLE_MAX * (fi - max / 2) / (max * 2);
        speed = 512 + 16384 * (PFREAL_ONE + fsin(ia)) / PFREAL_ONE;
    }

    /* a sixth of the way left a frame, from a slide a frame up, doubling:
       hundreds of albums in a second or so, slowing onto the last ones */
    if (seeking) {
        const int left = abs((target << 16) - slide_frame);
        seek_cap = fmin(seek_cap * 2, 64 * 65536);
        speed = fmax(fmin(left / 6, seek_cap), 16384);
    }

    if (roll_speed) {
        const int left = step > 0 ? number_of_slides - 1 - center_index
                                  : center_index;
        if (roll_free)
            roll_aim -= roll_aim * anim_part / (ROLL_FRICTION * ANIM_ONE);
        else
            roll_carry -= roll_carry * anim_part / (ROLL_FRICTION * ANIM_ONE);
        /* another flick takes them out of it */
        if (roll_settling && roll_aim >= ROLL_SPEED_MIN)
            roll_settling = false;
        if (!roll_settling) {
            roll_speed += (fmin(roll_aim, left * ROLL_LAST_SLIDES + ROLL_LAST_SPEED)
                           - roll_speed) * fmin(anim_part, ROLL_FOLLOW * ANIM_ONE)
                          / (ROLL_FOLLOW * ANIM_ONE);
            roll_speed = fmax(roll_speed, ROLL_SPEED_MIN);
            /* the last slide being nearer than that, they stop on it as usual */
            if (target == center_index + 2 * step)
                speed = roll_speed;
            /* run down, or the end of the albums near enough to glide to it */
            if (roll_free
                && ((roll_aim < ROLL_SPEED_MIN && roll_speed < ROLL_SPEED_MIN + 2048)
                    || left * 65536 <= roll_speed * ROLL_SETTLE_FRAMES))
                roll_settle_start();
        }
        if (roll_settling) {
            /* a finger on the wheel: the next cover will do, at the same pace */
            if (!roll_free && abs(target - center_index) > 1)
                target = slide_ahead(1);
            int left_of_way = abs((target << 16) - slide_frame);
            speed = fmax(left_of_way * 16 / roll_settle_time, ROLL_SETTLE_MIN);
            roll_speed = speed;
        }
    }
    anim_speed = speed;

    /* as far as the time of the frame takes them, and not past the cover they
       stop on, which a stall would: going back, they are on it below its place */
    /* never still, as a cover just reached going back would be the one left */
    int move = fmax(speed * anim_part / ANIM_ONE, 1);
    const int way = step > 0 ? (target << 16) - slide_frame
                             : slide_frame - ((target << 16) - 1);
    if (way >= 0 && move > way)
        move = way;
    slide_frame += move * step;

    int index = slide_frame >> 16;
    int pos = slide_frame & 0xffff;
    int neg = 65536 - pos;
    int tick = (step < 0) ? neg : pos;
    PFreal ftick = (tick * PFREAL_ONE) >> 16;

    /* the leftmost and rightmost slide must fade away */
    fade = pos / 256;

    if (step < 0)
        index++;
    if (center_index != index) {
        center_index = index;
        /* one waiting is enough: past album after album, quicker than the
           slide thread read their covers, as from songs with no cover made,
           they filled its queue, and the plugin stopped on it */
        rb->queue_remove_from_head(&thread_q, EV_WAKEUP);
        rb->queue_post(&thread_q, EV_WAKEUP, 0);
        center_slide.slide_index = center_index;
        for (i = 0; i < pf_cfg.num_slides; i++)
            left_slides[i].slide_index = center_index - 1 - i;
        for (i = 0; i < pf_cfg.num_slides; i++)
            right_slides[i].slide_index = center_index + 1 + i;
        /* gliding with the wheel let go, a click an album as it passes, as its
           steps click while it's turned (watch_wheel()); one a frame at most */
        if (roll_speed && roll_free)
            rb->keyclick_click(true, step > 0 ? BUTTON_SCROLL_FWD
                                              : BUTTON_SCROLL_BACK);
        const bool rolled = roll_speed != 0 || seeking;
        if (roll_speed && !roll_settling) {
            /* a finger stopped them: from the speed that the stop on the next
               slide starts with */
            if (roll_aim < ROLL_SPEED_MIN && roll_speed < ROLL_SPEED_MIN + 2048) {
                roll_speed = 0;
                target = slide_ahead(1);
            } else
                target = slide_ahead(2);
        }
        /* rolling, nothing is dropped so they go evenly; and turning back this is
           the same place counted from the other slide, not a slide reached */
        if (!anim_turning && !rolled)
            slide_frame = index << 16;
    }
    anim_turning = false;

    center_slide.angle = (step * tick * itilt) >> 16;
    center_slide.cx = -step * fmul(offsetX, ftick);
    center_slide.cy = fmul(offsetY, ftick);

    if (center_index == target) {
        seeking = false;
        reset_slides();
        pf_state = pf_idle;
        slide_frame = center_index << 16;
        step = 0;
        fade = 256;
        roll_speed = 0;
        roll_settling = false;
        return;
    }

    for (i = 0; i < pf_cfg.num_slides; i++) {
        struct slide_data *si = &left_slides[i];
        si->angle = itilt;
        si->cx =
            -(offsetX + pf_cfg.slide_spacing * i * PFREAL_ONE + step
                        * pf_cfg.slide_spacing * ftick);
        si->cy = offsetY;
    }

    for (i = 0; i < pf_cfg.num_slides; i++) {
        struct slide_data *si = &right_slides[i];
        si->angle = -itilt;
        si->cx =
            offsetX + pf_cfg.slide_spacing * i * PFREAL_ONE - step
                      * pf_cfg.slide_spacing * ftick;
        si->cy = offsetY;
    }

    if (step > 0) {
        PFreal ftick = (neg * PFREAL_ONE) >> 16;
        right_slides[0].angle = -(neg * itilt) >> 16;
        right_slides[0].cx = fmul(offsetX, ftick);
        right_slides[0].cy = fmul(offsetY, ftick);
    } else {
        PFreal ftick = (pos * PFREAL_ONE) >> 16;
        left_slides[0].angle = (pos * itilt) >> 16;
        left_slides[0].cx = -fmul(offsetX, ftick);
        left_slides[0].cy = fmul(offsetY, ftick);
    }

    /* must change direction ? */
    if (target < index)
        if (step > 0)
            step = -1;
    if (target > index)
        if (step < 0)
            step = 1;
}


#if defined(ITUNESDB_HAVE_ART) && defined(HAVE_LCD_COLOR)
/* The tracks on the back of the cover, as a stock iPod shows them: a panel in
   the middle with the album and its artist at its top, in the theme's colours of
   a selection (or, by the theme, on the panel's ground over a hairline), its
   tracks and their lengths under them, the covers aside behind */
#ifdef UI_220X176
/* the iPod photo's: its fonts of 14 and 15 px in a head of 34 and rows of
   18, six of them, and as wide as that is tall */
#define PANEL_HEAD   34
#define PANEL_ROW    18
#define PANEL_ROWS   6
#define PANEL_W      PANEL_H
#else
#define PANEL_HEAD   40
#define PANEL_ROW    20
#define PANEL_ROWS   8
#define PANEL_W      (LCD_WIDTH * 5 / 8)
#endif
#define PANEL_X      ((LCD_WIDTH - PANEL_W) / 2)
#define PANEL_H      (PANEL_HEAD + PANEL_ROW * PANEL_ROWS)
#define PANEL_Y      ((LCD_HEIGHT - PANEL_H) / 2)
/* the card turns into the panel as a cover turns, and only a square one can
   (CARD_SQUARE): without, it would open with no card */
#if PANEL_W != PANEL_H
#error "Cover Flow's track panel must be square"
#endif
#define PANEL_PAD    6    /* the text's from the panel's sides */
#define PANEL_DOT_ROOM 6  /* and more for a theme's cursor's dot, as in its lists */
#define PANEL_GAP    8    /* between a track's name and its length */

static int panel_font = FONT_UI;        /* of the tracks */
static int panel_small_font = FONT_UI;  /* of the artist */
/* where the panel is drawn: on the screen, or into the card that turns into it */
static int panel_ox = PANEL_X, panel_oy = PANEL_Y;
static struct frame_buffer_t *panel_fb; /* NULL for the screen's */

/* The theme's, which are loaded already: asked for with as many glyphs as it
   has them, so they are shared as they are and take no room of their own */
static void load_panel_fonts(void)
{
    const int glyphs = rb->global_settings->glyphs_to_cache;
    /* an album header's two, which the theme has loaded: no memory is taken
       for them here, under the music */
    int id = rb->font_load_ex(rb->ui_font_file(UI_FONT_HEADER_ARTIST), 0, glyphs);
    panel_font = id >= 0 ? id : FONT_UI;
    id = rb->font_load_ex(rb->ui_font_file(UI_FONT_HEADER_COUNTS), 0, glyphs);
    panel_small_font = id >= 0 ? id : FONT_UI;
}

static void unload_panel_fonts(void)
{
    if (panel_font != FONT_UI)
        rb->font_unload(panel_font);
    if (panel_small_font != FONT_UI)
        rb->font_unload(panel_small_font);
    panel_font = panel_small_font = FONT_UI;
}

/* the theme's selection, its start colour at the top, its end at the bottom */
static void panel_gradient(int x, int y, int w, int h)
{
    const unsigned a = rb->global_settings->lss_color;
    const unsigned b = rb->global_settings->lse_color;
    rb->lcd_set_drawmode(DRMODE_SOLID);
    for (int r = 0; r < h; r++)
    {
        const int t = h > 1 ? r * 256 / (h - 1) : 0;
        rb->lcd_set_foreground(LCD_RGBPACK(
            RGB_UNPACK_RED(a) + (RGB_UNPACK_RED(b) - RGB_UNPACK_RED(a)) * t / 256,
            RGB_UNPACK_GREEN(a) + (RGB_UNPACK_GREEN(b) - RGB_UNPACK_GREEN(a)) * t / 256,
            RGB_UNPACK_BLUE(a) + (RGB_UNPACK_BLUE(b) - RGB_UNPACK_BLUE(a)) * t / 256));
        rb->lcd_hline(x, x + w - 1, y + r);
    }
}

/* str as it fits in width in the current font, cut with an ellipsis */
static const char *fit_text(const char *str, int width, char *buf, size_t size)
{
    static const char ellipsis[] = "\xe2\x80\xa6";
    int w = rb->lcd_getstringsize(str, NULL, NULL);
    if (w <= width)
        return str;
    const int ew = rb->lcd_getstringsize(ellipsis, NULL, NULL);
    size_t fits = 0, len = 0;
    while (str[len] && len + sizeof(ellipsis) < size)
    {
        /* to the end of this character */
        size_t next = len + 1;
        while ((str[next] & 0xc0) == 0x80)
            next++;
        if (next + sizeof(ellipsis) > size)
            break;
        rb->memcpy(buf, str, next);
        buf[next] = '\0';
        if (rb->lcd_getstringsize(buf, NULL, NULL) + ew > width)
            break;
        fits = len = next;
    }
    rb->memcpy(buf + fits, ellipsis, sizeof(ellipsis));
    return buf;
}

/* str fitted, in the form of its ideographs whole, as the selected track's
   name scrolls whole */
static void put_fitted(int x, int y, const char *str, int width, char *buf, size_t size)
{
    rb->font_set_text_form(rb->font_text_form((const unsigned char *)str, -1));
    rb->lcd_putsxy(x, y, fit_text(str, width, buf, size));
    rb->font_set_text_form(-1);
}

static void draw_track_panel(bool loading)
{
    const bool light = pf_light;
    /* a theme's cursor, its signal colour's dot left of the chosen track:
       the tracks' names after its room (PANEL_DOT_ROOM), the panel in the
       theme's own ground, ink and hairline, the lengths in its grey */
    const bool dot = rb->ui_metric(UI_METRIC_SELECTION_DOT) > 0;
    const unsigned ink = dot ? rb->ui_colour(UI_COLOUR_TEXT)
                       : light ? LCD_BLACK : LCD_RGBPACK(242, 242, 246);
    const unsigned paper = dot ? rb->ui_colour(UI_COLOUR_GROUND)
                         : light ? LCD_WHITE : LCD_RGBPACK(28, 28, 30);
    const unsigned edge = dot ? rb->ui_colour(UI_COLOUR_GROUP_RULE)
                        : light ? LCD_RGBPACK(190, 190, 190) : LCD_RGBPACK(64, 64, 68);
    const unsigned sel_ink = rb->global_settings->lst_color;
    const int px = panel_ox, py = panel_oy;
    const int text_x = px + PANEL_PAD + (dot ? PANEL_DOT_ROOM : 0);
    const int text_w = PANEL_W - 2 * PANEL_PAD - (dot ? PANEL_DOT_ROOM : 0);
    char buf[MAX_PATH];

    rb->lcd_set_drawmode(DRMODE_SOLID);
    rb->lcd_set_foreground(edge);
    rb->lcd_drawrect(px - 1, py - 1, PANEL_W + 2, PANEL_H + 2);
    /* by the theme the head isn't in the selection's colours, under which
       the first track would not read as selected: the album in the panel's
       ink on its paper, the artist in the theme's small grey, a hairline
       under them */
    const bool plain_head = rb->ui_metric(UI_METRIC_PANEL_HEAD_PLAIN);
    rb->lcd_set_foreground(paper);
    if (plain_head)
    {
        rb->lcd_fillrect(px, py, PANEL_W, PANEL_H);
        rb->lcd_set_foreground(edge);
        rb->lcd_hline(px, px + PANEL_W - 1, py + PANEL_HEAD - 1);
    }
    else
    {
        rb->lcd_fillrect(px, py + PANEL_HEAD, PANEL_W, PANEL_H - PANEL_HEAD);
        panel_gradient(px, py, PANEL_W, PANEL_HEAD);
    }

    /* the album and its artist */
    rb->lcd_set_drawmode(DRMODE_FG);
    rb->lcd_set_foreground(plain_head ? ink : sel_ink);
    int album_idx = 0;
    const int idx = center_slide.slide_index;
    const char *album = get_album_name_idx(idx, &album_idx);
    /* the album at the top, the artist's line at the bottom, 3 from it */
    rb->lcd_setfont(FONT_UI);
    if (album_idx != (int) pf_idx.album_untagged_idx)
        put_fitted(text_x, py + 1, album, text_w, buf, sizeof(buf));
    rb->lcd_setfont(panel_small_font);
    if (plain_head)
        rb->lcd_set_foreground(rb->ui_colour(UI_COLOUR_VALUE));
    /* the artist cut short, if it must be, rather than the year after it */
    const int artist_y = py + PANEL_HEAD - 3 - rb->font_get(panel_small_font)->height;
    char year[16] = "";
    if (album_shown_year(idx) > 0)
        rb->snprintf(year, sizeof(year), " · %d", album_shown_year(idx));
    const int year_w = rb->lcd_getstringsize(year, NULL, NULL);
    const char *artist = get_album_artist(idx);
    rb->font_set_text_form(rb->font_text_form((const unsigned char *)artist, -1));
    artist = fit_text(artist, text_w - year_w, buf, sizeof(buf));
    const int artist_w = rb->lcd_getstringsize(artist, NULL, NULL);
    rb->lcd_putsxy(text_x, artist_y, artist);
    rb->font_set_text_form(-1);
    if (*year)
        rb->lcd_putsxy(text_x + artist_w, artist_y, year);

    rb->lcd_setfont(panel_font);
    const int fh = rb->font_get(panel_font)->height;
    int y = py + PANEL_HEAD;
    if (loading)
    {
        rb->lcd_set_foreground(ink);
        const char *wait = rb->str(LANG_WAIT);
        rb->lcd_putsxy(px + (PANEL_W - rb->lcd_getstringsize(wait, NULL, NULL)) / 2,
                       y + (PANEL_H - PANEL_HEAD - fh) / 2, wait);
        rb->lcd_setfont(FONT_UI);
        return;
    }

    for (int i = pf_tracks.list_start;
         i < pf_tracks.list_start + pf_tracks.list_visible; i++, y += PANEL_ROW)
    {
        const bool selected = i == pf_tracks.sel && !show_tracks_while_browsing;
        if (selected)
            panel_gradient(px, y, PANEL_W, PANEL_ROW);
        if (selected && dot)
        {
            /* a disc of 6 px, its corners cut, its middle on a pixel's
               corner at the panel's pad */
            const int cx = px + PANEL_PAD, cy = y + PANEL_ROW / 2;
            rb->lcd_set_drawmode(DRMODE_SOLID);
            rb->lcd_set_foreground(rb->ui_colour(UI_COLOUR_SIGNAL));
            rb->lcd_fillrect(cx - 2, cy - 3, 4, 6);
            rb->lcd_fillrect(cx - 3, cy - 2, 6, 4);
        }
        rb->lcd_set_drawmode(DRMODE_FG);
        rb->lcd_set_foreground(selected ? sel_ink : ink);
        const int ty = y + (PANEL_ROW - fh) / 2;

        /* its length at the right, the name in what's left */
        int secs = pf_tracks.index[i].length, room = text_w;
        if (secs > 0)
        {
            char length[16];
            if (secs >= 3600)
                rb->snprintf(length, sizeof(length), "%d:%02d:%02d",
                             secs / 3600, secs / 60 % 60, secs % 60);
            else
                rb->snprintf(length, sizeof(length), "%d:%02d", secs / 60, secs % 60);
            const int lw = rb->lcd_getstringsize(length, NULL, NULL);
            if (dot)
                rb->lcd_set_foreground(rb->ui_colour(UI_COLOUR_VALUE));
            rb->lcd_putsxy(text_x + text_w - lw, ty, length);
            rb->lcd_set_foreground(selected ? sel_ink : ink);
            room = text_w - lw - PANEL_GAP;
        }

        char *name = get_track_name(i);
        if (!selected)
        {
            put_fitted(text_x, ty, name, room, buf, sizeof(buf));
            continue;
        }
        /* the chosen one scrolls when it doesn't fit, within its place */
        if (pf_tracks.sel != pf_tracks.last_sel)
        {
            set_scroll_line_in(name, PF_SCROLL_TRACK, room, false);
            pf_tracks.last_sel = pf_tracks.sel;
        }
        struct viewport vp;
        rb->memset(&vp, 0, sizeof(vp));
        rb->viewport_set_fullscreen(&vp, SCREEN_MAIN);
        rb->viewport_set_buffer(&vp, panel_fb, SCREEN_MAIN);
        vp.x = text_x;
        vp.y = ty;
        vp.width = room;
        vp.height = fh;
        vp.font = panel_font;
        vp.drawmode = DRMODE_FG;
        struct viewport *const last = rb->lcd_set_viewport(&vp);
        rb->lcd_set_foreground(sel_ink);
        rb->lcd_putsxy(get_scroll_line_offset(PF_SCROLL_TRACK), 0, name);
        rb->lcd_set_viewport(last);
        rb->lcd_setfont(panel_font);
    }
    rb->lcd_setfont(FONT_UI);
}

/* the panel of the album in the middle into the card, false without its tracks */
static bool make_card(void)
{
    card_drawn = false;
    if (!card || !CARD_SQUARE)
        return false;
    if (center_slide.slide_index != pf_tracks.cur_idx)
    {
#ifdef HAVE_TC_RAMCACHE
        /* the database from the disk shows its wait first */
        if (!rb->tagcache_is_in_ram())
            return false;
#endif
        create_track_index(center_slide.slide_index);
        if (pf_tracks.count == 0)
        {
            free_borrowed_tracks();
            return false;
        }
        reset_track_list();
    }

    pix_t *pixels = (pix_t *)(card + 1);
    static struct frame_buffer_t card_fb;
    card_fb.fb_ptr = pixels;
    card_fb.get_address_fn = NULL;
    card_fb.stride = CARD_W;
    card_fb.elems = CARD_W * CARD_H;
    struct viewport vp;
    rb->memset(&vp, 0, sizeof(vp));
    rb->viewport_set_fullscreen(&vp, SCREEN_MAIN);
    rb->viewport_set_buffer(&vp, &card_fb, SCREEN_MAIN);
    vp.x = vp.y = 0;
    vp.width = CARD_W;
    vp.height = CARD_H;
    struct viewport *const last = rb->lcd_set_viewport(&vp);
    panel_ox = panel_oy = 1;
    panel_fb = &card_fb;
    draw_track_panel(false);
    panel_fb = NULL;
    panel_ox = PANEL_X;
    panel_oy = PANEL_Y;
    rb->lcd_set_viewport(last);

    /* its rows into columns, as the slides have them */
    for (int y = 0; y < CARD_H; y++)
        for (int x = y + 1; x < CARD_W; x++)
        {
            const pix_t t = pixels[y * CARD_W + x];
            pixels[y * CARD_W + x] = pixels[x * CARD_H + y];
            pixels[x * CARD_H + y] = t;
        }
    card->width = CARD_W;
    card->height = CARD_H;
    card_drawn = true;
    card_album = center_slide.slide_index;
    return true;
}
#endif /* ITUNESDB_HAVE_ART && HAVE_LCD_COLOR */

/* The session's lines for the health log, in memory until USB or the sleep:
   how long Cover Flow held 80 MHz, how albums opened, and when the screen went
   dark and the iPod to sleep in it */
static void cf_marks_note(void)
{
    if (!cf_marks.opened_at)
        return;
    const long open = *rb->current_tick - cf_marks.opened_at;
    rb->health_note("cover flow: %ld s, 80 MHz its own %ld.%ld s; opened %d,"
                    " %d made ahead, %d at Select (most %lu ms, all %lu ms);"
                    " made ahead %d steps (most %lu ms); longest frame opening"
                    " %lu ms at key %d, closing %lu ms",
                    open / HZ, cf_marks.boosted / HZ,
                    cf_marks.boosted % HZ * 10 / HZ, cf_marks.opened,
                    cf_marks.ready, cf_marks.opened - cf_marks.ready,
                    cf_marks.made_most / 1000, cf_marks.made_all / 1000,
                    cf_marks.prepared, cf_marks.prepared_most / 1000,
                    cf_marks.frame_in / 1000, cf_marks.frame_in_key,
                    cf_marks.frame_out / 1000);
    if (cf_marks.darks || cf_marks.sleeps)
        rb->health_note("cover flow: screen dark %d times, the last %ld s after"
                        " a use; asleep %d times, the last %ld s after",
                        cf_marks.darks, cf_marks.dark_after / HZ,
                        cf_marks.sleeps, cf_marks.sleep_after / HZ);
    cf_marks.opened_at = 0;
}

/**
  Cleanup the plugin
*/
static void cleanup(void)
{
    wants_to_quit = true;
    drop_frame();
#ifdef HAVE_WHEEL_POSITION
    /* not otherwise, as the timer may be the backlight's, fading */
    if (wheel_timer_set)
        rb->timer_unregister();
    wheel_timer_set = false;
    rb->wheel_send_events(true);
    wheel_events = true;
#endif
    if (buf_ctx_locked)
    {
        buf_ctx_locked = false;
        buf_ctx_unlock();
    }

    pf_boost(false);
    cf_marks_note();
    end_pf_thread();
#ifdef PF_COP_FROM
    end_cop_thread();
#endif
#ifdef ITUNESDB_HAVE_ART
    rb->itunesdb_slides_done();
#endif
    rb->pcmbuf_hold_for_ui(0); /* the music goes on decoding at once */

    /* Turn on backlight timeout (revert to settings) */
    backlight_use_settings();
#if defined(ITUNESDB_HAVE_ART) && defined(HAVE_LCD_COLOR)
    unload_panel_fonts();
#endif

#ifdef USEGSLIB
    grey_release();
#endif
}

static void skip_animation_to_show_tracks(void);
static void adjust_album_display_for_setting(int old_val, int new_val)
{
    if (old_val == new_val)
        return;

    reset_track_list();
    recalc_offsets();
    reset_slides();

    if (pf_state == pf_show_tracks)
        skip_animation_to_show_tracks();
}

static int display_settings_menu(void)
{
    int selection = 0;
    int old_val;

    MENUITEM_STRINGLIST(display_menu, ID2P(LANG_DISPLAY), NULL,
                        ID2P(LANG_BACKLIGHT),
                        ID2P(LANG_DISPLAY_FPS),
                        ID2P(LANG_SPACING),
                        ID2P(LANG_CENTRE_MARGIN),
                        ID2P(LANG_NUMBER_OF_SLIDES),
                        ID2P(LANG_ZOOM),
                        ID2P(LANG_RESIZE_COVERS));

    static const struct opt_items backlight_options[] = {
        { STR(LANG_ALWAYS_ON) },
        { STR(LANG_NORMAL) }};

    do {
        selection=rb->do_menu(&display_menu, &selection, NULL, false);
        switch(selection) {
            case 0:
                rb->set_option(rb->str(LANG_BACKLIGHT),
                        &pf_cfg.backlight_mode, RB_INT, backlight_options, 2, NULL);
                if(pf_cfg.backlight_mode == 0)
                    backlight_ignore_timeout();
                else
                    backlight_use_settings();
                break;
            case 1:
                old_val = pf_cfg.show_fps;
                rb->set_bool(rb->str(LANG_DISPLAY_FPS), &pf_cfg.show_fps);
                if (old_val != pf_cfg.show_fps)
                    reset_track_list();
                break;
            case 2:
                old_val = pf_cfg.slide_spacing;
                rb->set_int(rb->str(LANG_SPACING), "", 1,
                            &pf_cfg.slide_spacing,
                            NULL, 1, 0, 100, NULL );
                adjust_album_display_for_setting(old_val, pf_cfg.slide_spacing);
                break;
            case 3:
                old_val = pf_cfg.center_margin;
                rb->set_int(rb->str(LANG_CENTRE_MARGIN), "", 1,
                            &pf_cfg.center_margin,
                            NULL, 1, 0, 80, NULL );
                adjust_album_display_for_setting(old_val, pf_cfg.center_margin);
                break;
            case 4:
                old_val = pf_cfg.num_slides;
                rb->set_int(rb->str(LANG_NUMBER_OF_SLIDES), "", 1,
                        &pf_cfg.num_slides, NULL, 1, 1, MAX_SLIDES_COUNT, NULL );
                adjust_album_display_for_setting(old_val, pf_cfg.num_slides);
                break;
            case 5:
                old_val = pf_cfg.zoom;
                rb->set_int(rb->str(LANG_ZOOM), "", 1, &pf_cfg.zoom,
                            NULL, 1, 10, 300, NULL );
                adjust_album_display_for_setting(old_val, pf_cfg.zoom);
                break;
            case 6:
                old_val = pf_cfg.resize;
                rb->set_bool(rb->str(LANG_RESIZE_COVERS), &pf_cfg.resize);
                if (old_val == pf_cfg.resize) /* changed? */
                    break;
                else if (!rb->yesno_pop_confirm(ID2P(LANG_RESIZE_COVERS)))
                {
                    pf_cfg.resize = old_val;
                    break;
                }

                pf_cfg.update_albumart = false;
                pf_cfg.cache_version = CACHE_REBUILD;
                rb->remove(EMPTY_SLIDE);
                rb->remove(UNSHOWN_SLIDE);
                configfile_save(CONFIG_FILE, config,
                                CONFIG_NUM_ITEMS, CONFIG_VERSION);
                return -3; /* re-init */
            case MENU_ATTACHED_USB:
                return PLUGIN_USB_CONNECTED;
        }
    } while (selection >= 0);

    return 0;
}

/**
  Shows the settings menu
 */
static int settings_menu(void)
{
    int selection = 0;
    int old_val, result;

    MENUITEM_STRINGLIST(settings_menu, "PictureFlow Settings", NULL,
                        ID2P(LANG_SHOW_ALBUM_TITLE),
                        ID2P(LANG_SHOW_YEAR_IN_ALBUM_TITLE),
                        ID2P(LANG_YEAR_SORT_ORDER),
                        ID2P(LANG_WPS_INTEGRATION),
                        ID2P(LANG_DISPLAY));

    static const struct opt_items album_name_options[] = {
        { STR(LANG_HIDE_INFO) },
        { STR(LANG_SHOW_AT_THE_BOTTOM_NEW) },
        { STR(LANG_SHOW_AT_THE_TOP_NEW) },
        { STR(LANG_SHOW_ALL_AT_THE_TOP) },
        { STR(LANG_SHOW_ALL_AT_THE_BOTTOM) },
    };
    static const struct opt_items year_sort_order_options[] = {
        { STR(LANG_ASCENDING) },
        { STR(LANG_DESCENDING) }
    };
    static const struct opt_items wps_options[] = {
        { STR(LANG_OFF) },
        { STR(LANG_DIRECT) },
        { STR(LANG_VIA_TRACK_LIST) }
    };

    do {
        selection=rb->do_menu(&settings_menu,&selection, NULL, false);
        switch(selection) {
            case 0:
                old_val = pf_cfg.show_album_name;
                rb->set_option(rb->str(LANG_SHOW_ALBUM_TITLE),
                      &pf_cfg.show_album_name, RB_INT, album_name_options, 5, NULL);
                adjust_album_display_for_setting(old_val, pf_cfg.show_album_name);
                break;
            case 1:
                rb->set_bool(rb->str(LANG_SHOW_YEAR_IN_ALBUM_TITLE), &pf_cfg.show_year);
                break;
            case 2:
                old_val = pf_cfg.year_sort_order;
                rb->set_option(rb->str(LANG_YEAR_SORT_ORDER),
                      &pf_cfg.year_sort_order, RB_INT, year_sort_order_options, 2, NULL);
                if (old_val != pf_cfg.year_sort_order &&
                    !sort_albums(pf_cfg.sort_albums_by, true))
                    pf_cfg.year_sort_order = old_val;
                break;
            case 3:
#ifdef ITUNESDB_HAVE_ART
            {
                /* without "off", as music doesn't play in Cover Flow */
                int wps_option = pf_cfg.auto_wps - 1;
                rb->set_option(rb->str(LANG_WPS_INTEGRATION),
                               &wps_option, RB_INT, wps_options + 1, 2, NULL);
                pf_cfg.auto_wps = wps_option + 1;
                break;
            }
#else
                rb->set_option(rb->str(LANG_WPS_INTEGRATION),
                               &pf_cfg.auto_wps, RB_INT, wps_options, 3, NULL);
                break;
#endif
            case 4:
                if ((result = display_settings_menu()))
                    return result;
                break;
            case MENU_ATTACHED_USB:
                return PLUGIN_USB_CONNECTED;
        }
    } while ( selection >= 0 );

    return 0;
}

/**
  Show the main menu
 */
enum {
    PF_SORT_ALBUMS_BY,
    PF_SHOW_TRACKS_WHILE_BROWSING,
    PF_GOTO_LAST_ALBUM,
    PF_GOTO_WPS,
#if PF_PLAYBACK_CAPABLE
    PF_MENU_PLAYBACK_CONTROL,
#endif
    PF_REBUILD_CACHE,
    PF_UPDATE_CACHE,
    PF_MENU_SETTINGS,
    PF_MENU_QUIT,
};

static int main_menu(void)
{
    int selection = 0;
    int result, curr_album, old_val;

#if LCD_DEPTH > 1
    rb->lcd_set_foreground(N_BRIGHT(255));
#endif

    MENUITEM_STRINGLIST(main_menu, "PictureFlow", NULL,
                        ID2P(LANG_SORT_ALBUMS_BY),
                        ID2P(LANG_SHOW_TRACKS_WHILE_BROWSING),
                        ID2P(LANG_GOTO_LAST_ALBUM),
                        ID2P(LANG_GOTO_WPS),
#if PF_PLAYBACK_CAPABLE
                        ID2P(LANG_PLAYBACK_CONTROL),
#endif
                        ID2P(LANG_REBUILD_CACHE),
                        ID2P(LANG_UPDATE_CACHE),
                        ID2P(LANG_SETTINGS),
                        ID2P(LANG_MENU_QUIT));

    static const struct opt_items sort_options[] = {
        { STR(LANG_ARTIST_PLUS_NAME) },
        { STR(LANG_ARTIST_PLUS_YEAR) },
        { STR(LANG_ID3_YEAR) },
        { STR(LANG_NAME) }};

    while (1)  {
        switch (rb->do_menu(&main_menu,&selection, NULL, false)) {
            case PF_SORT_ALBUMS_BY:
                old_val = pf_cfg.sort_albums_by;
                rb->set_option(rb->str(LANG_SORT_ALBUMS_BY),
                      &pf_cfg.sort_albums_by, RB_INT, sort_options, 4, NULL);
                if (old_val != pf_cfg.sort_albums_by &&
                    !sort_albums(pf_cfg.sort_albums_by, true))
                    pf_cfg.sort_albums_by = old_val;
                if (old_val == pf_cfg.sort_albums_by)
                    break;
                return 0;
            case PF_SHOW_TRACKS_WHILE_BROWSING:
                if (pf_state != pf_show_tracks)
                {
                    if (pf_state == pf_scrolling)
                        set_current_slide(target);

                    skip_animation_to_show_tracks();
                }
                show_tracks_while_browsing = true;
                return 0;
            case PF_GOTO_LAST_ALBUM:
                if (pf_state == pf_scrolling)
                    curr_album = target;
                else
                    curr_album = center_index;

                if (pf_state == pf_show_tracks)
                    free_borrowed_tracks();
                if (pf_state == pf_show_tracks ||
                    pf_state == pf_cover_in ||
                    pf_state == pf_cover_out)
                    skip_animation_to_idle_state();

                set_current_slide(pf_cfg.last_album);
                pf_cfg.last_album = curr_album;

                pf_state = pf_idle;
                return 0;
            case PF_GOTO_WPS: /* WPS */
                return -2;
#if PF_PLAYBACK_CAPABLE
            case PF_MENU_PLAYBACK_CONTROL: /* Playback Control */
                playback_control(NULL);
                break;
#endif
            case PF_REBUILD_CACHE:
                if (!rb->yesno_pop_confirm(ID2P(LANG_REBUILD_CACHE)))
                    break;
                pf_cfg.update_albumart = false;
                pf_cfg.cache_version = CACHE_REBUILD;
                rb->remove(EMPTY_SLIDE);
                rb->remove(UNSHOWN_SLIDE);
                configfile_save(CONFIG_FILE, config,
                                CONFIG_NUM_ITEMS, CONFIG_VERSION);
                return -3; /* re-init */
            case PF_UPDATE_CACHE:
                if (!rb->yesno_pop_confirm(ID2P(LANG_UPDATE_CACHE)))
                    break;
                pf_cfg.update_albumart = true;
                pf_cfg.cache_version = CACHE_REBUILD;
                rb->remove(EMPTY_SLIDE);
                rb->remove(UNSHOWN_SLIDE);
                configfile_save(CONFIG_FILE, config,
                                CONFIG_NUM_ITEMS, CONFIG_VERSION);
                return -3; /* re-init */
            case PF_MENU_SETTINGS:
                result = settings_menu();
                if (result != 0)
                    return result;
                break;
            case PF_MENU_QUIT:
                return -1;

            case MENU_ATTACHED_USB:
                return PLUGIN_USB_CONNECTED;

            default:
                return 0;
        }
    }
}

#define ZOOMIN_FRAME_COUNT  13
#define ZOOMIN_FRAME_DIST   -7
#define ZOOMIN_FRAME_ANGLE  1
#ifdef ITUNESDB_HAVE_ART
/* the covers aside stay as they are behind the tracks, as on a stock iPod */
#define ZOOMIN_FRAME_FADE   0
#else
#define ZOOMIN_FRAME_FADE   19
#endif

#define ROTATE_FRAME_COUNT  10
#define ROTATE_FRAME_ANGLE  24

#define KEYFRAME_COUNT (ZOOMIN_FRAME_COUNT + ROTATE_FRAME_COUNT)

#define ALL_KEYFRAMES       (KEYFRAME_COUNT + CARD_FRAME_COUNT)

/* The opening as it is that many key frames in, the closing the same way back */
static void set_cover_animation(int keyframe)
{
    const int zoom = fbound(0, keyframe, ZOOMIN_FRAME_COUNT);
    const int turn = fbound(0, keyframe - ZOOMIN_FRAME_COUNT, ROTATE_FRAME_COUNT);
    center_slide.distance = zoom * ZOOMIN_FRAME_DIST;
    center_slide.angle = zoom * ZOOMIN_FRAME_ANGLE + turn * ROTATE_FRAME_ANGLE;
    extra_fade = zoom * ZOOMIN_FRAME_FADE;
    card_frame = fbound(0, keyframe - KEYFRAME_COUNT, CARD_FRAME_COUNT);
#if CARD_FRAME_COUNT > 0
    if (card_frame)
    {
        /* the other side, turned on from where the cover's edge was */
        const int left = CARD_FRAME_COUNT - card_frame;
        card_slide.slide_index = -1;
        card_slide.angle = fmin(0, center_slide.angle
                                   + card_frame * ROTATE_FRAME_ANGLE - IANGLE_MAX / 2);
        card_slide.cx = card_slide.cy = 0;
        card_slide.distance = CARD_FRAME_DIST * left / CARD_FRAME_COUNT;
    }
#endif
}

/* the card's rows below the horizon, as it comes */
static int card_offs(void)
{
#if CARD_FRAME_COUNT > 0
    return CARD_FRAME_OFFS
           + (CARD_OFFS - CARD_FRAME_OFFS) * card_frame / CARD_FRAME_COUNT;
#else
    return DISPLAY_OFFS;
#endif
}

/**
   Animation step for zooming into the current cover
 */
static void update_cover_in_animation(void)
{
    cover_animation_keyframe++;
#if CARD_FRAME_COUNT > 0
    /* the card when the cover is on its edge, made as it started; without one,
       the panel then */
    if (cover_animation_keyframe == KEYFRAME_COUNT + 1
        && !(card_drawn && card_album == center_slide.slide_index) && !make_card())
        cover_animation_keyframe = ALL_KEYFRAMES + 1;
#endif
    if (cover_animation_keyframe > ALL_KEYFRAMES)
    {
        cover_animation_keyframe = 0;
        card_frame = 0;
        pf_state = pf_show_tracks;
        return;
    }
    set_cover_animation(cover_animation_keyframe);
}

/**
   Animation step for zooming out the current cover
 */
static void update_cover_out_animation(void)
{
    cover_animation_keyframe++;
    if (cover_animation_keyframe >= ALL_KEYFRAMES)
    {
        cover_animation_keyframe = 0;
        card_frame = 0;
#if CARD_FRAME_COUNT > 0
        card_drawn = false;
#endif
        /* its tracks kept, for it to open again from the first at once */
        tracks_done();
        set_cover_animation(0);
        pf_state = pf_idle;
        return;
    }
    set_cover_animation(ALL_KEYFRAMES - cover_animation_keyframe);
}

/* From the tracks back to the covers: the card of them as they are now, which
   turns back into the cover. They're kept, as the album may open again on the
   way, as it was, or once it's closed, from its first */
static void start_cover_out(void)
{
    cover_animation_keyframe = 0;
#if CARD_FRAME_COUNT > 0
    if (pf_state != pf_show_tracks
        || pf_tracks.cur_idx != center_slide.slide_index || !make_card())
        cover_animation_keyframe = CARD_FRAME_COUNT; /* from the cover's edge */
#endif
    pf_state = pf_cover_out;
    set_cover_animation(ALL_KEYFRAMES - cover_animation_keyframe);
}

/**
   Immediately show tracks and skip any animation frames
*/
static void skip_animation_to_show_tracks(void)
{
    pf_state = pf_show_tracks;
    cover_animation_keyframe = 0;
    set_cover_animation(KEYFRAME_COUNT);
    card_frame = 0;
}

/**
   Immediately transition to idle state and skip any animation frames
*/
static void skip_animation_to_idle_state(void)
{
    tracks_done(); /* as at the end of a closing */
    pf_state = pf_idle;
    cover_animation_keyframe = 0;
    card_frame = 0;
#if CARD_FRAME_COUNT > 0
    card_drawn = false;
#endif
    extra_fade = 0;
    set_current_slide(center_index);
}

/**
  Change direction during cover in/out animation
*/
static void reverse_animation(void)
{
    pf_state = pf_state == pf_cover_out ? pf_cover_in : pf_cover_out;
    cover_animation_keyframe = ALL_KEYFRAMES - cover_animation_keyframe;
}

#if !(defined(ITUNESDB_HAVE_ART) && defined(HAVE_LCD_COLOR))
/**
   Draw a blue gradient at y with height h
 */
static inline void draw_gradient(int y, int h)
{
    int r, inc, c;
    inc = (100 << 8) / h;
    c = 0;
    pf_tracks.sel_pulse = (pf_tracks.sel_pulse+1) % 10;
    int c2 = pf_tracks.sel_pulse - 5;
    for (r=0; r<h; r++) {
#ifdef HAVE_LCD_COLOR
        mylcd_set_foreground(G_PIX(c2+80-(c >> 9), c2+100-(c >> 9),
                                           c2+250-(c >> 8)));
#else
        mylcd_set_foreground(G_BRIGHT(c2+160-(c >> 8)));
#endif
        mylcd_hline(0, LCD_WIDTH, r+y);
        if ( r > h/2 )
            c-=inc;
        else
            c+=inc;
    }
}


static void track_list_yh(int char_height)
{
    bool needs_space = pf_cfg.show_fps || aa_cache.inspected < pf_idx.album_ct;

    switch (pf_cfg.show_album_name)
    {
        case ALBUM_NAME_HIDE:
            pf_tracks.list_y = (needs_space ? char_height : 0);
            pf_tracks.list_h = LCD_HEIGHT - pf_tracks.list_y;
            break;
        case ALBUM_NAME_BOTTOM:
            pf_tracks.list_y = (needs_space ? char_height : 0);
            pf_tracks.list_h = LCD_HEIGHT - pf_tracks.list_y - (char_height * 3);
            break;
        case ALBUM_AND_ARTIST_TOP:
            pf_tracks.list_y = char_height * 3;
            pf_tracks.list_h = LCD_HEIGHT - pf_tracks.list_y -
                           (needs_space ? char_height : 0);
            break;
        case ALBUM_AND_ARTIST_BOTTOM:
            pf_tracks.list_y = (needs_space ? char_height : 0);
            pf_tracks.list_h = LCD_HEIGHT - pf_tracks.list_y - (char_height * 3);
            break;
        case ALBUM_NAME_TOP:
        default:
            pf_tracks.list_y = char_height * 3;
            pf_tracks.list_h = LCD_HEIGHT - pf_tracks.list_y -
                           (needs_space ? char_height : 0);
            break;
    }
}
#endif /* the plugin's own list of tracks */

/**
    Reset the track list after a album change
 */
void reset_track_list(void)
{
#if defined(ITUNESDB_HAVE_ART) && defined(HAVE_LCD_COLOR)
    pf_tracks.list_visible = MIN(PANEL_ROWS, pf_tracks.count);
    pf_tracks.list_start = 0;
    pf_tracks.sel = 0;
    pf_tracks.last_sel = -1;
#else
    int char_height = rb->screens[SCREEN_MAIN]->getcharheight();
    int total_height;
    track_list_yh(char_height);
    pf_tracks.list_visible =
                         fmin( pf_tracks.list_h/char_height , pf_tracks.count );

    pf_tracks.list_start = 0;
    pf_tracks.sel = 0;
    pf_tracks.last_sel = -1;

    /* let the tracklist start more centered
     * if the screen isn't filled with tracks */
    total_height = pf_tracks.count*char_height;
    if (total_height < pf_tracks.list_h)
    {
        pf_tracks.list_y += (pf_tracks.list_h - total_height) / 2;
        pf_tracks.list_h = total_height;
    }
#endif
}

static void draw_album_text(void);
static void show_track_list_loading(void)
{
#if defined(ITUNESDB_HAVE_ART) && defined(HAVE_LCD_COLOR)
    draw_track_panel(true);
    mylcd_update();
#else
    int x = (LCD_WIDTH - mylcd_getstringsize(rb->str(LANG_WAIT), NULL, NULL)) / 2;
    mylcd_set_foreground(G_BRIGHT(255));
    int char_height = rb->screens[SCREEN_MAIN]->getcharheight();
    track_list_yh(char_height);
    mylcd_putsxy(x, pf_tracks.list_y + (pf_tracks.list_h  - char_height) / 2,
                 rb->str(LANG_WAIT));
    draw_album_text();
    mylcd_update();
    mylcd_clear_display();
#endif
}

/**
  Display the list of tracks
 */
static bool show_track_list(void)
{
#if defined(ITUNESDB_HAVE_ART) && defined(HAVE_LCD_COLOR)
    render_all_slides();
#else
    mylcd_clear_display();
#endif
    if ( center_slide.slide_index != pf_tracks.cur_idx ) {
#ifdef HAVE_TC_RAMCACHE
        if (!rb->tagcache_is_in_ram())
#endif
            show_track_list_loading();
        create_track_index(center_slide.slide_index);
        if (pf_tracks.count == 0)
        {
            start_cover_out();
            return false;
        }
        reset_track_list();
    }
#if defined(ITUNESDB_HAVE_ART) && defined(HAVE_LCD_COLOR)
    draw_track_panel(false);
#else
    int titletxt_w, titletxt_x, color, titletxt_h;
    titletxt_h = rb->screens[SCREEN_MAIN]->getcharheight();

    int titletxt_y = pf_tracks.list_y;
    int track_i;
    int fade;

    track_i = pf_tracks.list_start;
    for (; track_i < pf_tracks.list_visible + pf_tracks.list_start; track_i++)
    {
        char *trackname = get_track_name(track_i);
        if (track_i == pf_tracks.sel && !show_tracks_while_browsing) {
            if (pf_tracks.sel != pf_tracks.last_sel) {
                set_scroll_line(trackname, PF_SCROLL_TRACK);
                pf_tracks.last_sel = pf_tracks.sel;
            }
            draw_gradient(titletxt_y, titletxt_h);
            titletxt_x = get_scroll_line_offset(PF_SCROLL_TRACK);
            color = 255;
        }
        else {
            titletxt_w = mylcd_getstringsize(trackname, NULL, NULL);
            titletxt_x = (LCD_WIDTH-titletxt_w)/2;
            fade = (abs(pf_tracks.sel - track_i) * 200 / pf_tracks.count);
            color = 250 - fade;
        }
        mylcd_set_foreground(G_BRIGHT(color));
        mylcd_putsxy(titletxt_x,titletxt_y,trackname);
        titletxt_y += titletxt_h;
    }
#endif
    return true;
}

static void select_next_track(void)
{
    if ( pf_tracks.sel < pf_tracks.count - 1 ) {
        pf_tracks.sel++;
        if (pf_tracks.sel==(pf_tracks.list_visible+pf_tracks.list_start))
            pf_tracks.list_start++;
    } else if (rb->global_settings->list_wraparound) {
        /* Rollover */
        pf_tracks.sel = 0;
        pf_tracks.list_start = 0;
    }
}

static void select_prev_track(void)
{
    if (pf_tracks.sel > 0 ) {
        if (pf_tracks.sel==pf_tracks.list_start) pf_tracks.list_start--;
        pf_tracks.sel--;
    } else if (rb->global_settings->list_wraparound) {
        /* Rolllover */
        pf_tracks.sel = pf_tracks.count - 1;
        pf_tracks.list_start = pf_tracks.count - pf_tracks.list_visible;
    }
}

static void select_next_album(void)
{
    if (center_index < number_of_slides - 1) {
        free_borrowed_tracks();
        target = center_index + 1;
        set_current_slide(target);
        skip_animation_to_show_tracks();
    }
}

static void select_prev_album(void)
{
    if (center_index > 0) {
        free_borrowed_tracks();
        target = center_index - 1;
        set_current_slide(target);
        skip_animation_to_show_tracks();
    }
}

#if PF_PLAYBACK_CAPABLE
static bool pf_current_playlist_insert(int position, bool queue, bool create_new)
{
    if (position == PLAYLIST_REPLACE)
    {
        if ((!create_new && rb->playlist_remove_all_tracks(NULL) == 0) ||
             (create_new && rb->playlist_create(NULL, NULL) == 0))
            position = PLAYLIST_INSERT_LAST;
        else
            return false;
    }

    if (!insert_whole_album)
        rb->playlist_insert_track(NULL, get_track_filename(pf_tracks.sel),
                                        position, queue, false);
    else
    {
        int i = 0;
        do {
            rb->yield();
            if (rb->playlist_insert_track(NULL, get_track_filename(i),
                    position, queue, false) < 0)
                break;
            if (position == PLAYLIST_INSERT_FIRST)
                position = PLAYLIST_INSERT;
        } while(++i < pf_tracks.count);
    }
    rb->playlist_sync(NULL);
    old_playlist = create_new ? center_slide.slide_index : -1;
    return true;
}


/* the song, or with Insert Whole Album its album, at the end of On-The-Go,
   as Music's lists add them: rather than to a playlist picked or named with
   the keyboard, as a stock iPod has it */
static void pf_add_to_on_the_go(void)
{
    /* its index and, if it plays, the queue with it, as Music's add them */
    if (!insert_whole_album)
    {
        rb->ip_add_file(OTG_FILE, get_track_filename(pf_tracks.sel));
        return;
    }
    for (int i = 0; i < pf_tracks.count; i++)
    {
        if (!rb->ip_add_file(OTG_FILE, get_track_filename(i)))
            break;
        rb->yield();
    }
}


/* the same at the end of a playlist made on the iPod, picked or made */
static int pf_add_to_playlist(void)
{
    char target[MAX_PATH];
    const int r = rb->ip_pick_playlist(target, sizeof(target));
    if (r <= 0)
        return r < 0 ? PLUGIN_USB_CONNECTED : 0;
    if (!insert_whole_album)
    {
        rb->ip_add_file(target, get_track_filename(pf_tracks.sel));
        return 0;
    }
    for (int i = 0; i < pf_tracks.count; i++)
    {
        if (!rb->ip_add_file(target, get_track_filename(i)))
            break;
        rb->yield();
    }
    return 0;
}


static bool track_list_ready(void)
{
    if (pf_state != pf_show_tracks)
    {
#ifdef HAVE_TC_RAMCACHE
        if (!rb->tagcache_is_in_ram())
#endif
            rb->splash(0, ID2P(LANG_WAIT));
        create_track_index(center_slide.slide_index);
        if (pf_tracks.count == 0)
        {
            free_borrowed_tracks();
            return false;
        }
        reset_track_list();
    }
    return true;
}


static bool menu_was_wide;

static bool context_menu_ready(void)
{
#ifdef USEGSLIB
    grey_show(false);
    rb->lcd_clear_display();
    rb->lcd_update();
#endif
    if (!track_list_ready())
    {
#ifdef USEGSLIB
        grey_show(true);
#endif
        return false;
    }
#if LCD_DEPTH > 1
#ifdef USEGSLIB
    rb->lcd_set_foreground(N_BRIGHT(0));
    rb->lcd_set_background(N_BRIGHT(255));
#elif defined (HAVE_LCD_COLOR)
    rb->lcd_set_background(rb->global_settings->bg_color);
    rb->lcd_set_foreground(rb->global_settings->fg_color);
#endif
#endif
    insert_whole_album = (pf_state != pf_show_tracks) || show_tracks_while_browsing;
    FOR_NB_SCREENS(i)
        rb->viewportmanager_theme_enable(i, true, NULL);
    /* the whole width, for Sort By's value beside it in every language */
    menu_was_wide = rb->menu_wide();
    rb->menu_set_wide(true);

    return true;
}

static void context_menu_cleanup(void)
{
    rb->menu_set_wide(menu_was_wide);
    FOR_NB_SCREENS(i)
        rb->viewportmanager_theme_undo(i, false);
    if (pf_state != pf_show_tracks)
        free_borrowed_tracks();
#ifdef USEGSLIB
    grey_show(true);
#elif LCD_DEPTH > 1 && defined(HAVE_LCD_COLOR)
    rb->lcd_set_background(N_BRIGHT(0));
    rb->lcd_set_foreground(N_BRIGHT(255));
#endif
    mylcd_set_drawmode(DRMODE_FG);
}


/* context_menu()'s answer for Go to Artist: Cover Flow quits, for the lists */
#define PF_LEAVE_FOR_LISTS (-100)

static int context_menu(void)
{
    /* as Music's lists have it: On-The-Go first, Add to Playlist, then, while music plays,
       Play Next and Play Last, for the album or, on its songs, the song; and Go to
       Artist, its album artist's albums, Menu there back here (root_menu.c) */
    enum {
        PF_ON_THE_GO = 0,
        PF_ADD_TO_PLAYLIST,
        PF_PLAY_NEXT,
        PF_PLAY_LAST,
        PF_GO_TO_ARTIST
    };
    /* and last Sort By, its value at the right end as in Albums: by album
       artist or by year */
    static char sort_by[64];
    rb->snprintf(sort_by, sizeof(sort_by), "%s\t%s", rb->str(LANG_SORT_BY),
                 rb->str(pf_cfg.sort_albums_by == SORT_BY_YEAR ? LANG_SORT_YEAR
                                                               : LANG_SORT_ARTIST));
    MENUITEM_STRINGLIST(playing_menu, ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                        ID2P(LANG_ADD_TO_ON_THE_GO),
                        ID2P(LANG_ADD_TO_PL),
                        ID2P(LANG_PLAY_NEXT),
                        ID2P(LANG_PLAY_LAST),
                        ID2P(LANG_GO_TO_ARTIST),
                        sort_by);
    MENUITEM_STRINGLIST(stopped_menu, ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                        ID2P(LANG_ADD_TO_ON_THE_GO),
                        ID2P(LANG_ADD_TO_PL),
                        ID2P(LANG_GO_TO_ARTIST),
                        sort_by);
    /* without Go to Artist where it isn't offered, as opened by a file's
       menu: its rows are the first of those, then Sort By */
    MENUITEM_STRINGLIST(playing_menu_here, ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                        ID2P(LANG_ADD_TO_ON_THE_GO),
                        ID2P(LANG_ADD_TO_PL),
                        ID2P(LANG_PLAY_NEXT),
                        ID2P(LANG_PLAY_LAST),
                        sort_by);
    MENUITEM_STRINGLIST(stopped_menu_here, ID2P(LANG_ONPLAY_MENU_TITLE), NULL,
                        ID2P(LANG_ADD_TO_ON_THE_GO),
                        ID2P(LANG_ADD_TO_PL),
                        sort_by);
    const bool playing = rb->audio_status() & AUDIO_STATUS_PLAY;
    const bool go_to = rb->root_menu_go_to_later(NULL);
    const int sort_row = (playing ? 4 : 2) + (go_to ? 1 : 0);

    int chosen = rb->do_menu(playing ? (go_to ? &playing_menu : &playing_menu_here)
                                     : (go_to ? &stopped_menu : &stopped_menu_here),
                             NULL, NULL, false);
    if (chosen == sort_row)
    {
        sort_albums(pf_cfg.sort_albums_by == SORT_BY_YEAR ? SORT_BY_ARTIST_AND_NAME
                                                          : SORT_BY_YEAR, true);
        /* kept at once, as USB or switching off saves nothing of it */
        configfile_save(CONFIG_FILE, config, CONFIG_NUM_ITEMS, CONFIG_VERSION);
        return 0;
    }
    if (!playing && go_to && chosen == PF_PLAY_NEXT)
        chosen = PF_GO_TO_ARTIST;
    switch (chosen)
    {
        case PF_GO_TO_ARTIST:
        {
            /* a slide's songs have one album artist: the song's, or the
               album's first */
            const char *path = get_track_filename(insert_whole_album ? 0
                                                                     : pf_tracks.sel);
            if (!path)
                return 0;
            rb->root_menu_go_to_later(path);
            return PF_LEAVE_FOR_LISTS;
        }
        case PF_ON_THE_GO:
            pf_add_to_on_the_go();
            return 0;
        case PF_ADD_TO_PLAYLIST:
            return pf_add_to_playlist();
        case PF_PLAY_NEXT:
        case PF_PLAY_LAST:
            rb->splash(0, ID2P(LANG_WAIT));
            pf_current_playlist_insert(chosen == PF_PLAY_NEXT ?
                                       PLAYLIST_INSERT_FIRST : PLAYLIST_INSERT_LAST,
                                       false, false);
            rb->playlist_set_modified(NULL, true);
            return 0;
        case MENU_ATTACHED_USB:
            return PLUGIN_USB_CONNECTED;
        default:
            return 0;
    }
}



/*
 * Puts selected album's tracks into a newly created playlist and starts playing
 */
static bool start_playback(bool return_to_WPS)
{
#ifdef USEGSLIB
    grey_show(false);
#if LCD_DEPTH > 1
    rb->lcd_set_background(N_BRIGHT(0));
    rb->lcd_set_foreground(N_BRIGHT(255));
#endif
    rb->lcd_clear_display();
    rb->lcd_update();
#else /* if !USEGSLIB */
    (void) return_to_WPS;
#endif

    if (!rb->warn_on_pl_erase() || !track_list_ready())
    {
#ifdef USEGSLIB
        grey_show(true);
#endif
        return false;
    }

#ifdef ITUNESDB_HAVE_ART
    /* A single: within all songs, as an album of one would stop after it. Its
       playlist was made at the import; without one, as any album. */
    if (pf_tracks.count == 1
        && rb->tagtree_play_song_in_library(get_track_filename(0)))
    {
        old_playlist = -1;
        return true;
    }
#endif

    insert_whole_album = true;
    int start_index = pf_tracks.sel;
    bool shuffle = rb->global_settings->playlist_shuffle;
    /* can't reuse playlist if it may be out of sync with our track list */
    if (shuffle || center_slide.slide_index != old_playlist
                || (old_shuffle != shuffle))
    {
        if (!pf_current_playlist_insert(PLAYLIST_REPLACE, false, true))
        {
#ifdef USEGSLIB
            grey_show(true);
#endif
            return false;
        }
        if (shuffle)
            start_index = rb->playlist_shuffle(0, pf_tracks.sel);
    }
    rb->playlist_start(start_index, 0, 0);
    old_shuffle = shuffle;
#ifdef USEGSLIB
    if (!return_to_WPS)
        grey_show(true);
#endif
    return true;
}
#endif /* PF_PLAYBACK_CAPABLE */

/**
   Draw the current album name
 */
static void draw_album_text(void)
{
    char album_and_year[MAX_PATH];

    if (pf_cfg.show_album_name == ALBUM_NAME_HIDE)
        return;
#if defined(ITUNESDB_HAVE_ART) && defined(HAVE_LCD_COLOR)
    /* the card over where it was, and then the panel */
    if (pf_state == pf_show_tracks || card_frame)
        return;
#endif

    static int prev_albumtxt_index = -1;
    static bool prev_show_year = false;
    int albumtxt_index;
    int char_height;
    int albumtxt_x, albumtxt_y, artisttxt_x;
    int album_idx = 0;

    char *albumtxt;
    char *artisttxt;
    /* the album in the middle, or the one coming to it from half way there: its
       name at once, as on a stock iPod. Faded out and in between albums, the
       names blinked as the covers rolled by. */
    albumtxt_index = center_index;
    if ( pf_state == pf_scrolling ) {
        /* where the covers are in this frame: slide_frame is set to a new
           centre as they reach it, which going left read as the whole way
           to the next one, and the album beyond showed for a frame */
        int part = fade;
        if (step < 0)
            part = 255 - part;
        if (part > 128)
            albumtxt_index = center_index + step;
    }
    albumtxt = get_album_name_idx(albumtxt_index, &album_idx);

    if (pf_cfg.show_year && album_shown_year(albumtxt_index) > 0)
    {
        rb->snprintf(album_and_year, sizeof(album_and_year), "%s – %d",
             albumtxt, album_shown_year(albumtxt_index));
    } else
        rb->snprintf(album_and_year, sizeof(album_and_year), "%s", albumtxt);

    /* dark on the light behind the covers */
    mylcd_set_foreground(pf_light ? G_BRIGHT(0) : G_BRIGHT(255));
    /* measured once an album, and the artist's then too, which set each frame
       was measured each and never scrolled */
    const bool new_album = albumtxt_index != prev_albumtxt_index
                           || pf_cfg.show_year != prev_show_year;
    if (new_album) {
        set_scroll_line(album_and_year, PF_SCROLL_ALBUM);
        prev_albumtxt_index = albumtxt_index;
        prev_show_year = pf_cfg.show_year;
    }

    char_height = rb->screens[SCREEN_MAIN]->getcharheight();
    switch(pf_cfg.show_album_name){
        case ALBUM_AND_ARTIST_TOP:
            albumtxt_y = 0;
            break;
        case ALBUM_NAME_BOTTOM:
        case ALBUM_AND_ARTIST_BOTTOM:
            albumtxt_y = (LCD_HEIGHT - (char_height * 5 / 2));
            break;
        case ALBUM_NAME_TOP:
        default:
            albumtxt_y = char_height / 2;
            break;
    }

    albumtxt_x = get_scroll_line_offset(PF_SCROLL_ALBUM);


    if ((pf_cfg.show_album_name == ALBUM_AND_ARTIST_TOP)
        || (pf_cfg.show_album_name == ALBUM_AND_ARTIST_BOTTOM)){

        if (album_idx != (int) pf_idx.album_untagged_idx)
            mylcd_putsxy(albumtxt_x, albumtxt_y, album_and_year);

        static char artist_and_year[MAX_PATH];
        artisttxt = (char *)get_album_artist_year(albumtxt_index, artist_and_year,
                                                  sizeof(artist_and_year));
        if (new_album)
            set_scroll_line(artisttxt, PF_SCROLL_ARTIST);
        artisttxt_x = get_scroll_line_offset(PF_SCROLL_ARTIST);
#ifdef ITUNESDB_HAVE_ART
        /* the two lines close together, as on a stock iPod */
        int y_offset = COVER_FLOW_LINES;
#else
        int y_offset = char_height + char_height/2;
#endif
        mylcd_putsxy(artisttxt_x, albumtxt_y + y_offset, artisttxt);
    } else {
        mylcd_putsxy(albumtxt_x, albumtxt_y, album_and_year);
    }
}

/**
  Display an error message and wait for input.
*/
static void error_wait(const char *message)
{
    pf_boost(false);
    rb->splashf(0, "%s. Press any button to continue.", message);
    while (rb->get_action(CONTEXT_STD, 1) == ACTION_NONE)
        rb->yield();
    rb->sleep(2 * HZ);
}

static bool init(void)
{
    int ret = SUCCESS;
    void * buf;
    size_t buf_size;

    rb->memset(&cf_marks, 0, sizeof(cf_marks));
    cf_marks.opened_at = *rb->current_tick;
    /* while it opens; then the main loop's, as the covers move */
    pf_boost(true);

    wants_to_quit = false;
#if defined(ITUNESDB_HAVE_ART) && defined(HAVE_LCD_COLOR)
    pf_light = !rb->global_settings->dark_theme;
#endif
    pf_back = pf_light ? G_BRIGHT(255) : G_BRIGHT(0);

    /* must appear before config load */
    rb->memset(&aa_cache, 0, sizeof(struct albumart_t));

    config_set_defaults(&pf_cfg); /* must appear before configfile_save */
    configfile_load(CONFIG_FILE, config, CONFIG_NUM_ITEMS, CONFIG_VERSION);
#ifdef ITUNESDB_HAVE_ART
    pf_cfg.slide_spacing = COVER_FLOW_SPACING;
    pf_cfg.center_margin = COVER_FLOW_MARGIN;
    pf_cfg.zoom = COVER_FLOW_ZOOM;
    pf_cfg.num_slides = COVER_FLOW_SLIDES;
#endif

#ifdef ITUNESDB_HAVE_ART
    /* music waits while here, so a track that is chosen plays on the WPS */
    if (pf_cfg.auto_wps == 0)
        pf_cfg.auto_wps = 2;
#endif

#ifndef ITUNESDB_HAVE_ART
    if(pf_cfg.backlight_mode == 0)
        backlight_ignore_timeout(); /* restore in cleanup */
#endif
    /* else the screen goes dark and the iPod to sleep as anywhere, whatever
       an old config says: no menu here sets it */

#if PF_PLAYBACK_CAPABLE
    buf = rb->plugin_get_buffer(&buf_size);
#else
    buf = rb->plugin_get_audio_buffer(&buf_size);
#ifndef SIMULATOR
    if ((uintptr_t)buf < (uintptr_t)plugin_start_addr)
    {
        uint32_t tmp_size = (uintptr_t)plugin_start_addr - (uintptr_t)buf;
        buf_size = MIN(buf_size, tmp_size);
    }
#endif
#endif

#ifdef USEGSLIB
    long grey_buf_used;
    if (!grey_init(buf, buf_size, GREY_BUFFERED|GREY_ON_COP,
                   LCD_WIDTH, LCD_HEIGHT, &grey_buf_used))
    {
        error_wait("Greylib init failed!");
        return false;
    }
    grey_setfont(FONT_UI);
    buf_size -= grey_buf_used;
    buf = (void*)(grey_buf_used + (char*)buf);
#endif

    /* store buffer pointers and sizes */
    pf_idx.buf = buf;
    pf_idx.buf_sz = buf_size;

    rb->lcd_setfont(FONT_UI);
#if defined(ITUNESDB_HAVE_ART) && defined(HAVE_LCD_COLOR)
    load_panel_fonts();
#endif

    if (!rb->dir_exists(CACHE_PREFIX))
    {
        if (rb->mkdir( CACHE_PREFIX ) < 0)
        {
            error_wait("Could not create directory " CACHE_PREFIX);
            return false;
        }
    }

    rb->mutex_init(&buf_ctx_mutex);

    init_scroll_lines();
    init_reflect_table();

    /*Scan will trigger when no file is found or the option was activated*/
    if ((pf_cfg.cache_version != CACHE_VERSION)|| (load_album_index() < 0))
    {
        ret = create_album_index();

        if (ret == 0)
        {
            pf_cfg.cache_version = CACHE_REBUILD;
            if (save_album_index() < 0)
                rb->splash(HZ, "Could not write index");
        }
    }

    if (ret == ERROR_BUFFER_FULL)
    {
        error_wait("Not enough memory for album names");
        return false;
    }
    else if (ret == ERROR_NO_ALBUMS)
    {
        error_wait("No albums found. Please enable database");
        return false;
    }
    else if (ret == ERROR_USER_ABORT)
        return false;

    number_of_slides = pf_idx.album_ct;

    /* a quarter, but no more than a cover being made needs: the rest is
       for the slides */
    size_t aa_bufsz = ALIGN_DOWN(MIN(pf_idx.buf_sz / 4, 0x40000),
                                 sizeof(long));
    if (aa_bufsz < DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(pix_t))
    {
        error_wait("Not enough memory for album art cache");
        return false;
    }

    ALIGN_BUFFER(pf_idx.buf, pf_idx.buf_sz, sizeof(long));
    aa_cache.buf = (char*) pf_idx.buf;
    aa_cache.buf_sz = aa_bufsz;

    pf_idx.buf += aa_bufsz;
    pf_idx.buf_sz -= aa_bufsz;
#ifdef ITUNESDB_HAVE_ART
    const size_t slides_sz = ALIGN_UP(pf_idx.album_ct * sizeof(int32_t), sizeof(long));
    album_slides = NULL;
    if (slides_sz < pf_idx.buf_sz / 8)
    {
        album_slides = (int32_t *)pf_idx.buf;
        rb->memset(album_slides, 0, slides_sz);
        pf_idx.buf += slides_sz;
        pf_idx.buf_sz -= slides_sz;
    }
#endif
#ifdef PF_SHOW_ON_COP
    /* the second frame, if it leaves the covers room, on cache lines of its
       own as the COP reads it */
    frame_other = NULL;
    shown_later = NULL;
    ALIGN_BUFFER(pf_idx.buf, pf_idx.buf_sz, 32);
    const size_t frame_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(pix_t);
    if (frame_sz < pf_idx.buf_sz / 4)
    {
        frame_other = (pix_t *)pf_idx.buf;
        frame_other_fb.fb_ptr = frame_other;
        frame_other_fb.get_address_fn = NULL;
        frame_other_fb.stride = 0;
        frame_other_fb.elems = LCD_WIDTH * LCD_HEIGHT;
        pf_idx.buf = (char *)pf_idx.buf + frame_sz;
        pf_idx.buf_sz -= frame_sz;
    }
#endif
#if CARD_FRAME_COUNT > 0
    /* the card, if it leaves the covers room */
    card = NULL;
    card_drawn = false;
    ALIGN_BUFFER(pf_idx.buf, pf_idx.buf_sz, sizeof(long));
    const size_t card_sz = sizeof(struct dim) + CARD_W * CARD_H * sizeof(pix_t);
    if (CARD_SQUARE && card_sz < pf_idx.buf_sz / 4)
    {
        card = (struct dim *)pf_idx.buf;
        pf_idx.buf = (char *)pf_idx.buf + card_sz;
        pf_idx.buf_sz -= card_sz;
    }
#endif

    rb->buflib_init(&buf_ctx, (void *)pf_idx.buf, pf_idx.buf_sz);
    initialize_slide_cache();
    is_initial_slide = true;

    if (!create_empty_slide(pf_cfg.cache_version != CACHE_VERSION))
    {
        config_save(CACHE_REBUILD, false);
        error_wait("Could not load the empty slide");
        return false;
    }

    bool prepare = pf_cfg.cache_version != CACHE_VERSION;
#ifdef ITUNESDB_HAVE_ART
    bool background_note = false; /* the covers just come, as on an iPod */
#else
    bool background_note = true;
#endif
#ifdef ITUNESDB_HAVE_ART
    /* The covers are the slides made at Updating Library of those Finder made:
       nothing is read of the music's files here, neither now nor while it
       stands still. Of an album without one the note shows, as on an iPod. */
    prepare = false;
#endif
    if (prepare && !create_albumart_cache())
    {
        config_save(CACHE_REBUILD, false);
        error_wait("Could not create album art cache");
    }
    else if (background_note && aa_cache.inspected < pf_idx.album_ct)
        rb->splash(HZ * 2, "Updating album art cache in background");

    if (pf_cfg.cache_version != CACHE_VERSION)
        config_save(CACHE_VERSION, pf_cfg.update_albumart);

    /* made again if it doesn't read, as when power was cut as it was written */
    empty_slide_hid = -1; /* what a failure to read it gives */
    if ((empty_slide_hid = read_pfraw(EMPTY_SLIDE, 0)) < 0
        && (!create_empty_slide(true)
            || (empty_slide_hid = read_pfraw(EMPTY_SLIDE, 0)) < 0))
    {
        error_wait("Unable to load empty slide image");
        return false;
    }
    load_unshown_slide();

    if (!create_pf_thread())
    {
        error_wait("Cannot create thread!");
        return false;
    }
#ifdef PF_COP_FROM
    create_cop_thread(); /* or the CPU draws all */
#endif

    buffer = LCD_BUF;

    pf_state = pf_idle;

    pf_tracks.cur_idx = -1;
    pf_tracks.borrowed = 0;
    pf_tracks.used = 0;

    extra_fade = 0;
    slide_frame = 0;
    step = 0;
    target = 0;
    fade = 256;

    recalc_offsets();
    reset_slides();

#ifdef USEGSLIB
    grey_set_drawmode(DRMODE_FG);
#endif
    rb->lcd_set_drawmode(DRMODE_FG);

    return true;
}

static bool reinit(void)
{
    return_to_idle_state();

    unsigned int hash_album = mfnv(get_album_name(center_index));
    unsigned int hash_artist = mfnv(get_album_artist(center_index));

    cleanup();
    if (init())
    {
        wheel_start();
        reselect(hash_album, hash_artist); /* splash if not found */
#ifdef USEGSLIB
        grey_show(true);
#endif
        return true;
    }
    return false;
}

static bool prompt_reinit(void)
{
    const struct text_message prompt = {
      (const char*[]) {ID2P(LANG_TAGCACHE_BUSY), ID2P(LANG_TAGCACHE_UPDATE)}, 2};
    pf_boost(false); /* a question, as the menus */
#ifdef USEGSLIB
    grey_show(false);
#endif
    if (rb->gui_syncyesno_run(&prompt, NULL, NULL) == YESNO_YES)
    {
        pf_cfg.update_albumart = true;
        pf_cfg.cache_version = CACHE_REBUILD;
        rb->remove(EMPTY_SLIDE);
        rb->remove(UNSHOWN_SLIDE);
        configfile_save(CONFIG_FILE, config, CONFIG_NUM_ITEMS, CONFIG_VERSION);
        if (!reinit())
            return false;
    }
    else
    {
#ifdef USEGSLIB
        grey_show(true);
#endif
        mylcd_set_drawmode(DRMODE_FG);
    }
    wheel_clicks_taken();
    return true;
}

#ifdef HAVE_WHEEL_POSITION
#define WHEEL_CLICKS        96 /* in a turn */
#define WHEEL_READS         100 /* a second */
#define WHEEL_RECENT        12 /* readings that its speed is taken over */
#define WHEEL_RECENT_MIN    4  /* as if at least this many, as one can be off */
#define WHEEL_CLICKS_MAX    8  /* in a reading: more is the place jumping when a finger
                                  comes off, as no one turns it 8 times a second */
#define WHEEL_LOST          3  /* readings of no finger, as one can be lost for a moment */
#define WHEEL_HELD          8  /* readings of a finger not moving, that stops the covers */

/* The wheel, read by a timer and not once a frame: a quick swipe can be over before
   the next frame */
static struct {
    int position; /* -1 with no finger on it */
    int lost;
    signed char recent[WHEEL_RECENT];
    int oldest;
    int sum;      /* of recent */
    int moving;   /* of recent, since it started to move */
    int readings; /* since it was touched, or turned back */
    int travel;   /* clicks this way, positive towards the next slide */
    int energy;   /* what the covers are to follow, kept when it's let go */
    bool let_go;
    /* every click it went, positive towards the next slide: the slides it
       steps go by these (watch_wheel()), and not by its events, of which a
       frame takes one, those coming meanwhile held as one by its driver */
    volatile int clicks;
    volatile long moved_at; /* the tick it last went a click */
} wheel = { .position = -1 };

static void wheel_read(void)
{
#ifdef HAS_BUTTON_HOLD
    /* Hold turns the sensor off and leaves its last place: no finger on it */
    const int position = rb->button_hold() ? -1 : rb->wheel_status();
#else
    const int position = rb->wheel_status();
#endif

    if (position < 0)
    {
        if (wheel.position >= 0 && ++wheel.lost >= WHEEL_LOST)
        {
            wheel.position = -1;
            wheel.let_go = true;
        }
        return;
    }
    wheel.lost = 0;

    bool from_start = wheel.position < 0;
    int delta = 0;
    if (!from_start)
    {
        delta = position - wheel.position;
        if (delta < -WHEEL_CLICKS / 2)
            delta += WHEEL_CLICKS;
        else if (delta > WHEEL_CLICKS / 2)
            delta -= WHEEL_CLICKS;
        /* the place jumping, as a finger comes off and on: from where it is
           now, as the wheel's driver does, and not a flick of 8 clicks more,
           or one turned back at its end */
        if (delta > WHEEL_CLICKS_MAX || delta < -WHEEL_CLICKS_MAX)
            delta = 0;
        /* it wavers by a click or two when it's held */
        from_start = (delta ^ wheel.travel) < 0 && abs(delta) > 2;
    }
    wheel.position = position;
    if (delta)
    {
        wheel.clicks += delta;
        wheel.moved_at = *rb->current_tick;
    }

    if (from_start)
    {
        rb->memset(wheel.recent, 0, sizeof(wheel.recent));
        wheel.sum = wheel.moving = wheel.travel = wheel.energy = wheel.readings = 0;
        wheel.let_go = false;
    }
    wheel.readings++;
    if (delta == 0 && wheel.moving == 0)
        return;

    wheel.sum += delta - wheel.recent[wheel.oldest];
    wheel.recent[wheel.oldest] = delta;
    wheel.oldest = (wheel.oldest + 1) % WHEEL_RECENT;
    wheel.moving = fmin(wheel.moving + 1, WHEEL_RECENT);
    wheel.travel += delta;

    const int direction = wheel.travel > 0 ? 1 : -1;
    const int speed = direction * wheel.sum * WHEEL_READS
                      / fmax(wheel.moving, WHEEL_RECENT_MIN);
    wheel.energy = fmin(fbound(0, speed, ROLL_CLICKS_MAX) * ROLL_PER_CLICK
                        * fmin(direction * wheel.travel, ROLL_TRAVEL_FULL)
                        / ROLL_TRAVEL_FULL, ROLL_SPEED_MAX);
}

/* Start rolling that way if they can. The wheel can be too quick for its events to
   have got them moving, or turned them back: going the other way, the first only
   stops them. */
static void roll_start(int direction)
{
    if (pf_state != pf_idle && pf_state != pf_scrolling)
        return;
    seeking = false;
    roll_carry = 0;
    for (int i = 0; i < 2 && step != direction; i++)
    {
        if (direction > 0)
            show_next_slide();
        else
            show_previous_slide();
    }
    if (pf_state == pf_scrolling && step == direction)
    {
        roll_settling = false;
        roll_speed = fmax(anim_speed, ROLL_SPEED_MIN);
        target = slide_ahead(2);
    }
}

/* The clicks counted into slides (watch_wheel()): those seen, and those this
   way short of a slide */
static int wheel_clicks_seen, wheel_clicks_part;

/* the clicks so far aren't the covers': a menu or a question had the wheel,
   or an event stepped for them */
static void wheel_clicks_taken(void)
{
    wheel_clicks_seen = wheel.clicks;
    wheel_clicks_part = 0;
    wheel.let_go = false;
}

/* the wheel read from now on, at the start and after a reinit()'s cleanup(),
   which let the timer go */
static void wheel_start(void)
{
    rb->memset(&wheel, 0, sizeof(wheel));
    wheel.position = -1;
    wheel_waking = false;
    wheel_clicks_taken();
    wheel_timer_set = rb->timer_register(1, NULL, TIMER_FREQ / WHEEL_READS,
                                         wheel_read IF_COP(, CPU));
}

/* The wheel's events, and the click each makes, only away from the covers: there
   the slides step and click by its readings (watch_wheel()), a click a slide, as
   its events, a step each 4 clicks, would click 1 to 3 times a slide; gliding,
   each album passing clicks (update_scroll_animation()). Menus and
   questions opened from the covers have them (pictureflow_main()). */
static void wheel_events_on(bool on)
{
    if (on != wheel_events)
    {
        wheel_events = on;
        rb->wheel_send_events(on);
    }
}

/* Once a frame: what the wheel means for the covers. What they follow is its speed
   over the last moments, for as much of a turn as it has made: a touch sends them
   nowhere, and a short quick swipe as far as it deserves. A finger put back on the
   wheel adds that to what they have, so that turn after turn speeds them up; held
   still it stops them, and turning the other way turns them back. */
static void watch_wheel(bool lit)
{
    /* the slides it stepped: one each WHEEL_SLIDE_CLICKS this way, counted
       again from a turn back */
    const int clicks = wheel.clicks;
    const int moved = clicks - wheel_clicks_seen;
    wheel_clicks_seen = clicks;
    const bool covers = pf_state == pf_idle || pf_state == pf_scrolling;
    /* The touch that lights the dark screen only lights it, as its first
       press would: its clicks aren't steps, nor its flick a glide, until the
       finger is off or still a moment with the screen lit. The driver's
       events are off meanwhile, in the tracks too, so it doesn't click */
    if (!lit)
        wheel_waking = true;
    if (wheel_timer_set)
        wheel_events_on(!covers && !wheel_waking);
    if (wheel_waking)
    {
        if (roll_speed && !roll_free)
        {
            /* dark under a finger on the wheel (Hold): they run down as
               after a lift with nothing given, not on to the last album */
            roll_free = true;
            roll_aim = 0;
            roll_carry = 0;
        }
        wheel_clicks_taken();
        if (lit && (wheel.position < 0
                    || TIME_AFTER(*rb->current_tick, wheel.moved_at + HZ / 4)))
            wheel_waking = false;
        return;
    }
    if (!covers)
        wheel_clicks_part = 0; /* the tracks and a cover turning go by its events */
    else
    {
        int stepped = 0;
        if ((moved > 0 && wheel_clicks_part < 0)
            || (moved < 0 && wheel_clicks_part > 0))
            wheel_clicks_part = 0;
        for (wheel_clicks_part += moved;
             wheel_clicks_part >= WHEEL_SLIDE_CLICKS;
             wheel_clicks_part -= WHEEL_SLIDE_CLICKS, stepped = 1)
            show_next_slide();
        for (; wheel_clicks_part <= -WHEEL_SLIDE_CLICKS;
             wheel_clicks_part += WHEEL_SLIDE_CLICKS, stepped = -1)
            show_previous_slide();
        /* a click a slide, as its events made one a step: one a frame at
           most, each holding the frame for its sound */
        if (stepped)
            rb->keyclick_click(true, stepped > 0 ? BUTTON_SCROLL_FWD
                                                 : BUTTON_SCROLL_BACK);
    }

    const int direction = wheel.travel > 0 ? 1 : -1;
    const int energy = wheel.energy;

    if (wheel.let_go)
    {
        wheel.let_go = false;
        if ((!roll_speed || step != direction) && energy >= ROLL_SPEED_MIN)
            roll_start(direction);
        if (roll_speed)
        {
            roll_aim = fmin(roll_carry + (step == direction ? energy : 0),
                            ROLL_SPEED_TOP);
            roll_carry = 0;
            roll_free = true;
        }
        return;
    }

    if (wheel.position < 0)
        return;

    if (roll_free)
    {
        roll_free = false;
        roll_carry = roll_aim;
    }
    if (wheel.readings >= WHEEL_HELD && abs(wheel.sum) <= 1)
        roll_carry = 0;
    if ((!roll_speed || step != direction) && energy >= ROLL_SPEED_MIN
        && pf_state == pf_scrolling)
        roll_start(direction);
    if (roll_speed)
        roll_aim = fmin(roll_carry + (step == direction ? energy : 0), ROLL_SPEED_TOP);
}

/* the event a step of the click wheel's, with its readings taken: a finger on
   it, or one just off it with the event behind. Plugins can't read an event's
   data on the iPod (no HAVE_BUTTON_DATA): a remote's next or previous is told
   by the wheel being still */
static bool wheel_stepped(void)
{
    return wheel_timer_set
           && (wheel.position >= 0
               || TIME_BEFORE(*rb->current_tick, wheel.moved_at + HZ / 4));
}

/* a finger on the wheel: with its events off, nothing else wakes the loop */
static bool wheel_touched(void)
{
    return wheel_timer_set && wheel.position >= 0;
}
#else
static inline bool wheel_stepped(void) { return false; }
static inline bool wheel_touched(void) { return false; }
#endif /* HAVE_WHEEL_POSITION */

#ifdef PF_SHOW_ON_COP
/* The frame drawn last on the screen, if the COP was to show it, and the
   screen's to draw in again */
static void show_frame_now(void)
{
    cop_show = NULL;
    const pix_t *frame = shown_later;
    shown_later = NULL;
    buffer = LCD_BUF;
    if (!frame)
        return;
    if (frame != LCD_BUF)
        rb->memcpy(LCD_BUF, frame, LCD_WIDTH * LCD_HEIGHT * sizeof(pix_t));
    mylcd_update();
}

/* The lines over a frame go into the one it's drawn in: the viewport to go
   back to, NULL if that's the screen's */
static struct viewport *draw_in_frame(void)
{
    if (buffer == LCD_BUF)
        return NULL;
    struct viewport *vp = rb->lcd_set_viewport(NULL);
    rb->lcd_set_viewport(vp);
    frame_other_vp = *vp;
    frame_other_vp.buffer = &frame_other_fb;
    rb->lcd_set_viewport(&frame_other_vp);
    return vp;
}
#endif

static unsigned long anim_now(void)
{
#ifdef PF_USEC_TIMER
    return PF_USEC_TIMER;
#else
    return (unsigned long)*rb->current_tick * (1000000 / HZ);
#endif
}

/* Once a frame: the time since the last one, which the animation goes on by;
   the first of one goes a frame's way */
static void anim_clock_tick(bool animating)
{
    const unsigned long now = anim_now();
    if (!animating)
        anim_clock_running = false;
    else if (!anim_clock_running)
    {
        anim_part = ANIM_ONE;
        anim_keys = 0;
        anim_clock_running = true;
    }
    else
    {
        const unsigned long took = now - anim_clock;
        if (pf_state == pf_cover_in && took > cf_marks.frame_in)
        {
            cf_marks.frame_in = took;
            cf_marks.frame_in_key = cover_animation_keyframe;
        }
        else if (pf_state == pf_cover_out && took > cf_marks.frame_out)
            cf_marks.frame_out = took;
        const unsigned long us = MIN(took,
                                     (unsigned long)ANIM_FRAME_US * ANIM_MOST / ANIM_ONE);
        anim_part = us * ANIM_ONE / ANIM_FRAME_US;
    }
    anim_clock = now;
}

/* the last time the covers moved, the wheel turned or a button was pressed */
static long used_at;
static long last_use(void)
{
#ifdef HAVE_WHEEL_POSITION
    if (TIME_AFTER(wheel.moved_at, used_at))
        return wheel.moved_at;
#endif
    return used_at;
}

/* slides for the thread to load, or a wakeup it hasn't seen yet */
static bool slides_pending(void)
{
    return thread_loading || !rb->queue_empty(&thread_q);
}

/* one of these lines moving on the screen now, rather than waiting at an end:
   each of its steps draws a frame of the covers */
static bool line_moves(unsigned lines)
{
    for (int i = 0; i < PF_MAX_SCROLL_LINES; i++)
        if ((lines & (1u << i)) && scroll_lines[i].step
            && !TIME_BEFORE(*rb->current_tick, scroll_lines[i].start_tick))
            return true;
    return false;
}

#if CARD_FRAME_COUNT > 0
/* The covers still on an album this long, its tracks and card are made then,
   so that Select opens it at once: longer than the stops of a turn of two
   covers a second, and shorter than a look before a press */
#define PREPARE_REST        (HZ * 2 / 5)

/* The covers still on an album for PREPARE_REST and the slide thread through:
   its tracks, then its card, a step a turn of the loop, on which Select opens
   it at once. Not while anything moves, nor with slides to load, as making the
   tracks takes buf_ctx for a while. */
static void prepare_ahead(void)
{
    const int idx = center_slide.slide_index;
    if (pf_state != pf_idle || !card || idx == prepare_failed
        || (card_drawn && card_album == idx && pf_tracks.cur_idx == idx)
        || TIME_BEFORE(*rb->current_tick, last_use() + PREPARE_REST)
        || slides_pending())
        return;
#ifdef HAVE_TC_RAMCACHE
    if (!rb->tagcache_is_in_ram())
        return; /* from the disk, with its wait shown, at Select */
#endif
    pf_boost(true); /* the next turn lets it go */
    const unsigned long start = anim_now();
    if (pf_tracks.cur_idx != idx)
    {
        create_track_index(idx);
        if (pf_tracks.count == 0 || pf_tracks.borrowed > TRACKS_KEEP)
        {
            /* made again at Select, as before, which then turns back or
               gives the room back at the close: not kept at rest */
            free_borrowed_tracks();
            rb->queue_post(&thread_q, EV_WAKEUP, 0);
            prepare_failed = idx;
        }
        else
        {
            reset_track_list();
            tracks_unlock();
        }
    }
    else
        make_card();
    const unsigned long took = anim_now() - start;
    cf_marks.prepared++;
    cf_marks.prepared_most = MAX(cf_marks.prepared_most, took);
}
#endif

/**
  Main function that also contain the main plasma
  algorithm.
 */
static int pictureflow_main(void)
{
    int ret;
    char fpstxt[10];
    int button;
    int frames = 0;
    long last_update = *rb->current_tick;
    long current_update;
    long update_interval = 100;
    int fps = 0;
    int fpstxt_y;
    bool instant_update;
    /* Still covers are drawn again only when something on them changed: a
       cover that came, a line that scrolls, a button, which may have been a
       menu over them. Each of the others is a fifth of a second of the CPU's. */
    bool idle_shown = false;
    unsigned idle_slides = 0;

    wheel_start();

    used_at = *rb->current_tick;
    bool was_lit = true;
    while (true) {
        current_update = *rb->current_tick;
        frames++;

        /* Initial rendering */
        instant_update = false;

        /* The screen dark, at its timeout as anywhere: nothing still is drawn
           until it's lit again, and then it is */
#ifdef HAS_BUTTON_HOLD
        /* on Hold its own setting, by default off */
        const bool lit = rb->is_backlight_on(rb->button_hold());
#else
        const bool lit = rb->is_backlight_on(false);
#endif
        if (was_lit && !lit)
        {
            cf_marks.darks++;
            cf_marks.dark_after = current_update - last_use();
        }
        was_lit = lit;

        const unsigned lines_moved = update_scroll_lines();
#ifdef HAVE_WHEEL_POSITION
        watch_wheel(lit);
#endif
        const bool animating = pf_state == pf_scrolling || pf_state == pf_cover_in
                               || pf_state == pf_cover_out;
        anim_clock_tick(animating);
        if (animating)
            used_at = *rb->current_tick;
        /* 80 MHz while they move, a slide loads, just after a use (a step of
           the tracks), or while a line moves on the screen: each of its steps
           draws the covers again, about a step's time at 30 MHz. Not at rest,
           where the iPod can then sleep at its lower clock. */
        const bool loading = slides_pending() && !buf_ctx_locked;
        pf_boost(animating || loading
                 || TIME_BEFORE(*rb->current_tick, last_use() + HZ / 2)
                 || (lit && line_moves(pf_state == pf_show_tracks
                                       ? 1u << PF_SCROLL_TRACK
                                       : pf_state == pf_idle
                                       ? 1u << PF_SCROLL_ALBUM | 1u << PF_SCROLL_ARTIST
                                       : 0)));
        /* The disk sleeps 5 s after it was last used and is off 2 s later: the
           next cover read while scrolling would hold the animation until it
           is back. It is kept on while they move or load, and while the screen
           is lit, at most 30 s, the longest timeout short of always on; then
           it's let go, as anywhere: else the iPod never slept in here. */
        if (animating || loading
            || (lit && TIME_BEFORE(*rb->current_tick, last_use() + 30 * HZ)))
            rb->storage_spin();
        /* the music decoded ahead of the covers, and while they move not, as
           long as there's 3 s of it: it is made up when they stop */
        if (animating)
            rb->pcmbuf_hold_for_ui(HZ / 4);
#ifdef PF_SHOW_ON_COP
        bool show_on_cop = animating && cop_draws && frame_other && !cpu_shows;
        if (show_on_cop)
            cop_show = shown_later;
        else
            show_frame_now();
#endif
        bool drawn = true;
        /* still, the covers or the tracks: drawn again only for a change */
        static int shown_state;
        if ((pf_state != pf_idle && pf_state != pf_show_tracks)
            || pf_state != shown_state)
            idle_shown = false;
        shown_state = pf_state;

        /* Handle states */
        switch ( pf_state ) {
            case pf_scrolling:
                update_scroll_animation();
                render_all_slides();
                instant_update = true;
                break;
            case pf_cover_in:
                /* the key frames the time took, none if it was short: a
                   quarter faster than the frames of the rest */
                for (anim_keys += anim_part + anim_part / 4;
                     anim_keys >= ANIM_ONE && pf_state == pf_cover_in;
                     anim_keys -= ANIM_ONE)
                    update_cover_in_animation();
                if (pf_state == pf_show_tracks)
                {
                    /* the panel in this frame, where the card came to: drawn as the
                       next one, it left a frame of the covers without either */
#ifdef PF_SHOW_ON_COP
                    show_frame_now();
                    show_on_cop = false;
#endif
                    if (!show_track_list() && !prompt_reinit())
                        return PLUGIN_OK;
                    break;
                }
                render_all_slides();
                instant_update = true;
                break;
            case pf_cover_out:
                show_tracks_while_browsing = false;
                for (anim_keys += anim_part + anim_part / 4;
                     anim_keys >= ANIM_ONE && pf_state == pf_cover_out;
                     anim_keys -= ANIM_ONE)
                    update_cover_out_animation();
                render_all_slides();
                instant_update = true;
                break;
            case pf_show_tracks:
                /* the covers behind it were drawn 16 times a second, which
                   held the music back from getting ahead */
                if (!lit || (idle_shown && !(lines_moved & (1u << PF_SCROLL_TRACK))
                             && !pf_cfg.show_fps))
                {
                    idle_shown = idle_shown && lit;
                    drawn = false;
                    break;
                }
                idle_shown = true;
                if (!show_track_list() && !prompt_reinit())
                    return PLUGIN_OK;
                break;
            case pf_idle:
                show_tracks_while_browsing = false;
                if (!lit || (idle_shown
                    && !(lines_moved & (1u << PF_SCROLL_ALBUM | 1u << PF_SCROLL_ARTIST))
                    && idle_slides == slides_arrived && !pf_cfg.show_fps))
                {
                    idle_shown = idle_shown && lit;
                    drawn = false;
                    break;
                }
                idle_slides = slides_arrived;
                idle_shown = true;
                render_all_slides();
#ifndef ITUNESDB_HAVE_ART
                if (aa_cache.inspected < pf_idx.album_ct)
                {
                    buf_ctx_lock();
                    incremental_albumart_cache(false);
                    buf_ctx_unlock();
                }
#endif
                break;
        }

        /* Calculate FPS */
        if (current_update - last_update > update_interval) {
            fps = frames * HZ / (current_update - last_update);
            last_update = current_update;
            frames = 0;
        }
        /* Draw FPS or draw percentage of already built album cache; not the
           percentage in Cover Flow, whose covers just come */
#ifdef PF_SHOW_ON_COP
        struct viewport *const frame_vp = drawn ? draw_in_frame() : NULL;
#endif
#ifdef ITUNESDB_HAVE_ART
        if (drawn && pf_cfg.show_fps)
#else
        if (drawn && (pf_cfg.show_fps || aa_cache.inspected < pf_idx.album_ct))
#endif
        {
#ifdef USEGSLIB
            mylcd_set_foreground(G_BRIGHT(255));
#else
            mylcd_set_foreground(G_PIX(255,0,0));
#endif
#ifdef ITUNESDB_HAVE_ART
            /* the frames always: how far the covers are looked through is
               nothing to show */
            rb->snprintf(fpstxt, sizeof(fpstxt), "FPS: %d", fps);
#else
            if(aa_cache.inspected >= pf_idx.album_ct)
                 rb->snprintf(fpstxt, sizeof(fpstxt), "FPS: %d", fps);
            else
            {
                int progress_pct = 100 * aa_cache.inspected / pf_idx.album_ct;
                rb->snprintf(fpstxt, sizeof(fpstxt), "%d %%", progress_pct);
            }
#endif

            if (pf_cfg.show_album_name == ALBUM_NAME_TOP ||
                pf_cfg.show_album_name == ALBUM_AND_ARTIST_TOP)
                fpstxt_y = LCD_HEIGHT -
                           rb->screens[SCREEN_MAIN]->getcharheight();
            else
                fpstxt_y = 0;
            mylcd_putsxy(0, fpstxt_y, fpstxt);
        }
        if (drawn)
        {
            draw_album_text();
#ifdef PF_SHOW_ON_COP
            if (frame_vp)
                rb->lcd_set_viewport(frame_vp);
#endif

            /* the others' turn as the frame is drawn: the codec's on this core,
               so that the COP is through with its part by the next one, and the
               slide thread's, which loads the covers that come into view */
            rb->yield();
            /* and the frame on the screen */
#ifdef PF_SHOW_ON_COP
            if (show_on_cop)
            {
                /* the COP shows it as the next is drawn in the other */
                shown_later = buffer;
                buffer = buffer == LCD_BUF ? frame_other : LCD_BUF;
            }
            else
#endif
            if (is_initial_slide == false)
                mylcd_update();
        }
#if CARD_FRAME_COUNT > 0
        else
            prepare_ahead(); /* between frames, still */
#endif
        rb->yield();

        /*/ Handle buttons */
        button = rb->get_custom_action(CONTEXT_PLUGIN
#ifndef USE_CORE_PREVNEXT
            |(pf_state == pf_show_tracks ? 1 : 0)
#endif
            ,instant_update ? 0 : wheel_touched() ? 1 : HZ/16,
            get_context_map);
        if (button != ACTION_NONE)
            idle_shown = false;
        if (button != ACTION_NONE && !IS_SYSEVENT(button))
            used_at = *rb->current_tick;
#ifdef SYS_SLEEP_FLUSH
        else if (button == SYS_SLEEP_FLUSH) /* the iPod falls asleep */
        {
            cf_marks.sleeps++;
            cf_marks.sleep_after = *rb->current_tick - last_use();
        }
#endif
#ifdef HAVE_WHEEL_POSITION
        /* a button may open a menu or a question, which go by the wheel's
           events: they're back until the next frame of the covers */
        if (button != ACTION_NONE && button != PF_NEXT && button != PF_PREV
            && button != PF_NEXT_REPEAT && button != PF_PREV_REPEAT)
            wheel_events_on(true);
#endif

        switch (button) {
#ifdef ITUNESDB_HAVE_ART
        case ACTION_LIST_PLAYPAUSE:
            /* Play pauses the music and goes on with it, as anywhere, while
               the settings keep it playing in here; with none, as after a
               restart, it goes on where it was, as Resume Playback does */
            if (rb->global_settings->coverflow_pause)
                break;
            if (rb->audio_status() & AUDIO_STATUS_PLAY)
            {
                if (rb->audio_status() & AUDIO_STATUS_PAUSE)
                    rb->audio_resume();
                else
                    rb->audio_pause();
            }
            else if (rb->global_status->resume_index != -1
                     && (drop_frame(), rb->playlist_resume() != -1))
                rb->playlist_resume_track(rb->global_status->resume_index,
                                          rb->global_status->resume_crc32,
                                          rb->global_status->resume_elapsed,
                                          rb->global_status->resume_offset);
            break;
        case ACTION_TREE_STOP:
            /* held, to Now Playing if it has a song, as from the lists:
               stopped, it shows it paused and starts nothing; the core's
               now_playing_available() */
            if (rb->audio_status() || rb->global_status->resume_index != -1
                || rb->file_exists(PLAYLIST_CONTROL_FILE))
                return PLUGIN_GOTO_WPS;
            break;
#endif
        case PF_WPS:
            return PLUGIN_GOTO_WPS;
        case PF_QUIT:
#ifndef ITUNESDB_HAVE_ART
            return PLUGIN_OK;
#endif
            /* Menu, a level back as everywhere on an iPod: from an album's
               tracks to the covers, and only from those out */
            /* fallthrough */
        case PF_BACK:
            if (show_tracks_while_browsing)
                show_tracks_while_browsing = false;
            else if (pf_state == pf_show_tracks)
                start_cover_out();
            else if (pf_state == pf_cover_in)
                reverse_animation();
            else if (pf_state == pf_cover_out)
                skip_animation_to_idle_state();
            else if (pf_state == pf_idle || pf_state == pf_scrolling)
                return PLUGIN_OK;
            break;
        case PF_MENU:
#ifdef ITUNESDB_HAVE_ART
            return PLUGIN_GOTO_ROOT; /* Menu held, as everywhere */
#endif
            drop_frame();
#ifdef USEGSLIB
            grey_show(false);
#endif
            FOR_NB_SCREENS(i)
                rb->viewportmanager_theme_enable(i, true, NULL);
            pf_boost(false); /* a menu, as anywhere */
            ret = main_menu();
            FOR_NB_SCREENS(i)
                rb->viewportmanager_theme_undo(i, false);
            wheel_clicks_taken();

            if (ret == -3)
            {
                if (!reinit())
                    return PLUGIN_OK;
            }
            else if (ret == -2)
                return PLUGIN_GOTO_WPS;
            else if (ret == -1)
                return PLUGIN_OK;
            else if (ret != 0 )
                return ret;
            else
            {
#ifdef USEGSLIB
                grey_show(true);
#endif
                mylcd_set_drawmode(DRMODE_FG);
            }
            break;

        case PF_NEXT:
        case PF_NEXT_REPEAT:
            /* the click wheel's steps through the covers come from its
               readings (watch_wheel()) */
            if ((pf_state == pf_idle || pf_state == pf_scrolling)
                && wheel_stepped())
                break;
            if ( pf_state == pf_show_tracks )
            {
                if (show_tracks_while_browsing)
                    select_next_album();
                else
                    select_next_track();
            }
            else if (pf_state == pf_cover_in)
                skip_animation_to_show_tracks();
            else if (pf_state == pf_cover_out)
            {
                skip_animation_to_idle_state();
                /* this step stands for the clicks so far */
                wheel_clicks_taken();
            }

            if ( pf_state == pf_idle || pf_state == pf_scrolling )
                show_next_slide();
            break;

        case PF_PREV:
        case PF_PREV_REPEAT:
            if ((pf_state == pf_idle || pf_state == pf_scrolling)
                && wheel_stepped())
                break;
            if ( pf_state == pf_show_tracks )
            {
                if (show_tracks_while_browsing)
                    select_prev_album();
                else
                    select_prev_track();
            }
            else if (pf_state == pf_cover_in)
                skip_animation_to_show_tracks();
            else if (pf_state == pf_cover_out)
            {
                skip_animation_to_idle_state();
                wheel_clicks_taken();
            }

            if ( pf_state == pf_idle || pf_state == pf_scrolling )
                show_previous_slide();
            break;
        case PF_SORTING_NEXT:
            sort_albums((pf_cfg.sort_albums_by + 1) % SORT_VALUES_SIZE, false);
            break;
        case PF_SORTING_PREV:
            sort_albums((pf_cfg.sort_albums_by + (SORT_VALUES_SIZE - 1)) % SORT_VALUES_SIZE, false);
            break;
        /* Next and Previous go an album at a time, as on a stock iPod; with
           the tracks shown, to the next album's */
        case PF_ALBUM_NEXT:
            if (pf_state == pf_idle || pf_state == pf_scrolling)
                show_next_slide();
            else if ( pf_state == pf_show_tracks )
                select_next_album();
            break;
        /* held: on to the first album or the last, quickly */
        case PF_ALBUM_FIRST:
        case PF_ALBUM_LAST:
            if (pf_state == pf_idle || pf_state == pf_scrolling)
            {
                const int to = button == PF_ALBUM_FIRST ? 0 : number_of_slides - 1;
                const int way = button == PF_ALBUM_FIRST ? -1 : 1;
                if (to != center_index && !(seeking && target == to))
                {
                    roll_speed = 0;
                    roll_settling = false;
                    if (pf_state == pf_idle || step != way)
                    {
                        if (way > 0)
                            show_next_slide();
                        else
                            show_previous_slide();
                    }
                    target = to;
                    seeking = true;
                    seek_cap = 65536;
                }
            }
            break;
        case PF_ALBUM_PREV:
            if (pf_state == pf_idle || pf_state == pf_scrolling)
                show_previous_slide();
            else if ( pf_state == pf_show_tracks )
                select_prev_album();
            else if (pf_state == pf_cover_in)
                reverse_animation();
            else if (pf_state == pf_cover_out)
                skip_animation_to_idle_state();
            break;
#if PF_PLAYBACK_CAPABLE
        case PF_CONTEXT:
            if (pf_state == pf_idle || pf_state == pf_scrolling ||
                pf_state == pf_show_tracks || pf_state == pf_cover_out)
            {
                if ( pf_state == pf_scrolling)
                {
                    set_current_slide(target);
                    pf_state = pf_idle;
                }
                else if (pf_state == pf_cover_out)
                    skip_animation_to_idle_state();

                drop_frame();
                pf_boost(false); /* a menu, as anywhere */
                if (context_menu_ready())
                {
                    ret = context_menu();
                    context_menu_cleanup();
                    wheel_clicks_taken();
                    if (ret == PF_LEAVE_FOR_LISTS)
                        return PLUGIN_OK;
                    if ( ret != 0 ) return ret;
                }
                else if (!prompt_reinit())
                    return PLUGIN_OK;
            }
            break;
#endif
        case PF_TRACKLIST:
        case PF_SELECT:
            if (pf_state == pf_idle || pf_state == pf_scrolling)
            {
                if (pf_state == pf_scrolling)
                    set_current_slide(target);
#if PF_PLAYBACK_CAPABLE
                if(pf_cfg.auto_wps == 1 && button == PF_SELECT)
                {
                    if (start_playback(true))
                        return PLUGIN_GOTO_WPS;
                }
                else
#endif
                {
#if CARD_FRAME_COUNT > 0
                    /* the album's tracks and its card before it moves, unless
                       the covers' rest made them (prepare_ahead()): where the
                       cover turns to its edge, finding them held it up there.
                       No slide loads while it turns. */
                    pf_boost(true);
                    const int idx = center_slide.slide_index;
                    const unsigned long start = anim_now();
                    tracks_lock();
                    cf_marks.opened++;
                    if (card_drawn && card_album == idx && pf_tracks.cur_idx == idx)
                    {
                        cf_marks.ready++;
                        /* the chosen track's name from where the card has it */
                        restart_scroll_line(PF_SCROLL_TRACK);
                    }
                    else
                    {
                        make_card();
                        const unsigned long took = anim_now() - start;
                        cf_marks.made_all += took;
                        cf_marks.made_most = MAX(cf_marks.made_most, took);
                    }
#endif
                    pf_state = pf_cover_in;
                }
            }
            else if (pf_state == pf_cover_out)
                reverse_animation();
            else if (pf_state == pf_cover_in)
                skip_animation_to_show_tracks();
            else if (pf_state == pf_show_tracks)
            {
                if (show_tracks_while_browsing)
                    show_tracks_while_browsing = false;
#if PF_PLAYBACK_CAPABLE
                else if(pf_cfg.auto_wps != 0) {
                    if (start_playback(true))
                        return PLUGIN_GOTO_WPS;
                }
                else
                    start_playback(false);
#endif
            }
            break;
        default:
            exit_on_usb(button);
            break;
        }
    }
}

/*************************** Plugin entry point ****************************/

#ifdef ITUNESDB_HAVE_ART
/**
  At the end of an import, on its screen: only the album list, so that Cover
  Flow opens at once. If it fails or Menu leaves it, Cover Flow makes it when
  it opens, as for any list older than the database.
 */
/** The covers of the albums of songs copied outside Finder's folder, made
    here on Updating Library in Finder's forms (itunesdb.h): of Finder's cover
    of a song of the album, or else a JPEG embedded in one outside the folder
    or an image in its folder, as the album art setting prefers **/

#ifdef HAVE_TC_RAMCACHE /* the albums are found in the database in memory */
#define COVER_FRAME 200 /* the largest, as Finder's */

struct cover_job {
    uint64_t key;       /* the album's */
    uint64_t song;      /* the song's path's itunesdb_path_hash() */
    int32_t track;      /* a song of it outside Finder's folder */
    int32_t finder;     /* one of Finder's with a cover, -1 for none: in an
                           album's first job */
    uint32_t stamp;     /* of the song outside, its path and time */
    int32_t album;      /* where its album's and album artist's names are */
    int32_t artist;
};

/* by album, and in it by song: its songs in the same order each time, the
   first with a picture shown giving the album its cover (prepare_covers()),
   rather than whichever qsort left first, and the album made again for
   another */
static int cmp_cover_job(const void *a, const void *b)
{
    const struct cover_job *x = a, *y = b;
    if (x->key != y->key)
        return (x->key > y->key) - (x->key < y->key);
    return (x->track > y->track) - (x->track < y->track);
}

static uint32_t text_stamp(const char *s, uint32_t h)
{
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

/* the album key of the song, with tcs searching */
static uint64_t song_album_key(int idx_id, const char *path)
{
    static char album[TAGCACHE_BUFSZ], artist[TAGCACHE_BUFSZ];
    if (!rb->tagcache_retrieve(&tcs, idx_id, tag_album, album, sizeof(album)))
        album[0] = '\0';
    if (!rb->tagcache_retrieve(&tcs, idx_id, tag_albumartist, artist,
                               sizeof(artist)))
        artist[0] = '\0';
    return rb->itunesdb_album_key(album, artist, path);
}

struct cover_jobs {
    struct cover_job *jobs;
    int count, room;
    /* the jobs by where their names are, for Finder's songs of their albums */
    int32_t *by_names;
    int seen;
};

/* each song outside Finder's folder, a job of its album's */
static void collect_song(int idx_id, const int32_t *seek, void *data)
{
    struct cover_jobs *c = data;
    static char path[MAX_PATH];
    if (!(++c->seen & 255))
        rb->yield();
    if (c->count >= c->room
        || !rb->tagcache_retrieve(&tcs, idx_id, tag_filename, path, sizeof(path))
        || rb->itunesdb_synced_path(path))
        return;
    struct cover_job *job = &c->jobs[c->count++];
    job->key = song_album_key(idx_id, path);
    job->song = rb->itunesdb_path_hash(path);
    job->track = idx_id;
    job->finder = -1;
    job->stamp = text_stamp(path, 2166136261u) ^ (uint32_t)seek[tag_mtime];
    job->album = seek[tag_album];
    job->artist = seek[tag_albumartist];
}

static int cmp_names(const struct cover_job *x, int32_t album, int32_t artist)
{
    if (x->album != album)
        return (x->album > album) - (x->album < album);
    return (x->artist > artist) - (x->artist < artist);
}

static struct cover_jobs *sort_jobs; /* for qsort, which has no data */
static int cmp_by_names(const void *a, const void *b)
{
    const struct cover_job *x = &sort_jobs->jobs[*(const int32_t *)a];
    const struct cover_job *y = &sort_jobs->jobs[*(const int32_t *)b];
    return cmp_names(x, y->album, y->artist);
}

/* For an album of songs both synced and copied, Finder's cover of one of
   them, which is all the same then: a song of Finder's whose album and
   artist are those of a job's, by where their names are, with no name read
   but for those */
static void find_finder_song(int idx_id, const int32_t *seek, void *data)
{
    struct cover_jobs *c = data;
    static char path[MAX_PATH];
    if (!(++c->seen & 255))
        rb->yield();
    int lo = 0, hi = c->count - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        struct cover_job *job = &c->jobs[c->by_names[mid]];
        const int cmp = cmp_names(job, seek[tag_album], seek[tag_albumartist]);
        if (cmp == 0)
        {
            struct itunesdb_art art;
            if (job->finder < 0
                && rb->tagcache_retrieve(&tcs, idx_id, tag_filename, path,
                                         sizeof(path))
                && rb->itunesdb_synced_path(path)
                /* the same album, as one without a name is its folder */
                && song_album_key(idx_id, path) == job->key
                && rb->itunesdb_find_art(path, &art))
            {
                job->finder = idx_id;
                /* its colour too: Finder writes another cover in a place */
                job->stamp = text_stamp(art.path, art.offset) ^ art.colour;
            }
            return;
        }
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
}

/* The songs outside Finder's folder by album, from the database in memory,
   and how many albums they are: with none, that's all there is to do. Album
   i's songs are jobs from (*starts)[i] up to (*starts)[i + 1], in the room
   after them. -1 without the database in memory; *all false when there
   wasn't room for every song; *tracks the database's songs. */
static int collect_cover_jobs(struct cover_job *jobs, int room, bool *all,
                              int *tracks, int32_t **starts)
{
    /* each song, and for its album, if it's one's first, where it starts and
       its place by name */
    const int most = room * sizeof(*jobs)
                     / (sizeof(*jobs) + 2 * sizeof(int32_t)) - 1;
    struct cover_jobs c = { jobs, 0, most, NULL, 0 };
    if (most <= 0 || tcs.valid || !rb->tagcache_search(&tcs, tag_filename))
        return -1;
    if (!rb->tagcache_for_each_track(collect_song, &c))
    {
        rb->tagcache_search_finish(&tcs);
        return -1;
    }
    *all = c.count < most;
    *tracks = c.seen;
    int albums = 0;
    int32_t *start = (int32_t *)&jobs[c.count];
    if (c.count > 0)
    {
        rb->qsort(jobs, c.count, sizeof(*jobs), cmp_cover_job);
        for (int i = 0; i < c.count; i++)
        {
            if (i == 0 || jobs[i - 1].key != jobs[i].key)
                start[albums++] = i;
        }
    }
    start[albums] = c.count;
    *starts = start;
    if (albums > 0)
    {
        /* each album's first job by its names */
        c.by_names = start + albums + 1;
        for (int i = 0; i < albums; i++)
            c.by_names[i] = start[i];
        c.count = albums;
        sort_jobs = &c;
        rb->qsort(c.by_names, albums, sizeof(int32_t), cmp_by_names);
        rb->tagcache_for_each_track(find_finder_song, &c);
    }
    rb->tagcache_search_finish(&tcs);
    return albums;
}

/* the path of a song of the database */
static bool song_path(int32_t track, char *path, size_t size)
{
    if (tcs.valid || !rb->tagcache_search(&tcs, tag_filename))
        return false;
    bool ok = rb->tagcache_retrieve(&tcs, track, tag_filename, path, size);
    rb->tagcache_search_finish(&tcs);
    return ok;
}

/* cover.png or folder.png in the folder of the song at path, which the
   core's search, for the images it decodes, passes over */
static bool folder_png(const char *path, char *image, size_t size)
{
    static const char * const names[] = { "cover.png", "folder.png" };
    const char *slash = rb->strrchr(path, '/');
    if (!slash)
        return false;
    for (unsigned i = 0; i < ARRAYLEN(names); i++)
    {
        rb->snprintf(image, size, "%.*s/%s", (int)(slash - path), path,
                     names[i]);
        if (rb->file_exists(image))
            return true;
    }
    return false;
}

/* the stamp of the image file at path: its name, size and time, as a cover
   saved again changes them; 0 for none */
static uint32_t image_stamp(const char *path)
{
    static char dir_path[MAX_PATH];
    const char *slash = rb->strrchr(path, '/');
    if (!slash)
        return 0;
    rb->snprintf(dir_path, sizeof(dir_path), "%.*s",
                 (int)MAX(slash - path, 1), path);
    DIR *dir = rb->opendir(dir_path);
    if (!dir)
        return 0;
    uint32_t stamp = 0;
    struct dirent *e;
    while ((e = rb->readdir(dir)) != NULL)
    {
        if (rb->strcasecmp(e->d_name, slash + 1))
            continue;
        struct dirinfo info = rb->dir_get_info(dir, e);
        stamp = text_stamp(path, 2166136261u) ^ (uint32_t)info.size
                ^ ((uint32_t)info.mtime * 40503u);
        stamp += !stamp; /* not 0, which is none */
        break;
    }
    rb->closedir(dir);
    return stamp;
}

/** Covers the core's decoder can't read, a PNG or a progressive JPEG, aren't
    made: they decode whole, and on the iPod one of 1000 by 1000 took 8 s, or
    13 s for a PNG, against 1.5 s for a standard JPEG, and one of 600 by 600
    still 3.5 s or 5 s. A library of a thousand albums would have taken an
    hour. Their songs are known by why, and Updating Library says how many
    albums have one, to be saved as a standard JPEG. **/
static int covers_unshown;

/* why a picture at pos of the file isn't decoded, by its header: a PNG, a
   progressive JPEG, or another the decoder turned down */
static int picture_kind(const char *file, off_t pos)
{
    static const unsigned char png_sig[8] =
        { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    unsigned char b[8];
    int kind = ITUNESDB_COVER_UNREADABLE;
    int fd = rb->open(file, O_RDONLY);
    if (fd < 0)
        return kind;
    if (rb->lseek(fd, pos, SEEK_SET) == pos
        && rb->read(fd, b, sizeof(b)) == (ssize_t)sizeof(b))
    {
        if (!rb->memcmp(b, png_sig, sizeof(png_sig)))
            kind = ITUNESDB_COVER_PNG;
        else if (b[0] == 0xff && b[1] == 0xd8)
        {
            /* its frame's marker, past the others: SOF2 for progressive */
            off_t at = pos + 2;
            for (int n = 0; n < 64; n++)
            {
                unsigned char m[4];
                if (rb->lseek(fd, at, SEEK_SET) != at
                    || rb->read(fd, m, sizeof(m)) != (ssize_t)sizeof(m)
                    || m[0] != 0xff)
                    break;
                if (m[1] == 0xff)
                {
                    at++; /* a fill byte */
                    continue;
                }
                const int len = (m[2] << 8) | m[3];
                if (m[1] == 0xc2)
                {
                    kind = ITUNESDB_COVER_PROGRESSIVE;
                    break;
                }
                /* another frame, the core's, or the scan */
                if ((m[1] >= 0xc0 && m[1] <= 0xcf && m[1] != 0xc4
                     && m[1] != 0xc8 && m[1] != 0xcc) || m[1] == 0xda
                    || len < 2)
                    break;
                at += 2 + len;
            }
        }
    }
    rb->close(fd);
    return kind;
}

/** Each song's own picture, made on Updating Library in Finder's sizes for
    every song Finder has none of: the picture in the song first, then its
    folder's image. Each picture is decoded once, at 240 px, the 200 px frame
    and the slide made of it, however many songs have it: those after the
    first are known by their bytes' sums. **/

/* a song to make, by its number in the database */
struct song_job {
    int32_t track;
    uint32_t mtime;
    bool images;    /* known, but its folder's image may give it another */
#ifdef ITUNESDB_FINDER_LARGE
    bool finder;    /* Finder has its cover: only the pane's picture made */
#endif
};

struct song_jobs {
    struct song_job *jobs;
    int count, room;
    int seen;
};

/* each song Finder has no cover of, a job if it isn't known as it is */
static void collect_song_job(int idx_id, const int32_t *seek, void *data)
{
    struct song_jobs *c = data;
    static char path[MAX_PATH];
    struct itunesdb_art art;
    if (!(++c->seen & 255))
        rb->yield();
#ifdef ITUNESDB_FINDER_LARGE
    if (c->count >= c->room
        || !rb->tagcache_retrieve(&tcs, idx_id, tag_filename, path, sizeof(path)))
        return;
    const uint32_t mtime = seek[tag_mtime];
    if (rb->itunesdb_find_art(path, &art))
    {
        /* the pane's picture of Finder's cover, if not known as it is */
        if (rb->itunesdb_finder_large_known(rb->itunesdb_path_hash(path),
                                            mtime, &art) != 1)
        {
            struct song_job *job = &c->jobs[c->count++];
            job->track = idx_id;
            job->mtime = mtime;
            job->images = false;
            job->finder = true;
        }
        return;
    }
#else
    if (c->count >= c->room
        || !rb->tagcache_retrieve(&tcs, idx_id, tag_filename, path, sizeof(path))
        || rb->itunesdb_find_art(path, &art))
        return;
    const uint32_t mtime = seek[tag_mtime];
#endif
    const int known = rb->itunesdb_song_known(rb->itunesdb_path_hash(path),
                                              mtime, ~0u);
    if (known == 1)
        return;
    struct song_job *job = &c->jobs[c->count++];
    job->track = idx_id;
    job->mtime = mtime;
    job->images = known == 2;
#ifdef ITUNESDB_FINDER_LARGE
    job->finder = false;
#endif
}

/* the stamp of the folder's image the song would have now, by its tags in the
   database, 0 for none */
static uint32_t folder_source(int32_t track, const char *path)
{
    static char album[TAGCACHE_BUFSZ], artist[TAGCACHE_BUFSZ];
    static char image[MAX_PATH];
    static struct mp3entry tags;
    rb->memset(&tags, 0, sizeof(tags));
    rb->strlcpy(tags.path, path, sizeof(tags.path));
    if (!tcs.valid && rb->tagcache_search(&tcs, tag_filename))
    {
        if (rb->tagcache_retrieve(&tcs, track, tag_album, album, sizeof(album)))
            tags.album = album;
        if (rb->tagcache_retrieve(&tcs, track, tag_artist, artist,
                                  sizeof(artist)))
            tags.artist = artist;
        rb->tagcache_search_finish(&tcs);
    }
    if (search_albumart_files(&tags, ":", image, sizeof(image))
        || folder_png(path, image, sizeof(image)))
        return image_stamp(image);
    return 0;
}

/* the album key of the song, as Cover Flow's albums have it */
static uint64_t song_album(int32_t track, const char *path)
{
    if (tcs.valid || !rb->tagcache_search(&tcs, tag_filename))
        return 0;
    const uint64_t key = song_album_key(track, path);
    rb->tagcache_search_finish(&tcs);
    return key;
}

/* the sums of a picture's bytes, read through buf */
static bool sum_picture(const char *file, off_t pos, uint32_t size, void *buf,
                        size_t buf_size, uint32_t *crc, uint32_t *adler)
{
    int fd = rb->open(file, O_RDONLY);
    bool ok = fd >= 0 && size > 0 && rb->lseek(fd, pos, SEEK_SET) == pos;
    *crc = COVERS_CRC32_INIT;
    *adler = COVERS_ADLER32_INIT;
    while (ok && size > 0)
    {
        const size_t n = MIN(size, buf_size);
        ok = rb->read(fd, buf, n) == (ssize_t)n;
        if (ok)
        {
            *crc = rb->covers_crc32(*crc, buf, n);
            *adler = rb->covers_adler32(*adler, buf, n);
        }
        size -= n;
        rb->yield();
    }
    if (fd >= 0)
        rb->close(fd);
    return ok;
}

/* what the step took, for the health log: the decodes, and the reads of the
   pictures' bytes */
static struct {
    long started;
    int songs, pictures, sums, known;
    long decode_ticks, sum_ticks, decode_most;
    int noted;
#ifdef ITUNESDB_FINDER_LARGE
    /* the pane's pictures of Finder's songs */
    int finder_songs, finder_pictures;
    long finder_ticks;
#endif
} covers_time;
#define COVERS_NOTED 12 /* pictures with a line each, the first ones */

/* A picture tried for the song: its sums, and a decode at 240 px if no song
   had it before. 1 made, or known; -1 no room left; 0 not decoded. */
static int song_picture(struct itunesdb_song_cover *song, const char *file,
                        off_t pos, uint32_t size, uint32_t source,
                        const struct mp3_albumart *aa, void *buf,
                        size_t buf_size)
{
    song->pic_pos = pos;
    song->pic_size = size;
    song->source = source;
    long t = *rb->current_tick;
    if (!sum_picture(file, pos, size, buf, buf_size, &song->crc, &song->adler))
        return 0;
    covers_time.sum_ticks += *rb->current_tick - t;
    covers_time.sums++;
    if (rb->itunesdb_song_picture(song))
    {
        covers_time.known++;
        return 1;
    }
    /* said first, so that a picture which crashes the decoder or hangs it
       is given up rather than tried at every boot */
    rb->itunesdb_song_making(song);
    struct bitmap bm = { .width = ITUNESDB_LARGE_SIDE,
                         .height = ITUNESDB_LARGE_SIDE, .data = buf };
    const int format = FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT;
    t = *rb->current_tick;
    const int got = aa ? read_embedded_jpeg(file, aa, &bm, buf_size, format, NULL)
                       : read_image_file(file, &bm, buf_size, format, NULL);
    t = *rb->current_tick - t;
    covers_time.decode_ticks += t;
    covers_time.decode_most = MAX(covers_time.decode_most, t);
    covers_time.pictures++;
    if (covers_time.noted < COVERS_NOTED)
    {
        covers_time.noted++;
        rb->health_note("covers: a picture of %lu KB, %s, decoded in %ld ms%s",
                        (unsigned long)(size / 1024), aa ? "embedded" : "a file",
                        t * 1000 / HZ, got > 0 ? "" : ", not shown");
    }
    if (got <= 0)
        return 0;
    const int made = rb->itunesdb_song_put(song, &bm, ITUNESDB_COVER_SHOWN);
    return made < 0 ? -1 : 1;
}

/* the song's cover made, of its own picture or its folder's, or known why it
   has none: -1 for no room left on the disk, which ends the making */
static int make_song(const struct song_job *job, void *buf, size_t buf_size)
{
    static char path[MAX_PATH], image[MAX_PATH];
    if (!song_path(job->track, path, sizeof(path)))
        return 0;
    struct itunesdb_song_cover song;
    rb->memset(&song, 0, sizeof(song));
    song.path = rb->itunesdb_path_hash(path);
    song.mtime = job->mtime;
    /* known but for its folder, whose image it would have is the one it has */
    if (job->images
        && rb->itunesdb_song_known(song.path, job->mtime,
                                   folder_source(job->track, path)) == 1)
        return 1;
    /* the tags as playback reads them, which a changed file differs by */
    if (!rb->get_metadata(&id3, -1, path))
        return 0;
    covers_time.songs++;
    const bool own = id3.has_embedded_albumart;
    song.size = id3.filesize;
    song.check = rb->covers_file_check(id3.filesize,
                                       own ? id3.albumart.pos : 0,
                                       own ? id3.albumart.size : 0);
    song.album = song_album(job->track, path);
    song.album_stamp = text_stamp(path, 2166136261u) ^ job->mtime;
    int reason = ITUNESDB_COVER_SHOWN, made;
    if (own)
    {
        const struct mp3_albumart aa = id3.albumart;
        if ((aa.type & AA_CLEAR_FLAGS_MASK) == AA_TYPE_JPG)
        {
            made = song_picture(&song, path, aa.pos, aa.size, 0, &aa, buf,
                                buf_size);
            if (made)
                return made;
            reason = picture_kind(path, aa.pos);
        }
        else
            reason = (aa.type & AA_CLEAR_FLAGS_MASK) == AA_TYPE_PNG
                     ? ITUNESDB_COVER_PNG : ITUNESDB_COVER_UNREADABLE;
    }
    /* the folder's image, the core's search's */
    if (search_albumart_files(&id3, ":", image, sizeof(image)))
    {
        const int fd = rb->open(image, O_RDONLY);
        const off_t size = fd >= 0 ? rb->ffilesize(fd) : 0;
        if (fd >= 0)
            rb->close(fd);
        made = size > 0 ? song_picture(&song, image, 0, size,
                                       image_stamp(image), NULL, buf, buf_size)
                        : 0;
        if (made)
            return made;
        if (!reason)
            reason = picture_kind(image, 0);
    }
    /* a PNG beside it, which the search passes over */
    else if (!reason && folder_png(path, image, sizeof(image)))
    {
        reason = ITUNESDB_COVER_PNG;
        song.source = image_stamp(image);
    }
    return rb->itunesdb_song_put(&song, NULL, reason) < 0 ? -1 : 1;
}

#ifdef ITUNESDB_FINDER_LARGE
/* The pane's picture of a song Finder has the cover of: its own picture,
   decoded at the pane's size if no song had it before, and kept if it is
   Finder's cover. -1 for no room left on the disk, which ends the making. */
static int make_finder_song(const struct song_job *job, void *buf,
                            size_t buf_size)
{
    static char path[MAX_PATH];
    struct itunesdb_art art;
    if (!song_path(job->track, path, sizeof(path))
        || !rb->itunesdb_find_art(path, &art)
        || !rb->get_metadata(&id3, -1, path))
        return 0;
    covers_time.finder_songs++;
    struct itunesdb_finder_song song;
    rb->memset(&song, 0, sizeof(song));
    song.path = rb->itunesdb_path_hash(path);
    song.mtime = job->mtime;
    const bool own = id3.has_embedded_albumart
        && (id3.albumart.type & AA_CLEAR_FLAGS_MASK) == AA_TYPE_JPG;
    song.check = rb->covers_file_check(id3.filesize,
                                       own ? id3.albumart.pos : 0,
                                       own ? id3.albumart.size : 0);
    /* only the song's own picture, which Finder's cover is made of */
    if (!own)
        return rb->itunesdb_finder_large_put(&song, &art, NULL) < 0 ? -1 : 1;
    const struct mp3_albumart aa = id3.albumart;
    song.pic_size = aa.size;
    long t = *rb->current_tick;
    if (!sum_picture(path, aa.pos, aa.size, buf, buf_size, &song.crc,
                     &song.adler))
        return 0;
    covers_time.sum_ticks += *rb->current_tick - t;
    covers_time.sums++;
    const int known = rb->itunesdb_finder_large_picture(&song, &art);
    if (known)
        return 1;
    /* said first, as a song's own: a picture that crashes the decoder or
       hangs it is given up */
    struct itunesdb_song_cover making;
    rb->memset(&making, 0, sizeof(making));
    making.path = song.path;
    making.mtime = song.mtime;
    rb->itunesdb_song_making(&making);
    struct bitmap bm = { .width = ITUNESDB_LARGE_SIDE,
                         .height = ITUNESDB_LARGE_SIDE, .data = buf };
    const int format = FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT;
    t = *rb->current_tick;
    const int got = read_embedded_jpeg(path, &aa, &bm, buf_size, format, NULL);
    t = *rb->current_tick - t;
    covers_time.finder_ticks += t;
    covers_time.decode_most = MAX(covers_time.decode_most, t);
    covers_time.finder_pictures++;
    return rb->itunesdb_finder_large_put(&song, &art, got > 0 ? &bm : NULL) < 0
           ? -1 : 1;
}
#endif

/* the album's cover of Finder's song of it, made into buf:
   itunesdb_cover_put()'s answer */
static int make_finder_cover(const struct cover_job *job, void *buf,
                             size_t buf_size)
{
    static char path[MAX_PATH];
    struct bitmap bm = { .width = COVER_FRAME, .height = COVER_FRAME,
                         .data = buf };
    int size = -1;
    struct itunesdb_art art;
    if (song_path(job->finder, path, sizeof(path))
        && rb->itunesdb_find_art(path, &art))
    {
        bm.width = MIN(art.width, COVER_FRAME);
        bm.height = MIN(art.height, COVER_FRAME);
        size = rb->itunesdb_load_art(&art, &bm, buf_size);
    }
    return rb->itunesdb_cover_put(job->key, job->stamp, size > 0 ? &bm : NULL);
}

/* the covers said to be cut short, made at the next boot, the album list
   with them */
static void covers_later(void)
{
    if (rb->itunesdb_covers_begin(0, 0, false))
        rb->itunesdb_covers_end(COVERS_CUT);
}

/* How many of the albums have a cover in a format that isn't shown, and what
   to do: until a button is pressed, or some seconds. USB and switching off
   are answered once Updating Library is over, as during the covers. */
static void show_unshown(void)
{
    rb->splashf(0, rb->str(LANG_COVERS_UNSHOWN), covers_unshown);
    const long until = *rb->current_tick + 5 * HZ;
    while (TIME_BEFORE(*rb->current_tick, until))
    {
        const long button = rb->button_get_w_tmo(HZ / 10);
        if (button == SYS_USB_CONNECTED || button == SYS_POWEROFF
            || button == SYS_REBOOT)
        {
            rb->library_update_hold(button);
            break;
        }
        if (IS_SYSEVENT(button))
            rb->default_event_handler(button);
        /* a press, not Menu still held from leaving the covers, nor a turn
           of the wheel */
        else if (button != BUTTON_NONE
                 && !(button & (BUTTON_REL | BUTTON_REPEAT))
#ifdef BUTTON_SCROLL_FWD
                 && !(button & (BUTTON_SCROLL_FWD | BUTTON_SCROLL_BACK))
#endif
                )
            break;
    }
}

/* Menu, USB or switching off, between two covers: true to stop. USB and
   switching off are answered once Updating Library is over. */
static bool covers_stop(bool *held)
{
    const long button = rb->button_get(false);
    if (button == SYS_USB_CONNECTED || button == SYS_POWEROFF
        || button == SYS_REBOOT)
    {
        rb->library_update_hold(button);
        *held = true;
        return true;
    }
    else if (IS_SYSEVENT(button))
        rb->default_event_handler(button); /* as the headphones' */
#ifdef BUTTON_MENU
    if (button == BUTTON_MENU)
        return true;
#endif
    return false;
}

/* Each song's and each album's made once, and again when what it's made of
   changes. Menu leaves the rest to the next Updating Library. USB and
   switching off stop it too, and are answered once Updating Library is over,
   as for its fonts: answered here, the steps after it went on with what was
   before USB. */
static void prepare_covers(void)
{
    /* the jobs, and room for a picture and the decoder after them */
    const size_t decode_size = 0x40000;
    ALIGN_BUFFER(pf_idx.buf, pf_idx.buf_sz, sizeof(uint64_t));
    if (pf_idx.buf_sz < decode_size + 64 * sizeof(struct cover_job))
    {
        /* none for this database rather than Updating Library at each boot */
        rb->itunesdb_covers_skip();
        return;
    }
    struct cover_job *jobs = pf_idx.buf;
    const int room = (pf_idx.buf_sz - decode_size) / sizeof(*jobs);
    void *decode = jobs + room;
    bool all = true;
    int tracks = 0;
    int32_t *starts = NULL;
    const int albums = collect_cover_jobs(jobs, room, &all, &tracks, &starts);
    const bool images_changed = rb->itunesdb_covers_images_changed();

    /* none known without the database in memory, rather than all gone */
    if (albums < 0 || !rb->itunesdb_covers_begin(albums, tracks, images_changed))
    {
        rb->itunesdb_covers_skip();
        return;
    }
    rb->memset(&covers_time, 0, sizeof(covers_time));
    covers_time.started = *rb->current_tick;
    bool whole = true, held = false;
    covers_unshown = 0;

    /* the songs: those to make, in the room after the albums' starts, which
       their names' order there is done with */
    struct song_job *song_room = (struct song_job *)(starts + albums + 1);
    struct song_jobs songs = { song_room, 0,
                               ((char *)decode - (char *)song_room)
                               / sizeof(struct song_job), 0 };
    if (!tcs.valid && rb->tagcache_search(&tcs, tag_filename))
    {
        if (!rb->tagcache_for_each_track(collect_song_job, &songs))
            whole = false;
        rb->tagcache_search_finish(&tcs);
    }
    else
        whole = false;
    /* with songs left out for room, those are made the next time */
    if (songs.count >= songs.room)
        all = false;
    const int total = songs.count + albums;
    for (int i = 0; whole && i < songs.count; i++)
    {
        if (covers_stop(&held))
        {
            whole = false;
            break;
        }
        rb->library_update_covers_progress(i, total);
        rb->reset_poweroff_timer();
#ifdef ITUNESDB_FINDER_LARGE
        if (songs.jobs[i].finder
            ? make_finder_song(&songs.jobs[i], decode, decode_size) < 0
            : make_song(&songs.jobs[i], decode, decode_size) < 0)
#else
        if (make_song(&songs.jobs[i], decode, decode_size) < 0)
#endif
        {
            /* the disk nearly full: the rest at a boot with room */
            whole = false;
            break;
        }
        rb->yield();
    }

    /* then the albums, of Finder's cover or of their songs': the first
       song's whose picture is shown, else the first song's, which is none or
       why its picture isn't shown */
    for (int i = 0; whole && i < albums; i++)
    {
        if (covers_stop(&held))
        {
            whole = false;
            break;
        }
        const struct cover_job *first = &jobs[starts[i]];
        if (first->finder < 0)
        {
            const struct cover_job *job = first;
            for (int k = starts[i]; k < starts[i + 1]; k++)
            {
                if (rb->itunesdb_song_usable(jobs[k].song))
                {
                    job = &jobs[k];
                    break;
                }
            }
            if (rb->itunesdb_cover_of_song(job->key, job->stamp, job->song) == 2)
                covers_unshown++;
            continue;
        }
        if (rb->itunesdb_cover_known(first->key, first->stamp))
            continue;
        rb->library_update_covers_progress(songs.count + i, total);
        rb->reset_poweroff_timer();
        if (make_finder_cover(first, decode, decode_size) < 0)
        {
            whole = false;
            break;
        }
        rb->yield();
    }
    /* with songs left out for room, the albums not seen are kept */
    rb->itunesdb_covers_end(!whole ? COVERS_CUT
                            : all ? COVERS_WHOLE : COVERS_PARTLY);
    if (covers_time.songs || covers_time.pictures)
        rb->health_note("covers: %d songs read, %d pictures summed in %ld ms "
                        "(%d known), %d decoded in %ld ms (the most %ld ms), "
                        "the step %ld ms%s",
                        covers_time.songs, covers_time.sums,
                        covers_time.sum_ticks * 1000 / HZ, covers_time.known,
                        covers_time.pictures,
                        covers_time.decode_ticks * 1000 / HZ,
                        covers_time.decode_most * 1000 / HZ,
                        (*rb->current_tick - covers_time.started) * 1000 / HZ,
                        whole ? "" : ", cut short");
#ifdef ITUNESDB_FINDER_LARGE
    if (covers_time.finder_songs)
        rb->health_note("covers: the pane's pictures of %d of Finder's songs, "
                        "%d decoded in %ld ms",
                        covers_time.finder_songs, covers_time.finder_pictures,
                        covers_time.finder_ticks * 1000 / HZ);
#endif
    /* also after Menu, as those counted are taken for told; not before USB
       or switching off, which are waited for */
    if (!held && covers_unshown > 0)
        show_unshown();
}
#endif /* HAVE_TC_RAMCACHE */

/* The covers older versions kept as .pfraw files, one an album, which
   nothing reads now: a second's worth of them let go of at a time, as
   hundreds took long, the note's slide kept */
static void remove_old_slides(void)
{
    /* their names in the buffer the album list, saved, has done with */
    typedef char slide_name[MAX_PATH / 4];
    slide_name *names = pf_idx.buf;
    const int most = MIN(32, (int)(pf_idx.buf_sz / sizeof(slide_name)));
    char path[MAX_PATH];
    if (most <= 0)
        return;
    const long until = *rb->current_tick + HZ;
    bool more = true;
    while (more && TIME_BEFORE(*rb->current_tick, until))
    {
        int n = 0;
        DIR *dir = rb->opendir(CACHE_PREFIX);
        if (!dir)
            return;
        struct dirent *e;
        while (n < most && (e = rb->readdir(dir)) != NULL)
        {
            const char *dot = rb->strrchr(e->d_name, '.');
            if (dot && !rb->strcasecmp(dot, ".pfraw")
                && rb->strcasecmp(e->d_name, rb->strrchr(EMPTY_SLIDE, '/') + 1)
                && rb->strcasecmp(e->d_name, rb->strrchr(UNSHOWN_SLIDE, '/') + 1)
                && rb->strlen(e->d_name) < sizeof(names[0]))
                rb->strcpy(names[n++], e->d_name);
        }
        more = n == most;
        rb->closedir(dir);
        for (int i = 0; i < n && TIME_BEFORE(*rb->current_tick, until); i++)
        {
            rb->snprintf(path, sizeof(path), CACHE_PREFIX "/%s", names[i]);
            rb->remove(path);
        }
    }
}

static enum plugin_status prepare_album_list(void)
{
    struct tagcache_stat *stat = rb->tagcache_get_stat();
    size_t buf_size;

    if (!(stat->initialized && stat->ready))
        return PLUGIN_OK;
#ifdef HAVE_TC_RAMCACHE
    /* The list from the database in memory is one pass over its tracks. After
       an import that only made slides it's loaded a moment later, and without
       it the list took two searches of every track for each album. A load
       that fails says the database isn't ready: no waiting for it then. */
    const long ramcache_until = *rb->current_tick + 15 * HZ;
    while (rb->global_settings->tagcache_ram && !stat->ramcache && stat->ready
           && TIME_BEFORE(*rb->current_tick, ramcache_until))
        rb->sleep(HZ / 10);
#endif
    pf_boost(true); /* down in cleanup */
    rb->memset(&aa_cache, 0, sizeof(struct albumart_t));
    config_set_defaults(&pf_cfg);
    configfile_load(CONFIG_FILE, config, CONFIG_NUM_ITEMS, CONFIG_VERSION);

    pf_idx.buf = rb->plugin_get_buffer(&buf_size);
    pf_idx.buf_sz = buf_size;
    if (!rb->dir_exists(CACHE_PREFIX) && rb->mkdir(CACHE_PREFIX) < 0)
        return PLUGIN_OK;

    preparing = true;
    bool listed = true;
    if (pf_cfg.cache_version != CACHE_VERSION || load_album_index() < 0)
    {
        const int made = create_album_index();
        listed = made == 0;
        /* none for a library without albums or without room for the list,
           rather than tried at every boot; left by Menu, USB or switching
           off, at the next, rather than at the next sync */
        if (!listed && made != ERROR_USER_ABORT)
            rb->itunesdb_covers_skip();
#ifdef HAVE_TC_RAMCACHE
        else if (!listed)
            covers_later();
#endif
        if (listed && save_album_index() < 0)
        {
#ifdef HAVE_TC_RAMCACHE
            /* for want of room, as a rule: the list and the covers at a
               boot with room */
            covers_later();
#endif
            return PLUGIN_OK;
        }
        if (listed)
        {
            /* as init() does for a new list */
            aa_cache.idx = aa_cache.inspected = 0;
            config_save(CACHE_VERSION, pf_cfg.update_albumart);
        }
    }
    /* the old slides, and the covers in what of the buffer is left after the
       list: not when Menu, USB or switching off has left the list, or when
       there was no room for it */
    if (listed)
        remove_old_slides();
#ifdef HAVE_TC_RAMCACHE
    if (listed)
        prepare_covers();
#endif
    return PLUGIN_OK;
}
#endif /* ITUNESDB_HAVE_ART */

enum plugin_status plugin_start(const void *parameter)
{
    struct viewport *vp_main = rb->lcd_set_viewport(NULL);
    lcd_fb = vp_main->buffer->fb_ptr;

    int ret;
    const char *file = parameter;
    bool file_id3 = (parameter && (((char *) parameter)[0] == '/'));
#ifdef ITUNESDB_HAVE_ART
    bool paused_music = false;

    if (parameter && !rb->strcmp(parameter, ITUNESDB_ALBUM_LIST))
    {
        atexit(cleanup);
        return prepare_album_list();
    }
#endif

    if (!check_database())
    {
        error_wait("Please enable database");
        return PLUGIN_OK;
    }
    atexit(cleanup);

    if (init())
    {
        set_initial_slide(file_id3 ? file : NULL); /* may call splash */
#ifdef USEGSLIB
        grey_show(true);
#endif
#ifdef ITUNESDB_HAVE_ART
        /* the codec takes the CPU from the animation, the covers going by
           slower with music: it waits for them, unless the settings say to
           keep it playing */
        if (rb->global_settings->coverflow_pause
            && rb->audio_status() == AUDIO_STATUS_PLAY)
        {
            rb->audio_pause();
            paused_music = true;
        }
#endif
        ret = pictureflow_main();
#ifdef ITUNESDB_HAVE_ART
        /* not when an album was started here meanwhile, which plays */
        if (paused_music && (rb->audio_status() & AUDIO_STATUS_PAUSE))
            rb->audio_resume();
#endif
    }
    else
        ret = PLUGIN_OK;

    if ( ret == PLUGIN_OK || ret == PLUGIN_GOTO_WPS || ret == PLUGIN_GOTO_ROOT)
    {
        if (pf_state == pf_scrolling)
            pf_cfg.last_album = target;
        else
            pf_cfg.last_album = center_index;

        if (configfile_save(CONFIG_FILE, config, CONFIG_NUM_ITEMS,
                            CONFIG_VERSION))
        {
#ifdef USEGSLIB
            grey_show(false);
#endif
            rb->splash(HZ, ID2P(LANG_ERROR_WRITING_CONFIG));
            ret = PLUGIN_OK;
        }
    }
    return ret;
}
