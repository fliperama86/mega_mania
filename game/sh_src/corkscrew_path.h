#ifndef CORKSCREW_PATH_H
#define CORKSCREW_PATH_H

#include "player.h"

/* CorkscrewPath, ported from SonicMania/Objects/GHZ/CorkscrewPath.c: a
 * per-scene table of GHZ1's 2 CorkscrewPath entities. Each bends the
 * player's Y position along one arch of a cosine curve (RSDK.Cos1024) as
 * they run through it fast enough while grounded -- the "loop-de-loop"/
 * corkscrew hill shape drawn into GHZ1's own tile art, this object is the
 * INVISIBLE LOGIC that makes the player's position actually follow that
 * drawn curve instead of falling straight through it or grinding along the
 * flat tile collision underneath.
 *
 * CONFIRMED to draw nothing in retail: CorkscrewPath_Draw (CorkscrewPath.c:
 * 78) is a literal empty function body, `void CorkscrewPath_Draw(void) {}`
 * -- no self->visible field exists on this class at all (Serialize only
 * registers period/amplitude/angle, CorkscrewPath.c:115-120), so there is
 * no DebugMode->debugActive pattern to look for here the way InvisibleBlock/
 * SpinBooster have one -- this class simply never draws under ANY
 * condition, debug or retail (its EditorDraw, GAME_INCLUDE_EDITOR-only, is
 * the only place it ever shows a sprite, and that build configuration does
 * not exist in this port at all).
 *
 * This CPU owns CorkscrewPath, same reason spring.c/force_spin.c/plane_
 * switch.c/invisible_block.c/spin_booster.c do: it writes Player's own
 * position/velocity/onGround fields directly.
 *
 * COMPROMISES, surfaced up front (see corkscrew_path.c's own comments for
 * the full derivation of each):
 *   - CorkscrewPath_Update's own `self->activePlayers` bookkeeping
 *     (CorkscrewPath.c:29,31,50,53-54) is DEAD CODE in the original itself:
 *     the only statement that ever sets the bit (line 54) sits inside a
 *     branch reachable only when the bit is ALREADY set, so it can never
 *     transition from 0 to nonzero and the whole "already active, keep
 *     riding" branch (lines 53-69) can never execute. Not a simplification
 *     this port invented -- verified against the literal control flow of
 *     the decompiled source itself -- so it is not transcribed at all; see
 *     corkscrew_path.c's own top comment for the full proof.
 *   - The player-facing visual (ANI_SPRING_CS, a dedicated 24-frame rolling
 *     animation matched to the corkscrew's own current angle,
 *     CorkscrewPath.c:41/45/62/66) has NO converted asset in this port at
 *     all -- this port's own sonic_data.h ANI_* enum has no ANI_SPRING_CS
 *     entry, and converting Sonic's sprite sheet is out of this batch's
 *     scope. The physics (position snapped onto the curve, onGround forced
 *     true, velocity.y zeroed) is fully ported; the animation switch itself
 *     is CUT, not substituted with a stand-in pose -- the player keeps
 *     whatever ground animation they already had (typically ANI_RUN/
 *     ANI_DASH at the speeds this trigger requires) while their Y position
 *     rides the curve. Visually incomplete (no visible "roll" through the
 *     loop), functionally complete (the position genuinely follows the
 *     drawn art).
 *   - The matching `player->direction |= FLIP_Y` / `&= ~FLIP_Y` (upside-down
 *     sprite flip for the underside of the loop, CorkscrewPath.c:22,40,44,
 *     61,65) is CUT: this port's own Player.direction is a plain 0/1
 *     left/right boolean (player.h) with no second flip axis at all, a
 *     structural gap, not a missing table value -- there is no field to
 *     write this into. */
void corkscrew_path_apply(Player *p);

#endif
