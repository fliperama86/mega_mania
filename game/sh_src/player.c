#include <stdint.h>
#include "player.h"
#include "trig.h"

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
	p->minJogVelocity = 0x40000;
	p->minRunVelocity = 0x60000;
	p->minDashVelocity = 0xC0000;
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
			 * only site that ever sets that bit on the PLAYER's direction is
			 * the death-state branch at Player.c:190-193, and this port has
			 * no death state (see this file's top-of-file comment), so the
			 * relaunch is always the else arm below, +PHYS_TUBE_LAUNCH_SPEED. */
			p->e.groundVel = PHYS_TUBE_LAUNCH_SPEED;
	} else if ((p->e.groundVel >= 0 && initialVel <= 0)
	           || (p->e.groundVel <= 0 && initialVel >= 0)) {
		p->e.groundVel = 0;
		p->state = PSTATE_NORMAL;   /* self->state = Player_State_Ground; */
	}
}

/* Player_HandleAirMovement (Player.c:3255-3272), minus Player_Gravity_True
 * (s_main.c's airborne parameter to camera_update already covers it) and
 * Player_HandleAirRotation/pushing=0 (no sprite-rotation output and no
 * pushing feature in this port -- see state_roll's comment for why nothing
 * here reads a "rotation" field). Used by air_state's full per-frame body
 * below and by the Roll/TubeRoll states' same-frame fall-off-the-ground
 * transition (Player.c:3941-3943, 3980-3982), which call only this much of
 * what air_state does -- not air_state's own friction/animation-switch
 * halves, matching the original calling only Player_HandleAirMovement
 * there, not the full Player_State_Air. */
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

/* Player_State_Roll (Player.c:3932-3958). Player_HandleGroundRotation is not
 * ported: this build's comm protocol (comm.h) has no sprite-rotation field,
 * only an animation-frame index and a direction bit, so nothing downstream
 * of this port could ever read the value RSDK's version computes -- same
 * reasoning as air_gravity dropping Player_HandleAirRotation above. */
static void state_roll(Player *p, uint16_t pad, uint16_t jumpPress)
{
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
 * either -- tube rolling cannot jump. Player_HandleGroundRotation not ported,
 * see state_roll's comment. */
static void state_tube_roll(Player *p, uint16_t pad)
{
	uint16_t maskedPad = pad;

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
 * not ported, see state_roll's comment. */
static void state_tube_air(Player *p, uint16_t pad)
{
	uint16_t maskedPad = pad;

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

void player_update(Player *p, uint16_t pad)
{
	static uint16_t prevPad;
	uint16_t jumpPress = (pad & ~prevPad) & (PAD_A | PAD_B | PAD_C);
	const SonicFrame *f;

	prevPad = pad;

	switch (p->state) {
	case PSTATE_ROLL:      state_roll(p, pad, jumpPress); break;
	case PSTATE_TUBE_ROLL: state_tube_roll(p, pad); break;
	case PSTATE_TUBE_AIR:  state_tube_air(p, pad); break;
	default:                /* PSTATE_NORMAL: Player_State_Ground / Air */
		if (p->e.onGround) {
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

	if (p->e.onGround) path_grip(&p->e);
	else path_air(&p->e);

	if (p->e.onGround) p->applyJumpCap = 0;

	/* Player_LateUpdate's grounded branch (Player.c ~305-310): only touched
	 * while onGround, so it keeps its last value through a jump, same as the
	 * original only assigning self->camera->adjustY inside
	 * "if (self->onGround)". */
	if (p->e.onGround)
		p->camAdjustY = (p->animator.anim == ANI_JUMP) ? PHYS_JUMP_OFFSET : 0;

	sonic_process_anim(&p->animator);
}

/* Zone_HandlePlayerBounds, Left/Right/Bottom boundaries (Zone.c ~568-639).
 * Top and the Death Boundary in between are not ported: this port has no
 * death/respawn state, so the Bottom clamp below stands in for the original
 * falling-off-the-map death, rather than being a literal reading of its rule. */
void player_apply_world_bounds(Player *p, int32_t boundL, int32_t boundR,
                                int32_t boundB)
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

	/* Zone.c ~632-639. TO_FIXED(20): 20 pixels, 16.16 fixed. */
#define WORLD_BOUND_MARGIN_Y (20 << 16)
	if (p->e.y + WORLD_BOUND_MARGIN_Y > boundB) {
		p->e.y = boundB - WORLD_BOUND_MARGIN_Y;
		p->e.velY = 0;
		p->e.onGround = 1;
	}
#undef WORLD_BOUND_MARGIN_Y
}
