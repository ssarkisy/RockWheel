#!/usr/bin/env python3
"""The crash test of the file system, every scenario cut after each of its
writes in turn (see fstest.c, and fatimg.py for what is checked):

    python3 -I tools/fstest/run.py [--rev <commit>] [--only a,b] [--work <dir>]

--rev builds the file system of another commit instead of the working tree,
as the control that the test finds what it looks for. Each scenario runs over
--layouts layouts of .rockbox (that many more files in it each time, so its
entries fall elsewhere in the folder's sectors), each with the cache's slots
dealt in --seeds orders; --memory 4 gives a cache small enough to evict. The
images and builds go to tools/fstest/work unless --work says otherwise.

A line per scenario: in how many of its runs nothing was found, then a line
for each other run, by layout and order: what fatimg.py found on the disk as
the cut left it ("cut") and after the next boot wrote on it ("next boot"),
at which cuts k, and what the scenario's own check of its files said, if not
ok; with --outcomes, how many cuts left each outcome the scenario's check
found ok. Known and left as it is: renamed over another's name, a file has
that name twice (dupname) between its new entry and the other's removal.
After a scenario's runs, "leaks": in how many of them the whole scenario,
then everything removed, left clusters taken that a fresh volume has free.
Then, without cuts, what errno open() sets, and the FAT's sectors each write
past a full volume's last cluster reads (fstest full), and whether a second
stream of a file is right after its write failed there (fstest two).

Last, "damage" (--only damage for these alone): a file's or a folder's chain
or entry damaged by fatimg.py, then removed, written again or read. A line
each: what the operation returned, whether the other file is whole then and
after the next writer took all free space, the stop recorded (checked: its
kind, and that it is the only one), the clusters left taken, what fatimg.py
finds, and "stray" for anything written to the other files, their FAT
entries or the reserved sectors. Before it, "chain mark": the limit of a file
cut at a sync and then removed while open (fstest mark).
"""
import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
import fatimg  # noqa: E402


def sh(*args, **kw):
    return subprocess.run(args, check=True, capture_output=True, text=True,
                          **kw).stdout


def build(src, out, memory):
    shutil.rmtree(out, ignore_errors=True)
    sh("make", "-C", HERE, "SRC=" + src, "O=" + out, "MEMORYSIZE=%d" % memory)
    return os.path.join(out, "fstest")


def export(rev, work):
    src = os.path.join(work, "src-" + rev)
    if not os.path.isdir(src):
        os.makedirs(src)
        root = sh("git", "-C", HERE, "rev-parse", "--show-toplevel").strip()
        tar = subprocess.run(["git", "-C", root, "archive", rev, "firmware",
                              "apps/settings.c"], check=True,
                             capture_output=True).stdout
        subprocess.run(["tar", "-x", "-C", src], input=tar, check=True)
    return src


def kinds(found):
    return sorted(k for k in found if k in fatimg.ERRORS)


def cut(fstest, scen, d, k, one):
    crash = os.path.join(d, "crash-%d.img" % k)
    post = os.path.join(d, "post-%d.img" % k)
    r = subprocess.run([fstest, "crash", scen, os.path.join(d, "base.img"),
                        os.path.join(d, "journal"), str(k), crash, post, one],
                       capture_output=True, text=True)
    verdict = r.stdout.strip() if r.returncode == 0 else \
        "BAD post failed: " + (r.stderr.strip() or "exit %d" % r.returncode)
    c, p = fatimg.check(crash), fatimg.check(post)
    os.remove(crash)
    os.remove(post)
    return k, c, p, verdict


def ranges(ks):
    out, ks = [], sorted(ks)
    for k in ks:
        if out and out[-1][1] == k - 1:
            out[-1][1] = k
        else:
            out.append([k, k])
    return ",".join(str(a) if a == b else "%d-%d" % (a, b) for a, b in out)


def run(*args):
    """a line of a result, or of what went wrong"""
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return False, "no end after 120 s"
    if r.returncode == 0:
        return True, r.stdout.strip()
    return False, r.stderr.strip() or "exit %d" % r.returncode


def leak(fstest, img, fresh):
    """the clusters still taken once everything on a copy of img is removed,
    fresh being the free clusters of an empty volume"""
    wiped = img + ".wiped"
    sh("cp", "-c", img, wiped)
    ok, said = run(fstest, "wipe", wiped)
    free, info = fatimg.free_clusters(wiped)
    os.remove(wiped)
    if not ok:
        return "removing everything failed: " + said
    if free != fresh or info != fresh:
        return "%d clusters leaked (FSInfo says %d)" % (fresh - free,
                                                        fresh - info)
    return None


# For each damage and operation: the clusters left taken that no file has
# (those of the chain past the place where freeing stops, fat.h), the one
# stop it must record, and what fatimg.py may still find afterwards.
DAMAGE = (
    ("none", "remove", 0, None), ("none", "trunc", 0, None),
    ("none", "rmdir", 0, None),
    ("one", "remove", 2, "range"), ("one", "trunc", 2, "range"),
    ("beyond", "remove", 2, "range"), ("beyond", "trunc", 2, "range"),
    ("cross", "remove", 0, "limit"), ("cross", "trunc", 0, "limit"),
    ("long", "remove", 2, "limit"), ("long", "trunc", 2, "limit"),
    ("loop", "remove", 0, "limit"), ("loop", "trunc", 0, "limit"),
    # a short count and no bytes of another file; size: the file is as it was
    ("short", "read", 0, None, "size"), ("short", "trunc", 0, None),
    ("short", "remove", 0, None),
    # the entry's first cluster is no cluster: the file's own four are lost
    # to it already; it is removed, or written again from a new chain
    ("first-one", "remove", 4, "range"), ("first-one", "trunc", 4, "range"),
    ("first-one-small", "trunc", 4, "range"),
    ("first-one-small", "cfg", 4, "range"),
    ("first-one-empty", "touch", 4, "range"),
    ("first-beyond", "remove", 4, "range"),
    ("first-beyond", "trunc", 4, "range"),
    ("first-beyond-small", "trunc", 4, "range"),
    ("first-beyond-empty", "touch", 4, "range"),
    ("dir-beyond", "rmdir", 0, "range"),
    # the cluster a cut would keep as the end is no cluster: refused, and
    # nothing written; the file stays as damaged as it was. (No such row for
    # a link outside the volume: the check of fstest's own fat.c, TEST_FAT,
    # walks that chain for ever at the close.)
    ("none", "cut", 0, None),
    ("one-mid", "cut", 2, "range", "badref,size",
     "ftruncate failed (errno 5)"),
)
DAMAGE_FILE = "/.rockbox/victim.dat"
DAMAGE_DIR = "/.rockbox/emptydir"
DAMAGE_OTHER = "/.rockbox/bystander.dat"


def damage(fstest, work):
    d = os.path.join(work, "damage")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    base = os.path.join(d, "base.img")
    fatimg.mkfs(base)
    sh(fstest, "setup", "damage", base)
    for kind, op, lost, stop, *more in DAMAGE:
        before = os.path.join(d, "%s-%s-before.img" % (kind, op))
        after = os.path.join(d, "%s-%s-after.img" % (kind, op))
        sh("cp", "-c", base, before)
        fatimg.damage(before, kind, DAMAGE_DIR if op == "rmdir" else
                      DAMAGE_FILE, DAMAGE_OTHER)
        sh("cp", "-c", before, after)
        ok, said = run(fstest, "damage", op, after)
        line = [said]
        found = fatimg.check(after)
        stray = fatimg.strays(before, after, (DAMAGE_FILE, DAMAGE_DIR))
        if ok:
            ok = (more[1] in said if more[1:] else said.startswith("ok:")) \
                and "NOT WHOLE" not in said
            # a tree without the record prints none
            want = "stops: " + ({"range": "1 range, 0 limit",
                                 "limit": "0 range, 1 limit"}.get(stop, "none"))
            if "stops: " in said and want not in said:
                line.append("WRONG STOPS, expected " + want[7:])
                ok = False
            left = int(found.get("lost", ["0"])[0].split()[0])
            line.append("%d clusters left taken" % left)
            ok = ok and left == lost
            filled, other = run(fstest, "fill", after)
            line.append("after the next writer the other file is "
                        + (other if filled else "unknown: " + other))
            ok = ok and filled and other == "whole"
        found = sorted(k for k in found if k not in ("lost", "fatcopy"))
        if found:
            line.append("check: " + ",".join(found))
        if stray:
            line.append("stray: " + "; ".join(stray[:3])
                        + (" and %d more" % (len(stray) - 3)
                           if len(stray) > 3 else ""))
        ok = ok and ",".join(found) == "".join(more[:1]) and not stray
        print("damage %s, %s: %s\n  %s" % (kind, op, "ok" if ok else "BAD",
                                           "; ".join(line)), flush=True)
        os.remove(before)
        os.remove(after)
    os.remove(base)


def scenario(fstest, scen, layout, seed, work, verbose, keep):
    """seed: the order the cache's slots are dealt in (fstest.c); the
    images kept only if keep, but for the errno check's"""
    name = "layout %d order %d" % (layout, seed)
    d = os.path.join(work, "%s-%d-%d" % (scen, layout, seed))
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    base = os.path.join(d, "base.img")
    fatimg.mkfs(base)
    fresh = fatimg.free_clusters(base)[0]
    sh(fstest, "setup", scen, base, str(layout))
    bad = kinds(fatimg.check(base))
    if bad:
        return "%s: setup left %s" % (name, ",".join(bad)), set(), {}, False
    final = os.path.join(d, "final.img")
    sh("cp", "-c", base, final)
    ok, said = run(fstest, "action", scen, final, os.path.join(d, "journal"),
                   str(seed))
    if not ok:
        return "%s: the scenario failed: %s" % (name, said), set(), {}, False
    n, one = said.split()
    n = int(n)
    leaked = leak(fstest, final, fresh)
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        results = list(pool.map(lambda k: cut(fstest, scen, d, k, one),
                                range(n + 1)))
    if not keep:
        os.remove(final)
        if scen != "settings" or layout or seed:
            os.remove(base)
    crash, post, verdicts, notes, outcomes = {}, {}, {}, set(), {}
    for k, c, p, verdict in results:
        for kind in kinds(c):
            crash.setdefault(kind, []).append(k)
        for kind in kinds(p):
            post.setdefault(kind, []).append(k)
        if not verdict.startswith("ok"):
            verdicts.setdefault(verdict, []).append(k)
        elif verdict[3:]:
            outcomes[verdict[3:]] = outcomes.get(verdict[3:], 0) + 1
        notes.update(x for x in c if x not in fatimg.ERRORS)
        if verbose:
            print("  k=%d crash %s post %s: %s" % (k, c, p, verdict))
    line = ["%s, %d writes" % (name, n)]
    for kind, ks in sorted(crash.items()):
        line.append("cut: %s at k=%s" % (kind, ranges(ks)))
    for kind, ks in sorted(post.items()):
        line.append("next boot: %s at k=%s" % (kind, ranges(ks)))
    for verdict, ks in sorted(verdicts.items()):
        line.append("%s at k=%s" % (verdict, ranges(ks)))
    return "; ".join(line) if len(line) > 1 else None, notes, outcomes, \
        leaked and "%s: %s" % (name, leaked)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--rev")
    ap.add_argument("--only")
    ap.add_argument("--work", default=os.path.join(HERE, "work"))
    ap.add_argument("--layouts", type=int, default=8)
    ap.add_argument("--seeds", type=int, default=4)
    ap.add_argument("--memory", type=int, default=32,
                    help="MB of the target the disk cache is sized for: "
                    "below 8 it has 32 sectors, so it evicts")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--keep", action="store_true",
                    help="keep each scenario's images before and after")
    ap.add_argument("--outcomes", action="store_true",
                    help="count the outcomes the scenarios' checks found ok")
    args = ap.parse_args()
    work = os.path.abspath(args.work)
    os.makedirs(work, exist_ok=True)
    if args.rev:
        src, name = export(args.rev, work), args.rev
    else:
        src, name = os.path.abspath(os.path.join(HERE, "..", "..")), "tree"
    if args.memory != 32:
        name += "-%dmb" % args.memory
    fstest = build(src, os.path.join(work, "build-" + name), args.memory)
    os.makedirs(os.path.join(work, name), exist_ok=True)
    names = sh(fstest, "list").split()
    if args.only:
        names = [s for s in names if s in args.only.split(",")]
    for scen in names:
        found, notes, outcomes, leaks, checked = [], set(), {}, [], 0
        for layout in range(args.layouts):
            for seed in range(args.seeds):
                line, more, seen, leaked = scenario(
                    fstest, scen, layout, seed, os.path.join(work, name),
                    args.verbose, args.keep)
                notes |= more
                for o, n in seen.items():
                    outcomes[o] = outcomes.get(o, 0) + n
                if line:
                    found.append(line)
                checked += leaked is not False
                if leaked:
                    leaks.append(leaked)
        runs = args.layouts * args.seeds
        print("%s: %d of %d runs clean%s" % (
            scen, runs - len(found), runs,
            " (also %s)" % ",".join(sorted(notes)) if notes else ""))
        if args.outcomes and outcomes:
            print("  outcomes: " + (", ".join(
                "%s %d" % o for o in sorted(outcomes.items()))
                if len(outcomes) <= 8 else "%d different" % len(outcomes)))
        for line in found:
            print("  " + line, flush=True)
        print("  leaks: none in %d of %d runs" % (checked - len(leaks),
                                                 checked))
        for line in leaks:
            print("    " + line, flush=True)
    base = os.path.join(work, name, "settings-0-0", "base.img")
    if os.path.exists(base):
        print(sh(fstest, "errno", base), end="")
        # and of a create on a full volume, in a folder that is full too
        img = os.path.join(work, name, "enospc.img")
        fatimg.mkfs(img)
        sh(fstest, "setup", "dirgrow", img)
        print(sh(fstest, "enospc", img), end="")
        os.remove(img)
    if "full" in names:
        # a FAT of 17 sectors: the reads of each write past the last cluster
        img = os.path.join(work, name, "full.img")
        fatimg.mkfs(img, 2048)
        print(sh(fstest, "full", img), end="")
        os.remove(img)
    if not args.only or "two" in args.only.split(","):
        # two streams of one file, the write of one failing (fstest two)
        img = os.path.join(work, name, "two.img")
        fatimg.mkfs(img)
        sh(fstest, "setup", "full", img)
        ok, said = run(fstest, "two", img)
        found = fatimg.check(img)
        print("%s%s%s" % ("" if ok else "two streams: BAD: ", said,
                          "; check: " + ",".join(sorted(found))
                          if found else ""))
        os.remove(img)
    if not args.only or "mark" in args.only.split(","):
        # the limit of a file cut at a sync and removed while open
        img = os.path.join(work, name, "mark.img")
        fatimg.mkfs(img)
        sh(fstest, "setup", "damage", img)
        ok, said = run(fstest, "mark", img)
        if ok and "stops: " in said and "stops: 0 range, 1 limit" not in said:
            said = said.replace("chain mark: ok", "chain mark: BAD (stops)")
        found = kinds(fatimg.check(img))
        print("%s%s%s" % ("" if ok else "chain mark: BAD: ", said,
                          "; check: " + ",".join(found) if found else ""))
        os.remove(img)
    if not args.only or "damage" in args.only.split(","):
        damage(fstest, os.path.join(work, name))


main()
