# iPod themes

Themes for the iPod Video (320x240), kept here in the layout they are
installed in (`<theme>/.rockbox/...`). iClassic Square is the main theme,
iLike is the alternative.

Both are licensed under Creative Commons Attribution-Share Alike 3.0
(http://creativecommons.org/licenses/by-sa/3.0/), and so are the changes
made to them here.

`fonts/` holds the fonts both themes use: Inter (https://rsms.me/inter/),
converted with `tools/make_fonts.sh`, under the SIL Open Font License 1.1
(`fonts/OFL.txt`). The fonts the themes came with had no Cyrillic.

- **iClassic Square** by Humberto Santana, derived from iLike.
  https://themes.rockbox.org/index.php?themeid=1311&target=ipodvideo
- **iLike** by Alan Brito, ported from the 176x132 version by Hayden Pearce.
  https://themes.rockbox.org/index.php?themeid=1260&target=ipodvideo

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
- `list text padding: 6` in the .cfg: list text keeps clear of the left
  edge, as on a stock iPod.
- The statusbar title is the title of the list ("iPod" in the main menu)
  rather than "Playlist" for every screen but the main menu.
- iClassic: no "iClassic v1.1" in the right panel of the menus when nothing
  is playing.

The extra bitmaps are drawn by `tools/make_assets.py`.

To try them in the simulator, copy them over the simulator disk:

    rsync -a themes/*/.rockbox/ build-sim/simdisk/.rockbox/
