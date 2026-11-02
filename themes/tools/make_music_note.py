#!/usr/bin/env python3
"""Draw iClassic Square's music note, for when there is no cover: on the right panel of
the menus' backdrop, and as the placeholder art of Now Playing's. Both on the gradient of
the panel shown when Settings is selected, in the same material as what that shows: light
above a curved sheen and silver below it, with a light rim and a soft shadow.

Plain Python, no imaging libraries. Usage:

    python3 themes/tools/make_music_note.py

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

# The note, in pixels from its box's top left. The stems are on whole pixels, to be sharp.
NOTE_W, NOTE_H = 72, 86
STEM = 7
LEFT_STEM = 22
RIGHT_STEM = NOTE_W - STEM
BEAM_LEFT_TOP, BEAM_RIGHT_TOP = 12.0, 0.0
BEAM = 17.0
# The heads rise to the right. A stem's right edge meets its head where that reaches
# furthest right, and ends there, for the edge to run on into the head's without a step.
HEAD_RX, HEAD_RY, HEAD_TILT = 15.0, 11.0, math.radians(25)
HEAD_HALF_W = math.hypot(HEAD_RX * math.cos(HEAD_TILT), HEAD_RY * math.sin(HEAD_TILT))
HEAD_MEETS_STEM = (HEAD_RX ** 2 - HEAD_RY ** 2) * math.sin(HEAD_TILT) * math.cos(HEAD_TILT) \
    / HEAD_HALF_W
HEADS = ((LEFT_STEM + STEM - HEAD_HALF_W, 74.0), (NOTE_W - HEAD_HALF_W, 71.0))
# What's inside this circle is above the sheen
SHEEN = (-12.0, -62.0, 108.0)

LIGHT = ((250, 250, 249), (238, 239, 240))  # above the sheen, from top to bottom
SILVER = ((186, 193, 199), (228, 231, 234))  # below it, from the sheen to the bottom
RIM = (247, 248, 249)
RIM_AMOUNT = 0.55
SHADOW = 0.26
SHADOW_RADIUS = 3
SHADOW_DOWN = 1
# How far towards its centre of mass the note is centred, from the middle of its box: the
# beam and the right stem make it look off to the right, but the heads stick out on the
# left, so all the way would be too far
OPTICAL_CENTRE = 0.6


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


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def inside_note(x, y):
    """Whether a point is in the note"""
    if 0 <= x <= NOTE_W and 0 <= y <= NOTE_H:
        top = BEAM_LEFT_TOP + (BEAM_RIGHT_TOP - BEAM_LEFT_TOP) * (x - LEFT_STEM) \
            / (NOTE_W - LEFT_STEM)
        if LEFT_STEM <= x <= NOTE_W and top <= y <= top + BEAM:
            return True
        if LEFT_STEM <= x <= LEFT_STEM + STEM and top <= y <= HEADS[0][1] - HEAD_MEETS_STEM:
            return True
        if RIGHT_STEM <= x <= NOTE_W and top <= y <= HEADS[1][1] - HEAD_MEETS_STEM:
            return True
    cos, sin = math.cos(HEAD_TILT), math.sin(HEAD_TILT)
    for cx, cy in HEADS:
        dx, dy = x - cx, y - cy
        u, v = dx * cos - dy * sin, dx * sin + dy * cos
        if (u / HEAD_RX) ** 2 + (v / HEAD_RY) ** 2 <= 1:
            return True
    return False


def coverage(margin):
    """How much of each pixel the note and a band of a pixel inside its edge take up, in
    a grid with room around the note for its shadow"""
    w, h = NOTE_W + 2 * margin, NOTE_H + 2 * margin
    grid = [[inside_note((x + 0.5) / SS - margin, (y + 0.5) / SS - margin)
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


def material(x, y):
    """The note's colour at a point of it"""
    t = y / NOTE_H
    if math.hypot(x - SHEEN[0], y - SHEEN[1]) < SHEEN[2]:
        return mix(LIGHT[0], LIGHT[1], t)
    return mix(SILVER[0], SILVER[1], t)


def draw_note(rows, left, top):
    margin = SHADOW_RADIUS * 2 + SHADOW_DOWN
    cover, rim = coverage(margin)
    soft = blurred(cover, SHADOW_RADIUS)
    for y, row in enumerate(cover):
        py = top + y - margin
        for x, amount in enumerate(row):
            px = left + x - margin
            back = rows[py][px]
            shadow = soft[y - SHADOW_DOWN][x] if y >= SHADOW_DOWN else 0
            back = mix(back, (0, 0, 0), shadow * SHADOW)
            if amount:
                colour = material(x - margin + 0.5, y - margin + 0.5)
                if rim[y][x]:
                    colour = mix(colour, RIM, RIM_AMOUNT * min(1, rim[y][x] / amount))
                back = mix(back, colour, amount)
            rows[py][px] = tuple(int(round(c)) for c in back)


def optical_offset():
    """How far to move the note from having its box centred, in whole pixels to keep the
    stems sharp"""
    margin = 2
    cover = coverage(margin)[0]
    mass = sum(map(sum, cover))
    x = sum((i + 0.5 - margin) * c for row in cover for i, c in enumerate(row)) / mass
    y = sum((j + 0.5 - margin) * c for j, row in enumerate(cover) for c in row) / mass
    return (round((x - NOTE_W / 2) * OPTICAL_CENTRE), round((y - NOTE_H / 2) * OPTICAL_CENTRE))


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
    """What read_bmp_fd makes of these as a dithered backdrop"""
    table = (0, 192, 48, 240, 12, 204, 60, 252, 3, 195, 51, 243, 15, 207, 63, 255)
    seen = set()
    for y, row in enumerate(rows):
        dy = table[y & 15] & 0xAA
        for x, (r, g, b) in enumerate(row):
            d = table[x & 15] ^ dy
            seen.add(((31 * r + (r >> 3) + d) >> 8, (63 * g + (g >> 2) + d) >> 8,
                      (31 * b + (b >> 3) + d) >> 8))
    return len(seen)


def main():
    gradient = panel_gradient()
    dx, dy = optical_offset()

    path = os.path.join(THEME, 'backdrops', 'iClassic_bd.bmp')
    rows = read_bmp(path)
    for y, row in enumerate(rows):
        row[PANEL_X:] = [gradient[y]] * (len(row) - PANEL_X)
    # around where the Settings panel has what it shows, with the text under it in mind
    draw_note(rows, PANEL_X + (160 - NOTE_W) // 2 - dx, 59 - dy)
    write_bmp(path, rows)

    path = os.path.join(WPS, 'iClassic_bg.bmp')
    rows = read_bmp(path)
    art_h = ART[3] - ART[1]
    for y in range(art_h):
        colour = gradient[y * (len(gradient) - 1) // (art_h - 1)]
        rows[ART[1] + y][ART[0]:ART[2]] = [colour] * (ART[2] - ART[0])
    draw_note(rows, ART[0] + (ART[2] - ART[0] - NOTE_W) // 2 - dx,
              ART[1] + (art_h - NOTE_H) // 2 - dy)
    colours = native_colours(rows)
    print(f"Now Playing's backdrop: {colours} colours, of 256 at most")
    if colours > 256:
        raise SystemExit('too many: Whole Screen would not tint it')
    write_bmp(path, rows)


if __name__ == '__main__':
    main()
