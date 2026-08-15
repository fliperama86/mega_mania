#!/usr/bin/env python3
"""Convert a Mania stage into Mega Drive assets.

Emits into <outdir>:
    pal.bin      4 palettes of 16 MD colours (BGR333, entry 0 transparent)
    tiles.bin    unique 8x8 4bpp tiles, deduplicated including flips
    blocks.bin   per 16x16 block, 4 name table entries (tile, palette, flips)
    map_fg.bin   FG Low layout: u16 per cell, bits 0-11 block index,
                 bit 12 floor solid, bit 13 wall and roof solid (RSDK path A)
    map_bg.bin   BG Outside layout, same format
    collide.bin  per block, 16 column heights plus angle and flag

Block 0 is blank and block 1 is a visible X, so empty map cells read as sky
while anything that failed conversion shows up as an obvious marker.

The MD has four palettes of fifteen usable colours and every 8x8 tile draws
from one of them, so tiles are fitted to palettes most-used-first. A tile whose
colours do not fit any palette goes to the nearest one and has its pixels
remapped to the closest available shade, which costs a little colour accuracy
and avoids holes in the map.

Usage: convert_stage.py <Data.rsdk> <stage> <outdir> [topN] [mapW] [mapH]
"""

import io
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import scene
from rsdk import Pack

from PIL import Image

BLANK = 0
PLACEHOLDER = 1

FG_LAYER = "FG Low"
BG_LAYER = "BG Outside"


def md_colour(rgb):
    """Quantize to the MD's three bits per channel."""
    return (rgb[0] >> 5, rgb[1] >> 5, rgb[2] >> 5)


def md_word(c):
    """MD CRAM word: 0000 BBB0 GGG0 RRR0"""
    return (c[2] << 9) | (c[1] << 5) | (c[0] << 1)


def load_collision(pack, stage, paths=1, tiles=1024):
    """TileConfig.bin: per collision path, per tile, 16 height bytes, 16
    active bytes, then yFlip, floor/lWall/rWall/roof angles and a flag.
    Only path 0 and the floor mask are needed for now."""
    data = pack.read(f"Data/Stages/{stage}/TileConfig.bin")
    if data is None or data[:3] != b"TIL":
        return None
    r = scene.Reader(data)
    r.p = 4
    buf = r.compressed()

    out = {}
    pos = 0
    for p in range(2):
        for t in range(tiles):
            heights = buf[pos:pos + 16]; pos += 16
            active = buf[pos:pos + 16]; pos += 16
            yflip = buf[pos]; pos += 1
            floor = buf[pos]; pos += 1
            pos += 3                      # lWall, rWall, roof angles
            flag = buf[pos]; pos += 1
            if p == 0:
                cols = bytes(heights[c] if active[c] else 0xFF
                             for c in range(16))
                out[t] = (cols, floor, flag, yflip)
    return out


class Tileset:
    """The stage's 16x16 tiles, as a 16 x 16384 indexed GIF."""

    def __init__(self, gif):
        img = Image.open(io.BytesIO(gif))
        self.pal = img.getpalette()
        self.px = list(img.getdata())
        self.w = img.size[0]

    def colours(self, t):
        base = t * 16 * self.w
        out = set()
        for y in range(16):
            for idx in self.px[base + y * self.w: base + y * self.w + 16]:
                if idx:
                    out.add(md_colour(tuple(self.pal[idx * 3: idx * 3 + 3])))
        return out

    def pixel(self, t, x, y):
        idx = self.px[(t * 16 + y) * self.w + x]
        if idx == 0:
            return None
        return md_colour(tuple(self.pal[idx * 3: idx * 3 + 3]))


def fit_palettes(tiles, colours, usage, count=4, size=15):
    """Assign tiles to palettes, most used first so failures are rare tiles.
    Returns (palettes, assignment, exact) where exact is the set of tiles whose
    colours all fit their palette."""
    palettes = [set() for _ in range(count)]
    assign = {}
    exact = set()
    order = sorted(tiles, key=lambda t: -usage[t])

    seeds = []
    for t in order:
        if len(seeds) == count:
            break
        if all(len(colours[t] & colours[s]) < 6 for s in seeds):
            seeds.append(t)
    for i, t in enumerate(seeds):
        palettes[i] |= colours[t]
        assign[t] = i
        exact.add(t)

    for t in order:
        if t in assign:
            continue
        best, cost = None, None
        for i, p in enumerate(palettes):
            if len(p | colours[t]) <= size:
                c = len(colours[t] - p)
                if cost is None or c < cost:
                    best, cost = i, c
        if best is not None:
            palettes[best] |= colours[t]
            assign[t] = best
            exact.add(t)
        else:
            # nearest palette by total colour distance; pixels get remapped
            assign[t] = min(range(count), key=lambda i: sum(
                min(sum((c[k] - q[k]) ** 2 for k in range(3)) for q in palettes[i])
                for c in colours[t]))
    return palettes, assign, exact


class TileBank:
    """Unique 8x8 tiles, deduplicated across the four flip variants."""

    def __init__(self):
        self.index = {}
        self.data = []

    def add(self, rows):
        h = tuple(tuple(reversed(r)) for r in rows)
        v = tuple(reversed(rows))
        hv = tuple(reversed(h))
        for (hf, vf), variant in (((0, 0), rows), ((1, 0), h),
                                  ((0, 1), v), ((1, 1), hv)):
            if variant in self.index:
                return self.index[variant], hf, vf
        self.index[rows] = len(self.data)
        self.data.append(rows)
        return len(self.data) - 1, 0, 0


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    pack = Pack(sys.argv[1])
    stage = sys.argv[2]
    out = sys.argv[3]
    topn = int(sys.argv[4]) if len(sys.argv) > 4 else 600
    mapw = int(sys.argv[5]) if len(sys.argv) > 5 else 256
    maph = int(sys.argv[6]) if len(sys.argv) > 6 else 128
    os.makedirs(out, exist_ok=True)

    ts = Tileset(pack.read(f"Data/Stages/{stage}/16x16Tiles.gif"))
    layers = scene.load(pack, stage)
    if FG_LAYER not in layers:
        raise SystemExit(f"no {FG_LAYER}; have: " + ", ".join(layers))

    # Fit palettes across foreground and background together, since they share
    # one 64 colour CRAM between them.
    wanted = [("fg", FG_LAYER), ("bg", BG_LAYER)]
    usage = Counter()
    used = {}
    for tag, name in wanted:
        if name in layers:
            used[tag] = layers[name]
            usage.update(layers[name].usage())

    keep = [t for t, _ in usage.most_common(topn)]
    colours = {t: ts.colours(t) for t in keep}
    palettes, assign, exact = fit_palettes(keep, colours, usage)
    pal_index = [{c: i + 1 for i, c in enumerate(sorted(p))} for p in palettes]

    bank = TileBank()
    bank.add(tuple(tuple(0 for _ in range(8)) for _ in range(8)))       # blank
    bank.add(tuple(tuple(1 if (x == y or x == 7 - y) else 0
                         for x in range(8)) for y in range(8)))          # X

    blocks = [[BLANK] * 4, [PLACEHOLDER] * 4]
    block_of = {}
    remapped = 0

    for t in sorted(keep, key=lambda t: -usage[t]):
        p = assign[t]
        idxmap = pal_index[p]
        entries = []
        for by in (0, 8):
            for bx in (0, 8):
                rows = []
                for y in range(8):
                    row = []
                    for x in range(8):
                        c = ts.pixel(t, bx + x, by + y)
                        if c is None:
                            row.append(0)
                        else:
                            j = idxmap.get(c)
                            if j is None:
                                j = idxmap[min(idxmap, key=lambda q: sum(
                                    (c[k] - q[k]) ** 2 for k in range(3)))]
                                remapped += 1
                            row.append(j)
                    rows.append(tuple(row))
                ti, hf, vf = bank.add(tuple(rows))
                entries.append((ti & 0x7FF) | (p << 13) | (hf << 11) | (vf << 12))
        block_of[t] = len(blocks)
        blocks.append(entries)

    collision = load_collision(pack, stage)

    maps = {}
    for tag, layer in used.items():
        w = min(mapw, layer.w) if tag == "fg" else layer.w
        h = min(maph, layer.h) if tag == "fg" else layer.h
        data = bytearray()
        missing = 0
        for y in range(h):
            for x in range(w):
                t = layer.tile(x, y)
                if t is None:
                    b = BLANK
                else:
                    b = block_of.get(t, PLACEHOLDER)
                    if b == PLACEHOLDER:
                        missing += 1
                    # carry RSDK path A solidity: bit 12 floor, bit 13 sides.
                    # Without it every decorative flower reads as ground.
                    b |= layer.entry(x, y) & 0x3000
                data += struct.pack(">H", b)
        maps[tag] = (w, h, data, missing)

    with open(f"{out}/pal.bin", "wb") as f:
        for p in palettes:
            entries = [(0, 0, 0)] + sorted(p)
            for i in range(16):
                f.write(struct.pack(">H", md_word(entries[i] if i < len(entries)
                                                 else (0, 0, 0))))

    with open(f"{out}/tiles.bin", "wb") as f:
        for rows in bank.data:
            for row in rows:
                v = 0
                for pxl in row:
                    v = (v << 4) | (pxl & 0xF)
                f.write(struct.pack(">I", v))

    with open(f"{out}/blocks.bin", "wb") as f:
        for entries in blocks:
            for e in entries:
                f.write(struct.pack(">H", e))

    for tag, (w, h, data, _) in maps.items():
        with open(f"{out}/map_{tag}.bin", "wb") as f:
            f.write(data)

    if collision:
        # index aligned with blocks.bin; blank and placeholder are empty
        with open(f"{out}/collide.bin", "wb") as f:
            solid = 0
            for i in range(len(blocks)):
                t = next((k for k, v in block_of.items() if v == i), None)
                if t is None or t not in collision:
                    f.write(b"\xff" * 16 + b"\x00\x00")
                else:
                    cols, floor, flag, _ = collision[t]
                    f.write(cols + bytes([floor, flag]))
                    if any(c != 0xFF for c in cols):
                        solid += 1
            print(f"  collision           {len(blocks)} blocks, {solid} solid")

    print(f"stage {stage} -> {out}")
    print(f"  tiles used          {len(keep)}  ({len(exact)} fitted exactly)")
    print(f"  unique 8x8 tiles    {len(bank.data)}  ({len(bank.data)*32:,} bytes)")
    print(f"  blocks              {len(blocks)}  ({len(blocks)*8:,} bytes)")
    for tag, (w, h, data, missing) in maps.items():
        print(f"  map {tag}              {w} x {h}  ({len(data):,} bytes, "
              f"{missing} placeholders)")
    print(f"  palettes            {[len(p) for p in palettes]} colours")
    print(f"  pixels remapped     {remapped:,}")


if __name__ == "__main__":
    main()
