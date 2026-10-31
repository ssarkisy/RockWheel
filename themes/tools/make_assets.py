#!/usr/bin/env python3
"""Draw the extra bitmaps the iPod themes need for the WPS display modes.

Plain Python, no imaging libraries. Usage:

    python3 themes/tools/make_assets.py

writes into the image folder of each theme:
  Stars.bmp        rating view, 6 frames (0..5 stars) stacked vertically,
                   covering the progress bar trough of the backdrop
  Scrub Knob.bmp   scrubber slider, 32 bit with alpha, as tall as the bar
  Art Mask.bmp     album art view, hides the backdrop next to the art
and, for iClassic, the menus' backdrop with its placeholder note in the
middle of the right panel, from originals/iClassic_bd.bmp
"""
import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ICLASSIC = os.path.join(HERE, '..', 'iClassic-Square', '.rockbox', 'wps',
                        'iClassic-Square')

SS = 4  # supersampling per axis


def read_bmp(path):
    d = open(path, 'rb').read()
    off, = struct.unpack_from('<I', d, 10)
    w, h, _, bpp = struct.unpack_from('<iiHH', d, 18)
    bottom_up = h > 0
    h = abs(h)
    row = (w * bpp // 8 + 3) & ~3
    px = []
    for y in range(h):
        base = off + (h - 1 - y if bottom_up else y) * row
        px.append([tuple(reversed(d[base + x * bpp // 8:
                                    base + x * bpp // 8 + 3]))
                   for x in range(w)])
    return px


def write_bmp(path, px, alpha=None):
    """px: rows of (r, g, b); alpha: rows of 0..255 for a 32 bit image"""
    h, w = len(px), len(px[0])
    bpp = 32 if alpha else 24
    row = (w * bpp // 8 + 3) & ~3
    data = bytearray()
    for y in range(h - 1, -1, -1):
        line = bytearray()
        for x in range(w):
            r, g, b = px[y][x]
            line += bytes((b, g, r))
            if alpha:
                line.append(alpha[y][x])
        data += line + bytes(row - len(line))
    header = struct.pack('<2sIHHI', b'BM', 54 + len(data), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, w, h, 1, bpp, 0, len(data),
                       2835, 2835, 0, 0)
    with open(path, 'wb') as f:
        f.write(header + info + data)


def coverage(inside, x, y):
    """fraction of pixel (x, y) for which inside(px, py) holds"""
    hit = 0
    for sy in range(SS):
        for sx in range(SS):
            if inside(x + (sx + 0.5) / SS, y + (sy + 0.5) / SS):
                hit += 1
    return hit / (SS * SS)


def blend(c0, c1, a):
    return tuple(round(c0[i] * (1 - a) + c1[i] * a) for i in range(3))


def star(cx, cy, r_out, r_in):
    pts = []
    for i in range(10):
        r = r_out if i % 2 == 0 else r_in
        ang = -math.pi / 2 + i * math.pi / 5
        pts.append((cx + r * math.cos(ang), cy + r * math.sin(ang)))

    def inside(x, y):
        c = False
        for i in range(10):
            (x1, y1), (x2, y2) = pts[i], pts[i - 1]
            if (y1 > y) != (y2 > y) and \
               x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
                c = not c
        return c
    return inside


def disc(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def diamond(cx, cy, a, b):
    return lambda x, y: abs(x - cx) / a + abs(y - cy) / b <= 1


def make_stars(folder, plain, cy):
    """Rating view, drawn over plain: the backdrop without the trough"""
    h, w = len(plain), len(plain[0])
    star_col, dot_col = (74, 74, 74), (140, 140, 140)
    pitch, r_out = 26, 8.5
    first = w / 2 - 2 * pitch
    frames = []
    for n in range(6):
        shapes = []
        for i in range(5):
            cx = first + i * pitch
            if i < n:
                shapes.append((star(cx, cy, r_out, r_out * 0.4), star_col))
            else:
                shapes.append((disc(cx, cy, 1.8), dot_col))
        frame = [row[:] for row in plain]
        for y in range(h):
            for x in range(w):
                for inside, col in shapes:
                    a = coverage(inside, x, y)
                    if a:
                        frame[y][x] = blend(frame[y][x], col, a)
        frames += frame
    write_bmp(os.path.join(folder, 'Stars.bmp'), frames)


def make_knob(folder, h, cy, a, b):
    """Scrubber slider: a diamond of half sizes a, b centred at cy"""
    w = round(2 * a + 2)
    cx = w / 2
    outer = diamond(cx, cy, a, b)
    inner = diamond(cx, cy, a - 1.3, b - 1.7)
    px, alpha = [], []
    for y in range(h):
        prow, arow = [], []
        for x in range(w):
            ao, ai = coverage(outer, x, y), coverage(inner, x, y)
            col = blend((90, 90, 90), (250, 250, 250), ai / ao) if ao \
                else (0, 0, 0)
            prow.append(col)
            arow.append(round(ao * 255))
        px.append(prow)
        alpha.append(arow)
    write_bmp(os.path.join(folder, 'Scrub Knob.bmp'), px, alpha)


def make_mask(folder, w, h):
    """Album art view: plain white to hide the parts of the backdrop next
    to the art. Transparent pixels would show the backdrop, not the art,
    so this must not overlap it."""
    write_bmp(os.path.join(folder, 'Art Mask.bmp'),
              [[(255, 255, 255)] * w for _ in range(h)])


def solve(a, b):
    """Gaussian elimination for the small normal equations below"""
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(m[r][c]))
        m[c], m[p] = m[p], m[c]
        for r in range(n):
            if r != c:
                f = m[r][c] / m[c][c]
                for k in range(c, n + 1):
                    m[r][k] -= f * m[c][k]
    return [m[i][n] / m[i][i] for i in range(n)]


def fit_surface(px, area, skip):
    """Least squares cubic surface of each colour over area (x0, y0, x1, y1),
    leaving out skip; returns a function giving the colour at x, y"""
    x0, y0, x1, y1 = area
    cx, cy, sx, sy = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2

    def terms(x, y):
        x, y = (x - cx) / sx, (y - cy) / sy
        return [1, x, y, x*x, x*y, y*y, x*x*x, x*x*y, x*y*y, y*y*y]

    samples = [(x, y) for y in range(y0, y1 + 1, 2) for x in range(x0, x1 + 1, 2)
               if not (skip[0] <= x <= skip[2] and skip[1] <= y <= skip[3])]
    coef = []
    for c in range(3):
        a = [[0.0] * 10 for _ in range(10)]
        b = [0.0] * 10
        for x, y in samples:
            t = terms(x, y)
            for i in range(10):
                b[i] += t[i] * px[y][x][c]
                for j in range(10):
                    a[i][j] += t[i] * t[j]
        coef.append(solve(a, b))
    return lambda x, y: [sum(k * t for k, t in zip(coef[c], terms(x, y)))
                         for c in range(3)]


def centre_note():
    """The menus' right panel (x 160..319) has a music note placeholder, high
    up for the text under it. Move it down a little, so that it and the line
    under it (the song count, or the playing track's title) are in the
    middle: the panel's gradient is fitted without the note, and the note
    with its shadow is moved as its difference from that gradient."""
    px = read_bmp(os.path.join(ICLASSIC, '..', '..', '..', 'originals',
                               'iClassic_bd.bmp'))
    note = (190, 30, 288, 142)        # the note and its shadow
    # its light part is 197..274 x 36..128; this puts it at 51..143, with
    # room above the text line at y 158
    dx, dy = 1, 15
    bg = fit_surface(px, (176, 0, 319, 239), note)

    def inside(x, y):
        return note[0] <= x <= note[2] and note[1] <= y <= note[3]

    out = [row[:] for row in px]
    for y in range(note[1], note[3] + dy + 1):
        for x in range(note[0], note[2] + dx + 1):
            col = bg(x, y)
            if inside(x - dx, y - dy):
                src, sbg = px[y - dy][x - dx], bg(x - dx, y - dy)
                col = [col[c] + src[c] - sbg[c] for c in range(3)]
            out[y][x] = tuple(max(0, min(255, round(v))) for v in col)
    write_bmp(os.path.join(ICLASSIC, '..', '..', 'backdrops',
                           'iClassic_bd.bmp'), out)


def iclassic():
    # trough at x 58..261, y 206..229; the album art reflection fades down
    # its left part (same in every column, sampled at x=30), white elsewhere
    bg = read_bmp(os.path.join(ICLASSIC, 'iClassic_bg.bmp'))
    plain = [[bg[206 + y][30] if 58 + x <= 147 else (255, 255, 255)
              for x in range(204)] for y in range(24)]
    make_stars(ICLASSIC, plain, 11.5)
    # the blue part of the 23px bar is its top 14 rows
    make_knob(ICLASSIC, 23, 7, 5.5, 7)
    # the art goes at 60,29 (200x200, Finder's size); left of it the
    # backdrop has the placeholder art and its reflection, and the ends of
    # the progress bar trough stick out on both sides: the WPS draws this
    # mask at 0 and at 260
    make_mask(ICLASSIC, 60, 221)
    centre_note()


if __name__ == '__main__':
    iclassic()
