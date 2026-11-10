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
ok. Known and left as they are: a rename cut inside fat_rename(), its two
entries in two sectors, can leave both on one file (cross) or, renamed over
another's name, that name twice (dupname).
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


def cut(fstest, scen, d, k):
    crash = os.path.join(d, "crash-%d.img" % k)
    post = os.path.join(d, "post-%d.img" % k)
    r = subprocess.run([fstest, "crash", scen, os.path.join(d, "base.img"),
                        os.path.join(d, "journal"), str(k), crash, post],
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


def scenario(fstest, scen, layout, seed, work, verbose, keep):
    """seed: the order the cache's slots are dealt in (fstest.c); the
    images kept only if keep, but for the errno check's"""
    name = "layout %d order %d" % (layout, seed)
    d = os.path.join(work, "%s-%d-%d" % (scen, layout, seed))
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    base = os.path.join(d, "base.img")
    fatimg.mkfs(base)
    sh(fstest, "setup", scen, base, str(layout))
    bad = kinds(fatimg.check(base))
    if bad:
        return "%s: setup left %s" % (name, ",".join(bad)), set()
    final = os.path.join(d, "final.img")
    sh("cp", "-c", base, final)
    n = int(sh(fstest, "action", scen, final, os.path.join(d, "journal"),
               str(seed)))
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        results = list(pool.map(lambda k: cut(fstest, scen, d, k),
                                range(n + 1)))
    if not keep:
        os.remove(final)
        if scen != "settings" or layout or seed:
            os.remove(base)
    crash, post, verdicts, notes = {}, {}, {}, set()
    for k, c, p, verdict in results:
        for kind in kinds(c):
            crash.setdefault(kind, []).append(k)
        for kind in kinds(p):
            post.setdefault(kind, []).append(k)
        if not verdict.startswith("ok"):
            verdicts.setdefault(verdict, []).append(k)
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
    return "; ".join(line) if len(line) > 1 else None, notes


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
    names = sh(fstest, "list").split()
    if args.only:
        names = [s for s in names if s in args.only.split(",")]
    for scen in names:
        found, notes = [], set()
        for layout in range(args.layouts):
            for seed in range(args.seeds):
                line, more = scenario(fstest, scen, layout, seed,
                                      os.path.join(work, name), args.verbose,
                                      args.keep)
                notes |= more
                if line:
                    found.append(line)
        runs = args.layouts * args.seeds
        print("%s: %d of %d runs clean%s" % (
            scen, runs - len(found), runs,
            " (also %s)" % ",".join(sorted(notes)) if notes else ""))
        for line in found:
            print("  " + line, flush=True)
    base = os.path.join(work, name, "settings-0-0", "base.img")
    if os.path.exists(base):
        print(sh(fstest, "errno", base), end="")


main()
