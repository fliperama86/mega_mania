#!/usr/bin/env python3
"""Draw a region of a converted stage with its collision heightmaps on top.

Physics is only as good as the collision data underneath it, so this checks
the data before anything depends on it: the red line should sit exactly on the
visible ground, walls should show as vertical runs, and empty sky should have
no line at all.

Usage: collision_preview.py <assets dir> <out.png> [blockX] [blockY] [w] [h]
"""

import struct
import sys

from PIL import Image

MAP_W, MAP_H = 256, 128


def read(path):
    return open(path, "rb").read()


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: collision_preview.py <assets> <out.png> "
                         "[bx] [by] [w] [h]")
    d = sys.argv[1]
    out = sys.argv[2]
    bx = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    by = int(sys.argv[4]) if len(sys.argv) > 4 else 48
    bw = int(sys.argv[5]) if len(sys.argv) > 5 else 32
    bh = int(sys.argv[6]) if len(sys.argv) > 6 else 16

    pal = read(f"{d}/pal.bin")
    tiles = read(f"{d}/tiles.bin")
    blocks = read(f"{d}/blocks.bin")
    fgmap = read(f"{d}/map_fg.bin")
    coll = read(f"{d}/collide.bin")

    # MD palette words to RGB
    colours = []
    for i in range(len(pal) // 2):
        w = struct.unpack_from(">H", pal, i * 2)[0]
        r = ((w >> 1) & 7) * 36
        g = ((w >> 5) & 7) * 36
        b = ((w >> 9) & 7) * 36
        colours.append((r, g, b))

    img = Image.new("RGB", (bw * 16, bh * 16), (0, 0, 0))
    px = img.load()

    def draw_tile(idx, pal_no, hf, vf, ox, oy):
        base = idx * 32
        for y in range(8):
            row = struct.unpack_from(">I", tiles, base + y * 4)[0]
            for x in range(8):
                v = (row >> ((7 - x) * 4)) & 0xF
                if not v:
                    continue
                sx = ox + (7 - x if hf else x)
                sy = oy + (7 - y if vf else y)
                if 0 <= sx < img.width and 0 <= sy < img.height:
                    px[sx, sy] = colours[pal_no * 16 + v]

    for y in range(bh):
        for x in range(bw):
            mx, my = bx + x, by + y
            if mx >= MAP_W or my >= MAP_H:
                continue
            cell = struct.unpack_from(">H", fgmap, (my * MAP_W + mx) * 2)[0]
            b = cell & 0x0FFF
            floor_solid = cell & 0x1000
            for i in range(4):
                e = struct.unpack_from(">H", blocks, (b * 4 + i) * 2)[0]
                draw_tile(e & 0x7FF, (e >> 13) & 3, (e >> 11) & 1,
                          (e >> 12) & 1,
                          x * 16 + (i & 1) * 8, y * 16 + (i >> 1) * 8)

            # collision surface, in red, only where the placement is solid
            if not floor_solid:
                continue
            off = b * 18
            for c in range(16):
                hgt = coll[off + c]
                if hgt == 0xFF:
                    continue
                sy = y * 16 + (15 - hgt)
                sx = x * 16 + c
                if 0 <= sx < img.width and 0 <= sy < img.height:
                    px[sx, sy] = (255, 0, 0)

    img = img.resize((img.width * 2, img.height * 2), Image.NEAREST)
    img.save(out)
    print(f"{out}  {bw}x{bh} blocks from ({bx},{by})")


if __name__ == "__main__":
    main()
