#ifndef PLAYER_H
#define PLAYER_H

/* Sonic's ground physics, ported from the RSDKv5 decompilation:
 * constants from Player.h's sonicPhysicsTable, movement from
 * Player_HandleGroundMovement in Player.c.
 *
 * Positions and velocities are 16.16 fixed point, angles are RSDK's 0-255
 * over a full turn with 0 flat. */

typedef struct {
	int32_t x, y;          /* world position, 16.16 */
	int32_t groundVel;
	int32_t velX, velY;
	uint8_t angle;
	uint8_t onGround;
	uint8_t direction;     /* 0 right, 1 left */
	int16_t controlLock;
	int16_t skidding;
} Player;

/* Sonic, not underwater, no shoes: sonicPhysicsTable entries 0-7 */
#define PHYS_TOP_SPEED    0x60000
#define PHYS_ACCELERATION 0xC00
#define PHYS_DECELERATION 0xC00
#define PHYS_SKID_SPEED   0x8000
#define PHYS_JUMP         0x68000
#define PHYS_JUMP_CAP     -0x40000
#define PHYS_GRAVITY      0x3800

void player_init(Player *p, int32_t x, int32_t y);
void player_update(Player *p, uint16_t pad);

#endif
