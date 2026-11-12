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

COLOURS = {
    # DENSE: small text, the clock (#56524D, #B8B1A8 as the screen has them);
    # SECONDARY is left to the bar's fill and the ring. CLOCK is the status
    # glyphs' grey, a step of the screen's from DENSE: the small text takes
    # the user's colour, which is known by DENSE; the clock stays as the
    # battery beside it is
    "Neo":      dict(PAPER="EFEBE7", PRIMARY="292421", SECONDARY="6B6963",
                     TERTIARY="A59E94", TRACK="E7DFD6", SELECTED="DEDBD6",
                     SURFACE="E7E3DE", DENSE="52514A", CLOCK="524D4A"),
    "Neo-Dark": dict(PAPER="211C18", PRIMARY="EFEBE7", SECONDARY="A59E94",
                     TERTIARY="6B6963", TRACK="313029", SELECTED="312C29",
                     SURFACE="312C29", DENSE="B5B2A5", CLOCK="B5AEA5"),
}

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
# Now Playing: the cover; the title's two lines of 18 beside it; the bar's box,
# as high as the knob, and the line under it. As much ground over the cover,
# from the hairline, as under that line's baseline (TIMES_Y + 11) to the
# screen's foot: 23 rows each
COVER, COVER_Y = 136, 44
TITLE_Y, TITLE_LINE = COVER_Y + 2, 18
BAR_Y, TIMES_Y = COVER_Y + 149, COVER_Y + 161


CLOCK = "%al%?cf<%cH:%cM|%cl:%cM>"


def frames(label, count):
    return ["%%xd(%s%s)" % (label, chr(ord("a") + i)) for i in range(count)]


def battery():
    """charging in turn, charged, or its level of twenty"""
    f = frames("B", SIZES["Battery"][2])
    levels = f[3:]
    return "%%?bp<%%?bc<%%t(1.0)%s;%%t(1.0)%s|%s>|%%?bl<%s>>" % (
        f[0], f[1], f[2], "|".join([levels[0]] + levels + [levels[-1]] * 2))


def status(width, modes, prefix="", name=None):
    """The glyphs' viewports and the line that shows them: from the right the
    battery, Night Mode's moon, play or pause or the padlock, and in Now
    Playing shuffle and repeat. What is left of an absent one moves up, so
    each has a viewport for every place it can be in. name: the text left of
    them, in a viewport that ends a gap before the leftmost; in the main
    menu (its title's icon, 33) the clock stands there instead, as it does
    at the left of a screen of the whole width."""
    out = []
    if name:
        out += ["%%Vl(%sclock,%d,%d,64,15,3)%%Vf(CLOCK)" % (prefix, MARGIN, TEXT_Y), CLOCK]

    def vp(tag, name, x):
        w, h, _ = SIZES[name]
        return "%%Vl(%s%s,%d,%d,%d,%d,-)" % (prefix, tag, x, FOOT - h, w, h)

    x = width - MARGIN - SIZES["Battery"][0]
    out += [vp("bat", "Battery", x), battery()]
    play = frames("P", 4)
    rep = frames("R", 4)
    # %mp: stopped, playing, paused, forward, rewind; %mm: off, all, one, shuffle, A-B
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
        at -= GAP + SIZES["Playing_Status"][0]
        out += [vp("play" + tag, "Playing_Status", at), state]
        this.append("%%Vd(%splay%s)" % (prefix, tag))
        if name:
            out += ["%%Vl(%sname%s,%d,%d,%d,15,3)%%Vf(PRIMARY)" % (
                prefix, tag, MARGIN, TEXT_Y, at - GAP - MARGIN), name]
            this.append("%%?if(%%Li, =, 33)<%%Vd(%sclock)|%%Vd(%sname%s)>" % (
                prefix, prefix, tag))
        if modes:
            inner = []
            for shuffle in (1, 0):
                a = at
                t = tag + "s%d" % shuffle
                s = ""
                if shuffle:
                    a -= GAP + SIZES["Shuffle"][0]
                    out += [vp("shuf" + tag, "Shuffle", a), "%?pa<%xd(Sb)|%xd(Sa)>"]
                    s = "%%Vd(%sshuf%s)" % (prefix, tag)
                a -= GAP + SIZES["Repeat"][0]
                out += [vp("rep" + t, "Repeat", a),
                        "%%?mm<|%s|%s|%s|%s>" % (rep[0], rep[1], rep[2], rep[3])]
                inner.append(s + "%%Vd(%srep%s)" % (prefix, t))
            this.append("%%?ps<%s|%s>" % (inner[0], inner[1]))
        branches.append("".join(this))
    show = "%%Vd(%sbat)%%?nN<%s|%s>" % (prefix, branches[1], branches[0])
    return "\n".join(out), show


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
%?LW<%VI(wide)%Vd(rulewide)%Vd(clockwide)%Vd(titlewide){wide}|%VI(half)%Vd(rulehalf)%Vd(column){half}>
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
# a screen that takes the whole width: the clock, its title, the glyphs
%Vl(clockwide,{margin},{text_y},64,15,3)%Vf(CLOCK)
%al%?cf<%cH:%cM|%cl:%cM>
%Vl(titlewide,84,{text_y},152,15,3)%Vf(PRIMARY)
%s%ac%Lt
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
{images}
# the views, cycled with the centre button:
# normal | scrubber | album art | lyrics | rating | speed
%?mw<%?nW(title)<%Vd(title)|%Vd(titlea)>%?mv(2.0)<%Vd(volume)|%Vd(pb)%Vd(under)>|%?nW(title)<%Vd(title)|%Vd(titlea)>%Vd(scrub)%?kk<%Vd(under)|%Vd(hints)|%Vd(hints)>|%?C<%Vd(bigart)|%Vd(bignote)>|%Vd(lyrics)%Vd(pb)|%?nW(title)<%Vd(title)|%Vd(titlea)>%Vd(rating)|%?nW(title)<%Vd(title)|%Vd(titlea)>%Vd(speed)>
# under the title by the track's kind: a song's artist and album; an
# episode's show and date; a book's author and chapter: 4 px under the
# title's last line, so higher under a title of one line (a) than of two (b).
# By the cover's foot a song's stars and format, an episode's time left, the speed
{lines}
%?mw<%?kk<%Vd(stars)||>|%?kk<%Vd(stars)||>||||>
# the cover, its corners rounded by the theme; without one, a note on a tile
# rounded as it is (%nc)
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
# with Angled Cover it is turned, its corners square, over a faint reflection
# of 10 rows, which ends above the bar; plain and rounded without
%Cl(0,0,{cover},{cover},c,c)
%Cr(10,56)
# where it is not shown, the ground where a turned cover and its reflection
# were, before what is drawn there: 12 px past the cover's place, 10 rows under.
# It must stay defined before noart, bigart, bignote and lyrics: viewports are
# drawn in the file's order, and it clears where they draw
%Vl(spill,{spill_x},{spill_y},{spill_w},{spill_h},-)
%Vl(art,{margin},{cover_y},{cover},{cover},-)
%Cd
%xl(O,Note Playing.bmp,{noart_x},{noart_y})
%xN(O)
%Vl(noart,{margin},{cover_y},{cover},{cover},-)%Vb(SELECTED)
%xd(O)%nc
# the title beside it on one line or two, which never scroll: %nw breaks it at
# a space and ends the second with an ellipsis
# (a viewport for each, so that one line's does not lie over what is under it)
%Vl(title,{col},{title_y},{col_w},{title_h},2)%Vf(PRIMARY)
%nw(1)
%nw(2)
%Vl(titlea,{col},{title_y},{col_w},{title_line},2)%Vf(PRIMARY)
%nw(1)
{line_vps}
%Vl(episode,{col},{foot_y},{col_w},14,4)%Vf(DENSE)
%al%kl%?ks< · %ks|>
%Vl(book,{col},{foot_y},{col_w},14,4)%Vf(DENSE)
%al%?ks<%ks|>
# five stars, the set ones and the outlines of the unset in one picture, their
# foot 6 clear rows over the format's sign, a column left of the texts' edge,
# where a star's point is; the set ones take the cover's colour with the text,
# an outline the theme's colour for it, or under Text and Stars the cover's
# muted one (%xC). The firmware knows an
# outline's pixel by its grey: make_theme_glyphs.py fails if another pixel
# of a %xC picture has it
%xl(G,Ratings.bmp,0,0,{ratings_n})
%xC(G)
%Vl(stars,{ratings_x},{stars_y},{ratings_w},{ratings_h},-)
{small_stars}
# the format and the bitrate under them
%Vl(song,{col},{foot_y},{col_w},14,4)%Vf(DENSE)
%fB
%Vl(bitrate,0,0,1,1,4)
# the bar: a capsule 5 px thick across the grid, in a box as high as the knob;
# under it on one line the time played, the song's place in its list or the
# chapter it is in, cut with an ellipsis and never scrolled, and the time left
%xl(K,Bar Track.bmp,0,0)
%xl(k,Knob.bmp,0,0)
%Vl(pb,0,{bar_y},320,{bar_h},-)
%pb({bar})
%Vl(pb,{margin},{times_y},64,14,4)%Vf(DENSE)
%al%pc
%Vl(pb,{times_right},{times_y},64,14,4)%Vf(DENSE)
%ar-%pr
# the scrubber: the same with its knob
%Vl(scrub,0,{bar_y},320,{bar_h},-)
%pb({bar},slider,k,slidercentre)
%Vl(scrub,{margin},{times_y},64,14,4)%Vf(DENSE)
%al%pc
%Vl(scrub,{times_right},{times_y},64,14,4)%Vf(DENSE)
%ar-%pr
%Vl(under,{under_x},{times_y},{under_w},14,4)%Vf(DENSE)
%nx%ac%?kn<%?kk<%kn%?kc< · %kc|>|%kn%?kc< · %kc|>|%kn %Sx(of) %kt>|%pp %Sx(of) %pe>
# a podcast's or a book's scrubber: what Previous and Next do there
%Vl(hints,{under_x},{times_y},{under_w},14,4)%Vf(DENSE)
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
%Vl(volume,{margin},{times_y},{grid},14,4)%Vf(DENSE)
%ac%nv
# rating: five large stars in the bar's place, the wheel sets them
%xl(T,Stars.bmp,0,0,{stars_n})
%xC(T)
%Vl(rating,{stars_x},{rating_y},{stars_w},{stars_h},-)
{big_stars}
%Vl(rating,{margin},{rating_text_y},{grid},14,4)%Vf(DENSE)
%ac%pp %Sx(of) %pe
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
    wide_vps, wide = status(320, False)
    half_vps, half = status(160, False, "h", "%al%s%Lt")
    music = ("%?C<%Vd(a)|%Vd(surface)%Vd(note)%?mp<%Vd(stopped)|%Vd(normal)|%Vd(normal)"
             "|%Vd(normal)|%Vd(normal)|%Vd(normal)|%Vd(normal)|%Vd(stopped)|%Vd(stopped)>>")
    note, photos = SIZES["Note_Pane"], SIZES["Photos_Pane"]
    return fill(SBS, images=images(False), wide=wide, half=half, wide_vps=wide_vps,
                half_vps=half_vps, music=music, margin=MARGIN, text_y=TEXT_Y,
                status=STATUS, list_top=LIST_TOP, list_rows=LIST_ROWS,
                photos_x=240 - photos[0] // 2, photos_y=104 - photos[1] // 2,
                photos_w=photos[0], photos_h=photos[1],
                note_x=240 - note[0] // 2, note_y=104 - note[1] // 2,
                note_w=note[0], note_h=note[1])


def wps():
    vps, show = status(320, True)
    ratings, stars = SIZES["Ratings"], SIZES["Stars"]
    left, right = SIZES["Volume_Left"], SIZES["Volume_Right"]
    note, large = SIZES["Note_Playing"], SIZES["Note_Large"]
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
    # the two lines under the title, for each kind, under one line and under two
    texts = {
        "song": ("%s%al%?ia<%ia|%?iA<%iA|%Sx(Unknown Artist)>>",
                 "%s%al%?id<%id|%Sx(Unknown Album)>"),
        "episode": ("%s%al%?id<%id|%Sx(Unknown Album)>", "%al%kd"),
        "book": ("%s%al%?ia<%ia|%?iA<%iA|%Sx(Unknown Author)>>", "%s%al%?kc<%kc|%id>"),
    }
    line_vps = []
    for suffix, count in (("a", 1), ("b", 2)):
        y = TITLE_Y + count * TITLE_LINE + 4
        for kind, (first, second) in texts.items():
            line_vps += ["%%Vl(%s%s,%d,%d,%d,17,3)%%Vf(PRIMARY)" % (kind, suffix, col, y, col_w),
                         first,
                         "%%Vl(%s%s,%d,%d,%d,17,3)%%Vf(DENSE)" % (kind, suffix, col, y + 17, col_w),
                         second]
    kinds = "%%?kk<%%Vd(song)%%Vd(song%s)|%%Vd(episode)%%Vd(episode%s)|%%Vd(book)%%Vd(book%s)>"
    under = "%%?nW(title)<%s|%s>" % (kinds % ("b", "b", "b"), kinds % ("a", "a", "a"))
    lines = "%%?mw<%s|%s|||%s|%s>" % (under, under, under, under)
    # %rr: none, then by half stars; the rating view has whole stars
    text = fill(WPS, images=images(True), vps=vps, margin=MARGIN, text_y=TEXT_Y,
                status=STATUS, lines=lines, line_vps="\n".join(line_vps),
                cover=COVER, cover_y=COVER_Y, spill_x=MARGIN - 12, spill_y=COVER_Y - 12,
                spill_w=COVER + 24, spill_h=COVER + 12 + 10, col=col, col_w=col_w, grid=grid,
                title_y=TITLE_Y, title_h=2 * TITLE_LINE, title_line=TITLE_LINE,
                ratings_x=col - 1, stars_y=COVER_Y + COVER - 20 - ratings[1],
                foot_y=COVER_Y + COVER - 15,
                noart_x=(COVER - note[0]) // 2, noart_y=(COVER - note[1]) // 2,
                ratings_n=ratings[2], ratings_w=ratings[0], ratings_h=ratings[1],
                small_stars="%%?rr<%s>" % "|".join(r),
                bar="%d,0,%d,%d,Bar Fill.bmp,backdrop,K" % (MARGIN, bar_w, bar_h),
                volume_bar="%d,0,%d,%d,Volume Fill.bmp,backdrop,W" % (
                    volume_x, volume_w, bar_h),
                lyrics_h=BAR_Y - 5 - 28, speed_y=BAR_Y - 3,
                bar_y=BAR_Y, bar_h=bar_h, times_y=TIMES_Y, times_right=320 - MARGIN - 64,
                under_x=MARGIN + 68, under_w=grid - 136,
                marks_y=BAR_Y + 1, left_x=left_x, left_w=left[0], left_h=left[1],
                right_x=right_x, right_w=right[0], right_h=right[1],
                stars_n=stars[2], stars_x=(320 - stars[0]) // 2, stars_w=stars[0],
                stars_h=stars[1], rating_y=BAR_Y - 1, rating_text_y=TIMES_Y + 10,
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
