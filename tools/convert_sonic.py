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
import math
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
              "Air Walk", "Jump", "Push", "Look Up", "Crouch",
              "Spring Twirl", "Spring Diagonal", "Hurt", "Die"]
SHEET_DIR = "Data/Sprites/"
PALETTE_COLOURS = 15   # usable colours; pal.bin entry 0 is transparent

# COMM_ANIM's frameIndex is 7 bits (sh_src/comm.h): the whole exported
# sonic_frames[] table, base animations plus these two, must stay <= 127.
MAX_EXPORTED_FRAMES = 127

# Player.c's animator.rotationStyle per animation (not computed here -- it
# rides each animation's own .ani data in the pack, outside anim.py's parsed
# fields -- but this port's design already settled which of the three
# applies to each of the twelve animations converted, see docs/green-hill.md
# and sh_src/player.h's rotation field comment):
#   ROTSTYLE_FULL (smooth in the original): baked as 8 stepped orientations
#   here, 3 of which (45/90/135 degrees) need new baked art -- the rest are
#   flips of those three or of the unrotated frame (md_src/sonic.c).
ROTATE_ANIMS = {"Walk", "Jog", "Run", "Dash", "Air Walk"}
# ROTSTYLE_180DEG: only two states (upright / flipped both axes), which is
# an exact pixel operation (rotating a raster 180 degrees is flipH+flipV) --
# no baked art needed, md_src/sonic.c does it with the existing frame.
ROT180_ANIMS = {"Idle", "Push", "Look Up", "Crouch"}
# Everything else (Skid, Skid Turn, Jump) is ROTSTYLE_NONE in the original
# sprite sheet: rotation is computed every frame (sh_src/player.c) but never
# displayed for these.
ROTCLASS_NONE, ROTCLASS_R180, ROTCLASS_FULL = 0, 1, 2

# RSDK's rotation unit is 0-511 over a full turn (sh_src/player.h); 64 units
# is 45 degrees. Only these three need baked art -- 0/180 are the unrotated
# frame and its flipH+flipV, and 90/225/270/315 are exact flips of these
# three plus 0 (md_src/sonic.c's orientation-fold comment has the table).
ROT_STEPS_UNITS = [64, 128, 192]   # 45, 90, 135 degrees
# Tile-byte budget for assets/sonic/rot_tiles.bin: sh_src/mars.ld's sonicrot
# region is 0x24000 (143,360 + 4,096 spare); this is the region size minus
# that spare, so a build that fits here is guaranteed to link. Shrunk from
# the original 0x2C000/0x2B000 when springs/signpost art needed a slice of
# sonicrot's own unused tail (sh_src/obj_tiles.s's own comment has the byte
# accounting) -- actual usage today, 113,408 bytes, is comfortably under
# both the old and the new budget.
ROT_TILE_BUDGET = 0x23000


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


def emit_frame_pieces(pixel_grid, w, h, tiles_buf, pieces_buf):
    """Split a w x h palette-index grid (row-major list of lists, index 0
    transparent) into the piece-chunked tile format sonic_frames/
    sonic_pieces use (and, for rotated frames, sonic_rot_frames/
    sonic_rot_pieces share): pad to a multiple of 8, walk 4x4-tile chunks,
    emit one piece per nonempty chunk's trimmed bounding box, tiles
    column-major within a piece so one DMA loads the frame. Appends into
    tiles_buf/pieces_buf in place; returns
    (tileOffset, pieceOffset, tileCount, pieceCount). Factored out of the
    per-base-frame loop below so the rotated-frame baking pass can reuse the
    exact same chunking rule without drifting from it."""
    gw, gh = -(-w // 8) * 8, -(-h // 8) * 8
    tw, th = gw // 8, gh // 8

    grid = [[0] * gw for _ in range(gh)]
    for y in range(h):
        row = pixel_grid[y]
        for x in range(w):
            grid[y][x] = row[x]

    frame_tile_offset = len(tiles_buf) // 32
    frame_piece_offset = len(pieces_buf)
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
                    tiles_buf.extend(pack_tile(tile_at(grid, tx, ty)))
                    local_tiles += 1

            size = ((pw - 1) << 2) | (ph - 1)
            pieces_buf.append((minx * 8, miny * 8, size, piece_tile))

    return (frame_tile_offset, frame_piece_offset, local_tiles,
            len(pieces_buf) - frame_piece_offset)


def rotate_grid(grid, w, h, pivotX, pivotY, rot_units):
    """Nearest-neighbour rotate a w x h palette-index grid (row-major list
    of lists, index 0 transparent) about the frame's pivot by rot_units
    (RSDK's 0-511 rotation scale), mirroring RSDKv5's DrawSpriteRotozoom
    inverse-mapping sampling (dependencies/RSDKv5/RSDKv5/RSDK/Graphics/
    Drawing.cpp:3515-3690, the draw used whenever FX_ROTATE is set):

    - Nearest-neighbour, floor-truncated to an integer source index, same
      spirit as Drawing.cpp's FROM_FIXED (a plain right-shift, i.e. floor,
      not round-to-nearest) -- "mirror its rounding" from this port's own
      design brief -- but sampled at each destination pixel's CENTRE rather
      than its corner (see the centre-vs-corner comment inside the sampling
      loop below for why: corner sampling has a parity bug at exact
      90-degree steps that silently drops half the source image).
    - The same forward-rotation direction the renderer's own angle
      transform produces: angle = 0x200 - (rotation & 0x1FF) unless
      rotation&0x1FF is 0 (Drawing.cpp:3541-3543), then
      srcX = destX*cos(angle) - destY*sin(angle),
      srcY = destX*sin(angle) + destY*cos(angle) (the deltaX/deltaXLen/
      deltaY/deltaYLen accumulation at Drawing.cpp:3644-3667, reduced to its
      closed form for this port's fixed 1:1 scale and FLIP_NONE direction --
      scale/direction the live renderer always uses for ROTSTYLE_FULL, since
      Sonic is never simultaneously scaled while running a loop).
    - The rotated bounding box comes from the same rule DrawSpriteRotozoom
      uses for its own posX[]/posY[] (Drawing.cpp:3601-3631): rotate the
      source rect's four corners forward by rot_units and take the min/max.

    At rot_units == 128 (90 degrees) the trig terms are exactly (0, +-1), so
    this reduces to a pure transpose+flip, pixel-identical to the source, as
    called for by this port's design brief; at 64/192 (45/135 degrees) it is
    ordinary nearest-neighbour sampling.

    Returns (new_grid, new_w, new_h, new_pivotX, new_pivotY), where the
    output pivot is expressed the same way SonicFrame.pivotX/Y already are:
    the P-space (pivot-relative) coordinate of the output's own local (0,0)
    corner, so "top-left drawn at entityX+pivotX" still holds after baking."""
    # round(..., 9): math.cos/sin of a multiple of pi/2 (our 45-degree steps
    # always land on one at the 90/180-degree cases the trig terms should be
    # exactly 0/+-1, per this function's own "90 degrees pixel-exact" claim)
    # comes back as a ~1e-16 epsilon, not a true zero -- left alone, that
    # epsilon pushes floor()/ceil() below to the wrong integer and quietly
    # breaks the exact-transpose case this function exists to get right.
    # Rounding to 9 decimals clears the epsilon while changing nothing at
    # 45/135 degrees, whose true values (+-0.70710678...) are nowhere near
    # an integer boundary.
    angle_table = (0x200 - (rot_units & 0x1FF)) if (rot_units & 0x1FF) else 0
    theta = angle_table * (2.0 * math.pi / 512.0)
    cos_t, sin_t = round(math.cos(theta), 9), round(math.sin(theta), 9)

    rot_deg = rot_units * (360.0 / 512.0)
    rt = math.radians(rot_deg)
    rc, rs = round(math.cos(rt), 9), round(math.sin(rt), 9)

    # Source rect's four corners in P-space (P-space = local + pivot, the
    # same relation SonicFrame.pivotX/Y already encode), rotated forward by
    # rot_units to bound the output.
    corners = [(pivotX, pivotY), (pivotX + w, pivotY),
               (pivotX, pivotY + h), (pivotX + w, pivotY + h)]
    rx = [rc * cx - rs * cy for cx, cy in corners]
    ry = [rs * cx + rc * cy for cx, cy in corners]

    out_min_x = math.floor(min(rx))
    out_min_y = math.floor(min(ry))
    out_w = math.ceil(max(rx)) - out_min_x
    out_h = math.ceil(max(ry)) - out_min_y

    # Sample at pixel CENTRES, not the corner RSDK's own FROM_FIXED technically
    # truncates at: corner-referenced sampling turns out to have a corner-
    # parity bug at exact 90-degree steps (a destination pixel's top-left
    # corner does not rotate to a source pixel's top-left corner, so floor()
    # lands one pixel off along one axis, silently dropping half the source
    # image instead of the exact bijective transpose this port's design
    # brief calls for). Centre-referenced sampling is rotation-symmetric --
    # a pixel's centre always maps to some other pixel's centre under a pure
    # rotation, with no parity hazard -- and is the standard nearest-
    # neighbour convention for exactly this reason; it changes nothing at
    # 45/135 degrees (still ordinary nearest-neighbour) and makes 90 degrees
    # the exact, lossless transpose+flip by construction rather than by luck.
    out = [[0] * out_w for _ in range(out_h)]
    for oy in range(out_h):
        dest_y = out_min_y + oy + 0.5
        for ox in range(out_w):
            dest_x = out_min_x + ox + 0.5
            src_px = cos_t * dest_x - sin_t * dest_y
            src_py = sin_t * dest_x + cos_t * dest_y
            sx = math.floor(src_px - pivotX)
            sy = math.floor(src_py - pivotY)
            if 0 <= sx < w and 0 <= sy < h:
                out[oy][ox] = grid[sy][sx]

    return out, out_w, out_h, out_min_x, out_min_y


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

    # "verify, don't assume" (this port's own rule): check the pack's own
    # per-animation rotationFlag (anim.py's Animation.rotationFlag) against
    # which of the three ROTATE_ANIMS/ROT180_ANIMS/neither bucket each name
    # is hardcoded into above, rather than trusting that bucketing blind.
    # Observed values (dumped once against this pack): 0 = ROTSTYLE_NONE,
    # 1 = ROTSTYLE_FULL, 4 = ROTSTYLE_180DEG -- Spring Twirl/Spring Diagonal
    # both come back 0 (ROTSTYLE_NONE, same class as Jump/Skid/Skid Turn),
    # confirmed here rather than assumed from the brief.
    rot_bad = []
    for a in anims:
        if a.name in ROTATE_ANIMS:
            want = 1
        elif a.name in ROT180_ANIMS:
            want = 4
        else:
            want = 0
        if a.rotationFlag != want:
            rot_bad.append(f"{a.name}: pack rotationFlag={a.rotationFlag}, "
                            f"this converter assumed {want}")
    if rot_bad:
        raise SystemExit("rotationFlag mismatch(es) against this converter's "
                          "ROTATE_ANIMS/ROT180_ANIMS buckets:\n  " + "\n  ".join(rot_bad))

    total_frames = sum(len(a.frames) for a in anims)
    if total_frames > MAX_EXPORTED_FRAMES:
        raise SystemExit(
            f"{total_frames} exported frames exceeds COMM_ANIM's "
            f"{MAX_EXPORTED_FRAMES}-frame budget (sh_src/comm.h's 7-bit "
            f"frameIndex) -- stopping rather than silently truncating; per-"
            f"animation counts: " + ", ".join(f"{a.name}={len(a.frames)}" for a in anims))

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
        raw_grid = [[px_index(sh, px[y * w + x]) for x in range(w)] for y in range(h)]

        frame_tile_offset, frame_piece_offset, local_tiles, piece_count = \
            emit_frame_pieces(raw_grid, w, h, tiles, pieces)

        outer, inner = f.hitboxes[0], f.hitboxes[1]
        frame_rows.append({
            "anim": a.name, "index": fi,
            "tileOffset": frame_tile_offset, "pieceOffset": frame_piece_offset,
            "tileCount": local_tiles, "pieceCount": piece_count,
            "pivotX": f.pivotX, "pivotY": f.pivotY, "duration": f.duration,
            "outer": outer, "inner": inner,
            # Kept only for animations that need baked rotated art (see
            # ROTATE_ANIMS above); discarded below once the rotation pass
            # has consumed it, so it never reaches the summary/emit code.
            "rawGrid": raw_grid if a.name in ROTATE_ANIMS else None,
            "w": w, "h": h,
        })

    # 3b. rotated frames (WALK/JOG/RUN/DASH/AIR_WALK only): three baked
    # orientations per frame -- 45, 90, 135 degrees, RSDK's own
    # ROTSTYLE_45DEG snap (Drawing.cpp:2703-2704); 180 is flipH+flipV of the
    # unrotated frame and 225/270/315 are flipH+flipV of these three, both
    # handled at render time by md_src/sonic.c, not baked here (this port's
    # design brief).
    rot_tiles = bytearray()
    rot_pieces = []
    rot_frame_rows = []          # 3 consecutive entries per contributing base frame
    rot_index = [-1] * len(frame_rows)     # base frame index -> first of its 3 entries
    rot_class = [ROTCLASS_NONE] * len(frame_rows)
    rot_pivot_bad = []

    for bi, r in enumerate(frame_rows):
        if r["anim"] in ROT180_ANIMS:
            rot_class[bi] = ROTCLASS_R180
        elif r["anim"] in ROTATE_ANIMS:
            rot_class[bi] = ROTCLASS_FULL
            rot_index[bi] = len(rot_frame_rows)
            for units in ROT_STEPS_UNITS:
                rg, rw, rh, rpx, rpy = rotate_grid(r["rawGrid"], r["w"], r["h"],
                                                    r["pivotX"], r["pivotY"], units)
                if not (-128 <= rpx <= 127 and -128 <= rpy <= 127):
                    rot_pivot_bad.append(
                        f"{r['anim']} frame {r['index']} @ {units * 360 // 512}deg: "
                        f"pivot ({rpx},{rpy}) out of int8_t")
                    rpx = max(-128, min(127, rpx))
                    rpy = max(-128, min(127, rpy))
                tOff, pOff, tCount, pCount = emit_frame_pieces(rg, rw, rh, rot_tiles, rot_pieces)
                rot_frame_rows.append({
                    "anim": r["anim"], "index": r["index"], "deg": units * 360 // 512,
                    "tileOffset": tOff, "pieceOffset": pOff,
                    "tileCount": tCount, "pieceCount": pCount,
                    "pivotX": rpx, "pivotY": rpy,
                })
        # else ROTCLASS_NONE (Skid/Skid Turn/Jump): index stays -1
        r.pop("rawGrid", None)
        r.pop("w", None)
        r.pop("h", None)

    if rot_pivot_bad:
        for msg in rot_pivot_bad:
            print(f"  WARNING {msg}")
        raise SystemExit(f"{len(rot_pivot_bad)} rotated frame(s) out of int8_t pivot range, aborting")

    if len(rot_tiles) > ROT_TILE_BUDGET:
        raise SystemExit(
            f"rotated tile data {len(rot_tiles):,} bytes exceeds the "
            f"{ROT_TILE_BUDGET:,}-byte budget (sh_src/mars.ld's sonicrot "
            f"region) -- stopping rather than silently dropping animations "
            f"or shrinking art; see this run's summary for the per-anim "
            f"breakdown and report back")

    rot_max_pieces_row = max(rot_frame_rows, key=lambda r: r["pieceCount"], default=None)

    # 4. animation table: frame ranges into frame_rows, in ANIMATIONS order
    anim_rows = []
    first = 0
    for a in anims:
        anim_rows.append({"name": a.name, "first": first, "count": len(a.frames),
                          "loop": a.loopIndex, "speed": a.speed})
        first += len(a.frames)

    # SONIC_MAX_FRAME_TILES/SONIC_MAX_PIECES size md_src/sonic.c's per-frame
    # VRAM window and main.c's sprite-list allocation respectively; both have
    # to cover whichever frame set (base or rotated) sonic_upload/sonic_build
    # end up sourcing from for a given display frame, so the true max is
    # over both tables, not just frame_rows.
    max_tiles_frame = max(frame_rows, key=lambda r: r["tileCount"])
    max_pieces_frame = max(frame_rows, key=lambda r: r["pieceCount"])
    max_tiles = max_tiles_frame["tileCount"]
    max_pieces = max_pieces_frame["pieceCount"]

    rot_max_tiles_row = max(rot_frame_rows, key=lambda r: r["tileCount"], default=None)
    if rot_max_tiles_row and rot_max_tiles_row["tileCount"] > max_tiles:
        max_tiles = rot_max_tiles_row["tileCount"]
        max_tiles_frame = rot_max_tiles_row
    if rot_max_pieces_row and rot_max_pieces_row["pieceCount"] > max_pieces:
        max_pieces = rot_max_pieces_row["pieceCount"]
        max_pieces_frame = rot_max_pieces_row

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

/* sonic_pal/sonic_tiles are NOT declared here: they live in cartridge bank 1
 * (game/tools/gen_assets.py's manifest, ASSET_SONIC_PAL/ASSET_SONIC_TILES in
 * the generated game/md_src/assets_gen.h), reached from game/md_src/main.c
 * and game/md_src/sonic.c through that generated pointer instead of a
 * linked extern array -- see those files' own comments. */

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

    # Rotated frames -- tile pixels go to rot_tiles.bin, linked into the SH2
    # program (sh_src/sonic_rot.s) and read back by the 68000 through the
    # fixed pointer sonic_rot_tiles_md (md_src/sonic.c), same trick as FG
    # High's map (sh_src/map_fgh.s / md_src/main.c's ghz_map_fgh_md); the
    # small per-frame metadata tables below are ordinary 68000-linked C data.
    with open(f"{assetdir}/rot_tiles.bin", "wb") as fp:
        fp.write(rot_tiles)

    with open(f"{srcdir}/sonic_rot_data.h", "w") as fp:
        fp.write(f"""/* Generated by tools/convert_sonic.py. Do not edit by hand. */

#ifndef SONIC_ROT_DATA_H
#define SONIC_ROT_DATA_H

#include <stdint.h>
#include "sonic_data.h"

/* Baked rotated Sonic frames: three orientations (45, 90, 135 degrees) per
 * base frame of WALK/JOG/RUN/DASH/AIR_WALK, the ROTSTYLE_FULL animations
 * (see tools/convert_sonic.py's ROTATE_ANIMS comment and sh_src/player.h's
 * rotation field comment). Tile pixels live in assets/sonic/rot_tiles.bin,
 * linked into the SH2 program (sh_src/sonic_rot.s / sh_src/mars.ld's
 * sonicrot region) and read by the 68000 through the fixed pointer
 * sonic_rot_tiles_md (md_src/sonic.c) -- not through sonic_rot_tiles[] the
 * way sonic_tiles[] is linked in directly, since this data does not fit the
 * 68000's own 512 KB ROM window alongside everything else (same reasoning
 * as FG High's map, md_src/main.c's ghz_map_fgh_md comment). */

typedef struct {{
    uint16_t tileOffset;  /* into sonic_rot_tiles_md, in tiles */
    uint16_t pieceOffset; /* into sonic_rot_pieces */
    uint8_t  tileCount;
    uint8_t  pieceCount;
    int8_t   pivotX, pivotY;
}} SonicRotFrame;

/* Per-animation rotation-display class, indexed by the same absolute frame
 * index sonic_frames[]/COMM_ANIM's frameIndex use (md_src/sonic.c's
 * orientation-fold comment has the full per-class render rule):
 *   SONIC_ROTCLASS_NONE  ANI_JUMP/ANI_SKID/ANI_SKID_TURN -- rotation
 *                         computed (sh_src/player.c) but never displayed,
 *                         baked ROTSTYLE_NONE in the original sprite sheet.
 *   SONIC_ROTCLASS_R180   ANI_IDLE/ANI_PUSH/ANI_LOOK_UP/ANI_CROUCH --
 *                         flipH+flipV of the base frame when dispRot==4,
 *                         upright otherwise; no baked art (exact pixel op).
 *   SONIC_ROTCLASS_FULL   ANI_WALK/ANI_JOG/ANI_RUN/ANI_DASH/ANI_AIR_WALK --
 *                         the 8-orientation fold below, using the 3 baked
 *                         sets this file's tables carry. */
enum {{ SONIC_ROTCLASS_NONE, SONIC_ROTCLASS_R180, SONIC_ROTCLASS_FULL }};

extern const uint8_t       sonic_rot_class[SONIC_FRAME_COUNT];
/* Index into sonic_rot_frames[] of the 45-degree entry for a SONIC_ROTCLASS_
 * FULL base frame (90 is +1, 135 is +2); -1 for every other frame. */
extern const int16_t       sonic_rot_index[SONIC_FRAME_COUNT];
extern const SonicPiece    sonic_rot_pieces[];
extern const SonicRotFrame sonic_rot_frames[];

#endif
""")

    with open(f"{srcdir}/sonic_rot_data.c", "w") as fp:
        fp.write("/* Generated by tools/convert_sonic.py. Do not edit by hand. */\n\n")
        fp.write('#include "sonic_rot_data.h"\n\n')

        fp.write(f"const uint8_t sonic_rot_class[SONIC_FRAME_COUNT] = {{\n")
        class_name = {ROTCLASS_NONE: "SONIC_ROTCLASS_NONE", ROTCLASS_R180: "SONIC_ROTCLASS_R180",
                      ROTCLASS_FULL: "SONIC_ROTCLASS_FULL"}
        for r, cls in zip(frame_rows, rot_class):
            fp.write(f"    {class_name[cls]}, /* {r['anim']} frame {r['index']} */\n")
        fp.write("};\n\n")

        fp.write(f"const int16_t sonic_rot_index[SONIC_FRAME_COUNT] = {{\n")
        for r, idx in zip(frame_rows, rot_index):
            fp.write(f"    {idx}, /* {r['anim']} frame {r['index']} */\n")
        fp.write("};\n\n")

        fp.write("const SonicPiece sonic_rot_pieces[] = {\n")
        pi = 0
        for r in rot_frame_rows:
            for j in range(r["pieceCount"]):
                dx, dy, size, tile = rot_pieces[pi]
                fp.write(f"    {{ {dx}, {dy}, {size}, {tile} }},"
                         f" /* {r['anim']} frame {r['index']} @ {r['deg']}deg piece {j} */\n")
                pi += 1
        fp.write("};\n\n")

        fp.write("const SonicRotFrame sonic_rot_frames[] = {\n")
        for r in rot_frame_rows:
            fp.write(f"    {{ {r['tileOffset']}, {r['pieceOffset']}, {r['tileCount']}, "
                     f"{r['pieceCount']}, {r['pivotX']}, {r['pivotY']} }},"
                     f" /* {r['anim']} frame {r['index']} @ {r['deg']}deg */\n")
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

    print(f"\n  rotated frames (WALK/JOG/RUN/DASH/AIR_WALK, 45/90/135deg):")
    print(f"    baked frames         {len(rot_frame_rows)}  "
          f"(48 base frames x 3 orientations)")
    print(f"    tiles                {len(rot_tiles) // 32}  ({len(rot_tiles):,} bytes)")
    print(f"    budget               {ROT_TILE_BUDGET:,} bytes "
          f"(sh_src/mars.ld sonicrot region 0x2C000 minus spare) -- "
          f"{'OK' if len(rot_tiles) <= ROT_TILE_BUDGET else 'OVER BUDGET'}")
    print(f"    pieces               {len(rot_pieces)}")
    if rot_max_tiles_row:
        print(f"    max tiles/frame      {rot_max_tiles_row['tileCount']}  "
              f"({rot_max_tiles_row['anim']} frame {rot_max_tiles_row['index']} "
              f"@ {rot_max_tiles_row['deg']}deg)")
    if rot_max_pieces_row:
        print(f"    max pieces/frame     {rot_max_pieces_row['pieceCount']}  "
              f"({rot_max_pieces_row['anim']} frame {rot_max_pieces_row['index']} "
              f"@ {rot_max_pieces_row['deg']}deg)"
              + ("  ** exceeds the 4 pieces this port assumed -- "
                 "SONIC_MAX_PIECES raised, report this **"
                 if rot_max_pieces_row["pieceCount"] > 4 else ""))
    print(f"    per-anim tile counts (sum of that anim's 3 orientations x its frame count):")
    by_anim = {}
    for r in rot_frame_rows:
        by_anim.setdefault(r["anim"], []).append(r["tileCount"])
    for name in ANIMATIONS:
        if name in by_anim:
            counts = by_anim[name]
            print(f"      {name:<10} {sum(counts):>5} tiles over {len(counts):>3} baked frames")


if __name__ == "__main__":
    main()
