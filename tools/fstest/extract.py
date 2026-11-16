#!/usr/bin/env python3
"""Copies the functions of apps/settings.c that save and recover config.cfg
and .resume.cfg into a file fstest.c includes, so the crash test runs them as
they are, and defines HAVE_<name> for each one found; with a third argument,
apps/file_write.c too, whole but for its #include "..." lines (fstest.c has
the headers), after apps/file_write.h, and HAVE_FILE_WRITE:

    python3 -I extract.py ../../apps/settings.c settings_fns.c [file_write.c]
"""
import os
import re
import sys

NAMES = ("rename_temp_file", "settings_file_whole", "settings_file_replace",
         "settings_file_recover", "settings_save_new")


def extract(lines, name):
    start = re.compile(r"^static\b[^;]*\b%s\(" % name)
    for i, line in enumerate(lines):
        if start.match(line):
            for j in range(i, len(lines)):
                if lines[j].rstrip("\n") == "}":
                    return i, lines[i:j + 1]
    return None, None


def without_includes(path):
    with open(path, encoding="utf-8") as f:
        lines = f.readlines()
    out = ['#line 1 "%s"\n' % path]
    for line in lines:
        out.append("\n" if line.startswith('#include "') else line)
    return out


def main():
    src, out = sys.argv[1], sys.argv[2]
    with open(src, encoding="utf-8") as f:
        lines = f.readlines()
    parts = []
    if len(sys.argv) > 3 and sys.argv[3]:
        fw = sys.argv[3]
        parts.append("#define HAVE_FILE_WRITE\n")
        parts.extend(without_includes(os.path.splitext(fw)[0] + ".h"))
        parts.extend(without_includes(fw))
    for name in NAMES:
        at, body = extract(lines, name)
        if body:
            parts.append("#define HAVE_%s\n" % name.upper())
            parts.append('#line %d "%s"\n' % (at + 1, src))
            parts.extend(body)
    with open(out, "w", encoding="utf-8") as f:
        f.writelines(parts)


main()
