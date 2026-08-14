# mega_mania

A Sonic game for the Sega 32X with Mega CD, built with Sonic Mania's DNA. The
RSDKv5 decompilation is used as reference and as an asset source, not as a
dependency: the engine here is new, sized for the hardware.

See `docs/hardware-budget.md` for what the hardware measured out at and the
architecture that follows from it.

## Layout

    tools/          asset pipeline, host side
      rsdk.py       RSDKv5 data pack: file table, name hashing, decryption
      scene.py      Scene*.bin parsing, tile layers
      convert_stage.py   stage -> Mega Drive tiles, palettes, blocks, maps
      tile_usage.py analysis: which tiles actually carry a map

    ghzview/        Mega Drive ROM that renders a converted stage
    blitbench/      32X benchmark, measures the hardware. Not part of the game.
    assets/         converted output, gitignored, regenerate as needed
    docs/           findings

## Building

Toolchain is marsdev, installed at `~/mars` (`make m68k-toolchain-newlib` and
`make sh-toolchain-newlib` in the marsdev checkout).

Convert a stage, then build the viewer:

    python3 tools/convert_stage.py /path/to/Data.rsdk GHZ assets/ghz
    cd ghzview && make

Run it:

    ares --system "Mega Drive" ghzview/out.bin

The 32X benchmark is separate:

    cd blitbench && make
    ares --system "Mega 32X" blitbench/blitbench.32x

## Notes

Both Makefiles carry an explicit dependency from the `.incbin` object to the
data it includes. Without it the ROM silently ships stale data, which is a trap
marsdev's own examples fall into and which cost hours here.
