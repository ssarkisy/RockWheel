#!/usr/bin/env python3
# Make the repeat and shuffle icons of the iClassic Square status bar from the
# theme's larger ones, which have their shadows on white:
#
#     themes/tools/make_status_icons.py
#
# Needs Pillow. The icons are taken off the white, scaled to 0.8, and set a little
# down so that they sit in the middle of the bar. A partly covered pixel gets the
# colour of the bar in its row, and an empty one the magenta that skins take as
# transparent, as the bar is in the backdrop. Run it again if the backdrop changes.
import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
THEME = os.path.join(HERE, '..', 'iClassic-Square', '.rockbox', 'wps', 'iClassic-Square')
SCALE = 0.8
SIZE = 19, 20  # of an icon in the larger images
OUT_WIDTH, OUT_HEIGHT = 16, 16
TOP = 2  # the row of the bar the images start at
FRACTION = 0.3  # how far below that the icons start
BAR_COLUMNS = range(228, 265)
TRANSPARENT = 255, 0, 255


def scaled_frame(source, frame, backdrop):
    width, height = SIZE
    cover = [[[0.0, 0.0, 0.0, 0.0] for _ in range(OUT_WIDTH)] for _ in range(OUT_HEIGHT)]
    for sy in range(height):
        for sx in range(width):
            pixel = source.getpixel((sx, frame * height + sy))
            alpha = 1 - min(pixel) / 255
            if alpha == 0:
                continue
            colour = [(c - (1 - alpha) * 255) / alpha for c in pixel]
            left, right = sx * SCALE, (sx + 1) * SCALE
            top, bottom = FRACTION + sy * SCALE, FRACTION + (sy + 1) * SCALE
            for y in range(int(top), min(int(bottom) + 1, OUT_HEIGHT)):
                for x in range(int(left), min(int(right) + 1, OUT_WIDTH)):
                    area = (max(0, min(right, x + 1) - max(left, x)) *
                            max(0, min(bottom, y + 1) - max(top, y)))
                    total = cover[y][x]
                    total[3] += area * alpha
                    for i in range(3):
                        total[i] += area * alpha * colour[i]
    image = Image.new('RGB', (OUT_WIDTH, OUT_HEIGHT), TRANSPARENT)
    for y in range(OUT_HEIGHT):
        bar = [sum(backdrop.getpixel((x, TOP + y))[i] for x in BAR_COLUMNS) / len(BAR_COLUMNS)
               for i in range(3)]
        for x in range(OUT_WIDTH):
            r, g, b, alpha = cover[y][x]
            if alpha < 0.02:
                continue
            image.putpixel((x, y), tuple(round(v + (1 - alpha) * bar[i])
                                         for i, v in enumerate((r, g, b))))
    return image


def make(source_name, out_name, backdrop):
    source = Image.open(os.path.join(THEME, source_name)).convert('RGB')
    frames = source.height // SIZE[1]
    out = Image.new('RGB', (OUT_WIDTH, OUT_HEIGHT * frames))
    for frame in range(frames):
        out.paste(scaled_frame(source, frame, backdrop), (0, frame * OUT_HEIGHT))
    out.save(os.path.join(THEME, out_name))


backdrop = Image.open(os.path.join(THEME, 'iClassic_bg.bmp')).convert('RGB')
make('Repeat Icon.bmp', 'Status Repeat.bmp', backdrop)
make('Shuffle Icon.bmp', 'Status Shuffle.bmp', backdrop)
