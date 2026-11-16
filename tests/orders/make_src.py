#!/usr/bin/env python3
# The parts of apps/tagtree.c that tests/orders checks, taken out of the
# tree as they are: the Songs orders' sort from a hint, with its proof, and
# the full sort it stands in for.
#
#   python3 -I tests/orders/make_src.py <out.c>

import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

def read(path):
    with open(os.path.join(ROOT, path)) as f:
        return f.read().split('\n')

def find(src, head):
    starts = [i for i, l in enumerate(src) if l == head]
    if len(starts) != 1:
        sys.exit(f'make_src.py: {len(starts)} lines "{head}"')
    return starts[0]

def block(src, head):
    """from the line head to the first line that closes it at its start"""
    i = find(src, head)
    end = next(k for k in range(i, len(src)) if src[k] in ('}', '};'))
    return '\n'.join(src[i:end + 1]) + '\n'

def define(src, name):
    found = [l for l in src if l.startswith(f'#define {name} ')]
    if len(found) != 1:
        sys.exit(f'make_src.py: {len(found)} #define {name}')
    return found[0] + '\n'

tagtree = read('apps/tagtree.c')
parts = ['/* taken out of apps/tagtree.c by tests/orders/make_src.py */\n']
for head in ('struct song_key {', 'struct song_place {'):
    parts.append(block(tagtree, head))
parts.append(define(tagtree, 'SONGS_ROW_NONE'))
for head in ('static bool key_before(const struct song_key *a, const struct song_key *b)',
             'static bool songs_row_before(const struct song_key *keys, int x, int y)',
             'static uint16_t *songs_rows_sort(const struct song_key *keys, int n,',
             'struct songs_hint {',
             'static const uint16_t *songs_order_from_hint(const struct song_key *keys, int n,'):
    parts.append(block(tagtree, head))
with open(sys.argv[1], 'w') as f:
    f.write('\n'.join(parts))
