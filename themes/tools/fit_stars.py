#!/usr/bin/env python3
"""Start Inter's stars where its letters start, for make_fonts.sh:

    fit_stars.py <font made by convttf>

The names of Finder's rating playlists, "★ – dislike" to "★★★★★", start
with U+2605, and U+2606 is its outline. Inter gives both a left side bearing
of 186 units, its capitals 105 to 125 on average by weight, and the star's
left point is thin: in every size its first ink came about a pixel to the
right of the first ink of a name that starts with a letter, in a list and
in a header over its second line. Takes as many blank columns off the left
of both stars as bring their left edge nearest the capitals' (the mean of A
to Z), and as many off their width, so that what follows a star keeps its
place by it. An edge is where its column's darkest pixel says the ink
starts: a column of 40 % ink has 0.4 of a column of it. The stars' own
pixels, height and right side stay as they are. Run again on its own
output, it changes nothing; merge_fonts.py keeps the stars as they are here.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from merge_fonts import HIGH, HEADER, LOW, Fnt  # noqa: E402

STARS = (0x2605, 0x2606)
CAPITALS = range(0x41, 0x5B)


def columns(f, c):
    """the glyph's rows of pixels, 0 full ink to 15 none, and each column's
    darkest"""
    bits, w = f.glyph(c)
    nib = bytearray(2 * len(bits))
    nib[0::2] = bits.translate(LOW)
    nib[1::2] = bits.translate(HIGH)
    rows = [nib[y * w:(y + 1) * w] for y in range(f.height)]
    return rows, [min(r[x] for r in rows) for x in range(w)]


def left_edge(f, c):
    _, darkest = columns(f, c)
    x = next(x for x, v in enumerate(darkest) if v < 15)
    return x + 1 - (15 - darkest[x]) / 15


def pack(rows):
    """as convttf packs a glyph: a pixel a half byte, the first in the low
    half, the last byte's high half 0 if the pixels are odd"""
    nib = bytes(v for r in rows for v in r)
    if len(nib) & 1:
        nib += b"\0"
    return bytes(nib[i] | nib[i + 1] << 4 for i in range(0, len(nib), 2))


def main():
    path = sys.argv[1]
    f = Fnt(path)
    capitals = sum(left_edge(f, c) for c in CAPITALS) / len(CAPITALS)
    star = left_edge(f, STARS[0])
    cut = max(0, round(star - capitals))
    glyphs = []
    for c in STARS:
        rows, darkest = columns(f, c)
        cut = min(cut, next(x for x, v in enumerate(darkest) if v < 15))
        glyphs.append(rows)
    if cut == 0:
        return
    # convttf puts the bits of the codes in their order, unpadded
    first, second = (f.offsets[c - f.first] for c in STARS)
    end = second + (f.height * f.width(STARS[1]) + 1) // 2
    assert first + (f.height * f.width(STARS[0]) + 1) // 2 == second
    packed = [pack([r[cut:] for r in rows]) for rows in glyphs]
    bits = f.bits[:first] + packed[0] + packed[1] + f.bits[end:]
    moved = len(bits) - len(f.bits)
    offsets = [o + moved if o >= end else o for o in f.offsets]
    offsets[STARS[1] - f.first] = first + len(packed[0])
    widths = bytearray(f.widths)
    for c in STARS:
        widths[c - f.first] -= cut
    long_offsets = len(f.bits) >= 0xFFDB
    assert (len(bits) >= 0xFFDB) == long_offsets
    out = bytearray(open(path, "rb").read()[:HEADER])
    if f.maxwidth == max(f.widths):
        struct.pack_into("<H", out, 4, max(widths))
    struct.pack_into("<I", out, 24, len(bits))
    out += bits
    out += b"\0" * (-len(out) & (3 if long_offsets else 1))
    out += struct.pack("<%d%s" % (len(offsets), "I" if long_offsets else "H"), *offsets)
    out += widths
    with open(path, "wb") as g:
        g.write(out)
    print("%s: the stars %d column%s to the left, their edge %.2f to %.2f, the capitals' %.2f"
          % (os.path.basename(path), cut, "s" * (cut > 1), star, star - cut, capitals))


if __name__ == "__main__":
    main()
