#!/usr/bin/env python3
# Makes the scrubber's hints of iClassic Square, shown for a podcast or an audiobook, whose
# Previous and Next there go 15 seconds back and 30 on:
#
#     themes/tools/make_scrub_hints.py
#
# Scrub Hints.bmp: two images, one over the other, each a turning arrow and a number: the
# arrow drawn here at eight times the size and brought down, the digits those of the
# theme's 12-Inter-SemiBold.fnt as they are, not scaled. Grey with an alpha channel, so it
# sits on any background and takes the cover's colours as the theme's other images do.
# Standard library only. Run make_dark_theme.py after it.
import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
THEME = os.path.join(HERE, "..", "iClassic-Square", ".rockbox", "wps", "iClassic-Square")
FONT = os.path.join(HERE, "..", "fonts", ".rockbox", "fonts", "12-Inter-SemiBold.fnt")
INK = (0x70, 0x70, 0x70)    # a shade under the chapter line's, which the dark theme lightens
WIDTH, HEIGHT = 26, 13      # of one image
SCALE = 8
CX, CY, RADIUS, STROKE = 5.5, 7.5, 4.0, 1.4
GAP = 62                    # degrees of the ring left open, before the arrow's head
HEAD = ((1.6, 3.5), (6.3, 0.7), (6.3, 6.3))    # the head of the arrow that turns back
DIGITS_TOP = 2              # the font's row 0: its digits are in rows 2 to 8


def glyphs(text):
    """The text in the font: rows of 0..15, how much of each pixel is ink"""
    with open(FONT, "rb") as f:
        d = f.read()
    assert d[:4] == b"RB12"
    height, depth = struct.unpack_from("<H", d, 6)[0], struct.unpack_from("<H", d, 10)[0]
    first, default, size, nbits, noffset, nwidth = struct.unpack_from("<IIIIII", d, 12)
    assert depth == 1                       # anti-aliased
    long = nbits >= 0xFFDB                  # then the offsets are of 32 bits
    p = (36 + nbits + 3) & ~3 if long else (36 + nbits + 1) & ~1
    offsets = struct.unpack_from("<%d%s" % (noffset, "I" if long else "H"), d, p)
    p += noffset * (4 if long else 2)
    widths = d[p:p + nwidth]
    rows = [[] for _ in range(height)]
    for ch in text:
        i = ord(ch) - first
        w, o = widths[i], 36 + offsets[i]
        for y in range(height):
            for x in range(w):
                n = y * w + x
                v = d[o + n // 2] >> 4 if n & 1 else d[o + n // 2] & 15
                rows[y].append(15 - v)
    return rows


def in_head(x, y):
    sign = None
    for i in range(3):
        (ax, ay), (bx, by) = HEAD[i], HEAD[(i + 1) % 3]
        side = (bx - ax) * (y - ay) - (by - ay) * (x - ax) >= 0
        if sign is None:
            sign = side
        elif side != sign:
            return False
    return True


def arrow(x, y):
    """Is this point of the arrow that turns back: a ring open from its top leftwards, the
    head at the top pointing left"""
    if in_head(x, y):
        return True
    dx, dy = x - CX, y - CY
    if abs(math.hypot(dx, dy) - RADIUS) > STROKE / 2:
        return False
    clockwise_from_top = math.degrees(math.atan2(dx, -dy)) % 360
    return clockwise_from_top <= 360 - GAP


def image(text, forward):
    """Coverage, 0..255, of one image: the arrow at the outer end, the number inside"""
    cover = [[0] * WIDTH for _ in range(HEIGHT)]
    for y in range(HEIGHT):
        for x in range(11):
            hits = sum(arrow(x + (i + 0.5) / SCALE, y + (j + 0.5) / SCALE)
                       for i in range(SCALE) for j in range(SCALE))
            cover[y][x] = round(255 * hits / SCALE ** 2)
    digits = glyphs(text)
    for y, row in enumerate(digits):
        for x, v in enumerate(row):
            if v and 0 <= y + DIGITS_TOP < HEIGHT:
                cover[y + DIGITS_TOP][13 + x] = v * 17
    used = 13 + len(digits[0])
    assert used <= WIDTH
    if forward:     # the arrow at the right, turning the other way, the number before it
        mirrored = [[0] * WIDTH for _ in range(HEIGHT)]
        for y in range(HEIGHT):
            for x in range(11):
                mirrored[y][WIDTH - 1 - x] = cover[y][x]
            for x in range(13, used):
                mirrored[y][WIDTH - used + x - 13] = cover[y][x]
        cover = mirrored
    return cover


def main():
    rows = image("15", False) + image("30", True)
    stride = WIDTH * 4
    out = bytearray(b"BM" + struct.pack("<IHHI", 54 + stride * len(rows), 0, 0, 54))
    out += struct.pack("<IiiHHIIiiII", 40, WIDTH, len(rows), 1, 32, 0, stride * len(rows),
                       2835, 2835, 0, 0)
    for row in reversed(rows):
        for a in row:
            out += bytes((INK[2], INK[1], INK[0], a))
    with open(os.path.join(THEME, "Scrub Hints.bmp"), "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main()
