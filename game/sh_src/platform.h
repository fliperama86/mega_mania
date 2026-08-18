#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include "player.h"

/* Platform (Common/Platform.c), GHZ Act 1's 60 instances, five of the
 * decompilation's ~15 PlatformTypes (Platform.h:6-24) -- the only ones Act 1's
 * own scene data ever uses, verified against tools/convert_objects.py's own
 * _validate_platform(): PLATFORM_FIXED(0)/FALL(1)/LINEAR(2)/SWING(4)/PUSH(6).
 * See md_src/platform.c's own top comment for the drawn-position agreement
 * proof and md_src/platform.c/this file's own per-state comments for exact
 * decomp line references.
 *
 * g_platform_tick: the shared logical clock every time-driven formula in this
 * whole TRAVERSAL batch (Platform/Bridge/CollapsingPlatform) is a pure
 * function of, standing in for Zone->timer (never ported -- this project has
 * no Zone struct at all). Incremented exactly once per s_main.c loop
 * iteration, i.e. once per physics tick, by platform_apply() itself (the
 * first of this batch's three _apply() calls in s_main.c, so it only
 * advances once per tick regardless of how many of this batch's functions
 * read it). The 68000 side (md_src/platform_clock.c) reconstructs the exact
 * same value with no new comm register at all -- see that file's own top
 * comment for the full construction and why it is exact, not approximate. */
extern uint32_t g_platform_tick;

/* Player_CheckCollisionBox/Player_CheckCollisionPlatform equivalents against
 * every one of GHZ1's 60 Platform entries, in scene-table order, mutating
 * p->e.x/y/velY/onGround/groundVel/collisionMode exactly like Platform_
 * Collision_Solid/_Platform (Platform.c:1351-1411,1737-1760) do through
 * Player_CheckCollisionBox/Touch -- see platform.c's own per-type comments.
 * Call once per tick, after spring_apply() (s_main.c) -- order relative to
 * spring_apply does not matter in GHZ1 (no spring sits inside a platform's
 * own footprint in this stage's data), but Platform has to run before
 * anything that reads p->e.onGround as already final for this tick the way
 * bounds_apply_markers/player_apply_world_bounds do. */
void platform_apply(Player *p);

#endif
