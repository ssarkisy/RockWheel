#!/usr/bin/env python3
"""What each of the themes' fonts has: its line, its size, and of each alphabet
how many letters and how tall their ink is, read from the .fnt files as the
firmware reads them.

    themes/tools/font_coverage.py [<fonts folder>] [--sheet <out.bmp>]

Without a folder, themes/fonts/.rockbox/fonts. A letter a font hasn't is drawn
from 16-GNU-Unifont by the firmware, so "0" under an alphabet means that. With
--sheet, a line of each of seven languages in every font, as its glyphs are,
into a BMP: letters in the order they are read, right to left for Hebrew and
Arabic, Arabic's not joined (the firmware joins them, firmware/arabjoin.c).
"""
import os
import struct
import sys

HEADER = 36
SCRIPTS = (
    ("Latin", ((0x20, 0x24F),)),
    ("Greek", ((0x370, 0x3FF),)),
    ("Cyrillic", ((0x400, 0x4FF),)),
    ("Hebrew", ((0x590, 0x5FF),)),
    ("Arabic", ((0x600, 0x6FF), (0xFB50, 0xFDFF), (0xFE70, 0xFEFF))),
    ("Thai", ((0xE00, 0xE7F),)),
    ("Kana", ((0x3040, 0x30FF),)),
    ("Han", ((0x4E00, 0x9FFF),)),
    ("Hangul", ((0x3131, 0x318E), (0xAC00, 0xD7A3))),
    ("Forms", ((0x10000, 0x1FFFF),)),  # the ideographs' other forms, past U+FFFF
)
# a letter of each whose ink is as tall as its alphabet's are
TALL = {"Latin": "H", "Greek": "Η", "Cyrillic": "Н", "Hebrew": "ה", "Arabic": "ا",
        "Thai": "ก", "Kana": "あ", "Han": "国", "Hangul": "한"}
SAMPLES = (
    ("English", "Now Playing 12:45 Shuffle Songs", False),
    ("Russian", "Сейчас исполняется Перемешать", False),
    ("Japanese", "再生中 シャッフル アルバム 国", False),
    ("Korean", "지금 재생 중 임의 재생 앨범", False),
    ("Hebrew", "מתנגן כעת ערבוב שירים", True),
    ("Arabic", "قيد التشغيل الآن ألبومات", True),
    ("Thai", "กำลังเล่น สุ่มเพลง อัลบั้ม", False),
)


class Fnt:
    def __init__(self, path):
        b = open(path, "rb").read()
        if b[:4] != b"RB12":
            raise ValueError("not an RB12 font")
        self.bytes = len(b)
        self.maxwidth, self.height, self.ascent, self.depth = struct.unpack_from("<4H", b, 4)
        self.first, self.default, self.size, nbits, noff, nwid = struct.unpack_from("<6I", b, 12)
        long_offsets = nbits >= 0xFFDB
        pos = HEADER + nbits
        pos = (pos + 3) & ~3 if long_offsets else (pos + 1) & ~1
        self.offsets = struct.unpack_from("<%d%s" % (noff, "I" if long_offsets else "H"), b, pos)
        pos += noff * (4 if long_offsets else 2)
        self.widths = b[pos:pos + nwid] if nwid else bytes([self.maxwidth]) * self.size
        self.bits = b[HEADER:HEADER + nbits]

    def has(self, c):
        i = c - self.first
        if not 0 <= i < self.size or self.widths[i] == 0:
            return False
        d = self.default - self.first
        return c == self.default or self.offsets[i] != self.offsets[d]

    def glyph(self, c):
        """rows of alpha, 0 opaque to 15 clear"""
        i = c - self.first
        w = self.widths[i]
        if self.depth != 1:
            return w, []
        raw = self.bits[self.offsets[i]:self.offsets[i] + (self.height * w + 1) // 2]
        nib = []
        for byte in raw:
            nib += [byte & 15, byte >> 4]
        return w, [nib[y * w:(y + 1) * w] for y in range(self.height)]

    def ink_height(self, c):
        _, rows = self.glyph(c)
        inked = [y for y, row in enumerate(rows) if row and min(row) < 8]
        return inked[-1] - inked[0] + 1 if inked else 0


def report(folder, names):
    print("%-22s %4s %3s %9s %6s  %s" % ("font", "line", "asc", "bytes", "glyphs",
                                         "  ".join("%s" % n for n, _ in SCRIPTS)))
    print("%-22s %4s %3s %9s %6s  (letters / ink of %s in px)"
          % ("", "", "", "", "", " ".join(TALL.values())))
    fonts = {}
    for name in names:
        try:
            f = Fnt(os.path.join(folder, name))
        except ValueError:
            continue
        fonts[name] = f
        cells = []
        total = 0
        for script, ranges in SCRIPTS:
            n = sum(1 for lo, hi in ranges for c in range(lo, hi + 1) if f.has(c))
            total += n
            tall = TALL.get(script)
            px = f.ink_height(ord(tall)) if tall and n and f.has(ord(tall)) else 0
            cells.append("%d/%d" % (n, px) if tall else "%d" % n)
        line = int(name.split("-")[0])
        note = "" if line == f.height or name.startswith(("24-", "53-")) else "  NAME != LINE"
        print("%-22s %4d %3d %9d %6d  %s%s" % (name[:-4], f.height, f.ascent, f.bytes, total,
                                               "  ".join(cells), note))
    return fonts


def sheet(fonts, path):
    """every sample in every font, black on white"""
    rows = []  # (y, font, text, rtl)
    pad, y, width = 6, 6, 0
    for name, f in fonts.items():
        if f.depth != 1 or name.startswith("53-"):
            continue
        rows.append((y, None, name[:-4], False))
        y += 14
        for _, text, rtl in SAMPLES:
            rows.append((y, f, text, rtl))
            y += f.height + 2
        y += 8
    label = fonts.get("14-Inter-SemiBold.fnt") or next(iter(fonts.values()))
    W, H = 420, y + pad
    img = [bytearray(b"\xff" * W * 3) for _ in range(H)]
    for y, f, text, rtl in rows:
        font = f or label
        x = pad
        for ch in (reversed(text) if rtl else text):
            c = ord(ch)
            if not font.has(c):
                # what the firmware takes from Unifont: a grey box here
                for yy in range(y + 2, y + font.height - 2):
                    for xx in range(x, min(x + 6, W)):
                        img[yy][xx * 3:xx * 3 + 3] = b"\xc0\xc0\xff"
                x += 8
                continue
            w, g = font.glyph(c)
            for gy, grow in enumerate(g):
                for gx, a in enumerate(grow):
                    if a < 15 and x + gx < W and y + gy < H:
                        v = a * 17
                        img[y + gy][(x + gx) * 3:(x + gx) * 3 + 3] = bytes((v, v, v))
            x += w
    padding = (-W * 3) % 4
    data = b"".join(bytes(r) + b"\0" * padding for r in reversed(img))
    with open(path, "wb") as out:
        out.write(b"BM" + struct.pack("<IHHI", 54 + len(data), 0, 0, 54))
        out.write(struct.pack("<IiiHHIIiiII", 40, W, H, 1, 24, 0, len(data), 2835, 2835, 0, 0))
        out.write(data)


def main():
    args = sys.argv[1:]
    out = None
    if "--sheet" in args:
        i = args.index("--sheet")
        out = args[i + 1]
        del args[i:i + 2]
    here = os.path.dirname(os.path.abspath(__file__))
    folder = args[0] if args else os.path.join(here, "..", "fonts", ".rockbox", "fonts")
    names = sorted((n for n in os.listdir(folder) if n.endswith(".fnt")),
                   key=lambda n: (int(n.split("-")[0]), n))
    fonts = report(folder, names)
    if out:
        sheet(fonts, out)


main()
