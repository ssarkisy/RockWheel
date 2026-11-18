#!/bin/bash
# Installs or updates RockWheel on an iPod Video (5th or 5.5th generation) or an
# iPod photo from a Mac.
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
# Each model's files in a release, and the bootloader this installer writes on
# it, by its SHA-256 (the app's table, core/src/bootloaders.rs, has the same).
# The Video: Rockbox's own bootloader, the one Rockbox Utility installs.
VIDEO_FILES="rockbox.zip bootloader-ipodvideo.ipod rockbox-games.zip"
VIDEO_BOOTLOADER_SHA256=19dfa0e930689f5afdeaae18f4c56b472cbed9c6c3a7039bb32b646d8040298f
# The photo: RockWheel's rw-bl2. Rockbox's v4.0 for it can't read a fragmented
# rockbox.ipod, so an iPod photo with v4.0 gets rw-bl2 in its place.
PHOTO_FILES="rockbox-ipodcolor.zip bootloader-ipodcolor.ipod rockbox-games-ipodcolor.zip"
PHOTO_BOOTLOADER_SHA256=ff9f7be5a7c85832443ef188e8655de82249c84bf9c1f1202f9bdc8b01f4e1cc
# What a bootloader on the photo is, by the SHA-256 of its bytes on the iPod:
# v4.0 is replaced; RockWheel's own and any other are kept
PHOTO_REPLACED=5c47d85d9d0346fcbb17ac2847ca5d97c1ef0cea055b6d1fd9778135be8aba6f

DRY_RUN=0
UNINSTALL=0
FOLDERS=0
GAMES=""
FROM=""
WORK=""
DISK=""
# read_directory's error after a write (add_bootloader)
DIR_FAIL=""

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

# Run as /bin/bash -c "$(curl ...)" --games, bash takes the first word after the
# script for its $0: an option there is the first of the options
case "$0" in
    -*) set -- "$0" "$@" ;;
esac

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)   DRY_RUN=1 ;;
        --uninstall) UNINSTALL=1 ;;
        --folders)   FOLDERS=1 ;;
        --games)     GAMES=yes ;;
        --no-games)  GAMES=no ;;
        --from)      shift; FROM="${1:-}"; [ -n "$FROM" ] || die "--from needs a folder" ;;
        -h|--help)   if [ -f "$0" ]; then sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'
                     else say "Options: --dry-run --folders --games --no-games --from DIR --uninstall"; fi
                     exit 0 ;;
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
        die "This installer is for macOS and Windows. The manual steps are in docs/advanced.md." ;;
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

# The checksum file `sums` has a line for `name`
listed() {  # <sums file> <name>
    awk -v n="$2" '$2 == n || $2 == "*" n { f = 1 } END { exit !f }' "$FILES/$1"
}

# The release's file into the files' folder: 0; 2 when the release hasn't it
# (404); 1 when the download itself failed
fetch() {  # <name>
    local code
    code=$(curl -sSL -o "$FILES/$1" -w '%{http_code}' "$RELEASE_URL/$1") && [ "$code" = 200 ] && return 0
    rm -f "${FILES:?}/${1:?}"
    [ "$code" = 404 ] && return 2
    return 1
}

# The release's (or --from's) file `name` is listed in the checksum file
# `sums`, and has that checksum
check_sum() {  # <sums file> <name>
    local want
    want=$(awk -v n="$2" '$2 == n || $2 == "*" n { print $1; exit }' "$FILES/$1")
    [ -n "$want" ] && [ "$(shasum -a 256 "$FILES/$2" | cut -d' ' -f1)" = "$want" ]
}

# ipodpatcher first, which reads the iPod's model: the rest of the files are
# that model's (fetch_model)
fetch_patcher() {
    step "Getting the files"
    if [ -n "$FROM" ]; then
        WORK=""
        FILES="$FROM"
    else
        WORK=$(mktemp -d "${TMPDIR:-/tmp}/rockwheel.XXXXXX") || die "Couldn't make a temporary folder."
        FILES="$WORK"
        local f
        for f in ipodpatcher SHA256SUMS; do
            say "    $f"
            curl -fsSL -o "$FILES/$f" "$RELEASE_URL/$f" || die "Couldn't download $f from $RELEASE_URL"
        done
        check_sum SHA256SUMS ipodpatcher || die "The download is damaged: its checksums don't match."
    fi
    [ -f "$FILES/ipodpatcher" ] || die "ipodpatcher is missing from $FILES"
    chmod +x "$FILES/ipodpatcher"
    xattr -d com.apple.quarantine "$FILES/ipodpatcher" 2>/dev/null
    PATCHER="$FILES/ipodpatcher"
}

# The model's firmware, bootloader and games, each by its line in the
# checksum files: a release lists both models' files, and only this model's
# are downloaded. The games have a checksum file of their own, as SHA256SUMS
# has the files a script of 1.2 checks; a release without them is only asked
# nothing about them.
fetch_model() {
    local f
    # shellcheck disable=SC2086 # the three names, split on purpose
    set -- $MODEL_FILES
    FIRMWARE=$1 BOOTLOADER=$2 GAMES_ZIP=$3
    if [ -z "$FROM" ]; then
        step "Getting RockWheel for the $MODEL_NAME"
        for f in "$FIRMWARE" "$BOOTLOADER"; do
            listed SHA256SUMS "$f" \
                || die "$f isn't in this release's SHA256SUMS: this release may have no RockWheel for the $MODEL_NAME."
            say "    $f"
            fetch "$f"
            case $? in
                0) ;;
                2) die "Couldn't download $f from $RELEASE_URL: this release may have no RockWheel for the $MODEL_NAME." ;;
                *) die "Couldn't download $f from $RELEASE_URL" ;;
            esac
            check_sum SHA256SUMS "$f" || die "The download is damaged: its checksums don't match."
        done
        if curl -fsSL -o "$FILES/$GAMES_ZIP" "$RELEASE_URL/$GAMES_ZIP" 2>/dev/null \
            && curl -fsSL -o "$FILES/SHA256SUMS-games" "$RELEASE_URL/SHA256SUMS-games" 2>/dev/null; then
            say "    $GAMES_ZIP"
            check_sum SHA256SUMS-games "$GAMES_ZIP" || die "The download is damaged: its checksums don't match."
        else
            rm -f "${FILES:?}/${GAMES_ZIP:?}" "${FILES:?}/SHA256SUMS-games"
        fi
    fi
    for f in "$FIRMWARE" "$BOOTLOADER"; do
        [ -f "$FILES/$f" ] || die "$f is missing from $FILES"
    done
    [ "$(shasum -a 256 "$FILES/$BOOTLOADER" | cut -d' ' -f1)" = "$BOOTLOADER_SHA256" ] \
        || die "$BOOTLOADER isn't the bootloader this installer expects."
    # the firmware's own word on the player it is for
    unzip -p "$FILES/$FIRMWARE" .rockbox/rockbox-info.txt 2>/dev/null | grep -q "^Target: $TARGET\$" \
        || die "$FIRMWARE isn't RockWheel for the $MODEL_NAME."
}

read_ipod() {
    INFO=$(sudo "$PATCHER" "/dev/$DISK" --list 2>&1)
    MODEL=$(printf '%s\n' "$INFO" | sed -n 's/.*iPod model: \(.*\) ("[a-z]*pod").*/\1/p' | head -1)
    KIND=$(printf '%s\n' "$INFO" | sed -n 's/.*iPod model: .*("\([a-z]*pod\)").*/\1/p' | head -1)
    [ -n "$KIND" ] || { printf '%s\n' "$INFO" | tail -5 >&2; die "ipodpatcher doesn't recognise /dev/$DISK as an iPod."; }
    HAS_BOOTLOADER=0
    case "$INFO" in *"Third-party bootloader"*) HAS_BOOTLOADER=1 ;; esac
}

# What went wrong, as ipodpatcher said it: its [ERR] lines, or else its last ones
patcher_said() {  # <its output>
    printf '\n' >&2
    { printf '%s\n' "$1" | grep '^\[ERR\]' || printf '%s\n' "$1" | grep -v '^[[:space:]]*$' | tail -5; } \
        | sed 's/^/    /' >&2
}

# What ipodpatcher's model is to this installer: its files, its target, its name
choose_model() {
    case "$MODEL" in
        *Video*)       MODEL_NAME="iPod Video" TARGET=ipodvideo MODEL_FILES=$VIDEO_FILES
                       BOOTLOADER_SHA256=$VIDEO_BOOTLOADER_SHA256 BOOT_NAME="the Rockbox bootloader" ;;
        *Photo/Color*) MODEL_NAME="iPod photo" TARGET=ipodcolor MODEL_FILES=$PHOTO_FILES
                       BOOTLOADER_SHA256=$PHOTO_BOOTLOADER_SHA256 BOOT_NAME="RockWheel's bootloader" ;;
        *) die "This isn't an iPod Video or an iPod photo, the models RockWheel supports." ;;
    esac
}

# Sectors of the iPod's disk, read as root, only read: <first byte> <bytes>,
# the first on a sector boundary; to stdout
read_disk() {
    sudo dd if="/dev/r$DISK" bs="$SECTOR" skip=$(( $1 / SECTOR )) count=$(( ($2 + SECTOR - 1) / SECTOR )) 2>/dev/null
}

# Little-endian and big-endian numbers in a file: <file> <offset>
# (od ends with an empty line, which NF leaves out)
le32() { od -An -tu1 -j "$2" -N4 "$1" | awk 'NF { print $1 + 256 * ($2 + 256 * ($3 + 256 * $4)); exit }'; }
be32() { od -An -tu1 -j "$2" -N4 "$1" | awk 'NF { print $4 + 256 * ($3 + 256 * ($2 + 256 * $1)); exit }'; }
# <file> <offset> <count>: those bytes as text, NULs left out (bash would warn)
bytes_at() { dd if="$1" bs=1 skip="$2" count="$3" 2>/dev/null | tr -d '\0'; }

# read_directory's stop: its own words, or DIR_FAIL's after a write
dir_die() { die "${DIR_FAIL:-$1}"; }

# The firmware partition's directory, as ipodpatcher reads it: its main
# firmware's version (Apple's, in its low byte) and the bootloader in it, by
# the SHA-256 of its bytes. Sets OSOS_VERS, and BOOT_SHA256 (empty: none).
read_directory() {
    local t fw=0 n i e dir x dev len entry at
    SECTOR=$(disk_value DeviceBlockSize "$DISK")
    [ -n "$SECTOR" ] || dir_die "Couldn't read the iPod's sector size."
    t=$(mktemp -d "${TMPDIR:-/tmp}/rockwheel-dir.XXXXXX") || dir_die "Couldn't make a temporary folder."
    read_disk 0 $((64 * SECTOR)) > "$t/head"
    if [ "$(od -An -tx1 -j 510 -N2 "$t/head" 2>/dev/null | tr -d ' ')" = 55aa ]; then
        fw=$(le32 "$t/head" $((0x1be + 8)))
    elif [ "$(bytes_at "$t/head" 0 2)" = ER ]; then
        # the Apple partition map's firmware partition (Apple_MDFW)
        n=$(be32 "$t/head" $((SECTOR + 4)))
        for ((i = 1; i <= n && i < 64; i++)); do
            e=$((i * SECTOR))
            if [ "$(bytes_at "$t/head" $((e + 48)) 11)" = Apple_MDFW ]; then
                fw=$(be32 "$t/head" $((e + 8)))
                break
            fi
        done
    fi
    if [ "${fw:-0}" -le 0 ]; then
        rm -rf "${t:?}"
        dir_die "Couldn't find the iPod's firmware partition."
    fi
    fw=$((fw * SECTOR))
    read_disk "$fw" "$SECTOR" > "$t/fw"
    dir=$(( $(le32 "$t/fw" $((0x104))) + 0x200 ))
    x=$((dir % SECTOR))
    read_disk $((fw + dir - x)) $((2 * SECTOR)) > "$t/dir"
    OSOS_VERS="" BOOT_SHA256=""
    for ((i = 0; i < 10; i++)); do
        e=$((x + 40 * i))
        [ "$(bytes_at "$t/dir" "$e" 4)" = '!ATA' ] || break
        [ "$(bytes_at "$t/dir" $((e + 4)) 4)" = soso ] || continue
        dev=$(le32 "$t/dir" $((e + 12)))
        len=$(le32 "$t/dir" $((e + 16)))
        entry=$(le32 "$t/dir" $((e + 24)))
        OSOS_VERS=$(le32 "$t/dir" $((e + 32)))
        if [ "$entry" -gt 0 ] && [ "$len" -gt "$entry" ]; then
            # an image's data is a sector past the partition's start
            at=$((fw + SECTOR + dev + entry))
            read_disk $((at / SECTOR * SECTOR)) $((at % SECTOR + len - entry)) \
                | tail -c +$((at % SECTOR + 1)) | head -c $((len - entry)) > "$t/boot"
            # after a write, bytes that couldn't be read are a read that failed
            if [ -n "$DIR_FAIL" ] && [ "$(wc -c < "$t/boot")" -ne $((len - entry)) ]; then
                rm -rf "${t:?}"
                die "$DIR_FAIL"
            fi
            BOOT_SHA256=$(shasum -a 256 < "$t/boot" | cut -d' ' -f1)
        fi
        break
    done
    rm -rf "${t:?}"
    [ -n "$OSOS_VERS" ] || dir_die "Couldn't read the iPod's firmware directory."
}

# RockWheel's bootloader needs Apple's last firmware for the iPod: the Video's
# 1.3, the photo's 1.2.1 (on its 1.0 the bootloader hung on Apple's logo); in
# the main firmware's version, the low byte 0x12 and 0x13. Checked before a
# bootloader is written, not before an update of the files.
check_apple_firmware() {
    local low=$((OSOS_VERS & 0xff)) need has
    case "$TARGET" in
        ipodvideo) need=$((0x12)) has="1.3" ;;
        ipodcolor) need=$((0x13)) has="1.2.1" ;;
    esac
    [ "$low" -ge "$need" ] && return
    case "$TARGET/$low" in
        ipodcolor/0) low="1.0" ;;
        *) low=$(printf '0x%04x' "$OSOS_VERS") ;;
    esac
    die "This iPod has Apple's software $low, and RockWheel needs $has. Select the iPod in Finder and click Update, not Restore: Update keeps your music. Then run this again. Nothing was changed."
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

# Clusters of 32 KB, the first FAT and the clusters on 4 MiB boundaries (each FAT is
# a whole number of 2 MiB, so the second one starts on 2 MiB): flash cards write much
# faster like this, and a hard drive doesn't mind.
format_data() {
    local part="${DISK}s2" figures="${DISK}s2" block off size per4m reserved clusters fat fatunit cluster
    # a dry run on a Mac-formatted iPod sees its Apple map still: its HFS+
    # partition, s3, is the one the new table makes s2
    [ "$DRY_RUN" = 1 ] && [ "$KIND" = macpod ] && figures="${DISK}s3"
    block=$(disk_value DeviceBlockSize "$figures")
    off=$(disk_value PartitionMapPartitionOffset "$figures")
    size=$(disk_value TotalSize "$figures")
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
    local out
    step "Installing the bootloader"
    would "add $BOOT_NAME to the firmware partition, keeping the stock firmware" && return
    diskutil unmountDisk "/dev/$DISK" >/dev/null 2>&1
    # ipodpatcher answers 0 also when it couldn't write it
    out=$(sudo "$PATCHER" "/dev/$DISK" --add-bootloader "$FILES/$BOOTLOADER" 2>&1) \
        || { patcher_said "$out"; die "Installing the bootloader failed."; }
    case "$out" in *"--add-bootloader failed"*) patcher_said "$out"; die "Installing the bootloader failed." ;; esac
    sudo "$PATCHER" "/dev/$DISK" --list 2>&1 | grep -q "Third-party bootloader" \
        || { patcher_said "$out"; die "The bootloader didn't end up on the iPod."; }
    # the one written, by its bytes on the iPod (the file less its 8-byte header)
    DIR_FAIL="The bootloader is on the iPod, but it couldn't be read back to check it. RockWheel's files aren't on the iPod yet: run the installer again to finish."
    read_directory
    DIR_FAIL=""
    [ "$BOOT_SHA256" = "$(tail -c +9 "$FILES/$BOOTLOADER" | shasum -a 256 | cut -d' ' -f1)" ] \
        || die "The bootloader on the iPod isn't the one written."
}

# Stops before anything is changed if the archive's files don't fit: cut
# short, a copy leaves the new firmware with some of the old one's fonts and
# languages. What the archive replaces on the iPod gives its room back, so an
# update needs far less than a first install; a plain Rockbox folder that is
# moved aside gives none. 40 MB are kept spare for the file system's clusters
# and the ._ files unzip writes on a Mac until they are removed.
check_room() {
    local zip="$1" mount="$2" moving="$3" need free have=0
    need=$(unzip -l "$zip" | tail -1 | awk '{ print int($1 / 1024) + 1 }')
    if [ "$moving" != 1 ]; then
        have=$(unzip -Z1 "$zip" | grep -v '/$' | while IFS= read -r f; do
                   [ -f "$mount/$f" ] && stat -f %z "$mount/$f"
               done | awk '{ s += $1 } END { print int(s / 1024) }')
    fi
    free=$(df -k "$mount" | tail -1 | awk '{ print $4 }')
    need=$((need - have + 40960))
    if [ "$free" -lt "$need" ]; then
        die "The iPod needs $(( (need - free) / 1024 + 1 )) MB more free space for this update. Free some up (remove a few albums in Finder) and run the installer again. Nothing has been changed on the iPod."
    fi
}

# Before the bootloader goes on and before an old folder is moved aside, so
# that a refusal leaves the iPod as it was
ensure_room() {
    local part="${DISK}s2" mount="" moving=0
    [ "$DRY_RUN" = 1 ] && return
    diskutil mount "$part" >/dev/null 2>&1
    mount=$(disk_value MountPoint "$part")
    [ -n "$mount" ] && [ -d "$mount" ] || die "Couldn't mount the iPod's music partition."
    if [ -d "$mount/.rockbox" ] && [ ! -f "$mount/.rockbox/wps/iClassic-Square/About Backdrop.bmp" ]; then
        moving=1
    fi
    check_room "$FILES/$FIRMWARE" "$mount" "$moving"
}

copy_firmware() {
    local part="${DISK}s2" mount=""
    step "Copying the firmware"
    if would "copy the files of $FIRMWARE to the iPod"; then
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
    unzip -oq "$FILES/$FIRMWARE" -d "$mount" || die "Copying failed. Is the iPod full?"
    find "$mount/.rockbox" -name '._*' -delete 2>/dev/null
    sync
}

# Rockbox's games for Extras > Games, in an archive of their own: asked about
# while they aren't on the iPod, then kept up to date with the firmware. A no is
# kept in a file on the iPod, so that an update doesn't ask again.
copy_games() {
    local part="${DISK}s2" mount files count size have=0 declined
    if [ ! -f "$FILES/$GAMES_ZIP" ]; then    # a release or --from folder without them
        [ -n "$GAMES" ] && say "" && say "    There are no games with this release, so they were left as they are."
        return 0
    fi
    files=$(unzip -Z1 "$FILES/$GAMES_ZIP" | grep -v '/$')
    count=$(printf '%s\n' "$files" | grep -c '\.rock$')
    size=$(unzip -l "$FILES/$GAMES_ZIP" | tail -1 | awk '{ printf "%.1f", $1 / 1048576 }')
    mount=$(disk_value MountPoint "$part")
    declined="$mount/.rockbox/no-extra-games"
    if [ -n "$mount" ]; then
        while IFS= read -r f; do
            case "$f" in *.rock) [ -f "$mount/$f" ] && have=1 ;; esac
        done <<< "$files"
    fi

    if [ "$GAMES" = "no" ]; then
        step "Taking the extra games off the iPod"
        would "remove the $count games of Extras > Games that came with $GAMES_ZIP" && return
        [ -n "$mount" ] || die "Couldn't find the iPod's music partition."
        while IFS= read -r f; do rm -f "$mount/$f"; done <<< "$files"
        : > "$declined"
        sync
        return
    fi
    if [ "$GAMES" != "yes" ] && [ "$have" = 0 ]; then
        say ""
        if [ -n "$mount" ] && [ -f "$declined" ]; then
            say "    The extra games are left out, as you chose before. To add them, run"
            say "    this again with --games at the end."
            return
        fi
        if [ "$DRY_RUN" = 1 ]; then
            say "    Would ask whether to add $count games from Rockbox to Extras > Games."
            return
        fi
        ask "Add $count games from Rockbox to Extras > Games? About $size MB. [y/N]"
        case "$REPLY" in
            [yY]|[yY][eE][sS]) ;;
            *)  [ -n "$mount" ] && : > "$declined"
                say "    Not added, and not asked again. To add them later, run this again"
                say "    with --games at the end."
                return ;;
        esac
    fi
    step "Copying the games"
    would "copy the files of $GAMES_ZIP to the iPod" && return
    [ -n "$mount" ] || die "Couldn't find the iPod's music partition."
    unzip -oq "$FILES/$GAMES_ZIP" -d "$mount" || die "Copying the games failed. Is the iPod full?"
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

step "RockWheel for iPod Video and iPod photo"
[ "$DRY_RUN" = 1 ] && say "This is a dry run: nothing on the iPod is changed."
say "People run it on iPods with flash cards, SD card adapters and the original hard"
say "drive. Details are in the advanced guide, docs/advanced.md."

if [ "$FOLDERS" = 1 ]; then
    step "Looking for the iPod"
    find_ipod
    put_folders
    exit 0
fi

fetch_patcher

step "Looking for the iPod"
find_ipod
say "Found /dev/$DISK."
say "macOS will ask for your password, to read and write the iPod's disk directly."
sudo -v || die "No permission to reach the disk."
read_ipod
say "Model: $MODEL, $KIND."
choose_model

if [ "$UNINSTALL" = 1 ]; then
    [ "$HAS_BOOTLOADER" = 1 ] || die "No bootloader is installed on this iPod."
    step "Removing the bootloader"
    ask "The iPod will start its stock firmware again. Type yes to go on:"
    [ "$REPLY" = "yes" ] || die "Stopped. Nothing was changed."
    if ! would "remove the bootloader"; then
        diskutil unmountDisk "/dev/$DISK" >/dev/null 2>&1
        SAID=$(sudo "$PATCHER" "/dev/$DISK" --delete-bootloader 2>&1) \
            || { patcher_said "$SAID"; die "Removing the bootloader failed."; }
        # ipodpatcher answers 0 also when it couldn't remove it
        INFO=$(sudo "$PATCHER" "/dev/$DISK" --list 2>&1) || { patcher_said "$INFO"; die "Removing the bootloader failed."; }
        case "$INFO" in *"Third-party bootloader"*) patcher_said "$SAID"; die "Removing the bootloader failed." ;; esac
    fi
    eject_ipod
    say "Done. The .rockbox folder is still on the iPod and can be deleted from Finder."
    exit 0
fi

fetch_model

# The bootloader to write: none there yet, or (the photo) Rockbox's v4.0 in place
# of RockWheel's. Apple's last firmware is checked first, before anything changes:
# a Mac-formatted iPod's before it is erased.
read_directory
WRITE_BOOTLOADER=0
if [ "$HAS_BOOTLOADER" = 0 ]; then
    WRITE_BOOTLOADER=1
elif [ "$TARGET" = ipodcolor ] && [ "$BOOT_SHA256" = "$PHOTO_REPLACED" ]; then
    WRITE_BOOTLOADER=1
fi
[ "$WRITE_BOOTLOADER" = 1 ] && check_apple_firmware

FORMAT_NEEDED=0
FRESH=0
if [ "$KIND" = "macpod" ]; then
    say ""
    say "This iPod is Mac-formatted, and RockWheel needs FAT32."
    say "CONVERTING ERASES EVERYTHING ON THE IPOD: music, photos and anything else you"
    say "keep on it. When RockWheel is installed, a sync from Finder brings back what's"
    say "in your Music library; plays and ratings not yet synced to the Mac are lost, so"
    say "sync once first."
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
ensure_room

if [ "$WRITE_BOOTLOADER" = 0 ]; then
    say ""
    say "The bootloader is already installed, so this only updates the firmware files."
    say "Settings, the library and play counts on the iPod are kept."
else
    if [ "$HAS_BOOTLOADER" = 1 ]; then
        say ""
        say "Rockbox's bootloader v4.0 is on this iPod photo: RockWheel's takes its place."
    fi
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
if [ "$FRESH" = 1 ] || [ "$WRITE_BOOTLOADER" = 1 ]; then
    say "1. Unplug the iPod. If it doesn't restart by itself, hold Menu and Select until it does."
    say "2. Connect it again. It appears in Finder as an iPod: sync your music from there."
    say "   Don't choose Restore in Finder. That erases the iPod and removes RockWheel."
    say "3. Unplug it. The library comes in while the progress bar runs."
    say ""
    if [ "$TARGET" = ipodcolor ]; then
        say "To start the stock firmware instead, turn the Hold switch on while it boots."
    else
        say "To start the stock firmware instead, for video, turn the Hold switch on while it boots."
    fi
else
    say "Unplug the iPod. If it doesn't restart by itself, hold Menu and Select until it does."
fi
