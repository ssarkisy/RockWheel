#!/bin/sh
# Checks firmware/common/nfc.c against lib/utf8proc, which it replaced in the
# firmware: run after either changes (a new utf8proc brings new tables).
#   sh tools/nfctest/run.sh [--update] [corpus files...]
# Makes nfc_table.h again and compares it with the tree's (--update writes it),
# then runs every test of nfctest.c. Each line must end "0 differ"; the exit
# code is 1 otherwise. Corpus files hold strings divided by NUL bytes.
# Under a minute on eight cores. The work goes to $NFCTEST_WORK or a
# temporary folder.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
work=${NFCTEST_WORK:-$(mktemp -d)}
mkdir -p "$work"
cc=${CC:-cc}
update=
[ "$1" = --update ] && { update=1; shift; }

$cc -O2 -w -DUTF8PROC_STATIC -I"$root/lib/utf8proc" \
    "$root/tools/nfctest/gen_table.c" -o "$work/gen_table"
"$work/gen_table" > "$work/nfc_table.h"
if ! cmp -s "$work/nfc_table.h" "$root/firmware/common/nfc_table.h"; then
    [ -n "$update" ] || { echo "nfc_table.h is stale: run with --update"; exit 1; }
    cp "$work/nfc_table.h" "$root/firmware/common/nfc_table.h"
    echo "nfc_table.h updated"
fi

# the file as the firmware builds it, for its warnings
$cc -O2 -Wall -Wextra -c -I"$root/tools/nfctest/stub" \
    "$root/firmware/common/nfc.c" -o "$work/nfc.o"
$cc -O2 -w -DUTF8PROC_STATIC -I"$root/lib/utf8proc" \
    -I"$root/tools/nfctest/stub" "$root/tools/nfctest/nfctest.c" -o "$work/nfctest"

rm -f "$work/fail"
run() { "$work/nfctest" "$@" > "$work/out.$1.$3" 2>&1 || touch "$work/fail"; }
run classes &
run seconds &
run firsts &
run between &
run marks &
run edge &
run random 10000000 1 &
run random 10000000 2 &
run readonly 300000 3 &
wait
cat "$work"/out.*
for f in "$@"; do
    "$work/nfctest" corpus "$f" || touch "$work/fail"
done
[ ! -e "$work/fail" ] || { echo "FAILED"; exit 1; }
echo "all agree"
