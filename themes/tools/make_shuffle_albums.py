#!/usr/bin/env python3
# Makes the status bar's icon of Shuffle by albums from the one of Shuffle by songs, in
# both looks of iClassic Square: the crossing arrows with an album between their tails.
#
#     themes/tools/make_shuffle_albums.py [block|lines|disc] [--to DIR]
#
# The marks, of the icon's darkest colour, at the left between the two tails: a block,
# a second line under each tail, or a ring. Without a name, the block. With --to, the
# two files go to DIR under the variant's name instead of into the theme, for a sheet
# to choose from. Run make_dark_theme.py after it.
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
THEME = os.path.join(HERE, '..', 'iClassic-Square', '.rockbox', 'wps', 'iClassic-Square')
# (x, y) from the top left of the 16x16 icon
MARKS = {
    'Status Shuffle': {
        'block': [(x, y) for y in (6, 7, 8) for x in (0, 1, 2)],
        'lines': [(x, 6) for x in range(4)] + [(x, 12) for x in range(4)],
        'disc': [(1, 5), (2, 5), (0, 6), (3, 6), (0, 7), (3, 7), (1, 8), (2, 8)],
    },
    'iOS Shuffle': {
        'block': [(x, y) for y in (7, 8, 9) for x in (1, 2, 3)],
        'lines': [(x, 7) for x in range(1, 5)] + [(x, 13) for x in range(1, 5)],
        'disc': [(2, 6), (3, 6), (1, 7), (4, 7), (1, 8), (4, 8), (2, 9), (3, 9)],
    },
}


def make(name, variant, out):
    data = bytearray(open(os.path.join(THEME, name + '.bmp'), 'rb').read())
    start, = struct.unpack_from('<I', data, 10)
    width, height = struct.unpack_from('<ii', data, 18)
    size = struct.unpack_from('<H', data, 28)[0] // 8
    row = (width * size + 3) // 4 * 4

    def at(x, y):
        return start + (height - 1 - y if height > 0 else y) * row + x * size

    pixels = [bytes(data[at(x, y):at(x, y) + size]) for y in range(abs(height))
              for x in range(width)]
    if size == 4:
        ink = max(pixels, key=lambda p: p[3])
    else:
        ink = min((p for p in pixels if p != b'\xff\x00\xff'), key=sum)
    for x, y in MARKS[name][variant]:
        data[at(x, y):at(x, y) + size] = ink
    open(out, 'wb').write(data)


def main():
    args = sys.argv[1:]
    to = None
    if '--to' in args:
        to = args[args.index('--to') + 1]
        del args[args.index('--to'):args.index('--to') + 2]
    variant = args[0] if args else 'block'
    for name in MARKS:
        out = os.path.join(to, '%s %s.bmp' % (name, variant)) if to \
            else os.path.join(THEME, name + ' Albums.bmp')
        make(name, variant, out)


if __name__ == '__main__':
    main()
