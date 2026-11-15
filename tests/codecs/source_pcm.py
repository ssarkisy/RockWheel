#!/usr/bin/env python3
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

"""The fixtures' sound, made the same each time: a tone, a chirp, noise and
silence, a quarter of the time each, the channels different. A source is
"<rate>:<channels>:<bits>:<seconds>", as expected.txt names it.

    source_pcm.py wav <source> <file.wav>   write it as a WAV file
"""

import math
import struct
import sys


def parse(source):
    rate, channels, bits, seconds = source.split(':')
    return int(rate), int(channels), int(bits), float(seconds)


def samples(source):
    """the frames, interleaved, as integers of the source's bits"""
    rate, channels, bits, seconds = parse(source)
    n = int(round(rate * seconds))
    full = (1 << (bits - 1)) - 1
    seed = 12345
    out = []
    quarter = n // 4
    for i in range(n):
        t = i / rate
        part = min(i // quarter, 3) if quarter else 0
        for c in range(channels):
            if part == 0:
                v = 0.5 * math.sin(2 * math.pi * (440 + 220 * c) * t)
            elif part == 1:
                # a sweep from 100 Hz to 10 kHz over its quarter
                d = quarter / rate
                k = math.log(100.0) / d
                v = 0.4 * math.sin(2 * math.pi * 100 * (math.exp(k * (t - d)) - 1) / k
                                   + c * 0.5)
            elif part == 2:
                seed = (seed * 1103515245 + 12345) & 0x7fffffff
                v = 0.05 * ((seed >> 8) / float(1 << 22) - 1.0)
            else:
                v = 0.0
            out.append(max(-full - 1, min(full, int(round(v * full)))))
    return out


def write_wav(source, path):
    rate, channels, bits, _ = parse(source)
    data = samples(source)
    width = bits // 8
    body = bytearray()
    for s in data:
        body += (s & ((1 << bits) - 1)).to_bytes(width, 'little')
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(body)) + b'WAVE')
        f.write(b'fmt ' + struct.pack('<IHHIIHH', 16, 1, channels, rate,
                                      rate * channels * width,
                                      channels * width, bits))
        f.write(b'data' + struct.pack('<I', len(body)) + bytes(body))


if __name__ == '__main__':
    if len(sys.argv) != 4 or sys.argv[1] != 'wav':
        sys.exit(__doc__)
    write_wav(sys.argv[2], sys.argv[3])
