# iPod themes

The themes for the iPod Video (320x240), each kept here in the layout it is
installed in (`<theme>/.rockbox/...`): iClassic Square, which Settings calls
Classic, Modern and Neo, each with a dark version beside it.

iClassic Square is licensed under Creative Commons Attribution-Share Alike 3.0
(http://creativecommons.org/licenses/by-sa/3.0/), and so are the changes made
to it here. Modern and Neo were designed by Sergei Sarkisian for RockWheel, from
scratch. Their skins, images and glyphs are licensed under the same Creative
Commons Attribution-ShareAlike 3.0, so all three themes share one licence, or,
at your choice, under the GNU General Public License, version 2 or later, as
their glyphs are also built into the firmware (`apps/gui/*_glyphs.h`). The
scripts in `tools/` are code, licensed under the GNU General Public License,
version 2 or later, as the rest of the repository. The fonts' licences are
below. `Modern/README.md` and `Neo/README.md` describe those two themes and
how they are made.

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

- **Modern** and **Neo**: designed by Sergei Sarkisian for RockWheel.
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

## Adding a theme

The Theme setting is a fixed list built into RockWheel's firmware, not
Rockbox's theme browser. A theme is a value of that list, an entry in the
theme table and a folder here. A fork that adds one goes through these; the
paths are from the repository's root.

- [ ] **The list.** In `apps/gui/ui_theme.h`, a value in the enum of
  `UI_THEME_CLASSIC`, `UI_THEME_MODERN`, `UI_THEME_NEO`, before
  `UI_THEME_COUNT`. In `apps/settings_list.c`, the `ui_theme` setting: add
  the theme's name for config.cfg to `"classic,modern,neo"`, raise the count
  of values (3), and add its name phrase (`ID2P(LANG_THEME_…)`), all in the
  enum's order. config.cfg keeps the theme by that name, so the order is only
  the row's.
- [ ] **The table entry.** `ui_themes[]` in `apps/gui/ui_theme.c`, one entry
  for each theme in the enum's order (`struct ui_theme` in `ui_theme.h`):
  - `look[0]` and `look[1]`, light and dark: `skin`, the base name of the
    `.sbs` and `.wps`; `backdrop`, `"-"` for none; `colour[]`, by
    `enum ui_colour`; `scrollbar_track` and `initial_opacity`;
  - `list_font`, the font setting's name for the lists' font, and `font[]`,
    a file for each `enum ui_font` role. The firmware loads the roles from
    `UI_FONT_HINT` to `UI_FONT_GROUP` and Quick Scroll's letter itself
    (`apps/gui/bitmap/list.c`); `UI_FONT_HEADER_NAME` is used only if its
    file is loaded already, as by a skin;
  - `metric[]`, by `enum ui_metric`, whose comments in `ui_theme.h` say what
    each one does, and often what 0 means;
  - the flags `selector_bevel`; `cover_colours`, Now Playing in colours from
    the cover, as Classic; `cover_tone`, a tinted theme as Neo, with Colour
    and Tinted Background; `flat_cover`, a cover that never turns, which
    hides Angled Cover;
  - `scrollbar_thumb[]`.
- [ ] **Code that names a theme.** `glyphs[]` in `apps/gui/line.c` has the
  list marks of each theme: give the new one a row, an existing theme's if
  need be. The note of an album without a cover (`apps/album_header.c`) and
  the USB screen (`apps/gui/usb_screen.c`) are Classic's unless the theme is
  named there. `apps/menus/display_menu.c` hides rows by the flags above, and
  Classic's own Colour row by `UI_THEME_CLASSIC`.
- [ ] **The name phrase.** A `LANG_THEME_<NAME>` phrase at the end of
  `apps/lang/english.lang`, as that file asks, and in every other
  `apps/lang/*.lang` but `walon.lang`, in each language's own terms (Modern
  and Neo are proper names, kept as they are). `tools/lang_phrase.py` writes
  all the files in one step from a batch: see its header and
  `tools/lang_phrase.example.json`, and give `"after"` the last id of
  `english.lang`. Then check; `check_langs.py` prints problems without
  failing, so read its output:

      python3 -I tools/lang_phrase.py --check
      git show <base>:apps/lang/english.lang > base-english.lang
      python3 tools/check_langs.py apps/lang base-english.lang
      python3 -I tools/lang_phrase.py --fit LANG_THEME_<NAME> --name LANG_THEME

  `<base>` is the revision before the change. The last command measures the
  name beside "Theme" in each language.
- [ ] **Assets and fonts.** `themes/<Name>/.rockbox/wps/` holds `<Name>.sbs`
  and `<Name>.wps`, `<Name>-Dark.sbs` and `<Name>-Dark.wps`, and the folders
  `<Name>/` and `<Name>-Dark/` with their bitmaps: a skin reads its images
  from the folder of its own name. Classic also has a `.fms`, for the radio
  screen; Modern and Neo don't. The skins load their fonts with `%Fl`. A new
  font goes in `themes/fonts/.rockbox/fonts/` with its licence; the Inter
  ones are made by `themes/tools/make_fonts.sh` and
  `themes/tools/make_script_fonts.sh`, as described above.
- [ ] **The generator and the dark version.** Write the skins with a script
  in `themes/tools/`, as `make_modern_theme.py` and `make_neo_theme.py` do:
  templates, a table of colours for each look, and the glyphs' sizes read
  from a file, so that one run makes both versions. Both must load the same
  fonts and the same cover sizes, or Dark Theme can't change while music
  plays. Commit the script with what it writes, and don't edit what it
  writes.
- [ ] **What the build installs.** `tools/buildzip.pl` copies every
  `themes/*/.rockbox` folder into `.rockbox` for a 320x240 colour target, in
  `make zip` and in the simulator's `make install`. It turns 24-bit BMPs into
  the screen's 16-bit format and copies 32-bit ones, with alpha, as they are.
  A new folder under `themes/` needs no other change.
- [ ] **PLUGIN_API_REVISION.** Raise it in `apps/plugin.h` when
  `user_settings`, a structure plugins see or an enum they use changes, even
  if no size does: a new `ui_colour`, `ui_metric` or `ui_font` role is one, as
  plugins call `ui_colour()`, `ui_metric()` and `ui_font_file()`. A new field
  of `user_settings` goes at its end. The count of phrases is already part of
  `PLUGIN_API_VERSION`. Install plugins built with the same firmware.
- [ ] **Checks in the simulator.** In a build folder in the repository, such
  as `build-sim/`, configure with
  `../tools/configure --target=ipodvideo --type=s`, then run `make` and
  `make install`, which puts the themes in `simdisk/.rockbox`. Choose the
  theme under Settings > Appearance > Theme and look, in both versions, at:
  the main menu beside each pane (a cover, no cover, Settings, Photos);
  lists of one and two lines, an album's header, section headings, a long
  value beside its name and the switches; Quick Scroll and a message; Now
  Playing in each view, with and without a cover and with a long title, for a
  song, a podcast and an audiobook; Dark Theme changed while music plays;
  Night Mode's moon; and a language written right to left and one in
  Chinese, Japanese or Korean. `RBSIM_SCRIPT` drives the simulator from a
  file and saves screen dumps: see the comment above the script code in
  `firmware/target/hosted/sdl/button-sdl.c`. The simulator shows layout and
  colour, not the player's speed.
