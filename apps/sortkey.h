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
   Latin names first, a letter with an accent with its letter, then those of
   the other alphabets, then names starting with a digit, and last those of
   marks alone. Case doesn't count in any alphabet, "Ё" goes with "Е", and
   numbers go by their value. Names the same by all that go by their bytes. */

/* What the commit sorts by goes with the database: a change of these rules
   sorts it again at the next boot */
#define SORTKEY_RULES_VERSION 1

/* A letter as names are searched and sorted by: small, without its accents,
   so that "é" is found by "e" and goes with it. 0 for a mark between words;
   SORTKEY_SKIP for one left out, as an accent written apart. */
#define SORTKEY_SKIP 0xffff
ucschar_t sortkey_fold(ucschar_t c);

/* Where a name sorts from: past its article, if article, and the marks
   before its first letter or digit. A Sort field, as Music takes it, keeps
   its article. A name of marks alone sorts from its start. */
const char *sortkey_start(const char *name, bool article);

/* The group of a name, from its start: in this order */
enum { SORTKEY_LATIN, SORTKEY_LETTERS, SORTKEY_DIGITS, SORTKEY_MARKS };
int sortkey_group(const char *start);

/* Two names from their starts, of one group: 0 when they're the same by the
   rules, if not by their bytes */
int sortkey_compare_from(const char *a, const char *b);

/* Two names, wholly: 0 only for the same bytes */
int sortkey_compare(const char *a, const char *b);

#endif /* _SORTKEY_H_ */
