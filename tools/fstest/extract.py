#!/usr/bin/env python3
"""Copies the functions of apps/settings.c that save and recover config.cfg
and .resume.cfg into a file fstest.c includes, so the crash test runs them as
they are, and defines HAVE_<name> for each one found:

    python3 -I extract.py ../../apps/settings.c settings_fns.c
"""
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


def main():
    src, out = sys.argv[1], sys.argv[2]
    with open(src, encoding="utf-8") as f:
        lines = f.readlines()
    parts = []
    for name in NAMES:
        at, body = extract(lines, name)
        if body:
            parts.append("#define HAVE_%s\n" % name.upper())
            parts.append('#line %d "%s"\n' % (at + 1, src))
            parts.extend(body)
    with open(out, "w", encoding="utf-8") as f:
        f.writelines(parts)


main()
