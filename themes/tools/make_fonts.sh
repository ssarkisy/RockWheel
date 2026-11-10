#!/bin/sh
# Make the Inter fonts the iPod themes use, from the static TTFs of an Inter
# release (https://github.com/rsms/inter/releases, extras/ttf/ in the zip):
#
#     themes/tools/make_fonts.sh <folder with Inter-Bold.ttf, Inter-SemiBold.ttf, Inter-Medium.ttf>
#
# Needs a C compiler and FreeType (on macOS: brew install freetype) to build
# Rockbox's tools/convttf, and fontTools (pip install fonttools; PYTHON names
# a python3 with it) for the font of Quick Scroll. The fonts are anti-aliased
# and named after their line height, like the fonts they replace. Run
# make_script_fonts.sh after it, which gives the 24, 18, 17 and 14 px ones the
# other alphabets.
set -e

here=$(cd "$(dirname "$0")" && pwd)
root="$here/../.."
ttf=${1:?usage: $0 <folder with Inter-Bold.ttf, Inter-SemiBold.ttf and Inter-Medium.ttf>}
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
# that numbered lists line up and times don't jitter. The letters spaced as
# Inter spaces them (-A): as wide as their images only, as before 1.3, those
# with straight sides touched, the i and P of iPod run into one.
font() {
    name=$1 file=$2 size=$3
    shift 3
    "$build/convttf" -s 32 -l 9839 -W -A -p "$size" "$@" \
        -o "$out/$name" "$ttf/$file" > /dev/null
}

# menus and titles: 5 more pixels of line height, 2 of them above the text
# so that it sits in the middle of the selection bar
font 24-Inter-Bold.fnt     Inter-Bold.ttf     18 -r 3 -Ta-2
font 18-Inter-SemiBold.fnt Inter-SemiBold.ttf 17    # iClassic lyrics, hints
font 17-Inter-SemiBold.fnt Inter-SemiBold.ttf 15    # iClassic track info
font 17-Inter-Medium.fnt   Inter-Medium.ttf   15    # iClassic album
font 18-Inter-Bold.fnt     Inter-Bold.ttf     17    # the lyrics, hints, Bold Text
font 17-Inter-Bold.fnt     Inter-Bold.ttf     15    # track info, Bold Text
font 14-Inter-SemiBold.fnt Inter-SemiBold.ttf 13    # small text, lyrics
font 14-Inter-Bold.fnt     Inter-Bold.ttf     13    # iClassic screen titles
font 12-Inter-SemiBold.fnt Inter-SemiBold.ttf 11    # iClassic bitrate

# Quick Scroll's letter over a list (apps/gui/bitmap/list.c), 52 px with
# capitals 32 px tall: Inter Bold cut to what it shows first, the capitals of
# Latin, Greek and Cyrillic, the digits of years and "#", as convttf would make
# every glyph of the range in that size. The firmware keeps it whole in memory.
"${PYTHON:-python3}" -m fontTools.subset "$ttf/Inter-Bold.ttf" \
    --unicodes=20,23,30-39,41-5A,DE,391-3A1,3A3-3A9,402,404,406-40B,40E-42F,490 \
    --layout-features='' --output-file="$build/Letters.ttf"
"$build/convttf" -s 32 -l 1168 -W -A -p 52 -o "$out/53-Inter-Letters.fnt" \
    "$build/Letters.ttf" > /dev/null

ls -l "$out"

# the letters Inter doesn't have, from Rockbox's own font: the basic plane is all
# the firmware can show of it, and without the rest the file is a third of the size
"$here/../../tools/convbdf" -f -l 65535 -o "$out/16-GNU-Unifont.fnt" \
    "$here/../../fonts/16-GNU-Unifont.bdf" > /dev/null
