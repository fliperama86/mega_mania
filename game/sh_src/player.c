#include <stdint.h>
#include "player.h"
#include "trig.h"
#include "comm.h"

/* Filled in by assets_init() (assets.c); see sonic_anim.c, which reads
 * frame/anim data through the same pointer. */
extern const SonicAnim *g_sonic_anims;

static int32_t abs32(int32_t v) { return v < 0 ? -v : v; }

void player_init(Player *p, int32_t x, int32_t y)
{
	p->e.x = x << 16;
	p->e.y = y << 16;
	p->e.groundVel = 0;
	p->e.velX = p->e.velY = 0;
	p->e.angle = 0;
	p->e.collisionMode = CMODE_FLOOR;
	p->e.collisionPlane = 0;
	/* Already standing, not dropped in. The original hands control over with
	 * the player grounded: the title card plays over a settled camera, and
	 * the scene's spawn sits at the surface rather than above it. Starting
	 * airborne makes the first second a different situation entirely, since
	 * the camera pins its vertical dead zone open for the whole fall
	 * (Player_Gravity_True, see camera.c) and only then settles. */
	p->e.onGround = 1;
	p->direction = 0;
	p->applyJumpCap = 0;
	p->state = PSTATE_NORMAL;
	p->camAdjustY = 0;
	p->controlLock = 0;
	p->skidding = 0;
	p->rotation = 0;
	p->blinkTimer = 0;
	p->hidden = 0;
	p->respawnPending = 0;
	/* Zone->playerDrawGroup[0], the low group -- see player.h's field
	 * comment and sh_src/plane_switch.c. */
	p->drawGroupHigh = 0;
	p->minJogVelocity = 0x40000;
	p->minRunVelocity = 0x60000;
	p->minDashVelocity = 0xC0000;
	p->animationReserve = ANI_WALK;
	sonic_set_anim(&p->animator, ANI_IDLE, 1, 0);
}

/* Player_HandleGroundMovement, minus the states this port does not have yet:
 * no super, no inverted gravity. */
static void ground_movement(Player *p, uint16_t pad)
{
	int32_t slope = ((int32_t)sin256(p->e.angle) << 13) >> 8;
	uint16_t left = pad & PAD_LEFT;
	uint16_t right = pad & PAD_RIGHT;

	if (p->controlLock > 0) {
		p->controlLock--;
		p->e.groundVel += slope;
	} else {
		if (left) {
			if (p->e.groundVel > -PHYS_TOP_SPEED) {
				if (p->e.groundVel <= 0) {
					p->e.groundVel -= PHYS_ACCELERATION;
				} else {
					if (p->e.collisionMode == CMODE_FLOOR && p->e.groundVel > 0x40000) {
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
					if (p->e.collisionMode == CMODE_FLOOR && p->e.groundVel < -0x40000) {
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

			/* slipping back down a steep slope when too slow. Player.c:
			 * 3152-3163 nests this in "if (right) { if (!left) {...} } else
			 * if (left) {...}", so holding both keys runs neither branch --
			 * "else if (left)" is only reachable when right is false. */
			if (right && !left) {
				if (p->e.angle > 0xC0 && p->e.angle < 0xE4
				    && p->e.groundVel > -0x20000 && p->e.groundVel < 0x28000)
					p->controlLock = 30;
			} else if (left && !right) {
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

	/* Player_HandleGroundMovement's tail (Player.c:3197-3205): runs every
	 * call, including controlLock frames -- unlike every branch above, it
	 * sits outside the controlLock gate. invertGravity is always false in
	 * this port (no inverted-gravity zones ported) and collisionMode is
	 * always <= CMODE_RWALL (there is no fifth mode), so the original's
	 * "!invertGravity && collisionMode != CMODE_FLOOR && collisionMode <=
	 * CMODE_RWALL" reduces to just "not on the floor". */
	if (p->e.collisionMode != CMODE_FLOOR
	    && p->e.angle >= 0x40 && p->e.angle <= 0xC0
	    && abs32(p->e.groundVel) < 0x20000) {
		p->e.velX = (p->e.groundVel * cos256(p->e.angle)) >> 8;
		p->e.velY = (p->e.groundVel * sin256(p->e.angle)) >> 8;
		p->e.onGround = 0;
		p->e.angle = 0;
		p->e.collisionMode = CMODE_FLOOR;
	}
}

/* Player_HandleGroundRotation (Player.c:3207-3237). Feeds the display-only
 * rotation this port did not carry until now: comm.h's COMM6 repack snaps
 * this into a 3-bit dispRot the 68000 uses to pick one of 8 baked
 * orientations (md_src/sonic.c). Called every grounded frame regardless of
 * animation -- see player.h's rotation field comment for why ANI_JUMP/
 * ANI_SKID/ANI_SKID_TURN still run this despite never displaying it. */
static void ground_rotation(Player *p)
{
	if (p->e.angle <= 0x04 || p->e.angle >= 0xFC) {
		p->rotation = 0;
	} else {
		int32_t targetRotation = 0;
		int32_t rotate, shift;

		if (p->e.angle > 0x10 && p->e.angle < 0xE8)
			targetRotation = (int32_t)p->e.angle << 1;

		rotate = targetRotation - (int32_t)p->rotation;
		shift = (abs32(p->e.groundVel) <= 0x60000) + 1;

		if (abs32(rotate) >= abs32(rotate - 0x200)) {
			if (abs32(rotate - 0x200) < abs32(rotate + 0x200))
				p->rotation = (uint16_t)(p->rotation + ((rotate - 0x200) >> shift));
			else
				p->rotation = (uint16_t)(p->rotation + ((rotate + 0x200) >> shift));
		} else {
			if (abs32(rotate) < abs32(rotate + 0x200))
				p->rotation = (uint16_t)(p->rotation + (rotate >> shift));
			else
				p->rotation = (uint16_t)(p->rotation + ((rotate + 0x200) >> shift));
		}

		p->rotation &= 0x1FF;
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
				    == (uint16_t)(g_sonic_anims[ANI_SKID_TURN].count - 1)) {
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

/* Player_Action_Jump (Player.c:3295-3329). jumpAbilityState and the PlaySfx
 * call are not ported: no ability system, no audio hooks at this layer. */
static void action_jump(Player *p)
{
	int32_t force = PHYS_GRAVITY + PHYS_JUMP;
	int32_t speed;

	p->controlLock = 0;
	p->e.onGround = 0;
	/* Player.c:3300 -- the state half of this guard was dropped as
	 * unreachable (no Roll state existed yet); restored now that
	 * PSTATE_ROLL does, so jumping out of a roll does not re-apply the
	 * y-adjustment Player_Action_Roll already applied on entry. */
	if (p->e.collisionMode == CMODE_FLOOR && p->state != PSTATE_ROLL)
		p->e.y += PHYS_JUMP_OFFSET;

	p->e.velX = (p->e.groundVel * cos256(p->e.angle)
	             + force * sin256(p->e.angle)) >> 8;
	p->e.velY = (p->e.groundVel * sin256(p->e.angle)
	             - force * cos256(p->e.angle)) >> 8;

	sonic_set_anim(&p->animator, ANI_JUMP, 0, 0);
	/* Player.c:3316-3319. The Tails-only fixed-120 branch is not ported
	 * (Sonic only, see player.h). */
	speed = ((abs32(p->e.groundVel) * 0xF0) / PHYS_TOP_SPEED) + 0x30;
	p->animator.speed = (speed > 0xF0) ? (int16_t)0xF0 : (int16_t)speed;

	p->e.angle = 0;
	p->e.collisionMode = CMODE_FLOOR;
	p->skidding = 0;
	p->applyJumpCap = 1;
	p->state = PSTATE_NORMAL;   /* entity->state = Player_State_Air; */
}

/* Player_Action_Roll (Player.c:3330-3340). self->pushing is not ported: this
 * port has no pushing-against-a-wall feature/field to reset. */
static void action_roll(Player *p)
{
	sonic_set_anim(&p->animator, ANI_JUMP, 0, 0);
	p->state = PSTATE_ROLL;
	if (p->e.collisionMode == CMODE_FLOOR) p->e.y += PHYS_JUMP_OFFSET;
}

/* Player_State_Ground's roll trigger (Player.c:3849-3855). minRollVel's
 * Player_State_Crouch branch (0x11000) never applies: this port has no
 * Crouch state (see player.h), so it is always the 0x8800 branch. PlaySfx
 * not ported (no audio hooks at this layer). */
static void roll_entry(Player *p, uint16_t pad)
{
	if (p->e.groundVel && abs32(p->e.groundVel) >= 0x8800
	    && !(pad & PAD_LEFT) && !(pad & PAD_RIGHT) && (pad & PAD_DOWN))
		action_roll(p);
}

/* Player_HandleRollDeceleration (Player.c:3466-3556). */
static void roll_deceleration(Player *p, uint16_t pad)
{
	int32_t initialVel = p->e.groundVel;
	int32_t s;

	if ((pad & PAD_RIGHT) && p->e.groundVel < 0) p->e.groundVel += PHYS_ROLL_DECEL;
	if ((pad & PAD_LEFT) && p->e.groundVel > 0) p->e.groundVel -= PHYS_ROLL_DECEL;

	if (p->e.groundVel) {
		s = sin256(p->e.angle);
		if (p->e.groundVel < 0) {
			p->e.groundVel += PHYS_ROLL_FRICTION;
			p->e.groundVel += ((s >= 0) ? 0x1400 : 0x5000) * s >> 8;
			if (p->e.groundVel < -PHYS_ROLL_SPEED_CAP) p->e.groundVel = -PHYS_ROLL_SPEED_CAP;
		} else {
			p->e.groundVel -= PHYS_ROLL_FRICTION;
			p->e.groundVel += ((s <= 0) ? 0x1400 : 0x5000) * s >> 8;
			if (p->e.groundVel > PHYS_ROLL_SPEED_CAP) p->e.groundVel = PHYS_ROLL_SPEED_CAP;
		}
	} else {
		p->e.groundVel += 0x5000 * sin256(p->e.angle) >> 8;
	}

	/* Player_HandleRollDeceleration's CMODE_LWALL/CMODE_RWALL cases
	 * (Player.c:3523-3533) and its CMODE_ROOF case (3536-3554, whose own
	 * invertGravity branch never taken in this port -- see ground_movement's
	 * comment on the same reduction) are all three identical once
	 * invertGravity is always false, so a single "not on the floor" test
	 * covers them. */
	if (p->e.collisionMode != CMODE_FLOOR) {
		if (p->e.angle >= 0x40 && p->e.angle <= 0xC0
		    && abs32(p->e.groundVel) < 0x20000) {
			p->e.velX = (p->e.groundVel * cos256(p->e.angle)) >> 8;
			p->e.velY = (p->e.groundVel * sin256(p->e.angle)) >> 8;
			p->e.onGround = 0;
			p->e.angle = 0;
			p->e.collisionMode = CMODE_FLOOR;
		}
	} else if (p->state == PSTATE_TUBE_ROLL) {
		if (abs32(p->e.groundVel) < 0x10000)
			/* self->direction & FLIP_Y (Player.c:3509) is always 0 here: the
			 * only site that ever sets that bit on the PLAYER's direction in
			 * the original is the death-state setup at Player.c:190-193, and
			 * this port's direction field (player.h) never carries a FLIP_Y
			 * bit at all -- 0/1 facing only, an unconditional reduction, not
			 * one that depends on whether a death state exists (this port
			 * has one now: player_kill()/state_death() never touch
			 * direction either) -- so the relaunch is always the else arm
			 * below, +PHYS_TUBE_LAUNCH_SPEED. */
			p->e.groundVel = PHYS_TUBE_LAUNCH_SPEED;
	} else if ((p->e.groundVel >= 0 && initialVel <= 0)
	           || (p->e.groundVel <= 0 && initialVel >= 0)) {
		p->e.groundVel = 0;
		p->state = PSTATE_NORMAL;   /* self->state = Player_State_Ground; */
	}
}

/* Player_HandleAirMovement (Player.c:3255-3272), minus Player_Gravity_True
 * (s_main.c's airborne parameter to camera_update already covers it) and
 * pushing=0 (no pushing feature in this port). Used by air_state's full
 * per-frame body below and by the Roll/TubeRoll states' same-frame
 * fall-off-the-ground transition (Player.c:3941-3943, 3980-3982), which call
 * only this much of what air_state does -- not air_state's own friction/
 * animation-switch halves, matching the original calling only
 * Player_HandleAirMovement there, not the full Player_State_Air. */
static void air_gravity(Player *p, uint16_t pad)
{
	p->e.velY += PHYS_GRAVITY;

	/* Releasing jump early caps the rise, which is the short hop */
	if (p->e.velY < PHYS_JUMP_CAP && p->animator.anim == ANI_JUMP
	    && !(pad & (PAD_A | PAD_B | PAD_C)) && p->applyJumpCap) {
		p->e.velX -= p->e.velX >> 5;
		p->e.velY = PHYS_JUMP_CAP;
	}

	p->e.collisionMode = CMODE_FLOOR;

	/* Player_HandleAirRotation (Player.c:3238-3254), called unconditionally
	 * at the end of Player_HandleAirMovement, same as here. */
	if (p->rotation >= 0x100) {
		if (p->rotation < 0x200) p->rotation += 4;
		else p->rotation = 0;
	} else {
		if (p->rotation > 0) p->rotation -= 4;
		else p->rotation = 0;
	}
}

/* Player_HandleAirFriction (Player.c:3273-3293). Takes its own pad so
 * state_tube_air below can pass its controlLock-masked copy while air_state
 * passes the real one, the same split air_gravity already has. */
static void air_friction(Player *p, uint16_t pad)
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
}

/* Player_HandleAirFriction, then air_gravity, then the animation switch from
 * Player_State_Air (Player.c:3871-3930) -- Player_State_Air itself calls
 * HandleAirFriction unconditionally, then HandleAirMovement only while still
 * airborne, in that same order. */
static void air_state(Player *p, uint16_t pad)
{
	air_friction(p, pad);
	air_gravity(p, pad);

	/* Player_State_Air, Player.c:3889-3897: past the apex (velocity.y>0),
	 * a spring-pose animation reverts to whatever was reserved when the
	 * spring triggered (sh_src/spring.c's vertical/diagonal effects). The
	 * ANI_SPRING_CS branch (Player.c:3894-3896) is not ported: this port
	 * has no cutscene-spring animation (see tools/convert_sonic.py's
	 * ANIMATIONS list). */
	if (p->e.velY > 0) {
		if (p->animator.anim == ANI_SPRING_TWIRL || p->animator.anim == ANI_SPRING_DIAGONAL)
			sonic_set_anim(&p->animator, p->animationReserve, 0, 0);
	}

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

/* Player_State_Roll (Player.c:3932-3958). Player_HandleGroundRotation runs
 * first, exactly where the original calls it (Player.c:3936, before
 * HandleRollDeceleration) -- see player.h's rotation field comment. */
static void state_roll(Player *p, uint16_t pad, uint16_t jumpPress)
{
	ground_rotation(p);
	roll_deceleration(p, pad);
	p->applyJumpCap = 0;

	if (!p->e.onGround) {
		p->state = PSTATE_NORMAL;   /* self->state = Player_State_Air; */
		air_gravity(p, pad);
	} else {
		/* Player.c:3945-3951. Tails-only fixed-120 branch not ported. */
		int32_t speed = ((abs32(p->e.groundVel) * 0xF0) / PHYS_TOP_SPEED) + 0x30;
		p->animator.speed = (speed > 0xF0) ? (int16_t)0xF0 : (int16_t)speed;
		if (jumpPress) action_jump(p);
	}
}

/* Player_State_TubeRoll (Player.c:3959-3994). No jumpPress check anywhere in
 * this function: the original never reads self->jumpPress in this state
 * either -- tube rolling cannot jump. Player_HandleGroundRotation runs first,
 * before the controlLock masking below, exactly where Player.c:3963 calls it
 * (ahead of its own left/right save-and-restore at 3965-3971). */
static void state_tube_roll(Player *p, uint16_t pad)
{
	uint16_t maskedPad = pad;

	ground_rotation(p);

	if (p->controlLock > 0) {
		maskedPad = (uint16_t)(pad & ~(PAD_LEFT | PAD_RIGHT));
		p->controlLock--;
	}

	roll_deceleration(p, maskedPad);
	p->applyJumpCap = 0;

	if (!p->e.onGround) {
		p->state = PSTATE_TUBE_AIR;   /* self->state = Player_State_TubeAirRoll; */
		air_gravity(p, pad);
	} else {
		/* Player.c:3984-3990. Tails-only fixed-120 branch not ported. */
		int32_t speed = ((abs32(p->e.groundVel) * 0xF0) / PHYS_TOP_SPEED) + 0x30;
		p->animator.speed = (speed > 0xF0) ? (int16_t)0xF0 : (int16_t)speed;
	}
}

/* Player_State_TubeAirRoll (Player.c:3995-4025). Player_HandleGroundRotation
 * runs first, exactly where Player.c:3999 calls it (the function's very
 * first line, ahead of its own controlLock masking below). */
static void state_tube_air(Player *p, uint16_t pad)
{
	uint16_t maskedPad = pad;

	ground_rotation(p);

	if (p->controlLock > 0) {
		maskedPad = (uint16_t)(pad & ~(PAD_LEFT | PAD_RIGHT));
		p->controlLock--;
	}

	air_friction(p, maskedPad);
	p->applyJumpCap = 0;

	if (!p->e.onGround)
		air_gravity(p, pad);
	else
		p->state = PSTATE_TUBE_ROLL;   /* self->state = Player_State_TubeRoll; */
}

/* Player_CheckAttacking/Player_CheckAttackingNoInvTimer, see player.h's own
 * comment on this function for the exact reduction. */
uint8_t player_is_attacking(const Player *p)
{
	return p->animator.anim == ANI_JUMP;
}

/* Player_Hurt/Player_HurtFlip + Player_Hit, see player.h's own comment for
 * the full transcription notes and the 0-rings-death rule. */
uint8_t player_hit(Player *p, int32_t hazardWorldX)
{
	if (p->state == PSTATE_HURT || p->state == PSTATE_DEATH || p->blinkTimer > 0)
		return 0;

	/* Player_Hit's hurtType decision, Player.c:3572: `hurtType =
	 * (player->rings <= 0) + PLAYER_HURT_RINGLOSS` -- 0 rings selects
	 * PLAYER_HURT_DIE instead of the HASSHIELD/RINGLOSS knockback below.
	 * comm_has_rings() (sh_src/comm.h/comm.c) is this port's substitute for
	 * player->rings: the SH2 never sees the real count, only this one bit
	 * published from md_src/rings.c's counter every 68000 vblank. The DIE
	 * branch (Player.c:3624-3626) only sets deathType; this port's
	 * player_kill() already carries that branch's full death setup (see
	 * its own comment), so it is called directly here instead of a
	 * deathType flag threaded through another tick. */
	if (!comm_has_rings()) {
		player_kill(p);
		return 1;
	}

	/* Player_Hurt, Player.c:2401: `player->position.x > entity->position.x
	 * ? 0x20000 : -0x20000` -- away from the hazard. */
	p->e.velX = (p->e.x > hazardWorldX) ? PHYS_HURT_KNOCKBACK_X : -PHYS_HURT_KNOCKBACK_X;

	/* Player_Hit's shared HASSHIELD/RINGLOSS body, Player.c:3587-3611 (both
	 * branches are physically identical -- see player.h's own comment on
	 * why this port never distinguishes them). */
	p->state = PSTATE_HURT;
	sonic_set_anim(&p->animator, ANI_HURT, 0, 0);
	p->e.velY = PHYS_HURT_KNOCKBACK_Y;
	p->e.onGround = 0;
	p->blinkTimer = PLAYER_BLINK_TIME;
	p->hidden = 0;
	p->controlLock = 0;
	p->skidding = 0;
	p->applyJumpCap = 0;
	return 1;
}

/* Player_State_Hurt (Player.c:4363-4397). Player_Gravity_False/True are
 * camera-only (see this file's own note on ground_movement's tail for the
 * same reduction s_main.c already relies on: the camera's dead zone is
 * driven purely off onGround), so neither is transcribed here. No
 * underwater branch: this port has no water. */
static void state_hurt(Player *p)
{
	if (p->e.onGround) {
		p->state = PSTATE_NORMAL;   /* self->state = Player_State_Ground; */

		if (p->e.velX >= -PHYS_HURT_KNOCKBACK_X) {
			if (p->e.velX <= PHYS_HURT_KNOCKBACK_X) p->e.groundVel = 0;
			else p->e.groundVel -= PHYS_HURT_KNOCKBACK_X;
		} else {
			p->e.groundVel += PHYS_HURT_KNOCKBACK_X;
		}

		p->controlLock = 0;
		p->skidding = 0;
	} else {
		p->e.velY += PHYS_HURT_GRAVITY;
		p->skidding = 0;
	}
}

/* Player_CheckBadnikBreak's bounce-off, see player.h's own comment. */
void player_bounce_badnik(Player *p, int32_t hazardWorldY)
{
	if (p->e.velY <= 0) {
		p->e.velY += 0x10000;
	} else if (p->e.y >= hazardWorldY) {
		p->e.velY -= 0x10000;
	} else {
		p->e.velY = -(p->e.velY + 2 * PHYS_GRAVITY);
	}
}

/* Player_Hit's shared death setup plus PLAYER_DEATH_DIE_USESFX, see
 * player.h's own comment for the exact reduction. */
void player_kill(Player *p)
{
	if (p->state == PSTATE_DEATH) return;

	p->e.velX = 0;
	p->e.velY = PHYS_DEATH_POP_Y;
	p->e.groundVel = 0;
	p->e.onGround = 0;
	p->state = PSTATE_DEATH;
	p->blinkTimer = 0;
	p->hidden = 0;
	p->controlLock = 0;
	p->skidding = 0;
	p->applyJumpCap = 0;
	p->respawnPending = 0;
	sonic_set_anim(&p->animator, ANI_DIE, 1, 0);
}

/* Player_State_Death (Player.c:4398-4426), minus the superState/blinkTimer-
 * clear/camera-pin/sidekick branches this port has none of (see player.h's
 * own comment on player_kill for the full list). The camera-pin skip is a
 * real, if minor, visual deviation: the original stops the dying Sonic from
 * outrunning the camera downward (Player.c:4416-4421); this port's camera
 * (sh_src/camera.c) has no equivalent hook from player.c, so the fall can
 * run slightly ahead of the camera's own follow lag before respawn fires. */
static void state_death(Player *p)
{
	p->e.velX = 0;
	p->e.velY += PHYS_GRAVITY;
	sonic_set_anim(&p->animator, ANI_DIE, 0, 0);

	if (p->e.velY > PLAYER_DEATH_RESPAWN_VY) p->respawnPending = 1;
}

void player_update(Player *p, uint16_t pad)
{
	static uint16_t prevPad;
	uint16_t jumpPress = (pad & ~prevPad) & (PAD_A | PAD_B | PAD_C);
	const SonicFrame *f;

	prevPad = pad;

	/* Player_Update's blink-timer preamble (Player.c ~88-93), read against
	 * p->state as it stood at THIS tick's entry -- same ordering as the
	 * original, which runs this ahead of StateMachine_Run(self->state)
	 * (Player.c:129), before whatever this tick's own state dispatch below
	 * might do (e.g. player_hit() setting PSTATE_HURT with a fresh
	 * blinkTimer, which must NOT immediately re-decrement on the very tick
	 * it was set -- it does not, since this runs first, still seeing last
	 * tick's state). No else branch needed: blinkTimer only ever starts at
	 * PLAYER_BLINK_TIME (120, a multiple of 8), so the final tick of the
	 * countdown (120->...->1->0) always lands on a "visible" phase of the
	 * bit-4 toggle below, the same reason the original never needs one either. */
	if (p->state != PSTATE_HURT && p->blinkTimer > 0) {
		p->blinkTimer--;
		p->hidden = (p->blinkTimer & 4) ? 1 : 0;
	}

	switch (p->state) {
	case PSTATE_ROLL:      state_roll(p, pad, jumpPress); break;
	case PSTATE_TUBE_ROLL: state_tube_roll(p, pad); break;
	case PSTATE_TUBE_AIR:  state_tube_air(p, pad); break;
	case PSTATE_HURT:      state_hurt(p); break;
	case PSTATE_DEATH:     state_death(p); break;
	default:                /* PSTATE_NORMAL: Player_State_Ground / Air */
		if (p->e.onGround) {
			/* Player_State_Ground calls Player_HandleGroundRotation before
			 * Player_HandleGroundMovement (Player.c:3835-3836). */
			ground_rotation(p);
			ground_movement(p, pad);
			ground_animation(p);
			if (jumpPress) action_jump(p);
			else roll_entry(p, pad);
		} else {
			air_state(p, pad);
		}
		break;
	}

	/* RSDK takes the collision box from the animation frame every update, so
	 * the curled jump box arrives with the jump animation rather than being
	 * switched by hand. */
	f = sonic_anim_frame(&p->animator);
	p->e.outer.left = f->outerLeft;
	p->e.outer.top = f->outerTop;
	p->e.outer.right = f->outerRight;
	p->e.outer.bottom = f->outerBottom;
	p->e.inner.left = f->innerLeft;
	p->e.inner.top = f->innerTop;
	p->e.inner.right = f->innerRight;
	p->e.inner.bottom = f->innerBottom;

	if (p->state == PSTATE_DEATH) {
		/* Player_Hit's deathType setup sets tileCollisions = TILECOLLISION_
		 * NONE (Player.c:202): the death-fall arc is meant to carry Sonic
		 * straight through terrain, not snag on a wall or ceiling mid-fall.
		 * Integrate position directly instead of calling path_grip/path_air
		 * (both of which run full tile collision unconditionally). */
		p->e.x += p->e.velX;
		p->e.y += p->e.velY;
	} else if (p->e.onGround) {
		path_grip(&p->e);
	} else {
		path_air(&p->e);
	}

	if (p->e.onGround) p->applyJumpCap = 0;

	/* Player_LateUpdate's grounded branch (Player.c ~305-310): only touched
	 * while onGround, so it keeps its last value through a jump, same as the
	 * original only assigning self->camera->adjustY inside
	 * "if (self->onGround)". */
	if (p->e.onGround)
		p->camAdjustY = (p->animator.anim == ANI_JUMP) ? PHYS_JUMP_OFFSET : 0;

	sonic_process_anim(&p->animator);
}

/* Zone_HandlePlayerBounds, Left/Right/Death/Bottom boundaries (Zone.c
 * ~568-639). Top is still not ported (Zone_StageLoad leaves
 * playerBoundActiveT off and nothing in GHZ1 ever turns it on, same as
 * before). */
void player_apply_world_bounds(Player *p, int32_t boundL, int32_t boundR,
                                int32_t boundB, int32_t deathBoundB)
{
	/* Zone.c ~570-585. hitbox->left is a negative extent (outer.left is too,
	 * same RSDK convention), so negating it first gives a positive offset,
	 * matching "-TO_FIXED(1) * playerHitbox->left" without left-shifting a
	 * negative value. No auto-scroll here (Zone->autoScrollSpeed is always 0
	 * for this act), so the velocity floor Zone.c applies to autoScrollSpeed
	 * floors to 0 instead. */
	int32_t offsetL = (-(int32_t)p->e.outer.left) << 16;
	if (p->e.x - offsetL <= boundL) {
		p->e.x = boundL + offsetL;
		if (p->e.onGround) {
			if (p->e.groundVel < 0) {
				p->e.velX = 0;
				p->e.groundVel = 0;
			}
		} else if (p->e.velX < 0) {
			p->e.velX = 0;
			p->e.groundVel = 0;
		}
	}

	/* Zone.c ~588-608, the mirror of the left boundary using the box's right
	 * extent, which is already a positive offset. */
	{
		int32_t offsetR = (int32_t)p->e.outer.right << 16;
		if (p->e.x + offsetR >= boundR) {
			p->e.x = boundR - offsetR;
			if (p->e.onGround) {
				if (p->e.groundVel > 0) {
					p->e.velX = 0;
					p->e.groundVel = 0;
				}
			} else if (p->e.velX > 0) {
				p->e.velX = 0;
				p->e.groundVel = 0;
			}
		}
	}

	/* Zone_HandlePlayerBounds' Death Boundary (Zone.c:618-630): the original
	 * branches on `Zone->playerBoundsB[playerID] <= Zone->deathBoundary
	 * [playerID]`, comparing position.y against deathBoundary in that arm or
	 * against the (possibly marker-narrowed) playerBoundsB itself in the
	 * else arm. Reduces to a single "position.y > deathBoundB" test here,
	 * proven rather than assumed: deathBoundsB is this act's cameraBoundsB
	 * AT STAGE LOAD (bounds_init, before any marker narrows it), and
	 * bounds.c's own k_markers table never writes playerBoundsB past that
	 * same load-time value (every BOUNDSMARKER_ANY_Y/ABOVE_Y row's y is
	 * <= g_map_h*16) -- so "playerBoundsB <= deathBoundary" holds for every
	 * marker this act has, and the original's other branch,
	 * `position.y > playerBoundsB`, is unreachable for GHZ1. Once death
	 * triggers, the original also clears playerBoundActiveB so the ordinary
	 * floor clamp below stops re-catching the falling player
	 * (Zone.c:623/628); this port gates that same effect on
	 * `p->state != PSTATE_DEATH` instead of a persistent flag, since
	 * PSTATE_DEATH here only ever starts from a kill. */
	if (p->state != PSTATE_DEATH && p->e.y > deathBoundB)
		player_kill(p);

	/* Zone.c ~632-639. TO_FIXED(20): 20 pixels, 16.16 fixed. */
	if (p->state != PSTATE_DEATH) {
#define WORLD_BOUND_MARGIN_Y (20 << 16)
		if (p->e.y + WORLD_BOUND_MARGIN_Y > boundB) {
			p->e.y = boundB - WORLD_BOUND_MARGIN_Y;
			p->e.velY = 0;
			p->e.onGround = 1;
		}
#undef WORLD_BOUND_MARGIN_Y
	}
}
