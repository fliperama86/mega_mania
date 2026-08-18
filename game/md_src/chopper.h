#ifndef CHOPPER_H
#define CHOPPER_H

#include "md.h"

/* Chopper (Chopper.c), JUMP variant only: a vertical bounce anchored at its
 * own spawn Y, GHZ1 x13 (both types combined in the scene table -- SWIM
 * instances are skipped, see below). CHOPPER_JUMP never touches
 * self->velocity.x at all (Chopper_State_Jump only ever updates
 * position.y, Chopper.c:141-170 -- velocity.x is set in Chopper_State_Init
 * but never read again on this path), so this port's Jump chopper is
 * purely vertical, at a fixed X -- and needs no terrain data either
 * (Chopper_State_Jump's own bounce trigger is `position.y > startPos.y`,
 * Chopper.c:161-164, a comparison against its OWN spawn Y, not a tile
 * query). Every constant below is decomp-exact.
 *
 * CHOPPER_SWIM IS OUT OF SCOPE, PER THIS BATCH'S OWN BRIEF: "this port has
 * no water yet, so implement the non-water variant and report the
 * omission". Chopper_State_Swim/ChargeDelay/Charge also depend on real
 * wall-tile collision (RSDK.ObjectTileCollision, Chopper.c:178-296) this
 * CPU cannot reach either, on top of the water gate -- a scene entry whose
 * own `type` byte reads CHOPPER_SWIM (1) is never drawn (OBJ_SKIP always),
 * not approximated. */

#define CHOPPER_SPRITE_CAP 13   /* 13 instances * 1 piece/frame (chopper_data.h) */

void chopper_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);
uint16_t chopper_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                      uint16_t camX, uint16_t camY,
                      int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

#endif
