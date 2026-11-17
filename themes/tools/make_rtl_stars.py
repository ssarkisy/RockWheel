#!/usr/bin/env python3
# Makes the stars' pictures for a language written right to left: each
# theme's Ratings.bmp and Stars.bmp mirrored, as Ratings RTL.bmp and
# Stars RTL.bmp beside them, in every theme's folders (light and dark, the
# Video's and the iPod photo's). A skin shows them where %Sr says the
# language is one: the stars at the right end of the details' column, set
# from the right. Their frames are stacked from the top, so each row of the
# picture mirrored is each frame's. Run it after what makes the pictures it
# mirrors, make_theme_glyphs.py, make_modern_glyphs.py,
# make_classic_ratings.py, make_classic_220x176.py and make_dark_theme.py:
#
#   themes/tools/make_rtl_stars.py
#
# The header and each row's padding are kept as they are; only the pixels of
# a row change places. With --check it writes nothing: it names each mirror
# that isn't its picture's as made now, and fails if one isn't.

import os
import struct
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
THEMES = (("Neo", ("Neo", "Neo-Dark")),
          ("Modern", ("Modern", "Modern-Dark")),
          ("iClassic-Square", ("iClassic-Square", "iClassic-Square-Dark")))
SIZES = (".rockbox", os.path.join("220x176", ".rockbox"))
PICTURES = ("Ratings", "Stars")


def mirrored(src):
    with open(src, "rb") as f:
        data = bytearray(f.read())
    assert data[:2] == b"BM", src
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bpp, compression = struct.unpack_from("<HI", data, 28)
    assert bpp in (24, 32) and compression in (0, 3), (src, bpp, compression)
    size = bpp // 8
    stride = (width * size + 3) & ~3
    for row in range(abs(height)):
        start = offset + row * stride
        pixels = [data[start + x * size:start + (x + 1) * size] for x in range(width)]
        data[start:start + width * size] = b"".join(reversed(pixels))
    return bytes(data)


def main():
    if sys.argv[1:] not in ([], ["--check"]):
        sys.exit("usage: make_rtl_stars.py [--check]")
    check = sys.argv[1:] == ["--check"]
    stale = 0
    for theme, folders in THEMES:
        for size in SIZES:
            for folder in folders:
                where = os.path.join(ROOT, theme, size, "wps", folder)
                for name in PICTURES:
                    data = mirrored(os.path.join(where, name + ".bmp"))
                    dst = os.path.join(where, name + " RTL.bmp")
                    if not check:
                        with open(dst, "wb") as f:
                            f.write(data)
                        continue
                    try:
                        with open(dst, "rb") as f:
                            same = f.read() == data
                    except OSError:
                        same = False
                    if not same:
                        print("not the mirror of %s.bmp: %s" % (name, os.path.relpath(dst, ROOT)))
                        stale += 1
    if check and stale:
        sys.exit(1)


main()
