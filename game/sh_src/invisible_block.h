#ifndef INVISIBLE_BLOCK_H
#define INVISIBLE_BLOCK_H

#include "player.h"

/* InvisibleBlock, ported from SonicMania/Objects/Global/InvisibleBlock.c: a
 * per-scene table of GHZ1's 19 Mania-mode InvisibleBlock entities -- solid
 * rectangular collision with no sprite at all in retail, applied every
 * frame in table order (same shape as sh_src/spring.c/force_spin.c/plane_
 * switch.c) after player_update has settled this frame's position.
 *
 * CONFIRMED to draw nothing in retail (InvisibleBlock.c):
 *   - Create() (line 62): `self->visible = false;` unconditionally.
 *   - Update() (line 45): `self->visible = DebugMode->debugActive;` -- the
 *     ONLY other write to self->visible in the whole file, and it is gated
 *     on DebugMode->debugActive, which is permanently false in a retail
 *     build (no debug menu exists in this port). So visible is false at
 *     creation and stays false every frame after, matching the "visible
 *     only under DebugMode->debugActive" pattern this task was briefed to
 *     look for. Draw()/DrawSprites() are ported nowhere on the 68000 side:
 *     see tools/convert_objects.py's own INVISIBLEBLOCK_SCENE comment for
 *     why no art recipe exists for this class either.
 *
 * This CPU (not the 68000) owns InvisibleBlock, for the same reason spring.c/
 * force_spin.c/plane_switch.c do: solidity has to move Player's own
 * position/velocity/collisionMode fields, which only the SH2 running
 * player.c can write. See invisible_block.c for the per-marker collision
 * math and the crush (death) rule. */
void invisible_block_apply(Player *p);

#endif
