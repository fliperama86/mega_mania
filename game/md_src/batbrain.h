#ifndef BATBRAIN_H
#define BATBRAIN_H

#include "md.h"

/* Batbrain (Batbrain.c): perches at its own spawn point, dive-bombs the
 * player when they wander near and below, then returns, GHZ1 x7.
 * Batbrain_State_CheckPlayerInRange/DropToPlayer/Fly all key off Sonic's
 * position and are terrain-free, ported closely; Batbrain_State_
 * FlyToCeiling's own return trip is NOT (RSDK.ObjectTileCollision against
 * a real ceiling tile, Batbrain.c:199, unreachable from the 68000) -- this
 * port returns to the instance's own scene-authored spawn Y instead of a
 * real ceiling scan, the same fixed-reference substitute signpost.c's own
 * fall uses for its landing Y (see that file's header comment for the
 * precedent).
 *
 * RSDK.Rand(0,8)-GATED TRIGGERS REPLACED WITH FIXED DELAYS: the real
 * CheckPlayerInRange (Batbrain.c:133) and Fly->FlyToCeiling (Batbrain.c:
 * 178) transitions each roll a ~1-in-8 chance per tick once their distance
 * gate is satisfied. Two independent RNG streams (68000 and SH2, no shared
 * seed or comm channel to synchronize one) would make the two sides'
 * SIMULATED positions diverge exactly where this batch's shared-state
 * design needs them to agree (see badnik_base.h's own top comment) --
 * so both this file and sh_src/badniks.c's own Batbrain model replace the
 * roll with a fixed tick delay instead: same eventual behaviour (drops
 * after noticing the player, flies back after drifting far enough), a
 * different, deterministic timing distribution. */

#define BATBRAIN_SPRITE_CAP 14   /* 7 instances * 2 pieces/frame (batbrain_data.h) */

void batbrain_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);
uint16_t batbrain_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                       uint16_t camX, uint16_t camY,
                       int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

#endif
