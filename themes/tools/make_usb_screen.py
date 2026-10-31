#!/usr/bin/env python3
# Makes the bitmaps of the screen shown while connected over USB, which has to
# be built in as the disk can't be read then: the red sign, with the white around
# it, and the battery in the title bar, as 24-bit BMPs for bmp2rb.
#
#   themes/tools/make_usb_screen.py THEME/wps/iClassic-Square/Battery.bmp OUTDIR
#
# The sign is drawn here, in the size and colours of the stock iPod's. The battery
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
SHADE = ((0.00, (230, 64, 32)), (0.55, (226, 62, 26)),
         (0.78, (190, 46, 22)), (1.00, (124, 30, 13)))


def distance(x, y):
    """To the edge of the sign, negative inside"""
    dx, dy = x - CX, y - CY
    r = math.hypot(dx, dy)
    ring = max(r - R_OUT, R_IN - r)
    bar = max(r - R_OUT, abs(dx - dy) / math.sqrt(2) - BAR / 2)
    return min(ring, bar)


def shade(y):
    t = min(max((y - (CY - R_OUT)) / (2 * R_OUT), 0.0), 1.0)
    for (t0, c0), (t1, c1) in zip(SHADE, SHADE[1:]):
        if t <= t1:
            k = (t - t0) / (t1 - t0)
            return tuple(a + (b - a) * k for a, b in zip(c0, c1))
    return SHADE[-1][1]


def sign_pixel(x, y):
    # what's behind the sign: its shadow on white
    d = distance(x + 0.5, y + 0.5 - SHADOW_DROP)
    a = SHADOW_ALPHA * max(0.0, min(1.0, 1 - d / SHADOW_REACH)) ** 2
    back = tuple(255 + (c - 255) * a for c in SHADOW)
    # the sign itself, 4 x 4 samples for each pixel
    inside = 0
    for sy in range(4):
        for sx in range(4):
            if distance(x + (sx + 0.5) / 4, y + (sy + 0.5) / 4) <= 0:
                inside += 1
    k = inside / 16
    colour = shade(y + 0.5)
    return tuple(int(round(b + (c - b) * k)) for b, c in zip(back, colour))


write_bmp(os.path.join(out_dir, "nodisconnect.%dx%dx16.bmp" % (WIDTH, HEIGHT)),
          WIDTH, HEIGHT, sign_pixel)


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
