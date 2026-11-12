#!/usr/bin/env python3
"""Classic's small rating row with the unset stars as outlines.

    python3 make_classic_ratings.py <repo>/themes/iClassic-Square/.rockbox/wps <out folder> [--frames 10]

Reads the light theme's Ratings.bmp as the repository has it (64 x 130, 24 bit, ten frames of 13 rows: half a
star to five, a half being the star's left six columns) and writes, without any imaging library,

    <out>/iClassic-Square/Ratings.bmp        64 x 143, 24 bit, eleven frames of 13 rows: none, then half to five
    <out>/iClassic-Square-Dark/Ratings.bmp   the same through make_dark_theme.py's glyph_pixel()

The set stars are the repository's pixels, untouched (checked below).  In the place of every unset star is an
outline of 1 px: THE DRAWING is CENTRE and OUTLINE, a five-pointed star of Classic's own proportions (the set
star measures: centre 6.0, 6.6 in its 13 px cell, radius 6.4, inner radius 0.42 of it) drawn a little smaller,
so that it lies inside a set star's edge, and 0.3 px higher, so that the stroke of its arms is one whole row
(row 4) and not two half ones; in the grey of Classic's secondary text.  Of a half star the left six columns
are the repository's and the right seven the outline's.  With --frames 10 the frame of
none is left out: a file of the old size and frame count that drops in without a skin change (an unrated song
then shows nothing, as before).
"""
import math
import os
import struct
import sys

CELL, PITCH, STARS, WIDTH = 13, 13, 5, 64      # a frame's rows, a star's step, stars, the strip's width
CENTRE = (6.0, 6.3)                            # of the outline in its cell
OUTLINE = dict(radius=5.3, inner=0.42, width=1.0)
GREY = (0x76, 0x76, 0x76)                      # %Vf(767676): the album's line, the times
GROUND = (255, 255, 255)
SS = 8                                         # samples a pixel, each way
# make_dark_theme.py: BG, INK, luma(), mix(), glyph_pixel() for a grey picture
BG, INK = (18, 18, 20), (242, 242, 246)


def read_bmp(path):
    d = open(path, "rb").read()
    off, w, h, bpp = (struct.unpack_from("<I", d, 10)[0], struct.unpack_from("<i", d, 18)[0],
                      struct.unpack_from("<i", d, 22)[0], struct.unpack_from("<H", d, 28)[0])
    assert bpp == 24, path
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(abs(h)):
        o = off + (abs(h) - 1 - y if h > 0 else y) * stride
        rows.append([(d[o + x * 3 + 2], d[o + x * 3 + 1], d[o + x * 3]) for x in range(w)])
    return w, abs(h), rows


def write_bmp(path, rows):
    w, h = len(rows[0]), len(rows)
    stride = (w * 3 + 3) & ~3
    out = bytearray(b"BM" + struct.pack("<IHHI", 54 + stride * h, 0, 0, 54))
    out += struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, stride * h, 2835, 2835, 0, 0)
    for y in range(h - 1, -1, -1):
        line = bytearray()
        for r, g, b in rows[y]:
            line += bytes((b, g, r))
        out += line + bytes(stride - w * 3)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(out)


def star(cx, cy, radius, inner):
    return [(cx + math.cos(-math.pi / 2 + i * math.pi / 5) * (radius * inner if i % 2 else radius),
             cy + math.sin(-math.pi / 2 + i * math.pi / 5) * (radius * inner if i % 2 else radius))
            for i in range(10)]


def offset(poly, d):
    """The polygon with every edge moved d outwards (mitred corners); the star goes clockwise on the screen"""
    lines = []
    for i in range(len(poly)):
        (x1, y1), (x2, y2) = poly[i], poly[(i + 1) % len(poly)]
        n = math.hypot(x2 - x1, y2 - y1)
        nx, ny = (y2 - y1) / n, -(x2 - x1) / n          # to the left of the way round: outwards
        lines.append((x1 + nx * d, y1 + ny * d, x2 - x1, y2 - y1))
    out = []
    for i in range(len(lines)):
        ax, ay, adx, ady = lines[i - 1]
        bx, by, bdx, bdy = lines[i]
        t = ((bx - ax) * bdy - (by - ay) * bdx) / (adx * bdy - ady * bdx)
        out.append((ax + adx * t, ay + ady * t))
    return out


def inside(poly, x, y):
    c = False
    for i in range(len(poly)):
        (x1, y1), (x2, y2) = poly[i], poly[(i + 1) % len(poly)]
        if (y1 > y) != (y2 > y) and x < x1 + (y - y1) * (x2 - x1) / (y2 - y1):
            c = not c
    return c


def outline_cell():
    """How much of each pixel of a 13 x 13 cell the outline covers, 0..1"""
    path = star(CENTRE[0], CENTRE[1], OUTLINE["radius"], OUTLINE["inner"])
    outer, inner = offset(path, OUTLINE["width"] / 2), offset(path, -OUTLINE["width"] / 2)
    cell = []
    for y in range(CELL):
        row = []
        for x in range(PITCH):
            n = 0
            for j in range(SS):
                for i in range(SS):
                    px, py = x + (i + .5) / SS, y + (j + .5) / SS
                    n += inside(outer, px, py) and not inside(inner, px, py)
            row.append(n / (SS * SS))
        cell.append(row)
    return cell


def luma(p):
    return (54 * p[0] + 183 * p[1] + 19 * p[2]) >> 8


def mix(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def glyph_pixel(p):
    return mix(INK, BG, min(luma(p) / 236, 1))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    wps, out = args[0], args[1]
    with_none = "--frames" not in sys.argv or sys.argv[sys.argv.index("--frames") + 1] != "10"
    w, h, rows = read_bmp(os.path.join(wps, "iClassic-Square", "Ratings.bmp"))
    assert (w, h) == (WIDTH, 10 * CELL), "the repository's strip is not the one this was drawn for"
    cell = outline_cell()
    result = []
    for count in range(0 if with_none else 1, 11):          # half stars set
        frame = [list(rows[(count - 1) * CELL + y]) if count else [GROUND] * w for y in range(CELL)]
        for i in range(STARS):
            if i * 2 + 2 <= count:
                continue                                    # a set star: the repository's pixels
            first = PITCH // 2 if i * 2 + 1 == count else 0  # of a half the right side alone
            for y in range(CELL):
                for cx in range(first, PITCH):
                    x = i * PITCH + cx
                    if x < w and cell[y][cx]:
                        under = mix(GROUND, GREY, cell[y][cx])
                        frame[y][x] = tuple(min(a, b) for a, b in zip(frame[y][x], under))
        if count:   # nothing of a set star has changed
            for y in range(CELL):
                whole, half = count // 2, count % 2
                kept = whole * PITCH + (PITCH // 2 if half else 0)
                assert frame[y][:kept] == rows[(count - 1) * CELL + y][:kept], (count, y)
        result += frame
    write_bmp(os.path.join(out, "iClassic-Square", "Ratings.bmp"), result)
    write_bmp(os.path.join(out, "iClassic-Square-Dark", "Ratings.bmp"),
              [[glyph_pixel(p) for p in row] for row in result])
    print("%d x %d, %d frames of %d rows" % (w, len(result), len(result) // CELL, CELL))


if __name__ == "__main__":
    main()
