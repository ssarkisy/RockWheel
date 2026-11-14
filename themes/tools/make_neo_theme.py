#!/usr/bin/env python3
# Makes Neo's skins, light and dark, from the texts below, its colours and
# the sizes of its glyphs:
#
#   themes/tools/make_neo_theme.py
#
# Neither skin is edited by hand: change the text or the colours here and run
# it again. Run make_theme_glyphs.py first: it makes the skins' bitmaps from
# the mockups' export and writes their sizes to neo_glyph_sizes.txt, by which
# they are placed here, so a redrawn glyph of another size needs no change.
# The colours are those of apps/gui/ui_theme.c's entry for Neo, the mockup's
# as the screen has them. A light and a dark skin name the same fonts and the
# same sizes of covers, so Dark Theme comes with the music playing.
#
# The places are the mockup's (work/mockups/ipod-neo.html, its panel) but
# where the figures below say otherwise: the status line with a hairline
# under it, its glyphs from the side margin leftwards 6 px apart on one foot
# line; the lists under it; the main menu's pane right of a hairline on
# column 159.

import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "Neo", ".rockbox", "wps")

# the reflection's rows and its strength at the top, of 256: the firmware's
# default %Cr in the light skin; on the dark paper a cover's dark foot was
# lost at half, so the dark skin's starts stronger (owner, 2026-11-13)
REFLECT_LIGHT, REFLECT_DARK = "22,128", "22,176"

COLOURS = {
    # DENSE: small text, the clock (#56524D, #B8B1A8 as the screen has them);
    # SECONDARY is left to the bar's fill and the ring. CLOCK is the status
    # glyphs' grey, a step of the screen's from DENSE: the small text takes
    # the user's colour, which is known by DENSE; the clock stays as the
    # battery beside it is
    "Neo":      dict(PAPER="EFEBE7", PRIMARY="292421", SECONDARY="6B6963",
                     TERTIARY="A59E94", TRACK="E7DFD6", SELECTED="DEDBD6",
                     SURFACE="E7E3DE", DENSE="52514A", CLOCK="524D4A",
                     NEUTRAL="524D4A", ACCENT="52554A", REFLECT=REFLECT_LIGHT),
    "Neo-Dark": dict(PAPER="211C18", PRIMARY="EFEBE7", SECONDARY="A59E94",
                     TERTIARY="6B6963", TRACK="313029", SELECTED="312C29",
                     SURFACE="312C29", DENSE="B5B2A5", CLOCK="B5AEA5",
                     NEUTRAL="B5AEA5", ACCENT="B5B6A5", REFLECT=REFLECT_DARK),
}
# NEUTRAL: Now Playing's grey that stays grey, a step from DENSE as CLOCK is:
# of a song's details only the artist takes the user's colour (ACCENT), the
# album, the place in the list, the format and the times under the bar not.
# ACCENT is the look's UI_COLOUR_ACCENT in ui_theme.c, a step from DENSE the
# other way: the artist stays in the colour where the small text is a neutral
# grey (Tinted Background off), and is the cover's second text under Cover
# Colours as DENSE is

SIZES = {}
for line in open(os.path.join(HERE, "neo_glyph_sizes.txt")):
    if not line.startswith("#"):
        name, w, h, count = line.split()
        SIZES[name] = (int(w), int(h), int(count))

STATUS = 20             # the status line's rows; the hairline is the next
MARGIN = 14             # the sides': the clock's start, the glyphs' end
# the lists' area: seven rows of 30 or six of 36; apps/gui/ui_theme.c has the rows' insets
LIST_TOP, LIST_ROWS = 23, 216
TEXT_Y = STATUS - 18    # the status line's text, a cell of 15
FOOT = STATUS - 6       # the row under the status glyphs' feet
GAP = 6                 # between them
# Now Playing: the cover 12 rows under the hairline, its reflection of 22 rows
# under it; the bar's box, as high as the knob, and the line of the times
# under it, where they were with the cover 12 rows lower
COVER, COVER_Y = 136, 32
BAR_Y, TIMES_Y = 193, 205
REFLECTION = 22         # rows, as REFLECT_LIGHT and REFLECT_DARK have them
# the song's details beside the cover: one block in the middle of its height,
# of these rows each line has, with these clear rows over some
TITLE_LINE = 18         # 18-Inter-SemiBold, one line or two
DETAIL = 16             # artist and album, 15-Inter-Medium
SMALL = 14              # the place in the list, the format, an episode's time left
COUNT_GAP = 4           # over the place in the list
STARS_GAP = 6           # over the stars
FORMAT_GAP = 5          # over the format, or what is left of an episode or a book
FORMAT_UNDER_STARS = 3  # over the format, under the stars
# the modes: under the status line's right end, 4 rows under the hairline
MODES_Y = STATUS + 5


CLOCK = "%al%?cf<%cH:%cM|%cl:%cM>"


def frames(label, count):
    return ["%%xd(%s%s)" % (label, chr(ord("a") + i)) for i in range(count)]


def battery():
    """charging in turn, charged, or its level of twenty"""
    f = frames("B", SIZES["Battery"][2])
    levels = f[3:]
    return "%%?bp<%%?bc<%%t(1.0)%s;%%t(1.0)%s|%s>|%%?bl<%s>>" % (
        f[0], f[1], f[2], "|".join([levels[0]] + levels + [levels[-1]] * 2))


def title_vps(label, night, rooms, text):
    """A title left of the glyphs, from the margin to a gap before the
    leftmost one shown: the playing status's place (<label>n<night>p), or the
    moon or the battery past it when it shows nothing (<label>n<night>)"""
    return ["%%Vl(%sn%dp,%d,%d,%d,15,3)%%Vf(PRIMARY)" % (label, night, MARGIN, TEXT_Y, rooms[0]),
            text,
            "%%Vl(%sn%d,%d,%d,%d,15,3)%%Vf(PRIMARY)" % (label, night, MARGIN, TEXT_Y, rooms[1]),
            text]


def title_show(label, night):
    """which of those: the padlock or the playing status shown, or nothing"""
    shown, empty = "%%Vd(%sn%dp)" % (label, night), "%%Vd(%sn%d)" % (label, night)
    return "%%?mh<%s|%%?mp<%s|%s|%s|%s|%s>>" % (shown, empty, shown, shown, shown, shown)


# a title that doesn't fit its place: left-aligned, never scrolled, ended in
# an ellipsis if it is wider still (%nx)
TITLE = "%al%nx%Lt"


def status(width, prefix="", name=None):
    """The glyphs' viewports and the line that shows them: from the right the
    battery, Night Mode's moon, play or pause or the padlock (Now Playing's
    shuffle and repeat stand under the line). What is left of an absent one
    moves up, so each has a viewport for every place it can be in. name: the
    text left of them (title_vps()); in the main menu (its title's icon, 33)
    the clock stands there instead, as it does at the left of a screen of
    the whole width. Also the rooms left of the glyphs, by Night Mode: with
    the playing status's place and without"""
    out = []
    rooms = []
    if name:
        out += ["%%Vl(%sclock,%d,%d,64,15,3)%%Vf(CLOCK)" % (prefix, MARGIN, TEXT_Y), CLOCK]

    def vp(tag, name, x):
        w, h, _ = SIZES[name]
        return "%%Vl(%s%s,%d,%d,%d,%d,-)" % (prefix, tag, x, FOOT - h, w, h)

    x = width - MARGIN - SIZES["Battery"][0]
    out += [vp("bat", "Battery", x), battery()]
    play = frames("P", 4)
    # %mp: stopped, playing, paused, forward, rewind
    state = "%%?mh<%%xd(H)|%%?mp<|%s|%s|%s|%s>>" % (play[0], play[1], play[2], play[3])
    branches = []
    for night in (0, 1):
        at = x
        tag = "n%d" % night
        this = []
        if night:
            at -= GAP + SIZES["Night"][0]
            out += [vp("moon", "Night", at), "%xd(N)"]
            this.append("%%Vd(%smoon)" % prefix)
        past = at
        at -= GAP + SIZES["Playing_Status"][0]
        out += [vp("play" + tag, "Playing_Status", at), state]
        this.append("%%Vd(%splay%s)" % (prefix, tag))
        rooms.append((at - GAP - MARGIN, past - GAP - MARGIN))
        if name:
            out += title_vps(prefix + "name", night, rooms[night], name)
            this.append("%%?if(%%Li, =, 33)<%%Vd(%sclock)|%s>" % (
                prefix, title_show(prefix + "name", night)))
        branches.append("".join(this))
    show = "%%Vd(%sbat)%%?nN<%s|%s>" % (prefix, branches[1], branches[0])
    return "\n".join(out), show, rooms


def images(modes):
    out = []
    for name, label in (("Battery", "B"), ("Playing_Status", "P"), ("Hold", "H"),
                        ("Night", "N"), ("Shuffle", "S"), ("Repeat", "R")):
        if name in ("Shuffle", "Repeat") and not modes:
            continue
        n = SIZES[name][2]
        out.append("%%xl(%s,%s.bmp,0,0%s)" % (label, name.replace("_", " "),
                                              ",%d" % n if n > 1 else ""))
    return "\n".join(out)


def fill(text, **parts):
    for key, value in parts.items():
        text = text.replace("{" + key + "}", str(value))
    return text


SBS = """\
# Neo: the status line and the main menu's pane. Made by themes/tools/make_neo_theme.py
%wd
%Fl(2,15-Inter-Medium.fnt)
%Fl(3,15-Inter-SemiBold.fnt)
{images}
%?LW<%VI(wide)%Vd(rulewide){title}{wide}|%VI(half)%Vd(rulehalf)%Vd(column){half}>
# the pane by the row selected: Settings, Photos, or the music, which Extras has too
%?LW<|%?LS<%Vd(surface)%Vd(ring)%Vd(about)|{music}|%Vd(surface)%?nP<%Vd(collage)%Vd(photocount)|%Vd(nophotos)%Vd(photocaption)>|{music}>>
# the cover fills the pane, nothing over it: Finder's 240 px frame, its middle 160
%Cl(0,0,240,240,c,c)
%Vl(a,120,0,200,240,-)
%Cd
%Vl(a,120,0,40,240,-)
# the lists: under the status line and its hairline, rows of 30; beside the
# pane 156 wide, so that the selected row ends at 150
%Vi(wide,0,{list_top},320,{list_rows},1)%Vf(PRIMARY)%Vb(PAPER)
%Vi(half,0,{list_top},156,{list_rows},1)%Vf(PRIMARY)%Vb(PAPER)
# hairlines: under the status line, over the menu only beside the pane, and
# column 159 between the menu and the pane
%Vl(rulewide,0,{status},320,1,-)%Vb(TRACK)
%Vl(rulehalf,0,{status},159,1,-)%Vb(TRACK)
%Vl(column,159,0,1,240,-)%Vb(TRACK)
# a screen that takes the whole width: the clock, its title, the glyphs;
# a title wider than its place between them takes the clock's too
# (%Lf, title_vps())
%Vl(clockwide,{margin},{text_y},64,15,3)%Vf(CLOCK)
%al%?cf<%cH:%cM|%cl:%cM>
%Vl(titlewide,84,{text_y},152,15,3)%Vf(PRIMARY)
%ac%Lt
{long_vps}
{wide_vps}
# beside the pane: the clock in the main menu, where a screen of the whole
# width has it; a menu's title in any other
{half_vps}
# the pane without a cover: a surface, and on it by the row
%Vl(surface,160,0,160,240,-)%Vb(SURFACE)
# Settings: the disk's use as a ring round (240, 96), the used part in the
# second grey over a track of the ground's colour; what is free, and the whole
%Vl(ring,200,56,80,80,-)%Vf(SECONDARY)%Vb(SURFACE)
%nR
%Vl(about,160,157,160,19,1)%Vf(PRIMARY)%Vb(SURFACE)
%s%ac%nf
%Vl(about,160,180,160,15,2)%Vf(DENSE)%Vb(SURFACE)
%ac%nT
%Vl(collage,160,0,160,176,-)%Vb(SURFACE)
%xl(Q,/.rockbox/photos_panel.bmp,0,0)
%xd(Q)
%Vl(photocount,160,200,160,15,2)%Vf(DENSE)%Vb(SURFACE)
%s%ac%nP
# no photos: their mark, a mask in the note's two colours as the note is
%xl(I,Photos Pane.bmp,0,0)
%xN(I)
%Vl(nophotos,{photos_x},{photos_y},{photos_w},{photos_h},-)%Vb(SURFACE)
%xd(I)
%Vl(photocaption,160,160,160,15,2)%Vf(DENSE)%Vb(SURFACE)
%ac%nP
# nothing playing, or a track without a cover: the note, and under it the
# library's count or the track. A note is its mask, drawn in the theme's two
# colours for it (%xN), which take the theme's tone as the surfaces do
%xl(O,Note Pane.bmp,0,0)
%xN(O)
%Vl(note,{note_x},{note_y},{note_w},{note_h},-)%Vb(SURFACE)
%xd(O)
%Vl(stopped,160,160,160,15,2)%Vf(DENSE)%Vb(SURFACE)
%ac%ns
%Vl(normal,168,160,144,15,2)%Vf(DENSE)%Vb(SURFACE)
%s%ac%?it<%it|%?fn<%fn|>>
"""

WPS = """\
# Neo: Now Playing. Made by themes/tools/make_neo_theme.py
%wd
%Fl(2,18-Inter-SemiBold.fnt)
%Fl(3,17-Inter-Medium-13.fnt)
%Fl(4,14-Inter-Medium.fnt)
%Fl(5,15-Inter-SemiBold.fnt)
%Fl(6,15-Inter-Medium.fnt)
{images}
# the views, cycled with the centre button:
# normal | scrubber | album art | lyrics | rating | speed
%?mw<%?mv(2.0)<%Vd(volume)|%Vd(pb)%Vd(under)>|%Vd(scrub)%?kk<%Vd(under)|%Vd(hints)|%Vd(hints)>|%?C<%Vd(bigart)|%Vd(bignote)>|%Vd(lyrics)%Vd(pb)|%Vd(rating)|%Vd(speed)>
# The track's details beside the cover, one block in the middle of its
# height, as on a stock iPod: the title on one line or two; a song's artist,
# album and place in its list, then its stars and its format as the settings
# say (Rating, Format and Bitrate); an episode's show, date, place and time
# left; a book's author, chapter, place and speed. Skins place nothing by
# what is shown, so each of these has its viewports, named by what it holds:
# s<lines><stars><format>, e<lines>, b<lines><speed>. The rating view leaves
# the small stars out, the speed view a book's speed: neither moves as it is set
{block}
# repeat and shuffle under the status line's right end, repeat at the margin
%?mw<{show_modes}|{show_modes}|||{show_modes}|{show_modes}>
# the cover, its top corners as round as the theme has them; without one, a
# note on a tile with all its corners so (%nc)
%?mw<%?C<%Vd(art)|%Vd(spill)%Vd(noart)>|%?C<%Vd(art)|%Vd(spill)%Vd(noart)>|%Vd(spill)|%Vd(spill)|%?C<%Vd(art)|%Vd(spill)%Vd(noart)>|%?C<%Vd(art)|%Vd(spill)%Vd(noart)>>
# the status line: the clock, the title, the glyphs, a hairline under it. Its
# ground first, a viewport of its own: under Cover Colours' Fill the firmware
# gives what lies in these rows a shade of the cover's colour
%V(0,0,320,{status},-)
%V({margin},{text_y},64,15,5)%Vf(CLOCK)
%al%?cf<%cH:%cM|%cl:%cM>
%V(100,{text_y},120,15,5)%Vf(PRIMARY)
%s%ac%Sx(Now Playing)
%V(0,{status},320,1,-)%Vb(TRACK)
%?nN<{show}
{vps}
{mode_vps}
# over a reflection of {reflection} rows, its foot square, which the reflection
# goes on from; with Angled Cover turned, its corners all square
%Cl(0,0,{cover},{cover},c,c)
%Cr(REFLECT)
# where it is not shown, the ground where a turned cover and its reflection
# were, before what is drawn there: 12 px past the cover's place but under the
# hairline, {reflection} rows under it.
# It must stay defined before noart, bigart, bignote and lyrics: viewports are
# drawn in the file's order, and it clears where they draw
%Vl(spill,{spill_x},{spill_y},{spill_w},{spill_h},-)
%Vl(art,{margin},{cover_y},{cover},{cover},-)
%Cd
%xl(O,Note Playing.bmp,{noart_x},{noart_y})
%xN(O)
%Vl(noart,{margin},{cover_y},{cover},{cover},-)%Vb(SELECTED)
%xd(O)%nc
# The title on one line or two, which never scroll: %nw breaks it at a space
# and ends the second with an ellipsis. %?nW asks this viewport, never shown,
# whether it takes two
%Vl(title,{col},0,{col_w},{title_h},2)
# five stars, the set ones and the outlines of the unset in one picture, a
# column left of the texts' edge, where a star's point is; the set ones take
# the cover's colour with the text, an outline the theme's colour for it, or
# under Text and Stars the cover's muted one (%xC). The firmware knows an
# outline's pixel by its grey: make_theme_glyphs.py fails if another pixel
# of a %xC picture has it
%xl(G,Ratings.bmp,0,0,{ratings_n})
%xC(G)
{block_vps}
# never shown, it only gives %fB the font of the bitrate
%Vl(bitrate,0,0,1,1,4)
# the bar: a capsule across the grid, in a box as high as the knob (the
# pictures are make_theme_glyphs.py's); under it on one line the time played,
# the chapter of a track that has them, cut with an ellipsis and never
# scrolled, and the time left
%xl(K,Bar Track.bmp,0,0)
%xl(k,Knob.bmp,0,0)
%Vl(pb,0,{bar_y},320,{bar_h},-)
%pb({bar})
%Vl(pb,{margin},{times_y},64,14,4)%Vf(NEUTRAL)
%al%pc
%Vl(pb,{times_right},{times_y},64,14,4)%Vf(NEUTRAL)
%ar-%pr
# the scrubber: the same with its knob
%Vl(scrub,0,{bar_y},320,{bar_h},-)
%pb({bar},slider,k,slidercentre)
%Vl(scrub,{margin},{times_y},64,14,4)%Vf(NEUTRAL)
%al%pc
%Vl(scrub,{times_right},{times_y},64,14,4)%Vf(NEUTRAL)
%ar-%pr
%Vl(under,{under_x},{times_y},{under_w},14,4)%Vf(NEUTRAL)
%nx%ac%?kn<%?kk<%kn%?kc< · %kc|>|%kn%?kc< · %kc|>|%kn %Sx(of) %kt>|>
# a podcast's or a book's scrubber: what Previous and Next do there
%Vl(hints,{under_x},{times_y},{under_w},14,4)%Vf(NEUTRAL)
%ac-15 / +30
# the volume in the bar's place, as the wheel turns, shorter between its marks,
# whose ink is as far from it on either side
%xl(W,Volume Track.bmp,0,0)
%xl(v,Volume Left.bmp,0,0)
%xl(V,Volume Right.bmp,0,0)
%Vl(volume,0,{bar_y},320,{bar_h},-)
%pv({volume_bar})
%Vl(volume,{left_x},{marks_y},{left_w},{left_h},-)
%xd(v)
%Vl(volume,{right_x},{marks_y},{right_w},{right_h},-)
%xd(V)
%Vl(volume,{margin},{times_y},{grid},14,4)%Vf(NEUTRAL)
%ac%nv
# rating: five large stars in the bar's place, the wheel sets them
%xl(T,Stars.bmp,0,0,{stars_n})
%xC(T)
%Vl(rating,{stars_x},{rating_y},{stars_w},{stars_h},-)
{big_stars}
# the large cover, and the note in its place
%Vl(bigart,60,30,200,200,-)
%Cl(0,0,200,200,c,c)
%Cd
%xl(L,Note Large.bmp,{large_x},{large_y})
%xN(L)
%Vl(bignote,60,30,200,200,-)%Vb(SELECTED)
%xd(L)%nc
# the lyrics, lines of the text font, over the bar; and the speeds: the WPS
# code draws into these
%Vl(lyrics,{margin},28,{grid},{lyrics_h},3)%Vf(PRIMARY)
%Vl(speed,0,{speed_y},320,44,4)%Vf(DENSE)
"""


def sbs():
    wide_vps, wide, rooms = status(320)
    half_vps, half, _ = status(160, "h", TITLE)
    long_vps = "\n".join(title_vps("long", 0, rooms[0], TITLE)
                         + title_vps("long", 1, rooms[1], TITLE))
    title = "%%?Lf(titlewide)<%%Vd(clockwide)%%Vd(titlewide)|%%?nN<%s|%s>>" % (
        title_show("long", 1), title_show("long", 0))
    music = ("%?C<%Vd(a)|%Vd(surface)%Vd(note)%?mp<%Vd(stopped)|%Vd(normal)|%Vd(normal)"
             "|%Vd(normal)|%Vd(normal)|%Vd(normal)|%Vd(normal)|%Vd(stopped)|%Vd(stopped)>>")
    note, photos = SIZES["Note_Pane"], SIZES["Photos_Pane"]
    return fill(SBS, images=images(False), wide=wide, half=half, wide_vps=wide_vps,
                half_vps=half_vps, long_vps=long_vps, title=title, music=music, margin=MARGIN, text_y=TEXT_Y,
                status=STATUS, list_top=LIST_TOP, list_rows=LIST_ROWS,
                photos_x=240 - photos[0] // 2, photos_y=104 - photos[1] // 2,
                photos_w=photos[0], photos_h=photos[1],
                note_x=240 - note[0] // 2, note_y=104 - note[1] // 2,
                note_w=note[0], note_h=note[1])


def detail_block(parts):
    """parts: (what, rows, clear rows over it); each one's top row, the block
    in the middle of the cover's height"""
    y = COVER_Y + COVER // 2 - sum(rows + gap for _, rows, gap in parts) // 2
    out = []
    for what, rows, gap in parts:
        y += gap
        out.append((what, y, rows))
        y += rows
    return out


# An episode's time left, and at the right end the speed where it isn't the
# normal one, always whole. Where both don't fit, the time left ends with "…"
# before the speed (%nx with a left and a right text, write_line() in
# apps/gui/skin_engine/skin_display.c), two spaces apart: the one the cut
# leaves and the speed's own. With no speed, the time left has the whole width.
LEFT = "%nx%al%kl%ar%?ks< %ks|>"


def wps():
    vps, show, _ = status(320)
    ratings, stars = SIZES["Ratings"], SIZES["Stars"]
    left, right = SIZES["Volume_Left"], SIZES["Volume_Right"]
    note, large = SIZES["Note_Playing"], SIZES["Note_Large"]
    repeat, shuffle = SIZES["Repeat"], SIZES["Shuffle"]
    bar_w, bar_h, _ = SIZES["Bar"]
    volume_w = SIZES["Volume_Bar"][0]
    # the marks' ink 6 px from the bar's ends: the left one's ends a column
    # before its picture does, the right one's starts a column in (the export)
    volume_x = (320 - volume_w) // 2
    left_x = volume_x - 6 - (left[0] - 1)
    right_x = volume_x + volume_w + 6 - 1
    r = frames("G", ratings[2])
    t = frames("T", stars[2])
    grid = 320 - 2 * MARGIN
    col = MARGIN + COVER + 14
    col_w = 320 - MARGIN - col

    # each line of the block: its viewport's font and colour, and its text
    line = {
        "artist": (6, "ACCENT", "%s%al%?ia<%ia|%?iA<%iA|%Sx(Unknown Artist)>>"),
        "album": (6, "NEUTRAL", "%s%al%?id<%id|%Sx(Unknown Album)>"),
        "show": (6, "ACCENT", "%s%al%?id<%id|%Sx(Unknown Album)>"),
        "date": (6, "NEUTRAL", "%al%kd"),
        "author": (6, "ACCENT", "%s%al%?ia<%ia|%?iA<%iA|%Sx(Unknown Author)>>"),
        "chapter": (6, "NEUTRAL", "%s%al%?kc<%kc|%id>"),
        "count": (4, "NEUTRAL", "%al%pp %Sx(of) %pe"),
        "format": (4, "NEUTRAL", "%fB"),
        "left": (4, "NEUTRAL", LEFT),
        "speed": (4, "NEUTRAL", "%al%?ks<%ks|>"),
    }

    def title(lines):
        return [("title%d" % lines, TITLE_LINE * lines, 0)]

    def details(first, second):
        return [(first, DETAIL, 0), (second, DETAIL, 0), ("count", SMALL, COUNT_GAP)]

    variants = {}
    for lines in (1, 2):
        for stars_on in (0, 1):
            for format_on in (0, 1):
                parts = title(lines) + details("artist", "album")
                if stars_on:
                    parts.append(("stars", ratings[1], STARS_GAP))
                if format_on:
                    parts.append(("format", SMALL,
                                  FORMAT_UNDER_STARS if stars_on else FORMAT_GAP))
                variants["s%d%d%d" % (lines, stars_on, format_on)] = parts
        variants["e%d" % lines] = title(lines) + details("show", "date") + [
            ("left", SMALL, FORMAT_GAP)]
        for speed_on in (0, 1):
            variants["b%d%d" % (lines, speed_on)] = title(lines) + details(
                "author", "chapter") + ([("speed", SMALL, FORMAT_GAP)] if speed_on else [])
    block_vps = []
    for label, parts in variants.items():
        for what, y, rows in detail_block(parts):
            if what.startswith("title"):
                block_vps += ["%%Vl(%s,%d,%d,%d,%d,2)%%Vf(PRIMARY)" % (label, col, y, col_w, rows),
                              "%nw(1)"] + (["%nw(2)"] if what == "title2" else [])
            elif what == "stars":
                block_vps += ["%%Vl(%s,%d,%d,%d,%d,-)" % (label, col - 1, y, ratings[0], rows),
                              "%%?rr<%s>" % "|".join(r)]
            else:
                font, colour, text = line[what]
                block_vps += ["%%Vl(%s,%d,%d,%d,%d,%d)%%Vf(%s)" % (
                    label, col, y, col_w, rows, font, colour), text]

    def choose(view):
        """the block's viewports by the track and the settings, in a view"""
        def song(lines):
            def stars(format_on):
                on, off = ("%%Vd(s%d%d%d)" % (lines, s, format_on) for s in (1, 0))
                if view == "rating":
                    return off
                # Rating: Always, Only Rated (%rr: none, then by half stars), Never
                return "%%?St(now playing rating)<%s|%%?rr<%s|%s>|%s>" % (on, off, on, off)
            return "%%?St(now playing format)<%s|%s>" % (stars(1), stars(0))

        def book(lines):
            if view == "speed":
                return "%%Vd(b%d0)" % lines
            return "%%?ks<%%Vd(b%d1)|%%Vd(b%d0)>" % (lines, lines)

        def by_title(kind):
            return "%%?nW(title)<%s|%s>" % (kind(2), kind(1))
        # a song has no speed view
        return "%%?kk<%s|%s|%s>" % ("" if view == "speed" else by_title(song),
                                     by_title(lambda n: "%%Vd(e%d)" % n), by_title(book))
    # one choice for the normal view and the scrubber (%mw: 1, 2), as a skin
    # keeps each written: the tree is in memory while the theme is
    block = "%%?if(%%mw, <, 3)<%s|%%?mw<||||%s|%s>>" % (choose("normal"), choose("rating"),
                                                       choose("speed"))

    # repeat at the margin, shuffle left of it, or in its place with repeat off
    # (%mm: off, all, one, shuffle, A-B)
    rep = frames("R", repeat[2])
    shuf = "%?ps<%?pa<%xd(Sb)|%xd(Sa)>|>"
    mode1_x = 320 - MARGIN - repeat[0]
    mode2_x = mode1_x - GAP - shuffle[0]
    mode_vps = "\n".join([
        "%%Vl(mode1,%d,%d,%d,%d,-)" % (mode1_x, MODES_Y, repeat[0], repeat[1]),
        "%%?mm<%s|%s>" % (shuf, "|".join(rep)),
        "%%Vl(mode2,%d,%d,%d,%d,-)" % (mode2_x, MODES_Y, shuffle[0], shuffle[1]),
        "%%?mm<|%s>" % "|".join([shuf] * 4)])

    spill_y = max(COVER_Y - 12, STATUS + 1)
    # %rr: none, then by half stars; the rating view has whole stars
    text = fill(WPS, images=images(True), vps=vps, margin=MARGIN, text_y=TEXT_Y,
                status=STATUS, block=block, block_vps="\n".join(block_vps),
                show_modes="%Vd(mode1)%Vd(mode2)", mode_vps=mode_vps,
                reflection=REFLECTION,
                cover=COVER, cover_y=COVER_Y, spill_x=MARGIN - 12, spill_y=spill_y,
                spill_w=COVER + 24, spill_h=COVER_Y + COVER + REFLECTION - spill_y,
                col=col, col_w=col_w, grid=grid, title_h=2 * TITLE_LINE,
                noart_x=(COVER - note[0]) // 2, noart_y=(COVER - note[1]) // 2,
                ratings_n=ratings[2],
                bar="%d,0,%d,%d,Bar Fill.bmp,backdrop,K" % (MARGIN, bar_w, bar_h),
                volume_bar="%d,0,%d,%d,Volume Fill.bmp,backdrop,W" % (
                    volume_x, volume_w, bar_h),
                lyrics_h=BAR_Y - 5 - 28, speed_y=BAR_Y - 3,
                bar_y=BAR_Y, bar_h=bar_h, times_y=TIMES_Y, times_right=320 - MARGIN - 64,
                under_x=MARGIN + 68, under_w=grid - 136,
                marks_y=BAR_Y + 1, left_x=left_x, left_w=left[0], left_h=left[1],
                right_x=right_x, right_w=right[0], right_h=right[1],
                stars_n=stars[2], stars_x=(320 - stars[0]) // 2, stars_w=stars[0],
                stars_h=stars[1], rating_y=BAR_Y - 1,
                big_stars="%%?rr<%s>" % "|".join([t[0]] + [t[(i + 1) // 2]
                                                            for i in range(1, 11)]),
                large_x=(200 - large[0]) // 2, large_y=(200 - large[1]) // 2)
    return text.replace("%?nN<{show}", show)


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, colours in COLOURS.items():
        for ext, text in (("sbs", sbs()), ("wps", wps())):
            for role, value in colours.items():
                text = text.replace("(%s)" % role, "(%s)" % value)
            with open(os.path.join(OUT, "%s.%s" % (name, ext)), "w") as f:
                f.write(text)


main()
