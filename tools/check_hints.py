#!/usr/bin/env python3
"""The hints under Settings' folders, LANG_HINT_* in every language of
apps/lang, measured with the fonts the iPod draws them in: each must go into
three lines of the band under the list, wrapped as apps/gui/bitmap/list.c wraps
them, by words, a word longer than a line by its letters. Prints the ones that
don't, with the lines they'd take, and exits with 1 then.

    python3 tools/check_hints.py apps/lang themes/fonts/.rockbox/fonts

The theme's 18-Inter-SemiBold draws them, and 18-Inter-Bold with Bold Text;
both are checked. They have every alphabet the iPod has fonts of; what they
haven't comes from 16-GNU-Unifont. The ideographs and kana are in the forms of
the language, Japan's or Taiwan's under those menus, past U+FFFF in the fonts
(firmware/font_han_forms.h), some a pixel wider or narrower than China's.
Arabic is measured in the joined forms of its letters, which are what's drawn,
by the tables of firmware/arabjoin.c.
"""
import glob
import os
import re
import struct
import sys

LINE_WIDTH = 297  # the band's text, as list.c has it beside the scrollbar
LINE_BYTES = 192 - 4  # and its buffer for a line, HINT_BYTES
LINES = 3
THEME_FONTS = ('18-Inter-SemiBold.fnt', '18-Inter-Bold.fnt')


class Font:
    def __init__(self, path):
        d = open(path, 'rb').read()
        assert d[:4] == b'RB12', path
        (self.maxwidth, self.height, self.ascent, self.depth, self.first,
         self.default, self.size, nbits, noffset, nwidth) = struct.unpack_from(
            '<HHHHIIIIII', d, 4)
        pos = 36 + nbits
        long_offsets = nbits >= 0xFFDB
        pos = (pos + 3) & ~3 if long_offsets else (pos + 1) & ~1
        self.offsets = None
        if noffset:
            fmt = '<%d%s' % (noffset, 'I' if long_offsets else 'H')
            self.offsets = struct.unpack_from(fmt, d, pos)
            pos += noffset * (4 if long_offsets else 2)
        self.widths = d[pos:pos + nwidth] if nwidth else None

    def in_range(self, ch):
        return 0 <= ord(ch) - self.first < self.size

    def width(self, ch):
        if self.widths is None:
            return self.maxwidth
        return self.widths[ord(ch) - self.first]


def diacritics(root):
    """the ranges of marks drawn over the letter before them, with no width of
    their own, from firmware/common/diacritic.c, as the font draws them"""
    src = open(os.path.join(root, 'firmware/common/diacritic.c')).read()
    ranges = []
    for a, b in re.findall(r'DIAC_RANGE_ENTRY\((\w+),\s*(0x[0-9a-fA-F]+)', src):
        first = 0x300 if a == 'FIRST_DIACRITIC' else int(a, 16)
        if first:
            ranges.append((first, int(b, 16)))
    return ranges


DIACRITICS = []


class Forms:
    """firmware/font_han_forms.h: the glyphs of the ideographs and kana in
    Japan's and Taiwan's forms, by their codes"""

    def __init__(self, root):
        src = open(os.path.join(root, 'firmware/font_han_forms.h')).read()
        glyph = int(re.search(r'FONT_HAN_HIDDEN (0x[0-9A-Fa-f]+)', src).group(1), 16)
        self.glyphs = {}
        for form in ('JP', 'TC'):
            bits = re.search(r'font_han_%s_bits\[\d+\] = \{([^}]*)\}' % form.lower(), src)
            words = [int(w, 16) for w in bits.group(1).replace(',', ' ').split()]
            self.glyphs[form] = {}
            for i in range(32 * len(words)):
                if words[i >> 5] >> (i & 31) & 1:
                    # U+3000..9FFF, U+F900..FAFF and U+FF00..FFEF, one after another
                    c = (0x3000 + i if i < 0x7000 else 0xf900 + i - 0x7000 if i < 0x7200
                         else 0xff00 + i - 0x7200)
                    self.glyphs[form][c] = glyph
                    glyph += 1


FORMS = []


class Fonts:
    """the fonts a hint in one language is drawn in"""
    form = None

    def __init__(self, fontdir, theme, lang):
        def load(name):
            path = os.path.join(fontdir, name)
            return Font(path) if os.path.exists(path) else None
        self.theme = load(theme)
        self.unifont = load('16-GNU-Unifont.fnt')
        self.form = {'japanese': 'JP', 'chinese-trad': 'TC'}.get(lang)

    def font_for(self, ch):
        """font_for_char(): the theme's font up to Hebrew, and for all it has a
        glyph of; Unifont for the rest"""
        c = ord(ch)
        if c < 0x0590 or not self.unifont \
                or (self.theme.in_range(ch) and self.theme.width(ch) > 0):
            return self.theme
        return self.unifont

    def width_of(self, ch):
        """of a character as it's drawn, as font_measurestring() takes it"""
        c = ord(ch)
        if c >= 0x300 and any(a <= c < b for a, b in DIACRITICS):
            return 0
        if 0x200b <= c <= 0x200f or 0x2060 <= c <= 0x2064 or 0xfe00 <= c <= 0xfe0f \
                or c == 0xfeff:
            return 0
        if c > 0xffff:
            ch = '□'
        font = self.font_for(ch)
        if font is self.theme and self.form:
            ch = chr(FORMS[0].glyphs[self.form].get(ord(ch), ord(ch)))
        return font.width(ch)

    def text_width(self, s):
        """as drawn: Arabic's letters in their joined forms"""
        return sum(self.width_of(ch) for ch in arabic_join(s))


class Arab:
    """firmware/arabjoin.c: each letter's isolated, final, medial and initial
    forms, as arab_lookup() in firmware/bidi.c finds them"""

    def __init__(self, root):
        src = open(os.path.join(root, 'firmware/arabjoin.c')).read()
        # the rows left out of the build
        src = re.sub(r'#\s*if\s+0.*?#\s*endif', '', src, flags=re.S)

        def table(name):
            m = re.search(r'%s\[\]\s*=\s*\{(.*?)\};' % name, src, re.S)
            return [tuple(int(x, 16) for x in row) for row in
                    re.findall(r'\{\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*'
                               r'(0x[0-9a-fA-F]+|0),\s*(0x[0-9a-fA-F]+|0)\s*\}',
                               m.group(1))]
        self.lamaleph = table('lamaleph')
        self.jointable = table('jointable')
        self.zwj = (0x20, 0x20, 0x20, 0x20)

    def lookup(self, c):
        a = None
        if 0x621 <= c <= 0x63a:
            a = self.jointable[c - 0x621]
        elif 0x640 <= c <= 0x64a:
            a = self.jointable[c - 0x621 - 5]
        elif 0x671 <= c <= 0x6d5:
            a = self.jointable[c - 0x621 - 5 - 38]
        elif c == 0x200d:
            a = self.zwj
        # a letter without forms, as it is
        return a if a and a[0] else None


ARAB = []


class Join:
    """arabic_join_next() of firmware/bidi.c: a letter of Arabic waits for the
    next character to know its form; what's drawn before each character"""

    def __init__(self):
        self.prev, self.connected = None, False

    def next(self, ch):
        arab = ARAB[0]
        prev, cur = self.prev, arab.lookup(ord(ch))
        if cur is None:
            out = []
            if prev:
                out.append(chr(prev[1] if self.connected else prev[0]))
                self.connected, self.prev = False, None
            return out + [ch]
        if prev is None:
            self.prev = cur
            return []
        if prev[0] == 0xfedd:
            lig = {0xfe8d: 0, 0xfe87: 1, 0xfe83: 2, 0xfe81: 3}.get(cur[0])
            if lig is not None:
                self.prev = arab.lamaleph[lig]
                return []
        if self.connected:
            if prev[2] and cur[1]:
                out = [chr(prev[2])]
            else:
                out = [chr(prev[1])]
                self.connected = False
        elif prev[3] and cur[1]:
            out = [chr(prev[3])]
            self.connected = True
        else:
            out = [chr(prev[0])]
        self.prev = cur
        return out


def arabic_join(s):
    """what's drawn for s, Arabic's letters in their joined forms"""
    join = Join()
    out = []
    for ch in list(s) + ['\0']:
        out += join.next(ch)
    return ''.join(out[:-1])


# keeps_with_before() of apps/gui/bitmap/list.c: the closing punctuation and the
# small kana, which don't start a line
KEEPS_WITH_BEFORE = set(map(ord,
    '、。〉》」』】〕〗〙〛・ー'
    'ぁぃぅぇぉっゃゅょゎ'
    'ァィゥェォッャュョヮヵヶ'
    '！），．：；？］｝'))


def breaks_before(ch):
    c = ord(ch)
    return ((0x2e80 <= c <= 0x9fff or 0xf900 <= c <= 0xfaff or 0xff00 <= c <= 0xffef)
            and c not in KEEPS_WITH_BEFORE)


def measurestring(fonts, s, maxbytes, maxwidth):
    """font_measurestring() of firmware/font.c: how many characters of s fit in
    maxwidth, each joined form at the letter it is, as it counts them"""
    join = Join()
    waiting_at = 0
    width = 0
    for i, ch in enumerate(list(s) + ['\0']):
        end = ch == '\0' or len(s[:i].encode('utf-8')) >= maxbytes
        waiting = join.prev is not None
        shown = join.next('\0' if end else ch)
        at = [i, i]
        if end or ARAB[0].lookup(ord(ch)) is None:
            if waiting:
                at[0] = waiting_at
            if end:
                shown = shown[:-1]
        else:
            at[0] = waiting_at
            if shown or not waiting:
                waiting_at = i
        for k, c in enumerate(shown):
            w = fonts.width_of(c)
            if width + w > maxwidth:
                return at[k]
            width += w
        if end:
            return i


def wrap(fonts, text):
    """the lines, as hint_line() in apps/gui/bitmap/list.c breaks them: the
    longest part from the line's start to a space, a letter of Chinese or
    Japanese or the end that fits, measured as drawn; else, a word longer than
    a line, as many of its letters, one at least"""
    lines = []
    p = 0
    while p < len(text) and len(lines) < 10:
        while p < len(text) and text[p] == ' ':
            p += 1
        if p >= len(text):
            break
        q, end = p, None
        while True:
            ch = text[q] if q < len(text) else '\0'
            if ch == '\0' or (q > p and (ch == ' ' or breaks_before(ch))):
                if len(text[p:q].encode('utf-8')) > LINE_BYTES:
                    break
                if fonts.text_width(text[p:q]) > LINE_WIDTH:
                    break
                end = q
                if ch == '\0':
                    break
            q += 1
        if end is None:
            n = measurestring(fonts, text[p:], LINE_BYTES, LINE_WIDTH)
            # of the letters that start and end within the buffer
            while n > 0 and len(text[p:p + n].encode('utf-8')) > LINE_BYTES:
                n -= 1
            end = p + max(n, 1)
        lines.append(text[p:end].rstrip(' '))
        p = end
    return lines


def hints(path):
    t = open(path, encoding='utf-8').read()
    for m in re.finditer(r'<phrase>(.*?)</phrase>', t, re.S):
        body = m.group(1)
        pid = re.search(r'id:\s*(\S+)', body)
        if not pid or not pid.group(1).startswith('LANG_HINT_'):
            continue
        dest = re.search(r'<dest>(.*?)</dest>', body, re.S)
        text = re.search(r'\*:\s*"(.*)"', dest.group(1)) if dest else None
        if text:
            yield pid.group(1), text.group(1)


def main():
    langs = sys.argv[1] if len(sys.argv) > 1 else 'apps/lang'
    fontdir = sys.argv[2] if len(sys.argv) > 2 else 'themes/fonts/.rockbox/fonts'
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    DIACRITICS.extend(diacritics(root))
    ARAB.append(Arab(root))
    FORMS.append(Forms(root))
    bad = 0
    count = 0
    for f in sorted(glob.glob(os.path.join(langs, '*.lang'))):
        lang = os.path.basename(f)[:-5]
        if lang == 'walon':
            continue
        sets = [Fonts(fontdir, theme, lang) for theme in THEME_FONTS]
        for pid, text in hints(f):
            count += 1
            for theme, fonts in zip(THEME_FONTS, sets):
                lines = wrap(fonts, text)
                if len(lines) > LINES:
                    bad += 1
                    print('%s %s in %s: %d lines' % (lang, pid, theme, len(lines)))
                    for line in lines:
                        print('    %3d  %s' % (fonts.text_width(line), line))
    print('%d hints, %d too long' % (count, bad))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
