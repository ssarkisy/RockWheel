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
        assert b[:4] == b"RB12", path
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
# The Video's, the mockups' grid: one margin of 16 px; the status line's 20
# rows, its hairline and lip under them, the lists from 22; rows of 30 and of
# 36 with covers of 32; the main menu's pane from column 160; Now Playing's
# cover of 144 at (16, 28), its details from 176, the scale's window across
# the margin under it, the times under the window.
VIDEO = dict(
    NAME="video", W=320, H=240, OUT=THEME, M=16, STATUS=20, TOP=22, ROW=30,
    PANE=160, BATT=(20, 10), GAP=6, PLAY_H=9,
    # the clock's width, the title's place and width; Now Playing's title's
    CLOCK=(64, 84, 152), NP_TITLE=(100, 120),
    COVER=144, COVER_Y=28, COL=176,
    WIN=(16, 182, 304, 206), TICK=12, MAJOR=72, BASE_UP=6, MAJOR_H=7, TIMES_Y=210,
    GRILLE=(54, 8.5, 2.0), RING=78, PANE_GAP=18, LINE2=18, DIAL_MINOR=5,
    QS=72, KEY_R=11.5, KEY_PITCH=40, LAMP_R=3.2, SMALL_R=2.6, SMALL_PITCH=9, LAMPS_H=12,
    KNOB_R=5, NO_COVER_GRILLE=(0.36, 0.059, 0.0139),
    F_ROW="19-Inter-SemiBold", F_SUB="15-Inter-Medium", F_STAT="15-Inter-SemiBold",
    F_TITLE="18-Inter-SemiBold", F_NPSUB="15-Inter-Medium", F_SMALL="14-Inter-Medium",
    F_LYRICS="17-Inter-Medium-13",
    ARTIST_GAP=3, PLACE_GAP=7, RATING_GAP=7, FORMAT_GAP=5,
    MODES_Y=25, BIG=200, BIG_X=60, BIG_Y=30, LYRICS_Y=28, SPEED_H=44,
    # the list marks the firmware draws (dieter_glyphs.h)
    DOT_R=3.0, RING_R=3.25, CHEV=(5, 9), ROW_COVER=32, HEADER_COVER=52,
    TW_R=10, USB_R=30, USB_W=4,
)
# The iPod photo's, by the same rules on its screen: a margin of 14 px; the
# status line's 16 rows, the lists from 18; rows of 22, of two lines the
# firmware's 31 with covers of 29 (LINE_COVER_SIZE); the pane from 110, as
# the port's; Now Playing's cover of 96 at (14, 22), its details from 122.
PHOTO = dict(
    NAME="photo", W=220, H=176, OUT=os.path.join(THEME, "220x176"), M=14, STATUS=16,
    TOP=18, ROW=22, PANE=110, BATT=(17, 8), GAP=5, PLAY_H=8,
    CLOCK=(48, 58, 104), NP_TITLE=(62, 96),
    COVER=96, COVER_Y=22, COL=122,
    WIN=(14, 126, 206, 146), TICK=8, MAJOR=48, BASE_UP=5, MAJOR_H=5, TIMES_Y=149,
    GRILLE=(40, 7.0, 1.7), RING=63, PANE_GAP=12, LINE2=15, DIAL_MINOR=4.5,
    QS=56, KEY_R=8.5, KEY_PITCH=28, LAMP_R=2.4, SMALL_R=2.2, SMALL_PITCH=7.5, LAMPS_H=10,
    KNOB_R=4, NO_COVER_GRILLE=(0.36, 0.059, 0.0139),
    F_ROW="15-Inter-SemiBold", F_SUB="14-Inter-Medium", F_STAT="14-Inter-SemiBold",
    F_TITLE="15-Inter-SemiBold", F_NPSUB="14-Inter-Medium", F_SMALL="14-Inter-Medium",
    F_LYRICS="14-Inter-Medium",
    ARTIST_GAP=2, PLACE_GAP=4, RATING_GAP=3, FORMAT_GAP=2,
    MODES_Y=20, BIG=140, BIG_X=40, BIG_Y=26, LYRICS_Y=22, SPEED_H=40,
    DOT_R=2.5, RING_R=3.0, CHEV=(4, 7), ROW_COVER=29, HEADER_COVER=38,
    TW_R=9, USB_R=22, USB_W=3,
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


def no_cover(p):
    """the cover's place without one: the pane's recess with the grille"""
    C = L["COVER"]
    cv = Canvas(C, C + 1)
    cv.vgrad(0, 0, C, C, p["P_TOP"], p["P_FOOT"])
    cv.rect(0, 0, C, 2, p["P_SHADE"])
    cv.rect(0, 0, 2, C, p["P_SHADE"])
    cv.rect(0, C, C, C + 1, (255, 255, 255) if not p["DARK"] else p["W_RIM"])
    a, b, c = L["NO_COVER_GRILLE"]
    ring_grille(cv, C / 2, C / 2, C * a, C * b, C * c, p)
    return cv


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


def small_lamps(p):
    """the small rating line's lamps over the ground: dark, a smoked lens
    with a speck; and lit, a mask the firmware draws in the signal colour
    (%xS), a disc with a faint glow"""
    r = L["SMALL_R"]
    size = int(math.ceil(2 * r + 2))
    c = size / 2
    dark = Canvas(size, size)
    lens = mix(p["G_FOOT"], (0, 0, 0), 30)
    dark.shape(lambda x, y: (x - c) ** 2 + (y - c) ** 2 <= r * r, (0, 0, size, size),
               lambda x, y: lerp(lens, (0, 0, 0), 0.25 * max(0, (y - c) / r)), ss=4)
    dark.circle(c - r * 0.35, c - r * 0.4, max(0.6, r * 0.28), lerp(lens, (255, 255, 255), 0.45))
    lit = Canvas(size, size)
    glow = Canvas(size, size)
    glow.shape(lambda x, y: (x - c) ** 2 + (y - c) ** 2 <= (r + 1) ** 2, (0, 0, size, size),
               (0, 0, 0), ss=4)
    for yy in range(size):
        for xx in range(size):
            if glow.a[yy][xx] > 0:
                lit.put(xx, yy, (0, 0, 0), glow.a[yy][xx] * 0.35)
    lit.shape(lambda x, y: (x - c) ** 2 + (y - c) ** 2 <= r * r, (0, 0, size, size),
              (0, 0, 0), ss=4)
    return dark, lit, size


def qs_key(p):
    """Quick Scroll's light key, convex and lit from the top left, without a
    drop shadow: the key alone and a pixel round it for
    its edge, its centre half the picture's width from its top and its left,
    on a pixel's corner. The firmware cuts the letter into it (line.c) and
    stretches it by its middle column into a capsule for a wider text"""
    s = L["QS"]
    r = s / 2
    cv = Canvas(s + 2, s + 2)
    c = cv.w // 2
    round_key(cv, c, c, r, p["QS_KEY"], shadow=False)
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


def mode_frames(p, name):
    """Now Playing's modes in fine strokes: repeat all, one, shuffle (the
    repeat mode of that name), A-B; shuffle by songs and by albums"""
    w, h = 12, 11
    sw = 1.3
    c = p["INK"]
    if name == "Repeat":
        cv = Canvas(w, h * 4)
        for k in range(4):
            y = k * h
            loop = [(2.5, y + 7.5), (2.5, y + 4), (3.5, y + 3), (9.5, y + 3)]
            back = [(9.5, y + 3.5), (9.5, y + 7), (8.5, y + 8), (2.5, y + 8)]
            cv.stroke(loop, sw, c)
            cv.stroke(back, sw, c)
            cv.poly([(8, y + 0.8), (11, y + 3), (8, y + 5.2)], c)
            cv.poly([(4, y + 5.8), (1, y + 8), (4, y + 10.2)], c)
            if k == 1:          # one: a dot in the loop
                cv.circle(6, y + 5.5, 1.2, c)
            elif k == 2:        # shuffle: a cross in it
                cv.stroke([(4.8, y + 4.6), (7.2, y + 6.6)], 1.0, c)
                cv.stroke([(4.8, y + 6.6), (7.2, y + 4.6)], 1.0, c)
            elif k == 3:        # A-B: two ticks in it
                cv.rect(4.5, y + 4.5, 5.5, y + 6.5, c)
                cv.rect(6.5, y + 4.5, 7.5, y + 6.5, c)
        return cv, 4
    cv = Canvas(w, h * 2)
    for k in range(2):
        y = k * h
        cv.stroke([(0.5, y + 3), (3.5, y + 3), (7.5, y + 8), (9.5, y + 8)], sw, c)
        cv.stroke([(0.5, y + 8), (3.5, y + 8), (4.6, y + 6.6)], sw, c)
        cv.stroke([(6.4, y + 4.4), (7.5, y + 3), (9.5, y + 3)], sw, c)
        cv.poly([(9, y + 0.8), (12, y + 3), (9, y + 5.2)], c)
        cv.poly([(9, y + 5.8), (12, y + 8), (9, y + 10.2)], c)
        if k == 1:              # by albums: a small square under it
            pass
    return cv, 2


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


def dot_mask(r):
    """a disc of radius r centred on a pixel's corner, in the box of its ink"""
    n = int(math.ceil(r)) + 1
    return mask(2 * n, 2 * n, lambda cv: cv.circle(n, n, r, (0, 0, 0))), n


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
    for layout, cond in ((PHOTO, "#ifdef UI_220X176"), (VIDEO, "#else")):
        use(layout)
        out.append(cond)
        items = [("chevron", chevron_mask()), ("speaker", speaker_mask())]
        dot, dot_n = dot_mask(L["DOT_R"])
        items.append(("dot", dot))
        ring_n = int(math.ceil(L["RING_R"])) + 1
        items.append(("ring", ring_mask(L["RING_R"])))
        items.append(("ring_half", ring_mask(L["RING_R"], True)))
        disc, _ = dot_mask(L["RING_R"])
        # the disc the cursor fills a ring with, as big as the ring's box
        disc_cv = Canvas(2 * ring_n, 2 * ring_n)
        disc_cv.circle(ring_n, ring_n, L["RING_R"], (0, 0, 0))
        items.append(("ring_fill", disc_cv))
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
        out.append("/* the cursor's dot and an episode's ring: their middles on a pixel's")
        out.append("   corner, so many pixels from their boxes' sides */")
        out.append("#define DIETER_DOT_HALF %d" % dot_n)
        out.append("#define DIETER_RING_HALF %d" % ring_n)
        out.append("#define DIETER_SEARCH_KEY_HALF %d" % (int(L["TW_R"]) + 2))
        for name, cv in items:
            up = name.upper()
            out.append("#define DIETER_%s_W %d" % (up, cv.w))
            out.append("#define DIETER_%s_H %d" % (up, cv.h))
            out.append(c_table("dieter_%s_alpha" % name, alpha_table(cv)))
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


def status(width, prefix=""):
    """The status line: from the right the battery, at the margin, Night
    Mode's moon, play or pause or the padlock, 6 px apart (the iPod photo's
    5). As Neo's (make_neo_theme.py), each glyph has a viewport for every
    place it can be in; and the rooms left of them for a title, by Night
    Mode, with the playing status's place and without"""
    M = L["M"]
    f = Font.get(L["F_STAT"])
    text_y = f.text_top(0, L["STATUS"])
    cell = f.height
    top = L["STATUS"] // 2 - GLYPH_MID
    out, rooms = [], []

    def vp(tag, name, x):
        w, h, _ = SIZES[name]
        return "%%Vl(%s%s,%d,%d,%d,%d,-)" % (prefix, tag, x, top, w, h)

    bx = width - M - L["BATT"][0]
    out += [vp("bat", "Battery", bx), battery_tag()]
    play = frames("P", 4)
    state = "%%?mh<%%xd(H)|%%?mp<|%s|%s|%s|%s>>" % tuple(play)
    branches = []
    for night in (0, 1):
        at = bx
        this = []
        if night:
            mw = SIZES["Night"][0]
            out += [vp("moon", "Night", at - L["GAP"] - mw), "%xd(N)"]
            this.append("%%Vd(%smoon)" % prefix)
            at -= L["GAP"] + mw
        past = at
        pw = SIZES["Playing Status"][0]
        out += [vp("playn%d" % night, "Playing Status", at - L["GAP"] - pw), state]
        this.append("%%Vd(%splayn%d)" % (prefix, night))
        # the triangle's ink starts 2 columns into its frame
        at -= L["GAP"] + pw - 2
        rooms.append((at - L["GAP"] - M, past - L["GAP"] - M))
        branches.append(this)
    return out, branches, rooms, text_y, cell


def battery_tag():
    f = frames("B", SIZES["Battery"][2])
    levels = f[3:]
    return "%%?bp<%%?bc<%%t(1.0)%s;%%t(1.0)%s|%s>|%%?bl<%s>>" % (
        f[0], f[1], f[2], "|".join([levels[0]] + levels + [levels[-1]] * 2))


def title_vps(label, rooms, text_y, cell, text):
    M = L["M"]
    out = []
    for night in (0, 1):
        out += ["%%Vl(%sn%dp,%d,%d,%d,%d,3)%%Vf(INK)" % (label, night, M, text_y,
                                                         rooms[night][0], cell), text,
                "%%Vl(%sn%d,%d,%d,%d,%d,3)%%Vf(INK)" % (label, night, M, text_y,
                                                        rooms[night][1], cell), text]
    return out


def title_show(label, night):
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
%?LW<%VI(wide)%?if(%Li, =, 33)<%Vd(clockwide)|{title}>{wide}|%VI(half){half}>
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

    def show(prefix, branches, name):
        b = []
        for night, this in enumerate(branches):
            extra = ""
            if name:
                extra = "%%?if(%%Li, =, 33)<%%Vd(hclock)|%s>" % title_show("hname", night)
            b.append("".join(this) + extra)
        return "%%Vd(%sbat)%%?nN<%s|%s>" % (prefix, b[1], b[0])
    wide = show("", wide_br, False)
    half = show("h", half_br, True)
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
                title=title, wide=wide, half=half, music=music,
                cover_size=cover_size, cover_x=X - cut, cover_w=W - (X - cut), cut=cut,
                h=H, w=W, pane=X, pane_w=pane_w, top=L["TOP"], rows=H - L["TOP"],
                margin=M, text_y=text_y, cell=cell, clock_w=clock_w, clock=CLOCK,
                title_x=title_x, title_w=title_w, long_vps=long_vps,
                wide_vps="\n".join(wide_out), half_vps="\n".join(half_out + [hname]),
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
%?mw<%?C<%Vd(art)|%Vd(noart)>|%?C<%Vd(art)|%Vd(noart)>|||%?C<%Vd(art)|%Vd(noart)>|%?C<%Vd(art)|%Vd(noart)>>
# the status line on the backdrop's strip: the clock, the title, the glyphs
%V({margin},{text_y},{clock_w},{cell},5)%Vf(INK)
{clock}
%V({title_x},{text_y},{title_w},{cell},5)%Vf(INK)
%s%ac%Sx(Now Playing)
%?nN<{show}
{vps}
{mode_vps}
# the cover, square, on the surface with a shadow of a pixel right of it and
# under it; without one, its place recessed with the grille punched in it
%Cl(0,0,{cover},{cover},c,c)
%Vl(art,{margin},{cover_y},{cover},{cover},-)
%Cd
%xl(c,Cover Right.bmp,0,0)
%xl(f,Cover Foot.bmp,0,0)
%Vl(art,{shadow_x},{shadow_y},1,{cover},-)
%xd(c)
%Vl(art,{shadow_x2},{shadow_y2},{cover},1,-)
%xd(f)
%xl(O,No Cover.bmp,0,0)
%Vl(noart,{margin},{cover_y},{cover},{cover_h},-)
%xd(O)
# the title on one line or two, which never scroll; %?nW asks this viewport,
# never shown, whether it takes two
%Vl(title,{col},0,{col_w},{title_h},2)
# the small rating line: five small lamps, lit in the signal colour (a
# mask the firmware draws in it, %xS), each a label of the same pictures at
# its place in the line's viewport
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
# the volume in the window, its level the inked part with the needle at it;
# its printed label under the window, the level at the right
%Vl(volume,{margin},{times_y},{half_grid},{time_rows},4)%Vf(SECOND)
%al%Sx(Volume)
%Vl(volume,{level_x},{times_y},{half_grid},{time_rows},4)%Vf(INK)
%ar%nv(1)
# rating: five push keys, each in its hole, its lamp lit in the signal colour
# for the stars set; the wheel sets them
{rating}
# the large cover, and the grille in its place
%Vl(bigart,{big_x},{big_y},{big},{big},-)
%Cl(0,0,{big},{big},c,c)
%Cd
%Vl(bignote,{big_x},{big_y},{big},{big},-)%Vb(PANE)
# the lyrics over the window; and the speeds: the WPS code draws into these
%Vl(lyrics,{margin},{lyrics_y},{grid},{lyrics_h},{lyrics_font})%Vf(INK)
%Vl(speed,0,{speed_y},{w},{speed_h},4)%Vf(SECOND)
"""

SIGNAL_SETTING = "dieter colour"


def lamp_x(i, col, lamps):
    """a small lamp's picture's left column, its middle as the mockups'"""
    return int(round(col + L["SMALL_R"] + i * L["SMALL_PITCH"] - lamps[0] / 2))


def by_rating(least, lit, dark):
    """lit for a rating past least half stars (%rr: none, then by half
    stars)"""
    return "%%?rr<%s>" % "|".join(lit if v > least else dark for v in range(11))


def by_signal(branches):
    """a choice by the signal colour (Settings > Appearance > Colour)"""
    return "%%?St(%s)<%s>" % (SIGNAL_SETTING, "|".join(branches))


def wps():
    W, H, M = L["W"], L["H"], L["M"]
    out, branches, rooms, text_y, cell = status(W)
    show = "%%Vd(bat)%%?nN<%s|%s>" % ("".join(branches[1]), "".join(branches[0]))
    C, cy0, col = L["COVER"], L["COVER_Y"], L["COL"]
    col_w = W - M - col
    ft, fs, fsm = Font.get(L["F_TITLE"]), Font.get(L["F_NPSUB"]), Font.get(L["F_SMALL"])
    lamps = SIZES["Small Lamps"]
    rep, shuf = SIZES["Repeat"], SIZES["Shuffle"]
    clock_w, _, _ = L["CLOCK"]
    title_x, title_w = L["NP_TITLE"]

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
    for n in (1, 2):
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
        modes at the column's right end, and over the scale's window: where
        both can't be, as with a title of two lines, the rating and the
        format on the iPod photo, over the window"""
        total = sum(r + g for _, r, g in parts)
        y = int(cy0 + C / 2 - total / 2)
        y = max(y, L["MODES_Y"] + rep[1] + 2)
        y = max(min(y, L["WIN"][1] - 2 - total), L["STATUS"] + 2)
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
                block_vps += ["%%Vl(%s,%d,%d,%d,%d,2)%%Vf(INK)" % (label, col, y, col_w, rows),
                              "%nw(1)"] + (["%nw(2)"] if what == "title2" else [])
            elif what == "lamps":
                ly = y + (L["LAMPS_H"] - lamps[1]) // 2
                x0 = lamp_x(0, col, lamps)
                block_vps.append("%%Vl(%s,%d,%d,%d,%d,-)" % (
                    label, x0, ly, lamp_x(4, col, lamps) - x0 + lamps[0], lamps[1]))
                block_vps.append("".join(by_rating(2 * i, "%%xd(ll%d)" % i, "%%xd(ld%d)" % i)
                                         for i in range(5)))
            else:
                font, colour, text = line[what]
                block_vps += ["%%Vl(%s,%d,%d,%d,%d,%d)%%Vf(%s)" % (label, col, y, col_w, rows,
                                                                 font, colour), text]

    def choose(view):
        def song(n):
            def rating(fmt):
                on, off = ("%%Vd(s%d%d%d)" % (n, r, fmt) for r in (1, 0))
                if view == "rating":
                    return off
                return "%%?St(now playing rating)<%s|%%?rr<%s|%s>|%s>" % (on, off, on, off)
            return "%%?St(now playing format)<%s|%s>" % (rating(1), rating(0))

        def book(n):
            if view == "speed":
                return "%%Vd(b%d0)" % n
            return "%%?ks<%%Vd(b%d1)|%%Vd(b%d0)>" % (n, n)

        def by_title(kind):
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
        lamp_images += ["%%xl(ld%d,Small Lamp.bmp,%d,0)" % (i, x),
                        "%%xl(ll%d,Small Lamp Lit.bmp,%d,0)" % (i, x), "%%xS(ll%d)" % i]

    # the bar: one viewport a needle, by the signal colour and while paused;
    # the scrubber's has a knob
    bx, by, bw, bh = bar_box()
    names = [s[0] for s in SIGNALS]
    needles = []
    for i, name in enumerate(names):
        needles.append("%%xl(n%d,Needle %s.bmp,0,0)" % (i, name))
        needles.append("%%xl(j%d,Knob %s.bmp,0,0)" % (i, name))
    needles.append("%xl(np,Needle Paused.bmp,0,0)")
    needles.append("%xl(jp,Knob Paused.bmp,0,0)")
    bar_vps = []
    for kind, tag, slider_l, label in (("pb", "%pb", "n", "pb"), ("sc", "%pb", "j", "sc"),
                                       ("vo", "%pv", "n", "vo")):
        for i in list(range(len(names))) + ["p"]:
            bar_vps += ["%%Vl(%s%s,0,%d,%d,%d,-)" % (label, i, by, W, bh),
                        "%s(%d,0,%d,%d,k,backdrop,K,slider,%s%s,fillunder)"
                        % (tag, bx, bw, bh, slider_l, i)]

    def pick(label):
        lit = by_signal(["%%Vd(%s%d)" % (label, i) for i in range(len(names))])
        if label == "vo":
            return lit
        # %mp: stopped, playing, paused, forward, rewind: grey but playing
        grey = "%%Vd(%sp)" % label
        return "%%?mp<%s|%s|%s|%s|%s>" % (grey, lit, grey, lit, lit)
    bar = "%Vd(lip)" + pick("pb")
    scrub = "%Vd(lip)" + pick("sc") + "%Vd(times)"
    volume = "%Vd(lip)" + pick("vo") + "%Vd(volume)"

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
    rating.append("".join(by_rating(2 * k, "%%xd(yk%d,%%St(%s),1)" % (k, SIGNAL_SETTING),
                                    "%%xd(yk%d,1)" % k) for k in range(5)))

    grid = W - 2 * M
    time_w = fsm.maxwidth * 5 if L is VIDEO else 61
    wfonts = [L["F_TITLE"], L["F_NPSUB"], L["F_SMALL"], L["F_STAT"]]
    lyrics_font = wfonts.index(L["F_LYRICS"]) + 2 if L["F_LYRICS"] in wfonts else 6
    if lyrics_font == 6:
        wfonts.append(L["F_LYRICS"])
    text = fill(WPS, fonts=fonts(wfonts), lyrics_font=lyrics_font,
                images=images(True), block=block, show_modes="%Vd(mode1)%Vd(mode2)",
                margin=M, text_y=text_y, cell=cell, clock_w=clock_w, clock=CLOCK,
                title_x=title_x, title_w=title_w, vps="\n".join(out),
                mode_vps=mode_vps, cover=C, cover_y=cy0, cover_h=C + 1,
                shadow_x=M + C, shadow_y=cy0 + 1, shadow_x2=M + 1, shadow_y2=cy0 + C,
                col=col, col_w=col_w, title_h=2 * ft.height, lamp_images="\n".join(lamp_images),
                block_vps="\n".join(block_vps), needles="\n".join(needles),
                win_x=L["WIN"][0], lip_y=L["WIN"][3], win_w=L["WIN"][2] - L["WIN"][0],
                bar_vps="\n".join(bar_vps), bar=bar, scrub=scrub, volume=volume,
                times_y=L["TIMES_Y"], time_w=time_w, time_rows=fsm.height,
                times_right=W - M - time_w, under_x=M + time_w + 4,
                under_w=grid - 2 * (time_w + 4), grid=grid, rating="\n".join(rating),
                half_grid=grid // 2, level_x=W - M - grid // 2,
                big=L["BIG"], big_x=L["BIG_X"], big_y=L["BIG_Y"], w=W,
                lyrics_y=L["LYRICS_Y"], lyrics_h=L["WIN"][1] - 5 - L["LYRICS_Y"],
                speed_y=min(L["WIN"][1] - 3, H - L["SPEED_H"]), speed_h=L["SPEED_H"])
    return text.replace("%?nN<{show}", show)


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
    write_bmp16(os.path.join(folder, "No Cover.bmp"), no_cover(p), L["M"], L["COVER_Y"])
    # the cover's shadow of a pixel right of it and under it, on the ground
    M, C, cy0 = L["M"], L["COVER"], L["COVER_Y"]
    shade = mix(p["G_FOOT"], (0, 0, 0), 9)
    write_bmp16(os.path.join(folder, "Cover Right.bmp"), Canvas(1, C, shade), M + C, cy0 + 1)
    write_bmp16(os.path.join(folder, "Cover Foot.bmp"), Canvas(C, 1, shade), M + 1, cy0 + C)
    dark, lit, _ = small_lamps(p)
    write_bmp32(os.path.join(folder, "Small Lamp.bmp"), dark)
    write_bmp32(os.path.join(folder, "Small Lamp Lit.bmp"), lit)
    keys = [rating_key(p, colour)[0] for colour in [None] + p["SIGNALS"]]
    strip = Canvas(keys[0].w, keys[0].h * len(keys))
    for i, cv in enumerate(keys):
        for yy in range(cv.h):
            strip.px[i * cv.h + yy] = [list(c) for c in cv.px[yy]]
            strip.a[i * cv.h + yy] = list(cv.a[yy])
    write_bmp32(os.path.join(folder, "Rating Key.bmp"), strip)
    return folder


def main():
    header()
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
            r = L["SMALL_R"]
            size = int(math.ceil(2 * r + 2))
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
