# RockWheel's performance

The iPod Video has two ARM7TDMI cores at 80 MHz and 32 or 64 MB of RAM. RockWheel was developed on the 32 MB model, the harder of the two. Making that feel fast took work in nearly every layer: the codecs, the database, the file system, the USB stack and the drawing code.

Everything here was measured on the test iPod: a 5.5G with 32 MB of RAM and an iFlash card of about 256 GB. On a hard drive, anything that reads from the disk takes longer.

[Back to the README](../README.md)

## Compared with Rockbox

The same work done by Rockbox's own code, as plain Rockbox does it.

| | Rockbox | RockWheel |
|---|---:|---:|
| List scrolling while MP3 plays | 11 fps | **20.7 fps** |
| List scrolling while AAC plays | 17 fps | **20.2 fps** |
| Cover Flow | 14–17 fps with 3 covers, about 8 with music playing | **27 fps**, and 17–27 with the music playing on |
| Showing a cover on track change | 2–3 s from a 1000×1000 embedded JPEG | **0.07–0.25 s** |
| Music after pressing Next | 0.65–1.1 s | **0.45–0.6 s** |
| A step of the volume | about 100 ms | **11 ms** |
| Getting a list of 13,180 songs ready to play | 4.5 s | **0.3–0.5 s** |
| Opening Songs, 13,000 songs | 0.85 s, each time | **0.21 s, then at once** |
| Checking every file's path at each start, 13,000 songs | 84 s in the background | **skipped** |
| Removing 50 songs deleted from the iPod | 8.5 s | **0.7 s** |
| A band of the EQ, per sample | 50 cycles | **31 cycles** |
| Switching between light and dark while music plays (1.3) | 8 s | **0.45–0.65 s** |
| Parsing the skins, at every start | 282 ms | **54 ms** |
| Writing over USB | 6.6 MB/s | **8.4–8.7 MB/s** |

## Compared with earlier RockWheel builds

Work that plain Rockbox doesn't do, timed against the RockWheel build the work started from.

| | Earlier RockWheel | RockWheel |
|---|---:|---:|
| Library update after a sync | about 90 s for 200 new songs | **about 17 s for 200–300 new**, in a library of 6,810 |
| Sending your play counts to Finder, at each connection | 1.8 s | **0.18 s** |
| Connecting until mounted on the Mac | about 12 s | **about 9 s** |
| Opening the menu over Now Playing after a pause | 0.78 s | **0.13 s** |
| The library's fonts after a sync, 13,000 songs | 5.2 s | **0.6 s** |
| Finding 2,000 songs copied outside Finder, after USB | 1.14 s | **0.18 s** |

## New in 1.4

Times for what 1.4 adds, on the test iPod with about 13,000 songs.

| | RockWheel 1.4 |
|---|---:|
| Opening Albums, 1,698 albums | **0.08 s** |
| Opening Artists | **0.011 s** |
| Opening Audiobooks | **0.12 s** |
| Opening Songs sorted by artist, album or year | **0.36 s**; by name 0.20 s |
| Starting a song from Songs with Shuffle on Albums, 13,188 songs | **0.70 s**, 0.40 s of it for the shuffle |
| Shuffle Songs from the main menu, 13,188 songs | **about 1.5 s** |
| Reading Resume Playback's list while the iPod starts, 13,188 songs | **0.33 s**, so that Resume Playback starts without "Loading…" |
| The sharp cover beside the main menu, drawn from the song's file | **2.5–3.9 s** after the track starts for ordinary covers; 3.2 s for a 600-pixel cover in FLAC, 13.2 s in Apple Lossless |
| A full library rebuild, now that the fonts come ready | **34 s shorter** |
| A sync that changes only plays, ratings or playlists | **about 1 s shorter**: Cover Flow's albums and the lists' covers are left as they are |
| The firmware file | **about 125 KB smaller** than 1.3.1's, with all of 1.4's features |

## Compared with the stock firmware

| | Stock firmware | RockWheel |
|---|---:|---:|
| Writing over USB | 3.2 MB/s in Apple's disk mode | **8.4–8.7 MB/s** |

## How that was done

- **Cover Flow draws front to back.** Half of each frame is rendered on the second core. With the music playing on, the second core's share follows how much the decoder needs, and evens out within a few frames.
- **Decoding moved to the second core.** MP3 and AAC frames are decoded on the COP into a queue, so the main CPU is free for the UI.
- **Faster codecs.** A new IMDCT path in libmad takes MP3 decoding from 7.8 to 6.7 ms a frame, and libfaad clears and copies less, taking AAC from 11.4 to 10.55 ms.
- **The next cover at the press.** When you skip, Now Playing reads the cover from Finder's artwork itself, along with the title from the database, instead of waiting for playback to open the song. Meanwhile the decoder no longer spends half the processor on the song you're leaving.
- **The whole database in RAM.** Paths are included, and hash tables replace repeated disk searches. It loads under the boot logo, before the menu, so no list waits for it.
- **Lists kept.** Songs, made at the library update, and the list you came from stay in memory while the database is unchanged, so going back to them from Now Playing is instant. Playlists from iTunesDB get an index like Songs at the library update, and smart playlists by ratings, play counts and kind of media are worked out from the database in memory, songs outside Finder included.
- **Reads and writes lined up for DMA.** The disk controller only uses DMA from aligned memory, so Play Counts, playlists, Cover Flow's images and the files of a library update were copied by the processor instead. They now line up, Play Counts goes out in whole blocks, and Sound Check finds a song with one read.
- **A sync with nothing new is recognized.** The library update compares Finder's songs with those of the last update and skips the rest, and removing songs no longer reads every tag file from start to end.
- **Songs outside Finder checked at every connection.** After USB, the iPod checks the files outside iPod_Control through the paths in memory, each looked for from where the last one was found: 0.1 s when nothing changed, 0.18 s for 2,000 files in 40 folders. Songs gone are removed together, in one pass over the database's files. The Trash, Spotlight's folders and macOS's `._` files are passed over.
- **Covers for music Finder didn't sync.** Each album's cover is decoded once, on the library update screen, and kept in the same forms as Finder's: a 200×200 frame for Now Playing and a slide for Cover Flow. Playing a song then reads 80 KB instead of decoding a 1000×1000 JPEG three times. PNG and progressive JPEG covers are left out: they can only be decoded whole, which took 8 s for a progressive JPEG of 1000×1000 on the iPod and 13 s for a PNG, against 1.5 s for a standard JPEG, and would have made the first update of a large library take an hour.
- **Work done ahead of time.** Cover Flow's images, each album's color, the collage of photos, the playlist of all songs and Search's names are prepared while the library updates, so nothing has to be built when you open them.
- **Search in memory.** The library update writes the names of artists, albums and songs already folded for case and accents, and the letters of each alphabet they use. Search reads them once, narrows within what it found as you type, and searches after a pause of 0.3 s, so typing never waits.
- **Fonts made from your library.** At the library update, the iPod takes from Pretendard, Noto Sans JP, SC, TC and Hebrew, Vazirmatn and IBM Plex Sans Thai only the glyphs your tags, playlists and menus use, and makes three small fonts of them in the theme's sizes. They're made again only when new letters arrive.
- **Letters spaced as their typefaces space them.** The fonts are converted with each letter's advance and side bearings rather than at the width of its image, so letters with straight sides no longer touch.
- **The wheel by Apple's stages.** As in Apple's patent US 7,312,785, a turn faster than one a second goes twice as far every 0.3 s, up to eight times, and further in long lists. Steps that arrive while the screen is drawing are kept rather than lost.
- **A cheaper EQ.** A peaking band takes three multiplications instead of four, with the same output to the bit.
- **A new color in about 65 ms, drawing included.** The memory for it is only taken when you choose Whole Screen or Fill.
- **EQ presets keep their level.** The EQ lowers the signal by the peak its bands add up to, so loud songs can't clip. The volume then rises by the same amount. For this, the volume scale stops at 0 dB and leaves the amplifier's last 6 dB to the EQ. Only a preset that boosts more than 6 dB is a little quieter at the very top.
- **Fewer writes to the sound chip.** Each write waits for the chip's bus. A step of the volume used to take twelve writes, and now takes two. After a skip, the decoder also lets the menus run while it refills, unless the music is about to run out.
- **Flash stays powered while the screen is on.** An iFlash card used to be switched off a few seconds after every read, like a hard drive, and took 0.6 s to come back. Now, while the screen is on, it only sleeps and wakes in 35–50 ms. It also starts waking as soon as the screen lights up. An SD card adapter that doesn't say it's flash is recognized by how quickly it reads after power-on: under a second, where a hard drive takes 1.5 s or more. A hard drive works as before.
- **Background work yields to the screen.** A thread behind the UI waits on a flash read for 8 ms at most, then lets the UI run.
- **The cover beside the menus comes from memory.** It's the copy Now Playing already holds, so a menu opened after a pause doesn't wait for the disk.
- **Less reading at startup.** Fonts load in a few large reads rather than one for each letter. Folders stay in a larger disk cache, built under the boot logo, instead of being read again for each file. The albums' colors are read in large blocks.
- **Sound Check from Finder's own values.** They're looked up on disk as a track loads. It only ever lowers a track, since Finder's database doesn't say where its peaks are.
- **Faster theme changes.** A theme's pictures are packaged in the screen's own format and come in one DMA read rather than a row at a time, which took a backdrop from about a second to 14 ms with music playing. The fonts stay loaded, and the album art slots are held, so the audio buffer is never rebuilt.
- **An index on the skin tags.** Looking them up again and again while scanning ahead took 80% of the parser's time.
- **Extras' words load with them.** The games, the clock, the calendar and the stopwatch read their phrases from the language file as they open, so they take no memory otherwise.
- **Journaled library updates.** Cut off at 10 different points in the simulator, an update recovered every time.
- **Device description on demand.** The one Finder asks for is generated from the iPod's own flash the first time it's needed.
- **Finder's database is read once,** as a stream in aligned 64 KB blocks, because it doesn't fit in memory.
- **FAT seek marks.** A jump deep into a 500 MB artwork file doesn't walk the whole chain.
- **Flash-friendly formatting.** The Mac installer places the clusters on 4 MiB boundaries, which raised write speed in Apple's disk mode from 1.3 to 3.2 MB/s on the test card.
- **USB write-behind.** Blocks are written while the next command arrives, on a dedicated thread with 16 slots.
- **Assembly dithering, and a mixer that ramps volume changes across samples.** The ramp also removed fade clicks.
