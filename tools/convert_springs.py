#!/usr/bin/env python3
"""Convert a Mania stage's Spring entities into a Mega Drive spring table.

Parses the stage's Scene1.bin object section the same way convert_rings.py
does (RSDK::LoadSceneFile's object section, Scene.cpp:454+ -- scene.py only
covers tile layers). This is the 68000-side half of the springs feature: the
slave SH2 owns spring PHYSICS from its own hand-transcribed table
(sh_src/spring.c, same scene data, same Mania filter, kept in scene slot
order like sh_src/force_spin.c/plane_switch.c); this converter produces the
independent x-sorted table game/md_src/springs.c needs to DRAW springs and
run its own observational AABB bounce-animation trigger (see that file's own
doc comment for why the two are separate, never-communicating mechanisms).

Spring's own editable vars, in Spring_Serialize order (Spring.c:394-400):
type (VAR_ENUM), flipFlag (VAR_ENUM), onGround (VAR_BOOL), planeFilter
(VAR_UINT8).

This converter only handles the case this port's architecture commits to:
every kept spring must have planeFilter=0 (Spring_State_*'s
`!self->planeFilter` gate always true, Spring.c:141/182/215/258/304) --
verified below, not assumed. onGround is NOT required to be zero for every
row (see sh_src/spring.c's own comment: it is dead data on every vertical/
diagonal row and this port's own scene data happens to have it zero on
every horizontal-type row, where it is the only place the field is ever
read) -- this converter instead asserts the narrower, load-bearing fact
directly: every horizontal-type (type>>1==1) kept entry has onGround=0.

Emits into <outdir>:
    springs.bin  big-endian uint16 count, then count entries of
                 int16 x, int16 y, uint8 type, uint8 flipFlag (Spring.c's own
                 field values, Spring_Create:44-124), pixel-space centre,
                 sorted ascending by x so game/md_src/springs.c can hold a
                 sliding window into this table as the camera moves, same
                 convention as tools/convert_rings.py's rings.bin.

Usage: convert_springs.py <Data.rsdk> <stage> <outdir>
"""

import hashlib
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import scene
from rsdk import Pack

SPRING_CLASS = "Spring"
VAR_NAMES = ("filter", "type", "flipFlag", "onGround", "planeFilter")

FILTER_BOTH, FILTER_MANIA = 1, 2
MANIA_MASK = FILTER_BOTH | FILTER_MANIA


def _md5v(name):
    d = hashlib.md5(name.encode()).digest()
    return {d: name, b"".join(d[i:i + 4][::-1] for i in range(0, 16, 4)): name}


def i32(r):
    v = r.u32()
    return v - (1 << 32) if v & (1 << 31) else v


def load_springs(pack, stage):
    classes = _md5v(SPRING_CLASS)
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

    for _ in range(r.u8()):
        r.u8(); r.string(); r.u8(); r.u8(); r.u16(); r.u16(); r.u16(); r.u16()
        for _ in range(r.u16()):
            r.u16(); r.u16(); r.u8(); r.u8()
        r.compressed()
        r.compressed()

    all_springs, mania_springs = [], []
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

            if cls != SPRING_CLASS:
                continue

            rec = dict(zip(var_tag, vals))
            filt = rec.get("filter", 0xFF)
            entry = {
                "slot": slot,
                "x_px": x >> 16, "y_px": y >> 16,
                "type": rec.get("type", 0) % 6,       # Spring_Create:44
                "flipFlag": rec.get("flipFlag", 0),
                "onGround": rec.get("onGround", 0),
                "planeFilter": rec.get("planeFilter", 0),
            }
            all_springs.append(entry)
            if MANIA_MASK & filt:
                mania_springs.append(entry)

    return all_springs, mania_springs


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    pack = Pack(sys.argv[1])
    stage = sys.argv[2]
    outdir = sys.argv[3]

    all_springs, springs = load_springs(pack, stage)

    bad_plane = [e for e in springs if e["planeFilter"] != 0]
    if bad_plane:
        lines = "\n".join(f"  slot {e['slot']}: planeFilter={e['planeFilter']}"
                           for e in bad_plane[:20])
        raise SystemExit(
            f"{len(bad_plane)} spring(s) in {stage} have planeFilter != 0 -- "
            f"sh_src/spring.c's k_springs table and this converter both "
            f"assume planeFilter==0 for every row, extend both together "
            f"before converting this stage:\n{lines}")

    bad_ground = [e for e in springs if (e["type"] >> 1) == 1 and e["onGround"]]
    if bad_ground:
        lines = "\n".join(f"  slot {e['slot']}: onGround={e['onGround']}"
                           for e in bad_ground[:20])
        raise SystemExit(
            f"{len(bad_ground)} horizontal-type spring(s) in {stage} have "
            f"onGround != 0 -- sh_src/spring.c's spring_horizontal() assumes "
            f"\"!self->onGround || player->onGround\" is unconditionally true "
            f"(every horizontal row has onGround==0), which this data no "
            f"longer satisfies; extend spring.c before converting this "
            f"stage:\n{lines}")

    springs.sort(key=lambda e: e["x_px"])

    import os
    os.makedirs(outdir, exist_ok=True)
    with open(f"{outdir}/springs.bin", "wb") as fp:
        fp.write(struct.pack(">H", len(springs)))
        for e in springs:
            fp.write(struct.pack(">hhBB", e["x_px"], e["y_px"], e["type"], e["flipFlag"]))

    dropped = len(all_springs) - len(springs)
    xs = [e["x_px"] for e in springs]
    by_orient = {}
    for e in springs:
        by_orient.setdefault(e["type"] >> 1, []).append(e)

    print(f"{stage} Scene1.bin -> {outdir}/springs.bin")
    print(f"  springs kept (Mania filter)  {len(springs)}  ({dropped} dropped, other modes only)")
    print(f"  x range                      {min(xs)} .. {max(xs)}")
    print(f"  springs.bin                  {2 + len(springs) * 6:,} bytes")
    names = {0: "vertical", 1: "horizontal", 2: "diagonal"}
    for k in sorted(by_orient):
        print(f"  {names[k]:<10}                  {len(by_orient[k])}")


if __name__ == "__main__":
    main()
