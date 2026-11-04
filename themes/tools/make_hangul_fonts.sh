#!/bin/sh
# Make the fonts of Hangul that the menus in Korean use, from the static OTFs of a
# Pretendard release (https://github.com/orioncactus/pretendard/releases,
# public/static/ in the zip):
#
#     themes/tools/make_hangul_fonts.sh <folder with Pretendard-Bold.otf and Pretendard-SemiBold.otf>
#
# Each goes with a font of the theme, of the line height it's named after: the
# firmware takes the syllables from it rather than from GNU Unifont, set on that
# font's baseline. Pretendard's Latin is Inter's, so the two sit together. Under the
# OFL a font made from it can't be called Pretendard, so these are "Hangul", with
# its licence beside them. Any of them can be replaced by one's own font of that
# name, made by tools/convttf from another typeface.
#
# Needs a C compiler and FreeType (on macOS: brew install freetype).
set -e

here=$(cd "$(dirname "$0")" && pwd)
root="$here/../.."
otf=${1:?usage: $0 <folder with Pretendard-Bold.otf and Pretendard-SemiBold.otf>}
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

# font <name> <otf> <pixel size>: the compatibility jamo, U+3131, to the last
# syllable, U+D7A3; the syllables about as tall as the capitals of the Inter they go
# with, as Korean text set beside Latin is
font() {
    "$build/convttf" -s 12593 -l 55203 -p "$3" -o "$out/$1" "$otf/$2" > /dev/null
}

font 24-Hangul.fnt Pretendard-Bold.otf     16   # the menus, Inter Bold 18
font 17-Hangul.fnt Pretendard-SemiBold.otf 13   # Now Playing, Inter SemiBold 15
font 14-Hangul.fnt Pretendard-Bold.otf     12   # titles and small text, Inter Bold 13
cp "$otf/LICENSE.txt" "$out/Hangul-OFL.txt" 2>/dev/null \
    || echo "put Pretendard's LICENSE beside the fonts as Hangul-OFL.txt"
ls -l "$out"/*-Hangul.fnt
