#!/usr/bin/env python3
"""Convert a Mania stage into Mega Drive assets.

Emits into <outdir>:
    pal.bin      3 palettes of 16 MD colours (BGR333, entry 0 transparent);
                 the fourth hardware palette is left for the player
    tiles.bin    unique 8x8 4bpp tiles, deduplicated including flips
    blocks.bin   per 16x16 block, 4 name table entries (tile, palette, flips).
                 A layout cell whose scene entry has flipX/flipY set (see
                 scene.py) does not reuse the plain block: it points at a
                 flip-variant block instead, the same 4 entries recomposed
                 for that mirroring (quadrant swap plus per-entry hflip/vflip
                 XOR). Variants are appended after every base block, in the
                 order their (base block, flipX, flipY) combination is first
                 encountered scanning FG Low, then BG Outside, then FG High.
                 Still one block index space, under the map cell's 12-bit
                 budget (4095 blocks max).
    map_fg.bin   FG Low layout: u16 per cell, bits 0-11 block index (base or
                 flip variant -- the flip itself is baked into which block
                 this is, not stored again here), bits 12-15 the scene
                 entry's full solidity nibble unmasked: bit 12 floor solid,
                 bit 13 wall and roof solid (RSDK path A, tested explicitly
                 by game/md_src/main.c:39-41's MAP_SOLID_FLOOR/MAP_SOLID_SIDES
                 and game/sh_src/path.c:37-39's SOLID_FLOOR/SOLID_SIDES),
                 bits 14-15 RSDK path B (carried through but not tested by
                 anything yet -- see collide_rows.bin below). The block index
                 mask (MAP_BLOCK_MASK, both files above) is 0x0FFF, so bits
                 12-15 riding along unmasked cost nothing today.
    map_fgh.bin  FG High layout (loop fronts, overhangs), same format and
                 dimensions as map_fg.bin -- the runtime has no separate
                 ghz_map_w/ghz_map_h for it, it reuses FG Low's published
                 ones (game/md_src/descriptor.h's GHZ_MAP_W/GHZ_MAP_H
                 comment), so this converter asserts the two layers' scene
                 dimensions match before emitting either map.
    map_bg.bin   BG Outside layout, same format
    collide_rows.bin   each DISTINCT 70-byte collision row once (floor, left
                 wall, right wall and roof masks of 16 each, then the four
                 angles, the flag and a pad byte to keep the stride even),
                 first-encounter order over blocks.bin's block order. Rows
                 only ever encode RSDK path A (TileConfig path 0, see
                 load_collision below) -- path B's bits 14-15 in the map
                 files above have no matching row of their own yet; that is
                 deferred until path B collision support lands.
    collide_index.bin  one big-endian u16 per block (base or flip variant,
                 same order as blocks.bin): the row number into
                 collide_rows.bin for that block. Many blocks share a row
                 (different tiles that collide the same way, or flip
                 variants whose masks happen to be symmetric), so this is
                 usually much smaller than blocks*70 bytes -- see
                 game/md_src/descriptor.h's ghz_collide_index/
                 ghz_collide_rows comment for why that saving matters

Block 0 is blank and block 1 is a visible X, so empty map cells read as sky
while anything that failed conversion shows up as an obvious marker.

Every 8x8 tile draws from one hardware palette of fifteen usable colours, so
tiles are fitted to palettes most-used-first. A tile whose
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

# Layout entry flip bits (tools/scene.py's docstring: "bits 0-9 tile index,
# 10-11 flip"), distinct from the nametable word's own hflip/vflip bits below
# -- source format and MD output format, same concept, different bit
# positions, so these get separate names to avoid mixing them up.
ENTRY_FLIPX = 1 << 10
ENTRY_FLIPY = 1 << 11

# MD nametable word flip bits (main.c's format comment, convert_stage.py's
# own block-building loop below): bits 0-10 tile, 11 hflip, 12 vflip.
NAMETABLE_HFLIP = 1 << 11
NAMETABLE_VFLIP = 1 << 12

FG_LAYER = "FG Low"
BG_LAYER = "BG Outside"
FGH_LAYER = "FG High"

# The last hardware palette belongs to the player, so the stage gets three.
# That costs some colour accuracy on rare tiles and is the only way a character
# can keep its own colours.
STAGE_PALETTES = 3


def md_colour(rgb):
    """Quantize to the MD's three bits per channel."""
    return (rgb[0] >> 5, rgb[1] >> 5, rgb[2] >> 5)


def md_word(c):
    """MD CRAM word: 0000 BBB0 GGG0 RRR0"""
    return (c[2] << 9) | (c[1] << 5) | (c[0] << 1)


def load_collision(pack, stage, paths=1, tiles=1024):
    """TileConfig.bin: per collision path, per tile, 16 height bytes, 16
    active bytes, then yFlip, floor/lWall/rWall/roof angles and a flag.
    Only path 0 is needed for now (RSDK path A; path B is bits 14-15 of the
    scene entry, not carried yet, see the module docstring).

    yFlip (Scene.cpp:753-808) is a Mania tile-authoring flag baked into the
    tile's own collision shape -- unrelated to the per-cell flipX/flipY a
    layout entry can also carry, which flip_x_collision/flip_y_collision
    below handle separately."""
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
            lwa = buf[pos]; pos += 1
            rwa = buf[pos]; pos += 1
            roofa = buf[pos]; pos += 1
            flag = buf[pos]; pos += 1
            if p == 0:
                if yflip:
                    # A yFlip tile hangs its mask from the roof instead of
                    # the floor: floor is "no gap" (0x00) wherever active,
                    # and the stored heights become the roof. Scene.cpp:753-762.
                    floors = [0x00 if active[c] else 0xFF for c in range(16)]
                    roof = [heights[c] if active[c] else 0xFF for c in range(16)]

                    # Wall rotations scan the ROOF masks with c <= m, the
                    # mirror of the regular-tile scan below (FLOOR masks,
                    # c >= m). Scene.cpp:766-785 (lwall), 788-807 (rwall).
                    lwall = []
                    for c in range(16):
                        h = 0
                        while True:
                            if h == 16:
                                lwall.append(0xFF); break
                            m = roof[h]
                            if m != 0xFF and c <= m:
                                lwall.append(h); break
                            h += 1
                    rwall = []
                    for c in range(16):
                        h = 15
                        while True:
                            if h == -1:
                                rwall.append(0xFF); break
                            m = roof[h]
                            if m != 0xFF and c <= m:
                                rwall.append(h); break
                            h -= 1
                else:
                    floors = [heights[c] if active[c] else 0xFF for c in range(16)]

                    # Wall masks are not stored; RSDK rotates the floor masks in
                    # LoadTileConfig and this reproduces those loops exactly.
                    # Scene.cpp:824-843 (lwall), 845-865 (rwall).
                    lwall = []
                    for c in range(16):
                        h = 0
                        while True:
                            if h == 16:
                                lwall.append(0xFF); break
                            m = floors[h]
                            if m != 0xFF and c >= m:
                                lwall.append(h); break
                            h += 1
                    rwall = []
                    for c in range(16):
                        h = 15
                        while True:
                            if h == -1:
                                rwall.append(0xFF); break
                            m = floors[h]
                            if m != 0xFF and c >= m:
                                rwall.append(h); break
                            h -= 1

                    # RSDK gives a regular tile a flat roof mask wherever the
                    # column is active, see LoadTileConfig, Scene.cpp:815
                    roof = [0x0F if active[c] else 0xFF for c in range(16)]

                out[t] = (bytes(floors), bytes(lwall), bytes(rwall),
                          bytes(roof), floor, lwa, rwa, roofa, flag)
    return out


def flip_x_collision(row):
    """A block's collision row, mirrored horizontally: RSDK's FlipX variant
    generation, transcribed exactly from Scene.cpp:870-894. Wall masks swap
    sides and mirror within the row (0xFF, "no wall", stays 0xFF); floor and
    roof masks mirror column-for-column; angles negate, with lWall/rWall
    swapping like the masks; the flag passes through unchanged."""
    fl, lw, rw, rf, fa, la, ra, roa, flag = row
    new_fa = (-fa) & 0xFF
    new_la = (-ra) & 0xFF
    new_ra = (-la) & 0xFF
    new_roa = (-roa) & 0xFF
    new_lw = bytes(0xFF if rw[c] == 0xFF else 0xF - rw[c] for c in range(16))
    new_rw = bytes(0xFF if lw[c] == 0xFF else 0xF - lw[c] for c in range(16))
    new_fl = bytes(fl[15 - c] for c in range(16))
    new_rf = bytes(rf[15 - c] for c in range(16))
    return (new_fl, new_lw, new_rw, new_rf, new_fa, new_la, new_ra, new_roa, flag)


def flip_y_collision(row):
    """A block's collision row, mirrored vertically: RSDK's FlipY variant
    generation, transcribed exactly from Scene.cpp:897-921. Floor and roof
    masks swap top for bottom (0xFF stays 0xFF); wall masks mirror
    column-for-column; angles negate through -0x80 (RSDK's flip-vertical
    angle identity, not a plain negate like FlipX); the flag passes through
    unchanged."""
    fl, lw, rw, rf, fa, la, ra, roa, flag = row
    new_fa = (-0x80 - roa) & 0xFF
    new_la = (-0x80 - la) & 0xFF
    new_ra = (-0x80 - ra) & 0xFF
    new_roa = (-0x80 - fa) & 0xFF
    new_fl = bytes(0xFF if rf[c] == 0xFF else 0xF - rf[c] for c in range(16))
    new_rf = bytes(0xFF if fl[c] == 0xFF else 0xF - fl[c] for c in range(16))
    new_lw = bytes(lw[15 - c] for c in range(16))
    new_rw = bytes(rw[15 - c] for c in range(16))
    return (new_fl, new_lw, new_rw, new_rf, new_fa, new_la, new_ra, new_roa, flag)


def flip_xy_collision(row):
    """A block's collision row, mirrored both ways: RSDK's FlipXY variant
    generation, Scene.cpp:924-949. The reference builds it by applying the
    FlipX transform to the FlipY variant's fields (tileInfo/collisionMasks
    index off + offY as source), not to the base row directly -- so this is
    that same composition, and matches Scene.cpp line for line under
    substitution: every read of "t + offY" above becomes the flip_y_collision
    result fed into flip_x_collision here."""
    return flip_x_collision(flip_y_collision(row))


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


def flip_block_entries(entries, flip_x, flip_y):
    """A block's 4 nametable words (order TL, TR, BL, BR, see main.c's
    draw_block_column/row), recomposed for a flipped layout cell: quadrants
    swap to mirror the 16x16 arrangement, and each surviving quadrant's own
    hflip/vflip bit is XORed (not set outright, so this composes correctly
    with whatever flip TileBank's 8x8 dedup already baked into that entry)."""
    tl, tr, bl, br = entries
    if flip_x and flip_y:
        order, xor = (br, bl, tr, tl), NAMETABLE_HFLIP | NAMETABLE_VFLIP
    elif flip_x:
        order, xor = (tr, tl, br, bl), NAMETABLE_HFLIP
    else:
        order, xor = (bl, br, tl, tr), NAMETABLE_VFLIP
    return [e ^ xor for e in order]


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

    if FGH_LAYER in layers:
        # The runtime has no ghz_map_w/ghz_map_h of its own for FG High; it
        # reuses FG Low's (descriptor.h's GHZ_MAP_W/GHZ_MAP_H comment), so a
        # scene where the two layers disagree in size would silently
        # under/over-read map_fgh.bin at runtime instead of failing here.
        fg_layer, fgh_layer = layers[FG_LAYER], layers[FGH_LAYER]
        if (fgh_layer.w, fgh_layer.h) != (fg_layer.w, fg_layer.h):
            raise SystemExit(
                f"{FGH_LAYER} is {fgh_layer.w}x{fgh_layer.h} but {FG_LAYER} is "
                f"{fg_layer.w}x{fg_layer.h} -- the runtime publishes only one "
                f"map_w/map_h pair and reuses it for both layers, so they "
                f"must match")

    # Fit palettes across foreground, background and FG High together, since
    # they share one 64 colour CRAM between them.
    wanted = [("fg", FG_LAYER), ("bg", BG_LAYER), ("fgh", FGH_LAYER)]
    usage = Counter()
    used = {}
    for tag, name in wanted:
        if name in layers:
            used[tag] = layers[name]
            usage.update(layers[name].usage())

    keep = [t for t, _ in usage.most_common(topn)]
    colours = {t: ts.colours(t) for t in keep}
    palettes, assign, exact = fit_palettes(keep, colours, usage,
                                           count=STAGE_PALETTES)
    pal_index = [{c: i + 1 for i, c in enumerate(sorted(p))} for p in palettes]

    bank = TileBank()
    bank.add(tuple(tuple(0 for _ in range(8)) for _ in range(8)))       # blank
    bank.add(tuple(tuple(1 if (x == y or x == 7 - y) else 0
                         for x in range(8)) for y in range(8)))          # X

    # Loaded before the block loop below so it can populate block_collision
    # (base tile -> collision row keyed by BLOCK index) as each block is
    # created; the old code loaded this after and re-derived tile-from-block
    # by reverse-scanning block_of at write time, which has no entry for a
    # flip-variant block (variants are never in block_of, only in blocks).
    collision = load_collision(pack, stage)

    blocks = [[BLANK] * 4, [PLACEHOLDER] * 4]
    block_of = {}
    block_collision = {}
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
        if collision and t in collision:
            block_collision[len(blocks)] = collision[t]
        blocks.append(entries)

    # Flip-variant blocks, created on demand by the map loop below as
    # flipped cells are found; variant_of dedups so the same (base block,
    # flipX, flipY) combination never gets a second block. New blocks/rows
    # are appended, never inserted, so every index handed out earlier in
    # this function (BLANK, PLACEHOLDER, every base block above) keeps its
    # value -- blocks.bin's base-block section, and the block_collision row
    # each base block maps to, are byte-identical to what this function
    # produced before flips existed (collide_index.bin/collide_rows.bin
    # below repackage block_collision, so this guarantee carries through
    # the dedup too, just not as a literal file-prefix match anymore).
    variant_of = {}

    def variant_block(base_b, flip_x, flip_y):
        key = (base_b, flip_x, flip_y)
        b = variant_of.get(key)
        if b is not None:
            return b
        b = len(blocks)
        variant_of[key] = b
        blocks.append(flip_block_entries(blocks[base_b], flip_x, flip_y))
        row = block_collision.get(base_b)
        if row is not None:
            if flip_x and flip_y:
                block_collision[b] = flip_xy_collision(row)
            elif flip_x:
                block_collision[b] = flip_x_collision(row)
            else:
                block_collision[b] = flip_y_collision(row)
        return b

    maps = {}
    for tag, layer in used.items():
        # FG High shares FG Low's mapW/mapH clamp -- the assert above already
        # guarantees they're the same scene size, and the runtime reuses FG
        # Low's published dimensions for FG High too.
        w = min(mapw, layer.w) if tag in ("fg", "fgh") else layer.w
        h = min(maph, layer.h) if tag in ("fg", "fgh") else layer.h
        data = bytearray()
        missing = 0
        for y in range(h):
            for x in range(w):
                e = layer.entry(x, y)
                if e == scene.EMPTY:
                    b = BLANK
                else:
                    t = e & scene.TILE_MASK
                    base_b = block_of.get(t)
                    if base_b is None:
                        b = PLACEHOLDER
                        missing += 1
                    else:
                        flip_x = bool(e & ENTRY_FLIPX)
                        flip_y = bool(e & ENTRY_FLIPY)
                        b = variant_block(base_b, flip_x, flip_y) \
                            if (flip_x or flip_y) else base_b
                    # Carry the scene entry's full solidity nibble (bits
                    # 12-15), not just path A's bits 12-13: without bit 12/13
                    # every decorative flower reads as ground, and bits 14-15
                    # (path B) are inert today (MAP_BLOCK_MASK/BLOCK_MASK is
                    # 0x0FFF and only bits 12-13 are tested -- game/md_src/
                    # main.c:39-41, game/sh_src/path.c:37-39) but future-proof
                    # to carry now rather than mask off and re-derive later.
                    b |= e & 0xF000
                data += struct.pack(">H", b)
        maps[tag] = (w, h, data, missing)

    if len(blocks) >= 4096:
        raise SystemExit(f"block budget exceeded: {len(blocks)} blocks, but "
                          f"a map cell's block index is 12 bits (max 4095) -- "
                          f"see convert_stage.py's docstring")

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

    unique_rows = 0
    stale = f"{out}/collide.bin"
    if os.path.exists(stale):
        # old single-file packaging, superseded by the index/rows split
        # below; never leave a stale copy for md_src/assets.s to drift from.
        os.remove(stale)

    if collision:
        # collide_rows.bin holds each DISTINCT 70-byte row once, first
        # encounter order; collide_index.bin holds one big-endian u16 per
        # block (same order as blocks.bin) naming that row. Many blocks
        # share a row -- different tiles that happen to collide the same
        # way, or flip variants whose masks happen to be symmetric -- so
        # this is a real saving, not just bookkeeping: on GHZ it cuts
        # collision packaging by about three quarters (see game/md_src/
        # descriptor.h's ghz_collide_index/ghz_collide_rows comment for the
        # exact before/after this brought the ROM under its 512 KB window).
        # blank, placeholder and any block with no TileConfig entry share
        # the same all-absent (0xFF) row like everything else here.
        # block_collision is keyed directly by block index (filled in
        # above, for both base blocks and flip variants), not re-derived
        # from block_of -- a variant block was never a key of block_of.
        row_index = {}    # 70-byte row bytes -> index into row_bytes_list
        row_bytes_list = []
        indices = []
        solid = 0
        for i in range(len(blocks)):
            row = block_collision.get(i)
            if row is None:
                row_bytes = b"\xff" * 64 + bytes(6)
            else:
                fl, lw, rw, rf, fa, la, ra, roa, flag = row
                row_bytes = fl + lw + rw + rf + bytes([fa, la, ra, roa, flag, 0])
                if any(c != 0xFF for c in fl):
                    solid += 1
            ri = row_index.get(row_bytes)
            if ri is None:
                ri = len(row_bytes_list)
                row_index[row_bytes] = ri
                row_bytes_list.append(row_bytes)
            indices.append(ri)

        with open(f"{out}/collide_index.bin", "wb") as f:
            for ri in indices:
                f.write(struct.pack(">H", ri))
        with open(f"{out}/collide_rows.bin", "wb") as f:
            for row_bytes in row_bytes_list:
                f.write(row_bytes)

        unique_rows = len(row_bytes_list)
        print(f"  collision           {len(blocks)} blocks, {solid} solid, "
              f"{unique_rows} unique rows")

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
