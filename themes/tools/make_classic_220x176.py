#!/usr/bin/env python3
# Makes iClassic Square's pictures for the iPod photo's 220x176 screen from the
# light theme's of 320x240, into themes/iClassic-Square/220x176/.rockbox:
#
#   themes/tools/make_classic_220x176.py && themes/tools/make_classic_np.py \
#       && themes/tools/make_dark_theme.py && themes/tools/make_rtl_stars.py
#
# Nothing is scaled. What is the same on both screens is taken as it is: the
# status bar's rows, cut to the screen's width (its texture repeats every 20
# px, so it goes on unbroken), three of its gradient's left out for a bar of
# 16 rows over lists of eight rows of 20; the glyphs; the progress bar and its trough,
# their two ends with the middle between them shortened. The note of a
# missing cover is drawn anew at the cover's size here, by
# make_menu_icons.py's drawing, its strokes on whole pixels. Then
# make_classic_np.py writes the details' part of the skin and
# make_dark_theme.py the dark theme, as for 320x240. The skins themselves,
# iClassic-Square.sbs and .wps, are edited by hand. The main menu has a pane
# by the row selected, as on the Video: the music in the lists' backdrop;
# Settings and Extras the About Backdrop, its pane the Video's scaled to the
# pane's height, and Disk Fill at that scale, made from this branch's own
# 320x240 pictures, whichever they are; Photos the collage, or the Photos
# Icon drawn anew at the note's size. The other lists, and the main menu
# with Cover Beside Menu off, have the Wide Backdrop.

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
ART = (4, 31, 100, 127)         # the cover's place, 96 px, at the lists' edge
REFLECTION_ROWS = 14            # %Cr(14,128)
BAR_X, BAR_Y, BAR_W = 40, 143, 140
# the 320x240 theme's trough, its bar's 14 rows and the reflection's under
# them (make_menu_icons.py's TROUGH), and its fill's width
TROUGH = (58, 206, 262, 230)
FILL_W = 204
ART_MASK = (40, 157)            # either side of the album art view's 140 px
# the main menu's pane, from this column to the screen's right; the note,
# Now Playing's, in the middle of this box over the captions, as the skin
# (iClassic-Square.sbs) has them
PANE = 110
PANE_NOTE = (PANE, 24, W, 104)
# the About Backdrop's pane: the Video's 160x240 at 176/240, its middle 110
# columns of the 117 that gives; its trough for the disk's use, at
# (180, 174) on the Video, 128 by 11, so at (121, 128), 94 by 8 here, which
# the skin's %nu(11,128,94,8,Disk Fill.bmp) has
ABOUT_SCALE = H / 240
ABOUT_SRC_W = round((W - PANE) / ABOUT_SCALE)
ABOUT_SRC_X = 160 + (160 - ABOUT_SRC_W) // 2
DISK_FILL = (round(128 * ABOUT_SCALE), round(11 * ABOUT_SCALE))

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


class Picture(icons.Picture):
    """make_menu_icons.py's picture of photos, as much smaller as the note"""
    K = Note.K
    W, H = round(icons.Picture.W * K), round(icons.Picture.H * K)
    SHEEN = tuple(v * Note.K for v in icons.Picture.SHEEN)

    @classmethod
    def inside(cls, x, y):
        return icons.Picture.inside(x / cls.K, y / cls.K)


def resample(rows, x0, y0, w, h, nw, nh):
    """rows[y0:y0+h] by [x0:x0+w] as nw by nh, each pixel the average of what
    it covers"""
    sx, sy = w / nw, h / nh
    out = []
    for j in range(nh):
        a, b = y0 + j * sy, y0 + (j + 1) * sy
        row = []
        for i in range(nw):
            c, d = x0 + i * sx, x0 + (i + 1) * sx
            acc, total = [0.0, 0.0, 0.0], 0.0
            y = int(a)
            while y < b:
                wy = min(b, y + 1) - max(a, y)
                x = int(c)
                while x < d:
                    k = (min(d, x + 1) - max(c, x)) * wy
                    p = rows[y][x]
                    acc = [acc[n] + p[n] * k for n in range(3)]
                    total += k
                    x += 1
                y += 1
            row.append(tuple(int(v / total + 0.5) for v in acc))
        out.append(row)
    return out


def crop(rows, columns, top=0, bottom=None):
    return [list(row[columns[0]:columns[1]]) for row in rows[top:bottom]]


def shortened(rows, width):
    """rows of a bar, its ends kept and its middle cut to width"""
    half = width // 2
    return [row[:half] + row[len(row) - (width - half):] for row in rows]


# the status bar: 15 rows of the Video's 18 of gradient, taken evenly from
# its first to its last, then its dark line; the lists' 160 rows under it,
# five of 32 or eight of 20 (apps/gui/ui_theme.c), from row 16
BAR_ROWS = 15
VIDEO_BAR_ROWS = 18


def list_backdrop():
    """the status bar and the lists' white: the wide screen's, cut"""
    src = read_bmp(os.path.join(LIGHT, "Wide Backdrop.bmp"))
    bar = [src[round(y * (VIDEO_BAR_ROWS - 1) / (BAR_ROWS - 1))]
           for y in range(BAR_ROWS)] + [src[VIDEO_BAR_ROWS]]
    rows = bar + src[VIDEO_BAR_ROWS + 1:VIDEO_BAR_ROWS + 1 + H - len(bar)]
    return crop(rows, (0, W), 0, H)


def pane_backdrop(screen):
    """the lists' backdrop with the main menu's pane: the Video's panel's
    gradient from the top to the foot, and on it the note where the cover
    goes without one, as make_menu_icons.py draws them there"""
    rows = [list(row) for row in screen]
    gradient = icons.panel_gradient()
    for y in range(H):
        rows[y][PANE:W] = [gradient[y * (len(gradient) - 1) // (H - 1)]] * (W - PANE)
    dx, dy = icons.optical_offset(Note)
    radius = icons.SHADOW_RADIUS
    icons.SHADOW_RADIUS = 2         # the shadow as much smaller
    left, top, right, bottom = PANE_NOTE
    icons.draw(rows, Note, left + (right - left - Note.W) // 2 - dx,
               top + (bottom - top - Note.H) // 2 - dy)
    icons.SHADOW_RADIUS = radius
    return rows


def about_backdrop(screen):
    """Settings' and Extras' pane, as on the Video: its About Backdrop's pane
    at the pane's height, its middle columns, beside the lists' backdrop"""
    src = read_bmp(os.path.join(LIGHT, "About Backdrop.bmp"))
    pane = resample(src, ABOUT_SRC_X, 0, ABOUT_SRC_W, 240, W - PANE, H)
    return [list(row[:PANE]) + pane[y] for y, row in enumerate(screen)]


def photos_icon():
    """Photos' picture when there are none, over the note in the pane: on the
    pane's gradient, dithered as the backdrop under it is"""
    gradient = icons.panel_gradient()
    rows = [[gradient[y * (len(gradient) - 1) // (H - 1)]] * W for y in range(H)]
    left, top, right, bottom = PANE_NOTE
    radius = icons.SHADOW_RADIUS
    icons.SHADOW_RADIUS = 2         # the shadow as much smaller, as the note's
    icons.draw(rows, Picture, left + (right - left - Picture.W) // 2,
               top + (bottom - top - Picture.H) // 2)
    icons.SHADOW_RADIUS = radius
    return [row[left:right] for row in rows[top:bottom]]


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
    write_bmp(os.path.join(OUT, "backdrops", "iClassic_bd.bmp"), pane_backdrop(screen))
    write_bmp(os.path.join(wps, "Wide Backdrop.bmp"), screen)
    write_bmp(os.path.join(wps, "iClassic_bg.bmp"), now_playing(screen))
    write_bmp(os.path.join(wps, "PB.bmp"),
              shortened(read_bmp(os.path.join(LIGHT, "PB.bmp")), BAR_W))
    assert len(read_bmp(os.path.join(LIGHT, "PB.bmp"))[0]) == FILL_W
    write_bmp(os.path.join(wps, "Art Mask.bmp"), [[(255, 255, 255)] * ART_MASK[0]] * ART_MASK[1])
    write_bmp(os.path.join(wps, "About Backdrop.bmp"), about_backdrop(screen))
    fill = read_bmp(os.path.join(LIGHT, "Disk Fill.bmp"))
    write_bmp(os.path.join(wps, "Disk Fill.bmp"),
              resample(fill, 0, 0, len(fill[0]), len(fill), *DISK_FILL))
    icons.write_patch(os.path.join(wps, "Photos Icon.bmp"), photos_icon(), PANE_NOTE)
    for name in GLYPHS:
        shutil.copy(os.path.join(LIGHT, name + ".bmp"), os.path.join(wps, name + ".bmp"))


if __name__ == "__main__":
    main()
