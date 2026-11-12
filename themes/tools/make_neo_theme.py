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
# The places are the mockup's (work/mockups/ipod-neo.html, its panel):
# the status line 22 px with a hairline on row 22, its glyphs from x 304
# leftwards 6 px apart, their feet on row 15; lists from y 34; the main
# menu's pane right of a hairline on column 159.

import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "Neo", ".rockbox", "wps")

COLOURS = {
    "Neo":      dict(PAPER="EFEBE7", PRIMARY="292421", SECONDARY="6B6963",
                     TERTIARY="A59E94", TRACK="E7DFD6", SELECTED="DEDBD6",
                     SURFACE="E7E3DE", PANETEXT="635D5A"),
    "Neo-Dark": dict(PAPER="211C18", PRIMARY="EFEBE7", SECONDARY="A59E94",
                     TERTIARY="6B6963", TRACK="313029", SELECTED="312C29",
                     SURFACE="312C29", PANETEXT="A59E94"),
}

SIZES = {}
for line in open(os.path.join(HERE, "neo_glyph_sizes.txt")):
    if not line.startswith("#"):
        name, w, h, count = line.split()
        SIZES[name] = (int(w), int(h), int(count))

FOOT = 16   # the row under the status glyphs' feet
GAP = 6     # between them


def frames(label, count):
    return ["%%xd(%s%s)" % (label, chr(ord("a") + i)) for i in range(count)]


def battery():
    """charging in turn, charged, or its level of twenty"""
    f = frames("B", SIZES["Battery"][2])
    levels = f[3:]
    return "%%?bp<%%?bc<%%t(1.0)%s;%%t(1.0)%s|%s>|%%?bl<%s>>" % (
        f[0], f[1], f[2], "|".join([levels[0]] + levels + [levels[-1]] * 2))


def status(width, modes, prefix=""):
    """The glyphs' viewports and the line that shows them: from the right the
    battery, Night Mode's moon, play or pause or the padlock, and in Now
    Playing shuffle and repeat. What is left of an absent one moves up, so
    each has a viewport for every place it can be in."""
    out = []

    def vp(tag, name, x):
        w, h, _ = SIZES[name]
        return "%%Vl(%s%s,%d,%d,%d,%d,-)" % (prefix, tag, x, FOOT - h, w, h)

    x = width - 16 - SIZES["Battery"][0]
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
%Fl(4,15-Inter-Regular.fnt)
{images}
%?LW<%VI(wide)%Vd(rulewide)%Vd(clockwide)%Vd(titlewide){wide}|%VI(half)%Vd(rulehalf)%Vd(column)%Vd(name){half}>
# the pane by the row selected: Settings, Extras, Photos, or the music
%?LW<|%?LS<%Vd(surface)%Vd(ring)%Vd(about)|%Vd(surface)%Vd(dial)%Vd(extras)|%Vd(surface)%?nP<%Vd(collage)%Vd(photocount)|%Vd(nophotos)%Vd(photocaption)>|%?C<%Vd(a)|%Vd(surface)%Vd(note)%?mp<%Vd(stopped)|%Vd(normal)|%Vd(normal)|%Vd(normal)|%Vd(normal)|%Vd(normal)|%Vd(normal)|%Vd(stopped)|%Vd(stopped)>>>>
# the cover fills the pane, nothing over it: Finder's 240 px frame, its middle 160
%Cl(0,0,240,240,c,c)
%Vl(a,120,0,200,240,-)
%Cd
%Vl(a,120,0,40,240,-)
# the lists: under the status line's 22 px and its hairline, rows of 30; beside
# the pane 156 wide, so that the selected row ends at 150
%Vi(wide,0,34,320,194,1)%Vf(PRIMARY)%Vb(PAPER)
%Vi(half,0,34,156,194,1)%Vf(PRIMARY)%Vb(PAPER)
# hairlines: row 22 under the status line, over the menu only beside the pane,
# and column 159 between the menu and the pane
%Vl(rulewide,0,22,320,1,-)%Vb(TRACK)
%Vl(rulehalf,0,22,159,1,-)%Vb(TRACK)
%Vl(column,159,0,1,240,-)%Vb(TRACK)
# a screen that takes the whole width: the clock, its title, the glyphs
%Vl(clockwide,16,4,64,15,2)%Vf(SECONDARY)
%al%?cf<%cH:%cM|%cl:%cM>
%Vl(titlewide,84,4,152,15,3)%Vf(PRIMARY)
%s%ac%Lt
{wide_vps}
# beside the pane: the name given in Finder in the main menu, a menu's title
%Vl(name,16,4,84,15,3)%Vf(PRIMARY)
%al%?if(%Li, =, 33)<%nn|%s%Lt>
{half_vps}
# the pane without a cover: a surface, and on it by the row
%Vl(surface,160,0,160,240,-)%Vb(SURFACE)
# Settings: the disk's use as a ring round (240, 96), the used part in the
# second grey over a track of the ground's colour; what is free, and the whole
%Vl(ring,200,56,80,80,-)%Vf(SECONDARY)%Vb(SURFACE)
%nR
%Vl(about,160,157,160,19,1)%Vf(PRIMARY)%Vb(SURFACE)
%s%ac%nf
%Vl(about,160,180,160,15,4)%Vf(PANETEXT)%Vb(SURFACE)
%ac%nT
# Extras: a clock's dial round (240, 104) at the time it is, and what is there
%Vl(dial,200,64,80,80,-)%Vf(SECONDARY)%Vb(SURFACE)
%nD
%Vl(extras,160,168,160,15,4)%Vf(PANETEXT)%Vb(SURFACE)
%s%ac%Sx(Clock) · %Sx(Games) · %Sx(Calendar)
%Vl(collage,160,0,160,176,-)
%xl(Q,/.rockbox/photos_panel.bmp,0,0)
%xd(Q)
%Vl(photocount,160,200,160,15,2)%Vf(PANETEXT)%Vb(SURFACE)
%s%ac%nP
%xl(I,Photos Pane.bmp,0,0)
%Vl(nophotos,{photos_x},{photos_y},{photos_w},{photos_h},-)%Vb(SURFACE)
%xd(I)
%Vl(photocaption,160,160,160,15,2)%Vf(PANETEXT)%Vb(SURFACE)
%ac%nP
# nothing playing, or a track without a cover: the note, and under it the
# library's count or the track
%xl(O,Note Pane.bmp,0,0)
%Vl(note,{note_x},{note_y},{note_w},{note_h},-)%Vb(SURFACE)
%xd(O)
%Vl(stopped,160,160,160,15,2)%Vf(PANETEXT)%Vb(SURFACE)
%ac%ns
%Vl(normal,168,160,144,15,2)%Vf(PANETEXT)%Vb(SURFACE)
%s%ac%?it<%it|%?fn<%fn|>>
"""

WPS = """\
# Neo: Now Playing. Made by themes/tools/make_neo_theme.py
%wd
%Fl(2,22-Inter-SemiBold.fnt)
%Fl(3,17-Inter-Regular.fnt)
%Fl(4,14-Inter-Medium.fnt)
%Fl(5,15-Inter-SemiBold.fnt)
%Fl(6,15-Inter-Medium.fnt)
{images}
# the views, cycled with the centre button:
# normal | scrubber | album art | lyrics | rating | speed
%?mw<%Vd(info)%?mv(2.0)<%Vd(volume)|%Vd(pb)%Vd(under)>|%Vd(info)%Vd(scrub)%?kk<%Vd(under)|%Vd(hints)|%Vd(hints)>|%?C<%Vd(bigart)|%Vd(bignote)>|%Vd(lyrics)%Vd(pb)|%Vd(info)%Vd(rating)|%Vd(info)%Vd(speed)>
# under the title by the track's kind: a song's artist, album and stars; an
# episode's show, date and what is left of it; a book's author and chapter
%?mw<%?kk<%Vd(song)|%Vd(episode)|%Vd(book)>|%?kk<%Vd(song)|%Vd(episode)|%Vd(book)>|||%?kk<%Vd(song)|%Vd(episode)|%Vd(book)>|%?kk<%Vd(song)|%Vd(episode)|%Vd(book)>>
%?mw<%?kk<%Vd(stars)||>|%?kk<%Vd(stars)||>||||>
# the cover, 128 px, its corners rounded by the theme; without one, a note on a tile
%?mw<%?C<%Vd(art)|%Vd(noart)>|%?C<%Vd(art)|%Vd(noart)>|||%?C<%Vd(art)|%Vd(noart)>|%?C<%Vd(art)|%Vd(noart)>>
# the status line: the clock, the title, the glyphs, a hairline on row 22
%V(16,4,64,15,6)%Vf(SECONDARY)
%al%?cf<%cH:%cM|%cl:%cM>
%V(100,4,120,15,5)%Vf(PRIMARY)
%s%ac%Sx(Now Playing)
%V(0,22,320,1,-)%Vb(TRACK)
%?nN<{show}
{vps}
%Cl(0,0,128,128,c,c)
%Vl(art,16,40,128,128,-)
%Cd
%xl(O,Note Playing.bmp,{noart_x},{noart_y})
%Vl(noart,16,40,128,128,-)%Vb(SELECTED)
%xd(O)
# the three lines beside it: baselines 80, 102 and 118
%Vl(info,160,63,144,22,2)%Vf(PRIMARY)
%s%al%?it<%it|%fn>
%Vl(song,160,89,144,17,3)%Vf(SECONDARY)
%s%al%?ia<%ia|%?iA<%iA|%Sx(Unknown Artist)>>
%Vl(song,160,105,144,17,3)%Vf(SECONDARY)
%s%al%?id<%id|%Sx(Unknown Album)>
%Vl(episode,160,89,144,17,3)%Vf(SECONDARY)
%s%al%?id<%id|%Sx(Unknown Album)>
%Vl(episode,160,105,144,17,3)%Vf(SECONDARY)
%al%kd
%Vl(episode,160,131,144,14,4)%Vf(SECONDARY)
%al%kl%?ks< · %ks|>
%Vl(book,160,89,144,17,3)%Vf(SECONDARY)
%s%al%?ia<%ia|%?iA<%iA|%Sx(Unknown Author)>>
%Vl(book,160,105,144,17,3)%Vf(SECONDARY)
%s%al%?kc<%kc|%id>
%Vl(book,160,131,144,14,4)%Vf(SECONDARY)
%al%?ks<%ks|>
# five stars from y 134, the set ones and the unset in one picture
%xl(G,Ratings.bmp,0,0,{ratings_n})
%Vl(stars,160,134,{ratings_w},{ratings_h},-)
{small_stars}
# the format and the bitrate under them
%Vl(song,160,149,144,14,4)%Vf(SECONDARY)
%fB
%Vl(bitrate,0,0,1,1,4)
# the bar: a capsule from x 52 at y 198, its times level with it, each in the
# middle of the room beside it; 8 px shorter at each end for an hour or more
%xl(K,Bar Track.bmp,0,0)
%xl(k,Knob.bmp,0,0)
%Vl(pb,0,190,320,19,4)%Vf(SECONDARY)
%pb({bar},times,3,inset,8,14)
# the scrubber: the same with its knob
%Vl(scrub,0,190,320,19,4)%Vf(SECONDARY)
%pb({bar},slider,k,slidercentre,times,3,inset,8,14)
# under the bar: the song's place in its list, or the chapter it is in
%Vl(under,16,209,288,14,4)%Vf(SECONDARY)
%s%ac%?kn<%?kk<%kn%?kc< · %kc|>|%kn%?kc< · %kc|>|%kn %Sx(of) %kt>|%pp %Sx(of) %pe>
# a podcast's or a book's scrubber: what Previous and Next do there
%Vl(hints,16,209,40,14,4)%Vf(SECONDARY)
%al-15
%Vl(hints,264,209,40,14,4)%Vf(SECONDARY)
%ar+30
%Vl(hints,60,209,200,14,4)%Vf(SECONDARY)
%s%ac%?kn<%?kk<|%kn%?kc< · %kc|>|%kn %Sx(of) %kt>|>
# the volume in the bar's place, as the wheel turns
%xl(v,Volume Left.bmp,0,0)
%xl(V,Volume Right.bmp,0,0)
%Vl(volume,0,190,320,19,-)
%pv({bar})
%Vl(volume,21,{left_y},{left_w},{left_h},-)
%xd(v)
%Vl(volume,286,{right_y},{right_w},{right_h},-)
%xd(V)
%Vl(volume,16,209,288,14,4)%Vf(SECONDARY)
%ac%nv
# rating: five large stars in the bar's place, the wheel sets them
%xl(T,Stars.bmp,0,0,{stars_n})
%Vl(rating,{stars_x},188,{stars_w},{stars_h},-)
{big_stars}
%Vl(rating,16,213,288,14,4)%Vf(SECONDARY)
%ac%pp %Sx(of) %pe
# the large cover, and the note in its place
%Vl(bigart,60,30,200,200,-)
%Cl(0,0,200,200,c,c)
%Cd
%xl(L,Note Large.bmp,{large_x},{large_y})
%Vl(bignote,60,30,200,200,-)%Vb(SELECTED)
%xd(L)
# the lyrics, lines of the text font, over the bar; and the speeds: the WPS
# code draws into these
%Vl(lyrics,16,34,288,150,3)%Vf(PRIMARY)
%Vl(speed,0,184,320,44,4)%Vf(SECONDARY)
"""


def sbs():
    wide_vps, wide = status(320, False)
    half_vps, half = status(160, False, "h")
    note, photos = SIZES["Note_Pane"], SIZES["Photos_Pane"]
    return fill(SBS, images=images(False), wide=wide, half=half, wide_vps=wide_vps,
                half_vps=half_vps,
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
    r = frames("G", ratings[2])
    t = frames("T", stars[2])
    # %rr: none, then by half stars; the rating view has whole stars
    text = fill(WPS, images=images(True), vps=vps,
                noart_x=(128 - note[0]) // 2, noart_y=(128 - note[1]) // 2,
                ratings_n=ratings[2], ratings_w=ratings[0], ratings_h=ratings[1],
                small_stars="%%?rr<%s>" % "|".join(r),
                bar="52,%d,%d,%d,Bar Fill.bmp,backdrop,K" % (9 - bar_h // 2, bar_w, bar_h),
                left_y=194 + (11 - left[1]) // 2, left_w=left[0], left_h=left[1],
                right_y=194 + (11 - right[1]) // 2, right_w=right[0], right_h=right[1],
                stars_n=stars[2], stars_x=(320 - stars[0]) // 2, stars_w=stars[0],
                stars_h=stars[1],
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
