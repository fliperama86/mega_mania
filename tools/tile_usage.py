#!/usr/bin/env python3
"""How often each tile is actually placed in a stage layout.

Answers the question that decides the art budget: how much of the tileset is
carrying the map, and how much is a long tail of one-off decoration that can be
cut cheaply.

Usage: tile_usage.py <Data.rsdk> <stage> [scene]
"""

import sys
from collections import Counter

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import scene as scenelib
from rsdk import Pack


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: tile_usage.py <Data.rsdk> <stage> [scene]")
    pack = Pack(sys.argv[1])
    stage = sys.argv[2]
    name = sys.argv[3] if len(sys.argv) > 3 else "Scene1.bin"

    layers = scenelib.load(pack, stage, name)
    total = Counter()

    print(f"stage {stage} / {name}")
    for layer in layers.values():
        counts = layer.usage()
        total.update(counts)
        print(f"  layer {layer.name:<12} {layer.w:>4} x {layer.h:<4} "
              f"{sum(counts.values()):>7,} placed  {len(counts):>4} distinct")

    placed = sum(total.values())
    ranked = total.most_common()
    print(f"\ntotal: {placed:,} placements over {len(ranked)} distinct tiles")
    print("\ncoverage by tile count, most used first:")
    acc, mi = 0, 0
    marks = [0.50, 0.75, 0.90, 0.95, 0.99]
    for i, (_, c) in enumerate(ranked, 1):
        acc += c
        while mi < len(marks) and acc >= placed * marks[mi]:
            print(f"  {marks[mi]*100:>5.0f}% of the map drawn by {i:>4} tiles "
                  f"({i * 100.0 / len(ranked):.0f}% of the set)")
            mi += 1

    print(f"\n  used once:             {sum(1 for _, c in ranked if c == 1)}")
    print(f"  used 4 times or fewer: {sum(1 for _, c in ranked if c <= 4)}")


if __name__ == "__main__":
    main()
