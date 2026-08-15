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
      anim.py       RSDKv5 SpriteAnimation: frames, pivots, hitboxes
      convert_stage.py   stage -> Mega Drive tiles, palettes, blocks, maps
      convert_sonic.py   Sonic's frames -> hardware sprite pieces and tiles
      tile_usage.py analysis: which tiles actually carry a map
      collision_preview.py  draws the collision masks over the converted art

    ghzview/        32X ROM: a converted stage with Sonic on it
      md_src/       68000: the VDP, sprite emission, tile upload, the assets
      sh_src/       SH-2: pad, physics, collision, camera, animation
    blitbench/      32X benchmark, measures the hardware. Not part of the game.
    assets/         converted output, gitignored, regenerate as needed
    docs/           findings

## Building

Toolchain is marsdev, installed at `~/mars` (`make m68k-toolchain-newlib` and
`make sh-toolchain-newlib` in the marsdev checkout).

Convert a stage and the character, then build the viewer:

    python3 tools/convert_stage.py /path/to/Data.rsdk GHZ assets/ghz
    python3 tools/convert_sonic.py /path/to/Data.rsdk assets/sonic ghzview/md_src
    cd ghzview && make

`convert_sonic.py` also writes `ghzview/md_src/sonic_data.{c,h}`, which are
generated and should not be hand edited. `sh_src/sonic_data.h` is a hand-kept
mirror of the same struct layout with none of the data in it, since the SH-2
reads the 68000's one copy through the descriptor table; if the generated
layout changes, that mirror has to be changed to match.

Run it:

    ares --system "Mega 32X" ghzview/ghzview.32x

The 32X benchmark is separate:

    cd blitbench && make
    ares --system "Mega 32X" blitbench/blitbench.32x

## Notes

Both Makefiles carry an explicit dependency from the `.incbin` object to the
data it includes. Without it the ROM silently ships stale data, which is a trap
marsdev's own examples fall into and which cost hours here.

Sonic's frames are uploaded one DMA per frame into a small VRAM window, so only
the largest frame has to fit rather than the whole sheet. A VRAM DMA that
crosses a 128 KB boundary in ROM wraps inside that block on real hardware, so
`assets.s` starts the tile data on one.

The stage is fitted into three hardware palettes and the fourth is the
character's. Collision boxes are not constants: they come from the current
animation frame every update, the way RSDK does it, so curling into a jump
shrinks the box on its own.
