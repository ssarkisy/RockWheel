#!/bin/bash
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
# Copyright (C) 2026 Sergei Sarkisian
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.
#
# The codecs' fixtures, from source_pcm.py's sound, by the encoders of a Mac:
# afconvert (Apple's AAC, HE-AAC and ALAC, as Finder converts), lame, flac,
# oggenc, ffmpeg's libopus and wavpack. Writes fixtures/ and expected.txt;
# then check_codecs.py --record notes the lossy ones' decoded sums. Made once
# and committed: made again, the files differ. afconvert's AAC and HE-AAC
# differ even from one run to the next, as do the containers of ALAC, Ogg and
# Opus (times, serial numbers); FLAC, MP3 and WavPack come out the same.
#
#   tests/codecs/make_fixtures.sh

set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$here/fixtures
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
rm -rf "$out"
mkdir -p "$out"

src() { # <source> -> a WAV of it in $work, its path printed
    local f=$work/${1//:/_}.wav
    [ -f "$f" ] || python3 -I -B "$here/source_pcm.py" wav "$1" "$f"
    echo "$f"
}

exec 3>"$here/expected.txt"
cat >&3 <<'EOF'
# The codecs' fixtures (make_fixtures.sh), as check_codecs.py checks them:
# <file> <lossless|lossy> <source> <rate decoded> <sum of the decoded PCM>
# The source is source_pcm.py's <rate>:<channels>:<bits>:<seconds>. A lossless
# file decodes to it exactly; a lossy one to the sum noted for it, at the rate
# given and for as long as its source.
# A lossy file's sum is of that file as committed, decoded by the codecs' C
# built with the compiler below: a new compiler can change it where a codec's
# C is undefined, as UBSan finds in libfaad's shifts. A sum that changes is
# looked into, and noted again (check_codecs.py --record) only from a commit
# known to decode right; the files themselves are not made again
# (make_fixtures.sh).
EOF
row() { echo "$1 $2 $3 $4 -" >&3; }

# MP3, LAME's gapless header
s=44100:2:16:3
lame --quiet -b 128 "$(src $s)" "$out/mp3-128.mp3"
row mp3-128.mp3 lossy $s 44100
s=44100:1:16:2
lame --quiet -V 6 "$(src $s)" "$out/mp3-v6-mono.mp3"
row mp3-v6-mono.mp3 lossy $s 44100

# AAC-LC and HE-AAC v1 and v2 by Apple's encoder; the iPod decodes no SBR
# (config.h: no CODEC_AAC_SBR_DEC on PP5022), so HE-AAC is its core at half
# the rate
s=44100:2:16:3
afconvert -f m4af -d aac -b 128000 "$(src $s)" "$out/aac-lc-128.m4a"
row aac-lc-128.m4a lossy $s 44100
afconvert -f m4af -d aach -b 48000 "$(src $s)" "$out/he-aac-v1-48.m4a"
row he-aac-v1-48.m4a lossy $s 22050
afconvert -f m4af -d aacp -b 32000 "$(src $s)" "$out/he-aac-v2-32.m4a"
row he-aac-v2-32.m4a lossy $s 22050

# ALAC 16 and 24 bit, Apple's
s=44100:2:16:2
afconvert -f m4af -d alac "$(src $s)" "$out/alac-16.m4a"
row alac-16.m4a lossless $s 44100
s=48000:2:24:1
afconvert -f m4af -d alac "$(src $s)" "$out/alac-24-48k.m4a"
row alac-24-48k.m4a lossless $s 48000

# FLAC: its usual blocks, 8192 and 16384 (past the subset: --lax), 24/96,
# and mono
s=44100:2:16:2
flac --silent -5 -o "$out/flac-16.flac" "$(src $s)"
row flac-16.flac lossless $s 44100
s=44100:2:16:1.5
flac --silent --lax -5 -b 8192 -o "$out/flac-16-b8192.flac" "$(src $s)"
row flac-16-b8192.flac lossless $s 44100
flac --silent --lax -5 -b 16384 -o "$out/flac-16-b16384.flac" "$(src $s)"
row flac-16-b16384.flac lossless $s 44100
s=96000:2:24:0.75
flac --silent -5 -o "$out/flac-24-96k.flac" "$(src $s)"
row flac-24-96k.flac lossless $s 96000
s=22050:1:16:2
flac --silent -5 -o "$out/flac-16-mono-22k.flac" "$(src $s)"
row flac-16-mono-22k.flac lossless $s 22050

# Vorbis
s=44100:2:16:3
oggenc --quiet -q 4 -o "$out/vorbis-q4.ogg" "$(src $s)"
row vorbis-q4.ogg lossy $s 44100

# Opus, at 48 kHz always; and one with OpusTags over several pages
s=48000:2:16:3
ffmpeg -nostdin -hide_banner -loglevel error -i "$(src $s)" -c:a libopus \
    -b:a 96k "$out/opus-96.opus"
row opus-96.opus lossy $s 48000
s=48000:2:16:1
big=$(python3 -I -B -c "print('Synthetic comment for the host tests. ' * 3000)")
ffmpeg -nostdin -hide_banner -loglevel error -i "$(src $s)" -c:a libopus \
    -b:a 64k -metadata comment="$big" "$out/opus-bigtags.opus"
row opus-bigtags.opus lossy $s 48000

# WavPack, lossless 16 bit and 24 bit
s=44100:2:16:1.5
wavpack -q -y "$(src $s)" -o "$out/wavpack-16.wv"
row wavpack-16.wv lossless $s 44100
s=48000:2:24:0.75
wavpack -q -y -h "$(src $s)" -o "$out/wavpack-24-48k.wv"
row wavpack-24-48k.wv lossless $s 48000

exec 3>&-
du -sh "$out"
