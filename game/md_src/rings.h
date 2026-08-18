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
 * tiles.bin + the generated ring_data.h/.c this file depends on).
 *
 * Draw path: the ring TABLE (scan/window/touch-test/collect) is table-
 * driven, on md_src/obj_generic.h's shared skeleton -- rings.c's own
 * ring_decide() is the one per-object hook, and owns the touch test/collect/
 * sparkle-spawn side effects the old hand-rolled scan used to run inline
 * (see rings.c's own comment on why the generic engine's per-entry loop has
 * to keep calling decide() past its own visible-sprite cap for this exact
 * reason). rings_emit_sparkles() below stays entirely hand-written: it is a
 * runtime particle pool (fixed-size, age-based eviction), not x-sorted scene
 * entries, and does not fit the ObjTypeDesc shape at all. */

#include "md.h"

/* Hardware-sprite budget for rings: measured worst case is 32 rings live in
 * any single 320x224 camera window (recon over GHZ1's 445 Mania-mode Ring
 * entities); 48 leaves headroom without help. This is now the generic
 * path's own per-frame maxCount for this type (obj_generic.h) -- how big a
 * slice of scratch RAM rings.c's own candidates can ever need -- not a
 * permanent reservation out of the VDP's 80-sprite hardware table any more;
 * main.c's shared pool (md_src/obj_pool.h) is what actually enforces that
 * 80-sprite limit today, across every type's candidates plus Sonic's own
 * reserved piece count. */
#define RING_SPRITE_CAP    48

/* Ring_Collect spawns exactly 2 sparkles per ring (Ring.c:155-178 -- the
 * loop's own `sparkle->timer = 2 * i++` double-increments, so only i=0 and
 * i=2 of the nominal 4 iterations ever run; see rings.c). 16 gives 8
 * simultaneous collections of headroom; beyond that the oldest sparkle is
 * dropped to make room (deviation, cosmetic -- see rings.c). */
#define SPARKLE_POOL_SIZE  16

/* Zero all ring state (collected bitfield, ring counter, sparkle pool,
 * sliding-window indices, RNG seed), then bring up TWO independent VRAM
 * tenants through md_src/obj_generic.h, both OBJ_PRI_RING (the highest
 * priority any tenant carries today, since a still-visible ring is a missed
 * collectible):
 *
 *   - The 16-frame rotation cycle every ring on screen shares, through the
 *     per-class ANIMATION WINDOW (obj_anim_window_register()): 8 tiles (one
 *     4-tile frame, double-buffered) instead of the 64 tiles the whole
 *     rotation cycle used to cost resident. Boot-loaded synchronously
 *     (obj_anim_window_boot_load()) with frame 0 already showing, since
 *     GHZ1's first ring sits inside the very first camera view and the
 *     normal runtime admission path only starts running once main()'s own
 *     loop does.
 *   - The 92-tile sparkle portion (RING_SPARKLE1/3, ring_data.h), still
 *     whole-sheet resident on the ordinary arena (obj_arena_register()),
 *     unmigrated on purpose -- see rings.c's own comment on
 *     ringSparkleArenaDesc for why simultaneous, independently-timed
 *     sparkles are exactly the workload the animation window does not try
 *     to serve.
 *
 * Total resident footprint: 8 + 92 = 100 tiles, down from the previous
 * 156 (the whole rotation+sparkle sheet, always resident) -- see
 * obj_generic.h's own top-of-section comment on the animation window for
 * the full reasoning, including the one real, visible compromise it buys
 * (rings already shared one rotation phase before this file existed, so it
 * costs rings nothing; a class with independently-animated instances would
 * pay a real "lockstep" cost this file's own comment does not, and should
 * not, hide).
 *
 * Also runtime-checks that rings.bin's own leading count word matches the
 * compile-time RING_COUNT (the same "cannot go stale in one file and not
 * another" reasoning as GHZ_MAP_W/GHZ_MAP_H travelling through the
 * descriptor table); on either that or a VRAM allocation failure, rings are
 * permanently disabled for this run (every entry point becomes a no-op,
 * rings_enabled() reports it) rather than walking a mis-sized table or
 * uploading over whatever else the arena already holds. A sparkle-portion
 * allocation failure specifically degrades more gently: rotation stays live
 * and rings remain collectible, only the collect-sparkle visual is lost
 * (rings_emit_sparkles() simply never draws anything). */
void rings_init(void);

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
 * for the debug overlay, and also what md_src/comm.c's comm_send_input()
 * reads (nonzero or not) to publish the hasRings bit sh_src/player.c's
 * player_hit() needs for the 0-rings-death rule (sh_src/comm.h's COMM_TICK
 * entry). Not a second ring counter for that use -- comm_send_input() only
 * ever asks "!= 0", never reads the count itself. */
uint16_t rings_collected_count(void);

/* Award `amount` rings directly, clamped to 999 the same way ring_decide()'s
 * own collect path already clamps (Player_GiveRings, Player.c:926) --
 * ItemBox's Ring/HyperRing monitor types (md_src/itembox.c) are this
 * function's only callers: ItemBox_GivePowerup's ITEMBOX_RING case
 * (ItemBox.c:481, `Player_GiveRings(player, 10, true)`) genuinely has rings
 * to give in this port (rings.c owns a real counter), so it is implemented
 * for real rather than reported inert. See itembox.c's own comment on why
 * HyperRing (ITEMBOX_HYPERRING, ItemBox.c:540-543, `player->hyperRing =
 * true`) also routes here instead of being left inert: this port has no
 * hyperRing/ring-magnetism system to flip a flag on, so it substitutes the
 * one verified "an item box gives rings" amount this file has (10, the Ring
 * type's own constant) rather than inventing a new number. */
void rings_add(uint16_t amount);

/* False once rings_init() found the tile budget or the table count check
 * failed -- for the debug overlay, so a corrupted font or a mis-walked
 * table would never be the first sign of trouble. */
uint8_t rings_enabled(void);

/* How many times the sparkle pool was full and the oldest sparkle got
 * evicted to make room for a new one (deviation, cosmetic -- rings.c's
 * spawn_sparkle()). Expected to stay 0 in normal play; exposed for the
 * debug overlay rather than assumed. */
uint16_t rings_sparkle_drop_count(void);

/* Ring_LoseRings' inner+outer tiers (Ring.c:199-246, up to 32 rings): a
 * runtime particle pool, same shape as the sparkle pool above, but with real
 * physics (gravity, an approximate bounce, its own re-collection window and
 * expiry) instead of a fire-and-forget animation -- see rings.c's own
 * comment at LOST_RING_CAP for why the original's third "big ring" tier
 * (rings > 32) is not carried, and at the bounce site for the flat-plane
 * approximation this port's 68000 side has to use instead of real tile
 * collision (that data reaches only the slave SH2 -- see path.c's own
 * comment, sh_src/path.c). No trigger parameter: rings_lost_tick() below
 * infers a hit entirely from the already-published Sonic animation frame
 * index entering ANI_HURT's range (sh_src/player.c's player_hit() sets that
 * animation on every hit, no comm protocol change needed) and scatters
 * whatever ringPlayerCount already holds, matching Ring_Hit's own
 * `Ring_LoseRings(player, player->rings, ...); player->rings = 0;` -- see
 * that function's own comment for why this port cannot instead be TOLD a
 * hit happened or how many rings to drop. Registered as one row in
 * md_src/main.c's OBJ_TYPE_LIST (matches obj_pool.h's ObjDrawFn/ObjTickFn
 * exactly), the one main.c touch this feature needs. */
#define LOST_RING_CAP 32

uint16_t rings_lost_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                         uint16_t camX, uint16_t camY,
                         int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);
void rings_lost_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

#endif
