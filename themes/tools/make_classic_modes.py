#!/usr/bin/env python3
# Draws iClassic Square's Now Playing modes for the iPod photo's 220x176
# screen, 16x17 a frame: Shuffle Icon.bmp, Repeat Icon.bmp's four frames and
# Status Shuffle Albums.bmp. They are the Video's 19x20 ones at 85 %, drawn
# anew, as resampling blurs them: dark strokes 2 px wide with a light shade
# one and two rows under them, straight edges on whole pixels, curves and
# slants smoothed. Each is drawn 16 times as large and averaged down, its
# colours mixed in linear light. Plain Python, no imaging libraries.
#
# make_classic_220x176.py writes the light theme's with this, and
# make_dark_theme.py the dark one's in DARK_COLOURS: its mix of a light
# picture by luma (glyph_pixel()) would thin these smoothed edges on the dark.

import math
import os

from make_menu_icons import write_bmp

W, H, K = 16, 17, 16
STROKE = 2.0
HEAD = 6.0                      # an arrowhead's height; its length is half that
SHUFFLE_BANDS = (3, 10)         # the shuffle's bands' top rows
LOOP = (3, 12)                  # the repeat loop's outer edges, top and bottom
CORNER = 3.5                    # its corners' radius, at the stroke's middle
# repeat one's and A-B's badges: a disc of 7 px with a letter of 3 by 5 in the
# ground's colour on whole pixels, the size of the Video's, as a smaller letter
# would not read
BADGE = 7
LETTERS = {
    "1": (".#.", "##.", ".#.", ".#.", "###"),
    "A": (".#.", "#.#", "###", "#.#", "#.#"),
    "B": ("##.", "#.#", "##.", "#.#", "##."),
}
# Shuffle Albums': two albums of 6 px, framed by a pixel, at the top right and
# the bottom left, the one the other turned half round about (W / 2, this)
ALBUM = 6
ALBUMS_MIDDLE = 8

# the ink, the shade under it, the fainter shade a row lower, and the ground
LIGHT_COLOURS = ((59, 59, 59), (226, 226, 226), (241, 241, 241), (255, 255, 255))
# the ink make_dark_theme.py's glyph_pixel() of the light one's, the ground its
# BG, the shade a little lighter than that
DARK_COLOURS = ((186, 186, 190), (28, 28, 31), (19, 19, 21), (18, 18, 20))


def disc_rows(a, b):
    """the rows of a filled ellipse whose box's corners are a and b pixels
    apart, as (row, first, last): a midpoint walk round a quarter of it on
    doubled coordinates, each row as wide as its first point there"""
    a2, b2 = a * a, b * b

    def off(x, y):
        return abs(a2 * y * y + b2 * x * x - a2 * b2)
    x, y = a, b % 2
    first = {}
    while True:
        first.setdefault(y, x)
        if (x, y) == (a % 2, b):
            break
        nx, ny = x, y + 2
        best = off(nx, ny)
        if x > 1:
            for cx, cy in ((x - 2, y + 2), (x - 2, y)):
                d = off(cx, cy)
                if best > d:
                    nx, ny, best = cx, cy, d
        x, y = nx, ny
    return [((sy + b) // 2, (a - r) // 2, (a + r) // 2)
            for y, r in first.items() for sy in {y, -y}]


class Canvas:
    """a shape, K times as large as the picture"""

    def __init__(self):
        self.m = [bytearray(W * K) for _ in range(H * K)]
        self.discs = {}

    def span(self, y, x0, x1):
        if 0 <= y < H * K:
            x0, x1 = max(0, x0), min(W * K - 1, x1)
            if x0 <= x1:
                self.m[y][x0:x1 + 1] = b"\xff" * (x1 - x0 + 1)

    def rect(self, x0, y0, x1, y1):
        """whole pixels, from (x0, y0) to before (x1, y1)"""
        for y in range(y0 * K, y1 * K):
            self.span(y, x0 * K, x1 * K - 1)

    def poly(self, pts):
        """a convex polygon, with every point on its edges"""
        pts = [(x * K, y * K) for x, y in pts]
        for y in range(math.ceil(min(p[1] for p in pts)),
                       math.floor(max(p[1] for p in pts)) + 1):
            xs = []
            for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]):
                if min(y0, y1) <= y <= max(y0, y1):
                    xs += [x0, x1] if y0 == y1 else [x0 + (x1 - x0) * (y - y0) / (y1 - y0)]
            self.span(y, math.ceil(min(xs)), math.floor(max(xs)))

    def disc(self, cx, cy, r):
        # its box from its pixels' edges, each cut to a whole one toward 0
        x0, y0 = int((cx - r) * K), int((cy - r) * K)
        a, b = int((cx + r) * K - 1) - x0, int((cy + r) * K - 1) - y0
        if (a, b) not in self.discs:
            self.discs[a, b] = disc_rows(a, b)
        for y, first, last in self.discs[a, b]:
            self.span(y0 + y, x0 + first, x0 + last)

    def path(self, pts, w):
        """a line w wide through the points: a round pen every half a pixel of
        the large drawing"""
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            n = max(1, int(math.hypot(x1 - x0, y1 - y0) * K / 2))
            for i in range(n + 1):
                t = i / n
                self.disc(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, w / 2)

    def cover(self):
        """how much of each pixel the shape covers, 0 to 1: averaged along the
        rows then down the columns, each to a whole 255th, rounded"""
        rows = [[(sum(row[x * K:x * K + K]) + K // 2) // K for x in range(W)] for row in self.m]
        return [[(sum(rows[y * K + j][x] for j in range(K)) + K // 2) // K / 255
                 for x in range(W)] for y in range(H)]


def lin(c):
    c = c / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def srgb(v):
    v = min(1.0, max(0.0, v))
    v = v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055
    return min(255, max(0, round(v * 255)))


def compose(ink, colours, shape=None, letters=None):
    """the picture of the ink's coverage over its shade: the shape's (the
    ink's, or with its letters filled) one and two rows lower, the second
    fainter; letters in the ground's colour over that"""
    shape = shape or ink
    li, l1, l2, lb = ([lin(v) for v in c] for c in colours)
    out = []
    for y in range(H):
        row = []
        for x in range(W):
            s1 = shape[y - 1][x] if y >= 1 else 0.0
            s2 = shape[y - 2][x] if y >= 2 else 0.0
            px = []
            for n in range(3):
                a = lb[n] + (l2[n] - lb[n]) * s2
                a = a + (l1[n] - a) * s1
                if letters and letters[y][x]:
                    a = a + (lb[n] - a) * letters[y][x]
                a = a + (li[n] - a) * ink[y][x]
                px.append(srgb(a))
            row.append(tuple(px))
        out.append(row)
    return out


def bezier(p0, p1, p2, p3, n=48):
    return [tuple((1 - t) ** 3 * a + 3 * (1 - t) ** 2 * t * b + 3 * (1 - t) * t ** 2 * c
                  + t ** 3 * d for a, b, c, d in zip(p0, p1, p2, p3))
            for t in (i / n for i in range(n + 1))]


def arc(cx, cy, r, a0, a1, n=24):
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
             cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]


def arrow(c, xb, yc, xt):
    """a head: its back on x = xb, HEAD high about yc, its tip at (xt, yc)"""
    c.poly([(xb, yc - HEAD / 2), (xt, yc), (xb, yc + HEAD / 2)])


def shuffle():
    s = STROKE
    t, b = (y + s / 2 for y in SHUFFLE_BANDS)   # the bands' middles
    under, over = Canvas(), Canvas()
    # from the top left down to the bottom right's head, under the other
    under.path([(-1, t), (3, t)] + bezier((3, t), (7, t), (7, b), (11, b)) + [(12.5, b)], s)
    # from the bottom left up to the top right's head, over it, with a gap
    up = [(-1, b), (3, b)] + bezier((3, b), (7, b), (7, t), (11, t)) + [(12.5, t)]
    over.path(up, s)
    gap = Canvas()
    gap.path(up[3:-2], s + 2)
    for c, yc in ((over, t), (under, b)):
        arrow(c, 12, yc, 12 + HEAD / 2)
    cu, co, cg = under.cover(), over.cover(), gap.cover()
    return [[min(1.0, max(u * (1 - g), o)) for u, o, g in zip(*rows)]
            for rows in zip(cu, co, cg)]


def loop_half(curl=True, end=10):
    """the loop's upper left half, the stroke's middle: from its corner at the
    bottom left (from a little round it with curl, as the Video's ends curl
    in) up and along the top to x = end"""
    s = STROKE
    x_l, y_t, y_b = s / 2, LOOP[0] + s / 2, LOOP[1] - s / 2
    return (arc(x_l + CORNER, y_b - CORNER, CORNER, 150, 180) if curl else []) \
        + arc(x_l + CORNER, y_t + CORNER, CORNER, 180, 270) + [(end, y_t)]


def turned(pts):
    """the points turned half round about the loop's middle"""
    return [(W - x, LOOP[0] + LOOP[1] - y) for x, y in pts]


def badge(c, letters, x, y, ch):
    """a badge, its box's top left at (x, y): its disc on c, its letter's
    pixels in letters"""
    c.disc(x + BADGE / 2, y + BADGE / 2, BADGE / 2)
    for j, line in enumerate(LETTERS[ch]):
        for i, p in enumerate(line):
            if p == "#":
                letters[y + 1 + j][x + 2 + i] = 1.0


def repeat(frame):
    """a frame of the repeat icon, for compose(): repeat all (0), one (1), the
    first mirrored (2, shown in turn with it for repeat-shuffle), A-B (3). A
    badge stands in a head's place, half a pixel further out; the side of the
    loop it is on goes without its curl, and its other side ends a pixel from
    it"""
    if frame == 2:
        return [row[::-1] for row in repeat(0)[0]], None, None
    s = STROKE
    y_t = LOOP[0] + s / 2
    c = Canvas()
    letters = [[0.0] * W for _ in range(H)]
    near = W - BADGE - 2
    low, high = (0, LOOP[1] - 4), (W - BADGE, LOOP[0] - 3)
    if frame == 0:
        c.path(loop_half(), s)
        c.path(turned(loop_half()), s)
        arrow(c, 10, y_t, 10 + HEAD / 2)
        arrow(c, W - 10, LOOP[0] + LOOP[1] - y_t, W - 10 - HEAD / 2)
    elif frame == 1:
        c.path(loop_half(curl=False), s)
        arrow(c, 10, y_t, 10 + HEAD / 2)
        c.path(turned(loop_half(end=near)), s)
        badge(c, letters, *low, "1")
    else:
        c.path(loop_half(curl=False, end=near), s)
        c.path(turned(loop_half(curl=False, end=near)), s)
        badge(c, letters, *low, "A")
        badge(c, letters, *high, "B")
    shape = c.cover()
    ink = [[v * (1 - l) for v, l in zip(rv, rl)] for rv, rl in zip(shape, letters)]
    return ink, shape, letters


def albums():
    """shuffle by albums: the albums, and from each an arrow to the other's
    place: up from two rows over the lower one, round a corner and along to a
    head at the upper one's middle, a pixel from it; the other turned"""
    n, mid = ALBUM, ALBUMS_MIDDLE
    c = Canvas()
    for x, y in ((W - n, 1), (0, 2 * mid - 1 - n)):
        c.rect(x, y, x + n, y + 1)
        c.rect(x, y + n - 1, x + n, y + n)
        c.rect(x, y, x + 1, y + n)
        c.rect(x + n - 1, y, x + n, y + n)
    x, y, r = n // 2, 1 + n // 2, 2
    tip = W - n - 1
    up = [(x, 2 * mid - 1 - n - 2)] + arc(x + r, y + r, r, 180, 270) + [(tip - HEAD / 2, y)]
    c.path(up, STROKE)
    c.path([(W - px, 2 * mid - py) for px, py in up], STROKE)
    arrow(c, tip - HEAD / 2, y, tip)
    arrow(c, W - tip + HEAD / 2, 2 * mid - y, W - tip)
    return c.cover()


def write(folder, colours):
    """the three pictures into a theme's folder, in its colours"""
    write_bmp(os.path.join(folder, "Shuffle Icon.bmp"), compose(shuffle(), colours))
    rows = []
    for frame in range(4):
        ink, shape, letters = repeat(frame)
        rows += compose(ink, colours, shape, letters)
    write_bmp(os.path.join(folder, "Repeat Icon.bmp"), rows)
    write_bmp(os.path.join(folder, "Status Shuffle Albums.bmp"), compose(albums(), colours))
