# Green Hill, end to end

What is left to get one zone actually working, and what each item costs. The
measured hardware facts behind these decisions are in `hardware-budget.md`; this
file is only the state of the work.

"Working" here means: Act 1 traversable from start to finish, looking like Green
Hill, with music, at 60 Hz. Objects are deliberately not in that definition yet.

## Done

- [x] Stage conversion from the RSDK pack: tiles, blocks, palettes, layout,
      per-block collision (`tools/convert_stage.py`)
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

## Next

1. **Background on the 32X.** BG Outside is 512x24 blocks and the layer behind
   the Mega Drive is still a placeholder gradient. The 32X is 8bpp with a
   256 entry CRAM, so this one gets the original colours rather than the
   foreground's three 15-colour palettes. Parallax comes from the framebuffer's
   line table: one word offset per scanline, so independent bands cost 224 word
   writes a frame and drift without the camera moving. The constraint is
   storage, not time: one 128 KB bank holds the table and the image, which is
   about 570 bytes per line at full screen height, and every line needs its own
   width plus a screen of slack before it has to rebase.
   **Measured, so the design does not have to guess.** The layer is 512x24
   blocks, 8192x384 px, and it carries 109 scroll bands indexed once per pixel
   row: lines 0-63 are three cloud bands at parallax factor 48 with their own
   camera-independent scroll speeds, which is what makes the sky drift while
   standing still; 64-111 at 96; 112-151 at 128; 152-254 a one-band-per-line
   gradient from 127 to 229, which is the ground plane running away from you;
   255-383 at 230. Every deform flag is 0, so there is no wobble table.

   The art is small and the arrangement is not: each row uses between 2 and 26
   distinct blocks, rows 17-23 are one block repeated, but the 512 column
   layout never repeats. So the background is kept as blocks and drawn, not
   stored as a picture. A buffer somewhat wider than the screen sits in one
   framebuffer bank, each scanline gets its own offset word, and a 16 px column
   is drawn ahead of the edge as a line's phase advances. Worst case, every
   line wanting a column in the same frame, that is 224x16 bytes against the
   71 KB a full redraw would cost. The flat bottom rows let many scanlines
   share one stored line and buy back space.

   The shimmer is a rotation of four palette entries roughly every six frames,
   which is a couple of CRAM writes. GHZ's animated tiles are foreground only,
   and the water line is an object rather than part of the layer, so neither is
   in scope here.

2. **FG High.** The second foreground layer, also 1024x128 blocks, currently not
   converted at all. Another 256 KB of map, and there is only about 87 KB left
   in the fixed 512 KB window, so this is what forces either the banked window
   at 0x900000 or compressing the layout. Plane B is free for it once the
   background moves to the 32X.

3. **A world clamp on the player.** The camera is clamped to the map and the
   player is not, so walking off the edge finds no floor and falls forever. An
   act needs ends.

4. **The zone's own music.** The disc currently loops a menu track. Per-stage
   music cannot be found by name: the pack addresses files by the MD5 of their
   path and the stage's track is named in a scene object property the converter
   does not parse yet.

## Known gaps, not yet scheduled

- Objects of any kind: rings, springs, badniks, the goal. Out of scope by
  decision, and the thing that separates "a zone that runs" from "a zone you
  play"
- Act 2. Its FG Low is 1280x96 blocks, wider still than Act 1
- The palette budget is already spent: all three stage palettes go to the
  foreground and the fourth is Sonic's. FG High may not fit in what is left
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
