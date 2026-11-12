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
# its last row a hairline; the lists under it, rows of 26; the main menu 144
# wide beside a pane of 176 by 216, which the cover fills.

import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "Modern", ".rockbox", "wps")

COLOURS = {
    "Modern":      dict(GROUND="FFFFFF", INK="000000", SMALL="525152",
                        MUTED="73757B", ACCENT="FF2D52", ACCENTTEXT="DE244A",
                        TOP="F7F7F7", RULE="E6E7EF", PANE="EFF3F7",
                        TILE="EFF3F7",
                        COLUMN="E6E7EF"),
    "Modern-Dark": dict(GROUND="000000", INK="FFFFFF", SMALL="B4B6BD",
                        MUTED="8C8E94", ACCENT="FF345A", ACCENTTEXT="FF345A",
                        TOP="101010", RULE="202429", PANE="000000",
                        # a missing cover's tile, as a row's (UI_COLOUR_COVER_TILE):
                        # on the pane's black the note had no tile
                        TILE="181C20",
                        COLUMN="181818"),
}

SIZES = {}
for line in open(os.path.join(HERE, "modern_glyph_sizes.txt")):
    if not line.startswith("#"):
        name, w, h, count = line.split()
        SIZES[name] = (int(w), int(h), int(count))

STATUS = 24             # the status bar's rows, the last its hairline
MARGIN = 12             # the sides'
# to the screen's foot: eight rows of 26, or six of the 35 a row of two lines
# has, which 208 px cut to five
LIST_TOP, LIST_ROWS = 28, 212
SPLIT = 144             # the main menu's width; the pane is the rest
TEXT_Y = 5              # the status bar's text, a cell of 14 with its baseline on row 16
GLYPH_Y = 7             # its glyphs' top
# the pane's pictures and their captions: clear rows from a picture to its
# caption's capitals; and in a font's cell, the row its capitals start on and
# the one its letters stand on
PANE_GAP = 8
CELL_14 = (3, 10)             # 14-Inter-Medium
CELL_17_SEMIBOLD = (4, 12)
CELL_18_SEMIBOLD = (3, 13)
# Now Playing: the cover, and the column beside it
COVER_TOP, COVER = 36, 140
TITLE_INSET = 13        # from the cover's top row to the title's capitals; the same under the format's line
STAR_GAP = 7            # clear rows between the small stars and the format's glyph
STAR_SHIFT = 1          # the stars' row left of the text's edge


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
%Fl(3,18-Inter-SemiBold.fnt)
%Fl(4,14-Inter-Medium.fnt)
%Fl(5,17-Inter-SemiBold.fnt)
# an album's name over its songs is in this one (apps/gui/ui_theme.c)
%Fl(6,20-Inter-SemiBold.fnt)
{images}
# beside the pane the bar is as wide as the menu: the clock and the glyphs,
# no title; the pane has the screen's whole height
%?LW<%VI(wide)%Vd(bar)%Vd(rule)%Vd(clock)%Vd(title){show}|%VI(half)%Vd(barh)%Vd(ruleh)%Vd(clock){showh}>
# the pane by the row selected: Settings, Photos, or the music, which Extras has too
%?LW<|%?LS<%Vd(pane)%Vd(ring)%Vd(about)|{music}|%Vd(pane)%?nP<%Vd(collage)%Vd(photocount)|%Vd(nophotos)%Vd(photocaption)>|{music}>>
# the cover fills the pane, nothing over it: Finder's 240 px frame, its middle
# 176 columns. It is drawn from 32 columns before the pane, which are the
# menu's and its bar's, drawn after it
%Cl(0,0,240,240,c,c)
%Vl(a,{cover_x},{cover_y},{cover_w},{cover_h},-)
%Cd
%Vl(a,{cover_x},{cover_y},{cover_cut},{cover_h},-)%Vb(GROUND)
# the lists: under the status bar, rows of 26; beside the pane 144 wide
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
%Fl(3,20-Inter-SemiBold.fnt)
%Fl(4,17-Inter-Medium.fnt)
%Fl(5,14-Inter-Medium.fnt)
{images}
# the views, cycled with the centre button:
# normal | scrubber | album art | lyrics | rating | speed
%?mw<%Vd(info)%Vd(modes)%?mv(2.0)<%Vd(volume)|%Vd(pb)%Vd(times)%Vd(under)>|%Vd(info)%Vd(modes)%Vd(scrub)%Vd(times)%?kk<%Vd(under)|%Vd(hints)|%Vd(hints)>|%?C<%Vd(bigart)|%Vd(bignote)>|%Vd(lyrics)%Vd(thick)%Vd(lowtimes)|%Vd(info)%Vd(rating)|%Vd(info)%Vd(speed)>
# under the title by the track's kind: a song's artist, album and stars; an
# episode's show, date and what is left of it; a book's author and chapter
%?mw<%?kk<%Vd(song)|%Vd(episode)|%Vd(book)>|%?kk<%Vd(song)|%Vd(episode)|%Vd(book)>|||%?kk<%Vd(song)|%Vd(episode)|%Vd(book)>|%?kk<%Vd(song)|%Vd(episode)|%Vd(book)>>
%?mw<%?kk<%Vd(stars)||>|%?kk<%Vd(stars)||>||||>
# the cover, 140 px, its corners rounded by the theme; without one, a note on a tile
%?mw<%?C<%Vd(art)|%Vd(noart)>|%?C<%Vd(art)|%Vd(noart)>|||%?C<%Vd(art)|%Vd(noart)>|%?C<%Vd(art)|%Vd(noart)>>
# the status bar, as the lists have it
%V(0,0,320,{bar_rows},-)%Vb(TOP)
%V(0,{bar_rows},320,1,-)%Vb(RULE)
%V({margin},{text_y},56,14,2)%Vf(INK)%Vb(TOP)
%al%?cf<%cH:%cM|%cl:%cM>
%V(72,{text_y},168,14,2)%Vf(INK)%Vb(TOP)
%s%ac%Sx(Now Playing)
{show}
{vps}
%Cl(0,0,140,140,c,c)
%Vl(art,{margin},36,140,140,-)
%Cd
%xl(O,Note Playing.bmp,{noart_x},{noart_y})
%Vl(noart,{margin},36,140,140,-)%Vb(TILE)
%nc%xd(O)
# the lines beside it, one block from the cover's top, by the rows their
# letters stand on: the title's on 60, its capitals' tops {inset} rows under the
# cover's top; the artist in the accent a size under it on 76; the album on 92
%Vl(info,168,45,140,20,3)%Vf(INK)
%s%al%?it<%it|%fn>
%Vl(song,168,64,140,17,4)%Vf(ACCENTTEXT)
%s%al%?ia<%ia|%?iA<%iA|%Sx(Unknown Artist)>>
%Vl(song,168,82,140,14,5)%Vf(SMALL)
%s%al%?id<%id|%Sx(Unknown Album)>
%Vl(episode,168,64,140,17,4)%Vf(ACCENTTEXT)
%s%al%?id<%id|%Sx(Unknown Album)>
%Vl(episode,168,82,140,14,5)%Vf(SMALL)
%al%kd
%Vl(episode,168,{foot_y},140,14,5)%Vf(SMALL)
%al%kl%?ks< · %ks|>
%Vl(book,168,64,140,17,4)%Vf(ACCENTTEXT)
%s%al%?ia<%ia|%?iA<%iA|%Sx(Unknown Author)>>
%Vl(book,168,82,140,14,5)%Vf(SMALL)
%s%al%?kc<%kc|%id>
%Vl(book,168,{foot_y},140,14,5)%Vf(SMALL)
%al%?ks<%ks|>
# a second group at the cover's foot: the format and the bitrate, the row
# their letters stand on as far over the cover's foot as the title's capitals
# are under its top, and the stars {star_gap} clear rows over the format's glyph,
# {star_shift} px left of the text's edge; what is left of an episode, or a
# book's speed, in the format's place
%xl(G,Ratings.bmp,0,0,{ratings_n})
%Vl(stars,{ratings_x},{ratings_y},{ratings_w},{ratings_h},-)
{small_stars}
%Vl(song,168,{foot_y},140,14,5)%Vf(SMALL)
%fB
%Vl(bitrate,0,0,1,1,5)
# over the bar's right end: repeat, and shuffle right of it
%xl(R,Repeat.bmp,0,0,{repeat_n})
%xl(S,Shuffle.bmp,0,0,{shuffle_n})
%Vl(modes,{shuffle_x},178,{shuffle_w},{shuffle_h},-)
%?ps<%?pa<%xd(Sb)|%xd(Sa)>|{repeat}>
%Vl(modes,{repeat_x},178,{repeat_w},{repeat_h},-)
%?ps<{repeat}|>
# the bar: 5 px from x 12 at y 199, in a box as high as the large knob; at
# rest its knob is a dot as thick as the bar, the fill's end
%xl(K,Bar Track.bmp,0,0)
%xl(k,Knob Small.bmp,0,0)
%xl(n,Knob.bmp,0,0)
%Vl(pb,0,196,320,{bar_h},-)
%pb({bar},slider,k,slidercentre)
# the scrubber: the same with the large knob
%Vl(scrub,0,196,320,{bar_h},-)
%pb({bar},slider,n,slidercentre)
# the times under its ends
%Vl(times,{margin},207,100,14,5)%Vf(SMALL)
%al%pc
%Vl(times,208,207,100,14,5)%Vf(SMALL)
%ar−%pr
# under them: the song's place in its list, or the chapter it is in
%Vl(under,{margin},221,296,14,5)%Vf(SMALL)
%s%ac%?kn<%?kk<%kn%?kc< · %kc|>|%kn%?kc< · %kc|>|%kn %Sx(of) %kt>|%pp %Sx(of) %pe>
# a podcast's or a book's scrubber: what Previous and Next do there
%Vl(hints,{margin},221,40,14,5)%Vf(SMALL)
%al−15
%Vl(hints,268,221,40,14,5)%Vf(SMALL)
%ar+30
%Vl(hints,56,221,208,14,5)%Vf(SMALL)
%s%ac%?kn<%?kk<|%kn%?kc< · %kc|>|%kn %Sx(of) %kt>|>
# the volume in the bar's place as the wheel turns; its speaker and its figure
# under the times, where the song's place was: clear of the cover, as the bar
%xl(v,Volume Left.bmp,0,0)
%Vl(volume,0,196,320,{bar_h},-)
%pv({bar})
%Vl(volume,{margin},223,{left_w},{left_h},-)
%xd(v)
%Vl(volume,30,221,180,14,5)%Vf(SMALL)
%al%nv
%Vl(volume,{margin},207,100,14,5)%Vf(SMALL)
%al%pc
%Vl(volume,208,207,100,14,5)%Vf(SMALL)
%ar−%pr
# rating: five large stars in the bar's place, their middle on its line, 14 px
# under the cover; the wheel sets them. The song's place under them as under the bar
%xl(T,Stars.bmp,0,0,{stars_n})
%Vl(rating,{stars_x},190,{stars_w},{stars_h},-)
{big_stars}
%Vl(rating,{margin},221,296,14,5)%Vf(SMALL)
%ac%pp %Sx(of) %pe
# the large cover, and the note in its place
%Vl(bigart,60,32,200,200,-)
%Cl(0,0,200,200,c,c)
%Cd
%xl(L,Note Large.bmp,{large_x},{large_y})
%Vl(bignote,60,32,200,200,-)%Vb(TILE)
%nc%xd(L)
# the lyrics in the lists' font over a thicker bar and its times; and the
# speeds: the WPS code draws into these
%Vl(lyrics,{margin},30,296,180,1)%Vf(INK)
%xl(M,Thick Bar Track.bmp,0,0)
%Vl(thick,0,211,320,{thick_h},-)
%pb({thick})
%Vl(lowtimes,{margin},220,100,14,5)%Vf(SMALL)
%al%pc
%Vl(lowtimes,208,220,100,14,5)%Vf(SMALL)
%ar−%pr
%Vl(speed,0,188,320,44,5)%Vf(SMALL)
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
                cover_x=SPLIT - 32, cover_y=0, cover_w=pane_w + 32,
                cover_h=240, cover_cut=32,
                ring_x=centre - 40, caption_x=SPLIT + 8, caption_w=pane_w - 16,
                collage_x=centre - 80,
                pane_gap=PANE_GAP, ring_y=ring_y - 1, free_y=free_y, total_y=free_y + 21,
                collage_y=collage_y, count_y=count_y,
                photos_x=centre - photos[0] // 2, photos_y=photos_y, nophotos_y=nophotos_y,
                photos_w=photos[0], photos_h=photos[1],
                note_x=centre - note[0] // 2, note_y=note_y,
                stopped_y=stopped_y, normal_y=normal_y,
                note_w=note[0], note_h=note[1])


def wps():
    vps, show = status()
    ratings, stars = SIZES["Ratings"], SIZES["Stars"]
    left = SIZES["Volume_Left"]
    note, large = SIZES["Note_Playing"], SIZES["Note_Large"]
    bar_w, bar_h, _ = SIZES["Bar"]
    thick_w, thick_h, _ = SIZES["Thick_Bar"]
    repeat, shuffle = SIZES["Repeat"], SIZES["Shuffle"]
    r = frames("G", ratings[2])
    # the 14 px font's letters stand on its cell's twelfth row
    foot_y = COVER_TOP + COVER - 1 - TITLE_INSET - 11
    t = frames("T", stars[2])
    rep = frames("R", repeat[2])
    shuffle_x = 320 - MARGIN - shuffle[0]
    return fill(WPS, images=images(), show=show, vps=vps, margin=MARGIN,
                text_y=TEXT_Y, bar_rows=STATUS - 1,
                noart_x=(140 - note[0]) // 2, noart_y=(140 - note[1]) // 2,
                ratings_n=ratings[2], ratings_w=ratings[0], ratings_h=ratings[1],
                inset=TITLE_INSET, foot_y=foot_y, star_gap=STAR_GAP, star_shift=STAR_SHIFT,
                ratings_x=168 - STAR_SHIFT,
                # the format's glyph starts a row under its cell's top
                ratings_y=foot_y + 1 - STAR_GAP - ratings[1],
                # %rr: none, then by half stars
                small_stars="%%?rr<%s>" % "|".join(r),
                repeat_n=repeat[2], shuffle_n=shuffle[2],
                shuffle_x=shuffle_x, shuffle_w=shuffle[0], shuffle_h=shuffle[1],
                repeat_x=shuffle_x - 7 - repeat[0], repeat_w=repeat[0], repeat_h=repeat[1],
                # %mm: off, all, one, shuffle, A-B
                repeat="%%?mm<|%s|%s|%s|%s>" % tuple(rep),
                bar="%d,0,%d,%d,Bar Fill.bmp,backdrop,K" % (MARGIN, bar_w, bar_h),
                bar_h=bar_h,
                thick="%d,0,%d,%d,Thick Bar Fill.bmp,backdrop,M" % (MARGIN, thick_w, thick_h),
                thick_h=thick_h, left_w=left[0], left_h=left[1],
                stars_n=stars[2], stars_x=(320 - stars[0]) // 2, stars_w=stars[0],
                stars_h=stars[1],
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
