#include <stdint.h>
#include "camera.h"

/* Camera_Create: horizontal dead zone half-width, constant for the life of
 * the camera. */
#define OFFSET_X         0x80000   /* TO_FIXED(8) */

/* Camera_StageLoad: the most the camera may move in one frame. Camera's
 * Camera_StaticUpdate ramp only matters after something drops these below
 * the caps (TitleCard.c ~511 sets TO_FIXED(2) and Zone.c ~447 sets 8/4,
 * both during act transitions this port does not have); nothing here ever
 * lowers them, so the settled values are right from the first frame and
 * the ramp would have nothing to do. */
#define CENTER_BOUNDS_X  0x100000  /* TO_FIXED(16) */
#define CENTER_BOUNDS_Y  0x180000  /* TO_FIXED(24) */

/* Player_Gravity_True (Player.c ~6094, and the identical pair in
 * Player_Action_Jump ~3306): how far the vertical dead zone is held open
 * for as long as gravity applies. */
#define AIR_OFFSET_Y     0x200000

/* Camera_Create:70 (self->boundsOffset.y = 2): the most boundsT/boundsB may
 * ease per frame while not pressed against the corresponding screen edge. */
#define BOUNDS_OFFSET_Y  2

/* This port's only screen size (mars.h's SCREEN_HEIGHT; md_src/vdp.c ~53
 * has the PAL/NTSC-aware value this hardcodes past, same simplification
 * s_main.c's own 224 already made). Camera_HandleVBounds needs it to find
 * the screen's bottom edge from prevScreenTop. */
#define VBOUNDS_SCREEN_H 224

void camera_init(Camera *c, int32_t x, int32_t y, const ZoneBounds *z)
{
	c->x = x;
	c->y = y;
	c->offsetY = 0;
	c->velocityY = 0;

	/* Camera_Create:73-77 plus BoundsMarker_Create's setPos pass
	 * (BoundsMarker.c:88-96): see this function's declaration in camera.h
	 * for why both collapse into copying z here. */
	c->boundsT = z->cameraBoundsT;
	c->boundsB = z->cameraBoundsB;
}

/* Camera_HandleVBounds (Camera.c:205-257), transcribed branch for branch.
 * FROM_FIXED is >>16, screen->size.y is VBOUNDS_SCREEN_H, self->boundsOffset.y
 * is BOUNDS_OFFSET_Y. Does not port the final Zone->playerBoundsT/B writeback
 * (Camera.c:255-256): in the original that lets a Camera update later in the
 * same frame overwrite what BoundsMarker_Update wrote earlier, but this port
 * gives bounds_apply_markers sole ownership of ZoneBounds' playerBoundsT/B
 * every frame (see s_main.c's loop order), so feeding the eased camera-local
 * bound back into it here would just be overwritten again next frame with no
 * frame in between where it was ever read - the self-check in the fix's
 * report traces this produces the same frame-1 numbers either way. */
void camera_apply_vbounds(Camera *c, const ZoneBounds *z, int32_t prevScreenTop)
{
	int32_t screenBottom = prevScreenTop + VBOUNDS_SCREEN_H;

	/* Top, contraction: Zone wants boundsT bigger, the visible area
	 * shrinking from above (Camera.c:211-216). */
	if (z->cameraBoundsT > c->boundsT) {
		if (prevScreenTop <= c->boundsT)
			c->boundsT = prevScreenTop + BOUNDS_OFFSET_Y;
		else
			c->boundsT = prevScreenTop;
	}

	/* Top, expansion: Zone wants boundsT smaller, the visible area
	 * opening back up (Camera.c:218-231). */
	if (z->cameraBoundsT < c->boundsT) {
		if (prevScreenTop <= c->boundsT) {
			c->boundsT = c->boundsT - BOUNDS_OFFSET_Y;

			if (c->velocityY < 0) {
				c->boundsT += c->velocityY >> 16;
				if (c->boundsT < z->cameraBoundsT)
					c->boundsT = z->cameraBoundsT;
			}
		} else {
			c->boundsT = z->cameraBoundsT;
		}
	}

	/* Bottom, contraction: Zone wants boundsB smaller (Camera.c:233-238). */
	if (z->cameraBoundsB < c->boundsB) {
		if (screenBottom >= c->boundsB)
			c->boundsB -= BOUNDS_OFFSET_Y;
		else
			c->boundsB = screenBottom;
	}

	/* Bottom, expansion: Zone wants boundsB bigger (Camera.c:240-253). */
	if (z->cameraBoundsB > c->boundsB) {
		if (screenBottom >= c->boundsB) {
			c->boundsB += BOUNDS_OFFSET_Y;

			if (c->velocityY > 0) {
				c->boundsB += c->velocityY >> 16;
				if (c->boundsB > z->cameraBoundsB)
					c->boundsB = z->cameraBoundsB;
			}
		} else {
			c->boundsB = z->cameraBoundsB;
		}
	}
}

/* Camera_State_FollowXY, minus the call to Camera_HandleHBounds (camera.h's
 * header comment explains why) and the Camera_HandleVBounds call, which the
 * caller makes separately just before this (camera_apply_vbounds above, so
 * it can also feed camera_update's previous-call velocity, see below).
 * Also dropped: screen shake, lerp, and the targetMoveVel lookahead (it
 * nets to zero on the target's position there regardless of branch, so it
 * never mattered here). adjustY comes from the caller now (see camera.h);
 * it is the player's jumpOffset while grounded in the curled-up jump
 * animation, 0 otherwise. */
void camera_update(Camera *c, int32_t targetX, int32_t targetY, int32_t adjustY,
                   int32_t airborne)
{
	int32_t adjust;
	/* Camera_LateUpdate:18-19 (self->lastPos.y = self->position.y), captured
	 * before the follow math below can move c->y, so velocityY at the
	 * bottom of this function is this call's actual delta. */
	int32_t lastY = c->y;

	/* Player_Gravity_True (Player.c ~6094), run by Player_HandleAirMovement
	 * (~3255) on every airborne frame, before the camera state does: the
	 * dead zone is pinned fully open for the whole flight, so the camera
	 * leaves the player's Y alone until he crosses the 32-pixel window.
	 * The original's persistent disableYOffset flag collapses into this
	 * parameter because the player re-asserts it every single frame, from
	 * the air (Gravity_True) and ground (Gravity_False) sides both. */
	if (airborne) c->offsetY = AIR_OFFSET_Y;

	if (targetX <= c->x + OFFSET_X) {
		if (targetX < c->x - OFFSET_X) {
			int32_t pos = targetX + OFFSET_X - c->x;
			if (pos < -CENTER_BOUNDS_X) pos = -CENTER_BOUNDS_X;
			c->x += pos;
		}
	} else {
		int32_t pos = targetX - c->x - OFFSET_X;
		if (pos > CENTER_BOUNDS_X) pos = CENTER_BOUNDS_X;
		c->x += pos;
	}

	adjust = targetY - adjustY;
	if (adjust <= c->y + c->offsetY) {
		if (adjust < c->y - c->offsetY) {
			int32_t pos = targetY + c->offsetY - c->y - adjustY;
			if (pos < -CENTER_BOUNDS_Y) pos = -CENTER_BOUNDS_Y;
			c->y += pos;
		}
	} else {
		int32_t pos = adjust - c->y - c->offsetY;
		if (pos > CENTER_BOUNDS_Y) pos = CENTER_BOUNDS_Y;
		c->y += pos;
	}

	/* Camera_LateUpdate (Camera.c ~42): the offset only decays while
	 * disableYOffset is clear, which Player_Gravity_False (Player.c ~6103)
	 * makes mean "while grounded". Decaying it every frame instead is what
	 * glued the camera to the player mid-jump: the window closed within a
	 * few frames and the follow branch re-centred him. */
	if (!airborne) {
		c->offsetY = c->offsetY - (c->offsetY >> 3);
		if (c->offsetY < 0) c->offsetY = 0;
	}

	/* Camera_LateUpdate:23-24 (self->velocity.y = self->position.y -
	 * self->lastPos.y): next frame's camera_apply_vbounds call reads this
	 * while it still holds THIS call's delta, since that call always runs
	 * before the follow math above produces a new one. */
	c->velocityY = c->y - lastY;
}
