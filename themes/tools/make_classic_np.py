#!/usr/bin/env python3
# Makes the parts of iClassic Square's Now Playing that place the track's
# details, between the lines that name this script in
# themes/iClassic-Square/.rockbox/wps/iClassic-Square.wps; the rest of that
# skin is edited by hand. Then make_dark_theme.py makes the dark one, and
# make_rtl_stars.py the stars' mirrors for right to left:
#
#   themes/tools/make_classic_np.py && themes/tools/make_dark_theme.py \
#       && themes/tools/make_rtl_stars.py
#
# The details stand beside the cover as one block in the middle of its
# height, as on a stock iPod: the title; a song's artist and album, its stars
# as Settings > Appearance says (Rating), then its place in its list, a line
# as the album's (the stock guide's figure: the stars, then "10 of 12"), and
# its format (Format and Bitrate); an episode's show, date, place and time left;
# a book's author, chapter, place, its speed when it isn't the normal one and
# its format. Every line stands on one pitch, the stars and the format in a
# line's place, and the block's ink in the middle of the cover's height
# (block()). A skin places nothing by what is shown, so each of these has
# viewports of its own, named by what it holds: s<stars><format>, e,
# b<speed><format>. The same in the iPod photo's skin,
# themes/iClassic-Square/220x176, by its layout (PHOTO).

import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
BEGIN = "# begin: made by themes/tools/make_classic_np.py, not edited here"
END = "# end: made by themes/tools/make_classic_np.py"

ARTIST = "%?ia<%ia|%?iA<%iA|%Sx(Unknown Artist)>>"
AUTHOR = "%?ia<%ia|%?iA<%iA|%Sx(Unknown Author)>>"
ALBUM = "%?id<%id|%Sx(Unknown Album)>"
CHAPTER = "%?kc<%kc|%id>"
# An episode's time left, and at the right end the speed where it isn't the
# normal one, always whole. Where both don't fit, the time left ends with "…"
# before the speed (%nx with a left and a right text, write_line() in
# apps/gui/skin_engine/skin_display.c), two spaces apart: the one the cut
# leaves and the speed's own. With no speed, the time left has the whole width.
LEFT = "%nx%al%kl%ar%?ks< %ks|>"
PLACE = "%s%al%pp %Sx(of) %pe"


# The layouts by screen, set in turn as globals (use()); the iPod Video's:
VIDEO = dict(
    WPS=os.path.join(HERE, "..", "iClassic-Square", ".rockbox", "wps", "iClassic-Square.wps"),
    COVER_Y=41, COVER=136,
    COL=160, COL_W=148,
    # each line: its viewport's font, rows (fewer where the next line's
    # starts sooner), colour (None: the firmware's, %fB's) and text
    LINES={
        "title": (1, 21, "000000", "%t(8)%s%al%?it<%it|%fn>;%t(16)%al%?it<%it|%fn>"),
        "artist": (3, 18, "666666", "%%t(8)%%al%s;%%t(8)%%s%%al%s;%%t(8)%%al%s" % ((ARTIST,) * 3)),
        "album": (7, 18, "767676", "%%t(16)%%al%s;%%t(8)%%al%%s%s" % ((ALBUM,) * 2)),
        "show": (3, 18, "666666", "%%t(8)%%al%s;%%t(8)%%s%%al%s;%%t(8)%%al%s" % ((ALBUM,) * 3)),
        "date": (2, 18, "767676", "%al%kd"),
        "author": (3, 18, "666666", "%%t(8)%%al%s;%%t(8)%%s%%al%s;%%t(8)%%al%s" % ((AUTHOR,) * 3)),
        "chapter": (2, 18, "767676", "%%t(16)%%al%s;%%t(8)%%al%%s%s" % ((CHAPTER,) * 2)),
        "count": (2, 14, "767676", PLACE),
        # a song's place, under its stars or its album, in the album's font
        # and colour
        "place": (7, 18, "767676", PLACE),
        "format": (4, 18, None, "%fB"),
        "left": (2, 16, "767676", LEFT),
        "speed": (2, 16, "767676", "%al%?ks<%ks|>"),
    },
    PITCH=18,               # from a line's baseline to the next one's
    # each font's ink in its viewport: its capitals' top row and its
    # baseline; the stars' and the format's first and last rows
    INK={1: (6, 16), 3: (4, 12), 7: (4, 12), 2: (3, 10), 4: (3, 13), "stars": (1, 11)},
    # repeat and shuffle under the status line's right end, their ink's last
    # column the battery's (its viewport at 291, its picture's last column
    # 24): the large icons (their white is the ground's), and for Shuffle by
    # albums its own icon of the status bar, at their place's right end; two
    # rows under the dark line of the bar of 24 (make_classic_header.py)
    BATTERY_RIGHT=315,
    MODES_Y=25,
    MODE_W=19, MODE_H=20, MODE_GAP=4,
    MODES_FOOT=21,          # the details' top, from the modes': a row under them
)
# The iPod photo's 220x176: the cover 96 px, the column beside it in its
# fonts, 17 px for the title and 14 for the rest, on a pitch of 14, so that
# the tallest block, a rated song's with its format, stays beside the cover
# under the modes
PHOTO = dict(
    WPS=os.path.join(HERE, "..", "iClassic-Square", "220x176", ".rockbox", "wps",
                     "iClassic-Square.wps"),
    COVER_Y=31, COVER=96,
    # at the lists' edge of 4 px, the cover too (the screen is small, the
    # margins took the text's room)
    COL=106, COL_W=110,
    LINES=dict(VIDEO["LINES"], **{
        "title": (1, 17) + VIDEO["LINES"]["title"][2:],
        "artist": (3, 15) + VIDEO["LINES"]["artist"][2:],
        "album": (7, 15) + VIDEO["LINES"]["album"][2:],
        "show": (3, 15) + VIDEO["LINES"]["show"][2:],
        "date": (2, 15) + VIDEO["LINES"]["date"][2:],
        "author": (3, 15) + VIDEO["LINES"]["author"][2:],
        "chapter": (2, 15) + VIDEO["LINES"]["chapter"][2:],
        "place": (7, 15) + VIDEO["LINES"]["place"][2:],
        "format": (4, 15) + VIDEO["LINES"]["format"][2:],
        "left": (2, 15) + VIDEO["LINES"]["left"][2:],
        "speed": (2, 15) + VIDEO["LINES"]["speed"][2:],
    }),
    PITCH=14,
    INK={1: (4, 12), 3: (3, 10), 7: (3, 10), 2: (3, 10), 4: (3, 13), "stars": (1, 11)},
    # the battery's viewport at 191; two rows under the bar's line of 16
    BATTERY_RIGHT=215,
    MODES_Y=17,
    # make_classic_modes.py's, 85 % of the Video's; the details' top where the
    # Video's size had it, not moved with these
    MODE_W=16, MODE_H=17, MODE_GAP=3,
    MODES_FOOT=21,
)
STARS_ROWS = 13  # the small stars' row; their width is the Ratings picture's


def use(layout):
    globals().update(layout)


use(VIDEO)


def ink(what):
    """the line's ink in its viewport (INK)"""
    return INK["stars"] if what == "stars" else INK[LINES[what][0]]


def block(lines):
    """the lines' viewports, (what, top row, rows) each: their baselines PITCH
    apart, the stars and the format in a line's place, in the middle of its
    capitals; the block's ink, from the first line's capitals to the last
    one's foot, in the middle of the cover's height, but under the modes"""
    caps = ink("artist")
    middle = (caps[1] - caps[0]) // 2
    out = []
    for n, what in enumerate(lines):
        top, foot = ink(what)
        base = n * PITCH
        y = base - middle - (top + foot) // 2 if what in ("stars", "format") else base - foot
        out.append([what, y, STARS_ROWS if what == "stars" else LINES[what][1]])
    first = out[0][1] + ink(out[0][0])[0]
    last = out[-1][1] + ink(out[-1][0])[1]
    shift = max(COVER_Y + COVER // 2 - (first + last + 1) // 2,
                MODES_Y + MODES_FOOT - out[0][1])
    for line in out:
        line[1] += shift
    # none taller than to the next one's top, which it would clear
    for line, below in zip(out, out[1:]):
        line[2] = min(line[2], below[1] - line[1])
    return [tuple(line) for line in out]


def variants():
    out = {}
    for stars in (0, 1):
        for fmt in (0, 1):
            # a song's place right under its album without stars
            out["s%d%d" % (stars, fmt)] = (["title", "artist", "album"]
                                           + (["stars"] if stars else []) + ["place"]
                                           + (["format"] if fmt else []))
    out["e"] = ["title", "show", "date", "count", "left"]
    for speed in (0, 1):
        for fmt in (0, 1):
            out["b%d%d" % (speed, fmt)] = (["title", "author", "chapter", "count"]
                                           + (["speed"] if speed else [])
                                           + (["format"] if fmt else []))
    return out


def viewports():
    out = []
    for label, parts in variants().items():
        for what, y, rows in block(parts):
            if what == "stars":
                out += ["%%Vl(%s,%d,%d,%d,%d,-)" % (label, COL, y, COL_W, STARS_ROWS),
                        "%?Sr<" + "|".join(
                            "%?rr<" + "|".join("%%xd(%s%s)" % (pic, chr(ord("a") + i))
                                                for i in range(11)) + ">"
                            for pic in "qR") + ">"]
                continue
            font, _, colour, text = LINES[what]
            # an episode's and a book's place, in the smaller font, its figures
            # a column left, as in 1.4; a song's at the album's column
            x, w = (COL - 1, COL_W + 1) if what == "count" else (COL, COL_W)
            out += ["%%Vl(%s,%d,%d,%d,%d,%d)%s" % (label, x, y, w, rows, font,
                                                   "%%Vf(%s)" % colour if colour else ""),
                    text]
    return out


def choose(view):
    """the block's viewports by the track and the settings, in a view"""
    def stars(fmt):
        on, off = ("%%Vd(s%d%d)" % (s, fmt) for s in (1, 0))
        if view == "rating":
            return off
        # Rating: Always, Only Rated (%rr: none, then by half stars), Never
        return "%%?St(now playing rating)<%s|%%?rr<%s|%s>|%s>" % (on, off, on, off)

    def book(fmt):
        if view == "speed":
            return "%%Vd(b0%d)" % fmt
        return "%%?ks<%%Vd(b1%d)|%%Vd(b0%d)>" % (fmt, fmt)
    song = "%%?St(now playing format)<%s|%s>" % (stars(1), stars(0))
    books = "%%?St(now playing format)<%s|%s>" % (book(1), book(0))
    # a song has no speed view
    return "%%?kk<%s|%%Vd(e)|%s>" % ("" if view == "speed" else song, books)


def modes():
    shuffle = "%?ps<%?pa<%xd(A)|%xd(S)>|>"
    # %mm: off, all, one, shuffle, A-B
    repeat = "%xd(ra)|%xd(rb)|%t(1.0)%xd(rc);%t(1.0)%xd(ra)|%xd(rd)"
    right = BATTERY_RIGHT - MODE_W + 1
    albums_w, albums_h = picture_size("Status Shuffle Albums.bmp")
    return [
        "%xl(r,Repeat Icon.bmp,0,0,4)",
        "%xl(S,Shuffle Icon.bmp,0,0)",
        "%%xl(A,Status Shuffle Albums.bmp,%d,%d)" % (MODE_W - albums_w, (MODE_H - albums_h) // 2),
        "%%Vl(mode1,%d,%d,%d,%d,-)" % (right, MODES_Y, MODE_W, MODE_H),
        "%%?mm<%s|%s>" % (shuffle, repeat),
        "%%Vl(mode2,%d,%d,%d,%d,-)" % (right - MODE_GAP - MODE_W, MODES_Y, MODE_W, MODE_H),
        "%%?mm<|%s>" % "|".join([shuffle] * 4),
    ]


def shown(normal, rating, speed):
    """in the normal view and the scrubber (%mw 1, 2), the album art view of a
    track without a cover, the rating and the speed views"""
    return "%%?if(%%mw, <, 3)<%s|%%?mw<||%%?C<|%s>||%s|%s>>" % (normal, normal, rating, speed)


def picture_size(name):
    """a picture's width and height, of the light skin's"""
    with open(os.path.join(os.path.dirname(WPS), "iClassic-Square", name), "rb") as f:
        w, h = struct.unpack_from("<ii", f.read(26), 18)
    return w, abs(h)


def ratings_width():
    """the Ratings picture's width, for its mirror's place at the column's end"""
    return picture_size("Ratings.bmp")[0]


def write():
    lines = open(WPS, encoding="utf-8").read().split("\n")
    ends = [n for n, l in enumerate(lines) if l == END]
    begins = [n for n, l in enumerate(lines) if l == BEGIN]
    assert len(begins) == len(ends) == 2, "two parts of the skin are this script's"
    parts = [
        ["# the details' viewports by the track and the settings, and the modes; the",
         "# rating view leaves the small stars out, the speed view a book's speed:",
         "# neither moves as it is set",
         shown(choose("normal"), choose("rating"), choose("speed")),
         shown("%Vd(mode1)%Vd(mode2)", "%Vd(mode1)%Vd(mode2)", "%Vd(mode1)%Vd(mode2)")],
        ["# the details; five stars of the Ratings picture, the set ones over the outlines;",
         "# right to left (%Sr) their mirror, make_rtl_stars.py's, at the column's other",
         "# end and set from the right, in the same viewport, as wide as the column",
         "%xl(R,Ratings.bmp,0,0,11)",
         "%xC(R)",
         "%%xl(q,Ratings RTL.bmp,%d,0,11)" % (COL_W - ratings_width()),
         "%xC(q)"] + viewports() + [
         "# never shown, it only gives %fB the font of the bitrate",
         "%Vl(bitrate,0,0,1,1,6)",
         "# repeat and shuffle under the status bar's right end, ending where the battery does"]
        + modes(),
    ]
    for (b, e), part in reversed(list(zip(zip(begins, ends), parts))):
        lines[b + 1:e] = part
    with open(WPS, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def main():
    for layout in (VIDEO, PHOTO):
        use(layout)
        write()


main()
