#!/usr/bin/env python3
"""Draw the speaker that marks the song playing in a list, as on a stock iPod, the marks
of the catalogue's playlists and folders and of a podcast's episodes, new and begun, and print them
as the C arrays apps/gui/line.c draws them from; and the note of an album's header
without a cover, which
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


def ellipse(x, y, cx, cy, rx, ry, angle):
    """inside an ellipse turned by angle"""
    c, s = math.cos(angle), math.sin(angle)
    dx, dy = x - cx, y - cy
    u, v = dx * c + dy * s, -dx * s + dy * c
    return (u / rx) ** 2 + (v / ry) ** 2 <= 1


def quad(x, y, pts):
    """inside a convex polygon, its points clockwise on screen"""
    n = len(pts)
    for i in range(n):
        (ax, ay), (bx, by) = pts[i], pts[(i + 1) % n]
        if (bx - ax) * (y - ay) - (by - ay) * (x - ax) < 0:
            return False
    return True


# the marks of the catalogue's playlists and folders, before their names, filled,
# which read better than outlines at this size: one for every playlist, whatever
# made it, lines and a note as Music's, and one for every folder. Solid, the lines
# two pixels thick with their edges on the pixels' edges, which keeps them sharp
PLAYLIST_W, PLAYLIST_H = 13, 11


def playlist(x, y):
    # three lines, the last shorter, before the note
    for top, right in ((1, 7), (4, 7), (7, 4)):
        if 0 <= x <= right and top <= y <= top + 2:
            return True
    # its stem, and its flag down to the right
    if 10 <= x <= 11.6 and 0 <= y <= 9:
        return True
    if quad(x, y, ((11.6, 0), (13, 1.5), (13, 3.7), (11.6, 2.2))):
        return True
    # its head, an oval tilted up to the right, left of the stem's foot
    return ellipse(x, y, 9.1, 9.0, 2.3, 1.75, math.radians(-22))


FOLDER_W, FOLDER_H = 13, 10


def folder(x, y):
    # its tab at the top left, the right side slanting, on the body
    if rounded_rect(x, y, 0, 0, 7, 4, 1.0) and x - 5 <= y:
        return True
    return rounded_rect(x, y, 0, 2, 13, 10, 1.4)


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


# the mark of Finder's smart playlists, as Music marks them: a filled gear,
# eight teeth on the grid (owner, 2026-11-13; a small gear on the playlist's
# mark couldn't be made out)
SMART_GEAR_W = SMART_GEAR_H = 11
smart_gear = gear(5.5, 5.5, 5.5, 3.7, 1.5, 8, 2.0, 0)


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
    for name, inside, w, h in (('speaker', speaker, W, H),
                               ('playlist', playlist, PLAYLIST_W, PLAYLIST_H),
                               ('folder', folder, FOLDER_W, FOLDER_H),
                               ('smart_gear', smart_gear, SMART_GEAR_W, SMART_GEAR_H),
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
