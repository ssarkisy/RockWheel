#!/usr/bin/env python3
"""A theme's glyphs from the mockups' export into what the firmware draws:

    themes/tools/make_theme_glyphs.py <folder with manifest.json and <theme>/*.png> [<theme>]

The folder is the export of the mockups' own drawing code (names, sizes and
frames in its manifest.json: nothing of them is written here), but for
Playlists' marks, drawn here (catalogue_marks()). A mask is black
with alpha, its frames stacked top to bottom; a coloured glyph (the notes of
a missing cover) is a file a look, as drawn, with a mask of its shape beside
it as "<Name> Mask": the firmware draws the mask in the theme's two colours
for a note, so the coloured files are not read. Written, for theme "neo":

- apps/gui/neo_glyphs.h: the masks the lists draw themselves (LIST below), as
  tables of alpha in the format of an anti-aliased font's glyphs (4 bits from
  0 opaque to 15 clear, low nibble first, rows run on), with each one's
  width, height and count of frames; of a coloured glyph its mask;
- themes/Neo/.rockbox/wps/Neo/<Name>.bmp and Neo-Dark/<Name>.bmp: the masks
  the skins show (SKIN below), in the colour each has in that look, 32-bit
  with the mask as alpha, so they sit on any ground; frames top to bottom
  as %xl takes them; of a coloured glyph its mask, which the skin draws as
  a note (%xN). And the bar's two capsules, which are no glyphs.
- themes/tools/neo_glyph_sizes.txt: their sizes, for make_neo_theme.py,
  which places them.

Run again after the export changes; the manifest's sha256 is in the header.
"""
import hashlib
import json
import math
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")

# what the list code draws, by the manifest's names; "Catalogue Marks" is drawn
# here (catalogue_marks() below), not exported
LIST = ("Chevron", "Speaker", "Catalogue Marks", "Smart Gear", "Episode Dots",
        "Note Small", "Note Header", "Check", "Plus", "Search Keys")
# its width in Neo, a pixel narrower than Classic's and Modern's, as Neo's
# glyphs are; and "Smart Gear" (smart_gear() below)
NEO_CATALOGUE_W = 12
# what the screen shown over USB draws (apps/gui/neo_usb_glyphs.h): the sign
# while connected and the one once it may be unplugged; of the status line's
# battery the frames charging and charged
USB = (("USB", None), ("USB Check", None), ("Battery", (0, 2)))
# what the skins show: name -> the role of its colour
# (the status line's are the small text's grey: the third was too pale on the LCD)
SKIN = {"Battery": "DENSE", "Playing Status": "DENSE", "Hold": "DENSE",
        "Night": "DENSE", "Repeat": "DENSE", "Shuffle": "DENSE",
        "Knob": "SECONDARY", "Volume Left": "TERTIARY",
        "Volume Right": "TERTIARY", "Note Playing": "SECONDARY",
        "Note Large": "SECONDARY", "Note Pane": "SECONDARY",
        "Photos Pane": "TERTIARY"}
# the set stars over the unset ones, in one bitmap: name -> its ground,
# whether a first frame of the ground alone is added (the small ones have no
# frame of none), the roles of the set and the unset ones' colours, and
# whether the ground is an outline. An outline is left out wherever a set
# star has ink, so a pixel is wholly the one colour or the other: the
# firmware knows the outline by its grey (UNSET) and draws it in the user's
# colour. So no other pixel of a picture the skin marks %xC (these two) may
# come to that grey in the screen's 16 bits: checked below
STARS = {"Ratings": ("Ratings Ground", True, "PRIMARY", "UNSET", True),
         "Stars": ("Stars Ground", False, "DENSE", "TERTIARY", False)}
# Now Playing's bar: its track and its fill, capsules 9 px thick in a box as
# high as the knob, which the skin's bar draws inside its own height; and the
# volume's, shorter between its marks
BAR = (292, 11, 9)
VOLUME_BAR = 256
LOOKS = {  # as apps/gui/ui_theme.c and make_neo_theme.py have them
    "Neo":      dict(PAPER="EFEBE7", PRIMARY="292421", SECONDARY="6B6963",
                     TERTIARY="A59E94", TRACK="E7DFD6", SELECTED="DEDBD6",
                     DENSE="52514A", BARTRACK="D6D3CE", UNSET="8C8E8C"),
    "Neo-Dark": dict(PAPER="211C18", PRIMARY="EFEBE7", SECONDARY="A59E94",
                     TERTIARY="6B6963", TRACK="313029", SELECTED="312C29",
                     DENSE="B5B2A5", BARTRACK="423C39", UNSET="7B797B"),
}


def read_png(path, colours=False):
    """rows of alpha of an 8-bit RGBA PNG, not interlaced, or of (r, g, b, a)"""
    b = open(path, "rb").read()
    assert b[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, data, w = 8, b"", 0
    while pos < len(b):
        n, kind = struct.unpack(">I4s", b[pos:pos + 8])
        chunk = b[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
            assert (depth, colour, interlace) == (8, 6, 0), (path, depth, colour, interlace)
        elif kind == b"IDAT":
            data += chunk
        pos += 12 + n
    raw = zlib.decompress(data)
    stride, bpp = w * 4, 4
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            up = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + up) & 255
            elif f == 3:
                line[i] = (line[i] + (a + up) // 2) & 255
            elif f == 4:
                p = a + up - c
                pa, pb, pc = abs(p - a), abs(p - up), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else up if pb <= pc else c)) & 255
        rows.append([tuple(line[i:i + 4]) for i in range(0, stride, 4)] if colours
                    else bytes(line[3::4]))
        prev = line
    return w, h, rows


def ident(name):
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


def table(rows):
    nib = [15 - (a * 15 + 127) // 255 for row in rows for a in row]
    if len(nib) & 1:
        nib.append(15)
    return bytes(nib[i] | nib[i + 1] << 4 for i in range(0, len(nib), 2))


def bmp(path, w, h, ink, rows):
    """rows of alpha, top to bottom, in the colour ink, or rows of (r, g, b, a):
    32-bit with alpha"""
    if ink is None:
        data = b"".join(b"".join(bytes((b_, g, r, a)) for r, g, b_, a in row)
                        for row in reversed(rows))
    else:
        r, g, b_ = ink
        data = b"".join(b"".join(bytes((b_, g, r, a)) for a in row) for row in reversed(rows))
    with open(path, "wb") as f:
        f.write(b"BM" + struct.pack("<IHHI", 54 + len(data), 0, 0, 54))
        f.write(struct.pack("<IiiHHIIiiII", 40, w, h, 1, 32, 0, len(data), 2835, 2835, 0, 0))
        f.write(data)


def rgb(hexs):
    return tuple(int(hexs[i:i + 2], 16) for i in (0, 2, 4))


def rgb565(colour):
    """the colour's 16 bits as the firmware's loader makes them, rounded
    (apps/recorder/bmp.c, no dither)"""
    r, g, b = colour[:3]
    return ((31 * r + (r >> 3) + 127) >> 8, (63 * g + (g >> 2) + 127) >> 8,
            (31 * b + (b >> 3) + 127) >> 8)


def shape(width, height, inside):
    """rows of alpha of a shape, by how many of 4 by 4 points of each pixel
    inside(x, y) takes in"""
    rows = []
    for y in range(height):
        row = bytearray(width)
        for x in range(width):
            n = sum(inside(x + (i + 0.5) / 4, y + (j + 0.5) / 4)
                    for i in range(4) for j in range(4))
            row[x] = (n * 255 + 8) // 16
        rows.append(bytes(row))
    return rows


def capsule(width, height, thick):
    """a bar thick px high in the middle of the height, its ends round"""
    r = thick / 2
    top = (height - thick) // 2
    middle = top + r

    def inside(x, y):
        cx = min(max(x, r), width - r)
        return (x - cx) ** 2 + (y - middle) ** 2 <= r * r
    return shape(width, height, inside)


def disc(size, height):
    """a round dot size px across, in the middle of the height"""
    r = size / 2
    top = (height - size) // 2

    def inside(x, y):
        return (x - r) ** 2 + (y - top - r) ** 2 <= r * r
    return shape(size, height, inside)


def catalogue_marks(w):
    """Playlists' marks before the names, filled, which read better than
    outlines at this size: two frames of w by 11, one for every playlist
    whatever made it, three lines and a note as Music's, and one for every
    folder. The lines 2 px thick with round ends on whole rows; the note's
    stem 1.4 px, its flag a band down to the right, its head an oval tilted
    up. As a mask, a glyph's dict as the manifest has it, and its rows"""
    k = w - 13

    def ellipse(x, y, cx, cy, rx, ry, angle):
        c, s = math.cos(angle), math.sin(angle)
        u, v = (x - cx) * c + (y - cy) * s, -(x - cx) * s + (y - cy) * c
        return (u / rx) ** 2 + (v / ry) ** 2 <= 1

    def line(x, y, right, top):
        r = 1
        cx = min(max(x, r), right - r)
        return (x - cx) ** 2 + (y - top - r) ** 2 <= r * r

    def rounded(x, y, x0, y0, x1, y1, r):
        if x < x0 or x > x1 or y < y0 or y > y1:
            return False
        cx, cy = min(max(x, x0 + r), x1 - r), min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r

    def playlist(x, y):
        if any(line(x, y, right, top) for top, right in ((1, 7 + k), (4, 7 + k), (7, 4 + k))):
            return True
        sx = 10.1 + k
        if sx <= x <= sx + 1.4 and 0.3 <= y <= 9:
            return True
        d = (x - sx - 1.4) * 0.9
        if sx + 1.4 <= x <= w and 0.3 + d <= y <= 2.4 + d:
            return True
        return ellipse(x, y, sx - 0.95, 9.0, 2.2, 1.7, math.radians(-22))

    def folder(x, y):
        # its tab at the top left, its right side slanting, on the body
        if rounded(x, y, 0, 0, 7 + k, 4, 1.0) and x - 5 - k <= y:
            return True
        return rounded(x, y, 0, 2, w, 11, 1.5)

    g = {"name": "Catalogue Marks", "width": w, "height": 11, "frames": 2, "kind": "mask"}
    return g, shape(w, 11, playlist) + shape(w, 11, folder)


def gear(cx, cy, r_out, r_body, r_hole, teeth, tooth_w, phase):
    """inside a filled gear: a disc of r_body with teeth tooth_w across out to
    r_out, the first at phase, a hole of r_hole at its centre"""
    def inside(x, y):
        dx, dy = x - cx, y - cy
        r = math.hypot(dx, dy)
        if r <= r_hole or r > r_out:
            return False
        if r <= r_body:
            return True
        step = 2 * math.pi / teeth
        a = (math.atan2(dy, dx) - phase + step / 2) % step - step / 2
        return abs(r * math.sin(a)) <= tooth_w / 2 and math.cos(a) > 0
    return inside


def smart_gear():
    """The mark of Finder's smart playlists in Playlists, as Music marks them:
    a filled gear, 11 by 11, eight teeth on the grid (owner, 2026-11-13; a
    small gear on the playlist's mark couldn't be made out). As a mask, a
    glyph's dict as the manifest has it, and its rows"""
    g = {"name": "Smart Gear", "width": 11, "height": 11, "frames": 1, "kind": "mask"}
    return g, shape(11, 11, gear(5.5, 5.5, 5.5, 3.7, 1.5, 8, 2.0, 0))


def main():
    folder = sys.argv[1]
    theme = sys.argv[2] if len(sys.argv) > 2 else "neo"
    assert theme == "neo", "only Neo's are taken yet"
    manifest_path = os.path.join(folder, "manifest.json")
    digest = hashlib.sha256(open(manifest_path, "rb").read()).hexdigest()
    glyphs = {g["name"]: g for g in json.load(open(manifest_path))["themes"][theme]}

    def load(name, look=None):
        """a mask's alpha; with look, a coloured glyph's pixels in that look"""
        g = glyphs[name]
        if look is None and g["kind"] == "coloured":
            g = dict(glyphs[name + " Mask"], name=name)
        assert g["kind"] == ("mask" if look is None else "coloured"), name
        file = g["files"][0] if look is None else next(
            f for f in g["files"] if "/%s/" % look in f)
        w, h, rows = read_png(os.path.join(folder, file), look is not None)
        assert w == g["width"] and h == g["height"] * g["frames"], (name, w, h)
        return g, rows

    out = ["/* Made by themes/tools/make_theme_glyphs.py from the mockups' export of Neo's",
           "   glyphs (manifest.json sha256 %s):" % digest,
           "   made anew when that changes, not edited. Alpha as an anti-aliased font's",
           "   glyphs have it; frames one under another. */"]
    drawn = {"Catalogue Marks": catalogue_marks(NEO_CATALOGUE_W),
             "Smart Gear": smart_gear()}
    for name in LIST:
        g, rows = drawn[name] if name in drawn else load(name)
        c = ident(name)
        data = table(rows)
        out.append("#define NEO_%s_W %d" % (c.upper(), g["width"]))
        out.append("#define NEO_%s_H %d" % (c.upper(), g["height"]))
        out.append("#define NEO_%s_FRAMES %d" % (c.upper(), g["frames"]))
        out.append("static const unsigned char neo_%s_alpha[] = {" % c)
        for i in range(0, len(data), 12):
            out.append("    " + " ".join("0x%02x," % v for v in data[i:i + 12]))
        out.append("};")
    with open(os.path.join(ROOT, "apps", "gui", "neo_glyphs.h"), "w") as f:
        f.write("\n".join(out) + "\n")

    # the screen shown over USB is built in, as no file can be read then
    out = out[:4]
    for name, kept in USB:
        g, rows = load(name)
        h = g["height"]
        if kept:
            rows = [row for f in kept for row in rows[f * h:(f + 1) * h]]
        c = ident(name)
        data = table(rows)
        out.append("#define NEO_USB_%s_W %d" % (c.upper(), g["width"]))
        out.append("#define NEO_USB_%s_H %d" % (c.upper(), h))
        out.append("static const unsigned char neo_usb_%s_alpha[] = {" % c)
        for i in range(0, len(data), 12):
            out.append("    " + " ".join("0x%02x," % v for v in data[i:i + 12]))
        out.append("};")
    with open(os.path.join(ROOT, "apps", "gui", "neo_usb_glyphs.h"), "w") as f:
        f.write("\n".join(out) + "\n")

    sizes = []
    for look, colours in LOOKS.items():
        folder_out = os.path.join(ROOT, "themes", "Neo", ".rockbox", "wps", look)
        os.makedirs(folder_out, exist_ok=True)
        for name, role in SKIN.items():
            g, rows = load(name)
            bmp(os.path.join(folder_out, name + ".bmp"), g["width"], len(rows),
                rgb(colours[role]) if role else None, rows)
            if look == "Neo":
                sizes.append("%s %d %d %d" % (name.replace(" ", "_"), g["width"], g["height"],
                                              g["frames"]))
        for name, (ground_name, none, set_role, unset_role, outline) in STARS.items():
            g, rows = load(name)
            gg, ground = load(ground_name)
            assert (gg["width"], gg["height"]) == (g["width"], g["height"]), name
            h = g["height"]
            ink, quiet = rgb(colours[set_role]), rgb(colours[unset_role])
            out = []
            for f in range(-1 if none else 0, g["frames"]):
                for y in range(h):
                    row = []
                    for x in range(g["width"]):
                        a = rows[f * h + y][x] if f >= 0 else 0
                        under = ground[y][x] * (255 - a) // 255
                        if outline and a:
                            under = 0
                        total = a + under
                        row.append(tuple((ink[k] * a + quiet[k] * under) // max(total, 1)
                                         for k in range(3)) + (total,))
                        is_outline = outline and under > 0
                        assert is_outline or not total or rgb565(row[-1]) != rgb565(
                            rgb(colours["UNSET"])), (
                            "%s (%s) frame %d (%d, %d): a pixel that is no outline has the "
                            "outline's grey" % (name, look, f, x, y))
                    out.append(row)
            bmp(os.path.join(folder_out, name + ".bmp"), g["width"], len(out), None, out)
            if look == "Neo":
                sizes.append("%s %d %d %d" % (name, g["width"], h, len(out) // h))
        # the capsules, their ends round; the track a step darker than the
        # ground, which the LCD shows
        w, h, thick = BAR
        for name, width in (("Bar", w), ("Volume", VOLUME_BAR)):
            rows = capsule(width, h, thick)
            bmp(os.path.join(folder_out, name + " Track.bmp"), width, h,
                rgb(colours["BARTRACK"]), rows)
            bmp(os.path.join(folder_out, name + " Fill.bmp"), width, h,
                rgb(colours["SECONDARY"]), rows)
    sizes.append("Bar %d %d 1" % BAR[:2])
    sizes.append("Volume_Bar %d %d 1" % (VOLUME_BAR, BAR[1]))
    # for make_neo_theme.py, which places them by these
    with open(os.path.join(HERE, "neo_glyph_sizes.txt"), "w") as f:
        f.write("# name width height frames, from manifest.json sha256 %s\n" % digest)
        f.write("\n".join(sizes) + "\n")


if __name__ == "__main__":
    main()
