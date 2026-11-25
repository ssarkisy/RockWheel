#!/usr/bin/env python3
"""iClassic Square's status line as a stock 5G's on the iPod Video: 24 px, its
gradient over rows 0 to 22 and its dark line on row 23; the lists from row 24
as before, so the 5 px of white between them is gone, and Now Playing's and
the radio's the same.

    python3 themes/tools/make_classic_header.py

after make_menu_icons.py, then make_night_icon.py, make_classic_220x176.py and
make_dark_theme.py.
Plain Python, no imaging libraries.

In the light backdrops (the lists', the About Backdrop, the Wide Backdrop and
Now Playing's, which the radio's skin has too) the bar's 17 rows of gradient
are stretched to 22, a row of them taken again at five even places, as the
bar's texture is in its rows; under them its lighter last row, then its dark
line, in place of the white. The panel beside the main menu goes up to the
screen's top and stays as it is. A backdrop already done is left alone, and
one whose status line is not the original theme's (a dark line under 18
rows, white under it) stops the script. The original rows are all still in
the taller bar: original_header() gives them back, for what is drawn from
them (the iPod photo's status line, these glyphs).

The glyphs of the skins' status line, the battery, the play glyph and the
lock, on either screen and in either look, light and dark: each pixel's ink
and how much of it the pixel covers (layers()), on the rows of that screen's
bar where its skins draw it (STATUS), transparent where it covers nothing, so
that the bar shows through. They were cut from the original bar: a pixel
within NEAR of one of the colours of its row there is the bar, and one at the
glyph's edge beside the bar round it (edge_layers()) as much ink as it is of
the blend of the bar and the neighbour's colour that fits it best. Their own
light parts stay, as the charging battery's body, but for three of its pixels
(frame Bb) within NEAR of their row's colours, which show the bar there,
all but the same colour. The play glyph's triangles are drawn anew from their
shapes (PLAY_SHAPES) in the colours of the gloss inside them, so that their
edges are smooth as the moon's (make_night_icon.py, which puts it on the bars
the same way). The dark look's inks are make_dark_theme.py's header_pixel(),
on the dark look's bar: an edge carried the light bar's colour before, which
showed as a light fringe round the play glyph on the dark bar.
"""
import os
import struct

import make_dark_theme as dark_tool

HERE = os.path.dirname(os.path.abspath(__file__))
THEME = os.path.join(HERE, '..', 'iClassic-Square', '.rockbox')
WPS = os.path.join(THEME, 'wps', 'iClassic-Square')

W = 320
OLD_LINE = 18           # the dark line, under the gradient and its lighter row
NEW_LINE = 23
LIST_TOP = 24           # the lists' first row (the .sbs's %Vi), not moved
PANEL_X = 160           # the panel beside the main menu and Settings
# the backdrops, and where their status line ends
BACKDROPS = ((os.path.join(THEME, 'backdrops', 'iClassic_bd.bmp'), PANEL_X),
             (os.path.join(WPS, 'About Backdrop.bmp'), PANEL_X),
             (os.path.join(WPS, 'Wide Backdrop.bmp'), W),
             (os.path.join(WPS, 'iClassic_bg.bmp'), W))
# Now Playing's, whose original bar the glyphs stood on
REFERENCE = os.path.join(WPS, 'iClassic_bg.bmp')
# The status line's glyphs: the picture each is cut from, on the original bar,
# its frames, and the row of that bar its first row was cut from: the
# battery's where the original skins drew it (its viewport at 4), the play
# glyph's and the lock's a row above where they drew them (viewports at 3, the
# lock 1 lower, %xl(h,...,0,1)), as the pictures' bar matches. Then each
# screen's file of it and the row its skins draw it at: on the Video's bar of
# 24 rows the battery at 6, the play glyph at 5 and the lock a row lower; on
# the iPod photo's of 16 at 2, 1 and 2, the battery's and the play glyph's ink
# in the middle of its 15 rows over the dark line
STATUS = (('Battery', 24, 4, ('Status Battery', 6), ('Battery', 2)),
          ('Playing Status', 4, 2, ('Status Playing', 5), ('Playing Status', 1)),
          ('Hold Icon', 1, 3, ('Status Hold', 6), ('Hold Icon', 2)))
# the play glyph's frames, in its 16 by 13 cells, as convex polygons of
# (x, y): playing, a triangle 2 px wider each row down to its middle, its tip
# cut at x 12.5 as the picture's; paused, two bars on whole pixels; forward and
# back, two triangles each, the second over the first's tip. Fitted to the
# rows of the original's ink, whose edges blended over a row's two pixels
PLAY_SHAPES = (
    (((1, 0.5), (12.5, 6.25), (12.5, 6.75), (1, 12.5)),),
    (((1, 1), (5, 1), (5, 12), (1, 12)), ((8, 1), (12, 1), (12, 12), (8, 12))),
    (((0, 2.125), (8.75, 6.5), (0, 10.875)), ((8, 2.125), (16.75, 6.5), (8, 10.875))),
    (((16, 2.125), (7.25, 6.5), (16, 10.875)), ((8, 2.125), (-0.75, 6.5), (8, 10.875))),
)
SAMPLES = 16            # each way in a pixel, for a shape's coverage
TRANSPARENT = (255, 0, 255)
NEAR = 4                # a pixel this close to one of its row's is the bar
LEAST = 0.02            # a pixel covered less is transparent
BLEND = 8               # a pixel this close to a blend of the bar and ink is one
# each screen's light and dark folders, and the rows of its status line: the
# iPod photo's 15 of the original 18 of gradient, taken evenly from its first
# to its last (make_classic_220x176.py's bar), then its dark line
PHOTO_WPS = os.path.join(HERE, '..', 'iClassic-Square', '220x176', '.rockbox', 'wps')
FOLDERS = ((WPS, os.path.join(THEME, 'wps', 'iClassic-Square-Dark')),
           (os.path.join(PHOTO_WPS, 'iClassic-Square'),
            os.path.join(PHOTO_WPS, 'iClassic-Square-Dark')))
PHOTO_ROWS = 15


def read(path):
    """The file's bytes, and where each row's pixels start, from the top"""
    data = bytearray(open(path, 'rb').read())
    off, = struct.unpack_from('<I', data, 10)
    w, h, _, bpp = struct.unpack_from('<iiHH', data, 18)
    if bpp != 24 or h <= 0:
        raise SystemExit('%s: not a bottom-up BMP of 24 bits' % path)
    stride = (w * 3 + 3) & ~3
    return data, w, h, [off + (h - 1 - y) * stride for y in range(h)]


def pixel(data, starts, x, y):
    o = starts[y] + 3 * x
    return data[o + 2], data[o + 1], data[o]


def put(data, starts, x, y, rgb):
    o = starts[y] + 3 * x
    data[o:o + 3] = bytes((rgb[2], rgb[1], rgb[0]))


def dark_line(column, y):
    """the row is the bar's dark line: two thirds as light as the row over it"""
    return 3 * sum(column[y]) < 2 * sum(column[y - 1])


def taller(data, starts):
    """the picture's bar is the taller one already"""
    column = [pixel(data, starts, 0, y) for y in range(LIST_TOP)]
    return dark_line(column, NEW_LINE) and column[OLD_LINE + 1] != (255, 255, 255)


def stretched(y):
    """the original row a row of the taller bar's gradient is"""
    return round(y * (OLD_LINE - 2) / (NEW_LINE - 2))


def original_header(path):
    """the picture's status line as the original theme had it, its rows 0 to
    18 of (r, g, b), from the taller bar or not"""
    data, w, h, starts = read(path)
    rows = [[pixel(data, starts, x, y) for x in range(w)] for y in range(LIST_TOP)]
    if not taller(data, starts):
        return rows[:OLD_LINE + 1]
    first = [min(y for y in range(NEW_LINE - 1) if stretched(y) == k)
             for k in range(OLD_LINE - 1)]
    return [rows[y] for y in first] + [rows[NEW_LINE - 1], rows[NEW_LINE]]


def taller_bar(path, end):
    data, w, h, starts = read(path)
    if w != W:
        raise SystemExit('%s: %d px wide' % (path, w))
    if taller(data, starts):
        print('%s: done already' % os.path.basename(path))
        return
    for x in range(end):
        column = [pixel(data, starts, x, y) for y in range(LIST_TOP)]
        if not dark_line(column, OLD_LINE) \
                or any(rgb != (255, 255, 255) for rgb in column[OLD_LINE + 1:]):
            raise SystemExit('%s: no dark line on row %d with white under it at x %d'
                             % (path, OLD_LINE, x))
    for x in range(end):
        old = [pixel(data, starts, x, y) for y in range(OLD_LINE + 1)]
        # the gradient's rows, then its lighter one and its line
        new = [old[stretched(y)] for y in range(NEW_LINE - 1)]
        new += [old[OLD_LINE - 1], old[OLD_LINE]]
        for y, rgb in enumerate(new):
            put(data, starts, x, y, rgb)
    open(path, 'wb').write(data)
    print('%s: status line of %d rows' % (os.path.basename(path), LIST_TOP))


def photo_bar(header):
    """the iPod photo's status line of the original one's rows"""
    return [header[round(y * (OLD_LINE - 1) / (PHOTO_ROWS - 1))]
            for y in range(PHOTO_ROWS)] + [header[OLD_LINE]]


def mean(row):
    return tuple(sum(p[i] for p in row) / len(row) for i in range(3))


def bars():
    """each screen's bar, light and dark: the colour of each of its rows, the
    texture of its columns, a level or two, left out. The Video's is the
    taller one, made already"""
    data, w, h, starts = read(REFERENCE)
    if not taller(data, starts):
        raise SystemExit('%s: run make_classic_header.py first' % REFERENCE)
    video = [[pixel(data, starts, x, y) for x in range(w)] for y in range(LIST_TOP)]
    photo = photo_bar(original_header(REFERENCE))
    return [([mean(row) for row in rows],
             [mean([dark_tool.header_pixel(p) for p in row]) for row in rows])
            for rows in (video, photo)]


def inside(polygon, x, y):
    """the point is in the convex polygon, of either winding"""
    side = 0
    for (x0, y0), (x1, y1) in zip(polygon, polygon[1:] + polygon[:1]):
        cross = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0)
        if cross:
            if side and (cross > 0) != (side > 0):
                return False
            side = cross
    return True


def coverage(polygons, x, y):
    """how much of the pixel the polygons cover, by SAMPLES points each way"""
    points = [(x + (i + 0.5) / SAMPLES, y + (j + 0.5) / SAMPLES)
              for j in range(SAMPLES) for i in range(SAMPLES)]
    return sum(any(inside(p, *q) for p in polygons) for q in points) / len(points)


def distance(a, b):
    return sum((a[i] - b[i]) ** 2 for i in range(3))


def blend(p, ink, bg):
    """how much of ink over bg p is, and how far p is from that blend"""
    d = [ink[i] - bg[i] for i in range(3)]
    n = sum(v * v for v in d)
    if not n:
        return 0.0, float('inf')
    a = sum((p[i] - bg[i]) * d[i] for i in range(3)) / n
    return a, distance(p, [bg[i] + a * d[i] for i in range(3)]) ** 0.5


def edge_layers(rows, bar, bg):
    """A frame's pixels as (ink, coverage): the bar's none; one beside the bar
    round the glyph (not a hole in it, as the charging battery's) a blend of
    the bar and a neighbour's colour, if it is one within BLEND, as much of
    that as it is; the rest wholly their own"""
    h, w = len(rows), len(rows[0])
    outside = set()
    todo = [(x, y) for y in range(h) for x in range(w)
            if (x in (0, w - 1) or y in (0, h - 1)) and bar[y](rows[y][x])]
    while todo:
        x, y = todo.pop()
        if (x, y) in outside:
            continue
        outside.add((x, y))
        for u, v in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if 0 <= u < w and 0 <= v < h and (u, v) not in outside and bar[v](rows[v][u]):
                todo.append((u, v))
    out = []
    for y in range(h):
        line = []
        for x in range(w):
            p = rows[y][x]
            if bar[y](p):
                line.append((p, 0.0))
                continue
            line.append((p, 1.0))
            if not any((u, v) in outside for u, v in
                       ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1))):
                continue
            near = [rows[v][u] for v in range(max(0, y - 1), min(h, y + 2))
                    for u in range(max(0, x - 1), min(w, x + 2))
                    if (u, v) != (x, y) and not bar[v](rows[v][u])]
            fits = [(blend(p, c, bg[y]), c) for c in near]
            fits = [(off, a, c) for (a, off), c in fits if 0 < a < 0.98 and off <= BLEND]
            if fits:
                _, a, c = min(fits)
                line[-1] = (c, max(a, 0.0))
        out.append(line)
    return out


def shape_layers(rows, bar, polygons):
    """A frame drawn anew from its shape: each pixel as much ink as the shape
    covers of it, in the colour of the original's pixel there if the shape
    covers it whole, else of the nearest one it covers whole, in its row if
    it has one: the gloss as it was, to the edges"""
    h, w = len(rows), len(rows[0])
    cover = [[coverage(polygons, x, y) for x in range(w)] for y in range(h)]
    whole = [(x, y) for y in range(h) for x in range(w)
             if cover[y][x] > 0.999 and not bar[y](rows[y][x])]
    out = []
    for y in range(h):
        line = []
        for x in range(w):
            a = cover[y][x]
            u, v = min(whole, key=lambda q: (q[1] != y, abs(q[0] - x) + abs(q[1] - y)))
            line.append((rows[v][u], a))
        out.append(line)
    return out


def layers(name, frames, top):
    """the picture's frames, one under another, as rows of (ink, coverage)"""
    header = original_header(REFERENCE)
    colours = [set(row) for row in header[:OLD_LINE]]
    data, w, h, starts = read(os.path.join(WPS, name + '.bmp'))
    fh = h // frames
    out = []
    for f in range(frames):
        rows = [[pixel(data, starts, x, f * fh + y) for x in range(w)] for y in range(fh)]
        bar = [lambda p, c=colours[top + y]: min(max(abs(a - b) for a, b in zip(p, k))
                                                 for k in c) <= NEAR for y in range(fh)]
        if name == 'Playing Status':
            out += shape_layers(rows, bar, PLAY_SHAPES[f])
        else:
            out += edge_layers(rows, bar, [mean(header[top + y]) for y in range(fh)])
    return out


def put_on_bar(rows, frames, bar, top, ink=lambda p: p):
    """the glyph's rows of (ink, coverage) over the bar's rows from top: the
    ink as the look has it, and the magenta skins take as transparent where
    it covers nothing"""
    fh = len(rows) // frames
    out = []
    for y, row in enumerate(rows):
        line = []
        for p, a in row:
            if a < LEAST:
                line.append(TRANSPARENT)
                continue
            bg = bar[top + y % fh]
            line.append(tuple(dark_tool.clamp(a * c + (1 - a) * bg[i])
                              for i, c in enumerate(ink(p))))
        out.append(line)
    return out


def status_glyphs(rows, frames, places):
    """the glyph's rows of (ink, coverage) on each screen's bar, light and
    dark, at places: (file, row) of each screen"""
    for (light, dark), (lights, darks), (out, top) in zip(FOLDERS, bars(), places):
        dark_tool.write_bmp(os.path.join(light, out + '.bmp'),
                            put_on_bar(rows, frames, lights, top))
        dark_tool.write_bmp(os.path.join(dark, out + '.bmp'),
                            put_on_bar(rows, frames, darks, top, dark_tool.header_pixel))


def main():
    for path, end in BACKDROPS:
        taller_bar(path, end)
    for name, frames, top, video, photo in STATUS:
        status_glyphs(layers(name, frames, top), frames, (video, photo))


if __name__ == '__main__':
    main()
