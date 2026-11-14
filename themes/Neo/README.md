# Neo

Neo is one of RockWheel's three themes, with Classic and Modern. It is chosen
under Settings > Appearance > Theme.

These skins run only on RockWheel's firmware. They use skin tags, theme
metrics and settings that stock Rockbox doesn't have, and stock Rockbox can't
load them. Neo's colours also depend on the firmware: its skins name surfaces
and texts that the firmware tints (see below).

## What it looks like

Neo is calm and airy:

- Flat surfaces, no gradients, and a lot of space. Fine type: Inter SemiBold
  and Medium at 11 to 15 px. Warm near-black text.
- It is never plain. One colour, chosen under Colour, carries the theme: the
  selected row, the switches, the chevrons and the scrollbar, the marks before
  names, the speaker of the song playing, Now Playing's artist and progress
  bar, and the disk's ring. With Tinted Background off, the default, the
  surfaces are neutral: a white ground with light grey panes, bands and
  fields, and grey small text. With it on, the surfaces take a pale tint of
  the colour, and small text and section names a deeper one.
- Rows 30 px high in Inter SemiBold, 36 px for a row of two lines. The
  selected row is a rounded rectangle inset from the row's sides, in the
  colour, with white text. A chevron on every row that leads on. A section's
  name stands on a band as high as a row. On and Off values are capsule
  switches, in the colour when on and grey when off. The scrollbar is a slim
  capsule, and a message stands on a dark rounded plaque.
- A status line 20 px high, on the screen's own ground, with a hairline under
  it: the clock at the left, the screen's title in the middle, and at the
  right, from the edge inwards, the battery, a moon while Night Mode is on,
  and play or pause (a padlock while Hold is on). A title too wide for the
  middle takes the clock's place too, and ends in an ellipsis if it is wider
  still. Beside the main menu's pane the line holds only the clock and the
  glyphs.
- The main menu's list sits beside a pane that starts after a hairline on
  column 159. The pane is filled by the cover of the song playing, or, with
  nothing playing, of the song it would resume. Without a cover it shows a
  note, and under it the number of songs or the song's title. Beside Settings
  it shows the disk's use as a ring in the colour, with the free space and the
  disk's size; beside Photos, a collage of the photos.
- Now Playing has a square 136 px cover at the left with a reflection under
  it. Beside it, as one block centred on the cover's height: the title on one
  line or two, never scrolled; the artist in the colour; the album; the song's
  place in its list; then the stars and the format and bitrate if the settings
  show them. A podcast episode shows its show, date and time left; an
  audiobook its author, chapter and speed. Across the screen under them is a
  capsule progress bar in the colour; under it, on one line, the time played,
  the chapter where there are chapters, and the time left. Turning the wheel
  shows the volume in the bar's place: a shorter bar between two marks, and
  the volume as text under it. Repeat and shuffle stand under the right end
  of the status line.
- The centre button steps through the other views where they apply: the
  scrubber, which has a knob, the speed of a podcast or an audiobook, the
  rating (five large stars), a large cover and the lyrics.

The dark version, Neo-Dark, has dark neutral grey surfaces, or a dark tint of
the colour with Tinted Background, and light warm text.

## Settings

These settings change how Neo looks:

- **Colour** (Settings > Appearance): Multicolour, the default, then Red,
  Yellow, Green, Teal, Blue, Purple, Pink, Orange, Desert, Olive, Glacier,
  Deep Blue and Burgundy. Any one colour is used on every screen. Multicolour
  gives each section its own:
  - music, red-pink: Music's lists, Cover Flow,
    Search, Playlists, and Now Playing with a song;
  - podcasts, purple: Podcasts' lists, and Now Playing with an episode;
  - audiobooks, orange: Audiobooks' lists, and Now Playing with a book;
  - the rest, blue: the main menu, Settings, Extras, Photos and the other
    screens.
- **Tinted Background** (Settings > Appearance): off, the default, keeps the
  surfaces and small text in neutral greys, with the colour on the selection
  and the marks. On, the ground, panes, bands, fields, tiles, hairlines and
  the bar's track take a tint of the colour, and small text and section names
  a deeper tint.
- **Dark Theme** (Settings > Appearance): the dark version. It changes while
  music plays.
- **Cover Colours** (Appearance > Now Playing): Now Playing in colours from
  the cover.
  - Off: the chosen colour.
  - Text and Stars: Now Playing's text and stars in the cover's hue; the
    surfaces stay in the chosen colour.
  - Whole Screen: Now Playing's surfaces, the status line's too, in the
    cover's hue.
  - Fill: Now Playing filled flat with the cover's own colour, the status line
    a shade of it.
  - A grey or nearly grey cover leaves Text and Stars and Whole Screen as Off.
- **Angled Cover** (Appearance > Now Playing): Now Playing's cover is turned
  in perspective above its reflection. Off, it faces the front.
- **Rating** (Always, Only Rated, Never) and **Format and Bitrate**
  (Appearance > Now Playing): whether Now Playing shows the stars, and the
  format and bitrate, under the song's place in its list.
- **Moving Cover** (Appearance > Main Menu): the pane's cover pans slowly from
  side to side.
- **Night Mode** (Settings > Night Mode): the moon in the status line while it
  is on. With its Dark Theme option on, the default, it also switches to the
  dark version.

Status Icons doesn't show in Neo, whose status glyphs keep their grey. The
other Colour row, with Graphite, is Classic's own. The menus hide them
(`apps/menus/display_menu.c`).

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

`make_neo_theme.py` writes `Neo.sbs` (the status line and the main menu's
pane), `Neo.wps` (Now Playing), `Neo-Dark.sbs` and `Neo-Dark.wps`. Each comes
from a template in the script, a look's colours and the glyphs' sizes in
`neo_glyph_sizes.txt`, by which it places each glyph: a glyph redrawn at
another size needs no other change.

Its main parameters, at the top of the script:

- `COLOURS`: each look's colours by role (`PAPER`, `PRIMARY`, `SECONDARY`,
  `TERTIARY`, `TRACK`, `SELECTED`, `SURFACE`, `DENSE`, `CLOCK`, `NEUTRAL`,
  `ACCENT`, `REFLECT`). The templates name the roles, as in `%Vf(PRIMARY)`,
  and each look's values take their place. Most of these colours are names
  the firmware tints, so they must match Neo's entry in
  `apps/gui/ui_theme.c` (see "The colours are names" below).
- `REFLECT_LIGHT` and `REFLECT_DARK`: the reflection's rows and its strength
  at the top, out of 256, for `%Cr`.
- The layout: `STATUS` (the status line's rows; its hairline is the next
  row), `MARGIN`, `LIST_TOP` and `LIST_ROWS` (the lists' area), `TEXT_Y`,
  `FOOT` and `GAP` (the status line's text and glyphs), `COVER` and `COVER_Y`
  (Now Playing's cover), `REFLECTION`, `TITLE_LINE`, `DETAIL`, `SMALL` and the
  gaps of the details block, `BAR_Y`, `TIMES_Y` and `MODES_Y`.

Skins can't place one viewport by what another shows, so the script writes a
set of viewports for each combination of lines in the details block (a title
of one line or two, stars or not, format or not, a song, an episode or a
book) and a conditional that picks one.

### The glyphs

The glyphs are drawn in the design mockups, which are not in this repository,
and exported as PNG files with a `manifest.json`. `make_theme_glyphs.py`
reads that folder and writes:

- `Neo/<Name>.bmp` and `Neo-Dark/<Name>.bmp`: the skins' glyphs, 32-bit with
  alpha, each in its look's colour (`SKIN` in the script gives each glyph's
  colour role, `LOOKS` the colours), and the bars' capsules;
- `apps/gui/neo_glyphs.h`: the marks the lists and Search draw, such as the
  chevron and the speaker;
- `apps/gui/neo_usb_glyphs.h`: the screen shown while connected over USB,
  which is compiled in because no file can be read then;
- `themes/tools/neo_glyph_sizes.txt`: each glyph's width, height and number
  of frames, for `make_neo_theme.py`.

The export's format:

- `manifest.json` lists, under `themes` > `neo`, each glyph's `name`,
  `files`, `width`, `height`, `frames` and `kind`.
- A `mask` is one black PNG with alpha. Its frames are stacked top to bottom,
  each `height` px high. The script colours it for each look.
- A `coloured` glyph, a note shown for a missing cover, has a PNG a look in
  `neo/light/` and `neo/dark/`, and beside it a mask, `<Name> Mask`. Neo uses
  only the mask: the skins draw it in the theme's two note colours (`%xN`),
  which take the tint.
- The PNGs are 8-bit RGBA, not interlaced.
- The stars are each frame of `Ratings` or `Stars` over the outlines in
  `Ratings Ground` or `Stars Ground`. The small stars' outline is drawn in a
  grey (`UNSET` in `LOOKS`) by which the firmware knows it and draws it in
  the colour. The script stops if any other pixel of those pictures comes to
  that grey on the screen.

Some glyphs are drawn by code instead: Playlists' marks and the smart
playlists' gear (`catalogue_marks()` and `smart_gear()`), and the bars'
capsules (`BAR` and `VOLUME_BAR`).

To redraw a glyph, change its PNG, and its size or frames in `manifest.json`
if they change. Then run both scripts, as below. The list marks and the USB
screen are compiled into the firmware, so rebuild it after they change. The
headers and the sizes file record the manifest's sha256.

### The dark version

The same runs make Neo-Dark, from the `Neo-Dark` entries of `COLOURS` and
`LOOKS`. They match the second look (`look[1]`) of Neo's entry in
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
| `19-Inter-SemiBold` | SemiBold, 15 px | the lists' rows, an album's name over its songs; the free space beside Settings |
| `18-Inter-SemiBold` | SemiBold, about 14 px | Now Playing's title; section names, a setting's value |
| `17-Inter-Medium-13` | Medium, 13 px | the hints under a setting, the artist in an album's header; the lyrics |
| `15-Inter-SemiBold` | SemiBold, 12 px | the status line's clock and title |
| `15-Inter-Medium` | Medium, 12 px | second lines, numbers before names, values, the counts in an album's header; Now Playing's artist and album lines; the pane's captions |
| `14-Inter-Medium` | Medium, 11 px | Now Playing's small text: the place in the list, the format, the times, the volume, the speed |
| `53-Inter-Letters` | Bold, 52 px | Quick Scroll's letter |

The skins load theirs with `%Fl`: `Neo.sbs` loads `15-Inter-Medium` and
`15-Inter-SemiBold`; `Neo.wps` loads `18-Inter-SemiBold`,
`17-Inter-Medium-13`, `14-Inter-Medium`, `15-Inter-SemiBold` and
`15-Inter-Medium`. The lists' fonts are named by Neo's `font[]` in
`apps/gui/ui_theme.c`, and the firmware loads them. Neo names no font for an
album's name over its songs, so that is the list's.

### Regenerating

From the repository's root, with Python 3 and no other packages:

    python3 themes/tools/make_neo_theme.py

That writes the four skins again. After a change to the glyph export, first
run, with the folder that holds `manifest.json` and `neo/`:

    python3 themes/tools/make_theme_glyphs.py <export>

Run them with `python3`: they are not executable files. They write UTF-8
whatever the locale. On the committed tree, `make_neo_theme.py` reproduces the
committed skins byte for byte, and so does `make_theme_glyphs.py` the bitmaps,
headers and sizes file, given the export whose sha256 they record. Never edit
the `.sbs`, `.wps` or `.bmp` files by hand: change the script and run it
again.

## What it depends on in the firmware

### Skin tags

These are the tags RockWheel added to `lib/skin_parser/tag_table.c`, or
changed there, that Neo's skins use. What they draw is in
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
| `%nN` | True while Night Mode is on. |
| `%nP` | The number of photos, or "No Photos"; as a condition, whether there are any. |
| `%nR` | The disk's use as a ring in the middle of the viewport, drawn by the firmware. |
| `%ns` | The number of songs in the library ("3 Songs"). |
| `%nv` | The volume as text: "Volume: 62%", or "Volume: 62/80%" with a limit set. |
| `%nw(1)`, `%nw(2)` | The title on two lines, broken at a space; the second ends in an ellipsis if the title is longer. Never scrolled. |
| `%nW(label)` | True when the title takes both lines in the viewport of that label. |
| `%nx` | On a line that doesn't scroll, a text too wide ends at a whole letter with an ellipsis. With a left and a right text, the right one stays whole and the left one is cut before it, as the time left beside the speed. |
| `%pa` | True when shuffle is by albums. |
| `%xC(label)` | The image, loaded before, is a picture of stars in the theme's colours: the set stars with the text, in the cover's colour under Cover Colours, and the outline of a star not set in the theme's colour for it. |
| `%xN(label)` | The image, loaded before, is a missing cover's note: only its alpha is drawn, in the theme's two colours for a note. |
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

### The colours are names

Neo has a tone (`cover_tone` in its entry), so most colours in its skins are
not shown as written. The firmware replaces them as it draws:

- a viewport's ground (`%Vb`) and a bar's track take the current tone's
  surface nearest to it in `tone_surfaces` in `apps/gui/ui_theme.c`. `PAPER`,
  `SURFACE`, `TRACK` and `SELECTED`, and the bars' `BARTRACK` in
  `make_theme_glyphs.py`, are such surfaces (`skin_surfaces()` in
  `apps/gui/skin_engine/skin_engine.c`, `ui_theme_surface()`);
- a text, a bar's fill or a knob in `DENSE`, `SECONDARY` or `ACCENT` takes
  the tone's small text, selected row or accent colour. They are the look's
  `UI_COLOUR_VALUE`, `UI_COLOUR_NOTE_TOP` and `UI_COLOUR_ACCENT`
  (`ui_theme_skin_text()`);
- `CLOCK` and `NEUTRAL` are a step away from `DENSE` on purpose: the clock
  and Now Playing's grey details stay grey. `PRIMARY` stays as written.

So a change to one of these colours goes into `apps/gui/ui_theme.c` too (the
look's `colour[]` and `tone_surfaces`), or the tint no longer reaches it.

### The `ui_theme` entry

Everything outside the skins is Neo's entry in `ui_themes[]` in
`apps/gui/ui_theme.c`. `apps/gui/ui_theme.h` says what each colour, metric and
font role does. The main values:

- **Looks:** `look[0]` is the skin `Neo`, `look[1]` `Neo-Dark`; no backdrop
  (`"-"`).
- **Flags:** no selection bevel; `cover_colours` off; `cover_tone` on, which
  gives Neo the Colour and Tinted Background rows and its own Cover Colours.
- **Tones:** `tone_set` (each value of Colour), `tone_sections` (Multicolour's
  colour for each section of `enum ui_section`), `tone_surfaces` (each
  surface's lightness in a tone), `tone_neutral` (the greys without Tinted
  Background) and `tone_make()`. `ui_theme_set_colour()` takes the settings;
  `ui_theme_set_section()`, called as screens open, gives Multicolour its
  section; `ui_theme_cover_tone()` and `ui_theme_cover_fill()` serve Cover
  Colours (`apps/gui/skin_engine/cover_palette.c`).
- **Rows:** `UI_METRIC_ROW_HEIGHT` 30; `UI_METRIC_TEXT_INSET` 14, the text's
  distance from the list's edge; `UI_METRIC_ROW_COVER_INSET` 8 for a row's
  cover.
- **Selection:** `UI_METRIC_SELECTION_RADIUS` 7, a rounded rectangle inset
  `UI_METRIC_SELECTION_INSET_X` 6 px from the row's sides and
  `UI_METRIC_SELECTION_INSET_Y` 2 px from its top and foot;
  `UI_METRIC_SELECTION_INK` 1, everything on it in the selection's text
  colour. The main menu's list is 156 px wide in `Neo.sbs`, so that its
  selection ends at column 150.
- **Right end of a row:** `UI_METRIC_CHEVRON_ALL` 1, a chevron on every row
  that leads on; `UI_METRIC_ROW_RIGHT`, `UI_METRIC_VALUE_MARGIN` and
  `UI_METRIC_VALUE_GAP` place the chevron and the value;
  `UI_METRIC_VALUE_CUT` 1, a value too long beside its name ends in an
  ellipsis.
- **Switches:** `UI_METRIC_SWITCHES` 2, a capsule of 46 by 22 px with a
  capsule knob, on in the selected row's colour, off in
  `UI_COLOUR_SWITCH_OFF` (`neo_switch()` in `apps/menu.c`).
- **Other parts:** `UI_METRIC_GROUP_BAR` 2, a section's name on a band;
  `UI_METRIC_SCROLLBAR_CAPSULE` 3, the scrollbar's width;
  `UI_METRIC_SPLASH_RADIUS` 9 with `UI_METRIC_SPLASH_PAD_X` and
  `UI_METRIC_SPLASH_PAD_Y`, the message's plaque in `UI_COLOUR_SPLASH_FILL`
  and `UI_COLOUR_SPLASH_TEXT`
  (`apps/gui/splash.c`); `UI_METRIC_STATUS_ROWS` 21, Now Playing's status
  line and its hairline, `STATUS` in the script plus one, which Cover Colours'
  Fill shades; `UI_METRIC_PANE_LEFT` 160, the pane's first column;
  `UI_METRIC_COVER_RADIUS` 0, square covers in Now Playing; the
  `UI_METRIC_HEADER_*` values for an album's header, its cover 52 px.
- **Fonts:** `list_font` `19-Inter-SemiBold` and `font[]`, as in the table
  above.

The selection and insets are drawn in `apps/gui/bitmap/list.c` and
`apps/gui/line.c`; the switches, section names and values in `apps/menu.c`.
The list marks for each theme are the `glyphs[]` table in `apps/gui/line.c`,
and the USB screen is in `apps/gui/usb_screen.c`.
