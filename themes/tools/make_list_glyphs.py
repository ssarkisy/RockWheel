#!/usr/bin/env python3
"""Draw the speaker that marks the song playing in a list, as on a stock iPod, the
shuffle arrows of a list's Shuffle Songs, the marks of the catalogue's playlists and of
a podcast's episodes, new and begun, and print them as the C arrays apps/gui/line.c
draws them from; and the note of an album's header without a cover, which
apps/album_header.c draws, and of an album's row in a list, which line.c draws: alpha masks in Rockbox' format, 4 bits a pixel from 0 opaque to 15
transparent, the low nibble first, the rows running on without padding, as the chevron's
there. The firmware colours them, so they're the same in every theme. Plain Python, no
imaging libraries. Usage:

    python3 themes/tools/make_list_glyphs.py [preview-prefix]

With a prefix it also writes each mask, eight times as large, black on white, to look at,
as <prefix>-<name>.bmp.
"""
import math
import struct
import sys

SS = 8  # samples per pixel on each axis

W, H = 15, 12
CENTRE_Y = H / 2
# the speaker: its box, then its cone opening to the right
BOX = (0.5, 4.0, 3.5, 8.0)
CONE_X0, CONE_X1 = 3.5, 7.5
CONE_TOP0, CONE_TOP1 = 4.0, 0.5
# its sound: two arcs about the cone's mouth, with round ends
WAVE_X = 7.5
WAVES = (3.2, 6.2)
WAVE_THICK = 1.5
WAVE_ANGLE = math.radians(48)


def speaker(x, y):
    if BOX[0] <= x <= BOX[2] and BOX[1] <= y <= BOX[3]:
        return True
    if CONE_X0 <= x <= CONE_X1:
        t = (x - CONE_X0) / (CONE_X1 - CONE_X0)
        top = CONE_TOP0 + (CONE_TOP1 - CONE_TOP0) * t
        if top <= y <= H - top:
            return True
    dx, dy = x - WAVE_X, y - CENTRE_Y
    r = math.hypot(dx, dy)
    a = math.atan2(dy, dx)
    for radius in WAVES:
        if abs(r - radius) <= WAVE_THICK / 2 and abs(a) <= WAVE_ANGLE:
            return True
        for end in (-WAVE_ANGLE, WAVE_ANGLE):
            ex = WAVE_X + radius * math.cos(end)
            ey = CENTRE_Y + radius * math.sin(end)
            if math.hypot(x - ex, y - ey) <= WAVE_THICK / 2:
                return True
    return False


# the shuffle arrows, as a stock iPod's: two lines from the left that cross on an S
# each, then run level to a filled arrowhead at the right end, the upper one coming
# from below; level on the middles of rows of pixels, sharp there
SW, SH = 16, 12
SHUF_Y = (2.5, 9.5)
SHUF_X0, SHUF_BEND0, SHUF_BEND1 = 0.75, 3.0, 10.5
SHUF_THICK = 1.5
HEAD_TIP, HEAD_BASE, HEAD_HALF = 15.5, 11.5, 2.5


def shuffle_path(y_from, y_to, steps=48):
    points = [(SHUF_X0, y_from), (SHUF_BEND0, y_from)]
    for i in range(1, steps):
        t = i / steps
        s = t * t * (3 - 2 * t)  # an S, level at both ends
        points.append((SHUF_BEND0 + (SHUF_BEND1 - SHUF_BEND0) * t,
                       y_from + (y_to - y_from) * s))
    points += [(SHUF_BEND1, y_to), (HEAD_BASE + 0.5, y_to)]
    return points


SHUF_SEGMENTS = []
for y_from, y_to in ((SHUF_Y[1], SHUF_Y[0]), (SHUF_Y[0], SHUF_Y[1])):
    path = shuffle_path(y_from, y_to)
    SHUF_SEGMENTS += list(zip(path, path[1:]))


def near_segment(x, y, a, b, r):
    (ax, ay), (bx, by) = a, b
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(x - (ax + t * dx), y - (ay + t * dy)) <= r


def shuffle(x, y):
    for cy in SHUF_Y:
        if HEAD_BASE <= x <= HEAD_TIP:
            if abs(y - cy) <= HEAD_HALF * (HEAD_TIP - x) / (HEAD_TIP - HEAD_BASE):
                return True
    return any(near_segment(x, y, a, b, SHUF_THICK / 2) for a, b in SHUF_SEGMENTS)


def rounded_rect_ring(x, y, x0, y0, x1, y1, r, thick):
    """inside the outline of a rectangle with round corners"""
    def inside(px, py, inset):
        a0, b0, a1, b1 = x0 + inset, y0 + inset, x1 - inset, y1 - inset
        rr = max(r - inset, 0)
        if px < a0 or px > a1 or py < b0 or py > b1:
            return False
        cx = min(max(px, a0 + rr), a1 - rr)
        cy = min(max(py, b0 + rr), b1 - rr)
        return math.hypot(px - cx, py - cy) <= rr
    return inside(x, y, 0) and not inside(x, y, thick)


def rounded_rect(x, y, x0, y0, x1, y1, r):
    """inside a rectangle with round corners"""
    return rounded_rect_ring(x, y, x0, y0, x1, y1, r, 1000)


# the marks of the catalogue's playlists and folders, before their names: a
# computer for those synced from it and an iPod for those made on it. One
# family: solid, what's in
# them cut out, two pixels of it everywhere, their edges on the pixels' edges,
# which keeps them sharp
COMPUTER_W, COMPUTER_H = 13, 12


def computer(x, y):
    # a display, its screen cut out, on a neck and a foot
    if rounded_rect(x, y, 0, 0, 13, 9, 1.5):
        return not (2 <= x <= 11 and 2 <= y <= 7)
    if 5 <= x <= 8 and 9 <= y <= 10:
        return True
    return rounded_rect(x, y, 3, 10, 10, 12, 0.8)


IPOD_W, IPOD_H = 10, 13


def ipod(x, y):
    # a classic iPod: its screen and its wheel cut out
    if not rounded_rect(x, y, 0, 0, 10, 13, 1.5):
        return False
    if 2 <= x <= 8 and 2 <= y <= 5:
        return False
    return math.hypot(x - 5, y - 9) > 2


# the note on the tile in place of an album's cover in its header, when it has none
# or while it's read (apps/album_header.c): the note of the menus' panel
# (make_menu_icons.py), scaled to a little over half the tile's 60 px
NOTE_SCALE = 0.4
NOTE_W, NOTE_H = 29, 35


def note(x, y):
    from make_menu_icons import Note
    return Note.inside(x / NOTE_SCALE, y / NOTE_SCALE)


# and on the smaller tile of an album's row in a list, 32 px (apps/gui/line.c): the
# same share of it
SMALL_NOTE_SCALE = NOTE_SCALE * 32 / 60
SMALL_NOTE_W, SMALL_NOTE_H = 16, 19


def small_note(x, y):
    from make_menu_icons import Note
    return Note.inside(x / SMALL_NOTE_SCALE, y / SMALL_NOTE_SCALE)


# the marks of a podcast's episodes: a dot for a new one, and for one begun a
# ring of the same size with its left half filled
DOT_W = DOT_H = 7
DOT_R = 3.5
RING_THICK = 1.25


def dot(x, y):
    return math.hypot(x - DOT_W / 2, y - DOT_H / 2) <= DOT_R


def half_dot(x, y):
    r = math.hypot(x - DOT_W / 2, y - DOT_H / 2)
    return r <= DOT_R and (x <= DOT_W / 2 or r >= DOT_R - RING_THICK)


def coverage(inside, w, h):
    rows = []
    for py in range(h):
        row = []
        for px in range(w):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    if inside(px + (sx + 0.5) / SS, py + (sy + 0.5) / SS):
                        hits += 1
            row.append(hits / (SS * SS))
        rows.append(row)
    return rows


def nibbles(cov):
    return [15 - round(15 * c) for row in cov for c in row]


def c_array(nib):
    if len(nib) % 2:
        nib = nib + [15]
    data = [nib[i] | (nib[i + 1] << 4) for i in range(0, len(nib), 2)]
    lines = []
    for i in range(0, len(data), 12):
        lines.append('    ' + ', '.join('0x%02x' % b for b in data[i:i + 12]) + ',')
    return '\n'.join(lines)


def preview(cov, path, k=8):
    w, h = len(cov[0]) * k, len(cov) * k
    stride = (w * 3 + 3) & ~3
    body = bytearray()
    for y in range(h - 1, -1, -1):
        line = bytearray()
        for x in range(w):
            v = round(255 * (1 - cov[y // k][x // k]))
            line += bytes((v, v, v))
        line += b'\0' * (stride - len(line))
        body += line
    head = b'BM' + struct.pack('<IHHI', 54 + len(body), 0, 0, 54)
    head += struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, len(body), 2835, 2835, 0, 0)
    with open(path, 'wb') as f:
        f.write(head + body)


def main():
    for name, inside, w, h in (('speaker', speaker, W, H), ('shuffle', shuffle, SW, SH),
                               ('computer', computer, COMPUTER_W, COMPUTER_H),
                               ('ipod', ipod, IPOD_W, IPOD_H),
                               ('note', note, NOTE_W, NOTE_H),
                               ('small_note', small_note, SMALL_NOTE_W, SMALL_NOTE_H),
                               ('dot', dot, DOT_W, DOT_H),
                               ('half_dot', half_dot, DOT_W, DOT_H)):
        cov = coverage(inside, w, h)
        print('#define %s_W %d' % (name.upper(), w))
        print('#define %s_H %d' % (name.upper(), h))
        print('static const unsigned char %s_alpha[] = {' % name)
        print(c_array(nibbles(cov)))
        print('};')
        if len(sys.argv) > 1:
            preview(cov, '%s-%s.bmp' % (sys.argv[1], name))


if __name__ == '__main__':
    main()
