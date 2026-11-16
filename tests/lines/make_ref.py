#!/usr/bin/env python3
# The reference for tests/lines: library_playlists.c and on_the_go.c as they
# were at BASE, each read of a playlist with a loop of its own, taken out of
# git verbatim. Run from anywhere in the repository:
#
#   python3 -I tests/lines/make_ref.py          writes tests/lines/ref/
#   python3 -I tests/lines/make_ref.py --check  compares ref/ with BASE

import os, subprocess, sys

BASE = '1be9ae95b6'
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = subprocess.check_output(['git', '-C', HERE, 'rev-parse', '--show-toplevel'],
                               text=True).strip()
FILES = ['apps/library_playlists.c', 'apps/on_the_go.c']

def show(path):
    return subprocess.check_output(['git', '-C', ROOT, 'show', f'{BASE}:{path}'],
                                   text=True)

def main():
    bad = 0
    for path in FILES:
        out = os.path.join(HERE, 'ref', os.path.basename(path))
        text = show(path)
        if '--check' in sys.argv:
            with open(out) as f:
                same = f.read() == text
            print(('same   ' if same else 'DIFFERS') + ' ' + out)
            bad += not same
        else:
            with open(out, 'w') as f:
                f.write(text)
            print('wrote ' + out)
    return 1 if bad else 0

sys.exit(main())
