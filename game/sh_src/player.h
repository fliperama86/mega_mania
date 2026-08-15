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
	uint8_t justJumped;    /* one-frame pulse from action_jump; s_main.c reads
	                         * it to open the camera's vertical dead zone, so
	                         * player.c never has to know Camera exists */
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

#endif
