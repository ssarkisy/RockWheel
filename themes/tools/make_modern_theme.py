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

import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "Modern", ".rockbox", "wps")

# the reflection's rows and its strength at the top, of 256: the firmware's
# default %Cr in the light skin; on black a cover's dark foot was lost at
# half, so the dark skin's starts stronger (owner, 2026-11-13)
REFLECT_LIGHT, REFLECT_DARK = "22,128", "22,176"

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

STATUS = 24             # the status bar's rows, the last its hairline
MARGIN = 12             # the sides'
# to the screen's foot: seven rows of 30, or five of the 36 a row of two lines
# has and a part of the sixth
LIST_TOP, LIST_ROWS = 28, 212
SPLIT = 160             # the main menu's width; the pane is the rest
TEXT_Y = 5              # the status bar's text, a cell of 14 with its baseline on row 16
GLYPH_Y = 7             # its glyphs' top
# the pane's pictures and their captions: clear rows from a picture to its
# caption's capitals; and in a font's cell, the row its capitals start on and
# the one its letters stand on
PANE_GAP = 8
CELL_14 = (3, 10)             # 14-Inter-Medium
CELL_17_SEMIBOLD = (4, 12)
CELL_18_SEMIBOLD = (3, 13)
# Now Playing: the cover, its reflection under it (REFLECT_LIGHT/DARK's rows),
# and the column beside it
COVER_TOP, COVER = 36, 140
REFLECTION = 22
COL = 168               # the column's left edge
STAR_SHIFT = 1          # the stars' row left of the text's edge
# the song's details: one block in the middle of the cover's height, of these
# rows each line has, with these clear rows over some
TITLE_ROWS = 18         # 18-Inter-SemiBold, Neo's title
ARTIST_ROWS = 17        # 17-Inter-Medium, with a clear row over it and the next
SMALL_ROWS = 14         # 14-Inter-Medium: the album, the place in the list, the format
LINE_GAP = 1
COUNT_GAP = 4           # over the place in the list
STARS_GAP = 6           # over the stars
FORMAT_GAP = 5          # over the format, or what is left of an episode or a book
FORMAT_UNDER_STARS = 3  # over the format, under the stars
# the bar's box, as high as the large knob, 6 rows lower than in 1.4 where the
# song's place in its list was; the times under it
BAR_Y, TIMES_Y = 202, 214
# repeat and shuffle under the status bar's right end
MODES_Y = 30


def frames(label, count):
    return ["%%xd(%s%s)" % (label, chr(ord("a") + i)) for i in range(count)]


def battery():
    """charging in turn, charged, or its level of twenty"""
    f = frames("B", SIZES["Battery"][2])
    levels = f[3:]
    return "%%?bp<%%?bc<%%t(1.0)%s;%%t(1.0)%s|%s>|%%?bl<%s>>" % (
        f[0], f[1], f[2], "|".join([levels[0]] + levels + [levels[-1]] * 2))


def status(right=320 - MARGIN, suffix=""):
    """The bar's glyphs, each where the mockup has it: the battery at the
    side margin, play or pause or the padlock left of it, Night Mode's moon
    left of those. From right leftwards; with suffix
    after their viewports' names for a second bar."""
    out = []
    bat_x = right - SIZES["Battery"][0]
    play_x = bat_x - 11 - SIZES["Playing_Status"][0]
    moon_x = play_x - 6 - SIZES["Night"][0]

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
%?LW<%VI(wide)%Vd(bar)%Vd(rule)%Vd(clock)%Vd(title){show}|%VI(half)%Vd(barh)%Vd(ruleh)%Vd(clock){showh}>
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
# the name given in Finder in the main menu; a menu's title
%Vl(title,72,{text_y},168,14,2)%Vf(INK)%Vb(TOP)
%s%ac%?if(%Li, =, 33)<%nn|%Lt>
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

WPS = """\
# Modern: Now Playing. Made by themes/tools/make_modern_theme.py
%wd
%Fl(2,14-Inter-SemiBold.fnt)
%Fl(3,18-Inter-SemiBold.fnt)
%Fl(4,17-Inter-Medium.fnt)
%Fl(5,14-Inter-Medium.fnt)
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
# the cover, 140 px, turned with Angled Cover; without one, a note on a tile.
# Their corners as round as the theme has them (UI_METRIC_COVER_RADIUS, %nc)
%?mw<%?C<%Vd(art)|%Vd(spill)%Vd(noart)>|%?C<%Vd(art)|%Vd(spill)%Vd(noart)>|%Vd(spill)|%Vd(spill)|%?C<%Vd(art)|%Vd(spill)%Vd(noart)>|%?C<%Vd(art)|%Vd(spill)%Vd(noart)>>
# the status bar, as the lists have it
%V(0,0,320,{bar_rows},-)%Vb(TOP)
%V(0,{bar_rows},320,1,-)%Vb(RULE)
%V({margin},{text_y},56,14,2)%Vf(INK)%Vb(TOP)
%al%?cf<%cH:%cM|%cl:%cM>
%V(72,{text_y},168,14,2)%Vf(INK)%Vb(TOP)
%s%ac%Sx(Now Playing)
{show}
{vps}
%xl(R,Repeat.bmp,0,0,{repeat_n})
%xl(S,Shuffle.bmp,0,0,{shuffle_n})
{mode_vps}
# over a reflection, its foot square, which the reflection goes on from; with
# Angled Cover turned
%Cl(0,0,140,140,c,c)
%Cr(REFLECT)
# where it is not shown, the ground where a turned cover and its reflection
# were, before what is drawn there: 12 px past the cover's place but under the
# hairline, {reflection} rows under it.
# It must stay defined before noart, bigart, bignote and lyrics: viewports are
# drawn in the file's order, and it clears where they draw
%Vl(spill,{spill_x},{spill_y},{spill_w},{spill_h},-)
%Vl(art,{margin},{cover_top},140,140,-)
%Cd
%xl(O,Note Playing.bmp,{noart_x},{noart_y})
%Vl(noart,{margin},{cover_top},140,140,-)%Vb(TILE)
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
%Vl(pb,0,{bar_y},320,{bar_h},-)
%pb({bar},slider,k,slidercentre)
# the scrubber: the same with the large knob
%Vl(scrub,0,{bar_y},320,{bar_h},-)
%pb({bar},slider,n,slidercentre)
# the times under its ends, and between them the chapter of a track that has
# them, cut with an ellipsis and never scrolled
%Vl(times,{margin},{times_y},64,14,5)%Vf(SMALL)
%al%pc
%Vl(times,{times_right},{times_y},64,14,5)%Vf(SMALL)
%ar−%pr
%Vl(under,{under_x},{times_y},{under_w},14,5)%Vf(SMALL)
%nx%ac%?kn<%?kk<%kn%?kc< · %kc|>|%kn%?kc< · %kc|>|%kn %Sx(of) %kt>|>
# a podcast's or a book's scrubber: what Previous and Next do there
%Vl(hints,{under_x},{times_y},{under_w},14,5)%Vf(SMALL)
%ac−15 / +30
# the volume in the bar's place as the wheel turns; its speaker and its figure
# in the times' place
%xl(v,Volume Left.bmp,0,0)
%Vl(volume,0,{bar_y},320,{bar_h},-)
%pv({bar})
%Vl(volume,{margin},{speaker_y},{left_w},{left_h},-)
%xd(v)
%Vl(volume,30,{times_y},180,14,5)%Vf(SMALL)
%al%nv
# rating: five large stars in the bar's place, under the reflection; the
# wheel sets them
%xl(T,Stars.bmp,0,0,{stars_n})
%Vl(rating,{stars_x},{rating_y},{stars_w},{stars_h},-)
{big_stars}
# the large cover, and the note in its place
%Vl(bigart,60,32,200,200,-)
%Cl(0,0,200,200,c,c)
%Cd
%xl(L,Note Large.bmp,{large_x},{large_y})
%Vl(bignote,60,32,200,200,-)%Vb(TILE)
%nc%xd(L)
# the lyrics in the lists' font over a bar without a knob and its times; and
# the speeds, under the reflection: the WPS code draws into these
%Vl(lyrics,{margin},30,296,180,1)%Vf(INK)
%xl(M,Thick Bar Track.bmp,0,0)
%Vl(thick,0,211,320,{thick_h},-)
%pb({thick})
%Vl(lowtimes,{margin},{lowtimes_y},100,14,5)%Vf(SMALL)
%al%pc
%Vl(lowtimes,208,{lowtimes_y},100,14,5)%Vf(SMALL)
%ar−%pr
%Vl(speed,0,{speed_y},320,{speed_h},5)%Vf(SMALL)
"""


def sbs():
    vps, show = status()
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
                music=music, margin=MARGIN,
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
    repeat, shuffle = SIZES["Repeat"], SIZES["Shuffle"]
    r = frames("G", ratings[2])
    t = frames("T", stars[2])
    col_w = 320 - MARGIN - COL

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
    mode1_x = 320 - MARGIN - repeat[0]
    mode2_x = mode1_x - 7 - shuffle[0]
    mode_vps = "\n".join([
        "%%Vl(mode1,%d,%d,%d,%d,-)" % (mode1_x, MODES_Y, repeat[0], repeat[1]),
        "%%?mm<%s|%s>" % (shuf, "|".join(rep)),
        "%%Vl(mode2,%d,%d,%d,%d,-)" % (mode2_x, MODES_Y, shuffle[0], shuffle[1]),
        "%%?mm<|%s>" % "|".join([shuf] * 4)])

    grid = 320 - 2 * MARGIN
    reflection_foot = COVER_TOP + COVER + REFLECTION
    # as far round the cover's place as a turned one goes (ART_SPILL in
    # skin_display.c), under the status bar
    spill_y = max(COVER_TOP - 12, STATUS)
    speed_y = reflection_foot
    return fill(WPS, images=images(), show=show, vps=vps, margin=MARGIN,
                text_y=TEXT_Y, bar_rows=STATUS - 1, block=block,
                block_vps="\n".join(block_vps), show_modes="%Vd(mode1)%Vd(mode2)",
                mode_vps=mode_vps, repeat_n=repeat[2], shuffle_n=shuffle[2],
                cover_top=COVER_TOP, star_shift=STAR_SHIFT,
                reflection=REFLECTION,
                spill_x=MARGIN - 12, spill_y=spill_y, spill_w=COVER + 24,
                spill_h=reflection_foot - spill_y,
                noart_x=(140 - note[0]) // 2, noart_y=(140 - note[1]) // 2,
                ratings_n=ratings[2],
                bar="%d,0,%d,%d,Bar Fill.bmp,backdrop,K" % (MARGIN, bar_w, bar_h),
                bar_y=BAR_Y, bar_h=bar_h, times_y=TIMES_Y, times_right=320 - MARGIN - 64,
                under_x=MARGIN + 68, under_w=grid - 136,
                thick="%d,0,%d,%d,Thick Bar Fill.bmp,backdrop,M" % (MARGIN, thick_w, thick_h),
                thick_h=thick_h, lowtimes_y=211 + thick_h + 2,
                left_w=left[0], left_h=left[1], speaker_y=TIMES_Y + 2,
                stars_n=stars[2], stars_x=(320 - stars[0]) // 2, stars_w=stars[0],
                stars_h=stars[1], rating_y=reflection_foot,
                speed_y=speed_y, speed_h=240 - speed_y,
                # the rating view has whole stars
                big_stars="%%?rr<%s>" % "|".join([t[0]] + [t[(i + 1) // 2]
                                                            for i in range(1, 11)]),
                large_x=(200 - large[0]) // 2, large_y=(200 - large[1]) // 2)


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, colours in COLOURS.items():
        for ext, text in (("sbs", sbs()), ("wps", wps())):
            for role, value in colours.items():
                text = text.replace("(%s)" % role, "(%s)" % value)
            with open(os.path.join(OUT, "%s.%s" % (name, ext)), "w", encoding="utf-8") as f:
                f.write(text)


main()
