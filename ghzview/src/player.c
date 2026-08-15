#include "md.h"
#include "player.h"
#include "collide.h"
#include "trig.h"
#include "pad.h"

#define SENSOR_DX   9        /* Sonic's floor sensors sit either side of centre */
#define BODY_HEIGHT 20       /* centre to feet */
#define STEP_LIMIT  14

void player_init(Player *p, int32_t x, int32_t y)
{
	p->x = x << 16;
	p->y = y << 16;
	p->groundVel = 0;
	p->velX = p->velY = 0;
	p->angle = 0;
	p->onGround = 0;
	p->direction = 0;
	p->controlLock = 0;
	p->skidding = 0;
}

/* Player_HandleGroundMovement, minus the states this port does not have yet:
 * no rolling, no super, no inverted gravity. */
static void ground_movement(Player *p, uint16_t pad)
{
	int32_t slope = ((int32_t)sin256(p->angle) << 13) >> 8;
	uint16_t left = pad & PAD_LEFT;
	uint16_t right = pad & PAD_RIGHT;

	if (p->controlLock > 0) {
		p->controlLock--;
		p->groundVel += slope;
		return;
	}

	if (left) {
		if (p->groundVel > -PHYS_TOP_SPEED) {
			if (p->groundVel <= 0) {
				p->groundVel -= PHYS_ACCELERATION;
			} else {
				if (p->groundVel > 0x40000) {
					p->direction = 0;
					p->skidding = 24;
				}
				if (p->groundVel < PHYS_SKID_SPEED)
					p->groundVel = -PHYS_SKID_SPEED;
				else
					p->groundVel -= PHYS_SKID_SPEED;
			}
		}
		if (p->groundVel <= 0 && p->skidding < 1) p->direction = 1;
	}

	if (right) {
		if (p->groundVel < PHYS_TOP_SPEED) {
			if (p->groundVel >= 0) {
				p->groundVel += PHYS_ACCELERATION;
			} else {
				if (p->groundVel < -0x40000) {
					p->direction = 1;
					p->skidding = 24;
				}
				if (p->groundVel > -PHYS_SKID_SPEED)
					p->groundVel = PHYS_SKID_SPEED;
				else
					p->groundVel += PHYS_SKID_SPEED;
			}
		}
		if (p->groundVel >= 0 && p->skidding < 1) p->direction = 0;
	}

	if (left || right) {
		p->groundVel += slope;

		/* slipping back down a steep slope when too slow */
		if (right && !left) {
			if (p->angle > 0xC0 && p->angle < 0xE4
			    && p->groundVel > -0x20000 && p->groundVel < 0x28000)
				p->controlLock = 30;
		} else if (left) {
			if (p->angle > 0x1C && p->angle < 0x40
			    && p->groundVel > -0x28000 && p->groundVel < 0x20000)
				p->controlLock = 30;
		}
	} else {
		if (p->groundVel <= 0) {
			p->groundVel += PHYS_DECELERATION;
			if (p->groundVel > 0) p->groundVel = 0;
		} else {
			p->groundVel -= PHYS_DECELERATION;
			if (p->groundVel < 0) p->groundVel = 0;
		}

		if (p->groundVel > 0x2000 || p->groundVel < -0x2000)
			p->groundVel += slope;

		if (p->angle > 0xC0 && p->angle < 0xE4) {
			if (p->groundVel < 0x10000 && p->groundVel > -0x10000)
				p->controlLock = 30;
		}
		if (p->angle > 0x1C && p->angle < 0x40) {
			if (p->groundVel < 0x10000 && p->groundVel > -0x10000)
				p->controlLock = 30;
		}
	}
}

/* Two floor sensors either side of centre; the higher surface wins. */
static int16_t find_ground(Player *p, uint8_t *angleOut)
{
	/* RSDK places the floor sensor at the feet and derives the entity
	 * position from the surface it finds. The routine itself scans a block
	 * above and below, so it still catches a rising slope. */
	int16_t px = (int16_t)(p->x >> 16);
	int16_t feet = (int16_t)(p->y >> 16) + BODY_HEIGHT;
	int16_t a = collide_floor(px - SENSOR_DX, feet, 0);
	int16_t b = collide_floor(px + SENSOR_DX, feet, 0);
	int16_t g = COLL_NONE;

	if (a != COLL_NONE && b != COLL_NONE) g = (a < b) ? a : b;
	else if (a != COLL_NONE)              g = a;
	else if (b != COLL_NONE)              g = b;

	if (g != COLL_NONE) *angleOut = collide_angle(px, g + 1);
	return g;
}

void player_update(Player *p, uint16_t pad)
{
	int16_t ground;
	uint8_t angle = 0;

	if (p->skidding > 0) p->skidding--;

	if (p->onGround) {
		ground_movement(p, pad);

		if ((pad & PAD_A) || (pad & PAD_B) || (pad & PAD_C)) {
			p->velX = (p->groundVel * cos256(p->angle)) >> 8;
			p->velY = (p->groundVel * sin256(p->angle)) >> 8;
			p->velX += (PHYS_JUMP * sin256(p->angle)) >> 8;
			p->velY -= (PHYS_JUMP * cos256(p->angle)) >> 8;
			p->onGround = 0;
			p->angle = 0;
		} else {
			p->velX = (p->groundVel * cos256(p->angle)) >> 8;
			p->velY = (p->groundVel * sin256(p->angle)) >> 8;
		}
	} else {
		/* air control, then gravity */
		if (pad & PAD_LEFT)  p->velX -= 0x1800;
		if (pad & PAD_RIGHT) p->velX += 0x1800;
		if (p->velX > PHYS_TOP_SPEED)  p->velX = PHYS_TOP_SPEED;
		if (p->velX < -PHYS_TOP_SPEED) p->velX = -PHYS_TOP_SPEED;
		p->velY += PHYS_GRAVITY;
	}

	p->x += p->velX;
	p->y += p->velY;
	if (p->x < 0) p->x = 0;

	ground = find_ground(p, &angle);
	if (ground != COLL_NONE) {
		int16_t feet = (int16_t)(p->y >> 16) + BODY_HEIGHT;
		int16_t d = ground - feet;

		if (p->onGround) {
			if (d >= -STEP_LIMIT && d <= STEP_LIMIT) {
				p->y = (int32_t)(ground - BODY_HEIGHT) << 16;
				p->angle = angle;
			} else if (d > STEP_LIMIT) {
				p->onGround = 0;         /* ran off the edge */
			}
		} else if (p->velY >= 0 && d <= 0 && d >= -STEP_LIMIT) {
			p->y = (int32_t)(ground - BODY_HEIGHT) << 16;
			p->angle = angle;
			p->onGround = 1;
			p->groundVel = p->velX;      /* landing keeps horizontal speed */
			p->velY = 0;
		}
	} else if (p->onGround) {
		p->onGround = 0;
	}
}
