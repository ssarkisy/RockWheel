#!/usr/bin/env python3
# Makes the parts of iClassic Square's Now Playing that place the track's
# details, between the lines that name this script in
# themes/iClassic-Square/.rockbox/wps/iClassic-Square.wps; the rest of that
# skin is edited by hand. Then make_dark_theme.py makes the dark one:
#
#   themes/tools/make_classic_np.py && themes/tools/make_dark_theme.py
#
# The details stand beside the cover as one block in the middle of its
# height, as on a stock iPod: the title; a song's artist, album and place in
# its list, then its stars and its format as Settings > Appearance says
# (Rating, Format and Bitrate); an episode's show, date, place and time left;
# a book's author, chapter, place, its speed when it isn't the normal one and
# its format. A skin places nothing by what is shown, so each of these has
# viewports of its own, named by what it holds: s<stars><format>, e,
# b<speed><format>. The rows are those the lines had in 1.4.

import os

HERE = os.path.dirname(os.path.abspath(__file__))
WPS = os.path.join(HERE, "..", "iClassic-Square", ".rockbox", "wps", "iClassic-Square.wps")
BEGIN = "# begin: made by themes/tools/make_classic_np.py, not edited here"
END = "# end: made by themes/tools/make_classic_np.py"

COVER_Y, COVER = 45, 136
COL, COL_W = 160, 148
# each line: its viewport's font, rows, colour (None: the firmware's, %fB's)
# and text; a line of the 14 px font after one of 17 stands 2 rows into its
# place of 18, on the same baseline
ARTIST = "%?ia<%ia|%?iA<%iA|%Sx(Unknown Artist)>>"
AUTHOR = "%?ia<%ia|%?iA<%iA|%Sx(Unknown Author)>>"
ALBUM = "%?id<%id|%Sx(Unknown Album)>"
CHAPTER = "%?kc<%kc|%id>"
LINES = {
    "title": (1, 21, "000000", "%t(8)%s%al%?it<%it|%fn>;%t(16)%al%?it<%it|%fn>"),
    "artist": (3, 18, "666666", "%%t(8)%%al%s;%%t(8)%%s%%al%s;%%t(8)%%al%s" % ((ARTIST,) * 3)),
    "album": (7, 18, "767676", "%%t(16)%%al%s;%%t(8)%%al%%s%s" % ((ALBUM,) * 2)),
    "show": (3, 18, "666666", "%%t(8)%%al%s;%%t(8)%%s%%al%s;%%t(8)%%al%s" % ((ALBUM,) * 3)),
    "date": (2, 18, "767676", "%al%kd"),
    "author": (3, 18, "666666", "%%t(8)%%al%s;%%t(8)%%s%%al%s;%%t(8)%%al%s" % ((AUTHOR,) * 3)),
    "chapter": (2, 18, "767676", "%%t(16)%%al%s;%%t(8)%%al%%s%s" % ((CHAPTER,) * 2)),
    "count": (2, 14, "767676", "%s%al%pp %Sx(of) %pe"),
    "format": (4, 18, None, "%fB"),
    "left": (2, 16, "767676", "%al%kl%?ks< · %ks|>"),
    "speed": (2, 16, "767676", "%al%?ks<%ks|>"),
}
SMALL_UNDER_LARGE = ("date", "chapter")
STARS = (66, 13)
COUNT_GAP = 4
STARS_GAP = 6
FOOT_GAP = 5            # over the first line under the place in the list
FOOT_UNDER_FOOT = 3     # over the format under the stars or the speed
# repeat and shuffle under the status bar's right end, repeat at the column's
# edge: the large icons (their white is the ground's), and for Shuffle by
# albums its own icon of the status bar, in the middle of their place
MODES_Y = 22
MODE_W, MODE_H = 19, 20
MODE_GAP = 4


def block(parts):
    """parts: (what, clear rows over it); each one's top row and rows, the
    block in the middle of the cover's height"""
    rows = [STARS[1] if what == "stars" else LINES[what][1] for what, _ in parts]
    y = COVER_Y + COVER // 2 - sum(r + gap for r, (_, gap) in zip(rows, parts)) // 2
    out = []
    for (what, gap), r in zip(parts, rows):
        y += gap
        out.append((what, y, r))
        y += r
    return out


def details(first, second):
    return [("title", 0), (first, 0), (second, 0), ("count", COUNT_GAP)]


def feet(*lines):
    """the lines under the place in the list, each one's gap"""
    return [(what, FOOT_GAP if n == 0 else FOOT_UNDER_FOOT) for n, what in enumerate(lines)]


def variants():
    out = {}
    for stars in (0, 1):
        for fmt in (0, 1):
            out["s%d%d" % (stars, fmt)] = details("artist", "album") + feet(
                *(["stars"] if stars else []) + (["format"] if fmt else []))
    out["e"] = details("show", "date") + feet("left")
    for speed in (0, 1):
        for fmt in (0, 1):
            out["b%d%d" % (speed, fmt)] = details("author", "chapter") + feet(
                *(["speed"] if speed else []) + (["format"] if fmt else []))
    # the stars' gap is their own
    for parts in out.values():
        for i, (what, gap) in enumerate(parts):
            if what == "stars":
                parts[i] = (what, STARS_GAP)
    return out


def viewports():
    out = []
    for label, parts in variants().items():
        for what, y, rows in block(parts):
            if what == "stars":
                out += ["%%Vl(%s,%d,%d,%d,%d,-)" % (label, COL, y, STARS[0], STARS[1]),
                        "%?rr<" + "|".join("%%xd(R%s)" % chr(ord("a") + i)
                                            for i in range(11)) + ">"]
                continue
            font, _, colour, text = LINES[what]
            if what in SMALL_UNDER_LARGE:
                y, rows = y + 2, rows - 2
            # the place's figures start a column left, as in 1.4
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
    right = COL + COL_W - MODE_W + 1
    return [
        "%xl(r,Repeat Icon.bmp,0,0,4)",
        "%xl(S,Shuffle Icon.bmp,0,0)",
        "%%xl(A,Status Shuffle Albums.bmp,%d,%d)" % ((MODE_W - 16 + 1) // 2, (MODE_H - 16) // 2),
        "%%Vl(mode1,%d,%d,%d,%d,-)" % (right, MODES_Y, MODE_W, MODE_H),
        "%%?mm<%s|%s>" % (shuffle, repeat),
        "%%Vl(mode2,%d,%d,%d,%d,-)" % (right - MODE_GAP - MODE_W, MODES_Y, MODE_W, MODE_H),
        "%%?mm<|%s>" % "|".join([shuffle] * 4),
    ]


def shown(normal, rating, speed):
    """in the normal view and the scrubber (%mw 1, 2), the album art view of a
    track without a cover, the rating and the speed views"""
    return "%%?if(%%mw, <, 3)<%s|%%?mw<||%%?C<|%s>||%s|%s>>" % (normal, normal, rating, speed)


def main():
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
        ["# the details; five stars of the Ratings picture, the set ones over the outlines",
         "%xl(R,Ratings.bmp,0,0,11)",
         "%xC(R)"] + viewports() + [
         "# never shown, it only gives %fB the font of the bitrate",
         "%Vl(bitrate,0,0,1,1,6)",
         "# repeat and shuffle under the status bar's right end, repeat at the column's edge"]
        + modes(),
    ]
    for (b, e), part in reversed(list(zip(zip(begins, ends), parts))):
        lines[b + 1:e] = part
    with open(WPS, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


main()
