#ifndef MOTOBUG_H
#define MOTOBUG_H

#include "md.h"

/* Motobug (Motobug.c): ground patrol badnik, GHZ1 x9. Terrain-free port --
 * see this file's own .c header comment for why (Motobug_State_Move/Fall/
 * Idle/Turn all key off RSDK.ObjectTileGrip, real per-pixel floor/ledge
 * sensing that only the slave SH2's own path.c can perform -- unreachable
 * from the 68000, rings.c's own comment on ghz_collide_index/rows has the
 * same wall this file hits). Patrols a fixed-radius triangle wave around
 * its scene spawn X instead. */

#define MOTOBUG_SPRITE_CAP 18   /* 9 instances * 2 pieces/frame (motobug_data.h) */

void motobug_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);
uint16_t motobug_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                      uint16_t camX, uint16_t camY,
                      int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

#endif
