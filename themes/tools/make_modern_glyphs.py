#!/usr/bin/env python3
"""Modern's glyphs from the mockups' export into what the firmware draws:

    themes/tools/make_modern_glyphs.py <folder with manifest.json and modern/*.png>

As make_theme_glyphs.py does Neo's, whose reader and writers it uses: names,
sizes and frames are the manifest's. Written:

- apps/gui/modern_glyphs.h: the masks the lists draw themselves (LIST below),
  as tables of alpha;
- apps/gui/modern_usb_glyphs.h: the masks of the screen shown over USB (USB
  below), built in as no file can be read then;
- themes/Modern/.rockbox/wps/Modern/<Name>.bmp and Modern-Dark/<Name>.bmp:
  the masks the skins show (SKIN below) in the colour each has in that look,
  32-bit with the mask as alpha; the battery as its look's file has it;
- themes/tools/modern_glyph_sizes.txt: their sizes, for make_modern_theme.py.

Run again after the export changes; the manifest's sha256 is in the header.
"""
import hashlib
import json
import os
import sys

from make_theme_glyphs import HERE, ROOT, bmp, capsule, disc, ident, read_png, rgb, table

LIST = ("Chevron", "Speaker", "Source", "Episode Dots", "Note Small", "Note Header",
        "Check", "Plus", "Search Keys", "Search Field", "Music Marks")
# what the screen shown over USB draws. The sign is three masks of one canvas:
# the computer with its cable, the prohibition mark on its screen while it is
# connected and the check once it may be unplugged; each is kept as its ink's
# box, with the box's place in the canvas. The battery's frame, and its charge
# charging and charged (frames 0 and 2)
USB_SIGN = ("USB", "USB Stop", "USB Check")
USB = (("Battery Frame", None), ("Battery Charge", (0, 2)))
# what the skins show: name -> the role of its colour, None for a coloured one
SKIN = {"Battery": None, "Playing Status": "ACCENT", "Hold": "ACCENT",
        "Night": "ACCENT", "Photos Pane": "MUTED", "Note Pane": None,
        "Repeat": "ACCENTTEXT", "Shuffle": "ACCENTTEXT", "Knob": "ACCENT",
        "Volume Left": "SMALL", "Note Playing": None, "Note Large": None}
# the set stars over the five unset ones' outlines, in one bitmap: name -> its
# ground, the set ones' colour, and whether a first frame of the ground alone
# is added (the small ones have no frame of none)
STARS = {"Ratings": ("Ratings Ground", "SMALL", True),
         "Stars": ("Stars Ground", "ACCENT", False)}
# Now Playing's bars, track and fill: a capsule 9 px thick in a box as high as
# the knob, which the skin's bar draws inside its own height; and the lyrics
# view's, as thick, without a knob
BARS = {"Bar": (296, 11, 9), "Thick Bar": (296, 9, 9)}
# the knob at rest, a dot as thick as the bar at the fill's end: a slider of
# the bar, so a picture as high as the bar's box, drawn here (the export's is
# the 5 px bar's)
KNOB_SMALL = (9, 11)
LOOKS = {  # as apps/gui/ui_theme.c and make_modern_theme.py have them
    "Modern":      dict(GROUND="FFFFFF", INK="000000", SMALL="525152",
                        MUTED="73757B", ACCENT="FF2D52", ACCENTTEXT="DE244A",
                        TRACK="DEE2E6"),
    "Modern-Dark": dict(GROUND="000000", INK="FFFFFF", SMALL="B4B6BD",
                        MUTED="8C8E94", ACCENT="FF345A", ACCENTTEXT="FF345A",
                        TRACK="393839"),
}


def main():
    folder = sys.argv[1]
    manifest_path = os.path.join(folder, "manifest.json")
    digest = hashlib.sha256(open(manifest_path, "rb").read()).hexdigest()
    glyphs = {g["name"]: g for g in json.load(open(manifest_path))["themes"]["modern"]}

    def load(name, look=None):
        """a mask's alpha; with look, a coloured glyph's pixels in that look"""
        g = glyphs[name]
        assert g["kind"] == ("mask" if look is None else "coloured"), name
        file = g["files"][0] if look is None else next(
            f for f in g["files"] if "/%s/" % look in f)
        w, h, rows = read_png(os.path.join(folder, file), look is not None)
        assert w == g["width"] and h == g["height"] * g["frames"], (name, w, h)
        return g, rows

    out = ["/* Made by themes/tools/make_modern_glyphs.py from the mockups' export of",
           "   Modern's glyphs (manifest.json sha256",
           "   %s): made anew when" % digest,
           "   that changes, not edited. Alpha as an anti-aliased font's glyphs have it;",
           "   frames one under another. */"]
    for name in LIST:
        g, rows = load(name)
        c = ident(name)
        data = table(rows)
        out.append("#define MODERN_%s_W %d" % (c.upper(), g["width"]))
        out.append("#define MODERN_%s_H %d" % (c.upper(), g["height"]))
        out.append("#define MODERN_%s_FRAMES %d" % (c.upper(), g["frames"]))
        out.append("static const unsigned char modern_%s_alpha[] = {" % c)
        for i in range(0, len(data), 12):
            out.append("    " + " ".join("0x%02x," % v for v in data[i:i + 12]))
        out.append("};")
    with open(os.path.join(ROOT, "apps", "gui", "modern_glyphs.h"), "w") as f:
        f.write("\n".join(out) + "\n")

    out = out[:5]
    canvas = None
    for name in USB_SIGN:
        g, rows = load(name)
        assert g["frames"] == 1 and canvas in (None, (g["width"], g["height"])), name
        canvas = (g["width"], g["height"])
        ys = [y for y, row in enumerate(rows) if any(row)]
        xs = [x for row in rows for x, a in enumerate(row) if a]
        x0, x1, y0, y1 = min(xs), max(xs) + 1, ys[0], ys[-1] + 1
        c = ident(name)
        data = table([row[x0:x1] for row in rows[y0:y1]])
        for key, value in (("X", x0), ("Y", y0), ("W", x1 - x0), ("H", y1 - y0)):
            out.append("#define MODERN_USB_%s_%s %d" % (c.upper(), key, value))
        out.append("static const unsigned char modern_usb_%s_alpha[] = {" % c)
        for i in range(0, len(data), 12):
            out.append("    " + " ".join("0x%02x," % v for v in data[i:i + 12]))
        out.append("};")
    out.append("#define MODERN_USB_SIGN_W %d" % canvas[0])
    out.append("#define MODERN_USB_SIGN_H %d" % canvas[1])
    for name, kept in USB:
        g, rows = load(name)
        h = g["height"]
        if kept:
            rows = [row for f in kept for row in rows[f * h:(f + 1) * h]]
        c = ident(name)
        data = table(rows)
        out.append("#define MODERN_USB_%s_W %d" % (c.upper(), g["width"]))
        out.append("#define MODERN_USB_%s_H %d" % (c.upper(), h))
        out.append("static const unsigned char modern_usb_%s_alpha[] = {" % c)
        for i in range(0, len(data), 12):
            out.append("    " + " ".join("0x%02x," % v for v in data[i:i + 12]))
        out.append("};")
    with open(os.path.join(ROOT, "apps", "gui", "modern_usb_glyphs.h"), "w") as f:
        f.write("\n".join(out) + "\n")

    sizes = []
    for look, colours in LOOKS.items():
        folder_out = os.path.join(ROOT, "themes", "Modern", ".rockbox", "wps", look)
        os.makedirs(folder_out, exist_ok=True)
        for name, role in SKIN.items():
            g, rows = load(name, None if role else "dark" if "Dark" in look else "light")
            bmp(os.path.join(folder_out, name + ".bmp"), g["width"], len(rows),
                rgb(colours[role]) if role else None, rows)
            if look == "Modern":
                sizes.append("%s %d %d %d" % (name.replace(" ", "_"), g["width"],
                                              g["height"], g["frames"]))
        # the stars: each frame's set ones over the ground's
        for name, (ground_name, role, none) in STARS.items():
            g, rows = load(name)
            gg, ground = load(ground_name)
            assert (gg["width"], gg["height"]) == (g["width"], g["height"]), name
            h = g["height"]
            ink, quiet = rgb(colours[role]), rgb(colours["MUTED"])
            stars = []
            for f in range(-1 if none else 0, g["frames"]):
                for y in range(h):
                    row = []
                    for x in range(g["width"]):
                        a = rows[f * h + y][x] if f >= 0 else 0
                        under = ground[y][x] * (255 - a) // 255
                        total = a + under
                        row.append(tuple((ink[k] * a + quiet[k] * under) // max(total, 1)
                                         for k in range(3)) + (total,))
                    stars.append(row)
            bmp(os.path.join(folder_out, name + ".bmp"), g["width"], len(stars), None, stars)
            if look == "Modern":
                sizes.append("%s %d %d %d" % (name, g["width"], h, len(stars) // h))
        # the bars, their ends round, and the dot at rest
        for name, (w, h, thick) in BARS.items():
            rows = capsule(w, h, thick)
            bmp(os.path.join(folder_out, name + " Track.bmp"), w, h,
                rgb(colours["TRACK"]), rows)
            bmp(os.path.join(folder_out, name + " Fill.bmp"), w, h,
                rgb(colours["ACCENT"]), rows)
        size, h = KNOB_SMALL
        bmp(os.path.join(folder_out, "Knob Small.bmp"), size, h, rgb(colours["ACCENT"]),
            disc(size, h))
    sizes.append("Knob_Small %d %d 1" % (KNOB_SMALL[0], KNOB_SMALL[0]))
    for name, (w, h, thick) in BARS.items():
        sizes.append("%s %d %d 1" % (name.replace(" ", "_"), w, h))
    with open(os.path.join(HERE, "modern_glyph_sizes.txt"), "w") as f:
        f.write("# name width height frames, from manifest.json sha256 %s\n" % digest)
        f.write("\n".join(sizes) + "\n")


if __name__ == "__main__":
    main()
