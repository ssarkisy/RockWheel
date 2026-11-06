#!/usr/bin/env python3
"""Draw the speaker that marks the song playing in a list, as on a stock iPod, and print it
as the C array apps/gui/line.c draws it from: an alpha mask in Rockbox' format, 4 bits a
pixel from 0 opaque to 15 transparent, the low nibble first, the rows running on without
padding, as the chevron's there. The firmware colours it, so it's the same in every theme.
Plain Python, no imaging libraries. Usage:

    python3 themes/tools/make_list_glyphs.py [preview.bmp]

With a file name it also writes the mask, eight times as large, black on white, to look at.
"""
import math
import struct
import sys

SS = 8  # samples per pixel on each axis

W, H = 15, 12
CENTRE_Y = H / 2
# the speaker: its box, then its cone opening to the right
BOX = (0.5, 4.0, 3.5, 8.0)
CONE_X0, CONE_X1 = 3.5, 7.5
CONE_TOP0, CONE_TOP1 = 4.0, 0.5
# its sound: two arcs about the cone's mouth, with round ends
WAVE_X = 7.5
WAVES = (3.2, 6.2)
WAVE_THICK = 1.5
WAVE_ANGLE = math.radians(48)


def inside(x, y):
    if BOX[0] <= x <= BOX[2] and BOX[1] <= y <= BOX[3]:
        return True
    if CONE_X0 <= x <= CONE_X1:
        t = (x - CONE_X0) / (CONE_X1 - CONE_X0)
        top = CONE_TOP0 + (CONE_TOP1 - CONE_TOP0) * t
        if top <= y <= H - top:
            return True
    dx, dy = x - WAVE_X, y - CENTRE_Y
    r = math.hypot(dx, dy)
    a = math.atan2(dy, dx)
    for radius in WAVES:
        if abs(r - radius) <= WAVE_THICK / 2 and abs(a) <= WAVE_ANGLE:
            return True
        for end in (-WAVE_ANGLE, WAVE_ANGLE):
            ex = WAVE_X + radius * math.cos(end)
            ey = CENTRE_Y + radius * math.sin(end)
            if math.hypot(x - ex, y - ey) <= WAVE_THICK / 2:
                return True
    return False


def coverage():
    rows = []
    for py in range(H):
        row = []
        for px in range(W):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    if inside(px + (sx + 0.5) / SS, py + (sy + 0.5) / SS):
                        hits += 1
            row.append(hits / (SS * SS))
        rows.append(row)
    return rows


def nibbles(cov):
    return [15 - round(15 * c) for row in cov for c in row]


def c_array(nib):
    if len(nib) % 2:
        nib = nib + [15]
    data = [nib[i] | (nib[i + 1] << 4) for i in range(0, len(nib), 2)]
    lines = []
    for i in range(0, len(data), 12):
        lines.append('    ' + ', '.join('0x%02x' % b for b in data[i:i + 12]) + ',')
    return '\n'.join(lines)


def preview(cov, path, k=8):
    w, h = W * k, H * k
    stride = (w * 3 + 3) & ~3
    body = bytearray()
    for y in range(h - 1, -1, -1):
        line = bytearray()
        for x in range(w):
            v = round(255 * (1 - cov[y // k][x // k]))
            line += bytes((v, v, v))
        line += b'\0' * (stride - len(line))
        body += line
    head = b'BM' + struct.pack('<IHHI', 54 + len(body), 0, 0, 54)
    head += struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, len(body), 2835, 2835, 0, 0)
    with open(path, 'wb') as f:
        f.write(head + body)


def main():
    cov = coverage()
    print('#define SPEAKER_W %d' % W)
    print('#define SPEAKER_H %d' % H)
    print('static const unsigned char speaker_alpha[] = {')
    print(c_array(nibbles(cov)))
    print('};')
    if len(sys.argv) > 1:
        preview(cov, sys.argv[1])


if __name__ == '__main__':
    main()
