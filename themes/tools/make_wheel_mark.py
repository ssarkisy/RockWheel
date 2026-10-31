#!/usr/bin/env python3
# Draws the mark of the boot screen and of the panel beside the main menu: a glossy
# ring like a click wheel, in place of any maker's logo. Two 24-bit BMPs for bmp2rb
# and make_about_panel.py:
#
#   themes/tools/make_wheel_mark.py OUTDIR
#
# bootlogo.64x80x16.bmp is the mark on black, for apps/bitmaps/native. Its picture
# for the panel is panel_picture.bmp: then
#
#   themes/tools/make_about_panel.py OUTDIR/panel_picture.bmp iClassic_bd.bmp OUTDIR

import math
import os
import struct
import sys

if len(sys.argv) != 2:
    sys.exit("usage: make_wheel_mark.py OUTDIR")
out_dir = sys.argv[1]

# the panel's gradient, top to bottom
PANEL_W, PANEL_H = 160, 240
PANEL_STOPS = ((0.0, (162, 174, 186)), (0.25, (141, 151, 161)), (0.45, (123, 131, 141)),
               (0.625, (108, 115, 123)), (0.833, (91, 95, 104)), (1.0, (76, 81, 87)))
PANEL_MARK_Y = 107


def mix(a, b, k):
    return tuple(a[i] + (b[i] - a[i]) * k for i in range(3))


def stops_at(stops, t):
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        if t <= t1:
            return mix(c0, c1, (t - t0) / (t1 - t0))
    return stops[-1][1]


def wheel(x, y, radius):
    """(cover 0..1, colour) of a point of the ring, relative to its centre"""
    d = math.hypot(x, y)
    hub = radius * 0.37
    if d > radius or d < hub:
        return None
    t = (y + radius) / (2 * radius)
    colour = stops_at(((0.0, (252, 252, 252)), (0.5, (214, 217, 220)),
                       (1.0, (142, 147, 152))), t)
    # the shine of the upper half, a soft edge across the ring
    if y < -0.05 * radius + 0.16 * x * x / radius:
        colour = mix(colour, (255, 255, 255), 0.45)
    # darker at both rims
    rim = min(radius - d, d - hub) / radius
    if rim < 0.05:
        colour = mix(colour, (120, 124, 130), 0.55 * (1 - rim / 0.05))
    return colour


def render(width, height, radius, cx, cy, back):
    rows = []
    for y in range(height):
        row = []
        for x in range(width):
            base = back(x, y)
            total = (0.0, 0.0, 0.0)
            for sy in range(4):
                for sx in range(4):
                    c = wheel(x + (sx + 0.5) / 4 - cx, y + (sy + 0.5) / 4 - cy, radius)
                    c = c if c else base
                    total = tuple(total[i] + c[i] for i in range(3))
            row.append(tuple(int(round(v / 16)) for v in total))
        rows.append(row)
    return rows


def write_bmp(name, rows):
    width, height = len(rows[0]), len(rows)
    stride = (width * 3 + 3) & ~3
    with open(os.path.join(out_dir, name), "wb") as f:
        f.write(b"BM" + struct.pack("<IHHI", 54 + stride * height, 0, 0, 54))
        f.write(struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0,
                            stride * height, 2835, 2835, 0, 0))
        for row in reversed(rows):
            data = bytearray()
            for r, g, b in row:
                data += bytes((b, g, r))
            f.write(data + b"\0" * (stride - width * 3))


BLACK = lambda x, y: (0, 0, 0)
write_bmp("bootlogo.64x80x16.bmp", render(64, 80, 31.5, 32, 40, BLACK))
write_bmp("panel_picture.bmp",
          render(PANEL_W, PANEL_H, 31, PANEL_W / 2, PANEL_MARK_Y,
                 lambda x, y: stops_at(PANEL_STOPS, (y + 0.5) / PANEL_H)))
