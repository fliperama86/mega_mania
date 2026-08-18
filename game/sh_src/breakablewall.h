#ifndef BREAKABLEWALL_H
#define BREAKABLEWALL_H

#include "player.h"

/* BreakableWall, ported from SonicMania/Objects/Common/BreakableWall.c: GHZ1's
 * 23 Mania-mode BreakableWall entities. See md_src/breakablewall.h's own
 * header comment for the full mechanism (the object redraws no sprite of its
 * own; it owns the STAGE's own tile solidity/graphic at its footprint) and
 * every compromise this port makes (single WALL-type break rule for every
 * instance, no onGround/groundVel gate, no flying debris).
 *
 * This file owns COLLISION: once a wall is broken, sh_src/path.c's cell_at()
 * (the one function every ground/air/wall finder in that file funnels
 * through) stops reading solidity off ghz_map/ghz_map_fgh at that wall's own
 * block footprint and reports it empty instead -- see path.c's own comment
 * at its cell_at() call site. Nothing else in this file resolves collision
 * directly: unlike Spikes/SpikeLog, a BreakableWall's own hitbox is never
 * pushed against on its own (see md_src/breakablewall.h's header comment on
 * why the standalone Player_CheckCollisionBox branch is not transcribed --
 * the real tile collision this stage's own converted map data already
 * carries at (some of) these 23 footprints is the sole solidity source). */
void breakablewall_apply(Player *p);

/* path.c's own hook (see this file's header comment): true if (blockX,
 * blockY) on tile LAYER `layer` (0 = FG Low/g_ghz_map, 1 = FG High/
 * ghz_map_fgh -- which physical map table path.c's cell_at() was about to
 * read, NOT PathEntity.collisionPlane/path A vs B, a different axis
 * entirely) falls inside a currently-broken instance's own footprint. */
uint8_t breakablewall_solid_override(int32_t blockX, int32_t blockY, uint8_t layer);

#endif
