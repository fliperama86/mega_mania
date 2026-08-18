#include <stdint.h>
#include "spin_booster.h"
#include "force_spin.h"   /* rotate_on_pivot() -- exported by force_spin.h for exactly
                           * this kind of reuse (plane_switch.c already shares it),
                           * unlike the solid-collision math this task's brief asked
                           * to keep self-contained per class. */
#include "trig.h"

#define TO_FIXED(x) ((int32_t)(x) << 16)

/* GHZ1's own 4 SpinBooster rows (direction, autoGrip, bias, size, boostPower,
 * boostAlways, forwardOnly, playSound, allowTubeInput -- SpinBooster_Serialize,
 * SpinBooster.c:511-522), read from assets/ghz/spinboosters.bin (tools/
 * convert_objects.py's SPINBOOSTER_SCENE, row_fmt ">hhBBBBiBBBB"). Verified
 * against the actual decoded bytes (not memory/a screenshot):
 *   x,y        5800/1736, 5816/1944, 6024/2024, 6116/1912
 *   direction  3, 1, 0, 3      (FLIP_XY, FLIP_X, FLIP_NONE, FLIP_XY -- never FLIP_Y)
 *   autoGrip   4, 4, 3, 0
 *   bias       0 (every row)
 *   size       4 (every row)
 *   boostPower 0, 0, 15, 0     -- never negative
 *   boostAlways 0 (every row)
 *   forwardOnly 0 (every row)
 *   playSound  1, 1, 1, 0
 *   allowTubeInput 0 (every row) */
typedef struct {
	int16_t x, y;
	uint8_t direction;
	int32_t boostPower;
	uint8_t forwardOnly;
	uint8_t allowTubeInput;
	uint8_t size;
} SpinBoosterRow;

#define SPINBOOSTER_COUNT 4

extern const uint16_t ghz_spinboosters_sh[];
#define SPINBOOSTER_ROW_SIZE 16

/* NOT a direct struct-cast over the linked blob (contrast sh_src/spring.c's
 * own k_springs, and this class's own CorkscrewPath sibling which IS safe to
 * cast directly -- see corkscrew_path.c's own comment on the difference):
 * this row is 16 bytes, a multiple of 4, but the table's own base is only
 * guaranteed 2-byte aligned (tools/gen_assets.py's manifest entry for
 * ghz_spinboosters requests align=2, not align=4), so the int32 boostPower
 * field inside each row is NOT provably 4-aligned -- a real SH2 address-error
 * risk for a 32-bit read, not just the odd-row-stride problem
 * invisible_block.c has. Every field below is read through single-byte
 * dereferences instead (always alignment-safe) and reconstructed by hand. */
static void read_row(uint16_t i, SpinBoosterRow *out)
{
	const uint8_t *base = (const uint8_t *)ghz_spinboosters_sh + 2;
	const uint8_t *rec = base + (uint32_t)i * SPINBOOSTER_ROW_SIZE;

	out->x = (int16_t)(((uint16_t)rec[0] << 8) | rec[1]);
	out->y = (int16_t)(((uint16_t)rec[2] << 8) | rec[3]);
	out->direction = rec[4];
	/* rec[5]=autoGrip, rec[6]=bias -- both feed only the cut autoGrip/
	 * GetRollDir tile-grip snap (see this file's own top comment), not read. */
	out->size = rec[7];
	out->boostPower = (int32_t)(((uint32_t)rec[8] << 24) | ((uint32_t)rec[9] << 16)
	                            | ((uint32_t)rec[10] << 8) | rec[11]);
	/* rec[12]=boostAlways -- always 0 for GHZ1, dead (see handle_force_roll's
	 * own comment), not read. rec[13]=forwardOnly. rec[14]=playSound -- no
	 * audio hooks at this layer (same convention force_spin.c's own
	 * set_tube_state comment documents), not read. rec[15]=allowTubeInput. */
	out->forwardOnly = rec[13];
	out->allowTubeInput = rec[15];
}

/* Per-marker latch: was the player already inside this marker's trigger box,
 * approaching from the entering side, last time it was checked -- see
 * SpinBooster_Update's own three-way branch (SpinBooster.c:16-59) for why
 * this debounce is real, load-bearing state (unlike sh_src/corkscrew_path.c's
 * own activePlayers, which is provably dead code for its class -- this is
 * not that same shape, do not conflate the two). One player, so a plain
 * bool per marker stands in for the original's `1 << RSDK.GetEntitySlot()`
 * bitmask, same simplification force_spin.c/plane_switch.c already use. */
static uint8_t active[SPINBOOSTER_COUNT];

static int32_t iabs(int32_t v) { return v < 0 ? -v : v; }
static int32_t sign32(int32_t v) { return (v > 0) - (v < 0); }

/* SpinBooster_Create's direction->angle switch (SpinBooster.c:81-86). angle
 * is otherwise a Create-time-only derived field (not itself scene-editable),
 * so it is recomputed from `direction` here rather than carried as a
 * redundant table column. */
static uint8_t spinbooster_angle(uint8_t direction)
{
	switch (direction) {
	case 1: return 0x40;   /* FLIP_X */
	case 2: return 0x80;   /* FLIP_Y -- never seen in GHZ1's own table, kept for fidelity */
	case 3: return 0xC0;   /* FLIP_XY */
	default: return 0x00;  /* FLIP_NONE */
	}
}

/* SpinBooster_ApplyRollVelocity (SpinBooster.c:301-341), minus its trailing
 * `if (self->boostPower < 0 ...)` release block: boostPower is 0 or 15 on
 * every one of GHZ1's 4 rows, never negative (verified above), so that
 * whole block -- along with the `else` half of both branches below, which
 * only differ from the `+=`/kept half when boostPower<0 -- is provably dead
 * for this stage's own data and is not transcribed. */
static void apply_roll_velocity(Player *p, const SpinBoosterRow *m, uint8_t angle)
{
	if (p->e.onGround) {
		int32_t entAng = cos256(angle) + sin256(angle);
		int32_t plrAng = cos256(p->e.angle) - sin256(p->e.angle);
		int32_t power = (m->boostPower << 15) * sign32(plrAng) * sign32(entAng);
		p->e.groundVel += power;
	} else {
		int32_t x = (0x80 * cos256(angle)) * m->boostPower;
		int32_t y = (-0x80 * sin256(angle)) * m->boostPower;
		p->e.velX += x;
		p->e.velY += y;
	}
}

/* SpinBooster_HandleForceRoll (SpinBooster.c:455-490), minus:
 *   - SpinBooster_HandleRollDir's autoGrip tile-grip snap (SpinBooster.c:
 *     215-300, called at line 459 before the branch below) -- CUT, see
 *     spin_booster.h's own top comment for why (no RSDK.ObjectTileGrip
 *     equivalent in this port).
 *   - the `if (self->boostAlways) ApplyRollVelocity()` re-trigger inside the
 *     "already in tube" branch (SpinBooster.c:463-464) -- boostAlways is 0
 *     on every GHZ1 row, so this arm is dead and not transcribed (the
 *     branch becomes a no-op, matching sh_src/force_spin.c's own
 *     set_tube_state early-return for "already in tube", just arrived at
 *     for a different, data-proven reason here rather than an unconditional
 *     early return).
 *   - RSDK.PlaySfx (self->playSound) and player->pushing -- no audio hooks
 *     at this layer, no pushing feature, same omissions force_spin.c's own
 *     set_tube_state already documents for the identical ANI_JUMP-entry
 *     idiom.
 *   - nextAirState/nextGroundState -- no StateMachine_None analog in this
 *     port (same as every other file in this codebase that ports a
 *     RSDK object).
 *   - player->jumpOffset -> PHYS_JUMP_OFFSET: this port has one fixed
 *     curl-offset constant (player.h), not a per-character field -- same
 *     substitution force_spin.c's own set_tube_state already makes. */
static void handle_force_roll(Player *p, const SpinBoosterRow *m, uint8_t angle)
{
	if (p->state == PSTATE_TUBE_ROLL || p->state == PSTATE_TUBE_AIR)
		return;   /* boostAlways dead for GHZ1 -- see this function's own comment */

	if (p->animator.anim != ANI_JUMP) {
		sonic_set_anim(&p->animator, ANI_JUMP, 0, 0);
		if (p->e.collisionMode == CMODE_FLOOR && p->e.onGround)
			p->e.y += PHYS_JUMP_OFFSET;
	}

	/* allowTubeInput is 0 on every GHZ1 row, so this always fires -- kept
	 * data-driven rather than hardcoded for fidelity to the source. 0x7FFF,
	 * not the original's 0xFFFF: player.h's own controlLock is a signed
	 * int16_t (this port's own field, not RSDK's uint16), so the largest
	 * representable positive value stands in for the original's "lock
	 * indefinitely" sentinel -- effectively the same intent (over 9 minutes
	 * at 60Hz before it could ever count down on its own), and this port
	 * always clears it to 0 explicitly on tube exit anyway (see
	 * spin_booster_apply's own exit branch, and SpinBooster.c:38-40's
	 * `player->controlLock = 0` counterpart). */
	if (!m->allowTubeInput) p->controlLock = 0x7FFF;

	p->state = p->e.onGround ? PSTATE_TUBE_ROLL : PSTATE_TUBE_AIR;

	if (iabs(p->e.groundVel) < TO_FIXED(1))
		p->e.groundVel = (m->direction & 1) ? -TO_FIXED(4) : TO_FIXED(4);   /* FLIP_X bit */

	apply_roll_velocity(p, m, angle);
}

/* SpinBooster_Update (SpinBooster.c:12-59), one player (foreach_active
 * always visits the one entity this port has, same reasoning force_spin.c's
 * own ForceSpin_Update comment gives), `1 << RSDK.GetEntitySlot(player)`
 * collapsed to the plain per-marker `active[i]` latch declared above. */
/* NOT gated by player-x window (2026-08-18 camera-X gating task): only 4
 * entries in this stage's own data (SPINBOOSTER_COUNT), so a binary search's
 * own overhead is not worth it against the few iabs/rotate_on_pivot calls a
 * full scan already costs -- see the other _apply() scans in this batch's
 * own comments for the windowing this file deliberately skips. */
void spin_booster_apply(Player *p)
{
	uint16_t count = ghz_spinboosters_sh[0];
	uint16_t i;

	if (count > SPINBOOSTER_COUNT) count = SPINBOOSTER_COUNT;

	for (i = 0; i < count; i++) {
		SpinBoosterRow m;
		uint8_t angle, negAngle;
		int32_t mx, my, px, py;

		read_row(i, &m);
		angle = spinbooster_angle(m.direction);
		negAngle = (uint8_t)(0 - angle);   /* SpinBooster_Update:16 */

		mx = TO_FIXED(m.x);
		my = TO_FIXED(m.y);
		px = p->e.x;
		py = p->e.y;
		rotate_on_pivot(&px, &py, mx, my, negAngle);

		if (iabs(px - mx) < TO_FIXED(24) && iabs(py - my) < ((int32_t)m.size << 19)) {
			if (px >= mx) {
				if (!active[i]) {
					handle_force_roll(p, &m, angle);
					active[i] = 1;
				}
			} else {
				/* Exit Tube (SpinBooster.c:32-47). forwardOnly is 0 on every
				 * GHZ1 row, so this gate is always open -- kept data-driven
				 * for fidelity. */
				if (active[i] && !m.forwardOnly) {
					if (p->state == PSTATE_TUBE_ROLL || p->state == PSTATE_TUBE_AIR) {
						if (!m.allowTubeInput) p->controlLock = 0;
						p->state = p->e.onGround ? PSTATE_ROLL : PSTATE_NORMAL;
					}
				}
				active[i] = 0;
			}
		} else {
			active[i] = (px >= mx) ? 1 : 0;
		}
	}
}
