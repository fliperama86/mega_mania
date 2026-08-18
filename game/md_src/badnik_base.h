#ifndef BADNIK_BASE_H
#define BADNIK_BASE_H

#include <stdint.h>

/* Shared badnik infrastructure (BADNIKS batch): the parts every one of
 * Motobug/Crabmeat/BuzzBomber/Chopper/Newtron/Batbrain repeats, factored
 * out once instead of six times -- exactly the duplication this project
 * "just spent a day curing" (see this batch's own brief). Six classes'
 * own movement state machines sit on top of this in their own .c files.
 *
 * ---- Where authoritative state lives (read this before editing either
 * side) ----------------------------------------------------------------
 *
 * Physics/collision-response (player_hit/player_bounce_badnik, sh_src/
 * player.h) can ONLY run on the slave SH2: those functions mutate Player,
 * which lives in sh_src's own address space, unreachable from the 68000 at
 * all (see comm.h's own "fully allocated, no spare bits" note -- there is
 * no wire path to ask the SH2 to apply a hit on this program's behalf, and
 * none to have the SH2 report back which badnik it just destroyed either).
 * So sh_src/badnik.c independently re-derives the SAME thing this file
 * derives for drawing: each instance's current position (a pure function
 * of the scene table + a class-owned tick counter, see below) and whether
 * it is destroyed (a per-class bitmap, latched the first tick that
 * instance's own touch test sees an attacking touch). Both sides run the
 * identical arithmetic against the identical scene table
 * (assets/ghz/<class>s.bin on the 68000 via tools/gen_assets.py's bank-1
 * manifest, the SAME bytes linked directly into the SH2's own program image
 * as ghz_<class>s_sh -- sh_src/spring.c's own established precedent, see
 * that file's header comment), so -- PROVIDED both sides observe the same
 * tick and the same Sonic position at the same moment -- they converge on
 * the same answer with no wire traffic at all, the same "computed
 * independently, agrees because the math is identical" pattern
 * md_src/springs.c's frame-selection and sh_src/spring.c's physics
 * resolution already use for deciding "is Sonic touching this spring"
 * without talking to each other either.
 *
 * They are NOT perfectly synchronized, and this is a real, deliberate,
 * reported compromise, not an oversight:
 *
 *   1. Tick phase: each side keeps its OWN local tick counter, incremented
 *      once per its own main loop iteration. The 68000's for(;;) loop and
 *      the SH2's for(;;) loop (s_main.c) run 1:1 -- the SH2 blocks in
 *      comm_wait_tick() until the 68000 writes a new tick, then runs
 *      exactly one iteration -- so the two counters stay a FIXED, bounded
 *      phase apart (set once at boot by the handshake's own timing), never
 *      drifting further apart tick over tick. A moving badnik's MD-drawn
 *      position and its SH2-physics position can therefore differ by
 *      roughly one tick's worth of travel (well under a pixel at these
 *      classes' walking speeds) at any instant -- visually and physically
 *      immaterial, the same order of staleness comm.h's own seqlock already
 *      accepts for Sonic's own published position.
 *
 *   2. Touch-test precision: the 68000 only ever sees Sonic's PUBLISHED,
 *      whole-pixel world position (comm.h's COMM8/COMM10, int16_t) and
 *      published frame index -- coarser than the SH2's own live 16.16
 *      Player.e.x/e.y. So the MD's own touch test can lag the SH2's true
 *      one by up to a pixel or two of Sonic's travel right at a hitbox
 *      boundary: the SH2 may already consider an instance destroyed a
 *      frame or two before the MD's own independent test agrees and stops
 *      drawing it. Same order of magnitude as (1), same already-accepted
 *      class of staleness (rings.c's own ring_touches_sonic doc comment
 *      makes the identical whole-pixel-truncation observation for a
 *      different collectible). Never a FALSE kill in the other direction at
 *      steady state: once both sides' inputs agree (Sonic's published
 *      position has caught up), both sides' identical arithmetic reaches
 *      the identical conclusion and stays there -- the destroyed bit only
 *      ever latches forward, never un-latches, on either side.
 *
 * player_is_attacking()'s own MD-side equivalent (badnik_sonic_attacking
 * below) reads this same coarser signal (published frameIndex against
 * ANI_JUMP's range) rather than Player.animator.anim directly, for the
 * identical reason -- see that function's own comment.
 *
 * ---- VRAM (read main.c's own registration comment and this batch's final
 * report for the full arithmetic) --------------------------------------
 *
 * Every class registers exactly ONE obj_anim_window_register() covering
 * ALL of its own poses (walk+idle+turn+puff for Motobug, stand+walk+shoot
 * for Crabmeat, ...) at maxFrameTiles = that class's own generated
 * <CLASS>_MAX_FRAME_TILES constant (already the whole-class max, per
 * <class>_data.h's own doc comment) -- NOT one window per discrete pose
 * family the way obj_generic.h's own top-of-file guidance would ideally
 * want. This is a real, additional, class-INTERNAL lockstep cost beyond
 * the documented "same animation, different phase" tradeoff: two
 * instances of ONE class in genuinely different sub-states at the same
 * instant (one mid-turn, one walking) will show whichever pose the shared
 * window currently holds, not necessarily their own -- self-correcting
 * within a couple of frames as the window catches up to whichever request
 * last won, never a stuck-wrong pose. Forced by the arena's own free
 * budget (~60 tiles after rings/springs/signpost -- six classes' worst-case
 * 2x-max-frame windows sum to 240 tiles, four times that), not a design
 * preference; seven separate per-family windows per class was never
 * affordable regardless of the ARENA_MAX_CLASSES/OBJ_ANIM_WINDOW_MAX
 * headroom this batch also had to raise (obj_generic.h/.c, flagged
 * separately). A DESTROYED instance needs no window at all: it draws
 * nothing (OBJ_SKIP), so destruction costs zero extra VRAM. */

/* Sonic's current-frame outer hitbox, off the published sonicFrameIndex --
 * same ASSET_SONIC_HITBOX table and FALLBACK_HITBOX_* reduction rings.c's
 * own (file-static) sonic_hitbox_at already uses; duplicated here rather
 * than shared across translation units, the same call already made for
 * springs.c's own copy. */
void badnik_sonic_hitbox(uint16_t sonicFrameIndex,
                          int8_t *l, int8_t *t, int8_t *r, int8_t *b);

/* Player_CheckCollisionTouch-equivalent symmetric AABB overlap (same
 * derivation as rings.c's ring_touches_sonic / springs.c's
 * spring_touches_sonic -- both hitboxes provably flip-symmetric for this
 * game's real data, see either's own comment). bx/by is the badnik's own
 * CURRENT world position this tick (already advanced by the caller's own
 * movement formula), hbL/T/R/B its own class hitbox, sx/sy/sHb* Sonic's. */
uint8_t badnik_touches_sonic(int16_t bx, int16_t by,
                              int8_t hbL, int8_t hbT, int8_t hbR, int8_t hbB,
                              int16_t sx, int16_t sy,
                              int8_t sHbL, int8_t sHbT, int8_t sHbR, int8_t sHbB);

/* sh_src/player.h's player_is_attacking(): "True while curled, whether
 * jumping, rolling, or in the air after either" == Player.animator.anim ==
 * ANI_JUMP. The 68000 has no direct read of Player.animator.anim (a
 * SH2-only struct field); it reads the same fact off the already-published
 * sonicFrameIndex instead, checking membership in sonic_anims[ANI_JUMP]'s
 * own [first,first+count) range -- exactly rings.c's own frame_in_hurt()
 * pattern (that file's ANI_HURT edge-detector), just a level check here
 * rather than an edge (the badnik decide() below only needs "is Sonic
 * attacking THIS instant", never a rising-edge event). */
uint8_t badnik_sonic_attacking(uint16_t sonicFrameIndex);

/* Destroyed-bitmap helpers: same bit-packed shape as rings.c's own
 * collected[]/ring_is_collected()/ring_set_collected(). Each class owns its
 * own bitmap, sized to its own instance count -- this file only implements
 * the bit arithmetic once. */
uint8_t badnik_is_destroyed(const uint8_t *bitmap, uint16_t i);
void    badnik_set_destroyed(uint8_t *bitmap, uint16_t i);

/* The shared "player collision test, attacking-vs-hurting decision,
 * destruction" every class's own decide() calls once per candidate,
 * BEFORE running its own movement/pose-selection: if this instance is
 * already destroyed, returns 1 immediately (caller's own decide() should
 * return OBJ_SKIP with no further work -- a destroyed badnik has no pose to
 * pick and no further collision to test, matching Motobug_State_Smoke's
 * kind of one-way transition, minus the smoke: see this batch's own report
 * for why the destroy-effect pose is cut). Otherwise touch-tests (bx,by,
 * hitbox) against Sonic's published position/hitbox and, on a touch, either
 * latches `bitmap`'s bit i and returns 1 (attacking -- MD's own half of
 * "destruction and how a destroyed instance stays destroyed"; the SH2's own
 * sh_src/badnik.c independently reaches the same latch and is the one that
 * actually calls player_bounce_badnik -- see this file's own top comment)
 * or returns 0 with the bitmap untouched (a survivable hurt: the badnik
 * itself does not change, only Player does, on the SH2 side this file
 * cannot reach). */
uint8_t badnik_decide_common(uint8_t *bitmap, uint16_t i,
                              int16_t bx, int16_t by,
                              int8_t hbL, int8_t hbT, int8_t hbR, int8_t hbB,
                              int16_t sonicWorldX, int16_t sonicWorldY,
                              uint16_t sonicFrameIndex);

#endif
