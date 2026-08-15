#!/usr/bin/env python3
"""RSDKv5 SpriteAnimation (.bin) parsing.

Layout, from RSDKv5 Animation.cpp LoadSpriteAnimation:
    'SPR\\0' <u32 totalFrameCount>               (sum of every anim's frames, unused here)
    <u8 sheetCount> sheetCount x string           (spritesheet paths, relative to Data/Sprites)
    <u8 hitboxCount> hitboxCount x string         (hitbox type names, shared by all frames)
    <u16 animCount>
      per animation: <string name> <u16 frameCount> <i16 speed>
                      <u8 loopIndex> <u8 rotationFlag>
        per frame: <u8 sheetIndex> <u16 duration> <u16 unicodeId>
                   <i16 x> <i16 y> <i16 w> <i16 h> <i16 pivotX> <i16 pivotY>
                   hitboxCount x (i16 left, i16 top, i16 right, i16 bottom)

Usage: anim.py <Data.rsdk> <path/in/pack.bin>
"""

import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import scene as scenelib
from rsdk import Pack


class Reader(scenelib.Reader):
    def i16(self):
        v = struct.unpack_from("<h", self.d, self.p)[0]
        self.p += 2
        return v


class Frame:
    def __init__(self, sheet, x, y, w, h, pivotX, pivotY, duration, id, hitboxes):
        self.sheet = sheet
        self.x = x
        self.y = y
        self.w = w
        self.h = h
        self.pivotX = pivotX
        self.pivotY = pivotY
        self.duration = duration
        self.id = id
        self.hitboxes = hitboxes


class Animation:
    def __init__(self, name, speed, loopIndex, rotationFlag, frames):
        self.name = name
        self.speed = speed
        self.loopIndex = loopIndex
        self.rotationFlag = rotationFlag
        self.frames = frames


class SpriteAnimation:
    def __init__(self, sheets, hitbox_types, animations):
        self.sheets = sheets
        self.hitbox_types = hitbox_types
        self.animations = animations


def load(pack, path):
    data = pack.read(path)
    if data is None or data[:3] != b"SPR":
        raise SystemExit(f"no usable SpriteAnimation at {path}")

    r = Reader(data)
    r.p = 4              # skip 'SPR\0' signature
    r.u32()               # total frame count across all animations, unused here

    sheets = [r.string() for _ in range(r.u8())]
    hitbox_types = [r.string() for _ in range(r.u8())]

    animations = []
    for _ in range(r.u16()):
        name = r.string()
        frameCount = r.u16()
        speed = r.i16()
        loopIndex = r.u8()
        rotationFlag = r.u8()

        frames = []
        for _ in range(frameCount):
            sheet = sheets[r.u8()]
            duration = r.u16()
            id = r.u16()
            x, y, w, h = r.i16(), r.i16(), r.i16(), r.i16()
            pivotX, pivotY = r.i16(), r.i16()
            hitboxes = [(r.i16(), r.i16(), r.i16(), r.i16()) for _ in hitbox_types]
            frames.append(Frame(sheet, x, y, w, h, pivotX, pivotY, duration, id, hitboxes))

        animations.append(Animation(name, speed, loopIndex, rotationFlag, frames))

    return SpriteAnimation(sheets, hitbox_types, animations)


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: anim.py <Data.rsdk> <path/in/pack.bin>")
    pack = Pack(sys.argv[1])
    path = sys.argv[2]

    spr = load(pack, path)

    print(f"{path}")

    print(f"\nsheets ({len(spr.sheets)}):")
    for s in spr.sheets:
        print(f"  {s}")

    print(f"\nhitbox types ({len(spr.hitbox_types)}):")
    for i, h in enumerate(spr.hitbox_types):
        print(f"  {i}: {h}")

    print(f"\nanimations ({len(spr.animations)}):")
    for a in spr.animations:
        print(f"  {a.name:<24} {len(a.frames):>3} frames  speed {a.speed:>4}  "
              f"loop {a.loopIndex:>3}  rot {a.rotationFlag}")
        if a.frames:
            f0 = a.frames[0]
            print(f"      frame0  sheet={f0.sheet}  "
                  f"rect=({f0.x},{f0.y},{f0.w},{f0.h})  pivot=({f0.pivotX},{f0.pivotY})  "
                  f"duration={f0.duration}  id={f0.id}")
            for name, box in zip(spr.hitbox_types, f0.hitboxes):
                print(f"        hitbox {name:<10} left={box[0]:>4} top={box[1]:>4} "
                      f"right={box[2]:>4} bottom={box[3]:>4}")


if __name__ == "__main__":
    main()
