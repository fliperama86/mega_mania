#ifndef PLAYER_H
#define PLAYER_H

/* Sonic, ported from the RSDKv5 decompilation: constants from Player.h's
 * sonicPhysicsTable, movement from Player_HandleGroundMovement and
 * Player_HandleAirFriction, animation from Player_HandleGroundAnimation and
 * Player_State_Air.
 *
 * Positions and velocities are 16.16 fixed point, angles are RSDK's 0-255
 * over a full turn with 0 flat.
 *
 * The collision box is not a constant here: RSDK reads it from the current
 * animation frame every update, so curling into a jump shrinks it on its own. */

#include "path.h"
#include "sonic_anim.h"

/* Copied from md_src/pad.h's bit layout, not a new convention: the 68000
 * forwards its pad_read() byte to the SH2 verbatim through the tick+pad
 * comm word (see sh_src/comm.h), so these five bits have to match exactly.
 * Only the bits player.c actually reads are copied; UP/DOWN/START are not. */
#define PAD_LEFT   0x04
#define PAD_RIGHT  0x08
#define PAD_B      0x10
#define PAD_C      0x20
#define PAD_A      0x40

typedef struct {
	PathEntity e;          /* position, velocity, angle, collision mode */
	Animator animator;
	uint8_t direction;     /* 0 right, 1 left */
	uint8_t applyJumpCap;
	int32_t camAdjustY;    /* Player_LateUpdate's self->camera->adjustY
	                         * (Player.c ~305-310): PHYS_JUMP_OFFSET while
	                         * grounded and curled up in the jump anim, 0
	                         * otherwise; holds its last value while airborne,
	                         * same as the original only writing it inside
	                         * "if (self->onGround)". s_main.c forwards it
	                         * into camera_update, so player.c never has to
	                         * know Camera exists either. */
	int16_t controlLock;
	int16_t skidding;
	/* Animation thresholds carry hysteresis, so they are state, not constants */
	int32_t minJogVelocity, minRunVelocity, minDashVelocity;
} Player;

/* Sonic, not underwater, no shoes: sonicPhysicsTable entries 0-7 */
#define PHYS_TOP_SPEED    0x60000
#define PHYS_ACCELERATION 0xC00
#define PHYS_DECELERATION 0xC00
#define PHYS_AIR_ACCEL    0x1800
#define PHYS_SKID_SPEED   0x8000
#define PHYS_JUMP         0x68000
#define PHYS_JUMP_CAP     (-0x40000)
#define PHYS_GRAVITY      0x3800
/* Curling up shortens the box by five pixels; this keeps the feet planted */
#define PHYS_JUMP_OFFSET  0x50000

void player_init(Player *p, int32_t x, int32_t y);
void player_update(Player *p, uint16_t pad);

/* Zone_HandlePlayerBounds (Zone.c ~558-640): clamp the player to the act's
 * world edges so walking off the map finds a floor instead of falling
 * forever. boundL/boundR/boundB are 16.16 fixed point, matching e.x/e.y's
 * scale (the original's Zone->playerBounds* are TO_FIXED too, and are set
 * from the camera bounds at stage load, per Zone.c ~227-230 - the same
 * numbers, so callers should pass CAM_BOUND_L/CAM_BOUND_B/the layer width
 * rather than a second copy of them). */
void player_apply_world_bounds(Player *p, int32_t boundL, int32_t boundR,
                                int32_t boundB);

#endif
