#include <stdint.h>
#include "camera.h"

/* Camera_Create: horizontal dead zone half-width, constant for the life of
 * the camera. */
#define OFFSET_X         0x80000   /* TO_FIXED(8) */

/* Camera_StageLoad: the most the camera may move in one frame. The real
 * object ramps these up from 0 via Camera_StaticUpdate; that ramp isn't
 * ported, so the camera just uses the settled values from the first frame. */
#define CENTER_BOUNDS_X  0x100000  /* TO_FIXED(16) */
#define CENTER_BOUNDS_Y  0x180000  /* TO_FIXED(24) */

/* Player_Action_Jump: how far the vertical dead zone opens on a jump. */
#define JUMP_OFFSET_Y    0x200000

void camera_init(Camera *c, int32_t x, int32_t y)
{
	c->x = x;
	c->y = y;
	c->offsetY = 0;
}

void camera_open_y_offset(Camera *c)
{
	c->offsetY = JUMP_OFFSET_Y;
}

/* Camera_State_FollowXY. Dropped from the original: the multi-screen bounds
 * objects (Camera_HandleHBounds/VBounds, all Zone->cameraBounds*), screen
 * shake, lerp, and the targetMoveVel lookahead (it nets to zero on the
 * target's position there regardless of branch, so it never mattered here).
 * adjustY comes from the caller now (see camera.h); it is the player's
 * jumpOffset while grounded in the curled-up jump animation, 0 otherwise. */
void camera_update(Camera *c, int32_t targetX, int32_t targetY, int32_t adjustY)
{
	int32_t adjust;

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

	/* Camera_LateUpdate's offset.y decay. disableYOffset is ignored: nothing
	 * else in this port ever sets or clears it, so honoring it would just
	 * mean the dead zone never closes again after the first jump. */
	c->offsetY = c->offsetY - (c->offsetY >> 3);
	if (c->offsetY < 0) c->offsetY = 0;
}
