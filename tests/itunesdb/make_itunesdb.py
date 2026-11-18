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
    unwritten <file>  no playlist written as this file
    notes <path>\t<text>  the notes of the podcast at this path, as Now
                    Playing reads them (itunesdb_read_notes())
    meta <path>\t<title>\t<artist>\t<album>\t<album artist>\t<year>\t<plays>\t<rating>
                    what the import's records give for the track at this
                    path (itunesdb_get_metadata()), "" for a tag it hasn't
    survive         only that nothing breaks: no sanitizer error, no hang
    known <how> <why>  a failure known and left as it is, of this kind
                    (check, timeout, crash): reported, not counted

Every case also reads no track twice, and asks the import's records for
every track read. A damaged case expects what reading it gives where that is
plainly right (damaged_itunesdb(), damaged_artworkdb(), combos(), pins()),
and only to survive elsewhere. A record lies within its parent, after the
parent's header, with its own header within it, or it isn't read and its walk
ends there (mh_walk.h); a string is read within its record
(mhod_string_size()); the tracks are all read when they end where their
section does. The file "count" says how many cases there are. Run with
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
         sound_check=0, played=0, bookmark=0, compilation=0):
    h = bytearray(header_len)
    h[0:4] = b'mhit'
    put32(h, 4, header_len)
    put32(h, 12, len(mhods))
    put32(h, 16, tid)
    put32(h, 20, 1)
    h[30] = compilation
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


def mhyp(name, items, *, hidden=0, podcast=0, pid=0, sort=1, extra_mhods=(),
         name_after=0):
    """name_after: as many mhods of type 100 before its name"""
    mhods = ([mhod_raw(100, le32(0) * 4)] * name_after
             + [mhod_string(1, name)] + list(extra_mhods))
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
     ':iPod_Control:Music:F02:AAAC.m4a', dict(year=2024, art_id=202,
                                              compilation=1)),
    (104, 'Episode 1', 'A Show', 'A Show',
     ':iPod_Control:Music:F03:AAAD.mp3', dict(media=4, played=2,
                                              bookmark=61000)),
]


# a podcast's subtitle and description, its notes in Now Playing: the
# description, or else the subtitle
NOTES = {104: ('A subtitle', 'The notes of the episode')}


def meta(tid, old_mhit=False, **tags):
    """the "meta" line of a track of TRACKS, its tags as tags gives them
    instead"""
    for t, title, artist, album, path, extra in TRACKS:
        if t == tid:
            break
    fields = dict(path=path.replace(':', '/'), title=title, artist=artist,
                  album=album,
                  album_artist='Test Band' if tid < 103 and not old_mhit else '',
                  year=extra.get('year', 0), plays=extra.get('plays', 0),
                  rating=extra.get('rating', 0) // 10)
    fields.update(tags)
    return 'meta ' + '\t'.join(str(fields[k]) for k in (
        'path', 'title', 'artist', 'album', 'album_artist', 'year', 'plays',
        'rating'))


def itunesdb(name='Test iPod', tracks=TRACKS, old_mhit=False,
             list_name='Road Trip', name_after=0, podcasts_first=False):
    items = []
    for tid, title, artist, album, path, extra in tracks:
        extra = dict(extra)
        notes = [mhod_string(18, NOTES[tid][0]), mhod_string(14, NOTES[tid][1])
                 ] if tid in NOTES else []
        if old_mhit:
            art = extra.pop('art_id', 0)
            extra['art_count'] = 1 if art else 0
            items.append(mhit(tid, track_mhods(title, artist, album, path)
                              + notes,
                              header_len=0x9c, dbid=0x1000 + tid, **extra))
        else:
            items.append(mhit(tid, track_mhods(title, artist, album, path,
                                               'Test Band' if tid < 103 else None)
                              + notes, dbid=0x1000 + tid, **extra))
    ids = [t[0] for t in tracks]
    master = mhyp(name, [mhip(i) for i in ids], hidden=1, pid=1,
                  name_after=name_after)
    road = mhyp(list_name, [mhip(i) for i in (102, 101, 103)], pid=2)
    smart = mhyp('Loved', [mhip(102)], pid=3, extra_mhods=smart_mhods())
    podcasts = mhyp('Podcasts', [mhip(0, 0x100, 900), mhip(104, 0, 0, 900)],
                    podcast=1, pid=4)
    lists = [master, road, smart]
    # podcasts_first: the podcasts' playlist first in its section, not last
    return mhbd([mhsd(1, mhlt(items)),
                 mhsd(3, mhlp([podcasts] + lists if podcasts_first
                              else lists + [podcasts])),
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


def artworkdb(images, images_last=False):
    """images_last: after the albums' section and one of files (type 3)"""
    items = b''.join(images)
    mhli = pad(b'mhli' + le32(0x5c) + le32(len(images)), 0x5c) + items
    sd1 = pad(b'mhsd' + le32(0x60) + le32(0x60 + len(mhli)) + le16(1), 0x60) + mhli
    mhla = pad(b'mhla' + le32(0x5c) + le32(0), 0x5c)
    sd2 = pad(b'mhsd' + le32(0x60) + le32(0x60 + len(mhla)) + le16(2), 0x60) + mhla
    sections = [sd1, sd2]
    if images_last:
        mhlf = pad(b'mhlf' + le32(0x5c) + le32(0), 0x5c)
        sd3 = pad(b'mhsd' + le32(0x60) + le32(0x60 + len(mhlf)) + le16(3),
                  0x60) + mhlf
        sections = [sd2, sd3, sd1]
    body = b''.join(sections)
    h = bytearray(0x84)
    h[0:4] = b'mhfd'
    put32(h, 4, 0x84)
    put32(h, 8, 0x84 + len(body))
    put32(h, 16, 2)
    put32(h, 20, len(sections))
    return bytes(h) + body


def artwork(old=False, images_last=False):
    """two covers, each in both sizes, one after the other in the files"""
    images = []
    for n, (image_id, tid) in enumerate(((201, 101), (202, 103))):
        frames = [mhni(fmt, n * w * h * 2, w, h, 1)
                  for fmt, (w, h) in FORMATS.items()]
        images.append(mhii(image_id, 0x1000 + tid if old else 0, frames))
    return artworkdb(images, images_last)


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


def broken(kind, value):
    """a record whose damaged length doesn't keep it within its parent, or
    its header within it, isn't read, and its walk ends there (mh_walk.h):
    any total damaged (0, past the end, negative, less than its header), a
    loop's total that leads back, a header of 0 or past the total. A header
    of 3 keeps it: what comes after it isn't found."""
    return kind in ('total', 'loop') or (kind == 'hlen' and value != 'tiny')


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
    playlists = max(o for o, t in recs if t == 'mhsd')
    for name, bad, how in damaged(db):
        kind, tag, off = how['kind'], how.get('tag'), how.get('off')
        value = how.get('value')
        expect = ['survive']
        if kind == 'cut':
            # a track has its path when its record is there up to it
            expect = ['paths %d' % sum(e <= how['cut'] for e in path_ends)]
        elif tag == 'mhbd' and kind == 'total' and value in ('past', 'max'):
            # longer than the file: the file ends its sections as well
            expect = list(whole)
        elif tag == 'mhbd' and kind in ('hlen', 'total'):
            # broken, or its sections not found
            expect = none + ['name iPod']
        elif tag == 'mhsd' and off == mhsd1 and kind in ('hlen', 'total'):
            # the tracks' section broken, and the walk of the sections ends
            # there; with a header of 3 its list isn't found, and the walk
            # goes on to the name
            expect = none + ['name ' + ('iPod' if broken(kind, value)
                                        else 'Test iPod')]
        elif tag == 'mhsd' and off == playlists:
            # the playlists' section, the last: the name in it not found
            expect = list(whole) + ['name iPod']
        elif off is not None and off >= tracks_end:
            # past the tracks, in the playlists: the tracks are all read
            expect = list(whole)
        elif tag == 'mhbd' and kind == 'count':
            # the sections read until the next record isn't one; a negative
            # count reads none (as 8cc113a9d3 does)
            expect = none if value == 'neg' else list(whole)
        elif tag == 'mhlt' and kind == 'hlen':
            expect = none
        elif tag == 'mhlt' and kind in ('total', 'count'):
            # its count (at 8): more than COUNT_MAX, or negative, is refused;
            # more than there are reads those there are, fewer as many, and
            # either is incomplete, as the last read doesn't end where the
            # section does. None, with tracks after it, isn't a library
            # emptied but one not read.
            expect = {'tiny': ['tracks 3', 'complete 0', 'paths 3'],
                      'past': ['tracks 4', 'complete 0', 'paths 4'],
                      'most': ['tracks 4', 'complete 0', 'paths 4']
                      }.get(value, none)
        elif tag == 'mhit' and broken(kind, value):
            # the track broken: those before it, once each, incomplete; with
            # none before it, not loaded
            k = mhits.index(off)
            expect = (['tracks %d' % k, 'complete 0', 'paths %d' % k] if k
                      else list(none)) + ['absent %d' % i for i in ids[k:]]
        elif tag == 'mhit' and kind == 'hlen':
            # a header of 3, shorter than any mhit's (0x9c): read as no track
            # (P2 of review-1), its mhods not found, but counted among the
            # tracks, and the read complete (as 8cc113a9d3 does)
            k = mhits.index(off)
            expect = ['tracks 4', 'complete 1', 'paths 3', 'absent %d' % ids[k]]
        elif tag == 'mhit' and kind == 'count':
            # mhods read until the next record isn't one; a negative count
            # reads none, so no path (as 8cc113a9d3 does)
            expect = (['tracks 4', 'complete 1', 'paths 3'] if value == 'neg'
                      else list(whole))
        elif tag == 'mhod' and broken(kind, value) and mhits[0] < off < mhits[1]:
            # one of the first track's mhods broken, its title's, or its
            # path's that leads back to it: the path not read
            expect = ['tracks 4', 'complete 1', 'paths 3']
        elif tag == 'mhod' and off == first_mhod and kind == 'hlen':
            # a header of 3: a string's mhod is read at its fixed offsets,
            # libgpod's, not by the length its header gives
            expect = list(whole)
        elif kind == 'strlen' and mhits[0] < off < mhits[1]:
            # a string of the first track longer than its record: read to the
            # record's end, which is the string's; one of 7 bytes, 3 units
            field, text = {1: ('title', TRACKS[0][1]), 2: ('path', TRACKS[0][4]),
                           3: ('album', TRACKS[0][3])}[u32(db, off + 12)]
            if value == 'odd':
                text = text[:3]
            if field == 'path':
                text = text.replace(':', '/')
            expect = list(whole) + [meta(101, **{field: text})]
        if name.startswith('loop-mhsd'):
            # the second section leading back to the first: the playlists'
            # section, the third, isn't reached, nor the name in it
            expect.append('name iPod')
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
    # the first image's first frame: its mhod
    first_frame = images[0] + u32(art, images[0] + 4)

    def covers(found):
        return ['covers %d' % sum(s for s, f in zip(songs, found) if f)]

    for name, bad, how in damaged(art):
        kind, tag, off = how['kind'], how.get('tag'), how.get('off')
        value = how.get('value')
        expect = []
        if kind == 'cut':
            expect = covers([min(e) <= how['cut'] for e in name_ends])
        elif tag == 'mhfd' and kind == 'total':
            # longer than the file, which ends its sections as well; broken
            expect = covers([value in ('past', 'max')] * 2)
        elif kind == 'hlen' and (tag in ('mhfd', 'mhli')
                                 or (tag == 'mhsd' and off == sections[0])):
            expect = covers([False, False])
        elif tag == 'mhsd' and off == sections[0] and kind == 'total':
            # the images' section broken
            expect = covers([False, False])
        elif tag == 'mhli' and kind in ('total', 'count'):
            # its count: none, or more than COUNT_MAX, or negative, is no
            # cover (as itunesdb.c's COUNT_MAX means); more than there are,
            # all
            expect = covers([value in ('tiny', 'past', 'most')] * 2)
        elif tag == 'mhii' and broken(kind, value):
            # the image broken, and the walk of the images ends there
            k = images.index(off)
            expect = covers([i < k for i in range(len(images))])
        elif tag == 'mhii' and kind == 'hlen':
            # a header of 3: that image's frames not found
            expect = covers([off != images[0], off != images[1]])
        elif tag == 'mhod' and off == first_frame and broken(kind, value):
            # the first frame's mhod broken: the walk of the image's frames
            # ends at it, and the image has none
            expect = covers([False, True])
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


ROAD_TRIP = playlist('/Playlists/Road Trip.m3u8', (102, 101, 103))
LOVED = playlist('/Playlists/Loved.m3u8', (102,))


def looped(data, first, second, count_at, count=0x7fffffff):
    """the record at second leading back to the one at first, and the count
    at count_at, which bounds that walk, too big to end it"""
    b = bytearray(data)
    put32(b, second + 8, (first - second) & 0xffffffff)
    put32(b, count_at, count)
    return bytes(b)


def combos(db, art, whole):
    """A loop of records in each walk, with the count that bounds the walk
    too big to end it: on the iPod's 32-bit offsets a length can lead back,
    and went round for minutes there (review-1 of itdbfix, N4). The walk ends
    where the next record isn't further on; what is before it is read as
    without the loop. As (name, iTunesDB, ArtworkDB, expect); whole is the
    expect of db and art."""
    def within(data, tag, start):
        end = start + u32(data, start + 8)
        return [o for o, t in records(data) if t == tag and start < o < end]

    def name(data, at, count_at, field):
        file = 'itunesdb' if data[:4] == b'mhbd' else 'artworkdb'
        return '%s-loop-%s@%d+count-%s+%d@%d' % (
            file, data[at:at + 4].decode(), at,
            data[count_at:count_at + 4].decode(), field, count_at)

    tracks_only = ['tracks 4', 'complete 1', 'paths 4']
    sections = {u32(db, o + 12): o for o, t in records(db) if t == 'mhsd'}
    order = sorted(sections.values())
    # the sections, the podcasts' (3) after the tracks' (1), the playlists'
    # (2) last: the one that leads back isn't read, nor what comes after it,
    # and at the second that leaves the shows, the playlists and the name
    # unread
    second, last = order[1], order[2]
    unread = ['unwritten /Playlists/Road Trip.m3u8',
              'unwritten /Playlists/Loved.m3u8']
    yield (name(db, second, 0, 20), looped(db, order[0], second, 20), art,
           tracks_only + ['shows 0', 'covers 3', 'name iPod'] + unread)
    yield (name(db, last, 0, 20), looped(db, order[0], last, 20), art,
           tracks_only + ['shows 1', 'covers 3', 'name iPod'] + unread)
    # the tracks: those before the loop, incomplete; the count no more than
    # COUNT_MAX, as more isn't read at all
    mhlt = sections[1] + u32(db, sections[1] + 4)
    mhits = within(db, 'mhit', sections[1])
    yield (name(db, mhits[1], mhlt, 8),
           looped(db, mhits[0], mhits[1], mhlt + 8, 1 << 20), art,
           ['tracks 1', 'complete 0', 'paths 1', 'absent 102', 'absent 103',
            'absent 104'])
    # a track's mhods: the path leading back to the title isn't read, and
    # the track has no path, nor its cover, nor a place in a playlist
    mhods = within(db, 'mhod', mhits[0])
    yield (name(db, mhods[1], mhits[0], 12),
           looped(db, mhods[0], mhods[1], mhits[0] + 12), art,
           ['tracks 4', 'complete 1', 'paths 3', 'covers 2', 'shows 1',
            'name Test iPod', LOVED,
            playlist('/Playlists/Road Trip.m3u8', (102, 103))])
    # the playlists (section 2): the one leading back isn't read, nor those
    # after it, and the old list is kept
    mhlp = sections[2] + u32(db, sections[2] + 4)
    lists = within(db, 'mhyp', sections[2])
    yield (name(db, lists[1], mhlp, 8),
           looped(db, lists[0], lists[1], mhlp + 8), art,
           tracks_only + ['name Test iPod'] + unread)
    # a playlist's songs: it not written, those after it are
    items = within(db, 'mhip', lists[1])
    yield (name(db, items[1], lists[1], 16),
           looped(db, items[0], items[1], lists[1] + 16), art,
           tracks_only + ['name Test iPod', LOVED,
                          'unwritten /Playlists/Road Trip.m3u8'])
    # a smart playlist's mhods, its rules leading back to its limits
    mhods = within(db, 'mhod', lists[2])
    yield (name(db, mhods[2], lists[2], 12),
           looped(db, mhods[1], mhods[2], lists[2] + 12), art,
           tracks_only + ['name Test iPod', ROAD_TRIP,
                          'unwritten /Playlists/Loved.m3u8'])
    # the podcasts' playlists (section 3), counted for the shows: the shows'
    # list past the loop, none counted
    mhlp = sections[3] + u32(db, sections[3] + 4)
    lists = within(db, 'mhyp', sections[3])
    yield (name(db, lists[1], mhlp, 8),
           looped(db, lists[0], lists[1], mhlp + 8), art,
           [w for w in whole if w != 'shows 1'] + ['shows 0'])
    # the shows' items: the show's header before the loop
    items = within(db, 'mhip', lists[3])
    yield (name(db, items[1], lists[3], 16),
           looped(db, items[0], items[1], lists[3] + 16), art, whole)
    # the name, after two other mhods of the hidden playlist (section 2), as
    # read_device_name() looks for it
    named = itunesdb(name_after=2)
    sections = {u32(named, o + 12): o for o, t in records(named)
                if t == 'mhsd'}
    master = within(named, 'mhyp', sections[2])[0]
    mhods = within(named, 'mhod', master)
    yield ('name-after-mhods', named, art, whole)
    yield (name(named, mhods[1], master, 12),
           looped(named, mhods[0], mhods[1], master + 12), art,
           [w for w in whole if not w.startswith('name ')] + ['name iPod'])

    # ArtworkDB: the images, the first before the loop
    found = ['tracks 4', 'paths 4']
    mhli = [o for o, t in records(art) if t == 'mhli'][0]
    images = [o for o, t in records(art) if t == 'mhii']
    yield (name(art, images[1], mhli, 8), db,
           looped(art, images[0], images[1], mhli + 8, 1 << 20),
           found + ['covers 2'])
    # an image's frames: the first before the loop, which the image keeps
    frames = [o for o in within(art, 'mhod', images[0])
              if u32(art, o + 12) & 0xffff == 2]
    yield (name(art, frames[1], images[0], 12), db,
           looped(art, frames[0], frames[1], images[0] + 12),
           found + ['covers 3'])
    # the images' section after the albums' and the files': read, and past
    # a loop of those two, not
    later = artwork(images_last=True)
    sections = [o for o, t in records(later) if t == 'mhsd']
    yield ('images-last', db, later, found + ['covers 3'])
    yield (name(later, sections[1], 0, 20), db,
           looped(later, sections[0], sections[1], 20), found + ['covers 0'])


def notes(tid, text):
    """the notes of a podcast, as Now Playing reads them"""
    return 'notes %s\t%s' % (PATHS[tid], text)


def pins(db, art):
    """Cases each of a bound the others leave free (review-1 of itdbwalks,
    N2), as combos() has them"""
    recs = records(db)
    mhits = [o for o, t in recs if t == 'mhit']
    tracks_only = ['tracks 4', 'complete 1', 'paths 4']
    # a length of 0, with a count too big to end its walk: not a record, or
    # the walk would read it again and again. The first track's path's.
    path = [o for o, t in recs if t == 'mhod' and mhits[0] < o < mhits[1]
            and u32(db, o + 12) == 2][0]
    b = bytearray(db)
    put32(b, path + 8, 0)
    put32(b, mhits[0] + 12, 0x7fffffff)
    yield ('itunesdb-total-mhod@%d-zero+count-mhit+12@%d' % (path, mhits[0]),
           bytes(b), art, ['tracks 4', 'complete 1', 'paths 3'])
    # the podcast's description, its notes, with a total that leaves it a
    # byte of text: no notes, and its subtitle instead
    k = [t[0] for t in TRACKS].index(104)
    desc = [o for o, t in recs if t == 'mhod' and o > mhits[k]
            and u32(db, o + 12) == 14][0]
    b = bytearray(db)
    put32(b, desc + 8, 41)
    yield ('itunesdb-notes-past-record', bytes(b), art,
           tracks_only + [notes(104, NOTES[104][0])])
    # the names of both frames of the second image, their totals cut to
    # ":F", which names no file: the image has no frame, its song no cover
    arecs = records(art)
    second = [o for o, t in arecs if t == 'mhii'][1]
    b = bytearray(art)
    for o, t in arecs:
        if t == 'mhod' and o > second and u32(art, o + 12) & 0xffff == 3:
            put32(b, o + 8, 40)
    yield ('artworkdb-names-past-record', db, bytes(b),
           ['tracks 4', 'paths 4', 'covers 2'])
    # a record a byte past its parent, inside the parent's parent, at each
    # level whose children are bounded by it (review-2 of itdbwalks, N2): the
    # last item of Road Trip, the name of the hidden playlist, which the
    # iPod's name is, and the header item of the podcasts' show, its playlist
    # first in its section so that the section goes on after it
    def within(start, data=db):
        end = start + u32(data, start + 8)
        return end, [o for o, t in records(data) if start < o < end]

    sections = {u32(db, o + 12): o for o, t in recs if t == 'mhsd'}
    lists2 = [o for o in within(sections[2])[1] if db[o:o + 4] == b'mhyp']
    road_end, road = within(lists2[1])
    item = [o for o in road if db[o:o + 4] == b'mhip'][-1]
    b = bytearray(db)
    put32(b, item + 8, u32(db, item + 8) + 1)
    yield ('itunesdb-mhip@%d-past-mhyp' % item, bytes(b), art,
           tracks_only + ['name Test iPod', LOVED,
                          'unwritten /Playlists/Road Trip.m3u8'])
    master_end, master = within(lists2[0])
    name = [o for o in master if db[o:o + 4] == b'mhod'][0]
    b = bytearray(db)
    put32(b, name + 8, master_end - name + 1)
    yield ('itunesdb-mhod@%d-past-mhyp' % name, bytes(b), art,
           tracks_only + ['name iPod', ROAD_TRIP, LOVED])
    first = itunesdb(podcasts_first=True)
    yield ('podcasts-first', first, art,
           tracks_only + ['shows 1', 'name Test iPod', ROAD_TRIP, LOVED])
    section = [o for o, t in records(first) if t == 'mhsd'
               and u32(first, o + 12) == 3][0]
    show = [o for o in within(section, first)[1]
            if first[o:o + 4] == b'mhyp'][0]
    show_end, items = within(show, first)
    header = [o for o in items if first[o:o + 4] == b'mhip'][0]
    b = bytearray(first)
    put32(b, header + 8, show_end - header + 1)
    yield ('itunesdb-mhip@%d-past-mhyp' % header, bytes(b), art,
           tracks_only + ['shows 0', 'name Test iPod', ROAD_TRIP, LOVED])
    # the same names' records longer than their mhni, the frame's record:
    # outside it, not read, though their text is whole
    b = bytearray(art)
    for o, t in arecs:
        if t == 'mhod' and o > second and u32(art, o + 12) & 0xffff == 3:
            put32(b, o + 8, u32(art, o + 8) + 100)
    yield ('artworkdb-names-past-mhni', db, bytes(b),
           ['tracks 4', 'paths 4', 'covers 2'])


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
                    'name Test iPod', 'covers 3', 'shows 1', ROAD_TRIP,
                    LOVED]
    case('whole', db, art,
         whole_expect + [meta(t[0]) for t in TRACKS]
         + [notes(104, NOTES[104][1])])

    # a database of iTunes before 7: the short mhit, covers by the songs' dbids
    # (each image names one song: the second song of the album has none)
    case('old-mhit', itunesdb(old_mhit=True), artwork(old=True),
         ['tracks 4', 'complete 1', 'paths 4', 'covers 2']
         + [meta(t[0], old_mhit=True) for t in TRACKS])
    # an odd number of songs: the import's records of their tags, which start
    # with a uint64_t, after a table of as many 32-bit offsets
    case('odd-tracks', itunesdb(tracks=TRACKS[:3]), artwork(),
         ['tracks 3', 'complete 1', 'paths 3', 'covers 3']
         + [meta(t[0]) for t in TRACKS[:3]])
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
    for name, bad_db, bad_art, expect in combos(db, art, whole_expect):
        case(name, bad_db, bad_art, expect)
    for name, bad_db, bad_art, expect in pins(db, art):
        case(name, bad_db, bad_art, expect)
    with open(os.path.join(root, 'count'), 'w') as f:
        f.write('%d\n' % made)
    print('%d cases in %s' % (made, root))


if __name__ == '__main__':
    main()
