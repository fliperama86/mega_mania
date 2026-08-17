#ifndef RINGS_H
#define RINGS_H

/* GHZ1's collectible rings: an entirely 68000-side feature, per this port's
 * architecture. The slave SH2 already publishes camera X/Y, Sonic's world
 * X/Y and his animation frame index every frame over the comm seqlock
 * (md_src/comm.c, sh_src/comm.h); the SH2 side never learns rings exist at
 * all -- ring/sparkle state, the collected bitfield and the touch test all
 * live here, driven purely off those existing comm fields. See
 * tools/convert_rings.py (the scene's ring table -> assets/ghz/rings.bin)
 * and tools/convert_ring.py (Ring.bin's sprite frames -> assets/ring/
 * tiles.bin + the generated ring_data.h/.c this file depends on). */

#include "md.h"

/* Hardware-sprite budget for rings: measured worst case is 32 rings live in
 * any single 320x224 camera window (recon over GHZ1's 445 Mania-mode Ring
 * entities); 48 leaves headroom without help. Sparkles are capped at the
 * pool size below regardless of how many are mid-animation. Sonic's own
 * pieces (SONIC_MAX_PIECES, sonic_data.h) plus these two must stay under
 * the VDP's 80-sprite hardware table (main.c sizes its sprite list off
 * this). */
#define RING_SPRITE_CAP    48

/* Ring_Collect spawns exactly 2 sparkles per ring (Ring.c:155-178 -- the
 * loop's own `sparkle->timer = 2 * i++` double-increments, so only i=0 and
 * i=2 of the nominal 4 iterations ever run; see rings.c). 16 gives 8
 * simultaneous collections of headroom; beyond that the oldest sparkle is
 * dropped to make room (deviation, cosmetic -- see rings.c). */
#define SPARKLE_POOL_SIZE  16

/* Upload every ring/sparkle tile once and zero all ring state (collected
 * bitfield, ring counter, sparkle pool, sliding-window indices, RNG seed).
 * firstTile is the first free VRAM tile after Sonic's per-frame upload
 * window (main.c: sonic_gfx_init()'s return value), matching how that
 * window itself sits right after the stage's own tiles.
 *
 * Runtime-checks that firstTile + RING_TILE_COUNT (ring_data.h) stays below
 * TILE_FONTINDEX, and that rings.bin's own leading count word matches the
 * compile-time RING_COUNT (the same "cannot go stale in one file and not
 * another" reasoning as GHZ_MAP_W/GHZ_MAP_H travelling through the
 * descriptor table); on either failure rings are permanently disabled for
 * this run (every entry point becomes a no-op) rather than walking a
 * mis-sized table or uploading over the font. Returns the next free VRAM
 * tile: firstTile + RING_TILE_COUNT if rings are live, firstTile unchanged
 * if they were disabled. */
uint16_t rings_init(uint16_t firstTile);

/* Sparkle pre-pass: advance every live sparkle's timer/animation one tick
 * and emit its hardware-sprite entry into list[] starting at
 * list[firstIndex], link chain built the same way sonic_build() builds its
 * own (the k-th entry written gets link = firstLink+k+1). Runs BEFORE
 * sonic_build() in main.c's build order because the original draws collect
 * sparkles in draw group 8, above the player's 4 and FG High's 6
 * (Ring_Collect, Ring.c:167; Zone.c:185-187; GHZ Scene1.bin layer header),
 * and MD sprite-vs-sprite overlap is resolved by table order alone -- so
 * sparkles must come earlier in the chain than Sonic, and their attr
 * carries the priority bit to clear Plane B as well.
 *
 * Owns the once-per-frame sparkle tick; rings_update() only spawns into the
 * pool. A sparkle spawned by this frame's collect is therefore first drawn
 * on the NEXT frame -- one tick later than the original, which draws its
 * reserve-slot entity within the touch tick (deviation, cosmetic, on a
 * roughly 30-tick effect).
 *
 * Returns how many list[] entries were written (0 when the pool is idle or
 * rings are disabled). */
uint16_t rings_emit_sparkles(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                             uint16_t camX, uint16_t camY);

/* Advance the shared ring rotation frame, run the touch test for every
 * uncollected ring in the camera's x window against Sonic's current-frame
 * hitbox (collects spawn sparkles into the pool for the next frame's
 * pre-pass), and append ring sprite entries for whatever is visible this
 * frame into list[], starting at list[firstIndex], continuing the same
 * link-chain convention as rings_emit_sparkles()/sonic_build() (main.c
 * fixes up the true last entry's link to 0 afterward, whichever function
 * wrote it).
 *
 * camX/camY are the same screen-space top-left main.c already streams the
 * tilemap from; sonicWorldX/Y and sonicFrameIndex are exactly what
 * comm_read_frame() published this frame (world position in whole pixels --
 * the original's positions are 16.16 fixed, so this touch test runs at
 * pixel, not sub-pixel, resolution; sub-pixel deviations of less than 1px
 * are possible and are this port's throughout, not new here).
 *
 * Returns how many list[] entries were written. */
uint16_t rings_update(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                      uint16_t camX, uint16_t camY,
                      int16_t sonicWorldX, int16_t sonicWorldY,
                      uint16_t sonicFrameIndex);

/* Rings collected so far, clamped 0..999 (Player_GiveRings, Player.c:926) --
 * for the debug overlay. */
uint16_t rings_collected_count(void);

/* False once rings_init() found the tile budget or the table count check
 * failed -- for the debug overlay, so a corrupted font or a mis-walked
 * table would never be the first sign of trouble. */
uint8_t rings_enabled(void);

/* How many times the sparkle pool was full and the oldest sparkle got
 * evicted to make room for a new one (deviation, cosmetic -- rings.c's
 * spawn_sparkle()). Expected to stay 0 in normal play; exposed for the
 * debug overlay rather than assumed. */
uint16_t rings_sparkle_drop_count(void);

#endif
