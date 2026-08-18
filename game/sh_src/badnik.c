#include <stdint.h>
#include "badnik.h"

/* Shared badnik collision core -- see badnik.h's own top comment. Six
 * classes below, each a compact independent re-derivation of its own
 * md_src/<class>.c's movement formula (same constants, same integer
 * arithmetic, transcribed twice rather than shared -- code cannot cross
 * the 68000/SH2 toolchain boundary at all, the same reason sh_src/spring.c
 * and md_src/springs.c already carry two independent copies of "is Sonic
 * touching this spring"). See md_src/badnik_base.h's own top comment for
 * why running the identical formula on both CPUs, off each CPU's own tick
 * counter and each CPU's own view of Sonic's position, is expected to
 * converge rather than needing a synchronized RNG/clock of its own. */

#define TO_FIXED(x) ((int32_t)(x) << 16)

static uint8_t badnik_touches(Player *p, int32_t bx, int32_t by,
                              int8_t hbL, int8_t hbT, int8_t hbR, int8_t hbB)
{
	int32_t thisIX = bx >> 16, thisIY = by >> 16;
	int32_t otherIX = p->e.x >> 16, otherIY = p->e.y >> 16;
	return thisIX + hbL < otherIX + p->e.outer.right
	    && thisIX + hbR > otherIX + p->e.outer.left
	    && thisIY + hbT < otherIY + p->e.outer.bottom
	    && thisIY + hbB > otherIY + p->e.outer.top;
}

uint8_t badnik_resolve(Player *p, int32_t bx, int32_t by,
                       int8_t hbL, int8_t hbT, int8_t hbR, int8_t hbB)
{
	if (!badnik_touches(p, bx, by, hbL, hbT, hbR, hbB)) return 0;
	if (player_is_attacking(p)) {
		player_bounce_badnik(p, by);
		return 1;
	}
	player_hit(p, bx);
	return 0;
}

/* ---- Motobug: sh_src mirror of md_src/motobug.c's own motobug_pose() --- */

typedef struct { int16_t x, y; } MotobugDef;
extern const uint16_t ghz_motobugs_sh[];
static const MotobugDef *const k_motobugs = (const MotobugDef *)((const uint8_t *)ghz_motobugs_sh + 2);
#define MOTOBUG_COUNT_SH (ghz_motobugs_sh[0])

#define MB_HB_L (-14)
#define MB_HB_T (-14)
#define MB_HB_R   14
#define MB_HB_B   14
#define MB_SPEED       1
#define MB_AMPLITUDE   48
#define MB_TURN_HOLD   12
#define MB_LEG         (MB_AMPLITUDE / MB_SPEED)
#define MB_CYCLE       (2 * (MB_LEG + MB_TURN_HOLD))

static uint32_t mbTick;
static uint8_t  mbDestroyed[(9 + 7) / 8];

/* Player-proximity gate, same shape/rationale every badnik sub-loop below
 * shares (see this file's own top comment): each class's own oscillation
 * clock (mbTick/cmTick/bbTick here) is a SHARED counter advanced once per
 * call, above/outside this loop, unconditionally -- so windowing only the
 * per-instance loop below never desyncs the shared phase, only skips
 * touch-testing instances too far from the player to matter (badnik_resolve
 * recomputes fresh from mbTick+offX every tick it does run, no drift on
 * re-entry). k_motobugs is x-sorted ascending (write_scene_table) and
 * MotobugDef's own sizeof (4, two naturally-aligned int16 fields) has no
 * padding hazard. MOTOBUG_GATE_MARGIN: MB_HB reach (14) + MB_AMPLITUDE (48,
 * this class's own max x drift) + player hitbox/buffer rounds up to 192. */
#define MOTOBUG_GATE_MARGIN 192

static void motobug_window(int32_t playerXpx, uint32_t n, uint32_t *lo, uint32_t *hi)
{
	int32_t xloWant = playerXpx - MOTOBUG_GATE_MARGIN;
	int32_t xhiWant = playerXpx + MOTOBUG_GATE_MARGIN;
	uint32_t a, b, m;

	a = 0; b = n;
	while (a < b) { m = a + (b - a) / 2; if (k_motobugs[m].x < xloWant) a = m + 1; else b = m; }
	*lo = a;
	a = *lo; b = n;
	while (a < b) { m = a + (b - a) / 2; if (k_motobugs[m].x < xhiWant) a = m + 1; else b = m; }
	*hi = a;
}

static void motobug_apply(Player *p)
{
	uint32_t i, lo, hi, n = MOTOBUG_COUNT_SH;
	int32_t phase = (int32_t)(mbTick % MB_CYCLE);
	int32_t offX;

	mbTick++;

	if (phase < MB_LEG) offX = -phase * MB_SPEED;
	else if (phase < MB_LEG + MB_TURN_HOLD) offX = -MB_AMPLITUDE;
	else if (phase < 2 * MB_LEG + MB_TURN_HOLD) offX = -MB_AMPLITUDE + (phase - MB_LEG - MB_TURN_HOLD) * MB_SPEED;
	else offX = 0;

	if (n > 9) n = 9;
	motobug_window(p->e.x >> 16, n, &lo, &hi);

	for (i = lo; i < hi; i++) {
		if ((mbDestroyed[i >> 3] >> (i & 7)) & 1) continue;
		if (badnik_resolve(p, TO_FIXED(k_motobugs[i].x + offX), TO_FIXED(k_motobugs[i].y),
		                   MB_HB_L, MB_HB_T, MB_HB_R, MB_HB_B) && player_is_attacking(p))
			mbDestroyed[i >> 3] |= (uint8_t)(1 << (i & 7));
	}
}

/* ---- Crabmeat: mirrors md_src/crabmeat.c's own crabmeat_pose() -------- */

typedef struct { int16_t x, y; } CrabmeatDef;
extern const uint16_t ghz_crabmeats_sh[];
static const CrabmeatDef *const k_crabmeats = (const CrabmeatDef *)((const uint8_t *)ghz_crabmeats_sh + 2);
#define CRABMEAT_COUNT_SH (ghz_crabmeats_sh[0])

#define CM_HB_L (-14)
#define CM_HB_T (-14)
#define CM_HB_R   14
#define CM_HB_B   14
#define CM_MOVE_TICKS  128
#define CM_AMPLITUDE   64
#define CM_SHOOT_HOLD  60
#define CM_CYCLE (2 * (CM_MOVE_TICKS + CM_SHOOT_HOLD))

static uint32_t cmTick;
static uint8_t  cmDestroyed[(11 + 7) / 8];

/* Same shape/rationale as motobug_window() above. CRABMEAT_GATE_MARGIN:
 * CM_HB reach (14) + CM_AMPLITUDE (64, this class's own max x drift) +
 * player hitbox/buffer rounds up to 192. */
#define CRABMEAT_GATE_MARGIN 192

static void crabmeat_window(int32_t playerXpx, uint32_t n, uint32_t *lo, uint32_t *hi)
{
	int32_t xloWant = playerXpx - CRABMEAT_GATE_MARGIN;
	int32_t xhiWant = playerXpx + CRABMEAT_GATE_MARGIN;
	uint32_t a, b, m;

	a = 0; b = n;
	while (a < b) { m = a + (b - a) / 2; if (k_crabmeats[m].x < xloWant) a = m + 1; else b = m; }
	*lo = a;
	a = *lo; b = n;
	while (a < b) { m = a + (b - a) / 2; if (k_crabmeats[m].x < xhiWant) a = m + 1; else b = m; }
	*hi = a;
}

static void crabmeat_apply(Player *p)
{
	uint32_t i, lo, hi, n = CRABMEAT_COUNT_SH;
	int32_t phase = (int32_t)(cmTick % CM_CYCLE);
	int32_t offX;

	cmTick++;

	if (phase < CM_MOVE_TICKS) offX = -(phase / 2);
	else if (phase < CM_MOVE_TICKS + CM_SHOOT_HOLD) offX = -CM_AMPLITUDE;
	else if (phase < 2 * CM_MOVE_TICKS + CM_SHOOT_HOLD) offX = -CM_AMPLITUDE + ((phase - CM_MOVE_TICKS - CM_SHOOT_HOLD) / 2);
	else offX = 0;

	if (n > 11) n = 11;
	crabmeat_window(p->e.x >> 16, n, &lo, &hi);

	for (i = lo; i < hi; i++) {
		if ((cmDestroyed[i >> 3] >> (i & 7)) & 1) continue;
		if (badnik_resolve(p, TO_FIXED(k_crabmeats[i].x + offX), TO_FIXED(k_crabmeats[i].y),
		                   CM_HB_L, CM_HB_T, CM_HB_R, CM_HB_B) && player_is_attacking(p))
			cmDestroyed[i >> 3] |= (uint8_t)(1 << (i & 7));
	}
}

/* ---- BuzzBomber: mirrors md_src/buzzbomber.c's own buzzbomber_pose() -- */

typedef struct { int16_t x, y; uint8_t direction, shotRange; } BuzzbomberDef;
extern const uint16_t ghz_buzzbombers_sh[];
static const BuzzbomberDef *const k_buzzbombers = (const BuzzbomberDef *)((const uint8_t *)ghz_buzzbombers_sh + 2);
#define BUZZBOMBER_COUNT_SH (ghz_buzzbombers_sh[0])

#define BB_HB_L (-24)
#define BB_HB_T (-12)
#define BB_HB_R   24
#define BB_HB_B   12
#define BB_SPEED      4
#define BB_FLY_TICKS  128
#define BB_IDLE_TICKS 60
#define BB_AMPLITUDE  (BB_SPEED * BB_FLY_TICKS)
#define BB_CYCLE      (2 * (BB_FLY_TICKS + BB_IDLE_TICKS))

static uint32_t bbTick;
static uint8_t  bbDestroyed[(18 + 7) / 8];

/* Same shape/rationale as motobug_window() above. BuzzbomberDef's own sizeof
 * (6, matching BUZZBOMBER_SCENE's row_fmt ">hhBB" exactly -- verified
 * against this project's actual sh-elf-gcc) has no padding hazard, unlike
 * ChopperDef's own struct just below (see chopper_apply's own comment).
 * BUZZBOMBER_GATE_MARGIN: BB_HB reach (24) + BB_AMPLITUDE (4*128=512, this
 * class's own max x drift, by far the largest of any badnik class) + player
 * hitbox/buffer rounds up to 640. */
#define BUZZBOMBER_GATE_MARGIN 640

static void buzzbomber_window(int32_t playerXpx, uint32_t n, uint32_t *lo, uint32_t *hi)
{
	int32_t xloWant = playerXpx - BUZZBOMBER_GATE_MARGIN;
	int32_t xhiWant = playerXpx + BUZZBOMBER_GATE_MARGIN;
	uint32_t a, b, m;

	a = 0; b = n;
	while (a < b) { m = a + (b - a) / 2; if (k_buzzbombers[m].x < xloWant) a = m + 1; else b = m; }
	*lo = a;
	a = *lo; b = n;
	while (a < b) { m = a + (b - a) / 2; if (k_buzzbombers[m].x < xhiWant) a = m + 1; else b = m; }
	*hi = a;
}

static void buzzbomber_apply(Player *p)
{
	uint32_t i, lo, hi, n = BUZZBOMBER_COUNT_SH;
	int32_t phase = (int32_t)(bbTick % BB_CYCLE);
	int32_t mag;

	bbTick++;

	if (phase < BB_FLY_TICKS) mag = phase * BB_SPEED;
	else if (phase < BB_FLY_TICKS + BB_IDLE_TICKS) mag = BB_AMPLITUDE;
	else if (phase < 2 * BB_FLY_TICKS + BB_IDLE_TICKS) mag = BB_AMPLITUDE - (phase - BB_FLY_TICKS - BB_IDLE_TICKS) * BB_SPEED;
	else mag = 0;

	if (n > 18) n = 18;
	buzzbomber_window(p->e.x >> 16, n, &lo, &hi);

	for (i = lo; i < hi; i++) {
		int8_t sign = (k_buzzbombers[i].direction & 1) ? 1 : -1;
		if ((bbDestroyed[i >> 3] >> (i & 7)) & 1) continue;
		if (badnik_resolve(p, TO_FIXED(k_buzzbombers[i].x + sign * mag), TO_FIXED(k_buzzbombers[i].y),
		                   BB_HB_L, BB_HB_T, BB_HB_R, BB_HB_B) && player_is_attacking(p))
			bbDestroyed[i >> 3] |= (uint8_t)(1 << (i & 7));
	}
}

/* ---- Chopper (Jump only): mirrors md_src/chopper.c's own bounce sim --- */

typedef struct { int16_t x, y; uint8_t type, direction, charge; } ChopperDef;
extern const uint16_t ghz_choppers_sh[];
static const ChopperDef *const k_choppers = (const ChopperDef *)((const uint8_t *)ghz_choppers_sh + 2);
#define CHOPPER_COUNT_SH (ghz_choppers_sh[0])
#define CHOPPER_JUMP_TYPE 0

#define CH_HB_L (-10)
#define CH_HB_T (-20)
#define CH_HB_R    6
#define CH_HB_B   20
#define CH_GRAVITY   0x1800
#define CH_LAUNCH_VY (-0x70000)

static int32_t chopperOffY = 0;
static int32_t chopperVelY = CH_LAUNCH_VY;
static uint8_t chDestroyed[(13 + 7) / 8];

/* NOT gated, unlike every other badnik sub-loop in this file -- an unrelated
 * perf note (2026-08-18 camera-X gating task): CHOPPER_COUNT_SH is only 13
 * rows, so leaving this one scan unwindowed costs little.
 *
 * ROW STRIDE (fixed 2026-08-18, data-layout bug fix): CHOPPER_SCENE's own
 * row_fmt is ">hhBBBx" (8 bytes, a trailing pad byte after `charge`), not
 * the logical ">hhBBB" (7 bytes) the five decoded fields alone would
 * suggest -- SceneRecipe now asserts every struct-cast-read table packs to
 * an EVEN size for exactly this reason (tools/convert_objects.py).
 * sizeof(ChopperDef) is 8 (its largest member needs 2-byte alignment), so
 * every row here already lines up with what k_choppers[i] reads, on both
 * CPUs (md_src/chopper.c's own ChopperEntry is the identical shape). */
static void chopper_apply(Player *p)
{
	uint32_t i;

	chopperOffY += chopperVelY;
	chopperVelY += CH_GRAVITY;
	if (chopperOffY > 0) { chopperOffY = 0; chopperVelY = CH_LAUNCH_VY; }

	for (i = 0; i < CHOPPER_COUNT_SH && i < 13; i++) {
		if (k_choppers[i].type != CHOPPER_JUMP_TYPE) continue;   /* Swim: out of scope */
		if ((chDestroyed[i >> 3] >> (i & 7)) & 1) continue;
		if (badnik_resolve(p, TO_FIXED(k_choppers[i].x), TO_FIXED(k_choppers[i].y) + chopperOffY,
		                   CH_HB_L, CH_HB_T, CH_HB_R, CH_HB_B) && player_is_attacking(p))
			chDestroyed[i >> 3] |= (uint8_t)(1 << (i & 7));
	}
}

/* ---- Newtron: mirrors md_src/newtron.c's own per-instance state ------- */

typedef struct { int16_t x, y; uint8_t type, direction; } NewtronDef;
extern const uint16_t ghz_newtrons_sh[];
static const NewtronDef *const k_newtrons = (const NewtronDef *)((const uint8_t *)ghz_newtrons_sh + 2);
#define NEWTRON_COUNT_SH (ghz_newtrons_sh[0])
#define NEWTRON_FLY_TYPE 1

/* +-128 (Newtron.c:97-100) does not fit int8_t -- see md_src/newtron.c's
 * own comment for why this is narrowed to +-127 on both sides. */
#define NT_TRIG_L (-127)
#define NT_TRIG_T  (-64)
#define NT_TRIG_R   127
#define NT_TRIG_B    64
#define NT_HB_L   (-12)
#define NT_HB_T   (-14)
#define NT_HB_R     12
#define NT_HB_B     14
#define NT_SHOOT_END   90
#define NT_FLY_SPEED    2
#define NT_FLY_TICKS  128

#define NT_STATE_DORMANT 0
#define NT_STATE_ACTIVE  1

static uint8_t  ntState[21];
static uint16_t ntTimer[21];
static uint8_t  ntDir[21];
static uint8_t  ntDestroyed[(21 + 7) / 8];

static uint8_t newtron_touch_point(Player *p, int32_t bx, int32_t by,
                                   int8_t hbL, int8_t hbT, int8_t hbR, int8_t hbB)
{
	return badnik_touches(p, bx, by, hbL, hbT, hbR, hbB);
}

/* Player-proximity gate, same shape as motobug_window() above, but this
 * class has real per-instance latched state (ntState[i]/ntTimer[i]/
 * ntDir[i], the dormant/active state machine): a windowed-out ACTIVE newtron
 * simply stops advancing ntTimer[i] until the player is back within range --
 * matching, not deviating from, the original's own RSDK::CheckOnScreen/
 * CheckPosOnScreen gate on Newtron_Update (Newtron.c:41-60, updateRange
 * 0x800000/0x200000 = 128px/32px), which freezes an off-screen Newtron's
 * timer the exact same way. NEWTRON_GATE_MARGIN must be generous enough that
 * this class's OWN largest reach from its authored x is always inside it:
 * the dormant trigger box (NT_TRIG, tested against e->x directly, no offset)
 * reaches +-127px; once ACTIVE, a flying Newtron's own offX can reach
 * NT_FLY_TICKS(128)*NT_FLY_SPEED(2)=256px plus its own NT_HB reach (12) =
 * 268px -- the larger of the two. 268 + player hitbox/buffer rounds up to
 * 384. */
#define NEWTRON_GATE_MARGIN 384

static void newtron_window(int32_t playerXpx, uint32_t n, uint32_t *lo, uint32_t *hi)
{
	int32_t xloWant = playerXpx - NEWTRON_GATE_MARGIN;
	int32_t xhiWant = playerXpx + NEWTRON_GATE_MARGIN;
	uint32_t a, b, m;

	a = 0; b = n;
	while (a < b) { m = a + (b - a) / 2; if (k_newtrons[m].x < xloWant) a = m + 1; else b = m; }
	*lo = a;
	a = *lo; b = n;
	while (a < b) { m = a + (b - a) / 2; if (k_newtrons[m].x < xhiWant) a = m + 1; else b = m; }
	*hi = a;
}

static void newtron_apply(Player *p)
{
	uint32_t i, lo, hi, n = NEWTRON_COUNT_SH;

	if (n > 21) n = 21;
	newtron_window(p->e.x >> 16, n, &lo, &hi);

	for (i = lo; i < hi; i++) {
		const NewtronDef *e = &k_newtrons[i];

		if ((ntDestroyed[i >> 3] >> (i & 7)) & 1) continue;

		if (ntState[i] == NT_STATE_DORMANT) {
			if (newtron_touch_point(p, TO_FIXED(e->x), TO_FIXED(e->y),
			                        NT_TRIG_L, NT_TRIG_T, NT_TRIG_R, NT_TRIG_B)) {
				ntState[i] = NT_STATE_ACTIVE;
				ntTimer[i] = 0;
				ntDir[i] = (e->type == NEWTRON_FLY_TYPE)
				           ? (uint8_t)((p->e.x >> 16) < e->x)
				           : (uint8_t)(e->direction & 1);
			}
			continue;
		}

		{
			uint16_t t = ntTimer[i];
			uint16_t limit = (e->type == NEWTRON_FLY_TYPE) ? NT_FLY_TICKS : NT_SHOOT_END;
			int32_t offX = 0;

			if (t >= limit) { ntState[i] = NT_STATE_DORMANT; continue; }
			ntTimer[i] = (uint16_t)(t + 1);

			if (e->type == NEWTRON_FLY_TYPE) {
				int32_t d = (int32_t)t * NT_FLY_SPEED;
				offX = ntDir[i] ? -d : d;
			}

			if (badnik_resolve(p, TO_FIXED(e->x + offX), TO_FIXED(e->y),
			                   NT_HB_L, NT_HB_T, NT_HB_R, NT_HB_B) && player_is_attacking(p))
				ntDestroyed[i >> 3] |= (uint8_t)(1 << (i & 7));
		}
	}
}

/* ---- Batbrain: mirrors md_src/batbrain.c's own dive-bomb state -------- */

typedef struct { int16_t x, y; } BatbrainDef;
extern const uint16_t ghz_batbrains_sh[];
static const BatbrainDef *const k_batbrains = (const BatbrainDef *)((const uint8_t *)ghz_batbrains_sh + 2);
#define BATBRAIN_COUNT_SH (ghz_batbrains_sh[0])

#define BR_HB_L (-12)
#define BR_HB_T (-18)
#define BR_HB_R   12
#define BR_HB_B   18
#define BR_TRIGGER_RANGE 128
#define BR_HANG_DELAY     40
#define BR_DROP_GRAVITY   0x1800
#define BR_ARRIVE_DIST    16
#define BR_FLY_SPEED      1
#define BR_FLY_RANGE    128
#define BR_RETURN_GRAVITY 0x1800

#define BR_STATE_HANG   0
#define BR_STATE_DROP   1
#define BR_STATE_FLY    2
#define BR_STATE_RETURN 3

static uint8_t  brState[7];
static uint16_t brHangTimer[7];
static uint16_t brFlyTimer[7];
static uint8_t  brDir[7];
static int16_t  brTargetY[7];
static int16_t  brOffX[7];
static int32_t  brOffY[7];
static int32_t  brVelY[7];
static uint8_t  brDestroyed[(7 + 7) / 8];

/* Player-proximity gate, same shape as newtron_window() above, and the same
 * "freezes in place while out of range, matching RSDK's own updateRange gate
 * on Batbrain_Update (Batbrain.c:37-38, updateRange 0x800000=128px both
 * axes)" reasoning for this class's own per-instance state machine
 * (brState[i] and friends). BATBRAIN_GATE_MARGIN: the largest reach from
 * this class's own authored x is max(BR_TRIGGER_RANGE=128 [HANG's own dx
 * test, against e->x directly], BR_FLY_RANGE(128)+BR_HB_R(12)=140 [FLY
 * state's own offX]) = 140; + player hitbox/buffer rounds up to 256. */
#define BATBRAIN_GATE_MARGIN 256

static void batbrain_window(int32_t playerXpx, uint32_t n, uint32_t *lo, uint32_t *hi)
{
	int32_t xloWant = playerXpx - BATBRAIN_GATE_MARGIN;
	int32_t xhiWant = playerXpx + BATBRAIN_GATE_MARGIN;
	uint32_t a, b, m;

	a = 0; b = n;
	while (a < b) { m = a + (b - a) / 2; if (k_batbrains[m].x < xloWant) a = m + 1; else b = m; }
	*lo = a;
	a = *lo; b = n;
	while (a < b) { m = a + (b - a) / 2; if (k_batbrains[m].x < xhiWant) a = m + 1; else b = m; }
	*hi = a;
}

static void batbrain_apply(Player *p)
{
	uint32_t i, lo, hi, n = BATBRAIN_COUNT_SH;
	int32_t sonicWorldX = p->e.x >> 16, sonicWorldY = p->e.y >> 16;

	if (n > 7) n = 7;
	batbrain_window(sonicWorldX, n, &lo, &hi);

	for (i = lo; i < hi; i++) {
		const BatbrainDef *e = &k_batbrains[i];

		if ((brDestroyed[i >> 3] >> (i & 7)) & 1) continue;

		switch (brState[i]) {
		case BR_STATE_HANG: {
			int32_t dx = sonicWorldX - e->x;
			if (dx < 0) dx = -dx;
			if (dx < BR_TRIGGER_RANGE && sonicWorldY >= e->y) {
				if (++brHangTimer[i] >= BR_HANG_DELAY) {
					brState[i] = BR_STATE_DROP;
					brHangTimer[i] = 0;
					brVelY[i] = 0;
					brOffY[i] = 0;
					brTargetY[i] = (int16_t)sonicWorldY;
					brDir[i] = (uint8_t)(sonicWorldX >= e->x);
				}
			} else {
				brHangTimer[i] = 0;
			}
			break;
		}
		case BR_STATE_DROP: {
			int16_t curY;
			brOffY[i] += brVelY[i];
			brVelY[i] += BR_DROP_GRAVITY;
			curY = (int16_t)(e->y + (brOffY[i] >> 16));
			if (brTargetY[i] - curY < BR_ARRIVE_DIST) {
				brVelY[i] = 0;
				brOffX[i] = 0;
				brState[i] = BR_STATE_FLY;
				brFlyTimer[i] = 0;
			}
			break;
		}
		case BR_STATE_FLY:
			brFlyTimer[i]++;
			brOffX[i] = (int16_t)(brDir[i] ? -(brFlyTimer[i] * BR_FLY_SPEED) : (brFlyTimer[i] * BR_FLY_SPEED));
			if (brFlyTimer[i] * BR_FLY_SPEED >= BR_FLY_RANGE) brState[i] = BR_STATE_RETURN;
			break;
		default: /* BR_STATE_RETURN */
			brVelY[i] -= BR_RETURN_GRAVITY;
			brOffY[i] += brVelY[i];
			if (brOffY[i] <= 0) {
				brState[i] = BR_STATE_HANG;
				brOffX[i] = 0; brOffY[i] = 0; brVelY[i] = 0;
			}
			break;
		}

		if (badnik_resolve(p, TO_FIXED(e->x + brOffX[i]), TO_FIXED(e->y) + brOffY[i],
		                   BR_HB_L, BR_HB_T, BR_HB_R, BR_HB_B) && player_is_attacking(p))
			brDestroyed[i >> 3] |= (uint8_t)(1 << (i & 7));
	}
}

void badnik_apply(Player *p)
{
	motobug_apply(p);
	crabmeat_apply(p);
	buzzbomber_apply(p);
	chopper_apply(p);
	newtron_apply(p);
	batbrain_apply(p);
}
