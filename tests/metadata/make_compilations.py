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

"""The codecs' fixtures again with the tags of a song part of a compilation,
as iTunes and the taggers mark one, or with a mark that says it isn't, for
check_compilations.py: each file with its tags rewritten, its sound as it
was.

    make_compilations.py <codecs' fixtures> <out dir>

writes the files and expected.txt beside them: <file> <mark 0|1> <titled
0|1>; and pairs.txt, of files whose tags fill the buffer before their
picture, each with the mark and without it. Its tagged_*() take other tags too, for the simulator's fixtures. Run
with python3 -I.
"""

import os
import struct
import sys

# ID3v2

def syncsafe(n):
    return bytes([(n >> 21) & 0x7f, (n >> 14) & 0x7f, (n >> 7) & 0x7f, n & 0x7f])


def id3_frame(version, fid, text):
    body = b'\x00' + text.encode('latin-1')
    if version == 2:
        return fid.encode() + len(body).to_bytes(3, 'big') + body
    size = syncsafe(len(body)) if version == 4 else len(body).to_bytes(4, 'big')
    return fid.encode() + size + b'\x00\x00' + body


def id3_tag(version, frames):
    """an ID3v2.<version> tag of (frame id, text) pairs"""
    body = b''.join(id3_frame(version, f, t) for f, t in frames)
    return b'ID3' + bytes([version, 0, 0]) + syncsafe(len(body)) + body


def mpeg_audio(data):
    """the frames of an MP3 file, past its ID3v2 tag"""
    if data[:3] == b'ID3':
        size = 0
        for b in data[6:10]:
            size = size << 7 | b
        data = data[10 + size + (10 if data[5] & 0x10 else 0):]
    if data[-128:-125] == b'TAG':
        data = data[:-128]
    return data


def id3v23_frame(fid, data):
    return fid.encode() + struct.pack('>I', len(data)) + b'\0\0' + data


def probe_mp3(audio, n, mark):
    """The layout that filled the tags' buffer before the picture: a title,
    four text frames of n bytes, the picture, and the mark of a compilation
    after it, before it or none (mark 'after', 'before', None)"""
    frames = [id3v23_frame('TIT2', b'\3Title')]
    for fid in ('TPE1', 'TALB', 'TCOM', 'TIT1'):
        frames.append(id3v23_frame(fid, b'\3' + b'x' * n))
    picture = id3v23_frame('APIC', b'\0image/jpeg\0\x03front cover\0'
                           + b'\xff\xd8\xff\xe0' + bytes(range(256)) * 80)
    tcmp = id3v23_frame('TCMP', b'\0' + b'1')
    frames += ([tcmp, picture] if mark == 'before' else
               [picture, tcmp] if mark == 'after' else [picture])
    body = b''.join(frames)
    return b'ID3\x03\x00\x00' + syncsafe(len(body)) + body + mpeg_audio(audio)


# the probe's text frames around those that fill the buffer, as a picture
# met with it full was placed otherwise: 534 to 539 bytes
PROBE_SIZES = range(526, 548)
# the mark's frame, which comes before the picture in 'before'
TCMP_FRAME = 12


def tagged_mp3(data, version, compilation, title='Song', artist='Artist',
               album='Album', album_artist=None):
    names = {2: ('TT2', 'TP1', 'TAL', 'TP2', 'TCP'),
             3: ('TIT2', 'TPE1', 'TALB', 'TPE2', 'TCMP'),
             4: ('TIT2', 'TPE1', 'TALB', 'TPE2', 'TCMP')}[version]
    frames = [(names[0], title), (names[1], artist), (names[2], album)]
    if album_artist:
        frames.append((names[3], album_artist))
    if compilation is not None:
        # after the others, and before them in another file
        frames.insert(len(frames) if version != 3 else 1,
                      (names[4], compilation))
    return id3_tag(version, frames) + mpeg_audio(data)


# MP4: ilst grows into the free atom after moov, so that mdat, which stco
# points into, stays where it is

def mp4_atoms(data, off, end):
    while off + 8 <= end:
        size, kind = struct.unpack('>I4s', data[off:off + 8])
        if size < 8:
            raise ValueError('an atom of no size')
        yield off, size, kind
        off += size


def mp4_find(data, path):
    """the offsets of the atoms on the path, from the top"""
    found, off, end = [], 0, len(data)
    for name in path:
        for o, size, kind in mp4_atoms(data, off, end):
            if kind == name:
                found.append((o, size))
                off, end = o + 8 + (4 if kind == b'meta' else 0), o + size
                break
        else:
            raise ValueError('no %s' % name)
    return found


def mp4_text(kind, text):
    v = text.encode('utf-8')
    return struct.pack('>I4sI4sII', 24 + len(v), kind, 16 + len(v), b'data',
                       1, 0) + v


def tagged_m4a(data, cpil, extra=()):
    """extra: (atom, text) pairs, as (b'\\xa9alb', 'Album')"""
    path = mp4_find(data, (b'moov', b'udta', b'meta', b'ilst'))
    ilst_off, ilst_size = path[-1]
    atom = struct.pack('>I4sI4sII', 25, b'cpil', 17, b'data', 21, 0) + bytes([cpil])
    atom += b''.join(mp4_text(k, t) for k, t in extra)
    moov_off, moov_size = path[0]
    free_off = moov_off + moov_size
    free_size, free_kind = struct.unpack('>I4s', data[free_off:free_off + 8])
    if free_kind != b'free' or free_size < len(atom) + 8:
        raise ValueError('no room after moov')
    out = bytearray(data[:ilst_off + ilst_size] + atom
                    + data[ilst_off + ilst_size:free_off])
    for off, size in path:
        struct.pack_into('>I', out, off, size + len(atom))
    out += struct.pack('>I4s', free_size - len(atom), b'free')
    out += data[free_off + 8 + len(atom):]
    assert len(out) == len(data)
    return bytes(out)


# Vorbis comments: FLAC's block, and Ogg's packet

def comments(vendor, items):
    out = struct.pack('<I', len(vendor)) + vendor + struct.pack('<I', len(items))
    for item in items:
        out += struct.pack('<I', len(item)) + item
    return out


def parse_comments(data):
    n = struct.unpack_from('<I', data, 0)[0]
    vendor = data[4:4 + n]
    off = 4 + n
    count = struct.unpack_from('<I', data, off)[0]
    off += 4
    items = []
    for _ in range(count):
        n = struct.unpack_from('<I', data, off)[0]
        items.append(data[off + 4:off + 4 + n])
        off += 4 + n
    return vendor, items, off


def tagged_flac(data, value, extra=()):
    assert data[:4] == b'fLaC'
    blocks, off = [], 4
    while True:
        head = data[off]
        size = int.from_bytes(data[off + 1:off + 4], 'big')
        blocks.append([head & 0x7f, data[off + 4:off + 4 + size]])
        off += 4 + size
        if head & 0x80:
            break
    audio = data[off:]
    for b in blocks:
        if b[0] == 4:
            vendor, items, _ = parse_comments(b[1])
            b[1] = comments(vendor, items + [b'TITLE=Song',
                                             b'COMPILATION=' + value.encode()]
                            + [e.encode() for e in extra])
    out = b'fLaC'
    for i, (kind, body) in enumerate(blocks):
        last = 0x80 if i == len(blocks) - 1 else 0
        out += bytes([kind | last]) + len(body).to_bytes(3, 'big') + body
    return out + audio


def ogg_crc(page):
    crc = 0
    for b in page:
        crc ^= b << 24
        for _ in range(8):
            crc = (crc << 1) ^ 0x04c11db7 if crc & 0x80000000 else crc << 1
            crc &= 0xffffffff
    return crc


def ogg_pages(data):
    off = 0
    while off < len(data):
        assert data[off:off + 4] == b'OggS'
        nsegs = data[off + 26]
        lacing = data[off + 27:off + 27 + nsegs]
        size = 27 + nsegs + sum(lacing)
        yield data[off:off + size]
        off += size


def ogg_page(header_type, granule, serial, seqno, packets_lacing, body):
    head = struct.pack('<4sBBqIII', b'OggS', 0, header_type, granule, serial,
                       seqno, 0) + bytes([len(packets_lacing)]) + bytes(packets_lacing)
    page = bytearray(head + body)
    struct.pack_into('<I', page, 22, ogg_crc(page))
    return bytes(page)


def tagged_ogg(data, value, extra=()):
    """the comment packet, the second, with COMPILATION=value; the header
    packets after the first laid out in pages again, and the pages after them
    numbered on"""
    pages = list(ogg_pages(data))
    serial = struct.unpack_from('<I', pages[0], 14)[0]
    # the header packets, from the pages after the first
    packets, current, used = [], b'', 1
    want = 3 if pages[0][28:35] == b'\x01vorbis' else 2
    while len(packets) < want - 1:
        page = pages[used]
        nsegs = page[26]
        lacing = page[27:27 + nsegs]
        off = 27 + nsegs
        for n in lacing:
            current += page[off:off + n]
            off += n
            if n < 255:
                packets.append(current)
                current = b''
        used += 1
        assert not (len(packets) >= want - 1 and current), 'audio on a header page'
    tags = packets[0]
    if tags[:7] == b'\x03vorbis':
        vendor, items, end = parse_comments(tags[7:])
        packets[0] = (b'\x03vorbis' + comments(vendor, items + [
            b'TITLE=Song', b'COMPILATION=' + value.encode()]
            + [e.encode() for e in extra]) + tags[7 + end:])
    else:
        assert tags[:8] == b'OpusTags'
        vendor, items, end = parse_comments(tags[8:])
        packets[0] = (b'OpusTags' + comments(vendor, items + [
            b'TITLE=Song', b'COMPILATION=' + value.encode()]
            + [e.encode() for e in extra]) + tags[8 + end:])
    out = [pages[0]]
    seqno = 1
    lacing, body = [], b''
    for p in packets:
        rest = p
        while True:
            n = min(len(rest), 255)
            lacing.append(n)
            body += rest[:n]
            rest = rest[n:]
            if len(lacing) == 255:
                out.append(ogg_page(0, 0, serial, seqno, lacing, body))
                seqno += 1
                lacing, body = [], b''
            if n < 255:
                break
    if lacing:
        out.append(ogg_page(0, 0, serial, seqno, lacing, body))
        seqno += 1
    for page in pages[used:]:
        page = bytearray(page)
        struct.pack_into('<I', page, 18, seqno)
        struct.pack_into('<I', page, 22, 0)
        struct.pack_into('<I', page, 22, ogg_crc(page))
        out.append(bytes(page))
        seqno += 1
    return b''.join(out)


# APEv2, at the end

def ape_tag(items):
    body = b''
    for key, value in items:
        v = value.encode('utf-8')
        body += struct.pack('<II', len(v), 0) + key.encode() + b'\x00' + v
    footer = struct.pack('<8sIIII8s', b'APETAGEX', 2000, len(body) + 32,
                         len(items), 0, bytes(8))
    return body + footer


def tagged_wv(data, value, extra=()):
    """extra: (key, value) pairs"""
    return data + ape_tag([('Title', 'Song'), ('Compilation', value)] + list(extra))


def make(fixtures, out):
    os.makedirs(out, exist_ok=True)

    def read(name):
        with open(os.path.join(fixtures, name), 'rb') as f:
            return f.read()

    mp3, m4a = read('mp3-128.mp3'), read('aac-lc-128.m4a')
    flac, ogg, opus, wv = (read('flac-16.flac'), read('vorbis-q4.ogg'),
                           read('opus-96.opus'), read('wavpack-16.wv'))
    # name, data, its mark, and whether it has a title too, which the mark
    # mustn't lose
    files = [
        ('id3v22-tcp.mp3', tagged_mp3(mp3, 2, '1'), 1, 1),
        ('id3v23-tcmp.mp3', tagged_mp3(mp3, 3, '1'), 1, 1),
        ('id3v24-tcmp.mp3', tagged_mp3(mp3, 4, '1', album_artist='Various Artists'), 1, 1),
        ('id3v23-tcmp0.mp3', tagged_mp3(mp3, 3, '0'), 0, 1),
        ('id3v24-none.mp3', tagged_mp3(mp3, 4, None), 0, 1),
        ('cpil.m4a', tagged_m4a(m4a, 1), 1, 0),
        ('cpil0.m4a', tagged_m4a(m4a, 0), 0, 0),
        ('none.m4a', m4a, 0, 0),
        ('compilation.flac', tagged_flac(flac, '1'), 1, 1),
        ('compilation0.flac', tagged_flac(flac, '0'), 0, 1),
        ('none.flac', flac, 0, 0),
        ('compilation.ogg', tagged_ogg(ogg, '1'), 1, 1),
        ('none.ogg', ogg, 0, 0),
        ('compilation.opus', tagged_ogg(opus, '1'), 1, 1),
        ('ape-compilation.wv', tagged_wv(wv, '1'), 1, 1),
        ('ape-compilation0.wv', tagged_wv(wv, '0'), 0, 1),
        ('none.wv', wv, 0, 0),
    ]
    with open(os.path.join(out, 'expected.txt'), 'w') as exp:
        for name, data, flag, titled in files:
            with open(os.path.join(out, name), 'wb') as f:
                f.write(data)
            exp.write('%s %d %d\n' % (name, flag, titled))
    # and the files whose other tags and picture must read as without the
    # mark: <with> <without> <how far the picture moved, by the mark's frame>
    with open(os.path.join(out, 'pairs.txt'), 'w') as pairs:
        for n in PROBE_SIZES:
            none = 'probe-%d-none.mp3' % n
            with open(os.path.join(out, none), 'wb') as f:
                f.write(probe_mp3(mp3, n, None))
            for mark, shift in (('after', 0), ('before', TCMP_FRAME)):
                name = 'probe-%d-%s.mp3' % (n, mark)
                with open(os.path.join(out, name), 'wb') as f:
                    f.write(probe_mp3(mp3, n, mark))
                pairs.write('%s %s %d\n' % (name, none, shift))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    make(sys.argv[1], sys.argv[2])
