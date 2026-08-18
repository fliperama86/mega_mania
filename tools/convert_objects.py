#!/usr/bin/env python3
"""Convert Sonic Mania scene entities + sprite art into Mega Drive assets, for
ANY object class -- the single converter tools/convert_rings.py,
tools/convert_springs.py, tools/convert_ring.py and tools/convert_spring.py
collapse into. Those four scripts existed because each new object got its own
pair of bespoke scripts; the scene-walking half of convert_rings.py and
convert_springs.py was a near-verbatim copy (see load_scene_entities()
below), and every art script needed the SAME per-class knowledge (which
animation names, which orientations) just expressed as ad hoc module-level
constants instead of a shared table. With ~21 more objects due, that pattern
does not scale: this script generalizes both halves ONCE and adds new
classes as data (CLASSES below), not as new files.

WHAT STAYS PER-CLASS, AND WHY:
  Scene extraction (load_scene_entities/write_scene_table) is written once
  and reused by every class: parse Scene1.bin's object section (RSDK::
  LoadSceneFile, Scene.cpp:454+), decode each entity's editable vars by
  hash, apply the Mania-mode filter (FILTER_BOTH|FILTER_MANIA, GameVariables
  .h:66-83), sort ascending by x, write a count-prefixed table. Only the
  VAR NAMES to decode and the RECORD PACKER (which decoded fields become
  which bytes) are per-class -- a SceneRecipe.

  Art extraction genuinely needs per-class knowledge: which sprite sheet,
  which named animations become which drawable "layer", which frames of
  each. That knowledge is An ArtRecipe -- the generalized form of convert_
  spring.py's own ORIENTATIONS/YELLOW_ANIMS constants -- consumed by ONE
  shared function, convert_layered_object(), for every class whose art is
  "N named animations, each independently piece-chunked" (Motobug, Spikes,
  ItemBox below, and presumably most of the next ~21).

  Two classes do NOT fit that generic shape and keep dedicated functions,
  same as they always needed dedicated code even under the old scripts:
    - Ring (convert_ring_art): every frame is exactly one fixed 16x16
      hardware sprite with the turn-half's flip BAKED into the tile pixels
      (Ring_Draw_Normal, Ring.c:781) -- no piece-chunking at all, a
      genuinely different hardware mapping from every piece-chunked class.
    - Spring + SignPost (convert_spring_signpost_art): converted together
      because they share ONE physical streamed VRAM window on the 68000
      (no GHZ1 spring and the signpost are ever both near the camera at
      once) -- a cross-class VRAM budget, not a per-class animation table,
      so it is not something a single ArtRecipe entry can express and stays
      its own function, same as it always was.
  Requesting either "Spring" or "SignPost" on the command line converts
  both (see CLASSES below) -- there is no such thing as one without the
  other's budget arithmetic.

THE GENERATED DESCRIPTOR FORMAT: every piece-chunked layer (Spring,
SignPost, Motobug, Spikes, ItemBox) now emits ObjFrame rows, not a
per-class SpringFrame/SignPostFrame -- ObjFrame lives once in game/md_src/
obj_data.h (owned by a parallel task, not this one; do not edit it here).
Ring keeps its own bespoke, non-piece frame shape (RING_ANIM0_TILE_BASE
etc in ring_data.h) since it was never piece data to begin with.

Usage:
    convert_objects.py <Data.rsdk> <stage> <Class> [<Class> ...] \\
        --scene-out <dir> --art-out <dir> --src-out <dir>

  <stage>       stage folder, e.g. GHZ (NOT "GHZ1" -- the Data.rsdk folder
                is "GHZ", only the act filename picks the act: Scene1.bin).
  --scene-out   directory every class's x-sorted scene table is written
                into (all classes share one directory, same as rings.bin/
                springs.bin already sitting side by side in assets/ghz/ --
                also where this script looks for ghz/pal.bin and
                ../sonic/pal.bin, the two existing CRAM lines every class's
                art is fit against).
  --art-out     directory each class's own <ClassNameLower>/ subdirectory
                (tiles.bin, stream_tiles.bin) is written under. Point this
                at the assets root directly (e.g. "assets") to reproduce
                the legacy assets/ring, assets/spring, assets/signpost
                layout exactly (this is what byte-identical verification
                below does); point it at a fresh subtree (e.g. assets/obj)
                to keep new objects' art out of that legacy top level
                without colliding with it -- both are the same rule
                (<art-out>/<class>), just a different root.
  --src-out     game/md_src -- generated <class>_data.h/.c land here.

Example, converting every class this script currently knows about:
    python3 tools/convert_objects.py "$HOME/Library/Application Support/RSDKv5/Data.rsdk" \\
        GHZ Ring Spring SignPost Motobug Spikes ItemBox \\
        --scene-out assets/ghz --art-out assets/obj --src-out game/md_src
"""

import hashlib
import io
import math
import os
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import anim
import scene
from convert_ring import Sheet, load_pal_lines, choose_palette
from convert_sonic import pack_tile, emit_frame_pieces, merge_palette
from rsdk import Pack

SHEET_DIR = "Data/Sprites/"
PALETTE_COLOURS = 15   # merge_palette's cap, same as convert_sonic.py/convert_spring.py

FILTER_BOTH, FILTER_MANIA = 1, 2          # GameVariables.h:66-83
MANIA_MASK = FILTER_BOTH | FILTER_MANIA   # Mania-mode playthrough, sceneInfo.filter == 3


# =============================================================================
# SCENE EXTRACTION -- written once (see module docstring), reused by every
# class through a SceneRecipe.
# =============================================================================

def _md5v(name):
    """Both the RSDK key (byte-reversed per 4-byte group, see rsdk.py's
    Pack.key) and the plain digest map to the same name -- see tools/
    convert_rings.py's own copy of this function for the full reasoning."""
    d = hashlib.md5(name.encode()).digest()
    return {d: name, b"".join(d[i:i + 4][::-1] for i in range(0, 16, 4)): name}


def _i32(r):
    v = r.u32()
    return v - (1 << 32) if v & (1 << 31) else v


def load_scene_entities(pack, stage, class_name, var_names):
    """Generic form of convert_rings.py's load_rings()/convert_springs.py's
    load_springs(): walk Scene1.bin's object section exactly once, decode
    every entity of `class_name` using `var_names` (its Serialize()-
    registered vars, matched by hash so declaration order in var_names does
    not need to match the file's own order) plus the implicit "filter" var
    every non-Zone class carries (Scene.cpp:489-492/541). Returns
    (all_entities, mania_entities), each a list of dicts with
    slot/x_px/y_px/<one key per var_names entry>."""
    classes = _md5v(class_name)
    var_hash = {}
    for n in ("filter",) + tuple(var_names):
        var_hash.update(_md5v(n))

    data = pack.read(f"Data/Stages/{stage}/Scene1.bin")
    if data is None or data[:3] != b"SCN":
        raise SystemExit(f"no usable Scene1.bin in {stage}")

    r = scene.Reader(data)
    r.p = 4 + 0x10
    n = r.u8()
    r.p += n + 1

    # Skip the layer section (same shape scene.py's load() walks) to reach
    # the object section right after it.
    for _ in range(r.u8()):
        r.u8(); r.string(); r.u8(); r.u8(); r.u16(); r.u16(); r.u16(); r.u16()
        for _ in range(r.u16()):
            r.u16(); r.u16(); r.u8(); r.u8()
        r.compressed()
        r.compressed()

    all_e, mania_e = [], []
    for _ in range(r.u8()):
        h = bytes(r.d[r.p:r.p + 16]); r.p += 16
        cls = classes.get(h)
        var_count = r.u8()
        var_types, var_tag = [], []
        for _ in range(var_count - 1):
            vh = bytes(r.d[r.p:r.p + 16]); r.p += 16
            var_types.append(r.u8())
            var_tag.append(var_hash.get(vh, "?"))

        entity_count = r.u16()
        for _ in range(entity_count):
            slot = r.u16()
            x, y = _i32(r), _i32(r)
            vals = []
            for t in var_types:
                if t == 8:                       # VAR_STRING
                    ln = r.u16(); r.p += 2 * ln; vals.append(None)
                elif t == 9:                      # VAR_VECTOR2
                    vals.append((_i32(r), _i32(r)))
                elif t in (0, 3):                 # VAR_UINT8 / VAR_INT8
                    vals.append(r.u8())
                elif t in (1, 4):                 # VAR_UINT16 / VAR_INT16
                    vals.append(r.u16())
                else:                             # VAR_UINT32/INT32/ENUM/BOOL/FLOAT/COLOR
                    vals.append(_i32(r))

            if cls != class_name:
                continue

            rec = dict(zip(var_tag, vals))
            filt = rec.get("filter", 0xFF)         # default per Scene.cpp:541
            entry = {"slot": slot, "x_px": x >> 16, "y_px": y >> 16}  # FROM_FIXED
            for name in var_names:
                entry[name] = rec.get(name, 0)
            all_e.append(entry)
            if MANIA_MASK & filt:
                mania_e.append(entry)

    return all_e, mania_e


class SceneRecipe:
    """The "record packer" the module docstring promises: how one class's
    decoded scene entities become one fixed-width binary row. var_names are
    this class's own Serialize()-registered vars (NOT "filter", added
    automatically). derive() optionally post-processes a decoded entity
    (e.g. Spring's type %= 6, Spring_Create:44) before validate()/packing.
    validate() raises SystemExit if the kept entities violate an assumption
    this port's runtime code bakes in (mirrors convert_rings.py/convert_
    springs.py's own abort checks) -- optional, since a class with no
    runtime consumer yet has nothing to protect. extra_report() prints any
    class-specific breakdown (e.g. Spring's per-orientation counts).

    ROW-STRIDE PARITY GUARD (added after a real bug: SPIKELOG_SCENE/
    CHOPPER_SCENE used to pack 5-byte/7-byte rows while every reader that
    casts this table through a C struct -- SH2 (sh_src/spikelog.c,
    sh_src/badnik.c's ChopperDef) and 68000 alike (md_src/spikelog.c's
    SpikeLogEntry, md_src/chopper.c's ChopperEntry, and md_src/obj_generic.c's
    recordSize-strided entry_x()/entry_y()) -- naturally pads a struct whose
    largest member needs 2-byte alignment up to an EVEN sizeof. An odd
    row_fmt therefore drifts every row after index 0 out from under every
    such reader, on both CPUs, at once. __init__ below asserts row_fmt packs
    to an even size unless a recipe explicitly opts out via
    odd_stride_byte_read=True -- meaning "I have checked: nothing anywhere
    reads this table through a struct cast or a fixed-recordSize stride: every
    reader below decodes it one uint8_t at a time (see e.g. sh_src/
    invisible_block.c's or md_src/decoration.c's own top-of-file comment)".
    Do NOT set that flag to silence this assert without also confirming (and
    keeping true forever after) that no reader ever gains a struct-cast/
    recordSize-strided consumer of this table."""

    def __init__(self, var_names, row_fmt, row_fields,
                 derive=None, validate=None, extra_report=None,
                 odd_stride_byte_read=False):
        row_size = struct.calcsize(row_fmt)
        if row_size % 2 != 0 and not odd_stride_byte_read:
            raise SystemExit(
                f"SceneRecipe row_fmt {row_fmt!r} packs to {row_size} bytes (ODD) -- "
                f"every struct-cast/recordSize-strided reader on both CPUs pads its own "
                f"row struct up to the next EVEN sizeof, which drifts every row after "
                f"index 0 out from under such a reader (see this class's own docstring "
                f"above for the real bug this caught: SPIKELOG_SCENE/CHOPPER_SCENE). "
                f"Either pad row_fmt to an even size (e.g. append 'x'), or -- ONLY if "
                f"every reader of this table is a verified byte-by-byte decode, never a "
                f"struct cast -- pass odd_stride_byte_read=True explicitly.")
        self.var_names = var_names
        self.row_fmt = row_fmt
        self.row_fields = row_fields
        self.derive = derive
        self.validate = validate
        self.extra_report = extra_report
        self.odd_stride_byte_read = odd_stride_byte_read


def write_scene_table(outdir, filename, entities, row_fmt, row_fields, extra_report=None):
    """Generic table writer, shared by every class: sort ascending by x_px
    (game/md_src/*.c hold a sliding window into this order as the camera
    moves), big-endian u16 count then that many fixed-width rows -- the FILE
    shape (count-prefixed, x-sorted) is universal; the ROW shape
    (row_fmt/row_fields) stays per-class, same as rings.bin's 4-byte rows
    next to springs.bin's 6-byte rows today."""
    entities = sorted(entities, key=lambda e: e["x_px"])
    os.makedirs(outdir, exist_ok=True)
    path = f"{outdir}/{filename}"
    with open(path, "wb") as fp:
        fp.write(struct.pack(">H", len(entities)))
        for e in entities:
            fp.write(struct.pack(row_fmt, *(e[f] for f in row_fields)))

    row_size = struct.calcsize(row_fmt)
    print(f"  -> {path}")
    print(f"     {len(entities)} entities kept, {2 + len(entities) * row_size:,} bytes "
          f"({row_size} bytes/row)")
    if entities:
        xs = [e["x_px"] for e in entities]
        print(f"     x range {min(xs)} .. {max(xs)}")
    if extra_report:
        extra_report(entities)
    return entities


def convert_scene(pack, stage, class_name, recipe, outdir, filename):
    print(f"[{class_name}] scene -> {filename}")
    all_e, mania_e = load_scene_entities(pack, stage, class_name, recipe.var_names)
    if recipe.derive:
        mania_e = [recipe.derive(dict(e)) for e in mania_e]
    if recipe.validate:
        recipe.validate(mania_e)
    kept = write_scene_table(outdir, filename, mania_e, recipe.row_fmt, recipe.row_fields,
                              recipe.extra_report)
    dropped = len(all_e) - len(kept)
    print(f"     ({dropped} dropped, other modes only, of {len(all_e)} total in the scene)")
    return kept


# --- Ring's scene recipe (tools/convert_rings.py's load_rings(), unchanged) -

def _validate_ring(entities):
    bad = [e for e in entities
           if e["type"] != 0 or e["planeFilter"] != 0 or e["moveType"] != 0
           or tuple(e["amplitude"]) != (0, 0)]
    if bad:
        lines = "\n".join(
            f"  slot {e['slot']}: type={e['type']} planeFilter={e['planeFilter']} "
            f"moveType={e['moveType']} amplitude={e['amplitude']}" for e in bad[:20])
        raise SystemExit(
            f"{len(bad)} ring(s) are not the static type=0/planeFilter=0/moveType=0 "
            f"case game/md_src/rings.c's touch test assumes -- extend both together "
            f"before converting this stage:\n{lines}")


RING_SCENE = SceneRecipe(
    var_names=("type", "planeFilter", "moveType", "amplitude", "speed", "angle"),
    row_fmt=">hh", row_fields=("x_px", "y_px"),
    validate=_validate_ring,
)

# --- Spring's scene recipe (tools/convert_springs.py's load_springs()) -----

def _derive_spring(e):
    e["type"] = e.get("type", 0) % 6   # Spring_Create:44
    return e


def _validate_spring(entities):
    bad_plane = [e for e in entities if e["planeFilter"] != 0]
    if bad_plane:
        lines = "\n".join(f"  slot {e['slot']}: planeFilter={e['planeFilter']}"
                           for e in bad_plane[:20])
        raise SystemExit(
            f"{len(bad_plane)} spring(s) have planeFilter != 0 -- sh_src/spring.c's "
            f"k_springs table and this converter both assume planeFilter==0 for "
            f"every row, extend both together before converting this stage:\n{lines}")

    bad_ground = [e for e in entities if (e["type"] >> 1) == 1 and e["onGround"]]
    if bad_ground:
        lines = "\n".join(f"  slot {e['slot']}: onGround={e['onGround']}"
                           for e in bad_ground[:20])
        raise SystemExit(
            f"{len(bad_ground)} horizontal-type spring(s) have onGround != 0 -- "
            f"sh_src/spring.c's spring_horizontal() assumes every horizontal row "
            f"has onGround==0; extend spring.c before converting this stage:\n{lines}")


def _report_spring(entities):
    by_orient = {}
    for e in entities:
        by_orient.setdefault(e["type"] >> 1, []).append(e)
    names = {0: "vertical", 1: "horizontal", 2: "diagonal"}
    for k in sorted(by_orient):
        print(f"     {names[k]:<10} {len(by_orient[k])}")


SPRING_SCENE = SceneRecipe(
    var_names=("type", "flipFlag", "onGround", "planeFilter"),
    row_fmt=">hhBB", row_fields=("x_px", "y_px", "type", "flipFlag"),
    derive=_derive_spring, validate=_validate_spring, extra_report=_report_spring,
)

# --- Motobug/Spikes/ItemBox scene recipes: no runtime consumer to protect
# yet, so no validate() -- these exist so the x-sorted table and every
# editable var this port might eventually need are on disk, matching this
# task's brief ("art and tables only", no game code changes). -------------

MOTOBUG_SCENE = SceneRecipe(
    var_names=(),   # Motobug_Serialize is literally empty (Motobug.c:254)
    row_fmt=">hh", row_fields=("x_px", "y_px"),
)

SPIKES_SCENE = SceneRecipe(
    # Spikes_Serialize order, Spikes.c:749-757
    var_names=("type", "moving", "count", "stagger", "timer", "planeFilter"),
    row_fmt=">hhBBBBBB",
    row_fields=("x_px", "y_px", "type", "moving", "count", "stagger", "timer", "planeFilter"),
)

ITEMBOX_SCENE = SceneRecipe(
    # ItemBox_Serialize order, ItemBox.c:1254-1262
    var_names=("type", "isFalling", "hidden", "direction", "planeFilter", "lrzConvPhys"),
    row_fmt=">hhBBBBBB",
    row_fields=("x_px", "y_px", "type", "isFalling", "hidden", "direction", "planeFilter",
                "lrzConvPhys"),
)


# --- Full-roster scene recipes (SpikeLog, Newtron, BuzzBomber, Chopper,
# Crabmeat, Motobug/Spikes/ItemBox above, Batbrain, Platform,
# CollapsingPlatform, Bridge, BreakableWall, Decoration, InvisibleBlock,
# SpinBooster, CorkscrewPath) -- none of these has a runtime consumer yet
# either, so same rule as Motobug/Spikes/ItemBox above: no validate() unless
# an assumption genuinely needs protecting (Platform's type set below is the
# one exception -- see its own comment), every Serialize()-registered var is
# captured, and row widths are chosen per-field from the REAL value domain
# (declared RSDK var type plus what this stage's own Scene1.bin actually
# contains -- probed once with load_scene_entities() directly, not guessed):
#   VAR_UINT8/VAR_BOOL/VAR_ENUM fields whose domain is a small flag or index
#     (every case below except the three noted otherwise) -> "B" (1 byte,
#     unsigned) -- same truncation SPIKES_SCENE/ITEMBOX_SCENE above already
#     make for their own VAR_ENUM "type"/"planeFilter" fields.
#   VAR_INT8 fields (SpikeLog has none; Platform's frameID is the one case)
#     decode through load_scene_entities' r.u8() with NO sign fix (that
#     function's own comment: VAR_UINT8/VAR_INT8 share one decode branch) --
#     a derive() below reinterprets the raw byte as signed before packing
#     with "b", same idea as SPRING_SCENE's own derive() (type %= 6).
#   VAR_VECTOR2 fields (Platform.amplitude/tileOrigin, CollapsingPlatform.
#     size, BreakableWall.size, Decoration.repeatTimes/repeatSpacing) decode
#     as a RAW (already sign-correct, see load_scene_entities' _i32) 32-bit
#     pair -- probing this stage's own data found these genuinely large
#     (Platform.amplitude reaches +/-6,291,456; CollapsingPlatform.size and
#     BreakableWall.size both exceed int16 range), so every one of them is
#     stored RAW as two "i" fields via a derive() that flattens the decoded
#     (x,y) tuple into "<field>_x"/"<field>_y" row keys -- no FROM_FIXED
#     rescale invented here (unlike x_px/y_px, which the file format itself
#     already defines that shift for): this port has no runtime consumer of
#     any of these fields yet to confirm what scale a rescale should even
#     target, so raw-and-lossless is the only defensible choice.
#   Two fields get a wider-than-"domain-looks-small" byte width because the
#     CODE reads a sentinel outside the observed range: CollapsingPlatform.
#     delay is compared against 0xFFFF (Update(), "self->delay < 0xFFFF") --
#     this stage's own data never uses that sentinel (delay is 0 or 30
#     here), but the field is "H" (uint16), not "B", because the CHECK
#     proves 0xFFFF is a legal value even though this stage doesn't emit it.
#     CorkscrewPath's three VAR_ENUM fields are actual magnitudes (period=
#     384 in this stage already exceeds a byte; CorkscrewPath_Create()
#     abs()'s period, implying signed input) -- "h" (int16, signed) each.
# -----------------------------------------------------------------------

SPIKELOG_SCENE = SceneRecipe(
    # SpikeLog_Serialize, SpikeLog.c:145 -- self->frame*4 selects a starting
    # point in the 32-frame "Rotate" animation (SpikeLog_Create:37).
    # row_fmt is ">hhBx" (6 bytes), NOT the logical ">hhB" (5 bytes): a
    # trailing pad byte ("x", struct.pack consumes no field for it) keeps
    # every row EVEN-sized, matching what every struct-cast reader's own
    # sizeof() already rounds up to (see SceneRecipe's own docstring on the
    # row-stride parity guard -- this is the exact bug that guard now
    # catches).
    var_names=("frame",),
    row_fmt=">hhBx", row_fields=("x_px", "y_px", "frame"),
)

NEWTRON_SCENE = SceneRecipe(
    # Newtron_Serialize, Newtron.c:339-343
    var_names=("type", "direction"),
    row_fmt=">hhBB", row_fields=("x_px", "y_px", "type", "direction"),
)

BUZZBOMBER_SCENE = SceneRecipe(
    # BuzzBomber_Serialize, BuzzBomber.c:316-320. shotRange is VAR_UINT8 but
    # this stage's own data only ever uses 48 or 96 -- still a full byte
    # either way, no truncation risk regardless.
    var_names=("direction", "shotRange"),
    row_fmt=">hhBB", row_fields=("x_px", "y_px", "direction", "shotRange"),
)

CHOPPER_SCENE = SceneRecipe(
    # Chopper_Serialize, Chopper.c:325-329
    # row_fmt is ">hhBBBx" (8 bytes), NOT the logical ">hhBBB" (7 bytes): a
    # trailing pad byte ("x", struct.pack consumes no field for it) keeps
    # every row EVEN-sized, matching what every struct-cast reader's own
    # sizeof() already rounds up to (see SceneRecipe's own docstring on the
    # row-stride parity guard -- this is the exact bug that guard now
    # catches).
    var_names=("type", "direction", "charge"),
    row_fmt=">hhBBBx", row_fields=("x_px", "y_px", "type", "direction", "charge"),
)

CRABMEAT_SCENE = SceneRecipe(
    var_names=(),   # Crabmeat_Serialize is literally empty (Crabmeat.c:215)
    row_fmt=">hh", row_fields=("x_px", "y_px"),
)

BATBRAIN_SCENE = SceneRecipe(
    var_names=(),   # Batbrain_Serialize is literally empty (Batbrain.c:221)
    row_fmt=">hh", row_fields=("x_px", "y_px"),
)


def _derive_platform(e):
    # frameID is VAR_INT8 (Platform_Serialize, Platform.c:2765) but
    # load_scene_entities' VAR_UINT8/VAR_INT8 decode branch (r.u8()) never
    # sign-fixes -- this stage's own data confirms the need: probing found
    # raw byte 255 present, which is frameID == -1 ("draw nothing this
    # instance", Platform_Draw: "if (self->frameID >= 0)"), not 255.
    f = e["frameID"]
    e["frameID"] = f - 256 if f >= 128 else f
    e["amplitude_x"], e["amplitude_y"] = e["amplitude"]
    e["tileOrigin_x"], e["tileOrigin_y"] = e["tileOrigin"]
    return e


def _validate_platform(entities):
    # Act1 only places PLATFORM_FIXED/FALL/LINEAR/SWING/PUSH (Platform.h's
    # own enum order: 0/1/2/4/6) -- probed directly against this stage's
    # Scene1.bin (all 60 kept entities are one of these five). Every other
    # PlatformTypes value drives Create()/Draw() paths (PATH, CIRCULAR,
    # TRACK, REACT, DOORSLIDE, CLACKER, CHILD, DIPROCK, ...) this port has
    # never exercised; if a future stage or edit ever introduces one, this
    # converter should stop and say so rather than silently emitting a row
    # nothing downstream is prepared to interpret.
    known = {0, 1, 2, 4, 6}
    bad = [e for e in entities if e["type"] not in known]
    if bad:
        lines = "\n".join(f"  slot at x_px={e['x_px']}: type={e['type']}" for e in bad[:20])
        raise SystemExit(
            f"{len(bad)} Platform entit(y/ies) use a type outside "
            f"{{Fixed,Fall,Linear,Swing,Push}} ({sorted(known)}) -- this port has only "
            f"ever exercised those five; extend PLATFORM_ART/the future runtime "
            f"consumer before converting a scene that uses another one:\n{lines}")


PLATFORM_SCENE = SceneRecipe(
    # Platform_Serialize order, Platform.c:2759-2770
    var_names=("type", "amplitude", "speed", "hasTension", "frameID", "collision",
               "tileOrigin", "childCount", "angle"),
    row_fmt=">hhBiibBbBiiBi",
    row_fields=("x_px", "y_px", "type", "amplitude_x", "amplitude_y", "speed", "hasTension",
                "frameID", "collision", "tileOrigin_x", "tileOrigin_y", "childCount", "angle"),
    derive=_derive_platform, validate=_validate_platform,
)


def _derive_collapsingplatform(e):
    e["size_x"], e["size_y"] = e["size"]
    return e


COLLAPSINGPLATFORM_SCENE = SceneRecipe(
    # CollapsingPlatform_Serialize, CollapsingPlatform.c:365-374
    var_names=("size", "respawn", "targetLayer", "type", "delay", "eventOnly", "mightyOnly"),
    row_fmt=">hhiiBHBHBB",
    row_fields=("x_px", "y_px", "size_x", "size_y", "respawn", "targetLayer", "type", "delay",
                "eventOnly", "mightyOnly"),
    derive=_derive_collapsingplatform,
)

BRIDGE_SCENE = SceneRecipe(
    # Bridge_Serialize, Bridge.c:309-313. `length` is the plank count Bridge_
    # Draw()/Bridge_HandleCollisions() expand at runtime (module docstring's
    # "trivial art, one sprite per plank at draw time" point).
    var_names=("length", "burnable"),
    row_fmt=">hhBB", row_fields=("x_px", "y_px", "length", "burnable"),
)


def _derive_breakablewall(e):
    e["size_x"], e["size_y"] = e["size"]
    return e


BREAKABLEWALL_SCENE = SceneRecipe(
    # BreakableWall_Serialize, BreakableWall.c:828-834. No art recipe below
    # -- see CLASSES' own comment for why (BreakableWall_Draw_Tile redraws
    # the STAGE's own tile, BreakableWall_Draw_Wall/_Floor's TicMark corner
    # markers are DebugMode-only, confirmed against every self->visible
    # assignment in BreakableWall.c).
    var_names=("type", "onlyKnux", "onlyMighty", "priority", "size"),
    row_fmt=">hhBBBBii",
    row_fields=("x_px", "y_px", "type", "onlyKnux", "onlyMighty", "priority", "size_x", "size_y"),
    derive=_derive_breakablewall,
)


def _derive_decoration(e):
    e["repeatTimes_x"], e["repeatTimes_y"] = e["repeatTimes"]
    e["repeatSpacing_x"], e["repeatSpacing_y"] = e["repeatSpacing"]
    return e


DECORATION_SCENE = SceneRecipe(
    # Decoration_Serialize, Decoration.c:149-156. `type` selects which named
    # animation of the per-stage sheet to show (Decoration_Create:62,
    # SetSpriteAnimation(..., self->type, ...)) -- GHZ/Decoration.bin has
    # exactly one ("Bridge Post"), and this stage's own data confirms every
    # kept entity uses type 0.
    var_names=("type", "direction", "rotSpeed", "repeatTimes", "repeatSpacing"),
    # 23 bytes -- ODD. Deliberate: md_src/decoration.c's own reader decodes
    # this table one uint8_t at a time (see that file's own top-of-file
    # comment), never through a struct cast, so no row ever needs to sit at
    # an even byte offset. odd_stride_byte_read=True documents that
    # invariant to SceneRecipe's own parity guard -- if this table ever
    # grows a struct-cast/recordSize-strided reader, that reader (not this
    # recipe) must change first.
    row_fmt=">hhBBbiiii",
    row_fields=("x_px", "y_px", "type", "direction", "rotSpeed", "repeatTimes_x",
                "repeatTimes_y", "repeatSpacing_x", "repeatSpacing_y"),
    derive=_derive_decoration,
    odd_stride_byte_read=True,
)

INVISIBLEBLOCK_SCENE = SceneRecipe(
    # InvisibleBlock_Serialize, Global/InvisibleBlock.c -- no art recipe
    # below: self->visible is only ever DebugMode->debugActive (Update()),
    # false in retail; StageLoad() borrows Global/ItemBox.bin frame 10
    # purely for that debug box, GHZ has no InvisibleBlock.bin of its own
    # (confirmed: anim.load() finds no usable SpriteAnimation there).
    var_names=("width", "height", "planeFilter", "noCrush", "activeNormal", "timeAttackOnly",
               "noChibi"),
    # 11 bytes -- ODD. Deliberate: sh_src/invisible_block.c's own reader
    # decodes this table one uint8_t at a time (see that file's own
    # top-of-file comment, which spells out the exact SH2 misaligned-word
    # crash risk an odd stride read through a struct cast would create), never
    # through a struct cast, so no row ever needs to sit at an even byte
    # offset. odd_stride_byte_read=True documents that invariant to
    # SceneRecipe's own parity guard -- if this table ever grows a
    # struct-cast/recordSize-strided reader, that reader (not this recipe)
    # must change first.
    row_fmt=">hhBBBBBBB",
    row_fields=("x_px", "y_px", "width", "height", "planeFilter", "noCrush", "activeNormal",
                "timeAttackOnly", "noChibi"),
    odd_stride_byte_read=True,
)

SPINBOOSTER_SCENE = SceneRecipe(
    # SpinBooster_Serialize, Common/SpinBooster.c:511-522 -- no art recipe
    # below, same DebugMode-only self->visible pattern as InvisibleBlock
    # (Update(): "self->visible = DebugMode->debugActive"); StageLoad()
    # loads Global/PlaneSwitch.bin only for that debug box, never anything
    # GHZ-specific.
    var_names=("direction", "autoGrip", "bias", "size", "boostPower", "boostAlways",
               "forwardOnly", "playSound", "allowTubeInput"),
    row_fmt=">hhBBBBiBBBB",
    row_fields=("x_px", "y_px", "direction", "autoGrip", "bias", "size", "boostPower",
                "boostAlways", "forwardOnly", "playSound", "allowTubeInput"),
)

CORKSCREWPATH_SCENE = SceneRecipe(
    # CorkscrewPath_Serialize, GHZ/CorkscrewPath.c:115-120 -- no art recipe
    # below: CorkscrewPath_Draw() is a literal empty function body; the only
    # sprite it ever loads is Editor/EditorIcons.bin, and only in
    # GAME_INCLUDE_EDITOR builds (CorkscrewPath_EditorLoad).
    var_names=("period", "amplitude", "angle"),
    row_fmt=">hhhhh", row_fields=("x_px", "y_px", "period", "amplitude", "angle"),
)


# =============================================================================
# ART EXTRACTION, SHARED HELPERS
# =============================================================================

def render_frame(sheet, frame, colour_index):
    """w x h grid of palette indices (0 transparent), a straight sheet crop
    -- no pivot-canvas centring or clipping. That is convert_ring.py's own
    Ring-only "fixed 16x16 canvas" convention, needed there because a ring
    frame must land inside exactly one hardware sprite with no piece table
    at all; every piece-chunked class below instead lets emit_frame_pieces
    size and place its own piece(s), so the raw crop is all any of them
    need (tools/convert_spring.py's own render_frame, now the one copy)."""
    px = sheet.crop(frame.x, frame.y, frame.w, frame.h)
    grid = [[0] * frame.w for _ in range(frame.h)]
    for v in range(frame.h):
        for u in range(frame.w):
            idx = px[v * frame.w + u]
            if idx:
                grid[v][u] = colour_index[sheet.colour(idx)]
    return grid


def build_frame_set(sheet, anim_obj, frame_ids, colour_index, tiles_buf, pieces_buf):
    """One ObjFrame row (dict) per requested frame id of anim_obj, tiles/
    pieces appended into the caller's buffers in place (tools/convert_
    spring.py's own build_frame_set, now the one copy every piece-chunked
    class's art recipe below calls)."""
    rows = []
    for fi in frame_ids:
        f = anim_obj.frames[fi]
        grid = render_frame(sheet, f, colour_index)
        tOff, pOff, tCount, pCount = emit_frame_pieces(grid, f.w, f.h, tiles_buf, pieces_buf)
        rows.append({"tileOffset": tOff, "pieceOffset": pOff, "tileCount": tCount,
                     "pieceCount": pCount, "pivotX": f.pivotX, "pivotY": f.pivotY,
                     "duration": f.duration})
    return rows


def load_palette_candidates(assets_root):
    """The four existing CRAM lines every class's art is fit against -- no
    class here adds a new stage palette, same convention convert_ring.py's
    own docstring gives (PAL0-2 = assets/ghz/pal.bin, PAL3 = assets/sonic/
    pal.bin)."""
    ghz_lines = load_pal_lines(f"{assets_root}/ghz/pal.bin", 3)
    sonic_lines = load_pal_lines(f"{assets_root}/sonic/pal.bin", 1)
    return [(f"PAL{i}", e) for i, e in enumerate(ghz_lines)] + [("PAL3", sonic_lines[0])]


def piece_c_array(name, pieces, typename="ObjPiece"):
    out = [f"const {typename} {name}[] = {{\n"]
    for dx, dy, size, tile in pieces:
        out.append(f"    {{ {dx}, {dy}, {size}, {tile} }},\n")
    out.append("};\n")
    return "".join(out)


def frame_c_row(r):
    """One ObjFrame initializer -- tileOffset,pieceOffset,tileCount,
    pieceCount,pivotX,pivotY,duration (obj_data.h's shape exactly);
    duration defaults to 0 ("0 for static frames", obj_data.h's own
    comment) for classes whose frames carry none."""
    return (f"{{ {r['tileOffset']}, {r['pieceOffset']}, {r['tileCount']}, "
            f"{r['pieceCount']}, {r['pivotX']}, {r['pivotY']}, {r.get('duration', 0)} }}")


# =============================================================================
# RING ART (dedicated -- see module docstring for why: no piece-chunking)
# =============================================================================

RING_ANIM = "Global/Ring.bin"
RING_ANIM_NORMAL = "Normal Ring"
RING_ANIM_SPARKLE1 = "Sparkle 1"
RING_ANIM_SPARKLE3 = "Sparkle 3"
RING_CANVAS = 16
RING_FLIP_FROM_FRAME = 9          # Ring_Draw_Normal: direction = frameID > 8
RING_PROMINENT_PIXELS = 16        # see convert_ring.py's own docstring


def _ring_render_frame(sheet, frame, flip):
    """16x16 grid of raw MD-quantized (r,g,b) colours or None (transparent),
    frame placed by its pivot with the canvas origin at (8,8) so the entity
    origin sits at the canvas centre -- tools/convert_ring.py's own
    render_frame, unchanged. Returns (grid, clipped_pixel_count)."""
    px = sheet.crop(frame.x, frame.y, frame.w, frame.h)
    grid = [[None] * RING_CANVAS for _ in range(RING_CANVAS)]
    clipped = 0
    for v in range(frame.h):
        cy = RING_CANVAS // 2 + frame.pivotY + v
        for u in range(frame.w):
            idx = px[v * frame.w + u]
            if not idx:
                continue
            cx = RING_CANVAS // 2 + frame.pivotX + u
            if not (0 <= cx < RING_CANVAS and 0 <= cy < RING_CANVAS):
                clipped += 1
                continue
            grid[cy][cx] = sheet.colour(idx)
    if flip:
        grid = [[row[RING_CANVAS - 1 - x] for x in range(RING_CANVAS)] for row in grid]
    return grid, clipped


def convert_ring_art(pack, assets_root, art_dir, srcdir):
    os.makedirs(art_dir, exist_ok=True)
    os.makedirs(srcdir, exist_ok=True)

    spr = anim.load(pack, SHEET_DIR + RING_ANIM)
    sheet = Sheet(pack.read(SHEET_DIR + spr.sheets[0]))
    byname = {a.name: a for a in spr.animations}
    a0 = byname[RING_ANIM_NORMAL]
    f0 = a0.frames

    def max_frame_count(a, animation_id):   # Ring_Collect, Ring.c:170-175
        fc = len(a.frames)
        if animation_id == 2:
            fc >>= 1
        return fc - 1

    a2, a4 = byname[RING_ANIM_SPARKLE1], byname[RING_ANIM_SPARKLE3]
    max2, max4 = max_frame_count(a2, 2), max_frame_count(a4, 4)
    f2, f4 = a2.frames[:max2], a4.frames[:max4]

    # DELIBERATE DEVIATION (kept from convert_ring.py, user's call 2026-08-17):
    # Ring_Draw_Normal's frameID>8 flip is not baked here -- see convert_
    # ring.py's own docstring point on this ("the highlight travels around
    # the band" reasoning). fi >= RING_FLIP_FROM_FRAME restores the exact draw.
    jobs = [(f, False, "anim0") for f in f0]
    jobs += [(f, False, "sparkle1") for f in f2]
    jobs += [(f, False, "sparkle3") for f in f4]

    usage, clip_report, rendered = {}, [], []
    for i, (f, flip, section) in enumerate(jobs):
        grid, clipped = _ring_render_frame(sheet, f, flip)
        rendered.append(grid)
        if clipped:
            clip_report.append((section, i, f.w, f.h, clipped))
        for row in grid:
            for c in row:
                if c is not None:
                    usage[c] = usage.get(c, 0) + 1

    candidates = load_palette_candidates(assets_root)
    label, entries, colour_index, worst, report = choose_palette(usage, candidates)
    pal_number = int(label[3])

    if worst > 1:
        lines = "\n".join(f"    {c} (n={n}) -> {bc} err={e}"
                           for c, n, bc, e in sorted(report, key=lambda r: -r[1]))
        raise SystemExit(
            f"no existing CRAM line fits the ring/sparkle colours within 1 MD step "
            f"for every colour used by >= {RING_PROMINENT_PIXELS} pixels; best was "
            f"{label}, worst-case error {worst}:\n{lines}")

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

    with open(f"{art_dir}/tiles.bin", "wb") as fp:
        fp.write(tiles)

    tiles_anim0 = len(f0) * 4
    tiles_sparkle1 = len(f2) * 4
    base_anim0, base_sparkle1 = 0, tiles_anim0
    base_sparkle3 = tiles_anim0 + tiles_sparkle1

    with open(f"{srcdir}/ring_data.h", "w") as fp:
        fp.write(f"""/* Generated by tools/convert_objects.py. Do not edit by hand. */

#ifndef RING_DATA_H
#define RING_DATA_H

#include <stdint.h>

#define RING_ANIM0_FRAMES      {len(f0)}
#define RING_SPARKLE1_MAXFRAME {max2}
#define RING_SPARKLE3_MAXFRAME {max4}

#define RING_ANIM0_TILE_BASE     {base_anim0}
#define RING_SPARKLE1_TILE_BASE  {base_sparkle1}
#define RING_SPARKLE3_TILE_BASE  {base_sparkle3}
#define RING_TILE_COUNT          {base_sparkle3 + len(f4) * 4}

#define RING_PAL {pal_number}

extern const uint16_t ring_sparkle1_durations[RING_SPARKLE1_MAXFRAME];
extern const uint16_t ring_sparkle3_durations[RING_SPARKLE3_MAXFRAME];

#endif
""")

    with open(f"{srcdir}/ring_data.c", "w") as fp:
        fp.write("/* Generated by tools/convert_objects.py. Do not edit by hand. */\n\n")
        fp.write('#include "ring_data.h"\n\n')
        d2 = ", ".join(str(f.duration) for f in f2)
        d4 = ", ".join(str(f.duration) for f in f4)
        fp.write(f"const uint16_t ring_sparkle1_durations[RING_SPARKLE1_MAXFRAME] = {{ {d2} }};\n")
        fp.write(f"const uint16_t ring_sparkle3_durations[RING_SPARKLE3_MAXFRAME] = {{ {d4} }};\n")

    print(f"[Ring] art -> {art_dir}, {srcdir}")
    print(f"     anim 0 Normal Ring    {len(f0)} frames")
    print(f"     anim 2 Sparkle 1      {len(f2)} of {len(a2.frames)} frames (maxFrameCount={max2})")
    print(f"     anim 4 Sparkle 3      {len(f4)} of {len(a4.frames)} frames (maxFrameCount={max4})")
    print(f"     tiles                 {len(tiles)//32}  ({len(tiles):,} bytes)")
    print(f"     palette               {label}, worst-case error: {worst} MD step(s)")
    if clip_report:
        print(f"     WARNING: {len(clip_report)} frame(s) clipped against the 16x16 canvas")

    return {"class": "Ring", "tiles": len(tiles) // 32, "frames": len(f0) + len(f2) + len(f4),
            "palette": label, "palette_worst": worst}


# =============================================================================
# SPRING + SIGNPOST ART (dedicated pair -- see module docstring for why:
# shared streamed VRAM window, a cross-class budget no single ArtRecipe
# entry can express)
# =============================================================================

SPRING_ORIENTATIONS = ["V", "H", "D"]     # Spring.c's type>>1
SPRING_YELLOW_ANIMS = {"V": "Yellow V", "H": "Yellow H", "D": "Yellow D"}
SPRING_RED_ANIMS = {"V": "Red V", "H": "Red H", "D": "Red D"}
SPRING_HITBOX = {  # left, top, right, bottom (Spring_Create:74-119)
    "V": (-16, -8, 16, 8),
    "H": (-8, -16, 8, 16),
    "D": (-12, -12, 12, 12),
}
SPRING_STREAM_FRAME_IDS = [4]   # see convert_spring.py's own docstring for why 1 of 7

SIGNPOST_FACES = ["Sonic", "Eggman"]
SIGNPOST_PLATE_STEPS = 2
POST_BITS = "Post Bits"
STREAM_SLOTS = 1   # see convert_spring.py's own docstring point (2)


def convert_spring_signpost_art(pack, assets_root, spring_dir, signpost_dir, srcdir):
    for d in (spring_dir, signpost_dir, srcdir):
        os.makedirs(d, exist_ok=True)

    # ============================== SPRINGS ================================
    spr = anim.load(pack, SHEET_DIR + "Global/Springs.bin")
    sheet = Sheet(pack.read(SHEET_DIR + spr.sheets[0]))
    byname = {a.name: a for a in spr.animations}

    for name in list(SPRING_YELLOW_ANIMS.values()) + list(SPRING_RED_ANIMS.values()):
        a = byname[name]
        f0, f8 = a.frames[0], a.frames[8]
        if (f0.x, f0.y, f0.w, f0.h, f0.pivotX, f0.pivotY) != \
           (f8.x, f8.y, f8.w, f8.h, f8.pivotX, f8.pivotY):
            raise SystemExit(f"{name}: frame 8 differs from frame 0, the resident/"
                              f"stream split assumes they match")

    for o in SPRING_ORIENTATIONS:
        ya, ra = byname[SPRING_YELLOW_ANIMS[o]], byname[SPRING_RED_ANIMS[o]]
        for fi in range(8):
            yf, rf = ya.frames[fi], ra.frames[fi]
            if (yf.w, yf.h) != (rf.w, rf.h):
                raise SystemExit(f"{o} frame {fi}: Yellow/Red size differs, cannot "
                                  f"be a colour-only recolour")
            yp = sheet.crop(yf.x, yf.y, yf.w, yf.h)
            rp = sheet.crop(rf.x, rf.y, rf.w, rf.h)
            if [1 if v else 0 for v in yp] != [1 if v else 0 for v in rp]:
                raise SystemExit(f"{o} frame {fi}: Yellow/Red transparency pattern "
                                  f"differs, cannot be a colour-only recolour")

    usage = {}
    for group in (SPRING_YELLOW_ANIMS, SPRING_RED_ANIMS):
        for o in SPRING_ORIENTATIONS:
            a = byname[group[o]]
            for fi in range(8):
                f = a.frames[fi]
                for idx in sheet.crop(f.x, f.y, f.w, f.h):
                    if idx:
                        c = sheet.colour(idx)
                        usage[c] = usage.get(c, 0) + 1

    merge_map, changed = ({c: c for c in usage}, 0) if len(usage) <= PALETTE_COLOURS \
        else merge_palette(usage, PALETTE_COLOURS)
    merged_usage = {}
    for c, n in usage.items():
        merged_usage[merge_map[c]] = merged_usage.get(merge_map[c], 0) + n

    candidates = load_palette_candidates(assets_root)
    label, entries, mapping, worst, report = choose_palette(merged_usage, candidates)
    spring_pal = int(label[3])
    print(f"[Spring] merged Yellow+Red palette: {len(usage)} raw colours -> "
          f"{len(set(merge_map.values()))} after merge ({changed:,} px remapped) -> "
          f"best fit {label}, worst-case error: {worst} MD step(s)")

    colour_index = {c: mapping[merge_map[c]] for c in usage}

    spring_tiles, spring_pieces, spring_resident = bytearray(), [], []
    spring_stream_tiles, spring_stream_pieces, spring_stream = bytearray(), [], []

    for o in SPRING_ORIENTATIONS:
        a = byname[SPRING_YELLOW_ANIMS[o]]
        resident = build_frame_set(sheet, a, [0], colour_index, spring_tiles, spring_pieces)
        spring_resident.append(resident[0])
        stream = build_frame_set(sheet, a, SPRING_STREAM_FRAME_IDS, colour_index,
                                 spring_stream_tiles, spring_stream_pieces)
        spring_stream.append(stream)

    with open(f"{spring_dir}/tiles.bin", "wb") as fp:
        fp.write(spring_tiles)
    with open(f"{spring_dir}/stream_tiles.bin", "wb") as fp:
        fp.write(spring_stream_tiles)

    max_stream_tiles = max(r["tileCount"] for rows in spring_stream for r in rows)
    max_stream_pieces = max(r["pieceCount"] for rows in spring_stream for r in rows)
    max_resident_tiles = max(r["tileCount"] for r in spring_resident)
    max_resident_pieces = max(r["pieceCount"] for r in spring_resident)

    # ============================= SIGNPOST =================================
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
    print(f"[SignPost] post-bits palette fit: {post_label}, worst-case error: "
          f"{post_worst} MD step(s)")

    post_colour_index = {c: post_mapping[c] for c in post_mapping}
    signpost_tiles, signpost_pieces = bytearray(), []
    signpost_post = build_frame_set(spsheet, postbits_anim, [0, 1, 2],
                                    post_colour_index, signpost_tiles, signpost_pieces)

    face_usage, face_frame = {}, {}
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
    print(f"[SignPost] face-plate palette fit: {face_label}, worst-case error: "
          f"{face_worst} MD step(s)")
    face_colour_index = {c: face_mapping[c] for c in face_mapping}

    def scale_grid(grid, w, h, ratio):
        new_w = max(1, round(w * ratio))
        out = [[0] * new_w for _ in range(h)]
        for y in range(h):
            for x in range(new_w):
                sx = min(w - 1, int(x / ratio)) if ratio > 0 else 0
                out[y][x] = grid[y][sx]
        return out, new_w

    signpost_stream_tiles, signpost_stream_pieces = bytearray(), []
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
                        "pieceCount": pCount, "pivotX": f.pivotX, "pivotY": f.pivotY,
                        "duration": 0})
        signpost_plate[name] = rows

    with open(f"{signpost_dir}/tiles.bin", "wb") as fp:
        fp.write(signpost_tiles)
    with open(f"{signpost_dir}/stream_tiles.bin", "wb") as fp:
        fp.write(signpost_stream_tiles)

    sp_max_stream_tiles = max(r["tileCount"] for rows in signpost_plate.values() for r in rows)
    sp_max_stream_pieces = max(r["pieceCount"] for rows in signpost_plate.values() for r in rows)
    sp_resident_tiles = sum(r["tileCount"] for r in signpost_post)
    sp_resident_pieces = max(r["pieceCount"] for r in signpost_post)

    shared_frame_tiles = max(max_stream_tiles, sp_max_stream_tiles)
    shared_window_tiles = shared_frame_tiles * STREAM_SLOTS
    resident_real = sum(r["tileCount"] for r in spring_resident) + sp_resident_tiles
    total_budget = resident_real + shared_window_tiles

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
                          f"budget(s):\n{lines}")

    # --- generated source: ObjFrame/ObjPiece from obj_data.h, not the old
    # per-class SpringFrame/SignPostFrame (obj_data.h's own comment) --------
    with open(f"{srcdir}/spring_data.h", "w") as fp:
        fp.write(f"""/* Generated by tools/convert_objects.py. Do not edit by hand. */

#ifndef SPRING_DATA_H
#define SPRING_DATA_H

#include <stdint.h>
#include "obj_data.h"   /* ObjFrame/ObjPiece -- shared with signpost_data.h
                          * and every other piece-chunked object's data
                          * header (obj_data.h's own comment). */

#define SPRING_ORIENT_COUNT     3   /* Spring.c's type>>1: 0 V, 1 H, 2 D */
#define SPRING_STREAM_FRAMES    {len(SPRING_STREAM_FRAME_IDS)}
#define SPRING_MAX_RESIDENT_TILES  {max_resident_tiles}
#define SPRING_MAX_RESIDENT_PIECES {max_resident_pieces}
#define SPRING_MAX_STREAM_TILES    {max_stream_tiles}
#define SPRING_MAX_STREAM_PIECES   {max_stream_pieces}

#define SPRING_PAL {spring_pal}

extern const int8_t spring_hitbox[SPRING_ORIENT_COUNT][4];

extern const ObjFrame spring_resident[SPRING_ORIENT_COUNT];
extern const ObjPiece spring_resident_pieces[];

extern const ObjFrame spring_stream[SPRING_ORIENT_COUNT][SPRING_STREAM_FRAMES];
extern const ObjPiece spring_stream_pieces[];

#endif
""")

    with open(f"{srcdir}/spring_data.c", "w") as fp:
        fp.write("/* Generated by tools/convert_objects.py. Do not edit by hand. */\n\n")
        fp.write('#include "spring_data.h"\n\n')
        fp.write("const int8_t spring_hitbox[SPRING_ORIENT_COUNT][4] = {\n")
        for o in SPRING_ORIENTATIONS:
            l, t, rr, b = SPRING_HITBOX[o]
            fp.write(f"    {{ {l}, {t}, {rr}, {b} }}, /* {o} */\n")
        fp.write("};\n\n")
        fp.write(piece_c_array("spring_resident_pieces", spring_pieces))
        fp.write("\nconst ObjFrame spring_resident[SPRING_ORIENT_COUNT] = {\n")
        for o, r in zip(SPRING_ORIENTATIONS, spring_resident):
            fp.write(f"    {frame_c_row(r)}, /* {o} */\n")
        fp.write("};\n\n")
        fp.write(piece_c_array("spring_stream_pieces", spring_stream_pieces))
        fp.write("\nconst ObjFrame spring_stream[SPRING_ORIENT_COUNT][SPRING_STREAM_FRAMES] = {\n")
        for o, rows in zip(SPRING_ORIENTATIONS, spring_stream):
            fp.write(f"    {{ /* {o} */\n")
            for r in rows:
                fp.write(f"        {frame_c_row(r)},\n")
            fp.write("    },\n")
        fp.write("};\n")

    with open(f"{srcdir}/signpost_data.h", "w") as fp:
        fp.write(f"""/* Generated by tools/convert_objects.py. Do not edit by hand. */

#ifndef SIGNPOST_DATA_H
#define SIGNPOST_DATA_H

#include <stdint.h>
#include "obj_data.h"   /* ObjFrame/ObjPiece -- shared with spring_data.h. */

#define SIGNPOST_PLATE_STEPS {SIGNPOST_PLATE_STEPS}
#define SIGNPOST_PAL_POST  {int(post_label[3])}
#define SIGNPOST_PAL_FACE  {int(face_label[3])}
#define SIGNPOST_MAX_RESIDENT_TILES  {sp_resident_tiles}
#define SIGNPOST_MAX_RESIDENT_PIECES {sp_resident_pieces}
#define SIGNPOST_MAX_STREAM_TILES    {sp_max_stream_tiles}
#define SIGNPOST_MAX_STREAM_PIECES   {sp_max_stream_pieces}

#define SIGNPOST_SPRING_SHARED_WINDOW_TILES {shared_window_tiles}

extern const ObjFrame signpost_post[3];
extern const ObjPiece signpost_post_pieces[];

extern const ObjFrame signpost_plate[2][SIGNPOST_PLATE_STEPS];
extern const ObjPiece signpost_plate_pieces[];

#endif
""")

    with open(f"{srcdir}/signpost_data.c", "w") as fp:
        fp.write("/* Generated by tools/convert_objects.py. Do not edit by hand. */\n\n")
        fp.write('#include "signpost_data.h"\n\n')
        fp.write(piece_c_array("signpost_post_pieces", signpost_pieces))
        fp.write("\nconst ObjFrame signpost_post[3] = {\n")
        for i, r in enumerate(signpost_post):
            fp.write(f"    {frame_c_row(r)}, /* Post Bits frame {i} */\n")
        fp.write("};\n\n")
        fp.write(piece_c_array("signpost_plate_pieces", signpost_stream_pieces))
        fp.write("\nconst ObjFrame signpost_plate[2][SIGNPOST_PLATE_STEPS] = {\n")
        for name in SIGNPOST_FACES:
            fp.write(f"    {{ /* {name} */\n")
            for r in signpost_plate[name]:
                fp.write(f"        {frame_c_row(r)},\n")
            fp.write("    },\n")
        fp.write("};\n")

    print(f"[Spring/SignPost] art -> {spring_dir}, {signpost_dir}, {srcdir}")
    print(f"     spring resident tiles   {sum(r['tileCount'] for r in spring_resident)}")
    print(f"     spring stream tiles     {sum(r['tileCount'] for rows in spring_stream for r in rows)}")
    print(f"     signpost resident tiles {sp_resident_tiles}")
    print(f"     signpost stream tiles   {sum(r['tileCount'] for rows in signpost_plate.values() for r in rows)}")
    print(f"     shared streamed window  {shared_window_tiles} tiles")
    print(f"     TOTAL VRAM (resident + shared window)  {total_budget} tiles "
          f"({'OK' if total_budget <= 71 else 'OVER the 71-tile budget by ' + str(total_budget - 71)})")

    return {"class": "Spring+SignPost", "spring_resident_tiles": sum(r["tileCount"] for r in spring_resident),
            "spring_stream_tiles": sum(r["tileCount"] for rows in spring_stream for r in rows),
            "signpost_resident_tiles": sp_resident_tiles,
            "signpost_stream_tiles": sum(r["tileCount"] for rows in signpost_plate.values() for r in rows),
            "total_vram": total_budget}


# =============================================================================
# GENERIC "N NAMED ANIMATIONS, EACH INDEPENDENTLY PIECE-CHUNKED" ART RECIPE
# -- the shape this task's brief asks every new object to fit, proven below
# against Motobug, Spikes and ItemBox.
# =============================================================================

class ArtRecipe:
    """One class's art conversion recipe -- the generalized form of
    convert_spring.py's own ORIENTATIONS/YELLOW_ANIMS module constants.

    sheet_path: sprite animation path relative to Data/Sprites/, e.g.
        "GHZ/Motobug.bin".
    layers: list of (layer_name, anim_name, frame_ids) tuples. frame_ids is
        None for "every frame in this animation", or an explicit list of
        indices (mirrors convert_ring.py's own max_frame_count/convert_
        spring.py's own SPRING_STREAM_FRAME_IDS pattern of not always
        wanting every frame).
    """

    def __init__(self, sheet_path, layers):
        self.sheet_path = sheet_path
        self.layers = layers


def convert_layered_object(pack, class_name, recipe, assets_root, art_dir, srcdir):
    """The one function every ArtRecipe above is consumed by: load its
    sheet, gather colour usage across every requested frame of every layer,
    best-fit one EXISTING CRAM line (this port adds no new stage palettes --
    same rule Ring/Spring/SignPost already follow), chunk every layer's
    frames into one shared tiles.bin + ObjPiece[] via build_frame_set(), and
    emit <class>_data.h/.c using the shared ObjFrame/ObjPiece types from
    game/md_src/obj_data.h. Returns a report dict for the caller's summary
    printing (main() prints the actual numbers)."""
    os.makedirs(art_dir, exist_ok=True)
    os.makedirs(srcdir, exist_ok=True)

    spr = anim.load(pack, SHEET_DIR + recipe.sheet_path)
    sheet = Sheet(pack.read(SHEET_DIR + spr.sheets[0]))
    byname = {a.name: a for a in spr.animations}

    usage = {}
    resolved = []   # (layer_name, anim_obj, frame_ids)
    skipped = []    # layers with zero convertible pixels (e.g. degenerate 0x0 frames)
    for layer_name, anim_name, frame_ids in recipe.layers:
        a = byname[anim_name]
        ids = list(range(len(a.frames))) if frame_ids is None else frame_ids
        resolved.append((layer_name, a, ids))
        layer_px = 0
        for fi in ids:
            f = a.frames[fi]
            for idx in sheet.crop(f.x, f.y, f.w, f.h):
                if idx:
                    c = sheet.colour(idx)
                    usage[c] = usage.get(c, 0) + 1
                    layer_px += 1
        if layer_px == 0:
            skipped.append(layer_name)

    if not usage:
        raise SystemExit(f"{class_name}: no opaque pixels in any requested layer")

    merge_map, changed = ({c: c for c in usage}, 0) if len(usage) <= PALETTE_COLOURS \
        else merge_palette(usage, PALETTE_COLOURS)
    merged_usage = {}
    for c, n in usage.items():
        merged_usage[merge_map[c]] = merged_usage.get(merge_map[c], 0) + n

    candidates = load_palette_candidates(assets_root)
    label, entries, mapping, worst, report = choose_palette(merged_usage, candidates)
    colour_index = {c: mapping[merge_map[c]] for c in usage}
    pal_number = int(label[3])

    tiles_buf = bytearray()
    pieces_buf = []
    layer_rows = {}
    for layer_name, a, ids in resolved:
        layer_rows[layer_name] = build_frame_set(sheet, a, ids, colour_index, tiles_buf, pieces_buf)

    with open(f"{art_dir}/tiles.bin", "wb") as fp:
        fp.write(tiles_buf)

    all_rows = [r for rows in layer_rows.values() for r in rows]
    total_tiles = sum(r["tileCount"] for r in all_rows)
    max_tiles = max((r["tileCount"] for r in all_rows), default=0)
    max_pieces = max((r["pieceCount"] for r in all_rows), default=0)

    cls_lower = class_name.lower()
    guard = f"{class_name.upper()}_DATA_H"
    with open(f"{srcdir}/{cls_lower}_data.h", "w") as fp:
        fp.write(f"""/* Generated by tools/convert_objects.py. Do not edit by hand. */

#ifndef {guard}
#define {guard}

#include <stdint.h>
#include "obj_data.h"

#define {class_name.upper()}_PAL {pal_number}
#define {class_name.upper()}_MAX_FRAME_TILES  {max_tiles}
#define {class_name.upper()}_MAX_FRAME_PIECES {max_pieces}

""")
        for layer_name, rows in layer_rows.items():
            arr = f"{cls_lower}_{layer_name}"
            fp.write(f"extern const ObjFrame {arr}[{len(rows)}];\n")
        fp.write(f"extern const ObjPiece {cls_lower}_pieces[];\n\n#endif\n")

    with open(f"{srcdir}/{cls_lower}_data.c", "w") as fp:
        fp.write("/* Generated by tools/convert_objects.py. Do not edit by hand. */\n\n")
        fp.write(f'#include "{cls_lower}_data.h"\n\n')
        fp.write(piece_c_array(f"{cls_lower}_pieces", pieces_buf))
        for layer_name, rows in layer_rows.items():
            arr = f"{cls_lower}_{layer_name}"
            fp.write(f"\nconst ObjFrame {arr}[{len(rows)}] = {{\n")
            for r in rows:
                fp.write(f"    {frame_c_row(r)},\n")
            fp.write("};\n")

    return {
        "class": class_name, "sheet": recipe.sheet_path,
        "layers": {name: len(rows) for name, rows in layer_rows.items()},
        "skipped_layers": skipped,
        "total_frames": sum(len(rows) for rows in layer_rows.values()),
        "total_tiles": total_tiles, "tiles_bytes": len(tiles_buf),
        "max_frame_tiles": max_tiles, "max_frame_pieces": max_pieces,
        "raw_colours": len(usage), "merged_colours": len(set(merge_map.values())),
        "palette": label, "palette_worst": worst, "palette_report": report,
    }


def print_layered_report(rep):
    print(f"[{rep['class']}] art -> tiles.bin, {rep['class'].lower()}_data.h/.c")
    print(f"     sheet             {rep['sheet']}")
    for name, n in rep["layers"].items():
        print(f"     layer {name:<14} {n} frame(s)")
    if rep["skipped_layers"]:
        print(f"     WARNING: layer(s) with zero opaque pixels (converted as empty "
              f"frames): {', '.join(rep['skipped_layers'])}")
    print(f"     total frames      {rep['total_frames']}")
    print(f"     total tiles       {rep['total_tiles']}  ({rep['tiles_bytes']:,} bytes)")
    print(f"     max single frame  {rep['max_frame_tiles']} tiles, {rep['max_frame_pieces']} piece(s)")
    print(f"     colours           {rep['raw_colours']} raw -> {rep['merged_colours']} after merge")
    print(f"     palette           {rep['palette']}, worst-case error: {rep['palette_worst']} MD step(s)")
    for c, n, bc, e in sorted(rep["palette_report"], key=lambda r: -r[1])[:8]:
        print(f"       {c} (n={n:5d}) -> {bc}  err={e}")


# --- Motobug art recipe (GHZ/Motobug.bin, GHZ/Objects.gif) -----------------
MOTOBUG_ART = ArtRecipe("GHZ/Motobug.bin", [
    ("move", "Move", None),   # 12 frames, walking
    ("idle", "Idle", None),   # 2 frames, standing at a ledge
    ("turn", "Turn", None),   # 6 frames, about-face at a ledge
    ("puff", "Puff", None),   # 9 frames, squash-smoke poof (Motobug_State_Smoke)
])

# --- Spikes art recipe (Global/Spikes.bin, Global/Objects.gif) -------------
SPIKES_ART = ArtRecipe("Global/Spikes.bin", [
    ("v", "Spikes V", None),   # 1 frame, static
    ("h", "Spikes H", None),   # 1 frame, static
])

# --- ItemBox art recipe (Global/ItemBox.bin, Global/Items.gif) -------------
# 8 of the sheet's 9 animations are converted. "Snow" is the one exclusion,
# and it is NOT excluded for a conversion-time reason -- it converts fine
# mechanically (24 frames, some legitimately 0x0 -- a blank beat in its own
# flicker cycle, same idea as Ring's frame-8-duplicates-frame-0) -- it is
# excluded because ItemBox.c itself never references a snow animator at all
# (grep confirms zero hits for "Snow"/"snow" in that file): dead weight for
# THIS class specifically, whatever it is for elsewhere in the sheet.
# "Debris" (ROTSTYLE_FULL, self->debrisAnimator, ItemBox.c:97/316-393/825-833)
# WAS wrongly excluded in an earlier pass of this recipe on the assumption
# that a rotating animation needs convert_sonic.py's rotate_grid baking the
# way Sonic's own rotated frames do -- verified wrong by actually running it
# (see this task's report): rotationFlag only tells RSDK's DRAW-time
# animator to keep re-rotating the same stored frame each tick (a runtime/
# gameplay concern for whoever eventually wires this up, same class of
# omission as this port's Ring sparkle losing its INK_ADD glow), not
# something the converter needs to precompute. It is included below.
ITEMBOX_ART = ArtRecipe("Global/ItemBox.bin", [
    ("box", "Normal", None),                 # 1 frame, the crate itself
    ("broken", "Broken", None),               # 3 frames, post-break box pieces
    ("contents", "Powerups", None),           # 18 frames, one per item type
    ("scanlines", "Scanlines", None),         # 2 frames, contents outline scroll
    ("disappear", "Item Disappear", None),    # 14 frames, collect sparkle
    ("debris", "Debris", None),               # 10 frames, broken-box chips (ROTSTYLE_FULL at runtime)
    ("change", "Change", None),               # 5 frames, "?" cycling box
    ("bonus", "Bonus", None),                 # 8 frames, bonus-stage variant
])

# --- Full-roster art recipes. Every one of these is the same "N named
# animations, each independently piece-chunked" shape as MOTOBUG_ART/
# SPIKES_ART/ITEMBOX_ART above -- convert_layered_object() needs no changes
# to consume any of them. Every named animation on each sheet is included
# (checked against every SetSpriteAnimation(<Class>->aniFrames, N, ...) call
# in that class's own .c file -- none of these sheets has an ItemBox-style
# "Snow": unreferenced animation to exclude). BreakableWall,
# CollapsingPlatform, InvisibleBlock, SpinBooster and CorkscrewPath have no
# entry here at all -- see their own SceneRecipe comments above for why each
# one draws no sprite art in retail play. -----------------------------------

SPIKELOG_ART = ArtRecipe("GHZ/SpikeLog.bin", [
    ("rotate", "Rotate", None),   # 32 frames, self->frame*4 + timer selects a start point
])

NEWTRON_ART = ArtRecipe("GHZ/Newtron.bin", [
    ("shootidle", "ShootIdle", None),   # 1 frame, NEWTRON_SHOOT idle pose
    ("shoot", "Shoot", None),           # 5 frames, firing animation
    ("flyidle", "FlyIdle", None),       # 1 frame, NEWTRON_FLY idle pose
    ("flyfall", "FlyFall", None),       # 5 frames, dive-bomb descent
    ("fly", "Fly", None),               # 1 frame, level flight
    ("flame", "Flame", None),           # 4 frames, self->flameAnimator overlay
    ("projectile", "Projectile", None), # 6 frames, the fired shot
])

BUZZBOMBER_ART = ArtRecipe("GHZ/BuzzBomber.bin", [
    ("fly", "Fly", None),               # 1 frame, cruising pose
    ("shoot", "Shoot", None),           # 11 frames, firing animation
    ("wings", "Wings", None),           # 4 frames, self->wingAnimator overlay
    ("thrust", "Thrust", None),         # 4 frames, self->thrustAnimator overlay
    ("projectile", "Projectile", None), # 12 frames, the fired shot
])

CHOPPER_ART = ArtRecipe("GHZ/Chopper.bin", [
    ("jump", "Jump", None),     # 8 frames, CHOPPER_JUMP arc
    ("swim", "Swim", None),     # 8 frames, CHOPPER_SWIM stroke
    ("charge", "Charge", None), # 8 frames, pre-jump/pre-swim wind-up
])

CRABMEAT_ART = ArtRecipe("GHZ/Crabmeat.bin", [
    ("stand", "Stand", None),           # 1 frame, idle
    ("walk", "Walk", None),             # 7 frames, patrol
    ("shoot", "Shoot", None),           # 8 frames, firing animation
    ("projectile", "Projectile", None), # 6 frames, the fired pellet
])

BATBRAIN_ART = ArtRecipe("GHZ/Batbrain.bin", [
    ("hang", "Hang", None),   # 1 frame, ceiling-hang idle
    ("fall", "Fall", None),   # 2 frames, drop-off
    ("fly", "Fly", None),     # 8 frames, swoop
])

# Platform's own frameID (Platform_Serialize) indexes across BOTH anims as
# one flat sequence -- Normal's 4 frames then Swing's 3 (Platform_Create:
# 322-333, "while f >= self->animator.frameCount: f -= frameCount, next
# anim"). Swing's 3 frames are not 3 alternate poses: frame 0 is the
# platform itself, frame+1 is a repeated chain link, frame+2 is the
# hub/pivot (Platform_Draw:124-146) -- one instance draws all three
# together, so the whole animation has to convert as a unit, which
# convert_layered_object() already does (every frame of a requested layer
# lands in the one shared tiles.bin/ObjFrame[] for that layer).
PLATFORM_ART = ArtRecipe("GHZ/Platform.bin", [
    ("normal", "Normal", None),   # 4 frames: PLATFORM_FIXED/FALL/LINEAR pose variants
    ("swing", "Swing", None),     # 3 frames: swing platform + chain link + hub
])

BRIDGE_ART = ArtRecipe("GHZ/Bridge.bin", [
    ("log", "Log", None),   # 1 frame, the single plank -- Bridge_Draw() repeats it self->length times
])

DECORATION_ART = ArtRecipe("GHZ/Decoration.bin", [
    ("bridgepost", "Bridge Post", None),   # 1 frame -- GHZ/Decoration.bin's only animation
])


CLASSES = {
    "Ring":     {"scene": RING_SCENE, "scene_file": "rings.bin", "art": "ring"},
    "Spring":   {"scene": SPRING_SCENE, "scene_file": "springs.bin", "art": "spring_signpost"},
    "SignPost": {"scene": None, "scene_file": None, "art": "spring_signpost"},
    "Motobug":  {"scene": MOTOBUG_SCENE, "scene_file": "motobugs.bin", "art": MOTOBUG_ART},
    "Spikes":   {"scene": SPIKES_SCENE, "scene_file": "spikes.bin", "art": SPIKES_ART},
    "ItemBox":  {"scene": ITEMBOX_SCENE, "scene_file": "itemboxes.bin", "art": ITEMBOX_ART},

    # --- Badniks -------------------------------------------------------
    "SpikeLog":   {"scene": SPIKELOG_SCENE, "scene_file": "spikelogs.bin", "art": SPIKELOG_ART},
    "Newtron":    {"scene": NEWTRON_SCENE, "scene_file": "newtrons.bin", "art": NEWTRON_ART},
    "BuzzBomber": {"scene": BUZZBOMBER_SCENE, "scene_file": "buzzbombers.bin", "art": BUZZBOMBER_ART},
    "Chopper":    {"scene": CHOPPER_SCENE, "scene_file": "choppers.bin", "art": CHOPPER_ART},
    "Crabmeat":   {"scene": CRABMEAT_SCENE, "scene_file": "crabmeats.bin", "art": CRABMEAT_ART},
    "Batbrain":   {"scene": BATBRAIN_SCENE, "scene_file": "batbrains.bin", "art": BATBRAIN_ART},

    # --- Traversal -------------------------------------------------------
    "Platform":           {"scene": PLATFORM_SCENE, "scene_file": "platforms.bin", "art": PLATFORM_ART},
    "CollapsingPlatform": {"scene": COLLAPSINGPLATFORM_SCENE, "scene_file": "collapsingplatforms.bin",
                            "art": None},   # redraws the stage's own tiles -- see its SceneRecipe comment
    "Bridge":             {"scene": BRIDGE_SCENE, "scene_file": "bridges.bin", "art": BRIDGE_ART},

    # --- Hazards and items -------------------------------------------------
    "BreakableWall": {"scene": BREAKABLEWALL_SCENE, "scene_file": "breakablewalls.bin",
                       "art": None},   # redraws the stage's own tiles -- see its SceneRecipe comment

    # --- Decorative and logic -----------------------------------------------
    "Decoration":     {"scene": DECORATION_SCENE, "scene_file": "decorations.bin", "art": DECORATION_ART},
    "InvisibleBlock": {"scene": INVISIBLEBLOCK_SCENE, "scene_file": "invisibleblocks.bin",
                        "art": None},   # DebugMode-only visual -- see its SceneRecipe comment
    "SpinBooster":    {"scene": SPINBOOSTER_SCENE, "scene_file": "spinboosters.bin",
                        "art": None},   # DebugMode-only visual -- see its SceneRecipe comment
    "CorkscrewPath":  {"scene": CORKSCREWPATH_SCENE, "scene_file": "corkscrewpaths.bin",
                        "art": None},   # CorkscrewPath_Draw() is empty -- see its SceneRecipe comment
}


def format_manifest_line(symbol, relpath, ctype="uint32_t", align=2):
    """One tools/gen_assets.py ASSETS-list tuple, formatted exactly like that
    list's own entries (see --print-manifest-lines' own comment in main())."""
    return f'    ("{symbol}",{" " * max(1, 22 - len(symbol))}"{relpath}",{" " * max(1, 28 - len(relpath))}"{ctype}", {align}, None),'


# Flags that take no value (present or absent), as opposed to every other
# --flag below which consumes the next argv as its value.
BOOL_FLAGS = {"print-manifest-lines"}


def main():
    argv = sys.argv[1:]
    flags = {}
    positional = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a.startswith("--"):
            name = a[2:]
            if name in BOOL_FLAGS:
                flags[name] = True
                i += 1
            else:
                flags[name] = argv[i + 1]
                i += 2
        else:
            positional.append(a)
            i += 1

    if len(positional) < 3 or not all(k in flags for k in ("scene-out", "art-out", "src-out")):
        raise SystemExit("usage: convert_objects.py <Data.rsdk> <stage> <Class> [<Class> ...] "
                          "--scene-out <dir> --art-out <dir> --src-out <dir> "
                          "[--print-manifest-lines]")

    pack_path, stage, *classes = positional
    scene_out, art_out, src_out = flags["scene-out"], flags["art-out"], flags["src-out"]
    assets_root = os.path.dirname(os.path.abspath(scene_out))

    unknown = [c for c in classes if c not in CLASSES]
    if unknown:
        raise SystemExit(f"no recipe for: {', '.join(unknown)} -- add one to CLASSES in "
                          f"this script (known: {', '.join(sorted(CLASSES))})")

    pack = Pack(pack_path)
    os.makedirs(src_out, exist_ok=True)

    # --print-manifest-lines: tools/gen_assets.py's own ASSETS list is a
    # hand-maintained manifest (that script's own module docstring), and
    # each ArtRecipe-driven class below writes exactly one blob
    # (<art_out>/<class.lower()>/tiles.bin) that manifest needs to know
    # about -- before this flag, that line was hand-typed by inspecting the
    # written file's path (this task's brief: "1-3 hand-added lines per
    # object"). This collects one ready-to-paste tuple per blob actually
    # written THIS run instead, in the exact syntax ASSETS' own entries use,
    # so adding an object's art to bank 1 is copy-paste, never
    # hand-composed. Deliberately scoped to ArtRecipe classes only (not
    # Ring/Spring/SignPost's "art") -- those three already carry their own
    # hand-tuned resident/stream split (spring_tiles vs spring_stream_tiles,
    # etc.) baked into CLASSES' bespoke functions, not the "1-3 uniform
    # lines per object" pain point this flag targets.
    manifest_lines = []

    art_done = set()
    for cls in classes:
        recipe = CLASSES[cls]

        if recipe["scene"] is not None:
            convert_scene(pack, stage, cls, recipe["scene"], scene_out, recipe["scene_file"])
            scene_relpath = os.path.relpath(f"{scene_out}/{recipe['scene_file']}", assets_root)
            scene_symbol = "ghz_" + recipe["scene_file"][:-4]   # strip ".bin", ghz_rings' own convention
            manifest_lines.append(format_manifest_line(scene_symbol, scene_relpath, ctype="uint16_t"))
        else:
            print(f"[{cls}] no scene table (see its CLASSES entry's own note)")

        art = recipe["art"]
        if art == "ring":
            if "ring" not in art_done:
                art_done.add("ring")
                convert_ring_art(pack, assets_root, f"{art_out}/ring", src_out)
        elif art == "spring_signpost":
            if "spring_signpost" not in art_done:
                art_done.add("spring_signpost")
                convert_spring_signpost_art(pack, assets_root, f"{art_out}/spring",
                                            f"{art_out}/signpost", src_out)
        elif isinstance(art, ArtRecipe):
            class_art_dir = f"{art_out}/{cls.lower()}"
            report = convert_layered_object(pack, cls, art, assets_root, class_art_dir, src_out)
            print_layered_report(report)
            relpath = os.path.relpath(f"{class_art_dir}/tiles.bin", assets_root)
            manifest_lines.append(format_manifest_line(f"{cls.lower()}_tiles", relpath))
        elif art is None:
            print(f"[{cls}] no art (see its CLASSES entry's own note)")
        else:
            raise SystemExit(f"{cls}: unrecognised art recipe {art!r}")

    if flags.get("print-manifest-lines") and manifest_lines:
        print("\n# --- paste into tools/gen_assets.py's ASSETS list -------------------------")
        for line in manifest_lines:
            print(line)


if __name__ == "__main__":
    main()
