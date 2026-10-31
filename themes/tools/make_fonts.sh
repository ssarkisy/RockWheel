#!/bin/sh
# Make the Inter fonts the iPod themes use, from the static TTFs of an Inter
# release (https://github.com/rsms/inter/releases, extras/ttf/ in the zip):
#
#     themes/tools/make_fonts.sh <folder with Inter-Bold.ttf, Inter-SemiBold.ttf>
#
# Needs a C compiler and FreeType (on macOS: brew install freetype) to build
# Rockbox's tools/convttf. The fonts are anti-aliased and named after their
# line height, like the fonts they replace.
set -e

here=$(cd "$(dirname "$0")" && pwd)
root="$here/../.."
ttf=${1:?usage: $0 <folder with Inter-Bold.ttf and Inter-SemiBold.ttf>}
out="$here/../fonts/.rockbox/fonts"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT

if pkg-config --exists freetype2 2>/dev/null; then
    ft=$(pkg-config --cflags --libs freetype2)
else
    prefix=$(brew --prefix freetype)
    ft="-I$prefix/include/freetype2 -L$prefix/lib -lfreetype"
fi
# shellcheck disable=SC2086
cc -std=c99 -O2 "$root/tools/convttf.c" $ft -lm -o "$build/convttf"

mkdir -p "$out"

# font <name> <ttf> <pixel size> [<more convttf options>]
# Latin with its extensions, Greek, Cyrillic, punctuation and symbols up to
# U+266F, which takes in the stars of Finder's rating playlists and notes;
# digits all as wide, as on a stock iPod (Inter's are proportional), so
# that numbered lists line up and times don't jitter
font() {
    name=$1 file=$2 size=$3
    shift 3
    "$build/convttf" -s 32 -l 9839 -W -p "$size" "$@" \
        -o "$out/$name" "$ttf/$file" > /dev/null
}

# menus and titles: 5 more pixels of line height, 2 of them above the text
# so that it sits in the middle of the selection bar
font 24-Inter-Bold.fnt     Inter-Bold.ttf     18 -r 3 -Ta-2
font 18-Inter-SemiBold.fnt Inter-SemiBold.ttf 17    # iClassic lyrics
font 17-Inter-SemiBold.fnt Inter-SemiBold.ttf 15    # iClassic track info
font 14-Inter-SemiBold.fnt Inter-SemiBold.ttf 13    # small text, lyrics
font 14-Inter-Bold.fnt     Inter-Bold.ttf     13    # iClassic screen titles

ls -l "$out"

# the letters Inter doesn't have, from Rockbox's own font: the basic plane is all
# the firmware can show of it, and without the rest the file is a third of the size
"$here/../../tools/convbdf" -f -l 65535 -o "$out/16-GNU-Unifont.fnt" \
    "$here/../../fonts/16-GNU-Unifont.bdf" > /dev/null
