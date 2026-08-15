#!/usr/bin/env python3
"""RSDKv5 scene (Scene*.bin) parsing.

Layout, from RSDKv5 Scene.cpp LoadScene:
    'SCN\0' <16 bytes metadata> <u8 len><len+1 bytes>
    <u8 layerCount>
      per layer: <u8 visible> <string name> <u8 type> <u8 drawGroup>
                 <u16 xsize> <u16 ysize> <u16 parallaxFactor> <u16 scrollSpeed>
                 <u16 scrollInfoCount> then 6 bytes each:
                   <u16 parallaxFactor> <u16 scrollSpeed> <u8 deform> <u8 unknown>
                 <compressed line scroll, one scrollInfo index per pixel row>
                 <compressed tile layout>

Tile entries are u16: bits 0-9 tile index, 10-11 flip, 12-15 solidity.
"""

import struct
import zlib
from collections import Counter

EMPTY = 0xFFFF
TILE_MASK = 0x3FF


class Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def u16(self):
        v = struct.unpack_from("<H", self.d, self.p)[0]
        self.p += 2
        return v

    def u32(self):
        v = struct.unpack_from("<I", self.d, self.p)[0]
        self.p += 4
        return v

    def string(self):
        n = self.u8()
        s = self.d[self.p:self.p + n]
        self.p += n
        return s.decode("latin1").rstrip("\x00")   # RSDK counts the terminator

    def compressed(self):
        csize = self.u32() - 4
        self.p += 4                                 # uncompressed size, unused
        blob = self.d[self.p:self.p + csize]
        self.p += csize
        return zlib.decompress(blob)


class ScrollInfo:
    """One 6-byte scroll band: its own parallax factor and drift speed."""

    def __init__(self, parallax, speed, deform, unknown):
        self.parallax = parallax
        self.speed = speed
        self.deform = deform
        self.unknown = unknown


class Layer:
    def __init__(self, name, w, h, layout, parallax=0, scroll_speed=0,
                 scroll_info=None, line_scroll=b""):
        self.name = name
        self.w = w
        self.h = h
        self.layout = layout
        self.parallax = parallax          # layer-level default
        self.scroll_speed = scroll_speed  # layer-level default
        self.scroll_info = scroll_info or []      # list of ScrollInfo bands
        self.line_scroll = line_scroll    # one scroll_info index per px row

    def entry(self, x, y):
        i = (y * self.w + x) * 2
        return self.layout[i] | (self.layout[i + 1] << 8)

    def tile(self, x, y):
        e = self.entry(x, y)
        return None if e == EMPTY else e & TILE_MASK

    def usage(self):
        c = Counter()
        for y in range(self.h):
            for x in range(self.w):
                t = self.tile(x, y)
                if t is not None:
                    c[t] += 1
        return c


def load(pack, stage, scene="Scene1.bin"):
    """Returns {layer name: Layer}."""
    data = pack.read(f"Data/Stages/{stage}/{scene}")
    if data is None or data[:3] != b"SCN":
        raise SystemExit(f"no usable {scene} in {stage}")

    r = Reader(data)
    r.p = 4 + 0x10
    n = r.u8()
    r.p += n + 1

    layers = {}
    for _ in range(r.u8()):
        r.u8()
        name = r.string()
        r.u8(); r.u8()
        w, h = r.u16(), r.u16()
        parallax, scroll_speed = r.u16(), r.u16()
        scroll_info = []
        for _ in range(r.u16()):
            p, s = r.u16(), r.u16()
            deform, unknown = r.u8(), r.u8()
            scroll_info.append(ScrollInfo(p, s, deform, unknown))
        line_scroll = r.compressed()
        layout = r.compressed()
        layers[name] = Layer(name, w, h, layout, parallax, scroll_speed,
                              scroll_info, line_scroll)
    return layers
