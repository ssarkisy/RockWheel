#!/usr/bin/env python3
"""A theme's glyphs from the mockups' export into what the firmware draws:

    themes/tools/make_theme_glyphs.py <folder with manifest.json and <theme>/*.png> [<theme>]

The folder is the export of the mockups' own drawing code (names, sizes and
frames in its manifest.json: nothing of them is written here). A mask is black
with alpha, its frames stacked top to bottom. Written, for theme "neo":

- apps/gui/neo_glyphs.h: the masks the lists draw themselves (LIST below), as
  tables of alpha in the format of an anti-aliased font's glyphs (4 bits from
  0 opaque to 15 clear, low nibble first, rows run on), with each one's
  width, height and count of frames;
- themes/Neo/.rockbox/wps/Neo/<Name>.bmp and Neo-Dark/<Name>.bmp: the masks
  the skins show (SKIN below), in the colour each has in that look, 32-bit
  with the mask as alpha, so they sit on any ground; frames top to bottom
  as %xl takes them. And the bar's two capsules, which are no glyphs.
- themes/tools/neo_glyph_sizes.txt: their sizes, for make_neo_theme.py,
  which places them.

Run again after the export changes; the manifest's sha256 is in the header.
"""
import hashlib
import json
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")

# what the list code draws, by the manifest's names
LIST = ("Chevron", "Speaker", "Shuffle", "Source", "Episode Dots", "Note Small",
        "Note Header", "Check", "Plus", "Search Keys")
# what the skins show: name -> the role of its colour
SKIN = {"Battery": "TERTIARY", "Playing Status": "TERTIARY", "Hold": "TERTIARY",
        "Night": "TERTIARY", "Repeat": "TERTIARY", "Shuffle": "TERTIARY",
        "Knob": "SECONDARY", "Volume Left": "TERTIARY",
        "Volume Right": "TERTIARY", "Note Playing": "TERTIARY", "Note Large": "TERTIARY",
        "Note Pane": "TERTIARY", "Photos Pane": "TERTIARY"}
# the set stars over the unset ones, in one bitmap: name -> its ground, and
# whether a first frame of the ground alone is added (the small ones have no
# frame of none)
STARS = {"Ratings": ("Ratings Ground", True), "Stars": ("Stars Ground", False)}
# Now Playing's bar: its track and its fill, capsules 3 px thick in a box as
# high as the knob, which the skin's bar draws inside its own height
BAR = (216, 11, 3)
LOOKS = {  # as apps/gui/ui_theme.c and make_neo_theme.py have them
    "Neo":      dict(PAPER="EFEBE7", PRIMARY="292421", SECONDARY="6B6963",
                     TERTIARY="A59E94", TRACK="E7DFD6", SELECTED="DEDBD6"),
    "Neo-Dark": dict(PAPER="211C18", PRIMARY="EFEBE7", SECONDARY="A59E94",
                     TERTIARY="6B6963", TRACK="313029", SELECTED="312C29"),
}


def read_png(path):
    """rows of alpha of an 8-bit RGBA PNG, not interlaced"""
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
        rows.append(bytes(line[3::4]))
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


def main():
    folder = sys.argv[1]
    theme = sys.argv[2] if len(sys.argv) > 2 else "neo"
    assert theme == "neo", "only Neo's are taken yet"
    manifest_path = os.path.join(folder, "manifest.json")
    digest = hashlib.sha256(open(manifest_path, "rb").read()).hexdigest()
    glyphs = {g["name"]: g for g in json.load(open(manifest_path))["themes"][theme]}

    def load(name):
        g = glyphs[name]
        assert g["kind"] == "mask", name
        w, h, rows = read_png(os.path.join(folder, g["files"][0]))
        assert w == g["width"] and h == g["height"] * g["frames"], (name, w, h)
        return g, rows

    out = ["/* Made by themes/tools/make_theme_glyphs.py from the mockups' export of Neo's",
           "   glyphs (manifest.json sha256 %s):" % digest,
           "   made anew when that changes, not edited. Alpha as an anti-aliased font's",
           "   glyphs have it; frames one under another. */"]
    for name in LIST:
        g, rows = load(name)
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

    sizes = []
    for look, colours in LOOKS.items():
        folder_out = os.path.join(ROOT, "themes", "Neo", ".rockbox", "wps", look)
        os.makedirs(folder_out, exist_ok=True)
        for name, role in SKIN.items():
            g, rows = load(name)
            bmp(os.path.join(folder_out, name + ".bmp"), g["width"], len(rows),
                rgb(colours[role]), rows)
            if look == "Neo":
                sizes.append("%s %d %d %d" % (name.replace(" ", "_"), g["width"], g["height"],
                                              g["frames"]))
        for name, (ground_name, none) in STARS.items():
            g, rows = load(name)
            gg, ground = load(ground_name)
            assert (gg["width"], gg["height"]) == (g["width"], g["height"]), name
            h = g["height"]
            ink, quiet = rgb(colours["SECONDARY"]), rgb(colours["TERTIARY"])
            out = []
            for f in range(-1 if none else 0, g["frames"]):
                for y in range(h):
                    row = []
                    for x in range(g["width"]):
                        a = rows[f * h + y][x] if f >= 0 else 0
                        under = ground[y][x] * (255 - a) // 255
                        total = a + under
                        row.append(tuple((ink[k] * a + quiet[k] * under) // max(total, 1)
                                         for k in range(3)) + (total,))
                    out.append(row)
            bmp(os.path.join(folder_out, name + ".bmp"), g["width"], len(out), None, out)
            if look == "Neo":
                sizes.append("%s %d %d %d" % (name, g["width"], h, len(out) // h))
        # a capsule 3 px thick is whole pixels: a bar with its four corners cut
        w, h, thick = BAR
        top = (h - thick) // 2
        capsule = [bytes(0 if not top <= y < top + thick
                         or (x in (0, w - 1) and y in (top, top + thick - 1)) else 255
                         for x in range(w)) for y in range(h)]
        bmp(os.path.join(folder_out, "Bar Track.bmp"), w, h, rgb(colours["TRACK"]), capsule)
        bmp(os.path.join(folder_out, "Bar Fill.bmp"), w, h, rgb(colours["SECONDARY"]), capsule)
    sizes.append("Bar %d %d 1" % BAR[:2])
    # for make_neo_theme.py, which places them by these
    with open(os.path.join(HERE, "neo_glyph_sizes.txt"), "w") as f:
        f.write("# name width height frames, from manifest.json sha256 %s\n" % digest)
        f.write("\n".join(sizes) + "\n")


main()
