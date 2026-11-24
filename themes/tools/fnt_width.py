# Text widths in the themes' fonts (themes/fonts/.rockbox/fonts/*.fnt, RB12),
# for the skins' scripts: what the firmware's getstringsize() gives, the
# glyphs' advances summed, for text of the fonts' own Latin.
import os
import struct

FONTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fonts",
                     ".rockbox", "fonts")
_widths = {}


def _load(name):
    b = open(os.path.join(FONTS, name + ".fnt"), "rb").read()
    assert b[:4] == b"RB12", name
    maxwidth = struct.unpack_from("<H", b, 4)[0]
    first, default, size, nbits, noff, nwid = struct.unpack_from("<6I", b, 12)
    long_offsets = nbits >= 0xFFDB
    pos = 36 + nbits
    pos = (pos + 3) & ~3 if long_offsets else (pos + 1) & ~1
    pos += noff * (4 if long_offsets else 2)
    widths = b[pos:pos + nwid] if nwid else bytes([maxwidth]) * size
    return first, default, widths


def width(font, text):
    """text's width in px in the font named as the skins load it, without .fnt"""
    if font not in _widths:
        _widths[font] = _load(font)
    first, default, widths = _widths[font]
    total = 0
    for c in text:
        i = ord(c) - first
        total += widths[i] if 0 <= i < len(widths) else widths[default - first]
    return total


def clock_width(font):
    """the widest of the status line's times, 0:00 to 23:59 (%cH:%cM and
    %cl:%cM)"""
    return max(width(font, form % (h, m)) for form in ("%d:%02d", "%02d:%02d")
               for h in range(24) for m in range(60))


def ink_start(font, char):
    """the first column of char's glyph that is at least half covered, in a
    font of 4-bit glyphs (rows run on, low nibble first, 0 the ink)"""
    b = open(os.path.join(FONTS, font + ".fnt"), "rb").read()
    height, _, depth = struct.unpack_from("<3H", b, 6)
    first, default, size, nbits, noff, nwid = struct.unpack_from("<6I", b, 12)
    assert depth == 1, font
    long_offsets = nbits >= 0xFFDB
    pos = 36 + nbits
    pos = (pos + 3) & ~3 if long_offsets else (pos + 1) & ~1
    offsets = struct.unpack_from(("<%dI" if long_offsets else "<%dH") % noff, b, pos)
    i = ord(char) - first
    w = width(font, char)
    for x in range(w):
        for y in range(height):
            k = y * w + x
            v = b[36 + offsets[i] + k // 2]
            if 15 - ((v >> 4) if k & 1 else (v & 15)) >= 8:
                return x
    return w


# Battery Percentage's level, "%bl%" right-aligned in its viewport: a glyph
# left of it placed by the level shown, so that it stands the same distance
# from its first digit's ink whatever the level is
LEVEL_CHAIN = ("", 100), ("2", 20), ("2i", 10), ("1", 0), ("1i", 1)


def level_places(font, right, before):
    """for each level 0 to 100 the x of a glyph that starts `before` columns
    left of the first digit's ink of the level, which ends at column right - 1:
    one whose ink ends at its column e stands a gap of g from it with
    before = 1 + g + e"""
    return [right - width(font, "%d%%" % n) + ink_start(font, str(n)[0]) - before
            for n in range(101)]


def level_cases(places):
    """the viewports' cases for those places: (suffix, x) each, and a function
    making the skin's conditional that shows show(suffix) for the level shown.
    Where the level's digits are of one width the chain of %?if: 100, 20-99,
    10-19, 0 and 2-9, and 1 (a 1's ink starts later); otherwise %?bl itself,
    one option a level, a case for each place (its first, unknown, shows the
    case of 0). In the simulator an option of one %Vd parses to about 180
    bytes; a chain of %?if halving the levels' runs of one place took more,
    about 420 a test, so a case is best one %Vd"""
    groups = {"": [100], "2": range(20, 100), "2i": range(10, 20), "1": [0] + list(range(2, 10)),
              "1i": [1]}
    if all(len({places[n] for n in groups[s]}) == 1 for s in groups):
        cases = [(s, places[groups[s][0]]) for s, _ in LEVEL_CHAIN]

        def select(show):
            return ("%?if(%bl, <, 10)<%?if(%bl, =, 1)<" + show("1i") + "|" + show("1") +
                    ">|%?if(%bl, <, 20)<" + show("2i") + "|%?if(%bl, <, 100)<" + show("2") +
                    "|" + show("") + ">>>")
        return cases, select
    xs = sorted(set(places))
    suffix = {x: "l%d" % i for i, x in enumerate(xs)}
    cases = [(suffix[x], x) for x in xs]

    def select(show):
        return "%?bl<" + "|".join(show(suffix[places[n]]) for n in [0] + list(range(101))) + ">"
    return cases, select
