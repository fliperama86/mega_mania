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
      make_disc.py  music -> a Mega CD audio disc, cue and bin
      run.sh        build a ROM and run it in ares, disc included

    game/           the game: a 32X ROM with the Mega CD under it
      md_src/       68000: the VDP, sprite emission, tile upload, the assets
      sh_src/       SH-2: pad, physics, collision, camera, animation
      cd_src/       Mega CD sub-CPU: the program the BIOS dispatches
    blitbench/      32X benchmark, measures the hardware. Not part of the game.
    cdbench/        Mega CD Mode 1 bring-up, reported step by step on screen
      cd/           the sub-CPU program, built separately and incbin'd
    assets/         converted output, gitignored, regenerate as needed
    docs/           findings

## Building

Toolchain is marsdev, installed at `~/mars` (`make m68k-toolchain-newlib` and
`make sh-toolchain-newlib` in the marsdev checkout).

Convert a stage and the character, then build the game:

    python3 tools/convert_stage.py /path/to/Data.rsdk GHZ assets/ghz
    python3 tools/convert_sonic.py /path/to/Data.rsdk assets/sonic game/md_src
    cd game && make

`convert_sonic.py` also writes `game/md_src/sonic_data.{c,h}`, which are
generated and should not be hand edited. `sh_src/sonic_data.h` is a hand-kept
mirror of the same struct layout with none of the data in it, since the SH-2
reads the 68000's one copy through the descriptor table; if the generated
layout changes, that mirror has to be changed to match.

Run it:

    tools/run.sh

That builds first, kills any stale ares (an old instance keeps rendering the ROM
it was launched with, which reads exactly like a fix that did not work), and
picks up a music disc from `assets/disc/` if one has been built. It takes
`cdbench` or `blitbench` as an argument to run those instead, and `ARES` in the
environment to point at another build. The plain form is:

    ares --system "Mega 32X" game/megamania.32x

The 32X benchmark is separate:

    cd blitbench && make
    ares --system "Mega 32X" blitbench/blitbench.32x

So is the Mega CD bring-up. It is launched as a 32X cart, not as a Mega CD 32X
one: ares attaches the CD hardware itself, because the ROM header declares `C`
in its device field, and `--system "Mega CD 32X"` would treat the ROM as a disc
image and boot the BIOS instead.

    cd cdbench && make
    ares --system "Mega 32X" --no-file-prompt cdbench/cdbench.32x

## Notes

Every Makefile here carries an explicit dependency from each `.incbin` object to
the data it includes. Without it the ROM silently ships stale data, which is a trap
marsdev's own examples fall into and which cost hours here.

Sonic's frames are uploaded one DMA per frame into a small VRAM window, so only
the largest frame has to fit rather than the whole sheet. A VRAM DMA that
crosses a 128 KB boundary in ROM wraps inside that block on real hardware, so
`assets.s` starts the tile data on one.

The stage is fitted into three hardware palettes and the fourth is the
character's. Collision boxes are not constants: they come from the current
animation frame every update, the way RSDK does it, so curling into a jump
shrinks the box on its own.
