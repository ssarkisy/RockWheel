#!/usr/bin/env python3
"""Make apps/library_fonts_han.h, the ideographs the library's fonts always have
(apps/library_fonts.c), whatever the tags are written in: lyrics in Chinese or
Japanese have others than their tags, which came from GNU Unifont.

    python3 themes/tools/make_common_han.py

They're the common ones of the three standards, as Python's codecs have them:
the 3,755 of GB2312's first level (rows 16 to 55), the 5,401 frequent ones of
Big5 (0xA440 to 0xC67E) and the 2,965 kanji of JIS X 0208's first level (rows 16
to 47), of those the fonts of fonts/scripts/ have in each size: Han-SC, Han-TC
and Han-JP, one of which has each. Run it again when those fonts are made anew
(make_script_fonts.sh).
"""
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')
FONTS = os.path.join(HERE, '..', 'fonts', '.rockbox', 'fonts', 'scripts')
OUT = os.path.join(ROOT, 'apps', 'library_fonts_han.h')


def decoded(codec, pairs):
    out = set()
    for hi, lo in pairs:
        try:
            out.add(ord(bytes([hi, lo]).decode(codec)))
        except UnicodeDecodeError:
            pass
    return out


def gb2312():
    return decoded('gb2312', ((0xa0 + r, 0xa0 + c) for r in range(16, 56) for c in range(1, 95)))


def big5():
    return decoded('big5', ((code >> 8, code & 0xff) for code in range(0xa440, 0xc67f)
                            if 0x40 <= code & 0xff <= 0x7e or 0xa1 <= code & 0xff <= 0xfe))


def jis():
    return decoded('euc_jp', ((0xa0 + r, 0xa0 + c) for r in range(16, 48) for c in range(1, 95)))


class Font:
    """the characters an RB12 font has, as convttf writes them: those of a
    width, but those pointing at the default character's glyph"""

    def __init__(self, path):
        d = open(path, 'rb').read()
        assert d[:4] == b'RB12', path
        (_, _, _, _, self.first, default, size, nbits, noffset, nwidth) = struct.unpack_from(
            '<HHHHIIIIII', d, 4)
        long_offsets = nbits >= 0xFFDB
        pos = 36 + nbits
        pos = (pos + 3) & ~3 if long_offsets else (pos + 1) & ~1
        offsets = struct.unpack_from('<%d%s' % (noffset, 'I' if long_offsets else 'H'), d, pos)
        widths = d[pos + noffset * (4 if long_offsets else 2):][:nwidth]
        d0 = default - self.first
        self.chars = {self.first + i for i in range(size)
                      if widths[i] and (self.first + i == default or offsets[i] != offsets[d0])}


def main():
    sets = (gb2312(), big5(), jis())
    union = set().union(*sets)
    have = {}
    for size in (24, 17, 14):
        for form in ('SC', 'TC', 'JP'):
            have[size, form] = Font(os.path.join(FONTS, '%d-Han-%s.fnt' % (size, form))).chars
    kept = sorted(c for c in union
                  if all(any(c in have[size, f] for f in ('SC', 'TC', 'JP')) for size in (24, 17, 14)))
    print('GB2312 %d, Big5 %d, JIS %d: %d together, %d of them in the fonts'
          % (len(sets[0]), len(sets[1]), len(sets[2]), len(union), len(kept)))
    for form in ('SC', 'TC', 'JP'):
        print('  Han-%s lacks %d of them' % (form, sum(1 for c in union if c not in have[24, form])))

    first, last = kept[0], kept[-1]
    bits = bytearray((last - first) // 8 + 1)
    for c in kept:
        bits[(c - first) >> 3] |= 1 << ((c - first) & 7)
    with open(OUT, 'w') as f:
        f.write('/* The ideographs the library\'s fonts always have (library_fonts.c): of\n'
                '   GB2312\'s, Big5\'s and JIS X 0208\'s first levels, the %d the fonts of\n'
                '   scripts/ have. A bit for each from COMMON_HAN_FIRST, by\n'
                '   themes/tools/make_common_han.py: don\'t edit, run it again. */\n'
                % len(kept))
        f.write('#define COMMON_HAN_FIRST 0x%04x\n' % first)
        f.write('#define COMMON_HAN_LAST  0x%04x\n' % last)
        f.write('static const uint8_t common_han[%d] = {\n' % len(bits))
        for i in range(0, len(bits), 12):
            f.write('    ' + ' '.join('0x%02x,' % b for b in bits[i:i + 12]) + '\n')
        f.write('};\n')
    print('wrote %s, %d bytes of bits' % (os.path.relpath(OUT, ROOT), len(bits)))


if __name__ == '__main__':
    main()
