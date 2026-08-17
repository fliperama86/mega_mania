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
#define PAD_DOWN   0x02
#define PAD_LEFT   0x04
#define PAD_RIGHT  0x08
#define PAD_B      0x10
#define PAD_C      0x20
#define PAD_A      0x40

/* Player.state, cut down from RSDK's function-pointer state machine to the
 * handful of Player_State_* this port has. PSTATE_NORMAL covers both
 * Player_State_Ground and Player_State_Air: the port has always picked
 * between those two off e.onGround alone (see player.c's player_update), so
 * they do not need separate state values the way Roll/TubeRoll/TubeAirRoll
 * do (those three are each a single Player_State_* function, and moving
 * between "grounded roll" and "airborne roll" is *not* the same
 * Ground<->Air split -- Player_State_Roll going airborne lands back in
 * plain Player_State_Air, but Player_State_TubeRoll going airborne lands in
 * the distinct Player_State_TubeAirRoll, which is why tube needs its own
 * pair). */
#define PSTATE_NORMAL    0  /* Player_State_Ground / Player_State_Air */
#define PSTATE_ROLL      1  /* Player_State_Roll */
#define PSTATE_TUBE_ROLL 2  /* Player_State_TubeRoll */
#define PSTATE_TUBE_AIR  3  /* Player_State_TubeAirRoll */

typedef struct {
	PathEntity e;          /* position, velocity, angle, collision mode */
	Animator animator;
	uint8_t direction;     /* 0 right, 1 left */
	uint8_t applyJumpCap;
	uint8_t state;          /* PSTATE_*, see above */
	int32_t camAdjustY;    /* Player_LateUpdate's self->camera->adjustY
	                         * (Player.c ~305-310): PHYS_JUMP_OFFSET while
	                         * grounded and curled up in the jump anim, 0
	                         * otherwise; holds its last value while airborne,
	                         * same as the original only writing it inside
	                         * "if (self->onGround)". s_main.c forwards it
	                         * into camera_update, so player.c never has to
	                         * know Camera exists either. Rolling also plays
	                         * ANI_JUMP (Player_Action_Roll), so this already
	                         * engages for rolling with no extra code. */
	int16_t controlLock;
	int16_t skidding;
	/* self->rotation (Player.c, e.g. Player_HandleGroundRotation/
	 * Player_HandleAirRotation at Player.c:3207-3254): 0-511 over a full
	 * turn, RSDK's finer rotation scale for sprite display (double
	 * player.h's 0-255 angle unit). Computed every frame exactly as the
	 * original does regardless of which animation is playing -- see
	 * player.c's ground_rotation/air_gravity -- because Player_State_Ground/
	 * Roll/TubeRoll/TubeAirRoll all call Player_HandleGroundRotation
	 * unconditionally too. Only comm.c's snap to dispRot (comm.h's COMM6
	 * repack) and md_src/sonic.c's per-frame rotation-class table decide
	 * whether a given animation's *display* ever uses this value: ANI_JUMP/
	 * ANI_SKID/ANI_SKID_TURN are baked ROTSTYLE_NONE in the original sprite
	 * sheet, so they compute rotation here but never draw rotated. */
	uint16_t rotation;
	/* Animation thresholds carry hysteresis, so they are state, not constants */
	int32_t minJogVelocity, minRunVelocity, minDashVelocity;
	/* PlaneSwitch_CheckCollisions' other write, alongside e.collisionPlane
	 * (PlaneSwitch.c:94-109): 0 low, 1 high, matching Zone->playerDrawGroup[0]
	 * (low) / [1] (high) -- see sh_src/plane_switch.c. NOT YET carried over
	 * the comm protocol to md_src/sonic.c's sprite priority: see comm.h's
	 * packed-anim-word comment for why that leg is currently blocked on a
	 * bit-budget question this port stops short of deciding unilaterally. */
	uint8_t drawGroupHigh;
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

/* rollingFriction: not sonicPhysicsTable[5] (indices 0-7, the non-underwater/
 * non-super/no-speed-shoes row) *within Player.c* -- the audit that flagged
 * this port's missing rolling physics could not find the table literal by
 * grepping Player.c alone. It is one file over: Player.h:161 (MANIA_USE_PLUS)
 * and :282 (else), TABLE(int32 sonicPhysicsTable[64], { 0x60000, 0xC00,
 * 0x1800, 0x600, 0x8000, 0x600, 0x68000, -0x40000, ... }), read through
 * tablePtr[tableID+5] at Player.c:2792. Index 5 of that first row is 0x600 --
 * also exactly the classic-engine rollingFriction value, so this is not a
 * guess standing in for a missing number, just corroborated by one. */
#define PHYS_ROLL_FRICTION   0x600
/* rollingDeceleration: fixed, not table-driven (Player.c:2795) */
#define PHYS_ROLL_DECEL      0x2000
/* Player_HandleRollDeceleration's +/- speed cap (Player.c:3486, 3497) */
#define PHYS_ROLL_SPEED_CAP  0x120000
/* The relaunch-if-too-slow speed both Player_HandleRollDeceleration's tube
 * branch (Player.c:3510/3512) and ForceSpin_SetPlayerState (ForceSpin.c:
 * 115/117) apply when entering/holding the tube below 0x10000 groundVel. */
#define PHYS_TUBE_LAUNCH_SPEED 0x40000

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
