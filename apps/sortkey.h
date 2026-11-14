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
#ifndef _SORTKEY_H_
#define _SORTKEY_H_

#include <stdbool.h>
#include "rbunicode.h"

/* The order of names in Music, as Music on the Mac has it: a leading "The ",
   "A " or "An " and the marks before the first letter or digit don't count;
   names in the menu language's own alphabet first, if it has one (those the
   database was last sorted with, sortkey_get_first(): the language's from
   the next sort after it changed), then Latin names, a letter with an accent
   with its letter, then those of the other alphabets, then names starting
   with a digit, and last those of marks alone. Case doesn't count in any
   alphabet, "Ё" goes with "Е", and numbers go by their value. Names the same
   by all that go by their bytes. */

/* What the commit sorts by goes with the database, and so do the letters it
   notes from it for Quick Scroll (sortkey_initial()): a change of these rules
   sorts it again at the next boot */
#define SORTKEY_RULES_VERSION 3

/* The alphabets of letters, in this order after the Latin one, as Unicode's
   collation table has them: a letter past fold()'s Latin ones, as the
   Icelandic "Þ", is one of its own after z */
enum sortkey_alphabet {
    SORTKEY_ALPHABET_LATIN, SORTKEY_ALPHABET_PAST_Z, SORTKEY_ALPHABET_GREEK,
    SORTKEY_ALPHABET_CYRILLIC, SORTKEY_ALPHABET_ARMENIAN,
    SORTKEY_ALPHABET_HEBREW, SORTKEY_ALPHABET_ARABIC, SORTKEY_ALPHABET_THAI,
    SORTKEY_ALPHABET_GEORGIAN, SORTKEY_ALPHABET_HANGUL, SORTKEY_ALPHABET_KANA,
    SORTKEY_ALPHABET_HAN, SORTKEY_ALPHABET_OTHER,
};

/* The alphabet of a letter, as sortkey_initial() gives it */
enum sortkey_alphabet sortkey_alphabet_of(ucschar_t letter);

/* The alphabets whose names go before the Latin ones, a bit each, 0 for
   none: one, or two as Japanese's kana and then Han, as their language
   reads them. Music on the Mac has a language's own first the same way. */
#define SORTKEY_FIRST(alphabet) (1u << (alphabet))

/* The menu languages written in an alphabet of their own: their lists have
   it first, and Search's ring of letters has its letters first */
struct sortkey_language {
    const char *lang;       /* as global_settings.lang_file names it */
    unsigned first;         /* SORTKEY_FIRST()'s */
    const char *letters;    /* the ring's, or NULL for the alphabet's own */
};
/* the menu language's, NULL for one written in Latin letters */
const struct sortkey_language *sortkey_language(const char *lang);
/* and the alphabets it has first */
unsigned sortkey_language_first(const char *lang);

/* Those the database's files were sorted with, which the lists go by so
   that all of them agree with the files: noted by tagcache.c with the files,
   and changed when it sorts them again for another menu language */
void sortkey_set_first(unsigned first);
unsigned sortkey_get_first(void);

/* A letter as names are searched and sorted by: small, without its accents,
   so that "é" is found by "e" and goes with it. 0 for a mark between words;
   SORTKEY_SKIP for one left out, as an accent written apart. */
#define SORTKEY_SKIP 0xffff
ucschar_t sortkey_fold(ucschar_t c);

/* Where a name sorts from: past its article, if article, and the marks
   before its first letter or digit. A Sort field, as Music takes it, keeps
   its article. A name of marks alone sorts from its start. */
const char *sortkey_start(const char *name, bool article);

/* The group of a name, from its start: in this order, the first for the
   alphabets first (sortkey_get_first()) */
enum { SORTKEY_FIRST_LETTERS, SORTKEY_LATIN, SORTKEY_LETTERS, SORTKEY_DIGITS,
       SORTKEY_MARKS };
int sortkey_group(const char *start);

/* Two names from their starts, of one group: 0 when they're the same by the
   rules, if not by their bytes */
int sortkey_compare_from(const char *a, const char *b);

/* the same with other alphabets first than the database's, as a commit sorts
   by the menu language's */
int sortkey_group_with(const char *start, unsigned first);
int sortkey_compare_from_with(const char *a, const char *b, unsigned first);

/* Two names, wholly: 0 only for the same bytes */
int sortkey_compare(const char *a, const char *b);

/* A letter as lists show it: a capital, of the alphabets that have them */
ucschar_t sortkey_capital(ucschar_t c);

/* The letter a name is listed under, from its start (sortkey_start()), as
   Quick Scroll shows it over a list: its first letter as a capital, without
   its accents; of Hangul the letter its syllable begins with, and of the kana
   the first of its row, as names starting with any of those go together; '#'
   for a name starting with a digit, or of marks alone. */
ucschar_t sortkey_initial(const char *start);

#endif /* _SORTKEY_H_ */
