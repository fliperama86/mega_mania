# Green Hill, end to end

What is left to get one zone actually working, and what each item costs. The
measured hardware facts behind these decisions are in `hardware-budget.md`; this
file is only the state of the work.

"Working" here means: Act 1 traversable from start to finish, looking like Green
Hill, with music, at 60 Hz. Objects are deliberately not in that definition yet.

## Done

- [x] Stage conversion from the RSDK pack: tiles, blocks, palettes, layout,
      per-block collision (`tools/convert_stage.py`)
- [x] **Per-cell flips, correctly.** The scene mirrors 14% of its nonempty
      cells (every loop and S-curve is half mirrored tiles) and the converter
      used to drop those bits, which is what shredded the S-tunnel. Flipped
      cells are now baked as variant blocks carrying RSDK's exact flipped
      collision (Scene.cpp's LoadTileConfig transforms), and the roof-hanging
      yFlip masks are honored instead of read-and-ignored. Collision storage
      is deduplicated (an index plus 163 unique rows for GHZ), which paid for
      the variants and then some: the 68000 program got smaller
- [x] **Camera and player bounds from the scene, not constants.** Zone.c's
      layer-size defaults narrowed per player position by Scene1.bin's 24
      BoundsMarker entities, with Camera_HandleVBounds' eased camera-local
      copy. This is what frames the act start like the original
- [x] Sonic's own sprite on hardware sprites, frames and pivots from the pack
- [x] Player.c's ground and air physics, RSDKv5 tile collision, Camera.c
- [x] Work split across three processors: 68000 owns the VDP, slave SH-2 owns
      pad, physics, collision, camera and the animator, five comm registers
      between them
- [x] **The whole act, not a quarter of it.** FG Low at its real 1024x128
      blocks. Map dimensions travel through the descriptor table, so they cannot
      go stale in one file and not another
- [x] Mega CD Mode 1 bring-up and CD audio music, with the ROM falling through
      to silence when there is no CD. Emulator only, see below
- [x] Audio hardware silenced at boot
- [x] **Background on the 32X.** BG Outside drawn by the master SH2 into the
      framebuffer behind the Mega Drive planes, with its 109 scroll bands
      through the line table: drifting cloud bands, the banded hills and the
      per-line ground-plane gradient, at the original colours (the 32X's own
      CRAM, not the foreground's three palettes). Landed with the "Put the
      real Green Hill on screen" commit; this list was slow to notice
- [x] **FG High, drawn and solid.** The second foreground layer rides Plane B
      at high priority, which stacks it above the player the way Mania draws
      it (Sonic's sprite priority dropped to low to match). Its 256 KB map
      sits at cartridge offset 0x80000: the SH2 links it there directly and
      the 68000 reads it at 0x980000 through the bankable window's reset
      default, bank 0, so no banking code exists. mars.ld's shrunk rom region
      turns any low-image overflow into a link error. Collision now scans
      both layers the way Zone.c registers them, ground and air finders both,
      with each finder carrying exactly the acceptance state its RSDKv5
      original carries. The palette scare evaporated: refitting with FG High
      included left pal.bin byte-identical, 213 of its 240 tiles fit exactly,
      27 remap. 60 VPS in ares with the doubled plane streaming

## Next

1. **The zone's own music.** The disc currently loops a menu track. Per-stage
   music cannot be found by name: the pack addresses files by the MD5 of their
   path and the stage's track is named in a scene object property the converter
   does not parse yet.

## Known gaps, not yet scheduled

- Objects of any kind: rings, springs, badniks, the goal. Out of scope by
  decision, and the thing that separates "a zone that runs" from "a zone you
  play"
- Collision path B and plane switching, which is what makes loops and the
  S-tunnels traversable rather than merely drawn. The layout's bits 14-15
  (path B solidity, 17.8% of nonempty cells) and TileConfig's second path are
  parsed nowhere yet, and the original drives the switch with PlaneSwitch and
  ForceSpin scene objects, so this lands with the object system rather than
  before it
- Act 2. Its FG Low is 1280x96 blocks, wider still than Act 1
- The palette budget held after all: FG High fit into the three stage
  palettes with pal.bin unchanged. Act 2 will re-ask the question
- Music takes about 30 seconds to start. The drive sits not-ready for that long
  before playback begins; the dummy data track being 300 sectors of zeros rather
  than a valid MODE1 track is the first suspect

## Open questions that need hardware

Everything here runs in ares. The 32X side can be checked on the Neptune; the CD
side cannot be checked anywhere, since there is no Mega CD and the MegaSD is
itself a cartridge (`hardware-budget.md`, section 8).

- Does a plain byte copy loop land one byte off on real hardware? GCC and ares
  disagree about `move.b (a0)+,(a0,d1.l)`, and settling it needs five
  instructions on the Neptune and no CD at all
- Does the MegaSD answer at 0x400000 with an adapter in the way? It is the only
  device here that could give the CD path a hardware test
- What the whole act costs per frame on real hardware, which is the number this
  milestone exists to produce
