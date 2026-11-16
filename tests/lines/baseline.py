#!/usr/bin/env python3
# The trace of check-lines' runs with a failed read or write, as the tree's
# readers make it since a failed read stops them (fix-read): held by
# fault-baseline.txt, a sha256 for each playlist's part of the trace.
#
#   python3 -I tests/lines/baseline.py --check <trace> <baseline>
#   python3 -I tests/lines/baseline.py --write <trace> <baseline>
#
# --write is for a change of what a failure does that is meant: it takes the
# trace as it is now for the new baseline.

import hashlib, sys

def digests(path):
    cases, name, h = [], None, None
    with open(path, 'rb') as f:
        for line in f:
            if line.startswith(b'case '):
                if name:
                    cases.append((name, h.hexdigest()))
                name, h = line[5:].strip().decode('latin-1'), hashlib.sha256()
            if h:
                h.update(line)
    if name:
        cases.append((name, h.hexdigest()))
    return cases

def main():
    mode, trace, baseline = sys.argv[1], sys.argv[2], sys.argv[3]
    now = digests(trace)
    if mode == '--write':
        with open(baseline, 'w') as f:
            f.write("# made by baseline.py --write from check-lines' trace of its runs\n"
                    "# with a failure: a line a playlist, its name and sha256\n")
            for name, d in now:
                f.write(f'{name} {d}\n')
        print(f'wrote {baseline}: {len(now)} playlists')
        return 0
    want = []
    with open(baseline) as f:
        for line in f:
            if line.strip() and not line.startswith('#'):
                name, d = line.split()
                want.append((name, d))
    if now == want:
        print(f'lines: with a failure, {len(now)} playlists as the baseline '
              f'has them')
        return 0
    differ = [n for (n, d), (m, e) in zip(now, want) if n != m or d != e]
    print(f'lines: with a failure, '
          f'{len(differ) + abs(len(now) - len(want))} playlists differ from '
          f'the baseline, the first {differ[:5]}')
    return 1

sys.exit(main())
