# Dieter

Dieter is one of RockWheel's four themes, with Classic, Modern and Neo. It is
chosen under Settings > Appearance > Theme.

These skins run only on RockWheel's firmware: they use skin tags, theme
metrics and settings that stock Rockbox doesn't have.

## What it looks like

Quiet and precise, with a little depth:

- Warm whites and light greys with near-black type; the dark version is the
  black of a device's body. The ground is lit from the top. Nothing is in
  colour but the cover and one small signal: the cursor's dot, Now Playing's
  needle, Search's caret and chosen key, Cover Flow's chosen track and the
  rating's lit lamps.
- One margin of 16 px (14 on the iPod photo) for the names, the covers and
  the scale; the status line's clock and glyphs 4 px from the screen's sides,
  where the menu's text starts.
- Rows of 30 px in Inter SemiBold, 36 for two lines. The selected row is a
  flat band the list's width, its top row lit and its foot shaded, with a dot
  in the signal colour in the left margin. A chevron on the selected row, and
  on the main menu's Now Playing row while it is selected, as in Classic. A
  group's name is small and grey, low in its row, a hairline under it. On and
  Off are words.
- A status line on its own strip, a hairline and a lit lip under it.
- The main menu's pane is a recess: the cover fills it, its wall's shadow over
  the cover's first columns. With nothing playing it shows a speaker's
  grille punched in its floor, the library's count under it; beside Settings
  the disk's use as a ring in a printed scale of ticks; beside Photos the
  collage.
- Now Playing: a square cover with a shadow of a pixel; the title, artist,
  album and place beside it as one block; the progress as a tuning scale
  behind glass, the played part in ink and a needle in the signal colour,
  grey while paused, with a knob while scrubbing. The volume shows in the same
  window, "Volume" printed under it and the level at the right. The rating
  view is five push keys, their lamps lit for the stars set; the small rating
  line is five small lamps. Without a cover, its place is recessed with the
  grille.
- Quick Scroll's letter is cut into a light round key; Search's letters are
  typewriter keys, the one chosen ringed in the signal colour.

## Settings

- **Colour** (Settings > Appearance): the signal colour, Orange (the default),
  Red, Yellow (a mustard in the light version) or Green.
- **Dark Theme**: the dark version; it changes while music plays.
- **Rating** and **Format and Bitrate** (Appearance > Now Playing), as in the
  other themes. Dieter has no Cover Colours and no Angled Cover: the cover is
  the only large colour, and it never turns.

## On the iPod photo

The same rules on 220x176: a margin of 14, a status line of 16 rows, rows of
20 px in Inter SemiBold of 12 px, two-line rows of 32 with covers of 32 px;
Now Playing's cover 96 px. The pane is beside the main menu alone, 110 px
from column 110.

## How it is built

Nothing under `.rockbox/` is edited by hand. One script writes all of it:

    python3 themes/tools/make_dieter_theme.py

It writes, for each screen and look, `Dieter.sbs` and `Dieter.wps`
(`Dieter-Dark.*`), every picture in `wps/Dieter/` and `wps/Dieter-Dark/`, the
backdrops (`backdrops/Dieter.bmp`, with the main menu's pane, and
`wps/<look>/Wide.bmp` for the screens of the whole width), and
`apps/gui/dieter_glyphs.h`: the list marks, the missing covers' tiles, the
dial's ticks, Search's keys and the USB screen's sign, which the firmware
draws itself. Every picture is made as the screen shows it, each pixel an
RGB565 colour, the gradients dithered at their places on the screen by the
table `tools/buildzip.pl` dithers a backdrop with. The colours are in
`LOOKS` at the script's top and must match Dieter's entry in
`apps/gui/ui_theme.c`. Rebuild the firmware after the header changes.

A light and a dark skin load the same fonts and pictures of the same sizes,
so that Dark Theme reloads them while music plays.

## What it depends on in the firmware

Its entry in `ui_themes[]` (`apps/gui/ui_theme.c`), and these metrics and
roles (`apps/gui/ui_theme.h`): `UI_METRIC_SELECTION_DOT` and
`UI_COLOUR_SIGNAL` (the cursor, the episodes' rings in its column), the
look's `bevel[]` (the band's lit top and shaded foot),
`UI_METRIC_GROUP_HAIRLINE`, `UI_METRIC_SCROLLBAR_THUMB_W`,
`UI_METRIC_PANE_SHADE`, `UI_METRIC_RING_TICKS`, `UI_METRIC_SEARCH_KEYS`,
`UI_METRIC_SPLASH_HIGHLIGHT`; Quick Scroll's key is the status bar skin's
picture labelled `qskey` (`skin_image()`). Its skins use `%nv(1)`, the
volume's level alone, and `%xS(label)`, a picture drawn as a mask in the
signal colour, besides the tags Neo's README lists. The setting is
`dieter colour`.

## Author and licence

Designed by Sergei Sarkisian for RockWheel, from scratch. The skins, images
and glyphs are licensed under Creative Commons Attribution-ShareAlike 3.0, or,
at your choice, under the GNU General Public License, version 2 or later, as
the other themes' are; the script is code, under the GPL. The fonts are under
the SIL Open Font License 1.1: see `../README.md`.
