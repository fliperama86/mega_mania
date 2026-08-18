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
#define PSTATE_HURT      4  /* Player_State_Hurt, Player.c:4363-4397 */
#define PSTATE_DEATH     5  /* Player_State_Death, Player.c:4398-4426 --
                              * Player_State_Drown is out of scope (no water
                              * ported yet), so this is the only death state. */

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
	/* player->animationReserve (Player.h): the animation to restore once a
	 * spring-pose animation (ANI_SPRING_TWIRL/ANI_SPRING_DIAGONAL) ends,
	 * written by sh_src/spring.c's vertical/diagonal triggers (Spring.c:
	 * 149-151, 329-331) and consumed by air_state()'s restore check
	 * (Player_State_Air, Player.c:3890-3897). */
	uint16_t animationReserve;
	/* player->blinkTimer (Player.h): post-hit invulnerability countdown, set
	 * to 120 by player_hit() (Player_Hit, Player.c:3593/3607) and decremented
	 * once per tick everywhere except PSTATE_HURT itself (Player_Update,
	 * Player.c ~88-93) -- see player.c's blink handling at the top of
	 * player_update(). Doubles as the hit-guard Player_Hurt/Player_HurtFlip
	 * check (`player->blinkTimer > 0`, Player.c:2397/2409): this port has no
	 * shield/star-power invincibleTimer (not ported, no shields exist), so
	 * blinkTimer alone gates player_hit()'s re-entrancy. */
	uint16_t blinkTimer;
	/* Player_Update's `self->visible` (Player.c ~91, set from blinkTimer's
	 * own bit-4 toggle): 1 on a blink-hidden tick. player.c never draws
	 * anything itself -- s_main.c reads this to publish COMM_ANIM's
	 * out-of-range SONIC_FRAME_COUNT sentinel instead of a real frame index
	 * on a hidden tick (sh_src/comm.h's COMM_ANIM entry, md_src/sonic.c's
	 * own bounds check), spending zero new comm bits. */
	uint8_t hidden;
	/* Set by state_death() once the death-fall arc's velocity.y crosses
	 * Player_HandleDeath's own trigger (Player.c:4424-4425, `velocity.y >
	 * 0x100000`); s_main.c owns PLAYER_SPAWN_X/Y (this file has never taken
	 * a spawn-position constant of its own, see player_init()'s own
	 * parameters) and re-calls player_init() with them when it sees this
	 * flag, then clears it. See player.c's own top-of-file comment for why
	 * this is a named deviation from Player_HandleDeath's real lives/
	 * checkpoint flow, which this port does not have. */
	uint8_t respawnPending;
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

/* Player_State_Hurt's own airborne "gravity" (Player.c:4392-4395, the
 * `!underwater` arm -- this port has no water, see this file's PSTATE_DEATH
 * comment): NOT PHYS_GRAVITY. Player_State_Hurt never calls
 * Player_HandleAirMovement/air_gravity at all -- it adds this smaller
 * constant to velocity.y directly, once a tick, its own thing. */
#define PHYS_HURT_GRAVITY 0x3000
/* Player_Hurt's knockback velocity.x magnitude (Player.c:2401/2413) and
 * Player_Hit's shared knockback velocity.y (Player.c:3590/3604) -- both
 * hurtType branches this port carries (HASSHIELD/RINGLOSS) use the same
 * pair; see player.c's player_hit() for why the third original branch
 * (PLAYER_HURT_DIE) does not apply here. */
#define PHYS_HURT_KNOCKBACK_X 0x20000
#define PHYS_HURT_KNOCKBACK_Y (-0x40000)
/* Player_Hit's blinkTimer start (Player.c:3593/3607). */
#define PLAYER_BLINK_TIME 120
/* Player_HandleDeath's death-fall arc trigger (Player.c:4424-4425): once the
 * fall's velocity.y crosses this, respawn fires. */
#define PLAYER_DEATH_RESPAWN_VY 0x100000
/* Player_State_Death's own velocity.y pop (Player.c:223) and its
 * self->velocity.y += self->gravityStrength (Player.c:4411, gravityStrength
 * for grounded Sonic/no-shield/non-super is PHYS_GRAVITY -- Player.h's
 * sonicPhysicsTable row 6, same table PHYS_GRAVITY above already reads). */
#define PHYS_DEATH_POP_Y (-0x68000)

void player_init(Player *p, int32_t x, int32_t y);
void player_update(Player *p, uint16_t pad);

/* ---- The badnik-facing contract -------------------------------------------
 *
 * Badniks/spikes/spike logs are being built separately, on their own scene
 * table (see game/sh_src's own convention: springs already do this --
 * sh_src/spring.c owns collision/player-response, md_src/springs.c owns
 * drawing only, from a SEPARATE converted table -- since only the SH2 can
 * write Player's own velocity/state fields). These four functions are that
 * same split's other half: whatever calls them supplies its own hazard
 * table and touch test (matching ring_touches_sonic's shape, sh_src has no
 * shared touch-test helper of its own yet), and calls into Player only once
 * it already knows a touch happened.
 *
 * A typical badnik's own per-tick touch handler:
 *     if (touching(&badnik, p)) {
 *         if (player_is_attacking(p)) {
 *             player_bounce_badnik(p, badnik.y);
 *             destroy(&badnik);            // badnik's own job
 *         } else {
 *             player_hit(p, badnik.x);
 *         }
 *     }
 * A hazard that always kills outright regardless of ring count (a
 * bottomless pit, a crusher, ...) calls player_kill() directly instead of
 * player_hit(): player_hit() now reaches the original's insta-kill branch
 * on its own (a hit while already at 0 rings, see player_hit()'s own
 * comment), but that path is still gated on ring count -- an unconditional
 * kill, independent of how many rings the player is carrying, still wants
 * player_kill() itself. */

/* Player_CheckAttacking/Player_CheckAttackingNoInvTimer (Player.c:2420-2466),
 * reduced to what this port's state machine can express: Sonic only (no
 * character switch), no shield/invincibleTimer (not ported, so the two
 * original functions collapse into one here), no spindash/dropdash (not
 * ported). ANI_JUMP alone covers both real jumping and this port's rolling,
 * since action_roll() plays that same animation (Player_Action_Roll,
 * Player.c:3330-3340) -- exactly like the original, which never
 * distinguishes Player_State_Jump from Player_State_Roll here either, only
 * the animation ID. True while curled, whether jumping, rolling, or in the
 * air after either. */
uint8_t player_is_attacking(const Player *p);

/* Player_Hurt/Player_HurtFlip + Player_Hit (Player.c:2392-2417, 3557-3627).
 * hazardWorldX is the hazard's own world X, used only for the knockback
 * direction (which side of the player it is on) -- matching Player_Hurt's
 * own `player->position.x > entity->position.x` test, not Player_HurtFlip's
 * facing-based variant (no caller in this port's current roster needs that
 * second rule; add a player_hurt_flip() if one shows up).
 *
 * Returns 0 and does nothing if the hit should be ignored (already hurt,
 * already dying, or still blink-invulnerable) -- same re-entrancy contract
 * as Player_Hurt's own bool32 return, so a caller that wants to know
 * whether its hazard should also flash/play a hit sound can check it.
 *
 * The original's Player_Hit picks one of three outcomes -- lose a shield,
 * lose rings, or (hit while already at 0 rings, no shield) die instantly
 * (Player.c:3572, `hurtType = (player->rings <= 0) + PLAYER_HURT_RINGLOSS`).
 * This port has no shields (HASSHIELD and RINGLOSS are physically identical
 * anyway -- same velocity/animation/blinkTimer, differing only in whether
 * rings scatter), so the real choice left is survivable knockback vs. the
 * 0-rings death. rings are entirely 68000-side state (md_src/rings.c's own
 * ring counter) and the SH2 never sees the count itself -- only one bit of
 * it crosses the wire, COMM_TICK's bit [8] (sh_src/comm.h), freed by
 * narrowing the tick field from 8 bits to 7 (that field is only ever read
 * as a wrapping delta, never an absolute value, so the narrower range costs
 * only wrap period, not correctness -- see comm.h's own entry for the
 * proof). player_hit() reads that bit through comm_has_rings() and calls
 * player_kill() instead of the survivable branch exactly when it reads 0,
 * matching the original's insta-kill. Ring loss itself (md_src/rings.c)
 * still fires the same way it always did on the survivable branch -- see
 * that file's own comment on how it infers a hit without this function
 * telling it anything; the death branch never plays ANI_HURT, so that
 * inference never fires there, which is correct since ringPlayerCount is
 * already 0 in exactly that case -- nothing to scatter. */
uint8_t player_hit(Player *p, int32_t hazardWorldX);

/* Player_CheckBadnikBreak's bounce-off (Player.c:2543-2557), the player-side
 * half of a successful badnik kill -- destroying the badnik itself (score,
 * dust, sfx) is the caller's job, not this function's. hazardWorldY is the
 * badnik's own world Y, needed for the "player is below the badnik" branch
 * (2 * PHYS_GRAVITY is Player_CheckBadnikBreak's own `2 *
 * player->gravityStrength`, gravityStrength for grounded/no-shield/
 * non-super Sonic being PHYS_GRAVITY, same table PHYS_GRAVITY already
 * reads). */
void player_bounce_badnik(Player *p, int32_t hazardWorldY);

/* Player_Hit's shared "if (self->deathType)" setup block plus the
 * PLAYER_DEATH_DIE_USESFX case (Player.c:166-224), collapsed to what this
 * port carries: no super state, no shield, no camera/drawGroup/
 * ENGINESTATE_FROZEN/scrollDelay hooks (s_main.c's camera reads only
 * onGround, see camAdjustY's own comment), no sidekick/encore/competition
 * branches (single player only). Unconditional: always enters PSTATE_DEATH
 * regardless of ring count, unlike a plain player_hit() touch -- see that
 * function's own comment on why it cannot make the ring-count-gated
 * decision itself. Exposed for a hazard that wants an unconditional kill
 * (a bottomless pit, a crusher, ...) and used by player_apply_world_bounds
 * below for the real falling-off-the-map death. */
void player_kill(Player *p);

/* Zone_HandlePlayerBounds (Zone.c ~558-640): clamp the player to the act's
 * world edges so walking off the map finds a floor instead of falling
 * forever, and kill the player for real if they fall past the act's own
 * Death Boundary (Zone.c:618-630) -- see player.c's own comment on this
 * function for the exact reduction that makes deathBoundB alone enough
 * (this act's own bounds.c marker table never needs Zone_HandlePlayerBounds'
 * second, playerBoundsB-relative branch). boundL/boundR/boundB/deathBoundB
 * are 16.16 fixed point, matching e.x/e.y's scale (the original's
 * Zone->playerBounds fields and Zone->deathBoundary are TO_FIXED too;
 * boundB is bounds.c's own possibly-marker-narrowed ZoneBounds.playerBoundsB,
 * deathBoundB is ZoneBounds.deathBoundsB, the act's un-narrowed floor --
 * see bounds.h). */
void player_apply_world_bounds(Player *p, int32_t boundL, int32_t boundR,
                                int32_t boundB, int32_t deathBoundB);

#endif
