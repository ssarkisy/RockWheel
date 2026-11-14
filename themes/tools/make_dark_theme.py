#!/usr/bin/env python3
# Makes the dark iClassic Square from the light one: its skins and every bitmap. Nothing
# in the dark theme is edited by hand, so change the light one and run this again:
#
#   themes/tools/make_dark_theme.py
#
# BMPs of 24 bits are worked on by the Python standard library, and of 32 their colours,
# their alpha kept. Scrub Knob.bmp reads well on both, so it's copied.

import os
import re
import shutil
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "iClassic-Square", ".rockbox")
LIGHT = os.path.join(ROOT, "wps", "iClassic-Square")
DARK = os.path.join(ROOT, "wps", "iClassic-Square-Dark")

BG = (18, 18, 20)
INK = (242, 242, 246)
HEADER_BASE = 34      # the status bar, where the light one's gradient ends
HEADER_SCALE = 0.30
HEADER_HEIGHT = 18    # above its dark line
BAR_X, BAR_Y, BAR_W, BAR_H, BAR_REFLECTION = 58, 206, 204, 14, 9
REFLECTION_ALPHA = 0.26
ART = (12, 45, 148, 181)
# the placeholder's reflection, as make_menu_icons.py and the firmware make it
ART_REFLECTION_ROWS = 22
ART_REFLECTION_ALPHA = 128 / 256
PANEL_X = 160         # the panel beside the lists, dark in both

TEXT = {"000000": "F2F2F6", "666666": "B4B6BA", "888888": "8E9094", "8a8d8a": "8E9094",
        "767676": "8E9094"}
BACK = {"FFFFFF": "121214"}


def read_bmp(path, alpha=None):
    """Its rows of (r, g, b), and of a 32-bit one its alpha into the list alpha"""
    with open(path, "rb") as f:
        d = f.read()
    off, w, h, bpp = (struct.unpack_from("<I", d, 10)[0], struct.unpack_from("<i", d, 18)[0],
                      struct.unpack_from("<i", d, 22)[0], struct.unpack_from("<H", d, 28)[0])
    if bpp != 24 and (bpp != 32 or alpha is None):
        raise SystemExit(f"{path}: {bpp} bits")
    size = bpp // 8
    stride = (w * size + 3) & ~3
    rows = []
    for y in range(abs(h)):
        y_file = abs(h) - 1 - y if h > 0 else y
        o = off + y_file * stride
        rows.append([(d[o + x * size + 2], d[o + x * size + 1], d[o + x * size]) for x in range(w)])
        if bpp == 32:
            alpha.append([d[o + x * 4 + 3] for x in range(w)])
    return w, abs(h), rows


def write_bmp(path, rows, alpha=None):
    w, h = len(rows[0]), len(rows)
    size = 4 if alpha else 3
    stride = (w * size + 3) & ~3
    out = bytearray(b"BM" + struct.pack("<IHHI", 54 + stride * h, 0, 0, 54))
    out += struct.pack("<IiiHHIIiiII", 40, w, h, 1, size * 8, 0, stride * h, 2835, 2835, 0, 0)
    for y in range(h - 1, -1, -1):
        line = bytearray()
        for x, (r, g, b) in enumerate(rows[y]):
            line += bytes((b, g, r) + ((alpha[y][x],) if alpha else ()))
        out += line + bytes(stride - w * size)
    with open(path, "wb") as f:
        f.write(out)


def luma(p):
    return (54 * p[0] + 183 * p[1] + 19 * p[2]) >> 8


def mix(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def clamp(v):
    return max(0, min(255, int(round(v))))


def header_tone(p):
    """The status bar's gradient stays the way up it is, in a dark grey"""
    return tuple(clamp(HEADER_BASE + (c - 196) * HEADER_SCALE) for c in p)


def is_coloured(p):
    hi, lo = max(p), min(p)
    s = hi + lo
    return hi - lo > 25 and s and (hi - lo) * 1000 / (510 - s if s > 255 else s) > 400


def header_pixel(p):
    """What's in the status bar: the bar itself, or an icon on it, whose dark parts turn
    light, and whose colours stay, as the play symbol and the charge are clear on dark"""
    if p == (255, 0, 255) or is_coloured(p):
        return p
    l = luma(p)
    if l >= 191:
        return header_tone(p)
    return mix(header_tone((191, 191, 191)), INK, min((191 - l) / 121, 1))


def glyph_pixel(p):
    """A grey image on the plain background: the lightest shades are its own background"""
    if p == (255, 0, 255) or is_coloured(p):
        return p
    return mix(INK, BG, min(luma(p) / 236, 1))


def reflected(p, t):
    return mix(BG, p, t)


def status_rows(rows, panel):
    """The status bar at the top of a backdrop. A panel beside the lists goes up to the
    top of the screen, and is dark in both themes."""
    w = PANEL_X if panel else len(rows[0])
    return [[header_pixel(p) for p in row[:w]] + row[w:] for row in rows[:HEADER_HEIGHT]] + \
           [[tuple(max(0, c - 100) // 8 for c in p) for p in rows[HEADER_HEIGHT][:w]]
            + rows[HEADER_HEIGHT][w:]]


def list_backdrop(src, dst, panel):
    w, h, rows = read_bmp(src)
    out = status_rows(rows, panel)
    for y in range(HEADER_HEIGHT + 1, h):
        out.append([rows[y][x] if panel and x >= PANEL_X else BG for x in range(w)])
    write_bmp(dst, out)


def now_playing_backdrop(src, dst):
    w, h, rows = read_bmp(src)
    out = status_rows(rows, False)
    for y in range(HEADER_HEIGHT + 1, h):
        row = []
        for x in range(w):
            p = rows[y][x]
            if ART[0] <= x < ART[2] and ART[1] <= y < ART[3]:
                row.append(mix((0, 0, 0), p, 0.7))              # the note, without a cover
            else:
                row.append(BG)
        out.append(row)
    for k in range(ART_REFLECTION_ROWS):
        t = ART_REFLECTION_ALPHA * (ART_REFLECTION_ROWS - (k + 0.5)) / ART_REFLECTION_ROWS
        for x in range(ART[0], ART[2]):
            out[ART[3] + k][x] = mix(BG, out[ART[3] - 1 - k][x], t)
    # the empty progress bar, and its reflection
    for y in range(BAR_H):
        for x in range(BAR_W):
            p = rows[BAR_Y + y][BAR_X + x]
            out[BAR_Y + y][BAR_X + x] = tuple(clamp(40 + (c - 205) * 0.55) for c in p)
    for y in range(BAR_REFLECTION):
        t = REFLECTION_ALPHA * (BAR_REFLECTION - y) / BAR_REFLECTION
        for x in range(BAR_W):
            out[BAR_Y + BAR_H + y][BAR_X + x] = reflected(out[BAR_Y + BAR_H - 1 - y][BAR_X + x], t)
    write_bmp(dst, out)


def progress_fill(src, dst):
    """Blue as it is, with its reflection on the dark background"""
    w, h, rows = read_bmp(src)
    out = rows[:BAR_H]
    for y in range(BAR_H, h):
        t = REFLECTION_ALPHA * (BAR_REFLECTION - (y - BAR_H)) / BAR_REFLECTION
        out.append([reflected(p, t) for p in rows[2 * BAR_H - 1 - y]])
    write_bmp(dst, out)


def each_pixel(src, dst, fn):
    alpha = []
    w, h, rows = read_bmp(src, alpha)
    write_bmp(dst, [[fn(p) for p in row] for row in rows], alpha)


# Now Playing's reflection under the cover: on the dark ground a cover's dark
# foot was lost at the default's half strength (owner, 2026-11-13); the light
# skin keeps %Cr's default, 22 rows from 128 of 256
DARK_REFLECTION = "%Cr(22,176)"


def skin(name):
    with open(os.path.join(ROOT, "wps", "iClassic-Square." + name), encoding="utf-8") as f:
        text = f.read()
    text = re.sub(r"^%Cr$", DARK_REFLECTION, text, flags=re.M)
    text = re.sub(r"%Vf\((\w{6})\)", lambda m: "%Vf(" + TEXT.get(m.group(1), m.group(1)) + ")", text)
    text = re.sub(r"%Vb\((\w{6})\)", lambda m: "%Vb(" + BACK.get(m.group(1), m.group(1)) + ")", text)
    with open(os.path.join(ROOT, "wps", "iClassic-Square-Dark." + name), "w", encoding="utf-8") as f:
        f.write("# Made from the light theme by themes/tools/make_dark_theme.py\n" + text)


def main():
    os.makedirs(DARK, exist_ok=True)
    for name in ("wps", "sbs", "fms"):
        skin(name)
    list_backdrop(os.path.join(ROOT, "backdrops", "iClassic_bd.bmp"),
                  os.path.join(ROOT, "backdrops", "iClassic_bd_dark.bmp"), True)
    list_backdrop(os.path.join(LIGHT, "About Backdrop.bmp"), os.path.join(DARK, "About Backdrop.bmp"), True)
    list_backdrop(os.path.join(LIGHT, "Wide Backdrop.bmp"), os.path.join(DARK, "Wide Backdrop.bmp"), False)
    now_playing_backdrop(os.path.join(LIGHT, "iClassic_bg.bmp"), os.path.join(DARK, "iClassic_bg.bmp"))
    progress_fill(os.path.join(LIGHT, "PB.bmp"), os.path.join(DARK, "PB.bmp"))
    for name in ("Battery", "Playing Status", "Hold Icon", "Status Night",
                 "Memory Access", "Radio Icon", "Stereo Icon"):
        each_pixel(os.path.join(LIGHT, name + ".bmp"), os.path.join(DARK, name + ".bmp"), header_pixel)
    # Now Playing's modes stand under the status bar, on the ground
    for name in ("Stars", "Ratings", "Signal", "Volume Left", "Volume Right", "Art Mask",
                 "Scrub Hints", "Repeat Icon", "Shuffle Icon", "Status Shuffle Albums"):
        each_pixel(os.path.join(LIGHT, name + ".bmp"), os.path.join(DARK, name + ".bmp"), glyph_pixel)
    for name in ("Scrub Knob", "Disk Fill", "Photos Icon"):
        shutil.copy(os.path.join(LIGHT, name + ".bmp"), os.path.join(DARK, name + ".bmp"))


if __name__ == "__main__":
    main()
