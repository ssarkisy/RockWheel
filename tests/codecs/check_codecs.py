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

"""The codecs through warble: each fixture of expected.txt decoded to the
codec's own output (warble -f, 64-bit float, before the DSP), then

  - a lossless one must be its source exactly, sample for sample;
  - a lossy one must have the sum noted for it, its rate, and the length of
    its source;
  - both: the length the metadata parser gives within a frame of it.

    check_codecs.py <warble> <fixtures> <expected.txt> [--record]

--record notes the lossy ones' sums in expected.txt, and the compiler, after
a change to a codec that is meant to change its output: only from a commit
known to decode right. Run with python3 -I -B.
"""

import hashlib
import os
import platform
import re
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import source_pcm  # noqa: E402  (beside this file)

# how far the decoded length may be from the source's: an encoder's delay
# and padding that the file doesn't note, at most a frame of the codec
LENGTH_SLACK_MS = 50
# and the length the metadata gives: MP4's counts the encoder's delay and
# padding too, which the decoder trims by iTunSMPB (HE-AAC's, 157 ms)
METADATA_SLACK_MS = 200


def read_float_wav(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise ValueError('not a WAV file')
    fmt_type, channels, rate = struct.unpack_from('<HHI', data, 0x14)
    bits = struct.unpack_from('<H', data, 0x22)[0]
    if fmt_type != 3 or bits != 64:
        raise ValueError('not 64-bit float: %d %d' % (fmt_type, bits))
    body = data[0x2e:]
    return channels, rate, body


def decode(warble, path):
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, 'out.wav')
        p = subprocess.run([warble, '-f', path, out], capture_output=True,
                           text=True, timeout=120)
        if p.returncode != 0:
            raise RuntimeError('warble exit %d:\n%s%s' % (p.returncode,
                                                          p.stdout[-2000:],
                                                          p.stderr[-4000:]))
        log = p.stdout + p.stderr
        m = re.search(r'Song length: (\d+) ms', log)
        length = int(m.group(1)) if m else None
        channels, rate, body = read_float_wav(out)
    return channels, rate, body, length


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    record = '--record' in sys.argv
    if len(args) != 3:
        sys.exit(__doc__)
    warble, fixtures, expected = args
    rows, lines = [], open(expected, encoding='utf-8').read().splitlines()
    for line in lines:
        if line.strip() and not line.startswith('#'):
            rows.append(line.split())
    # each fixture listed once, and only those there are
    listed = [r[0] for r in rows]
    present = sorted(f for f in os.listdir(fixtures) if not f.startswith('.'))
    if not rows or len(set(listed)) != len(listed) or sorted(listed) != present:
        print('FAIL %d fixtures listed, %d in %s: %s' % (
            len(listed), len(present), fixtures,
            ' '.join(sorted(set(listed) ^ set(present))) or 'listed twice'))
        return 1
    failed = 0
    sums = {}
    start = time.time()
    for name, kind, source, want_rate, want_sum in rows:
        rate, channels, bits, seconds = source_pcm.parse(source)
        problems = []
        try:
            got_channels, got_rate, body, length = decode(
                warble, os.path.join(fixtures, name))
        except Exception as e:  # noqa: BLE001  (each fixture its own result)
            print('FAIL %s: %s' % (name, e))
            failed += 1
            continue
        frames = len(body) // 8 // max(got_channels, 1)
        if got_channels != channels:
            problems.append('%d channels, not %d' % (got_channels, channels))
        if got_rate != int(want_rate):
            problems.append('rate %d, not %s' % (got_rate, want_rate))
        ms = frames * 1000.0 / got_rate if got_rate else 0
        if abs(ms - seconds * 1000) > LENGTH_SLACK_MS:
            problems.append('%.0f ms decoded, not %.0f' % (ms, seconds * 1000))
        if length is None or abs(length - seconds * 1000) > METADATA_SLACK_MS:
            problems.append('length %s ms in the metadata, not %.0f'
                            % (length, seconds * 1000))
        digest = hashlib.sha256(body).hexdigest()
        if kind == 'lossless':
            want = source_pcm.samples(source)
            values = struct.unpack('<%dd' % (len(body) // 8), body)
            scale = float(1 << (bits - 1))
            got = [v * scale for v in values]
            if len(got) != len(want):
                problems.append('%d samples, not %d' % (len(got), len(want)))
            for i, (g, w) in enumerate(zip(got, want)):
                if g != w:
                    problems.append('sample %d is %r, not %d' % (i, g, w))
                    break
        elif record:
            sums[name] = digest
        elif digest != want_sum:
            problems.append('sum %s, not %s' % (digest[:16], want_sum[:16]))
        if problems:
            failed += 1
            print('FAIL %s: %s' % (name, '; '.join(problems)))
    if record:
        # with the compiler that built them, which the sums can depend on
        cc = os.environ.get('HOST_CC', 'clang')
        version = subprocess.run([cc, '--version'], capture_output=True,
                                 text=True).stdout.splitlines()
        compiler = '# Recorded with: %s, %s' % (version[0] if version else cc,
                                                platform.machine())
        out = []
        for line in lines:
            f = line.split()
            if line.startswith('# Recorded with:'):
                line = compiler
                compiler = None
            elif f and not line.startswith('#') and f[0] in sums:
                if compiler:
                    out.append(compiler)
                    compiler = None
                f[4] = sums[f[0]]
                line = ' '.join(f)
            out.append(line)
        with open(expected, 'w', encoding='utf-8') as fh:
            fh.write('\n'.join(out) + '\n')
        print('noted the sums of %d lossy fixtures' % len(sums))
    print('%-10s %d fixtures, %d failed (%.1f s)' % ('codecs', len(rows), failed,
                                                    time.time() - start))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
