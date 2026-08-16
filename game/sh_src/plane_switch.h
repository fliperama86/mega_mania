#ifndef PLANE_SWITCH_H
#define PLANE_SWITCH_H

#include "player.h"

/* PlaneSwitch, ported from SonicMania/Objects/Global/PlaneSwitch.c's
 * PlaneSwitch_CheckCollisions: a per-scene table of line-segment markers
 * (GHZ Scene1.bin's PlaneSwitch entities) that flip the player's collision
 * plane -- and which foreground layer solidity bits path.c's finders test --
 * when crossing the line from the correct side. This is what makes GHZ's
 * loops and S-tunnels traversable rather than merely drawn: without it,
 * collisionPlane never leaves 0 and every path-B-only surface (the far side
 * of a loop, the tunnel floor under an S-curve) is passed straight through.
 * See plane_switch.c for the per-marker math and the table itself. */
void plane_switch_apply(Player *p);

#endif
