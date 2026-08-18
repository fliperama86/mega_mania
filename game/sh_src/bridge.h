#ifndef BRIDGE_H
#define BRIDGE_H

#include "player.h"

/* Bridge (GHZ/Bridge.c), GHZ Act 1's 13 rope-bridge instances: a sagging
 * chain of planks whose sag responds to the player's own weight/position
 * (Bridge_HandleCollisions' own depression/timer ramp, Bridge.c:143-279).
 * Single-player specialised (this port never has a second entity to stand on
 * a bridge) and with Bridge_Burn (fire-shield ignition, Bridge.c:110-141)
 * dropped entirely -- not a compromise: this port has no shields at all
 * (sh_src/player.h's own top comment, "This port has no shields"), so
 * self->shield==SHIELD_FIRE can never be true and Bridge_Burn's own trigger
 * (Bridge_Update:42-43/Bridge_HandleCollisions:234-236,261-262) is genuinely
 * unreachable code for this port, same as md_src/rings.c's own precedent for
 * "computed upstream but provably unused here" fields.
 *
 * See md_src/bridge.c's own top comment for how the 68000 side re-derives
 * the same per-tick timer/depression integration with no comm bits at all
 * (it never needed g_platform_tick either -- Bridge's own state evolves from
 * "is the player currently on this bridge", independently observable on both
 * CPUs from Sonic's own published world position, not from any shared
 * clock). */
void bridge_apply(Player *p);

#endif
