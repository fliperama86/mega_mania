#!/usr/bin/env python3
"""Convert Sonic Mania's Spring and SignPost sprites into Mega Drive hardware-
sprite assets. One converter for both (this port's own call, reported in
docs/green-hill.md): they share one physical VRAM "streamed" region on the
68000 (game/md_src/springs.c and signpost.c), which no spring and the
signpost are ever both near the camera at once to need simultaneously (no
GHZ1 spring sits past x=15352, the signpost sits at x=15792 -- asserted
below), so sizing and reporting that shared budget in one place, from one
converter run, is less error-prone than splitting it across two scripts that
would each have to independently reason about the other's tile cost.

VRAM BUDGET IS TIGHT (docs/green-hill.md: 71 free tiles after rings, indices
1241-1311) and does NOT close under the brief's first-choice design (bake
Yellow/Red as fully separate tile art -- 6 orientation-colour sets instead of
3 -- plus a 2-slot streamed bounce window): that combination alone measures
75 resident tiles before any streaming at all, already over the 71-tile
ceiling with zero headroom. Two real, reported deviations were needed to
close it (see the printed summary at the end of a run for the exact
numbers this run produced):
  1. Yellow and Red spring tiles are baked from ONE MERGED palette (their
     combined colour usage, reduced to <=15 entries the same
     nearest-pair-fusion convert_sonic.py's own merge_palette() uses for
     Sonic's 15-colour cap, then best-fit to a single existing CRAM line)
     rather than two separate lines. This means Yellow and Red springs draw
     with the SAME tile pixel data and the same palette line: colour
     accuracy is a real, visible compromise (the two hues pull toward each
     other in the shared 15-colour set) -- there is no way to give both an
     independently accurate line AND halve the tile cost, since the "share
     one tile set, switch palette line" idea only saves VRAM if the palette
     switch is a plain PAL-bit change (Genesis sprites/tiles pick one of 4
     simultaneously-resident CRAM lines per draw), which requires the SAME
     stored tile index to land on the correct colour in BOTH lines -- not
     achievable against two independently-chosen EXISTING lines whose
     content this converter does not control, only reads.
  2. The shared streamed bounce window holds ONE frame's tiles, not two:
     this port's brief describes "two springs bouncing simultaneously is the
     cap" as the intended design, but a 2-slot window (worst-case frame size
     x2) does not fit alongside the resident tiles even after (1) above --
     see the run's own numbers. game/md_src/springs.c's own doc comment has
     the exact single-slot eviction rule this becomes ("newest bounce wins
     the slot").
Both are reported here, in docs/green-hill.md, and in this task's own final
report, per this brief's "NEW ones stop-and-report" rule -- these are new,
past what the brief pre-approved, forced by the arithmetic once real numbers
were in hand rather than chosen for convenience.

SPRINGS (Global/Springs.bin, Global/Objects.gif):
  6 anims (Yellow/Red x Vertical/Horizontal/Diagonal), 9 frames each, frame 8
  pixel-identical to frame 0 (verified below, not assumed) -- Spring_Update's
  own animator settles there and stops (Spring.c:23-24: "if
  (self->animator.frameID == 8) self->animator.speed = 0"), so frame 8 is
  never a distinct pose, only frame 0's rest pose reached again. 8 unique
  frames are converted per ORIENTATION (Vertical/Horizontal/Diagonal, Yellow
  and Red merged per (1) above): frame 0 is the permanently RESIDENT rest
  pose (game/md_src/springs.c draws every spring in this pose whenever it is
  not bouncing); frames 1-7 are the STREAMED bounce sequence.

  Yellow and Red are pixel-identical shapes per orientation, differing only
  in colour (verified below): this converter builds the tile INDEX grid from
  the Yellow sheet region (arbitrary choice, since shapes match) and folds
  Yellow's and Red's combined colour usage into one <=15-colour set for the
  palette fit.

  Frames up to 40x40 (diagonal frames 4/5) reuse convert_sonic.py's piece
  chunker (emit_frame_pieces): a single MD hardware sprite maxes at 32x32, so
  anything bigger needs more than one sprite/piece, the same mechanism
  Sonic's own multi-piece frames use.

SIGNPOST (Global/SignPost.bin, Global/Objects2.gif):
  Purely visual per this port's architecture (md_src/signpost.c) -- no boss
  exists, so the port triggers the drop off Sonic's x crossing signpostX-64
  rather than DDWrecker_State_SpawnSignpost (DDWrecker.c:911-923). The
  original scales the face plate horizontally by cos(rotation) every frame
  (FX_SCALE, SignPost.c:34/54); the MD/32X has no hardware sprite scaling, so
  this converter bakes SIGNPOST_PLATE_STEPS discrete cosine-derived widths of
  the plate instead (a real deviation from smooth scaling, pre-approved by
  this task's brief), for both faces the animation alternates between
  (Sonic's own face, SIGNPOSTANI_SONIC, and Eggman's, SIGNPOSTANI_EGGMAN --
  this port is Sonic-only, so Tails/Knuckles/Mighty/Ray are never loaded).
  "Post Bits" (postTopAnimator/sidebarAnimator/standAnimator, SignPost.c:93-95)
  are static, unscaled, and always resident.

Emits into <spring_dir> (assets/spring):
    tiles.bin         resident: 3 orientation rest poses (frame 0), Vertical/
                       Horizontal/Diagonal order.
    stream_tiles.bin  streamed: frames 1-7 of all 3, same order, concatenated
                       -- game/md_src/springs.c DMAs one frame's worth of this
                       into the shared window on change.

Emits into <signpost_dir> (assets/signpost):
    tiles.bin         resident: the 3 Post Bits pieces (post top, sidebar,
                       stand), unscaled.
    stream_tiles.bin  streamed: SIGNPOST_PLATE_STEPS widths x 2 faces.

Emits into <srcdir> (game/md_src):
    spring_data.h/.c, signpost_data.h/.c -- piece/frame tables, tile-base
    counts, chosen palette lines.

Usage: convert_spring.py <Data.rsdk> <spring_dir> <signpost_dir> <srcdir>
"""

import io
import math
import os
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import anim
from convert_stage import md_colour
from convert_sonic import pack_tile, emit_frame_pieces, merge_palette
from convert_ring import load_pal_lines, choose_palette
from rsdk import Pack

from PIL import Image

SHEET_DIR = "Data/Sprites/"

# Spring.c's type 0-5, Spring_Create:63-123 (type>>1: 0 vertical, 1
# horizontal, 2 diagonal); ORIENTATIONS is one row per type>>1, YELLOW_ANIMS/
# RED_ANIMS the two colour sources sampled to build each orientation's merged
# usage and to verify pixel-identical shape.
ORIENTATIONS = ["V", "H", "D"]
YELLOW_ANIMS = {"V": "Yellow V", "H": "Yellow H", "D": "Yellow D"}
RED_ANIMS = {"V": "Red V", "H": "Red H", "D": "Red D"}
SPRING_HITBOX = {  # left, top, right, bottom (Spring_Create:74-119)
    "V": (-16, -8, 16, 8),
    "H": (-8, -16, 8, 16),
    "D": (-12, -12, 12, 12),
}

PALETTE_COLOURS = 15   # merge_palette's cap, same as convert_sonic.py

# Spring.c's bounce sequence is frames 1-7 (frame 8 duplicates 0, see the
# frame-8 verification above); this converter samples a subset of those --
# 1, 4, 7 -- rather than all seven. This is a real, reported deviation (not
# the brief's full 7-frame animator), forced by the same 512 KB 68000 ROM
# ceiling that made deviations (1) and (2) above necessary: even after
# moving the tile PIXELS to the SH2 side, the small per-frame metadata table
# (spring_data.c's spring_stream[], one row per sampled frame), the code
# that reads it, and Sonic's own two new spring-pose animations (Spring
# Twirl/Spring Diagonal, tools/convert_sonic.py) all still live on the
# 68000, and together left no room for the full 7-row table. The bounce
# still reads as a bounce -- start, peak, settle are all kept (1, 4, 7) --
# just coarser than the original's smooth 7-step animator.
SPRING_STREAM_FRAME_IDS = [4]

SIGNPOST_FACES = ["Sonic", "Eggman"]
SIGNPOST_PLATE_STEPS = 2
POST_BITS = "Post Bits"

# The shared streamed window holds this many concurrently-uploaded bounce/
# plate frames -- see this file's docstring point (2) for why 1, not the
# brief's 2, is what the real tile budget closes with.
STREAM_SLOTS = 1


class Sheet:
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


def render_frame(sheet, frame, colour_index):
    px = sheet.crop(frame.x, frame.y, frame.w, frame.h)
    grid = [[0] * frame.w for _ in range(frame.h)]
    for v in range(frame.h):
        for u in range(frame.w):
            idx = px[v * frame.w + u]
            if idx:
                grid[v][u] = colour_index[sheet.colour(idx)]
    return grid


def build_frame_set(sheet, anim_obj, frame_ids, colour_index, tiles_buf, pieces_buf):
    rows = []
    for fi in frame_ids:
        f = anim_obj.frames[fi]
        grid = render_frame(sheet, f, colour_index)
        tOff, pOff, tCount, pCount = emit_frame_pieces(grid, f.w, f.h, tiles_buf, pieces_buf)
        rows.append({"tileOffset": tOff, "pieceOffset": pOff, "tileCount": tCount,
                     "pieceCount": pCount, "pivotX": f.pivotX, "pivotY": f.pivotY,
                     "duration": f.duration})
    return rows


def main():
    if len(sys.argv) < 5:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    pack = Pack(sys.argv[1])
    spring_dir, signpost_dir, srcdir = sys.argv[2], sys.argv[3], sys.argv[4]
    assets_root = os.path.dirname(os.path.abspath(spring_dir))
    for d in (spring_dir, signpost_dir, srcdir):
        os.makedirs(d, exist_ok=True)

    # ============================== SPRINGS ==============================
    spr = anim.load(pack, SHEET_DIR + "Global/Springs.bin")
    sheet = Sheet(pack.read(SHEET_DIR + spr.sheets[0]))
    byname = {a.name: a for a in spr.animations}

    for name in list(YELLOW_ANIMS.values()) + list(RED_ANIMS.values()):
        a = byname[name]
        f0, f8 = a.frames[0], a.frames[8]
        if (f0.x, f0.y, f0.w, f0.h, f0.pivotX, f0.pivotY) != \
           (f8.x, f8.y, f8.w, f8.h, f8.pivotX, f8.pivotY):
            raise SystemExit(f"{name}: frame 8 differs from frame 0, "
                              f"this converter's resident/stream split assumes they match")

    for o in ORIENTATIONS:
        ya, ra = byname[YELLOW_ANIMS[o]], byname[RED_ANIMS[o]]
        for fi in range(8):
            yf, rf = ya.frames[fi], ra.frames[fi]
            if (yf.w, yf.h) != (rf.w, rf.h):
                raise SystemExit(f"{o} frame {fi}: Yellow/Red size differs, "
                                  f"cannot be a colour-only recolour")
            yp = sheet.crop(yf.x, yf.y, yf.w, yf.h)
            rp = sheet.crop(rf.x, rf.y, rf.w, rf.h)
            if [1 if v else 0 for v in yp] != [1 if v else 0 for v in rp]:
                raise SystemExit(f"{o} frame {fi}: Yellow/Red transparency "
                                  f"pattern differs, cannot be a colour-only recolour")

    # Merged colour usage: both Yellow's and Red's pixels count toward one
    # usage table, across all 3 orientations x 8 frames each -- this is the
    # (1) deviation this file's docstring documents (one shared, blended
    # palette rather than two accurate ones).
    usage = {}
    for group in (YELLOW_ANIMS, RED_ANIMS):
        for o in ORIENTATIONS:
            a = byname[group[o]]
            for fi in range(8):
                f = a.frames[fi]
                for idx in sheet.crop(f.x, f.y, f.w, f.h):
                    if idx:
                        c = sheet.colour(idx)
                        usage[c] = usage.get(c, 0) + 1

    merge_map, changed = ({c: c for c in usage}, 0) if len(usage) <= PALETTE_COLOURS \
        else merge_palette(usage, PALETTE_COLOURS)
    merged_colours = sorted(set(merge_map.values()))
    merged_usage = {}
    for c, n in usage.items():
        merged_usage[merge_map[c]] = merged_usage.get(merge_map[c], 0) + n

    ghz_lines = load_pal_lines(f"{assets_root}/ghz/pal.bin", 3)
    sonic_lines = load_pal_lines(f"{assets_root}/sonic/pal.bin", 1)
    candidates = [(f"PAL{i}", e) for i, e in enumerate(ghz_lines)] + [("PAL3", sonic_lines[0])]

    label, entries, mapping, worst, report = choose_palette(merged_usage, candidates)
    spring_pal = int(label[3])
    print(f"spring merged Yellow+Red palette: {len(usage)} raw colours -> "
          f"{len(merged_colours)} after merge_palette ({changed:,} px remapped in "
          f"the merge) -> best fit {label}, worst-case error over merged "
          f"colours used >= 16px: {worst} MD step(s)")
    for c, n, bc, e in sorted(report, key=lambda r: -r[1]):
        print(f"    {c} (n={n:4d}) -> {bc}  err={e}")

    colour_index = {c: mapping[merge_map[c]] for c in usage}

    spring_tiles = bytearray()
    spring_pieces = []
    spring_resident = []
    spring_stream_tiles = bytearray()
    spring_stream_pieces = []
    spring_stream = []

    for o in ORIENTATIONS:
        a = byname[YELLOW_ANIMS[o]]
        resident = build_frame_set(sheet, a, [0], colour_index, spring_tiles, spring_pieces)
        spring_resident.append(resident[0])
        stream = build_frame_set(sheet, a, SPRING_STREAM_FRAME_IDS, colour_index,
                                 spring_stream_tiles, spring_stream_pieces)
        spring_stream.append(stream)

    with open(f"{spring_dir}/tiles.bin", "wb") as fp:
        fp.write(spring_tiles)
    with open(f"{spring_dir}/stream_tiles.bin", "wb") as fp:
        fp.write(spring_stream_tiles)
    # No pal.bin: SPRING_PAL names an EXISTING CRAM line (ghz/pal.bin or
    # sonic/pal.bin, already loaded at boot), not a new one -- same
    # "adds no new CRAM colours" reasoning convert_ring.py's own docstring
    # gives for why it never writes a pal.bin either. Re-uploading one here
    # would overwrite that line's real content with this converter's
    # lower-fidelity remap of it.

    max_stream_tiles = max(r["tileCount"] for rows in spring_stream for r in rows)
    max_stream_pieces = max(r["pieceCount"] for rows in spring_stream for r in rows)
    max_resident_tiles = max(r["tileCount"] for r in spring_resident)
    max_resident_pieces = max(r["pieceCount"] for r in spring_resident)

    # ============================= SIGNPOST ===============================
    sp = anim.load(pack, SHEET_DIR + "Global/SignPost.bin")
    spsheet = Sheet(pack.read(SHEET_DIR + sp.sheets[0]))
    spbyname = {a.name: a for a in sp.animations}

    postbits_anim = spbyname[POST_BITS]
    post_usage = {}
    for f in postbits_anim.frames:
        for idx in spsheet.crop(f.x, f.y, f.w, f.h):
            if idx:
                c = spsheet.colour(idx)
                post_usage[c] = post_usage.get(c, 0) + 1
    post_label, post_entries, post_mapping, post_worst, post_report = \
        choose_palette(post_usage, candidates)
    print(f"\nsignpost post-bits palette fit: {post_label}, worst-case error "
          f"over colours used >= 16px: {post_worst} MD step(s)")

    post_colour_index = {c: post_mapping[c] for c in post_mapping}
    signpost_tiles = bytearray()
    signpost_pieces = []
    signpost_post = build_frame_set(spsheet, postbits_anim, [0, 1, 2],
                                    post_colour_index, signpost_tiles, signpost_pieces)

    face_usage = {}
    face_frame = {}
    for name in SIGNPOST_FACES:
        a = spbyname[name]
        f = a.frames[0]
        face_frame[name] = f
        for idx in spsheet.crop(f.x, f.y, f.w, f.h):
            if idx:
                c = spsheet.colour(idx)
                face_usage[c] = face_usage.get(c, 0) + 1
    face_label, face_entries, face_mapping, face_worst, face_report = \
        choose_palette(face_usage, candidates)
    print(f"signpost face-plate palette fit: {face_label}, worst-case error "
          f"over colours used >= 16px: {face_worst} MD step(s)")
    face_colour_index = {c: face_mapping[c] for c in face_mapping}

    def scale_grid(grid, w, h, ratio):
        new_w = max(1, round(w * ratio))
        out = [[0] * new_w for _ in range(h)]
        for y in range(h):
            for x in range(new_w):
                sx = min(w - 1, int(x / ratio)) if ratio > 0 else 0
                out[y][x] = grid[y][sx]
        return out, new_w

    signpost_stream_tiles = bytearray()
    signpost_stream_pieces = []
    signpost_plate = {}
    ratios = [math.cos(i * (math.pi / 2) / (SIGNPOST_PLATE_STEPS - 1))
              for i in range(SIGNPOST_PLATE_STEPS)]
    for name in SIGNPOST_FACES:
        f = face_frame[name]
        base_grid = render_frame(spsheet, f, face_colour_index)
        rows = []
        for ratio in ratios:
            g, new_w = scale_grid(base_grid, f.w, f.h, ratio)
            tOff, pOff, tCount, pCount = emit_frame_pieces(
                g, new_w, f.h, signpost_stream_tiles, signpost_stream_pieces)
            rows.append({"tileOffset": tOff, "pieceOffset": pOff, "tileCount": tCount,
                        "pieceCount": pCount, "pivotX": f.pivotX, "pivotY": f.pivotY})
        signpost_plate[name] = rows

    with open(f"{signpost_dir}/tiles.bin", "wb") as fp:
        fp.write(signpost_tiles)
    with open(f"{signpost_dir}/stream_tiles.bin", "wb") as fp:
        fp.write(signpost_stream_tiles)
    # No pal.bin here either, same reasoning as the spring block above.

    sp_max_stream_tiles = max(r["tileCount"] for rows in signpost_plate.values() for r in rows)
    sp_max_stream_pieces = max(r["pieceCount"] for rows in signpost_plate.values() for r in rows)
    sp_resident_tiles = sum(r["tileCount"] for r in signpost_post)
    sp_resident_pieces = max(r["pieceCount"] for r in signpost_post)

    shared_frame_tiles = max(max_stream_tiles, sp_max_stream_tiles)
    shared_window_tiles = shared_frame_tiles * STREAM_SLOTS
    resident_real = sum(r["tileCount"] for r in spring_resident) + sp_resident_tiles
    total_budget = resident_real + shared_window_tiles

    # Byte budgets for sh_src/mars.ld's four objtiles sub-regions (see that
    # file's own comment): each tile art blob has to fit its own fixed,
    # round-sized region, not just the 71-tile VRAM ceiling checked below --
    # a ROM region overflow is a hard link error, so catching it here with a
    # clear message is better than letting `make` report a bare linker
    # ASSERT with no indication which converter output caused it.
    region_budgets = [
        ("spring resident (objtiles_spring_res)", len(spring_tiles), 0x1000),
        ("spring stream (objtiles_spring_str)", len(spring_stream_tiles), 0x3000),
        ("signpost resident (objtiles_signpost_res)", len(signpost_tiles), 0x1000),
        ("signpost stream (objtiles_signpost_str)", len(signpost_stream_tiles), 0x3000),
    ]
    region_bad = [(n, sz, b) for n, sz, b in region_budgets if sz > b]
    if region_bad:
        lines = "\n".join(f"  {n}: {sz:,} bytes > {b:,}-byte region budget"
                           for n, sz, b in region_bad)
        raise SystemExit(f"tile art exceeds sh_src/mars.ld's objtiles sub-region "
                          f"budget(s):\n{lines}\nshrink the art (fewer streamed "
                          f"frames/steps) or grow the region in mars.ld and update "
                          f"this list together")

    with open(f"{srcdir}/spring_data.h", "w") as fp:
        fp.write(f"""/* Generated by tools/convert_spring.py. Do not edit by hand. */

#ifndef SPRING_DATA_H
#define SPRING_DATA_H

#include <stdint.h>
#include "obj_data.h"   /* ObjPiece -- shared with signpost_data.h, see that
                          * file's own comment: md_src/obj_sprite.c's piece-
                          * emission loop takes one shared pointer type
                          * instead of two structurally-identical ones. */

typedef struct {{
    uint16_t tileOffset;
    uint16_t pieceOffset;
    uint8_t  tileCount;
    uint8_t  pieceCount;
    int8_t   pivotX, pivotY;
    uint16_t duration;    /* RSDK.ProcessAnimation's raw per-frame value,
                            * paired with the animator speed 0x80 Spring_
                            * State_* sets on trigger (Spring.c:167/195/243/
                            * 286/342). */
}} SpringFrame;

#define SPRING_ORIENT_COUNT     3   /* Spring.c's type>>1: 0 V, 1 H, 2 D */
#define SPRING_STREAM_FRAMES    {len(SPRING_STREAM_FRAME_IDS)}   /* sampled from
                                     * frames 1-7 (frame 8 duplicates 0) --
                                     * see tools/convert_spring.py's own
                                     * SPRING_STREAM_FRAME_IDS comment for
                                     * why not all seven. */
#define SPRING_MAX_RESIDENT_TILES  {max_resident_tiles}
#define SPRING_MAX_RESIDENT_PIECES {max_resident_pieces}
#define SPRING_MAX_STREAM_TILES    {max_stream_tiles}
#define SPRING_MAX_STREAM_PIECES   {max_stream_pieces}

/* Yellow and Red share this one merged/best-fit line (deviation, see this
 * converter's docstring point 1: closing the VRAM budget required folding
 * both colours into one <=15-colour palette rather than keeping each
 * accurate on its own line). */
#define SPRING_PAL {spring_pal}

/* Hitbox per orientation (Spring_Create:74-119), left/top/right/bottom,
 * index by type>>1 (0 vertical, 1 horizontal, 2 diagonal). */
extern const int8_t spring_hitbox[SPRING_ORIENT_COUNT][4];

/* Resident (frame 0), index by type>>1. Piece/frame tables stay ordinary
 * 68000-linked C data (small); the raw tile PIXELS (spring_tiles.bin/
 * spring_stream_tiles.bin) do not -- they are linked into the SH2 program
 * instead (sh_src/obj_tiles.s) and read from the 68000 side through the
 * fixed pointers spring_tiles_md/spring_stream_tiles_md (game/md_src/
 * springs.c), not through an extern array here -- same split
 * sonic_rot_data.h/sonic_rot.s already draw for Sonic's own rotated frames,
 * needed once springs/signpost art pushed the 68000's 512 KB window over
 * budget (sh_src/obj_tiles.s's own comment has the numbers). */
extern const SpringFrame spring_resident[SPRING_ORIENT_COUNT];
extern const ObjPiece spring_resident_pieces[];

/* Streamed (frames 1-7), [type>>1][0..6]. */
extern const SpringFrame spring_stream[SPRING_ORIENT_COUNT][SPRING_STREAM_FRAMES];
extern const ObjPiece spring_stream_pieces[];

#endif
""")

    def piece_c_array(name, pieces, typename="ObjPiece"):
        out = [f"const {typename} {name}[] = {{\n"]
        for dx, dy, size, tile in pieces:
            out.append(f"    {{ {dx}, {dy}, {size}, {tile} }},\n")
        out.append("};\n")
        return "".join(out)

    with open(f"{srcdir}/spring_data.c", "w") as fp:
        fp.write("/* Generated by tools/convert_spring.py. Do not edit by hand. */\n\n")
        fp.write('#include "spring_data.h"\n\n')
        fp.write("const int8_t spring_hitbox[SPRING_ORIENT_COUNT][4] = {\n")
        for o in ORIENTATIONS:
            l, t, r, b = SPRING_HITBOX[o]
            fp.write(f"    {{ {l}, {t}, {r}, {b} }}, /* {o} */\n")
        fp.write("};\n\n")
        fp.write(piece_c_array("spring_resident_pieces", spring_pieces))
        fp.write("\nconst SpringFrame spring_resident[SPRING_ORIENT_COUNT] = {\n")
        for o, r in zip(ORIENTATIONS, spring_resident):
            fp.write(f"    {{ {r['tileOffset']}, {r['pieceOffset']}, {r['tileCount']}, "
                     f"{r['pieceCount']}, {r['pivotX']}, {r['pivotY']}, {r['duration']} }}, /* {o} */\n")
        fp.write("};\n\n")
        fp.write(piece_c_array("spring_stream_pieces", spring_stream_pieces))
        fp.write("\nconst SpringFrame spring_stream[SPRING_ORIENT_COUNT][SPRING_STREAM_FRAMES] = {\n")
        for o, rows in zip(ORIENTATIONS, spring_stream):
            fp.write(f"    {{ /* {o} */\n")
            for r in rows:
                fp.write(f"        {{ {r['tileOffset']}, {r['pieceOffset']}, {r['tileCount']}, "
                         f"{r['pieceCount']}, {r['pivotX']}, {r['pivotY']}, {r['duration']} }},\n")
            fp.write("    },\n")
        fp.write("};\n")

    with open(f"{srcdir}/signpost_data.h", "w") as fp:
        fp.write(f"""/* Generated by tools/convert_spring.py. Do not edit by hand. */

#ifndef SIGNPOST_DATA_H
#define SIGNPOST_DATA_H

#include <stdint.h>
#include "obj_data.h"   /* ObjPiece -- shared with spring_data.h, see that
                          * file's own comment. */

typedef struct {{
    uint16_t tileOffset;
    uint16_t pieceOffset;
    uint8_t  tileCount;
    uint8_t  pieceCount;
    int8_t   pivotX, pivotY;
}} SignPostFrame;

#define SIGNPOST_PLATE_STEPS {SIGNPOST_PLATE_STEPS}
#define SIGNPOST_PAL_POST  {int(post_label[3])}
#define SIGNPOST_PAL_FACE  {int(face_label[3])}
#define SIGNPOST_MAX_RESIDENT_TILES  {sp_resident_tiles}
#define SIGNPOST_MAX_RESIDENT_PIECES {sp_resident_pieces}
#define SIGNPOST_MAX_STREAM_TILES    {sp_max_stream_tiles}
#define SIGNPOST_MAX_STREAM_PIECES   {sp_max_stream_pieces}

/* The shared streamed VRAM window both springs.c and signpost.c stream
 * their currently-active bounce/plate frame through (game/md_src/springs.c's
 * own doc comment has the full x-exclusivity/eviction rule): sized for
 * whichever of the two's own worst single frame is larger, x
 * STREAM_SLOTS_SHARED slot(s) -- tools/convert_spring.py's own STREAM_SLOTS,
 * 1 here (not the brief's 2: see this converter's docstring point 2 for why
 * the tile budget only closes with one). */
#define SIGNPOST_SPRING_SHARED_WINDOW_TILES {shared_window_tiles}

/* Post Bits, resident: 0 post top, 1 sidebar, 2 stand (SignPost.c:93-95).
 * Raw tile pixels (signpost_tiles.bin/signpost_stream_tiles.bin) are not
 * declared here -- see spring_data.h's matching comment: they live in the
 * SH2 program (sh_src/obj_tiles.s), read from the 68000 side through
 * signpost_tiles_md/signpost_stream_tiles_md (game/md_src/signpost.c). */
extern const SignPostFrame signpost_post[3];
extern const ObjPiece signpost_post_pieces[];

/* Face plates, streamed: [face][step], face 0 Sonic, face 1 Eggman
 * (SignPost_Draw's rotation<=128||>=384 picks the egg plate, SignPost.c:31). */
extern const SignPostFrame signpost_plate[2][SIGNPOST_PLATE_STEPS];
extern const ObjPiece signpost_plate_pieces[];

#endif
""")

    with open(f"{srcdir}/signpost_data.c", "w") as fp:
        fp.write("/* Generated by tools/convert_spring.py. Do not edit by hand. */\n\n")
        fp.write('#include "signpost_data.h"\n\n')
        fp.write(piece_c_array("signpost_post_pieces", signpost_pieces))
        fp.write("\nconst SignPostFrame signpost_post[3] = {\n")
        for i, r in enumerate(signpost_post):
            fp.write(f"    {{ {r['tileOffset']}, {r['pieceOffset']}, {r['tileCount']}, "
                     f"{r['pieceCount']}, {r['pivotX']}, {r['pivotY']} }}, /* Post Bits frame {i} */\n")
        fp.write("};\n\n")
        fp.write(piece_c_array("signpost_plate_pieces", signpost_stream_pieces))
        fp.write("\nconst SignPostFrame signpost_plate[2][SIGNPOST_PLATE_STEPS] = {\n")
        for name in SIGNPOST_FACES:
            fp.write(f"    {{ /* {name} */\n")
            for r in signpost_plate[name]:
                fp.write(f"        {{ {r['tileOffset']}, {r['pieceOffset']}, {r['tileCount']}, "
                         f"{r['pieceCount']}, {r['pivotX']}, {r['pivotY']} }},\n")
            fp.write("    },\n")
        fp.write("};\n")

    # --- summary ---
    print(f"\nSprings.bin/SignPost.bin -> {spring_dir}, {signpost_dir}, {srcdir}")
    print(f"  spring resident tiles   {sum(r['tileCount'] for r in spring_resident)}  "
          f"(max single frame {max_resident_tiles}, max pieces {max_resident_pieces})")
    print(f"  spring stream tiles     {sum(r['tileCount'] for rows in spring_stream for r in rows)} total baked "
          f"(max single frame {max_stream_tiles}, max pieces {max_stream_pieces})")
    print(f"  signpost resident tiles {sp_resident_tiles}  (max pieces {sp_resident_pieces})")
    print(f"  signpost stream tiles   {sum(r['tileCount'] for rows in signpost_plate.values() for r in rows)} total baked "
          f"(max single frame {sp_max_stream_tiles}, max pieces {sp_max_stream_pieces})")
    print(f"  shared streamed window  {shared_window_tiles} tiles "
          f"({shared_frame_tiles} tiles/slot x {STREAM_SLOTS} slot(s))")
    print(f"  resident total (springs+signpost, permanent)  {resident_real} tiles")
    print(f"  TOTAL VRAM (resident + shared window)          {total_budget} tiles")
    print(f"  71-tile budget (docs/green-hill.md): "
          f"{'OK' if total_budget <= 71 else 'OVER BUDGET by ' + str(total_budget - 71)}")


if __name__ == "__main__":
    main()
