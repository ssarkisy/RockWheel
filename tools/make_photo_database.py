#!/usr/bin/env python3
"""Make an iPod Video photo database for testing the Photos plugin.

Writes <root>/Photos/Photo Database and the Photos/Thumbs/F*.ithmb files
the way iTunes does for the 5th generation iPod (record layouts as in
libgpod). Every directory given becomes an album named after it; the
Photo Library holds all the photos.

    tools/make_photo_database.py build-sim/simdisk "Trips" "Family"

Images are scaled with sips, so this runs on macOS only. Use it on the
simulator disk, not on a real iPod: Finder owns the database there.
"""

import os
import struct
import subprocess
import sys
import tempfile
import time

# The iPod Video photo formats: id, width, height. Finder crops the grid
# thumbnails (1036) to fill them and fits the photo in the others.
FORMATS = [(1036, 50, 41), (1015, 130, 88), (1024, 320, 240)]
CROPPED = (1036,)
IMAGE_EXTS = (".jpg", ".jpeg", ".png", ".heic", ".bmp", ".tif", ".tiff")
MAC_EPOCH = 2082844800  # seconds from 1904 to 1970
FIRST_ID = 0x64


def header(tag, header_len, *fields):
    """A record header padded with zeros to header_len."""
    data = tag + b"".join(fields)
    return data + b"\0" * (header_len - len(data))


def i16(v):
    return struct.pack("<h", v)


def i32(v):
    return struct.pack("<i", v)


def u32(v):
    return struct.pack("<I", v)


def u8(v):
    return struct.pack("<B", v)


def mhod_string(kind, text, utf16):
    raw = text.encode("utf-16-le" if utf16 else "utf-8")
    padding = (4 - (36 + len(raw)) % 4) % 4
    total = 36 + len(raw) + padding
    # header_len is 0x18 although the string header is 36 bytes long
    return (b"mhod" + i32(0x18) + i32(total) + i16(kind) + u8(0) +
            u8(padding) + i32(0) + i32(0) + i32(len(raw)) +
            u8(2 if utf16 else 1) + u8(0) + i16(0) + i32(0) +
            raw + b"\0" * padding)


def mhni(fmt, offset, size, width, height, hpad, vpad, filename):
    child = mhod_string(3, filename, True)
    return header(b"mhni", 0x4c, i32(0x4c), i32(0x4c + len(child)), i32(1),
                  i32(fmt), i32(offset), i32(size), i16(vpad), i16(hpad),
                  i16(height), i16(width)) + child


def mhod_thumb(child):
    return header(b"mhod", 0x18, i32(0x18), i32(0x18 + len(child)),
                  i16(2)) + child


def read_bmp(path):
    """Pixels of a 24 or 32 bit BMP as rows of (r, g, b), top row first."""
    with open(path, "rb") as f:
        data = f.read()
    offset, = struct.unpack_from("<I", data, 10)
    width, height, _, bpp = struct.unpack_from("<iiHH", data, 18)
    bottom_up = height > 0
    height = abs(height)
    step = bpp // 8
    stride = (width * step + 3) & ~3
    rows = []
    for y in range(height):
        base = offset + (height - 1 - y if bottom_up else y) * stride
        rows.append([(data[base + x * step + 2], data[base + x * step + 1],
                      data[base + x * step]) for x in range(width)])
    return rows


def image_size(path):
    out = subprocess.run(["sips", "-g", "pixelWidth", "-g", "pixelHeight",
                          path], capture_output=True, text=True,
                         check=True).stdout
    values = dict(line.split(":") for line in out.splitlines()[1:])
    return int(values["  pixelWidth"]), int(values["  pixelHeight"])


def frame(path, src_w, src_h, width, height, crop, tmp):
    """The photo scaled to fit width x height, centred on black, or cropped to
    fill it, as RGB565 little endian, and the size and padding of the picture
    inside."""
    scale = (max if crop else min)(width / src_w, height / src_h)
    w = max(1, round(src_w * scale))
    h = max(1, round(src_h * scale))
    bmp = os.path.join(tmp, "frame.bmp")
    command = ["sips", "-s", "format", "bmp", "--resampleHeightWidth",
               str(h), str(w)]
    if crop:
        command += ["--cropToHeightWidth", str(height), str(width)]
    w, h = min(w, width), min(h, height)
    subprocess.run(command + [path, "--out", bmp],
                   capture_output=True, check=True)
    rows = read_bmp(bmp)
    hpad, vpad = (width - w) // 2, (height - h) // 2
    pixels = bytearray(width * height * 2)
    for y, row in enumerate(rows[:h]):
        for x, (r, g, b) in enumerate(row[:w]):
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            struct.pack_into("<H", pixels, ((y + vpad) * width + x + hpad) * 2,
                             v)
    return bytes(pixels), w, h, hpad, vpad


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    root, album_dirs = sys.argv[1], sys.argv[2:]
    thumbs_dir = os.path.join(root, "Photos", "Thumbs")
    os.makedirs(thumbs_dir, exist_ok=True)

    albums = []  # (name, [image ids])
    photos = []  # (image id, path)
    for album_dir in album_dirs:
        ids = []
        for name in sorted(os.listdir(album_dir)):
            if name.lower().endswith(IMAGE_EXTS):
                image_id = FIRST_ID + len(photos)
                photos.append((image_id, os.path.join(album_dir, name)))
                ids.append(image_id)
        albums.append((os.path.basename(os.path.normpath(album_dir)), ids))

    ithmbs = {fmt: open(os.path.join(thumbs_dir, "F%d_1.ithmb" % fmt), "wb")
              for fmt, _, _ in FORMATS}
    now = int(time.time()) + MAC_EPOCH
    items = []
    with tempfile.TemporaryDirectory() as tmp:
        for image_id, path in photos:
            src_w, src_h = image_size(path)
            mhods = b""
            for fmt, width, height in FORMATS:
                pixels, w, h, hpad, vpad = frame(path, src_w, src_h,
                                                 width, height,
                                                 fmt in CROPPED, tmp)
                f = ithmbs[fmt]
                offset = f.tell()
                f.write(pixels)
                mhods += mhod_thumb(mhni(fmt, offset, len(pixels), w, h,
                                         hpad, vpad,
                                         ":Thumbs:F%d_1.ithmb" % fmt))
            items.append(header(b"mhii", 0x98, i32(0x98),
                                i32(0x98 + len(mhods)), i32(len(FORMATS)),
                                i32(image_id), b"\0" * 8, i32(0), i32(0),
                                i32(0), u32(now), u32(now),
                                i32(os.path.getsize(path))) + mhods)
            print(os.path.basename(path))
    for f in ithmbs.values():
        f.close()

    library = ("Library", [image_id for image_id, _ in photos])
    mhbas = []
    for number, (name, ids) in enumerate([library] + albums):
        album_id = FIRST_ID + len(photos) + number
        name_mhod = mhod_string(1, name, False)
        mhias = b"".join(header(b"mhia", 0x28, i32(0x28), i32(0x28), i32(0),
                                i32(image_id)) for image_id in ids)
        body = name_mhod + mhias
        mhbas.append(header(b"mhba", 0x94, i32(0x94), i32(0x94 + len(body)),
                            i32(1), i32(len(ids)), i32(album_id), i32(0),
                            i16(0), u8(1 if number == 0 else 2), u8(0),
                            u8(0), u8(0), u8(0), u8(0), i32(3), i32(1000))
                     + body)

    mhifs = [header(b"mhif", 0x7c, i32(0x7c), i32(0x7c), i32(0), i32(fmt),
                    i32(width * height * 2)) for fmt, width, height in FORMATS]

    def mhsd(index, tag, children):
        body = header(tag, 0x5c, i32(0x5c), i32(len(children))) + \
            b"".join(children)
        return header(b"mhsd", 0x60, i32(0x60), i32(0x60 + len(body)),
                      i16(index)) + body

    sections = (mhsd(1, b"mhli", items) + mhsd(2, b"mhla", mhbas) +
                mhsd(3, b"mhlf", mhifs))
    next_id = FIRST_ID + len(photos) + len(mhbas)
    db = header(b"mhfd", 0x84, i32(0x84), i32(0x84 + len(sections)), i32(0),
                i32(2), i32(3), i32(0), i32(next_id)) + sections
    with open(os.path.join(root, "Photos", "Photo Database"), "wb") as f:
        f.write(db)
    print("%d photos in %d albums" % (len(photos), len(albums)))


if __name__ == "__main__":
    main()
