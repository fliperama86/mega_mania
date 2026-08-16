#ifndef CAMERA_H
#define CAMERA_H

#include "bounds.h"

/* Camera, ported from the RSDKv5 Sonic Mania decompilation's
 * Camera_State_FollowXY, plus the constants set in Camera_Create and
 * Camera_StageLoad.
 *
 * Position is 16.16 fixed point in world pixels, the same scale PathEntity
 * uses. It names the camera's centre, not the screen's top-left corner:
 * turning that into a screen corner is s_main.c's job, not this file's.
 *
 * boundsT/boundsB are camera-local vertical bounds in px (Camera_HandleVBounds,
 * Camera.c:205-257): eased every frame toward the ZoneBounds values bounds.c
 * maintains, rather than snapping straight to them, which is what lets the
 * visible area grow and shrink smoothly as BoundsMarker entities come in and
 * out of range. s_main.c still does the final clamp of the screen corner
 * against these two (Camera_SetCameraBounds, Camera.c:103-119) and feeds
 * the clamped result back in next frame (see camera_apply_vbounds below) -
 * that half of the original job is still this file's, not s_main.c's.
 *
 * There is no boundsL/boundsR here: Camera_HandleHBounds (Camera.c:153-204)
 * is not ported, because nothing in GHZ Act 1 ever rewrites the horizontal
 * bounds after stage load (BoundsMarker only ever writes T/B, and
 * GHZSetup_HandleActTransition's left-bound rewrite, GHZSetup.c:148, is
 * gated to Act 2 only at GHZSetup.c:57). Camera-local L/R would therefore
 * just equal the ZoneBounds constants for the whole act, so s_main.c reads
 * those directly instead of this file tracking an always-equal copy. */

typedef struct {
	int32_t x, y;    /* 16.16, world position the camera is centred on */
	int32_t offsetY; /* vertical dead zone half-height; held open while
	                  * airborne, decays back to 0 while grounded */
	int32_t boundsT, boundsB; /* px, camera-local, eased toward ZoneBounds by
	                           * camera_apply_vbounds; seeded at init (see
	                           * camera_init) */
	int32_t velocityY; /* 16.16, this update's y position delta
	                    * (Camera_LateUpdate:23-24). camera_apply_vbounds
	                    * reads this still holding the PREVIOUS call's value,
	                    * since it always runs before camera_update
	                    * recomputes it (Camera_State_FollowXY:322-323 runs
	                    * HandleVBounds before the follow math). */
} Camera;

/* Camera_Create (Camera.c:57-88): starts centred on the target, dead zone
 * closed, boundsT/boundsB seeded straight from z. That is two things in the
 * original collapsed into one here: Camera_Create:73-77 seeds them
 * unequally from Zone's camera bounds, and then BoundsMarker_Create's
 * setPos pass (BoundsMarker.c:88-96) immediately overwrites them again from
 * the same Zone fields, now updated by that marker's own ApplyBounds call -
 * since z already holds the post-marker-pass values (bounds_init calls
 * bounds_apply_markers itself before returning), copying z here does the
 * work of both. */
void camera_init(Camera *c, int32_t x, int32_t y, const ZoneBounds *z);

/* Camera_HandleVBounds (Camera.c:205-257), called at the top of
 * Camera_State_FollowXY (Camera.c:322-323), before the follow math below
 * touches c->y: eases boundsT/boundsB toward z's current values by at most
 * 2px/frame (Camera_Create:70's boundsOffset.y), or by this frame's
 * velocity when the bound is expanding and the screen edge is already
 * pressed against it. prevScreenTop is last frame's clamped screen-space
 * top edge in px (Camera_SetCameraBounds's screen->position.y, Camera.c:
 * 103-119 - s_main.c's job, since this file never sees the screen corner).
 * c->velocityY is still holding last frame's delta when this runs, matching
 * the original reading self->velocity.y before Camera_LateUpdate
 * recomputes it three lines later. */
void camera_apply_vbounds(Camera *c, const ZoneBounds *z, int32_t prevScreenTop);

/* Camera_State_FollowXY, then Camera_LateUpdate's offset.y decay, run in
 * that order every frame (the decay applies to next frame's follow, since
 * FollowXY runs first in the original too). Call camera_apply_vbounds
 * before this, matching Camera_State_FollowXY:322-323 running HandleVBounds
 * before the same follow math. targetX/targetY are the followed entity's
 * position, 16.16 world pixels.
 *
 * adjustY is Player_LateUpdate's self->camera->adjustY (Player.c ~305-310):
 * the player's jumpOffset while grounded and curled up in the jump anim, 0
 * otherwise. It is subtracted from targetY before the follow math, so a
 * positive value settles the camera above the player. player.c computes it
 * (see Player.camAdjustY) and s_main.c passes it through here, so player.c
 * never has to know Camera exists.
 *
 * airborne is the original's disableYOffset, folded into a per-frame
 * parameter: Player_Gravity_True (Player.c ~6094, called from
 * Player_HandleAirMovement every airborne frame) holds the vertical dead
 * zone fully open and suppresses its decay, Player_Gravity_False (~6103,
 * from the ground states) lets it decay again. s_main.c passes
 * !onGround, which in this port is exactly when those run.
 *
 * Also updates velocityY from this call's actual y movement
 * (Camera_LateUpdate:18-24: lastPos captured before the state runs,
 * velocity computed after), for the next frame's camera_apply_vbounds call
 * to read. */
void camera_update(Camera *c, int32_t targetX, int32_t targetY, int32_t adjustY,
                   int32_t airborne);

#endif
