#!/bin/bash
# Installs or updates RockPod on an iPod Video (5th or 5.5th generation) from a Mac.
#
#   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/ssarkisy/RockPod/main/install.sh)"
#
# Options, after a local download of this script:
#   --dry-run        look at the iPod and say what would be done, changing nothing
#   --from DIR       take the files from DIR instead of downloading them
#   --uninstall      remove the bootloader, so the iPod starts its stock firmware
#
# It changes nothing without asking, unless all it has to do is update the firmware
# files. A Mac-formatted iPod has to be erased, and only after you type ERASE.

set -u

REPO="ssarkisy/RockPod"
RELEASE_URL="https://github.com/$REPO/releases/latest/download"
# Rockbox's own bootloader, the one Rockbox Utility installs
BOOTLOADER=bootloader-ipodvideo.ipod
BOOTLOADER_SHA256=19dfa0e930689f5afdeaae18f4c56b472cbed9c6c3a7039bb32b646d8040298f

DRY_RUN=0
UNINSTALL=0
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
        --from)      shift; FROM="${1:-}"; [ -n "$FROM" ] || die "--from needs a folder" ;;
        -h|--help)   sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)           die "Unknown option: $1" ;;
    esac
    shift
done

[ "$(uname -s)" = "Darwin" ] || die "This installer is for macOS. See the README for the manual steps."

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
        WORK=$(mktemp -d "${TMPDIR:-/tmp}/rockpod.XXXXXX") || die "Couldn't make a temporary folder."
        FILES="$WORK"
        local f
        for f in rockbox.zip ipodpatcher "$BOOTLOADER" SHA256SUMS; do
            say "    $f"
            curl -fsSL -o "$FILES/$f" "$RELEASE_URL/$f" || die "Couldn't download $f from $RELEASE_URL"
        done
        (cd "$FILES" && shasum -a 256 -c SHA256SUMS >/dev/null 2>&1) \
            || die "The download is damaged: its checksums don't match."
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
    local i=0
    while [ -z "$(disk_value DeviceNode "${DISK}s2")" ] && [ $i -lt 20 ]; do sleep 1; i=$((i + 1)); done
    [ -n "$(disk_value DeviceNode "${DISK}s2")" ] \
        || die "The Mac hasn't seen the new partitions. Unplug the iPod, connect it again and run this again."
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

eject_ipod() {
    step "Ejecting"
    would "eject the iPod" && return
    diskutil eject "/dev/$DISK" >/dev/null 2>&1 || say "    Eject it from Finder before you unplug it."
}

step "RockPod for iPod Video"
[ "$DRY_RUN" = 1 ] && say "This is a dry run: nothing on the iPod is changed."
say "It has been tested on a single iPod with a flash card, and never on an original"
say "hard drive. Details are in the README."

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
    *) die "This isn't an iPod Video, which is the only model RockPod supports." ;;
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
    say "This iPod is Mac-formatted, and RockPod needs FAT32."
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
eject_ipod

if [ "$DRY_RUN" = 1 ]; then
    step "Dry run finished. Nothing was changed."
    exit 0
fi

step "Done"
if [ "$FRESH" = 1 ] || [ "$HAS_BOOTLOADER" = 0 ]; then
    say "1. Unplug the iPod. If it doesn't restart by itself, hold Menu and Select until it does."
    say "2. Connect it again. It appears in Finder as an iPod: sync your music from there."
    say "   Don't choose Restore in Finder. That erases the iPod and removes RockPod."
    say "3. Unplug it. The library comes in while the progress bar runs."
    say ""
    say "To start the stock firmware instead, for video, turn the Hold switch on while it boots."
else
    say "Unplug the iPod. It restarts into the new firmware by itself."
fi
