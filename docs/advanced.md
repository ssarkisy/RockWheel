# RockWheel: the advanced guide

This page has everything the [README](../README.md) leaves out: installing from Terminal or PowerShell, installing by hand, the files in a release, how covers, sync and Last.fm work in detail, the exact limits, diagnostics, performance and building from source. To install RockWheel the simple way, use the app as the README describes.

- [Tested setups](#tested-setups)
- [Install from Terminal on a Mac](#install-from-terminal-on-a-mac)
- [Install from PowerShell on Windows](#install-from-powershell-on-windows)
- [Script options](#script-options)
- [Manual installation](#manual-installation)
- [Updating](#updating)
- [Removing RockWheel](#removing-rockwheel)
- [Release files and bootloaders](#release-files-and-bootloaders)
- [The app in detail](#the-app-in-detail)
- [Music in detail](#music-in-detail)
- [Last.fm in detail](#lastfm-in-detail)
- [Limits](#limits)
- [Diagnostics and troubleshooting](#diagnostics-and-troubleshooting)
- [Performance](#performance)
- [Build from source](#build-from-source)

## Tested setups

**iPod Video.** I test on one: a 5.5th generation 30 GB model with 32 MB of RAM, an iFlash card of about 256 GB, about 13,000 songs and macOS 26. All performance figures here come from that iPod. The 60 and 80 GB models have 64 MB of RAM. RockWheel adapts to the larger memory as it starts, but no 64 MB iPod has run it here yet.

A user has reported running it on a 5.5G 30 GB with its original hard drive, installed with the script. RockWheel was tuned on flash storage, so on a hard drive expect:

- slower Cover Flow and album art loading, especially when the drive has to spin up;
- shorter battery life, because reading artwork keeps the drive awake;
- longer library updates after syncing.

If you use it on a hard drive, please open an issue and describe how it runs.

**iPod Photo (Beta).** I've tested one: a 30 GB iPod Photo (M9829) on Apple's software 1.2.1, synced with Finder on macOS 26, first with its original hard drive and since 2.1 with a 256 GB SD card in an adapter in its place. The app also recognizes the 20, 40 and 60 GB iPod Photo and iPod color, and the color U2 and Harry Potter editions, but none of them has been tried. iPod Photos were made with several kinds of screen. On the kind in the test iPod, and one other, RockWheel writes each change so that it never shows in pieces. Only the test iPod's kind is switched off while the iPod sleeps: the others stay on, dark. RockWheel hasn't run on the other kinds of screen yet. The iPod Photo with iTunes on Windows hasn't been tried either.

**The scripts.** Both scripts changed in 2.0 for the iPod Photo, and the changes also run when you update an iPod Video. They were checked with the real release files and stand-ins for the iPod, but not yet on a real iPod.

**The app.** On a Mac (macOS 26), it has:

- installed RockWheel on an iPod Photo from scratch: after a Restore in Finder, it converted the photo to FAT32, wrote the bootloader and installed, and RockWheel started;
- updated an iPod Video, both from a download and from a folder;
- made covers for copied songs on both iPods;
- sent plays to Last.fm.

On Windows, it has installed itself, found and checked an iPod Video, updated it from a download and from a folder, brought playlist folders over from iTunes and ejected the iPod. A first install with the bootloader, Last.fm sending and making covers haven't run on Windows yet.

## Install from Terminal on a Mac

Connect the iPod, open Terminal and paste this:

```sh
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/ssarkisy/RockWheel/main/install.sh)"
```

The script works on the iPod Video and the iPod Photo:

1. It finds the iPod and reads its model, then downloads and verifies the files for that model.
2. It checks Apple's software on the iPod: 1.3 on the Video, 1.2.1 on the photo.
3. If the iPod is Mac-formatted, it converts it to FAT32, but only after you type `ERASE`.
4. It checks the free space and installs the bootloader: Rockbox's own on the Video, RockWheel's on the photo.
5. It copies the firmware and ejects the iPod.

On an iPod Photo with Rockbox's bootloader v4.0, RockWheel's bootloader takes its place. Any other bootloader stays, and only the firmware files are updated. macOS asks for your password, because reading the iPod's model and installing the bootloader need direct disk access. The script's iPod Photo support is new and hasn't run on a real iPod Photo yet; the app's has.

The script also asks whether to add eight extra games to Extras. Press Return to skip them. Your choice is saved for future updates. To add the games later, run the same command with `--games` at the end.

Apple's latest software is checked only before a bootloader is written. Updating the files of an iPod that already starts RockWheel doesn't need it.

## Install from PowerShell on Windows

The PowerShell script installs on the iPod Video and the iPod Photo. Its iPod Photo support is new and hasn't run on Windows or a real iPod Photo yet.

Connect the iPod, open PowerShell without administrator privileges and paste this:

```powershell
irm https://raw.githubusercontent.com/ssarkisy/RockWheel/main/install.ps1 | iex
```

The script downloads and verifies the files, finds the iPod and checks its free space. It installs the bootloader if needed, copies the firmware, asks about the extra games and ejects the iPod. It doesn't convert Mac-formatted iPods: restore those in iTunes on Windows first. On an iPod Photo with Rockbox's bootloader v4.0, RockWheel's bootloader takes its place. Any other bootloader stays, and only the firmware files are updated. Before it writes a bootloader, the script checks for Apple's latest software: 1.3 on the Video, 1.2.1 on the photo. If the iPod has an older version, update it in iTunes with Update, not Restore, then run the script again.

Windows asks for administrator permission to access the iPod's disk directly. On a first install, it asks again to install the bootloader. On an iPod Photo with Rockbox's bootloader v4.0, it asks once more, to read that bootloader before replacing it.

If an installation fails, please open an issue with the script's output, or with the output of a dry run (see [script options](#script-options)).

## Script options

Add options at the end of the install command. On a Mac:

```sh
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/ssarkisy/RockWheel/main/install.sh)" --games
```

On Windows, `irm … | iex` can't take options. Use this form instead:

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/ssarkisy/RockWheel/main/install.ps1))) -Games
```

You can also download a script and run it locally: [`install.sh`](../install.sh) as `bash install.sh --dry-run`, or [`install.ps1`](../install.ps1) as `powershell -ExecutionPolicy Bypass -File .\install.ps1 -DryRun`.

| Option | Mac | Windows |
|---|---|---|
| Preview the installation without making changes | `--dry-run` | `-DryRun` |
| Import playlist folders for the next sync | `--folders` | `-Folders` |
| Add or remove the extra games | `--games`, `--no-games` | `-Games`, `-NoGames` |
| Take the files from a folder instead of downloading them | `--from DIR` | `-From DIR` |
| Choose an iPod when more than one is connected | | `-Drive E:` |
| Remove the bootloader | `--uninstall` | `-Uninstall` |

Playlist folders are imported from Music on a Mac or iTunes on Windows. On the Mac, you're asked to let Terminal read them. On Windows, this needs iTunes from Apple's website: the Microsoft Store version doesn't support this kind of import.

## Manual installation

The app and the scripts do these steps for you. Use `--dry-run` to preview them without making changes.

### On a Mac

Build `ipodpatcher` from `utils/ipodpatcher` in this repository. Let it detect the iPod rather than guessing a disk number: it only detects iPods, not your Mac's internal disks.

1. **Update Apple's software** to 1.3 on the Video or 1.2.1 on the photo, with Update in Finder, not Restore. On older versions the iPod can hang at its logo once the bootloader is in place.
2. **Check the format.** `sudo ipodpatcher --scan` shows the iPod's disk, such as `/dev/disk4`, and reports `macpod` if it's Mac-formatted. To convert it, run `sudo ipodpatcher /dev/disk4 --convert`, with the disk shown by the scan. This erases the iPod. For iPods with 2048-byte sectors, which the official `ipodpatcher` refuses, the tool prints a command to format the music partition. Run that command as printed: it includes the correct disk.
3. **Install the bootloader.** Download `bootloader-ipodvideo.ipod` (for the iPod Photo, `bootloader-ipodcolor.ipod`) from [Releases](https://github.com/ssarkisy/RockWheel/releases). Run `sudo ipodpatcher --add-bootloader bootloader-ipodvideo.ipod`, with the file you downloaded. Without a disk argument, it detects the iPod automatically and stops if it finds none or more than one. On the iPod Video, the official [Rockbox Utility](https://www.rockbox.org/wiki/RockboxUtility), run as root, works too. On the iPod Photo, use RockWheel's bootloader: Rockbox's can't start RockWheel's firmware file when the file is stored in pieces on the disk.
4. **Copy the firmware.** Download `rockbox.zip` (for the iPod Photo, `rockbox-ipodcolor.zip`) from [Releases](https://github.com/ssarkisy/RockWheel/releases) and unzip it at the root of the iPod. Allow about 160 MB of free space on the Video, or about 100 MB on the photo. If extraction stops because the disk is full, free some space and unzip it again. For the extra games, unzip `rockbox-games.zip` (for the photo, `rockbox-games-ipodcolor.zip`) there too. If you previously used plain Rockbox, move its old `.rockbox` folder out of the way first.
5. **Restart and sync.** Eject the iPod, restart it with Menu and Select, reconnect it and sync from Finder. The library updates after you unplug it.

### On Windows (iPod Video)

Users have installed RockWheel this way. If these steps don't work for you, please open an issue.

1. **Update Apple's software** to 1.3 with Update in iTunes, not Restore.
2. **Check the format.** An iPod set up by iTunes on Windows is already FAT32. To convert a Mac-formatted iPod, restore it in iTunes on Windows. This erases everything on it.
3. **Install the Rockbox bootloader.** Run the official [Rockbox Utility](https://www.rockbox.org/wiki/RockboxUtility) as administrator. Choose the iPod Video and install only the bootloader. Skip this step if the iPod already starts Rockbox.
4. **Copy the firmware.** Download `rockbox.zip` from [Releases](https://github.com/ssarkisy/RockWheel/releases) and extract it to the iPod's drive, with `.rockbox` at the root. Allow about 160 MB of free space. If extraction stops because the disk is full, free some space and try again. Extract `rockbox-games.zip` the same way if you want the extra games. If you previously used plain Rockbox, rename its `.rockbox` folder first.
5. **Restart and sync.** Eject and unplug the iPod, then hold Menu and Select until it restarts. The first start reads your library. After that, sync from iTunes as usual.

## Updating

**With the app:** connect the iPod and click Update. **With a script:** run the install command again. Either way your settings, library, play counts and games are kept. A script asks for your password or administrator permission. If you haven't chosen whether to install the extra games before, it asks once and saves your choice.

**By hand:** extract the firmware zip for your iPod onto it, replacing the existing files. If you installed the extra games, extract the games zip too. Update both each time: games from an older version don't run with newer firmware. Saved games and high scores are kept.

**From 2.1.0:** see "Before you update" in the [2.1.1 release notes](https://github.com/ssarkisy/RockWheel/releases/tag/v2.1.1).

**From 2.0 or 2.0.1:** see "Before you update" in the [2.1 release notes](https://github.com/ssarkisy/RockWheel/releases/tag/v2.1.0).

**From 1.4.1:** see "Before you update" in the [2.0 release notes](https://github.com/ssarkisy/RockWheel/releases/tag/v2.0.0).

**From 1.3 or earlier:** the first start rebuilds the library, keeping play counts, ratings and playback positions. See [Updating to 1.4](updating-to-1.4.md) for the other changes.

## Removing RockWheel

The app doesn't remove RockWheel. Use a script:

- **Mac, either iPod:** run the install command with `--uninstall` at the end.
- **Windows, either iPod:** run the second form under [script options](#script-options), with `-Uninstall`.

This removes the bootloader, so the iPod starts its stock firmware every time. Your library is kept. You can then delete the remaining `.rockbox` folder in Finder or File Explorer.

Removing RockWheel from an iPod Photo with either script hasn't been tried on a real iPod Photo yet.

To start the stock firmware just once, turn the Hold switch on while the iPod starts.

## Release files and bootloaders


| File | What it is |
|---|---|
| RockWheel app for Mac (`.dmg`) and Windows (setup `.exe`) | The installer app |
| `rockbox.zip` | The firmware for the iPod Video |
| `rockbox-ipodcolor.zip` | The firmware for the iPod Photo |
| `rockbox-games.zip`, `rockbox-games-ipodcolor.zip` | The eight extra games, for each iPod |
| `bootloader-ipodvideo.ipod` | Rockbox's bootloader for the iPod Video, unchanged since 1.2.0 |
| `bootloader-ipodcolor.ipod` | RockWheel's bootloader for the iPod Photo |
| `ipodpatcher` (macOS), `ipodpatcher.exe` (Windows) | Used by the scripts |
| `SHA256SUMS`, `SHA256SUMS-games`, `SHA256SUMS-windows` | Checksums |

**The bootloader** is the small program the iPod starts first. It starts RockWheel, or Apple's firmware when Hold is on. It sits in the iPod's firmware partition beside Apple's software, which stays installed.

- **iPod Video:** Rockbox's own bootloader v4.0, the one Rockbox Utility installs. An iPod that already has a bootloader keeps it.
- **iPod Photo:** RockWheel's own. Rockbox's v4.0 for the photo can't start `rockbox.ipod` when the file is stored in pieces on the disk, so the app and the scripts replace v4.0 with it. Any other bootloader is kept. Right after Apple's disk mode, the photo's disk sometimes fails its first read, so RockWheel's bootloader tries up to five times, for at most 60 seconds, before it shows an error. Hold ⏭ while it starts to see what it's doing. It loads `.rockbox/rockbox.ipod`, or `rockbox.ipod` at the root if that's missing.

Apple's latest software (1.3 on the Video, 1.2.1 on the photo) is required before a bootloader is first written. On older versions the iPod can hang at its logo: the photo did on Apple's 1.0.

## The app in detail

**What it reads.** The iPod's model, its name, the version of RockWheel or Rockbox on it, its format, and which bootloader it starts, identified by its checksum. On a Mac it reads the disk through macOS's `authopen` after asking for your password. It needs Full Disk Access only if macOS refuses that, and then the window says how to allow it.

**What it writes.**

- **The firmware files.** Each file goes under a temporary name and is then renamed, with `rockbox.ipod` last. The app then reads the copy back from the iPod, past the computer's cache, and rewrites any file that doesn't match. If a copy is cut short, the iPod keeps starting its previous version, and the app offers Finish the Update the next time.
- **The bootloader, on a first install.** First it saves a backup on your computer of every byte it will change. It checks that it's still the same iPod, writes, reads the result back, and puts the old bytes back on a mismatch. On the iPod Photo, writing the bootloader moves Apple's update image along, as `ipodpatcher` does, and the moved bytes are backed up too. If the cable comes out mid-write, reconnect the iPod and press Continue: the app finishes or undoes the change.
- **The conversion, on a Mac.** For a Mac-formatted iPod, after you type ERASE, the app writes the partition table that `ipodpatcher --convert` writes. It then formats a FAT32 volume exactly as macOS's own `newfs_msdos` would with the script's settings. Apple's firmware partition is never written. If writing the table fails, the old one is put back and the iPod stays as it was.

**Network.** When the window opens, it asks GitHub for the latest RockWheel release and keeps the answer for an hour. That request carries nothing that identifies you, your computer or your iPod. An update downloads that release's files by their exact tag and checks them against the release's checksums.

**Install from a Folder…** installs from a release folder or a build folder with `rockbox.zip` in it, without downloading.

**Permissions on a Mac.**

- Your password, to reach the iPod's disk.
- Removable Volumes, to read the iPod's files.
- Automation for Music, the first time you bring playlist folders over.
- The app has to be in Applications before Last.fm can be turned on: the Last.fm helper starts at login from where the app is. The app offers to move itself there.

**Permissions on Windows.** The setup installs for all users, which needs administrator permission. The app asks again each time it reaches the iPod's disk. On Windows 10 the setup downloads Microsoft's WebView2 if it's missing, so it needs an internet connection then.

The app's source is in [RockWheel-Installer](https://github.com/ssarkisy/RockWheel-Installer), whose README is for developers.

## Music in detail

### Synced and copied music

| | Synced music | Copied music |
|---|---|---|
| How to add it | Sync from Finder, iTunes or [iOpenPod](https://github.com/TheRealSavi/iOpenPod) | Copy files into any folder outside `iPod_Control`, then unplug |
| Library preparation | Your computer prepares track details and covers, and the iPod imports them | The iPod reads tags and prepares album art after you unplug |
| Update time | Fast | Longer, depending on the number of new files |
| Where it appears | Artists, Albums, Compilations, Search, Cover Flow and Shuffle Songs | The same library as synced music; files in Audiobooks or Podcasts folders go to those sections |
| Play counts and ratings | Sync back to your computer | Stay on the iPod |
| Formats | What your sync app supports | Also FLAC and the other formats Rockbox plays |

Deleted files leave the library after the next connection and unplug. Moved or renamed files keep their play counts and ratings. Copied music is included in smart playlists too.

**Finder and play counts.** Finder on macOS 26 normally ignores play counts and ratings when podcasts are synced to an iPod, even one running Apple's firmware. RockWheel works around this. If a sync doesn't import the counts and ratings, they stay on the iPod for the next connection and aren't counted twice.

**Changed song details.** When a sync changes a song's title, artist, album, genre, composer, comment, album artist, grouping, or track or disc number, Updating Library takes the new values. The song keeps its play count (with plays not yet synced), its rating, its last-played date, its place in a podcast or book, and its date added. Finder writes a retagged song as a new file, which RockWheel takes as a moved song. Tools that change only the iPod's database, such as iOpenPod, are handled too. That path has been tested in the simulator, not yet on an iPod.

### Covers

**Synced music.** The cover can be any size or format that iTunes or Music accepts, PNG included: your computer converts it during the sync. On the iPod Video, choose Rebuild Synced Covers in Settings and then sync, to have Finder make every cover again, including covers synced before 1.4. On the iPod Photo, Finder writes the covers next to the main menu at their full size. Covers Finder wrote before you installed RockWheel don't have that size yet. For those songs, Updating Library makes the picture from the cover inside the song's own file, as long as it's the same picture Finder synced. To have Finder write them all again, choose Rebuild Synced Covers in Settings and then sync.

**Copied music.** Updating Library prepares each song's cover once, in every size the screens use. It takes the picture embedded in the song, or else an image in its folder. Nothing is read from the song's file while you browse or play. A song changed on the iPod shows the note until the next Updating Library. The iPod decodes standard (baseline) JPEG; 400×400 or 600×600 is enough, and larger pictures only take longer. The iPod can't decode PNG or progressive JPEG pictures. Convert them to standard JPEG, or let the app make the covers: open Covers for songs added without syncing and choose Make Covers. The app takes pictures from the songs or from a `cover.jpg` or `folder.jpg` beside them. Updating Library takes each cover the app made only if the song hasn't changed since. An app older than the firmware may make the list covers at another size. The iPod then makes those covers itself from Cover Flow's covers.

**Missing Covers Caution**, under Covers in Lists in Settings › Appearance, is on by default. When a song's picture can't be shown, it says why: a PNG, a progressive JPEG or an unreadable picture. On the iPod Video, Now Playing gives the problem and the fix in the cover's place, and the lists, the main menu's pane and Cover Flow show a "?". On the iPod Photo, Now Playing shows the note and the lists show the "?". Turned off, these songs show the note, as before.

### Lyrics, podcasts, audiobooks and chapters

Live Lyrics uses timed lyrics embedded in the file. For copied music, you can also put an `.lrc` file with the same name beside the song.

Copied books and podcasts work like synced ones. RockWheel recognizes them by the `.m4b` extension, the Audiobook or Podcast genre (as a name or as ID3's genre number), or a folder named Audiobooks or Podcasts.

Podcasts, audiobooks and recordings of ten minutes or longer have chapters, read from MP3, M4A and M4B files. That works for mixes and concerts stored as one file too. Next skips to the next chapter; Previous returns to the start of the current one. Hold Select in Now Playing and choose Chapters for the full list.

### Compilations

A song synced by Finder takes iTunes' "Part of a compilation" setting. A copied song takes its file's own mark:

- `TCMP` or `TCP` in ID3v2;
- `cpil` in MP4;
- `COMPILATION` in Vorbis comments and APE tags.

An album counts as a compilation when any of its music is marked. An album by various artists without an album artist is one row. While Music shows Compilations, Artists leaves out artists whose songs are all on compilations. With Group by Album Artist on, it also leaves out album artists who have only compilations, such as Various Artists. Compilations is on in Settings › Music Menu at first (a stock iPod classic has it off), and its row in Settings › Main Menu is off at first. The section is hidden while the library has no compilations.

### Formats

Now Playing's Format and Bitrate line, in Settings › Appearance, starts with the format's mark: MP3, AAC, AAC+ for HE-AAC, ALAC, FLAC, OPUS and the others. For lossless files it then shows the bit depth and sample rate read from the file: FLAC, ALAC, WAV and Wave64, AIFF, WavPack, APE, TTA and AU. Bitrates of 1000 kbps or more are shown in Mbps. Where a lossless track's line doesn't fit, as on the iPod Photo, its bitrate is left out first.

The iPod decodes nearly every format on its second processor core, leaving the first to the menus and the rest of playback. MP3 has its own thread there, and a few chiptune formats (SPC, KSS and VGM) work as before. A track that fails on the second core before any sound plays on the first instead.

- **HE-AAC** plays at the right speed, without its added high frequencies (SBR), so it sounds duller than its encoder intended.
- **FLAC** with large blocks plays, up to the 16,384 samples the FLAC standard's subset allows. This covers files that ffmpeg encodes at 88.2–192 kHz.
- **Too demanding for the iPod:** 24-bit/96 kHz WavPack in its highest modes, APE at Extra High and some SID tunes need more than the iPod's processor has, and stutter. APE at Normal and 16-bit WavPack play.
- **Known issues:**
  - seeking in Opus lands up to about 1.3 seconds before the point chosen;
  - 5.1-channel Vorbis and WAV files play silently;
  - some WMA Pro files are refused;
  - MOD files don't end by themselves;
  - the elapsed time of some chiptune formats is wrong.

### Sound

**Equalizer presets.** My Presets lists the files in `.rockbox/eqs` on the iPod, in up to four levels of folders and up to 256 presets and folders in all. Files you copy there appear after you unplug the iPod. It reads RockWheel's and Rockbox's `.cfg` presets and the `ParametricEQ.txt` files that AutoEq and Equalizer APO write, so you can copy AutoEq's folder for your headphones as it is. A folder that holds a single preset, as AutoEq makes one for each model, is listed as that preset.

The presets for some headphones are files there too: a folder 7Hz Zero 2 with its four presets, and the Koss KPH30i and Sony WH-1000XM4C presets beside your own. RockWheel writes them once, at the first start after you install or update it. A preset you delete doesn't come back with an update or a settings reset. A file of yours with the same name is never replaced. If the disk is full or can't be written, RockWheel tries again at the next start.

A `ParametricEQ.txt` file can have up to ten filters, each a peak (PK), a low shelf (LSC) or a high shelf (HSC). A file with more filters, another kind of filter or settings for one channel isn't used, as the iPod couldn't play it the way its curve shows. RockWheel ignores the file's Preamp and lowers the level by as much as the curve's highest point needs, as it does for every preset. Presets you save in the editor are written at the top of `.rockbox/eqs` in RockWheel's format.

**Sound Check › Albums** uses the album gain that ReplayGain tags store in the files. A song without one gets its track gain, as with On, so a library whose files have no album gain sounds the same with Albums as with On.

The Sony WH-1000XM4C preset is for a wired connection in Active Mode, with the headphones switched on.

## Last.fm in detail

Settings › Playback › Last.fm is on by default. The iPod then keeps `/.scrobbler.log` at its root, in the Audioscrobbler format that Last.fm uploaders read.

- **What counts.** A song longer than 30 seconds counts once what you actually heard reaches half its length or four minutes. Seeking and pausing add nothing. Repeat One counts each loop. Podcasts and audiobooks are left out.
- **The clock.** A play heard while the iPod's clock isn't set, for example after the battery ran flat, isn't logged: Last.fm would refuse its date. Set the date and time on the iPod.
- **Stopped part-way.** A song you stopped part-way, with its place kept, waits. If you resume it within seven days, it counts as the same play, even across a restart.
- **When it's written.** The log waits in memory. It reaches the disk at USB connection, at power-off, when the iPod goes to sleep, or when the disk is idle anyway after the music stops. It never wakes the disk for this. With the battery at 15% or less, it's written at every idle moment.

**The app's Last.fm helper.** Turn on Send my plays to Last.fm in the app's options and sign in once in your browser. A small helper then runs in the menu bar or the notification area. Whenever a RockWheel iPod is connected, it sends the plays in the log in batches, and keeps a record so that no play is sent twice. Once Last.fm has answered for every play, it empties the log. The log has no time zone, so the helper uses your computer's. Your Last.fm session is kept in the Keychain or the Credential Manager. The helper talks only to Last.fm, and sends each play's artist, title, album, length and time. Last.fm only takes plays from the last two weeks, so connect the iPod at least that often.

You can use any other uploader that reads `.scrobbler.log` instead, or turn Last.fm off on the iPod.

## Limits

- The playback queue holds the whole library, up to 65,000 songs, if the iPod has the memory for it when it starts. Otherwise it holds at most 32,000.
- With more than 65,534 songs, or without the memory for it, the Songs list can only be sorted by name.
- On a 32 MB iPod Video, the library holds about 58,000 songs when it's first built and about 54,000 when a sync adds songs to it. These figures come from an emulator, with names of average length; longer names lower both. If a sync brings more songs than fit, the iPod says so and leaves the new songs out, but your removals, play counts and ratings still go through, and the new songs are tried again at the next sync. Songs you copied without Finder past that point are left out too, and the iPod tries them again at every start.
- Libraries much larger than 25,000 songs haven't been tested on an iPod. In the simulator with 32 MB of RAM (tested with 1.4), the theme may not load if the iPod restarts straight into Updating Library.
- Music purchased from iTunes plays unless it has copy protection. Protected songs and audiobooks appear in the library, but RockWheel reports that it can't play them. Apple Music subscription tracks aren't supported.
- RockWheel doesn't play video. To watch videos on the iPod Video, start the stock firmware, which shares the same library, by turning the Hold switch on while the iPod starts.
- Podcast playback positions and completion don't sync back to Podcasts on the Mac.
- The high-resolution covers that Finder prepares have only been tested with Finder on macOS 26, not with iTunes on Windows or iOpenPod.
- Hindi and Thai song titles are displayed, but their combining marks can be misplaced. Neither language is available for the menus.
- Search can't enter Chinese characters or Japanese kanji. Names written in them appear in the library, but Search can't find them. The letter ring has the alphabets used in your library: Latin, Russian, Greek, Hebrew, Arabic, Thai, Korean and Japanese kana.
- Playlists created on the iPod, On-The-Go included, stay on the iPod. Finder and iTunes don't import them into your computer's library. That includes playlists you copy into `/Playlists`.

## Diagnostics and troubleshooting

**Files to attach to an issue,** all in `.rockbox` on the iPod:

- `health.log` records errors and when they happened. It also records how long the processor ran at each speed, and which formats played on which core. It's written at USB connection and at power-off. It has no song names and nothing that identifies your iPod.
- `drive.txt` describes the drive without its serial number.
- `crash.log`: after a crash or a freeze, the next start writes what happened there, with RockWheel's version. The crash screen and the freeze screen ask you to send this file. Older records move to `crash.old.log` once the file reaches 16 KB.

**The app** has Copy Report on every error screen, and Check iPod shows what it found. Neither contains the serial number, the computer's name or your home folder's paths.

**The iPod doesn't appear.** On the stock firmware, it has to be set for disk use in Finder or iTunes before the app can see it.

**The iPod doesn't start.** Its disk mode still works. Hold Menu and Select until it restarts, then hold Select and Play. In disk mode, the app can finish or undo an interrupted change, or you can restore the iPod in Finder or iTunes.

**A conversion that stopped half-way.** If the app says the disk has neither format whole, restore the iPod in Finder. That gives it Apple's software and the Mac format again. Then run the app again.

**macOS doesn't let the app reach the iPod.** Allow it in System Settings › Privacy & Security › Files and Folders › Removable Volumes. If the app asks for Full Disk Access, turn it on in System Settings › Privacy & Security › Full Disk Access. You can turn it off again afterwards.

**Settings files.** A damaged or hand-edited `config.cfg` with a number out of range is brought back into range as it's read.

## Performance

[Performance measurements](performance.md) compares RockWheel with Rockbox, with earlier RockWheel builds and with the stock firmware, and describes the changes behind the numbers. Those figures are for RockWheel 1.4 on the test iPod Video, with about 13,000 songs and 32 MB of RAM. There, Songs opened in 0.21 seconds, and album art appeared 0.07–0.25 seconds after a track change.

Measured on the test iPod Video for 2.0:

- **Sleep.** Awake, idle and with the screen dark, the iPod draws 21.9 mA. Asleep, it draws 11.8 mA, as the audio chip is put in standby and the processor slows to 24 MHz. 1.4.1 had no sleep: it turned off. The iPod Photo's sleep hasn't been measured.
- **Updating Library.** After three songs were added, the Songs step took 2.54 seconds instead of 2.83.

No user-facing speed figures are claimed for the move of decoding to the second core, or for the iPod Photo.

## Build from source

Build the Rockbox ARM toolchain with `tools/rockboxdev.sh --target=a`, then build the firmware:

```sh
mkdir build && cd build
../tools/configure --target=ipodvideo --type=n
make -j8 && make zip     # gives rockbox.zip
make gameszip            # gives rockbox-games.zip, the extra games
```

For the iPod Photo, use `--target=ipodcolor`. For a bootloader, use `--type=b`. For the simulator, use `--type=s`, then run `make -j8 && make install && ./rockboxui`. The screenshots in the README were made with the simulator.

`make -C tests check` builds parts of the firmware for the Mac: the sort keys, the iTunesDB and ArtworkDB readers, the codecs, the wheel and others. It checks them under AddressSanitizer and UBSan. `tools/crash_symbols.py` names the addresses in a `crash.log`, using the build its record names.

The app is built from [RockWheel-Installer](https://github.com/ssarkisy/RockWheel-Installer); its README describes how.
