#!/usr/bin/env python3
# Makes apps/gui/skin_engine/neo_ring.h: the table the main menu's pane draws
# its ring of the disk's use by (%nR), so that nothing of a circle is worked
# out as it is drawn:
#
#   themes/tools/make_neo_ring.py
#
# A quarter of the ring, the others being its mirror images: for each pixel how
# much of it the ring covers, in sixteenths, and where it is on the quarter's
# turn from 12 to 3 o'clock, in 64ths. The ring's middle is 36 px from its
# centre and it is 6 px thick, as the mockup's.

import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "apps", "gui", "skin_engine", "neo_ring.h")
Q, INNER, OUTER = 40, 33.0, 39.0

cover, turn = [], []
for y in range(Q):          # rows up from the centre
    for x in range(Q):      # columns right of it
        n = 0
        for j in range(4):
            for i in range(4):
                d = math.hypot(x + (i + .5) / 4, y + (j + .5) / 4)
                n += INNER <= d <= OUTER
        cover.append(min(n, 15))
        turn.append(min(63, int(math.atan2(x + .5, y + .5) / (math.pi / 2) * 64)))

def table(name, values):
    lines = ["static const unsigned char %s[NEO_RING_Q * NEO_RING_Q] = {" % name]
    for i in range(0, len(values), 20):
        lines.append("    " + ",".join("%d" % v for v in values[i:i + 20]) + ",")
    return "\n".join(lines) + "\n};\n"

with open(OUT, "w") as f:
    f.write("/* Made by themes/tools/make_neo_ring.py: a quarter of the ring of the disk's use,\n"
            "   rows up and columns right from its centre; not edited */\n"
            "#define NEO_RING_Q %d\n" % Q)
    f.write(table("neo_ring_cover", cover))
    f.write(table("neo_ring_turn", turn))
