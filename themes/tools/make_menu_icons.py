#!/usr/bin/env python3
"""Draw what iClassic Square shows on the right panel beside the main menu, all on the
gradient of the panel shown when Settings is selected, in the same material as what that
shows: light above a curved sheen and silver below it, with a light rim and a soft shadow.

  the music note, on the menus' panel and as the placeholder art of Now Playing's backdrop
  Photos Icon.bmp  for Photos when there are none: goes over the note on the menus' panel

That one is 16 bit and dithered like the backdrop around it, so it doesn't show as a
patch. Plain Python, no imaging libraries. Usage:

    python3 themes/tools/make_menu_icons.py

Then run make_dark_theme.py, which takes these from the light theme.

Whole Screen in Cover Colours indexes Now Playing's backdrop in 256 colours once it is
reduced to the screen's, and does nothing with more: the script prints the count and
stops if it's over.
"""
import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
THEME = os.path.join(HERE, '..', 'iClassic-Square', '.rockbox')
WPS = os.path.join(THEME, 'wps', 'iClassic-Square')

SS = 8  # samples per pixel on each axis

PANEL_X = 160
ART = (12, 45, 148, 181)  # the placeholder art in Now Playing's backdrop
# In the .sbs: the part of the panel the note takes up
ICON_AREA = (PANEL_X + 28, 48, PANEL_X + 132, 152)

LIGHT = ((250, 250, 249), (238, 239, 240))  # above the sheen, from top to bottom
SILVER = ((186, 193, 199), (228, 231, 234))  # below it, from the sheen to the bottom
RIM = (247, 248, 249)
RIM_AMOUNT = 0.55
SHADOW = 0.26
SHADOW_RADIUS = 3
SHADOW_DOWN = 1


class Note:
    """In pixels from its box's top left. The stems are on whole pixels, to be sharp."""
    W, H = 72, 86
    STEM = 7
    LEFT_STEM = 22
    RIGHT_STEM = W - STEM
    BEAM_LEFT_TOP, BEAM_RIGHT_TOP = 12.0, 0.0
    BEAM = 17.0
    # The heads rise to the right. A stem's right edge meets its head where that reaches
    # furthest right, and ends there, for the edge to run on into the head's without a step.
    HEAD_RX, HEAD_RY, HEAD_TILT = 15.0, 11.0, math.radians(25)
    HEAD_HALF_W = math.hypot(HEAD_RX * math.cos(HEAD_TILT), HEAD_RY * math.sin(HEAD_TILT))
    HEAD_MEETS_STEM = (HEAD_RX ** 2 - HEAD_RY ** 2) * math.sin(HEAD_TILT) \
        * math.cos(HEAD_TILT) / HEAD_HALF_W
    HEADS = ((LEFT_STEM + STEM - HEAD_HALF_W, 74.0), (W - HEAD_HALF_W, 71.0))
    # What's inside this circle is above the sheen
    SHEEN = (-12.0, -62.0, 108.0)
    # How far towards its centre of mass it is centred, from the middle of its box: the
    # beam and the right stem make it look off to the right, but the heads stick out on
    # the left, so all the way would be too far
    OPTICAL_CENTRE = 0.6

    @classmethod
    def inside(cls, x, y):
        if 0 <= x <= cls.W and 0 <= y <= cls.H:
            top = cls.BEAM_LEFT_TOP + (cls.BEAM_RIGHT_TOP - cls.BEAM_LEFT_TOP) \
                * (x - cls.LEFT_STEM) / (cls.W - cls.LEFT_STEM)
            if cls.LEFT_STEM <= x <= cls.W and top <= y <= top + cls.BEAM:
                return True
            if cls.LEFT_STEM <= x <= cls.LEFT_STEM + cls.STEM \
                    and top <= y <= cls.HEADS[0][1] - cls.HEAD_MEETS_STEM:
                return True
            if cls.RIGHT_STEM <= x <= cls.W \
                    and top <= y <= cls.HEADS[1][1] - cls.HEAD_MEETS_STEM:
                return True
        cos, sin = math.cos(cls.HEAD_TILT), math.sin(cls.HEAD_TILT)
        for cx, cy in cls.HEADS:
            dx, dy = x - cx, y - cy
            u, v = dx * cos - dy * sin, dx * sin + dy * cos
            if (u / cls.HEAD_RX) ** 2 + (v / cls.HEAD_RY) ** 2 <= 1:
                return True
        return False


def in_round_rect(x, y, left, top, right, bottom, r):
    if not (left <= x <= right and top <= y <= bottom):
        return False
    cx = min(max(x, left + r), right - r)
    cy = min(max(y, top + r), bottom - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r ** 2


def in_triangle(x, y, peak_x, peak_y, left, right, bottom):
    """A peak over a flat base"""
    if not peak_y <= y <= bottom:
        return False
    t = (y - peak_y) / (bottom - peak_y)
    return peak_x + (left - peak_x) * t <= x <= peak_x + (right - peak_x) * t


class Picture:
    """A framed landscape: a frame, a sun and two hills"""
    W, H = 86, 68
    FRAME = 7
    SHEEN = (-10.0, -60.0, 112.0)
    OPTICAL_CENTRE = 0.0

    @classmethod
    def inside(cls, x, y):
        if not in_round_rect(x, y, 0, 0, cls.W, cls.H, 10):
            return False
        if not in_round_rect(x, y, cls.FRAME, cls.FRAME, cls.W - cls.FRAME, cls.H - cls.FRAME, 4):
            return True
        bottom = cls.H - cls.FRAME
        return (math.hypot(x - 60, y - 24) <= 7
                or in_triangle(x, y, 32, 28, 9, 56, bottom)
                or in_triangle(x, y, 58, 41, 43, 76, bottom))


def read_bmp(path):
    d = open(path, 'rb').read()
    off, = struct.unpack_from('<I', d, 10)
    w, h, _, bpp = struct.unpack_from('<iiHH', d, 18)
    step = bpp // 8
    stride = (w * step + 3) & ~3
    rows = []
    for y in range(h):
        o = off + (h - 1 - y) * stride
        rows.append([(d[o + x * step + 2], d[o + x * step + 1], d[o + x * step])
                     for x in range(w)])
    return rows


def write_bmp(path, rows):
    h = len(rows)
    w = len(rows[0])
    stride = (w * 3 + 3) & ~3
    with open(path, 'wb') as f:
        f.write(b'BM' + struct.pack('<IHHI', 54 + stride * h, 0, 0, 54))
        f.write(struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, stride * h, 2835, 2835, 0, 0))
        for y in range(h - 1, -1, -1):
            row = bytearray()
            for r, g, b in rows[y]:
                row += bytes((b, g, r))
            f.write(row + b'\0' * (stride - w * 3))


DITHER = (0, 192, 48, 240, 12, 204, 60, 252, 3, 195, 51, 243, 15, 207, 63, 255)


def native(colour, x, y):
    """A pixel at a place on the screen as read_bmp_fd dithers a backdrop"""
    r, g, b = colour
    d = DITHER[x & 15] ^ (DITHER[y & 15] & 0xAA)
    return ((31 * r + (r >> 3) + d) >> 8, (63 * g + (g >> 2) + d) >> 8,
            (31 * b + (b >> 3) + d) >> 8)


def write_patch(path, rows, area):
    """What goes over a part of a backdrop, dithered to match it. Top-down RGB565, which
    read_bmp_fd takes as it is, and the packaging leaves alone."""
    left, top, right, bottom = area
    w, h = right - left, bottom - top
    pixels = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b = native(rows[y][x], left + x, top + y)
            pixels += struct.pack('<H', (r << 11) | (g << 5) | b)
        if w & 1:
            pixels += b'\0\0'
    with open(path, 'wb') as f:
        f.write(b'BM' + struct.pack('<IHHI', 66 + len(pixels), 0, 0, 66))
        f.write(struct.pack('<IiiHHIIiiII', 40, w, -h, 1, 16, 3, len(pixels), 0, 0, 0, 0))
        f.write(struct.pack('<III', 0xF800, 0x07E0, 0x001F))
        f.write(pixels)


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def coverage(shape, margin):
    """How much of each pixel a shape and a band of a pixel inside its edge take up, in a
    grid with room around it for its shadow"""
    w, h = shape.W + 2 * margin, shape.H + 2 * margin
    grid = [[shape.inside((x + 0.5) / SS - margin, (y + 0.5) / SS - margin)
             for x in range(w * SS)] for y in range(h * SS)]
    cover = [[0.0] * w for _ in range(h)]
    rim = [[0.0] * w for _ in range(h)]
    for y in range(h * SS):
        for x in range(w * SS):
            if not grid[y][x]:
                continue
            cover[y // SS][x // SS] += 1
            near_edge = any(not grid[y + dy][x + dx]
                            for dx, dy in ((-SS, 0), (SS, 0), (0, -SS), (0, SS),
                                           (-SS // 2, -SS // 2), (SS // 2, -SS // 2),
                                           (-SS // 2, SS // 2), (SS // 2, SS // 2)))
            if near_edge:
                rim[y // SS][x // SS] += 1
    n = SS * SS
    return ([[c / n for c in row] for row in cover], [[c / n for c in row] for row in rim])


def blurred(grid, radius):
    h, w = len(grid), len(grid[0])
    for _ in range(2):
        grid = [[sum(row[max(0, x - radius):x + radius + 1]) / (2 * radius + 1)
                 for x in range(w)] for row in grid]
        grid = [[sum(grid[j][x] for j in range(max(0, y - radius), min(h, y + radius + 1)))
                 / (2 * radius + 1) for x in range(w)] for y in range(h)]
    return grid


def material(shape, x, y):
    """A shape's colour at a point of it"""
    t = y / shape.H
    if math.hypot(x - shape.SHEEN[0], y - shape.SHEEN[1]) < shape.SHEEN[2]:
        return mix(LIGHT[0], LIGHT[1], t)
    return mix(SILVER[0], SILVER[1], t)


def draw(rows, shape, left, top):
    margin = SHADOW_RADIUS * 2 + SHADOW_DOWN
    cover, rim = coverage(shape, margin)
    soft = blurred(cover, SHADOW_RADIUS)
    for y, row in enumerate(cover):
        py = top + y - margin
        for x, amount in enumerate(row):
            px = left + x - margin
            back = rows[py][px]
            shadow = soft[y - SHADOW_DOWN][x] if y >= SHADOW_DOWN else 0
            back = mix(back, (0, 0, 0), shadow * SHADOW)
            if amount:
                colour = material(shape, x - margin + 0.5, y - margin + 0.5)
                if rim[y][x]:
                    colour = mix(colour, RIM, RIM_AMOUNT * min(1, rim[y][x] / amount))
                back = mix(back, colour, amount)
            rows[py][px] = tuple(int(round(c)) for c in back)


def optical_offset(shape):
    """How far to move a shape from having its box centred, in whole pixels to keep the
    stems sharp"""
    margin = 2
    cover = coverage(shape, margin)[0]
    mass = sum(map(sum, cover))
    x = sum((i + 0.5 - margin) * c for row in cover for i, c in enumerate(row)) / mass
    y = sum((j + 0.5 - margin) * c for j, row in enumerate(cover) for c in row) / mass
    return (round((x - shape.W / 2) * shape.OPTICAL_CENTRE),
            round((y - shape.H / 2) * shape.OPTICAL_CENTRE))


def panel_gradient():
    """Each row of the panel beside the Settings selection, at its sides, which only have
    the gradient: the middle of a few columns, to leave out the dither of its making"""
    rows = read_bmp(os.path.join(WPS, 'About Backdrop.bmp'))
    gradient = []
    for row in rows:
        side = sorted(row[x] for x in list(range(PANEL_X, PANEL_X + 8)) + list(range(312, 320)))
        gradient.append(side[len(side) // 2])
    return gradient


def native_colours(rows):
    seen = set()
    for y, row in enumerate(rows):
        for x, colour in enumerate(row):
            seen.add(native(colour, x, y))
    return len(seen)


def main():
    gradient = panel_gradient()
    dx, dy = optical_offset(Note)

    path = os.path.join(THEME, 'backdrops', 'iClassic_bd.bmp')
    rows = read_bmp(path)
    for y, row in enumerate(rows):
        row[PANEL_X:] = [gradient[y]] * (len(row) - PANEL_X)
    # around where the Settings panel has what it shows, with the text under it in mind
    draw(rows, Note, PANEL_X + (160 - Note.W) // 2 - dx, 59 - dy)
    write_bmp(path, rows)

    path = os.path.join(WPS, 'iClassic_bg.bmp')
    rows = read_bmp(path)
    art_h = ART[3] - ART[1]
    for y in range(art_h):
        colour = gradient[y * (len(gradient) - 1) // (art_h - 1)]
        rows[ART[1] + y][ART[0]:ART[2]] = [colour] * (ART[2] - ART[0])
    draw(rows, Note, ART[0] + (ART[2] - ART[0] - Note.W) // 2 - dx,
         ART[1] + (art_h - Note.H) // 2 - dy)
    colours = native_colours(rows)
    print(f"Now Playing's backdrop: {colours} colours, of 256 at most")
    if colours > 256:
        raise SystemExit('too many: Whole Screen would not tint it')
    write_bmp(path, rows)

    # on a whole panel, of which it takes its part
    panel = [[gradient[y]] * 320 for y in range(240)]
    draw(panel, Picture, PANEL_X + (160 - Picture.W) // 2, 64)
    write_patch(os.path.join(WPS, 'Photos Icon.bmp'),
                [row[ICON_AREA[0]:ICON_AREA[2]] for row in panel[ICON_AREA[1]:ICON_AREA[3]]],
                ICON_AREA)


if __name__ == '__main__':
    main()
