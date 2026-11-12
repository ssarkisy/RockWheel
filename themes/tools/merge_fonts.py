#!/usr/bin/env python3
"""Give the theme's Inter fonts every alphabet the iPod shows in its own
typefaces: Hangul, the ideographs and kana, Hebrew, Arabic and Thai, each glyph
on the Inter font's baseline and cut to its line, as firmware/font.c drew them
from fonts of their own before. Run by make_script_fonts.sh.

    merge_fonts.py <theme fonts folder> <sources folder> <C header> [<list of fonts with sources of their own>]

The sources are the fonts make_script_fonts.sh makes of each alphabet in three
sizes, 24, 17 and 14. Each font gets the sources of its line height or the one
below, Hangul those of its height or the one above: the 18s Hangul of 24 and
the rest of 17. A font named in the list, a name a line, has sources of its own
instead, "<its name>-Hebrew.fnt" and so on, in its weight and size; the forms
and the C header are still worked out from the three sizes alone, so adding
such a font changes neither the header nor the fonts there were, and a form
that differs only in the new font's size is drawn as SC's. The ideographs and kana are Noto Sans SC's on their codes, or
TC's or JP's where SC hasn't one; those Noto Sans JP or TC draws otherwise in
any size, by a pixel, are placed from U+10000, JP's and then TC's, each in the
order of its codes, and the C header, which firmware/font.c includes, says
which those are. By pixels rather than by outlines: of the same outlines, the
weights of the variable fonts give others to some, kana among them, as a
Japanese title had them before.
What the Inter font has of the alphabets other than Latin, Greek and Cyrillic
gives way to their own fonts, as the firmware had them, and a code no font has
is of no width, which the firmware draws from Unifont.

Glyphs of a code in a block of BLOCKS are written in records of one size, the
first one's at the record block's offset: a byte of the glyph's width, then its
bits; the offsets and widths are there for any reader of RB12 all the same. The
firmware reads such a glyph in one go rather than three, its width, its offset
and its bits from three places in a file of megabytes. The record blocks are
described at the start of the bits, which no glyph's offset points to: "RWFR",
their number, the CRC-32 of the header's tables of forms, which the firmware
takes the forms by only if it has the same, and for each block its first and
last code, its offset and the size of a record, all 32-bit. Run again on its
own output, it makes the same files.
"""
import operator
import os
import struct
import sys
import zlib

HIDDEN = 0x10000
BLOCKS = ((0x3000, 0x9FFF), (0xAC00, 0xD7A3), (0xFF00, 0xFFEF))  # and the forms past U+FFFF
HANGUL = ((0x3131, 0x318E), (0xAC00, 0xD7A3))
SCRIPTS = (((0x0590, 0x05FF), "Hebrew"), ((0x0600, 0x06FF), "Arabic"),
           ((0xFB50, 0xFDFF), "ArabicA"), ((0xFE70, 0xFEFF), "ArabicB"),
           ((0x0E00, 0x0E7F), "Thai"))
# the ideographs' sources by range, SC's first, the others for what it hasn't
HAN = (((0x3000, 0x9FFF), "Han", ("SC", "TC", "JP")),
       ((0xF900, 0xFAFF), "Compat", ("SC", "JP")),
       ((0xFF00, 0xFFEF), "Wide", ("SC", "TC", "JP")))
FORM_RANGES = tuple(r for r, _, _ in HAN)
FONTS = ("24-Inter-Bold", "18-Inter-SemiBold", "18-Inter-Bold", "17-Inter-SemiBold",
         "17-Inter-Medium", "17-Inter-Bold", "14-Inter-SemiBold", "14-Inter-Bold")
SIZES = (24, 17, 14)
HEADER = 36


def glyph_bytes(height, width):
    return (((height * width + 1) // 2) + 1) & ~1


def within(c, ranges):
    return any(lo <= c <= hi for lo, hi in ranges)


class Fnt:
    """an RB12 font read whole"""

    def __init__(self, path):
        b = open(path, "rb").read()
        assert b[:4] == b"RB12", path
        self.maxwidth, self.height, self.ascent, self.depth = struct.unpack_from("<4H", b, 4)
        (self.first, self.default, self.size, nbits, noff, nwid) = \
            struct.unpack_from("<6I", b, 12)
        assert self.depth == 1 and noff == nwid == self.size, path
        long_offsets = nbits >= 0xFFDB
        pos = HEADER + nbits
        pos = (pos + 3) & ~3 if long_offsets else (pos + 1) & ~1
        self.offsets = struct.unpack_from("<%d%s" % (noff, "I" if long_offsets else "H"), b, pos)
        pos += noff * (4 if long_offsets else 2)
        self.widths = b[pos:pos + nwid]
        self.bits = b[HEADER:HEADER + nbits]

    def width(self, c):
        i = c - self.first
        return self.widths[i] if 0 <= i < self.size else 0

    def has(self, c):
        """a glyph of its own: convttf gives a character it hasn't the
        default's glyph"""
        if self.width(c) == 0:
            return False
        d = self.default - self.first
        return c == self.default or self.offsets[c - self.first] != self.offsets[d]

    def glyph(self, c):
        """its bits, padded as the firmware reads them: convttf doesn't pad
        them, and the byte after a glyph is the next one's"""
        w = self.width(c)
        off = self.offsets[c - self.first]
        g = self.bits[off:off + (self.height * w + 1) // 2]
        return g.ljust(glyph_bytes(self.height, w), b"\xff"), w


LOW = bytes(i & 15 for i in range(256))
HIGH = bytes(i >> 4 for i in range(256))
SHL4 = bytes((i & 15) << 4 for i in range(256))


def repack(f, c, height, ascent):
    """the glyph of c on a line of this height and ascent, rows outside it left
    out, and whether any of those had ink"""
    bits, w = f.glyph(c)
    nib = bytearray(2 * len(bits))
    nib[0::2] = bits.translate(LOW)
    nib[1::2] = bits.translate(HIGH)
    shift = ascent - f.ascent
    out = bytearray(b"\x0f" * (height * w + 1))
    cut = False
    for y in range(f.height):
        row = nib[y * w:(y + 1) * w]
        ty = y + shift
        if 0 <= ty < height:
            out[ty * w:(ty + 1) * w] = row
        elif row and min(row) < 8:
            cut = True
    out = out[:height * w]
    if len(out) & 1:
        out.append(15)
    packed = bytes(map(operator.or_, out[0::2], out[1::2].translate(SHL4)))
    return packed + b"\xff" * (glyph_bytes(height, w) - len(packed)), w, cut


class Sources:
    def __init__(self, folder):
        self.folder = folder
        self.fonts = {}

    def get(self, size, name):
        key = "%s-%s" % (size, name)
        if key not in self.fonts:
            self.fonts[key] = Fnt(os.path.join(self.folder, key + ".fnt"))
        return self.fonts[key]

    def han(self, size, c, form=None):
        """the font of size with the ideograph c, of that form or else the
        first that has it"""
        for (lo, hi), name, forms in HAN:
            if lo <= c <= hi:
                for r in (form,) if form else forms:
                    f = self.get(size, "%s-%s" % (name, r))
                    if f.has(c):
                        return f
        return None


def size_below(height):
    return max([s for s in SIZES if s <= height] or [min(SIZES)])


def size_above(height):
    return min([s for s in SIZES if s >= height] or [max(SIZES)])


def ink(f, c):
    """the pixels of a glyph, its width and each row's from the baseline"""
    bits, w = f.glyph(c)
    nib = bytearray(2 * len(bits))
    nib[0::2] = bits.translate(LOW)
    nib[1::2] = bits.translate(HIGH)
    return w, {y - f.ascent: bytes(nib[y * w:(y + 1) * w]) for y in range(f.height)
               if min(nib[y * w:(y + 1) * w], default=15) < 15}


def other_forms(src, form):
    """the codes that the form has in every size, as the font has them on
    their codes, and draws otherwise in one of them"""
    codes = []
    for (lo, hi), _, _ in HAN:
        for c in range(lo, hi + 1):
            if within(c, HANGUL):
                continue
            pairs = [(src.han(s, c), src.han(s, c, form)) for s in SIZES]
            if all(base and f for base, f in pairs) \
                    and any(ink(base, c) != ink(f, c) for base, f in pairs):
                codes.append(c)
    return codes


def merge(base, src, forms, check, height_of, own=None):
    H, A = base.height, base.ascent
    below, above = (own, own) if own else (size_below(height_of), size_above(height_of))
    glyphs, cuts = {}, 0

    def put(c, f, at=None):
        nonlocal cuts
        g, w, cut = repack(f, c, H, A)
        glyphs[c if at is None else at] = (g, w)
        cuts += cut

    # Inter's own, but the alphabets of their own fonts
    script_ranges = [r for r, _ in SCRIPTS]
    for c in range(base.first, min(base.first + base.size, 0x3000)):
        if base.has(c) and not within(c, script_ranges):
            glyphs[c] = base.glyph(c)
    for r, name in SCRIPTS:
        f = src.get(below, name)
        for c in range(r[0], r[1] + 1):
            if f.has(c):
                put(c, f)
    f = src.get(above, "Hangul")
    for lo, hi in HANGUL:
        for c in range(lo, hi + 1):
            if f.has(c):
                put(c, f)
    for (lo, hi), _, _ in HAN:
        for c in range(lo, hi + 1):
            if c not in glyphs:
                f = src.han(below, c)
                if f:
                    put(c, f)
    hidden = HIDDEN
    for form, codes in forms:
        for c in codes:
            put(c, src.han(below, c, form) or src.han(below, c), hidden)
            hidden += 1

    first, last = base.first, hidden - 1
    blocks = list(BLOCKS) + [(HIDDEN, last)]
    in_block = lambda c: within(c, blocks)
    # the description of the blocks, then the glyphs of no block, then the blocks
    head = b"RWFR" + struct.pack("<II", len(blocks), check)
    bits = bytearray(len(head) + 16 * len(blocks))
    offsets, widths = {}, {}
    for c in sorted(glyphs):
        g, w = glyphs[c]
        if w and not in_block(c):
            offsets[c], widths[c] = len(bits), w
            bits += g
    for lo, hi in blocks:
        size = 1 + max(glyph_bytes(H, glyphs[c][1]) for c in range(lo, hi + 1) if c in glyphs)
        head += struct.pack("<4I", lo, hi, len(bits), size)
        for c in range(lo, hi + 1):
            g, w = glyphs.get(c, (b"", 0))
            if w:
                offsets[c], widths[c] = len(bits) + 1, w
            bits += (bytes((w,)) + g).ljust(size, b"\0")
    bits[:len(head)] = head
    # below Hebrew, what Inter hasn't stays its default, as it was
    d = base.default
    for c in range(first, 0x0590):
        if c not in widths and base.width(c):
            offsets[c], widths[c] = offsets[d], widths[d]
    maxwidth = max(widths.values())
    count = last - first + 1
    out = bytearray(b"RB12" + struct.pack("<4H", maxwidth, H, A, 1)
                    + struct.pack("<6I", first, d, count, len(bits), count, count))
    out += bits
    out += b"\0" * (-len(out) & 3)
    out += struct.pack("<%dI" % count, *(offsets.get(c, 0) for c in range(first, last + 1)))
    out += bytes(widths.get(c, 0) for c in range(first, last + 1))
    return bytes(out), maxwidth, cuts, len(glyphs)


def c_words(words, per_line):
    return ",\n".join("    " + ", ".join(words[i:i + per_line])
                      for i in range(0, len(words), per_line))


def form_words(codes):
    """a bit for each code of FORM_RANGES in order"""
    words = [0] * ((sum(hi - lo + 1 for lo, hi in FORM_RANGES) + 31) // 32)
    for c in codes:
        i, base = 0, 0
        for lo, hi in FORM_RANGES:
            if lo <= c <= hi:
                i = base + c - lo
            base += hi - lo + 1
        words[i >> 5] |= 1 << (i & 31)
    return words


def forms_check(forms):
    return zlib.crc32(b"".join(struct.pack("<%dI" % len(w), *w)
                               for w in (form_words(codes) for _, codes in forms)))


def header(forms):
    """the bits of the codes of each form other than SC's, and the count of
    them before each 256"""
    text = ["/* Made by themes/tools/merge_fonts.py with the theme's fonts, which have the",
            "   ideographs of these forms past U+FFFF in this order: made anew with them */",
            "#define FONT_HAN_HIDDEN 0x%X" % HIDDEN,
            "#define FONT_HAN_CHECK 0x%08xu /* the fonts' too */" % forms_check(forms)]
    for form, codes in forms:
        text.append("#define FONT_HAN_%s_COUNT %d" % (form, len(codes)))
    for form, codes in forms:
        words = form_words(codes)
        nwords = len(words)
        ranks, n = [], 0
        for i, w in enumerate(words):
            if i % 8 == 0:
                ranks.append(n)
            n += bin(w).count("1")
        text.append("static const uint32_t font_han_%s_bits[%d] = {" % (form.lower(), nwords))
        text.append(c_words(["0x%08x" % w for w in words], 6))
        text.append("};")
        text.append("static const uint16_t font_han_%s_ranks[%d] = {" % (form.lower(), len(ranks)))
        text.append(c_words(["%d" % r for r in ranks], 10))
        text.append("};")
    return "\n".join(text) + "\n"


def main():
    theme, folder, header_path = sys.argv[1:4]
    src = Sources(folder)
    forms = []
    for form in ("JP", "TC"):
        forms.append((form, other_forms(src, form)))
        print("%s forms: %d" % (form, len(forms[-1][1])))
    own = [l.strip() for l in open(sys.argv[4])] if len(sys.argv) > 4 else []
    for name in FONTS + tuple(n for n in own if n):
        path = os.path.join(theme, name + ".fnt")
        base = Fnt(path)
        data, maxwidth, cuts, count = merge(base, src, forms, forms_check(forms),
                                            int(name.split("-")[0]),
                                            name if name in own else None)
        with open(path, "wb") as f:
            f.write(data)
        print("%-18s %5d glyphs, %8d bytes, widest %d, %d with ink cut off"
              % (name, count, len(data), maxwidth, cuts))
    with open(header_path, "w") as f:
        f.write(header(forms))


main()
