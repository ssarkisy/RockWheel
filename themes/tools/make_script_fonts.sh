#!/bin/sh
# Make the fonts of the alphabets other than Latin, Greek and Cyrillic, which
# Inter has, from OFL typefaces of Google Fonts (https://github.com/google/fonts,
# ofl/<family>/):
#
#     themes/tools/make_script_fonts.sh <folder with a folder of each family>
#
# with notosansjp, notosanssc, notosanstc, notosanshebrew, vazirmatn and
# ibmplexsansthai in it, each with its TTF and OFL.txt. Hangul comes from
# make_hangul_fonts.sh.
#
# They are the sources the iPod takes glyphs from: on the Updating Library screen
# it gathers what the tags of the library and the names of the languages are
# written in, with the alphabets whole and the common ideographs, and makes fonts
# of just those glyphs (apps/library_fonts.c); run make_common_han.py after this,
# for the ideographs these have. These
# files stay on the disk and are only read then. Each comes in the three sizes of
# the theme's fonts, named after the line height of the font it goes with:
# 24 the menus (Inter Bold 18), 17 Now Playing (Inter SemiBold 15), 14 small text
# (Inter Bold 13). Under the OFL a font made from one can't carry its name, so they
# are named after their alphabet, with each licence beside them.
#
# Needs a C compiler, FreeType (on macOS: brew install freetype) and fontTools
# (pip install fonttools) for the weights of the variable fonts.
set -e

here=$(cd "$(dirname "$0")" && pwd)
root="$here/../.."
src=${1:?usage: $0 <folder with a folder of each family>}
out="$here/../fonts/.rockbox/fonts/scripts"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
mkdir -p "$out"

if pkg-config --exists freetype2 2>/dev/null; then
    ft=$(pkg-config --cflags --libs freetype2)
else
    prefix=$(brew --prefix freetype)
    ft="-I$prefix/include/freetype2 -L$prefix/lib -lfreetype"
fi
# shellcheck disable=SC2086
cc -std=c99 -O2 "$root/tools/convttf.c" $ft -lm -o "$build/convttf"
python=${PYTHON:-python3}

# weight <family folder> <weight> <static name>: the TTF of that weight, from the
# variable font if there is one
weight() {
    var=$(ls "$src/$1"/*\[*.ttf 2>/dev/null | head -1)
    if [ -n "$var" ]; then
        [ -f "$build/$3" ] || "$python" -m fontTools.varLib.instancer "$var" "wght=$2" \
            -o "$build/$3" > /dev/null
        echo "$build/$3"
    else
        case $2 in 700) w=Bold ;; *) w=SemiBold ;; esac
        ls "$src/$1"/*-$w.ttf | head -1
    fi
}

# font <name> <family> <first> <last> <menu px> <now playing px> <small px>
# spaced as the typeface spaces them (-A), as make_fonts.sh does Inter
font() {
    bold=$(weight "$2" 700 "$2-700.ttf")
    semi=$(weight "$2" 600 "$2-600.ttf")
    "$build/convttf" -s "$3" -l "$4" -A -p "$5" -o "$out/24-$1.fnt" "$bold" > /dev/null
    "$build/convttf" -s "$3" -l "$4" -A -p "$6" -o "$out/17-$1.fnt" "$semi" > /dev/null
    "$build/convttf" -s "$3" -l "$4" -A -p "$7" -o "$out/14-$1.fnt" "$bold" > /dev/null
    cp "$src/$2/OFL.txt" "$out/$1-OFL.txt"
}

# The ideographs and kana as tall as the capitals of the Inter beside them, as the
# syllables of Hangul are; the CJK punctuation and the full-width forms, as titles in
# Japanese and Chinese have them, apart from the ideographs
font Han-JP     notosansjp     12288 40959 16 13 12  # U+3000, kana, U+4E00..U+9FFF
font Wide-JP    notosansjp     65280 65519 16 13 12  # U+FF00..U+FFEF
font Han-SC     notosanssc     12288 40959 16 13 12
font Wide-SC    notosanssc     65280 65519 16 13 12
font Han-TC     notosanstc     12288 40959 16 13 12
font Wide-TC    notosanstc     65280 65519 16 13 12
# The others at the sizes of Inter, which their letters match
font Hebrew     notosanshebrew  1424  1535 18 15 13  # U+0590..U+05FF
font Arabic     vazirmatn       1536  1791 18 15 13  # U+0600..U+06FF
font ArabicA    vazirmatn      64336 65023 18 15 13  # forms U+FB50..U+FDFF
font ArabicB    vazirmatn      65136 65279 18 15 13  # forms U+FE70..U+FEFF
font Thai       ibmplexsansthai 3584  3711 18 15 13  # U+0E00..U+0E7F
ls -l "$out"
