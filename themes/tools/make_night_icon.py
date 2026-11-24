#!/usr/bin/env python3
# Make the moon of the iClassic Square status bar, shown while Night Mode is on:
#
#     themes/tools/make_night_icon.py
#
# after make_classic_header.py, whose Video bar it goes on. Plain Python. A crescent,
# shaded from top to bottom like the hold icon's lock, drawn at eight times the size and
# brought down: each pixel as much of the shade as the moon covers of it, on each
# screen's bar where its skins draw it, the Video's at y 4 and the iPod photo's at y 1,
# in either look, as make_classic_header.py puts the status line's other glyphs
# (status_glyphs()): the dark look's shade its header_pixel(), on the dark bar. An empty
# pixel is the magenta that skins take as transparent.
import make_classic_header as header_tool

SIZE = 16
SCALE = 8
LIGHT, DARK = (0x7a, 0x86, 0x96), (0x19, 0x1a, 0x23)  # the lock's, top and bottom
PLACES = (('Status Night', 4), ('Status Night', 1))  # the Video's and the photo's

# the moon: a disc with another taken out of its upper right
CENTRE, RADIUS = (7.6, 7.0), 6.2
CUT_CENTRE, CUT_RADIUS = (11.0, 4.0), 5.2


def covered(x, y):
    inside = (x - CENTRE[0]) ** 2 + (y - CENTRE[1]) ** 2 <= RADIUS ** 2
    cut = (x - CUT_CENTRE[0]) ** 2 + (y - CUT_CENTRE[1]) ** 2 <= CUT_RADIUS ** 2
    return inside and not cut


def main():
    top = CENTRE[1] - RADIUS
    rows = []
    for y in range(SIZE):
        t = min(max((y + 0.5 - top) / (2 * RADIUS), 0), 1)
        ink = tuple(round(LIGHT[i] + (DARK[i] - LIGHT[i]) * t) for i in range(3))
        rows.append([(ink, sum(covered(x + (sx + 0.5) / SCALE, y + (sy + 0.5) / SCALE)
                               for sy in range(SCALE) for sx in range(SCALE)) / SCALE ** 2)
                     for x in range(SIZE)])
    header_tool.status_glyphs(rows, 1, PLACES)


main()
