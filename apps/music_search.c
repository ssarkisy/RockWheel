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
#include <stdarg.h>
#include <string.h>
#include "string-extra.h"
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
#include "sortkey.h"
#include "crc32.h"
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
#include "onplay.h"
#include "filetypes.h"
#include "plugin.h"
#include "splash.h"
#include "album_years.h"
#include "list_covers.h"

/* The screen: the query, the row of letters under it and what matches under
   that, in groups as Music has them. The wheel goes along the letters and
   Select adds one; Play goes down into what was found and Menu back up. */

/* the row of letters: the alphabet the lists have first, if any, in the menu
   language's letters if it's that language's own; the Latin one; those of the
   other alphabets the library's names are written in; and the digits, then a
   space and a delete, around in a ring */
#define STRIP_SPACE  1
#define STRIP_DELETE 2
/* and as a name is typed: capitals, and done */
#define STRIP_SHIFT  3
#define STRIP_DONE   4
#define STRIP_LAST_SPECIAL STRIP_DONE
#define STRIP_MAX    400

/* Each alphabet: its letters as they're searched by (sortkey_fold()), from
   first to last, and those the ring has of it in their order. A letter of the
   library's that isn't among them comes after them. Japanese is typed in its
   kana, and Korean in the letters its syllables are written with. */
enum script { SCRIPT_LATIN, SCRIPT_GREEK, SCRIPT_CYRILLIC, SCRIPT_ARMENIAN,
              SCRIPT_HEBREW, SCRIPT_ARABIC, SCRIPT_THAI, SCRIPT_GEORGIAN,
              SCRIPT_HANGUL, SCRIPT_KANA, SCRIPTS };
static const struct {
    ucschar_t first, last;
    const char *letters;
    unsigned char alphabet; /* sortkey.h's */
} scripts[SCRIPTS] = {
    [SCRIPT_LATIN]    = { 'a', 'z', "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                          SORTKEY_ALPHABET_LATIN },
    [SCRIPT_GREEK]    = { 0x3b1, 0x3c9, "ΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩ",
                          SORTKEY_ALPHABET_GREEK },
    [SCRIPT_CYRILLIC] = { 0x430, 0x4ff, "АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ",
                          SORTKEY_ALPHABET_CYRILLIC },
    [SCRIPT_ARMENIAN] = { 0x561, 0x586, NULL, SORTKEY_ALPHABET_ARMENIAN },
    [SCRIPT_HEBREW]   = { 0x5d0, 0x5ea, "אבגדהוזחטיכלמנסעפצקרשת",
                          SORTKEY_ALPHABET_HEBREW },
    [SCRIPT_ARABIC]   = { 0x620, 0x6ff, "ابتثجحخدذرزسشصضطظعغفقكلمنهوي",
                          SORTKEY_ALPHABET_ARABIC },
    [SCRIPT_THAI]     = { 0xe01, 0xe45,
                          "กขคฆงจฉชซฌญฎฏฐฑฒณดตถทธนบปผฝพฟภมยรลวศษสหฬอฮ",
                          SORTKEY_ALPHABET_THAI },
    [SCRIPT_GEORGIAN] = { 0x10d0, 0x10ff, NULL, SORTKEY_ALPHABET_GEORGIAN },
    [SCRIPT_HANGUL]   = { 0x3131, 0x3163,
                          "ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ" "ㅏㅐㅑㅒㅓㅔㅕㅖㅗㅛㅜㅠㅡㅣ",
                          SORTKEY_ALPHABET_HANGUL },
    [SCRIPT_KANA]     = { 0x3041, 0x3096,
                          "あいうえおかきくけこさしすせそたちつてとなにぬねの"
                          "はひふへほまみむめもやゆよらりるれろわをん",
                          SORTKEY_ALPHABET_KANA },
};
static const char digit_letters[] = "0123456789";

/* The ring's alphabet of those sortkey.h has first, as Japanese's kana of
   its kana and Han: -1 for none, as for Han alone, which isn't typed */
static int first_script(unsigned first)
{
    for (int s = 0; s < SCRIPTS; s++)
    {
        if (first & SORTKEY_FIRST(scripts[s].alphabet))
            return s;
    }
    return -1;
}

/* The letters of the alphabets other than the Latin one that the library's
   names have, a bit each, as Updating Library finds them: from U+0370 to
   U+10FF, then from the kana to Hangul's letters. And the letters, Latin ones
   and digits too, that begin a word of a name: what may be typed first. With
   the commit of the database they're of. */
#define LETTERS_FILE    ROCKBOX_DIR "/search_letters.bin"
/* 8 with the names of music alone (group_listed()), 9 made without the
   tracks' kinds in memory: a file of the other is made again, as Music's
   lists are then the other too */
#define LETTERS_VERSION (tagcache_kinds_known() ? 8 : 9)
#define LETTERS_LOW     0x0370
#define LETTERS_MID     0x1100
#define LETTERS_HIGH    0x3040
#define LETTERS_END     0x3190
#define LETTER_BITS     ((LETTERS_MID - LETTERS_LOW) + (LETTERS_END - LETTERS_HIGH))
#define NEXT_BITS       (26 + 10 + LETTER_BITS) /* the Latin ones and the digits too */
#define NEXT_BYTES      ((NEXT_BITS + 7) / 8)
struct letters_file {
    int32_t version, commit;
    uint8_t present[LETTER_BITS / 8];
    uint8_t starts[NEXT_BYTES];
};

/* The library's names as they're searched by, made with the letters: after a
   header, each name's group, its track (a song's), where it's found (an
   artist's, ARTIST_SHOWN_OFF and ARTIST_SHOWN_ON) or its album's number in the
   table of years (an album's, album_years.h, -1 without it), its name as shown
   and its words folded (fold_words()) in UTF-8 with a 0 after each, the two
   after their lengths. An album's name is there once for each of its album
   artists, as Albums has them apart. A search reads it through in pieces and
   compares bytes: folding the database's every name again at each search
   took most of a second. */
#define NAMES_FILE      ROCKBOX_DIR "/search_names.bin"
#define NAMES_TEMP      NAMES_FILE ".tmp" /* whole_file_open()'s */
#define NAME_BYTES      255  /* of a name, and of its folded words, at most */
#define ENTRY_MAX       (1 + 4 + 1 + NAME_BYTES + 1 + NAME_BYTES)
struct names_header {
    int32_t version, commit, count;
    /* the table of years its albums' numbers are of: its count and hash */
    int32_t albums;
    uint32_t table;
};
/* Read in pieces from its start, as many sectors each, into a buffer aligned
   for the disk to put them in directly; the rest of the last entry of a piece
   goes before the next. From its header on, the pieces began within a sector,
   and the file system read them a sector at a time through its cache. */
#define NAMES_CHUNK     4096
#define NAMES_CARRY     1024 /* at least ENTRY_MAX, and as aligned */
static unsigned char names_buf[NAMES_CARRY + NAMES_CHUNK] CACHEALIGN_ATTR;
static int names_commit = -1;         /* of the file last found good */

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

/* a letter as searched by, a bit each of every alphabet's: -1 if it isn't one */
static int next_bit(ucschar_t c)
{
    if (c >= 'a' && c <= 'z')
        return c - 'a';
    if (c >= '0' && c <= '9')
        return 26 + c - '0';
    const int bit = letter_bit(c);
    return bit < 0 ? -1 : 36 + bit;
}

static void set_next(uint8_t *bits, ucschar_t c)
{
    const int bit = next_bit(c);
    if (bit >= 0)
        bits[bit >> 3] |= 1 << (bit & 7);
}

/* The letters that may come next, as what's typed so far leads to names that
   go on with them: the others are greyed in the ring, and the wheel goes past
   them. Known once the search of what's typed is through, all of them before
   that. */
static uint8_t next_letters[NEXT_BYTES];
static bool next_known;
static uint8_t word_starts[NEXT_BYTES]; /* of the library's, from the file */
static bool word_starts_known;

static struct {
    ucschar_t c[STRIP_MAX];
    unsigned char set[STRIP_MAX]; /* a gap between two sets */
    int count;
    int pos;                      /* the one chosen, in the middle */
    int pitch_font;               /* the font the pitch is of, or -1 */
    int room;                     /* as many as it may have yet */
} strip;

/* Naming, as a playlist or a folder is: the same ring with the menu
   language's letters and the Latin ones only, as the ring shows them, small
   or capital by ⇧, and ✓ for done; no search under it */
static bool naming;
static char *name_text;         /* the caller's */
static size_t name_max;         /* its bytes at most, without its 0 */
#define NAME_CHARS_MAX 64
enum { SHIFT_OFF, SHIFT_ONE, SHIFT_LOCK };
static int name_shift;
static const char *name_title;

/* kept while the screen is left for a list or for Now Playing, and come back to */
#define QUERY_SIZE 64
static char query[QUERY_SIZE];

/* Those of the last queries, so that deleting a letter has them back at once:
   what the shorter query matches takes a search of the whole library, and all
   the letters were open until it was through */
#define KEPT_LETTERS 8
static struct {
    char query[QUERY_SIZE];
    int commit;
    bool grouping; /* Group by Album Artist, which artists are found */
    uint8_t next[NEXT_BYTES];
} kept_letters[KEPT_LETTERS];
static int kept_next; /* the one to write next, around */

/* of this query, as the library is now */
static bool kept_letters_of(int k)
{
    return kept_letters[k].commit == tagcache_get_commitid()
           && kept_letters[k].grouping == global_settings.artists_by_album_artist
           && !strcmp(kept_letters[k].query, query);
}

static void keep_letters(void)
{
    int k = 0;
    while (k < KEPT_LETTERS && !kept_letters_of(k))
        k++;
    if (k == KEPT_LETTERS)
    {
        k = kept_next;
        kept_next = (kept_next + 1) % KEPT_LETTERS;
    }
    strmemccpy(kept_letters[k].query, query, QUERY_SIZE);
    kept_letters[k].commit = tagcache_get_commitid();
    kept_letters[k].grouping = global_settings.artists_by_album_artist;
    memcpy(kept_letters[k].next, next_letters, NEXT_BYTES);
}

/* the letters of the query as they were for it before, if they were known */
static bool kept_letters_back(void)
{
    for (int k = 0; k < KEPT_LETTERS; k++)
    {
        if (kept_letters[k].query[0] && kept_letters_of(k))
        {
            memcpy(next_letters, kept_letters[k].next, NEXT_BYTES);
            next_known = true;
            return true;
        }
    }
    return false;
}

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
    int32_t idx_id; /* the track of a song, an album's number */
} results[ARTISTS_MAX + ALBUMS_MAX + SONGS_MAX];
static int group_count[GROUPS];
static bool group_full[GROUPS]; /* more than it holds matched */
/* the results are of a query with words, of the database as it was then,
   of the artists Group by Album Artist had found, and of the albums of a
   name apart or not, as Artists in Albums had them */
static bool have_results;
static int results_commit;
static bool results_grouping, results_album_artists;

static bool results_current(void)
{
    return results_commit == tagcache_get_commitid()
           && results_grouping == global_settings.artists_by_album_artist
           && results_album_artists == global_settings.album_artists;
}

#define POOL_SIZE 24576
static char pool[POOL_SIZE];
static size_t pool_used;

/* the query's words, folded as names are */
#define QUERY_WORDS 16
static ucschar_t query_folded[QUERY_SIZE];
static int query_word[QUERY_WORDS + 1]; /* where each starts, then the end */
static int query_words;

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
    out[0] = sortkey_fold(c);
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
            /* left out, so that "R.E.M." and "Don't" are found as "REM"
               and "Dont" */
            if (c == SORTKEY_SKIP)
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

/* folded letters in UTF-8, a 0 for each 0 and one last: how many bytes, at
   most room */
static int folded_bytes(const ucschar_t *folded, int n, unsigned char *out, int room)
{
    int len = 0;
    for (int i = 0; i < n; i++)
    {
        unsigned char utf8[8];
        const int k = folded[i] ? utf8encode(folded[i], utf8) - utf8 : 1;
        if (len + k > room - 1)
            break;
        if (folded[i])
            memcpy(out + len, utf8, k);
        else
            out[len] = 0;
        len += k;
    }
    if (len == 0 || out[len - 1] != 0)
        out[len++] = 0;
    return len;
}

static bool query_is_open; /* as fold_query() found it, query_open() */

/* the query's words as the names' file has a name's, where each starts */
static unsigned char query_bytes[QUERY_SIZE * 4 + 1];
static int query_byte_word[QUERY_WORDS + 1];

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
    const size_t len = strlen(query);
    query_is_open = len == 0 || query[len - 1] == ' ';

    int at = 0;
    for (int w = 0; w < query_words; w++)
    {
        query_byte_word[w] = at;
        at += folded_bytes(&query_folded[query_word[w]],
                           query_word[w + 1] - query_word[w], query_bytes + at,
                           sizeof(query_bytes) - at);
    }
    query_byte_word[query_words] = at;
}

/* the query ends between words, as with nothing or a space last: a word is
   typed anew */
static bool query_open(void)
{
    return query_is_open;
}

/* the word begins one of the name's, folded; with next, the letter after it
   in each such word of the name is one that may be typed after it */
static bool word_begins(const ucschar_t *word, const ucschar_t *folded, int n,
                        uint8_t *next)
{
    bool found = false;
    for (int i = 0; i < n && (next || !found); i++)
    {
        if (i > 0 && folded[i - 1] != 0)
            continue;
        int k = 0;
        while (word[k] && word[k] == folded[i + k])
            k++;
        if (word[k] == 0)
        {
            found = true;
            if (next && folded[i + k])
                set_next(next, folded[i + k]);
        }
    }
    return found;
}

/* Every word of the query begins one of the name's. With next, the letters
   that may be typed after the query in the name: after the start of a word
   of it that the last word of the query is, or with the query open, the first
   of each of its words. */
static bool matches(const char *name, uint8_t *next)
{
    ucschar_t folded[256];
    const int n = fold_words(name, folded, ARRAYLEN(folded));
    const bool open = query_open();
    const int typed = open ? query_words : query_words - 1; /* the last's apart */
    for (int w = 0; w < typed; w++)
    {
        if (!word_begins(&query_folded[query_word[w]], folded, n, NULL))
            return false;
    }
    if (open)
    {
        if (next)
        {
            for (int i = 0; i < n; i++)
            {
                if (folded[i] && (i == 0 || folded[i - 1] == 0))
                    set_next(next, folded[i]);
            }
        }
        return query_words > 0;
    }
    return word_begins(&query_folded[query_word[typed]], folded, n, next);
}

/* As word_begins() and matches(), of a name of the names' file: its words in
   UTF-8, folded already */
static bool bytes_begin(const unsigned char *word, int wlen,
                        const unsigned char *f, int n, uint8_t *next)
{
    bool found = false;
    for (int i = 0; i + wlen <= n && (next || !found); i++)
    {
        if (f[i] != word[0] || (i > 0 && f[i - 1] != 0)
            || memcmp(f + i, word, wlen))
            continue;
        found = true;
        if (next && f[i + wlen])
        {
            ucschar_t c;
            utf8decode(f + i + wlen, &c);
            set_next(next, c);
        }
    }
    return found;
}

static bool bytes_match(const unsigned char *f, int n, uint8_t *next)
{
    const bool open = query_open();
    const int typed = open ? query_words : query_words - 1;
    for (int w = 0; w < typed; w++)
    {
        const int at = query_byte_word[w];
        const int wlen = query_byte_word[w + 1] - at - 1; /* its 0 apart */
        if (!bytes_begin(query_bytes + at, wlen, f, n, NULL))
            return false;
    }
    if (open)
    {
        if (next)
        {
            for (int i = 0; i < n; i++)
            {
                if (f[i] && (i == 0 || f[i - 1] == 0))
                {
                    ucschar_t c;
                    utf8decode(f + i, &c);
                    set_next(next, c);
                }
            }
        }
        return query_words > 0;
    }
    const int at = query_byte_word[typed];
    return bytes_begin(query_bytes + at, query_byte_word[typed + 1] - at - 1,
                       f, n, next);
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

/* Where a name of the group of artists is found: as Artists lists the songs'
   artists (Group by Album Artist off), or the albums' and, as guests, the
   songs' (on). A name the two spell otherwise, in the case of a Latin letter,
   is there twice, as each list has it. */
#define ARTIST_SHOWN_OFF 1
#define ARTIST_SHOWN_ON  2

static bool artist_shown(int32_t shown)
{
    return shown & (global_settings.artists_by_album_artist ? ARTIST_SHOWN_ON
                                                            : ARTIST_SHOWN_OFF);
}

/* as the database's files are sorted (tagcache.c's compare()): <Untagged>
   first, then by Music's rules, but for the Sort fields of each file's own */
static int file_order(const char *a, const char *b)
{
    const bool untagged_a = !strcmp(a, UNTAGGED);
    const bool untagged_b = !strcmp(b, UNTAGGED);
    if (untagged_a || untagged_b)
        return untagged_b - untagged_a;
    return sortkey_compare(a, b);
}

/* a name's crc without regard to the case of Latin letters, as the database
   takes two names for one */
static uint32_t name_crc(const char *name)
{
    uint32_t crc = 0xffffffff;
    for (const unsigned char *c = (const unsigned char *)name; *c; c++)
    {
        const unsigned char low = *c >= 'A' && *c <= 'Z' ? *c + ('a' - 'A') : *c;
        crc = crc_32(&low, 1, crc);
    }
    return crc;
}

static int cmp_crc(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* A group's names, in the order of the database's files. The artists are the
   songs' and the albums' at once, their two files read side by side, which
   puts the names in one order: a name in both comes once where the two come
   together. Their Sort fields may have them apart, each file's of its own, so
   that a song's artist is known to be an album's by the albums' artists'
   names, read first: as their crcs, sorted, in the room the caller gives. */
static struct {
    int files;
    int group;
    /* the artists with a song of music: a bit for each place a name starts
       at in the songs' and the albums' artists' files, NULL for all */
    uint8_t *music[2];
    struct tagcache_search tcs[2];
    bool searching[2];  /* begun, to be finished */
    bool have[2];       /* its name read, not given yet */
    char name[2][TAGCACHE_BUFSZ];
    int read;           /* of the files' names, deleted ones not */
    uint32_t *albums;   /* the albums' artists' name_crc()s, NULL for none */
    int album_count;
} names_in;

/* the albums' artists into the room, false if they don't fit */
static bool albums_read(void *room, size_t size)
{
    struct tagcache_search tcs;
    uint32_t *crcs = (uint32_t *)ALIGN_UP((char *)room, sizeof(uint32_t));
    const size_t max = size > sizeof(uint32_t) ? size / sizeof(uint32_t) - 1 : 0;
    size_t n = 0;
    bool fits = true;
    if (!room || !tagcache_search(&tcs, tag_albumartist))
        return false;
    while (tagcache_get_next(&tcs, names_in.name[1], sizeof(names_in.name[1])))
    {
        if (n == max)
        {
            fits = false;
            break;
        }
        crcs[n++] = name_crc(names_in.name[1]);
    }
    tagcache_search_finish(&tcs);
    if (!fits)
        return false;
    qsort(crcs, n, sizeof(*crcs), cmp_crc);
    names_in.albums = crcs;
    names_in.album_count = n;
    return true;
}

/* the songs' artist is one of the albums', in their spelling or another;
   without the albums' read, none is */
static bool album_artist(const char *name)
{
    if (!names_in.albums)
        return false;
    const uint32_t crc = name_crc(name);
    int lo = 0, hi = names_in.album_count - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (names_in.albums[mid] == crc)
            return true;
        if (names_in.albums[mid] < crc)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}

#ifdef HAVE_TC_RAMCACHE
/* the places of the two artists' names of each track: the last of them
   first, then a bit for each that a song of music has. A name's entry is
   over 8 bytes, so a bit an 8 bytes tells them apart. */
static int32_t artists_last[2];

static void artist_places(int idx_id, const int32_t *tag_seek, void *data)
{
    const int tags[2] = { group_tag[GROUP_ARTISTS], tag_albumartist };
    for (int i = 0; i < 2; i++)
    {
        const int32_t seek = tag_seek[tags[i]];
        if (!data)
            artists_last[i] = MAX(artists_last[i], seek);
        else if (seek >= 0 && tagcache_track_kind(idx_id) == 0)
            names_in.music[i][seek >> 6] |= 1 << ((seek >> 3) & 7);
    }
}
#endif

/* Songs of music; artists with a song of music, as Music's Artists lists
   them, an author of shows or books alone having no row to go to; every
   album's name, a show's and a book's too, which open their own screens */
static bool group_listed(int i)
{
    if (names_in.group == GROUP_SONGS)
        return tagcache_track_kind(names_in.tcs[0].idx_id) == 0;
    const int32_t seek = names_in.tcs[i].result_seek;
    return names_in.group != GROUP_ARTISTS || !names_in.music[i] || seek < 0
           || (names_in.music[i][seek >> 6] & (1 << ((seek >> 3) & 7)));
}

/* the artists' with room for the albums' (albums_read()) */
static void group_begin(int g, void *room, size_t size)
{
    names_in.files = g == GROUP_ARTISTS ? 2 : 1;
    names_in.group = g;
    names_in.music[0] = names_in.music[1] = NULL;
    names_in.read = 0;
    names_in.albums = NULL;
    names_in.album_count = 0;
#ifdef HAVE_TC_RAMCACHE
    artists_last[0] = artists_last[1] = -1;
    if (g == GROUP_ARTISTS && room && tagcache_kinds_known()
        && group_tag[g] < TAG_COUNT && tagcache_for_each_track(artist_places, NULL))
    {
        /* the bits at the room's end, the albums' artists before them */
        const size_t bytes[2] = { (artists_last[0] >> 6) + 2,
                                  (artists_last[1] >> 6) + 2 };
        if (bytes[0] + bytes[1] < size / 2)
        {
            size -= bytes[0] + bytes[1];
            names_in.music[0] = (uint8_t *)room + size;
            names_in.music[1] = names_in.music[0] + bytes[0];
            memset(names_in.music[0], 0, bytes[0] + bytes[1]);
            tagcache_for_each_track(artist_places, names_in.music);
        }
    }
#endif
    if (g == GROUP_ARTISTS)
        albums_read(room, size);
    for (int i = 0; i < names_in.files; i++)
    {
        names_in.searching[i] = tagcache_search(&names_in.tcs[i],
                                                i ? tag_albumartist : group_tag[g]);
        names_in.have[i] = false;
    }
}

/* the next name, NULL past the last: a song's track in *idx_id, or where an
   artist is found (ARTIST_SHOWN_OFF, ARTIST_SHOWN_ON) */
static const char *group_next(int32_t *idx_id)
{
    for (int i = 0; i < names_in.files; i++)
    {
        while (names_in.searching[i] && !names_in.have[i]
               && tagcache_get_next(&names_in.tcs[i], names_in.name[i],
                                    sizeof(names_in.name[i])))
        {
            names_in.have[i] = group_listed(i);
            names_in.read++;
        }
    }
    if (names_in.files == 1)
    {
        if (!names_in.have[0])
            return NULL;
        names_in.have[0] = false;
        *idx_id = names_in.tcs[0].idx_id;
        return names_in.name[0];
    }
    if (!names_in.have[0] && !names_in.have[1])
        return NULL;
    const int order = !names_in.have[1] ? -1 : !names_in.have[0] ? 1
                      : file_order(names_in.name[0], names_in.name[1]);
    if (order < 0)
    {
        /* a songs' artist, a guest's unless an album's too, which shows
           with Group by Album Artist on in its albums' spelling */
        names_in.have[0] = false;
        *idx_id = album_artist(names_in.name[0]) ? ARTIST_SHOWN_OFF
                  : ARTIST_SHOWN_ON | ARTIST_SHOWN_OFF;
        return names_in.name[0];
    }
    /* an albums' artist, the same of the songs' with it once */
    *idx_id = order == 0 ? ARTIST_SHOWN_ON | ARTIST_SHOWN_OFF : ARTIST_SHOWN_ON;
    names_in.have[0] = names_in.have[0] && order > 0;
    names_in.have[1] = false;
    return names_in.name[1];
}

static void group_finish(void)
{
    for (int i = 0; i < names_in.files; i++)
    {
        if (names_in.searching[i])
            tagcache_search_finish(&names_in.tcs[i]);
    }
}

/* through the database: its files of artists, albums and titles. If it may be
   stopped, a press stops it, and false says so: what it found by then is
   kept, as not all */

/* The names' file in the plugins' buffer while Search is open, read whole as
   it opens: from the disk at each letter, its 157 reads were half of each
   search. The buffer is the core's while no plugin runs, and lent to others,
   as the playlist of a song played from Search: let go of for that, and read
   again as Search comes back. Marked at its end, and the copy is used only as
   long as the mark and its header are as they were. */
#define NAMES_MARK 0x5246534eu
static unsigned char *names_ram;
static size_t names_ram_len;

static unsigned int names_ram_gen; /* of the plugins' buffer, as they went in */

static bool names_held(void)
{
    if (!names_ram)
        return false;
    if (plugin_buffer_gen() != names_ram_gen)
    {
        names_ram = NULL;
        return false;
    }
    uint32_t mark;
    struct names_header h;
    memcpy(&mark, names_ram + names_ram_len, sizeof(mark));
    memcpy(&h, names_ram, sizeof(h));
    if (mark == NAMES_MARK && h.version == LETTERS_VERSION
        && h.commit == tagcache_get_commitid())
        return true;
    names_ram = NULL;
    return false;
}

static void names_let_go(void)
{
    names_ram = NULL;
}

static void names_hold(void)
{
    /* still there: not read again, 1 MB, each time Search is back */
    if (names_held())
        return;
    names_ram = NULL;
    if (plugin_is_running())
        return;
    size_t size;
    unsigned char *const buf = plugin_get_buffer(&size);
    const int fd = open(NAMES_FILE, O_RDONLY);
    if (!buf || fd < 0)
    {
        if (fd >= 0)
            close(fd);
        return;
    }
    const off_t len = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    const bool ok = len >= (off_t)sizeof(struct names_header)
                    && (size_t)len + sizeof(uint32_t) <= size
                    && read(fd, buf, len) == len;
    close(fd);
    if (!ok)
        return;
    const uint32_t mark = NAMES_MARK;
    memcpy(buf + len, &mark, sizeof(mark));
    names_ram = buf;
    names_ram_len = len;
    names_ram_gen = plugin_buffer_gen();
    if (!names_held())
        names_ram = NULL;
}

/* the albums' numbers of the names' file are of the table of years as it
   is, which it was written with: as the search found them */
static bool albums_known;

/* An album of the name of the one found before it, which the names' file has
   next to it: of another album artist, a row of its own only as Albums has
   them apart (Artists in Albums), and with the numbers that tell them apart */
static bool same_album_name(const char *name)
{
    const int n = group_count[GROUP_ALBUMS];
    return (!global_settings.album_artists || !albums_known) && n > 0
           && !strcmp(pool + results[group_first[GROUP_ALBUMS] + n - 1].name, name);
}

static bool names_albums_known(const struct names_header *h)
{
    return h->albums == album_years_count() && h->table == album_years_hash();
}

/* the number of a found album in the table of years, -1 if it isn't known,
   as without the names' file or the table */
static int album_of(const struct result *r)
{
    return albums_known && r->idx_id >= 0 && r->idx_id < album_years_count()
           ? r->idx_id : -1;
}

/* its album artist, after its name in Search's list as Albums has it, with
   MENU_LIGHT_SEP between: the name alone without Artists in Albums, or if
   it isn't known */
static const char *album_row_name(const struct result *r, char *buf, size_t size)
{
    const char *name = pool + r->name;
    const int no = album_of(r);
    const int len = strlen(name);
    if (!global_settings.album_artists || no < 0 || (size_t)len + 2 >= size)
        return name;
    memcpy(buf, name, len);
    buf[len] = MENU_LIGHT_SEP;
#ifdef HAVE_TC_RAMCACHE
    if (tagcache_tag_string(tag_albumartist, album_years_artist(no),
                            buf + len + 1, size - len - 1))
        return buf;
#endif
    return name;
}

/* the names' file opened past its header, if it's of this database: -1 if not */
static int open_names(struct names_header *h)
{
    int fd = open(NAMES_FILE, O_RDONLY);
    if (fd < 0)
        return -1;
    if (read(fd, h, sizeof(*h)) != sizeof(*h) || h->version != LETTERS_VERSION
        || h->commit != tagcache_get_commitid() || h->count < 0)
    {
        close(fd);
        return -1;
    }
    names_commit = h->commit;
    return fd;
}

/* Through the names' file, every name of it, as the letters that may follow
   are of all that match: false if there's none of this database. A press
   stops it, if it may be stopped, as *stopped says. */
static bool search_names(bool stoppable, bool *stopped)
{
    struct names_header h;
    const unsigned char *p, *end;
    ssize_t got;
    bool more;
    int fd = -1;
    unsigned char *const chunk = names_buf + NAMES_CARRY;
    if (names_held())
    {
        /* all of it in memory, as read when Search opened */
        memcpy(&h, names_ram, sizeof(h));
        p = names_ram + sizeof(h);
        end = names_ram + names_ram_len;
        more = false;
    }
    else
    {
        fd = open(NAMES_FILE, O_RDONLY);
        if (fd < 0)
            return false;
        got = read(fd, chunk, NAMES_CHUNK);
        if (got < (ssize_t)sizeof(h))
        {
            close(fd);
            return false;
        }
        memcpy(&h, chunk, sizeof(h));
        if (h.version != LETTERS_VERSION || h.commit != tagcache_get_commitid()
            || h.count < 0)
        {
            close(fd);
            return false;
        }
        p = chunk + sizeof(h);
        end = chunk + got;
        more = got == NAMES_CHUNK;
    }
    names_commit = h.commit;
    albums_known = names_albums_known(&h);
    int e;
    for (e = 0; e < h.count; e++)
    {
        if (end - p < ENTRY_MAX && more)
        {
            const int rest = end - p;
            memmove(chunk - rest, p, rest);
            got = read(fd, chunk, NAMES_CHUNK);
            if (got < 0)
                got = 0;
            more = got == NAMES_CHUNK;
            p = chunk - rest;
            end = chunk + got;
        }
        if (end - p < 7)
            break;
        const int g = p[0], name_len = p[5];
        const unsigned char *const name = p + 6;
        const int folded_len = name[name_len];
        const unsigned char *const folded = name + name_len + 1;
        if (g >= GROUPS || end - p < 7 + name_len + folded_len)
            break; /* not as written */
        const unsigned char *const entry = p;
        p += 7 + name_len + folded_len;

        if ((e & 255) == 255)
        {
            yield();
            if (stoppable && !button_queue_empty())
            {
                *stopped = true;
                break;
            }
        }
        int32_t idx_id;
        memcpy(&idx_id, entry + 1, sizeof(idx_id));
        if ((g == GROUP_ARTISTS && !artist_shown(idx_id))
            || !bytes_match(folded, folded_len, next_letters) || group_full[g])
            continue;
        char shown[NAME_BYTES + 1];
        memcpy(shown, name, name_len);
        shown[name_len] = '\0';
        if (g == GROUP_ALBUMS && same_album_name(shown))
            continue;
        if (!add_result(g, shown, idx_id))
            group_full[g] = true;
    }
    if (fd >= 0)
        close(fd);
    return true;
}

/* through the database: its files of artists, albums and titles, as the names'
   file has them if it's there. If it may be stopped, a press stops it, and
   false says so: what it found by then is kept, as not all */
static bool search_all(bool stoppable)
{
    pool_used = 0;
    for (int g = 0; g < GROUPS; g++)
    {
        group_count[g] = 0;
        group_full[g] = false;
    }
    have_results = query_words > 0;
    results_commit = tagcache_get_commitid();
    results_grouping = global_settings.artists_by_album_artist;
    results_album_artists = global_settings.album_artists;
    memset(next_letters, 0, sizeof(next_letters));
    next_known = false;
    /* nothing typed: what begins a word of a name, as Updating Library found */
    if (query_words == 0)
    {
        memcpy(next_letters, word_starts, sizeof(next_letters));
        next_known = word_starts_known;
        return true;
    }
    bool stopped = false, all_found;
    cpu_boost(true);
    if (search_names(stoppable, &stopped))
        all_found = !stopped; /* every name looked at, past full groups too */
    else
    {
        /* the albums' artists in the plugins' buffer, which the names' file
           isn't in */
        size_t room = 0;
        void *albums = NULL;
        if (!plugin_is_running())
        {
            names_let_go();
            albums = plugin_get_buffer(&room);
        }
        for (int g = 0; g < GROUPS && !stopped; g++)
        {
            const char *name;
            int32_t idx_id;
            int n = 0;
            group_begin(g, albums, room);
            while ((name = group_next(&idx_id)))
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
                if (!strcmp(name, UNTAGGED)
                    || (g == GROUP_ARTISTS && !artist_shown(idx_id))
                    || !matches(name, next_letters))
                    continue;
                /* an album by its name alone, without the names' file */
                if (g == GROUP_ALBUMS)
                    idx_id = -1;
                if (!add_result(g, name, idx_id))
                {
                    group_full[g] = true;
                    break;
                }
            }
            group_finish();
        }
        /* The letters that may follow are of all the names that match, and
           so known only when they all are found: going on past a full group
           for them, folding each name, made each search four times as long */
        all_found = !stopped;
        for (int g = 0; g < GROUPS; g++)
            all_found = all_found && !group_full[g];
    }
    cpu_boost(false);
    if (stopped)
        have_results = false;
    next_known = all_found;
    if (next_known)
        keep_letters();
    return !stopped;
}

/* with a letter more, among what matched already, which is all that does */
static void search_within(void)
{
    memset(next_letters, 0, sizeof(next_letters));
    for (int g = 0; g < GROUPS; g++)
    {
        struct result *r = &results[group_first[g]];
        int kept = 0;
        for (int i = 0; i < group_count[g]; i++)
        {
            if (matches(pool + r[i].name, next_letters))
                r[kept++] = r[i];
        }
        group_count[g] = kept;
    }
    next_known = true;
    keep_letters();
}

/* what matched before is all that may match now: before, with all of it kept,
   leads the query, of the same database and artists */
static bool narrows(const char *before)
{
    bool narrower = have_results && before[0]
                    && !strncmp(query, before, strlen(before))
                    && results_current();
    for (int g = 0; g < GROUPS; g++)
        narrower = narrower && !group_full[g];
    return narrower;
}

/* the results, of the query: false if a press stopped that */
static bool search(const char *before, bool stoppable)
{
    fold_query();
    if (!narrows(before))
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
    if (g == GROUP_ALBUMS)
        return album_row_name(&results[group_first[g] + index], buf, size);
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
            have = sortkey_fold(strip.c[i]) == c;
        if (!have)
            add_letter(sortkey_capital(c), set);
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
    memcpy(word_starts, f.starts, sizeof(word_starts));
    word_starts_known = f.version == LETTERS_VERSION;
    int set = 0;
    strip.count = 0;
    strip.pitch_font = -1;
    /* room kept for the digits, the space and the delete, which come last */
    strip.room = STRIP_MAX - (int)(sizeof(digit_letters) - 1) - 2;
    /* first as the lists have it, with the menu language's own letters of it */
    const unsigned first = sortkey_get_first();
    const int native = first_script(first);
    const struct sortkey_language *lang =
        sortkey_language((const char *)global_settings.lang_file);
    if (native >= 0)
        add_script(native, lang && lang->first == first ? lang->letters : NULL,
                   f.present, set++);
    for (int s = 0; s < SCRIPTS; s++)
    {
        if (s != native && (s == SCRIPT_LATIN || script_present(s, f.present)))
            add_script(s, NULL, f.present, set++);
    }
    strip.room = STRIP_MAX;
    add_letters(digit_letters, set++);
    add_letter(STRIP_SPACE, set);
    add_letter(STRIP_DELETE, set);
    if (strip.pos >= strip.count)
        strip.pos = 0;
}

/* The Latin alphabets of the menu languages that have letters of their
   own, in their order, as capitals: the others type A to Z */
static const struct {
    const char *lang;
    const char *letters;
} latin_letters[] = {
    { "deutsch",      "AÄBCDEFGHIJKLMNOÖPQRSßTUÜVWXYZ" },
    { "francais",     "AÀÂÆBCÇDEÉÈÊËFGHIÎÏJKLMNOÔŒPQRSTUÙÛÜVWXYŸZ" },
    { "espanol",      "AÁBCDEÉFGHIÍJKLMNÑOÓPQRSTUÚÜVWXYZ" },
    { "galego",       "AÁBCDEÉFGHIÍJKLMNÑOÓPQRSTUÚÜVWXYZ" },
    { "basque",       "ABCDEFGHIJKLMNÑOPQRSTUVWXYZ" },
    { "catala",       "AÀBCÇDEÉÈFGHIÍÏJKLMNOÓÒPQRSTUÚÜVWXYZ" },
    { "italiano",     "AÀBCDEÈÉFGHIÌJKLMNOÒPQRSTUÙVWXYZ" },
    { "portugues",    "AÁÂÃÀBCÇDEÉÊFGHIÍJKLMNOÓÔÕPQRSTUÚVWXYZ" },
    { "portugues-brasileiro", "AÁÂÃÀBCÇDEÉÊFGHIÍJKLMNOÓÔÕPQRSTUÚVWXYZ" },
    { "polski",       "AĄBCĆDEĘFGHIJKLŁMNŃOÓPQRSŚTUVWXYZŹŻ" },
    { "czech",        "AÁBCČDĎEÉĚFGHIÍJKLMNŇOÓPQRŘSŠTŤUÚŮVWXYÝZŽ" },
    { "slovak",       "AÁÄBCČDĎEÉFGHIÍJKLĹĽMNŇOÓÔPQRŔSŠTŤUÚVWXYÝZŽ" },
    { "magyar",       "AÁBCDEÉFGHIÍJKLMNOÓÖŐPQRSTUÚÜŰVWXYZ" },
    { "romaneste",    "AĂÂBCDEFGHIÎJKLMNOPQRSȘTȚUVWXYZ" },
    { "moldoveneste", "AĂÂBCDEFGHIÎJKLMNOPQRSȘTȚUVWXYZ" },
    { "turkce",       "ABCÇDEFGĞHIİJKLMNOÖPQRSŞTUÜVWXYZ" },
    { "dansk",        "ABCDEFGHIJKLMNOPQRSTUVWXYZÆØÅ" },
    { "norsk",        "ABCDEFGHIJKLMNOPQRSTUVWXYZÆØÅ" },
    { "norsk-nynorsk", "ABCDEFGHIJKLMNOPQRSTUVWXYZÆØÅ" },
    { "svenska",      "ABCDEFGHIJKLMNOPQRSTUVWXYZÅÄÖ" },
    { "finnish",      "ABCDEFGHIJKLMNOPQRSTUVWXYZÅÄÖ" },
    { "eesti",        "ABCDEFGHIJKLMNOPQRSŠZŽTUVWÕÄÖÜXY" },
    { "latviesu",     "AĀBCČDEĒFGĢHIĪJKĶLĻMNŅOPQRSŠTUŪVWXYZŽ" },
    { "lietuviu",     "AĄBCČDEĘĖFGHIĮYJKLMNOPQRSŠTUŲŪVWXZŽ" },
    { "slovenscina",  "ABCČDEFGHIJKLMNOPQRSŠTUVWXYZŽ" },
    { "hrvatski",     "ABCČĆDĐEFGHIJKLMNOPQRSŠTUVWXYZŽ" },
    { "islenska",     "AÁBCDÐEÉFGHIÍJKLMNOÓPQRSTUÚVWXYÝZÞÆÖ" },
    { "vietnamese",   "AĂÂBCDĐEÊFGHIJKLMNOÔƠPQRSTUƯVWXYZ" },
    { "esperanto",    "ABCĈDEFGĜHĤIJĴKLMNOPQRSŜTUŬVWXYZ" },
    { "tagalog",      "ABCDEFGHIJKLMNÑOPQRSTUVWXYZ" },
};
/* Japanese names in both kanas, voiced and small ones too, which Search
   folds away; Russian with Ё */
static const char hiragana_letters[] =
    "あいうえおかがきぎくぐけげこごさざしじすずせぜそぞただちぢっつづてでとど"
    "なにぬねのはばぱひびぴふぶぷへべぺほぼぽまみむめもゃやゅゆょよらりるれろ"
    "わをんー";
static const char katakana_letters[] =
    "アイウエオカガキギクグケゲコゴサザシジスズセゼソゾタダチヂッツヅテデトド"
    "ナニヌネノハバパヒビピフブプヘベペホボポマミムメモャヤュユョヨラリルレロ"
    "ワヲンー";
static const char russian_letters[] = "АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ";
static const char name_marks[] = "-&'.()";

/* ⇧, the menu language's letters if it has its own, the Latin ones, the
   digits and the marks, then the space, the delete and ✓ */
static void make_name_strip(void)
{
    const char *lang = (const char *)global_settings.lang_file;
    const char *latin = scripts[SCRIPT_LATIN].letters;
    int set = 0;
    strip.count = 0;
    strip.pitch_font = -1;
    strip.room = STRIP_MAX;
    add_letter(STRIP_SHIFT, set++);
    const struct sortkey_language *own = sortkey_language(lang);
    const int s = own ? first_script(own->first) : -1;
    if (s == SCRIPT_KANA)
    {
        add_letters(hiragana_letters, set++);
        add_letters(katakana_letters, set++);
    }
    else if (s >= 0 && s != SCRIPT_THAI)
        add_letters(!strcmp(lang, "russian") ? russian_letters :
                    own->letters ? own->letters : scripts[s].letters, set++);
    for (size_t i = 0; i < ARRAYLEN(latin_letters); i++)
    {
        if (!strcmp(lang, latin_letters[i].lang))
            latin = latin_letters[i].letters;
    }
    add_letters(latin, set++);
    add_letters(digit_letters, set);
    add_letters(name_marks, set++);
    add_letter(STRIP_SPACE, set);
    add_letter(STRIP_DELETE, set);
    add_letter(STRIP_DONE, set);
    strip.pos = 1; /* the first letter */
}

void music_search_forget(void)
{
    remove(LETTERS_FILE);
    remove(NAMES_FILE);
    remove(NAMES_TEMP);
}

bool music_search_letters_stale(void)
{
    /* none to look through yet: not Updating Library at each start for that */
    if (!tagcache_is_usable())
        return false;
    struct letters_file f;
    read_letters(&f, true);
    if (f.version != LETTERS_VERSION)
        return true;
    /* the names' file is of another database, or its version: none, as when
       it couldn't be written, and Search goes through the database instead
       until the next Updating Library, which isn't at each start for it */
    if (!file_exists(NAMES_FILE))
        return false;
    struct names_header h;
    const int fd = open_names(&h);
    if (fd < 0)
        return true;
    close(fd);
    /* its albums' numbers of another table, as one not made as it was
       written: known once the table is made */
    return album_years_ready() && !names_albums_known(&h);
}

/* What the names' file is gathered in before it's written: the plugins' buffer,
   free on Updating Library, so that all of it goes in one write, rather than
   some 140 that began within a sector; names_buf without that */
static unsigned char *put_buf;
static int put_size;
static bool put_flushed; /* some of it written already, the header first */

/* an entry of the names' file into put_buf at *used, written out first if
   there's no room: false if writing failed */
static bool put_name(int fd, int *used, int group, int32_t idx_id, const char *name,
                     const ucschar_t *folded, int n)
{
    if (*used + ENTRY_MAX > put_size)
    {
        if (write(fd, put_buf, *used) != *used)
            return false;
        *used = 0;
        put_flushed = true;
    }
    unsigned char *p = put_buf + *used;
    /* as long as it may be, cut where a character starts */
    int name_len = strlen(name);
    if (name_len > NAME_BYTES)
    {
        name_len = NAME_BYTES;
        while (name_len > 0 && ((unsigned char)name[name_len] & 0xc0) == 0x80)
            name_len--;
    }
    p[0] = group;
    memcpy(p + 1, &idx_id, sizeof(idx_id));
    p[5] = name_len;
    memcpy(p + 6, name, name_len);
    const int folded_len = folded_bytes(folded, n, p + 7 + name_len, NAME_BYTES);
    p[6 + name_len] = folded_len;
    *used += 7 + name_len + folded_len;
    return true;
}

void music_search_letters_make(void (*progress)(int done, int total))
{
    if (!tagcache_is_usable())
        return;
    /* the names of the tag files, deleted ones too: Updating Library's bar
       stood empty for the seconds this took */
    int done = 0, total = MAX(0, tagcache_get_tag_count(tag_albumartist));
    for (int g = 0; g < GROUPS; g++)
        total += MAX(0, tagcache_get_tag_count(group_tag[g]));
    struct letters_file f;
    memset(&f, 0, sizeof(f));
    f.version = LETTERS_VERSION;
    f.commit = tagcache_get_commitid();
    struct names_header h = { LETTERS_VERSION, f.commit, 0, album_years_count(),
                              album_years_hash() };
    int names_fd = whole_file_open(NAMES_FILE);
    size_t room = 0;
    put_buf = plugin_get_buffer(&room);
    put_size = room;
    /* the albums' artists at its end */
    const size_t albums = sizeof(uint32_t)
        * (MAX(0, tagcache_get_tag_count(tag_albumartist)) + 2);
    void *albums_room = NULL;
    if (put_buf && room >= albums + sizeof(names_buf))
    {
        put_size = room - albums;
        albums_room = put_buf + put_size;
    }
    if (!put_buf || put_size < (int)sizeof(names_buf))
    {
        put_buf = names_buf;
        put_size = sizeof(names_buf);
    }
    /* the header first, its count put in at the end */
    memcpy(put_buf, &h, sizeof(h));
    int used = sizeof(h);
    put_flushed = false;
    cpu_boost(true);
    for (int g = 0; g < GROUPS; g++)
    {
        const char *name;
        int32_t idx_id;
        int names = 0;
        group_begin(g, albums_room, albums);
        while ((name = group_next(&idx_id)))
        {
            if (!(++names & 255))
                progress(done + names_in.read, total);
            if (!strcmp(name, UNTAGGED))
                continue;
            ucschar_t folded[256];
            const int n = fold_words(name, folded, ARRAYLEN(folded));
            for (int i = 0; i < n; i++)
            {
                const int bit = letter_bit(folded[i]);
                if (bit >= 0)
                    f.present[bit >> 3] |= 1 << (bit & 7);
                if (folded[i] && (i == 0 || folded[i - 1] == 0))
                    set_next(f.starts, folded[i]);
            }
            /* an album's name once for each of its album artists, by the
               number of each album, or once without the table */
            int of_name = 1;
            const int first = g == GROUP_ALBUMS ?
                album_years_find(names_in.tcs[0].result_seek, &of_name) : -1;
            if (first < 0)
                of_name = 1;
            for (int k = 0; k < of_name && names_fd >= 0 && n > 0; k++)
            {
                const int32_t id = g != GROUP_ALBUMS ? idx_id
                                   : first < 0 ? -1 : first + k;
                if (put_name(names_fd, &used, g, id, name, folded, n))
                    h.count++;
                else
                {
                    whole_file_close(names_fd, NAMES_FILE, false);
                    names_fd = -1;
                }
            }
        }
        done += names_in.read;
        group_finish();
    }

    /* whole or not at all: the header, with the count, in it last */
    bool names_ok;
    if (!put_flushed)
    {
        memcpy(put_buf, &h, sizeof(h));
        names_ok = names_fd >= 0 && write(names_fd, put_buf, used) == used;
    }
    else
        names_ok = names_fd >= 0 && write(names_fd, put_buf, used) == used
                   && lseek(names_fd, 0, SEEK_SET) == 0
                   && write(names_fd, &h, sizeof(h)) == sizeof(h);
    /* none rather than those of the database before */
    if (names_fd < 0 || !whole_file_close(names_fd, NAMES_FILE, names_ok))
        remove(NAMES_FILE);

    whole_file_write(LETTERS_FILE, &f, sizeof(f));
    /* after the files too: the iPod writes as fast as its CPU goes, and at
       30 MHz the names' 650 KB took 2 s */
    cpu_boost(false);
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

/* A letter's small form, as the ring shows them without ⇧: of the Latin
   alphabets of the menu languages, Greek and Cyrillic */
static ucschar_t small(ucschar_t c)
{
    if (c == 0x130) /* Turkish İ */
        return 'i';
    if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7)
        || (c >= 0x391 && c <= 0x3a9) || (c >= 0x410 && c <= 0x42f))
        return c + 0x20;
    if (c >= 0x400 && c <= 0x40f)
        return c + 0x50;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177)
        || (c >= 0x218 && c <= 0x21b) || c == 0x1a0 || (c >= 0x490 && c <= 0x4bf))
        return c | 1;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e))
        return c & 1 ? c + 1 : c;
    if (c == 0x178)
        return 0xff;
    if (c == 0x1af)
        return 0x1b0;
    return c;
}

/* the letter as the ring shows it and types it */
static ucschar_t name_letter(ucschar_t c)
{
    return name_shift == SHIFT_OFF ? small(c) : c;
}

static size_t name_chars(void)
{
    size_t n = 0;
    for (const char *s = name_text; *s; s++)
        n += ((unsigned char)*s & 0xc0) != 0x80;
    return n;
}

static ucschar_t name_last(size_t *at)
{
    size_t len = strlen(name_text);
    while (len > 0 && ((unsigned char)name_text[--len] & 0xc0) == 0x80)
        ;
    ucschar_t c = 0;
    if (name_text[len])
        utf8decode((const unsigned char *)name_text + len, &c);
    if (at)
        *at = len;
    return c;
}

/* room for one more letter of this one */
static bool name_room(ucschar_t c)
{
    unsigned char utf8[8];
    return name_chars() < NAME_CHARS_MAX
           && strlen(name_text) + (utf8encode(c, utf8) - utf8) <= name_max;
}

/* anything but spaces and dots, which a file's name loses at its end */
static bool name_ready(void)
{
    for (const char *s = name_text; *s; s++)
    {
        if (*s != ' ' && *s != '.')
            return true;
    }
    return false;
}

static bool name_possible(ucschar_t c)
{
    switch (c)
    {
        case STRIP_SHIFT:
        case STRIP_DELETE:
            return true;
        case STRIP_DONE:
            return name_ready();
        case STRIP_SPACE:
            /* not first, nor two together */
            return name_text[0] && name_last(NULL) != ' ' && name_room(' ');
        default:
            return name_room(c);
    }
}

/* the last letter as c, and after it, if next */
static bool name_put(size_t at, ucschar_t c, ucschar_t next)
{
    unsigned char utf8[16];
    unsigned char *e = utf8encode(c, utf8);
    if (next)
        e = utf8encode(next, e);
    const size_t len = e - utf8;
    if (at + len > name_max)
        return false;
    memcpy(name_text + at, utf8, len);
    name_text[at + len] = '\0';
    return true;
}

static int jamo_find(const uint16_t *table, int n, ucschar_t c)
{
    for (int i = 0; i < n; i++)
    {
        if (table[i] == c)
            return i;
    }
    return -1;
}

static ucschar_t jamo_join(ucschar_t a, ucschar_t b)
{
    for (size_t k = 0; k < ARRAYLEN(jamo_two); k++)
    {
        if (jamo_two[k][1] == a && jamo_two[k][2] == b)
            return jamo_two[k][0];
    }
    return 0;
}

#define HANGUL_FIRST 0xac00
#define JAMO_VOWEL   0x314f /* the first, ㅏ */
static ucschar_t syllable(int first, int vowel, int last)
{
    return HANGUL_FIRST + (first * 21 + vowel) * 28 + last;
}

/* A letter of Hangul after the last one, into a syllable with it as a phone
   types them: a vowel after a consonant, a consonant under a syllable, two of
   them into one, and a syllable's last consonant on to the next one before a
   vowel. False if it stands alone. */
static bool name_compose(ucschar_t c)
{
    size_t at;
    const ucschar_t p = name_last(&at);
    const bool vowel = c >= JAMO_VOWEL && c <= 0x3163;
    if (p >= HANGUL_FIRST && p <= 0xd7a3)
    {
        const int s = p - HANGUL_FIRST;
        const int first = s / (21 * 28), v = s / 28 % 21, last = s % 28;
        if (!vowel)
        {
            const ucschar_t under = last ? jamo_join(jamo_last[last - 1], c) : c;
            const int l = under ? jamo_find(jamo_last, ARRAYLEN(jamo_last), under) : -1;
            return l >= 0 && name_put(at, syllable(first, v, l + 1), 0);
        }
        if (!last)
        {
            const ucschar_t two = jamo_join(JAMO_VOWEL + v, c);
            return two && name_put(at, syllable(first, two - JAMO_VOWEL, 0), 0);
        }
        /* the last consonant, or the second of two, begins the next one */
        ucschar_t keep = 0, move = jamo_last[last - 1];
        for (size_t k = 0; k < ARRAYLEN(jamo_two); k++)
        {
            if (jamo_two[k][0] == move)
            {
                keep = jamo_two[k][1];
                move = jamo_two[k][2];
                break;
            }
        }
        const int f = jamo_find(jamo_first, ARRAYLEN(jamo_first), move);
        if (f < 0)
            return false;
        const int l = keep ? jamo_find(jamo_last, ARRAYLEN(jamo_last), keep) + 1 : 0;
        return name_put(at, syllable(first, v, l),
                        syllable(f, c - JAMO_VOWEL, 0));
    }
    const int f = jamo_find(jamo_first, ARRAYLEN(jamo_first), p);
    return vowel && f >= 0 && name_put(at, syllable(f, c - JAMO_VOWEL, 0), 0);
}

static void name_add(ucschar_t c)
{
    if (!name_room(c))
        return;
    if (!(c >= 0x3131 && c <= 0x3163 && name_compose(c)))
    {
        size_t len = strlen(name_text);
        name_put(len, c, 0);
    }
    if (name_shift == SHIFT_ONE)
        name_shift = SHIFT_OFF;
}

static void name_delete(void)
{
    size_t at;
    if (name_last(&at))
        name_text[at] = '\0';
    /* capitals again, for a name typed anew */
    if (!name_text[0] && name_shift == SHIFT_OFF)
        name_shift = SHIFT_ONE;
}

/* the letter at i may come next: a space and the delete always */
static bool letter_possible(int i)
{
    const ucschar_t c = strip.c[i];
    if (naming)
        return name_possible(c);
    if (!global_settings.smart_search || !next_known || c <= STRIP_LAST_SPECIAL)
        return true;
    const int bit = next_bit(sortkey_fold(c));
    return bit < 0 || (next_letters[bit >> 3] & (1 << (bit & 7)));
}

/* The one chosen, by the wheel: on to the next that may come in that way */
static void strip_step(int dir)
{
    for (int k = 0; k < strip.count; k++)
    {
        strip.pos = (strip.pos + dir + strip.count) % strip.count;
        if (letter_possible(strip.pos))
            return;
    }
}

/* and once what may come next is known, the letter nearest the one chosen that
   may, if it may not, ahead first; with none of them, as after a whole word,
   the space */
static void strip_settle(void)
{
    if (letter_possible(strip.pos))
        return;
    for (int d = 1; d <= strip.count / 2; d++)
    {
        for (int dir = 1; dir >= -1; dir -= 2)
        {
            const int i = (strip.pos + dir * d + strip.count) % strip.count;
            if (strip.c[i] > STRIP_LAST_SPECIAL && letter_possible(i))
            {
                strip.pos = i;
                return;
            }
        }
    }
    for (int i = 0; i < strip.count; i++)
    {
        if (strip.c[i] == STRIP_SPACE)
            strip.pos = i;
    }
}

/* Drawing */
#define GREY_TEXT  ui_colour(UI_COLOUR_SEARCH_SECOND)
#define FIELD_FILL ui_colour(UI_COLOUR_SEARCH_FIELD)
#define RULE       ui_colour(UI_COLOUR_SEARCH_RULE)
/* a letter no name goes on with */
#define DIM_TEXT   ui_colour(UI_COLOUR_SEARCH_DIM)

/* Neo's shapes: a rounded field 26 px high, the letters in the text font 24
   px apart, the one chosen on a rounded rectangle of 22 by 24, its keys from
   the theme's glyphs, no rule under them */
#include "gui/neo_glyphs.h"
#include "gui/modern_glyphs.h"
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
extern void lcd_alpha_bitmap_part(const unsigned char *src, int src_x,
                                  int src_y, int stride, int x, int y,
                                  int width, int height);
#endif
#define ROUNDED       (ui_metric(UI_METRIC_SELECTION_RADIUS) > 0 \
                       || ui_metric(UI_METRIC_SEARCH_RADIUS) > 0)
/* Modern's: the field's own corners, a magnifier in it, the marker's corners
   tighter, its keys' glyphs */
#define MODERN        (ui_metric(UI_METRIC_SEARCH_RADIUS) > 0)
#define FIELD_RADIUS  (MODERN ? ui_metric(UI_METRIC_SEARCH_RADIUS) \
                              : ui_metric(UI_METRIC_SELECTION_RADIUS))
#define FIELD_HEIGHT  26
#define LETTER_PITCH  24
#define MARKER_W      22
#define MARKER_H      24

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
    /* the list's rows are the theme's, or the font's cell */
    const int row = ui_metric(UI_METRIC_ROW_HEIGHT) > 0 ?
                    ui_metric(UI_METRIC_ROW_HEIGHT) : line;
    int field_h = line + 8, strip_h = line + 10;
    if (ROUNDED)
    {
        field_h = FIELD_HEIGHT + 2;
        /* the key and 2 px over and under it: with 3, Modern's list was
           2 px short of a sixth row, and those went to the letters */
        strip_h = MARKER_H + 4;
    }
    int lines = (ui.height - field_h - strip_h) / row;
    if (lines < 1)
        lines = 1;
    int spare = ui.height - field_h - strip_h - lines * row;
    if (spare > 0 && ROUNDED)
        strip_h += spare; /* the field stays at the top */
    else if (spare > 0)
    {
        field_h += spare / 2;
        strip_h += spare - spare / 2;
    }
    field_vp = strip_vp = ui;
    /* the letters in the hints' font: 13 px Medium in Neo */
    if (ROUNDED && font_find(ui_font_file(UI_FONT_HINT)) >= 0)
        strip_vp.font = font_find(ui_font_file(UI_FONT_HINT));
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
            list_vp[i].height = lines * row;
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
        if (strip.c[i] <= STRIP_LAST_SPECIAL)
            continue;
        *utf8encode(strip.c[i], utf8) = '\0';
        display->getstringsize((char *)utf8, &w, &h);
        pitch = MAX(pitch, w + 8);
    }
    if (ROUNDED)
        pitch = MAX(pitch, LETTER_PITCH);
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
    /* as wide as the selected row, or from where the rows' text starts */
    const int x = ROUNDED ? global_settings.list_text_padding : 6;
    const int w = field_vp.width - 2 * x;
    const int h = ROUNDED ? FIELD_HEIGHT : line + 2;
    const int y = ROUNDED ? 0 : (field_vp.height - h) / 2;
    int text_w, text_h;

    display->set_viewport(&field_vp);
    display->clear_viewport();
    if (ROUNDED)
        line_fill_rounded(display, x, y, w, h, FIELD_RADIUS, FIELD_FILL);
    else
        fill_rounded(display, x, y, w, h, FIELD_FILL, FIELD_FILL);

    /* its end, if it is longer than the field */
    const char *text = naming ? name_text : query;
    display->getstringsize(text, &text_w, &text_h);
    while (*text && text_w > w - (MODERN ? 40 : 20))
    {
        text = (const char *)utf8decode((const unsigned char *)text, &(ucschar_t){0});
        display->getstringsize(text, &text_w, &text_h);
    }
    int text_x = x + 8;
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (MODERN)
    {
        /* the magnifier, the text after it */
        display->set_foreground(ui_colour(UI_COLOUR_QUIET));
        display->set_drawmode(DRMODE_FG);
        lcd_alpha_bitmap_part(modern_search_field_alpha, 0, 0, MODERN_SEARCH_FIELD_W,
                              x + 8, y + (h - MODERN_SEARCH_FIELD_H) / 2,
                              MODERN_SEARCH_FIELD_W, MODERN_SEARCH_FIELD_H);
        text_x = x + 28;
    }
#endif
    display->set_foreground(text_colour);
    display->set_drawmode(DRMODE_FG);
    display->putsxy(text_x, y + (h - text_h) / 2, text);
    display->set_drawmode(DRMODE_SOLID);
    if (!in_results)
    {
        if (MODERN)
        {
            /* a line of a pixel in the accent */
            display->set_foreground(ui_colour(UI_COLOUR_ACCENT));
            display->vline(text_x + text_w + 2, y + 6, y + h - 7);
        }
        else
        {
            display->set_foreground(global_settings.lse_color);
            display->fillrect(text_x + text_w + 1, y + 3, 2, h - 6);
        }
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

/* ⇧, as an arrow up: hollow, filled for a capital, and underlined too while
   they all are */
static void draw_shift(struct screen *display, int cx, int cy)
{
    const int top = cy - 7, base = cy;
    if (name_shift == SHIFT_OFF)
    {
        display->drawline(cx, top, cx - 7, base);
        display->drawline(cx, top, cx + 7, base);
        display->hline(cx - 7, cx - 3, base);
        display->hline(cx + 3, cx + 7, base);
        display->vline(cx - 3, base, base + 5);
        display->vline(cx + 3, base, base + 5);
        display->hline(cx - 3, cx + 3, base + 5);
    }
    else
    {
        for (int y = 0; y <= 7; y++)
            display->hline(cx - y, cx + y, top + y);
        display->fillrect(cx - 3, base, 7, 6);
    }
    if (name_shift == SHIFT_LOCK)
        display->fillrect(cx - 4, base + 7, 9, 2);
}

/* ✓, two strokes two pixels thick */
static void draw_done(struct screen *display, int cx, int cy)
{
    for (int d = 0; d < 2; d++)
    {
        display->drawline(cx - 6, cy + d, cx - 2, cy + 4 + d);
        display->drawline(cx - 2, cy + 4 + d, cx + 6, cy - 5 + d);
    }
}

static void draw_strip_item(struct screen *display, int i, int cx, bool chosen)
{
    const struct font *pf = font_get(strip_vp.font);
    const int cy = strip_vp.height / 2;
    if (chosen)
    {
        /* odd, so that its middle is a pixel and not between two */
        const int w = ROUNDED ? MARKER_W : pitch | 1;
        const int h = ROUNDED ? MARKER_H : (pf->height + 2) | 1;
        const int x = cx - w / 2, y = cy - h / 2;
        if (ROUNDED)
            /* where the selection is a bar of the accent, the key has it
               only while the letters are being chosen: the field's quiet
               fill once a result is selected, as two bars read as two
               selections */
            line_fill_rounded(display, x, y, w, h, MODERN ? 3 : 5,
                              in_results && ui_metric(UI_METRIC_SELECTION_INK) ?
                              FIELD_FILL : (unsigned)global_settings.lss_color);
        else if (in_results)
            fill_rounded(display, x, y, w, h, FIELD_FILL, FIELD_FILL);
        else
            fill_rounded(display, x, y, w, h, global_settings.lss_color,
                         global_settings.lse_color);
    }
    unsigned colour = letter_possible(i) ? GREY_TEXT : DIM_TEXT;
    if (chosen && ROUNDED && !in_results && ui_metric(UI_METRIC_SELECTION_INK))
        colour = global_settings.lst_color; /* on the accent's bar */
    else if (chosen && (in_results || ROUNDED))
        colour = text_colour;
    else if (chosen)
        colour = global_settings.lst_color;
    display->set_foreground(colour);
#if defined(HAVE_LCD_COLOR) && !defined(DISABLE_ALPHA_BITMAP)
    if (MODERN && (strip.c[i] == STRIP_DONE || strip.c[i] == STRIP_DELETE))
    {
        /* Modern's keys: delete, then return; its space is draw_space()'s */
        display->set_drawmode(DRMODE_FG);
        lcd_alpha_bitmap_part(modern_search_keys_alpha, 0,
                              strip.c[i] == STRIP_DONE ? MODERN_SEARCH_KEYS_H : 0,
                              MODERN_SEARCH_KEYS_W, cx - MODERN_SEARCH_KEYS_W / 2,
                              cy - MODERN_SEARCH_KEYS_H / 2,
                              MODERN_SEARCH_KEYS_W, MODERN_SEARCH_KEYS_H);
        display->set_drawmode(DRMODE_SOLID);
        return;
    }
    if (ROUNDED && !MODERN
        && (strip.c[i] == STRIP_SPACE || strip.c[i] == STRIP_DELETE))
    {
        /* the theme's keys: delete, then space */
        display->set_drawmode(DRMODE_FG);
        lcd_alpha_bitmap_part(neo_search_keys_alpha, 0,
                              strip.c[i] == STRIP_SPACE ? NEO_SEARCH_KEYS_H : 0,
                              NEO_SEARCH_KEYS_W, cx - NEO_SEARCH_KEYS_W / 2,
                              cy - NEO_SEARCH_KEYS_H / 2,
                              NEO_SEARCH_KEYS_W, NEO_SEARCH_KEYS_H);
        display->set_drawmode(DRMODE_SOLID);
        return;
    }
#endif
    if (strip.c[i] == STRIP_SPACE)
        draw_space(display, cx, cy);
    else if (strip.c[i] == STRIP_DELETE)
        draw_delete(display, cx, cy);
    else if (strip.c[i] == STRIP_SHIFT)
        draw_shift(display, cx, cy);
    else if (strip.c[i] == STRIP_DONE)
        draw_done(display, cx, cy);
    else
    {
        unsigned char utf8[8];
        int w, h;
        *utf8encode(naming ? name_letter(strip.c[i]) : strip.c[i], utf8) = '\0';
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
    if (!ROUNDED)
        display->hline(0, strip_vp.width - 1, strip_vp.height - 1);
    display->set_foreground(text_colour);
    display->update_viewport();
}

#define NAME_KEYS_BAND 33 /* a line of hints and its margins */
static void draw(void)
{
    struct screen *display = &screens[SCREEN_MAIN];
    make_layout();
    FOR_NB_SCREENS(i)
        sb_set_title_text(naming ? name_title : (const char *)str(LANG_SEARCH),
                          Icon_NOICON, i);
    struct viewport *last = display->set_viewport(NULL);
    draw_field(display);
    draw_strip(display);
    if (naming)
    {
        /* the buttons that end it, at the bottom in a band as Settings'
           hints, of a line */
        struct viewport band = list_vp[SCREEN_MAIN];
        display->set_viewport(&list_vp[SCREEN_MAIN]);
        display->clear_viewport();
        display->update_viewport();
        if (band.height > NAME_KEYS_BAND)
        {
            band.y += band.height - NAME_KEYS_BAND;
            band.height = NAME_KEYS_BAND;
        }
        list_draw_hint(display, &band, str(LANG_NAME_KEYS));
        display->update_viewport();
    }
    display->set_viewport(last);
    if (!naming)
        gui_synclist_draw(&lists);
}

/* the field and the letters, as typed, before the search of it */
static void draw_query(void)
{
    struct screen *display = &screens[SCREEN_MAIN];
    struct viewport *last = display->set_viewport(NULL);
    draw_field(display);
    draw_strip(display);
    display->set_viewport(last);
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
    {
        strcpy(searched, query);
        strip_settle();
    }
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
              && !track_protected_splash(path) && warn_on_pl_erase();
    /* Now Playing comes later than the slide's usual wait, as from Music */
    if (ok)
        transition_wait(HZ);
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
    transition_begin(TRANSITION_FORWARD);
    if (g == GROUP_SONGS)
    {
        if (play_song(index))
            return GO_TO_WPS;
        transition_cancel();
        return 0;
    }
    const struct result *r = &results[group_first[g] + index];
    /* an album by its album artist too, as Albums has the albums of a name
       apart */
    const int no = g == GROUP_ALBUMS && global_settings.album_artists ?
                   album_of(r) : -1;
    int screen = g == GROUP_ALBUMS ?
        root_menu_go_to_album(pool + r->name, no < 0 ? -1 : album_years_artist(no))
        : root_menu_go_to_name(group_tag[g], pool + r->name);
    if (screen < 0)
    {
        transition_cancel();
        return 0;
    }
    return screen;
}

/* a held Select on a song: the menu a song has in Music's lists, Add to
   On-The-Go first; the screen to go to, or 0 for this one */
static int song_menu(int index)
{
    char path[MAX_PATH];
    struct tagcache_search tcs;
    const struct result *r = &results[group_first[GROUP_SONGS] + index];
    if (!tagcache_search(&tcs, tag_filename))
        return 0;
    const bool found = tagcache_retrieve(&tcs, r->idx_id, tag_filename,
                                         path, sizeof(path));
    tagcache_search_finish(&tcs);
    if (!found)
        return 0;
    onplay_set_title(pool + r->name);
    switch (onplay(path, FILE_ATTR_AUDIO, CONTEXT_STD, false,
                   ONPLAY_NO_CUSTOMACTION))
    {
        case ONPLAY_START_PLAY:
            return GO_TO_WPS;
        case ONPLAY_MAINMENU:
            return GO_TO_ROOT;
        default:
            return 0;
    }
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

/* See All of albums as Albums has them: each in two lines, its year, and its
   album artist with Artists in Albums, under its name, and with its cover */
static const char *album_row_subline(int row, void *data, char *buf, size_t size)
{
    (void)data;
    const int no = album_of(&results[group_first[GROUP_ALBUMS] + row]);
    if (no < 0)
        return NULL;
    const bool apart = global_settings.album_artists;
    return tagtree_album_subline(no, apart ? album_years_year(no)
                                           : album_years_name_year_of(no),
                                 apart, NULL, buf, size);
}

static int album_row_cover(int row, void *data, const fb_data **pixels)
{
    (void)data;
    return list_covers_get(album_of(&results[group_first[GROUP_ALBUMS] + row]),
                           pixels);
}

static void album_rows_before_draw(struct gui_synclist *list)
{
    list_covers_frame();
    list_covers_draw_soon(list);
}

/* Quick Scroll's letter of a row, as the group's names go by them: by the
   name, as an album's or artist's row hasn't its place in the database's
   files, which a Sort field would have put elsewhere */
static const char *group_row_initial(int row, void *data, char *buf, size_t size)
{
    const char *name = pool + results[group_first[(intptr_t)data] + row].name;
    const ucschar_t letter = sortkey_initial(sortkey_start(name, true));
    if (size < 5)
        return NULL;
    *utf8encode(letter, (unsigned char *)buf) = '\0';
    return buf;
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
    /* the covers' memory, if turned on as music played, which has stopped */
    list_covers_apply();
    /* of the albums' numbers in the table of years, if they're known */
    const bool numbered = g == GROUP_ALBUMS && albums_known && album_years_count() > 0;
    const bool covers = numbered && list_covers_on();
    if (numbered && (global_settings.album_artists || covers))
        gui_synclist_set_subline_callback(&list, album_row_subline);
    if (covers)
    {
        gui_synclist_set_cover_callback(&list, album_row_cover);
        gui_synclist_set_before_draw(&list, album_rows_before_draw);
        list_covers_opened();
    }
    gui_synclist_set_initial_callback(&list, group_row_initial);
    gui_synclist_set_nb_items(&list, group_count[g]);
    gui_synclist_select_item(&list, 0);
    bool redraw = true;
    while (ret == 0)
    {
        if (redraw)
            gui_synclist_draw(&list);
        redraw = false;
        int timeout = list_do_action_timeout(&list, HZ / 2);
        if (covers)
            timeout = list_covers_timeout(timeout);
        int action = get_action(CONTEXT_TREE, timeout);
        /* the covers the reader reads as the wheel rests, drawn as they
           come */
        if (covers && action != ACTION_NONE)
            list_covers_input();
        if (covers && list_covers_poll())
            redraw = true;
        if (gui_synclist_do_button(&list, &action))
            continue;
        switch (action)
        {
            case ACTION_STD_OK:
                ret = open_result(g, gui_synclist_get_sel_pos(&list));
                redraw = true;
                break;
            case ACTION_STD_CONTEXT:
                if (g == GROUP_SONGS)
                {
                    names_let_go();
                    ret = song_menu(gui_synclist_get_sel_pos(&list));
                    if (ret == 0)
                        names_hold();
                    redraw = true;
                }
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

/* Opened from Music, Search starts with nothing typed; come back to from what
   it found, as from Now Playing, it's as it was left */
void music_search_new(void)
{
    query[0] = '\0';
    searched[0] = '\0';
    have_results = false;
    in_results = false;
    chosen_row = 0;
    strip.pos = 0;
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
    list_pin_groups(&lists);
    lists.callback_draw_item = menu_draw_value_line;
    /* whether the names' file is there for this database, and all of it in
       memory while Search is open */
    struct names_header h;
    const int names_fd = open_names(&h);
    if (names_fd >= 0)
        close(names_fd);
    names_hold();
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
        /* the letters, where ⏮/⏭ type, don't take them with the screen dark */
        wake_screen_set_typing(!in_results);
        int action = get_action(CONTEXT_TREE, timeout);
        wake_screen_set_typing(false);
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
           the press is of a row that may be another now; and Group by Album
           Artist switched, other artists are found */
        if (have_results && !results_current())
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
                    /* what it opens may take the plugins' buffer */
                    names_let_go();
                    ret = open_row(chosen_row);
                    if (ret == 0)
                        names_hold();
                    redraw = true;
                    break;
                case ACTION_STD_CONTEXT:
                {
                    int index;
                    chosen_row = gui_synclist_get_sel_pos(&lists);
                    if (row_group(chosen_row, &index) == GROUP_SONGS && index >= 0
                        && !see_all(GROUP_SONGS, index))
                    {
                        names_let_go();
                        ret = song_menu(index);
                        if (ret == 0)
                            names_hold();
                        redraw = true;
                    }
                    break;
                }
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
                strip_step(1);
                redraw_letters = true;
                break;
            case ACTION_STD_PREV:
            case ACTION_STD_PREVREPEAT:
                strip_step(-1);
                redraw_letters = true;
                break;
            case ACTION_STD_OK:
                if (!letter_possible(strip.pos))
                    break;
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
            {
#ifdef BUTTON_LEFT
                /* holding Previous deletes all of it, a letter at a time
                   being slow */
                int button;
                get_action_statuscode(&button);
                if (action == ACTION_UNKNOWN && button == (BUTTON_LEFT | BUTTON_REPEAT))
                {
                    query[0] = '\0';
                    break;
                }
#endif
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                    ret = GO_TO_ROOT;
                break;
            }
        }
        if (strcmp(before, query))
        {
            /* A letter more, among the names found, is quick: at once, and the
               letters that may follow with it. So is the whole library through
               its names' file, if a press doesn't stop it. Without that, once
               the typing pauses, with all the letters meanwhile. */
            if (narrows(searched) || !query[0])
                run_search(false);
            else if (names_commit == tagcache_get_commitid())
            {
                /* the letter in the field first, as the search takes a moment;
                   all of it, as the letters that may follow are wanted as the
                   wheel turns on, which stopped it */
                draw_query();
                run_search(false);
            }
            else
            {
                search_waits = true;
                search_due = current_tick + SEARCH_PAUSE;
                next_known = false;
                if (kept_letters_back())
                    strip_settle();
            }
            redraw = true;
        }
    }

    names_let_go();
    if (!was_wide)
        menu_set_wide(false);
    if (ret == GO_TO_WPS)
        pop_current_activity_without_refresh();
    else
        pop_current_activity();
    return ret;
}

int music_search_name(const char *title, char *buf, size_t size,
                      size_t max_bytes, bool (*taken)(const char *name))
{
    if (size < 2)
        return 0;
    /* Search's, if this is opened from it, as from a song's menu there */
    const int search_pos = strip.pos;
    const bool search_in_results = in_results;
    naming = true;
    name_text = buf;
    name_max = MIN(size - 1, max_bytes);
    name_text[name_max] = '\0';
    name_title = title;
    /* capitals for a name typed anew, as one starts; small for one renamed */
    name_shift = buf[0] ? SHIFT_OFF : SHIFT_ONE;
    in_results = false;
    const bool was_wide = menu_wide();
    if (!was_wide)
        menu_set_wide(true);
    make_name_strip();
    make_layout();

    int ret = -2;
    bool redraw = true, redraw_letters = false;
    while (ret == -2)
    {
        if (redraw)
            draw();
        else if (redraw_letters)
            draw_letters();
        redraw = redraw_letters = false;
        wake_screen_set_typing(true);
        int action = get_action(CONTEXT_TREE, HZ / 2);
        wake_screen_set_typing(false);
        char before[MAX_PATH];
        strmemccpy(before, name_text, sizeof(before));
        const int shift_before = name_shift;
        bool done = false;
        switch (action)
        {
            case ACTION_REDRAW:
                strip.pitch_font = -1;
                redraw = true;
                break;
            case ACTION_STD_NEXT:
            case ACTION_STD_NEXTREPEAT:
                strip_step(1);
                redraw_letters = true;
                break;
            case ACTION_STD_PREV:
            case ACTION_STD_PREVREPEAT:
                strip_step(-1);
                redraw_letters = true;
                break;
            case ACTION_STD_OK:
            {
                const ucschar_t c = strip.c[strip.pos];
                if (!letter_possible(strip.pos))
                    break;
                if (c == STRIP_SHIFT)
                    name_shift = (name_shift + 1) % 3;
                else if (c == STRIP_DELETE)
                    name_delete();
                else if (c == STRIP_SPACE)
                    name_add(' ');
                else if (c == STRIP_DONE)
                    done = true;
                else
                    name_add(name_letter(c));
                break;
            }
            case ACTION_LIST_SKIPNEXT:
                if (name_possible(STRIP_SPACE))
                    name_add(' ');
                break;
            case ACTION_LIST_SKIPPREV:
                name_delete();
                break;
            case ACTION_LIST_PLAYPAUSE:
                done = name_ready();
                break;
            case ACTION_STD_CANCEL:
            case ACTION_STD_MENU:
                ret = 0;
                break;
            default:
            {
#ifdef BUTTON_LEFT
                /* holding Previous deletes all of it */
                int button;
                get_action_statuscode(&button);
                if (action == ACTION_UNKNOWN && button == (BUTTON_LEFT | BUTTON_REPEAT))
                {
                    name_text[0] = '\0';
                    name_shift = SHIFT_ONE;
                    break;
                }
#endif
#ifdef BUTTON_RIGHT
                /* holding Next is ⇧, once a hold, as it's pressed on the ring */
                if (action == ACTION_UNKNOWN && button == (BUTTON_RIGHT | BUTTON_REPEAT))
                {
                    name_shift = (name_shift + 1) % 3;
                    action_wait_for_release();
                    break;
                }
#endif
                if (default_event_handler(action) == SYS_USB_CONNECTED)
                    ret = -1;
                break;
            }
        }
        if (done)
        {
            /* without the spaces and dots at its end, which FAT drops */
            size_t len = strlen(name_text);
            while (len > 0 && (name_text[len - 1] == ' ' || name_text[len - 1] == '.'))
                name_text[--len] = '\0';
            if (taken && taken(name_text))
            {
                splash(HZ, ID2P(LANG_NAME_TAKEN));
                redraw = true;
            }
            else
                ret = 1;
        }
        if (strcmp(before, name_text) || shift_before != name_shift)
            redraw = true;
    }

    naming = false;
    /* Search's ring again, for its screen under this one */
    make_strip();
    strip.pos = search_pos < strip.count ? search_pos : 0;
    in_results = search_in_results;
    if (!was_wide)
        menu_set_wide(false);
    return ret;
}

#endif /* HAVE_TAGCACHE */
