#!/usr/bin/env python3
# Makes Dieter, light and dark, on both screens, from the approved mockups'
# rules (warm greys lit from the top, one small signal colour, circles, dot
# grilles and scales with ticks):
#
#   python3 themes/tools/make_dieter_theme.py
#
# It writes, from the tables and drawings below and nothing else:
#
# - themes/Dieter/.rockbox/wps/Dieter.sbs, Dieter.wps, Dieter-Dark.sbs,
#   Dieter-Dark.wps, and the same in themes/Dieter/220x176/.rockbox/wps for
#   the iPod photo's screen;
# - their pictures in wps/Dieter/ and wps/Dieter-Dark/, and the backdrops in
#   backdrops/Dieter.bmp and Dieter-Dark.bmp;
# - apps/gui/dieter_glyphs.h: what the firmware draws itself in Dieter, the
#   list's marks, the album's tile, Search's keys and the USB screen's sign.
#
# Every picture is made as the screen shows it: each pixel one of RGB565's
# colours, the gradients dithered at their places on the screen by the table
# tools/buildzip.pl dithers a backdrop with, so that a picture laid on the
# backdrop or on another picture goes on without a seam. Opaque pictures are
# 16-bit BMPs, which buildzip.pl copies as they are; those with alpha 32-bit,
# their colours already the screen's. Light comes from the top left.
#
# A light and a dark skin load the same fonts, the same cover sizes and
# pictures of the same sizes, so that Dark Theme changes while music plays.
# The colours must match Dieter's entry in apps/gui/ui_theme.c (LOOKS here).
# Run with Python 3 and no other packages; it needs the themes' fonts in
# themes/fonts/.rockbox/fonts for the places of the texts it lays out.

import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
FONTS = os.path.join(ROOT, "themes", "fonts", ".rockbox", "fonts")
THEME = os.path.join(ROOT, "themes", "Dieter")
HEADER = os.path.join(ROOT, "apps", "gui", "dieter_glyphs.h")

# --- colours -------------------------------------------------------------------
# By look. The flat roles are RGB565's colours, as the firmware's table has
# them (apps/gui/ui_theme.c); the material's are those the pictures are made
# of, dithered. GROUND the screen; PANE the pane's floor; INK names, titles,
# glyphs and the played part of the scale; SECOND second lines, counts,
# times, the chevron, major ticks and the needle while paused; QUIET minor
# ticks; RULE hairlines; BAND the selected row. The ground is lit from the
# top (G_TOP to G_FOOT), the status line a strip (STRIP) with a hairline
# (HAIR) and a lit lip (LIP) under it; the pane is a recess (P_TOP to P_FOOT)
# with its left wall's shadow (P_SHADE); the grille's punched holes have a
# dark near wall (HOLE_DEEP) and a lit far rim (HOLE_RIM); the scale lies in
# a window (W_IN, its top wall W_SHADE, its lit lip W_RIM) with ticks
# (T_MAJOR, T_MINOR) on a line (T_BASE).
LOOKS = {
    "Dieter": dict(
        GROUND="F7F3EF", PANE="E7E3DE", INK="211C18", SECOND="6B6963",
        QUIET="A5A29C", RULE="CECBC6", BAND="D6D3CE",
        G_TOP="FAF7F3", G_FOOT="EEEAE5", STRIP="EDEAE5", HAIR="C3C0BA",
        LIP="FFFFFF", P_TOP="E7E3DE", P_FOOT="E1DDD8", P_SHADE="C9C5BF",
        HOLE="A29E97", HOLE_DEEP="7C7872", HOLE_RIM="FFFFFF",
        W_IN="EDE9E3", W_SHADE="CCC8C2", W_RIM="FFFFFF",
        T_MAJOR="6B6963", T_MINOR="A5A29C", T_BASE="C6C2BC", PLAYED="211C18",
        # the rating's keys, Quick Scroll's key and Search's keys' faces
        KEY="E4E1DC", QS_KEY="E2DFDA", TW_FACE="F5F2EB", DARK=False),
    "Dieter-Dark": dict(
        GROUND="181818", PANE="262626", INK="EFEFEF", SECOND="9C9A9C",
        QUIET="636163", RULE="393839", BAND="313031",
        G_TOP="1F1F1F", G_FOOT="141414", STRIP="242424", HAIR="060606",
        LIP="2E2E2E", P_TOP="272727", P_FOOT="222222", P_SHADE="101010",
        HOLE="0A0A0A", HOLE_DEEP="000000", HOLE_RIM="555555",
        W_IN="101010", W_SHADE="030303", W_RIM="2E2E2E",
        T_MAJOR="9C9A9C", T_MINOR="5A5A5A", T_BASE="3A3A3A", PLAYED="E0E0E0",
        KEY="3C3C3C", QS_KEY="6A6A6A", TW_FACE="1C1C1C", DARK=True),
}
# the signal colours, light and dark, in the order of enum ui_signal_colour
# (apps/gui/ui_theme.h): the only colour of the chrome, on the cursor, the
# needle, Search's caret and chosen key, Cover Flow's track and the lit lamps
SIGNALS = (("Orange", "E75921", "EF6929"), ("Red", "D62821", "EF4539"),
           ("Yellow", "AD8208", "F7C331"), ("Green", "299A42", "4AC363"))
# buildzip.pl's ordered dither of a backdrop
DITHER = (0, 192, 48, 240, 12, 204, 60, 252, 3, 195, 51, 243, 15, 207, 63, 255)


def rgb(h):
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))


def q565(c):
    """the colour's nearest RGB565 one, expanded as the screen shows it"""
    r, g, b = (min(255, max(0, int(round(v)))) for v in c)
    r5 = (31 * r + (r >> 3) + 127) >> 8
    g6 = (63 * g + (g >> 2) + 127) >> 8
    b5 = (31 * b + (b >> 3) + 127) >> 8
    return ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))


def hex565(c):
    return "%02X%02X%02X" % q565(c)


def mix(c1, c2, pct):
    """the firmware's mix_color(): c1 moved pct percent of the way to c2"""
    return tuple(c1[i] + int((c2[i] - c1[i]) * pct / 100) for i in range(3))


def lerp(c1, c2, t):
    return tuple(c1[i] + (c2[i] - c1[i]) * t for i in range(3))


def palette(look):
    p = {}
    for k, v in LOOKS[look].items():
        p[k] = rgb(v) if isinstance(v, str) else v
    for k in ("GROUND", "PANE", "INK", "SECOND", "QUIET", "RULE", "BAND", "KEY",
              "QS_KEY", "TW_FACE"):
        p[k] = q565(p[k])
    dark = p["DARK"]
    p["SIGNALS"] = [q565(rgb(s[2 if dark else 1])) for s in SIGNALS]
    return p


# --- a raster ----------------------------------------------------------------------
class Canvas:
    """colours as floats with coverage, each pixel's alpha too, so that a
    picture can be opaque on a ground or alpha over the screen"""

    def __init__(self, w, h, ground=None):
        self.w, self.h = w, h
        self.px = [[list(ground) if ground else [0.0, 0.0, 0.0] for _ in range(w)]
                   for _ in range(h)]
        self.a = [[1.0 if ground else 0.0] * w for _ in range(h)]

    def put(self, x, y, c, a=1.0):
        if a <= 0 or not (0 <= x < self.w and 0 <= y < self.h):
            return
        a = min(a, 1.0)
        p, old = self.px[y][x], self.a[y][x]
        out = a + old * (1 - a)
        if out <= 0:
            return
        for i in range(3):
            p[i] = (c[i] * a + p[i] * old * (1 - a)) / out
        self.a[y][x] = out

    def rect(self, x0, y0, x1, y1, c):
        for y in range(max(0, int(y0)), min(self.h, int(y1))):
            for x in range(max(0, int(x0)), min(self.w, int(x1))):
                self.px[y][x] = list(c)
                self.a[y][x] = 1.0

    def vgrad(self, x0, y0, x1, y1, top, foot, rows=None):
        """top row to foot row; rows, the gradient's own rows if it goes on
        past the rect (a picture of a part of it)"""
        y_start, n = (y0, max(1, y1 - y0 - 1)) if rows is None else rows
        for y in range(max(0, y0), min(self.h, y1)):
            t = (y - y_start) / n
            self.rect(x0, y, x1, y + 1, lerp(top, foot, t))

    def shape(self, inside, box, c, ss=6):
        x0, y0, x1, y1 = (int(math.floor(v)) for v in box)
        offs = [(i + 0.5) / ss for i in range(ss)]
        for y in range(max(0, y0), min(self.h, y1 + 1)):
            for x in range(max(0, x0), min(self.w, x1 + 1)):
                k = sum(1 for oy in offs for ox in offs if inside(x + ox, y + oy))
                if k:
                    self.put(x, y, c(x + 0.5, y + 0.5) if callable(c) else c, k / (ss * ss))

    def circle(self, cx, cy, r, c):
        self.shape(lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r,
                   (cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1), c)

    def ring(self, cx, cy, r, w, c):
        self.shape(lambda x, y: (r - w) ** 2 <= (x - cx) ** 2 + (y - cy) ** 2 <= r * r,
                   (cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1), c)

    def rrect(self, x0, y0, x1, y1, r, c):
        def inside(x, y):
            if not (x0 <= x < x1 and y0 <= y < y1):
                return False
            dx = max(x0 + r - x, 0, x - (x1 - r))
            dy = max(y0 + r - y, 0, y - (y1 - r))
            return dx * dx + dy * dy <= r * r
        self.shape(inside, (x0 - 1, y0 - 1, x1, y1), c)

    def poly(self, pts, c):
        def inside(x, y):
            j, ins = len(pts) - 1, False
            for i in range(len(pts)):
                xi, yi = pts[i]
                xj, yj = pts[j]
                if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                    ins = not ins
                j = i
            return ins
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        self.shape(inside, (min(xs) - 1, min(ys) - 1, max(xs) + 1, max(ys) + 1), c)

    def stroke(self, pts, w, c):
        segs = list(zip(pts, pts[1:]))
        h = w / 2

        def inside(x, y):
            for (ax, ay), (bx, by) in segs:
                dx, dy = bx - ax, by - ay
                n = dx * dx + dy * dy
                t = 0 if n == 0 else max(0, min(1, ((x - ax) * dx + (y - ay) * dy) / n))
                px, py = ax + t * dx - x, ay + t * dy - y
                if px * px + py * py <= h * h:
                    return True
            return False
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        self.shape(inside, (min(xs) - h - 1, min(ys) - h - 1, max(xs) + h + 1,
                            max(ys) + h + 1), c)

    def shadow(self, dist, box, strength, blur):
        """a soft shadow: dist(x, y), the distance outside the shape moved"""
        x0, y0, x1, y1 = (int(math.floor(v)) for v in box)
        for y in range(max(0, y0), min(self.h, y1 + 1)):
            for x in range(max(0, x0), min(self.w, x1 + 1)):
                d = dist(x + 0.5, y + 0.5)
                if d < blur:
                    a = strength * min(1.0, max(0.0, 1 - max(d, 0) / blur)) ** 2
                    if a > 0.004:
                        self.put(x, y, (0, 0, 0), a)

    def crop(self, x0, y0, w, h):
        out = Canvas(w, h)
        for y in range(h):
            for x in range(w):
                out.px[y][x] = list(self.px[y0 + y][x0 + x])
                out.a[y][x] = self.a[y0 + y][x0 + x]
        return out

    def screen(self, ox, oy, dither=True):
        """rows of RGB565 codes as the screen shows the picture at (ox, oy):
        dithered by buildzip.pl's table at the screen's places, or rounded"""
        out = []
        for y, row in enumerate(self.px):
            dy = DITHER[(y + oy) & 15] & 0xAA
            codes = []
            for x, (r, g, b) in enumerate(row):
                r, g, b = (min(255, max(0, int(v + 0.5))) for v in (r, g, b))
                # a colour the screen has already stays as it is, which
                # the dither doesn't always leave it
                d = DITHER[(x + ox) & 15] ^ dy if dither and q565((r, g, b)) != (r, g, b) else 127
                codes.append((((31 * r + (r >> 3) + d) >> 8) << 11)
                             | (((63 * g + (g >> 2) + d) >> 8) << 5)
                             | ((31 * b + (b >> 3) + d) >> 8))
            out.append(codes)
        return out


def expand(code):
    r, g, b = code >> 11, (code >> 5) & 63, code & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def write_bmp16(path, cv, ox, oy, rows=None):
    """an opaque picture, dithered at its place on the screen: 16-bit
    RGB565, top-down, as buildzip.pl writes the ones it converts; or rows of
    RGB565 codes made already"""
    rows = rows if rows is not None else cv.screen(ox, oy)
    w, h = len(rows[0]), len(rows)
    data = b"".join(struct.pack("<%dH" % w, *row) + (b"\0\0" if w & 1 else b"")
                    for row in rows)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(struct.pack("<2sIHHI", b"BM", 66 + len(data), 0, 0, 66))
        f.write(struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 16, 3, len(data), 0, 0, 0, 0))
        f.write(struct.pack("<III", 0xF800, 0x07E0, 0x001F))
        f.write(data)


def write_bmp32(path, cv, ox=0, oy=0, dither=False):
    """a picture with alpha: 32-bit, each colour already RGB565's, which the
    firmware's loader keeps (apps/recorder/bmp.c rounds)"""
    rows = cv.screen(ox, oy, dither)
    data = b""
    for y in reversed(range(cv.h)):
        for x in range(cv.w):
            r, g, b = expand(rows[y][x])
            a = int(round(cv.a[y][x] * 255))
            data += bytes((b, g, r, a)) if a else b"\0\0\0\0"
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"BM" + struct.pack("<IHHI", 54 + len(data), 0, 0, 54))
        f.write(struct.pack("<IiiHHIIiiII", 40, cv.w, cv.h, 1, 32, 0, len(data), 2835, 2835, 0, 0))
        f.write(data)


# --- the themes' fonts, read as the firmware reads them ------------------------------
class Font:
    _cache = {}

    @classmethod
    def get(cls, name):
        if name not in cls._cache:
            cls._cache[name] = cls(os.path.join(FONTS, name + ".fnt"))
        return cls._cache[name]

    def __init__(self, path):
        b = open(path, "rb").read()
        if b[:4] != b"RB12":
            raise ValueError("not a font of Rockbox's (RB12): %s" % path)
        self.maxwidth, self.height, self.ascent, self.depth = struct.unpack_from("<4H", b, 4)
        self.first, self.default, self.size, nbits, noff, nwid = struct.unpack_from("<6I", b, 12)
        long_offsets = nbits >= 0xFFDB
        pos = 36 + nbits
        pos = (pos + 3) & ~3 if long_offsets else (pos + 1) & ~1
        self.offsets = struct.unpack_from("<%d%s" % (noff, "I" if long_offsets else "H"), b, pos)
        pos += noff * (4 if long_offsets else 2)
        self.widths = b[pos:pos + nwid] if nwid else bytes([self.maxwidth]) * self.size
        self.bits = b[36:36 + nbits]

    def glyph(self, ch):
        i = ord(ch) - self.first
        if not 0 <= i < self.size or self.widths[i] == 0:
            i = self.default - self.first
        w = self.widths[i]
        raw = self.bits[self.offsets[i]:self.offsets[i] + (self.height * w + 1) // 2]
        nib = []
        for byte in raw:
            nib += [byte & 15, byte >> 4]
        return w, [[(15 - n) / 15 for n in nib[y * w:(y + 1) * w]] for y in range(self.height)]

    def width(self, text):
        """text's width as the firmware's getstringsize() gives it: its
        glyphs' advances, the default glyph's for one the font hasn't"""
        total = 0
        for ch in text:
            i = ord(ch) - self.first
            if not 0 <= i < self.size or self.widths[i] == 0:
                i = self.default - self.first
            total += self.widths[i]
        return total

    def ink(self, text):
        """the rows of the cell its ink takes, at least a quarter drawn"""
        rows = [y for ch in text for y, row in enumerate(self.glyph(ch)[1])
                if row and max(row) > 0.25]
        return min(rows), max(rows)

    def cap(self):
        rows = [y for y, row in enumerate(self.glyph("H")[1]) if row and max(row) > 0.5]
        return rows[0], rows[-1]

    def text_top(self, y0, height):
        """the cell's top for its capitals in the middle of the rows"""
        ct, cb = self.cap()
        return int(round(y0 + height / 2 - (ct + cb + 1) / 2))


# --- the screens' layouts --------------------------------------------------------------
# The Video's, the mockups' grid: one margin of 16 px; the status line's
# clock and glyphs at the lists' text inset (SM = UI_METRIC_TEXT_INSET in
# apps/gui/ui_theme.c), so that the clock starts where the menu's text does;
# the status line's 20 rows, its hairline and lip under them, the lists
# from 24, rows of 27 and of 36 with covers of 32, so that 8 or 6 fill them
# (the owner's rule: equal rows, a whole number of them); the main menu's
# pane from column 160; Now Playing's cover of 144 at (16, 28), its details
# from 176, the scale's window across the margin under it, the times under
# the window.
VIDEO = dict(
    NAME="video", W=320, H=240, OUT=THEME, M=16, SM=4, STATUS=20, TOP=24, ROW=27,
    PANE=160, BATT=(20, 10), GAP=6, PCT_GAP=3, PLAY_H=9,
    # the clock's width, the title's place and width; Now Playing's title's
    CLOCK=(64, 84, 152), NP_TITLE=(100, 120),
    # Now Playing's but its modes 5 px lower than the grid's (the modes'
    # glyphs room over the title), from under the times
    COVER=144, COVER_Y=33, COL=176,
    WIN=(16, 187, 304, 211), TICK=12, MAJOR=72, BASE_UP=6, MAJOR_H=7, TIMES_Y=215,
    GRILLE=(54, 8.5, 2.0), RING=78, PANE_GAP=18, LINE2=18, DIAL_MINOR=5,
    QS=72, KEY_R=11.5, KEY_PITCH=40, LAMP_R=3.2, SMALL_R=2.6, SMALL_BEZ=1.2, SMALL_PITCH=10.5,
    LAMPS_H=12,
    KNOB_R=5, NO_COVER_GRILLE=(0.36, 0.059, 0.0139),
    F_ROW="19-Inter-SemiBold", F_SUB="15-Inter-Medium", F_STAT="15-Inter-SemiBold",
    F_TITLE="18-Inter-SemiBold", F_NPSUB="15-Inter-Medium", F_SMALL="14-Inter-Medium",
    F_LYRICS="17-Inter-Medium-13",
    ARTIST_GAP=3, PLACE_GAP=7, RATING_GAP=7, FORMAT_GAP=5,
    # the big cover in the middle of the rows under the status line
    MODES_Y=25, BIG=200, BIG_X=60, BIG_Y=30, LYRICS_Y=33, SPEED_H=44, TITLE_LINES=2,
    # the list marks the firmware draws (dieter_glyphs.h)
    RING_R=3.25, CHEV=(5, 9), ROW_COVER=32, HEADER_COVER=52,
    TW_R=14, USB_R=30, USB_W=4,
)
# The iPod photo's, by the same rules on its screen: a margin of 14 px, the
# status line at the lists' text inset as the Video's; the status line's 16
# rows, the lists from 16 (over its
# hairline and lip); rows
# of 20, of two lines the firmware's 32 with covers of 29 (LINE_COVER_SIZE),
# so that 8 or 5 fill them; the pane from 110, as the port's. Now Playing
# at the lists' edge (NP_M: the screen is small, the margins took the
# text's room): its cover of 96 at (4, 30), its details from 108 to 4 px
# from the right, the scale's window and the times as wide.
PHOTO = dict(
    NAME="photo", W=220, H=176, OUT=os.path.join(THEME, "220x176"), M=14, SM=4, STATUS=16,
    TOP=16, ROW=20, PANE=110, BATT=(17, 8), GAP=5, PCT_GAP=2, PLAY_H=8,
    # the clock short of the title's viewport: right to left its text ends
    # at its viewport's end, which the title's drew over
    CLOCK=(42, 58, 104), NP_TITLE=(62, 96),
    NP_M=4, COVER=96, COVER_Y=30, COL=108,
    # the cover lower and the scale's window under it low and narrow, the
    # times at the screen's foot, so that the tallest details stand under
    # the modes, not under their glyphs
    WIN=(4, 141, 216, 157), TICK=8, MAJOR=48, BASE_UP=5, MAJOR_H=5, TIMES_Y=160,
    GRILLE=(40, 7.0, 1.7), RING=63, PANE_GAP=12, LINE2=15, DIAL_MINOR=4.5,
    QS=56, KEY_R=8.5, KEY_PITCH=28, LAMP_R=2.4, SMALL_R=2.0, SMALL_BEZ=0.9, SMALL_PITCH=8,
    LAMPS_H=10,
    KNOB_R=4, NO_COVER_GRILLE=(0.36, 0.059, 0.0139),
    F_ROW="15-Inter-SemiBold", F_SUB="14-Inter-Medium", F_STAT="14-Inter-SemiBold",
    F_TITLE="15-Inter-SemiBold", F_NPSUB="14-Inter-Medium", F_SMALL="14-Inter-Medium",
    F_LYRICS="14-Inter-Medium",
    ARTIST_GAP=2, PLACE_GAP=3, RATING_GAP=2, FORMAT_GAP=2,
    # the title on one line, scrolling, as the column is narrow
    MODES_Y=20, BIG=140, BIG_X=40, BIG_Y=26, LYRICS_Y=22, SPEED_H=40, TITLE_LINES=1,
    RING_R=3.0, CHEV=(4, 7), ROW_COVER=29, HEADER_COVER=38,
    TW_R=13, USB_R=22, USB_W=3,
)
LAYOUTS = (VIDEO, PHOTO)
L = VIDEO


def use(layout):
    global L
    L = layout


# --- the pictures' parts ----------------------------------------------------------------
def hole(cv, x, y, r, p, flat=False):
    """a punched hole lit from the top left: its far rim catches the light,
    its near wall is in shadow; flat, one colour"""
    if not flat:
        cv.circle(x + 0.7, y + 0.7, r, p["HOLE_RIM"])
    cv.circle(x, y, r, p["HOLE"])
    if not flat:
        cv.shape(lambda px, py: (px - x) ** 2 + (py - y) ** 2 <= r * r
                 and (px - (x + 0.7)) ** 2 + (py - (y + 0.7)) ** 2 > (r * 0.95) ** 2,
                 (x - r - 1, y - r - 1, x + r + 1, y + r + 1), p["HOLE_DEEP"])


def ring_grille(cv, cx, cy, r, pitch, hr, p, flat=False):
    """holes on concentric circles, about pitch apart along each"""
    hole(cv, cx, cy, hr, p, flat)
    k = 1
    while k * pitch <= r - hr:
        rk = k * pitch
        n = max(6, round(2 * math.pi * rk / pitch))
        off = (math.pi / n) if k % 2 else 0.0
        for i in range(n):
            a = off + 2 * math.pi * i / n
            hole(cv, cx + rk * math.cos(a), cy + rk * math.sin(a), hr, p, flat)
        k += 1


def grille_ink(r, pitch, hr):
    return int((r - hr) // pitch) * pitch + hr


def backdrop(p, wide):
    """the lit ground, the status line's strip with its hairline and lip;
    and but where a screen takes the whole width, the main menu's pane,
    from the screen's top: a skin's text over a picture has the backdrop
    for its ground, so the pane's floor, under its texts, is the backdrop's"""
    W, X = L["W"], L["W"] if wide else L["PANE"]
    cv = Canvas(W, L["H"])
    cv.vgrad(0, 0, W, L["H"], p["G_TOP"], p["G_FOOT"])
    cv.rect(0, 0, X, L["STATUS"], p["STRIP"])
    cv.rect(0, L["STATUS"], X, L["STATUS"] + 1, p["HAIR"])
    cv.rect(0, L["STATUS"] + 1, X, L["STATUS"] + 2, p["LIP"])
    if not wide:
        pn = pane(p)
        for y in range(L["H"]):
            for x in range(pn.w):
                cv.px[y][X + x] = list(pn.px[y][x])
    return cv


def floor(p):
    """the pane's floor: one colour, the mockups' P_TOP, whose fall to P_FOOT
    is a level of RGB565 or two over its height; so the grille's and the
    dial's pictures, cut from it, lie on the backdrop's without a seam"""
    return q565(p["P_TOP"])


def wall(p):
    """the pane's left wall's shadow on the floor, its two columns"""
    return [q565(lerp(p["P_SHADE"], floor(p), i / 2)) for i in range(2)]


def pane(p):
    """the floor with its wall's shadow, as a picture to cut the grille's and
    the dial's from"""
    w, h = L["W"] - L["PANE"], L["H"]
    cv = Canvas(w, h, floor(p))
    for i, c in enumerate(wall(p)):
        cv.rect(i, 0, i + 1, h, c)
    return cv


def pane_shadow():
    """the wall's shadow over a cover in the pane"""
    cv = Canvas(2, L["H"])
    for i in range(2):
        for y in range(L["H"]):
            cv.put(i, y, (0, 0, 0), 0.35 * (1 - i / 2))
    return cv


def region():
    """the pane's height under the status line, where its group stands"""
    return L["TOP"], L["H"]


def pane_cx():
    return L["PANE"] + (L["W"] - L["PANE"]) / 2


def grille_place():
    """the grille and the count under it, one group in the middle of the
    pane: the grille's centre, and the count's cell's top"""
    r, pitch, hr = L["GRILLE"]
    ink = grille_ink(r, pitch, hr)
    t, b = Font.get(L["F_SUB"]).ink("31 Songs")
    top0, bot = region()
    group = 2 * ink + 1 + L["PANE_GAP"] + (b - t + 1)
    top = top0 + (bot - top0 - group) / 2
    cy = int(round(top + ink))
    return cy, int(round(cy + ink + 1 + L["PANE_GAP"] - t))


def grille_picture(p):
    """the grille on the pane's floor, cut to its holes: (picture, x, y)"""
    r, pitch, hr = L["GRILLE"]
    cy, _ = grille_place()
    cx = pane_cx()
    full = pane(p)
    X = L["PANE"]
    ring_grille(full, cx - X, cy, r, pitch, hr, p)
    ink = grille_ink(r, pitch, hr)
    x0, y0 = int(math.floor(cx - ink - 2)), int(math.floor(cy - ink - 2))
    x1, y1 = int(math.ceil(cx + ink + 3)), int(math.ceil(cy + ink + 3))
    return full.crop(x0 - X, y0, x1 - x0, y1 - y0), x0, y0


def dial_place():
    """Settings' disk: the ring of the firmware's %nR (L['RING'] across) in
    a printed scale of 60 ticks, and two lines under it, one group in the
    middle of the pane: its centre, on a pixel's corner, and the lines' cells"""
    r = L["RING"] / 2
    tick = r + 8 if L is VIDEO else r + 7
    f1 = Font.get(L["F_ROW"] if L is VIDEO else L["F_SUB"])
    t1, _ = f1.ink("116 GB Free")
    _, b2 = Font.get(L["F_SUB"]).ink("128 GB")
    top0, bot = region()
    group = 2 * tick + L["PANE_GAP"] + (L["LINE2"] + b2 + 1 - t1)
    top = top0 + (bot - top0 - group) / 2
    cy = int(round(top + tick))
    y1 = int(round(cy + tick + L["PANE_GAP"] - t1))
    return cy, r, tick, y1, y1 + L["LINE2"]


def collage_place():
    """Photos' collage, its cells as photos_panel.c lays them (2 px in, 3
    apart), and its count under it, one group in the middle of the pane: the
    collage's top and the count's cell's top"""
    if L is VIDEO:
        gh = 4 * 41 + 3 * 3
    else:
        gh = 4 * 30 + 3 * 3
    t, b = Font.get(L["F_SUB"]).ink("12 Photos")
    top0, bot = region()
    group = gh + L["PANE_GAP"] + (b - t + 1)
    top = int(round(top0 + (bot - top0 - group) / 2))
    return top - 2, top + gh + L["PANE_GAP"] - t


# --- Now Playing's pictures --------------------------------------------------------------
def window_canvas(p, x0, x1):
    """the scale behind glass in its recess, from column x0 to x1 of the
    screen (the window's own columns, the ground beside it): its top and
    left walls, the ticks on their line; the lit lip under it is a hairline
    of its own (the skin's)"""
    wx0, wy0, wx1, wy1 = L["WIN"]
    cv = Canvas(x1 - x0, wy1 - wy0)
    cv.vgrad(0, 0, x1 - x0, wy1 - wy0, p["G_TOP"], p["G_FOOT"], rows=(-wy0, L["H"] - 1))
    ox = -x0
    cv.rect(wx0 + ox, 0, wx1 + ox, wy1 - wy0, p["W_IN"])
    for i in range(2):
        cv.rect(wx0 + ox, i, wx1 + ox, i + 1, lerp(p["W_SHADE"], p["W_IN"], i / 2))
    for i in range(2):
        for y in range(wy1 - wy0):
            cv.put(wx0 + ox + i, y, p["W_SHADE"], 0.6 - 0.3 * i)
    sx0, sx1, base = scale()
    tick = L["TICK"]
    n = (sx1 - sx0) // tick
    for i in range(n + 1):
        x = sx0 + i * tick + ox
        major = (i * tick) % L["MAJOR"] == 0
        h = L["MAJOR_H"] if major else 3
        cv.rect(x, base - h - wy0, x + 1, base - wy0, p["T_MAJOR"] if major else p["T_MINOR"])
    cv.rect(sx0 + ox, base - wy0, sx1 + 1 + ox, base + 1 - wy0, p["T_BASE"])
    return cv


def scale():
    """the scale's first and last ticks' columns and its line's row"""
    wx0, wy0, wx1, wy1 = L["WIN"]
    tick = L["TICK"]
    n = (wx1 - wx0 - 8) // tick
    sx0 = wx0 + (wx1 - wx0 - n * tick) // 2
    return sx0, sx0 + n * tick, wy1 - L["BASE_UP"]


# The bar's box: a slider of SLIDER_W columns not centred on its end, the
# needle in its middle two, goes the box's width less its own
# (draw_progressbar()); the box from the slider's left edge with the needle
# on the scale's first tick (the needle's columns the tick's and the one
# before it) to its right edge with the needle on the last. The fill goes on
# under the slider to its middle (fillunder), so that the ink meets the
# needle. Its rows the window's.
SLIDER_W = 15
NEEDLE_AT = 6       # the needle's first column in the slider


def bar_box():
    sx0, sx1, _ = scale()
    wx0, wy0, wx1, wy1 = L["WIN"]
    x = sx0 - 1 - NEEDLE_AT
    return x, wy0, (sx1 - sx0) + SLIDER_W, wy1 - wy0


def window_pictures(p):
    """the bar's backdrop and its fill: the window, and the window with the
    played part in ink on the scale's line, from the first tick"""
    x, y, w, h = bar_box()
    track = window_canvas(p, x, x + w)
    fill = window_canvas(p, x, x + w)
    sx0, sx1, base = scale()
    fill.rect(sx0 - x, base - y, sx1 + 1 - x, base + 2 - y, p["PLAYED"])
    return track, fill


def slider(p, colour, knob):
    """the needle in the slider's middle columns: two columns of the colour
    over a faint shadow right of them; scrubbing, a round knob on its top
    with a speck of light, as a tuning knob in the hand"""
    _, wy0, _, wy1 = L["WIN"]
    cv = Canvas(SLIDER_W, wy1 - wy0)
    x = NEEDLE_AT + 1           # the mockup's needle at x: its columns x - 1, x
    for yy in range(2, wy1 - wy0 - 1):
        cv.put(x + 1, yy, (0, 0, 0), 0.18)
    cv.rect(x - 1, 2, x + 1, wy1 - wy0 - 1, colour)
    if knob:
        r = L["KNOB_R"]
        cv.circle(x + 0.5, r + 1.5, r + 1, mix(p["W_IN"], (0, 0, 0), 30))
        cv.circle(x, r + 1, r, colour)
        cv.circle(x - r * 0.35, r * 0.65 + 1, r * 0.35, mix(colour, (255, 255, 255), 50))
    return cv


# The cover behind glass in its window (the owner's pick, glass b): the
# window's top and left walls' shade on the cover, a dark rim round it, a lit
# lip outside its foot and right side; over the glass a soft diagonal glare
# from the top left. In sixteenths, as the firmware draws it over a cover
# (cover_glass(), skin_display.c) and this script over the empty window
GLASS_DEPTH = 5


GLASS_EDGE = 2      # the lit edge inside the bottom and right walls, its rows
GLASS_BAND = 3      # the band all round inside the rim, its rows


def glass_tables(dark):
    """the walls' shade by the row or column in, the rim's, the lip's; and the
    glare by u = (x / w + y / h) / 2 in 256ths: in sixteenths. The glass over
    the cover: the walls' soft shade, a frame of a darker rim and a band
    inside it, the lip outside brighter, and a lit edge inside the bottom and
    right walls"""
    sh = 0.55 if dark else 0.42
    wall = [int(round(16 * sh * max(0.0, 1 - d / GLASS_DEPTH) ** 1.6)) for d in range(GLASS_DEPTH)]
    rim = int(round(16 * (0.60 if dark else 0.45)))
    lip = 0.32 if dark else 0.75
    peak = 0.09 if dark else 0.11
    glare = []
    for k in range(256):
        u = (k + 0.5) / 256
        band = math.exp(-((u - 0.30) / 0.11) ** 2)
        sheen = 0.35 * max(0.0, 1 - u / 0.5)
        glare.append(int(round(16 * peak * min(1.0, band + sheen))))
    return wall, rim, lip, glare


def glass_frame(dark):
    """the lit edge's light by the row or column in from the bottom or right
    wall, and the band's shade: in sixteenths"""
    lit = [int(round(16 * v)) for v in ((0.18, 0.08) if dark else (0.28, 0.12))]
    return lit, int(round(16 * 0.12)) if dark else int(round(16 * 0.10))


def glass_over(cv, x0, y0, w, h, dark, left=True, rims=True):
    """the glass over a canvas's rect, as cover_glass() draws it; rims: with
    the window's left wall, rim and frame (GLASS_WINDOW)"""
    wall, rim, _, glare = glass_tables(dark)
    lit, band = glass_frame(dark)
    for y in range(h):
        for x in range(w):
            up, in_right = h - 1 - y, w - 1 - x
            a = wall[y] if y < GLASS_DEPTH else 0
            if left and x < GLASS_DEPTH:
                a = max(a, wall[x])
            if rims and (y == h - 1 or x == w - 1 or (left and x == 0) or y == 0):
                a = max(a, rim)
            if rims and min(x, y, up, in_right) < GLASS_BAND:
                a = max(a, band)
            u = ((x * 256 // w) + (y * 256 // h)) >> 1
            lv = glare[u]
            if rims and up < GLASS_EDGE:
                lv = max(lv, lit[up])
            if rims and in_right < GLASS_EDGE and up >= in_right:
                lv = max(lv, lit[in_right])
            px = cv.px[y0 + y][x0 + x]
            for i in range(3):
                v = px[i] * (16 - a) / 16
                px[i] = v + (255 - v) * lv / 16


def lip_colour(p, y):
    """the lit lip outside the window, on the ground at row y"""
    ground = lerp(p["G_TOP"], p["G_FOOT"], y / (L["H"] - 1))
    return lerp(ground, (255, 255, 255), glass_tables(p["DARK"])[2])


def no_cover(p):
    """the cover's place without one: the grille punched in the screen's own
    ground, with no glass, window or lip (no speaker behind glass), as
    wide as the cover and its lip; the ground the backdrop's at
    the rows it stands on"""
    C = L["COVER"]
    cv = Canvas(C + 1, C + 1)
    y0 = L["COVER_Y"]
    for yy in range(C + 1):
        cv.rect(0, yy, C + 1, yy + 1, lerp(p["G_TOP"], p["G_FOOT"], (y0 + yy) / (L["H"] - 1)))
    a, b, c = L["NO_COVER_GRILLE"]
    ring_grille(cv, C / 2, C / 2, C * a, C * b, C * c, p)
    return cv


def np_margin():
    """Now Playing's margin: the iPod photo's its own, the lists' edge"""
    return L.get("NP_M", L["M"])


def big_no_cover_place():
    """the big cover's place without one: no_cover()'s picture in the middle
    of the big cover's, at a whole number of the dither's 4 rows and columns
    from its own place so that its ground meets the backdrop's"""
    C, big = L["COVER"], L["BIG"]
    x = L["BIG_X"] + (big - C) // 2
    y = L["BIG_Y"] + (big - C) // 2
    x -= (x - np_margin()) % 4
    y -= (y - L["COVER_Y"]) % 4
    return x, y


# the rating's push keys, each in its hole, its lamp lit in the signal colour
# or a dark lens; and the small line's lamps
LIGHT = (-0.55, -0.83)


def dome(base, nx, ny, hi=0.30, lo=0.32, convex=True):
    d = LIGHT[0] * nx + LIGHT[1] * ny
    if not convex:
        d = -d
    if d >= 0:
        return lerp(base, (255, 255, 255), hi * d ** 1.2)
    return lerp(base, (0, 0, 0), lo * (-d) ** 1.1)


def round_key(cv, cx, cy, r, base, drop=(1.0, 2.0), drop_s=0.30, rim=True, spec=True,
              shadow=True):
    """a convex round key: a soft drop shadow unless shadow is False, the
    dome lit from the top left, a lit rim there, a soft speck of light"""
    if shadow:
        cv.shadow(lambda x, y: math.hypot(x - cx - drop[0], y - cy - drop[1]) - r,
                  (cx - r - 6, cy - r - 6, cx + r + 7, cy + r + 8), drop_s, 4.0)

    def col(x, y):
        nx, ny = (x - cx) / r, (y - cy) / r
        rr = min(1.0, math.hypot(nx, ny))
        c = dome(base, nx * rr, ny * rr)
        if rim and rr > 0.88:
            t = (LIGHT[0] * nx + LIGHT[1] * ny) / max(rr, 1e-6)
            c = lerp(c, (255, 255, 255), 0.30 * t * (rr - 0.88) / 0.12) if t > 0 else \
                lerp(c, (0, 0, 0), 0.18 * (-t) * (rr - 0.88) / 0.12)
        if spec:
            sx, sy = nx + 0.38, ny + 0.48
            c = lerp(c, (255, 255, 255), 0.16 * math.exp(-(sx * sx + sy * sy) / 0.06))
        return c
    cv.shape(lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r, (cx - r - 1, cy - r - 1,
             cx + r + 1, cy + r + 1), col, ss=4)


def lamp(cv, cx, cy, r, colour, on, ground):
    """lit: the colour with a bright core and a faint glow; dark: a smoked
    lens with a speck"""
    if on:
        for k, a in ((2.2, 0.10), (1.2, 0.18)):
            cv.shape(lambda x, y, k=k: (x - cx) ** 2 + (y - cy) ** 2 <= (r + k) ** 2,
                     (cx - r - 3, cy - r - 3, cx + r + 3, cy + r + 3),
                     lerp(ground, colour, a / 0.18 * 0.35), ss=4)
        cv.shape(lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r,
                 (cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1),
                 lambda x, y: lerp(colour, (255, 255, 255), 0.55 * math.exp(
                     -(((x - cx + r * 0.25) / r) ** 2 + ((y - cy + r * 0.3) / r) ** 2) / 0.25)),
                 ss=4)
    else:
        lens = mix(ground, (0, 0, 0), 30)
        cv.shape(lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r,
                 (cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1),
                 lambda x, y: lerp(lens, (0, 0, 0), 0.25 * max(0, (y - cy) / r)), ss=4)
        cv.circle(cx - r * 0.35, cy - r * 0.4, max(0.6, r * 0.28), lerp(lens, (255, 255, 255), 0.45))


def key_place():
    """the rating's keys: their row's middle, where the scale's window and
    the times are in the other views, and their centres' columns"""
    top, bot = (180, 238) if L is VIDEO else (124, 174)
    cy = (top + bot) / 2
    return cy, [L["W"] / 2 + (i - 2) * L["KEY_PITCH"] for i in range(5)]


KEY_BOX = 2         # how far the key's picture reaches past its hole


def rating_key(p, lit=None):
    """a key in its hole, with alpha over the ground, its lamp a dark lens
    or lit in a colour; its middle on a pixel's corner, half its picture's
    side from its top left"""
    r = L["KEY_R"]
    half = int(math.ceil(r + 2.2 + 4))
    cv = Canvas(2 * half, 2 * half)
    c = half
    dark = p["DARK"]
    ground = q565(lerp(p["G_TOP"], p["G_FOOT"], key_place()[0] / (L["H"] - 1)))
    cv.circle(c + 0.4, c + 0.6, r + 2.2, mix(ground, (255, 255, 255), 8 if dark else 50))
    cv.circle(c, c, r + 2.0, mix(ground, (0, 0, 0), 55 if dark else 22))
    round_key(cv, c, c, r, p["KEY"], drop=(0.6, 1.4), drop_s=0.6 if dark else 0.35)
    lamp(cv, c, c, L["LAMP_R"], lit or (0, 0, 0), lit is not None, p["KEY"])
    return cv, half


# Now Playing's volume as a knob in the cover's place (%nK): its body masks
# by look, drawn by the firmware in the signal colour (deeper in the dark
# look, less lit, so that it doesn't glow), its ticks and pointer drawn there
KNOB = 0.30         # the knob's radius, of the cover's side
KNOB_LOOKS = {False: dict(body=0, hi=0.30, spec=0.16, drop=0.35, pointer=0),
              True: dict(body=22, hi=0.18, spec=0.08, drop=0.55, pointer=25)}


def knob_masks(dark):
    """the knob's disc, its dark (the drop shadow, the dome's shade, the
    flat top's ring) and its light (the dome's, the lit rim, the speck):
    each a pixel's alpha, the knob's middle on a pixel's corner half the
    box from its top and left"""
    k = KNOB_LOOKS[dark]
    r = L["COVER"] * KNOB
    half = int(math.ceil(r + 4))
    c = half
    disc = Canvas(2 * half, 2 * half)
    disc.shape(lambda x, y: (x - c) ** 2 + (y - c) ** 2 <= r * r,
               (c - r - 1, c - r - 1, c + r + 1, c + r + 1), (0, 0, 0), ss=4)
    shade = Canvas(2 * half, 2 * half)
    shade.shadow(lambda x, y: math.hypot(x - c - 0.8, y - c - 1.6) - r,
                 (0, 0, 2 * half - 1, 2 * half - 1), k["drop"], 4.0)
    for yy in range(2 * half):          # under the disc, its own shade only
        for xx in range(2 * half):
            shade.a[yy][xx] *= 1 - disc.a[yy][xx]
    light = Canvas(2 * half, 2 * half)
    for yy in range(2 * half):
        for xx in range(2 * half):
            cover = disc.a[yy][xx]
            if cover <= 0:
                continue
            nx, ny = (xx + 0.5 - c) / r, (yy + 0.5 - c) / r
            rr = min(1.0, math.hypot(nx, ny))
            d = (LIGHT[0] * nx + LIGHT[1] * ny)
            dark_a = 0.32 * (-d) ** 1.1 if d < 0 else 0.0
            light_a = k["hi"] * d ** 1.2 if d > 0 else 0.0
            if rr > 0.88:
                t = d / max(rr, 1e-6)
                if t > 0:
                    light_a = max(light_a, light_a + 0.30 * t * (rr - 0.88) / 0.12)
                else:
                    dark_a = dark_a + 0.18 * (-t) * (rr - 0.88) / 0.12
            sx, sy = nx + 0.38, ny + 0.48
            light_a += k["spec"] * math.exp(-(sx * sx + sy * sy) / 0.06)
            if 0.72 <= rr <= 0.80:          # the flat top's edge
                dark_a += 0.6 * 0.18
            shade.put(xx, yy, (0, 0, 0), min(1.0, dark_a) * cover)
            light.put(xx, yy, (255, 255, 255), min(1.0, light_a) * cover)
    return disc, shade, light, half


def knob_geometry():
    """in sixteenths of a pixel: the knob's radius; its ticks' inner and
    outer radii (major, minor) and widths; the pointer's from and to and width"""
    r = L["COVER"] * KNOB
    s = 1.0 if L is VIDEO else 0.75
    t0, t1 = r + 5 * s, r + 12 * s
    return dict(R=r, TICK_IN=t0, TICK_OUT=t1, TICK_MINOR=t0 + (t1 - t0) * 0.6,
                TICK_W=1.2, TICK_W_MINOR=1.0, POINT_IN=0.30 * r, POINT_OUT=0.86 * r,
                POINT_W=max(1.6, r * 0.12))


# Settings' On/Off: a small round push key in its hole, its lamp in its face
# (the owner's pick, key A): On the key latched in, a little lower and less
# lit, its lamp lit in the signal colour (the firmware's masks); Off the key
# out, its lamp a dark lens. The iPod photo's larger, in the middle of its
# Settings' rows of two lines
SWITCH_R = {"video": 7.0, "photo": 7.25}
SWITCH_LAMP_R = {"video": 2.9, "photo": 3.05}


def switch_key(p, on):
    """the key's picture, its middle on a pixel's corner at the box's middle"""
    dark = p["DARK"]
    s = L["NAME"]
    r, lr = SWITCH_R[s], SWITCH_LAMP_R[s]
    half = int(math.ceil(r + 2.4 + 2))
    cv = Canvas(2 * half, 2 * half)
    c = half
    hole_alpha(cv, c, c, r + 2.0, dark)
    strong = globals()["dome"]

    def stronger(base, nx, ny, hi=0.42, lo=0.42, convex=True):
        return strong(base, nx, ny, hi, lo, convex)
    globals()["dome"] = stronger
    try:
        if on:
            round_key(cv, c + 0.3, c + 0.4, r, mix(p["KEY"], (0, 0, 0), 8 if dark else 5),
                      drop=(0.2, 0.4), drop_s=0.45 if dark else 0.22)
        else:
            round_key(cv, c, c, r, p["KEY"], drop=(0.5, 1.1), drop_s=0.6 if dark else 0.35)
            lamp(cv, c, c, lr, (0, 0, 0), False, p["KEY"])
    finally:
        globals()["dome"] = strong
    return cv, half


def switch_lamp_masks():
    """the lit lamp, in the latched key's face: the disc and its glow, and
    its bright core, each a mask in the key's box"""
    s = L["NAME"]
    r, lr = SWITCH_R[s], SWITCH_LAMP_R[s]
    half = int(math.ceil(r + 2.4 + 2))
    cx, cy = half + 0.3, half + 0.4
    glow, core = Canvas(2 * half, 2 * half), Canvas(2 * half, 2 * half)
    for k, a in ((2.2, 0.19), (1.2, 0.35)):
        g = Canvas(2 * half, 2 * half)
        g.shape(lambda x, y, k=k: (x - cx) ** 2 + (y - cy) ** 2 <= (lr + k) ** 2,
                (0, 0, 2 * half, 2 * half), (0, 0, 0), ss=4)
        for yy in range(2 * half):
            for xx in range(2 * half):
                if g.a[yy][xx] > 0:
                    glow.put(xx, yy, (0, 0, 0), g.a[yy][xx] * a)
    glow.shape(lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= lr * lr, (0, 0, 2 * half, 2 * half),
               (0, 0, 0), ss=4)
    core.shape(lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= lr * lr, (0, 0, 2 * half, 2 * half),
               (255, 255, 255), ss=4)
    for yy in range(2 * half):
        for xx in range(2 * half):
            gx, gy = (xx + 0.5 - cx + lr * 0.25) / lr, (yy + 0.5 - cy + lr * 0.3) / lr
            core.a[yy][xx] *= 0.55 * math.exp(-(gx * gx + gy * gy) / 0.25)
    return glow, core


def small_lamps(p):
    """the small rating line's lamps, each a lens in its bezel as the On/Off
    key's lamp, over the ground: frames of the dark one, a smoked lens with a
    speck, then lit in each signal colour, the disc with its glow and bright
    core (the rating keys' way: a skin's pictures blend over the backdrop,
    so one can't be drawn over another). Their middles half the side from
    the picture's top and left"""
    r, bez = L["SMALL_R"], L["SMALL_BEZ"]
    size = small_lamp_size()
    c = size / 2
    frames = []
    for colour in [None] + p["SIGNALS"]:
        cv = Canvas(size, size)
        hole_alpha(cv, c, c, r + bez, p["DARK"])
        if colour is None:
            lamp(cv, c, c, r, (0, 0, 0), False, mix(p["GROUND"], (0, 0, 0), 20))
        else:
            for k, a in ((2.2, 0.19), (1.2, 0.35)):
                g = Canvas(size, size)
                g.shape(lambda x, y, k=k: (x - c) ** 2 + (y - c) ** 2 <= (r + k) ** 2,
                        (0, 0, size, size), colour, ss=4)
                for yy in range(size):
                    for xx in range(size):
                        if g.a[yy][xx] > 0:
                            cv.put(xx, yy, colour, g.a[yy][xx] * a)
            cv.shape(lambda x, y: (x - c) ** 2 + (y - c) ** 2 <= r * r, (0, 0, size, size),
                     lambda x, y: lerp(colour, (255, 255, 255), 0.55 * math.exp(
                         -(((x - c + r * 0.25) / r) ** 2 + ((y - c + r * 0.3) / r) ** 2) / 0.25)),
                     ss=4)
        frames.append(cv)
    strip = Canvas(size, size * len(frames))
    for f, cv in enumerate(frames):
        for yy in range(size):
            strip.px[f * size + yy] = [list(v) for v in cv.px[yy]]
            strip.a[f * size + yy] = list(cv.a[yy])
    return strip, size


def small_lamp_size():
    return int(math.ceil(2 * (L["SMALL_R"] + 2.2))) + 2


def hole_alpha(cv, cx, cy, r, dark):
    """a round hole in the panel, in alpha so that it lies on any ground: its
    lit lip at the lower right, its dark wall"""
    for (ox, oy, rr, colour, a) in ((0.4, 0.6, r + 0.2, (255, 255, 255), 0.10 if dark else 0.55),
                                    (0, 0, r, (0, 0, 0), 0.55 if dark else 0.25)):
        g = Canvas(cv.w, cv.h)
        g.shape(lambda x, y: (x - cx - ox) ** 2 + (y - cy - oy) ** 2 <= rr * rr,
                (cx - rr - 2, cy - rr - 2, cx + rr + 2, cy + rr + 2), colour, ss=6)
        for yy in range(cv.h):
            for xx in range(cv.w):
                if g.a[yy][xx] > 0:
                    cv.put(xx, yy, colour, g.a[yy][xx] * a)


def qs_key(p):
    """Quick Scroll's key, flat (the owner's word: no dome, no bevel), its
    face of one colour with a hairline a little darker round it for its
    edge; the firmware engraves the letter into it (line.c, the face's
    darker floor, a shade at its top left, a lit lip at its lower right),
    and stretches it by its middle column into a capsule for a wider text.
    Its centre half the picture's width from its top and its left, on a
    pixel's corner"""
    s = L["QS"]
    r = s / 2
    cv = Canvas(s + 2, s + 2)
    c = cv.w // 2
    face = p["QS_KEY"]
    cv.circle(c, c, r, mix(face, (0, 0, 0), 14 if p["DARK"] else 9))
    cv.circle(c, c, r - 1, face)
    return cv


# --- status glyphs -------------------------------------------------------------------------
# Each frame GLYPH_H rows, its middle on the row corner GLYPH_MID: the status
# line's middle is a row corner too, so its viewport's top is that less GLYPH_MID
GLYPH_H, GLYPH_MID = 10, 5


def battery_frames(p):
    """charging (two frames in turn), charged, then its level of twenty: an
    outline with round corners, a fill to its level, a nub of a column; the
    inside is the strip's"""
    w, h = L["BATT"]
    body = w - 2
    levels = [None, None, None] + list(range(21))
    cv = Canvas(body + 1, GLYPH_H * len(levels))
    for i, level in enumerate(levels):
        y0 = i * GLYPH_H + GLYPH_MID - h / 2
        cv.rrect(0, y0, body, y0 + h, 2, p["INK"])
        inner = Canvas(body, GLYPH_H)
        inner.rrect(1, GLYPH_MID - h / 2 + 1, body - 1, GLYPH_MID + h / 2 - 1, 1, (0, 0, 0))
        for yy in range(GLYPH_H):
            for xx in range(body):
                if inner.a[yy][xx] > 0:
                    cv.a[i * GLYPH_H + yy][xx] *= 1 - inner.a[yy][xx]
        cv.rect(body, i * GLYPH_H + GLYPH_MID - 2, body + 1, i * GLYPH_H + GLYPH_MID + 2, p["INK"])
        if i == 0:              # charging: a bolt in the empty case
            cx, top, foot = body / 2, y0 + 1.5, y0 + h - 1.5
            mid = i * GLYPH_H + GLYPH_MID
            cv.poly([(cx + 1.5, top), (cx - 2.5, mid + 0.5), (cx - 0.2, mid + 0.5),
                     (cx - 1.5, foot), (cx + 2.5, mid - 0.5), (cx + 0.2, mid - 0.5)], p["INK"])
        elif i in (1, 2):       # charging's second frame, and charged: full
            cv.rect(2, int(y0 + 2), body - 2, int(y0 + h - 2), p["INK"])
        else:
            cv.rect(2, int(y0 + 2), 2 + round((body - 4) * level / 20), int(y0 + h - 2), p["INK"])
    return cv, len(levels)


def play_frames(p):
    """play, pause, forward, rewind, at the frame's right"""
    hh = L["PLAY_H"]
    w = 7 if L is VIDEO else 6
    fw = 9
    cv = Canvas(fw, GLYPH_H * 4)
    cy = GLYPH_MID
    cv.poly([(fw - w, cy - hh / 2), (fw, cy), (fw - w, cy + hh / 2)], p["INK"])
    y = GLYPH_H
    cv.rect(fw - 7, y + int(cy - hh / 2), fw - 5, y + int(cy + hh / 2), p["INK"])
    cv.rect(fw - 2, y + int(cy - hh / 2), fw, y + int(cy + hh / 2), p["INK"])
    for k, sign in ((2, 1), (3, -1)):
        y = k * GLYPH_H + cy
        for off in (0, 4.5):
            if sign > 0:
                cv.poly([(off, y - 3.5), (off + 4.5, y), (off, y + 3.5)], p["INK"])
            else:
                cv.poly([(fw - off, y - 3.5), (fw - off - 4.5, y), (fw - off, y + 3.5)], p["INK"])
    return cv


def padlock(p):
    """in the play glyph's place while Hold is on, at its right"""
    w = 8 if L is VIDEO else 7
    fw = 9
    cv = Canvas(fw, GLYPH_H)
    cy = GLYPH_MID
    x0 = fw - w
    cv.ring(x0 + w / 2, cy - 1.5, w / 2 - 1, 1.4, p["INK"])
    cv.rect(x0, int(cy - 1), fw, int(cy + 5), p["INK"])
    return cv


def moon(p):
    """Night Mode's: a crescent"""
    s = 10 if L is VIDEO else 9
    cv = Canvas(s, GLYPH_H)
    r = s / 2 - 0.5
    cx, cy = s / 2, GLYPH_MID
    cv.shape(lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r
             and (x - cx - 3.2) ** 2 + (y - cy + 1.8) ** 2 > (r * 0.85) ** 2,
             (0, 0, s, GLYPH_H), p["INK"])
    return cv


NEO = os.path.join(ROOT, "themes", "Neo")


def mode_frames(p, name):
    """Now Playing's modes: Neo's glyphs (the owner's pick), its pictures'
    alpha in Dieter's ink: repeat all, one, shuffle (the repeat mode of that
    name), A-B; shuffle by songs and by albums. Frames of 12 by 11, as
    Dieter's own were, on both screens"""
    path = os.path.join(NEO, "" if L is VIDEO else "220x176", ".rockbox", "wps", "Neo",
                        name + ".bmp")
    with open(path, "rb") as f:
        b = f.read()
    off = struct.unpack_from("<I", b, 10)[0]
    w, h = struct.unpack_from("<ii", b, 18)
    if struct.unpack_from("<H", b, 28)[0] != 32:
        raise ValueError("not a 32-bit picture: %s" % path)
    rows = abs(h)
    cv = Canvas(w, rows)
    for y in range(rows):
        src = rows - 1 - y if h > 0 else y
        for x in range(w):
            a = b[off + (src * w + x) * 4 + 3] / 255
            if a > 0:
                cv.put(x, y, p["INK"], a)
    return cv, (4 if name == "Repeat" else 2)


# --- the list's marks, Search's keys and the USB sign, for the firmware ---------------------
def alpha_table(cv):
    """Rockbox' alpha format: 4 bits a pixel, 0 opaque to 15 none, low nibble
    first, rows run on"""
    nib = [15 - int(round(cv.a[y][x] * 15)) for y in range(cv.h) for x in range(cv.w)]
    if len(nib) & 1:
        nib.append(15)
    return bytes(nib[i] | nib[i + 1] << 4 for i in range(0, len(nib), 2))


def bitmap_alpha_table(cv):
    """the same, a bitmap's: its rows each an even number of pixels"""
    out = b""
    for y in range(cv.h):
        nib = [15 - int(round(cv.a[y][x] * 15)) for x in range(cv.w)]
        if len(nib) & 1:
            nib.append(15)
        out += bytes(nib[i] | nib[i + 1] << 4 for i in range(0, len(nib), 2))
    return out


def mask(w, h, draw):
    cv = Canvas(w, h)
    draw(cv)
    return cv


def chevron_mask():
    cw, ch = L["CHEV"]
    w, h = cw + 1, ch + 1
    cy = h / 2
    right = w
    return mask(w, h, lambda cv: cv.stroke([(right - cw, cy - ch / 2 + 0.5), (right - 0.75, cy),
                                            (right - cw, cy + ch / 2 - 0.5)], 1.5, (0, 0, 0)))


def speaker_mask():
    s = 1.0 if L is VIDEO else 0.8
    w, h = int(round(15 * s)), int(round(13 * s))
    cy = h / 2

    def draw(cv):
        x0 = 0.5
        cv.rect(int(x0), int(cy - 2 * s), int(x0 + 3 * s), int(cy + 2 * s), (0, 0, 0))
        cv.poly([(x0 + 2 * s, cy - 2 * s), (x0 + 6 * s, cy - 5.5 * s), (x0 + 6 * s, cy + 5.5 * s),
                 (x0 + 2 * s, cy + 2 * s)], (0, 0, 0))
        a = [(x0 + 8.5 * s + 3.2 * s * math.cos(t), cy + 3.2 * s * math.sin(t))
             for t in [i * 0.2 - 0.9 for i in range(10)]]
        cv.stroke(a, 1.2, (0, 0, 0))
        b = [(x0 + 8.5 * s + 6 * s * math.cos(t), cy + 6 * s * math.sin(t))
             for t in [i * 0.15 - 0.85 for i in range(12)]]
        cv.stroke(b, 1.2, (0, 0, 0))
    return mask(w, h, draw)


def ring_mask(r, half=False):
    n = int(math.ceil(r)) + 1

    def draw(cv):
        if half:
            cv.shape(lambda x, y: (x - n) ** 2 + (y - n) ** 2 <= r * r and x >= n,
                     (0, 0, 2 * n, 2 * n), (0, 0, 0))
        cv.ring(n, n, r, 1.25, (0, 0, 0))
    return mask(2 * n, 2 * n, draw)


def tile_mask(size):
    """a missing cover's tile: a small grille in one colour on the pane's"""
    flat = {"HOLE": (0, 0, 0)}
    return mask(size, size, lambda cv: ring_grille(cv, size / 2, size / 2, size * 0.36,
                                                    size * 0.17, size * 0.045, flat, True))


def dial_ticks():
    """the dial's 60 printed ticks round the firmware's ring (%nR), as a
    quarter of them: rows down and columns right of the centre, a pixel's
    cover in sixteenths, 16 more for a major tick; the others are its mirror
    images (draw_disk_ring())"""
    _, r, tick, _, _ = dial_place()
    q = int(math.ceil(tick)) + 2
    cv_major, cv_minor = Canvas(q, q), Canvas(q, q)
    for i in range(16):         # 0 to 90 degrees from the top, clockwise
        a = -math.pi / 2 + 2 * math.pi * i / 60
        major = i % 15 == 0
        r0, r1 = r + 3, (tick if major else r + L["DIAL_MINOR"])
        pts = [(r0 * math.cos(a), r0 * math.sin(a) + q), (r1 * math.cos(a), r1 * math.sin(a) + q)]
        # drawn in a frame whose row q is the centre's, mirrored down below
        (cv_major if major else cv_minor).stroke(pts, 1.0 if major else 0.9, (0, 0, 0))
    table = []
    for y in range(q):          # rows down from the centre: the frame's q - 1 - y up
        for x in range(q):
            row = q - 1 - y
            a_major = cv_major.a[row][x] if 0 <= row < q else 0
            a_minor = cv_minor.a[row][x] if 0 <= row < q else 0
            if a_major >= a_minor:
                table.append(min(15, int(round(a_major * 15))) | (16 if a_major > 0 else 0))
            else:
                table.append(min(15, int(round(a_minor * 15))))
    return q, table


def tw_key(p, R):
    """Search's typewriter key: a thin metal ring, bright at the top left and
    dark at the lower right, round a slightly dished face, a soft shadow
    under it; its centre on a pixel's corner at (R + 2, R + 2)"""
    n = int(R) + 2
    size = 2 * n + 2
    cv = Canvas(size, size)
    cx = cy = n
    dark = p["DARK"]
    cv.shadow(lambda x, y: math.hypot(x - cx - 0.5, y - cy - 1.2) - R,
              (cx - R - 3, cy - R - 3, cx + R + 4, cy + R + 4), 0.6 if dark else 0.30, 2.0)
    metal, rw = (176, 176, 178), 1.6

    def ring(x, y):
        nx, ny = (x - cx) / R, (y - cy) / R
        rr = max(math.hypot(nx, ny), 1e-6)
        t = (LIGHT[0] * nx + LIGHT[1] * ny) / rr
        return lerp(metal, (255, 255, 255), 0.75 * t) if t > 0 else \
            lerp(metal, (40, 40, 42), 0.75 * -t)
    cv.shape(lambda x, y: (R - rw) ** 2 <= (x - cx) ** 2 + (y - cy) ** 2 <= R * R,
             (cx - R - 1, cy - R - 1, cx + R + 1, cy + R + 1), ring, ss=4)
    face = p["TW_FACE"]
    cv.shape(lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 < (R - rw) ** 2,
             (cx - R, cy - R, cx + R, cy + R),
             lambda x, y: dome(face, (x - cx) / R, (y - cy) / R, 0.08 if dark else 0.10,
                               0.10 if dark else 0.08, convex=False), ss=4)
    return cv, n


def tw_ring_mask(R):
    n = int(R) + 2
    size = 2 * n + 2
    return mask(size, size, lambda cv: cv.ring(n, n, R, 1.6, (0, 0, 0)))


def usb_mask():
    """the sync sign: two arcs, each ending in an arrowhead"""
    r, w = L["USB_R"], L["USB_W"]
    s = 7 if L is VIDEO else 5
    n = r + s + 2
    size = 2 * n

    def draw(cv):
        cx = cy = n
        for a0 in (0.25, math.pi + 0.25):
            pts = [(cx + r * math.cos(a0 + t * 0.1), cy + r * math.sin(a0 + t * 0.1))
                   for t in range(26)]
            cv.stroke(pts, w, (0, 0, 0))
            ex, ey = pts[-1]
            a = a0 + 2.6
            tx, ty = -math.sin(a), math.cos(a)
            nx, ny = math.cos(a), math.sin(a)
            cv.poly([(ex + tx * s, ey + ty * s), (ex + nx * s, ey + ny * s),
                     (ex - nx * s, ey - ny * s)], (0, 0, 0))
    return mask(size, size, draw)


def c_table(name, data, per_line=12):
    lines = ["static const unsigned char %s[] = {" % name]
    for i in range(0, len(data), per_line):
        lines.append("    " + " ".join("0x%02x," % b for b in data[i:i + per_line]))
    lines.append("};")
    return "\n".join(lines)


def c_pixels(name, codes, per_line=8):
    lines = ["static const unsigned short %s[] = {" % name]
    flat = [c for row in codes for c in row]
    for i in range(0, len(flat), per_line):
        lines.append("    " + " ".join("0x%04x," % c for c in flat[i:i + per_line]))
    lines.append("};")
    return "\n".join(lines)


def c_key(name, codes, alpha, per_line=8):
    lines = ["static const struct dieter_key %s = {" % name, "  {"]
    flat = [c for row in codes for c in row]
    for i in range(0, len(flat), per_line):
        lines.append("    " + " ".join("0x%04x," % c for c in flat[i:i + per_line]))
    lines.append("  },")
    lines.append("  {")
    for i in range(0, len(alpha), 12):
        lines.append("    " + " ".join("0x%02x," % b for b in alpha[i:i + 12]))
    lines.append("  },")
    lines.append("};")
    return "\n".join(lines)


def header():
    """apps/gui/dieter_glyphs.h, both screens'"""
    out = ["/* Made by themes/tools/make_dieter_theme.py: what the firmware draws",
           "   itself in Dieter, both screens' (the iPod photo's under UI_220X176);",
           "   made anew when the script changes, not edited. Masks in Rockbox' alpha",
           "   format (4 bits, 0 opaque to 15 none); a key's pixels RGB565, its",
           "   alpha after them as a bitmap's, rows of an even count. */",
           "#ifndef _DIETER_GLYPHS_H_", "#define _DIETER_GLYPHS_H_"]
    out.append("/* the glass over a cover (cover_glass()): by look (light, dark), in")
    out.append("   sixteenths, the window's walls' shade by the row or column in, the")
    out.append("   rim's, the frame's (a lit edge and a band), and the glare by")
    out.append("   u = (x / w + y / h) / 2 in 256ths; the last u the glare lights */")
    out.append("#define DIETER_GLASS_DEPTH %d" % GLASS_DEPTH)
    walls, rims, glares, lasts = [], [], [], []
    for dark in (False, True):
        wall, rim, _, glare = glass_tables(dark)
        walls.append(wall)
        rims.append(rim)
        glares.append(glare)
        lasts.append(max(k for k in range(256) if glare[k]))
    out.append("static const unsigned char dieter_glass_wall[2][DIETER_GLASS_DEPTH] = {%s};"
               % ", ".join("{%s}" % ", ".join(str(v) for v in w) for w in walls))
    out.append("static const unsigned char dieter_glass_rim[2] = {%d, %d};" % tuple(rims))
    # the frame: the lit edge inside the bottom and right walls by the row or
    # column in from them, and a band's shade all round inside the rim
    frames = [glass_frame(dark) for dark in (False, True)]
    out.append("#define DIETER_GLASS_EDGE %d" % GLASS_EDGE)
    out.append("#define DIETER_GLASS_BAND %d" % GLASS_BAND)
    out.append("static const unsigned char dieter_glass_lit[2][DIETER_GLASS_EDGE] = {%s};"
               % ", ".join("{%s}" % ", ".join(str(v) for v in f[0]) for f in frames))
    out.append("static const unsigned char dieter_glass_band[2] = {%d, %d};"
               % tuple(f[1] for f in frames))
    out.append("static const unsigned char dieter_glass_last[2] = {%d, %d};" % tuple(lasts))
    out.append("static const unsigned char dieter_glass_glare[2][256] = {")
    for g in glares:
        out.append("  {" + ",".join(str(v) for v in g) + "},")
    out.append("};")
    for layout, cond in ((PHOTO, "#ifdef UI_220X176"), (VIDEO, "#else")):
        use(layout)
        out.append(cond)
        items = [("chevron", chevron_mask()), ("speaker", speaker_mask())]
        # an episode's marks, new and begun, in the marks' column
        items.append(("ring", ring_mask(L["RING_R"])))
        items.append(("ring_half", ring_mask(L["RING_R"], True)))
        items.append(("tile", tile_mask(L["ROW_COVER"])))
        items.append(("header_tile", tile_mask(L["HEADER_COVER"])))
        items.append(("search_ring", tw_ring_mask(L["TW_R"])))
        items.append(("usb", usb_mask()))
        # the USB screen's battery: charging, and charged (the skins' frames)
        batt, _ = battery_frames({"INK": (0, 0, 0)})
        two = Canvas(batt.w, 2 * GLYPH_H)
        for f, src in enumerate((0, 2)):
            for yy in range(GLYPH_H):
                two.px[f * GLYPH_H + yy] = [list(c) for c in batt.px[src * GLYPH_H + yy]]
                two.a[f * GLYPH_H + yy] = list(batt.a[src * GLYPH_H + yy])
        items.append(("usb_battery", two))
        q, ticks = dial_ticks()
        out.append("/* Settings' dial: its ticks' quarter, rows up and columns right of the")
        out.append("   ring's centre, a pixel's cover in sixteenths, 16 more for a major one */")
        out.append("#define DIETER_TICKS_Q %d" % q)
        out.append("/* the status line's rows, its glyphs' frames' height and their middles */")
        out.append("#define DIETER_STATUS %d" % L["STATUS"])
        out.append("#define DIETER_GLYPH_H %d" % GLYPH_H)
        out.append("#define DIETER_GLYPH_MID %d" % GLYPH_MID)
        out.append("#define DIETER_MARGIN %d" % L["M"])
        lines = ["static const unsigned char dieter_ticks[DIETER_TICKS_Q * DIETER_TICKS_Q] = {"]
        for i in range(0, len(ticks), q):
            lines.append("    " + ",".join("%d" % v for v in ticks[i:i + q]) + ",")
        out.append("\n".join(lines) + "\n};")
        out.append("#define DIETER_SEARCH_KEY_HALF %d" % (int(L["TW_R"]) + 2))
        for name, cv in items:
            up = name.upper()
            out.append("#define DIETER_%s_W %d" % (up, cv.w))
            out.append("#define DIETER_%s_H %d" % (up, cv.h))
            out.append(c_table("dieter_%s_alpha" % name, alpha_table(cv)))
        out.append("/* Now Playing's volume knob (%nK, skin_display.c): its body's masks, the")
        out.append("   disc, then by look (light, dark) its dark and its light, in a box of")
        out.append("   DIETER_KNOB_SIDE, its middle on the box's middle; its parts' radii and")
        out.append("   widths in sixteenths of a pixel; the body's colour by look, so many")
        out.append("   percent of the signal colour to black, the pointer's so many of the")
        out.append("   way from its light to the body's colour */")
        kg = knob_geometry()
        for dark in (False, True):
            disc, shade, light, khalf = knob_masks(dark)
            if not dark:
                out.append("#define DIETER_KNOB_SIDE %d" % (2 * khalf))
                for key in ("R", "TICK_IN", "TICK_OUT", "TICK_MINOR", "TICK_W", "TICK_W_MINOR",
                            "POINT_IN", "POINT_OUT", "POINT_W"):
                    out.append("#define DIETER_KNOB_%s %d" % (key, int(round(kg[key] * 16))))
                out.append("#define DIETER_KNOB_BODY_DARK %d" % KNOB_LOOKS[True]["body"])
                out.append("#define DIETER_KNOB_POINTER_DARK %d" % KNOB_LOOKS[True]["pointer"])
                out.append(c_table("dieter_knob_disc_alpha", bitmap_alpha_table(disc)))
            tag = "dark" if dark else "light"
            out.append(c_table("dieter_knob_shade_%s_alpha" % tag, bitmap_alpha_table(shade)))
            out.append(c_table("dieter_knob_light_%s_alpha" % tag, bitmap_alpha_table(light)))
        out.append("/* Search's key by look: its pixels, then its alpha, in one, so")
        out.append("   that the alpha follows them as a bitmap's does */")
        for look, tag in (("Dieter", "light"), ("Dieter-Dark", "dark")):
            p = palette(look)
            key, _ = tw_key(p, L["TW_R"])
            if tag == "light":
                out.append("#define DIETER_SEARCH_KEY_W %d" % key.w)
                out.append("#define DIETER_SEARCH_KEY_H %d" % key.h)
                out.append("struct dieter_key {")
                out.append("    unsigned short pixels[DIETER_SEARCH_KEY_W * DIETER_SEARCH_KEY_H];")
                out.append("    unsigned char alpha[(DIETER_SEARCH_KEY_W + 1) / 2 * DIETER_SEARCH_KEY_H];")
                out.append("};")
            out.append(c_key("dieter_search_key_%s" % tag, key.screen(0, 0, False),
                             bitmap_alpha_table(key)))
        out.append("/* Settings' On/Off key (UI_METRIC_SWITCHES 2, menu.c): by look and state,")
        out.append("   light off, light on, dark off, dark on, its pixels then its alpha; the")
        out.append("   lit lamp's disc and glow, drawn in the signal colour, and its core in")
        out.append("   white, masks in the key's box */")
        pics = []
        for look in ("Dieter", "Dieter-Dark"):
            for on in (False, True):
                pics.append(switch_key(palette(look), on)[0])
        glow, core = switch_lamp_masks()
        # all of them cut to the box of their ink, so that the key's width
        # is what its row must have room for
        ink = [(x, y) for cv in pics + [glow] for y in range(cv.h) for x in range(cv.w)
               if cv.a[y][x] * 15 >= 0.5]
        bx0, by0 = min(x for x, _ in ink), min(y for _, y in ink)
        bw = max(x for x, _ in ink) - bx0 + 1
        bh = max(y for _, y in ink) - by0 + 1
        pics = [cv.crop(bx0, by0, bw, bh) for cv in pics]
        glow, core = glow.crop(bx0, by0, bw, bh), core.crop(bx0, by0, bw, bh)
        sw = pics[0]
        out.append("#define DIETER_SWITCH_W %d" % sw.w)
        out.append("#define DIETER_SWITCH_H %d" % sw.h)
        out.append("struct dieter_switch {")
        out.append("    unsigned short pixels[DIETER_SWITCH_W * DIETER_SWITCH_H];")
        out.append("    unsigned char alpha[(DIETER_SWITCH_W + 1) / 2 * DIETER_SWITCH_H];")
        out.append("};")
        out.append("static const struct dieter_switch dieter_switch[4] = {")
        for cv in pics:
            body = c_key("x", cv.screen(0, 0, False), bitmap_alpha_table(cv)).split("\n")[1:]
            body[-1] = body[-1].rstrip(";")
            out.append("\n".join(["  {"] + body[:-1] + ["  },"]))
        out.append("};")
        out.append(c_table("dieter_switch_glow_alpha", bitmap_alpha_table(glow)))
        out.append(c_table("dieter_switch_core_alpha", bitmap_alpha_table(core)))
    out.append("#endif /* UI_220X176 */")
    out.append("#endif /* _DIETER_GLYPHS_H_ */")
    with open(HEADER, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


# --- the skins ------------------------------------------------------------------------------
CLOCK = "%al%?cf<%cH:%cM|%cl:%cM>"
TITLE = "%al%nx%Lt"


def frames(label, count):
    return ["%%xd(%s%s)" % (label, chr(ord("a") + i)) for i in range(count)]


def fill(text, **parts):
    for k, v in parts.items():
        text = text.replace("{" + k + "}", str(v))
    return text


def fonts(names):
    return "\n".join("%%Fl(%d,%s.fnt)" % (i + 2, n) for i, n in enumerate(names))


SIZES = {}


def glyph_sizes(p):
    """the status line's and Now Playing's glyphs of this look: name ->
    (picture, width, a frame's height, frames)"""
    out = {}
    cv, n = battery_frames(p)
    out["Battery"] = (cv, cv.w, GLYPH_H, n)
    cv = play_frames(p)
    out["Playing Status"] = (cv, cv.w, GLYPH_H, 4)
    cv = padlock(p)
    out["Hold"] = (cv, cv.w, GLYPH_H, 1)
    cv = moon(p)
    out["Night"] = (cv, cv.w, GLYPH_H, 1)
    for name in ("Repeat", "Shuffle"):
        cv, n = mode_frames(p, name)
        out[name] = (cv, cv.w, cv.h // n, n)
    return out


# Settings > Appearance > Battery Percentage: the level left of the battery
# in the clock's ink, against the battery in either direction (%aR keeps it
# right-aligned right to left too); none while the level isn't known (%bl's
# first case)
LEVEL = "%aR%?bl<|%bl%%>"


def battery_line(on, off):
    """Battery Percentage's line, or the one without it"""
    return "%%?St(battery percentage)<%s|%s>" % (on, off)


def clock_width(f):
    """the widest of CLOCK's times in font f, %cH:%cM's and %cl:%cM's, whose
    hour a space pads"""
    return max(f.width(form % (h, m)) for form in ("%2d:%02d", "%02d:%02d")
               for h in range(24) for m in range(60))


def status(width, prefix="", pct=False, narrow=False, level=None):
    """The status line: from the right the battery, at the margin, Night
    Mode's moon, play or pause or the padlock, 6 px apart (the iPod photo's
    5). As Neo's (make_neo_theme.py), each glyph has a viewport for every
    place it can be in; and the rooms left of them for a title, by Night
    Mode, with the playing status's place and without.
    pct: with Battery Percentage, the level PCT_GAP left of the battery,
    right-aligned in a box as wide as "100%", and the glyphs left of it GAP
    from its text (as far as they're from each other): a place
    of each by the level's count of digits, as wide as its widest of them;
    the titles' rooms those of "100%". level is its font's name and the
    font's number in the skin, by default the status line's font as the
    status bar numbers it. The glyphs are then prefix + "p"'s viewports,
    the battery the line's without it.
    narrow: beside the iPod photo's pane, whose 110 px have no room for the
    clock and all three: the level left of the battery without the playing
    status, Night Mode's moon instead of it and Hold's padlock instead of
    either, both where the playing status stands without the level; the
    rooms then the padlock's, and the level's or the moon's"""
    M = L["SM"]
    f = Font.get(L["F_STAT"])
    text_y = f.text_top(0, L["STATUS"])
    cell = f.height
    top = L["STATUS"] // 2 - GLYPH_MID
    out, rooms = [], []
    name = prefix + ("p" if pct else "")

    def vp(tag, glyph, x):
        w, h, _ = SIZES[glyph]
        return "%%Vl(%s%s,%d,%d,%d,%d,-)" % (name, tag, x, top, w, h)

    bx = width - M - L["BATT"][0]
    right = bx
    if pct:
        font, number = level or (L["F_STAT"], 3)
        lf = Font.get(font)
        level_w = lf.width("100%")
        right = bx - L["PCT_GAP"] - level_w
        out += ["%%Vl(%spct,%d,%d,%d,%d,%d)%%Vf(INK)" % (
            name, right, lf.text_top(0, L["STATUS"]), level_w, lf.height, number), LEVEL]
    else:
        out += [vp("bat", "Battery", bx), battery_tag()]
    pw, mw = SIZES["Playing Status"][0], SIZES["Night"][0]
    branches = []
    # the level's text's left edge by its count of digits, 3 that of "100%"
    lefts = {3: right}
    if pct:
        for digits, values in ((1, range(10)), (2, range(10, 100))):
            lefts[digits] = bx - L["PCT_GAP"] - max(lf.width("%d%%" % v) for v in values)
    if narrow:
        hold_x, moon_x = bx - L["GAP"] - pw, bx - L["GAP"] - mw
        out += [vp("hold", "Playing Status", hold_x), "%xd(H)",
                vp("moon", "Night", moon_x), "%xd(N)"]
        for night, x in ((0, right), (1, moon_x)):
            branches.append(["%%?mh<%%Vd(%shold)|%%Vd(%s%s)>" % (
                name, name, "moon" if night else "pct")])
            # the padlock's room as the playing status's, whose frame it has
            rooms.append((hold_x + 2 - L["GAP"] - M, x - L["GAP"] - M))
        return out, branches, rooms, text_y, cell
    play = frames("P", 4)
    state = "%%?mh<%%xd(H)|%%?mp<|%s|%s|%s|%s>>" % tuple(play)
    for night in (0, 1):
        shown = {}
        for digits in sorted(lefts, reverse=True):
            # the viewports of "100%"'s place unnumbered, the others by digits
            tag = "" if digits == 3 else str(digits)
            at = lefts[digits]
            this = []
            if night:
                out += [vp("moon" + tag, "Night", at - L["GAP"] - mw), "%xd(N)"]
                this.append("%%Vd(%smoon%s)" % (name, tag))
                at -= L["GAP"] + mw
            past = at
            out += [vp("playn%d%s" % (night, tag), "Playing Status", at - L["GAP"] - pw), state]
            this.append("%%Vd(%splayn%d%s)" % (name, night, tag))
            if digits == 3:
                # the triangle's ink starts 2 columns into its frame
                at -= L["GAP"] + pw - 2
                rooms.append((at - L["GAP"] - M, past - L["GAP"] - M))
            shown[digits] = "".join(this)
        if pct:
            branches.append(["%%Vd(%spct)" % name, "%%?if(%%bl, <, 10)<%s|%%?if(%%bl, <, 100)<%s|%s>>" % (
                shown[1], shown[2], shown[3])])
        else:
            branches.append([shown[3]])
    return out, branches, rooms, text_y, cell


def battery_tag():
    f = frames("B", SIZES["Battery"][2])
    levels = f[3:]
    return "%%?bp<%%?bc<%%t(1.0)%s;%%t(1.0)%s|%s>|%%?bl<%s>>" % (
        f[0], f[1], f[2], "|".join([levels[0]] + levels + [levels[-1]] * 2))


def title_vps(label, rooms, text_y, cell, text, narrow=False):
    """a title's viewports at the margin, one a room; narrow: for status()'s
    narrow rooms, one viewport for the padlock's, which Night Mode doesn't
    change"""
    M = L["SM"]

    def vp(tag, w):
        return ["%%Vl(%s%s,%d,%d,%d,%d,3)%%Vf(INK)" % (label, tag, M, text_y, w, cell), text]
    if narrow:
        return vp("h", rooms[0][0]) + vp("n0", rooms[0][1]) + vp("n1", rooms[1][1])
    out = []
    for night in (0, 1):
        out += vp("n%dp" % night, rooms[night][0]) + vp("n%d" % night, rooms[night][1])
    return out


def title_show(label, night, narrow=False):
    """which of those: with the playing status's place while it is shown or
    the padlock, else without; narrow, the padlock's or the other's"""
    if narrow:
        return "%%?mh<%%Vd(%sh)|%%Vd(%sn%d)>" % (label, label, night)
    shown, empty = "%%Vd(%sn%dp)" % (label, night), "%%Vd(%sn%d)" % (label, night)
    return "%%?mh<%s|%%?mp<%s|%s|%s|%s|%s>>" % (shown, empty, shown, shown, shown, shown)


def images(modes):
    out = []
    for name, label in (("Battery", "B"), ("Playing Status", "P"), ("Hold", "H"),
                        ("Night", "N"), ("Shuffle", "S"), ("Repeat", "R")):
        if name in ("Shuffle", "Repeat") and not modes:
            continue
        n = SIZES[name][2]
        out.append("%%xl(%s,%s.bmp,0,0%s)" % (label, name, ",%d" % n if n > 1 else ""))
    return "\n".join(out)


SBS = """\
# Dieter: the status line and the main menu's pane. Made by
# themes/tools/make_dieter_theme.py
%wd
{fonts}
{images}
%?LW<%VI(wide){wide}|%VI(half){half}>
# the pane by the row selected: Settings' disk, Photos' collage, or the music
%?LW<|%?LS<%Vd(floor)%Vd(ring)%Vd(about)|{music}|%Vd(floor)%?nP<%Vd(collage)%Vd(photocount)|%Vd(grille)%Vd(photocaption)>|{music}>>
# the cover fills the pane, Finder's {cover_size} px frame, its middle {pane_w}
# columns
%Cl(0,0,{cover_size},{cover_size},c,c)
%Vl(a,{cover_x},0,{cover_w},{h},-)
%Cd
%Vl(a,{cover_x},0,{cut},{h},-)
# the lists: under the status line, its hairline and its lip; beside the
# pane as wide as the list is to the pane's wall. The ground is the
# backdrop's (Dieter.bmp), lit from the top
%Vi(wide,0,{top},{w},{rows},1)%Vf(INK)%Vb(GROUND)
%Vi(half,0,{top},{pane},{rows},1)%Vf(INK)%Vb(GROUND)
# a screen of the whole width: the clock, its title, the glyphs; a title
# wider than its place between them takes the clock's too
%Vl(clockwide,{margin},{text_y},{clock_w},{cell},3)%Vf(INK)
{clock}
%Vl(titlewide,{title_x},{text_y},{title_w},{cell},3)%Vf(INK)
%ac%Lt
{long_vps}
{wide_vps}
# beside the pane: the clock in the main menu, a menu's title in any other
%Vl(hclock,{margin},{text_y},{clock_w},{cell},3)%Vf(INK)
{clock}
{half_vps}
# the pane, a recess from the screen's top, its floor and its left wall's
# shadow, is the backdrop's (Dieter.bmp); a screen of the whole width has
# its own, the ground and the status line across it
%XW(Wide.bmp)
%Vl(floor,{pane},0,{pane_w},{h},-)
# Settings: the disk's use as the firmware's ring (%nR), its used part in ink
# over its track, in its printed scale of ticks (UI_METRIC_RING_TICKS); what
# is free, and the whole, one group in the middle of the pane
%Vl(ring,{ring_x},{ring_y},{ring_box},{ring_box},-)%Vf(INK)%Vb({ring_ground})
%nR
%Vl(about,{text_x},{free_y},{text_w},{free_h},{free_font})%Vf(INK)%Vb(FLOOR)
%s%ac%nf
%Vl(about,{text_x},{total_y},{text_w},{sub_h},2)%Vf(SECOND)%Vb(FLOOR)
%ac%nT
# Photos: the collage, made at Updating Library (photos_panel.c), and their
# count under it; without photos, the grille and "No Photos"
%xl(Q,/.rockbox/photos_panel.bmp,0,0)
%Vl(collage,{pane},{collage_y},{pane_w},{collage_h},-)
%xd(Q)
%Vl(photocount,{text_x},{count_y},{text_w},{sub_h},2)%Vf(SECOND)%Vb(FLOOR)
%s%ac%nP
%Vl(photocaption,{text_x},{caption_y},{text_w},{sub_h},2)%Vf(SECOND)%Vb(FLOOR)
%ac%nP
# the music without a cover: the speaker's grille punched in the floor, and
# under it the library's count or the track
%xl(G,Grille.bmp,0,0)
%Vl(grille,{grille_x},{grille_y},{grille_w},{grille_h},-)
%xd(G)
%Vl(stopped,{text_x},{caption_y},{text_w},{sub_h},2)%Vf(SECOND)%Vb(FLOOR)
%ac%ns
%Vl(normal,{caption_x},{caption_y},{caption_w},{sub_h},2)%Vf(SECOND)%Vb(FLOOR)
%s%ac%?it<%it|%?fn<%fn|>>
# Quick Scroll's key, which the firmware draws over the list with the letter
# cut into it (line.c finds it by its label)
%xl(qskey,Quick Scroll.bmp,0,0)
"""


def sbs():
    W, H, M, X = L["W"], L["H"], L["M"], L["PANE"]
    wide_out, wide_br, rooms, text_y, cell = status(W)
    half_out, half_br, hrooms, _, _ = status(X, "h")
    long_vps = "\n".join(title_vps("long", rooms, text_y, cell, TITLE))
    hname = "\n".join(title_vps("hname", hrooms, text_y, cell, TITLE))
    clock_w, title_x, title_w = L["CLOCK"]
    title = "%%?Lf(titlewide)<%%Vd(clockwide)%%Vd(titlewide)|%%?nN<%s|%s>>" % (
        title_show("long", 1), title_show("long", 0))

    def show(prefix, branches, clock=None, label=None, narrow=False):
        b = []
        for night, this in enumerate(branches):
            extra = ""
            if clock:
                extra = "%%?if(%%Li, =, 33)<%%Vd(%s)|%s>" % (clock,
                                                           title_show(label, night, narrow))
            b.append("".join(this) + extra)
        return "%%Vd(%sbat)%%?nN<%s|%s>" % (prefix, b[1], b[0])

    def wide_line(title, glyphs):
        return "%%?if(%%Li, =, 33)<%%Vd(clockwide)|%s>%s" % (title, glyphs)

    # with Battery Percentage: the glyphs further left and the titles
    # shorter: in the screen's middle a gap short of the leftmost glyph, or
    # where it doesn't fit there in the middle of the room between the clock
    # and the glyphs, before it takes the clock's place; the clock beside the
    # pane as wide as its widest time; beside the iPod photo's pane the
    # narrow line
    narrow = L is PHOTO
    pwide_out, pwide_br, prooms, _, _ = status(W, pct=True)
    phalf_out, phalf_br, phrooms, _, _ = status(X, "h", pct=True, narrow=narrow)
    centre = title_x + title_w // 2
    ptitle_vps = []
    for night in (0, 1):
        limit = L["SM"] + prooms[night][0]
        half_w = min(title_w // 2, limit - centre)
        ptitle_vps += ["%%Vl(ptitlewide%d,%d,%d,%d,%d,3)%%Vf(INK)" % (
            night, centre - half_w, text_y, 2 * half_w, cell), "%ac%Lt",
            "%%Vl(proomwide%d,%d,%d,%d,%d,3)%%Vf(INK)" % (
            night, title_x, text_y, limit - title_x, cell), "%ac%Lt"]
    ptitle = "%%?nN<%s|%s>" % tuple(
        "%%?Lf(ptitlewide%d)<%%Vd(clockwide)%%Vd(ptitlewide%d)|"
        "%%?Lf(proomwide%d)<%%Vd(clockwide)%%Vd(proomwide%d)|%s>>" % (
            n, n, n, n, title_show("plong", n)) for n in (1, 0))
    pclock = ["%%Vl(hpclock,%d,%d,%d,%d,3)%%Vf(INK)" % (
        L["SM"], text_y, clock_width(Font.get(L["F_STAT"])), cell), CLOCK]
    wide = battery_line(wide_line(ptitle, show("", pwide_br)),
                        wide_line(title, show("", wide_br)))
    half = battery_line(show("h", phalf_br, "hpclock", "hpname", narrow),
                        show("h", half_br, "hclock", "hname"))
    wide_out += ptitle_vps + title_vps("plong", prooms, text_y, cell, TITLE) + pwide_out
    half_out += [hname] + pclock + title_vps("hpname", phrooms, text_y, cell, TITLE,
                                             narrow) + phalf_out
    music = ("%?C<%Vd(a)|%Vd(floor)%Vd(grille)%?mp<%Vd(stopped)|%Vd(normal)|%Vd(normal)"
             "|%Vd(normal)|%Vd(normal)|%Vd(normal)|%Vd(normal)|%Vd(stopped)|%Vd(stopped)>>")
    pane_w = W - X
    cover_size = 240 if L is VIDEO else 176
    cut = (cover_size - pane_w) // 2
    sub = Font.get(L["F_SUB"])
    row = Font.get(L["F_ROW"])
    gcy, gcap = grille_place()
    r, pitch, hr = L["GRILLE"]
    ink = grille_ink(r, pitch, hr)
    cx = pane_cx()
    gx0, gy0 = int(math.floor(cx - ink - 2)), int(math.floor(gcy - ink - 2))
    gx1, gy1 = int(math.ceil(cx + ink + 3)), int(math.ceil(gcy + ink + 3))
    dcy, dr, tick, free_y, total_y = dial_place()
    # the ring's viewport as large as its ticks, its middle the dial's
    ring_box = 2 * dial_ticks()[0]
    collage_y, count_y = collage_place()
    p = palette("Dieter")
    text = fill(SBS, fonts=fonts([L["F_SUB"], L["F_STAT"]]), images=images(False),
                wide=wide, half=half, music=music,
                cover_size=cover_size, cover_x=X - cut, cover_w=W - (X - cut), cut=cut,
                h=H, w=W, pane=X, pane_w=pane_w, top=L["TOP"], rows=H - L["TOP"],
                margin=L["SM"], text_y=text_y, cell=cell, clock_w=clock_w, clock=CLOCK,
                title_x=title_x, title_w=title_w, long_vps=long_vps,
                wide_vps="\n".join(wide_out), half_vps="\n".join(half_out),
                ring_x=int(cx) - ring_box // 2, ring_y=dcy - ring_box // 2, ring_box=ring_box,
                ring_ground="RING_GROUND",
                free_y=free_y, free_h=(row if L is VIDEO else sub).height,
                free_font=1 if L is VIDEO else 2, total_y=total_y, sub_h=sub.height,
                collage_y=collage_y, collage_h=176 if L is VIDEO else 132, count_y=count_y,
                caption_y=gcap, caption_x=X + 6, caption_w=pane_w - 12,
                text_x=X + 2, text_w=pane_w - 4, pane1=X + 1,
                grille_x=gx0, grille_y=gy0, grille_w=gx1 - gx0, grille_h=gy1 - gy0)
    return text


def ring_ground(p):
    """the floor, under the ring's antialiased edge (draw_disk_ring() mixes
    with the viewport's ground)"""
    return "%02X%02X%02X" % floor(p)


WPS = """\
# Dieter: Now Playing. Made by themes/tools/make_dieter_theme.py
%wd
# the backdrop of the screens of the whole width, the status bar's %XW
%X(Wide.bmp)
{fonts}
{images}
# the views, cycled with the centre button:
# normal | scrubber | album art | lyrics | rating | speed
%?mw<%?mv(2.0)<{volume}|{bar}%Vd(times)>|{scrub}%?kk<%Vd(under)|%Vd(hints)|%Vd(hints)>|%?C<%Vd(bigart)|%Vd(bignote)>|%Vd(lyrics){bar}|%Vd(rating)|%Vd(speed)>
# The track's details beside the cover, one block in the middle of its
# height, as Neo's (make_neo_theme.py): s<lines><rating><format>, e<lines>,
# b<lines><speed>
{block}
%?mw<{show_modes}|{show_modes}|||{show_modes}|{show_modes}>
%?mw<%?mv(2.0)<%Vd(knob)|%?C<%Vd(art)|%Vd(noart)>>|%?C<%Vd(art)|%Vd(noart)>|||%?C<%Vd(art)|%Vd(noart)>|%?C<%Vd(art)|%Vd(noart)>>
{status_show}
# the status line on the backdrop's strip: the clock, the title, the glyphs;
# with Battery Percentage the level left of the battery, the glyphs left of
# it and the title shorter (np_status())
{np_vps}
{vps}
{mode_vps}
# the cover, square, in its window behind glass (the firmware draws the
# glass over it, cover_glass()), a lit lip outside its foot and right side;
# without one, the grille punched in the screen's ground, with no glass
%Cl(0,0,{cover},{cover},c,c)
%Vl(art,{margin},{cover_y},{cover},{cover},-)
%Cd
%xl(c,Cover Right.bmp,0,0)
%xl(f,Cover Foot.bmp,0,0)
%Vl(art,{shadow_x},{shadow_y},1,{cover_h},-)
%xd(c)
%Vl(art,{shadow_x2},{shadow_y2},{cover},1,-)
%xd(f)
%xl(O,No Cover.bmp,0,0)
%Vl(noart,{margin},{cover_y},{cover_h},{cover_h},-)
%xd(O)
# the title on one line or two, which never scroll; %?nW asks this viewport,
# never shown, whether it takes two
%Vl(title,{col},0,{col_w},{title_h},2)
# the small rating line: five small lamps, lenses in bezels, a frame of the
# picture each by the rating and the signal colour, each a label of the same
# picture at its place in the line's viewport; right to left the line at
# the column's end, filling from the right
{lamp_images}
{block_vps}
# never shown, it only gives %fB the font of the bitrate
%Vl(bitrate,0,0,1,1,4)
# the scale behind glass in its recess: the played part in ink on its line,
# the needle in the signal colour at the playhead, grey while paused; its lit
# lip a hairline under the window. The times under it at the margins
%xl(K,Window.bmp,0,0)
%xl(k,Window Fill.bmp,0,0)
{needles}
%Vl(lip,{win_x},{lip_y},{win_w},1,-)%Vb(W_RIM)
{bar_vps}
%Vl(times,{margin},{times_y},{time_w},{time_rows},4)%Vf(SECOND)
%al%pc
%Vl(times,{times_right},{times_y},{time_w},{time_rows},4)%Vf(SECOND)
%ar-%pr
%Vl(under,{under_x},{times_y},{under_w},{time_rows},4)%Vf(SECOND)
%nx%ac%?kn<%?kk<%kn%?kc< · %kc|>|%kn%?kc< · %kc|>|%kn %Sx(of) %kt>|>
# a podcast's or a book's scrubber: what Previous and Next do there
%Vl(hints,{under_x},{times_y},{under_w},{time_rows},4)%Vf(SECOND)
%ac-15 / +30
# the volume: a knob turned to it in the cover's place (%nK, the firmware's),
# the window the song's progress as ever (the knob shows the volume); under
# the window, in the times' place, the word and the level beside it, at the
# line's start
%Vl(volume,{margin},{times_y},{grid},{time_rows},4)%Vf(INK)
%al%nv(2)
%Vl(knob,{margin},{cover_y},{cover},{cover},-)%Vf(INK)
%nK
# rating: five push keys, each in its hole, its lamp lit in the signal colour
# for the stars set; the wheel sets them
{rating}
# the large cover, and the grille in its place
%Vl(bigart,{big_x},{big_y},{big},{big},-)
%Cl(0,0,{big},{big},c,c)
%Cd
%Vl(bignote,{bignote_x},{bignote_y},{cover_h},{cover_h},-)
%xd(O)
# the lyrics over the window; and the speeds: the WPS code draws into these
%Vl(lyrics,{margin},{lyrics_y},{grid},{lyrics_h},{lyrics_font})%Vf(INK)
%Vl(speed,0,{speed_y},{w},{speed_h},4)%Vf(SECOND)
"""

SIGNAL_SETTING = "dieter colour"


def np_status(prooms, text_y, cell):
    """Now Playing's clock and title, and with Battery Percentage theirs: on
    the iPod photo the title in the middle of the room between the clock,
    then as wide as its widest time, and the glyphs; on the Video in the
    screen's middle, narrower where it would reach them. Its viewports and
    what Battery Percentage shows of them"""
    M = L["SM"]
    clock_w, _, _ = L["CLOCK"]
    title_x, title_w = L["NP_TITLE"]
    text = "%s%ac%Sx(Now Playing)"

    def vp(label, x, w):
        return "%%Vl(%s,%d,%d,%d,%d,5)%%Vf(INK)" % (label, x, text_y, w, cell)
    out = [vp("npclock", M, clock_w), CLOCK, vp("nptitle", title_x, title_w), text]
    clock, boxes = "npclock", []
    if L is PHOTO:
        pclock_w = clock_width(Font.get(L["F_STAT"]))
        out += [vp("pnpclock", M, pclock_w), CLOCK]
        clock = "pnpclock"
        left = M + pclock_w + L["GAP"]
        boxes = [(left, M + prooms[n][0] - left) for n in (0, 1)]
    else:
        centre = title_x + title_w // 2
        for n in (0, 1):
            half = min(title_w // 2, M + prooms[n][0] - centre)
            boxes.append((centre - half, 2 * half))
    titles = []
    for n, (x, w) in enumerate(boxes):
        if (x, w) == (title_x, title_w):
            titles.append("nptitle")
            continue
        out += [vp("pnptitle%d" % n, x, w), text]
        titles.append("pnptitle%d" % n)
    return out, "%%Vd(%s)%%?nN<%%Vd(%s)|%%Vd(%s)>" % (clock, titles[1], titles[0])


def lamp_x(i, col, lamps):
    """a small lamp's picture's left column: the first bezel's edge at col,
    the lamps SMALL_PITCH apart"""
    return int(round(col + L["SMALL_R"] + L["SMALL_BEZ"] + i * L["SMALL_PITCH"] - lamps[0] / 2))


def by_rating_rtl(position, lit, dark):
    """lit by the lamp's place in a line that fills from its end, right to
    left: the place at the right the first star"""
    return by_rating(2 * (4 - position), lit, dark)


def by_rating(least, lit, dark):
    """lit for a rating past least half stars (%rr, 0 to 10): one condition
    of two branches, %rr's value to %?if one more than the rating, rather
    than eleven, as each branch's tags are kept in the skin's memory"""
    return "%%?if(%%rr, >, %d)<%s|%s>" % (least + 1, lit, dark)


def by_signal(branches):
    """a choice by the signal colour (Settings > Appearance > Colour)"""
    return "%%?St(%s)<%s>" % (SIGNAL_SETTING, "|".join(branches))


def wps():
    W, H, M = L["W"], L["H"], np_margin()
    out, branches, rooms, text_y, cell = status(W)
    show = "%%Vd(bat)%%?nN<%s|%s>" % ("".join(branches[1]), "".join(branches[0]))
    pout, pbranches, prooms, _, _ = status(W, pct=True, level=(L["F_STAT"], 5))
    pshow = "%%Vd(bat)%%?nN<%s|%s>" % ("".join(pbranches[1]), "".join(pbranches[0]))
    np_vps, np_show = np_status(prooms, text_y, cell)
    status_show = battery_line(np_show + pshow, "%Vd(npclock)%Vd(nptitle)" + show)
    C, cy0, col = L["COVER"], L["COVER_Y"], L["COL"]
    col_w = W - M - col
    ft, fs, fsm = Font.get(L["F_TITLE"]), Font.get(L["F_NPSUB"]), Font.get(L["F_SMALL"])
    lamps = SIZES["Small Lamps"]
    rep, shuf = SIZES["Repeat"], SIZES["Shuffle"]

    line = {
        "artist": (3, "INK", "%s%al%?ia<%ia|%?iA<%iA|%Sx(Unknown Artist)>>"),
        "album": (3, "SECOND", "%s%al%?id<%id|%Sx(Unknown Album)>"),
        "show": (3, "INK", "%s%al%?id<%id|%Sx(Unknown Album)>"),
        "date": (3, "SECOND", "%al%kd"),
        "author": (3, "INK", "%s%al%?ia<%ia|%?iA<%iA|%Sx(Unknown Author)>>"),
        "chapter": (3, "SECOND", "%s%al%?kc<%kc|%id>"),
        "count": (4, "SECOND", "%al%pp %Sx(of) %pe"),
        "format": (4, "SECOND", "%fB"),
        "left": (4, "SECOND", "%nx%al%kl%ar%?ks< %ks|>"),
        "speed": (4, "SECOND", "%al%?ks<%ks|>"),
    }

    def title(n):
        return [("title%d" % n, ft.height * n, 0)]

    def details(first, second):
        return [(first, fs.height, L["ARTIST_GAP"]), (second, fs.height, 0),
                ("count", fsm.height, L["PLACE_GAP"])]

    variants = {}
    for n in range(1, L["TITLE_LINES"] + 1):
        for rating in (0, 1):
            for fmt in (0, 1):
                parts = title(n) + details("artist", "album")
                if rating:
                    parts.append(("lamps", L["LAMPS_H"], L["RATING_GAP"]))
                if fmt:
                    parts.append(("format", fsm.height, L["FORMAT_GAP"]))
                variants["s%d%d%d" % (n, rating, fmt)] = parts
        variants["e%d" % n] = title(n) + details("show", "date") + [
            ("left", fsm.height, L["FORMAT_GAP"])]
        for sp in (0, 1):
            variants["b%d%d" % (n, sp)] = title(n) + details("author", "chapter") + (
                [("speed", fsm.height, L["FORMAT_GAP"])] if sp else [])

    def place(parts):
        """the block in the middle of the cover's height, but under the
        modes at the column's right end, and over the scale's window; the
        window is low enough for the tallest to be both, or its title would
        run under the modes' glyphs"""
        total = sum(r + g for _, r, g in parts)
        under = L["MODES_Y"] + rep[1] + 2
        y = max(int(cy0 + C / 2 - total / 2), under)
        y = max(min(y, L["WIN"][1] - 2 - total), L["STATUS"] + 2)
        if y < under:
            raise ValueError("the details reach the modes' glyphs: %d rows over" % (under - y))
        res = []
        for what, rows, gap in parts:
            y += gap
            res.append((what, y, rows))
            y += rows
        return res

    block_vps = []
    for label, parts in variants.items():
        for what, y, rows in place(parts):
            if what.startswith("title"):
                # on a line that scrolls where it has one, or on two that don't
                first = "%nw(1)" if L["TITLE_LINES"] > 1 else "%s%al%?it<%it|%fn>"
                block_vps += ["%%Vl(%s,%d,%d,%d,%d,2)%%Vf(INK)" % (label, col, y, col_w, rows),
                              first] + (["%nw(2)"] if what == "title2" else [])
            elif what == "lamps":
                # their own viewports, shown by the language's direction:
                # at the column's start filling left to right, at its end
                # right to left
                ly = y + (L["LAMPS_H"] - lamps[1]) // 2
                x0 = lamp_x(0, col, lamps)
                line_w = lamp_x(4, col, lamps) - x0 + lamps[0]
                lit = "%%%%xd(ld%%d,%%%%St(%s),1)" % SIGNAL_SETTING
                for suffix, x in (("l", x0), ("q", col + col_w - line_w)):
                    block_vps.append("%%Vl(%s%s,%d,%d,%d,%d,-)" % (label, suffix, x, ly, line_w,
                                                                   lamps[1]))
                    block_vps.append("".join(
                        (by_rating(2 * i, lit % i, "%%xd(ld%d,1)" % i)
                         if suffix == "l" else by_rating_rtl(i, lit % i, "%%xd(ld%d,1)" % i))
                        for i in range(5)))
            else:
                font, colour, text = line[what]
                block_vps += ["%%Vl(%s,%d,%d,%d,%d,%d)%%Vf(%s)" % (label, col, y, col_w, rows,
                                                                 font, colour), text]

    def choose(view):
        def song(n):
            def rating(fmt):
                on, off = ("%%Vd(s%d%d%d)" % (n, r, fmt) for r in (1, 0))
                lamps_vp = "s%d1%d" % (n, fmt)
                on += "%%?Sr<%%Vd(%sq)|%%Vd(%sl)>" % (lamps_vp, lamps_vp)
                if view == "rating":
                    return off
                return "%%?St(now playing rating)<%s|%%?rr<%s|%s>|%s>" % (on, off, on, off)
            return "%%?St(now playing format)<%s|%s>" % (rating(1), rating(0))

        def book(n):
            if view == "speed":
                return "%%Vd(b%d0)" % n
            return "%%?ks<%%Vd(b%d1)|%%Vd(b%d0)>" % (n, n)

        def by_title(kind):
            if L["TITLE_LINES"] == 1:
                return kind(1)
            return "%%?nW(title)<%s|%s>" % (kind(2), kind(1))
        return "%%?kk<%s|%s|%s>" % ("" if view == "speed" else by_title(song),
                                     by_title(lambda n: "%%Vd(e%d)" % n), by_title(book))
    block = "%%?if(%%mw, <, 3)<%s|%%?mw<||||%s|%s>>" % (choose("normal"), choose("rating"),
                                                       choose("speed"))

    reps = frames("R", rep[2])
    shuf_tag = "%?ps<%?pa<%xd(Sb)|%xd(Sa)>|>"
    m1 = W - M - rep[0]
    m2 = m1 - L["GAP"] - shuf[0]
    mode_vps = "\n".join([
        "%%Vl(mode1,%d,%d,%d,%d,-)" % (m1, L["MODES_Y"], rep[0], rep[1]),
        "%%?mm<%s|%s>" % (shuf_tag, "|".join(reps)),
        "%%Vl(mode2,%d,%d,%d,%d,-)" % (m2, L["MODES_Y"], shuf[0], shuf[1]),
        "%%?mm<|%s>" % "|".join([shuf_tag] * 4)])

    x0 = lamp_x(0, col, lamps)
    lamp_images = []
    for i in range(5):
        x = lamp_x(i, col, lamps) - x0
        lamp_images.append("%%xl(ld%d,Small Lamp.bmp,%d,0,%d)" % (i, x, 1 + len(SIGNALS)))

    # the bar: one viewport a needle, by the signal colour and while paused;
    # the scrubber's has a knob. No volume's: the window keeps the song's
    # progress while the volume shows, the knob in the cover's place showing
    # the volume
    bx, by, bw, bh = bar_box()
    names = [s[0] for s in SIGNALS]
    needles = []
    for i, name in enumerate(names):
        needles.append("%%xl(n%d,Needle %s.bmp,0,0)" % (i, name))
        needles.append("%%xl(j%d,Knob %s.bmp,0,0)" % (i, name))
    needles.append("%xl(np,Needle Paused.bmp,0,0)")
    needles.append("%xl(jp,Knob Paused.bmp,0,0)")
    bar_vps = []
    for kind, tag, slider_l, label in (("pb", "%pb", "n", "pb"), ("sc", "%pb", "j", "sc")):
        for i in list(range(len(names))) + ["p"]:
            bar_vps += ["%%Vl(%s%s,0,%d,%d,%d,-)" % (label, i, by, W, bh),
                        "%s(%d,0,%d,%d,k,backdrop,K,slider,%s%s,fillunder)"
                        % (tag, bx, bw, bh, slider_l, i)]

    def pick(label):
        lit = by_signal(["%%Vd(%s%d)" % (label, i) for i in range(len(names))])
        # %mp: stopped, playing, paused, forward, rewind: grey but playing
        grey = "%%Vd(%sp)" % label
        return "%%?mp<%s|%s|%s|%s|%s>" % (grey, lit, grey, lit, lit)
    bar = "%Vd(lip)" + pick("pb")
    scrub = "%Vd(lip)" + pick("sc") + "%Vd(times)"
    volume = bar + "%Vd(volume)"

    # the rating's keys: each the key's picture, and over it its lamp lit
    kcy, kxs = key_place()
    _, half = rating_key(palette("Dieter"))
    # its frames: the lamp dark, then lit in each signal colour, by the
    # setting's value (a frame by a tag's value counts from 1)
    # one viewport, each key a label of the same picture at its place
    rx0, ry0 = int(round(kxs[0])) - half, int(round(kcy)) - half
    rating = []
    for k, kx in enumerate(kxs):
        rating.append("%%xl(yk%d,Rating Key.bmp,%d,0,%d)" % (k, int(round(kx)) - half - rx0,
                                                           1 + len(SIGNALS)))
    rating.append("%%Vl(rating,%d,%d,%d,%d,-)" % (rx0, ry0, int(round(kxs[-1])) + half - rx0,
                                                  2 * half))
    keys_ltr = "".join(by_rating(2 * k, "%%xd(yk%d,%%St(%s),1)" % (k, SIGNAL_SETTING),
                                 "%%xd(yk%d,1)" % k) for k in range(5))
    keys_rtl = "".join(by_rating_rtl(k, "%%xd(yk%d,%%St(%s),1)" % (k, SIGNAL_SETTING),
                                     "%%xd(yk%d,1)" % k) for k in range(5))
    rating.append("%%?Sr<%s|%s>" % (keys_rtl, keys_ltr))

    grid = W - 2 * M
    time_w = fsm.maxwidth * 5 if L is VIDEO else 61
    wfonts = [L["F_TITLE"], L["F_NPSUB"], L["F_SMALL"], L["F_STAT"]]
    lyrics_font = wfonts.index(L["F_LYRICS"]) + 2 if L["F_LYRICS"] in wfonts else 6
    if lyrics_font == 6:
        wfonts.append(L["F_LYRICS"])
    text = fill(WPS, fonts=fonts(wfonts), lyrics_font=lyrics_font,
                images=images(True), block=block, show_modes="%Vd(mode1)%Vd(mode2)",
                margin=M, status_show=status_show, np_vps="\n".join(np_vps),
                vps="\n".join(out + pout),
                mode_vps=mode_vps, cover=C, cover_y=cy0, cover_h=C + 1,
                shadow_x=M + C, shadow_y=cy0, shadow_x2=M, shadow_y2=cy0 + C,
                col=col, col_w=col_w, title_h=2 * ft.height, lamp_images="\n".join(lamp_images),
                block_vps="\n".join(block_vps), needles="\n".join(needles),
                win_x=L["WIN"][0], lip_y=L["WIN"][3], win_w=L["WIN"][2] - L["WIN"][0],
                bar_vps="\n".join(bar_vps), bar=bar, scrub=scrub, volume=volume,
                times_y=L["TIMES_Y"], time_w=time_w, time_rows=fsm.height,
                times_right=W - M - time_w, under_x=M + time_w + 4,
                under_w=grid - 2 * (time_w + 4), grid=grid, rating="\n".join(rating),
                half_grid=grid // 2, level_x=W - M - grid // 2,
                big=L["BIG"], big_x=L["BIG_X"], big_y=L["BIG_Y"], w=W,
                bignote_x=big_no_cover_place()[0], bignote_y=big_no_cover_place()[1],
                lyrics_y=L["LYRICS_Y"], lyrics_h=L["WIN"][1] - 5 - L["LYRICS_Y"],
                speed_y=min(L["WIN"][1] - 3, H - L["SPEED_H"]), speed_h=L["SPEED_H"])
    return text


# --- writing it all -------------------------------------------------------------------------
def look_pictures(look, p):
    """every picture of a look on the current screen, into its folder"""
    folder = os.path.join(L["OUT"], ".rockbox", "wps", look)
    os.makedirs(folder, exist_ok=True)
    for name, (cv, w, h, n) in glyph_sizes(p).items():
        write_bmp32(os.path.join(folder, name + ".bmp"), cv)
    write_bmp16(os.path.join(L["OUT"], ".rockbox", "backdrops", look + ".bmp"),
                backdrop(p, False), 0, 0)
    write_bmp16(os.path.join(folder, "Wide.bmp"), backdrop(p, True), 0, 0)
    cv, x, y = grille_picture(p)
    write_bmp16(os.path.join(folder, "Grille.bmp"), cv, x, y)
    write_bmp32(os.path.join(folder, "Quick Scroll.bmp"), qs_key(p))
    bx, by, _, _ = bar_box()
    track, filled = window_pictures(p)
    write_bmp16(os.path.join(folder, "Window.bmp"), track, bx, by)
    write_bmp16(os.path.join(folder, "Window Fill.bmp"), filled, bx, by)
    for (name, _, _), colour in zip(SIGNALS, p["SIGNALS"]):
        write_bmp32(os.path.join(folder, "Needle %s.bmp" % name), slider(p, colour, False))
        write_bmp32(os.path.join(folder, "Knob %s.bmp" % name), slider(p, colour, True))
    write_bmp32(os.path.join(folder, "Needle Paused.bmp"), slider(p, p["SECOND"], False))
    write_bmp32(os.path.join(folder, "Knob Paused.bmp"), slider(p, p["SECOND"], True))
    write_bmp16(os.path.join(folder, "No Cover.bmp"), no_cover(p), np_margin(), L["COVER_Y"])
    # the cover's shadow of a pixel right of it and under it, on the ground
    M, C, cy0 = np_margin(), L["COVER"], L["COVER_Y"]
    shade = mix(p["G_FOOT"], (0, 0, 0), 9)
    right = Canvas(1, C + 1)
    for yy in range(C + 1):
        right.rect(0, yy, 1, yy + 1, lip_colour(p, cy0 + yy))
    write_bmp16(os.path.join(folder, "Cover Right.bmp"), right, M + C, cy0)
    write_bmp16(os.path.join(folder, "Cover Foot.bmp"), Canvas(C, 1, lip_colour(p, cy0 + C)),
                M, cy0 + C)
    lamps, _ = small_lamps(p)
    write_bmp32(os.path.join(folder, "Small Lamp.bmp"), lamps)
    keys = [rating_key(p, colour)[0] for colour in [None] + p["SIGNALS"]]
    strip = Canvas(keys[0].w, keys[0].h * len(keys))
    for i, cv in enumerate(keys):
        for yy in range(cv.h):
            strip.px[i * cv.h + yy] = [list(c) for c in cv.px[yy]]
            strip.a[i * cv.h + yy] = list(cv.a[yy])
    write_bmp32(os.path.join(folder, "Rating Key.bmp"), strip)
    return folder


def cover_flow_slides():
    """Cover Flow's slide for an album without a cover (the owner's pick, A):
    the pane's floor with the grille, as Now Playing's empty window, by look;
    136 px, as pictureflow's own (it scales them)"""
    side = 136
    a, b, c = 0.36, 0.059, 0.0139
    for look, tag in (("Dieter", ""), ("Dieter-Dark", "_dark")):
        p = palette(look)
        cv = Canvas(side, side)
        cv.vgrad(0, 0, side, side, p["P_TOP"], p["P_FOOT"])
        ring_grille(cv, side / 2, side / 2, side * a, side * b, side * c, p)
        write_bmp16(os.path.join(ROOT, "apps", "plugins", "bitmaps", "native",
                                 "pictureflow_emptyslide_dieter%s.136x136x16.bmp" % tag),
                    cv, 0, 0)


def main():
    header()
    cover_flow_slides()
    for layout in LAYOUTS:
        use(layout)
        L["SMALL_PITCH_PX"] = int(round(L["SMALL_PITCH"]))
        out = os.path.join(L["OUT"], ".rockbox", "wps")
        os.makedirs(out, exist_ok=True)
        for look in LOOKS:
            p = palette(look)
            g = glyph_sizes(p)
            SIZES.clear()
            for name, (cv, w, h, n) in g.items():
                SIZES[name] = (w, h, n)
            size = small_lamp_size()
            SIZES["Small Lamps"] = (size, size, 1)
            look_pictures(look, p)
            texts = {"sbs": sbs(), "wps": wps()}
            colours = {
                "INK": hex565(p["INK"]), "SECOND": hex565(p["SECOND"]),
                "PANE": hex565(p["PANE"]), "W_RIM": hex565(p["W_RIM"]),
                "GROUND": hex565(p["GROUND"]),
                "COVER_SHADOW": hex565(mix(p["G_FOOT"], (0, 0, 0), 9)),
                "RING_GROUND": ring_ground(p),
            }
            colours["FLOOR"] = "%02X%02X%02X" % floor(p)
            for i, c in enumerate(wall(p)):
                colours["WALL%d" % i] = "%02X%02X%02X" % c
            for ext, text in texts.items():
                for role, value in colours.items():
                    text = text.replace("(%s)" % role, "(%s)" % value)
                with open(os.path.join(out, "%s.%s" % (look, ext)), "w", encoding="utf-8") as f:
                    f.write(text)


main()
