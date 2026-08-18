#ifndef MD_BRIDGE_H
#define MD_BRIDGE_H

#include "md.h"

/* Bridge (GHZ/Bridge.c): GHZ Act 1's 13 rope-bridge instances, drawing only
 * -- sag physics/collision is sh_src/bridge.c's own. See that file's own top
 * comment for the single-player-specialised Bridge_Update+HandleCollisions
 * port this mirrors, and this file's own .c for how the 68000 side
 * re-derives the same timer/depression integration purely from Sonic's
 * published world position (Bridge needs no g_platform_tick at all -- its
 * own state evolves from "is the player on it right now", independently
 * observable on both CPUs, not from a shared clock).
 *
 * SPRITE BUDGET: this is the batch's own headline hazard. Bridge_Draw
 * expands ONE entity into self->length+1 individual plank sprites (Bridge.c:
 * 50-81) -- GHZ1's own longest single bridge is 17 planks (x=7992), and two
 * bridges sit close enough together (x=4752 length 9->10 planks, x=4920
 * length 8->9 planks, 168px apart, well within one 320px camera view) to
 * both be on screen at once: 10+9 = 19 planks live simultaneously, this
 * batch's own measured worst case (the brief's own "+27 worst case" already
 * anticipated a number in this neighbourhood). BRIDGE_SPRITE_CAP below is
 * set with real headroom above that measurement, not tight against it. */
#define BRIDGE_SPRITE_CAP 24

void bridge_init(void);

/* ObjTickFn: re-derives the timer/depression sag state for every bridge,
 * observationally, off Sonic's published world position -- see bridge.c's
 * own top comment for the exact reduction from sh_src/bridge.c's real,
 * collision-driven version (no velY/onGround published, so this side
 * approximates both from this-tick-vs-last-tick worldY). */
void bridge_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

uint16_t bridge_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                     uint16_t camX, uint16_t camY,
                     int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

#endif
