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

/* Writes firmware/common/nfc_table.h to stdout: what utf8proc's composition
 * pass reads from its tables, for firmware/common/nfc.c. run.sh builds and
 * runs it; it fails where the data breaks an assumption nfc.c makes. */
#include "utf8proc.c" /* lib/utf8proc: its tables are static */
#include <stdio.h>
#include <stdlib.h>

#define LIMIT 0x20000 /* nfc.c looks no higher for marks and seconds */
#define MAX_PAIRS 4096

struct pair { int first, second, both; };
static struct pair pairs[MAX_PAIRS];
static int npairs;
static unsigned char pages[LIMIT >> 11];

static void fail(const char *what, int cp)
{
    fprintf(stderr, "gen_table: %s (U+%04X)\n", what, cp);
    exit(1);
}

static int valid(int cp)
{
    return cp < 0xd800 || cp > 0xdfff;
}

static int utf8_length(int cp)
{
    return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

static void mark(int cp)
{
    if (cp < 0x300 || cp >= LIMIT)
        fail("a mark or a second outside U+0300..U+1FFFF", cp);
    pages[cp >> 11] |= 1 << (cp >> 8 & 7);
}

/* utf8proc_normalize_utf32()'s search of a starter's seconds */
static int lookup(const utf8proc_property_t *starter, int second)
{
    int idx = starter->comb_index, len = starter->comb_length;
    if (idx >= 0x3ff || !unsafe_get_property(second)->comb_issecond ||
        second > utf8proc_combinations_second[idx + len - 1])
        return -1;
    for (int off = 0; off < len; off++) {
        if (second < utf8proc_combinations_second[idx + off])
            break;
        if (second == utf8proc_combinations_second[idx + off])
            return utf8proc_combinations_combined[idx + off];
    }
    return -1;
}

static int by_first(const void *a, const void *b)
{
    const struct pair *p = a, *q = b;
    return p->first != q->first ? p->first - q->first : p->second - q->second;
}

static void collect_pairs(void)
{
    for (int cp = 0; cp < 0x110000; cp++) {
        if (!valid(cp))
            continue;
        const utf8proc_property_t *p = unsafe_get_property(cp);
        if (p->comb_index >= 0x3ff)
            continue;
        for (int off = 0; off < p->comb_length; off++) {
            int second = utf8proc_combinations_second[p->comb_index + off];
            int both = lookup(p, second);
            if (both != utf8proc_combinations_combined[p->comb_index + off])
                fail("a pair utf8proc's own search does not reach", cp);
            if (!valid(both) || both >= 0x110000)
                fail("a composition that is no code point", cp);
            if (utf8_length(both) > utf8_length(cp) + utf8_length(second))
                fail("a composition longer than its halves", cp);
            if ((cp | second) < 0x10000 && both >= 0x10000)
                fail("a wide composition of two narrow halves", cp);
            if (npairs == MAX_PAIRS)
                fail("too many pairs", cp);
            pairs[npairs++] = (struct pair){ cp, second, both };
            mark(second);
        }
    }
    qsort(pairs, npairs, sizeof(pairs[0]), by_first);
    for (int i = 1; i < npairs; i++)
        if (!by_first(&pairs[i - 1], &pairs[i]))
            fail("a pair listed twice", pairs[i].first);
}

static void print_pairs(int wide)
{
    int n = 0;
    for (int i = 0; i < npairs; i++)
        n += ((pairs[i].first | pairs[i].second) >= 0x10000) == wide;
    printf("\n/* first, second, their composition; ascending */\n"
           "#define NFC_%sPAIRS %d\n"
           "static const uint%d_t nfc_%spairs[NFC_%sPAIRS][3] = {\n",
           wide ? "WIDE_" : "", n, wide ? 32 : 16,
           wide ? "wide_" : "", wide ? "WIDE_" : "");
    for (int i = 0, col = 0; i < npairs; i++) {
        if (((pairs[i].first | pairs[i].second) >= 0x10000) != wide)
            continue;
        printf(wide ? "%s{0x%05X,0x%05X,0x%05X}," : "%s{0x%04X,0x%04X,0x%04X},",
               col ? " " : "    ", pairs[i].first, pairs[i].second,
               pairs[i].both);
        if (++col == (wide ? 2 : 3)) {
            printf("\n");
            col = 0;
        }
        if (!--n && col)
            printf("\n");
    }
    printf("};\n");
}

static void print_classes(void)
{
    int nruns = 0;
    printf("\n/* Runs of one combining class, ascending:\n"
           "   first << 15 | (length - 1) << 8 | class */\n"
           "static const uint32_t nfc_runs[] = {\n");
    for (int cp = 0; cp < 0x110000; ) {
        int class = valid(cp) ? unsafe_get_property(cp)->combining_class : 0;
        int len = 1;
        if (!class) {
            cp++;
            continue;
        }
        while (len < 128 && cp + len < 0x110000 && valid(cp + len) &&
               unsafe_get_property(cp + len)->combining_class == class)
            len++;
        for (int i = 0; i < len; i++)
            mark(cp + i);
        printf("%s0x%08X,%s", nruns % 6 ? " " : "    ",
               (unsigned)cp << 15 | (len - 1) << 8 | class,
               nruns % 6 == 5 ? "\n" : "");
        nruns++;
        cp += len;
    }
    printf("%s};\n#define NFC_RUNS %d\n", nruns % 6 ? "\n" : "", nruns);
}

int main(void)
{
    collect_pairs();
    /* Hangul's vowels and finals compose by arithmetic in nfc.c */
    for (int cp = UTF8PROC_HANGUL_VBASE; cp <= UTF8PROC_HANGUL_TBASE +
                                               UTF8PROC_HANGUL_TCOUNT - 1; cp++)
        mark(cp);

    printf("/* Made by tools/nfctest/gen_table.c from lib/utf8proc %s (Unicode %s):\n"
           " * do not edit, run tools/nfctest/run.sh. For firmware/common/nfc.c.\n"
           " *\n"
           " * The data is utf8proc's, which derives it from the Unicode Character\n"
           " * Database (Copyright (c) 1991-2007 Unicode, Inc., distributed under the\n"
           " * Terms of Use in http://www.unicode.org/copyright.html; modified here);\n"
           " * see lib/utf8proc/LICENSE.md for both licenses in full.\n"
           " *\n"
           " *  Copyright (c) 2014-2021 Steven G. Johnson, Jiahao Chen, Peter Colberg, Tony Kelman, Scott P. Jones, and other contributors.\n"
           " *  Copyright (c) 2009 Public Software Group e. V., Berlin, Germany\n"
           " *\n"
           " *  Permission is hereby granted, free of charge, to any person obtaining a\n"
           " *  copy of this software and associated documentation files (the \"Software\"),\n"
           " *  to deal in the Software without restriction, including without limitation\n"
           " *  the rights to use, copy, modify, merge, publish, distribute, sublicense,\n"
           " *  and/or sell copies of the Software, and to permit persons to whom the\n"
           " *  Software is furnished to do so, subject to the following conditions:\n"
           " *\n"
           " *  The above copyright notice and this permission notice shall be included in\n"
           " *  all copies or substantial portions of the Software.\n"
           " *\n"
           " *  THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
           " *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
           " *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n"
           " *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
           " *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING\n"
           " *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER\n"
           " *  DEALINGS IN THE SOFTWARE.\n"
           " */\n"
           "#define NFC_UTF8PROC_VERSION \"%s\"\n",
           utf8proc_version(), utf8proc_unicode_version(), utf8proc_version());

    print_pairs(0);
    print_pairs(1);
    print_classes();

    printf("\n/* A bit for each 256 code points below U+20000 that hold a mark or a\n"
           "   second half of a pair; there are none elsewhere */\n"
           "static const uint8_t nfc_pages[%d] = {", (int)sizeof(pages));
    for (unsigned i = 0; i < sizeof(pages); i++)
        printf("%s0x%02X,", i % 12 ? " " : "\n    ", pages[i]);
    printf("\n};\n");
    return 0;
}
