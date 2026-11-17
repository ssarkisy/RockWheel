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
