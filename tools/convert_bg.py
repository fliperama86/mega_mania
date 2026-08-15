#!/usr/bin/env python3
"""Convert Sonic Mania's background layer into 32X framebuffer assets.

Emits into <outdir>:
    bg_pal.bin     256 entries, 2 bytes each, big endian, 32X CRAM format
                   (0BBBBBGGGGGRRRRR). The 32X is 8bpp with one full 256
                   entry CRAM, so this is the tileset's own colours with no
                   fitting or reduction. Index 0 is the tileset's own
                   transparent/blank colour.
    bg_blocks.bin  the distinct 16x16 blocks the layer actually uses, raw
                   8bpp pixels (indices into bg_pal.bin), 256 bytes per
                   block, row major, deduplicated. RSDK's per-cell H/V flip
                   is baked into the pixels here, since bg_map.bin below
                   carries no flip flag of its own.
    bg_map.bin     the layer's layout, big endian u16 indices into
                   bg_blocks.bin, row major.
    bg_lines.bin   one entry per background pixel row, 4 bytes each big
                   endian: u16 parallaxFactor, u16 scrollSpeed. Flattens the
                   scene's band indirection so the renderer just looks up a
                   line and gets its numbers.

Usage: convert_bg.py <Data.rsdk> <stage> <outdir> [scene] [layer]
"""

import io
import os
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import scene
from rsdk import Pack

from PIL import Image

DEFAULT_LAYER = "BG Outside"


def cram_word(rgb):
    """32X CRAM word: 0BBBBBGGGGGRRRRR, 5 bits per channel, straight from
    the tileset's own 8-bit colour, no quantisation."""
    r, g, b = rgb
    return ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3)


class Tileset:
    """The stage's 16x16 tiles, as a 16 x 16384 indexed GIF. Raw palette
    indices, unlike convert_stage.py's MD-quantized reader: the 32X keeps
    the tileset's own 8bpp colours untouched."""

    def __init__(self, gif):
        img = Image.open(io.BytesIO(gif))
        pal = img.getpalette()
        self.colours = [tuple(pal[i:i + 3]) for i in range(0, len(pal), 3)]
        self.px = list(img.getdata())
        self.w = img.size[0]

    def block(self, t, hflip, vflip):
        """16x16 raw palette-index rows for tile t, flipped as requested."""
        base = t * 16 * self.w
        rows = [self.px[base + y * self.w: base + y * self.w + 16]
                for y in range(16)]
        if vflip:
            rows.reverse()
        if hflip:
            rows = [row[::-1] for row in rows]
        return rows


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    pack = Pack(sys.argv[1])
    stage = sys.argv[2]
    out = sys.argv[3]
    scene_name = sys.argv[4] if len(sys.argv) > 4 else "Scene1.bin"
    layer_name = sys.argv[5] if len(sys.argv) > 5 else DEFAULT_LAYER
    os.makedirs(out, exist_ok=True)

    ts = Tileset(pack.read(f"Data/Stages/{stage}/16x16Tiles.gif"))
    layers = scene.load(pack, stage, scene_name)
    if layer_name not in layers:
        raise SystemExit(f"no {layer_name}; have: " + ", ".join(layers))
    layer = layers[layer_name]

    # Distinct blocks actually referenced. Flip is decoded per RSDKv5's own
    # FlipFlags (FLIP_X = bit 10, FLIP_Y = bit 11, see Scene.cpp DrawLayerBasic
    # and Drawing.hpp) and baked into the pixels before dedup, since the map
    # format below has no flip flag of its own.
    block_index = {}      # pixel bytes -> block index
    block_data = []        # 256-byte blocks, first-seen order
    map_indices = []       # w*h entries, row major

    for y in range(layer.h):
        for x in range(layer.w):
            e = layer.entry(x, y)
            t = e & scene.TILE_MASK
            hflip = bool((e >> 10) & 1)
            vflip = bool((e >> 11) & 1)
            rows = ts.block(t, hflip, vflip)
            key = bytes(v for row in rows for v in row)
            idx = block_index.get(key)
            if idx is None:
                idx = len(block_data)
                block_index[key] = idx
                block_data.append(key)
            map_indices.append(idx)

    # Verify the scroll data's shape rather than trusting it: one band index
    # per pixel row, and RSDKv5's deform field (per-line rotozoom) unused.
    if len(layer.line_scroll) != layer.h * 16:
        raise SystemExit(f"line scroll is {len(layer.line_scroll)} bytes, "
                          f"expected {layer.h * 16} (layer height in px)")
    if any(b.deform for b in layer.scroll_info):
        raise SystemExit("a scroll band uses deform; renderer format has "
                          "no field for it")

    lines = bytearray()
    line_vals = []
    for band_idx in layer.line_scroll:
        band = layer.scroll_info[band_idx]
        line_vals.append((band.parallax, band.speed))
        lines += struct.pack(">HH", band.parallax, band.speed)

    with open(f"{out}/bg_pal.bin", "wb") as f:
        for i in range(256):
            c = ts.colours[i] if i < len(ts.colours) else (0, 0, 0)
            f.write(struct.pack(">H", cram_word(c)))

    with open(f"{out}/bg_blocks.bin", "wb") as f:
        for b in block_data:
            f.write(b)

    with open(f"{out}/bg_map.bin", "wb") as f:
        for idx in map_indices:
            f.write(struct.pack(">H", idx))

    with open(f"{out}/bg_lines.bin", "wb") as f:
        f.write(lines)

    used_colours = {px for b in block_data for px in b}
    parallax_factors = sorted({p for p, _ in line_vals})

    # Runs of consecutive rows that drift under their own scroll speed,
    # independent of the camera-driven parallaxFactor.
    drift_runs = []
    run_start, run_val = 0, line_vals[0]
    for i in range(1, len(line_vals) + 1):
        v = line_vals[i] if i < len(line_vals) else None
        if v != run_val:
            if run_val[1] != 0:
                drift_runs.append((run_start, i - 1, run_val))
            run_start, run_val = i, v

    print(f"stage {stage} / {layer_name} -> {out}")
    print(f"  blocks              {len(block_data)}  "
          f"({len(block_data) * 256:,} bytes)")
    print(f"  map                 {layer.w} x {layer.h}  "
          f"({len(map_indices) * 2:,} bytes)")
    print(f"  palette             256 entries  "
          f"({len(used_colours)} colours used)")
    print(f"  parallax factors    {len(parallax_factors)} distinct "
          f"({min(parallax_factors)}-{max(parallax_factors)})")
    if drift_runs:
        print(f"  drift               {len(drift_runs)} line ranges scroll "
              f"independent of the camera:")
        for lo, hi, (parallax, speed) in drift_runs:
            print(f"    rows {lo:>3}-{hi:<3}  parallax {parallax:<4} "
                  f"speed {speed}")
    else:
        print("  drift               none")


if __name__ == "__main__":
    main()
