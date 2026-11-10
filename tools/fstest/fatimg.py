#!/usr/bin/env python3
"""FAT32 images for the crash test: makes one, and checks one the way fsck
would, without trusting the file system that wrote it.

    python3 -I fatimg.py mkfs <img> [clusters]
    python3 -I fatimg.py check <img>

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


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "mkfs":
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
