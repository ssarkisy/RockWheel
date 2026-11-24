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
#
# Each screen has its skins: the iPod Video's 320x240 in Neo/.rockbox, the
# iPod photo's 220x176 in Neo/220x176/.rockbox, its own layout below (LAYOUTS)
# by the same rules, in smaller fonts. Its pane is beside the main menu alone,
# 110 px right of a hairline on column 109, and shows the music whatever row
# is selected; every other list, Music's menu too, takes the screen's width,
# as does the main menu with Cover Beside Menu off (the firmware's %LW).

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fnt_width  # noqa: E402

# the reflection's rows and its strength at the top, of 256: the firmware's
# default %Cr in the light skin; on the dark paper a cover's dark foot was
# lost at half, so the dark skin's starts stronger (owner, 2026-11-13)
REFLECT_LIGHT, REFLECT_DARK = "22,128", "22,176"
# the iPod photo's, of its cover's rows as these are of the Video's
REFLECT_LIGHT_220, REFLECT_DARK_220 = "14,128", "14,176"

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
for line in open(os.path.join(HERE, "neo_glyph_sizes.txt"), encoding="utf-8"):
    if not line.startswith("#"):
        name, w, h, count = line.split()
        SIZES[name] = (int(w), int(h), int(count))

# The layouts by screen, set in turn as globals (use()). The Video's:
VIDEO = dict(
    W=320, H=240,
    OUT=os.path.join(HERE, "..", "Neo", ".rockbox", "wps"),
    STATUS=20,              # the status line's rows; the hairline is the next
    MARGIN=14,              # the sides': the clock's start, the glyphs' end
    NP_MARGIN=14,           # Now Playing's
    # the status line's sides, and the list's width beside the pane (the
    # iPod photo's: its narrow sides, ui_theme.c)
    SBS_MARGIN=10, HALF_W=158,
    # the lists' area, to the screen's foot, whole rows of either kind: seven
    # of 30, or six of the 35 a row of two lines has; apps/gui/ui_theme.c has
    # the rows' insets
    LIST_TOP=29, LIST_ROWS=210,
    TEXT_Y=2,               # the status line's text, a cell of 15 (CELL)
    CELL=15,
    PCT_GAP=3,              # Battery Percentage's level to the battery
    FOOT=14,                # the row under the status glyphs' feet
    GAP=6,                  # between them
    CLOCK_W=64,             # the clock's viewport
    TITLE_X=84, TITLE_W=152,        # a list's title, in the middle
    NP_TITLE_X=100, NP_TITLE_W=120,  # Now Playing's
    # Now Playing: the cover 18 rows under the hairline, its reflection of 22
    # rows under it, so the rows left free over the cover and under the
    # reflection split about 3:1, as in Modern (owner, 2026-11-14); the bar's
    # box, as high as the knob, clear of the reflection, and the line of the
    # times under it, ending 12 rows over the screen's foot. The rating
    # view's stars stand where the bar is
    COVER=136, COVER_Y=38, COL_GAP=14,
    BAR_Y=202, TIMES_Y=214,
    REFLECTION=22,          # rows, as REFLECT_LIGHT and REFLECT_DARK have them
    REFLECTS=(REFLECT_LIGHT, REFLECT_DARK),
    # the song's details beside the cover: one block in the middle of its
    # height, of these rows each line has, with these clear rows over some
    TITLE_LINE=18,          # 18-Inter-SemiBold, one line or two
    DETAIL=16,              # artist and album, 15-Inter-Medium
    SMALL=14,               # the place in the list, the format, an episode's time left
    COUNT_GAP=4,            # over the place in the list
    STARS_GAP=6,            # over the stars
    FORMAT_GAP=5,           # over the format, or what is left of an episode or a book
    FORMAT_UNDER_STARS=3,   # over the format, under the stars
    # the modes: under the status line's right end, 4 rows under the hairline
    MODES_Y=25,
    TIME_W=64, TIME_ROWS=14,        # the times under the bar, 14-Inter-Medium
    BIG=200, BIG_X=60, BIG_Y=30,    # the album art view's cover
    LYRICS_Y=28,
    SPEED_H=44,             # the speed view's rows
    VOLUME_BAR=None,        # neo_glyph_sizes.txt's
    SBS_FONTS=("15-Inter-Medium", "15-Inter-SemiBold"),
    WPS_FONTS=("18-Inter-SemiBold", "17-Inter-Medium-13", "14-Inter-Medium",
               "15-Inter-SemiBold", "15-Inter-Medium"),
)
# The iPod photo's, the Video's by its own rules on a screen three quarters as
# high and two thirds as wide, and seen a little larger: a status line of 15
# rows, its text and glyphs a row higher than at 16, over the hairline; the
# lists' rows of 20 in 15 px type, eight of them under it, or five of 32 with
# a second line, whole rows of either kind (apps/gui/ui_theme.c); Now
# Playing's cover 96 px, 12 rows under the hairline, its reflection 14, the
# bar 4 under it and the times 8 over the foot. The glyphs are the Video's,
# at their own sizes; the bar's capsules are made for this width
# (make_theme_glyphs.py)
PHOTO = dict(
    W=220, H=176,
    OUT=os.path.join(HERE, "..", "Neo", "220x176", ".rockbox", "wps"),
    STATUS=15, MARGIN=10, SBS_MARGIN=4, HALF_W=109,
    # Now Playing's sides at the lists' edge (the screen is small, the
    # margins took the text's room); its status line's at MARGIN
    NP_MARGIN=6,
    LIST_TOP=16, LIST_ROWS=160,
    TEXT_Y=1, CELL=14, FOOT=12, GAP=5, PCT_GAP=2,
    CLOCK_W=48,
    TITLE_X=58, TITLE_W=104,
    NP_TITLE_X=62, NP_TITLE_W=96,
    COVER=96, COVER_Y=28, COL_GAP=8,
    BAR_Y=142, TIMES_Y=154,
    REFLECTION=14,
    REFLECTS=(REFLECT_LIGHT_220, REFLECT_DARK_220),
    TITLE_LINE=15,          # 15-Inter-SemiBold
    DETAIL=14,              # 14-Inter-Medium
    SMALL=14,               # 14-Inter-Medium: the place in the list has "of" in every alphabet
    COUNT_GAP=2, STARS_GAP=3, FORMAT_GAP=2, FORMAT_UNDER_STARS=1,
    MODES_Y=20,
    TIME_W=61, TIME_ROWS=14,        # "-44:44:44", the widest time of 10 h or more
    BIG=140, BIG_X=40, BIG_Y=26,    # Finder's frame of the iPod photo, 140 px
    LYRICS_Y=22,
    SPEED_H=40,
    VOLUME_BAR=160,
    # the main menu's pane: its first column, right of the hairline; the
    # cover fills its height, Finder's frame at the pane's size; Settings'
    # ring (neo_ring.h) and Photos' collage (photos_panel.h)
    PANE=110, PANE_COVER=176, PANE_RING=64, PANE_COLLAGE=132,
    SBS_FONTS=("14-Inter-Medium", "14-Inter-SemiBold"),
    WPS_FONTS=("15-Inter-SemiBold", "15-Inter-Medium", "14-Inter-Medium",
               "14-Inter-SemiBold", "14-Inter-Medium"),
)
LAYOUTS = (VIDEO, PHOTO)


def use(layout):
    globals().update(layout)


use(VIDEO)


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
    return ["%%Vl(%sn%dp,%d,%d,%d,%d,3)%%Vf(PRIMARY)" % (label, night, MARGIN, TEXT_Y, rooms[0],
                                                       CELL),
            text,
            "%%Vl(%sn%d,%d,%d,%d,%d,3)%%Vf(PRIMARY)" % (label, night, MARGIN, TEXT_Y, rooms[1],
                                                     CELL),
            text]


def title_show(label, night):
    """which of those: the padlock or the playing status shown, or nothing"""
    shown, empty = "%%Vd(%sn%dp)" % (label, night), "%%Vd(%sn%d)" % (label, night)
    return "%%?mh<%s|%%?mp<%s|%s|%s|%s|%s>>" % (shown, empty, shown, shown, shown, shown)


# a title that doesn't fit its place: left-aligned, never scrolled, ended in
# an ellipsis if it is wider still (%nx)
TITLE = "%al%nx%Lt"


# Settings > Appearance > Battery Percentage: the level left of the battery,
# right-aligned in the clock's font and colour, PCT_GAP from it; the glyph
# next to it, the playing status's or Night Mode's moon, by the level shown,
# its ink LEVEL_GAP from the first digit's (the owner's, 2026-11-24), the
# other GAP left of it; the titles' room as for 100 %, the glyphs' leftmost.
# Against the battery right to left too (%aR). None while the level isn't
# known (%bl's first case)
PCT_TEXT = "%aR%?bl<|%bl%%>"
LEVEL_GAP = 6
# the half-covered columns of the playing glyphs end at column 6 (from 0), the
# moon's at column 8
INK_END = {"Playing_Status": 6, "Night": 8}


def glyph_offsets():
    """With Battery Percentage, one viewport a place of the level's holds the
    glyphs next to it: from its left the playing status left of Night Mode's
    moon, the moon, and without the moon the playing status where the moon
    would be, as much further right as the moon's ink ends later. The moon's
    and that playing status's columns in it (M, and D and E: images())"""
    moon = GAP + SIZES["Playing_Status"][0]
    return moon, moon + INK_END["Night"] - INK_END["Playing_Status"]


def status(width, prefix="", name=None, pct=False, narrow=False, font=3,
           font_name=None):
    """The glyphs' viewports and the line that shows them: from the right the
    battery, Night Mode's moon, play or pause or the padlock (Now Playing's
    shuffle and repeat stand under the line). What is left of an absent one
    moves up, so each has a viewport for every place it can be in. name: the
    text left of them (title_vps()); in the main menu (its title's icon, 33)
    the clock stands there instead, as it does at the left of a screen of
    the whole width. Also the rooms left of the glyphs, by Night Mode: with
    the playing status's place and without.
    pct: with Battery Percentage, the level left of the battery in the font
    numbered font, font_name; the clock's viewport then only as wide as its
    widest time, clear of the glyphs. narrow: beside the iPod photo's pane,
    where the level takes the playing status's place, and Night Mode's moon
    or Hold's padlock the level's: 110 px have no room for the clock and all
    three"""
    out = []
    rooms = []
    if name:
        clock_w = fnt_width.clock_width(font_name) if pct else CLOCK_W
        out += ["%%Vl(%sclock,%d,%d,%d,%d,3)%%Vf(CLOCK)" % (prefix, MARGIN, TEXT_Y, clock_w,
                                                              CELL), CLOCK]

    def vp(tag, name, x):
        w, h, _ = SIZES[name]
        return "%%Vl(%s%s,%d,%d,%d,%d,-)" % (prefix, tag, x, FOOT - h, w, h)

    x = width - MARGIN - SIZES["Battery"][0]
    out += [vp("bat", "Battery", x), battery()]
    level_x = x
    if pct:
        pct_w = fnt_width.width(font_name, "100%")
        level_x = x - PCT_GAP - pct_w
        out += ["%%Vl(%spct,%d,%d,%d,%d,%d)%%Vf(CLOCK)" % (prefix, level_x, TEXT_Y, pct_w,
                                                            CELL, font), PCT_TEXT]
    def states(play, hold):
        # %mp: stopped, playing, paused, forward, rewind
        f = frames(play, 4)
        return "%%?mh<%%xd(%s)|%%?mp<|%s|%s|%s|%s>>" % (hold, f[0], f[1], f[2], f[3])
    state = states("P", "H")
    glyphs = ""
    if pct and not narrow:
        # by the level shown, one viewport a place (glyph_offsets()), one
        # conditional for either: Night Mode's places are the moon's, as
        # many columns left of the playing status's alone at every level
        places = {near: fnt_width.level_places(font_name, level_x + pct_w,
                                               1 + LEVEL_GAP + INK_END[near])
                  for near in ("Playing_Status", "Night")}
        moon_at, play_at = glyph_offsets()
        assert all(a - b == play_at - moon_at
                   for a, b in zip(places["Playing_Status"], places["Night"]))
        assert SIZES["Night"][1] == SIZES["Playing_Status"][1] == SIZES["Hold"][1]
        cases, select = fnt_width.level_cases(places["Playing_Status"])
        h = SIZES["Playing_Status"][1]
        for case, gx in cases:
            out += ["%%Vl(%sglyphs%s,%d,%d,%d,%d,-)" % (
                        prefix, case, gx - play_at, FOOT - h, play_at + SIZES["Playing_Status"][0],
                        h),
                    "%%?nN<%%xd(M)%s|%s>" % (state, states("D", "E"))]
        glyphs = select(lambda case: "%%Vd(%sglyphs%s)" % (prefix, case))
    if narrow:
        # Hold's padlock, without the playing status, in the level's place
        out += [vp("hold", "Playing_Status", x - GAP - SIZES["Playing_Status"][0]), "%xd(H)"]
    branches = []
    for night in (0, 1):
        at = x
        tag = "n%d" % night
        this = []
        if pct and not narrow:
            # the glyphs by the level shown (glyphs, above)
            this.append("%%Vd(%spct)" % prefix)
            near = "Night" if night else "Playing_Status"
            # the leftmost, 100 %'s
            past = min(places[near])
            at = past - (GAP + SIZES["Playing_Status"][0] if night else 0)
            if not night:
                past = level_x
            rooms.append((at - GAP - MARGIN, past - GAP - MARGIN))
            if name:
                out += title_vps(prefix + "name", night, rooms[night], name)
                this.append("%%?if(%%Li, =, 33)<%%Vd(%sclock)|%s>" % (
                    prefix, title_show(prefix + "name", night)))
            branches.append("".join(this))
            continue
        if pct and not (narrow and night):
            at = level_x
            this.append("%%Vd(%spct)" % prefix)
        if night:
            at -= GAP + SIZES["Night"][0]
            out += [vp("moon", "Night", at), "%xd(N)"]
            this.append("%%Vd(%smoon)" % prefix)
        if narrow:
            this = ["%%?mh<%%Vd(%shold)|%s>" % (prefix, this[-1])]
        past = at
        if not narrow:
            at -= GAP + SIZES["Playing_Status"][0]
            out += [vp("play" + tag, "Playing_Status", at), state]
            this.append("%%Vd(%splay%s)" % (prefix, tag))
        rooms.append((at - GAP - MARGIN, past - GAP - MARGIN))
        if name:
            out += title_vps(prefix + "name", night, rooms[night], name)
            this.append("%%?if(%%Li, =, 33)<%%Vd(%sclock)|%s>" % (
                prefix, title_show(prefix + "name", night)))
        branches.append("".join(this))
    show = "%%Vd(%sbat)%s%%?nN<%s|%s>" % (prefix, glyphs, branches[1], branches[0])
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
    # the glyphs next to Battery Percentage's level, in their viewport
    # (glyph_offsets())
    moon_at, play_at = glyph_offsets()
    out += ["%%xl(M,Night.bmp,%d,0)" % moon_at,
            "%%xl(D,Playing Status.bmp,%d,0,4)" % play_at,
            "%%xl(E,Hold.bmp,%d,0)" % play_at]
    return "\n".join(out)


def fill(text, **parts):
    for key, value in parts.items():
        text = text.replace("{" + key + "}", str(value))
    return text


SBS = """\
# Neo: the status line and the main menu's pane. Made by themes/tools/make_neo_theme.py
%wd
{fonts}
{images}
%?LW<%VI(wide)%Vd(rulewide){wideline}|%VI(half)%Vd(rulehalf)%Vd(column){halfline}>
# the pane by the row selected: Settings, Photos, or the music, which Extras has too
%?LW<|%?LS<%Vd(surface)%Vd(ring)%Vd(about)|{music}|%Vd(surface)%?nP<%Vd(collage)%Vd(photocount)|%Vd(nophotos)%Vd(photocaption)>|{music}>>
# the cover fills the pane, nothing over it: Finder's 240 px frame, its middle 160
%Cl(0,0,240,240,c,c)
%Vl(a,120,0,200,240,-)
%Cd
%Vl(a,120,0,40,240,-)
# the lists: under the status line and its hairline, rows of 30; beside the
# pane {half_w} wide, so that a column is left between the hairline and the
# scrollbar's capsule at the list's edge
%Vi(wide,0,{list_top},320,{list_rows},1)%Vf(PRIMARY)%Vb(PAPER)
%Vi(half,0,{list_top},{half_w},{list_rows},1)%Vf(PRIMARY)%Vb(PAPER)
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

# The iPod photo's: the pane beside the main menu alone, by the row selected
# as on the Video, in a pane of 110; the rest, and the main menu with Cover
# Beside Menu off, take the screen's width as on a stock iPod photo
SBS_PHOTO = """\
# Neo: the status line, and the main menu's pane or the lists of the screen's
# whole width. Made by themes/tools/make_neo_theme.py
%wd
{fonts}
{images}
%?LW<%VI(wide)%Vd(rulewide){wideline}|%VI(half)%Vd(rulehalf)%Vd(column){halfline}>
# the pane by the row selected: Settings, Photos, or the music, which Extras
# has too
%?LW<|%?LS<%Vd(surface)%Vd(ring)%Vd(about)|{music}|%Vd(surface)%?nP<%Vd(collage)%Vd(photocount)|%Vd(nophotos)%Vd(photocaption)>|{music}>>
# the music: the song's cover, or the note and under it the library's count
# or the track. The cover fills the pane, nothing over it, as on the Video: Finder's
# {cover} px, its middle {pane_w} columns. It is drawn from {cut} columns
# before the pane, which are the menu's, drawn after it
%Cl(0,0,{cover},{cover},c,c)
%Vl(a,{cover_x},0,{cover_w},{h},-)
%Cd
%Vl(a,{cover_x},0,{cut},{h},-)
# the lists: under the status line and its hairline; beside the pane {half_w}
# wide, so that the selected row ends as far from the hairline as it starts
# from the screen's edge
%Vi(wide,0,{list_top},{w},{list_rows},1)%Vf(PRIMARY)%Vb(PAPER)
%Vi(half,0,{list_top},{half_w},{list_rows},1)%Vf(PRIMARY)%Vb(PAPER)
# hairlines: under the status line, over the menu only beside the pane, and
# column {column} between the menu and the pane
%Vl(rulewide,0,{status},{w},1,-)%Vb(TRACK)
%Vl(rulehalf,0,{status},{column},1,-)%Vb(TRACK)
%Vl(column,{column},0,1,{h},-)%Vb(TRACK)
# a screen of the whole width: the clock, the title, the glyphs; in the main
# menu the clock alone; a title wider than its place between them takes the
# clock's too (%Lf, title_vps())
%Vl(clockwide,{margin},{text_y},{clock_w},{cell},3)%Vf(CLOCK)
%al%?cf<%cH:%cM|%cl:%cM>
%Vl(titlewide,{title_x},{text_y},{title_w},{cell},3)%Vf(PRIMARY)
%ac%Lt
{long_vps}
{wide_vps}
# beside the pane: the clock, and the glyphs at the menu's end
{half_vps}
# the pane's surface, under all but a cover
%Vl(surface,{pane},0,{pane_w},{h},-)%Vb(SURFACE)
# Settings: the disk's use as a ring of {ring} px (neo_ring.h), the used part
# in the second grey over a track of the ground's colour; what is free, and
# the whole, one block in the middle of the pane
%Vl(ring,{ring_x},{ring_y},{ring},{ring},-)%Vf(SECONDARY)%Vb(SURFACE)
%nR
%Vl(about,{about_x},{free_y},{about_w},{cell},3)%Vf(PRIMARY)%Vb(SURFACE)
%s%ac%nf
%Vl(about,{about_x},{total_y},{about_w},{cell},2)%Vf(DENSE)%Vb(SURFACE)
%ac%nT
# Photos: four rows of three of them, made at Updating Library
# (photos_panel.c), round and between which the pane shows, and their count
# under them; without any, their mark and "No Photos"
%Vl(collage,{pane},{collage_y},{pane_w},{collage_h},-)%Vb(SURFACE)
%xl(Q,/.rockbox/photos_panel.bmp,0,0)
%xd(Q)
%Vl(photocount,{about_x},{count_y},{about_w},{cell},2)%Vf(DENSE)%Vb(SURFACE)
%s%ac%nP
%xl(I,Photos Pane.bmp,0,0)
%xN(I)
%Vl(nophotos,{photos_x},{photos_y},{photos_w},{photos_h},-)%Vb(SURFACE)
%xd(I)
%Vl(photocaption,{about_x},{nophotos_y},{about_w},{cell},2)%Vf(DENSE)%Vb(SURFACE)
%ac%nP
# the music without a cover: the note, a mask drawn in the theme's two
# colours for it (%xN), which take the theme's tone as the surfaces do, and
# under it the library's count or the track
%xl(O,Note Pane.bmp,0,0)
%xN(O)
%Vl(note,{note_x},{note_y},{note_w},{note_h},-)%Vb(SURFACE)
%xd(O)
%Vl(stopped,{pane},{caption_y},{pane_w},{cell},2)%Vf(DENSE)%Vb(SURFACE)
%ac%ns
%Vl(normal,{caption_x},{caption_y},{caption_w},{cell},2)%Vf(DENSE)%Vb(SURFACE)
%s%ac%?it<%it|%?fn<%fn|>>
"""

WPS = """\
# Neo: Now Playing. Made by themes/tools/make_neo_theme.py
%wd
{fonts}
{images}
# the status line's clock and title: with Battery Percentage where the glyphs
# leave room
{np_show}
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
# note on a tile of its size (%nc), which the firmware draws as the cover:
# its reflection and corners, turned with Angled Cover
%?mw<%?C<%Vd(art)|%Vd(spill)%Vd(noart)>|%?C<%Vd(art)|%Vd(spill)%Vd(noart)>|%Vd(spill)|%Vd(spill)|%?C<%Vd(art)|%Vd(spill)%Vd(noart)>|%?C<%Vd(art)|%Vd(spill)%Vd(noart)>>
# the status line: the clock, the title, the glyphs, a hairline under it. Its
# ground first, a viewport of its own: under Cover Colours' Fill the firmware
# gives what lies in these rows a shade of the cover's colour
%V(0,0,{w},{status},-)
{np_vps}
%V(0,{status},{w},1,-)%Vb(TRACK)
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
# of a %xC picture has it. Right to left (%Sr) their mirror, make_rtl_stars.py's,
# a column right of the texts' other edge and set from the right, in the same
# viewport, which has the column's width and a column each side
%xl(G,Ratings.bmp,0,0,{ratings_n})
%xC(G)
%xl(g,Ratings RTL.bmp,{ratings_rtl_x},0,{ratings_n})
%xC(g)
{block_vps}
# never shown, it only gives %fB the font of the bitrate
%Vl(bitrate,0,0,1,1,4)
# the bar: a capsule across the grid, in a box as high as the knob (the
# pictures are make_theme_glyphs.py's); under it on one line the time played,
# the chapter of a track that has them, cut with an ellipsis and never
# scrolled, and the time left
%xl(K,Bar Track.bmp,0,0)
%xl(k,Knob.bmp,0,0)
%Vl(pb,0,{bar_y},{w},{bar_h},-)
%pb({bar})
%Vl(pb,{margin},{times_y},{time_w},{time_rows},4)%Vf(NEUTRAL)
%al%pc
%Vl(pb,{times_right},{times_y},{time_w},{time_rows},4)%Vf(NEUTRAL)
%ar-%pr
# the scrubber: the same with its knob
%Vl(scrub,0,{bar_y},{w},{bar_h},-)
%pb({bar},slider,k,slidercentre)
%Vl(scrub,{margin},{times_y},{time_w},{time_rows},4)%Vf(NEUTRAL)
%al%pc
%Vl(scrub,{times_right},{times_y},{time_w},{time_rows},4)%Vf(NEUTRAL)
%ar-%pr
%Vl(under,{under_x},{times_y},{under_w},{time_rows},4)%Vf(NEUTRAL)
%nx%ac%?kn<%?kk<%kn%?kc< · %kc|>|%kn%?kc< · %kc|>|%kn %Sx(of) %kt>|>
# a podcast's or a book's scrubber: what Previous and Next do there
%Vl(hints,{under_x},{times_y},{under_w},{time_rows},4)%Vf(NEUTRAL)
%ac-15 / +30
# the volume in the bar's place, as the wheel turns, shorter between its marks,
# whose ink is as far from it on either side
%xl(W,Volume Track.bmp,0,0)
%xl(v,Volume Left.bmp,0,0)
%xl(V,Volume Right.bmp,0,0)
%Vl(volume,0,{bar_y},{w},{bar_h},-)
%pv({volume_bar})
%Vl(volume,{left_x},{marks_y},{left_w},{left_h},-)
%xd(v)
%Vl(volume,{right_x},{marks_y},{right_w},{right_h},-)
%xd(V)
%Vl(volume,{margin},{times_y},{grid},{time_rows},4)%Vf(NEUTRAL)
%ac%nv
# rating: five large stars in the bar's place, the wheel sets them; right to
# left their mirror, set from the right
%xl(T,Stars.bmp,0,0,{stars_n})
%xC(T)
%xl(t,Stars RTL.bmp,0,0,{stars_n})
%xC(t)
%Vl(rating,{stars_x},{rating_y},{stars_w},{stars_h},-)
{big_stars}
# the large cover, and the note in its place
%Vl(bigart,{big_x},{big_y},{big},{big},-)
%Cl(0,0,{big},{big},c,c)
%Cd
%xl(L,Note Large.bmp,{large_x},{large_y})
%xN(L)
%Vl(bignote,{big_x},{big_y},{big},{big},-)%Vb(SELECTED)
%xd(L)%nc
# the lyrics, lines of the text font, over the bar; and the speeds: the WPS
# code draws into these
%Vl(lyrics,{margin},{lyrics_y},{grid},{lyrics_h},3)%Vf(PRIMARY)
%Vl(speed,0,{speed_y},{w},{speed_h},4)%Vf(DENSE)
"""


def fonts(names):
    """the skin's fonts by their numbers, from 2"""
    return "\n".join("%%Fl(%d,%s.fnt)" % (i + 2, name)
                     for i, name in enumerate(names) if name)


def sbs():
    """the status bar's skin, its sides SBS_MARGIN; Now Playing's are MARGIN"""
    global MARGIN
    keep, MARGIN = MARGIN, SBS_MARGIN
    try:
        return sbs_text()
    finally:
        MARGIN = keep


def centred(label, x, w, limit, font, text):
    """a title in the middle of x..x+w, narrower if it would reach limit"""
    centre = x + w // 2
    half = min(w // 2, limit - centre)
    return ["%%Vl(%s,%d,%d,%d,%d,%d)%%Vf(PRIMARY)" % (label, centre - half, TEXT_Y, 2 * half,
                                                     CELL, font), text]


def battery_line(on, off):
    """Battery Percentage's line, or the one without it"""
    return "%%?St(battery percentage)<%s|%s>" % (on, off)


def sbs_text():
    wide_vps, wide, rooms = status(W)
    long_vps = "\n".join(title_vps("long", 0, rooms[0], TITLE)
                         + title_vps("long", 1, rooms[1], TITLE))
    title = "%%?Lf(titlewide)<%%Vd(clockwide)%%Vd(titlewide)|%%?nN<%s|%s>>" % (
        title_show("long", 1), title_show("long", 0))
    # with Battery Percentage: the glyphs further left, the titles shorter;
    # the one in the middle a gap short of the leftmost glyph shown
    pwide_vps, pwide, prooms = status(W, "p", pct=True, font_name=SBS_FONTS[1])
    ptitle_vps = []
    for night in (0, 1):
        ptitle_vps += centred("ptitlewide%d" % night, TITLE_X, TITLE_W,
                              prooms[night][0] + MARGIN, 3, "%ac%Lt")
    pwide_vps = "\n".join([pwide_vps] + ptitle_vps + title_vps("plong", 0, prooms[0], TITLE)
                          + title_vps("plong", 1, prooms[1], TITLE))
    ptitle = "%%?nN<%s|%s>" % tuple(
        "%%?Lf(ptitlewide%d)<%%Vd(clockwide)%%Vd(ptitlewide%d)|%s>" % (n, n, title_show("plong", n))
        for n in (1, 0))
    if W < 320:
        wideline = battery_line("%%?if(%%Li, =, 33)<%%Vd(clockwide)|%s>%s" % (ptitle, pwide),
                                "%%?if(%%Li, =, 33)<%%Vd(clockwide)|%s>%s" % (title, wide))
    else:
        wideline = battery_line(ptitle + pwide, title + wide)
    wide_vps = wide_vps + "\n" + pwide_vps
    music = ("%?C<%Vd(a)|%Vd(surface)%Vd(note)%?mp<%Vd(stopped)|%Vd(normal)|%Vd(normal)"
             "|%Vd(normal)|%Vd(normal)|%Vd(normal)|%Vd(normal)|%Vd(stopped)|%Vd(stopped)>>")
    note, photos = SIZES["Note_Pane"], SIZES["Photos_Pane"]
    if W < 320:
        # the pane's note and caption one block in its middle, the caption's
        # capitals 12 rows under the note
        half_vps, half, _ = status(PANE, "h", TITLE)
        phalf_vps, phalf, _ = status(PANE, "ph", TITLE, pct=True, narrow=True,
                                     font_name=SBS_FONTS[1])
        halfline = battery_line(phalf, half)
        half_vps = half_vps + "\n" + phalf_vps
        pane_w = W - PANE
        cut = (PANE_COVER - pane_w) // 2
        note_y = (H - (note[1] + 12 + CELL)) // 2
        # Settings' ring and its two lines, Photos' collage and its count, and
        # their mark and caption: each one block in the middle of the pane
        ring_y = (H - (PANE_RING + 10 + 2 * CELL + 2)) // 2
        collage_y = (H - (PANE_COLLAGE + 6 + CELL)) // 2
        photos_y = (H - (photos[1] + 12 + CELL)) // 2
        return fill(SBS_PHOTO, fonts=fonts(SBS_FONTS), images=images(False),
                    wideline=wideline, halfline=halfline, wide_vps=wide_vps,
                    half_vps=half_vps, long_vps=long_vps, music=music, margin=MARGIN, text_y=TEXT_Y, cell=CELL,
                    clock_w=CLOCK_W, title_x=TITLE_X, title_w=TITLE_W, status=STATUS,
                    list_top=LIST_TOP, list_rows=LIST_ROWS, w=W, h=H,
                    half_w=HALF_W, column=PANE - 1, pane=PANE, pane_w=pane_w,
                    cover=PANE_COVER, cover_x=PANE - cut, cover_w=W - (PANE - cut),
                    cut=cut, note_x=PANE + (pane_w - note[0]) // 2, note_y=note_y,
                    note_w=note[0], note_h=note[1], caption_y=note_y + note[1] + 12,
                    caption_x=PANE + 6, caption_w=pane_w - 12,
                    ring=PANE_RING, ring_x=PANE + (pane_w - PANE_RING) // 2, ring_y=ring_y,
                    free_y=ring_y + PANE_RING + 10, total_y=ring_y + PANE_RING + 10 + CELL + 2,
                    about_x=PANE + 3, about_w=pane_w - 6,
                    collage_y=collage_y, collage_h=PANE_COLLAGE,
                    count_y=collage_y + PANE_COLLAGE + 6,
                    photos_x=PANE + (pane_w - photos[0]) // 2, photos_y=photos_y,
                    photos_w=photos[0], photos_h=photos[1],
                    nophotos_y=photos_y + photos[1] + 12)
    half_vps, half, _ = status(160, "h", TITLE)
    phalf_vps, phalf, _ = status(160, "ph", TITLE, pct=True, font_name=SBS_FONTS[1])
    halfline = battery_line(phalf, half)
    half_vps = half_vps + "\n" + phalf_vps
    return fill(SBS, fonts=fonts(SBS_FONTS), images=images(False), wideline=wideline,
                halfline=halfline, wide_vps=wide_vps,
                half_vps=half_vps, long_vps=long_vps, music=music, margin=MARGIN, text_y=TEXT_Y,
                status=STATUS, list_top=LIST_TOP, list_rows=LIST_ROWS, half_w=HALF_W,
                photos_x=240 - photos[0] // 2, photos_y=104 - photos[1] // 2,
                photos_w=photos[0], photos_h=photos[1],
                note_x=240 - note[0] // 2, note_y=104 - note[1] // 2,
                note_w=note[0], note_h=note[1])


def detail_block(parts):
    """parts: (what, rows, clear rows over it); each one's top row, the block
    in the middle of the cover's height"""
    y = COVER_Y + COVER // 2 - sum(rows + gap for _, rows, gap in parts) // 2
    # but under the modes, whose glyphs stand at the column's right end: on
    # a small screen the tallest block reaches up to them
    y = max(y, MODES_Y + SIZES["Repeat"][1] + 2)
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


NP_TITLE = "%s%ac%Sx(Now Playing)"


def np_status(prooms):
    """Now Playing's clock and title, and with Battery Percentage theirs: on
    the iPod photo the title in the middle of the room between the clock,
    as wide as its widest time, and the glyphs; on the Video in the screen's
    middle, narrower if it would reach them"""
    out = ["%%Vl(npclock,%d,%d,%d,%d,5)%%Vf(CLOCK)" % (MARGIN, TEXT_Y, CLOCK_W, CELL), CLOCK,
           "%%Vl(nptitle,%d,%d,%d,%d,5)%%Vf(PRIMARY)" % (NP_TITLE_X, TEXT_Y, NP_TITLE_W, CELL),
           NP_TITLE]
    clock_w = fnt_width.clock_width(WPS_FONTS[3]) if W < 320 else CLOCK_W
    out += ["%%Vl(pnpclock,%d,%d,%d,%d,5)%%Vf(CLOCK)" % (MARGIN, TEXT_Y, clock_w, CELL), CLOCK]
    for night in (0, 1):
        limit = prooms[night][0] + MARGIN       # a gap left of the leftmost glyph
        if W < 320:
            left = MARGIN + clock_w + GAP
            out += ["%%Vl(pnptitle%d,%d,%d,%d,%d,5)%%Vf(PRIMARY)" % (
                night, left, TEXT_Y, limit - left, CELL), NP_TITLE]
        else:
            out += centred("pnptitle%d" % night, NP_TITLE_X, NP_TITLE_W, limit, 5, NP_TITLE)
    show = battery_line("%Vd(pnpclock)%?nN<%Vd(pnptitle1)|%Vd(pnptitle0)>",
                        "%Vd(npclock)%Vd(nptitle)")
    return "\n".join(out), show


def wps():
    vps, show, _ = status(W)
    pvps, pshow, prooms = status(W, "p", pct=True, font=5, font_name=WPS_FONTS[3])
    np_vps, np_show = np_status(prooms)
    vps = vps + "\n" + pvps
    show = battery_line(pshow, show)
    ratings, stars = SIZES["Ratings"], SIZES["Stars"]
    left, right = SIZES["Volume_Left"], SIZES["Volume_Right"]
    note, large = SIZES["Note_Playing"], SIZES["Note_Large"]
    repeat, shuffle = SIZES["Repeat"], SIZES["Shuffle"]
    bar_w, bar_h, _ = SIZES["Bar"]
    volume_w = SIZES["Volume_Bar"][0]
    if W != 320:
        # the capsules make_theme_glyphs.py made for this screen
        bar_w = W - 2 * NP_MARGIN
        volume_w = VOLUME_BAR
        for name, width in (("Bar", bar_w), ("Volume", volume_w)):
            with open(os.path.join(OUT, "Neo", name + " Track.bmp"), "rb") as f:
                assert struct.unpack_from("<i", f.read(26), 18)[0] == width, name
    # the marks' ink 6 px from the bar's ends: the left one's ends a column
    # before its picture does, the right one's starts a column in (the export)
    volume_x = (W - volume_w) // 2
    left_x = volume_x - 6 - (left[0] - 1)
    right_x = volume_x + volume_w + 6 - 1
    r, r_rtl = frames("G", ratings[2]), frames("g", ratings[2])
    t, t_rtl = frames("T", stars[2]), frames("t", stars[2])
    grid = W - 2 * NP_MARGIN
    col = NP_MARGIN + COVER + COL_GAP
    col_w = W - NP_MARGIN - col

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
                block_vps += ["%%Vl(%s,%d,%d,%d,%d,-)" % (label, col - 1, y, col_w + 2, rows),
                              "%%?Sr<%%?rr<%s>|%%?rr<%s>>" % ("|".join(r_rtl), "|".join(r))]
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
    mode1_x = W - NP_MARGIN - repeat[0]
    mode2_x = mode1_x - GAP - shuffle[0]
    mode_vps = "\n".join([
        "%%Vl(mode1,%d,%d,%d,%d,-)" % (mode1_x, MODES_Y, repeat[0], repeat[1]),
        "%%?mm<%s|%s>" % (shuf, "|".join(rep)),
        "%%Vl(mode2,%d,%d,%d,%d,-)" % (mode2_x, MODES_Y, shuffle[0], shuffle[1]),
        "%%?mm<|%s>" % "|".join([shuf] * 4)])

    # the firmware's ART_SPILL round the cover, on the screen
    spill_x, spill_y = max(NP_MARGIN - 12, 0), max(COVER_Y - 12, STATUS + 1)
    # %rr: none, then by half stars; the rating view has whole stars
    text = fill(WPS, fonts=fonts(WPS_FONTS), images=images(True), vps=vps, margin=NP_MARGIN,
                np_vps=np_vps, np_show=np_show,
                text_y=TEXT_Y, w=W, cell=CELL, clock_w=CLOCK_W, np_title_x=NP_TITLE_X,
                np_title_w=NP_TITLE_W, time_w=TIME_W, time_rows=TIME_ROWS, big=BIG,
                big_x=BIG_X, big_y=BIG_Y, lyrics_y=LYRICS_Y, speed_h=SPEED_H,
                status=STATUS, block=block, block_vps="\n".join(block_vps),
                show_modes="%Vd(mode1)%Vd(mode2)", mode_vps=mode_vps,
                reflection=REFLECTION,
                cover=COVER, cover_y=COVER_Y, spill_x=spill_x, spill_y=spill_y,
                spill_w=NP_MARGIN + COVER + 12 - spill_x,
                spill_h=COVER_Y + COVER + REFLECTION - spill_y,
                col=col, col_w=col_w, grid=grid, title_h=2 * TITLE_LINE,
                noart_x=(COVER - note[0]) // 2, noart_y=(COVER - note[1]) // 2,
                ratings_n=ratings[2], ratings_rtl_x=col_w + 2 - ratings[0],
                bar="%d,0,%d,%d,Bar Fill.bmp,backdrop,K" % (NP_MARGIN, bar_w, bar_h),
                volume_bar="%d,0,%d,%d,Volume Fill.bmp,backdrop,W" % (
                    volume_x, volume_w, bar_h),
                # the speeds' rows from the bar, but on the screen: a skin
                # with a viewport past its foot doesn't load at all
                lyrics_h=BAR_Y - 5 - LYRICS_Y, speed_y=min(BAR_Y - 3, H - SPEED_H),
                bar_y=BAR_Y, bar_h=bar_h, times_y=TIMES_Y, times_right=W - NP_MARGIN - TIME_W,
                under_x=NP_MARGIN + TIME_W + 4, under_w=grid - 2 * (TIME_W + 4),
                marks_y=BAR_Y + 1, left_x=left_x, left_w=left[0], left_h=left[1],
                right_x=right_x, right_w=right[0], right_h=right[1],
                stars_n=stars[2], stars_x=(W - stars[0]) // 2, stars_w=stars[0],
                stars_h=stars[1], rating_y=BAR_Y - 1,
                big_stars="%%?Sr<%s|%s>" % tuple(
                    "%%?rr<%s>" % "|".join([f[0]] + [f[(i + 1) // 2] for i in range(1, 11)])
                    for f in (t_rtl, t)),
                large_x=(BIG - large[0]) // 2, large_y=(BIG - large[1]) // 2)
    return text.replace("%?nN<{show}", show)


def main():
    for layout in LAYOUTS:
        use(layout)
        os.makedirs(OUT, exist_ok=True)
        for (name, colours), reflect in zip(COLOURS.items(), REFLECTS):
            colours = dict(colours, REFLECT=reflect)
            for ext, text in (("sbs", sbs()), ("wps", wps())):
                for role, value in colours.items():
                    text = text.replace("(%s)" % role, "(%s)" % value)
                with open(os.path.join(OUT, "%s.%s" % (name, ext)), "w",
                          encoding="utf-8") as f:
                    f.write(text)


main()
