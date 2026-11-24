#!/usr/bin/env python3
"""iClassic Square's status line as a stock 5G's on the iPod Video: 24 px, its
gradient over rows 0 to 22 and its dark line on row 23; the lists from row 24
as before, so the 5 px of white between them is gone, and Now Playing's and
the radio's the same.

    python3 themes/tools/make_classic_header.py

after make_menu_icons.py, then make_classic_220x176.py and make_dark_theme.py.
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
them (the iPod photo's status line, the moon of Night Mode, these glyphs).

The glyphs of the skins' status line, which have them in the middle of the
taller bar: the battery, the play glyph and the lock, transparent where a
pixel is within NEAR of one of the colours of the original bar's row it was
cut from (GLYPHS), so that any row of the bar shows through; their own light
parts, as the charging battery's body, stay. The iPod photo's status line is
its own (make_classic_220x176.py).
"""
import os
import struct

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
# each with its frames, and the row of the bar its first row was cut from: the
# battery's where the original skins drew it (its viewport at 4), the play
# glyph's and the lock's a row above where they drew them (viewports at 3, the
# lock 1 lower, %xl(h,...,0,1)), as the pictures' bar matches
GLYPHS = (('Battery', 'Status Battery', 24, 4), ('Playing Status', 'Status Playing', 4, 2),
          ('Hold Icon', 'Status Hold', 1, 3))
TRANSPARENT = (255, 0, 255)
NEAR = 4                # a pixel this close to one of its row's is the bar


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


def transparent_glyph(name, out, frames, top, bar):
    """bar: the colours of each row of the original one"""
    data, w, h, starts = read(os.path.join(WPS, name + '.bmp'))
    for y in range(h):
        row = bar[top + y % (h // frames)]
        for x in range(w):
            rgb = pixel(data, starts, x, y)
            if min(max(abs(a - b) for a, b in zip(rgb, c)) for c in row) <= NEAR:
                put(data, starts, x, y, TRANSPARENT)
    open(os.path.join(WPS, out + '.bmp'), 'wb').write(data)


def main():
    bar = [set(row) for row in original_header(REFERENCE)[:OLD_LINE]]
    for path, end in BACKDROPS:
        taller_bar(path, end)
    for name, out, frames, top in GLYPHS:
        transparent_glyph(name, out, frames, top, bar)


if __name__ == '__main__':
    main()
