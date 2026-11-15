#!/usr/bin/env python3
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
# Copyright (C) 2026 Sergei Sarkisian
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.

"""The iPods of test_itunesdb: an iTunesDB and an ArtworkDB shaped as
Finder writes them (libgpod's layout, the fields apps/itunesdb.c reads), and
the same damaged: cut short, lengths past the end, counts too big, sizes of
0, records that lead back to themselves. Each case is a folder with the
iPod's paths under it and a file "expect" of what reading it gives:

    tracks <n>      the tracks db_load() has, and the import's count
    loaded <0|1>    db_load() read the tracks
    complete <0|1>  every track listed was read
    paths <n>       the tracks read with a path
    absent <id>     no track read has this id
    covers <n>      the tracks whose covers ArtworkDB has, in the index made
    shows <n>       the podcast shows counted
    name <text>     the iPod's name (itunesdb_device_name())
    playlist <file>\t<song>\t...  a playlist written, its songs in order
    survive         only that nothing breaks: no sanitizer error, no hang
    known <how> <why>  a failure known and left as it is, of this kind
                    (check, timeout, crash): reported, not counted

A damaged case expects what the parser of 8cc113a9d3 gives where that is
plainly right (damaged_itunesdb(), damaged_artworkdb()), and only to survive
elsewhere. The file "count" says how many cases there are. Run with
python3 -I -B; writes only under the folder it's given.
"""

import os
import struct
import sys

MAC_EPOCH = 2082844800


def le32(v):
    return struct.pack('<I', v & 0xffffffff)


def le16(v):
    return struct.pack('<H', v & 0xffff)


def pad(b, n):
    assert len(b) <= n, (len(b), n)
    return b + bytes(n - len(b))


def put32(buf, off, v):
    buf[off:off + 4] = le32(v)


# iTunesDB

def mhod_string(kind, text):
    s = text.encode('utf-16-le')
    return (b'mhod' + le32(24) + le32(40 + len(s)) + le32(kind) + le32(0) * 2
            + le32(1) + le32(len(s)) + le32(1) + le32(0) + s)


def mhod_raw(kind, body):
    return b'mhod' + le32(24) + le32(24 + len(body)) + le32(kind) + le32(0) * 2 + body


def mhit(tid, mhods, *, header_len=0x248, rating=0, plays=0, year=0,
         added=0, released=0, media=1, dbid=0, art_id=0, art_count=0,
         sound_check=0, played=0, bookmark=0):
    h = bytearray(header_len)
    h[0:4] = b'mhit'
    put32(h, 4, header_len)
    put32(h, 12, len(mhods))
    put32(h, 16, tid)
    put32(h, 20, 1)
    h[31] = rating
    put32(h, 52, year)
    put32(h, 76, sound_check)
    put32(h, 80, plays)
    put32(h, 0x68, added)
    struct.pack_into('<Q', h, 0x70, dbid)
    if header_len > 0x7e:
        struct.pack_into('<H', h, 0x7c, art_count)
    if header_len >= 0x90:
        put32(h, 0x8c, released)
    if header_len > 0xb2:
        h[0xb2] = played
    if header_len >= 212:
        put32(h, 208, media)
        put32(h, 0x6c, bookmark)
    if header_len >= 356:
        put32(h, 352, art_id)
    body = b''.join(mhods)
    put32(h, 8, header_len + len(body))
    return bytes(h) + body


def mhsd(kind, child, header_len=0x60):
    return pad(b'mhsd' + le32(header_len) + le32(header_len + len(child))
               + le32(kind), header_len) + child


def mhlt(items):
    return pad(b'mhlt' + le32(0x5c) + le32(len(items)), 0x5c) + b''.join(items)


def mhlp(lists):
    return pad(b'mhlp' + le32(0x5c) + le32(len(lists)), 0x5c) + b''.join(lists)


def mhip(track_id, group_flag=0, group_id=0, group_ref=0):
    pos = mhod_raw(100, le32(0) * 4)
    h = bytearray(0x4c)
    h[0:4] = b'mhip'
    put32(h, 4, 0x4c)
    put32(h, 8, 0x4c + len(pos))
    put32(h, 12, 1)
    put32(h, 16, group_flag)
    put32(h, 20, group_id)
    put32(h, 24, track_id)
    put32(h, 32, group_ref)
    return bytes(h) + pos


def mhyp(name, items, *, hidden=0, podcast=0, pid=0, sort=1, extra_mhods=()):
    mhods = [mhod_string(1, name)] + list(extra_mhods)
    h = bytearray(0x6c)
    h[0:4] = b'mhyp'
    put32(h, 4, 0x6c)
    put32(h, 12, len(mhods))
    put32(h, 16, len(items))
    put32(h, 20, hidden)
    put32(h, 24, MAC_EPOCH + 1700000000)
    struct.pack_into('<Q', h, 0x1c, pid)
    struct.pack_into('<H', h, 42, podcast)
    put32(h, 0x2c, sort)
    body = b''.join(mhods) + b''.join(items)
    put32(h, 8, 0x6c + len(body))
    return bytes(h) + body


def smart_mhods():
    """A live smart playlist: its prefs (mhod 50) and one rule (mhod 51),
    rating above 3 stars, big-endian as libgpod writes them"""
    prefs = bytes([1, 1, 0, 1, 2]) + bytes(3) + le32(25) + bytes([0, 0])
    prefs = pad(prefs, 74)
    rule = bytearray(56 + 0x44)
    struct.pack_into('>II', rule, 0, 0x19, 0x00000010)   # rating, greater
    struct.pack_into('>I', rule, 52, 0x44)
    struct.pack_into('>qqqq', rule, 56, 60, 0, 1, 60)
    slst = bytearray(136)
    slst[0:4] = b'SLst'
    struct.pack_into('>III', slst, 4, 0x00010000, 1, 0)
    return [mhod_raw(50, prefs), mhod_raw(51, bytes(slst) + bytes(rule))]


def mhbd(sections):
    body = b''.join(sections)
    h = bytearray(0xbc)
    h[0:4] = b'mhbd'
    put32(h, 4, 0xbc)
    put32(h, 8, 0xbc + len(body))
    put32(h, 12, 1)
    put32(h, 16, 0x19)
    put32(h, 20, len(sections))
    return bytes(h) + body


def track_mhods(title, artist, album, path, album_artist=None):
    m = [mhod_string(1, title), mhod_string(2, path),
         mhod_string(3, album), mhod_string(4, artist)]
    if album_artist:
        m.append(mhod_string(22, album_artist))
    return m


TRACKS = [
    # id, title, artist, album, path, extra
    (101, 'Come Together', 'Test Band', 'Test Album',
     ':iPod_Control:Music:F01:AAAA.mp3', dict(rating=80, plays=3, year=1969,
                                              released=MAC_EPOCH + 0, art_id=201)),
    (102, 'Something', 'Test Band', 'Test Album',
     ':iPod_Control:Music:F01:AAAB.m4a', dict(rating=100, plays=12, year=1969,
                                              art_id=201)),
    (103, 'Stars \U0001F31F and Ж', 'Other Artist', 'Single',
     ':iPod_Control:Music:F02:AAAC.m4a', dict(year=2024, art_id=202)),
    (104, 'Episode 1', 'A Show', 'A Show',
     ':iPod_Control:Music:F03:AAAD.mp3', dict(media=4, played=2,
                                              bookmark=61000)),
]


def itunesdb(name='Test iPod', tracks=TRACKS, old_mhit=False,
             list_name='Road Trip'):
    items = []
    for tid, title, artist, album, path, extra in tracks:
        extra = dict(extra)
        if old_mhit:
            art = extra.pop('art_id', 0)
            extra['art_count'] = 1 if art else 0
            items.append(mhit(tid, track_mhods(title, artist, album, path),
                              header_len=0x9c, dbid=0x1000 + tid, **extra))
        else:
            items.append(mhit(tid, track_mhods(title, artist, album, path,
                                               'Test Band' if tid < 103 else None),
                              dbid=0x1000 + tid, **extra))
    ids = [t[0] for t in tracks]
    master = mhyp(name, [mhip(i) for i in ids], hidden=1, pid=1)
    road = mhyp(list_name, [mhip(i) for i in (102, 101, 103)], pid=2)
    smart = mhyp('Loved', [mhip(102)], pid=3, extra_mhods=smart_mhods())
    podcasts = mhyp('Podcasts', [mhip(0, 0x100, 900), mhip(104, 0, 0, 900)],
                    podcast=1, pid=4)
    lists = [master, road, smart]
    return mhbd([mhsd(1, mhlt(items)),
                 mhsd(3, mhlp(lists + [podcasts])),
                 mhsd(2, mhlp(lists))])


# ArtworkDB: the frames of each cover in ithmb files, as Finder makes them
# for the iPod Video: 200x200 (format 1029) and 100x100 (1028)

FORMATS = {1029: (200, 200), 1028: (100, 100)}


def art_name(text):
    s = text.encode('utf-16-le')
    return (b'mhod' + le32(0x18) + le32(36 + len(s)) + le16(3) + le16(0)
            + le32(0) * 2 + le32(len(s)) + bytes([2]) + bytes(7) + s)


def mhni(fmt, offset, width, height, file_no, size=None, vpad=0, hpad=0):
    name = art_name(':F%d_%d.ithmb' % (fmt, file_no))
    size = width * height * 2 if size is None else size
    h = bytearray(0x4c)
    h[0:4] = b'mhni'
    put32(h, 4, 0x4c)
    put32(h, 8, 0x4c + len(name))
    put32(h, 12, 1)
    put32(h, 16, fmt)
    put32(h, 20, offset)
    put32(h, 24, size)
    struct.pack_into('<hhHH', h, 28, vpad, hpad, height, width)
    return bytes(h) + name


def art_container(child):
    return (b'mhod' + le32(0x18) + le32(0x18 + len(child)) + le16(2) + le16(0)
            + le32(0) * 2) + child


def mhii(image_id, song_dbid, frames):
    body = b''.join(art_container(f) for f in frames)
    h = bytearray(0x98)
    h[0:4] = b'mhii'
    put32(h, 4, 0x98)
    put32(h, 8, 0x98 + len(body))
    put32(h, 12, len(frames))
    put32(h, 16, image_id)
    struct.pack_into('<Q', h, 20, song_dbid)
    put32(h, 48, 300 * 300 * 3)
    return bytes(h) + body


def artworkdb(images):
    items = b''.join(images)
    mhli = pad(b'mhli' + le32(0x5c) + le32(len(images)), 0x5c) + items
    sd1 = pad(b'mhsd' + le32(0x60) + le32(0x60 + len(mhli)) + le16(1), 0x60) + mhli
    mhla = pad(b'mhla' + le32(0x5c) + le32(0), 0x5c)
    sd2 = pad(b'mhsd' + le32(0x60) + le32(0x60 + len(mhla)) + le16(2), 0x60) + mhla
    body = sd1 + sd2
    h = bytearray(0x84)
    h[0:4] = b'mhfd'
    put32(h, 4, 0x84)
    put32(h, 8, 0x84 + len(body))
    put32(h, 16, 2)
    put32(h, 20, 2)
    return bytes(h) + body


def artwork(old=False):
    """two covers, each in both sizes, one after the other in the files"""
    images = []
    for n, (image_id, tid) in enumerate(((201, 101), (202, 103))):
        frames = [mhni(fmt, n * w * h * 2, w, h, 1)
                  for fmt, (w, h) in FORMATS.items()]
        images.append(mhii(image_id, 0x1000 + tid if old else 0, frames))
    return artworkdb(images)


# the records of a file, by their tags: (offset, tag)

TAGS = (b'mhbd', b'mhsd', b'mhlt', b'mhit', b'mhod', b'mhlp', b'mhyp',
        b'mhip', b'mhfd', b'mhli', b'mhii', b'mhni', b'mhla')


def records(data):
    out = []
    for i in range(0, len(data) - 3):
        if bytes(data[i:i + 4]) in TAGS:
            out.append((i, bytes(data[i:i + 4]).decode()))
    return out


def first(data, tag, n=0):
    found = [o for o, t in records(data) if t == tag]
    return found[n] if n < len(found) else None


# the cases

class Case:
    def __init__(self, root, name):
        self.dir = os.path.join(root, name)
        os.makedirs(os.path.join(self.dir, 'iPod_Control', 'iTunes'))
        os.makedirs(os.path.join(self.dir, 'iPod_Control', 'Artwork'))
        os.makedirs(os.path.join(self.dir, '.rockbox'))
        os.makedirs(os.path.join(self.dir, 'Playlists'))

    def write(self, db, art=None, expect=('survive',), ithmb=True):
        with open(os.path.join(self.dir, 'iPod_Control', 'iTunes', 'iTunesDB'), 'wb') as f:
            f.write(db)
        if art is not None:
            with open(os.path.join(self.dir, 'iPod_Control', 'Artwork', 'ArtworkDB'), 'wb') as f:
                f.write(art)
            if ithmb:
                for fmt, (w, h) in FORMATS.items():
                    with open(os.path.join(self.dir, 'iPod_Control', 'Artwork',
                                           'F%d_1.ithmb' % fmt), 'wb') as f:
                        f.truncate(2 * w * h * 2)
        with open(os.path.join(self.dir, 'expect'), 'w', encoding='utf-8') as f:
            for line in expect:
                f.write(line + '\n')


def damaged(data):
    """the file damaged each way, as (name, bytes, how): how is the kind of
    damage ('cut', 'hlen', 'total', 'count', 'loop', 'strlen'), the record's
    tag and offset, and the field's offset and value's name, or where the
    file is cut"""
    out = []
    recs = records(data)
    size = len(data)
    # cut short: in each kind of record's header, and between records
    kinds_seen = {}
    for off, tag in recs:
        kinds_seen.setdefault(tag, []).append(off)
    for tag, offs in kinds_seen.items():
        for off in sorted(set((offs[0], offs[-1]))):
            for d in (0, 2, 6, 10, 14, 30, 60):
                cut = off + d
                if 0 < cut < size:
                    out.append(('cut-%s-%d' % (tag, cut), data[:cut],
                                dict(kind='cut', cut=cut)))
    out.append(('cut-last-byte', data[:-1], dict(kind='cut', cut=size - 1)))
    # lengths past the end, odd, and of 0, in each kind's first and last record
    for tag, offs in kinds_seen.items():
        for off in sorted(set((offs[0], offs[-1]))):
            for field, fname in ((4, 'hlen'), (8, 'total')):
                for v, vname in ((0, 'zero'), (size + 1, 'past'),
                                 (0x7fffffff, 'max'), (0xffffffff, 'neg'),
                                 (3, 'tiny')):
                    b = bytearray(data)
                    put32(b, off + field, v)
                    out.append(('%s-%s@%d-%s' % (fname, tag, off, vname), bytes(b),
                                dict(kind=fname, tag=tag, off=off, value=vname)))
    # counts too big: the counts at 8 of lists, at 12 of records with children,
    # at 16 of playlists' items and 20 of mhbd's and mhfd's sections
    count_fields = {'mhlt': (8,), 'mhlp': (8,), 'mhli': (8,), 'mhla': (8,),
                    'mhit': (12,), 'mhyp': (12, 16), 'mhii': (12,),
                    'mhni': (12,), 'mhbd': (20,), 'mhfd': (20,)}
    for tag, fields in count_fields.items():
        for off in kinds_seen.get(tag, [])[:1]:
            for field in fields:
                for v, vname in ((0xffffffff, 'neg'), (0x7fffffff, 'max'),
                                 ((1 << 20) + 1, 'over'), (1 << 20, 'most')):
                    b = bytearray(data)
                    put32(b, off + field, v)
                    out.append(('count-%s+%d@%d-%s' % (tag, field, off, vname),
                                bytes(b), dict(kind='count', tag=tag, off=off,
                                               field=field, value=vname)))
    # loops: a record's total leading back to itself or to the record before,
    # as on the iPod's 32-bit offsets
    for tag, offs in kinds_seen.items():
        if len(offs) >= 2:
            a, b_ = offs[0], offs[1]
            b = bytearray(data)
            put32(b, b_ + 8, (a - b_) & 0xffffffff)
            out.append(('loop-%s@%d' % (tag, b_), bytes(b),
                        dict(kind='loop', tag=tag, off=b_)))
    # strings: lengths past the end and odd
    for off in kinds_seen.get('mhod', [])[:3]:
        for v, vname in ((0xffffffff, 'neg'), (0x10001, 'big'), (7, 'odd')):
            b = bytearray(data)
            put32(b, off + 28, v)
            out.append(('strlen-mhod@%d-%s' % (off, vname), bytes(b),
                        dict(kind='strlen', tag='mhod', off=off, value=vname)))
    # each once: a cut can fall on the same byte from two records
    seen = set()
    return [c for c in out if not (c[0] in seen or seen.add(c[0]))]


def u32(data, off):
    return struct.unpack_from('<I', data, off)[0]


def damaged_itunesdb(db):
    """iTunesDB damaged, with what reading it gives: where that is plainly
    right, and, marked "as 8cc113a9d3 does", where it is what that parser
    gives and another could give otherwise, so that a change to it is seen
    and decided on. 'survive' alone where neither."""
    recs = records(db)
    mhits = [o for o, t in recs if t == 'mhit']
    mhsd1 = [o for o, t in recs if t == 'mhsd'][0]
    tracks_end = mhsd1 + u32(db, mhsd1 + 8)
    # where each track's path ends: its mhod of type 2
    path_ends = []
    for i, o in enumerate(mhits):
        stop = mhits[i + 1] if i + 1 < len(mhits) else tracks_end
        for m, t in recs:
            if t == 'mhod' and o < m < stop and u32(db, m + 12) == 2:
                path_ends.append(m + u32(db, m + 8))
    ids = [t[0] for t in TRACKS]
    first_mhod = mhits[0] + u32(db, mhits[0] + 4)
    whole = ['tracks 4', 'complete 1', 'paths 4']
    none = ['loaded 0', 'tracks 0']
    for name, bad, how in damaged(db):
        kind, tag, off = how['kind'], how.get('tag'), how.get('off')
        value = how.get('value')
        expect = ['survive']
        if kind == 'cut':
            # a track has its path when its record is there up to it
            expect = ['paths %d' % sum(e <= how['cut'] for e in path_ends)]
        elif tag in ('mhsd', 'mhbd') and kind == 'hlen' and off <= mhsd1:
            # the sections, or the tracks', not found
            expect = none
        elif off is not None and off >= tracks_end:
            # past the tracks, in the playlists: the tracks are all read
            expect = list(whole)
        elif tag in ('mhbd', 'mhsd') and kind == 'total':
            expect = list(whole)
        elif tag == 'mhbd' and kind == 'count':
            # the sections read until the next record isn't one; a negative
            # count reads none (as 8cc113a9d3 does)
            expect = none if value == 'neg' else list(whole)
        elif tag == 'mhlt' and kind == 'hlen':
            expect = none
        elif tag == 'mhlt' and kind in ('total', 'count'):
            # its count (at 8): more than COUNT_MAX, or negative, is refused,
            # and none reads none, a library emptied (itunesdb.c's comments);
            # more than there are reads those there are, incomplete; fewer
            # reads as many, complete (as 8cc113a9d3 does)
            expect = {'zero': ['loaded 1', 'tracks 0', 'complete 1'],
                      'tiny': ['tracks 3', 'complete 1', 'paths 3'],
                      'past': ['tracks 4', 'complete 0', 'paths 4'],
                      'most': ['tracks 4', 'complete 0', 'paths 4']
                      }.get(value, none)
        elif tag == 'mhit' and kind == 'hlen':
            # its header unreadable: no path. One shorter than any mhit's
            # (0x9c) is read as no track (P2 of review-1); either is counted
            # among the tracks, and the read complete (as 8cc113a9d3 does)
            k = mhits.index(off)
            expect = ['tracks 4', 'complete 1', 'paths 3']
            if value in ('zero', 'tiny'):
                expect.append('absent %d' % ids[k])
        elif tag == 'mhit' and kind == 'total':
            # the next record not found: those read so far, incomplete
            k = mhits.index(off) + 1
            expect = ['tracks %d' % k, 'complete %d' % (k == len(mhits)),
                      'paths %d' % k]
        elif tag == 'mhit' and kind == 'count':
            # mhods read until the next record isn't one; a negative count
            # reads none, so no path (as 8cc113a9d3 does)
            expect = (['tracks 4', 'complete 1', 'paths 3'] if value == 'neg'
                      else list(whole))
        elif tag == 'mhod' and off == first_mhod and kind == 'total':
            # the first track's title: its path beyond it not reached
            expect = ['tracks 4', 'complete 1', 'paths 3']
        elif tag == 'mhod' and off == first_mhod and kind == 'hlen':
            # a string's mhod is read at its fixed offsets, libgpod's, not by
            # the length its header gives
            expect = list(whole)
        elif kind == 'loop' and tag == 'mhod':
            # the path's mhod leading back to the title's: the mhods are
            # read as many times as the track counts them, the path first
            expect = list(whole)
        # Left to survive: the strings' lengths past their records (strlen-),
        # read past them as 8cc113a9d3 does, and loop-mhit, which reads two
        # tracks twice and calls it complete: neither is right to pin.
        if name.startswith('loop-mhsd'):
            expect.append('known timeout read_device_name() follows the '
                          'sections with no count: one whose length leads '
                          'back, as on the 32-bit offsets of the iPod, is a '
                          'loop')
        yield name, bad, expect


def damaged_artworkdb(art):
    """ArtworkDB damaged, with the covers found, as damaged_itunesdb() has
    them: of the first image, two songs' (101, 102), of the second one
    song's (103)"""
    recs = records(art)
    images = [o for o, t in recs if t == 'mhii']
    sections = [o for o, t in recs if t == 'mhsd']
    mhli = [o for o, t in recs if t == 'mhli'][0]
    images_end = sections[0] + u32(art, sections[0] + 8)
    # where each image's frames' names end: an image is found with one
    name_ends = []
    for i, o in enumerate(images):
        stop = images[i + 1] if i + 1 < len(images) else images_end
        name_ends.append([n + 0x4c + u32(art, n + 0x4c + 8) for n, t in recs
                          if t == 'mhni' and o < n < stop])
    songs = (2, 1)

    def covers(found):
        return ['covers %d' % sum(s for s, f in zip(songs, found) if f)]

    for name, bad, how in damaged(art):
        kind, tag, off = how['kind'], how.get('tag'), how.get('off')
        value = how.get('value')
        expect = []
        if kind == 'cut':
            expect = covers([min(e) <= how['cut'] for e in name_ends])
        elif kind == 'hlen' and (tag in ('mhfd', 'mhli')
                                 or (tag == 'mhsd' and off == sections[0])):
            expect = covers([False, False])
        elif tag == 'mhli' and kind in ('total', 'count'):
            # its count: none, or more than COUNT_MAX, or negative, is no
            # cover (as itunesdb.c's COUNT_MAX means); more than there are,
            # all
            expect = covers([value in ('tiny', 'past', 'most')] * 2)
        elif tag == 'mhii' and kind == 'hlen':
            expect = covers([off != images[0], off != images[1]])
        elif tag == 'mhii' and kind == 'total':
            # the next image not found
            expect = covers([True, off != images[0]])
        elif kind in ('count', 'loop', 'strlen', 'hlen', 'total') \
                and not (kind == 'count' and value == 'neg'):
            # the frames' own records, the album list, the file's total:
            # each image keeps a frame, as one damaged is passed over (as
            # 8cc113a9d3 does)
            expect = covers([True, True])
        yield name, bad, expect


PATHS = {t[0]: t[4].replace(':', '/') for t in TRACKS}


def playlist(file, ids):
    """a playlist written, with its songs in their order"""
    return '\t'.join(['playlist ' + file] + [PATHS[i] for i in ids])


def main():
    root = sys.argv[1]
    os.makedirs(root)
    made = 0

    def case(name, *args, **kwargs):
        nonlocal made
        Case(root, name).write(*args, **kwargs)
        made += 1

    db = itunesdb()
    art = artwork()
    whole_expect = ['tracks 4', 'loaded 1', 'complete 1', 'paths 4',
                    'name Test iPod', 'covers 3', 'shows 1',
                    playlist('/Playlists/Road Trip.m3u8', (102, 101, 103)),
                    playlist('/Playlists/Loved.m3u8', (102,))]
    case('whole', db, art, whole_expect)

    # a database of iTunes before 7: the short mhit, covers by the songs' dbids
    # (each image names one song: the second song of the album has none)
    case('old-mhit', itunesdb(old_mhit=True), artwork(old=True),
         ['tracks 4', 'complete 1', 'paths 4', 'covers 2'])
    # an odd number of songs: the import's records of their tags after a
    # table of as many 32-bit offsets (ubsan.supp)
    case('odd-tracks', itunesdb(tracks=TRACKS[:3]), artwork(),
         ['tracks 3', 'complete 1', 'paths 3', 'covers 3'])
    # no songs
    case('no-tracks', itunesdb(tracks=[]), None,
         ['tracks 0', 'loaded 1', 'complete 1', 'name Test iPod'])
    # a name cut in half of a surrogate pair, past the name's room
    case('name-long', itunesdb(name='\U0001F31F' * 100), None,
         ['tracks 4', 'paths 4'])
    # names cut by their room right after the first half of a surrogate
    # pair, the room of three bytes a unit filled by ideographs: a playlist's
    # (130 bytes, 43 units) and the iPod's (128, 42) (d710c4850f)
    case('names-cut-in-pair',
         itunesdb(name='\u6f22' * 41 + '\U0001F31F',
                  list_name='\u6f22' * 42 + '\U0001F31F' + 'end'), art,
         ['tracks 4', 'complete 1', 'name ' + '\u6f22' * 41])
    # the covers' frames past the end of their files
    case('ithmb-missing', db, art, ['tracks 4', 'covers 0'], ithmb=False)

    for name, bad, expect in damaged_itunesdb(db):
        case('itunesdb-' + name, bad, art, expect)
    for name, bad, expect in damaged_artworkdb(art):
        case('artworkdb-' + name, db, bad, ['tracks 4', 'paths 4'] + expect)
    with open(os.path.join(root, 'count'), 'w') as f:
        f.write('%d\n' % made)
    print('%d cases in %s' % (made, root))


if __name__ == '__main__':
    main()
