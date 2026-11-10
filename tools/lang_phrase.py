#!/usr/bin/env python3
"""lang_phrase.py: adds RockWheel's phrases to, or changes them in, english.lang
and every other apps/lang/*.lang but walon.lang in one step, written as the
fork writes them. Run with python3 -I.

    python3 -I tools/lang_phrase.py BATCH [--fit VALUES [--name ID]] [--dry-run]
    python3 -I tools/lang_phrase.py --check
    python3 -I tools/lang_phrase.py --fit LANG_ON,LANG_OFF --name LANG_X

BATCH, a .json (see tools/lang_phrase.example.json) or a .tsv, lists phrases:
  id, desc, user (core by default), english (the source), voice (English's,
  "" by default), translations {file name: text} for every language file but
  english and walon (english-us included), short {file name: text} where
  needed, voices {file name: text} if a voice isn't the default.
  With "after": a new phrase, put right after that id (or, in a file without
  it, after the nearest id before it in english.lang). Every language must
  have its text, or nothing is written.
  Without "after": a change of an existing phrase, of the fields given: the
  translations listed (a voice equal to the old text follows it), and desc,
  user, english or voice in every file. No other line moves.
  A translation's voice is "" when English's is "", else its full text.
A .tsv has one line per field: id<TAB>key<TAB>value, the keys as above, a
language's short form and voice as <file>.short and <file>.voice; # comments.

--fit VALUES (ids such as LANG_ON,LANG_OFF, or literal values): the setting's
name (--name, or the batch's one phrase that isn't a value, or those with short
forms) measured beside each value in each language, as
~/ipod-data/archive/work/settings-1.3/value_fit.py does (24-Inter-Bold of
themes/fonts, a row's room --room, 275 px in Settings).
Where a value would hide, the short form is written; with no short form that
fits, nothing is written. Without BATCH, it only reports.

--check: languages missing a fork phrase, extra or duplicate ids, empty
translations, then tools/check_langs.py against HEAD's english.lang. Exit 1 on
a problem. --lang-dir DIR works on a copy of apps/lang.
"""
import argparse
import glob
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP = 'walon'
# Hindi and Thai aren't offered in the language list: older fork phrases may
# be missing there (English is shown), as tools/check_langs.py allows
NOT_OFFERED = ('hindi', 'thai')
# every row of Settings had 286 px for its name and value in 1.3's English
# run (value_fit.py's VALSLACK log: slack + name + value, 55 rows); 275 in a
# list longer than the screen, beside the scrollbar's 11
SETTINGS_ROOM = 275
FMT = re.compile(r'%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t)?[diouxXcspfeEgG%]')
SECTION = re.compile(r'^<(/?)(source|dest|voice)>$')


def fmts(s):
    return [x for x in FMT.findall(s) if x != '%%']


def quoted(text):
    return '"%s"' % text


def unquote(v):
    return v[1:-1] if v and len(v) >= 2 and v[0] == '"' and v[-1] == '"' else v


class LangFile:
    """a .lang file as its lines, edited in place: nothing else moves"""

    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)[:-5]
        with open(path, encoding='utf-8', newline='') as f:
            self.orig = f.read()
        self.lines = self.orig.splitlines(keepends=True)
        self.nl = '\r\n' if self.lines and self.lines[0].endswith('\r\n') else '\n'
        self.added, self.changed, self.notes = [], [], []
        self.reindex()

    def reindex(self):
        self.blocks, self.order = {}, []
        start = pid = None
        for n, line in enumerate(self.lines):
            s = line.strip()
            if s == '<phrase>':
                start, pid = n, None
            elif start is not None and pid is None and s.startswith('id:'):
                pid = s[3:].strip()
            elif s == '</phrase>' and start is not None:
                if pid:
                    self.blocks.setdefault(pid, []).append((start, n + 1))
                    self.order.append(pid)
                start = None

    def has(self, pid):
        return pid in self.blocks

    def span(self, pid):
        spans = self.blocks[pid]
        if len(spans) > 1:
            raise ValueError('%s.lang has %s %d times' % (self.name, pid, len(spans)))
        return spans[0]

    def find(self, pid, section, target='*'):
        """the line number and value of a target's line in a section (None for
        the id, desc and user lines)"""
        start, end = self.span(pid)
        sect = None
        for n in range(start + 1, end - 1):
            s = self.lines[n].strip()
            m = SECTION.match(s)
            if m:
                sect = None if m.group(1) else m.group(2)
                continue
            m = re.match(r'^([^:]+):\s*(.*)$', s)
            if m and sect == section and m.group(1).strip() == target:
                return n, m.group(2).strip()
        return None, None

    def get(self, pid, section, target='*'):
        return self.find(pid, section, target)[1] if self.has(pid) else None

    def text(self, pid):
        v = self.get(pid, 'dest')
        return None if v is None or v == 'none' else unquote(v)

    def set(self, n, value):
        line = self.lines[n]
        body = line.rstrip('\r\n')
        self.lines[n] = re.match(r'^(\s*[^:]+:\s*)', body).group(1) + value + line[len(body):]

    def insert_after(self, anchor, pid, lines):
        _, end = self.span(anchor)
        self.lines[end:end] = [l + self.nl for l in lines]
        self.reindex()

    def content(self):
        return ''.join(self.lines)


def block(pid, desc, user, source, dest, voice):
    return ['<phrase>', '  id: ' + pid, '  desc: ' + desc, '  user: ' + user,
            '  <source>', '    *: ' + source, '  </source>',
            '  <dest>', '    *: ' + dest, '  </dest>',
            '  <voice>', '    *: ' + voice, '  </voice>', '</phrase>']


# the batch

FIELDS = ('id', 'desc', 'user', 'english', 'voice', 'after')


def load_batch(path):
    if path.endswith('.tsv'):
        phrases, by_id = [], {}
        with open(path, encoding='utf-8') as f:
            for ln, raw in enumerate(f, 1):
                line = raw.rstrip('\r\n')
                if not line.strip() or line.lstrip().startswith('#'):
                    continue
                parts = line.split('\t', 2)
                if len(parts) < 2:
                    sys.exit('%s:%d: id<TAB>key<TAB>value expected' % (path, ln))
                pid, key = parts[0].strip(), parts[1].strip()
                value = parts[2] if len(parts) > 2 else ''
                p = by_id.get(pid)
                if p is None:
                    p = by_id[pid] = {'id': pid}
                    phrases.append(p)
                if key in FIELDS:
                    p[key] = value
                elif key.endswith('.short'):
                    p.setdefault('short', {})[key[:-6]] = value
                elif key.endswith('.voice'):
                    p.setdefault('voices', {})[key[:-6]] = value
                else:
                    p.setdefault('translations', {})[key] = value
        return phrases
    with open(path, encoding='utf-8') as f:
        data = json.load(f)
    phrases = data['phrases'] if isinstance(data, dict) else data
    for p in phrases:
        unknown = set(p) - set(FIELDS) - {'translations', 'short', 'voices'}
        if unknown:
            sys.exit('%s: unknown keys %s' % (p.get('id'), sorted(unknown)))
    return phrases


def validate(phrases, files, en, errors):
    langs = [n for n in files if n not in ('english', SKIP)]
    seen = set()
    for p in phrases:
        pid = p.get('id', '')
        if not re.match(r'^LANG_[A-Z0-9_]+$', pid):
            errors.append('bad id %r' % pid)
            continue
        if pid in seen:
            errors.append('%s twice in the batch' % pid)
        seen.add(pid)
        adding = 'after' in p
        tr = p.setdefault('translations', {})
        p.setdefault('short', {})
        p.setdefault('voices', {})
        if adding:
            p.setdefault('user', 'core')
            p.setdefault('voice', '')
            for k in ('desc', 'english'):
                if not p.get(k):
                    errors.append('%s: no %s' % (pid, k))
            if en.has(pid):
                errors.append('%s is in english.lang already (leave out "after" to change it)' % pid)
            missing = [l for l in langs if not tr.get(l, '').strip()]
            if missing:
                errors.append('%s: no translation for %s' % (pid, ', '.join(missing)))
        elif not en.has(pid):
            errors.append('%s is not in english.lang (give "after" to add it)' % pid)
        for k in ('desc', 'user'):
            if k in p and ('\n' in p[k] or not p[k].strip()):
                errors.append('%s: %s must be one non-empty line' % (pid, k))
        english = p.get('english', en.text(pid) if en.has(pid) else '') or ''
        for kind in ('translations', 'short', 'voices'):
            for lang, text in p[kind].items():
                if lang == SKIP:
                    print('note: %s %s for walon left out' % (pid, kind))
                    continue
                if lang == 'english':
                    errors.append('%s: English goes in "english" and "voice"' % pid)
                elif lang not in files:
                    errors.append('%s: no %s.lang' % (pid, lang))
                elif kind != 'voices' and not text.strip():
                    errors.append('%s: empty %s for %s' % (pid, kind, lang))
                elif kind != 'voices' and fmts(text) != fmts(english):
                    errors.append('%s: %s %s has %s, English %s'
                                  % (pid, lang, 'text' if kind == 'translations' else 'short form',
                                     fmts(text), fmts(english)))
        for k, v in [('english', p.get('english', '')), ('voice', p.get('voice', ''))] + \
                [(l, t) for kind in ('translations', 'short', 'voices') for l, t in p[kind].items()]:
            if '"' in v or '\n' in v or '\t' in v:
                errors.append('%s: %s has a quote, tab or newline: %r' % (pid, k, v))


def apply(phrases, files, en, errors):
    """the batch, with full forms, into every file but walon"""
    targets = [f for n, f in sorted(files.items()) if n != SKIP]
    targets.sort(key=lambda f: f.name != 'english')  # english.lang first
    for p in phrases:
        pid = p['id']
        tr = p['translations']
        if 'after' in p:
            if not en.has(p['after']):
                errors.append('%s: no %s in english.lang to put it after' % (pid, p['after']))
                continue
            for f in targets:
                if f.has(pid):
                    errors.append('%s.lang has %s already' % (f.name, pid))
                    continue
                if f is en:
                    dest, voice = p['english'], p['voice']
                else:
                    dest = tr[f.name]
                    voice = p['voices'].get(f.name, dest if p['voice'] else '')
                anchor = p['after']
                if not f.has(anchor):
                    order = en.order
                    i = order.index(anchor)
                    anchor = next((x for x in reversed(order[:i]) if f.has(x)), None)
                    if anchor is None:
                        errors.append('%s.lang has nothing before %s' % (f.name, p['after']))
                        continue
                    f.notes.append('%s after %s (no %s)' % (pid, anchor, p['after']))
                try:
                    f.insert_after(anchor, pid, block(pid, p['desc'], p['user'], quoted(p['english']),
                                                      quoted(dest), quoted(voice)))
                except ValueError as e:
                    errors.append(str(e))
                    continue
                f.added.append(pid)
        else:
            for f in targets:
                if not f.has(pid):
                    if f.name in tr or f is en:
                        errors.append('%s.lang has no %s to change' % (f.name, pid))
                    continue
                try:
                    what = change(f, p, en)
                except ValueError as e:
                    errors.append(str(e))
                    continue
                if what:
                    f.changed.append('%s (%s)' % (pid, ', '.join(what)))


def change(f, p, en):
    pid, what = p['id'], []
    for k in ('desc', 'user'):
        if k in p:
            start, end = f.span(pid)
            for n in range(start, end):
                m = re.match(r'^\s*%s:\s*(.*)$' % k, f.lines[n].rstrip('\r\n'))
                if m and m.group(1) != p[k]:
                    f.set(n, p[k])
                    what.append(k)
    if 'english' in p:
        n, old = f.find(pid, 'source')
        if n is None:
            raise ValueError('%s.lang: %s has no *: source' % (f.name, pid))
        if old != quoted(p['english']):
            f.set(n, quoted(p['english']))
            what.append('source')
    if f is en:
        new_dest, new_voice = p.get('english'), p.get('voice')
    else:
        new_dest, new_voice = p['translations'].get(f.name), p['voices'].get(f.name)
    if new_dest is not None:
        n, old = f.find(pid, 'dest')
        if n is None:
            raise ValueError('%s.lang: %s has no *: dest' % (f.name, pid))
        if old != quoted(new_dest):
            f.set(n, quoted(new_dest))
            what.append('dest')
            vn, vold = f.find(pid, 'voice')
            if new_voice is None and vn is not None and vold == old and old != '""':
                new_voice = new_dest
    if new_voice is not None:
        n, old = f.find(pid, 'voice')
        if n is None:
            raise ValueError('%s.lang: %s has no *: voice' % (f.name, pid))
        if old != quoted(new_voice):
            f.set(n, quoted(new_voice))
            what.append('voice')
    return what


# the fit beside a setting's value

class Measure:
    """value_fit.py's measure: 24-Inter-Bold, through tools/check_hints.py"""

    def __init__(self):
        spec = importlib.util.spec_from_file_location(
            'check_hints', os.path.join(ROOT, 'tools', 'check_hints.py'))
        self.ch = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.ch)
        self.ch.DIACRITICS.extend(self.ch.diacritics(ROOT))
        self.ch.ARAB.append(self.ch.Arab(ROOT))
        self.ch.FORMS.append(self.ch.Forms(ROOT))
        self.cache = {}

    def slack(self, lang, name, values, room):
        """the least room left beside the name, of all the values"""
        fs = self.cache.get(lang) or self.cache.setdefault(lang, self.ch.Fonts(
            os.path.join(ROOT, 'themes/fonts/.rockbox/fonts'), '24-Inter-Bold.fnt', lang))
        return min(room - fs.text_width(name) - fs.text_width(v) for v in values)


def value_texts(f, en, values):
    out = []
    for v in values:
        if re.match(r'^LANG_[A-Z0-9_]+$', v):
            t = f.text(v) if f.has(v) else None
            t = t if t else en.text(v)
            if t is None:
                raise SystemExit('no %s in english.lang' % v)
            out.append(t)
        else:
            out.append(v)
    return out


def fit(files, en, names, values, room, shorts, errors, report_only):
    """each name beside the values, in each language; a short form where a
    value would hide. Returns the number of languages where one still hides."""
    m = Measure()
    hidden = 0
    for pid in names:
        used = []
        print('fit %s beside %s, room %d px:' % (pid, ' / '.join(values), room))
        for name, f in sorted(files.items()):
            if name == SKIP or not f.has(pid):
                continue
            text = f.text(pid)
            vals = value_texts(f, en, values)
            s = m.slack(name, text, vals, room)
            if s >= 0:
                continue
            short = shorts.get(pid, {}).get(name)
            line = '  %-22s %4d  %s' % (name, s, text)
            if short and not report_only:
                s2 = m.slack(name, short, vals, room)
                if s2 >= 0:
                    n, _ = f.find(pid, 'dest')
                    f.set(n, quoted(short))
                    f.notes.append('%s short form' % pid)
                    used.append(name)
                    print(line + '  -> short %+d  %s' % (s2, short))
                    continue
                line += '  -> short %+d  %s, still hidden' % (s2, short)
            elif not report_only:
                line += '  -> no short form'
            hidden += 1
            print(line)
            if not report_only:
                errors.append('%s: the value hides beside it in %s' % (pid, name))
        print('  %d languages take the short form%s' % (len(used), (': ' + ', '.join(used)) if used else ''))
    return hidden


# --check

def git(*args):
    r = subprocess.run(['git', '-C', ROOT] + list(args), capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def ids_of(text):
    return re.findall(r'^\s*id:\s*(\S+)\s*$', text or '', re.M)


def dups(order):
    seen, out = set(), []
    for i in order:
        if i in seen and i not in out:
            out.append(i)
        seen.add(i)
    return out


def check(lang_dir):
    files = {os.path.basename(p)[:-5]: LangFile(p)
             for p in sorted(glob.glob(os.path.join(lang_dir, '*.lang')))}
    en = files['english']
    head_en = git('show', 'HEAD:apps/lang/english.lang')
    mb = (git('merge-base', 'HEAD', 'origin/master') or '').strip()
    base_en = git('show', mb + ':apps/lang/english.lang') if mb else None
    if base_en is None:
        print('note: no merge base with origin/master; every id counts as the fork\'s')
    upstream = set(ids_of(base_en))
    fork = [i for i in en.order if i not in upstream]
    new = set(i for i in en.order if i not in set(ids_of(head_en)))
    en_set = set(en.order)
    problems = 0
    lines = []
    d = [i for i in dups(en.order) if i not in dups(ids_of(base_en))]
    if d:
        lines.append('english.lang: duplicate %s' % ', '.join(d))
    old_gaps = {}
    for name, f in sorted(files.items()):
        if name in ('english', SKIP):
            continue
        out = []
        missing = [i for i in fork if not f.has(i)]
        if name in NOT_OFFERED:
            old = [i for i in missing if i not in new]
            missing = [i for i in missing if i in new]
            if old:
                old_gaps[name] = len(old)
        if missing:
            out.append('missing %s' % ', '.join(missing))
        base_f = git('show', '%s:apps/lang/%s.lang' % (mb, name)) if mb else None
        base_ids = set(ids_of(base_f))
        extra = [i for i in f.order if i not in en_set and i not in base_ids]
        if extra:
            out.append('extra %s' % ', '.join(extra))
        d = [i for i in dups(f.order) if i not in dups(ids_of(base_f))]
        if d:
            out.append('duplicate %s' % ', '.join(d))
        empty = []
        for i in fork:
            if f.has(i) and len(f.blocks[i]) == 1 and en.text(i):
                if not (f.text(i) or '').strip():
                    empty.append(i)
        if empty:
            out.append('empty %s' % ', '.join(empty))
        if out:
            problems += 1
            lines.append('%s.lang: %s' % (name, '; '.join(out)))
    print('fork phrases %d (%d new since HEAD), languages %d' % (len(fork), len(new), len(files) - 2))
    for l in lines:
        print('  ' + l)
    if old_gaps:
        print('  %s: %s older fork phrases missing, as allowed (not offered, English shown)'
              % (', '.join(old_gaps), '/'.join(str(n) for n in old_gaps.values())))
    if not lines:
        print('  no language misses a fork phrase; no extra, duplicate or empty ones')
    # tools/check_langs.py against HEAD's english.lang
    if head_en is None:
        print('no HEAD:apps/lang/english.lang; check_langs.py not run')
        return 1
    fd, base = tempfile.mkstemp(suffix='.lang')
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as t:
            t.write(head_en)
        r = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'check_langs.py'),
                            lang_dir, base], capture_output=True, text=True)
    finally:
        os.unlink(base)
    print('check_langs.py against HEAD:')
    quiet = 0
    kinds = []
    for l in r.stdout.splitlines():
        if re.search(r' fork_missing 0 ', l):
            quiet += 1
            continue
        m = re.match(r'^== (\w+) ', l)
        if m:
            kinds.append(m.group(1))
        print('  ' + l)
    print('  (%d languages miss none of the new phrases)' % quiet)
    if r.returncode or r.stderr:
        print(r.stderr)
        problems += 1
    # chinese-trad.lang's duplicates come from upstream, and every run lists them
    problems += len([k for k in kinds if k != 'DUP'])
    return 1 if problems else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('batch', nargs='?')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--fit', metavar='VALUES')
    ap.add_argument('--name', metavar='ID')
    ap.add_argument('--room', type=int, default=SETTINGS_ROOM)
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--lang-dir', default=os.path.join(ROOT, 'apps', 'lang'))
    a = ap.parse_args()
    if not (a.batch or a.check or a.fit):
        ap.print_help()
        return 2
    status = 0
    values = [v.strip() for v in a.fit.split(',') if v.strip()] if a.fit else []
    if a.batch or a.fit:
        files = {os.path.basename(p)[:-5]: LangFile(p)
                 for p in sorted(glob.glob(os.path.join(a.lang_dir, '*.lang')))}
        en = files['english']
        errors = []
        phrases = []
        if a.batch:
            phrases = load_batch(a.batch)
            validate(phrases, files, en, errors)
            if not errors:
                apply(phrases, files, en, errors)
        if a.fit and not errors:
            if a.name:
                names = [a.name]
            else:
                cand = [p['id'] for p in phrases if p['id'] not in values]
                names = cand if len(cand) == 1 else [p['id'] for p in phrases if p.get('short')]
            if not names:
                sys.exit('--fit: which phrase is the setting\'s name? Give --name')
            for n in names:
                if not en.has(n):
                    sys.exit('--fit: no %s in english.lang' % n)
            shorts = {p['id']: p.get('short', {}) for p in phrases}
            if fit(files, en, names, values, a.room, shorts, errors, not a.batch):
                status = 1
        if errors:
            print('nothing written:')
            for e in errors:
                print('  ' + e)
            return 1
        if a.batch:
            written = 0
            for name, f in sorted(files.items()):
                new = f.content()
                if new == f.orig:
                    continue
                parts = []
                if f.added:
                    parts.append('added %s' % ', '.join(f.added))
                if f.changed:
                    parts.append('changed %s' % ', '.join(f.changed))
                parts += f.notes
                print('%-26s %s' % (name + '.lang', '; '.join(parts)))
                if not a.dry_run:
                    with open(f.path, 'w', encoding='utf-8', newline='') as out:
                        out.write(new)
                written += 1
            print('%d files %s' % (written, 'would change (dry run)' if a.dry_run else 'written'))
    if a.check:
        status |= check(a.lang_dir)
    return status


if __name__ == '__main__':
    sys.exit(main())
