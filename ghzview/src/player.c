#include "md.h"
#include "player.h"
#include "trig.h"
#include "pad.h"

static int32_t abs32(int32_t v) { return v < 0 ? -v : v; }

void player_init(Player *p, int32_t x, int32_t y)
{
	p->e.x = x << 16;
	p->e.y = y << 16;
	p->e.groundVel = 0;
	p->e.velX = p->e.velY = 0;
	p->e.angle = 0;
	p->e.collisionMode = CMODE_FLOOR;
	p->e.onGround = 0;
	p->direction = 0;
	p->applyJumpCap = 0;
	p->controlLock = 0;
	p->skidding = 0;
	p->minJogVelocity = 0x40000;
	p->minRunVelocity = 0x60000;
	p->minDashVelocity = 0xC0000;
	sonic_set_anim(&p->animator, ANI_IDLE, 1, 0);
}

/* Player_HandleGroundMovement, minus the states this port does not have yet:
 * no rolling, no super, no inverted gravity. */
static void ground_movement(Player *p, uint16_t pad)
{
	int32_t slope = ((int32_t)sin256(p->e.angle) << 13) >> 8;
	uint16_t left = pad & PAD_LEFT;
	uint16_t right = pad & PAD_RIGHT;

	if (p->controlLock > 0) {
		p->controlLock--;
		p->e.groundVel += slope;
		return;
	}

	if (left) {
		if (p->e.groundVel > -PHYS_TOP_SPEED) {
			if (p->e.groundVel <= 0) {
				p->e.groundVel -= PHYS_ACCELERATION;
			} else {
				if (p->e.groundVel > 0x40000) {
					p->direction = 0;
					p->skidding = 24;
				}
				if (p->e.groundVel < PHYS_SKID_SPEED)
					p->e.groundVel = -PHYS_SKID_SPEED;
				else
					p->e.groundVel -= PHYS_SKID_SPEED;
			}
		}
		if (p->e.groundVel <= 0 && p->skidding < 1) p->direction = 1;
	}

	if (right) {
		if (p->e.groundVel < PHYS_TOP_SPEED) {
			if (p->e.groundVel >= 0) {
				p->e.groundVel += PHYS_ACCELERATION;
			} else {
				if (p->e.groundVel < -0x40000) {
					p->direction = 1;
					p->skidding = 24;
				}
				if (p->e.groundVel > -PHYS_SKID_SPEED)
					p->e.groundVel = PHYS_SKID_SPEED;
				else
					p->e.groundVel += PHYS_SKID_SPEED;
			}
		}
		if (p->e.groundVel >= 0 && p->skidding < 1) p->direction = 0;
	}

	if (left || right) {
		p->e.groundVel += slope;

		/* slipping back down a steep slope when too slow */
		if (right && !left) {
			if (p->e.angle > 0xC0 && p->e.angle < 0xE4
			    && p->e.groundVel > -0x20000 && p->e.groundVel < 0x28000)
				p->controlLock = 30;
		} else if (left) {
			if (p->e.angle > 0x1C && p->e.angle < 0x40
			    && p->e.groundVel > -0x28000 && p->e.groundVel < 0x20000)
				p->controlLock = 30;
		}
	} else {
		if (p->e.groundVel <= 0) {
			p->e.groundVel += PHYS_DECELERATION;
			if (p->e.groundVel > 0) p->e.groundVel = 0;
		} else {
			p->e.groundVel -= PHYS_DECELERATION;
			if (p->e.groundVel < 0) p->e.groundVel = 0;
		}

		if (p->e.groundVel > 0x2000 || p->e.groundVel < -0x2000)
			p->e.groundVel += slope;

		if (p->e.angle > 0xC0 && p->e.angle < 0xE4) {
			if (p->e.groundVel < 0x10000 && p->e.groundVel > -0x10000)
				p->controlLock = 30;
		}
		if (p->e.angle > 0x1C && p->e.angle < 0x40) {
			if (p->e.groundVel < 0x10000 && p->e.groundVel > -0x10000)
				p->controlLock = 30;
		}
	}
}

/* Player_HandleGroundAnimation. The thresholds move as they are crossed, which
 * is what stops the walk and jog cycles flickering into each other at the
 * boundary. Balancing, boredom and pushing are not ported. */
static void ground_animation(Player *p)
{
	int32_t velocity;

	if (p->skidding > 0) {
		if (p->animator.anim != ANI_SKID) {
			if (p->animator.anim == ANI_SKID_TURN) {
				if (p->animator.frameID
				    == (uint16_t)(sonic_anims[ANI_SKID_TURN].count - 1)) {
					p->direction ^= 1;
					p->skidding = 1;
					sonic_set_anim(&p->animator, ANI_WALK, 0, 0);
				}
			} else {
				sonic_set_anim(&p->animator, ANI_SKID, 0, 0);
				if (abs32(p->e.groundVel) >= 0x60000)
					p->animator.speed =
						abs32(p->e.groundVel) >= 0xA0000 ? 64 : 144;
				else
					p->skidding -= 8;
			}
		} else {
			int16_t spd = p->animator.speed;
			if (p->direction) {
				if (p->e.groundVel >= 0)
					sonic_set_anim(&p->animator, ANI_SKID_TURN, 0, 0);
			} else if (p->e.groundVel <= 0) {
				sonic_set_anim(&p->animator, ANI_SKID_TURN, 0, 0);
			}
			p->animator.speed = spd;
		}
		p->skidding--;
		return;
	}

	if (p->e.groundVel || (p->e.angle >= 0x20 && p->e.angle <= 0xE0)) {
		velocity = abs32(p->e.groundVel);

		if (velocity < p->minJogVelocity) {
			if (p->animator.anim == ANI_JOG) {
				if (p->animator.frameID == 9)
					sonic_set_anim(&p->animator, ANI_WALK, 0, 9);
			} else if (p->animator.anim == ANI_AIR_WALK) {
				sonic_set_anim(&p->animator, ANI_WALK, 0, p->animator.frameID);
			} else {
				sonic_set_anim(&p->animator, ANI_WALK, 0, 0);
			}
			p->animator.speed = (velocity >> 12) + 48;
			p->minJogVelocity = 0x40000;
		} else if (velocity < p->minRunVelocity) {
			if (p->animator.anim != ANI_WALK || p->animator.frameID == 3)
				sonic_set_anim(&p->animator, ANI_JOG, 0, 0);
			p->animator.speed = (velocity >> 12) + 0x40;
			p->minJogVelocity = 0x38000;
			p->minRunVelocity = 0x60000;
		} else if (velocity < p->minDashVelocity) {
			sonic_set_anim(&p->animator, ANI_RUN, 0,
			               (p->animator.anim == ANI_DASH
			                || p->animator.anim == ANI_RUN) ? 0 : 1);
			p->animator.speed = (velocity >> 12) + 0x60;
			if (p->animator.speed > 0x200) p->animator.speed = 0x200;
			p->minRunVelocity = 0x58000;
			p->minDashVelocity = 0xC0000;
		} else {
			sonic_set_anim(&p->animator, ANI_DASH, 0,
			               (p->animator.anim == ANI_DASH
			                || p->animator.anim == ANI_RUN) ? 0 : 1);
			p->minDashVelocity = 0xB8000;
		}
	} else {
		p->minJogVelocity = 0x40000;
		p->minRunVelocity = 0x60000;
		p->minDashVelocity = 0xC0000;
		sonic_set_anim(&p->animator, ANI_IDLE, 0, 0);
	}
}

/* Player_Action_Jump */
static void action_jump(Player *p)
{
	int32_t force = PHYS_GRAVITY + PHYS_JUMP;
	int32_t speed;

	p->controlLock = 0;
	p->e.onGround = 0;
	if (p->e.collisionMode == CMODE_FLOOR) p->e.y += PHYS_JUMP_OFFSET;

	p->e.velX = (p->e.groundVel * cos256(p->e.angle)
	             + force * sin256(p->e.angle)) >> 8;
	p->e.velY = (p->e.groundVel * sin256(p->e.angle)
	             - force * cos256(p->e.angle)) >> 8;

	sonic_set_anim(&p->animator, ANI_JUMP, 0, 0);
	speed = ((abs32(p->e.groundVel) * 0xF0) / PHYS_TOP_SPEED) + 0x30;
	p->animator.speed = speed > 0xF0 ? 0xF0 : (int16_t)speed;

	p->e.angle = 0;
	p->e.collisionMode = CMODE_FLOOR;
	p->skidding = 0;
	p->applyJumpCap = 1;
}

/* Player_HandleAirFriction, then the animation switch from Player_State_Air */
static void air_state(Player *p, uint16_t pad)
{
	if (p->e.velY > -0x40000 && p->e.velY < 0)
		p->e.velX -= p->e.velX >> 5;

	if (pad & PAD_LEFT) {
		if (p->e.velX > -PHYS_TOP_SPEED) p->e.velX -= PHYS_AIR_ACCEL;
		p->direction = 1;
	}
	if (pad & PAD_RIGHT) {
		if (p->e.velX < PHYS_TOP_SPEED) p->e.velX += PHYS_AIR_ACCEL;
		p->direction = 0;
	}

	p->e.velY += PHYS_GRAVITY;

	/* Releasing jump early caps the rise, which is the short hop */
	if (p->e.velY < PHYS_JUMP_CAP && p->animator.anim == ANI_JUMP
	    && !(pad & (PAD_A | PAD_B | PAD_C)) && p->applyJumpCap) {
		p->e.velX -= p->e.velX >> 5;
		p->e.velY = PHYS_JUMP_CAP;
	}

	p->e.collisionMode = CMODE_FLOOR;

	switch (p->animator.anim) {
	case ANI_IDLE:
	case ANI_WALK:
		if (p->animator.speed < 64) p->animator.speed = 64;
		sonic_set_anim(&p->animator, ANI_AIR_WALK, 0, p->animator.frameID);
		break;
	case ANI_LOOK_UP:
	case ANI_CROUCH:
	case ANI_SKID_TURN:
	case ANI_JOG:
		sonic_set_anim(&p->animator, ANI_AIR_WALK, 0, 0);
		break;
	case ANI_SKID:
		if (p->skidding <= 0)
			sonic_set_anim(&p->animator, ANI_AIR_WALK, 0, p->animator.frameID);
		else
			p->skidding--;
		break;
	default:
		break;
	}
}

void player_update(Player *p, uint16_t pad)
{
	static uint16_t prevPad;
	uint16_t jumpPress = (pad & ~prevPad) & (PAD_A | PAD_B | PAD_C);
	const SonicFrame *f;

	prevPad = pad;

	if (p->e.onGround) {
		ground_movement(p, pad);
		ground_animation(p);
		if (jumpPress) action_jump(p);
	} else {
		air_state(p, pad);
	}

	/* RSDK takes the collision box from the animation frame every update, so
	 * the curled jump box arrives with the jump animation rather than being
	 * switched by hand. */
	f = sonic_frame(&p->animator);
	p->e.outer.left = f->outerLeft;
	p->e.outer.top = f->outerTop;
	p->e.outer.right = f->outerRight;
	p->e.outer.bottom = f->outerBottom;
	p->e.inner.left = f->innerLeft;
	p->e.inner.top = f->innerTop;
	p->e.inner.right = f->innerRight;
	p->e.inner.bottom = f->innerBottom;

	if (p->e.onGround) path_grip(&p->e);
	else path_air(&p->e);

	if (p->e.onGround) p->applyJumpCap = 0;

	sonic_process_anim(&p->animator);
}
