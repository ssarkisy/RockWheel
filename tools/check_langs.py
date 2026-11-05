#!/usr/bin/env python3
"""The phrases RockWheel added to or changed in english.lang, in every other
language of apps/lang: missing ones, and ones whose description, user, targets,
voice or format specifiers differ from English's. Run before a commit that
adds a phrase:

    git show $(git merge-base HEAD origin/master):apps/lang/english.lang > /tmp/base.lang
    python3 tools/check_langs.py apps/lang /tmp/base.lang

walon.lang is left out. Hindi and Thai, not offered in the language list, may
miss phrases: English's are shown for them.
"""
import re, sys, os, glob

def parse(path):
    phrases = {}
    order = []
    dups = []
    cur = None
    sect = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for ln, raw in enumerate(f, 1):
            line = raw.rstrip('\n').rstrip('\r')
            s = line.strip()
            if not s or s.startswith('#'):
                continue
            if s == '<phrase>':
                cur = {'line': ln, 'source': [], 'dest': [], 'voice': []}
                continue
            if s == '</phrase>':
                if cur is not None and 'id' in cur:
                    if cur['id'] in phrases:
                        dups.append((cur['id'], ln))
                    phrases[cur['id']] = cur
                    order.append(cur['id'])
                cur = None
                continue
            if cur is None:
                continue
            m = re.match(r'^<(/?)(source|dest|voice)>$', s)
            if m:
                sect = None if m.group(1) else m.group(2)
                continue
            if sect:
                m = re.match(r'^([^:]+):\s*(.*)$', s)
                if m:
                    cur[sect].append((m.group(1).strip(), m.group(2).strip()))
                else:
                    cur.setdefault('junk', []).append((ln, s))
            else:
                m = re.match(r'^(id|desc|user):\s*(.*)$', s)
                if m:
                    cur[m.group(1)] = m.group(2).strip()
                else:
                    cur.setdefault('junk', []).append((ln, s))
    return phrases, order, dups

FMT = re.compile(r'%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t)?[diouxXcspfeEgG%]')

def fmts(v):
    if not v.startswith('"'):
        return None
    return [x for x in FMT.findall(v) if x != '%%']

root = sys.argv[1]
base = sys.argv[2]
eng, eng_order, eng_dups = parse(os.path.join(root, 'english.lang'))
old, old_order, _ = parse(base)
new_ids = [i for i in eng_order if i not in old]
changed_ids = [i for i in eng_order if i in old and (old[i]['source'] != eng[i]['source'] or old[i].get('desc') != eng[i].get('desc') or old[i].get('user') != eng[i].get('user'))]
removed = [i for i in old_order if i not in eng]
# order check: old ids keep relative order and new ids appended?
old_in_new = [i for i in eng_order if i in old]
order_ok = old_in_new == [i for i in old_order if i in eng]
first_new_pos = min((eng_order.index(i) for i in new_ids), default=None)
inserted_mid = [i for i in new_ids if eng_order.index(i) < len(old_in_new) + 0 and any(eng_order.index(j) > eng_order.index(i) for j in old_in_new)]
print('english phrases', len(eng_order), 'base', len(old_order), 'new', len(new_ids), 'changed', len(changed_ids), 'removed', removed, 'order_ok', order_ok, 'eng dups', eng_dups)
print('new ids inserted before last old id:', len(inserted_mid), inserted_mid[:20])

# english self-consistency: source == dest
for i in new_ids + changed_ids:
    p = eng[i]
    if p['source'] != p['dest']:
        print('ENG source!=dest', i, p['source'], p['dest'])
    if 'desc' not in p or 'user' not in p:
        print('ENG missing desc/user', i)
    if not p['voice']:
        print('ENG no voice', i)

check = new_ids + changed_ids
langs = sorted(glob.glob(os.path.join(root, '*.lang')))
summary = {}
for path in langs:
    name = os.path.basename(path)
    if name in ('english.lang', 'walon.lang'):
        continue
    ph, order, dups = parse(path)
    issues = []
    if dups:
        issues.append(('DUP', dups))
    for i in check:
        e = eng[i]
        if i not in ph:
            issues.append(('MISSING', i))
            continue
        p = ph[i]
        if p.get('desc') != e.get('desc'):
            issues.append(('DESC', i, p.get('desc'), e.get('desc')))
        if p.get('user') != e.get('user'):
            issues.append(('USER', i, p.get('user'), e.get('user')))
        if p['source'] != e['source']:
            issues.append(('SOURCE', i, p['source'], e['source']))
        if 'junk' in p:
            issues.append(('JUNK', i, p['junk']))
        etargets = [t for t, v in e['source']]
        dtargets = [t for t, v in p['dest']]
        vtargets = [t for t, v in p['voice']]
        if dtargets != etargets:
            issues.append(('DEST_TARGETS', i, dtargets, etargets))
        if not p['voice']:
            issues.append(('NO_VOICE', i))
        elif vtargets != [t for t, v in e['voice']]:
            issues.append(('VOICE_TARGETS', i, vtargets, [t for t, v in e['voice']]))
        # format specifiers per target
        edict = dict(e['source'])
        for t, v in p['dest']:
            ev = edict.get(t)
            if ev is None:
                continue
            fe, fd = fmts(ev), fmts(v)
            if fe is not None and fd is not None and fe != fd:
                issues.append(('FMT', i, t, v, ev))
            if ev != 'none' and v == 'none':
                issues.append(('DEST_NONE', i, t))
            if ev == 'none' and v not in ('none',):
                issues.append(('DEST_NOT_NONE', i, t, v))
            if v == '""' and ev not in ('""', 'none'):
                issues.append(('DEST_EMPTY', i, t))
            if v.startswith('"') and not v.endswith('"'):
                issues.append(('QUOTE', i, t, v))
        # untranslated: dest == english dest for all-letter phrases
    # all ids in english present? (also older upstream ids)
    missing_all = [i for i in eng_order if i not in ph]
    extra = [i for i in order if i not in eng]
    summary[name] = (issues, missing_all, extra)

kinds = {}
for name, (issues, missing_all, extra) in summary.items():
    for it in issues:
        kinds.setdefault(it[0], []).append((name,) + tuple(it[1:]))
for k, v in kinds.items():
    print('==', k, len(v))
    for x in v[:25]:
        print('   ', x)
print('== per-lang missing (any english id), extra ids')
for name, (issues, missing_all, extra) in summary.items():
    fork_missing = [i for i in missing_all if i in new_ids]
    print(name, 'missing_any', len(missing_all), 'fork_missing', len(fork_missing), 'extra', len(extra), extra[:5])
