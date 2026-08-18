#ifndef NEWTRON_H
#define NEWTRON_H

#include "md.h"

/* Newtron (Newtron.c): a proximity-triggered badnik with two authored
 * variants, GHZ1 x21 (the most of any class in this batch). Both variants
 * are stateless until Sonic's PUBLISHED world position enters
 * hitboxRange (+-128,+-64, Newtron.c:97-100) of the instance's own spawn --
 * a per-instance trigger, unlike motobug.c/crabmeat.c/buzzbomber.c's own
 * free-running class-wide patrol, so this class keeps small per-instance
 * state arrays (state/timer/dir) instead of one shared phase.
 *
 *   NEWTRON_SHOOT: Newtron_State_Appear/Shoot/FadeAway (Newtron.c:185-287),
 *     decomp-exact timings (fire at tick 30, revert to idle pose at 45,
 *     deactivate at 90 -- Newtron.c:254-268) minus the projectile itself
 *     (cut across every class in this batch, see crabmeat.h's own
 *     comment) and minus the real alpha fade-in/out (Newtron_State_Appear/
 *     FadeAway's own self->alpha ramp -- real MD/32X hardware sprites have
 *     no per-sprite alpha, same reasoning rings.c's own sparkle comment
 *     gives; this port just cuts straight to visible/invisible).
 *
 *   NEWTRON_FLY: Newtron_State_StartFly/Fly (Newtron.c:209-248) fall to the
 *     floor and then floor-follow horizontally -- both terrain-dependent
 *     (RSDK.ObjectTileGrip, unreachable from the 68000, same wall motobug.c
 *     hits). This port's Fly instead launches immediately at its own spawn
 *     Y, in a direction snapshotted once at trigger toward wherever Sonic
 *     was (Newtron_GetTargetDir's own rule, Newtron.c:158-159), at the
 *     decomp's own 0x20000 (2px/tick) velocity (Newtron.c:222-224), for a
 *     fixed NEWTRON_FLY_TICKS before returning to its dormant spawn point
 *     -- a real, flagged divergence from the real floor-hugging flight. The
 *     self->flameAnimator overlay (Newtron_Draw, Newtron.c:26-27) is cut
 *     for the same "second VRAM window per instance" reason
 *     buzzbomber.h's own header comment gives for BuzzBomber's wing/
 *     thrust. */

#define NEWTRON_SPRITE_CAP 63   /* 21 instances * 3 pieces/frame (newtron_data.h) */

void newtron_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);
uint16_t newtron_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                      uint16_t camX, uint16_t camY,
                      int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

#endif
