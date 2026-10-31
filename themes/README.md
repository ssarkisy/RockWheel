# iPod theme

The theme for the iPod Video (320x240), kept here in the layout it is installed
in (`<theme>/.rockbox/...`). The menus don't offer another one.

It's licensed under Creative Commons Attribution-Share Alike 3.0
(http://creativecommons.org/licenses/by-sa/3.0/), and so are the changes made
to it here.

`fonts/` holds the fonts it uses: Inter (https://rsms.me/inter/), converted with
`tools/make_fonts.sh`, under the SIL Open Font License 1.1 (`fonts/OFL.txt`). The
fonts it came with had no Cyrillic. Letters Inter doesn't have come from GNU Unifont,
which Rockbox also carries in the top-level `fonts/` (`16-GNU-Unifont.fnt`).

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
- `list text padding: 6` in the .cfg: list text keeps clear of the left
  edge, as on a stock iPod.
- The statusbar title is the title of the list ("iPod" in the main menu)
  rather than "Playlist" for every screen but the main menu.
- iClassic: no "iClassic v1.1" in the right panel of the menus when nothing
  is playing.

The extra bitmaps are drawn by `tools/make_assets.py`.

To try them in the simulator, copy them over the simulator disk:

    rsync -a themes/*/.rockbox/ build-sim/simdisk/.rockbox/

The ring on the boot screen and on the panel beside the main menu is drawn by
`tools/make_wheel_mark.py`, and the panel made from it by `tools/make_about_panel.py`.
They are this project's own: no maker's logo is in this repository.
