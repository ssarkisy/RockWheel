#!/usr/bin/env python3
# Makes the bitmaps of the screen shown while connected over USB, which has to
# be built in as the disk can't be read then: the red sign and the green check
# mark, with the white around them, and the battery in the title bar, as 24-bit
# BMPs for bmp2rb.
#
#   themes/tools/make_usb_screen.py THEME/wps/iClassic-Square/Battery.bmp OUTDIR
#
# The two signs are drawn here, the red one in the size and colours of the stock
# iPod's, the green one in the same style. The battery
# has two frames from the iClassic theme's, by Humberto Santana (CC BY-SA 3.0):
# charging, and on power but not charging. What's around them is made transparent.

import math
import os
import struct
import sys

if len(sys.argv) != 3:
    sys.exit("usage: make_usb_screen.py Battery.bmp OUTDIR")
battery_path, out_dir = sys.argv[1:]


def write_bmp(path, width, height, pixel):
    stride = (width * 3 + 3) & ~3
    with open(path, "wb") as f:
        f.write(b"BM" + struct.pack("<IHHI", 54 + stride * height, 0, 0, 54))
        f.write(struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0,
                            stride * height, 2835, 2835, 0, 0))
        for y in range(height - 1, -1, -1):
            row = bytearray()
            for x in range(width):
                r, g, b = pixel(x, y)
                row += bytes((b, g, r))
            f.write(row + b"\0" * (stride - width * 3))
    print(path)


# --- the sign
WIDTH, HEIGHT = 96, 98
CX, CY = 47.5, 46.5
R_OUT, R_IN = 44.5, 31.5
BAR = 16.0                # width of the bar, from top left to bottom right
SHADOW_DROP = 2.0         # how far below the sign its shadow is
SHADOW_REACH = 3.5        # how far around it the shadow fades
SHADOW_ALPHA = 0.36
SHADOW = (150, 160, 205)
# from the top of the sign to its bottom
SHADE_RED = ((0.00, (230, 64, 32)), (0.55, (226, 62, 26)),
             (0.78, (190, 46, 22)), (1.00, (124, 30, 13)))
SHADE_GREEN = ((0.00, (128, 214, 80)), (0.45, (86, 188, 56)),
               (0.75, (52, 152, 40)), (1.00, (30, 112, 28)))
# The check mark: two strokes with round ends, in the bitmap's pixels, by the
# middle of each end and its radius
CHECK = ((14.0, 51.0, 7.0), (37.0, 74.0, 8.5), (82.5, 19.5, 5.0))
# It's lit from the top left, on an edge this wide, by this much
BEVEL = 5.0
BEVEL_LIGHT = 0.55
BEVEL_DARK = 0.30
LIGHT = (-0.55, -0.83)


def distance_red(x, y):
    """To the edge of the red sign, negative inside"""
    dx, dy = x - CX, y - CY
    r = math.hypot(dx, dy)
    ring = max(r - R_OUT, R_IN - r)
    bar = max(r - R_OUT, abs(dx - dy) / math.sqrt(2) - BAR / 2)
    return min(ring, bar)


def distance_check(x, y):
    """To the edge of the check mark, negative inside"""
    best = 1e9
    for (ax, ay, ar), (bx, by, br) in zip(CHECK, CHECK[1:]):
        ex, ey = bx - ax, by - ay
        t = ((x - ax) * ex + (y - ay) * ey) / (ex * ex + ey * ey)
        t = max(0.0, min(1.0, t))
        best = min(best, math.hypot(x - ax - t * ex, y - ay - t * ey)
                   - (ar + (br - ar) * t))
    return best


def shade(stops, y):
    t = min(max((y - (CY - R_OUT)) / (2 * R_OUT), 0.0), 1.0)
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        if t <= t1:
            k = (t - t0) / (t1 - t0)
            return tuple(a + (b - a) * k for a, b in zip(c0, c1))
    return stops[-1][1]


def shade_check(y):
    t = min(max((y - 12.0) / 72.0, 0.0), 1.0)
    return shade(SHADE_GREEN, CY - R_OUT + t * 2 * R_OUT)


def coverage(distance, x, y):
    """How much of a pixel is inside, by 4 x 4 samples"""
    inside = 0
    for sy in range(4):
        for sx in range(4):
            if distance(x + (sx + 0.5) / 4, y + (sy + 0.5) / 4) <= 0:
                inside += 1
    return inside / 16


def blend(back, colour, k):
    return tuple(b + (c - b) * k for b, c in zip(back, colour))


def shadowed_white(distance, x, y):
    d = distance(x + 0.5, y + 0.5 - SHADOW_DROP)
    a = SHADOW_ALPHA * max(0.0, min(1.0, 1 - d / SHADOW_REACH)) ** 2
    return tuple(255 + (c - 255) * a for c in SHADOW)


def rounded(colour):
    return tuple(int(round(c)) for c in colour)


def red_pixel(x, y):
    back = shadowed_white(distance_red, x, y)
    colour = shade(SHADE_RED, y + 0.5)
    return rounded(blend(back, colour, coverage(distance_red, x, y)))


def check_pixel(x, y):
    back = shadowed_white(distance_check, x, y)
    cx, cy = x + 0.5, y + 0.5
    colour = shade_check(cy)
    # the edge facing the light brighter, the one away from it darker
    d = distance_check(cx, cy)
    if d < 0:
        nx = distance_check(cx + 0.5, cy) - distance_check(cx - 0.5, cy)
        ny = distance_check(cx, cy + 0.5) - distance_check(cx, cy - 0.5)
        lit = (nx * LIGHT[0] + ny * LIGHT[1]) * max(0.0, 1 + d / BEVEL)
        if lit > 0:
            colour = blend(colour, (255, 255, 255), min(1.0, lit * BEVEL_LIGHT))
        else:
            colour = blend(colour, (0, 0, 0), min(1.0, -lit * BEVEL_DARK))
    return rounded(blend(back, colour, coverage(distance_check, x, y)))


write_bmp(os.path.join(out_dir, "nodisconnect.%dx%dx16.bmp" % (WIDTH, HEIGHT)),
          WIDTH, HEIGHT, red_pixel)
write_bmp(os.path.join(out_dir, "usbok.%dx%dx16.bmp" % (WIDTH, HEIGHT)),
          WIDTH, HEIGHT, check_pixel)


# --- the battery
data = open(battery_path, "rb").read()
offset = struct.unpack("<I", data[10:14])[0]
bat_w, bat_h = struct.unpack("<ii", data[18:26])
if struct.unpack("<H", data[28:30])[0] != 24:
    sys.exit("expected a 24-bit BMP")
bat_stride = (bat_w * 3 + 3) & ~3
FRAME_H = 11
FRAMES = (1, 2)
PINK = (255, 0, 255)


def theme_pixel(x, y):
    o = offset + (bat_h - 1 - y) * bat_stride + x * 3
    return data[o + 2], data[o + 1], data[o]


def battery_pixel(x, y):
    frame, row = y // FRAME_H, y % FRAME_H
    r, g, b = theme_pixel(x, FRAMES[frame] * FRAME_H + row)
    # the corners around the tip of the battery are the theme's backdrop
    if x >= 22 and row in (0, 1, 9, 10) and min(r, g, b) > 170:
        return PINK
    return r, g, b


write_bmp(os.path.join(out_dir, "usbbattery.%dx%dx16.bmp"
                       % (bat_w, FRAME_H * len(FRAMES))),
          bat_w, FRAME_H * len(FRAMES), battery_pixel)
