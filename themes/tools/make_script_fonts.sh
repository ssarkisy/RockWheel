#!/bin/sh
# Give the theme's Inter fonts the alphabets other than Latin, Greek and
# Cyrillic, from OFL typefaces: those of Google Fonts (https://github.com/google/fonts,
# ofl/<family>/) and Pretendard's Hangul (https://github.com/orioncactus/pretendard/releases,
# public/static/ in the zip):
#
#     themes/tools/make_script_fonts.sh <folder with a folder of each family> <folder with Pretendard's OTFs>
#
# with notosansjp, notosanssc, notosanstc, notosanshebrew, vazirmatn and
# ibmplexsansthai in the first, each with its TTF and OFL.txt, and
# Pretendard-SemiBold.otf, -Medium.otf, -Regular.otf, -Light.otf and LICENSE.txt
# in the second.
# Run it after make_fonts.sh, whose fonts of the menus and the text it merges
# into (merge_fonts.py); again on its own output, it makes the same.
#
# The alphabets come in the three sizes of the theme's fonts first, outside
# the theme: the ideographs and kana as tall as the capitals of Inter beside
# them, the others at the sizes of Inter, which their letters match. Under the
# OFL a font made from one can't carry its name; the fonts keep Inter's, which
# reserves none, with each typeface's licence beside them.
#
# Needs a C compiler, FreeType (on macOS: brew install freetype) and fontTools
# (pip install fonttools; PYTHON names a python3 with it). With FreeType 2.14
# and fontTools 4.68, the sources below gave the fonts in the theme.
set -e

here=$(cd "$(dirname "$0")" && pwd)
root="$here/../.."
usage="usage: $0 <folder with a folder of each family> <folder with Pretendard's OTFs>"
src=${1:?$usage}
otf=${2:?$usage}
out="$here/../fonts/.rockbox/fonts"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT

# the sources the fonts in the theme were made of, Google Fonts' as of 2026-11-04
echo "c2f3b4d463500a2ddcd3849cded1fceeb9fd6d1c32e6cbecd568453ba50fc68f $src/notosansjp/NotoSansJP[wght].ttf
a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da $src/notosanssc/NotoSansSC[wght].ttf
864727d210d54f2537bbe23b3a839436c3992af72de9322af5270897246bd44f $src/notosanstc/NotoSansTC[wght].ttf
7ef36a2c3593758cdb622e1bdef4f84523e92fbc3ccc667438dd80ff54c2de88 $src/notosanshebrew/NotoSansHebrew[wdth,wght].ttf
696249a2c74b39ffdef55de4df2809c5b639d3ff80d618d8160a095d2fd49dca $src/vazirmatn/Vazirmatn[wght].ttf
f88fbe0a3717c93deddf4b13e50a05c9adcd38f58d675d9d5d0b869961851667 $src/ibmplexsansthai/IBMPlexSansThai-Bold.ttf
9d7526a0c8dfad63c49815d0cce9eea1026f018468b81c579997e953620a67d3 $src/ibmplexsansthai/IBMPlexSansThai-SemiBold.ttf
46ba1b7365cc2c583914f23914cce46e836a72662e34f6e3029b15aaa10b792e $src/ibmplexsansthai/IBMPlexSansThai-Medium.ttf
eee061d1bac39be40f9bb94498898cd1894f5d3d9a2f0ee6cbd48a6bd03d052b $src/ibmplexsansthai/IBMPlexSansThai-Regular.ttf
c89bc43027dc7cde5726e96223376f8eec09302b2fc1f8147fd5b57cfc376118 $otf/Pretendard-SemiBold.otf
d39e50e4bb52b4993b6a4eeb821a171254745bd824446af01e1f616b89fface0 $otf/Pretendard-Medium.otf
3ffbacde6ab8411f1d2db54bb9b1f0b3ee2a738932033722cf0388c06aed1c93 $otf/Pretendard-Regular.otf
51418ee96ef96e9059db88d7da82f24fada1f4aca70d5157c878ea6b460dd343 $otf/Pretendard-Light.otf" |
while read -r sum file; do
    [ "$(shasum -a 256 "$file" | cut -d' ' -f1)" = "$sum" ] \
        || { echo "$file isn't the one the theme's fonts were made of" >&2; exit 1; }
done

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
        case $2 in 700) w=Bold ;; 500) w=Medium ;; 400) w=Regular ;; *) w=SemiBold ;; esac
        ls "$src/$1"/*-$w.ttf | head -1
    fi
}

# font <name> <family> <first> <last> <menu px> <now playing px> <small px>
# spaced as the typeface spaces them (-A), as make_fonts.sh does Inter
font() {
    bold=$(weight "$2" 700 "$2-700.ttf")
    semi=$(weight "$2" 600 "$2-600.ttf")
    "$build/convttf" -s "$3" -l "$4" -A -p "$5" -o "$build/24-$1.fnt" "$bold" > /dev/null
    "$build/convttf" -s "$3" -l "$4" -A -p "$6" -o "$build/17-$1.fnt" "$semi" > /dev/null
    "$build/convttf" -s "$3" -l "$4" -A -p "$7" -o "$build/14-$1.fnt" "$bold" > /dev/null
}

# U+3000 to U+9FFF, the compatibility ideographs and the full-width forms
for r in JP SC TC; do
    family=notosans$(echo $r | tr A-Z a-z)
    font Han-$r    $family 12288 40959 16 13 12
    font Compat-$r $family 63744 64255 16 13 12
    font Wide-$r   $family 65280 65519 16 13 12
done
font Hebrew  notosanshebrew  1424  1535 18 15 13  # U+0590..U+05FF
font Arabic  vazirmatn       1536  1791 18 15 13  # U+0600..U+06FF
font ArabicA vazirmatn      64336 65023 18 15 13  # forms U+FB50..U+FDFF
font ArabicB vazirmatn      65136 65279 18 15 13  # forms U+FE70..U+FEFF
font Thai    ibmplexsansthai 3584  3711 18 15 13  # U+0E00..U+0E7F
# Hangul's letters and syllables: the small ones a size up, as at 13 a third
# of the syllables come a pixel wider than the rest; a weight lighter, as a
# syllable has more strokes than a letter
hangul() {
    "$build/convttf" -s 12593 -l 55203 -A -p "$3" -o "$build/$1-Hangul.fnt" "$otf/$2" > /dev/null
}
hangul 24 Pretendard-SemiBold.otf 18
hangul 17 Pretendard-Medium.otf   15
hangul 14 Pretendard-SemiBold.otf 14

# Neo's and Modern's fonts, each with the alphabets in its own weight and size.
# px <em>: convttf's options for an em of that many pixels
px() {
    case $1 in
        *.5)   echo "-X 36 -p $(echo "$1" | sed 's/\.5$//' | awk '{ print 2 * $1 + 1 }')" ;;
        11.67) echo "-X 60 -p 14" ;;
        *)     echo "-X 72 -p $1" ;;
    esac
}
# pretendard <weight>: its file a weight lighter than Inter's, as above
pretendard() {
    case $1 in 600) w=Medium ;; 500) w=Regular ;; *) w=Light ;; esac
    echo "$otf/Pretendard-$w.otf"
}
# set <font> <weight> <ideographs' em> <the others' em> <Hangul's em>
# The ideographs and kana as tall as Inter's capitals, 10 px at the least, under
# which they can't be read; Hangul 11.67 at the least, as the 14 px fonts' above:
# in the smallest fonts both are taller than the Latin beside them, cut to the
# line where they don't fit (merge_fonts.py counts those).
: > "$build/sets.txt"
set_of() {
    name=$1 wght=$2 han=$3 em=$4 hangul=$5
    echo "$name" >> "$build/sets.txt"
    # shellcheck disable=SC2046
    for r in JP SC TC; do
        family=notosans$(echo $r | tr A-Z a-z)
        ttf=$(weight $family "$wght" "$family-$wght.ttf")
        "$build/convttf" -s 12288 -l 40959 -A $(px "$han") -o "$build/$name-Han-$r.fnt" "$ttf" > /dev/null
        "$build/convttf" -s 63744 -l 64255 -A $(px "$han") -o "$build/$name-Compat-$r.fnt" "$ttf" > /dev/null
        "$build/convttf" -s 65280 -l 65519 -A $(px "$han") -o "$build/$name-Wide-$r.fnt" "$ttf" > /dev/null
    done
    # shellcheck disable=SC2046
    {
    "$build/convttf" -s 1424 -l 1535 -A $(px "$em") -o "$build/$name-Hebrew.fnt" \
        "$(weight notosanshebrew "$wght" "notosanshebrew-$wght.ttf")" > /dev/null
    ttf=$(weight vazirmatn "$wght" "vazirmatn-$wght.ttf")
    "$build/convttf" -s 1536 -l 1791 -A $(px "$em") -o "$build/$name-Arabic.fnt" "$ttf" > /dev/null
    "$build/convttf" -s 64336 -l 65023 -A $(px "$em") -o "$build/$name-ArabicA.fnt" "$ttf" > /dev/null
    "$build/convttf" -s 65136 -l 65279 -A $(px "$em") -o "$build/$name-ArabicB.fnt" "$ttf" > /dev/null
    "$build/convttf" -s 3584 -l 3711 -A $(px "$em") -o "$build/$name-Thai.fnt" \
        "$(weight ibmplexsansthai "$wght" "ibmplexsansthai-$wght.ttf")" > /dev/null
    "$build/convttf" -s 12593 -l 55203 -A $(px "$hangul") -o "$build/$name-Hangul.fnt" \
        "$(pretendard "$wght")" > /dev/null
    }
}
set_of 19-Inter-SemiBold 600 13 15   15     # Neo row
set_of 17-Inter-Medium-13 500 12 13  13     # Neo text, values
set_of 15-Inter-Medium   500 11 12   12     # Neo label
set_of 15-Inter-SemiBold 600 11 12   12     # Neo name
set_of 14-Inter-Medium   500 10 11   11.67  # Neo small
set_of 20-Inter-SemiBold 600 14 15.5 15.5   # Modern titles
set_of 18-Inter-Regular  400 12 14   14     # Modern row

"$python" "$here/merge_fonts.py" "$out" "$build" "$root/firmware/font_han_forms.h" "$build/sets.txt"

# licence <from> <to>: with the line ends git keeps
licence() {
    tr -d '\r' < "$1" > "$out/$2"
}
licence "$src/notosanssc/OFL.txt" Han-OFL.txt # as JP's and TC's
licence "$src/notosanshebrew/OFL.txt" Hebrew-OFL.txt
licence "$src/vazirmatn/OFL.txt" Arabic-OFL.txt
licence "$src/ibmplexsansthai/OFL.txt" Thai-OFL.txt
licence "$otf/LICENSE.txt" Hangul-OFL.txt
ls -l "$out"
