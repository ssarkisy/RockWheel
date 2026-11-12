# iPod theme

The theme for the iPod Video (320x240), kept here in the layout it is installed
in (`<theme>/.rockbox/...`). The menus don't offer another one. Its dark version,
`iClassic-Square-Dark`, is made from it by `tools/make_dark_theme.py`.

It's licensed under Creative Commons Attribution-Share Alike 3.0
(http://creativecommons.org/licenses/by-sa/3.0/), and so are the changes made
to it here.

`fonts/` holds the fonts it uses: Inter (https://rsms.me/inter/), converted with
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

The fonts of the themes to come, Neo and Modern, are made by the same two
scripts, from Inter Regular too, at the em sizes of their mockups and named
after their line heights as the rest: `22-Inter-SemiBold` (17 px), `19-Inter-Medium`
(15), `17-Inter-Regular` (13), `15-Inter-Medium`, `-SemiBold` and `-Regular` (12)
and `14-Inter-Medium` (11) for Neo; `29-Inter-Bold` (23), `20-Inter-SemiBold`
(15.5), `18-Inter-Regular` (14), `14-Inter-Regular` (11) and `13-Inter-Regular`
(10) for Modern. iClassic Square loads none of them. Each has the other
alphabets in its own weight and size, but `29-Inter-Bold`, a large title's,
which has Latin, Greek and Cyrillic only. The ideographs and kana are 10 px at
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
- A dark version, `iClassic-Square-Dark`, which the Dark Theme setting shows.
- The iOS look of Display → Appearance: the status bar's icons, the rating's
  stars and the progress bar as Linus (u/Linusssk) drew them in the style of
  iOS 13's Music for iPodWizard, used with his permission. The skins show them
  or the theme's own by the settings (`%?St(status bar style)` and the others).
  The bar's knob and the volume's speakers are drawn by `make_ios_icons.py` as
  plain shapes, not by him.

The extra bitmaps are drawn by the scripts in `tools/`, which are run again
after a change, in this order:

- `make_assets.py`, `make_status_icons.py`, `make_night_icon.py`,
  `make_usb_screen.py`, `make_about_panel.py`: bitmaps of their own.
- `make_menu_icons.py`: the backdrops' placeholder note and its reflection, and
  the original theme's battery out of their status bar, where the skins draw one.
- `make_ios_icons.py <folder of Linus's BMPs>`: the iOS look's bitmaps, and the
  dark theme's battery.
- `make_dark_theme.py`: the dark theme, all of it from the light one.

To try them in the simulator, copy them over the simulator disk:

    rsync -a themes/*/.rockbox/ build-sim/simdisk/.rockbox/

The ring on the boot screen and on the panel beside the main menu is drawn by
`tools/make_wheel_mark.py`, and the panel made from it by `tools/make_about_panel.py`.
They are this project's own: no maker's logo is in this repository.
