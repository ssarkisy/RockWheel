#!/usr/bin/env python3
# Makes apps/gui/skin_engine/neo_ring.h: the table the main menu's pane draws
# its ring of the disk's use by (%nR), so that nothing of a circle is worked
# out as it is drawn:
#
#   themes/tools/make_neo_ring.py
#
# A quarter of the ring, the others being its mirror images: for each pixel how
# much of it the ring covers, in sixteenths, and where it is on the quarter's
# turn from 12 to 3 o'clock, in 64ths. On the Video the ring is 80 px, its
# middle 36 px from its centre and 6 px thick, as the mockup's; in the iPod
# photo's pane of 110 px it is 64 px, its middle 29 px from its centre and
# 5 px thick, as much of the pane as the Video's is of its 160.

import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "apps", "gui", "skin_engine", "neo_ring.h")
# (the screen's macro, or None for the rest), the quarter's side, the ring's
# inner and outer radii
RINGS = (("UI_220X176", 32, 26.5, 31.5), (None, 40, 33.0, 39.0))


def tables(q, inner, outer):
    cover, turn = [], []
    for y in range(q):          # rows up from the centre
        for x in range(q):      # columns right of it
            n = 0
            for j in range(4):
                for i in range(4):
                    d = math.hypot(x + (i + .5) / 4, y + (j + .5) / 4)
                    n += inner <= d <= outer
            cover.append(min(n, 15))
            turn.append(min(63, int(math.atan2(x + .5, y + .5) / (math.pi / 2) * 64)))
    return cover, turn


def table(name, values, q):
    lines = ["static const unsigned char %s[NEO_RING_Q * NEO_RING_Q] = {" % name]
    for i in range(0, len(values), q // 2):
        lines.append("    " + ",".join("%d" % v for v in values[i:i + q // 2]) + ",")
    return "\n".join(lines) + "\n};\n"


with open(OUT, "w") as f:
    f.write("/* Made by themes/tools/make_neo_ring.py: a quarter of the ring of the disk's use,\n"
            "   rows up and columns right from its centre; not edited. 64 px in the iPod\n"
            "   photo's pane, 80 px on the Video */\n")
    for k, (screen, q, inner, outer) in enumerate(RINGS):
        if screen:
            f.write("#ifdef %s\n" % screen)
        else:
            f.write("#else\n")
        cover, turn = tables(q, inner, outer)
        f.write("#define NEO_RING_Q %d\n" % q)
        f.write(table("neo_ring_cover", cover, q))
        f.write(table("neo_ring_turn", turn, q))
    f.write("#endif\n")
