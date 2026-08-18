#ifndef CRABMEAT_H
#define CRABMEAT_H

#include "md.h"

/* Crabmeat (Crabmeat.c): ground patrol + pause-and-"shoot" badnik, GHZ1
 * x11. Terrain-free port, same reasoning as motobug.h (Crabmeat_State_
 * Moving's own RSDK.ObjectTileGrip ledge check, Crabmeat.c:130, needs
 * terrain data unreachable from the 68000) -- patrols a fixed-radius
 * triangle wave, decomp-exact this time (the 128-tick move cap and
 * 60-tick shoot pause, Crabmeat.c:129/152, ARE fixed constants already,
 * not real terrain sensing, so no invented amplitude is needed the way
 * motobug.c's own MB_AMPLITUDE had to be).
 *
 * PROJECTILES CUT (both lobbed pellets, Crabmeat_State_Shoot's own
 * CREATE_ENTITY pair, Crabmeat.c:171-177): this batch cuts projectile
 * firing across every class uniformly (Crabmeat/BuzzBomber/Newtron all
 * have one in the decomp) rather than half-implementing it on some
 * classes and not others -- see this batch's own final report for the
 * full reasoning (a second, genuinely-divergent-state VRAM window per
 * projectile-bearing class, plus a mirrored SH2-side hazard pool per
 * class, did not fit this batch's time/VRAM budget alongside six real
 * movement state machines). Crabmeat still walks and pauses in its
 * "shoot" pose at each reversal -- it just never spawns anything while
 * doing so. */

#define CRABMEAT_SPRITE_CAP 22   /* 11 instances * 2 pieces/frame (crabmeat_data.h) */

void crabmeat_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);
uint16_t crabmeat_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                       uint16_t camX, uint16_t camY,
                       int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

#endif
