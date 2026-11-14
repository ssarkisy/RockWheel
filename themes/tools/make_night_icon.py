#!/usr/bin/env python3
# Make the moon of the iClassic Square status bar, shown while Night Mode is on:
#
#     themes/tools/make_night_icon.py
#
# Needs Pillow. A crescent, shaded from top to bottom like the hold icon's lock, drawn
# at eight times the size and brought down. A partly covered
# pixel gets the colour of the bar in its row, and an empty one the magenta that skins
# take as transparent. Run make_dark_theme.py after it.
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
THEME = os.path.join(HERE, '..', 'iClassic-Square', '.rockbox', 'wps', 'iClassic-Square')
SIZE = 16
SCALE = 8
TOP = 2  # the row of the bar the icon starts at
BAR_COLUMNS = range(228, 265)
TRANSPARENT = 255, 0, 255
LIGHT, DARK = (0x7a, 0x86, 0x96), (0x19, 0x1a, 0x23)  # the lock's, top and bottom

# the moon: a disc with another taken out of its upper right
CENTRE, RADIUS = (7.6, 7.0), 6.2
CUT_CENTRE, CUT_RADIUS = (11.0, 4.0), 5.2


def covered(x, y):
    inside = (x - CENTRE[0]) ** 2 + (y - CENTRE[1]) ** 2 <= RADIUS ** 2
    cut = (x - CUT_CENTRE[0]) ** 2 + (y - CUT_CENTRE[1]) ** 2 <= CUT_RADIUS ** 2
    return inside and not cut


def main():
    backdrop = Image.open(os.path.join(THEME, 'iClassic_bg.bmp')).convert('RGB')
    image = Image.new('RGB', (SIZE, SIZE), TRANSPARENT)
    top = CENTRE[1] - RADIUS
    for y in range(SIZE):
        bar = [sum(backdrop.getpixel((x, TOP + y))[i] for x in BAR_COLUMNS) / len(BAR_COLUMNS)
               for i in range(3)]
        t = min(max((y + 0.5 - top) / (2 * RADIUS), 0), 1)
        ink = [LIGHT[i] + (DARK[i] - LIGHT[i]) * t for i in range(3)]
        for x in range(SIZE):
            hits = sum(covered(x + (sx + 0.5) / SCALE, y + (sy + 0.5) / SCALE)
                       for sy in range(SCALE) for sx in range(SCALE))
            alpha = hits / SCALE ** 2
            if alpha < 0.02:
                continue
            image.putpixel((x, y), tuple(round(ink[i] * alpha + bar[i] * (1 - alpha))
                                         for i in range(3)))
    image.save(os.path.join(THEME, 'Status Night.bmp'))


main()
