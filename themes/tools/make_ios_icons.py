#!/usr/bin/env python3
"""Make the iOS look of iClassic Square: its status icons, rating and progress bar,
from the icons Linus (u/Linusssk) drew in the style of iOS 13 for iPodWizard, used
with his permission. Each is made to the place of the theme's own, so the theme
shows one or the other by the settings under Appearance:

    themes/tools/make_ios_icons.py <folder with his picNNNNN.bmp>

His drawings are kept where they fit: the battery loses a straight row, and the
rest are made the size of the theme's own, in their places. The play symbols,
the stars and the bar are drawn here in his colours, as simple shapes they are.
The icons of the status bar but the battery are of one colour with an alpha
channel, so the firmware gives them the cover's colour with their edges smooth
over whatever the bar is. The battery is made here for the dark theme too; run
themes/tools/make_dark_theme.py after it for the dark theme's others.
"""
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WPS = os.path.join(HERE, '..', 'iClassic-Square', '.rockbox', 'wps', 'iClassic-Square')
WPS_DARK = WPS + '-Dark'

# the dark theme's status bar and ink, as make_dark_theme.py makes them
_spec = importlib.util.spec_from_file_location('make_dark_theme', os.path.join(HERE, 'make_dark_theme.py'))
dark_theme = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dark_theme)

ACCENT = (234, 68, 90)      # his red, of iOS 13's Music
RED = (255, 59, 48)         # iOS's own, of a battery nearly empty, which stays red
TRACK = (220, 220, 220)     # the empty part of his bar, and the dot of no star
BLACK = (0, 0, 0)
WHITE = (255, 255, 255)


def read_bmp(path):
    d = open(path, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bpp = struct.unpack_from('<H', d, 28)[0]
    if bpp != 24:
        raise SystemExit(path + ': not a 24-bit BMP')
    row = (w * 3 + 3) & ~3
    flip = h > 0
    h = abs(h)
    out = []
    for y in range(h):
        base = off + (h - 1 - y if flip else y) * row
        out.append([(d[base + x * 3 + 2], d[base + x * 3 + 1], d[base + x * 3]) for x in range(w)])
    return out


def write_bmp(path, rows):
    """24 bits, or 32 with alpha if its pixels have four parts"""
    h = len(rows)
    w = len(rows[0])
    bpp = 8 * len(rows[0][0])
    stride = (w * bpp // 8 + 3) & ~3
    with open(path, 'wb') as f:
        f.write(b'BM' + struct.pack('<IHHI', 54 + stride * h, 0, 0, 54))
        f.write(struct.pack('<IiiHHIIiiII', 40, w, h, 1, bpp, 0, stride * h, 2835, 2835, 0, 0))
        for y in range(h - 1, -1, -1):
            line = bytes(c for p in rows[y] for c in (p[2], p[1], p[0]) + tuple(p[3:]))
            f.write(line + b'\0' * (stride - len(line)))


def his(num):
    return read_bmp(os.path.join(SOURCE, 'pic%d.bmp' % num))


def ink_box(px):
    pts = [(x, y) for y, row in enumerate(px) for x, p in enumerate(row) if min(p) < 236]
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1


def crop(px, box):
    x0, y0, x1, y1 = box
    return [row[x0:x1] for row in px[y0:y1]]


def mix(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def coverage(px):
    """each pixel as how much of it is ink, and the ink's colour, over white"""
    out = []
    for row in px:
        r = []
        for p in row:
            a = 1 - min(p) / 255.0
            ink = tuple(min(255, max(0, round(255 - (255 - c) / a))) for c in p) if a > 0.01 else WHITE
            r.append((a, ink))
        out.append(r)
    return out


def resample(cov, w, h):
    """to w x h, each pixel the average of what it covers, 4 x 4 samples"""
    sh, sw = len(cov), len(cov[0])
    out = []
    for oy in range(h):
        row = []
        for ox in range(w):
            a = 0.0
            c = [0.0, 0.0, 0.0]
            for sy in range(4):
                for sx in range(4):
                    x = min(sw - 1, int((ox + (sx + 0.5) / 4) * sw / w))
                    y = min(sh - 1, int((oy + (sy + 0.5) / 4) * sh / h))
                    pa, pc = cov[y][x]
                    a += pa
                    for i in range(3):
                        c[i] += pc[i] * pa
            row.append((a / 16, tuple(c[i] / a for i in range(3)) if a else WHITE))
        out.append(row)
    return out


def over(cov, bg):
    return [[mix(bg, c, a) for a, c in row] for row in cov]


def shape(w, h, inside, colour):
    """a shape drawn by a test of points, 4 x 4 samples a pixel, in colour"""
    out = []
    for y in range(h):
        row = []
        for x in range(w):
            n = sum(inside(x + (sx + 0.5) / 4, y + (sy + 0.5) / 4)
                    for sy in range(4) for sx in range(4))
            row.append((n / 16, colour))
        out.append(row)
    return out


def triangle(ax, ay, bx, by, cx, cy):
    def side(px, py, qx, qy, rx, ry):
        return (qx - px) * (ry - py) - (qy - py) * (rx - px)

    def inside(x, y):
        d1 = side(ax, ay, bx, by, x, y)
        d2 = side(bx, by, cx, cy, x, y)
        d3 = side(cx, cy, ax, ay, x, y)
        return not ((d1 < 0 or d2 < 0 or d3 < 0) and (d1 > 0 or d2 > 0 or d3 > 0))
    return inside


def union(*tests):
    return lambda x, y: any(t(x, y) for t in tests)


def stack(frames):
    return [row for f in frames for row in f]


def alpha(cov, ink):
    """in one colour, with how much of each pixel it covers as its alpha"""
    return [[ink + (round(a * 255),) for a, c in row] for row in cov]


# The battery over the theme's status bar, as the theme's own is: opaque, its
# smoothed edges into the bar's gradient at the rows it's drawn in, as the backdrop
# has a battery of its own under it
def header():
    bg = read_bmp(os.path.join(WPS, 'iClassic_bg.bmp'))
    return [bg[y][200] for y in range(dark_theme.HEADER_HEIGHT + 1)]


def on_bar(cov, top, bar):
    return [[mix(bar[top + y], c, a) for a, c in row] for y, row in enumerate(cov)]


def placed(cov, w, h, left, top):
    """cov in a w x h frame, at (left, top), nothing round it"""
    out = [[(0.0, WHITE)] * w for _ in range(h)]
    for y, row in enumerate(cov):
        for x, p in enumerate(row):
            if 0 <= top + y < h and 0 <= left + x < w:
                out[top + y][left + x] = p
    return out


def his_cov(num):
    px = his(num)
    return coverage(crop(px, ink_box(px)))


# the battery: 25 x 11, as the theme's, from his 25 x 12 with a straight row less.
# His outline is 13% of black, which is lost on the grey of the status bar, so it's
# made 60% on the light one, and on the dark one the 35% of white of iOS's own.
OUTLINE_LIGHT = 0.6 / 0.133
OUTLINE_DARK = 0.35 / 0.133


def battery_cov(num, outline):
    px = crop(his(num), (1, 0, 26, 12))
    cov = coverage(px[:6] + px[7:])
    for y, row in enumerate(cov):
        for x, (a, c) in enumerate(row):
            if not (2 <= x <= 20 and 2 <= y <= 8):
                row[x] = (min(1.0, a * outline), c)
    return cov


BATTERY_TOP = 4     # its row in the status bar


def battery(dark=False):
    outline = OUTLINE_DARK if dark else OUTLINE_LIGHT

    def battery_cov_(num):
        cov = battery_cov(num, outline)
        if dark:
            # his black, the outline's and the bolt's, light on dark
            cov = [[(a, dark_theme.INK if dark_theme.luma(c) < 60 else c) for a, c in row] for row in cov]
        return cov
    empty = battery_cov_(24316)
    # his low battery's sliver of red out of it: inside, the bar shows
    for y in range(2, 9):
        for x in range(2, 21):
            empty[y][x] = (0.0, WHITE)
    frames = [battery_cov_(24315), battery_cov_(24315), battery_cov_(24310)]
    for level in range(21):
        f = [row[:] for row in empty]
        width = round(19 * level / 20)
        colour = RED if level <= 4 else dark_theme.INK if dark else BLACK
        for y in range(2, 9):
            for x in range(2, 2 + width):
                f[y][x] = (1.0, colour)
        if width:
            # the corners of the fill rounded as his are
            for y, x in ((2, 2), (8, 2), (2, 1 + width), (8, 1 + width)):
                if width == 19 or x == 2:
                    f[y][x] = (0.5, colour)
        frames.append(f)
    bar = [dark_theme.header_tone(p) for p in header()] if dark else header()
    return stack([on_bar(f, BATTERY_TOP, bar) for f in frames])


def playing_status():
    w, h = 16, 13
    play = shape(w, h, triangle(3, 0, 3, 13, 14.5, 6.5), ACCENT)
    pause = placed(resample(his_cov(25552), 11, 11), w, h, 2, 1)
    ff = shape(w, h, union(triangle(0.5, 1.5, 0.5, 11.5, 8, 6.5),
                           triangle(8, 1.5, 8, 11.5, 15.5, 6.5)), ACCENT)
    rew = shape(w, h, union(triangle(15.5, 1.5, 15.5, 11.5, 8, 6.5),
                            triangle(8, 1.5, 8, 11.5, 0.5, 6.5)), ACCENT)
    return stack([alpha(f, ACCENT) for f in (play, pause, ff, rew)])


# repeat and shuffle in 16 x 16, as the theme's are: drawn 14 x 12 two rows down,
# so that they keep clear of the status bar's foot
def small(num):
    return alpha(placed(resample(his_cov(num), 14, 12), 16, 16, 1, 2), ACCENT)


def repeat():
    all_, one = small(24265), small(24266)
    return stack([all_, one, all_, one])


def shuffle():
    return small(24267)


def hold():
    cov = placed(resample(his_cov(24285), 7, 11), 9, 11, 1, 0)
    # his grey, as much of it as there is
    total = sum(a for row in cov for a, c in row)
    ink = tuple(round(sum(a * c[i] for row in cov for a, c in row) / total) for i in range(3))
    return alpha(cov, ink)


def star_cov(size):
    return resample(his_cov(24279), size, size)


def dot_cov(size):
    return resample(his_cov(24280), size, size)


def paste(canvas, cov, left, top, left_of=None, colour=None):
    """cov over canvas at (left, top), only its columns left of left_of if given"""
    for y, row in enumerate(cov):
        for x, (a, c) in enumerate(row):
            if left_of is not None and x >= left_of:
                continue
            bg = canvas[top + y][left + x]
            canvas[top + y][left + x] = mix(bg, colour or c, a)


def ratings():
    """Now Playing's: 64 x 13 for each half star, red stars on white, the
    half's other half in the grey of no star"""
    star, frames = star_cov(12), []
    for rating in range(1, 11):
        f = [[WHITE] * 64 for _ in range(13)]
        for i in range(5):
            stars = rating / 2 - i
            if stars >= 1:
                paste(f, star, i * 13, 0)
            elif stars > 0:
                paste(f, star, i * 13, 0, colour=TRACK)
                paste(f, star, i * 13, 0, left_of=6)
        frames.append(f)
    return stack(frames)


def stars():
    """the large art's: the theme's frames, its stars and dots in his"""
    ours = read_bmp(os.path.join(WPS, 'Stars.bmp'))
    w, fh = len(ours[0]), len(ours) // 6
    full = ours[5 * fh:6 * fh]
    # where the stars are: the columns of ink in the frame with five
    cols = [x for x in range(w) if any(min(full[y][x]) < 128 for y in range(fh))]
    groups, start = [], cols[0]
    for a, b in zip(cols, cols[1:] + [None]):
        if b is None or b != a + 1:
            groups.append((start, a))
            if b is not None:
                start = b
    centres = [(a + b) / 2 for a, b in groups]
    size = max(b - a + 1 for a, b in groups)
    star, dot = star_cov(size), dot_cov(5)
    frames = []
    for count in range(6):
        f = [row[:] for row in ours[count * fh:(count + 1) * fh]]
        # the theme's star or dot out, with the background left of its place
        for cx in centres:
            x0 = int(cx - size / 2) - 1
            if x0 < 1:
                raise SystemExit('Stars.bmp: a star at its left edge')
            for y in range(fh):
                for x in range(x0, x0 + size + 3):
                    f[y][x] = f[y][x0 - 1]
        top = (fh - size) // 2
        for i, cx in enumerate(centres):
            if i < count:
                paste(f, star, int(cx - size / 2), top)
            else:
                paste(f, dot, int(cx - 2.5), (fh - 5) // 2)
        frames.append(f)
    return stack(frames)


def bar(colour):
    """204 x 23 as the theme's: a round-ended bar of 6 rows in the middle of the
    14 its own has, white all round, as there's no reflection"""
    w, h, top, thick = 204, 23, 4, 6
    r = thick / 2

    def inside(x, y):
        if not top <= y < top + thick:
            return False
        cy = top + r
        if x < r:
            return (x - r) ** 2 + (y - cy) ** 2 <= r * r
        if x > w - r:
            return (x - (w - r)) ** 2 + (y - cy) ** 2 <= r * r
        return True
    return over(shape(w, h, inside, colour), WHITE)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    SOURCE = sys.argv[1]
    out = {
        'iOS Battery.bmp': battery(),
        'iOS Playing Status.bmp': playing_status(),
        'iOS Repeat.bmp': repeat(),
        'iOS Shuffle.bmp': shuffle(),
        'iOS Hold.bmp': hold(),
        'iOS Ratings.bmp': ratings(),
        'iOS Stars.bmp': stars(),
        'iOS Track.bmp': bar(TRACK),
        'iOS PB.bmp': bar(ACCENT),
    }
    for name, rows in out.items():
        write_bmp(os.path.join(WPS, name), rows)
        print('%-24s %d x %d' % (name, len(rows[0]), len(rows)))
    write_bmp(os.path.join(WPS_DARK, 'iOS Battery.bmp'), battery(dark=True))
