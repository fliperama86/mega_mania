#ifndef SPIN_BOOSTER_H
#define SPIN_BOOSTER_H

#include "player.h"

/* SpinBooster, ported from SonicMania/Objects/Common/SpinBooster.c: a
 * per-scene table of GHZ1's 4 SpinBooster entities -- a line-segment
 * trigger (rotated by its own `direction`) that forces the player into the
 * curled tube-roll state and (optionally) boosts their speed while passing
 * through, same "line segment the player crosses" shape as sh_src/
 * force_spin.c's own ForceSpin, just with a rotation angle and an optional
 * speed boost ForceSpin does not carry.
 *
 * CONFIRMED to draw nothing in retail (SpinBooster.c):
 *   - Create() (line 95): `self->visible = false;` unconditionally
 *     (SceneInfo->inEditor branch only).
 *   - Update() (line 58): `self->visible = DebugMode->debugActive;` -- the
 *     only other write to self->visible, gated on DebugMode->debugActive,
 *     permanently false in this port's retail build (no debug menu exists).
 *     Draw()/DrawSprites() are ported nowhere on the 68000 side: see tools/
 *     convert_objects.py's own SPINBOOSTER_SCENE comment (StageLoad() only
 *     borrows Global/PlaneSwitch.bin for that debug box).
 *
 * This CPU owns SpinBooster, same reason spring.c/force_spin.c/plane_
 * switch.c/invisible_block.c do: it writes Player's own state/velocity/
 * animation/controlLock fields directly, which only the SH2 running
 * player.c can do.
 *
 * COMPROMISE, surfaced up front: SpinBooster_HandleRollDir's `autoGrip`
 * mechanism (SpinBooster.c:215-300), which calls RSDK.ObjectTileGrip to snap
 * the player's position/angle onto a specific tube wall the instant they
 * enter, is CUT. This port has no equivalent of RSDK.ObjectTileGrip (a
 * generic tile-surface search-and-snap primitive) and implementing one from
 * scratch is out of this class's own scope -- see spin_booster.c's own
 * top comment for the full reasoning, including why this is provably dead
 * for 1 of GHZ1's 4 rows (autoGrip==0) but genuinely active, and cut, for
 * the other 3 (autoGrip 3 or 4). Effect: the player still enters the curled
 * tube-roll state and gets boosted, but is not forcibly re-angled onto the
 * tube's own curved wall geometry the instant they cross the trigger --
 * they continue riding on whatever collisionMode/onGround this port's own
 * ordinary path.c physics already had them on. Likely close to correct in
 * practice (GHZ1's own tube geometry is walked by ordinary floor/wall
 * collision the rest of the time regardless), but not verified pixel-exact
 * against the original, and flagged here rather than silently assumed
 * equivalent. */
void spin_booster_apply(Player *p);

#endif
