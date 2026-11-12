#!/usr/bin/env python3
"""FAT32 images for the crash test: makes one, and checks one the way fsck
would, without trusting the file system that wrote it.

    python3 -I fatimg.py mkfs <img> [clusters]
    python3 -I fatimg.py check <img>
    python3 -I fatimg.py damage <img> <kind> <file> <other file>

The geometry is the owner's iPod's: 512-byte sectors, 32 KB clusters. The
free clusters are filled with STALE, so a directory cluster read before it
was ever written shows as such rather than as an empty folder.

Findings, by kind:
  free      an entry or a chain reaches a cluster the FAT calls free: the next
            file written takes it, and two files share it
  cross     a cluster in two chains: two files on the same clusters
  loop      a chain that comes back on itself
  badref    a chain or an entry pointing outside the volume
  stale     a directory sector that was never written: its old data is read
            as entries, now or once the folder's entries reach it
  dupname   two entries of one name in a folder
  nodot     a folder whose first entries aren't '.' and '..': moving it
            fails, and fsck complains
and, not errors (fsck mends them, and no file shares a cluster):
  size      a file longer than its chain: fsck shortens it
  lost      clusters taken in the FAT that no file has
  long      a chain longer than its file needs
  fatcopy   the two FATs differ
  dotdot    a folder's '..' names another folder than its parent
"""
import struct
import sys

SECTOR = 512
SECPERCLUS = 64
CLUSTER = SECTOR * SECPERCLUS
RESERVED = 32
STALE = 0xA5
EOC = 0x0FFFFFF8
ERRORS = ("free", "cross", "loop", "badref", "stale", "dupname", "nodot")
NOTES = ("size", "lost", "long", "fatcopy", "dotdot")


def mkfs(path, clusters=256):
    fatsz = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
    total = RESERVED + 2 * fatsz + clusters * SECPERCLUS
    boot = bytearray(SECTOR)
    boot[0:3] = b"\xEB\x58\x90"
    boot[3:11] = b"MSWIN4.1"
    struct.pack_into("<HBHBHHBHHHII", boot, 11, SECTOR, SECPERCLUS, RESERVED,
                     2, 0, 0, 0xF8, 0, 63, 255, 0, total)
    struct.pack_into("<IHHIHH", boot, 36, fatsz, 0, 0, 2, 1, 6)
    boot[64] = 0x80
    boot[66] = 0x29
    struct.pack_into("<I", boot, 67, 0x12345678)
    boot[71:82] = b"FSTEST     "
    boot[82:90] = b"FAT32   "
    boot[510:512] = b"\x55\xAA"
    info = bytearray(SECTOR)
    struct.pack_into("<I", info, 0, 0x41615252)
    struct.pack_into("<III", info, 484, 0x61417272, clusters - 1, 3)
    struct.pack_into("<I", info, 508, 0xAA550000)
    fat = bytearray(fatsz * SECTOR)
    struct.pack_into("<III", fat, 0, 0x0FFFFFF8, 0x0FFFFFFF, 0x0FFFFFFF)
    with open(path, "wb") as f:
        reserved = bytearray(RESERVED * SECTOR)
        reserved[0:SECTOR] = boot
        reserved[SECTOR:2 * SECTOR] = info
        reserved[6 * SECTOR:7 * SECTOR] = boot
        f.write(reserved)
        f.write(fat)
        f.write(fat)
        f.write(bytes(CLUSTER))                    # the root folder
        f.write(bytes([STALE]) * (CLUSTER * (clusters - 1)))


class Image:
    def __init__(self, path):
        self.f = open(path, "rb")
        boot = self.read(0, 1)
        (bps, spc, rsvd, nfats) = struct.unpack_from("<HBHB", boot, 11)
        total = struct.unpack_from("<I", boot, 32)[0]
        fatsz, _, _, self.rootclus = struct.unpack_from("<IHHI", boot, 36)
        assert bps == SECTOR and spc == SECPERCLUS
        self.first_data = rsvd + nfats * fatsz
        self.nclusters = (total - self.first_data) // spc
        raw1 = self.read(rsvd, fatsz)
        raw2 = self.read(rsvd + fatsz, fatsz)
        n = self.nclusters + 2
        self.fat = [v & 0x0FFFFFFF for v in struct.unpack_from("<%dI" % n, raw1)]
        self.fatcopy = raw1[:n * 4] != raw2[:n * 4]

    def read(self, sector, count):
        self.f.seek(sector * SECTOR)
        return self.f.read(count * SECTOR)

    def cluster(self, c):
        return self.read(self.first_data + (c - 2) * SECPERCLUS, SECPERCLUS)

    def valid(self, c):
        return 2 <= c < self.nclusters + 2


def lfn_part(e):
    raw = e[1:11] + e[14:26] + e[28:32]
    s = raw.decode("utf-16-le", "replace")
    return s.split("\x00")[0].replace("￿", "")


def short_name(e):
    base = e[0:8].decode("latin-1").rstrip()
    ext = e[8:11].decode("latin-1").rstrip()
    return base + ("." + ext if ext else "")


def check(path):
    img = Image(path)
    found = {k: [] for k in ERRORS + NOTES}
    owner = {}

    def chain(start, what):
        """the clusters of a chain, findings along the way"""
        out = []
        c = start
        while True:
            if not img.valid(c):
                found["badref"].append("%s: cluster %#x" % (what, c))
                return out
            if c in owner:
                if owner[c] == what:
                    found["loop"].append("%s: at %d" % (what, c))
                else:
                    found["cross"].append("%s and %s: cluster %d"
                                          % (owner[c], what, c))
                return out
            owner[c] = what
            out.append(c)
            nxt = img.fat[c]
            if nxt == 0:
                found["free"].append("%s: cluster %d is free" % (what, c))
                return out
            if nxt >= EOC:
                return out
            c = nxt

    def walk(start, path, dotdot):
        clusters = chain(start, path or "/")
        entries = []
        lfn = ""
        done = False
        stale = None
        dots = []
        for c in clusters:
            data = img.cluster(c)
            for s in range(SECPERCLUS):
                sec = data[s * SECTOR:(s + 1) * SECTOR]
                if sec == bytes([STALE]) * SECTOR:
                    stale = stale or "%s: cluster %d sector %d" \
                        % (path or "/", c, s)
                    done = True
                if done:
                    continue
                for o in range(0, SECTOR, 32):
                    e = sec[o:o + 32]
                    if e[0] == 0:
                        done = True
                        break
                    if e[0] == 0xE5:
                        lfn = ""
                        continue
                    attr = e[11]
                    if attr == 0x0F:
                        lfn = lfn_part(e) + lfn if not e[0] & 0x40 \
                            else lfn_part(e)
                        continue
                    name = lfn or short_name(e)
                    lfn = ""
                    if len(dots) < 2:
                        dots.append(name)
                    first = (struct.unpack_from("<H", e, 20)[0] << 16) \
                        | struct.unpack_from("<H", e, 26)[0]
                    if name == ".." and first != dotdot:
                        found["dotdot"].append("%s: %d, not %d"
                                               % (path, first, dotdot))
                    if attr & 0x08 or name in (".", ".."):
                        continue
                    size = struct.unpack_from("<I", e, 28)[0]
                    entries.append((name, attr, first, size))
        if stale:
            found["stale"].append(stale)
        if path and dots[:2] != [".", ".."]:
            found["nodot"].append("%s: first entries %s" % (path, dots[:2]))
        seen = set()
        for name, attr, first, size in entries:
            full = (path + "/" + name)
            if name.lower() in seen:
                found["dupname"].append(full)
            seen.add(name.lower())
            if attr & 0x10:
                if first:
                    walk(first, full, start if path else 0)
                else:
                    found["badref"].append("%s: folder without cluster" % full)
                continue
            if not first:
                if size:
                    found["size"].append("%s: %d bytes, no cluster"
                                         % (full, size))
                continue
            got = chain(first, full)
            if size > len(got) * CLUSTER:
                found["size"].append("%s: %d bytes, %d clusters"
                                     % (full, size, len(got)))
            elif len(got) > max(1, -(-size // CLUSTER)):
                found["long"].append("%s: %d bytes, %d clusters"
                                     % (full, size, len(got)))

    walk(img.rootclus, "", 0)
    lost = [c for c in range(2, img.nclusters + 2)
            if img.fat[c] and c not in owner]
    if lost:
        found["lost"].append("%d clusters" % len(lost))
    if img.fatcopy:
        found["fatcopy"].append("FAT 1 and 2 differ")
    return {k: v for k, v in found.items() if v}


def free_clusters(path):
    """how many clusters the FAT calls free, and how many FSInfo says"""
    img = Image(path)
    info = struct.unpack_from("<I", img.read(1, 1), 488)[0]
    return sum(1 for c in range(2, img.nclusters + 2) if not img.fat[c]), info


def tree(img, start=None, path=""):
    """every file and folder below a folder, as the entries name them:
    (path, attr, first cluster, size, the short entry's place in the image,
    the chain as far as it can be followed)"""
    def follow(c):
        out = []
        while img.valid(c) and c not in out:
            out.append(c)
            c = img.fat[c]
        return out

    out = []
    lfn = ""
    for c in follow(start or img.rootclus):
        data = img.cluster(c)
        for o in range(0, CLUSTER, 32):
            e = data[o:o + 32]
            if e[0] == 0:
                return out
            if e[0] == 0xE5:
                lfn = ""
                continue
            if e[11] == 0x0F:
                lfn = lfn_part(e) + lfn if not e[0] & 0x40 else lfn_part(e)
                continue
            name, lfn = lfn or short_name(e), ""
            if e[11] & 0x08 or name in (".", ".."):
                continue
            first = (struct.unpack_from("<H", e, 20)[0] << 16) \
                | struct.unpack_from("<H", e, 26)[0]
            at = (img.first_data + (c - 2) * SECPERCLUS) * SECTOR + o
            full = path + "/" + name
            out.append((full, e[11], first, struct.unpack_from("<I", e, 28)[0],
                        at, follow(first)))
            if e[11] & 0x10:
                out.extend(tree(img, first, full))
    return out


def damage(path, kind, victim, other):
    """Damages the chain of victim, a file of four clusters or a folder of
    one; other is a file of four that is not to suffer:
      one         its second cluster's link is 1
      beyond      that link is a number outside the volume, whose place in
                  a FAT that long would be in other's third cluster
      cross       its last cluster links into other's second
      long        its size is 40000 bytes: two clusters, the chain has four
      loop        its last cluster links back to its second
      short       its chain ends after two clusters, the rest is free
      first-one   its entry's first cluster is 1
      first-beyond  its entry's first cluster is beyond's number
      dir-beyond  the folder's cluster links outside the volume, as beyond
    and, after the kind: -small, its size is 40000 bytes too (under 64 KB);
    -mid, 90000 (in its third cluster); -empty, 0.
    """
    size = None
    for suffix, bytes_ in (("-small", 40000), ("-mid", 90000), ("-empty", 0)):
        if kind.endswith(suffix):
            kind, size = kind[:-len(suffix)], bytes_
    img = Image(path)
    files = {f[0]: f for f in tree(img)}
    _, _, _, _, at, a = files[victim]
    b = files[other][5]
    fatsz = (img.first_data - RESERVED) // 2
    # the number whose FAT entry would be 20 bytes into other's third cluster
    out = ((img.first_data + (b[2] - 2) * SECPERCLUS) - RESERVED) \
        * (SECTOR // 4) + 5
    assert not img.valid(out)
    fat, entry = {}, {}
    if kind == "one":
        fat[a[1]] = 1
    elif kind in ("beyond", "dir-beyond"):
        fat[a[-1 if kind == "dir-beyond" else 1]] = out
    elif kind == "cross":
        fat[a[-1]] = b[1]
    elif kind == "long":
        entry[28] = struct.pack("<I", 40000)
    elif kind == "loop":
        fat[a[-1]] = a[1]
    elif kind == "short":
        fat.update({a[1]: 0x0FFFFFFF, a[2]: 0, a[3]: 0})
    elif kind in ("first-one", "first-beyond"):
        first = 1 if kind == "first-one" else out
        entry[20] = struct.pack("<H", first >> 16)
        entry[26] = struct.pack("<H", first & 0xFFFF)
    elif kind != "none":
        sys.exit("no damage " + kind)
    if size is not None:
        entry[28] = struct.pack("<I", size)
    with open(path, "r+b") as f:
        for c, v in fat.items():
            for copy in range(2):
                f.seek((RESERVED + copy * fatsz) * SECTOR + c * 4)
                f.write(struct.pack("<I", v))
        for o, v in entry.items():
            f.seek(at + o)
            f.write(v)


def strays(before, after, mine):
    """What an operation on the files and folders named in mine wrote where
    it had no business: other files' clusters and their FAT entries, and
    the reserved sectors but FSInfo. before is the image as damaged."""
    a, b = Image(before), Image(after)
    out = []
    for s in range(RESERVED):
        if s != 1 and a.read(s, 1) != b.read(s, 1):
            out.append("reserved sector %d" % s)
    for full, attr, _, _, _, chain in tree(a):
        if attr & 0x10 or full in mine:
            continue
        for i, c in enumerate(chain):
            if a.fat[c] != b.fat[c]:
                out.append("%s: FAT entry of its cluster %d was %#x, is %#x"
                           % (full, i, a.fat[c], b.fat[c]))
            if a.cluster(c) != b.cluster(c):
                out.append("%s: data of its cluster %d" % (full, i))
    a.f.seek(0, 2)
    b.f.seek(0, 2)
    if a.f.tell() != b.f.tell():
        out.append("image size")
    return out


def main():
    if len(sys.argv) == 6 and sys.argv[1] == "damage":
        damage(*sys.argv[2:6])
    elif len(sys.argv) >= 3 and sys.argv[1] == "mkfs":
        mkfs(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 256)
    elif len(sys.argv) == 3 and sys.argv[1] == "check":
        found = check(sys.argv[2])
        for kind, items in found.items():
            for item in items:
                print("%s: %s" % (kind, item))
        sys.exit(1 if any(k in ERRORS for k in found) else 0)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
