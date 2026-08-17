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
- [x] **Rings.** GHZ1's 445 Mania-mode Ring entities (`tools/convert_rings.py`,
      the scene's Mania-filter-and-sort pass, same filter rule PlaneSwitch's
      converter uses), drawn from a dedicated tile sheet
      (`tools/convert_ring.py`, Ring.bin + Items.gif -> `assets/ring/`, the
      >8 rotation-frame flip prebaked into the tiles, sharing FG High's
      CRAM line with a worst per-channel error of 1 MD step) and entirely
      68000-side: `game/md_src/rings.c` scans an x-sorted sliding window
      against the camera, runs RSDKv5's own touch test
      (`CheckObjectCollisionTouch`) against Sonic's per-frame hitbox
      (a new `assets/sonic/hitbox.bin`, additive to `convert_sonic.py`),
      and keeps the collected bitfield, ring counter and a small sparkle
      pool. The slave SH2 never learns rings exist -- no descriptor entry,
      no comm register spent. Sonic's pieces, ring sprites and sparkles
      share one static hardware-sprite list, linked in that draw order, comfortably
      under the VDP's 80-sprite table
- [x] **Collision path B, PlaneSwitch and ForceSpin -- loops and S-tunnels are
      traversable, not merely drawn.** TileConfig's second path is now parsed
      alongside the first (`tools/convert_stage.py`, 179 unique rows for GHZ
      against path A's 166, its own independent dedup pass) and linked
      straight into the SH2 program (`sh_src/collide_b.s`) rather than riding
      the 68000 program the way path A's descriptor route historically does:
      collision is a slave-SH2-only concern, so path B has no reason to spend
      any of that program's 512 KB window. Every finder in `sh_src/path.c`
      now picks its solid bits and its row table off
      `PathEntity.collisionPlane`, each transcribed from RSDKv5's own
      per-finder `solid =` line rather than one blanket rule (the air
      collision family reads a different bit pairing than the ground
      position finders do). `sh_src/plane_switch.c` ports
      `PlaneSwitch_CheckCollisions` itself: 106 scene markers (Mania filter)
      that rotate the player into the marker's own frame and flip
      collisionPlane -- and draw group -- on the correct side, applied every
      frame in scene slot order alongside ForceSpin (the loop-entry
      force-roll tube, `sh_src/force_spin.c`, landed earlier but undocumented
      until now) and BoundsMarker. The draw-group half now reaches Sonic's
      sprite too: `Player.drawGroupHigh` rides bit 15 of COMM6, the same
      register camera Y already used -- not a stolen coordinate bit, but an
      invariant-backed one, since camera Y is provably clamped well under
      2^15 for any act this converter could produce (`sh_src/comm.h`'s COMM6
      entry has the exact clamp chain). `md_src/sonic.c`'s sprite priority
      now follows that bit instead of a fixed low, matching
      `PlaneSwitch_CheckCollisions`' `other->drawGroup` write
- [x] **Sonic leans through loops.** `Player_HandleGroundRotation`/
      `HandleAirRotation` (Player.c:3207-3254) are transcribed onto the
      slave SH2 (`sh_src/player.c`), computed every frame exactly like the
      original regardless of animation, the same way `Player_State_Ground`/
      `Roll`/`TubeRoll`/`TubeAirRoll` all call it unconditionally. The MD has
      no sprite rotation, so this port shows the classic-engine rendition:
      8 stepped orientations instead of RSDK's smooth spin, snapped by the
      engine's own `ROTSTYLE_45DEG` formula (`Drawing.cpp:2703-2704`) and
      carried to the 68000 in 3 new bits of COMM6 (`sh_src/comm.h`'s repack,
      alongside camera Y and `drawGroupHigh`). Only 3 of the 8 orientations
      are baked art (45/90/135 degrees, `tools/convert_sonic.py`, nearest-
      neighbour sampled about the frame pivot the way `DrawSpriteRotozoom`
      would at those exact angles -- 90 degrees comes out an exact
      transpose+flip); the rest are flips `md_src/sonic.c` composes at
      render time: facing left negates the step before lookup, and the
      upper 4 steps are the lower 4 with both axes flipped. IDLE/PUSH/LOOK
      UP/CROUCH use the original's other rotation style, a plain flipH+flipV
      at the halfway point, no baked art needed; rolling and skidding never
      rotate, matching the original's own per-animation rotation style.
      113 KB of rotated tiles, comfortably under the budget `sh_src/mars.ld`
      carves out of the SH2's own ROM region for them

## Next

1. **The zone's own music.** The disc currently loops a menu track. Per-stage
   music cannot be found by name: the pack addresses files by the MD5 of their
   path and the stage's track is named in a scene object property the converter
   does not parse yet.

## Known gaps, not yet scheduled

- Springs, badniks, the goal -- rings are done (see above), the rest of
  GHZ1's objects are not. Still the thing that separates "a zone that runs"
  from "a zone you play"
- Ring sound. The original plays `Global/Ring.wav` with alternating left/
  right pan (`Player_GiveRings`, Player.c) on every collect; this port has no
  SFX system at all yet, so collecting is silent
- The 100-rings extra life (`Player_GiveRings`' `ringExtraLife` check,
  Player.c:928-934) is skipped -- there is no lives system to grant into
- Real HUD. The ring count only exists in the debug overlay text
  (`main.c`), not as in-game UI
- Authority seam: ring state (the collected bitfield, counter, sparkle pool)
  lives entirely on the 68000 because nothing else currently needs to know
  about rings. If damage-triggered ring scatter is ever built, the slave SH2
  (which owns collision and the player) would need to originate that event,
  which likely means ring state has to move to the SH2 side instead -- worth
  deciding before, not after, that feature starts
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
