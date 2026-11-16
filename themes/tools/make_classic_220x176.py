#!/usr/bin/env python3
# Makes iClassic Square's pictures for the iPod photo's 220x176 screen from the
# light theme's of 320x240, into themes/iClassic-Square/220x176/.rockbox:
#
#   themes/tools/make_classic_220x176.py && themes/tools/make_classic_np.py \
#       && themes/tools/make_dark_theme.py
#
# Nothing is scaled. What is the same on both screens is taken as it is: the
# status bar's rows, cut to the screen's width (its texture repeats every 20
# px, so it goes on unbroken); the glyphs; the progress bar and its trough,
# their two ends with the middle between them shortened. The note of a
# missing cover is drawn anew at the cover's size here, by
# make_menu_icons.py's drawing, its strokes on whole pixels. Then
# make_classic_np.py writes the details' part of the skin and
# make_dark_theme.py the dark theme, as for 320x240. The skins themselves,
# iClassic-Square.sbs and .wps, are edited by hand. There is no pane beside
# the main menu on this screen: no About or Wide Backdrop, no Photos.

import os
import shutil

import make_menu_icons as icons
from make_menu_icons import mix, read_bmp, write_bmp

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "iClassic-Square", ".rockbox")
OUT = os.path.join(HERE, "..", "iClassic-Square", "220x176", ".rockbox")
LIGHT = os.path.join(SRC, "wps", "iClassic-Square")
W, H = 220, 176

# as the skins of this screen have them (220x176/.rockbox/wps/iClassic-Square.wps)
ART = (9, 31, 105, 127)         # the cover's place, 96 px
REFLECTION_ROWS = 14            # %Cr(14,128)
BAR_X, BAR_Y, BAR_W = 40, 143, 140
# the 320x240 theme's trough, its bar's 14 rows and the reflection's under
# them (make_menu_icons.py's TROUGH), and its fill's width
TROUGH = (58, 206, 262, 230)
FILL_W = 204
ART_MASK = (40, 157)            # either side of the album art view's 140 px

GLYPHS = ("Battery", "Playing Status", "Hold Icon", "Status Night", "Status Shuffle Albums",
          "Repeat Icon", "Shuffle Icon", "Ratings", "Stars", "Scrub Knob", "Scrub Hints",
          "Volume Left", "Volume Right")


class Note(icons.Note):
    """make_menu_icons.py's note, its numbers as much smaller as the cover is,
    whole pixels where they are whole there"""
    K = (ART[2] - ART[0]) / (icons.ART[2] - icons.ART[0])
    W, H = round(72 * K), round(86 * K)
    STEM = round(7 * K)
    LEFT_STEM = round(22 * K)
    RIGHT_STEM = W - STEM
    BEAM_LEFT_TOP, BEAM_RIGHT_TOP = round(12 * K), 0.0
    BEAM = 17.0 * K
    HEAD_RX, HEAD_RY = 15.0 * K, 11.0 * K
    HEAD_HALF_W = icons.math.hypot(HEAD_RX * icons.math.cos(icons.Note.HEAD_TILT),
                                   HEAD_RY * icons.math.sin(icons.Note.HEAD_TILT))
    HEAD_MEETS_STEM = (HEAD_RX ** 2 - HEAD_RY ** 2) * icons.math.sin(icons.Note.HEAD_TILT) \
        * icons.math.cos(icons.Note.HEAD_TILT) / HEAD_HALF_W
    HEADS = ((LEFT_STEM + STEM - HEAD_HALF_W, 74.0 * K), (W - HEAD_HALF_W, 71.0 * K))
    SHEEN = (-12.0 * K, -62.0 * K, 108.0 * K)


def crop(rows, columns, top=0, bottom=None):
    return [list(row[columns[0]:columns[1]]) for row in rows[top:bottom]]


def shortened(rows, width):
    """rows of a bar, its ends kept and its middle cut to width"""
    half = width // 2
    return [row[:half] + row[len(row) - (width - half):] for row in rows]


def list_backdrop():
    """the status bar and the lists' white: the wide screen's, cut"""
    return crop(read_bmp(os.path.join(LIGHT, "Wide Backdrop.bmp")), (0, W), 0, H)


def now_playing(screen):
    rows = [list(row) for row in screen]
    # the cover's place without one: the panel's gradient, the note, the
    # cover's reflection under it, as make_menu_icons.py has them
    gradient = icons.panel_gradient()
    art_h = ART[3] - ART[1]
    for y in range(art_h):
        colour = gradient[y * (len(gradient) - 1) // (art_h - 1)]
        rows[ART[1] + y][ART[0]:ART[2]] = [colour] * (ART[2] - ART[0])
    dx, dy = icons.optical_offset(Note)
    radius = icons.SHADOW_RADIUS
    icons.SHADOW_RADIUS = 2         # the shadow as much smaller
    icons.draw(rows, Note, ART[0] + (ART[2] - ART[0] - Note.W) // 2 - dx,
               ART[1] + (art_h - Note.H) // 2 - dy)
    icons.SHADOW_RADIUS = radius
    for k in range(REFLECTION_ROWS):
        t = icons.REFLECTION_ALPHA * (REFLECTION_ROWS - (k + 0.5)) / REFLECTION_ROWS
        for x in range(ART[0], ART[2]):
            rows[ART[3] + k][x] = tuple(round(c) for c in
                                        mix((255, 255, 255), rows[ART[3] - 1 - k][x], t))
    # the empty bar and its reflection
    src = read_bmp(os.path.join(LIGHT, "iClassic_bg.bmp"))
    trough = shortened(crop(src, TROUGH[::2], TROUGH[1], TROUGH[3]), BAR_W)
    for y, row in enumerate(trough):
        rows[BAR_Y + y][BAR_X:BAR_X + BAR_W] = row
    colours = icons.native_colours(rows)
    print("Now Playing's backdrop (220x176): %d colours, of 256 at most" % colours)
    if colours > 256:
        raise SystemExit("too many: Whole Screen would not tint it")
    return rows


def main():
    wps = os.path.join(OUT, "wps", "iClassic-Square")
    os.makedirs(wps, exist_ok=True)
    os.makedirs(os.path.join(OUT, "backdrops"), exist_ok=True)
    screen = list_backdrop()
    write_bmp(os.path.join(OUT, "backdrops", "iClassic_bd.bmp"), screen)
    write_bmp(os.path.join(wps, "iClassic_bg.bmp"), now_playing(screen))
    write_bmp(os.path.join(wps, "PB.bmp"),
              shortened(read_bmp(os.path.join(LIGHT, "PB.bmp")), BAR_W))
    assert len(read_bmp(os.path.join(LIGHT, "PB.bmp"))[0]) == FILL_W
    write_bmp(os.path.join(wps, "Art Mask.bmp"), [[(255, 255, 255)] * ART_MASK[0]] * ART_MASK[1])
    for name in GLYPHS:
        shutil.copy(os.path.join(LIGHT, name + ".bmp"), os.path.join(wps, name + ".bmp"))


if __name__ == "__main__":
    main()
