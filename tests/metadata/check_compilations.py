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

"""The metadata parsers' compilation mark (struct mp3entry's compilation),
through warble, which prints it: each file of make_compilations.py's, its
mark as expected.txt has it, and its title still read where it has one;
then the pairs of pairs.txt, a file with the mark and without, whose other
tags and picture must read alike, as before the mark was read.

    check_compilations.py <warble> <dir>

Run with python3 -I -B.
"""

import os
import subprocess
import sys
import tempfile


def main(args):
    if len(args) != 2:
        sys.exit(__doc__)
    warble, folder = args
    failed = 0
    with open(os.path.join(folder, 'expected.txt')) as f:
        cases = [line.split() for line in f if line.strip()]
    with tempfile.TemporaryDirectory() as tmp:
        for name, flag, titled_too in cases:
            # the metadata is printed before the codec starts: halted at once
            p = subprocess.run([warble, '-f', '-c', 'halt=1',
                                os.path.join(folder, name),
                                os.path.join(tmp, 'out.wav')],
                               capture_output=True, text=True, errors='replace')
            mark = 'Compilation: true' in p.stderr.splitlines()
            titled = any(l.startswith('Title: ') for l in p.stderr.splitlines())
            ok = (p.returncode == 0 and mark == (flag == '1')
                  and (titled or titled_too == '0'))
            print('%-24s %s%s' % (name, 'compilation' if mark else '-',
                                  '' if ok else '  FAILED'))
            if not ok:
                failed += 1
                sys.stderr.write(p.stderr[-2000:])
        # The mark changes nothing else: the other tags, and the picture's
        # place, but for the mark's own frame before it
        pairs_file = os.path.join(folder, 'pairs.txt')
        pairs = []
        pictureless = 0
        if os.path.exists(pairs_file):
            with open(pairs_file) as f:
                pairs = [line.split() for line in f if line.strip()]
        for marked, plain, shift in pairs:
            a, b = tags(warble, os.path.join(folder, marked)), \
                tags(warble, os.path.join(folder, plain))
            art_a, art_b = a.pop('Album art', None), b.pop('Album art', None)
            moved = (art_a is None and art_b is None) or (
                art_a is not None and art_b is not None
                and art_a[0] == art_b[0] and art_a[2] == art_b[2]
                and art_a[1] == art_b[1] + int(shift))
            mark = a.pop('Compilation', None) == 'true'
            ok = mark and a == b and moved
            pictureless += art_b is None
            if not ok:
                failed += 1
                print('%-24s FAILED: mark %s, picture %s / %s, tags %s' % (
                    marked, mark, art_a, art_b, 'same' if a == b else 'differ'))
        if pairs:
            # those whose picture wasn't placed: the buffer full before it
            print('%d files of tags filling the buffer, %d of them before the'
                  ' picture, read as without the mark' % (len(pairs),
                                                          pictureless))
    print('%d of %d failed' % (failed, len(cases) + len(pairs)))
    return 1 if failed else 0


# the lines of warble -m that the tags give: by name; the picture's as
# (type, place, size)
KEPT = ('Title', 'Artist', 'Album', 'Composer', 'Grouping', 'Album artist',
        'Compilation', 'Album art')


def tags(warble, path):
    p = subprocess.run([warble, '-m', path], capture_output=True, text=True,
                       errors='replace')
    out = {}
    for line in p.stderr.splitlines():
        key, _, value = line.partition(': ')
        if key not in KEPT:
            continue
        if key == 'Album art':
            words = value.replace(',', '').split()
            value = (words[1], int(words[3]), int(words[4]))
        out[key] = value
    return out


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
