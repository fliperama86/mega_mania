#ifndef CAMERA_H
#define CAMERA_H

/* Camera, ported from the RSDKv5 Sonic Mania decompilation's
 * Camera_State_FollowXY, plus the constants set in Camera_Create and
 * Camera_StageLoad.
 *
 * Position is 16.16 fixed point in world pixels, the same scale PathEntity
 * uses. It names the camera's centre, not the screen's top-left corner:
 * turning that into a screen corner, and clamping it to the map, is
 * s_main.c's job, not this file's. */

typedef struct {
	int32_t x, y;    /* 16.16, world position the camera is centred on */
	int32_t offsetY; /* vertical dead zone half-height; opens on a jump, decays back */
} Camera;

/* Camera_Create: starts centred on the target, dead zone closed. */
void camera_init(Camera *c, int32_t x, int32_t y);

/* Camera_State_FollowXY, then Camera_LateUpdate's offset.y decay, run in
 * that order every frame (the decay applies to next frame's follow, since
 * FollowXY runs first in the original too). targetX/targetY are the
 * followed entity's position, 16.16 world pixels.
 *
 * adjustY is Player_LateUpdate's self->camera->adjustY (Player.c ~305-310):
 * the player's jumpOffset while grounded and curled up in the jump anim, 0
 * otherwise. It is subtracted from targetY before the follow math, so a
 * positive value settles the camera above the player. player.c computes it
 * (see Player.camAdjustY) and s_main.c passes it through here, so player.c
 * never has to know Camera exists. */
void camera_update(Camera *c, int32_t targetX, int32_t targetY, int32_t adjustY);

/* Player_Action_Jump: opens the vertical dead zone so the jump impulse
 * doesn't yank the screen. Called from s_main.c when the player just jumped,
 * keeping player.c from needing to know about Camera at all. */
void camera_open_y_offset(Camera *c);

#endif
