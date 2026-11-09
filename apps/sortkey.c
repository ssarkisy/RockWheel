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
#include <stdint.h>
#include <string.h>
#include "sortkey.h"

/* The rules are checked against the order Music on the Mac itself wrote into
   an iTunesDB of 13,000 songs: they put all but 3 of its 844 artists, 6 of
   its 1667 albums and 15 of its 12,047 song names where it does, and those
   by its Sort fields. */

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

ucschar_t sortkey_fold(ucschar_t c)
{
    if (c < 0x80)
    {
        if (c >= 'A' && c <= 'Z')
            return c + ('a' - 'A');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            return c;
        return c == '\'' || c == '.' ? SORTKEY_SKIP : 0;
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
        case 0x2bc: case 0x2019: return SORTKEY_SKIP; /* apostrophes */
        /* Arabic: an alef with its hamza or madda, the yeh and the kaf of
           Persian, as the letters they're typed as */
        case 0x622: case 0x623: case 0x625: case 0x671: return 0x627;
        case 0x624: return 0x648;
        case 0x626: case 0x649: case 0x6cc: return 0x64a;
        case 0x629: return 0x647;
        case 0x6a9: return 0x643;
        case 0x640: return SORTKEY_SKIP; /* the line letters are drawn out with */
        case 0x30fb: return 0;   /* the dot between Japanese words */
        case 0x30fc: return SORTKEY_SKIP; /* and the mark of a long vowel */
        case 0x3099: case 0x309a: case 0x309b: case 0x309c:
            return SORTKEY_SKIP; /* and the voicing marks written apart */
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
        return SORTKEY_SKIP; /* accents written apart, the points of Hebrew and
                                Arabic, and the tone marks of Thai */
    if ((c >= 0x2000 && c <= 0x206f) || (c >= 0x3000 && c <= 0x303f)
        || (c >= 0xff00 && c <= 0xff0f) || c == 0xe3f)
        return 0;
    return c;
}

/* A name sorts unit by unit: a mark (a space, punctuation or a symbol), a
   number (a run of digits) or a letter, in this order of kinds where two
   differ. Each unit as one number, its kind in the high bits and its weight
   in the rest; a number's digits are compared apart. */
#define KEY_END    0x00000000u
#define KEY_MARK   0x01000000u  /* | the mark's weight */
#define KEY_NUMBER 0x02000000u
#define KEY_LETTER 0x04000000u  /* | its alphabet << 21 | the letter folded */
#define IS_MARK(key) ((key) - KEY_MARK < KEY_NUMBER - KEY_MARK)

struct unit {
    uint32_t key;
    const unsigned char *digits; /* a number's, past its zeros before */
    int count;                   /* of them */
};

static inline bool is_digit(unsigned c)
{
    return c - '0' < 10;
}

/* The marks of ASCII in the order of Unicode's collation table, the space
   first: "Miley Cyrus - …" goes before "Miley Cyrus & …" */
static const char ascii_marks[] = " _-,;:!?.'\"()[]{}@*/\\&#%`^+<=>|~$";

static uint32_t mark_weight(ucschar_t c)
{
    switch (c)
    {
        case 0xa0:                                  /* the space not broken */
            c = ' ';
            break;
        case 0x2010: case 0x2011: case 0x2013: case 0x2014:
            c = '-';
            break;
        case 0x2bc: case 0x2018: case 0x2019: case 0x201a: case 0x2032:
            c = '\'';
            break;
        case 0x201c: case 0x201d: case 0x201e:
            c = '"';
            break;
    }
    if (c > 0 && c < 0x80)
    {
        const char *m = strchr(ascii_marks, (int)c);
        if (m)
            return m - ascii_marks;
    }
    return 64 + c; /* the others after those, by their code */
}

/* The alphabets after the Latin one, in the order of Unicode's collation
   table and of Search's alphabets: a letter beyond fold()'s Latin ones, as
   the Icelandic "Þ", is one of its own after z */
static uint32_t alphabet(ucschar_t f)
{
    if (f < 0x80)
        return 0;
    if (f < 0x370)
        return 1;
    if (f < 0x400)
        return 2;                                   /* Greek */
    if (f < 0x530)
        return 3;                                   /* Cyrillic */
    if (f < 0x590)
        return 4;                                   /* Armenian */
    if (f < 0x600)
        return 5;                                   /* Hebrew */
    if (f < 0x700)
        return 6;                                   /* Arabic */
    if (f >= 0xe00 && f < 0xe80)
        return 7;                                   /* Thai */
    if (f >= 0x10a0 && f < 0x1100)
        return 8;                                   /* Georgian */
    if ((f >= 0x1100 && f < 0x1200) || (f >= 0x3130 && f < 0x3190)
        || (f >= 0xac00 && f < 0xd7b0))
        return 9;                                   /* Hangul */
    if (f >= 0x3040 && f < 0x3100)
        return 10;                                  /* kana */
    if ((f >= 0x3400 && f < 0xa000) || (f >= 0xf900 && f < 0xfb00))
        return 11;                                  /* Han */
    return 12;
}

static const unsigned char digit_chars[] = "0123456789";

/* the unit at p into u; where the next starts */
static const unsigned char *next_unit(const unsigned char *p, struct unit *u)
{
    for (;;)
    {
        const unsigned c = *p;
        if (c < 0x80)
        {
            if (is_digit(c))
            {
                /* by its value: the zeros before it don't count */
                while (*p == '0' && is_digit(p[1]))
                    p++;
                u->key = KEY_NUMBER;
                u->digits = p;
                while (is_digit(*p))
                    p++;
                u->count = p - u->digits;
                return p;
            }
            if (c == 0)
            {
                u->key = KEY_END;
                return p;
            }
            if ((c | 0x20) - 'a' < 26)
                u->key = KEY_LETTER | (c | 0x20);
            else
                u->key = KEY_MARK | mark_weight(c);
            return p + 1;
        }
        ucschar_t uc;
        const unsigned char *at = p;
        p = utf8decode(p, &uc);
        /* one cut off by the name's end, as a long tag is, would have the
           decoder step past it */
        for (const unsigned char *q = at + 1; q < p; q++)
        {
            if (!*q)
            {
                p = q;
                uc = 0xfffd;
                break;
            }
        }
        if (uc == 0xde || uc == 0xfe)
        {
            u->key = KEY_LETTER | alphabet(0xfe) << 21 | 0xfe; /* Þ þ */
            return p;
        }
        /* the symbols and punctuation of Latin-1, of the blocks from
           letterlike symbols to dingbats and arrows, the emoji, and the
           apostrophes that fold() leaves out for Search */
        if (uc < 0xc0 || (uc >= 0x2100 && uc < 0x2c00)
            || (uc >= 0xfe00 && uc < 0xfe70) || uc >= 0x1f000
            || uc == 0x2019 || uc == 0x2bc)
        {
            u->key = KEY_MARK | mark_weight(uc);
            return p;
        }
        const ucschar_t f = sortkey_fold(uc);
        if (f == SORTKEY_SKIP)
            continue;
        if (f == 0)
            u->key = KEY_MARK | mark_weight(uc);
        else if (is_digit(f))
        {
            /* another script's digit, as a number of one */
            u->key = KEY_NUMBER;
            u->digits = &digit_chars[f - '0'];
            u->count = 1;
        }
        else
            u->key = KEY_LETTER | alphabet(f) << 21 | f;
        return p;
    }
}

/* past "The ", "A " or "An ", in any case, if something follows it: "a-ha"
   and "A" stay as they are */
static const unsigned char *past_article(const unsigned char *p)
{
    int n = 0;
    if ((p[0] | 0x20) == 't' && (p[1] | 0x20) == 'h' && (p[2] | 0x20) == 'e')
        n = 3;
    else if ((p[0] | 0x20) == 'a' && (p[1] | 0x20) == 'n')
        n = 2;
    else if ((p[0] | 0x20) == 'a')
        n = 1;
    return n && p[n] == ' ' && p[n + 1] ? p + n + 1 : p;
}

const char *sortkey_start(const char *name, bool article)
{
    const unsigned char *p = (const unsigned char *)name;
    if (article)
        p = past_article(p);
    for (const unsigned char *q = p; ; )
    {
        struct unit u;
        const unsigned char *next = next_unit(q, &u);
        if (u.key == KEY_END)
            return (const char *)p;
        if (!IS_MARK(u.key))
            return (const char *)q;
        q = next;
    }
}

int sortkey_group(const char *start)
{
    struct unit u;
    next_unit((const unsigned char *)start, &u);
    if (u.key >= KEY_LETTER)
        return u.key < (KEY_LETTER | 0x80) ? SORTKEY_LATIN : SORTKEY_LETTERS;
    return u.key == KEY_NUMBER ? SORTKEY_DIGITS : SORTKEY_MARKS;
}

int sortkey_compare_from(const char *s1, const char *s2)
{
    const unsigned char *a = (const unsigned char *)s1;
    const unsigned char *b = (const unsigned char *)s2;
    for (;;)
    {
        /* Most of two names is alike byte for byte, as their first words:
           past that without a look at what it is, but for digits, which go
           as a number from the first */
        while (*a == *b && *a - 1u < 0x7f && !is_digit(*a))
        {
            a++;
            b++;
        }
        struct unit ua, ub;
        a = next_unit(a, &ua);
        b = next_unit(b, &ub);
        if (ua.key != ub.key)
            return ua.key < ub.key ? -1 : 1;
        if (ua.key == KEY_END)
            return 0;
        if (ua.key == KEY_NUMBER)
        {
            if (ua.count != ub.count)
                return ua.count < ub.count ? -1 : 1;
            const int d = memcmp(ua.digits, ub.digits, ua.count);
            if (d)
                return d < 0 ? -1 : 1;
        }
    }
}

int sortkey_compare(const char *s1, const char *s2)
{
    const char *a = sortkey_start(s1, true), *b = sortkey_start(s2, true);
    const int ga = sortkey_group(a), gb = sortkey_group(b);
    if (ga != gb)
        return ga < gb ? -1 : 1;
    const int d = sortkey_compare_from(a, b);
    return d ? d : strcmp(s1, s2);
}
