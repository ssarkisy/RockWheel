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

/* The keys the lists sort and group by: apps/sortkey.c's order of names, as
   Music on the Mac has it, album_date_order() of the albums by date, and
   itunesdb_album_key(), by which Cover Flow and the lists find an album's
   cover. The cases are the rules' own comments and the commits that made
   them (dd495f0f2d, 4f3e46815b). */

#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "sortkey.h"
#include "album_years.h"
#include "itunesdb.h"
#include "tagcache.h"
#include "check.h"

/* a name's place in a list of them, as tagtree.c's sortkey_compare_n() */
static int sign(int x)
{
    return (x > 0) - (x < 0);
}

/* each name before the next, both ways round */
static void check_order(const char *what, const char *const *names, int n)
{
    check_case = what;
    for (int i = 0; i + 1 < n; i++)
    {
        const int ab = sortkey_compare(names[i], names[i + 1]);
        const int ba = sortkey_compare(names[i + 1], names[i]);
        check_count++;
        if (!(ab < 0 && ba > 0))
        {
            char buf[512];
            snprintf(buf, sizeof(buf), "\"%s\" before \"%s\" (%d, %d)",
                     names[i], names[i + 1], ab, ba);
            check_fail(__FILE__, __LINE__, buf);
        }
    }
    /* and the whole list sorted back into this order */
    const char **copy = malloc(n * sizeof(*copy));
    for (int i = 0; i < n; i++)
        copy[n - 1 - i] = names[i];
    for (int i = 1; i < n; i++)
    {
        for (int k = i; k > 0 && sortkey_compare(copy[k - 1], copy[k]) > 0; k--)
        {
            const char *t = copy[k];
            copy[k] = copy[k - 1];
            copy[k - 1] = t;
        }
    }
    for (int i = 0; i < n; i++)
        CHECK(copy[i] == names[i]);
    free(copy);
    check_case = NULL;
}
#define ORDER(what, ...) \
    do { \
        static const char *const names_[] = { __VA_ARGS__ }; \
        check_order(what, names_, sizeof(names_) / sizeof(names_[0])); \
    } while (0)

/* the same by the rules, from their starts, if not by their bytes */
static void check_alike(const char *a, const char *b)
{
    const char *sa = sortkey_start(a, true), *sb = sortkey_start(b, true);
    check_count++;
    if (sortkey_group(sa) != sortkey_group(sb) || sortkey_compare_from(sa, sb))
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "\"%s\" alike \"%s\"", a, b);
        check_fail(__FILE__, __LINE__, buf);
    }
    /* the whole compare still tells them apart, by their bytes */
    if (strcmp(a, b))
        CHECK(sign(sortkey_compare(a, b)) == sign(strcmp(a, b)));
}

static void check_start(const char *name, bool article, const char *expect)
{
    const char *s = sortkey_start(name, article);
    check_count++;
    if (strcmp(s, expect))
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "start of \"%s\" is \"%s\", not \"%s\"",
                 name, s, expect);
        check_fail(__FILE__, __LINE__, buf);
    }
}

static void check_initial(const char *name, ucschar_t expect)
{
    const ucschar_t c = sortkey_initial(sortkey_start(name, true));
    check_count++;
    if (c != expect)
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "initial of \"%s\" is U+%04X, not U+%04X",
                 name, (unsigned)c, (unsigned)expect);
        check_fail(__FILE__, __LINE__, buf);
    }
}

static void test_articles(void)
{
    /* "The Killers under K": The, A and An aside, in any case, if a name
       follows them */
    check_start("The Killers", true, "Killers");
    check_start("THE WHO", true, "WHO");
    check_start("A Perfect Circle", true, "Perfect Circle");
    check_start("An Albatross", true, "Albatross");
    check_start("a-ha", true, "a-ha");
    check_start("A", true, "A");
    check_start("The", true, "The");
    check_start("The ", true, "The ");
    check_start("Theory of a Deadman", true, "Theory of a Deadman");
    check_start("Anthrax", true, "Anthrax");
    check_start("Then", true, "Then");
    /* a Sort field keeps its article */
    check_start("The Killers", false, "The Killers");
    /* and the marks before the first letter or digit don't count */
    check_start("'Til Tuesday", true, "Til Tuesday");
    check_start("...And You Will Know Us", true, "And You Will Know Us");
    check_start("(Hed) P.E.", true, "Hed) P.E.");
    check_start("The 'Burbs", true, "Burbs");
    check_start("\xe2\x80\x9cQuoted\xe2\x80\x9d", true, "Quoted\xe2\x80\x9d");
    /* a name of marks alone sorts from its start */
    check_start("!!!", true, "!!!");
    check_start("?", true, "?");
    check_start("", true, "");

    ORDER("articles", "The Beatles", "Blur", "A Flock of Seagulls",
          "The Killers", "Kings of Leon", "An Pierlé", "A Tribe Called Quest");
}

static void test_case_and_marks(void)
{
    check_alike("ABBA", "abba");
    check_alike("Beyonc\xc3\xa9", "BEYONCE");
    /* the apostrophes are one */
    check_alike("Don't Stop", "Don\xe2\x80\x99t Stop");
    /* the space first of the marks: "Miley Cyrus - …" before "& …" */
    ORDER("marks", "Miley Cyrus - Wrecking Ball", "Miley Cyrus & Friends",
          "Miley CyrusA");
    /* and a mark before a letter */
    ORDER("mark before letter", "AC/DC", "ACDC", "Ace of Base");
}

static void test_numbers(void)
{
    /* by their value, the zeros before them aside */
    ORDER("numbers", "Track 2", "Track 9", "Track 10", "Track 100");
    check_alike("Opus 007", "Opus 7");
    ORDER("number runs", "2 Unlimited", "10cc", "50 Cent", "311", "1999");
    /* the digits of Arabic and Thai, and the full-width ones, are numbers */
    check_alike("Track \xd9\xa3", "Track 3");             /* ٣ */
    check_alike("Track \xe0\xb9\x93", "Track 3");         /* ๓ */
    check_alike("Track \xef\xbc\x93", "Track 3");         /* ３ */
}

static void test_accents(void)
{
    /* a letter with an accent goes with its letter */
    ORDER("accents", "Eagles", "\xc3\x89milie Simon", "Eminem", "Enya");
    check_alike("\xc3\x85ngstr\xc3\xb6m", "Angstrom");     /* Ångström */
    check_alike("Mot\xc3\xb6rhead", "Motorhead");
    check_alike("Sigur R\xc3\xb3s", "Sigur Ros");
    check_alike("Bj\xc3\xb8rn", "Bjorn");                  /* ø */
    check_alike("Stra\xc3\x9f" "e", "Strase");             /* ß as s */
    check_alike("Pi\xc3\xb1" "a", "Pina");
    /* an accent written apart is left out */
    check_alike("Beyonce\xcc\x81", "Beyonce");
    /* Vietnamese and pinyin */
    check_alike("Ph\xe1\xbb\x9f", "Pho");                  /* ở */
    check_alike("\xc7\x8e", "a");                          /* ǎ */
    /* a letter of its own after z: Þ */
    ORDER("thorn", "Zebra", "\xc3\x9e\x6fr", "\xd0\x90\xd0\xb1");
    CHECK_EQ(sortkey_group(sortkey_start("\xc3\x9e\x6fr", true)),
             SORTKEY_LETTERS);
    /* full-width Latin as the plain */
    check_alike("\xef\xbc\xa1\xef\xbc\xa2\xef\xbc\xa3", "abc");
}

static void test_scripts(void)
{
    sortkey_set_first(0);
    /* Latin, then the other alphabets in their order, then digits, then
       marks alone */
    ORDER("groups", "Zebra",
          "\xce\x91\xce\xbb\xcf\x86\xce\xb1",               /* Άλφα: Greek */
          "\xd0\x90\xd1\x80\xd0\xb8\xd1\x8f",               /* Ария */
          "\xd5\x80\xd5\xa1\xd5\xb5",                       /* Armenian */
          "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d",               /* שלום */
          "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7",       /* Arabic */
          "\xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa\xe0\xb8\x94\xe0\xb8\xb5", /* Thai */
          "\xed\x95\x9c\xea\xb8\x80",                       /* 한글 */
          "\xe3\x81\x82\xe3\x81\x84",                       /* あい */
          "\xe6\xbc\xa2\xe5\xad\x97",                       /* 漢字 */
          "10cc", "!!!");
    CHECK_EQ(sortkey_group("Zebra"), SORTKEY_LATIN);
    CHECK_EQ(sortkey_group("\xd0\x90"), SORTKEY_LETTERS);
    CHECK_EQ(sortkey_group("10cc"), SORTKEY_DIGITS);
    CHECK_EQ(sortkey_group("!!!"), SORTKEY_MARKS);

    /* case in every alphabet; Ё with Е, but Й a letter of its own */
    check_alike("\xd0\x90\xd0\xa0\xd0\x98\xd0\xaf", "\xd0\xb0\xd1\x80\xd0\xb8\xd1\x8f");
    check_alike("\xd0\x81\xd0\xbb\xd0\xba\xd0\xb0", "\xd0\x95\xd0\xbb\xd0\xba\xd0\xb0");
    ORDER("cyrillic", "\xd0\x95\xd0\xb4\xd0\xb5\xd1\x82",   /* Едет */
          "\xd0\x81\xd0\xb6",                               /* Ёж */
          "\xd0\x96\xd1\x83\xd0\xba",                       /* Жук */
          "\xd0\x98\xd0\xb2\xd0\xb0\xd0\xbd",               /* Иван */
          "\xd0\x99\xd0\xbe\xd0\xb3\xd0\xb0");              /* Йога */
    /* Greek without accents, one sigma */
    check_alike("\xce\x86\xce\xbb\xcf\x86\xce\xb1", "\xce\xb1\xce\xbb\xcf\x86\xce\xb1");
    check_alike("\xcf\x83\xcf\x82", "\xcf\x83\xcf\x83");
    /* katakana as hiragana, the voicing marks aside */
    check_alike("\xe3\x82\xab", "\xe3\x81\x8b");            /* カ か */

    /* the menu language's alphabet first (4f3e46815b) */
    sortkey_set_first(sortkey_language_first("russian"));
    ORDER("russian first", "\xd0\x90\xd1\x80\xd0\xb8\xd1\x8f", "Zebra",
          "\xce\x91\xce\xbb\xcf\x86\xce\xb1", "10cc", "!!!");
    CHECK_EQ(sortkey_group("\xd0\x90"), SORTKEY_FIRST_LETTERS);
    CHECK_EQ(sortkey_group("Zebra"), SORTKEY_LATIN);
    sortkey_set_first(sortkey_language_first("japanese"));
    ORDER("japanese first", "\xe3\x81\x82\xe3\x81\x84", "\xe6\xbc\xa2\xe5\xad\x97",
          "Zebra", "\xd0\x90\xd1\x80\xd0\xb8\xd1\x8f", "10cc");
    sortkey_set_first(0);

    CHECK(sortkey_language("english") == NULL);
    CHECK(sortkey_language("ukrainian") != NULL
          && sortkey_language("ukrainian")->letters != NULL);
    CHECK_EQ(sortkey_language_first("korean"),
             SORTKEY_FIRST(SORTKEY_ALPHABET_HANGUL));
    CHECK_EQ(sortkey_alphabet_of(0x416), SORTKEY_ALPHABET_CYRILLIC);
    CHECK_EQ(sortkey_alphabet_of(0xd55c), SORTKEY_ALPHABET_HANGUL);
    CHECK_EQ(sortkey_alphabet_of(0x30ab), SORTKEY_ALPHABET_KANA);
}

static void test_initials(void)
{
    check_initial("The Beatles", 'B');
    check_initial("\xc3\x89milie", 'E');
    check_initial("10cc", '#');
    check_initial("!!!", '#');
    check_initial("\xd1\x91\xd0\xbb\xd0\xba\xd0\xb0", 0x415);   /* ёлка: Е */
    check_initial("\xcf\x89\xce\xbc", 0x3a9);                   /* ωμ: Ω */
    check_initial("\xc3\xbe\x6fr", 0xde);                        /* þór: Þ */
    check_initial("\xea\xb9\x8c\xec\xb9\x98", 0x3131);           /* 까치: ㄱ */
    check_initial("\xe3\x81\x8c", 0x304b);                       /* が: か */
    check_initial("\xe3\x82\xb7", 0x3055);                       /* シ: さ */
    check_initial("\xe3\x82\x93", 0x308f);                       /* ん: わ */
}

/* names cut inside a letter, as a long tag is: nothing past the end read */
static void test_cut(void)
{
    static const char *const cut[] = {
        "abc\xd0", "abc\xe3\x81", "\xf0\x9f", "The \xe2", "\xc3", "1\xd9",
    };
    for (size_t i = 0; i < sizeof(cut) / sizeof(cut[0]); i++)
    {
        /* each one a heap copy of its own size, so that ASan sees a read
           past it */
        char *a = strdup(cut[i]);
        const char *s = sortkey_start(a, true);
        (void)sortkey_group(s);
        (void)sortkey_initial(s);
        for (size_t k = 0; k < sizeof(cut) / sizeof(cut[0]); k++)
        {
            char *b = strdup(cut[k]);
            const int ab = sortkey_compare(a, b), ba = sortkey_compare(b, a);
            CHECK(sign(ab) == -sign(ba));
            CHECK((ab == 0) == (i == k));
            free(b);
        }
        free(a);
    }
}

static void test_album_date_order(void)
{
    /* by year, a year alone after its dates, a month alone after its days;
       none, 0 */
    CHECK_EQ(album_date_order(0), 0);
    CHECK_EQ(album_date_order(-5), 0);
    const int32_t y = album_date_order(1999);
    const int32_t jan = album_date_order(19990100);
    const int32_t jan5 = album_date_order(19990105);
    const int32_t dec31 = album_date_order(19991231);
    CHECK(jan5 < jan);
    CHECK(jan < dec31);
    CHECK(dec31 < y);
    CHECK(y < album_date_order(20000101));
    CHECK(album_date_order(19981231) < jan5);
}

static void test_album_key(void)
{
    /* Latin letters in either case as one */
    const uint64_t k = itunesdb_album_key("Abbey Road", "The Beatles", NULL);
    CHECK(k == itunesdb_album_key("ABBEY ROAD", "the beatles", NULL));
    /* the album's artist tells two of a name apart */
    CHECK(k != itunesdb_album_key("Abbey Road", "Other", NULL));
    CHECK(itunesdb_album_key("Greatest Hits", "Queen", NULL)
          != itunesdb_album_key("Greatest Hits", "ABBA", NULL));
    /* the name and the artist aren't run together */
    CHECK(itunesdb_album_key("AB", "C", NULL)
          != itunesdb_album_key("A", "BC", NULL));
    /* no artist, or Rockbox's <Untagged>, is none */
    CHECK(itunesdb_album_key("Abbey Road", "", NULL)
          == itunesdb_album_key("Abbey Road", UNTAGGED, NULL));
    CHECK(itunesdb_album_key("Abbey Road", NULL, NULL)
          == itunesdb_album_key("Abbey Road", "", NULL));
    /* an album without a name is its folder, whatever the file and the
       artist, and whatever case the folder's path has */
    const uint64_t f = itunesdb_album_key(NULL, "x",
                                          "/Music/Artist/Album/01.mp3");
    CHECK(f == itunesdb_album_key("", "y", "/Music/Artist/Album/02.flac"));
    CHECK(f == itunesdb_album_key(UNTAGGED, NULL, "/music/artist/album/03.m4a"));
    CHECK(f != itunesdb_album_key(NULL, NULL, "/Music/Artist/Other/01.mp3"));
    /* nor named like one: "/" goes before the folder's */
    CHECK(f != itunesdb_album_key("Music/Artist/Album", NULL, NULL));
#ifdef HAVE_MULTIVOLUME
    /* the volume of the iPod's disk isn't part of it */
    CHECK(f == itunesdb_album_key(NULL, NULL,
                                  "/<HDD0>/Music/Artist/Album/01.mp3"));
#endif
    /* never 0, which is none */
    CHECK(itunesdb_album_key(NULL, NULL, NULL) != 0);
    CHECK(itunesdb_album_key("a", "b", NULL) != 0);
}

int main(void)
{
    /* a loop in the keys ends the run, not hangs it */
    check_time_limit(30);
    test_articles();
    test_case_and_marks();
    test_numbers();
    test_accents();
    test_scripts();
    test_initials();
    test_cut();
    test_album_date_order();
    test_album_key();
    return check_done("keys");
}
