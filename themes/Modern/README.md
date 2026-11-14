# Modern

Modern is one of RockWheel's three themes, with Classic and Neo. It is chosen
under Settings > Appearance > Theme.

These skins run only on RockWheel's firmware. They use skin tags, theme
metrics and settings that stock Rockbox doesn't have, and stock Rockbox can't
load them.

## What it looks like

Modern is a clean, flat list interface:

- A white ground, black text and one saturated pink-red accent. No gradients,
  bevels or textures.
- Rows 30 px high in Inter SemiBold, 36 px for a row of two lines, with no
  hairlines between them. The selected row is a flat band of the accent across
  the whole list, its text white. In Settings the band is a light grey and the
  setting's name is in the accent.
- A chevron on every row that leads on. Section headings, the marks before
  names and the speaker of the song playing are in the accent; Music's rows
  each have a mark. On and Off values are small capsule switches, green when
  on. The scrollbar is a thin line.
- A light grey status bar, 24 px with the hairline under it, on every screen:
  the clock at the left, the screen's title in the middle, and at the right,
  from the edge inwards, the battery, play or pause (a padlock while Hold is
  on) and a moon while Night Mode is on. A title too wide for the middle
  takes the clock's place too, and ends in an ellipsis if it is wider still.
  Beside the main menu's pane the bar has only the clock and the glyphs.
- The main menu is 160 px wide, beside a pane of 160 px. The pane is filled by
  the cover of the song playing, or, with nothing playing, of the song it
  would resume. Without a cover it shows a note, and under it the number of
  songs or the song's title. Beside Settings it shows the disk's use as a ring,
  with the free space and the disk's size; beside Photos, a collage of the
  photos.
- Now Playing has a square 140 px cover at the left with a reflection under
  it. Beside it, as one block centred on the cover's height: the title, the
  artist in the accent, the album, the song's place in its list, then the stars
  and the format and bitrate if the settings show them. A podcast episode shows
  its show, date and time left; an audiobook its author, chapter and speed.
  Across the screen under them is a capsule progress bar in the accent with
  the times under its ends; at rest its knob is a dot at the end of the fill.
  Turning the wheel shows the volume in the bar's place, and the volume as
  text under it. Repeat and shuffle stand under the right end of the status
  bar.
- The centre button steps through the other views where they apply: the
  scrubber, the speed of a podcast or an audiobook, the rating (five large
  stars), a large cover and the lyrics.

The dark version, Modern-Dark, has a black ground, white text, a near-black
status bar and the accent a little brighter. The selected row keeps the light
version's accent, on which white text reads better.

## Settings

Modern has no colour settings of its own: its accent is fixed. These settings
change how it looks:

- **Dark Theme** (Settings > Appearance): the dark version. It changes while
  music plays.
- **Angled Cover** (Appearance > Now Playing): Now Playing's cover is turned
  in perspective above its reflection. Off, it faces the front.
- **Rating** (Always, Only Rated, Never) and **Format and Bitrate**
  (Appearance > Now Playing): whether Now Playing shows the stars, and the
  format and bitrate, under the song's place in its list.
- **Moving Cover** (Appearance > Main Menu): the pane's cover pans slowly from
  side to side.
- **Night Mode** (Settings > Night Mode): the moon in the status bar while it
  is on. With its Dark Theme option on, the default, it also switches to the
  dark version.

Colour, Tinted Background, Cover Colours and Status Icons don't show in
Modern: it takes no colour from a cover or from a choice of tints. The menus
hide them (`apps/menus/display_menu.c`).

## Author and licence

Designed by Sergei Sarkisian for RockWheel. The theme was made from scratch.

- The skins, images and glyphs are licensed under Creative Commons
  Attribution-ShareAlike 3.0 (http://creativecommons.org/licenses/by-sa/3.0/),
  the same licence as iClassic Square, or, at your choice, under the GNU
  General Public License, version 2 or later. The glyphs are also built into
  the firmware, as C tables (`apps/gui/*_glyphs.h`), under the GPL with it.
- The scripts in `themes/tools/` that make them are code, under the GNU
  General Public License, version 2 or later, as the rest of the repository.
- The fonts are under the SIL Open Font License 1.1: see `../README.md`.

## How it is built

Nothing under `.rockbox/wps/` is edited by hand. Two scripts in
`themes/tools/` write all of it.

### The skins

`make_modern_theme.py` writes `Modern.sbs` (the status bar and the main
menu's pane), `Modern.wps` (Now Playing), `Modern-Dark.sbs` and
`Modern-Dark.wps`. Each comes from a template in the script, a look's colours
and the glyphs' sizes in `modern_glyph_sizes.txt`, by which it places each
glyph: a glyph redrawn at another size needs no other change.

Its main parameters, at the top of the script:

- `COLOURS`: each look's colours by role (`GROUND`, `INK`, `SMALL`, `MUTED`,
  `ACCENT`, `ACCENTTEXT`, `TOP`, `RULE`, `PANE`, `TILE`, `COLUMN`, `REFLECT`).
  The templates name the roles, as in `%Vb(TOP)`, and each look's values take
  their place. They are the colours of Modern's entry in
  `apps/gui/ui_theme.c`: change both together.
- `REFLECT_LIGHT` and `REFLECT_DARK`: the reflection's rows and its strength
  at the top, out of 256, for `%Cr`.
- The layout: `STATUS` (the status bar's rows), `MARGIN`, `LIST_TOP` and
  `LIST_ROWS` (the lists' area), `SPLIT` (the main menu's width), `COVER_TOP`
  and `COVER` (Now Playing's cover), `REFLECTION`, `COL` (the details' column),
  `TITLE_ROWS`, `ARTIST_ROWS`, `SMALL_ROWS` and the gaps of the details block,
  `BAR_Y`, `TIMES_Y` and `MODES_Y`.

Skins can't place one viewport by what another shows, so the script writes a
set of viewports for each combination of lines in the details block (stars or
not, format or not, a song, an episode or a book) and a conditional that
picks one.

### The glyphs

The glyphs are drawn in the design mockups, which are not in this repository,
and exported as PNG files with a `manifest.json`. `make_modern_glyphs.py`
reads that folder and writes:

- `Modern/<Name>.bmp` and `Modern-Dark/<Name>.bmp`: the skins' glyphs, 32-bit
  with alpha, each in its look's colour (`SKIN` in the script gives each
  glyph's colour role, `LOOKS` the colours);
- `apps/gui/modern_glyphs.h`: the marks the lists and Search draw, such as
  the chevron and the speaker;
- `apps/gui/modern_usb_glyphs.h`: the screen shown while connected over USB,
  which is compiled in because no file can be read then;
- `themes/tools/modern_glyph_sizes.txt`: each glyph's width, height and
  number of frames, for `make_modern_theme.py`.

The export's format:

- `manifest.json` lists, under `themes` > `modern`, each glyph's `name`,
  `files`, `width`, `height`, `frames` and `kind`.
- A `mask` is one black PNG with alpha. Its frames are stacked top to bottom,
  each `height` px high. The script colours it for each look.
- A `coloured` glyph (the battery and the notes shown for a missing cover) has
  one PNG a look, in `modern/light/` and `modern/dark/`, used as drawn.
- The PNGs are 8-bit RGBA, not interlaced.
- The stars are each frame of `Ratings` or `Stars` over the outlines in
  `Ratings Ground` or `Stars Ground`.

Some glyphs are drawn by code instead: Playlists' marks and the smart
playlists' gear (`catalogue_marks()` and `smart_gear()` in
`make_theme_glyphs.py`), the bars' capsules and the knob at rest. Shuffle's
first frame, for songs, is a table in `make_modern_glyphs.py`
(`SHUFFLE_SONGS`) that takes the place of the export's; its second frame,
for albums, is the export's.

To redraw a glyph, change its PNG (or, for Shuffle's first frame, that
table), and its size or frames in `manifest.json` if they change. Then run both scripts, as below. The list marks and the USB
screen are compiled into the firmware, so rebuild it after they change. The
headers and the sizes file record the manifest's sha256.

### The dark version

The same runs make Modern-Dark, from the `Modern-Dark` entries of `COLOURS`
and `LOOKS`. They match the second look (`look[1]`) of Modern's entry in
`apps/gui/ui_theme.c`. Both versions load the same fonts and the same cover
sizes. That is what lets Dark Theme reload the skins without stopping the
music (`skin_reload_while_playing()` in `apps/gui/skin_engine/skin_engine.c`),
so keep them so.

### Fonts

All are Inter, made by `themes/tools/make_fonts.sh` and
`themes/tools/make_script_fonts.sh` into `themes/fonts/.rockbox/fonts/` (see
`../README.md`). A file is named after its line height.

| File | Weight, em size | Used for |
|---|---|---|
| `19-Inter-SemiBold` | SemiBold, 15 px | the lists' rows; the lyrics |
| `18-Inter-SemiBold` | SemiBold, about 14 px | Now Playing's title; section headings, a setting's value, an album's name over its songs; the free space beside Settings |
| `17-Inter-SemiBold` | SemiBold, 12.5 px | the song's title under the pane's note |
| `17-Inter-Medium` | Medium, 12.5 px | Now Playing's artist, show or author; the artist in an album's header |
| `17-Inter-Medium-13` | Medium, 13 px | the hints under a setting |
| `15-Inter-Medium` | Medium, 12 px | second lines, numbers before names, values |
| `14-Inter-SemiBold` | SemiBold, about 11 px | the status bar's clock and title |
| `14-Inter-Medium` | Medium, 11 px | small text: captions, the album, times, the format, the counts in an album's header |
| `53-Inter-Letters` | Bold, 52 px | Quick Scroll's letter |

The skins load theirs with `%Fl`: `Modern.sbs` loads `14-Inter-SemiBold`,
`18-Inter-SemiBold`, `14-Inter-Medium` and `17-Inter-SemiBold`; `Modern.wps`
loads `14-Inter-SemiBold`, `18-Inter-SemiBold`, `17-Inter-Medium` and
`14-Inter-Medium`. The lists' fonts are named by Modern's `font[]` in
`apps/gui/ui_theme.c`, and the firmware loads them. The font of an album's
name over its songs (`UI_FONT_HEADER_NAME`) is the exception: the firmware
doesn't load it, and uses it only if it is loaded already. Here it is
`18-Inter-SemiBold`, which `Modern.sbs` loads.

### Regenerating

From the repository's root, with Python 3 and no other packages:

    python3 themes/tools/make_modern_theme.py

That writes the four skins again. After a change to the glyph export, first
run, with the folder that holds `manifest.json` and `modern/`:

    python3 themes/tools/make_modern_glyphs.py <export>

Run them with `python3`: they are not executable files. On the committed
tree, `make_modern_theme.py` reproduces the committed skins byte for byte, and
so does `make_modern_glyphs.py` the bitmaps, headers and sizes file, given the
export whose sha256 they record. Never edit the `.sbs`, `.wps` or `.bmp`
files by hand: change the script and run it again.

## What it depends on in the firmware

### Skin tags

These are the tags RockWheel added to `lib/skin_parser/tag_table.c`, or
changed there, that Modern's skins use. What they draw is in
`apps/gui/skin_engine/` (`skin_tokens.c`, `skin_render.c`,
`skin_display.c`).

| Tag | What it does |
|---|---|
| `%Cr(rows,strength)` | Draws the cover of the `%Cl` before it with a reflection under it, that many rows high, at that strength out of 256 at its top. With Angled Cover the cover is turned in perspective. |
| `%fB` | The format and bitrate, after a small mark of a sound wave. The bitrate takes the font of a hidden viewport labelled `bitrate`. |
| `%kk` | The track's kind, for `%?kk<song\|podcast\|audiobook>`. |
| `%kd` | A podcast episode's release date. |
| `%kl` | The time left, as "28 min left". |
| `%ks` | The speed a podcast or an audiobook plays at, as "1.25×"; nothing at normal speed. |
| `%kc`, `%kn`, `%kt` | The chapter's name, its number ("Chapter 2") and the number of chapters. |
| `%Lf(label)` | True when the screen's title fits the viewport of that label in its font. A longer title is given another place by it. |
| `%LS` | Which main menu row with a pane of its own is selected: Settings, Extras, Photos, or none of them. |
| `%LW` | True when a screen takes the whole width, with no pane beside it. |
| `%mw` | Now Playing's view: normal, scrubber, large cover, lyrics, rating or speed. |
| `%nc` | Rounds the viewport's corners as the theme rounds a cover there (`UI_METRIC_COVER_RADIUS`). |
| `%nf`, `%nT` | The disk's free space ("116 GB Free") and its size. |
| `%nn` | The player's name, as the synced library has it. |
| `%nN` | True while Night Mode is on. |
| `%nP` | The number of photos, or "No Photos"; as a condition, whether there are any. |
| `%nR` | The disk's use as a ring in the middle of the viewport, drawn by the firmware. |
| `%ns` | The number of songs in the library ("3 Songs"). |
| `%nv` | The volume as text: "Volume: 62%", or "Volume: 62/80%" with a limit set. |
| `%nx` | On a line that doesn't scroll, a text too wide ends at a whole letter with an ellipsis. With a left and a right text, the right one stays whole and the left one is cut before it. |
| `%pa` | True when shuffle is by albums. |
| `%pv` (changed) | The volume bar; it is now redrawn alone as the volume changes. |
| `%pb(…,slidercentre)` (new option) | The knob is centred on the end of the fill. The option is parsed in `apps/gui/skin_engine/skin_parser.c`. |

The skins also rely on these names, which the firmware looks for:

- the viewport labels `lyrics`, `speed` and `bitrate`: `apps/gui/wps.c` draws
  Now Playing's lyrics and speed controls into the first two, and the third
  gives `%fB` its font;
- `/.rockbox/photos_panel.bmp`: the Photos collage, which the firmware fills
  (`apps/photos_panel.h`);
- the settings `now playing rating` and `now playing format`, read with
  `%St`;
- the main menu's pane cover, a 240 px cover loaded with `%Cl` and panned by
  Moving Cover (`apps/gui/skin_engine/menu_cover.c`).

### The `ui_theme` entry

Everything outside the skins is Modern's entry in `ui_themes[]` in
`apps/gui/ui_theme.c`. `apps/gui/ui_theme.h` says what each colour, metric and
font role does. The main values:

- **Looks:** `look[0]` is the skin `Modern`, `look[1]` `Modern-Dark`; no
  backdrop (`"-"`).
- **Flags:** no selection bevel; `cover_colours` off and no `cover_tone`, so
  it takes no colour from covers and has no tints.
- **Rows:** `UI_METRIC_ROW_HEIGHT` 30; `UI_METRIC_TEXT_INSET` 8, the text's
  distance from the list's edge; `UI_METRIC_ROW_COVER_INSET` 4 for a row's
  cover; `UI_METRIC_ROW_RULE` 0, no hairlines.
- **Selection:** `UI_METRIC_SELECTION_RADIUS` 0, a band of the row's whole
  width; `UI_METRIC_SELECTION_INK` 1, everything on it in the selection's
  text colour; `UI_METRIC_SETTINGS_SELECTION` 1, Settings' grey band
  (`UI_COLOUR_SETTINGS_SELECTOR`).
- **Right end of a row:** `UI_METRIC_CHEVRON_ALL` 1, a chevron on every row
  that leads on; `UI_METRIC_ROW_RIGHT`, `UI_METRIC_CHEVRON_INSET`,
  `UI_METRIC_VALUE_MARGIN` and `UI_METRIC_VALUE_GAP` place the chevron and the
  value; `UI_METRIC_VALUE_CUT` 1, a value too long beside its name ends in an
  ellipsis.
- **Switches:** `UI_METRIC_SWITCHES` 1, a capsule of 27 by 16 px with a white
  knob, in `UI_COLOUR_SWITCH_ON` and `UI_COLOUR_SWITCH_OFF` (`apps/menu.c`).
- **Other parts:** `UI_METRIC_GROUP_BAR` 3, section headings in the accent;
  `UI_METRIC_SCROLLBAR_LINE` 2, the scrollbar as a line;
  `UI_METRIC_ACCENT_MARKS` and `UI_METRIC_MENU_MARKS` 1, the marks in the
  accent and on Music's rows; `UI_METRIC_PANE_LEFT` 160, the pane's first
  column, as `SPLIT` in the script; `UI_METRIC_COVER_RADIUS` 0, square covers
  in Now Playing; the `UI_METRIC_HEADER_*` values for an album's header.
- **Fonts:** `list_font` `19-Inter-SemiBold` and `font[]`, as in the table
  above.

The selection and insets are drawn in `apps/gui/bitmap/list.c` and
`apps/gui/line.c`; the switches, section headings and values in
`apps/menu.c`. The list marks for each theme are the `glyphs[]` table in
`apps/gui/line.c`, and the USB screen is in `apps/gui/usb_screen.c`.
