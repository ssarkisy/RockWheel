#!/usr/bin/env python3
# Makes Modern's skins, light and dark, from the texts below, its colours and
# the sizes of its glyphs:
#
#   themes/tools/make_modern_theme.py
#
# Neither skin is edited by hand: change the text or the colours here and run
# it again. Run make_modern_glyphs.py first: it makes the skins' bitmaps from
# the mockups' export and writes their sizes to modern_glyph_sizes.txt, by
# which they are placed here. The colours are those of apps/gui/ui_theme.c's
# entry for Modern, the mockup's (work/mockups/ipod-ios13.html) as the screen
# has them. A light and a dark skin name the same fonts and the same sizes of
# covers, so Dark Theme comes with the music playing.
#
# The places are the mockup's: a status bar of 24 rows across every screen,
# its last row a hairline; the lists under it, in Neo's rows of 30 (roadmap
# 1.4.1, A16); the main menu 160 wide, as Classic's and Neo's, beside a pane
# of 160, which the cover fills.
#
# Each screen has its skins: the iPod Video's 320x240 in Modern/.rockbox, the
# iPod photo's 220x176 in Modern/220x176/.rockbox, its own layout below
# (LAYOUTS) by the same rules, in smaller fonts. Its pane is beside the main
# menu alone, 110 px, and shows the music whatever row is selected; every
# other list, Music's menu too, takes the screen's width, as does the main
# menu with Cover Beside Menu off (the firmware's %LW).

import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))

# the reflection's rows and its strength at the top, of 256: the firmware's
# default %Cr in the light skin; on black a cover's dark foot was lost at
# half, so the dark skin's starts stronger (owner, 2026-11-13)
REFLECT_LIGHT, REFLECT_DARK = "22,128", "22,176"
# the iPod photo's, of its cover's rows as these are of the Video's
REFLECT_LIGHT_220, REFLECT_DARK_220 = "14,128", "14,176"

COLOURS = {
    "Modern":      dict(GROUND="FFFFFF", INK="000000", SMALL="525152",
                        MUTED="73757B", ACCENT="FF2D52", ACCENTTEXT="DE244A",
                        TOP="F7F7F7", RULE="E6E7EF", PANE="EFF3F7",
                        TILE="EFF3F7",
                        COLUMN="E6E7EF", REFLECT=REFLECT_LIGHT),
    "Modern-Dark": dict(GROUND="000000", INK="FFFFFF", SMALL="B4B6BD",
                        MUTED="8C8E94", ACCENT="FF345A", ACCENTTEXT="FF345A",
                        TOP="101010", RULE="202429", PANE="000000",
                        # a missing cover's tile, as a row's (UI_COLOUR_COVER_TILE):
                        # on the pane's black the note had no tile
                        TILE="181C20",
                        COLUMN="181818", REFLECT=REFLECT_DARK),
}

SIZES = {}
for line in open(os.path.join(HERE, "modern_glyph_sizes.txt")):
    if not line.startswith("#"):
        name, w, h, count = line.split()
        SIZES[name] = (int(w), int(h), int(count))

# The layouts by screen, set in turn as globals (use()). The Video's:
VIDEO = dict(
    W=320, H=240,
    OUT=os.path.join(HERE, "..", "Modern", ".rockbox", "wps"),
    STATUS=24,              # the status bar's rows, the last its hairline
    MARGIN=12,              # the sides'
    # to the screen's foot: seven rows of 30, or five of the 36 a row of two
    # lines has and a part of the sixth
    LIST_TOP=28, LIST_ROWS=212,
    SPLIT=160,              # the main menu's width; the pane is the rest
    TEXT_Y=5,               # the status bar's text, a cell of 14 with its baseline on row 16
    GLYPH_Y=7,              # its glyphs' top
    GLYPH_GAPS=(11, 6),     # the battery to play or pause, that to the moon
    CLOCK_W=56,
    TITLE_X=72, TITLE_W=168,
    # Now Playing: the cover, its reflection under it (REFLECT_LIGHT/DARK's
    # rows), and the column beside it
    COVER_TOP=36, COVER=140,
    REFLECTION=22,
    REFLECTS=(REFLECT_LIGHT, REFLECT_DARK),
    COL=168,                # the column's left edge
    # the song's details: one block in the middle of the cover's height, of
    # these rows each line has, with these clear rows over some
    TITLE_ROWS=18,          # 18-Inter-SemiBold, Neo's title
    ARTIST_ROWS=17,         # 17-Inter-Medium, with a clear row over it and the next
    SMALL_ROWS=14,          # 14-Inter-Medium: the album, the place in the list, the format
    # the bar's box, as high as the large knob, 6 rows lower than in 1.4
    # where the song's place in its list was; the times under it
    BAR_Y=202, TIMES_Y=214,
    TIME_W=64,
    MODES_Y=30,             # repeat and shuffle under the status bar's right end
    VOLUME_X=30, VOLUME_W=180,      # the volume's figure
    BIG=200, BIG_X=60, BIG_Y=32,    # the album art view's cover
    LYRICS_Y=30, LYRICS_H=180,
    THICK_Y=211, LOWTIME_W=100,     # the lyrics view's bar and its times
    SBS_FONTS=("14-Inter-SemiBold", "18-Inter-SemiBold", "14-Inter-Medium",
               "17-Inter-SemiBold"),
    WPS_FONTS=("14-Inter-SemiBold", "18-Inter-SemiBold", "17-Inter-Medium",
               "14-Inter-Medium"),
)
# The iPod photo's, the Video's by its own rules on a screen three quarters as
# high and two thirds as wide, and seen a little larger: a status bar of 16
# rows and its hairline; the lists' rows of 22 in 15 px type, seven of them
# under it, or of 34 with a second line (apps/gui/ui_theme.c); Now Playing's
# cover 96 px, 9 rows under the hairline, its reflection 14, the bar 4 under
# it and the times 10 over the foot. The glyphs are the Video's, at their own
# sizes; the bars are made for this width (make_modern_glyphs.py)
PHOTO = dict(
    W=220, H=176,
    OUT=os.path.join(HERE, "..", "Modern", "220x176", ".rockbox", "wps"),
    STATUS=17, MARGIN=9,
    LIST_TOP=19, LIST_ROWS=157,
    SPLIT=None,
    TEXT_Y=1, GLYPH_Y=3,
    GLYPH_GAPS=(8, 5),
    CLOCK_W=44,
    TITLE_X=60, TITLE_W=100,
    COVER_TOP=26, COVER=96,
    REFLECTION=14,
    REFLECTS=(REFLECT_LIGHT_220, REFLECT_DARK_220),
    COL=116,
    TITLE_ROWS=15,          # 15-Inter-SemiBold
    ARTIST_ROWS=15,         # 15-Inter-Medium
    SMALL_ROWS=14,          # 14-Inter-Medium
    BAR_Y=140, TIMES_Y=152,
    TIME_W=64,              # "−44:44:44", the widest time of 10 h or more
    MODES_Y=20,
    VOLUME_X=26, VOLUME_W=140,
    BIG=140, BIG_X=40, BIG_Y=26,    # Finder's frame of the iPod photo, 140 px
    LYRICS_Y=21, LYRICS_H=120,
    THICK_Y=143, LOWTIME_W=80,
    # the main menu's pane: its first column, which has its hairline; the
    # cover fills its height, Finder's frame at the pane's size
    PANE=110, PANE_COVER=176,
    SBS_FONTS=("14-Inter-SemiBold", "14-Inter-Medium"),
    WPS_FONTS=("14-Inter-SemiBold", "15-Inter-SemiBold", "15-Inter-Medium",
               "14-Inter-Medium"),
)
LAYOUTS = (VIDEO, PHOTO)
# the gaps in the column, the same on either screen
STAR_SHIFT = 1          # the stars' row left of the text's edge
LINE_GAP = 1
COUNT_GAP = 4           # over the place in the list
STARS_GAP = 6           # over the stars
FORMAT_GAP = 5          # over the format, or what is left of an episode or a book
FORMAT_UNDER_STARS = 3  # over the format, under the stars
# the pane's pictures and their captions: clear rows from a picture to its
# caption's capitals; and in a font's cell, the row its capitals start on and
# the one its letters stand on
PANE_GAP = 8
CELL_14 = (3, 10)             # 14-Inter-Medium
CELL_17_SEMIBOLD = (4, 12)
CELL_18_SEMIBOLD = (3, 13)


def use(layout):
    globals().update(layout)


use(VIDEO)


def frames(label, count):
    return ["%%xd(%s%s)" % (label, chr(ord("a") + i)) for i in range(count)]


def battery():
    """charging in turn, charged, or its level of twenty"""
    f = frames("B", SIZES["Battery"][2])
    levels = f[3:]
    return "%%?bp<%%?bc<%%t(1.0)%s;%%t(1.0)%s|%s>|%%?bl<%s>>" % (
        f[0], f[1], f[2], "|".join([levels[0]] + levels + [levels[-1]] * 2))


def glyphs_x(right=None):
    """the bar's glyphs' left columns: the battery, play or pause or the
    padlock, Night Mode's moon; from the screen's margin if no right end is
    given"""
    if right is None:
        right = W - MARGIN
    bat_x = right - SIZES["Battery"][0]
    play_x = bat_x - GLYPH_GAPS[0] - SIZES["Playing_Status"][0]
    moon_x = play_x - GLYPH_GAPS[1] - SIZES["Night"][0]
    return bat_x, play_x, moon_x


def long_title():
    """A title wider than its place between the clock and the glyphs takes
    the clock's too (%Lf): left-aligned from the margin to 6 px before the
    leftmost glyph shown, the moon (longm), the playing status or the
    padlock (longp), or the battery (long); never scrolled, ended in an
    ellipsis if it is wider still. Its viewports, and which shows"""
    bat_x, play_x, moon_x = glyphs_x()
    out = []
    for tag, x in (("longm", moon_x), ("longp", play_x), ("long", bat_x)):
        out += ["%%Vl(%s,%d,%d,%d,14,2)%%Vf(INK)%%Vb(TOP)" % (tag, MARGIN, TEXT_Y,
                                                            x - GLYPH_GAPS[1] - MARGIN),
                "%al%nx%Lt"]
    shown = "%Vd(longp)"
    show = "%%?nN<%%Vd(longm)|%%?mh<%s|%%?mp<%%Vd(long)|%s|%s|%s|%s>>>" % ((shown,) * 5)
    return "\n".join(out), "%%?Lf(title)<%%Vd(clock)%%Vd(title)|%s>" % show


def status(right=None, suffix=""):
    """The bar's glyphs, each where the mockup has it: the battery at the
    side margin, play or pause or the padlock left of it, Night Mode's moon
    left of those. From right leftwards; with suffix
    after their viewports' names for a second bar."""
    out = []
    bat_x, play_x, moon_x = glyphs_x(right)

    def vp(tag, name, x):
        w, h, _ = SIZES[name]
        return "%%Vl(%s%s,%d,%d,%d,%d,-)%%Vb(TOP)" % (tag, suffix, x, GLYPH_Y, w, h)

    play = frames("P", 4)
    out += [vp("bat", "Battery", bat_x), battery()]
    # %mp: stopped, playing, paused, forward, rewind; the padlock in their
    # place, and in their viewport: one of its own over it was cleared after
    # theirs at each whole draw and took the glyph away until its next change
    out += [vp("play", "Playing_Status", play_x),
            "%%?mh<%%xd(H)|%%?mp<|%s|%s|%s|%s>>" % tuple(play)]
    out += [vp("moon", "Night", moon_x), "%xd(N)"]
    return "\n".join(out), "%%Vd(bat%s)%%Vd(play%s)%%?nN<%%Vd(moon%s)>" % ((suffix,) * 3)


def images():
    out = []
    for name, label in (("Battery", "B"), ("Playing_Status", "P"), ("Hold", "H"),
                        ("Night", "N")):
        n = SIZES[name][2]
        # the padlock, narrower, in the middle of the playing glyphs' place
        x = (SIZES["Playing_Status"][0] - SIZES[name][0]) // 2 if name == "Hold" else 0
        out.append("%%xl(%s,%s.bmp,%d,0%s)" % (label, name.replace("_", " "), x,
                                               ",%d" % n if n > 1 else ""))
    return "\n".join(out)


def fill(text, **parts):
    for key, value in parts.items():
        text = text.replace("{" + key + "}", str(value))
    return text


SBS = """\
# Modern: the status bar and the main menu's pane. Made by themes/tools/make_modern_theme.py
%wd
%Fl(2,14-Inter-SemiBold.fnt)
# an album's name over its songs is in this one too (apps/gui/ui_theme.c)
%Fl(3,18-Inter-SemiBold.fnt)
%Fl(4,14-Inter-Medium.fnt)
%Fl(5,17-Inter-SemiBold.fnt)
{images}
# beside the pane the bar is as wide as the menu: the clock and the glyphs,
# no title; the pane has the screen's whole height
%?LW<%VI(wide)%Vd(bar)%Vd(rule){title}{show}|%VI(half)%Vd(barh)%Vd(ruleh)%Vd(clock){showh}>
# the pane by the row selected: Settings, Photos, or the music, which Extras has too
%?LW<|%?LS<%Vd(pane)%Vd(ring)%Vd(about)|{music}|%Vd(pane)%?nP<%Vd(collage)%Vd(photocount)|%Vd(nophotos)%Vd(photocaption)>|{music}>>
# the cover fills the pane, nothing over it: Finder's 240 px frame, its middle
# {pane_w} columns. It is drawn from {cover_cut} columns before the pane, which are the
# menu's and its bar's, drawn after it
%Cl(0,0,240,240,c,c)
%Vl(a,{cover_x},{cover_y},{cover_w},{cover_h},-)
%Cd
%Vl(a,{cover_x},{cover_y},{cover_cut},{cover_h},-)%Vb(GROUND)
# the lists: under the status bar, rows of 30; beside the pane {split} wide
%Vi(wide,0,{list_top},320,{list_rows},1)%Vf(INK)%Vb(GROUND)
%Vi(half,0,{list_top},{split},{list_rows},1)%Vf(INK)%Vb(GROUND)
# the status bar across every screen: its ground, the hairline under it, the
# clock, the title in the middle, the glyphs
%Vl(bar,0,0,320,{bar_rows},-)%Vb(TOP)
%Vl(rule,0,{bar_rows},320,1,-)%Vb(RULE)
%Vl(barh,0,0,{split},{bar_rows},-)%Vb(TOP)
%Vl(ruleh,0,{bar_rows},{split},1,-)%Vb(RULE)
%Vl(clock,{margin},{text_y},56,14,2)%Vf(INK)%Vb(TOP)
%al%?cf<%cH:%cM|%cl:%cM>
# the name given in Finder in the main menu; a menu's title, which takes
# the clock's place too if it is wider (long_title())
%Vl(title,72,{text_y},168,14,2)%Vf(INK)%Vb(TOP)
%ac%?if(%Li, =, 33)<%nn|%Lt>
{long_vps}
{vps}
{vpsh}
# the pane without a cover: a surface with a hairline at its left, and on it by the row
%Vl(pane,{split},0,{pane_w},240,-)%Vb(PANE)
%Vl(pane,{split},0,1,240,-)%Vb(COLUMN)
# each picture and its caption are one block in the middle of the pane's
# height, the caption's capitals {pane_gap} rows under the picture
# Settings: the disk's use as a ring, the used part in the accent on a track
# of the theme's (apps/gui/ui_theme.c); what is free, and the whole
%Vl(ring,{ring_x},{ring_y},80,80,-)%Vf(ACCENT)%Vb(PANE)
%nR
%Vl(about,{caption_x},{free_y},{caption_w},18,3)%Vf(INK)%Vb(PANE)
%s%ac%nf
%Vl(about,{caption_x},{total_y},{caption_w},14,4)%Vf(SMALL)%Vb(PANE)
%ac%nT
# the collage leaves out what is round and between its photos: the pane shows there
%Vl(collage,{collage_x},{collage_y},160,176,-)%Vb(PANE)
%xl(Q,/.rockbox/photos_panel.bmp,0,0)
%xd(Q)
%Vl(photocount,{caption_x},{count_y},{caption_w},14,4)%Vf(SMALL)%Vb(PANE)
%s%ac%nP
%xl(I,Photos Pane.bmp,0,0)
%Vl(nophotos,{photos_x},{photos_y},{photos_w},{photos_h},-)%Vb(PANE)
%xd(I)
%Vl(photocaption,{caption_x},{nophotos_y},{caption_w},14,4)%Vf(SMALL)%Vb(PANE)
%ac%nP
# nothing playing, or a track without a cover: the note, and under it the
# library's count or the track
%xl(O,Note Pane.bmp,0,0)
%Vl(note,{note_x},{note_y},{note_w},{note_h},-)%Vb(PANE)
%xd(O)
%Vl(stopped,{caption_x},{stopped_y},{caption_w},14,4)%Vf(SMALL)%Vb(PANE)
%ac%ns
%Vl(normal,{caption_x},{normal_y},{caption_w},17,5)%Vf(INK)%Vb(PANE)
%s%ac%?it<%it|%?fn<%fn|>>
"""

# The iPod photo's: the pane beside the main menu alone, the music in it
# whatever row is selected; the rest, and the main menu with Cover Beside
# Menu off, take the screen's width as on a stock iPod photo
SBS_PHOTO = """\
# Modern: the status bar, and the main menu's pane or the lists of the
# screen's whole width. Made by themes/tools/make_modern_theme.py
%wd
{fonts}
{images}
# beside the pane the bar is as wide as the menu: the clock and the glyphs;
# the pane has the screen's whole height
%?LW<%VI(wide)%Vd(bar)%Vd(rule)%?if(%Li, =, 33)<%Vd(clock)|{title}>{show}|%VI(half)%Vd(barh)%Vd(ruleh)%Vd(clock){showh}>
# the pane: the song's cover, or the note and under it the library's count
# or the track
%?LW<|{music}>
# the cover fills the pane, nothing over it, as on the Video: Finder's {cover}
# px, its middle {pane_w} columns. It is drawn from {cover_cut} columns before the pane,
# which are the menu's and its bar's, drawn after it
%Cl(0,0,{cover},{cover},c,c)
%Vl(a,{cover_x},0,{cover_w},{h},-)
%Cd
%Vl(a,{cover_x},0,{cover_cut},{h},-)%Vb(GROUND)
# the lists: under the status bar, rows of 22; beside the pane {pane} wide
%Vi(wide,0,{list_top},{w},{list_rows},1)%Vf(INK)%Vb(GROUND)
%Vi(half,0,{list_top},{pane},{list_rows},1)%Vf(INK)%Vb(GROUND)
# the status bar across every screen: its ground, the hairline under it, the
# clock, the title in the middle, the glyphs; in the main menu the clock
# alone
%Vl(bar,0,0,{w},{bar_rows},-)%Vb(TOP)
%Vl(rule,0,{bar_rows},{w},1,-)%Vb(RULE)
%Vl(barh,0,0,{pane},{bar_rows},-)%Vb(TOP)
%Vl(ruleh,0,{bar_rows},{pane},1,-)%Vb(RULE)
%Vl(clock,{margin},{text_y},{clock_w},14,2)%Vf(INK)%Vb(TOP)
%al%?cf<%cH:%cM|%cl:%cM>
# a menu's title, which takes the clock's place too if it is wider (long_title())
%Vl(title,{title_x},{text_y},{title_w},14,2)%Vf(INK)%Vb(TOP)
%ac%Lt
{long_vps}
{vps}
{vpsh}
# the pane without a cover: a surface with a hairline at its left, the note
# on it and its caption, one block in the middle of the pane's height, the
# caption's capitals {pane_gap} rows under the note
%Vl(pane,{pane},0,{pane_w},{h},-)%Vb(PANE)
%Vl(pane,{pane},0,1,{h},-)%Vb(COLUMN)
%xl(O,Note Pane.bmp,0,0)
%Vl(note,{note_x},{note_y},{note_w},{note_h},-)%Vb(PANE)
%xd(O)
%Vl(stopped,{caption_x},{stopped_y},{caption_w},14,3)%Vf(SMALL)%Vb(PANE)
%ac%ns
%Vl(normal,{caption_x},{normal_y},{caption_w},14,2)%Vf(INK)%Vb(PANE)
%s%ac%?it<%it|%?fn<%fn|>>
"""

WPS = """\
# Modern: Now Playing. Made by themes/tools/make_modern_theme.py
%wd
{fonts}
{images}
# the views, cycled with the centre button:
# normal | scrubber | album art | lyrics | rating | speed
%?mw<%?mv(2.0)<%Vd(volume)|%Vd(pb)%Vd(times)%Vd(under)>|%Vd(scrub)%Vd(times)%?kk<%Vd(under)|%Vd(hints)|%Vd(hints)>|%?C<%Vd(bigart)|%Vd(bignote)>|%Vd(lyrics)%Vd(thick)%Vd(lowtimes)|%Vd(rating)|%Vd(speed)>
# The track's details beside the cover, one block in the middle of its
# height, as on a stock iPod: the title; a song's artist in the accent, album
# and place in its list, then its stars and its format as the settings say
# (Rating, Format and Bitrate); an episode's show, date, place and time left;
# a book's author, chapter, place and speed. Skins place nothing by what is
# shown, so each of these has its viewports, named by what it holds:
# s<stars><format>, e, b<speed>. The rating view leaves the small stars out,
# the speed view a book's speed: neither moves as it is set
{block}
# repeat and shuffle under the status bar's right end, repeat at the margin
%?mw<{show_modes}|{show_modes}|||{show_modes}|{show_modes}>
# the cover, {cover} px, turned with Angled Cover; without one, a note on a tile
# of its size (%nc), which the firmware draws as the cover: its reflection,
# its corners as round as the theme has them (UI_METRIC_COVER_RADIUS), turned
%?mw<%?C<%Vd(art)|%Vd(spill)%Vd(noart)>|%?C<%Vd(art)|%Vd(spill)%Vd(noart)>|%Vd(spill)|%Vd(spill)|%?C<%Vd(art)|%Vd(spill)%Vd(noart)>|%?C<%Vd(art)|%Vd(spill)%Vd(noart)>>
# the status bar, as the lists have it
%V(0,0,{w},{bar_rows},-)%Vb(TOP)
%V(0,{bar_rows},{w},1,-)%Vb(RULE)
%V({margin},{text_y},{clock_w},14,2)%Vf(INK)%Vb(TOP)
%al%?cf<%cH:%cM|%cl:%cM>
%V({title_x},{text_y},{title_w},14,2)%Vf(INK)%Vb(TOP)
%s%ac%Sx(Now Playing)
{show}
{vps}
%xl(R,Repeat.bmp,0,0,{repeat_n})
%xl(S,Shuffle.bmp,0,0,{shuffle_n})
{mode_vps}
# over a reflection, its foot square, which the reflection goes on from; with
# Angled Cover turned
%Cl(0,0,{cover},{cover},c,c)
%Cr(REFLECT)
# where it is not shown, the ground where a turned cover and its reflection
# were, before what is drawn there: 12 px past the cover's place but under the
# hairline, {reflection} rows under it.
# It must stay defined before noart, bigart, bignote and lyrics: viewports are
# drawn in the file's order, and it clears where they draw
%Vl(spill,{spill_x},{spill_y},{spill_w},{spill_h},-)
%Vl(art,{margin},{cover_top},{cover},{cover},-)
%Cd
%xl(O,Note Playing.bmp,{noart_x},{noart_y})
%Vl(noart,{margin},{cover_top},{cover},{cover},-)%Vb(TILE)
%nc%xd(O)
# five stars, the set ones over the outlines of the unset, {star_shift} px left
# of the text's edge
%xl(G,Ratings.bmp,0,0,{ratings_n})
{block_vps}
# never shown, it only gives %fB the font of the bitrate
%Vl(bitrate,0,0,1,1,5)
# the bar: a capsule from x {margin} in a box as high as the large knob (the
# pictures are make_modern_glyphs.py's); at rest its knob is a dot as thick as
# the bar, the fill's end
%xl(K,Bar Track.bmp,0,0)
%xl(k,Knob Small.bmp,0,0)
%xl(n,Knob.bmp,0,0)
%Vl(pb,0,{bar_y},{w},{bar_h},-)
%pb({bar},slider,k,slidercentre)
# the scrubber: the same with the large knob
%Vl(scrub,0,{bar_y},{w},{bar_h},-)
%pb({bar},slider,n,slidercentre)
# the times under its ends, and between them the chapter of a track that has
# them, cut with an ellipsis and never scrolled
%Vl(times,{margin},{times_y},{time_w},14,5)%Vf(SMALL)
%al%pc
%Vl(times,{times_right},{times_y},{time_w},14,5)%Vf(SMALL)
%ar−%pr
%Vl(under,{under_x},{times_y},{under_w},14,5)%Vf(SMALL)
%nx%ac%?kn<%?kk<%kn%?kc< · %kc|>|%kn%?kc< · %kc|>|%kn %Sx(of) %kt>|>
# a podcast's or a book's scrubber: what Previous and Next do there
%Vl(hints,{under_x},{times_y},{under_w},14,5)%Vf(SMALL)
%ac−15 / +30
# the volume in the bar's place as the wheel turns; its speaker and its figure
# in the times' place
%xl(v,Volume Left.bmp,0,0)
%Vl(volume,0,{bar_y},{w},{bar_h},-)
%pv({bar})
%Vl(volume,{margin},{speaker_y},{left_w},{left_h},-)
%xd(v)
%Vl(volume,{volume_x},{times_y},{volume_w},14,5)%Vf(SMALL)
%al%nv
# rating: five large stars in the bar's place, under the reflection; the
# wheel sets them
%xl(T,Stars.bmp,0,0,{stars_n})
%Vl(rating,{stars_x},{rating_y},{stars_w},{stars_h},-)
{big_stars}
# the large cover, and the note in its place
%Vl(bigart,{big_x},{big_y},{big},{big},-)
%Cl(0,0,{big},{big},c,c)
%Cd
%xl(L,Note Large.bmp,{large_x},{large_y})
%Vl(bignote,{big_x},{big_y},{big},{big},-)%Vb(TILE)
%nc%xd(L)
# the lyrics in the lists' font over a bar without a knob and its times; and
# the speeds, under the reflection: the WPS code draws into these
%Vl(lyrics,{margin},{lyrics_y},{grid},{lyrics_h},1)%Vf(INK)
%xl(M,Thick Bar Track.bmp,0,0)
%Vl(thick,0,{thick_y},{w},{thick_h},-)
%pb({thick})
%Vl(lowtimes,{margin},{lowtimes_y},{lowtime_w},14,5)%Vf(SMALL)
%al%pc
%Vl(lowtimes,{lowtimes_right},{lowtimes_y},{lowtime_w},14,5)%Vf(SMALL)
%ar−%pr
%Vl(speed,0,{speed_y},{w},{speed_h},5)%Vf(SMALL)
"""


def fonts(names):
    """the skin's fonts by their numbers, from 2"""
    return "\n".join("%%Fl(%d,%s.fnt)" % (i + 2, name) for i, name in enumerate(names))


def sbs():
    vps, show = status()
    long_vps, title = long_title()
    if SPLIT is None:
        # the menu's bar: its glyphs end 8 px before the pane, as on the Video
        vpsh, showh = status(PANE - 8, "h")
        music = ("%?C<%Vd(a)|%Vd(pane)%Vd(note)%?mp<%Vd(stopped)|%Vd(normal)|%Vd(normal)"
                 "|%Vd(normal)|%Vd(normal)|%Vd(normal)|%Vd(normal)|%Vd(stopped)"
                 "|%Vd(stopped)>>")
        note = SIZES["Note_Pane"]
        pane_w = W - PANE
        cut = (PANE_COVER - pane_w) // 2
        # the note and its caption, from its capitals to the row they stand
        # on, one block in the middle of the screen's height; a title in
        # 14-Inter-SemiBold stands as the count in 14-Inter-Medium
        note_y = (H - (note[1] + PANE_GAP + CELL_14[1] - CELL_14[0] + 1)) // 2
        caption_y = note_y + note[1] + PANE_GAP - CELL_14[0]
        return fill(SBS_PHOTO, fonts=fonts(SBS_FONTS), images=images(), show=show, vps=vps,
                    showh=showh, vpsh=vpsh, title=title, long_vps=long_vps, music=music,
                    margin=MARGIN, text_y=TEXT_Y, bar_rows=STATUS - 1, list_top=LIST_TOP,
                    list_rows=LIST_ROWS, w=W, h=H, clock_w=CLOCK_W, title_x=TITLE_X,
                    title_w=TITLE_W, pane=PANE, pane_w=pane_w, cover=PANE_COVER,
                    cover_x=PANE - cut, cover_w=W - (PANE - cut), cover_cut=cut,
                    pane_gap=PANE_GAP, note_x=PANE + (pane_w - note[0]) // 2,
                    note_y=note_y, note_w=note[0], note_h=note[1],
                    caption_x=PANE + 6, caption_w=pane_w - 12,
                    stopped_y=caption_y, normal_y=caption_y)
    # the menu's bar: its glyphs end 8 px before the pane
    vpsh, showh = status(SPLIT - 8, "h")
    music = ("%?C<%Vd(a)|%Vd(pane)%Vd(note)%?mp<%Vd(stopped)|%Vd(normal)|%Vd(normal)"
             "|%Vd(normal)|%Vd(normal)|%Vd(normal)|%Vd(normal)|%Vd(stopped)|%Vd(stopped)>>")
    note, photos = SIZES["Note_Pane"], SIZES["Photos_Pane"]
    pane_w = 320 - SPLIT
    centre = SPLIT + pane_w // 2
    # the frame's columns left of its middle's, under the menu
    cut = (240 - pane_w) // 2

    def block(picture, first, last=None, pitch=0):
        """Where a picture so high and its caption go: the picture's top row
        and the first caption line's cell's, the block in the middle of the
        screen's height. The caption is from its first line's capitals to
        the row its last line stands on, that one's cell pitch under the
        first's."""
        last = last or first
        caption = pitch + last[1] - first[0] + 1
        top = (240 - (picture + PANE_GAP + caption)) // 2
        return top, top + picture + PANE_GAP - first[0]

    # the ring is drawn a row inside its box of 80
    ring_y, free_y = block(78, CELL_18_SEMIBOLD, CELL_14, 21)
    collage_y, count_y = block(176, CELL_14)
    photos_y, nophotos_y = block(photos[1], CELL_14)
    # the note stays where it is as a track starts: placed by the count
    note_y, stopped_y = block(note[1], CELL_14)
    normal_y = note_y + note[1] + PANE_GAP - CELL_17_SEMIBOLD[0]
    return fill(SBS, images=images(), show=show, vps=vps, showh=showh, vpsh=vpsh,
                title=title, long_vps=long_vps, music=music, margin=MARGIN,
                text_y=TEXT_Y, status=STATUS, bar_rows=STATUS - 1, list_top=LIST_TOP,
                list_rows=LIST_ROWS, split=SPLIT, pane_w=pane_w, pane_h=240 - STATUS,
                cover_x=SPLIT - cut, cover_y=0, cover_w=pane_w + cut,
                cover_h=240, cover_cut=cut,
                ring_x=centre - 40, caption_x=SPLIT + 8, caption_w=pane_w - 16,
                collage_x=centre - 80,
                pane_gap=PANE_GAP, ring_y=ring_y - 1, free_y=free_y, total_y=free_y + 21,
                collage_y=collage_y, count_y=count_y,
                photos_x=centre - photos[0] // 2, photos_y=photos_y, nophotos_y=nophotos_y,
                photos_w=photos[0], photos_h=photos[1],
                note_x=centre - note[0] // 2, note_y=note_y,
                stopped_y=stopped_y, normal_y=normal_y,
                note_w=note[0], note_h=note[1])


def detail_block(parts):
    """parts: (what, rows, clear rows over it); each one's top row, the block
    in the middle of the cover's height"""
    y = COVER_TOP + COVER // 2 - sum(rows + gap for _, rows, gap in parts) // 2
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


def wps():
    vps, show = status()
    ratings, stars = SIZES["Ratings"], SIZES["Stars"]
    left = SIZES["Volume_Left"]
    note, large = SIZES["Note_Playing"], SIZES["Note_Large"]
    bar_w, bar_h, _ = SIZES["Bar"]
    thick_w, thick_h, _ = SIZES["Thick_Bar"]
    if W != 320:
        # the bars make_modern_glyphs.py made for this screen
        bar_w = thick_w = W - 2 * MARGIN
        for name in ("Bar", "Thick Bar"):
            with open(os.path.join(OUT, "Modern", name + " Track.bmp"), "rb") as f:
                assert struct.unpack_from("<i", f.read(26), 18)[0] == bar_w, name
    repeat, shuffle = SIZES["Repeat"], SIZES["Shuffle"]
    r = frames("G", ratings[2])
    t = frames("T", stars[2])
    col_w = W - MARGIN - COL

    # each line of the block: its viewport's font and colour, and its text
    line = {
        "title": (3, "INK", "%s%al%?it<%it|%fn>"),
        "artist": (4, "ACCENTTEXT", "%s%al%?ia<%ia|%?iA<%iA|%Sx(Unknown Artist)>>"),
        "album": (5, "SMALL", "%s%al%?id<%id|%Sx(Unknown Album)>"),
        "show": (4, "ACCENTTEXT", "%s%al%?id<%id|%Sx(Unknown Album)>"),
        "date": (5, "SMALL", "%al%kd"),
        "author": (4, "ACCENTTEXT", "%s%al%?ia<%ia|%?iA<%iA|%Sx(Unknown Author)>>"),
        "chapter": (5, "SMALL", "%s%al%?kc<%kc|%id>"),
        "count": (5, "SMALL", "%al%pp %Sx(of) %pe"),
        "format": (5, "SMALL", "%fB"),
        "left": (5, "SMALL", LEFT),
        "speed": (5, "SMALL", "%al%?ks<%ks|>"),
    }

    def details(first, second):
        return [("title", TITLE_ROWS, 0), (first, ARTIST_ROWS, LINE_GAP),
                (second, SMALL_ROWS, LINE_GAP), ("count", SMALL_ROWS, COUNT_GAP)]

    variants = {}
    for stars_on in (0, 1):
        for format_on in (0, 1):
            parts = details("artist", "album")
            if stars_on:
                parts.append(("stars", ratings[1], STARS_GAP))
            if format_on:
                parts.append(("format", SMALL_ROWS,
                              FORMAT_UNDER_STARS if stars_on else FORMAT_GAP))
            variants["s%d%d" % (stars_on, format_on)] = parts
    variants["e"] = details("show", "date") + [("left", SMALL_ROWS, FORMAT_GAP)]
    for speed_on in (0, 1):
        variants["b%d" % speed_on] = details("author", "chapter") + (
            [("speed", SMALL_ROWS, FORMAT_GAP)] if speed_on else [])
    block_vps = []
    for label, parts in variants.items():
        for what, y, rows in detail_block(parts):
            if what == "stars":
                block_vps += ["%%Vl(%s,%d,%d,%d,%d,-)" % (label, COL - STAR_SHIFT, y,
                                                          ratings[0], rows),
                              "%%?rr<%s>" % "|".join(r)]
            else:
                font, colour, text = line[what]
                block_vps += ["%%Vl(%s,%d,%d,%d,%d,%d)%%Vf(%s)" % (
                    label, COL, y, col_w, rows, font, colour), text]

    def choose(view):
        """the block's viewports by the track and the settings, in a view"""
        def stars(format_on):
            on, off = ("%%Vd(s%d%d)" % (s, format_on) for s in (1, 0))
            if view == "rating":
                return off
            # Rating: Always, Only Rated (%rr: none, then by half stars), Never
            return "%%?St(now playing rating)<%s|%%?rr<%s|%s>|%s>" % (on, off, on, off)
        song = "%%?St(now playing format)<%s|%s>" % (stars(1), stars(0))
        book = "%Vd(b0)" if view == "speed" else "%?ks<%Vd(b1)|%Vd(b0)>"
        # a song has no speed view
        return "%%?kk<%s|%%Vd(e)|%s>" % ("" if view == "speed" else song, book)
    # one choice for the normal view and the scrubber (%mw: 1, 2), as a skin
    # keeps each written: the tree is in memory while the theme is
    block = "%%?if(%%mw, <, 3)<%s|%%?mw<||||%s|%s>>" % (choose("normal"), choose("rating"),
                                                       choose("speed"))

    # repeat at the margin, shuffle left of it, or in its place with repeat off
    # (%mm: off, all, one, shuffle, A-B)
    rep = frames("R", repeat[2])
    shuf = "%?ps<%?pa<%xd(Sb)|%xd(Sa)>|>"
    mode1_x = W - MARGIN - repeat[0]
    mode2_x = mode1_x - 7 - shuffle[0]
    mode_vps = "\n".join([
        "%%Vl(mode1,%d,%d,%d,%d,-)" % (mode1_x, MODES_Y, repeat[0], repeat[1]),
        "%%?mm<%s|%s>" % (shuf, "|".join(rep)),
        "%%Vl(mode2,%d,%d,%d,%d,-)" % (mode2_x, MODES_Y, shuffle[0], shuffle[1]),
        "%%?mm<|%s>" % "|".join([shuf] * 4)])

    grid = W - 2 * MARGIN
    reflection_foot = COVER_TOP + COVER + REFLECTION
    # as far round the cover's place as a turned one goes (ART_SPILL in
    # skin_display.c), under the status bar
    spill_x, spill_y = max(MARGIN - 12, 0), max(COVER_TOP - 12, STATUS)
    speed_y = reflection_foot
    return fill(WPS, fonts=fonts(WPS_FONTS), w=W, cover=COVER, clock_w=CLOCK_W,
                title_x=TITLE_X, title_w=TITLE_W, time_w=TIME_W, volume_x=VOLUME_X,
                volume_w=VOLUME_W, big=BIG, big_x=BIG_X, big_y=BIG_Y, grid=grid,
                lyrics_y=LYRICS_Y, lyrics_h=LYRICS_H, thick_y=THICK_Y,
                lowtime_w=LOWTIME_W, lowtimes_right=W - MARGIN - LOWTIME_W,
                images=images(), show=show, vps=vps, margin=MARGIN,
                text_y=TEXT_Y, bar_rows=STATUS - 1, block=block,
                block_vps="\n".join(block_vps), show_modes="%Vd(mode1)%Vd(mode2)",
                mode_vps=mode_vps, repeat_n=repeat[2], shuffle_n=shuffle[2],
                cover_top=COVER_TOP, star_shift=STAR_SHIFT,
                reflection=REFLECTION,
                spill_x=spill_x, spill_y=spill_y, spill_w=MARGIN + COVER + 12 - spill_x,
                spill_h=reflection_foot - spill_y,
                noart_x=(COVER - note[0]) // 2, noart_y=(COVER - note[1]) // 2,
                ratings_n=ratings[2],
                bar="%d,0,%d,%d,Bar Fill.bmp,backdrop,K" % (MARGIN, bar_w, bar_h),
                bar_y=BAR_Y, bar_h=bar_h, times_y=TIMES_Y, times_right=W - MARGIN - TIME_W,
                under_x=MARGIN + TIME_W + 4, under_w=grid - 2 * (TIME_W + 4),
                thick="%d,0,%d,%d,Thick Bar Fill.bmp,backdrop,M" % (MARGIN, thick_w, thick_h),
                thick_h=thick_h, lowtimes_y=THICK_Y + thick_h + 2,
                left_w=left[0], left_h=left[1], speaker_y=TIMES_Y + 2,
                stars_n=stars[2], stars_x=(W - stars[0]) // 2, stars_w=stars[0],
                stars_h=stars[1], rating_y=reflection_foot,
                speed_y=speed_y, speed_h=H - speed_y,
                # the rating view has whole stars
                big_stars="%%?rr<%s>" % "|".join([t[0]] + [t[(i + 1) // 2]
                                                            for i in range(1, 11)]),
                large_x=(BIG - large[0]) // 2, large_y=(BIG - large[1]) // 2)


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
