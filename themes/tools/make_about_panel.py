#!/usr/bin/env python3
# Makes what iClassic Square shows beside the main menu when Settings is selected,
# as 24-bit BMPs: a second backdrop with the theme's left half, and on the right
# the logo and the empty bar of how much of the disk is used, and that bar's fill.
#
#   themes/tools/make_about_panel.py PICTURE THEME_BACKDROP OUTDIR
#
# PICTURE has the logo in the middle of a gradient, and is cropped to the panel's
# shape around its middle, by sips (macOS).

import os
import struct
import subprocess
import sys
import tempfile

if len(sys.argv) != 4:
    sys.exit("usage: make_about_panel.py PICTURE THEME_BACKDROP OUTDIR")
picture, theme_backdrop, out_dir = sys.argv[1:]

PANEL_W, PANEL_H = 160, 240
BAR_X, BAR_Y, BAR_W, BAR_H = 20, 174, 128, 11   # as in the theme's .sbs
RADIUS = BAR_H / 2


def size_of(path):
    out = subprocess.check_output(["sips", "-g", "pixelWidth", "-g", "pixelHeight", path],
                                  text=True).split()
    return int(out[out.index("pixelWidth:") + 1]), int(out[out.index("pixelHeight:") + 1])


def read_bmp(path):
    data = open(path, "rb").read()
    offset = struct.unpack("<I", data[10:14])[0]
    width, height = struct.unpack("<ii", data[18:26])
    bpp = struct.unpack("<H", data[28:30])[0]
    top_down = height < 0
    height = abs(height)
    step = bpp // 8
    stride = (width * step + 3) & ~3

    def pixel(x, y):
        row = y if top_down else height - 1 - y
        o = offset + row * stride + x * step
        return data[o + 2], data[o + 1], data[o]
    return pixel


def write_bmp(name, width, height, pixel):
    stride = (width * 3 + 3) & ~3
    with open(os.path.join(out_dir, name), "wb") as f:
        f.write(b"BM" + struct.pack("<IHHI", 54 + stride * height, 0, 0, 54))
        f.write(struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0,
                            stride * height, 2835, 2835, 0, 0))
        for y in range(height - 1, -1, -1):
            row = bytearray()
            for x in range(width):
                r, g, b = pixel(x, y)
                row += bytes((b, g, r))
            f.write(row + b"\0" * (stride - width * 3))


def blend(back, colour, k):
    return tuple(int(round(b + (c - b) * k)) for b, c in zip(back, colour))


def inside_bar(x, y):
    """How much of a pixel of the bar is inside its rounded ends"""
    inside = 0
    for sy in range(4):
        for sx in range(4):
            px, py = x + (sx + 0.5) / 4, y + (sy + 0.5) / 4
            cx = min(max(px, RADIUS), BAR_W - RADIUS)
            if (px - cx) ** 2 + (py - RADIUS) ** 2 <= RADIUS ** 2:
                inside += 1
    return inside / 16


def vertical(stops, y):
    t = (y + 0.5) / BAR_H
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        if t <= t1:
            return blend(c0, c1, (t - t0) / (t1 - t0))
    return stops[-1][1]


TRACK = ((0.0, (28, 30, 34)), (0.5, (44, 47, 52)), (1.0, (66, 70, 76)))
FILL = ((0.0, (244, 245, 246)), (0.5, (214, 217, 220)), (1.0, (172, 176, 181)))

with tempfile.TemporaryDirectory() as tmp:
    w, h = size_of(picture)
    crop_w = h * PANEL_W // PANEL_H
    cropped = os.path.join(tmp, "panel.bmp")
    subprocess.check_call(["sips", "-s", "format", "bmp", picture,
                           "--cropOffset", "0", str((w - crop_w) // 2),
                           "-c", str(h), str(crop_w), "--out", cropped],
                          stdout=subprocess.DEVNULL)
    subprocess.check_call(["sips", "-z", str(PANEL_H), str(PANEL_W), cropped],
                          stdout=subprocess.DEVNULL)
    panel = read_bmp(cropped)
    theme = read_bmp(theme_backdrop)

    def track(x, y):
        return blend(panel(BAR_X + x, BAR_Y + y), vertical(TRACK, y), inside_bar(x, y))

    def fill(x, y):
        return blend(track(x, y), vertical(FILL, y), inside_bar(x, y))

    def backdrop(x, y):
        if x < PANEL_W:
            return theme(x, y)
        px = x - PANEL_W
        if BAR_X <= px < BAR_X + BAR_W and BAR_Y <= y < BAR_Y + BAR_H:
            return track(px - BAR_X, y - BAR_Y)
        return panel(px, y)

    write_bmp("About Backdrop.bmp", 2 * PANEL_W, PANEL_H, backdrop)
    write_bmp("Disk Fill.bmp", BAR_W, BAR_H, fill)
