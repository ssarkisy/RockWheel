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
top-level `fonts/` (`16-GNU-Unifont.fnt`).

Hangul comes from fonts of its own in the theme's sizes, `24-`, `17-` and
`14-Hangul.fnt`, made by `tools/make_hangul_fonts.sh` from Pretendard
(https://github.com/orioncactus/pretendard, its Hangul from Source Han Sans), under
the SIL Open Font License 1.1 (`Hangul-OFL.txt`). They aren't called Pretendard, a
name its license reserves.

The other alphabets come from `fonts/scripts/`, made in the same sizes by
`tools/make_script_fonts.sh`: Han and the full-width forms from Noto Sans JP, SC
and TC (their Han from Source Han Sans), Hebrew from Noto Sans Hebrew, Arabic from
Vazirmatn and Thai from IBM Plex Sans Thai, all from Google Fonts and under the SIL
Open Font License 1.1, each with its license beside it. They are named after their
alphabets, as Source and Plex are names those licenses reserve.

The iPod doesn't draw with these or the Hangul fonts directly. On the Updating
Library screen it takes from them just the glyphs that the library's tags, the
names of its playlists and the languages of the menus are written in, and makes
`24-`, `17-` and `14-Library.fnt` of those (`apps/library_fonts.c`), small enough
to stay loaded. Until it has, Korean menus fall back to the Hangul fonts.

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
