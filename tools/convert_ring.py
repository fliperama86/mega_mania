#!/usr/bin/env python3
"""Convert Sonic Mania's Ring sprite (the collectible, not badniks/springs)
into Mega Drive hardware-sprite tiles.

Every needed frame is exactly one 16x16 hardware sprite (2x2 tiles) -- unlike
Sonic (convert_sonic.py), a ring never exceeds one piece, so there is no
piece-chunking step here, just a frame -> 4-tile block.

Frames needed, and why (RSDK Ring_State_Normal/_Sparkle, Ring_Collect --
SonicMania/Objects/Global/Ring.c):
  anim 0 "Normal Ring", all 16 frames. Ring_Draw_Normal sets
    self->direction = self->animator.frameID > 8 before drawing (Ring.c:781),
    i.e. FLIP_X for frames 9..15: baked into the tile pixels here so
    game/md_src/rings.c never flips at runtime (it has no piece/flip
    machinery at all, just a flat tile index per ring frame).
  anim 2 "Sparkle 1" and anim 4 "Sparkle 3", frames 0..maxFrameCount-1 only.
    Ring_Collect (Ring.c:169-176) sets sparkle->maxFrameCount =
    frameCount-1, halving frameCount first if animationID==2 (Ring.c:171-174);
    Ring_State_Sparkle (Ring.c:757-769) destroys the sparkle the tick
    ProcessAnimation's frameID reaches maxFrameCount, so frameID never
    reaches or exceeds it -- frames maxFrameCount..frameCount-1 (including
    the frameID+16 "glow" range Ring_Draw_Sparkle reaches for INK_ADD,
    Ring.c:795-802) are provably never drawn and are not converted. The
    INK_ADD glow duplicate-draw itself is also not reproduced: Mega Drive/
    32X hardware sprites have no additive blend mode, only the global
    shadow/highlight mode, which cannot target one sprite -- see this
    project's report for the ring feature for how prominent that omission
    is (Sparkle 1's second, brighter draw pass).

Emits into <assetdir> (assets/ring):
    tiles.bin   4 tiles (2x2, column-major -- (tx0,ty0),(tx0,ty1),(tx1,ty0),
                (tx1,ty1), matching convert_stage.py/convert_sonic.py's
                block/piece tile order) per frame, frames concatenated in
                the order: anim 0 frames 0-15, then anim 2 frames
                0..maxFrameCount-1, then anim 4 frames 0..maxFrameCount-1.
                4bpp planar, same pack_tile format as convert_sonic.py.

Emits into <srcdir> (game/md_src):
    ring_data.h/.c   frame/tile-base counts, anim 2 and anim 4's per-frame
                durations (RSDK.ProcessAnimation's raw values, Animation.cpp
                Frame.duration), and the chosen CRAM line.

Palette: a ring adds no new CRAM colours -- it reuses one of the four
existing hardware palettes (PAL0-2 = assets/ghz/pal.bin, PAL3 =
assets/sonic/pal.bin, read from <assetdir>'s two sibling directories). Every
ring/sparkle colour actually used, MD-quantized to 3 bits/channel, is matched
to the closest colour already sitting in each candidate line; the line with
the smallest worst-case per-channel error, among colours used by more than
PROMINENT_PIXELS pixels, is chosen. A prominent colour still off by more than
one MD step in any channel under the best available line aborts the
conversion rather than wiring in a visibly wrong ring colour.

Usage: convert_ring.py <Data.rsdk> <assetdir> <srcdir>
"""

import io
import os
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import anim
from convert_stage import md_colour, md_word
from convert_sonic import pack_tile
from rsdk import Pack

from PIL import Image

SHEET_DIR = "Data/Sprites/"
RING_ANIM = "Global/Ring.bin"

ANIM_NORMAL = "Normal Ring"    # RING_TYPE_NORMAL == 0, Ring.h:7
ANIM_SPARKLE1 = "Sparkle 1"    # RING_TYPE_SPARKLE1 == 2, Ring.h:9
ANIM_SPARKLE3 = "Sparkle 3"    # RING_TYPE_SPARKLE3 == 4, Ring.h:11

CANVAS = 16                    # one hardware sprite, 2x2 tiles
FLIP_FROM_FRAME = 9            # Ring_Draw_Normal: direction = frameID > 8

# Below this pixel count a colour is treated as GIF edge noise, not a real
# ring/sparkle colour, when judging whether a palette line's fit is
# acceptable (see module docstring). The smallest genuine colour observed on
# GHZ1's ring/sparkle art sits at 116 pixels; the noise floor sits at 2.
PROMINENT_PIXELS = 16


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


def load_pal_lines(path, count):
    """count palettes of 16 big-endian MD colour words -> count lists of 16
    (r,g,b) 3-bit tuples, same decode as convert_stage.py's md_word encodes."""
    with open(path, "rb") as fp:
        data = fp.read()
    lines = []
    for line in range(count):
        entries = []
        for i in range(16):
            w = struct.unpack_from(">H", data, (line * 16 + i) * 2)[0]
            entries.append(((w >> 1) & 7, (w >> 5) & 7, (w >> 9) & 7))
        lines.append(entries)
    return lines


def choose_palette(usage, candidates):
    """candidates: list of (label, entries[16]). Returns (label, entries,
    colour -> index map, worst error over PROMINENT colours, full per-colour
    report for the winning line)."""
    best = None
    for label, entries in candidates:
        avail = entries[1:]  # index 0 is CRAM-transparent-by-convention, unused
        mapping = {}
        report = []
        worst_prominent = 0
        for col, n in usage.items():
            best_c, best_i, best_err = None, None, None
            for i, c in enumerate(avail):
                err = max(abs(col[k] - c[k]) for k in range(3))
                if best_err is None or err < best_err:
                    best_c, best_i, best_err = c, i + 1, err
            mapping[col] = best_i
            report.append((col, n, best_c, best_err))
            if n >= PROMINENT_PIXELS and best_err > worst_prominent:
                worst_prominent = best_err
        if best is None or worst_prominent < best[3]:
            best = (label, entries, mapping, worst_prominent, report)
    return best


def frame_list(spr, name, max_count=None):
    a = next(x for x in spr.animations if x.name == name)
    frames = a.frames if max_count is None else a.frames[:max_count]
    return a, frames


def render_frame(sheet, frame, flip):
    """16x16 grid of raw MD-quantized (r,g,b) colours or None (transparent),
    frame placed by its pivot the way RSDK draws it (top-left at entityX+
    pivotX, entityY+pivotY -- convert_sonic.py's docstring), canvas origin at
    (8,8) so the entity origin sits at the canvas centre. Returns
    (grid, clipped_pixel_count). flip mirrors the whole assembled canvas
    column-wise (u -> CANVAS-1-u): since the canvas is centred exactly on the
    entity origin, that is precisely RSDK's FLIP_X (mirror about the entity's
    own origin, CheckObjectCollisionTouch/DrawSprite's convention -- see
    convert_sonic.py's sonic_build for the same mirror-about-origin math
    applied to a multi-piece frame instead of one fixed-size canvas)."""
    px = sheet.crop(frame.x, frame.y, frame.w, frame.h)
    grid = [[None] * CANVAS for _ in range(CANVAS)]
    clipped = 0
    for v in range(frame.h):
        cy = CANVAS // 2 + frame.pivotY + v
        for u in range(frame.w):
            idx = px[v * frame.w + u]
            if not idx:
                continue
            cx = CANVAS // 2 + frame.pivotX + u
            if not (0 <= cx < CANVAS and 0 <= cy < CANVAS):
                clipped += 1
                continue
            grid[cy][cx] = sheet.colour(idx)
    if flip:
        grid = [[row[CANVAS - 1 - x] for x in range(CANVAS)] for row in grid]
    return grid, clipped


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    pack = Pack(sys.argv[1])
    assetdir = sys.argv[2]
    srcdir = sys.argv[3]
    assets_root = os.path.dirname(os.path.abspath(assetdir))
    os.makedirs(assetdir, exist_ok=True)
    os.makedirs(srcdir, exist_ok=True)

    spr = anim.load(pack, SHEET_DIR + RING_ANIM)
    sheet = Sheet(pack.read(SHEET_DIR + spr.sheets[0]))

    a0, f0 = frame_list(spr, ANIM_NORMAL)
    a2, _ = frame_list(spr, ANIM_SPARKLE1)
    a4, _ = frame_list(spr, ANIM_SPARKLE3)

    # Ring.c:170-175, transcribed: frameCount halves only for animationID==2
    # (Sparkle 1), then maxFrameCount = frameCount-1 either way.
    def max_frame_count(a, animation_id):
        fc = len(a.frames)
        if animation_id == 2:
            fc >>= 1
        return fc - 1

    max2 = max_frame_count(a2, 2)
    max4 = max_frame_count(a4, 4)
    f2 = a2.frames[:max2]
    f4 = a4.frames[:max4]

    # frame -> (flip, section) for the render pass below
    # DELIBERATE DEVIATION (user's call, 2026-08-17): Ring_Draw_Normal's
    # frameID > 8 flip exists to keep the highlight on the same side (the
    # sheet draws the turn's second half lit from the right, and the flip
    # normalises the lighting) -- baked faithfully, frames 9-15 come out
    # near-identical to 0-7 and a 16 px ring reads as swinging shut and
    # open rather than spinning. Playing the sheet's raw art instead lets
    # the highlight travel around the band, which reads as rotation.
    # Restore `fi >= FLIP_FROM_FRAME` for Mania's exact draw.
    jobs = [(f, False, "anim0") for fi, f in enumerate(f0)]
    jobs += [(f, False, "sparkle1") for f in f2]
    jobs += [(f, False, "sparkle3") for f in f4]

    # 1. colour usage across every needed frame (post-clip: a clipped pixel
    # never reaches the tile data, so it should not skew the palette fit)
    usage = {}
    clip_report = []
    rendered = []
    for i, (f, flip, section) in enumerate(jobs):
        grid, clipped = render_frame(sheet, f, flip)
        rendered.append(grid)
        if clipped:
            clip_report.append((section, i, f.w, f.h, clipped))
        for row in grid:
            for c in row:
                if c is not None:
                    usage[c] = usage.get(c, 0) + 1

    ghz_lines = load_pal_lines(f"{assets_root}/ghz/pal.bin", 3)
    sonic_lines = load_pal_lines(f"{assets_root}/sonic/pal.bin", 1)
    candidates = [(f"PAL{i} (ghz/pal.bin line {i})", e) for i, e in enumerate(ghz_lines)]
    candidates += [("PAL3 (sonic/pal.bin)", sonic_lines[0])]

    label, entries, colour_index, worst, report = choose_palette(usage, candidates)
    pal_number = int(label[3])  # "PAL<n> ..." -> n

    if worst > 1:
        lines = "\n".join(f"    {c} (n={n}) -> {bc} err={e}"
                           for c, n, bc, e in sorted(report, key=lambda r: -r[1]))
        raise SystemExit(
            f"no existing CRAM line fits the ring/sparkle colours within 1 MD "
            f"step for every colour used by >= {PROMINENT_PIXELS} pixels; best "
            f"was {label}, worst-case error {worst}:\n{lines}\n"
            f"stopping rather than wiring in a visibly wrong colour -- widen "
            f"PROMINENT_PIXELS, pick a different line by hand, or accept the "
            f"error and rerun with the check relaxed.")

    # 2. tiles: pack every rendered frame's canvas into 4 tiles, column-major
    tiles = bytearray()
    for grid in rendered:
        for tx in (0, 1):
            for ty in (0, 1):
                rows = []
                for ry in range(8):
                    row = []
                    for rx in range(8):
                        c = grid[ty * 8 + ry][tx * 8 + rx]
                        row.append(colour_index[c] if c is not None else 0)
                    rows.append(row)
                tiles += pack_tile(rows)

    with open(f"{assetdir}/tiles.bin", "wb") as fp:
        fp.write(tiles)

    # 3. generated source: frame/tile-base counts, sparkle durations, palette
    tiles_anim0 = len(f0) * 4
    tiles_sparkle1 = len(f2) * 4
    base_anim0 = 0
    base_sparkle1 = tiles_anim0
    base_sparkle3 = tiles_anim0 + tiles_sparkle1

    with open(f"{srcdir}/ring_data.h", "w") as fp:
        fp.write(f"""/* Generated by tools/convert_ring.py. Do not edit by hand. */

#ifndef RING_DATA_H
#define RING_DATA_H

#include <stdint.h>

/* Ring.c:170-175 (Ring_Collect): sparkle->maxFrameCount = frameCount-1,
 * frameCount halved first for animationID==2 (Sparkle 1). Precomputed here
 * since both operands are link-time constants; game/md_src/rings.c uses
 * these directly instead of re-deriving them from a raw frame count. */
#define RING_ANIM0_FRAMES      {len(f0)}   /* "Normal Ring", Ring.c:525 drives frameID directly */
#define RING_SPARKLE1_MAXFRAME {max2}   /* "Sparkle 1", RING_TYPE_SPARKLE1 == 2 */
#define RING_SPARKLE3_MAXFRAME {max4}   /* "Sparkle 3", RING_TYPE_SPARKLE3 == 4 */

/* Tile offsets into ring_tiles[], in tiles (4 tiles/frame, 2x2). */
#define RING_ANIM0_TILE_BASE     {base_anim0}
#define RING_SPARKLE1_TILE_BASE  {base_sparkle1}
#define RING_SPARKLE3_TILE_BASE  {base_sparkle3}
#define RING_TILE_COUNT          {base_sparkle3 + len(f4) * 4}

/* One of the four existing CRAM lines (PAL0-2 = assets/ghz/pal.bin, PAL3 =
 * assets/sonic/pal.bin); see this file's generator for the fit. */
#define RING_PAL {pal_number}

/* RSDK.ProcessAnimation's raw per-frame durations (Animation.cpp:150-176),
 * indices 0..RING_SPARKLE{{1,3}}_MAXFRAME-1, paired with the animator speed
 * RSDK.Rand(6,8) picks per sparkle (Ring.c:176). */
extern const uint16_t ring_sparkle1_durations[RING_SPARKLE1_MAXFRAME];
extern const uint16_t ring_sparkle3_durations[RING_SPARKLE3_MAXFRAME];

/* ring_tiles is NOT declared here: it lives in cartridge bank 1 (game/
 * tools/gen_assets.py's manifest, ASSET_RING_TILES in the generated
 * game/md_src/assets_gen.h), reached from game/md_src/rings.c through that
 * generated pointer instead of a linked extern array -- see that file's own
 * comment. */

#endif
""")

    with open(f"{srcdir}/ring_data.c", "w") as fp:
        fp.write("/* Generated by tools/convert_ring.py. Do not edit by hand. */\n\n")
        fp.write('#include "ring_data.h"\n\n')
        d2 = ", ".join(str(f.duration) for f in f2)
        d4 = ", ".join(str(f.duration) for f in f4)
        fp.write(f"const uint16_t ring_sparkle1_durations[RING_SPARKLE1_MAXFRAME] = {{ {d2} }};\n")
        fp.write(f"const uint16_t ring_sparkle3_durations[RING_SPARKLE3_MAXFRAME] = {{ {d4} }};\n")

    # --- summary ---
    print(f"Ring.bin -> {assetdir}, {srcdir}")
    print(f"  anim 0 Normal Ring    {len(f0)} frames (flip baked in from frame {FLIP_FROM_FRAME})")
    print(f"  anim 2 Sparkle 1      {len(f2)} of {len(a2.frames)} frames converted (maxFrameCount={max2})")
    print(f"  anim 4 Sparkle 3      {len(f4)} of {len(a4.frames)} frames converted (maxFrameCount={max4})")
    print(f"  tiles                 {len(tiles)//32}  ({len(tiles):,} bytes)")
    print(f"  palette               {label}, worst-case error over colours used "
          f">= {PROMINENT_PIXELS}px: {worst} MD step(s)")
    for c, n, bc, e in sorted(report, key=lambda r: -r[1]):
        flag = "" if n >= PROMINENT_PIXELS else "  (not prominent)"
        print(f"    {c} (n={n:4d}) -> {bc}  err={e}{flag}")
    if clip_report:
        print(f"  WARNING: {len(clip_report)} frame(s) exceeded the 16x16 canvas, clipped:")
        for section, i, w, h, clipped in clip_report:
            print(f"    {section} job#{i}: source {w}x{h} clipped {clipped} px "
                  f"(bottom/right edge dropped)")


if __name__ == "__main__":
    main()
