#!/usr/bin/env python3
"""The shadow of the list over the main menu's pane, as on a stock iPod classic
(2.0.5's PreviewArea_Shadow_Image, 26 px): the pane's columns from its left
edge darkened, the most at the edge, by STOCK, measured in its screenshots
(each value of 256 taken off). Classic has it, at stock's strength; Modern
and Neo don't, for now; Dieter's pane is a recess, its wall's shadow its own.
Plain Python, no imaging libraries:

    python3 themes/tools/make_pane_shadow.py

Classic: in the pictures under its pane, the lists' backdrops and the About
Backdrop, light and dark, on both screens, darkened in place, last: after
make_menu_icons.py, make_classic_220x176.py and make_dark_theme.py, and at a
release's carry after those, on that branch's own pictures. A picture already
done is left alone, and make_menu_icons.py and make_classic_220x176.py read the
pane's gradient past it. What is drawn over the pane rather than under it has
its shadow from the firmware (UI_METRIC_PANE_SHADOW): the cover, and Photos'
collage where it isn't the backdrop between its photos."""
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
THEMES = os.path.dirname(HERE)

STOCK = (119, 107, 99, 89, 80, 75, 68, 62, 54, 47, 40, 34, 28, 23, 18, 13, 11, 8,
         6, 4, 2, 1, 0, 0, 0, 0)


def profile(percent):
    """the columns' darkness at that strength, up to the last that has 3 of
    256 or more: the rest, under 1.2 %, isn't seen, and Classic's 20 leave
    the About Backdrop's columns from 180, where its disk's bar is, as they
    are"""
    d = [v * percent // 100 for v in STOCK]
    while d and d[-1] < 3:
        d.pop()
    return d


def band(percent):
    """the pane's columns the shadow is in: Classic's 20, Modern's and Neo's 19"""
    return len(profile(percent))


# the screens: their size, the pane's first column
SCREENS = {"": (320, 240, 160), "220x176": (220, 176, 110)}


def read_bmp(path):
    data = open(path, "rb").read()
    off, = struct.unpack_from("<I", data, 10)
    w, h = struct.unpack_from("<ii", data, 18)
    bpp, = struct.unpack_from("<H", data, 28)
    if bpp != 24:
        raise SystemExit("%s: %d bits, not 24" % (path, bpp))
    stride = (w * 3 + 3) & ~3
    return data, off, w, abs(h), stride, h > 0


def shade_bmp(path, left, percent):
    data, off, w, h, stride, bottom_up = read_bmp(path)
    px = bytearray(data)
    d = profile(percent)

    def at(x, y):
        row = (h - 1 - y) if bottom_up else y
        return off + row * stride + x * 3
    # done already: the pane's first column well under a column past the
    # shadow, along its height
    ratios = []
    for y in range(8, h - 8, 8):
        a, b = sum(px[at(left, y):at(left, y) + 3]), sum(px[at(left + 30, y):at(left + 30, y) + 3])
        if b > 60:
            ratios.append(a / b)
    ratios.sort()
    if ratios and ratios[len(ratios) // 2] < 0.75:
        print("done already:", os.path.relpath(path, THEMES))
        return
    for y in range(h):
        for i, v in enumerate(d):
            p = at(left + i, y)
            for c in range(3):
                px[p + c] = (px[p + c] * (256 - v) + 128) >> 8
    open(path, "wb").write(px)
    print("shaded:", os.path.relpath(path, THEMES))


def theme_dir(theme, screen):
    return os.path.join(THEMES, theme, screen, ".rockbox") if screen else \
        os.path.join(THEMES, theme, ".rockbox")


def main():
    for screen, (w, h, left) in SCREENS.items():
        root = theme_dir("iClassic-Square", screen)
        for name in ("iClassic_bd.bmp", "iClassic_bd_dark.bmp"):
            shade_bmp(os.path.join(root, "backdrops", name), left, 100)
        for look in ("iClassic-Square", "iClassic-Square-Dark"):
            shade_bmp(os.path.join(root, "wps", look, "About Backdrop.bmp"), left, 100)


if __name__ == "__main__":
    main()
