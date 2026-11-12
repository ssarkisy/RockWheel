#!/usr/bin/env python3
# Makes the iOS look's status icon of Shuffle by albums, "iOS Shuffle Albums.bmp", from
# the Classic look's, "Status Shuffle Albums.bmp", which is drawn by hand (two covers
# changing places; the owner's choice, 2026-11-11):
#
#     themes/tools/make_shuffle_albums.py
#
# The same shape in the ink of "iOS Shuffle.bmp", each pixel as opaque as it is dark in
# the Classic one, whose magenta is empty. Run make_dark_theme.py after it.
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
THEME = os.path.join(HERE, '..', 'iClassic-Square', '.rockbox', 'wps', 'iClassic-Square')
PAPER = 214  # the status bar's lightness, where the icon's ink has thinned to nothing


def pixels(data):
    start, = struct.unpack_from('<I', data, 10)
    width, height = struct.unpack_from('<ii', data, 18)
    size = struct.unpack_from('<H', data, 28)[0] // 8
    row = (width * size + 3) // 4 * 4
    return [start + (height - 1 - y if height > 0 else y) * row + x * size
            for y in range(abs(height)) for x in range(width)], size


def main():
    classic = open(os.path.join(THEME, 'Status Shuffle Albums.bmp'), 'rb').read()
    out = bytearray(open(os.path.join(THEME, 'iOS Shuffle.bmp'), 'rb').read())
    from_at, from_size = pixels(classic)
    to_at, to_size = pixels(out)
    assert len(from_at) == len(to_at) and from_size == 3 and to_size == 4
    ink = max((bytes(out[at:at + 4]) for at in to_at), key=lambda p: p[3])
    grey = [sum(classic[at:at + 3]) / 3 for at in from_at
            if classic[at:at + 3] != b'\xff\x00\xff']
    darkest = min(grey)
    for src, dst in zip(from_at, to_at):
        p = classic[src:src + 3]
        if p == b'\xff\x00\xff':
            out[dst:dst + 4] = b'\0\0\0\0'
            continue
        cover = (PAPER - sum(p) / 3) / (PAPER - darkest)
        out[dst:dst + 4] = ink[:3] + bytes([round(255 * min(max(cover, 0), 1))])
    open(os.path.join(THEME, 'iOS Shuffle Albums.bmp'), 'wb').write(out)


if __name__ == '__main__':
    main()
