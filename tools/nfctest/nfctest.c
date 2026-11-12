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

/* firmware/common/nfc.c against the utf8proc call it replaced: both normalize
 * the same strings and every byte must agree. run.sh builds and runs it.
 *
 *   nfctest classes         the combining class of every code point
 *   nfctest seconds         every code point, then every second half
 *   nfctest firsts          every first half, then every code point
 *   nfctest between         a pair with every code point between its halves
 *   nfctest marks           a pair with every two marks between its halves
 *   nfctest random N SEED   N strings of 1-24 code points, a quarter of
 *                           them with a byte damaged
 *   nfctest edge            every malformed sequence; the longest strings
 *   nfctest readonly N SEED strings that stay as they are must not be written
 *   nfctest corpus FILE     strings divided by NUL bytes
 *
 * Not compared: 2048 code points and more, where the old call read past its
 * buffer. A tag can be that long only as 2048-2167 bytes of mostly ASCII
 * (id3v2buf and id3v1buf together); the database keeps its first 520. */
#include "utf8proc.c" /* lib/utf8proc: its tables are static */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../../firmware/common/nfc.c"

/* ---- the old utf8_normalize() of firmware/common/unicode.c ---- */
static utf8proc_int32_t normbuf[2048];

static void old_normalize(char *string)
{
    utf8proc_ssize_t result, orig;

    if (!string || !*string)
         return;

    orig = strlen(string);
    result = utf8proc_decompose((const utf8proc_uint8_t *)string, 0, normbuf, sizeof(normbuf)/4 -1, UTF8PROC_NULLTERM);
    if (result > 0) {
        result = utf8proc_reencode(normbuf, result, UTF8PROC_NULLTERM|UTF8PROC_COMPOSE|UTF8PROC_STABLE);
        if (result > 0 && result <= orig && strcmp((char*)normbuf, string))
            strcpy(string, (char*)normbuf);
    }
}

#define MAXLEN 8192
static long ntotal, nchanged, ndiff;

static void hex(const char *name, const char *s)
{
    fprintf(stderr, "  %s:", name);
    for (; *s; s++)
        fprintf(stderr, " %02X", (unsigned char)*s);
    fprintf(stderr, "\n");
}

/* s: fewer than 2048 code points, at most MAXLEN - 9 bytes */
static void check(const char *s)
{
    static char old[MAXLEN], new[MAXLEN];
    size_t len = strlen(s);

    memcpy(old, s, len + 1);
    memcpy(new, s, len + 1);
    memset(new + len + 1, 0xa5, 8);
    old_normalize(old);
    utf8_normalize(new);
    ntotal++;
    nchanged += strcmp(old, s) != 0;
    if (strcmp(old, new) || memcmp(new + len + 1, "\xa5\xa5\xa5\xa5\xa5\xa5\xa5\xa5", 8)) {
        if (ndiff++ < 20) {
            hex("string", s);
            hex("utf8proc", old);
            hex("nfc.c", new);
        }
    }
}

static int valid(int cp)
{
    return cp >= 0 && cp < 0x110000 && (cp < 0xd800 || cp > 0xdfff);
}

static char *put(char *p, int cp)
{
    return p + utf8proc_encode_char(cp, (utf8proc_uint8_t *)p);
}

static void check_cps(const int *cps, int n)
{
    char s[64], *p = s;
    for (int i = 0; i < n; i++)
        p = put(p, cps[i]);
    *p = 0;
    check(s);
}

/* ---- what utf8proc's tables hold ---- */
static int firsts[4096], nfirsts;   /* table starters and Hangul L, LV, LVT */
static int seconds[1024], nseconds; /* comb_issecond and Hangul V, T */
static int marks[2048], nmarks;     /* non-zero combining class */

static void collect(void)
{
    for (int cp = 0; cp < 0x110000; cp++) {
        if (!valid(cp))
            continue;
        const utf8proc_property_t *p = unsafe_get_property(cp);
        if (p->comb_index < 0x3ff)
            firsts[nfirsts++] = cp;
        if (p->comb_issecond)
            seconds[nseconds++] = cp;
        if (p->combining_class)
            marks[nmarks++] = cp;
    }
    for (int cp = 0x1100; cp < 0x1100 + 19; cp++)
        firsts[nfirsts++] = cp;
    for (int cp = 0xac00; cp < 0xac00 + 19 * 21 * 28; cp += 28) {
        firsts[nfirsts++] = cp;     /* LV */
        firsts[nfirsts++] = cp + 1 + cp % 27; /* an LVT */
    }
    /* one before and after the vowels and the finals too */
    for (int cp = 0x1160; cp <= 0x11c3; cp++)
        seconds[nseconds++] = cp;
}

static uint64_t rng;
static unsigned rnd(unsigned n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (rng >> 16) % n;
}

static int random_cp(void)
{
    int cp;
    switch (rnd(12)) {
    case 0: case 1: return firsts[rnd(nfirsts)];
    case 2: case 3: return seconds[rnd(nseconds)];
    case 4: case 5: return marks[rnd(nmarks)];
    case 6: return 0x1100 + rnd(0x100);          /* Hangul letters */
    case 7: return 0xac00 + rnd(19 * 21 * 28);
    case 8: return 0x20 + rnd(0x5f);
    case 9: return 0x400 + rnd(0x100);
    case 10: return 0x3040 + rnd(0x6000);        /* kana, CJK */
    default:
        do
            cp = rnd(0x110000);
        while (!valid(cp) || !cp);
        return cp;
    }
}

/* A first half, some marks, then a second half of its own, twice at times */
static char *random_cluster(char *p, int *n)
{
    int first = firsts[rnd(nfirsts)];
    const utf8proc_property_t *prop = unsafe_get_property(first);
    int second = prop->comb_index < 0x3ff
        ? utf8proc_combinations_second[prop->comb_index + rnd(prop->comb_length)]
        : seconds[rnd(nseconds)];

    p = put(p, first);
    for (int i = rnd(4); i > 1 && *n > 3; i--, --*n)
        p = put(p, marks[rnd(nmarks)]);
    p = put(p, second);
    *n -= 2;
    if (*n > 0 && !rnd(3)) {
        p = put(p, rnd(2) ? second : seconds[rnd(nseconds)]);
        --*n;
    }
    return p;
}

static int random_string(char *s)
{
    char *p = s;
    int n = 1 + rnd(24);
    while (n > 0) {
        if (n > 1 && !rnd(4))
            p = random_cluster(p, &n);
        else {
            p = put(p, random_cp());
            n--;
        }
    }
    *p = 0;
    return p - s;
}

/* ---- the tests ---- */
static void test_classes(void)
{
    for (int cp = 0; cp < 0x110000; cp++) {
        int old = valid(cp) ? unsafe_get_property(cp)->combining_class : 0;
        ntotal++;
        if (combining_class(cp) != old && ndiff++ < 20)
            fprintf(stderr, "  U+%04X: class %d, utf8proc %d\n", cp,
                    combining_class(cp), old);
    }
}

static void test_seconds(void)
{
    for (int cp = 1; cp < 0x110000; cp++)
        for (int i = 0; valid(cp) && i < nseconds; i++)
            check_cps((int[]){ cp, seconds[i] }, 2);
}

static void test_firsts(void)
{
    for (int i = 0; i < nfirsts; i++)
        for (int cp = 1; cp < 0x110000; cp++)
            if (valid(cp))
                check_cps((int[]){ firsts[i], cp }, 2);
}

/* A pair for each class a second half has, and Hangul's two steps */
static int probes[64][2], nprobes;

static void collect_probes(void)
{
    int seen[256] = { 0 };
    for (int i = 0; i < nfirsts; i++) {
        const utf8proc_property_t *p = unsafe_get_property(firsts[i]);
        for (int off = 0; p->comb_index < 0x3ff && off < p->comb_length; off++) {
            int second = utf8proc_combinations_second[p->comb_index + off];
            int class = unsafe_get_property(second)->combining_class;
            if (seen[class]++ < 2 && nprobes < 60) {
                probes[nprobes][0] = firsts[i];
                probes[nprobes++][1] = second;
            }
        }
    }
    probes[nprobes][0] = 0x1100;
    probes[nprobes++][1] = 0x1161;
    probes[nprobes][0] = 0xac00;
    probes[nprobes++][1] = 0x11a8;
}

static void test_between(void)
{
    for (int i = 0; i < nprobes; i++)
        for (int cp = 1; cp < 0x110000; cp++)
            if (valid(cp))
                check_cps((int[]){ probes[i][0], cp, probes[i][1] }, 3);
}

static void test_marks(void)
{
    for (int i = 0; i < nprobes; i += 3)
        for (int a = 0; a < nmarks; a++)
            for (int b = 0; b < nmarks; b++)
                check_cps((int[]){ probes[i][0], marks[a], marks[b],
                                   probes[i][1] }, 4);
}

static void test_random(long n)
{
    char s[128];
    while (n--) {
        int len = random_string(s);
        switch (rnd(8)) {
        case 0: s[rnd(len)] = 1 + rnd(255); break;
        case 1: s[rnd(len)] = 0; break; /* cut, often inside a sequence */
        }
        check(s);
    }
}

static void test_edge(void)
{
    static const int tail[] = { 0x00, 0x01, 0x7f, 0x80, 0x8f, 0x90, 0x9f,
                                0xa0, 0xbf, 0xc0, 0xcc, 0xff };
    static char s[MAXLEN];
    char *p;

    /* Every lead byte with every second byte, between two pairs that
       compose. A zero ends the string early. */
    for (int b0 = 1; b0 < 256; b0++)
        for (int b1 = 0; b1 < 256; b1++)
            for (unsigned i = 0; i < sizeof(tail) / sizeof(tail[0]); i++)
                for (unsigned j = 0; j < sizeof(tail) / sizeof(tail[0]); j++) {
                    char m[] = { 'e', 0xcc, 0x81, b0, b1, tail[i], tail[j],
                                 'e', 0xcc, 0x81, 0 };
                    check(m);
                    check(m + 3);
                }

    /* Up to the longest tag (id3v2buf + id3v1buf), and the most code points
       the old call took */
    for (int bytes = 1790; bytes <= 2167; bytes++) {
        for (p = s; p - s + 3 <= bytes; p = put(put(p, 'e'), 0x301));
        *p = 0;
        check(s);
        for (p = s; p - s + 9 <= bytes; p = put(put(put(p, 0x1100), 0x1161), 0x11a8));
        *p = 0;
        check(s);
    }
    for (int cps = 2040; cps < 2048; cps++) {
        for (p = s; p - s < cps - 1; *p++ = 'a');
        *put(p, 0x301) = 0;
        check(s);
        p = put(s, 'x'); /* the starter grows from 1 byte to 3 */
        for (int i = 0; i < cps - 2; i++)
            p = put(p, 0x323);
        *put(p, 0x307) = 0;
        check(s);
        for (p = s; (p - s) / 2 < cps; p = put(p, 0x301));
        *p = 0;
        check(s);
    }
    /* Marks that stay between a starter and the mark that joins it */
    for (int kept = 0; kept < 40; kept++)
        for (int i = 0; i < nprobes; i++) {
            p = put(s, probes[i][0]);
            for (int k = 0; k < kept; k++)
                p = put(p, k % 3 ? 0x334 : 0x1d167); /* class 1 */
            p = put(p, probes[i][1]);
            p = put(p, probes[i][1]);
            *put(p, probes[i][0]) = 0;
            check(s);
        }
    check("");
    utf8_normalize(NULL);
}

static void test_readonly(long n)
{
    long page = sysconf(_SC_PAGESIZE);
    char *ro = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
                    -1, 0);
    char s[128], old[128];

    if (ro == MAP_FAILED)
        exit(2);
    while (n--) {
        int len = random_string(s);
        memcpy(old, s, len + 1);
        old_normalize(old);
        if (strcmp(old, s))
            continue;
        mprotect(ro, page, PROT_READ | PROT_WRITE);
        memcpy(ro, s, len + 1);
        mprotect(ro, page, PROT_READ);
        utf8_normalize(ro); /* a write ends the test with a fault */
        ntotal++;
    }
}

static void test_corpus(const char *file)
{
    FILE *f = fopen(file, "rb");
    long size;
    char *buf;

    if (!f || fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0) {
        perror(file);
        exit(2);
    }
    rewind(f);
    buf = malloc(size + 1);
    if (!buf || fread(buf, 1, size, f) != (size_t)size)
        exit(2);
    buf[size] = 0;
    for (char *p = buf; p < buf + size; p += strlen(p) + 1)
        if (strlen(p) < 2048)
            check(p);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";

    if (strcmp(NFC_UTF8PROC_VERSION, utf8proc_version())) {
        fprintf(stderr, "nfc_table.h is of utf8proc %s, lib/utf8proc is %s\n",
                NFC_UTF8PROC_VERSION, utf8proc_version());
        return 1;
    }
    collect();
    collect_probes();
    rng = 0x9e3779b97f4a7c15ull ^ (argc > 3 ? strtoull(argv[3], NULL, 0) : 0);

    if (!strcmp(mode, "classes"))
        test_classes();
    else if (!strcmp(mode, "seconds"))
        test_seconds();
    else if (!strcmp(mode, "firsts"))
        test_firsts();
    else if (!strcmp(mode, "between"))
        test_between();
    else if (!strcmp(mode, "marks"))
        test_marks();
    else if (!strcmp(mode, "random") && argc > 3)
        test_random(atol(argv[2]));
    else if (!strcmp(mode, "edge"))
        test_edge();
    else if (!strcmp(mode, "readonly") && argc > 3)
        test_readonly(atol(argv[2]));
    else if (!strcmp(mode, "corpus") && argc > 2)
        test_corpus(argv[2]);
    else {
        fprintf(stderr, "usage: see nfctest.c\n");
        return 2;
    }
    printf("%-8s %s: %ld compared, %ld changed by normalizing, %ld differ\n",
           mode, argc > 2 ? argv[argc - 1] : "", ntotal, nchanged, ndiff);
    return ndiff != 0;
}
