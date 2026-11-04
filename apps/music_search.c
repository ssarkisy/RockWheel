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
#include "config.h"
#ifdef HAVE_TAGCACHE
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "action.h"
#include "audio.h"
#include "font.h"
#include "lang.h"
#include "list.h"
#include "menu.h"
#include "misc.h"
#include "playlist.h"
#include "rbunicode.h"
#include "root_menu.h"
#include "screen_access.h"
#include "settings.h"
#include "statusbar-skinned.h"
#include "tagcache.h"
#include "tagtree.h"
#include "transition.h"
#include "viewport.h"
#include "wps.h"
#include "music_search.h"

/* The screen: the query, the row of letters under it and what matches under
   that, in groups as Music has them. The wheel goes along the letters and
   Select adds one; Play goes down into what was found and Menu back up. */

/* the row of letters: the menu language's alphabet, the Latin one, those of the
   other alphabets the library's names are written in, and the digits, then a
   space and a delete, around in a ring */
#define STRIP_SPACE  1
#define STRIP_DELETE 2
#define STRIP_MAX    400

/* Each alphabet: its letters as they're searched by (fold()), from first to
   last, and those the ring has of it in their order. A letter of the library's
   that isn't among them comes after them. Japanese is typed in its kana, and
   Korean in the letters its syllables are written with. */
enum script { SCRIPT_LATIN, SCRIPT_GREEK, SCRIPT_CYRILLIC, SCRIPT_ARMENIAN,
              SCRIPT_HEBREW, SCRIPT_ARABIC, SCRIPT_THAI, SCRIPT_GEORGIAN,
              SCRIPT_HANGUL, SCRIPT_KANA, SCRIPTS };
static const struct {
    ucschar_t first, last;
    const char *letters;
} scripts[SCRIPTS] = {
    [SCRIPT_LATIN]    = { 'a', 'z', "ABCDEFGHIJKLMNOPQRSTUVWXYZ" },
    [SCRIPT_GREEK]    = { 0x3b1, 0x3c9, "ΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩ" },
    [SCRIPT_CYRILLIC] = { 0x430, 0x4ff, "АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ" },
    [SCRIPT_ARMENIAN] = { 0x561, 0x586, NULL },
    [SCRIPT_HEBREW]   = { 0x5d0, 0x5ea, "אבגדהוזחטיכלמנסעפצקרשת" },
    [SCRIPT_ARABIC]   = { 0x620, 0x6ff, "ابتثجحخدذرزسشصضطظعغفقكلمنهوي" },
    [SCRIPT_THAI]     = { 0xe01, 0xe45,
                          "กขคฆงจฉชซฌญฎฏฐฑฒณดตถทธนบปผฝพฟภมยรลวศษสหฬอฮ" },
    [SCRIPT_GEORGIAN] = { 0x10d0, 0x10ff, NULL },
    [SCRIPT_HANGUL]   = { 0x3131, 0x3163,
                          "ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ" "ㅏㅐㅑㅒㅓㅔㅕㅖㅗㅛㅜㅠㅡㅣ" },
    [SCRIPT_KANA]     = { 0x3041, 0x3096,
                          "あいうえおかきくけこさしすせそたちつてとなにぬねの"
                          "はひふへほまみむめもやゆよらりるれろわをん" },
};
static const char digit_letters[] = "0123456789";

/* the menu language's alphabet, first */
static const struct {
    const char *lang;     /* as global_settings.lang_file names it */
    enum script script;
    const char *letters;  /* its own, or NULL for the alphabet's */
} native_letters[] = {
    { "russian",   SCRIPT_CYRILLIC, NULL },
    { "ukrainian", SCRIPT_CYRILLIC, "АБВГҐДЕЄЖЗИІЇЙКЛМНОПРСТУФХЦЧШЩЬЮЯ" },
    { "bulgarian", SCRIPT_CYRILLIC, "АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЬЮЯ" },
    { "srpski",    SCRIPT_CYRILLIC, "АБВГДЂЕЖЗИЈКЛЉМНЊОПРСТЋУФХЦЧЏШ" },
    { "greek",     SCRIPT_GREEK,    NULL },
    { "hebrew",    SCRIPT_HEBREW,   NULL },
    { "arabic",    SCRIPT_ARABIC,   NULL },
    { "thai",      SCRIPT_THAI,     NULL },
    { "korean",    SCRIPT_HANGUL,   NULL },
    { "japanese",  SCRIPT_KANA,     NULL },
};

/* The letters of the alphabets other than the Latin one that the library's
   names have, a bit each, as Updating Library finds them: from U+0370 to
   U+10FF, then from the kana to Hangul's letters. With the commit of the
   database they're of. */
#define LETTERS_FILE    ROCKBOX_DIR "/search_letters.bin"
#define LETTERS_VERSION 2
#define LETTERS_LOW     0x0370
#define LETTERS_MID     0x1100
#define LETTERS_HIGH    0x3040
#define LETTERS_END     0x3190
#define LETTER_BITS     ((LETTERS_MID - LETTERS_LOW) + (LETTERS_END - LETTERS_HIGH))
struct letters_file {
    int32_t version, commit;
    uint8_t present[LETTER_BITS / 8];
};

static int letter_bit(ucschar_t c)
{
    if (c >= LETTERS_LOW && c < LETTERS_MID)
        return c - LETTERS_LOW;
    if (c >= LETTERS_HIGH && c < LETTERS_END)
        return c - LETTERS_HIGH + (LETTERS_MID - LETTERS_LOW);
    return -1;
}

static bool letter_present(const uint8_t *present, ucschar_t c)
{
    const int bit = letter_bit(c);
    return bit >= 0 && (present[bit >> 3] & (1 << (bit & 7)));
}

static struct {
    ucschar_t c[STRIP_MAX];
    unsigned char set[STRIP_MAX]; /* a gap between two sets */
    int count;
    int pos;                      /* the one chosen, in the middle */
    int pitch_font;               /* the font the pitch is of, or -1 */
    int room;                     /* as many as it may have yet */
} strip;

/* kept while the screen is left for a list or for Now Playing, and come back to */
#define QUERY_SIZE 64
static char query[QUERY_SIZE];

/* What matches, in the order of the database's files, which is the lists'
   own: at most so many in each group, their names in the pool */
enum { GROUP_ARTISTS, GROUP_ALBUMS, GROUP_SONGS, GROUPS };
static const int group_tag[GROUPS] = {
    tag_virt_canonicalartist, tag_album, tag_title
};
static const int group_lang[GROUPS] = { LANG_ARTISTS, LANG_ALBUMS, LANG_SONGS };
#define ARTISTS_MAX 128
#define ALBUMS_MAX  192
#define SONGS_MAX   448
static const int group_max[GROUPS] = { ARTISTS_MAX, ALBUMS_MAX, SONGS_MAX };
static const int group_first[GROUPS] = { 0, ARTISTS_MAX, ARTISTS_MAX + ALBUMS_MAX };

static struct result {
    uint32_t name;  /* in the pool */
    int32_t idx_id; /* the track of a song */
} results[ARTISTS_MAX + ALBUMS_MAX + SONGS_MAX];
static int group_count[GROUPS];
static bool group_full[GROUPS]; /* more than it holds matched */
/* the results are of a query with words, of the database as it was then */
static bool have_results;
static int results_commit;

#define POOL_SIZE 24576
static char pool[POOL_SIZE];
static size_t pool_used;

/* the query's words, folded as names are */
#define QUERY_WORDS 16
static ucschar_t query_folded[QUERY_SIZE];
static int query_word[QUERY_WORDS + 1]; /* where each starts, then the end */
static int query_words;

/* Letters as they are searched by: small, without accents, so that a song
   with "é" is found by "E". 0 is between words; SKIP is left out, so that
   "R.E.M." and "Don't" are found as "REM" and "Dont". */
#define SKIP 0xffff

/* U+00C0 to U+00FF, then U+0100 to U+017F, by their letters; ' ' between words */
static const char latin1_base[] =
    "aaaaaaaceeeeiiiidnooooo ouuuuyts"
    "aaaaaaaceeeeiiiidnooooo ouuuuyty";
static const char latin_ext_base[] =
    "aaaaaacccccccc" "dddd" "eeeeeeeeee" "gggggggg" "hhhh" "iiiiiiiiii" "ii" "jj"
    "kkk" "llllllllll" "nnnnnnnnn" "oooooo" "oo" "rrrrrr" "ssssssss" "tttttt"
    "uuuuuuuuuuuu" "ww" "yyy" "zzzzzz" "s";

/* a hiragana without its small size or its voicing marks: "が" is found by "か" */
static ucschar_t kana_base(ucschar_t c)
{
    if (c <= 0x304a || (c >= 0x3083 && c <= 0x3088))
        return c & 1 ? c + 1 : c;       /* ぁ あ, ゃ や */
    if (c <= 0x3062)
        return c & 1 ? c : c - 1;       /* か が */
    if (c == 0x3063)
        return 0x3064;                  /* っ つ */
    if (c <= 0x3069)
        return c & 1 ? c - 1 : c;       /* つ づ */
    if (c >= 0x306f && c <= 0x307d)
        return c - (c - 0x306f) % 3;    /* は ば ぱ */
    switch (c)
    {
        case 0x308e: return 0x308f;     /* ゎ わ */
        case 0x3094: return 0x3046;     /* ゔ う */
        case 0x3095: return 0x304b;     /* ゕ か */
        case 0x3096: return 0x3051;     /* ゖ け */
    }
    return c;
}

static ucschar_t fold(ucschar_t c)
{
    if (c < 0x80)
    {
        if (c >= 'A' && c <= 'Z')
            return c + ('a' - 'A');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            return c;
        return c == '\'' || c == '.' ? SKIP : 0;
    }
    if (c >= 0xc0 && c <= 0xff)
        return latin1_base[c - 0xc0] == ' ' ? 0 : (ucschar_t)latin1_base[c - 0xc0];
    if (c >= 0x100 && c <= 0x17f)
        return latin_ext_base[c - 0x100];
    /* Vietnamese */
    if (c >= 0x1ea0 && c <= 0x1ef9)
        return c <= 0x1eb7 ? 'a' : c <= 0x1ec7 ? 'e' : c <= 0x1ecb ? 'i'
             : c <= 0x1ee3 ? 'o' : c <= 0x1ef1 ? 'u' : 'y';
    /* the kana: katakana as hiragana */
    if (c >= 0x30a1 && c <= 0x30f6)
        c -= 0x60;
    if (c >= 0x3041 && c <= 0x3096)
        return kana_base(c);
    /* the full-width Latin of Japanese titles as the plain */
    if (c >= 0xff10 && c <= 0xff19)
        return '0' + c - 0xff10;
    if (c >= 0xff21 && c <= 0xff3a)
        return 'a' + c - 0xff21;
    if (c >= 0xff41 && c <= 0xff5a)
        return 'a' + c - 0xff41;
    /* the digits of Arabic and Thai as the Latin ones */
    if (c >= 0x660 && c <= 0x669)
        return '0' + c - 0x660;
    if (c >= 0x6f0 && c <= 0x6f9)
        return '0' + c - 0x6f0;
    if (c >= 0xe50 && c <= 0xe59)
        return '0' + c - 0xe50;
    switch (c)
    {
        case 0x1a0: case 0x1a1: return 'o'; /* Vietnamese */
        case 0x1af: case 0x1b0: return 'u';
        case 0x218: case 0x219: return 's'; /* Romanian */
        case 0x21a: case 0x21b: return 't';
        case 0x2bc: case 0x2019: return SKIP; /* apostrophes */
        /* Arabic: an alef with its hamza or madda, the yeh and the kaf of
           Persian, as the letters they're typed as */
        case 0x622: case 0x623: case 0x625: case 0x671: return 0x627;
        case 0x624: return 0x648;
        case 0x626: case 0x649: case 0x6cc: return 0x64a;
        case 0x629: return 0x647;
        case 0x6a9: return 0x643;
        case 0x640: return SKIP; /* the line letters are drawn out with */
        case 0x30fb: return 0;   /* the dot between Japanese words */
        case 0x30fc: return SKIP; /* and the mark of a long vowel */
        case 0x3099: case 0x309a: case 0x309b: case 0x309c:
            return SKIP;         /* and the voicing marks written apart */
        case 0x5be: case 0x5c0: case 0x5c3:
            return 0;            /* the Hebrew hyphen and stops, between words */
        /* Greek, without its accents and with one sigma */
        case 0x386: case 0x3ac: return 0x3b1;
        case 0x388: case 0x3ad: return 0x3b5;
        case 0x389: case 0x3ae: return 0x3b7;
        case 0x38a: case 0x3af: case 0x390: case 0x3aa: case 0x3ca: return 0x3b9;
        case 0x38c: case 0x3cc: return 0x3bf;
        case 0x38e: case 0x3cd: case 0x3b0: case 0x3ab: case 0x3cb: return 0x3c5;
        case 0x38f: case 0x3ce: return 0x3c9;
        case 0x3c2: return 0x3c3;
        /* Cyrillic: "Ё" is found by "Е" */
        case 0x401: case 0x451: case 0x400: case 0x450: return 0x435;
        case 0x40d: case 0x45d: return 0x438;
        /* the final forms of Hebrew letters */
        case 0x5da: return 0x5db;
        case 0x5dd: return 0x5de;
        case 0x5df: return 0x5e0;
        case 0x5e3: return 0x5e4;
        case 0x5e5: return 0x5e6;
    }
    if (c >= 0x391 && c <= 0x3a9)
        return c + 0x20;
    if (c >= 0x400 && c <= 0x40f)
        return c + 0x50;
    if (c >= 0x410 && c <= 0x42f)
        return c + 0x20;
    if (c >= 0x4c1 && c <= 0x4ce)
        return c & 1 ? c + 1 : c; /* their capitals are the odd ones */
    if (c >= 0x460 && c <= 0x4ff && !(c & 1))
        return c + 1;
    if (c >= 0x531 && c <= 0x556)
        return c + 0x30; /* Armenian */
    if ((c >= 0x300 && c <= 0x36f) || (c >= 0x591 && c <= 0x5c7)
        || (c >= 0x64b && c <= 0x65f) || c == 0x670
        || (c >= 0xe47 && c <= 0xe4e) || c == 0xe2f || c == 0xe46)
        return SKIP; /* accents written apart, the points of Hebrew and Arabic,
                        and the tone marks of Thai */
    if ((c >= 0x2000 && c <= 0x206f) || (c >= 0x3000 && c <= 0x303f)
        || (c >= 0xff00 && c <= 0xff0f) || c == 0xe3f)
        return 0;
    return c;
}

/* A letter of the library's as the ring shows it: a capital */
static ucschar_t capital(ucschar_t c)
{
    if ((c >= 0x3b1 && c <= 0x3c9) || (c >= 0x430 && c <= 0x44f))
        return c - 0x20;
    if (c >= 0x450 && c <= 0x45f)
        return c - 0x50;
    if (c >= 0x4c2 && c <= 0x4ce)
        return c & 1 ? c : c - 1;
    if (c >= 0x461 && c <= 0x4ff && (c & 1))
        return c - 1;
    if (c >= 0x561 && c <= 0x586)
        return c - 0x30;
    return c;
}

/* Hangul's syllables as the letters they're written with: first, middle and
   last. Of those that are two, as a "ㅘ" of "ㅗ" and "ㅏ", both, so that they're
   typed one by one as on a phone. */
static const uint16_t jamo_first[19] = {
    0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141, 0x3142, 0x3143, 0x3145,
    0x3146, 0x3147, 0x3148, 0x3149, 0x314a, 0x314b, 0x314c, 0x314d, 0x314e,
};
static const uint16_t jamo_last[27] = {
    0x3131, 0x3132, 0x3133, 0x3134, 0x3135, 0x3136, 0x3137, 0x3139, 0x313a, 0x313b,
    0x313c, 0x313d, 0x313e, 0x313f, 0x3140, 0x3141, 0x3142, 0x3144, 0x3145, 0x3146,
    0x3147, 0x3148, 0x314a, 0x314b, 0x314c, 0x314d, 0x314e,
};
static const uint16_t jamo_two[][3] = {
    { 0x3133, 0x3131, 0x3145 }, { 0x3135, 0x3134, 0x3148 }, { 0x3136, 0x3134, 0x314e },
    { 0x313a, 0x3139, 0x3131 }, { 0x313b, 0x3139, 0x3141 }, { 0x313c, 0x3139, 0x3142 },
    { 0x313d, 0x3139, 0x3145 }, { 0x313e, 0x3139, 0x314c }, { 0x313f, 0x3139, 0x314d },
    { 0x3140, 0x3139, 0x314e }, { 0x3144, 0x3142, 0x3145 },
    { 0x3158, 0x3157, 0x314f }, { 0x3159, 0x3157, 0x3150 }, { 0x315a, 0x3157, 0x3163 },
    { 0x315d, 0x315c, 0x3153 }, { 0x315e, 0x315c, 0x3154 }, { 0x315f, 0x315c, 0x3163 },
    { 0x3162, 0x3161, 0x3163 },
};
#define FOLDED_MAX 6 /* the letters of a syllable at most */

static int jamo(ucschar_t c, ucschar_t *out)
{
    for (size_t i = 0; i < ARRAYLEN(jamo_two); i++)
    {
        if (jamo_two[i][0] == c)
        {
            out[0] = jamo_two[i][1];
            out[1] = jamo_two[i][2];
            return 2;
        }
    }
    out[0] = c;
    return 1;
}

/* c as it's searched by, in as many letters as that is */
static int fold_letters(ucschar_t c, ucschar_t out[FOLDED_MAX])
{
    if (c >= 0xac00 && c <= 0xd7a3)
    {
        const int s = c - 0xac00, last = s % 28;
        int n = 0;
        out[n++] = jamo_first[s / 588];
        n += jamo(0x314f + (s % 588) / 28, out + n);
        if (last)
            n += jamo(jamo_last[last - 1], out + n);
        return n;
    }
    if (c >= 0x3131 && c <= 0x3163)
        return jamo(c, out);
    out[0] = fold(c);
    return 1;
}

/* s folded into out, a 0 after each word; how many there are in out. To its
   end and no further, as a character cut off by it would step past it */
static int fold_words(const char *s, ucschar_t *out, int size)
{
    int n = 0;
    bool in_word = false;
    const char *const end = s + strlen(s);
    while (s < end && n < size - FOLDED_MAX)
    {
        ucschar_t c, letters[FOLDED_MAX];
        s = (const char *)utf8decode((const unsigned char *)s, &c);
        const int count = fold_letters(c, letters);
        for (int i = 0; i < count; i++)
        {
            c = letters[i];
            if (c == SKIP)
                continue;
            if (c == 0)
            {
                if (in_word)
                    out[n++] = 0;
                in_word = false;
                continue;
            }
            out[n++] = c;
            in_word = true;
        }
    }
    if (in_word)
        out[n++] = 0;
    return n;
}

static void fold_query(void)
{
    int n = fold_words(query, query_folded, QUERY_SIZE);
    query_words = 0;
    for (int i = 0; i < n && query_words < QUERY_WORDS; i++)
    {
        if (i == 0 || query_folded[i - 1] == 0)
            query_word[query_words++] = i;
    }
    query_word[query_words] = n;
}

/* every word of the query begins one of the name's */
static bool matches(const char *name)
{
    ucschar_t folded[256];
    int n = fold_words(name, folded, ARRAYLEN(folded));
    for (int w = 0; w < query_words; w++)
    {
        const ucschar_t *word = &query_folded[query_word[w]];
        bool found = false;
        for (int i = 0; i < n && !found; i++)
        {
            if (i > 0 && folded[i - 1] != 0)
                continue;
            int k = 0;
            while (word[k] && word[k] == folded[i + k])
                k++;
            found = word[k] == 0;
        }
        if (!found)
            return false;
    }
    return query_words > 0;
}

static bool add_result(int group, const char *name, int idx_id)
{
    size_t len = strlen(name) + 1;
    if (group_count[group] >= group_max[group] || pool_used + len > POOL_SIZE)
        return false;
    struct result *r = &results[group_first[group] + group_count[group]++];
    memcpy(pool + pool_used, name, len);
    r->name = pool_used;
    r->idx_id = idx_id;
    pool_used += len;
    return true;
}

/* through the database: its files of artists, albums and titles. If it may be
   stopped, a press stops it, and false says so: what it found by then is
   kept, as not all */
static bool search_all(bool stoppable)
{
    char buf[TAGCACHE_BUFSZ];
    pool_used = 0;
    for (int g = 0; g < GROUPS; g++)
    {
        group_count[g] = 0;
        group_full[g] = false;
    }
    have_results = query_words > 0;
    results_commit = tagcache_get_commitid();
    if (query_words == 0)
        return true;
    bool stopped = false;
    cpu_boost(true);
    for (int g = 0; g < GROUPS && !stopped; g++)
    {
        struct tagcache_search tcs;
        if (!tagcache_search(&tcs, group_tag[g]))
            continue;
        int n = 0;
        while (tagcache_get_next(&tcs, buf, sizeof(buf)))
        {
            if ((++n & 255) == 0)
            {
                yield();
                if (stoppable && !button_queue_empty())
                {
                    stopped = true;
                    break;
                }
            }
            if (!strcmp(buf, UNTAGGED) || !matches(buf))
                continue;
            if (!add_result(g, buf, tcs.idx_id))
            {
                group_full[g] = true;
                break;
            }
        }
        tagcache_search_finish(&tcs);
    }
    cpu_boost(false);
    if (stopped)
        have_results = false;
    return !stopped;
}

/* with a letter more, among what matched already */
static void search_within(void)
{
    for (int g = 0; g < GROUPS; g++)
    {
        struct result *r = &results[group_first[g]];
        int kept = 0;
        for (int i = 0; i < group_count[g]; i++)
        {
            if (matches(pool + r[i].name))
                r[kept++] = r[i];
        }
        group_count[g] = kept;
    }
}

/* the results, of the query: false if a press stopped that */
static bool search(const char *before, bool stoppable)
{
    fold_query();
    bool narrower = have_results && before[0]
                    && !strncmp(query, before, strlen(before))
                    && results_commit == tagcache_get_commitid();
    for (int g = 0; g < GROUPS; g++)
        narrower = narrower && !group_full[g];
    if (!narrower)
        return search_all(stoppable);
    search_within();
    return true;
}

/* The list: the name of each group that has any, then those in it. Of the
   artists and the albums only the first few, and See All for the rest as a
   list of its own, so that the songs aren't far down. */
#define PREVIEW 3

static bool previewed(int g)
{
    return g != GROUP_SONGS && group_count[g] > PREVIEW;
}

static int group_rows(int g)
{
    if (group_count[g] == 0)
        return 0;
    return 1 + (previewed(g) ? PREVIEW + 1 : group_count[g]);
}

static int row_count(void)
{
    int rows = 0;
    for (int g = 0; g < GROUPS; g++)
        rows += group_rows(g);
    return rows;
}

/* the group of a row, and its place in it: -1 for the group's name, PREVIEW
   for See All */
static int row_group(int row, int *index)
{
    for (int g = 0; g < GROUPS; g++)
    {
        if (row < group_rows(g))
        {
            *index = row - 1;
            return g;
        }
        row -= group_rows(g);
    }
    *index = -1;
    return -1;
}

static bool see_all(int g, int index)
{
    return previewed(g) && index == PREVIEW;
}

static const char *row_name(int row, void *data, char *buf, size_t size)
{
    (void)data;
    int index;
    int g = row_group(row, &index);
    if (g < 0)
        return "";
    if (index < 0)
        return str(group_lang[g]);
    if (see_all(g, index))
    {
        /* how many, unless more were found than are kept */
        if (group_full[g])
            return str(LANG_SEARCH_SEE_ALL);
        snprintf(buf, size, "%s\t%d", str(LANG_SEARCH_SEE_ALL), group_count[g]);
        return buf;
    }
    return pool + results[group_first[g] + index].name;
}

static bool row_is_group(int row, void *data)
{
    (void)data;
    int index;
    return row_group(row, &index) >= 0 && index < 0;
}

static bool row_has_children(int row, void *data)
{
    (void)data;
    int index;
    int g = row_group(row, &index);
    return index >= 0 && (g != GROUP_SONGS || see_all(g, index));
}

/* The strip of letters */
static void add_letter(ucschar_t c, int set)
{
    if (strip.count < strip.room)
    {
        strip.c[strip.count] = c;
        strip.set[strip.count++] = set;
    }
}

static void add_letters(const char *letters, int set)
{
    const unsigned char *p = (const unsigned char *)letters;
    while (p && *p)
    {
        ucschar_t c;
        p = utf8decode(p, &c);
        add_letter(c, set);
    }
}

/* an alphabet: its letters, then those of it the library has that they
   haven't, as "Ї" with Russian menus */
static void add_script(enum script s, const char *letters, const uint8_t *present,
                       int set)
{
    const int from = strip.count;
    add_letters(letters ? letters : scripts[s].letters, set);
    for (ucschar_t c = scripts[s].first; c <= scripts[s].last; c++)
    {
        if (!letter_present(present, c))
            continue;
        bool have = false;
        for (int i = from; i < strip.count && !have; i++)
            have = fold(strip.c[i]) == c;
        if (!have)
            add_letter(capital(c), set);
    }
}

static bool script_present(enum script s, const uint8_t *present)
{
    for (ucschar_t c = scripts[s].first; c <= scripts[s].last; c++)
    {
        if (letter_present(present, c))
            return true;
    }
    return false;
}

/* What the file says the library has. Of another database it's still that of
   one before, for the ring, until Updating Library looks again: one made in the
   background, as after Menu pressed during the import, would leave just the
   menu's letters and the Latin ones. */
static void read_letters(struct letters_file *f, bool this_database)
{
    int fd = open(LETTERS_FILE, O_RDONLY);
    bool ok = fd >= 0 && read(fd, f, sizeof(*f)) == sizeof(*f)
              && f->version == LETTERS_VERSION
              && (!this_database || f->commit == tagcache_get_commitid());
    if (fd >= 0)
        close(fd);
    if (!ok)
        memset(f, 0, sizeof(*f));
}

static void make_strip(void)
{
    struct letters_file f;
    read_letters(&f, false);
    int set = 0, native = -1;
    strip.count = 0;
    strip.pitch_font = -1;
    /* room kept for the digits, the space and the delete, which come last */
    strip.room = STRIP_MAX - (int)(sizeof(digit_letters) - 1) - 2;
    for (size_t i = 0; i < ARRAYLEN(native_letters); i++)
    {
        if (!strcmp((const char *)global_settings.lang_file, native_letters[i].lang))
            native = i;
    }
    if (native >= 0)
        add_script(native_letters[native].script, native_letters[native].letters,
                   f.present, set++);
    for (int s = 0; s < SCRIPTS; s++)
    {
        if ((native < 0 || s != (int)native_letters[native].script)
            && (s == SCRIPT_LATIN || script_present(s, f.present)))
            add_script(s, NULL, f.present, set++);
    }
    strip.room = STRIP_MAX;
    add_letters(digit_letters, set++);
    add_letter(STRIP_SPACE, set);
    add_letter(STRIP_DELETE, set);
    if (strip.pos >= strip.count)
        strip.pos = 0;
}

bool music_search_letters_stale(void)
{
    /* none to look through yet: not Updating Library at each start for that */
    if (!tagcache_is_usable())
        return false;
    struct letters_file f;
    read_letters(&f, true);
    return f.version != LETTERS_VERSION;
}

void music_search_letters_make(void)
{
    if (!tagcache_is_usable())
        return;
    struct letters_file f;
    char buf[TAGCACHE_BUFSZ];
    memset(&f, 0, sizeof(f));
    f.version = LETTERS_VERSION;
    f.commit = tagcache_get_commitid();
    cpu_boost(true);
    for (int g = 0; g < GROUPS; g++)
    {
        struct tagcache_search tcs;
        if (!tagcache_search(&tcs, group_tag[g]))
            continue;
        while (tagcache_get_next(&tcs, buf, sizeof(buf)))
        {
            /* to its end and no further, as a character cut off by it would
               step past it */
            const unsigned char *p = (const unsigned char *)buf;
            const unsigned char *const end = p + strlen(buf);
            while (p < end)
            {
                ucschar_t c, letters[FOLDED_MAX];
                p = utf8decode(p, &c);
                const int count = fold_letters(c, letters);
                for (int i = 0; i < count; i++)
                {
                    const int bit = letter_bit(letters[i]);
                    if (bit >= 0)
                        f.present[bit >> 3] |= 1 << (bit & 7);
                }
            }
        }
        tagcache_search_finish(&tcs);
    }
    cpu_boost(false);
    int fd = creat(LETTERS_FILE, 0666);
    if (fd >= 0)
    {
        write(fd, &f, sizeof(f));
        close(fd);
    }
}

static void add_char(ucschar_t c)
{
    unsigned char utf8[8];
    size_t len = utf8encode(c, utf8) - utf8;
    size_t used = strlen(query);
    if (used + len < QUERY_SIZE)
    {
        memcpy(query + used, utf8, len);
        query[used + len] = '\0';
    }
}

static void delete_char(void)
{
    size_t len = strlen(query);
    while (len > 0 && ((unsigned char)query[--len] & 0xc0) == 0x80)
        ;
    query[len] = '\0';
}

/* Drawing */
#define GREY_TEXT  (global_settings.dark_theme ? LCD_RGBPACK(0xa0, 0xa2, 0xa8) \
                                               : LCD_RGBPACK(0x66, 0x66, 0x66))
#define FIELD_FILL (global_settings.dark_theme ? LCD_RGBPACK(0x2c, 0x2e, 0x33) \
                                               : LCD_RGBPACK(0xe6, 0xe8, 0xec))
#define RULE       (global_settings.dark_theme ? LCD_RGBPACK(0x3a, 0x3c, 0x42) \
                                               : LCD_RGBPACK(0xd4, 0xd6, 0xda))

static struct viewport field_vp, strip_vp, list_vp[NB_SCREENS];
/* the theme's, as set_foreground() changes the viewport's own */
static unsigned text_colour;
static struct gui_synclist lists;
static bool in_results;
static int pitch; /* from one letter to the next */

/* the viewports, the list as many whole lines as fit under the letters */
static void make_layout(void)
{
    struct viewport ui;
    viewport_set_defaults(&ui, SCREEN_MAIN);
    const int line = font_get(ui.font)->height;
    int field_h = line + 8, strip_h = line + 10;
    int lines = (ui.height - field_h - strip_h) / line;
    if (lines < 1)
        lines = 1;
    int spare = ui.height - field_h - strip_h - lines * line;
    if (spare > 0)
    {
        field_h += spare / 2;
        strip_h += spare - spare / 2;
    }
    field_vp = strip_vp = ui;
    text_colour = ui.fg_pattern;
    field_vp.height = field_h;
    strip_vp.y = ui.y + field_h;
    strip_vp.height = strip_h;
    FOR_NB_SCREENS(i)
    {
        viewport_set_defaults(&list_vp[i], i);
        if (i == SCREEN_MAIN)
        {
            list_vp[i].y = strip_vp.y + strip_h;
            list_vp[i].height = lines * line;
        }
    }

    /* as wide as the widest of them, with room on both sides: once for the
       letters and the font, as there are hundreds */
    if (strip.pitch_font == strip_vp.font)
        return;
    strip.pitch_font = strip_vp.font;
    struct screen *display = &screens[SCREEN_MAIN];
    struct viewport *last = display->set_viewport(&strip_vp);
    pitch = line;
    for (int i = 0; i < strip.count; i++)
    {
        unsigned char utf8[8];
        int w, h;
        if (strip.c[i] <= STRIP_DELETE)
            continue;
        *utf8encode(strip.c[i], utf8) = '\0';
        display->getstringsize((char *)utf8, &w, &h);
        pitch = MAX(pitch, w + 8);
    }
    display->set_viewport(last);
}

/* a rectangle with its corners cut, as the selection's are, from the colour
   at its top to the one at its bottom: a line at a time, as
   gradient_fillrect() went a pixel past the width */
static void fill_rounded(struct screen *display, int x, int y, int w, int h,
                         unsigned top, unsigned bottom)
{
    const int r1 = RGB_UNPACK_RED(top), g1 = RGB_UNPACK_GREEN(top),
              b1 = RGB_UNPACK_BLUE(top);
    const int r2 = RGB_UNPACK_RED(bottom), g2 = RGB_UNPACK_GREEN(bottom),
              b2 = RGB_UNPACK_BLUE(bottom);
    for (int j = 0; j < h; j++)
    {
        const int edge = MIN(j, h - 1 - j);
        const int inset = edge == 0 ? 2 : edge == 1 ? 1 : 0;
        const int d = MAX(h - 1, 1);
        display->set_foreground(LCD_RGBPACK(r1 + (r2 - r1) * j / d,
                                            g1 + (g2 - g1) * j / d,
                                            b1 + (b2 - b1) * j / d));
        display->hline(x + inset, x + w - 1 - inset, y + j);
    }
}

static void draw_field(struct screen *display)
{
    const int line = font_get(field_vp.font)->height;
    const int x = 6, w = field_vp.width - 12;
    const int y = (field_vp.height - line - 2) / 2, h = line + 2;
    int text_w, text_h;

    display->set_viewport(&field_vp);
    display->clear_viewport();
    fill_rounded(display, x, y, w, h, FIELD_FILL, FIELD_FILL);

    /* its end, if it is longer than the field */
    const char *text = query;
    display->getstringsize(text, &text_w, &text_h);
    while (*text && text_w > w - 20)
    {
        text = (const char *)utf8decode((const unsigned char *)text, &(ucschar_t){0});
        display->getstringsize(text, &text_w, &text_h);
    }
    display->set_foreground(text_colour);
    display->set_drawmode(DRMODE_FG);
    display->putsxy(x + 8, y + (h - text_h) / 2, text);
    display->set_drawmode(DRMODE_SOLID);
    if (!in_results)
    {
        display->set_foreground(global_settings.lse_color);
        display->fillrect(x + 8 + text_w + 1, y + 3, 2, h - 6);
    }
    display->set_foreground(text_colour);
    display->update_viewport();
}

/* a space, as a bracket open at the top */
static void draw_space(struct screen *display, int cx, int cy)
{
    display->vline(cx - 7, cy - 1, cy + 3);
    display->vline(cx - 6, cy - 1, cy + 3);
    display->hline(cx - 7, cx + 7, cy + 3);
    display->hline(cx - 7, cx + 7, cy + 4);
    display->vline(cx + 6, cy - 1, cy + 3);
    display->vline(cx + 7, cy - 1, cy + 3);
}

/* a delete, as a key pointing left with a cross in it */
static void draw_delete(struct screen *display, int cx, int cy)
{
    const int l = cx - 9, r = cx + 8, t = cy - 6, b = cy + 6;
    display->hline(l + 6, r, t);
    display->hline(l + 6, r, b);
    display->vline(r, t, b);
    display->drawline(l, cy, l + 6, t);
    display->drawline(l, cy, l + 6, b);
    display->drawline(l + 1, cy, l + 7, t);
    display->drawline(l + 1, cy, l + 7, b);
    display->drawline(cx - 2, cy - 3, cx + 4, cy + 3);
    display->drawline(cx - 2, cy + 3, cx + 4, cy - 3);
}

static void draw_strip_item(struct screen *display, int i, int cx, bool chosen)
{
    const struct font *pf = font_get(strip_vp.font);
    const int cy = strip_vp.height / 2;
    if (chosen)
    {
        /* odd, so that its middle is a pixel and not between two */
        const int w = pitch | 1, h = (pf->height + 2) | 1;
        const int x = cx - w / 2, y = cy - h / 2;
        if (in_results)
            fill_rounded(display, x, y, w, h, FIELD_FILL, FIELD_FILL);
        else
            fill_rounded(display, x, y, w, h, global_settings.lss_color,
                         global_settings.lse_color);
    }
    unsigned colour = GREY_TEXT;
    if (chosen && in_results)
        colour = text_colour;
    else if (chosen)
        colour = global_settings.lst_color;
    display->set_foreground(colour);
    if (strip.c[i] == STRIP_SPACE)
        draw_space(display, cx, cy);
    else if (strip.c[i] == STRIP_DELETE)
        draw_delete(display, cx, cy);
    else
    {
        unsigned char utf8[8];
        int w, h;
        *utf8encode(strip.c[i], utf8) = '\0';
        display->getstringsize((char *)utf8, &w, &h);
        /* the middle of a capital, which stands on the baseline about two
           thirds of the way up to it, rather than of the font's line */
        const int cap = (pf->ascent * 2 + 1) / 3;
        display->set_drawmode(DRMODE_FG);
        display->putsxy(cx - w / 2, cy - (pf->ascent - (cap + 1) / 2), (char *)utf8);
        display->set_drawmode(DRMODE_SOLID);
    }
}

/* the one chosen in the middle, the others either side of it as far as
   they go, a little apart where a set ends */
static void draw_strip(struct screen *display)
{
    display->set_viewport(&strip_vp);
    display->clear_viewport();
    const int mid = strip_vp.width / 2, gap = pitch / 2;
    const int side = strip.count / 2;
    for (int dir = -1; dir <= 1; dir += 2)
    {
        int x = mid, i = strip.pos;
        for (int k = 1; k <= side; k++)
        {
            int next = (i + dir + strip.count) % strip.count;
            x += dir * (pitch + (strip.set[next] != strip.set[i] ? gap : 0));
            i = next;
            if (x < -pitch || x > strip_vp.width + pitch)
                break;
            draw_strip_item(display, i, x, false);
        }
    }
    draw_strip_item(display, strip.pos, mid, true);
    display->set_foreground(RULE);
    display->hline(0, strip_vp.width - 1, strip_vp.height - 1);
    display->set_foreground(text_colour);
    display->update_viewport();
}

static void draw(void)
{
    struct screen *display = &screens[SCREEN_MAIN];
    make_layout();
    FOR_NB_SCREENS(i)
        sb_set_title_text(str(LANG_SEARCH), Icon_NOICON, i);
    struct viewport *last = display->set_viewport(NULL);
    draw_field(display);
    draw_strip(display);
    display->set_viewport(last);
    gui_synclist_draw(&lists);
}

/* only the letters, as the wheel goes along them: the list under them as it was */
static void draw_letters(void)
{
    struct screen *display = &screens[SCREEN_MAIN];
    struct viewport *last = display->set_viewport(NULL);
    draw_strip(display);
    display->set_viewport(last);
}

static void list_results(void)
{
    gui_synclist_set_nb_items(&lists, row_count());
    gui_synclist_select_item(&lists, 0);
}

/* A letter shows in the field at once, and what matches is looked for once the
   typing pauses: through the whole library, that took longer than a turn of
   the wheel. Stopped by a press, it starts again at the next pause. */
#define SEARCH_PAUSE (HZ * 3 / 10)
static char searched[QUERY_SIZE]; /* what the results are of */
static bool search_waits;
static long search_due;

static void run_search(bool stoppable)
{
    search_waits = !search(searched, stoppable);
    if (search_waits)
        search_due = current_tick + SEARCH_PAUSE;
    else
        strcpy(searched, query);
    list_results();
}

/* No selection is shown in the results while the letters are typed, however
   the list is drawn: a selection of no rows */
static void show_selection(bool show)
{
    in_results = show;
    lists.selected_size = show ? 1 : 0;
}

/* the first row of what was found, past its group's name */
static void go_down(void)
{
    show_selection(true);
    gui_synclist_select_item(&lists, 1);
}

/* the row chosen, to be at again when the screen is come back to */
static int chosen_row;

static void go_up(void)
{
    show_selection(false);
    gui_synclist_select_item(&lists, 0);
}

/* the songs found, from that one on, if the list of all songs made at the
   import isn't there */
static bool play_found(struct tagcache_search *tcs, int index)
{
    char path[MAX_PATH];
    const struct result *songs = &results[group_first[GROUP_SONGS]];
    int start = 0, count = 0;
    if (audio_status() & AUDIO_STATUS_PLAY)
        audio_stop();
    if (playlist_create(NULL, NULL) < 0)
        return false;
    for (int i = 0; i < group_count[GROUP_SONGS]; i++)
    {
        if (i == index)
            start = count;
        if (tagcache_retrieve(tcs, songs[i].idx_id, tag_filename, path, sizeof(path))
            && playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST,
                                     false, false) >= 0)
            count++;
    }
    if (count == 0)
        return false;
    playlist_sync(NULL);
    if (global_settings.playlist_shuffle)
    {
        start = playlist_shuffle(0, start);
        if (!global_settings.play_selected)
            start = 0;
    }
    playlist_start(start, 0, 0);
    return true;
}

/* the song of the row playing, in the list of all songs as from Songs */
static bool play_song(int index)
{
    char path[MAX_PATH];
    struct tagcache_search tcs;
    const int idx_id = results[group_first[GROUP_SONGS] + index].idx_id;
    if (!tagcache_search(&tcs, tag_filename))
        return false;
    bool ok = tagcache_retrieve(&tcs, idx_id, tag_filename, path, sizeof(path))
              && warn_on_pl_erase();
    if (ok && !tagtree_play_song_in_library(path))
    {
        cpu_boost(true);
        ok = play_found(&tcs, index);
        cpu_boost(false);
    }
    tagcache_search_finish(&tcs);
    return ok;
}

/* what one found opens: the screen to go to, or 0 for the one it was in */
static int open_result(int g, int index)
{
    if (g == GROUP_SONGS)
        return play_song(index) ? GO_TO_WPS : 0;
    transition_begin(TRANSITION_FORWARD);
    int screen = root_menu_go_to_name(group_tag[g],
                                      pool + results[group_first[g] + index].name);
    if (screen < 0)
    {
        transition_cancel();
        return 0;
    }
    return screen;
}

static const char *group_row_name(int row, void *data, char *buf, size_t size)
{
    (void)buf;
    (void)size;
    return pool + results[group_first[(intptr_t)data] + row].name;
}

static bool group_row_has_children(int row, void *data)
{
    (void)row;
    return (intptr_t)data != GROUP_SONGS;
}

/* See All: those of a group alone, a list like any of Music's */
static int show_group(int g)
{
    struct gui_synclist list;
    int ret = 0;
    transition_begin(TRANSITION_FORWARD);
    gui_synclist_init(&list, group_row_name, (void *)(intptr_t)g, false, 1, NULL);
    gui_synclist_set_title(&list, str(group_lang[g]), Icon_NOICON);
    gui_synclist_set_icon_callback(&list, NULL);
    gui_synclist_set_has_children_callback(&list, group_row_has_children);
    gui_synclist_set_nb_items(&list, group_count[g]);
    gui_synclist_select_item(&list, 0);
    bool redraw = true;
    while (ret == 0)
    {
        if (redraw)
            gui_synclist_draw(&list);
        redraw = false;
        int action = get_action(CONTEXT_TREE, list_do_action_timeout(&list, HZ / 2));
        if (gui_synclist_do_button(&list, &action))
            continue;
        switch (action)
        {
            case ACTION_STD_OK:
                ret = open_result(g, gui_synclist_get_sel_pos(&list));
                redraw = true;
                break;
            case ACTION_STD_CANCEL:
                transition_begin(TRANSITION_BACK);
                return 0;
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
    return ret;
}

/* what the row opens: the screen to go to, or 0 for this one */
static int open_row(int row)
{
    int index;
    int g = row_group(row, &index);
    if (g < 0 || index < 0)
        return 0;
    if (see_all(g, index))
        return show_group(g);
    return open_result(g, index);
}

int music_search_screen(void *param)
{
    (void)param;
    if (!tagcache_is_usable() || !tagtree_ready())
        return GO_TO_DBBROWSER;

    const bool was_wide = menu_wide();
    if (!was_wide)
        menu_set_wide(true);
    push_current_activity(ACTIVITY_DATABASEBROWSER);

    make_strip();
    make_layout();
    gui_synclist_init(&lists, row_name, NULL, false, 1, list_vp);
    gui_synclist_set_title(&lists, str(LANG_SEARCH), Icon_NOICON);
    gui_synclist_set_icon_callback(&lists, NULL);
    gui_synclist_set_has_children_callback(&lists, row_has_children);
    lists.callback_is_group = row_is_group;
    lists.callback_draw_item = menu_draw_value_line;
    /* the results kept for the query, if they're whole and of this database:
       a search of the whole library otherwise */
    run_search(false);
    if (row_count() == 0)
        in_results = false;
    show_selection(in_results);
    if (in_results)
    {
        go_down();
        if (chosen_row > 0 && chosen_row < row_count() && !row_is_group(chosen_row, NULL))
            gui_synclist_select_item(&lists, chosen_row);
    }

    int ret = 0;
    bool redraw = true, redraw_letters = false;
    while (ret == 0)
    {
        if (redraw)
            draw();
        else if (redraw_letters)
            draw_letters();
        redraw = redraw_letters = false;
        int timeout = list_do_action_timeout(&lists, HZ / 2);
        if (search_waits)
            timeout = MAX(0, MIN(timeout, search_due - current_tick));
        int action = get_action(CONTEXT_TREE, timeout);
        /* the field and the letters with the list, as after a change of theme */
        if (action == ACTION_REDRAW)
        {
            strip.pitch_font = -1;
            redraw = true;
            continue;
        }
        /* the list as it was, if a press stopped the search: what it had found
           by then is drawn once it's done */
        if (search_waits && action == ACTION_NONE
            && TIME_AFTER(current_tick, search_due - 1))
        {
            run_search(true);
            redraw = !search_waits;
            continue;
        }
        /* a database made again meanwhile has other tracks under the numbers:
           the press is of a row that may be another now */
        if (have_results && results_commit != tagcache_get_commitid())
        {
            searched[0] = '\0';
            run_search(false);
            go_up();
            redraw = true;
            continue;
        }

        if (in_results)
        {
            if (gui_synclist_do_button(&lists, &action))
                continue;
            switch (action)
            {
                case ACTION_STD_OK:
                    chosen_row = gui_synclist_get_sel_pos(&lists);
                    ret = open_row(chosen_row);
                    redraw = true;
                    break;
                case ACTION_STD_CANCEL:
                    go_up();
                    redraw = true;
                    break;
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
            continue;
        }

        char before[QUERY_SIZE];
        strcpy(before, query);
        switch (action)
        {
            case ACTION_STD_NEXT:
            case ACTION_STD_NEXTREPEAT:
                strip.pos = (strip.pos + 1) % strip.count;
                redraw_letters = true;
                break;
            case ACTION_STD_PREV:
            case ACTION_STD_PREVREPEAT:
                strip.pos = (strip.pos + strip.count - 1) % strip.count;
                redraw_letters = true;
                break;
            case ACTION_STD_OK:
                if (strip.c[strip.pos] == STRIP_DELETE)
                    delete_char();
                else if (strip.c[strip.pos] == STRIP_SPACE)
                    add_char(' ');
                else
                    add_char(strip.c[strip.pos]);
                break;
            case ACTION_LIST_SKIPNEXT:
                add_char(' ');
                break;
            case ACTION_LIST_SKIPPREV:
                delete_char();
                break;
            case ACTION_LIST_PLAYPAUSE:
                if (search_waits)
                {
                    run_search(false);
                    redraw = true;
                }
                if (row_count() > 0)
                {
                    go_down();
                    redraw = true;
                }
                else if (audio_status() & AUDIO_STATUS_PLAY)
                    wps_do_playpause(false);
                break;
            case ACTION_STD_CANCEL:
                transition_begin(TRANSITION_BACK);
                ret = GO_TO_DBBROWSER;
                break;
            case ACTION_STD_MENU:
                ret = GO_TO_ROOT;
                break;
            default:
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                    ret = GO_TO_ROOT;
                break;
        }
        if (strcmp(before, query))
        {
            search_waits = true;
            search_due = current_tick + SEARCH_PAUSE;
            redraw = true;
        }
    }

    if (!was_wide)
        menu_set_wide(false);
    if (ret == GO_TO_WPS)
        pop_current_activity_without_refresh();
    else
        pop_current_activity();
    return ret;
}

#endif /* HAVE_TAGCACHE */
