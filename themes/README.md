# iPod themes

The themes for the iPod Video (320x240), each kept here in the layout it is
installed in (`<theme>/.rockbox/...`): iClassic Square, which Settings calls
Classic, Modern and Neo, each with a dark version beside it.

iClassic Square is licensed under Creative Commons Attribution-Share Alike 3.0
(http://creativecommons.org/licenses/by-sa/3.0/), and so are the changes made
to it here. Modern and Neo were made for RockWheel.

`fonts/` holds the fonts they use: Inter (https://rsms.me/inter/), converted with
`tools/make_fonts.sh`, under the SIL Open Font License 1.1 (`fonts/OFL.txt`,
installed as `Inter-OFL.txt`). The fonts it came with had no Cyrillic. Letters
Inter doesn't have come from GNU Unifont, which Rockbox also carries in the
top-level `fonts/` (`16-GNU-Unifont.fnt`). `53-Inter-Letters.fnt` is the large
letter of Quick Scroll over a long list: Inter Bold in 52 px, only the capitals
of Latin, Greek and Cyrillic, the digits and "#".

The fonts of the menus and the text, the 24, 18, 17 and 14 px ones, have the
other alphabets as well, merged into them by `tools/make_script_fonts.sh` after
`make_fonts.sh`, each glyph on Inter's baseline:

- Hangul from Pretendard (https://github.com/orioncactus/pretendard, its Hangul
  from Source Han Sans), `Hangul-OFL.txt`: SemiBold 18, Medium 15 and SemiBold 14,
  about the size of the Inter beside them and a weight lighter;
- the ideographs, kana and full-width forms from Noto Sans SC, JP and TC (Source
  Han Sans), `Han-OFL.txt`: all of SC's, and past U+FFFF the glyphs JP and TC
  draw otherwise, which the firmware takes in a line with kana, or under menus
  in Japanese or traditional Chinese;
- Hebrew from Noto Sans Hebrew, Arabic with the forms its letters are joined in
  from Vazirmatn, and Thai from IBM Plex Sans Thai, `Hebrew-`, `Arabic-` and
  `Thai-OFL.txt`.

All are under the SIL Open Font License 1.1, and all but Pretendard from Google
Fonts. The fonts keep Inter's names, as Pretendard, Source and Plex are names
those licenses reserve. The ideographs and kana are as tall as the capitals of
the Inter beside them, the others of Inter's size; the 18 px fonts of the lyrics
and the hints, between two sizes, have the menus' Hangul and the 17s' rest.
One's own `24-`, `17-` and `14-Hangul.fnt`, made by `tools/convttf` from another
typeface, still draw Hangul in those sizes.

The fonts of Neo and Modern are made by the same two scripts, from Inter
SemiBold and Medium, at the em sizes of their mockups and named after their
line heights as the rest: `19-Inter-SemiBold` (15 px),
`17-Inter-Medium-13` (13), `15-Inter-Medium` (12) and `14-Inter-Medium` (11)
for Neo and Modern, whose titles and the values in their menus are
`18-Inter-SemiBold`; `15-Inter-SemiBold` (12) for Neo alone. iClassic Square
loads none of them. Each has the other alphabets in its
own weight and size. The ideographs and kana are 10 px at
the least and Hangul 11.67, taller than the Latin of the smallest fonts and cut
to their lines. Of the ideographs' other forms these fonts have those the 24,
17 and 14 px ones differ in. `tools/font_coverage.py` lists what each file
has, and draws a sheet of them.

- **iClassic Square** by Humberto Santana, derived from iLike by Alan Brito, itself
  ported from the 176x132 version by Hayden Pearce.
  https://themes.rockbox.org/index.php?themeid=1311&target=ipodvideo

Changes from the originals:

- The WPS follows the display mode (`%mw`) that the centre button cycles
  through: normal, scrubber, album art, lyrics, rating.
  - The scrubber and rating views replace the progress bar (`Scrub Knob.bmp`,
    `Stars.bmp`).
  - The album art view is a second `%Cl` at a larger size; `Art Mask.bmp`
    hides the parts of the backdrop next to it.
  - The lyrics view is an empty viewport labelled `lyrics`, the WPS code
    draws the lyrics into it.

- Inter instead of the Helvetica based fonts.
- `list text padding: 5` in the .cfg: list text keeps clear of the left
  edge, as on a stock iPod.
- The statusbar title is the title of the list ("iPod" in the main menu)
  rather than "Playlist" for every screen but the main menu.
- iClassic: no "iClassic v1.1" in the right panel of the menus when nothing
  is playing.
- Now Playing's cover has a reflection under it, and is angled with the
  Angled Cover setting, where the WPS asks for it with `%Cr` after its `%Cl`.
- Now Playing's details stand beside the cover as one block in the middle of
  its height, as on a stock iPod: the title, artist, album, the song's place
  in its list, then the stars and the format as Settings > Appearance says
  (Rating, Format and Bitrate). A skin places nothing by what it shows, so
  each combination has viewports of its own; `make_classic_np.py` writes
  them, between the lines of the WPS that name it.
- Shuffle and repeat stand in Now Playing's top right corner, under the status
  bar, as on a stock iPod; the status bar has neither.
- A dark version, `iClassic-Square-Dark`, which the Dark Theme setting shows.

The extra bitmaps are drawn by the scripts in `tools/`, which are run again
after a change, in this order:

- `make_assets.py`, `make_night_icon.py`,
  `make_usb_screen.py`, `make_about_panel.py`: bitmaps of their own.
- `make_menu_icons.py`: the backdrops' placeholder note and its reflection, and
  the original theme's battery out of their status bar, where the skins draw one.
- `make_classic_np.py`: the details of Now Playing, in the light theme's WPS.
- `make_dark_theme.py`: the dark theme, all of it from the light one.

Modern's and Neo's skins, light and dark, are written by `make_modern_theme.py`
and `make_neo_theme.py`, and their glyphs come from the mockups' export through
`make_modern_glyphs.py` and `make_theme_glyphs.py`. None of these skins is edited
by hand.

To try them in the simulator, copy them over the simulator disk:

    rsync -a themes/*/.rockbox/ build-sim/simdisk/.rockbox/

The ring on the boot screen and on the panel beside the main menu is drawn by
`tools/make_wheel_mark.py`, and the panel made from it by `tools/make_about_panel.py`.
They are this project's own: no maker's logo is in this repository.
