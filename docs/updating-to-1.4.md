# Updating to 1.4

What someone who uses RockWheel 1.3 or 1.3.1 should know before and after updating.

## How to update

- Run the same command as for installing: `install.sh` on a Mac, `install.ps1` on Windows. Your settings, play counts, ratings and resume places are kept.
- If you install by hand, extract the whole of `rockbox.zip` over the old files, and `rockbox-games.zip` too if you have the games. Games and plugins from 1.3.1 don't open with 1.4.
- You don't have to sync first: plays and ratings that haven't reached your computer yet are carried over and offered at the next connection. Syncing first does no harm.

## Size and free space

- 1.4 is much larger than 1.3.1. `rockbox.zip` is about 63 MB, and on the iPod it takes about 145 MB; 1.3.1 was about 17 MB and 37 MB.
- Nearly all of it is fonts. Each of the three themes has its own, and every one holds every alphabet, so that a title in Korean, Japanese, Chinese, Hebrew, Arabic or Thai shows in any theme without anything being prepared on the iPod.
- The iPod needs about 180 MB free for the update. The installer checks this before it copies anything, and stops if there is less.
- If you install by hand and the copy stops because the iPod is full, free some space and extract the whole archive again before you restart. A copy cut short can leave the new firmware beside old fonts and old menu texts.
- Old font files that are no longer used are removed from the iPod at the first start.

## The first start

The first start after the update shows Updating Library once, and takes longer than usual. During it the iPod:

- rebuilds the library in a new format, sorted as in Music on your Mac;
- gives every song back its play count, rating, play time, last played date and its place in an audiobook or podcast;
- reads the podcast and audiobook details, release dates and dates added from the last sync;
- prepares the small covers for lists, the orders for Songs, and Search;
- adds the 240-pixel cover format to the iPod's description file, so that Finder prepares sharp covers for the main menu from the next sync on (see "Sharp covers" below).

It is safe to interrupt: if the iPod is connected or loses power during it, the update carries on at the next start. Nothing is lost, and nothing is counted twice.

**If the iPod is nearly full**, it says "Not Enough Space" and waits for a button. 1.4 can't read the library 1.3.1 made, so until there is room to build the new one, the library may be empty or incomplete: Music may show no songs, or only some of them. Nothing is lost. Your music, play counts, ratings and places are all still on the iPod. Free some space — remove an album or two in Finder, for instance — and the update finishes by itself at the next start with room, with everything in place.

With nothing playing, the cover of the song Resume Playback will start with now shows beside the main menu. Right after the update it appears once you have played or resumed something.

## Sharp covers

The cover beside the main menu is sharp in 1.4 on every iPod. Where it comes from:

- **Music you sync from now on** gets a cover Finder prepares at exactly the size the main menu shows. It is sharp the moment the song starts. At its first start 1.4 adds that size to the list of album-art formats in the iPod's description file, `iPod_Control/Device/SysInfoExtended`, by copying the iPod's own 320×240 photo format into it. The original file is kept beside it as `SysInfoExtended.original`.
- **Music synced before** keeps its old covers, which have no frame of that size. The iPod draws the sharp cover from the picture inside the song's file a few seconds after the song starts, and remembers the last 48.
- **Rebuild Synced Covers**, at the bottom of Settings, has Finder make every cover again, with the sharp ones:
  1. It shows the space the new covers need and the space free, and asks. Music stops if it was playing.
  2. It sets the iPod's covers aside. Until the next sync, synced songs show no covers.
  3. Sync with Finder. Finder writes every cover again, which takes a while: about 12 minutes for 13,000 songs on the test iPod.
  4. Unplug. During Updating Library the old covers are removed.

  Changed your mind before syncing? Choose Rebuild Synced Covers again, and it offers to put the old covers back.

What it costs: the new covers take about 250 KB a song, 3.3 GB for 13,000 songs, and while the old set waits for the sync, both are on the iPod.

What has been tried: one iPod, a 5.5G, with Finder on macOS 26. iTunes on Windows and iOpenPod haven't been tried with the changed description file or with Rebuild Synced Covers.

## What looks different

- **Music's lists hold music only.** Podcasts and audiobooks are no longer among Songs, Albums and Artists, in Cover Flow, or in Search's songs; they are in Music > Podcasts and Music > Audiobooks. The number of songs under Music in the main menu is smaller by as many.
- **Artists lists album artists.** A guest on one song no longer has a row of their own, and a compilation album stays together. To have Artists as in 1.3, turn off Settings > Music Menu > Group by Album Artist.
- **Names are in a different order**: as in Music on a Mac, with a leading The, A or An set aside, so The Killers is under K.
- **Albums has two lines and covers**, and albums with the same name by different artists are separate rows. Settings > Appearance > Artist under Album and Covers in Lists turn these off.
- **Lists have a scrollbar** at the right. Settings > Appearance > Scrollbar turns it off.
- **Fast scrolling shows a large letter, and then moves a letter at a time.** If you'd rather keep scrolling row by row, turn off Settings > Appearance > Jump by Letter.
- **The iPod comes up in the Classic theme**, the look it had, light or dark as you left it. Modern and Neo are in Settings > Appearance > Theme.
- **Classic's main menu has the time in its title**, where the iPod's name was. The name is beside the menu when Settings is selected.
- **Now Playing is bold.** In the Classic theme, Now Playing, Cover Flow's captions and album headers are in bold for everyone, as they were with Bold Text on.
- **Cover Flow is in a new order**: by album artist, each artist's albums in the order they came out. Holding Previous or Next now runs to the first or last album instead of stepping one album after another.
- **Songs without tags** are under Unknown Artist, Unknown Album and Unknown Genre, at the end of their lists, instead of "[Untagged]".
- **Stars that aren't set are outlines** in Classic's Now Playing, instead of gray stars.
- **Resume Playback starts at once**, without "Loading…". After you change a setting, open Cover Flow, Photos or a game, or a file on the iPod changes, it loads its list again with the message, once.
- **"Preparing Photos" no longer shows after every sync**, only when your photos have changed.

## Settings that are gone or renamed

| In 1.3.1 | In 1.4 |
|---|---|
| Settings > Appearance > Bold Text | Removed. Each theme has its own weights; Classic is bold where Bold Text made it bold in Now Playing, Cover Flow's captions and album headers. |
| Shuffle: Off or On | Shuffle: Off, Songs or Albums. Your setting carries over: On becomes Songs. |
| Shuffle Songs, first row of a playlist and of On-The-Go | Removed. Set Shuffle to Songs, in Settings or in the menu you get by holding Select in Now Playing, and play the playlist. |
| Crossfeed | Headphone Crossfeed. Your setting is kept. |
| Audiobooks: Slower, Normal, Faster | Audiobook Speed: 0.75×, Normal, 1.25×, and now 1.5×, 1.75× and 2×. Your setting is kept. Podcast Speed is new, beside it. |
| Current Playlist, in Now Playing's menu | Up Next, always shown. |
| Settings > Appearance > Status Bar, Rating, Progress Bar (iPod or iOS style) | Removed, with the iOS-style icons, stars and bar. Classic has the iPod look; if you had the iOS one, try the Modern theme. |
| — | Settings > Appearance > Theme: Classic, Modern or Neo, each light or dark by Dark Theme. Changing the theme stops the music; Resume Playback takes it up where it stopped. |
| — | Settings > Appearance > Color, in Neo: thirteen colors for the whole theme. |
| Cover Colors: Off, Text and Stars, Whole Screen, Fill | The same four in Classic, as before, and in Neo, where they act in Now Playing. Modern has no Cover Colors and no Angled Cover. Your setting is kept. |
| — | Settings > Rebuild Synced Covers, an action: see "Sharp covers". |

## Shuffle works differently

- **One setting decides.** In 1.3 a list could be shuffled by its Shuffle Songs row while the setting said Off. Now what plays always follows the Shuffle setting, and the icon in Now Playing shows it.
- **Shuffle Songs in the main menu turns the setting on.** It plays all your music in a random order, as before, and now leaves Shuffle on Songs. An album you start afterwards is shuffled too, until you set Shuffle to Off.
- **Albums** is new: whatever you play goes an album at a time, each in its own order, the albums at random. Shuffle Albums can be added to the main menu in Settings > Main Menu.
- **Podcasts and audiobooks are never shuffled**, and stay out of Shuffle Songs.
- Resume Playback brings back the same shuffled order, after a sync as well.

## If you changed the theme's files yourself

- **A custom copy of Now Playing's skin** (the `.wps` file) made from 1.3.1 will not load in 1.4: the way a skin names its fonts changed with Bold Text's removal. The iPod shows a plain built-in Now Playing instead. Start again from 1.4's skin and reapply your changes.
- A custom skin also lacks what 1.4's skin gained: the podcast and audiobook lines, the chapter line, the Speed view, the 15- and 30-second signs. Without them those views are simply absent.
- `bold text` in a `config.cfg` you keep or share is ignored, without a message. So are the lines of settings that no longer exist.
- Your own Korean font, under the name the theme used for it, is still used for the syllables it has. The Library fonts and the `fonts/scripts` folder of 1.3.1 are removed at the first start.
- The iOS-style pictures of 1.3.1's theme are gone with the three style settings; a custom theme that refers to them will miss them.
- A menu language file from 1.3.1 isn't loaded by 1.4: the iPod shows English until 1.4's own language files are in place. Copying the whole `rockbox.zip` puts them there.
- Themes other than RockWheel's own are not supported.

## Going back to 1.3.1

- Sync with Finder or iTunes first. 1.3.1 can't read 1.4's library: it rebuilds its own, and the plays and ratings of songs copied onto the iPod outside Finder are lost. Synced songs keep what has reached your computer.
- Install `rockbox.zip` of 1.3.1 over 1.4, and its games.
- 1.3.1 reads the new speed values as Normal, and resumes a queue shuffled by albums as a shuffle of songs, at the same song.
- The changed description file stays, and Finder goes on preparing the 240-pixel covers. To put the original file back, replace `SysInfoExtended` with `SysInfoExtended.original` in `iPod_Control/Device`.

## Podcasts and play counts

- If you sync podcasts with Finder on macOS 26, your song plays and ratings did not reach your Mac under 1.3.1 — nor under Apple's firmware — and could be lost on the iPod at the next sync. 1.4 gets them through, and keeps any that a sync doesn't take until the next connection.
- Plays lost before the update can't be brought back by it.
- Your place in an episode and its played mark stay on the iPod: Podcasts on the Mac (macOS 26) doesn't take them.
