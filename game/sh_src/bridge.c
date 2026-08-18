#include <stdint.h>
#include "bridge.h"
#include "platform_trig.h"
#include "path.h"

#define TO_FIXED(x) ((int32_t)(x) << 16)

/* GHZ Scene1.bin's Bridge entities (Bridge_Serialize, Bridge.c:309-313),
 * naturally-aligned {int16 x, int16 y, uint8 length, uint8 burnable} --
 * same direct pointer-cast convention sh_src/spring.c's own SpringEntry
 * already uses (both are 6-byte records with no compiler padding). */
typedef struct { int16_t x, y; uint8_t length, burnable; } BridgeDef;
extern const uint16_t ghz_bridges_sh[];
static const BridgeDef *const k_bridges = (const BridgeDef *)((const uint8_t *)ghz_bridges_sh + 2);

#define BRIDGE_COUNT 13   /* tools/convert_objects.py's kept count for GHZ1 */

#define BSTOOD_NONE   0   /* self->stoodEntity == -1 (never touched) */
#define BSTOOD_PLAYER 1   /* self->stoodEntity == the player */
#define BSTOOD_LEFT   2   /* self->stoodEntity == -2 (recently left/jumped away) */

typedef struct {
	int32_t timer;        /* self->timer, 0..~0x80 */
	int32_t depression;   /* self->depression */
	int32_t stoodPos;     /* self->stoodPos, 16.16, offset from startPos */
	uint8_t stoodState;   /* BSTOOD_* -- this port's single-player stand-in
	                        * for self->stoodEntity's three-way pointer/
	                        * sentinel value (Entity*, (void*)-1, (void*)-2) */
} BridgeState;

static BridgeState bstate[BRIDGE_COUNT];

/* Bridge_Update+Bridge_HandleCollisions (Bridge.c:12-44,143-279), reduced to
 * this port's single player (always "player1", so Bridge_HandleCollisions'
 * own `entity == player1` branch is the only one ever taken -- its sibling
 * branch, for a second/third player, is dead code here) and with
 * Bridge_Burn dropped (see bridge.h's own header comment for why that is
 * provably unreachable, not merely skipped).
 *
 * Every position here is 16.16, matching Bridge.c's own scale (self->
 * position, startPos/endPos, stoodPos, depression, bridgeDepth are all
 *16.16 in the original, unlike md_src/platform.c's own scene table, which
 * this port keeps in plain pixels -- Bridge's sine-driven sag needs the
 * finer fixed-point precision the original itself uses, so this file does
 * not convert to pixels the way sh_src/platform.c does). */
/* NOT gated by player-x window (2026-08-18 camera-X gating task), unlike
 * most of this batch's other _apply() scans -- deliberate, not an
 * oversight. bstate[i].timer/depression decay every tick a bridge is NOT
 * BSTOOD_PLAYER (Bridge.c:16-28's own ramp, transcribed at the top of this
 * loop below, BEFORE this file's own span-containment check), driven purely
 * by wall-clock ticks, not by the player's proximity -- windowing this loop
 * would freeze that decay while the player is out of range, letting a
 * bridge's sag linger a few ticks longer than intended once the player
 * returns (worst case ~16 ticks, this class's own timer/=8 ramp off a max
 * of 0x80). Purely cosmetic (bridgeDepth only shifts the one-way landing
 * band, never removes it, and always finishes decaying to 0 within those
 * same ~16 ticks regardless), but it is real latched, wall-clock-driven
 * state of the exact shape this task's own brief calls out for care, and
 * BRIDGE_COUNT is only 13 -- not enough entries for windowing's own savings
 * to outweigh introducing that timing wrinkle. Left as the full scan. */
void bridge_apply(Player *p)
{
	uint16_t n, i;

	n = ghz_bridges_sh[0];
	if (n > BRIDGE_COUNT) n = BRIDGE_COUNT;

	for (i = 0; i < n; i++) {
		const BridgeDef *def = &k_bridges[i];
		BridgeState *s = &bstate[i];
		int32_t posX = TO_FIXED(def->x), posY = TO_FIXED(def->y);
		/* Bridge_Create (Bridge.c:83-98): ++length first, then
		 * len = length<<19 (16.16, == planks*8 px -- half the total
		 * planks*16px span, since startPos/endPos are +-len from center). */
		int32_t planks = (int32_t)def->length + 1;
		int32_t half = planks << 19;
		int32_t startPos = posX - half, endPos = posX + half;
		int32_t bridgeDepth;

		/* Bridge_Update:16-28: timer ramp off whether the player was still
		 * recognised as standing at the END of the previous tick. */
		if (s->stoodState == BSTOOD_PLAYER) {
			if (s->timer < 0x80) s->timer += 8;
		} else if (s->timer) {
			s->stoodState = BSTOOD_LEFT;
			s->timer -= 8;
		} else {
			s->depression = 0;
		}
		bridgeDepth = (s->depression * s->timer) >> 7;

		if (p->e.x <= startPos || p->e.x >= endPos) {
			/* Bridge.c:273-276: leaving the span while still recognised as
			 * standing starts the "walked/fell off the end" decay. */
			if (s->stoodState == BSTOOD_PLAYER) {
				s->timer = 32;
				s->stoodState = BSTOOD_LEFT;
			}
			continue;
		}

		if (s->stoodState != BSTOOD_PLAYER) {
			/* Bridge.c:153-242, "not yet recognised as standing" branch.
			 * stoodEntityCount is always 0 here (single player, reset each
			 * tick, only ever incremented AFTER a successful landing below)
			 * so Bridge.c:155's own `!self->stoodEntityCount` gate is always
			 * true -- stoodPos is unconditionally refreshed to this entity's
			 * own position BEFORE the divisor/angle calc that reads it
			 * (Bridge.c:156 precedes 165-172), which is why, for a single
			 * entity, `entity.x - startPos <= stoodPos` always holds
			 * (equality, stoodPos having just been set to that exact value)
			 * and the "left of stoodPos" branch is always the one taken --
			 * not a simplification, a direct consequence of the source's
			 * own ordering with only one entity ever present. */
			int32_t offsetFromStart = p->e.x - startPos;

			if (p->e.velY >= 0) {
				int32_t divisor = offsetFromStart;   /* == stoodPos, just set */
				int32_t sinArg = (offsetFromStart << 7) / divisor;   /* == 1<<7, but written to mirror Bridge.c:167 exactly */
				int32_t hitY = (bridgeDepth * platform_sin512(sinArg) >> 9) - 0x80000;
				int32_t bandTop, bandBottom, playerBottom;

				if (p->e.velY >= 0x8000) { bandTop = hitY >> 16; bandBottom = bandTop + 8; }
				else { bandBottom = hitY >> 16; bandTop = bandBottom - 8; }

				playerBottom = (p->e.y >> 16) + p->e.outer.bottom;
				if (playerBottom >= bandTop && playerBottom <= bandBottom) {
					s->stoodPos = offsetFromStart;
					p->e.y = hitY + posY - TO_FIXED(p->e.outer.bottom);
					/* Bridge.c:203-209's own depression refresh is gated on
					 * stoodEntity having ALREADY been the real entity (not
					 * -1/-2) -- impossible here, since this whole branch
					 * only runs when stoodState != BSTOOD_PLAYER. Not
					 * refreshed on a fresh landing, matching the source. */
					s->stoodState = BSTOOD_PLAYER;
					if (p->e.velY < 0x10000) s->timer = 0x80;
					if (!p->e.onGround) { p->e.onGround = 1; p->e.groundVel = p->e.velX; }
					p->e.velY = 0;
				}
			}
		} else {
			/* Bridge.c:243-271, "already recognised as standing" branch:
			 * re-tested every tick, depression refreshed every tick. */
			int32_t distance = endPos - startPos;
			s->stoodPos = p->e.x - startPos;
			s->depression = platform_sin512((s->stoodPos >> 8) / (distance >> 16)) * (distance >> 13);

			if (p->e.y > posY - 0x300000) {
				if (p->e.velY >= 0) {
					p->e.y = posY + bridgeDepth - TO_FIXED(p->e.outer.bottom + 8);
					if (!p->e.onGround) { p->e.onGround = 1; p->e.groundVel = p->e.velX; }
					p->e.velY = 0;
				} else {
					s->stoodState = BSTOOD_LEFT;
				}
			}
		}
	}
}
