#!/usr/bin/env python3
"""Convert Sonic Mania's Sonic sprite frames into Mega Drive hardware-sprite assets.

Emits into <assetdir>:
    tiles.bin   every converted frame's tile block, concatenated in frame order
    pal.bin     16 big-endian MD colour words, entry 0 transparent, then
                Sonic's palette colours sorted
    hitbox.bin  animator hitbox 0 (the "outer" box RSDK.GetHitbox(&player->
                animator, 0) returns -- see Player_GetHitbox, SonicMania/
                Objects/Global/Player.c:2244-2248) of every frame, in the same
                frame order as sonic_frames below: 4 signed bytes per frame,
                left/top/right/bottom. Consumed by game/md_src/rings.c's touch
                test, which needs Sonic's current-frame hitbox but must not
                gain visibility into the rest of SonicFrame (that struct rides
                the descriptor table the slave SH2 also reads) -- a standalone
                file keeps the ring feature's one dependency on Sonic's data
                narrow and explicit.

Emits into <srcdir>:
    sonic_data.h / sonic_data.c   the SonicPiece, SonicFrame and SonicAnim
                tables plus the animation enum; tile pixels and the palette
                stay in the .bin files, .incbin'ed by assembly

A Mega Drive hardware sprite is at most 4x4 8x8 tiles, its tiles stored in
VRAM column-major. Sonic's frames are usually bigger than that, so each
frame is split into "pieces": walk its padded tile grid in 4x4 chunks
(columns of four, then rows of four), and for every chunk holding any
non-transparent tile, emit a piece for the trimmed bounding box of those
tiles. A frame's tile block is the concatenation of its pieces' tiles, each
piece's own tiles column-major, so one DMA of the block loads the frame.

RSDK draws a frame with its top-left at (entityX + pivotX, entityY + pivotY),
so unflipped, a piece goes on screen at
(entityX + pivotX + dx, entityY + pivotY + dy).

Frame durations are RSDK's raw values, not a precomputed hold time: the
animation's speed is only a default, and Player.c overrides it every frame from
the ground velocity, so the two have to stay separate for the walk cycle to
keep up with Sonic (Animation.cpp ProcessAnimation: a timer gains `speed` each
vsync and the frame advances once it passes `duration`).

Usage: convert_sonic.py <Data.rsdk> <assetdir> <srcdir>
"""

import io
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import anim
from convert_stage import md_colour, md_word
from rsdk import Pack

from PIL import Image

ANIMATIONS = ["Idle", "Walk", "Jog", "Run", "Dash", "Skid", "Skid Turn",
              "Air Walk", "Jump", "Push", "Look Up", "Crouch"]
SHEET_DIR = "Data/Sprites/"
PALETTE_COLOURS = 15   # usable colours; pal.bin entry 0 is transparent


class Sheet:
    """A decoded GIF spritesheet: flat indexed pixel access, index 0 transparent."""

    def __init__(self, gif):
        img = Image.open(io.BytesIO(gif))
        self.pal = img.getpalette()
        self.px = list(img.getdata())
        self.w, self.h = img.size

    def colour(self, idx):
        return md_colour(tuple(self.pal[idx * 3:idx * 3 + 3]))

    def crop(self, x, y, w, h):
        out = []
        for row in range(h):
            base = (y + row) * self.w + x
            out.extend(self.px[base:base + w])
        return out


def merge_palette(usage, limit):
    """Repeatedly fuse the closest pair of colours, lighter (by pixel count)
    absorbed into heavier, until at most `limit` remain. Returns (mapping
    colour -> surviving colour, pixel count that changed colour)."""
    weight = dict(usage)
    parent = {c: c for c in usage}
    roots = list(usage)

    def dist(a, b):
        return sum((a[k] - b[k]) ** 2 for k in range(3))

    while len(roots) > limit:
        _, i, j = min((dist(roots[i], roots[j]), i, j)
                      for i in range(len(roots)) for j in range(i + 1, len(roots)))
        a, b = roots[i], roots[j]
        if weight[a] < weight[b]:
            a, b = b, a
        weight[a] += weight[b]
        del weight[b]
        parent[b] = a
        roots.remove(b)

    def find(c):
        while parent[c] != c:
            c = parent[c]
        return c

    mapping = {c: find(c) for c in usage}
    changed = sum(usage[c] for c in usage if mapping[c] != c)
    return mapping, changed


def tile_at(grid, tx, ty):
    return [row[tx * 8:tx * 8 + 8] for row in grid[ty * 8:ty * 8 + 8]]


def tile_empty(tile):
    return not any(v for row in tile for v in row)


def pack_tile(rows):
    """8x8 palette-index rows -> 8 big-endian u32s, 4 bits/pixel, left pixel high."""
    out = b""
    for row in rows:
        v = 0
        for p in row:
            v = (v << 4) | (p & 0xF)
        out += struct.pack(">I", v)
    return out


def enum_name(name):
    return "ANI_" + name.upper().replace(" ", "_")


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    pack = Pack(sys.argv[1])
    assetdir = sys.argv[2]
    srcdir = sys.argv[3]
    os.makedirs(assetdir, exist_ok=True)
    os.makedirs(srcdir, exist_ok=True)

    spr = anim.load(pack, "Data/Sprites/Players/Sonic.bin")
    byname = {a.name: a for a in spr.animations}
    anims = [byname[n] for n in ANIMATIONS]

    sheets = {name: Sheet(pack.read(SHEET_DIR + name))
              for name in {f.sheet for a in anims for f in a.frames}}
    frames = [(a, fi, f, sheets[f.sheet], sheets[f.sheet].crop(f.x, f.y, f.w, f.h))
              for a in anims for fi, f in enumerate(a.frames)]

    # 1. one shared 15-colour palette across every converted frame
    usage = Counter()
    for a, fi, f, sh, px in frames:
        for idx in px:
            if idx:
                usage[sh.colour(idx)] += 1

    mapping, changed = {c: c for c in usage}, 0
    if len(usage) > PALETTE_COLOURS:
        mapping, changed = merge_palette(usage, PALETTE_COLOURS)

    final_colours = sorted(set(mapping.values()))
    colour_index = {c: i + 1 for i, c in enumerate(final_colours)}

    def px_index(sh, idx):
        return colour_index[mapping[sh.colour(idx)]] if idx else 0

    # 2. pivot and hitbox range check, and precomputed per-frame hold time
    bad = []
    hitbox_pairs = set()
    for a, fi, f, sh, px in frames:
        if not (-128 <= f.pivotX <= 127 and -128 <= f.pivotY <= 127):
            bad.append(f"{a.name} frame {fi}: pivot ({f.pivotX},{f.pivotY}) out of int8_t")
        outer, inner = f.hitboxes[0], f.hitboxes[1]
        hitbox_pairs.add((outer, inner))
        for v in outer + inner:
            if not (-128 <= v <= 127):
                bad.append(f"{a.name} frame {fi}: hitbox outer={outer} inner={inner} out of int8_t")
    for msg in bad:
        print(f"  WARNING {msg}")
    if bad:
        raise SystemExit(f"{len(bad)} frame(s) out of int8_t range, aborting")

    # 3. tiles, pieces (global) and per-frame metadata
    tiles = bytearray()
    pieces = []       # (dx, dy, size, tile) in emission order == sonic_pieces order
    frame_rows = []   # per-frame dicts, in sonic_frames order

    for a, fi, f, sh, px in frames:
        w, h = f.w, f.h
        gw, gh = -(-w // 8) * 8, -(-h // 8) * 8
        tw, th = gw // 8, gh // 8

        grid = [[0] * gw for _ in range(gh)]
        for y in range(h):
            base = y * w
            for x in range(w):
                grid[y][x] = px_index(sh, px[base + x])

        frame_tile_offset = len(tiles) // 32
        frame_piece_offset = len(pieces)
        local_tiles = 0

        for cx0 in range(0, tw, 4):
            for cy0 in range(0, th, 4):
                cols = range(cx0, min(cx0 + 4, tw))
                rows_ = range(cy0, min(cy0 + 4, th))
                used = [(tx, ty) for ty in rows_ for tx in cols
                        if not tile_empty(tile_at(grid, tx, ty))]
                if not used:
                    continue
                minx = min(t[0] for t in used); maxx = max(t[0] for t in used)
                miny = min(t[1] for t in used); maxy = max(t[1] for t in used)
                pw, ph = maxx - minx + 1, maxy - miny + 1

                piece_tile = local_tiles
                for tx in range(minx, maxx + 1):
                    for ty in range(miny, maxy + 1):
                        tiles.extend(pack_tile(tile_at(grid, tx, ty)))
                        local_tiles += 1

                size = ((pw - 1) << 2) | (ph - 1)
                pieces.append((minx * 8, miny * 8, size, piece_tile))

        outer, inner = f.hitboxes[0], f.hitboxes[1]
        frame_rows.append({
            "anim": a.name, "index": fi,
            "tileOffset": frame_tile_offset, "pieceOffset": frame_piece_offset,
            "tileCount": local_tiles, "pieceCount": len(pieces) - frame_piece_offset,
            "pivotX": f.pivotX, "pivotY": f.pivotY, "duration": f.duration,
            "outer": outer, "inner": inner,
        })

    # 4. animation table: frame ranges into frame_rows, in ANIMATIONS order
    anim_rows = []
    first = 0
    for a in anims:
        anim_rows.append({"name": a.name, "first": first, "count": len(a.frames),
                          "loop": a.loopIndex, "speed": a.speed})
        first += len(a.frames)

    max_tiles = max(r["tileCount"] for r in frame_rows)
    max_pieces = max(r["pieceCount"] for r in frame_rows)
    max_tiles_frame = max(frame_rows, key=lambda r: r["tileCount"])
    max_pieces_frame = max(frame_rows, key=lambda r: r["pieceCount"])

    # --- write assets ---
    with open(f"{assetdir}/pal.bin", "wb") as fp:
        entries = [(0, 0, 0)] + final_colours
        for i in range(16):
            fp.write(struct.pack(">H", md_word(entries[i] if i < len(entries) else (0, 0, 0))))

    with open(f"{assetdir}/tiles.bin", "wb") as fp:
        fp.write(tiles)

    # hitbox 0 ("outer") per frame, same order as sonic_frames -- see this
    # function's docstring update above for why this duplicates outer* rather
    # than letting rings.c read sonic_frames[] directly.
    with open(f"{assetdir}/hitbox.bin", "wb") as fp:
        for r in frame_rows:
            fp.write(struct.pack("4b", *r["outer"]))

    with open(f"{srcdir}/sonic_data.h", "w") as fp:
        fp.write(f"""/* Generated by tools/convert_sonic.py. Do not edit by hand. */

#ifndef SONIC_DATA_H
#define SONIC_DATA_H

#include <stdint.h>

typedef struct {{
    int8_t  dx, dy;       /* pixel offset from the frame's top left */
    uint8_t size;         /* ((w-1) << 2) | (h-1), the VDP sprite size field */
    uint8_t tile;         /* first tile, relative to the frame's tile block */
}} SonicPiece;

typedef struct {{
    uint16_t tileOffset;  /* into sonic_tiles, in tiles */
    uint16_t pieceOffset; /* into sonic_pieces */
    uint8_t  tileCount;
    uint8_t  pieceCount;
    int8_t   pivotX, pivotY;
    uint16_t duration;    /* RSDK frame duration, paired with the animator speed */
    int8_t   outerLeft, outerTop, outerRight, outerBottom;
    int8_t   innerLeft, innerTop, innerRight, innerBottom;
}} SonicFrame;

typedef struct {{
    uint16_t first;       /* first frame index */
    uint8_t  count;
    uint8_t  loop;
    int16_t  speed;
}} SonicAnim;

enum {{ {", ".join(enum_name(n) for n in ANIMATIONS)}, SONIC_ANIM_COUNT }};

#define SONIC_MAX_FRAME_TILES {max_tiles}
#define SONIC_MAX_PIECES      {max_pieces}
#define SONIC_FRAME_COUNT     {len(frame_rows)}   /* rows in hitbox.bin, and the
                                     * valid range of the comm frameIndex
                                     * (sh_src/comm.h's COMM_ANIM bits) */

extern const SonicPiece sonic_pieces[];
extern const SonicFrame sonic_frames[];
extern const SonicAnim  sonic_anims[SONIC_ANIM_COUNT];
extern const uint16_t   sonic_pal[16];
extern const uint32_t   sonic_tiles[];

#endif
""")

    with open(f"{srcdir}/sonic_data.c", "w") as fp:
        fp.write("/* Generated by tools/convert_sonic.py. Do not edit by hand. */\n\n")
        fp.write('#include "sonic_data.h"\n\n')

        fp.write("const SonicPiece sonic_pieces[] = {\n")
        pi = 0
        for r in frame_rows:
            for j in range(r["pieceCount"]):
                dx, dy, size, tile = pieces[pi]
                fp.write(f"    {{ {dx}, {dy}, {size}, {tile} }},"
                         f" /* {r['anim']} frame {r['index']} piece {j} */\n")
                pi += 1
        fp.write("};\n\n")

        fp.write("const SonicFrame sonic_frames[] = {\n")
        for r in frame_rows:
            ol, ot, orr, ob = r["outer"]
            il, it, ir, ib = r["inner"]
            fp.write(f"    {{ {r['tileOffset']}, {r['pieceOffset']}, {r['tileCount']}, "
                     f"{r['pieceCount']}, {r['pivotX']}, {r['pivotY']}, {r['duration']}, "
                     f"{ol}, {ot}, {orr}, {ob}, {il}, {it}, {ir}, {ib} }},"
                     f" /* {r['anim']} frame {r['index']} */\n")
        fp.write("};\n\n")

        fp.write("const SonicAnim sonic_anims[SONIC_ANIM_COUNT] = {\n")
        for r in anim_rows:
            fp.write(f"    {{ {r['first']}, {r['count']}, {r['loop']}, {r['speed']} }},"
                     f" /* {r['name']} */\n")
        fp.write("};\n")

    # --- summary ---
    tile_count = len(tiles) // 32
    piece_bytes = len(pieces) * 4
    frame_bytes = len(frame_rows) * 18   # 17 raw bytes, padded to 2-byte alignment
    anim_bytes = len(anim_rows) * 6
    rom_cost = len(tiles) + 32 + piece_bytes + frame_bytes + anim_bytes

    print(f"Sonic.bin -> {assetdir}, {srcdir}")
    print(f"  frames               {len(frame_rows)}")
    print(f"  hitbox.bin           {len(frame_rows) * 4:,} bytes (hitbox 0, one 4-byte row/frame)")
    print(f"  tiles                {tile_count}  ({len(tiles):,} bytes)")
    print(f"  pieces               {len(pieces)}")
    print(f"  rom cost             {rom_cost:,} bytes "
          f"(tiles {len(tiles):,} + pal 32 + pieces {piece_bytes:,} "
          f"+ frames {frame_bytes:,} + anims {anim_bytes:,})")
    print(f"  colours used         {len(usage)}"
          + (f", merged to {PALETTE_COLOURS} ({changed:,} pixels remapped)"
             if changed else ""))
    print(f"  SONIC_MAX_FRAME_TILES {max_tiles}  ({max_tiles_frame['anim']} "
          f"frame {max_tiles_frame['index']})")
    print(f"  SONIC_MAX_PIECES      {max_pieces}  ({max_pieces_frame['anim']} "
          f"frame {max_pieces_frame['index']})")
    print(f"  hitbox pairs         {len(hitbox_pairs)} distinct")
    for outer, inner in sorted(hitbox_pairs):
        print(f"    outer {outer}  inner {inner}")
    print(f"  animations:")
    for r in anim_rows:
        print(f"    {r['name']:<10} {r['count']:>3} frames  first {r['first']:>3}  "
              f"speed {r['speed']:>4}  loop {r['loop']:>3}")


if __name__ == "__main__":
    main()
