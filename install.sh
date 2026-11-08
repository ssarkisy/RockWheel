#!/bin/bash
# Installs or updates RockWheel on an iPod Video (5th or 5.5th generation) from a Mac.
#
#   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/ssarkisy/RockWheel/main/install.sh)"
#
# Options, after a local download of this script:
#   --dry-run        look at the iPod and say what would be done, changing nothing
#   --from DIR       take the files from DIR instead of downloading them
#   --uninstall      remove the bootloader, so the iPod starts its stock firmware
#   --folders        put your playlist folders from Music on the iPod, for them to
#                    show there after the next sync
#   --games          add Rockbox's games to Extras > Games without asking
#   --no-games       take those games off the iPod, and don't ask about them again
#
# It changes nothing without asking, unless all it has to do is update the firmware
# files, and the games if you added them. A Mac-formatted iPod has to be erased,
# and only after you type ERASE.

set -u

REPO="ssarkisy/RockWheel"
RELEASE_URL="https://github.com/$REPO/releases/latest/download"
# Rockbox's own bootloader, the one Rockbox Utility installs
BOOTLOADER=bootloader-ipodvideo.ipod
BOOTLOADER_SHA256=19dfa0e930689f5afdeaae18f4c56b472cbed9c6c3a7039bb32b646d8040298f

DRY_RUN=0
UNINSTALL=0
FOLDERS=0
GAMES=""
FROM=""
WORK=""
DISK=""

say()  { printf '%s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
die()  { printf '\nError: %s\n' "$*" >&2; exit 1; }

cleanup() { if [ -n "$WORK" ]; then rm -rf "$WORK"; fi; }
trap cleanup EXIT

# Answers come from the terminal, also when this script itself came down a pipe
if ! { exec 3</dev/tty; } 2>/dev/null; then
    exec 3<&0
fi
ask() {
    printf '%s ' "$1"
    REPLY=""
    read -r REPLY <&3 || REPLY=""
}

confirm_erase() {
    ask "Type ERASE to erase this iPod, anything else to stop:"
    [ "$REPLY" = "ERASE" ] || die "Stopped. Nothing was changed."
}

# Whatever writes is skipped in a dry run, and says what it would have done
would() {
    if [ "$DRY_RUN" = 1 ]; then
        say "    Would $*"
        return 0
    fi
    return 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)   DRY_RUN=1 ;;
        --uninstall) UNINSTALL=1 ;;
        --folders)   FOLDERS=1 ;;
        --games)     GAMES=yes ;;
        --no-games)  GAMES=no ;;
        --from)      shift; FROM="${1:-}"; [ -n "$FROM" ] || die "--from needs a folder" ;;
        -h|--help)   sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)           die "Unknown option: $1" ;;
    esac
    shift
done

case "$(uname -s)" in
    Darwin) ;;
    MINGW*|MSYS*|CYGWIN*|Windows*)
        die "This installer is for macOS. On Windows, run this in PowerShell:
    irm https://raw.githubusercontent.com/$REPO/main/install.ps1 | iex" ;;
    *)  if grep -qi microsoft /proc/version 2>/dev/null; then
            die "This installer is for macOS. On Windows, run this in PowerShell, not in WSL:
    irm https://raw.githubusercontent.com/$REPO/main/install.ps1 | iex"
        fi
        die "This installer is for macOS and Windows. See the README for the manual steps." ;;
esac

disk_value() {  # the value of a key in diskutil's description of a disk
    diskutil info -plist "$2" 2>/dev/null | plutil -extract "$1" raw -o - - 2>/dev/null
}

find_ipod() {
    local found="" d name
    while :; do
        found=""
        for d in $(diskutil list external physical 2>/dev/null | sed -n 's#^/dev/\(disk[0-9]*\).*#\1#p'); do
            name=$(disk_value MediaName "$d")
            case "$name" in
                *[iI][pP][oO][dD]*) found="$found $d" ;;
            esac
        done
        set -- $found
        case $# in
            1) DISK=$1; return ;;
            0) say "No iPod found. Connect it with its cable. On the stock firmware it should"
               say "show up in Finder; if it doesn't, enable disk use for it in Finder first."
               ask "Press Return to look again, or type q to stop:"
               [ "$REPLY" = "q" ] && die "Stopped. Nothing was changed." ;;
            *) die "More than one iPod is connected. Leave only the one to install on." ;;
        esac
    done
}

fetch_files() {
    step "Getting the files"
    if [ -n "$FROM" ]; then
        WORK=""
        FILES="$FROM"
    else
        WORK=$(mktemp -d "${TMPDIR:-/tmp}/rockwheel.XXXXXX") || die "Couldn't make a temporary folder."
        FILES="$WORK"
        local f
        for f in rockbox.zip ipodpatcher "$BOOTLOADER" SHA256SUMS; do
            say "    $f"
            curl -fsSL -o "$FILES/$f" "$RELEASE_URL/$f" || die "Couldn't download $f from $RELEASE_URL"
        done
        (cd "$FILES" && shasum -a 256 -c SHA256SUMS >/dev/null 2>&1) \
            || die "The download is damaged: its checksums don't match."
        # The games, with a checksum of their own, as SHA256SUMS has the four files a
        # script of 1.2 checks; a release without them is only asked nothing about them
        if curl -fsSL -o "$FILES/rockbox-games.zip" "$RELEASE_URL/rockbox-games.zip" 2>/dev/null \
            && curl -fsSL -o "$FILES/SHA256SUMS-games" "$RELEASE_URL/SHA256SUMS-games" 2>/dev/null; then
            say "    rockbox-games.zip"
            (cd "$FILES" && shasum -a 256 -c SHA256SUMS-games >/dev/null 2>&1) \
                || die "The download is damaged: its checksums don't match."
        else
            rm -f "$FILES/rockbox-games.zip" "$FILES/SHA256SUMS-games"
        fi
    fi
    for f in rockbox.zip ipodpatcher "$BOOTLOADER"; do
        [ -f "$FILES/$f" ] || die "$f is missing from $FILES"
    done
    [ "$(shasum -a 256 "$FILES/$BOOTLOADER" | cut -d' ' -f1)" = "$BOOTLOADER_SHA256" ] \
        || die "$BOOTLOADER isn't the bootloader this installer expects."
    chmod +x "$FILES/ipodpatcher"
    xattr -d com.apple.quarantine "$FILES/ipodpatcher" 2>/dev/null
    PATCHER="$FILES/ipodpatcher"
}

read_ipod() {
    INFO=$(sudo "$PATCHER" "/dev/$DISK" --list 2>&1)
    MODEL=$(printf '%s\n' "$INFO" | sed -n 's/.*iPod model: \(.*\) ("[a-z]*pod").*/\1/p' | head -1)
    KIND=$(printf '%s\n' "$INFO" | sed -n 's/.*iPod model: .*("\([a-z]*pod\)").*/\1/p' | head -1)
    [ -n "$KIND" ] || { printf '%s\n' "$INFO" | tail -5 >&2; die "ipodpatcher doesn't recognise /dev/$DISK as an iPod."; }
    HAS_BOOTLOADER=0
    case "$INFO" in *"Third-party bootloader"*) HAS_BOOTLOADER=1 ;; esac
}

convert_to_fat32() {
    step "Converting the iPod to FAT32"
    would "write a new partition table to /dev/$DISK" && return
    diskutil unmountDisk "/dev/$DISK" >/dev/null 2>&1 \
        || die "Couldn't unmount the iPod. Close Finder windows and apps using it, then run this again."
    printf 'y\n' | sudo "$PATCHER" "/dev/$DISK" --convert >/dev/null 2>&1 \
        || die "Writing the partition table failed."
    # ipodpatcher answers 0 even when it wrote nothing, and a Mac-formatted
    # iPod has an s2 already, its firmware partition, which formatting would
    # wipe: the Mac has to see the new table, and the iPod be a "winpod"
    local i=0
    while { [ "$(disk_value Content "$DISK")" != "FDisk_partition_scheme" ] \
            || [ -z "$(disk_value DeviceNode "${DISK}s2")" ]; } && [ $i -lt 20 ]; do
        sleep 1; i=$((i + 1))
    done
    [ "$(disk_value Content "$DISK")" = "FDisk_partition_scheme" ] \
        && [ -n "$(disk_value DeviceNode "${DISK}s2")" ] \
        || die "The Mac hasn't seen the new partitions. Unplug the iPod, connect it again and run this again."
    read_ipod
    [ "$KIND" = "winpod" ] \
        || die "Converting failed: the iPod is still Mac-formatted. Nothing was formatted."
}

# Clusters of 32 KB, with both copies of the FAT and the clusters on 4 MiB boundaries:
# flash cards write much faster like this, and a hard drive doesn't mind.
format_data() {
    local part="${DISK}s2" block off size per4m reserved clusters fat fatunit cluster
    block=$(disk_value DeviceBlockSize "$part")
    off=$(disk_value PartitionMapPartitionOffset "$part")
    size=$(disk_value TotalSize "$part")
    if [ -z "$block" ] || [ -z "$off" ] || [ -z "$size" ]; then
        die "Couldn't read the size of /dev/$part."
    fi
    per4m=$((4194304 / block))
    off=$((off / block))
    reserved=$(( (per4m - off % per4m) % per4m ))
    while [ "$reserved" -lt 32 ]; do reserved=$((reserved + per4m)); done
    cluster=$((32768 / block))
    clusters=$((size / 32768 + 2))
    fat=$(( (clusters * 4 + block - 1) / block ))
    fatunit=$((per4m / 2))
    fat=$(( (fat + fatunit - 1) / fatunit * fatunit ))
    step "Formatting the music partition as FAT32"
    would "run: newfs_msdos -F 32 -c $cluster -r $reserved -a $fat -v IPOD /dev/r$part" && return
    diskutil unmountDisk "/dev/$DISK" >/dev/null 2>&1
    sudo newfs_msdos -F 32 -c "$cluster" -r "$reserved" -a "$fat" -v IPOD "/dev/r$part" >/dev/null \
        || die "Formatting failed."
}

add_bootloader() {
    step "Installing the bootloader"
    would "add the Rockbox bootloader to the firmware partition, keeping the stock firmware" && return
    diskutil unmountDisk "/dev/$DISK" >/dev/null 2>&1
    sudo "$PATCHER" "/dev/$DISK" --add-bootloader "$FILES/$BOOTLOADER" >/dev/null 2>&1 \
        || die "Installing the bootloader failed."
    sudo "$PATCHER" "/dev/$DISK" --list 2>&1 | grep -q "Third-party bootloader" \
        || die "The bootloader didn't end up on the iPod."
}

copy_firmware() {
    local part="${DISK}s2" mount=""
    step "Copying the firmware"
    if would "copy the files of rockbox.zip to the iPod"; then
        return
    fi
    diskutil mount "$part" >/dev/null 2>&1
    mount=$(disk_value MountPoint "$part")
    [ -n "$mount" ] && [ -d "$mount" ] || die "Couldn't mount the iPod's music partition."
    # Plain Rockbox's settings and database don't carry over
    if [ -d "$mount/.rockbox" ] && [ ! -f "$mount/.rockbox/wps/iClassic-Square/About Backdrop.bmp" ]; then
        local old
        old=".rockbox-old-$(date +%Y%m%d-%H%M%S)"
        mv "$mount/.rockbox" "$mount/$old" || die "Couldn't move the old .rockbox folder aside."
        say "    Your previous Rockbox folder is now $old on the iPod."
    fi
    unzip -oq "$FILES/rockbox.zip" -d "$mount" || die "Copying failed. Is the iPod full?"
    find "$mount/.rockbox" -name '._*' -delete 2>/dev/null
    sync
}

# Rockbox's games for Extras > Games, in an archive of their own: asked about
# while they aren't on the iPod, then kept up to date with the firmware. A no is
# kept in a file on the iPod, so that an update doesn't ask again.
copy_games() {
    local part="${DISK}s2" mount files count size have=0 declined
    if [ ! -f "$FILES/rockbox-games.zip" ]; then    # a release or --from folder without them
        [ -n "$GAMES" ] && say "" && say "    There are no games with this release, so they were left as they are."
        return 0
    fi
    files=$(unzip -Z1 "$FILES/rockbox-games.zip" | grep -v '/$')
    count=$(printf '%s\n' "$files" | grep -c '\.rock$')
    size=$(unzip -l "$FILES/rockbox-games.zip" | tail -1 | awk '{ printf "%.1f", $1 / 1048576 }')
    mount=$(disk_value MountPoint "$part")
    declined="$mount/.rockbox/no-extra-games"
    if [ -n "$mount" ]; then
        while IFS= read -r f; do
            case "$f" in *.rock) [ -f "$mount/$f" ] && have=1 ;; esac
        done <<< "$files"
    fi

    if [ "$GAMES" = "no" ]; then
        step "Taking the extra games off the iPod"
        would "remove the $count games of Extras > Games that came with rockbox-games.zip" && return
        [ -n "$mount" ] || die "Couldn't find the iPod's music partition."
        while IFS= read -r f; do rm -f "$mount/$f"; done <<< "$files"
        : > "$declined"
        sync
        return
    fi
    if [ "$GAMES" != "yes" ] && [ "$have" = 0 ]; then
        [ -n "$mount" ] && [ -f "$declined" ] && return
        say ""
        if [ "$DRY_RUN" = 1 ]; then
            say "    Would ask whether to add $count games from Rockbox to Extras > Games."
            return
        fi
        ask "Add $count games from Rockbox to Extras > Games? About $size MB. [y/N]"
        case "$REPLY" in
            [yY]|[yY][eE][sS]) ;;
            *)  [ -n "$mount" ] && : > "$declined"
                say "    Not added. Run this with --games to add them later."
                return ;;
        esac
    fi
    step "Copying the games"
    would "copy the files of rockbox-games.zip to the iPod" && return
    [ -n "$mount" ] || die "Couldn't find the iPod's music partition."
    unzip -oq "$FILES/rockbox-games.zip" -d "$mount" || die "Copying the games failed. Is the iPod full?"
    find "$mount/.rockbox" -name '._*' -delete 2>/dev/null
    rm -f "$declined"
    sync
}

# Finder leaves playlist folders out of what it syncs to an iPod Video. Music knows
# them, by the same persistent IDs the iPod's database has for the playlists: a
# line for each folder and for each playlist in one, which RockWheel reads at the
# import after a sync.
read_folders() {
    osascript <<'APPLESCRIPT'
tell application "Music"
    set out to ""
    repeat with f in (every folder playlist)
        set par to "-"
        try
            set par to persistent ID of (parent of f)
        end try
        set out to out & "folder " & (persistent ID of f) & " " & par & " " & (name of f) & linefeed
    end repeat
    repeat with p in (every user playlist)
        try
            set par to persistent ID of (parent of p)
            set out to out & "playlist " & (persistent ID of p) & " " & par & linefeed
        end try
    end repeat
    return out
end tell
APPLESCRIPT
}

put_folders() {
    local part="${DISK}s2" mount lines count
    step "Reading your playlist folders from Music"
    lines=$(read_folders) || die "Couldn't read the playlists from Music. Allow the Terminal to control Music if macOS asks."
    count=$(printf '%s\n' "$lines" | grep -c '^folder ')
    say "    $count folders."
    mount=$(disk_value MountPoint "$part")
    [ -n "$mount" ] && [ -d "$mount/.rockbox" ] || die "The iPod's music partition isn't mounted, or RockWheel isn't on it."
    would "write them to .rockbox/playlist_folders.txt on the iPod" && return
    # whole or not at all: written beside it, then put in its place
    printf '# Playlist folders from Music, written by install.sh --folders\n%s\n' "$lines" \
        > "$mount/.rockbox/playlist_folders.tmp" \
        && mv -f "$mount/.rockbox/playlist_folders.tmp" "$mount/.rockbox/playlist_folders.txt" \
        || { rm -f "$mount/.rockbox/playlist_folders.tmp"; die "Couldn't write to the iPod."; }
    rm -f "$mount/.rockbox/._playlist_folders.txt" "$mount/.rockbox/._playlist_folders.tmp"
    sync
    say "    They show on the iPod after your next sync."
}

eject_ipod() {
    step "Ejecting"
    would "eject the iPod" && return
    diskutil eject "/dev/$DISK" >/dev/null 2>&1 || say "    Eject it from Finder before you unplug it."
}

step "RockWheel for iPod Video"
[ "$DRY_RUN" = 1 ] && say "This is a dry run: nothing on the iPod is changed."
say "People run it on iPods with flash cards, SD card adapters and the original hard"
say "drive. Details are in the README."

if [ "$FOLDERS" = 1 ]; then
    step "Looking for the iPod"
    find_ipod
    put_folders
    exit 0
fi

fetch_files

step "Looking for the iPod"
find_ipod
say "Found /dev/$DISK."
say "macOS will ask for your password, to read and write the iPod's disk directly."
sudo -v || die "No permission to reach the disk."
read_ipod
say "Model: $MODEL, $KIND."
case "$MODEL" in
    *Video*) ;;
    *) die "This isn't an iPod Video, which is the only model RockWheel supports." ;;
esac

if [ "$UNINSTALL" = 1 ]; then
    [ "$HAS_BOOTLOADER" = 1 ] || die "No bootloader is installed on this iPod."
    step "Removing the bootloader"
    ask "The iPod will start its stock firmware again. Type yes to go on:"
    [ "$REPLY" = "yes" ] || die "Stopped. Nothing was changed."
    if ! would "remove the bootloader"; then
        diskutil unmountDisk "/dev/$DISK" >/dev/null 2>&1
        sudo "$PATCHER" "/dev/$DISK" --delete-bootloader >/dev/null 2>&1 || die "Removing the bootloader failed."
    fi
    eject_ipod
    say "Done. The .rockbox folder is still on the iPod and can be deleted from Finder."
    exit 0
fi

FORMAT_NEEDED=0
FRESH=0
if [ "$KIND" = "macpod" ]; then
    say ""
    say "This iPod is Mac-formatted, and RockWheel needs FAT32."
    say "CONVERTING ERASES EVERYTHING ON THE IPOD: music, photos and anything else you"
    say "keep on it. What's in your Music library comes back with a sync from Finder;"
    say "plays and ratings not yet synced to the Mac are lost, so sync once first."
    [ "$DRY_RUN" = 1 ] || confirm_erase
    convert_to_fat32
    FORMAT_NEEDED=1
    FRESH=1
else
    # mounted, it has a file system, and FAT32 is what a "winpod" has
    case "$(disk_value FilesystemType "${DISK}s2")/$(disk_value Content "${DISK}s2")/$(disk_value MountPoint "${DISK}s2")" in
        msdos*|MSDOS*|*/*[fF][aA][tT]*|*/*/?*) ;;
        *)  say ""
            say "The iPod's music partition has no FAT32 file system."
            say "FORMATTING ERASES EVERYTHING IN IT."
            [ "$DRY_RUN" = 1 ] || confirm_erase
            FORMAT_NEEDED=1
            FRESH=1 ;;
    esac
fi

[ "$FORMAT_NEEDED" = 1 ] && format_data

if [ "$HAS_BOOTLOADER" = 1 ]; then
    say ""
    say "The bootloader is already installed, so this only updates the firmware files."
    say "Settings, the library and play counts on the iPod are kept."
else
    add_bootloader
fi
copy_firmware
copy_games
eject_ipod

if [ "$DRY_RUN" = 1 ]; then
    step "Dry run finished. Nothing was changed."
    exit 0
fi

step "Done"
if [ "$FRESH" = 1 ] || [ "$HAS_BOOTLOADER" = 0 ]; then
    say "1. Unplug the iPod. If it doesn't restart by itself, hold Menu and Select until it does."
    say "2. Connect it again. It appears in Finder as an iPod: sync your music from there."
    say "   Don't choose Restore in Finder. That erases the iPod and removes RockWheel."
    say "3. Unplug it. The library comes in while the progress bar runs."
    say ""
    say "To start the stock firmware instead, for video, turn the Hold switch on while it boots."
else
    say "Unplug the iPod. It restarts into the new firmware by itself."
fi
