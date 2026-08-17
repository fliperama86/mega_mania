#!/usr/bin/env python3
"""Convert a Mania stage's Ring entities into a Mega Drive ring table.

Parses the stage's Scene1.bin object section directly (RSDK::LoadSceneFile,
Scene.cpp:454+) the way scene.py's Layer/tile parsing does for tile layers,
since scene.py itself only covers tile layers, not the object section --
this is the same approach the scratchpad recon script (dump_rings.py) used
to produce the 445-ring GHZ1 count this converter now reproduces formally.

Every non-Zone class implicitly gets a "filter" VAR_UINT8 registered before
its own Serialize() runs (RSDKv5 Scene.cpp:489-492); a scene entry with no
explicit "filter" var defaults to entity->filter = 0xFF, i.e. active in every
mode (Scene.cpp:541). At load time RSDK keeps an entity only if
`sceneInfo.filter & entity->filter` (Scene.cpp:679). Mania-mode filter bits
(SonicMania/GameVariables.h:66-83): FILTER_BOTH = 1<<0, FILTER_MANIA = 1<<1,
so a Mania-mode playthrough uses sceneInfo.filter == 3 -- "Mania filter = keep
entity iff (filter & 3) != 0".

Ring's own editable vars, in Ring_Serialize order (Ring.c:920-928): type
(VAR_ENUM), planeFilter (VAR_ENUM), moveType (VAR_ENUM), amplitude
(VAR_VECTOR2), speed (VAR_ENUM), angle (VAR_INT32).

This converter only handles the static case the port's architecture commits
to: every kept ring must have type=RING_TYPE_NORMAL (0), planeFilter=0 (no
plane restriction, Ring_Collect's `!self->planeFilter` check always true,
Ring.c:138) and moveType=RING_MOVE_FIXED (0, Ring_Create's default case,
Ring.c:82-88 -- no oscillation, self->position is the whole story). GHZ1
happens to satisfy this for all 445 Mania-mode rings; a stage that doesn't
fails loudly here rather than silently dropping a ring's motion.

Emits into <outdir>:
    rings.bin   big-endian uint16 count, then count entries of int16 x,
                int16 y: the ring's pixel-space centre (scene position >> 16,
                RSDK's 16.16 fixed point -- same FROM_FIXED shift
                CheckObjectCollisionTouch itself applies, Collision.cpp:234-
                237), sorted ascending by x so game/md_src/rings.c can hold a
                sliding window into this table as the camera moves instead of
                scanning all of it every frame.

Usage: convert_rings.py <Data.rsdk> <stage> <outdir>
"""

import hashlib
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import scene
from rsdk import Pack

RING_CLASS = "Ring"
VAR_NAMES = ("filter", "type", "planeFilter", "moveType", "amplitude", "speed", "angle")

FILTER_BOTH, FILTER_MANIA = 1, 2          # GameVariables.h:66-83
MANIA_MASK = FILTER_BOTH | FILTER_MANIA   # == 3, this converter's target mode


def _md5v(name):
    """Both the RSDK key (byte-reversed per 4-byte group, see rsdk.py's Pack.key)
    and the plain digest map to the same name: the object-table class hash
    (Pack.key's own convention) and the per-scene var-table hash (read
    straight off the file, un-reversed) are computed differently upstream,
    so accept either without needing to know which is which here."""
    d = hashlib.md5(name.encode()).digest()
    return {d: name, b"".join(d[i:i + 4][::-1] for i in range(0, 16, 4)): name}


def i32(r):
    v = r.u32()
    return v - (1 << 32) if v & (1 << 31) else v


def load_rings(pack, stage):
    classes = _md5v(RING_CLASS)
    var_names = {}
    for n in VAR_NAMES:
        var_names.update(_md5v(n))

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

    all_rings = []
    mania_rings = []
    for _ in range(r.u8()):
        h = bytes(r.d[r.p:r.p + 16]); r.p += 16
        cls = classes.get(h)
        var_count = r.u8()
        var_types, var_tag = [], []
        for _ in range(var_count - 1):
            vh = bytes(r.d[r.p:r.p + 16]); r.p += 16
            var_types.append(r.u8())
            var_tag.append(var_names.get(vh, "?"))

        entity_count = r.u16()
        for _ in range(entity_count):
            slot = r.u16()
            x, y = i32(r), i32(r)
            vals = []
            for t in var_types:
                if t == 8:                       # VAR_STRING
                    ln = r.u16(); r.p += 2 * ln; vals.append(None)
                elif t == 9:                      # VAR_VECTOR2
                    vals.append((i32(r), i32(r)))
                elif t in (0, 3):                 # VAR_BOOL / VAR_UINT8
                    vals.append(r.u8())
                elif t in (1, 4):                 # VAR_UINT16 / VAR_ENUM
                    vals.append(r.u16())
                else:                             # VAR_INT32 and friends
                    vals.append(i32(r))

            if cls != RING_CLASS:
                continue

            rec = dict(zip(var_tag, vals))
            filt = rec.get("filter", 0xFF)        # default per Scene.cpp:541
            entry = {
                "slot": slot,
                "x_px": x >> 16, "y_px": y >> 16,  # FROM_FIXED, Collision.cpp:234-237
                "type": rec.get("type", 0),
                "planeFilter": rec.get("planeFilter", 0),
                "moveType": rec.get("moveType", 0),
                "amplitude": rec.get("amplitude", (0, 0)),
            }
            all_rings.append(entry)
            if MANIA_MASK & filt:
                mania_rings.append(entry)

    return all_rings, mania_rings


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    pack = Pack(sys.argv[1])
    stage = sys.argv[2]
    outdir = sys.argv[3]

    all_rings, rings = load_rings(pack, stage)

    bad = [e for e in rings
           if e["type"] != 0 or e["planeFilter"] != 0 or e["moveType"] != 0
           or tuple(e["amplitude"]) != (0, 0)]
    if bad:
        lines = "\n".join(
            f"  slot {e['slot']}: type={e['type']} planeFilter={e['planeFilter']} "
            f"moveType={e['moveType']} amplitude={e['amplitude']}" for e in bad[:20])
        raise SystemExit(
            f"{len(bad)} ring(s) in {stage} are not the static "
            f"type=0/planeFilter=0/moveType=0 case this converter and "
            f"game/md_src/rings.c's touch test assume -- extend both together "
            f"before converting this stage:\n{lines}")

    rings.sort(key=lambda e: e["x_px"])

    import os
    os.makedirs(outdir, exist_ok=True)
    with open(f"{outdir}/rings.bin", "wb") as fp:
        fp.write(struct.pack(">H", len(rings)))
        for e in rings:
            fp.write(struct.pack(">hh", e["x_px"], e["y_px"]))

    dropped = len(all_rings) - len(rings)
    xs = [e["x_px"] for e in rings]
    ys = [e["y_px"] for e in rings]
    print(f"{stage} Scene1.bin -> {outdir}/rings.bin")
    print(f"  rings kept (Mania filter)  {len(rings)}  ({dropped} dropped, other modes only)")
    print(f"  x range                    {min(xs)} .. {max(xs)}")
    print(f"  y range                    {min(ys)} .. {max(ys)}")
    print(f"  rings.bin                  {2 + len(rings) * 4:,} bytes")


if __name__ == "__main__":
    main()
