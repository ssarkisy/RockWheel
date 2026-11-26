#!/bin/bash

# Replace ./libiap with the latest upstream libiap.
#
# The copy here carries changes of its own (see libiap/README.rockbox).
# Before overwriting it, check it against upstream at the commit it was
# imported from (libiap/HEAD) and stop if it differs, so those changes are
# not lost unnoticed. --force overwrites them anyway.

set -e

url=https://github.com/mojyack/libiap.git
src=/tmp/libiap
dst=$PWD/libiap

force=false
if [[ $1 == --force ]]; then
    force=true
elif [[ $# -gt 0 ]]; then
    echo "usage: $0 [--force]" >&2
    exit 2
fi

if [[ ! -e $src ]]; then
    git clone "$url" "$src"
else
    git -C "$src" pull
fi

if ! $force; then
    base=$(cat "$dst/HEAD")
    if ! git -C "$src" cat-file -e "$base^{commit}" 2>/dev/null; then
        echo "libiap/HEAD ($base) is not in upstream;" \
             "cannot check for local changes." >&2
        echo "Use --force to overwrite libiap anyway." >&2
        exit 1
    fi

    imported=$(mktemp -d)
    trap 'rm -rf "$imported"' EXIT
    git -C "$src" archive "$base" src/iap | tar -x -C "$imported"

    if ! changes=$(diff -rq "$imported/src/iap" "$dst" \
                       -x README.rockbox -x HEAD -x LICENSE); then
        echo "libiap has local changes that a sync would discard:" >&2
        echo "$changes" | sed -e "s|$imported/src/iap|upstream|" \
                              -e "s|$dst|libiap|" >&2
        echo "Port them onto the new upstream," \
             "or use --force to discard them." >&2
        exit 1
    fi
fi

rsync --recursive --delete "$src/src/iap/" "$dst/" --exclude /README.rockbox
cp "$src/LICENSE" "$dst/LICENSE"
git -C "$src" rev-parse HEAD > "$dst/HEAD"
