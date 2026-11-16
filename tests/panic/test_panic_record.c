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

/* firmware/common/panic_record.c: the record, sealed, torn, foreign, counted,
 * its strings and its scrub of paths; and its three copies, voted and mended
 * after bits flipped as Menu+Select flips them (2 to 7 in 4096) and far more */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "panic_record.h"
#include "check.h"

static uint32_t rng = 0x12345678u;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void fill_random(void *p, size_t n)
{
    unsigned char *b = p;
    for (size_t i = 0; i < n; i++)
        b[i] = rnd();
}

/* the record lives in 512 bytes of RAM that held anything before */
static union { struct panic_record r; unsigned char b[PANIC_REC_RESERVE]; } area;

static void write_sample(struct panic_record *r, const char *msg)
{
    panic_record_write(r, PANIC_KIND_DATA_ABORT, 1, 0x0033e266, 0x0033e1f0,
                       0x4000b9a0, "audio", "e1c0b2f-261115", msg);
}

static void test_layout(void)
{
    if (0) printf("layout: sizeof %zu, msg at %zu, reserve %d\n",
           sizeof(struct panic_record), offsetof(struct panic_record, msg),
           PANIC_REC_RESERVE);
    CHECK(sizeof(struct panic_record) == 272);
    CHECK(offsetof(struct panic_record, magic) == 0);
    CHECK(offsetof(struct panic_record, sum) == 4);
    CHECK(offsetof(struct panic_record, kind) == 8);
    CHECK(offsetof(struct panic_record, regs) == 32);
    CHECK(offsetof(struct panic_record, trace) == 72);
    CHECK(offsetof(struct panic_record, thread) == 112);
    CHECK(offsetof(struct panic_record, version) == 128);
    CHECK(offsetof(struct panic_record, msg) == 152);
    CHECK(PANIC_REC_MAGIC == 0x31505752u
          && memcmp((const char[]){ 'R', 'W', 'P', '1' }, &(uint32_t){ PANIC_REC_MAGIC }, 4) == 0);
    {   /* no one-byte change turns one magic into the other */
        int bytes = 0;
        for (int i = 0; i < 4; i++)
            bytes += ((PANIC_REC_MAGIC ^ PANIC_REC_USED) >> (8 * i)) & 0xff ? 1 : 0;
        CHECK(bytes >= 2);
    }
    CHECK(PANIC_KIND_JUMP_TO_0 == 6 && PANIC_KIND_DATA_ABORT == 3);
}

static void test_valid(void)
{
    struct panic_record *r = &area.r;

    fill_random(&area, sizeof(area));
    write_sample(r, "Data abort at 0033e266");
    CHECK(panic_record_valid(r));
    CHECK(panic_record_peek(r) == r);
    CHECK(r->magic == PANIC_REC_MAGIC);
    CHECK(r->count == 1);
    CHECK(r->kind == PANIC_KIND_DATA_ABORT && r->core == 1);
    CHECK(r->pc == 0x0033e266 && r->lr == 0x0033e1f0 && r->sp == 0x4000b9a0);
    CHECK(!strcmp(r->thread, "audio"));
    CHECK(!strcmp(r->version, "e1c0b2f-261115"));
    CHECK(!strcmp(r->msg, "Data abort at 0033e266"));
    for (int i = 0; i < 8; i++)
        CHECK(r->regs[i] == 0);
    for (int i = 0; i < PANIC_REC_TRACE; i++)
        CHECK(r->trace[i] == 0);
    CHECK(r->uptime == 0 && r->wall == 0);

    /* fill the rest, as the hook does: clear magic, set, seal */
    r->magic = 0;
    CHECK(!panic_record_valid(r));
    for (int i = 0; i < 8; i++)
        r->regs[i] = 0x1000 + i;
    r->trace[0] = 0x0033e266;
    r->trace[1] = 0x00012345;
    r->uptime = 123456;
    r->wall = 1794787200u;
    panic_record_seal(r);
    CHECK(panic_record_valid(r));
    CHECK(r->count == 1);

    /* the same record whatever the RAM held before */
    {
        static union { struct panic_record r; unsigned char b[PANIC_REC_RESERVE]; } a, b;
        memset(&a, 0x00, sizeof(a));
        memset(&b, 0xa5, sizeof(b));
        write_sample(&a.r, "same");
        write_sample(&b.r, "same");
        CHECK(memcmp(&a.r, &b.r, sizeof(struct panic_record)) == 0);
    }
}

/* every byte changed every way (one byte at a time): never valid; peek
   takes none */
static void test_torn(void)
{
    struct panic_record good, t;
    int bad_valid = 0, bad_peek = 0, n = 0;

    fill_random(&area, sizeof(area));
    write_sample(&area.r, "torn");
    area.r.magic = 0;
    area.r.uptime = 77;
    area.r.trace[3] = 0xdeadbeef;
    panic_record_seal(&area.r);
    good = area.r;
    CHECK(panic_record_valid(&good));

    for (size_t i = 0; i < sizeof(good); i++)
        for (unsigned int x = 1; x < 256; x++)
        {
            t = good;
            ((unsigned char *)&t)[i] ^= x;
            n++;
            if (panic_record_valid(&t))
                bad_valid++;
            if (panic_record_peek(&t))
                bad_peek++;
        }
    if (0) printf("torn: %d one-byte changes, %d valid, %d peeked\n", n, bad_valid,
           bad_peek);
    CHECK(bad_valid == 0);
    CHECK(bad_peek == 0);

    /* and a logged record torn by one byte: peek takes none either */
    bad_peek = 0;
    t = good;
    CHECK(panic_record_take(&t) == &t);
    good = t;
    for (size_t i = 0; i < sizeof(good); i++)
        for (unsigned int x = 1; x < 256; x++)
        {
            t = good;
            ((unsigned char *)&t)[i] ^= x;
            if (panic_record_peek(&t) || panic_record_valid(&t))
                bad_peek++;
        }
    CHECK(bad_peek == 0);
}

static void test_foreign(void)
{
    int valid = 0, peek = 0, trials = 200000;

    for (int k = 0; k < trials; k++)
    {
        fill_random(&area, sizeof(area));
        if (k & 1)
            area.r.magic = PANIC_REC_MAGIC;     /* the magic right by chance */
        else if (k % 4 == 2)
            area.r.magic = PANIC_REC_USED;
        if (panic_record_valid(&area.r))
            valid++;
        if (panic_record_peek(&area.r))
            peek++;
        if (panic_record_take(&area.r))
            valid++;
    }
    if (0) printf("foreign: %d random fills, %d valid or taken, %d peeked\n", trials,
           valid, peek);
    CHECK(valid == 0 && peek == 0);

    memset(&area, 0, sizeof(area));
    CHECK(!panic_record_valid(&area.r) && !panic_record_peek(&area.r));
    memset(&area, 0xff, sizeof(area));
    CHECK(!panic_record_valid(&area.r) && !panic_record_peek(&area.r));
    CHECK(!panic_record_bump(&area.r));

    /* a record written over foreign bytes counts from 1 */
    fill_random(&area, sizeof(area));
    area.r.magic = PANIC_REC_MAGIC;
    area.r.count = 41;
    write_sample(&area.r, "first");
    CHECK(area.r.count == 1);
}

static void test_count(void)
{
    struct panic_record *r = &area.r;

    fill_random(&area, sizeof(area));
    write_sample(r, "one");
    CHECK(r->count == 1);
    write_sample(r, "two");                 /* not logged yet: goes on */
    CHECK(r->count == 2 && !strcmp(r->msg, "two"));
    CHECK(panic_record_bump(r));            /* a second panic, same boot */
    CHECK(r->count == 3 && !strcmp(r->msg, "two"));
    CHECK(panic_record_valid(r));
    CHECK(panic_record_take(r) == r);
    CHECK(!panic_record_bump(r));           /* logged: nothing to count on */
    CHECK(r->count == 3 && r->magic == PANIC_REC_USED);
    write_sample(r, "after");               /* after a logged one: from 1 */
    CHECK(r->count == 1 && panic_record_valid(r));
    ((unsigned char *)r)[200] ^= 0x04;      /* torn: from 1 */
    write_sample(r, "after torn");
    CHECK(r->count == 1);
}

static void test_strings(void)
{
    struct panic_record *r = &area.r;
    char longmsg[300], expect[PANIC_REC_MSG];

    for (int i = 0; i < (int)sizeof(longmsg) - 1; i++)
        longmsg[i] = 'a' + i % 26;
    longmsg[sizeof(longmsg) - 1] = '\0';

    fill_random(&area, sizeof(area));
    panic_record_write(r, 0, 0, 1, 2, 3, "a-thread-name-longer-than-16",
                       "a-version-string-longer-than-24-bytes", longmsg);
    CHECK(panic_record_valid(r));
    CHECK(strlen(r->thread) == PANIC_REC_THREAD - 1);
    CHECK(!strncmp(r->thread, "a-thread-name-longer-than-16",
                   PANIC_REC_THREAD - 1));
    CHECK(strlen(r->version) == PANIC_REC_VERSION - 1);
    CHECK(strlen(r->msg) == PANIC_REC_MSG - 1);
    memcpy(expect, longmsg, PANIC_REC_MSG - 1);
    expect[PANIC_REC_MSG - 1] = '\0';
    CHECK(!strcmp(r->msg, expect));

    /* exactly the field's room: kept whole */
    longmsg[PANIC_REC_MSG - 1] = '\0';
    panic_record_write(r, 0, 0, 1, 2, 3, "123456789012345", NULL, longmsg);
    CHECK(!strcmp(r->thread, "123456789012345"));
    CHECK(!strcmp(r->msg, longmsg));
    CHECK(r->version[0] == '\0');

    /* NULL strings: empty, and every byte after the end is zero */
    fill_random(&area, sizeof(area));
    panic_record_write(r, 0, 0, 1, 2, 3, NULL, "v", NULL);
    CHECK(r->thread[0] == '\0' && r->msg[0] == '\0');
    {
        int nonzero = 0;
        for (int i = 0; i < PANIC_REC_MSG; i++)
            nonzero += r->msg[i] != 0;
        for (int i = 1; i < PANIC_REC_VERSION; i++)
            nonzero += r->version[i] != 0;
        CHECK(nonzero == 0);
    }
    CHECK(panic_record_valid(r));
}

static void test_take(void)
{
    struct panic_record *r = &area.r;
    struct panic_record copy;

    fill_random(&area, sizeof(area));
    write_sample(r, "take me");
    copy = *r;
    CHECK(panic_record_take(r) == r);
    CHECK(r->magic == PANIC_REC_USED);
    CHECK(!panic_record_valid(r));
    CHECK(panic_record_peek(r) == r);       /* still whole, now logged */
    CHECK(!strcmp(r->msg, "take me") && r->pc == copy.pc);
    CHECK(memcmp((char *)r + 4, (char *)&copy + 4, sizeof(copy) - 4) == 0);
    CHECK(panic_record_take(r) == NULL);    /* once only */
    CHECK(panic_record_take(r) == NULL);
    CHECK(panic_record_peek(r) == r);
    CHECK(r->magic == PANIC_REC_USED);
}

static void scrub_case(const char *in, unsigned int size, const char *want)
{
    char out[256];

    memset(out, 0x7e, sizeof(out));
    panic_record_scrub(out, size, in);
    check_case = in;
    CHECK(!strcmp(out, want) && strlen(out) < size);
    check_case = NULL;
}

static void test_scrub(void)
{
    scrub_case("can't open /Music/My Album/01 Song.mp3", 120,
               "can't open <path>");
    scrub_case("open '/Music/a b.mp3' failed: -2", 120,
               "open '<path>' failed: -2");
    scrub_case("copy \"/a/b\" to \"/c d/e\" (rc 5)", 120,
               "copy \"<path>\" to \"<path>\" (rc 5)");
    scrub_case("/.rockbox/database_0.tcd corrupt", 120, "<path>");
    scrub_case("file=/x/y, line 3", 120, "file=<path>");
    scrub_case("path:/<HDD0>/Music", 120, "path:<path>");
    scrub_case("(/a/b)", 120, "(<path>");
    scrub_case("[/a]", 120, "[<path>");
    scrub_case("I/O error 3/4 and a / b", 120, "I/O error 3/4 and a / b");
    scrub_case("ends with a slash /", 120, "ends with a slash /");
    scrub_case("no path here", 120, "no path here");
    scrub_case("", 120, "");
    scrub_case("x /abc", 5, "x <p");                /* cut to the room */
    scrub_case("abcdef", 4, "abc");
    scrub_case("/abc", 1, "");
    scrub_case("tab\t/a/b", 120, "tab\t<path>");
    scrub_case("quote '/unterminated path", 120, "quote '<path>");

    /* size 0 writes nothing */
    {
        char c = 'z';
        panic_record_scrub(&c, 0, "/abc");
        CHECK(c == 'z');
    }
    /* NULL source: empty */
    {
        char out[8] = "xxxxxxx";
        panic_record_scrub(out, sizeof(out), NULL);
        CHECK(out[0] == '\0');
    }
}


/* the copies: written as the panic writes them (copy 0, then replicated) */
static union { struct panic_record r[PANIC_REC_COPIES];
               unsigned char b[PANIC_REC_RESERVE]; } copies;

#define REC_BITS ((int)sizeof(struct panic_record) * 8)

static void flip(int copy, int bit)
{
    ((unsigned char *)&copies.r[copy])[bit / 8] ^= 1u << (bit % 8);
}

static void make_copies(struct panic_record *good, bool logged)
{
    fill_random(&copies, sizeof(copies));
    write_sample(&copies.r[0], "copies");
    copies.r[0].magic = 0;
    copies.r[0].uptime = 4242;
    copies.r[0].trace[0] = 0x0033e266;
    panic_record_seal(&copies.r[0]);
    if (logged)
        panic_record_take(&copies.r[0]);
    panic_record_replicate(&copies.r[0]);
    *good = copies.r[0];
}

static void test_vote(void)
{
    struct panic_record good, out;
    int mended;

    make_copies(&good, false);
    CHECK(panic_record_recover(&out, copies.r, &mended) == &out);
    CHECK(mended == 0 && !memcmp(&out, &good, sizeof(good)));

    /* the panic's own vote, in place: a different bit in each copy */
    flip(0, 100); flip(1, 900); flip(2, 2000);
    panic_record_vote(copies.r);
    CHECK(!memcmp(&copies.r[0], &good, sizeof(good)));
    CHECK(panic_record_valid(&copies.r[0]));

    /* a whole copy can't be told from the vote by anything recover changes */
    make_copies(&good, false);
    struct panic_record before[PANIC_REC_COPIES];
    flip(1, 77);
    memcpy(before, copies.r, sizeof(before));
    CHECK(panic_record_recover(&out, copies.r, &mended) == &out);
    CHECK(!memcmp(before, copies.r, sizeof(before)));   /* area unchanged */
}

/* every bit flipped alike in two copies (the vote then wrong there), with a
   few more single flips about: mended back to the very record */
static void test_mend_one(void)
{
    struct panic_record good, out;
    int ok = 0, wrong = 0, lost = 0, mended;

    for (int bit = 0; bit < REC_BITS; bit++)
    {
        make_copies(&good, bit & 1);
        const int a = bit % 3, b = (a + 1) % 3;
        flip(a, bit);
        flip(b, bit);
        for (int k = 0; k < 6; k++)     /* the survive test's other damage */
            flip(rnd() % 3, rnd() % REC_BITS);
        const struct panic_record *r = panic_record_recover(&out, copies.r,
                                                            &mended);
        if (!r)
            lost++;
        else if (memcmp(r, &good, sizeof(good)))
            wrong++;
        else
            ok++;
    }
    if (0) printf("mend one: %d ok, %d lost, %d wrong\n", ok, lost, wrong);
    CHECK_EQ(wrong, 0);
    /* lost only where a random flip hit a third copy at the same bit, or
       made three twin bits */
    CHECK(lost <= REC_BITS / 100);
}

/* two twin flips: mended; three or more: lost, never wrong */
static void test_mend_more(void)
{
    struct panic_record good, out;
    int mended, wrong = 0, ok2 = 0, trials = 3000;

    for (int t = 0; t < trials; t++)
    {
        make_copies(&good, t & 1);
        const int twins = 2 + t % 3;    /* 2, 3 or 4 */
        for (int k = 0; k < twins; k++)
        {
            const int bit = rnd() % REC_BITS, a = rnd() % 3;
            flip(a, bit);
            flip((a + 1) % 3, bit);
        }
        const struct panic_record *r = panic_record_recover(&out, copies.r,
                                                            &mended);
        if (r && memcmp(r, &good, sizeof(good)))
            wrong++;
        if (twins == 2 && r && mended == 2)
            ok2++;
    }
    CHECK_EQ(wrong, 0);
    CHECK(ok2 >= trials / 3 * 9 / 10);
}

/* bits flipped at random, from the survive test's rate to fifty times it:
   the record back as it was, or none */
static void test_flips(void)
{
    struct panic_record good, out;
    int mended, wrong = 0, lost_low = 0, trials = 20000;

    for (int t = 0; t < trials; t++)
    {
        make_copies(&good, t & 1);
        const int n = t % 2 ? 1 + t % 11 : 1 + t % 400;   /* bits in 8192 */
        for (int k = 0; k < n; k++)
            flip(rnd() % 3, rnd() % REC_BITS);
        const struct panic_record *r = panic_record_recover(&out, copies.r,
                                                            &mended);
        if (r && memcmp(r, &good, sizeof(good)))
            wrong++;
        if (!r && n <= 11)
            lost_low++;
    }
    CHECK_EQ(wrong, 0);
    CHECK_EQ(lost_low, 0);
}

/* the CRC: every change of one or two bits of a sealed record is caught,
   and changes of three and four at random */
static void test_crc(void)
{
    struct panic_record good, t;
    int caught = 1;

    make_copies(&good, false);
    for (int i = 32; i < REC_BITS; i++)     /* the magic aside: its own test */
    {
        t = good;
        ((unsigned char *)&t)[i / 8] ^= 1u << (i % 8);
        caught &= !panic_record_peek(&t);
        for (int j = i + 1; j < REC_BITS; j += 7)
        {
            struct panic_record u = t;
            ((unsigned char *)&u)[j / 8] ^= 1u << (j % 8);
            caught &= !panic_record_peek(&u);
        }
    }
    for (int k = 0; k < 300000; k++)
    {
        t = good;
        const int n = 3 + k % 2;
        for (int m = 0; m < n; m++)
        {
            const int i = 32 + rnd() % (REC_BITS - 32);
            ((unsigned char *)&t)[i / 8] ^= 1u << (i % 8);
        }
        if (memcmp(&t, &good, sizeof(t)))
            caught &= !panic_record_peek(&t);
    }
    CHECK(caught);
}

/* RAM as a power-on leaves it: no record */
static void test_foreign_copies(void)
{
    struct panic_record out;
    int found = 0, mended;

    for (int t = 0; t < 20000; t++)
    {
        fill_random(&copies, sizeof(copies));
        if (t & 1)      /* the same rubbish in each copy */
            panic_record_replicate(copies.r);
        if (t % 4 < 2)
            for (int c = 0; c < PANIC_REC_COPIES; c++)
                copies.r[c].magic = t % 8 < 4 ? PANIC_REC_MAGIC : PANIC_REC_USED;
        found += panic_record_recover(&out, copies.r, &mended) != NULL;
    }
    memset(&copies, 0, sizeof(copies));
    found += panic_record_recover(&out, copies.r, &mended) != NULL;
    memset(&copies, 0xff, sizeof(copies));
    found += panic_record_recover(&out, copies.r, &mended) != NULL;
    CHECK_EQ(found, 0);
}

int main(void)
{
    check_time_limit(120);
    test_layout();
    test_valid();
    test_torn();
    test_foreign();
    test_count();
    test_strings();
    test_take();
    test_scrub();
    test_vote();
    test_mend_one();
    test_mend_more();
    test_flips();
    test_crc();
    test_foreign_copies();
    return check_done("panic_record");
}
