#!/usr/bin/env python3
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.
"""The addresses of a crash.log (apps/crash_log.c) as functions.

    crash_symbols.py crash.log [--build DIR | --elf FILE | --map FILE]
                               [--archive DIR] [--all]

Each record's pc, lr, trace and the registers that point into code are
looked up in the build its "version" line names: by default in
<archive>/<version>/ (rockbox.elf, else rockbox.map), the archive being
~/ipod-data/archive/device-builds. rockbox.elf names static functions too,
through the toolchain's nm (arm-elf-eabi-nm on PATH or in
~/Developer/rockbox-toolchain/bin); rockbox.map only the global ones, with
the object file each is in. Only the last record unless --all.

A return address (lr, the trace, a panicf's pc and a jump to 0's) is looked up
one byte back, inside the call that made it; a fault's pc as it is."""

import argparse
import os
import re
import shutil
import subprocess
import sys
from bisect import bisect_right

ARCHIVE = os.path.expanduser('~/ipod-data/archive/device-builds')
TOOLCHAIN = os.path.expanduser('~/Developer/rockbox-toolchain/bin')
# what the record's pc is: the fault itself, or a return address
PC_IS_RETURN = {'panicf', 'jump to 0', 'hang'}


def read_records(path):
    records, cur = [], None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('== crash record'):
                cur = {}
                records.append(cur)
            elif cur is not None and ': ' in line:
                key, value = line.split(': ', 1)
                cur[key.strip()] = value.strip()
            elif cur is not None and line.endswith(':'):
                cur[line[:-1].strip()] = ''
    return records


class Symbols:
    """Start addresses, names and (from a map) object files, sorted."""

    def __init__(self, entries, source):
        entries.sort()
        self.addrs = [e[0] for e in entries]
        self.entries = entries
        self.source = source

    def lookup(self, addr):
        i = bisect_right(self.addrs, addr) - 1
        if i < 0:
            return None
        start, name, size, obj = self.entries[i]
        if size and addr >= start + size:
            return None
        return start, name, obj


def nm_tool():
    for name in ('arm-elf-eabi-nm',):
        found = shutil.which(name) or (
            os.path.join(TOOLCHAIN, name)
            if os.access(os.path.join(TOOLCHAIN, name), os.X_OK) else None)
        if found:
            return found
    return None


def from_elf(path):
    nm = nm_tool()
    if not nm:
        return None
    out = subprocess.run([nm, '-S', '-n', path], capture_output=True,
                         text=True, check=True).stdout
    entries = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4:
            addr, size, kind, name = parts
            size = int(size, 16)
        elif len(parts) == 3:
            addr, kind, name = parts
            size = 0
        else:
            continue
        if kind.lower() != 't' or name.startswith('$'):
            continue
        entries.append((int(addr, 16), name, size, ''))
    return Symbols(entries, path)


def from_map(path):
    entries = []
    obj = ''
    section = re.compile(r'^ (\.text|\.icode|\.init\.text|\.init)(\.\S*)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)')
    symbol = re.compile(r'^\s+0x([0-9a-f]+)\s+([A-Za-z_][A-Za-z0-9_.$]*)\s*$')
    in_code = False
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = section.match(line)
            if m:
                in_code = True
                obj = os.path.basename(m.group(5))
                start = int(m.group(3), 16)
                # the object's own start, named after it, for its statics
                entries.append((start, '(' + obj + ')', int(m.group(4), 16),
                                obj))
                continue
            if line.startswith(' .') or line.startswith('.'):
                in_code = False
                continue
            m = symbol.match(line)
            if m and in_code:
                entries.append((int(m.group(1), 16), m.group(2), 0, obj))
    return Symbols(entries, path)


def load_symbols(args, version):
    if args.elf:
        return from_elf(args.elf) or sys.exit('no arm-elf-eabi-nm to read ' + args.elf)
    if args.map:
        return from_map(args.map)
    folder = args.build or os.path.join(args.archive, version)
    elf = os.path.join(folder, 'rockbox.elf')
    if os.path.exists(elf):
        syms = from_elf(elf)
        if syms:
            return syms
    mp = os.path.join(folder, 'rockbox.map')
    if os.path.exists(mp):
        return from_map(mp)
    return None


def describe(syms, value, is_return):
    addr = value & ~1
    probe = addr - 1 if is_return else addr
    hit = syms.lookup(probe)
    if not hit:
        return '?'
    start, name, obj = hit
    text = f'{name}+0x{addr - start:x}'
    if obj and not name.startswith('('):
        text += f' ({obj})'
    if value & 1:
        text += ' [Thumb]'
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('log')
    ap.add_argument('--build', help='a build folder with rockbox.elf or .map')
    ap.add_argument('--elf')
    ap.add_argument('--map')
    ap.add_argument('--archive', default=ARCHIVE)
    ap.add_argument('--all', action='store_true', help='every record')
    args = ap.parse_args()

    records = read_records(args.log)
    if not records:
        sys.exit('no crash record in ' + args.log)
    for rec in records if args.all else records[-1:]:
        version = rec.get('version', '?')
        what = rec.get('what', '?')
        print(f'== {what}, version {version}, {rec.get("when", "?")}, '
              f'uptime {rec.get("uptime", "?")}')
        print(f'   message: {rec.get("message", "")}')
        print(f'   thread {rec.get("thread", "?")} on {rec.get("core", "?")}'
              + (f', count {rec["count"]}' if 'count' in rec else '')
              + (f', mended {rec["mended"]}' if 'mended' in rec else ''))
        syms = load_symbols(args, version)
        if not syms:
            print(f'   no rockbox.elf or rockbox.map for {version} '
                  f'(in {os.path.join(args.archive, version)})')
            continue
        print(f'   symbols: {syms.source}')

        def show(label, text, is_return):
            try:
                value = int(text, 16)
            except ValueError:
                return
            print(f'   {label:<6} {value:08x}  '
                  + (describe(syms, value, is_return) if value else '-'))

        show('pc', rec.get('pc', ''), what in PC_IS_RETURN)
        show('lr', rec.get('lr', ''), True)
        print(f'   sp     {rec.get("sp", "?")}')
        regs = rec.get('regs', '').split()
        for i, r in enumerate(regs):
            value = int(r, 16)
            hit = syms.lookup(value & ~1)
            if value >= 0x1000 and hit:   # small numbers aren't code
                print(f'   r{i:<5} {value:08x}  {describe(syms, value, False)}')
        for i, t in enumerate(rec.get('trace', '').split()):
            show(f'#{i}', t, True)


if __name__ == '__main__':
    main()
