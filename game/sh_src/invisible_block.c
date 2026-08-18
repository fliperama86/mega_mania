#include <stdint.h>
#include "invisible_block.h"

#define TO_FIXED(x) ((int32_t)(x) << 16)

/* Collision.cpp's C_* result codes (RSDK.CheckObjectCollisionBox's return),
 * same values sh_src/spring.c's own copy uses (both derive from the same
 * upstream enum). */
#define C_NONE   0
#define C_TOP    1
#define C_LEFT   2
#define C_RIGHT  3
#define C_BOTTOM 4

/* assets/ghz/invisibleblocks.bin, linked into this program's own image at
 * its default `_ghz_invisibleblocks_sh` debug label (sh_src/assets_gen.s,
 * tools/gen_assets.py's manifest, align=2) -- same "read the generated
 * bytes directly, at their own linked address" convention sh_src/spring.c's
 * own top comment documents adopting for springs.
 *
 * NOT cast through a struct/array the way spring.c's k_springs is, and NOT
 * read via a single typed `[i]` index anywhere below: this class's own
 * generated row is 11 bytes (2+2+1+1+1+1+1+1+1, tools/convert_objects.py's
 * INVISIBLEBLOCK_SCENE row_fmt ">hhBBBBBBB") -- ODD -- so row 1 sits at byte
 * offset 11 from the table base, row 2 at 22, etc: every ODD-indexed row's
 * x/y int16 fields would land at an ODD address. The manifest's own
 * align=2 only guarantees the TABLE's base is 2-aligned, not every row
 * inside it, and SH2 (like every SH-2 core) raises a CPU address error
 * exception on a misaligned word access -- a real crash risk, not a style
 * nit. spring.c's own direct struct-cast is safe only because SpringDef's
 * row (6 bytes: 2+2+1+1) is EVEN, so every row's own offset stays
 * 2-aligned; this table does not have that property. Every field read below
 * is therefore a single uint8_t dereference (never misaligned regardless of
 * the resulting byte offset's parity), reconstructed into a value by hand. */
extern const uint16_t ghz_invisibleblocks_sh[];
#define INVISIBLEBLOCK_COUNT (ghz_invisibleblocks_sh[0])
#define INVISIBLEBLOCK_ROW_SIZE 11

static const uint8_t *invisibleblock_rec(uint16_t i)
{
	const uint8_t *base = (const uint8_t *)ghz_invisibleblocks_sh + 2;
	return base + (uint32_t)i * INVISIBLEBLOCK_ROW_SIZE;
}

static int16_t rd16be(const uint8_t *p)
{
	return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* Player-proximity gate: invisible_block_apply()'s only state is flagH/flagV,
 * both purely LOCAL to a single call (never persisted across ticks -- see
 * this function's own header comment), so there is no latch to desync here,
 * only the risk of one member of a crush PAIR falling outside the window
 * while the other stays in -- ruled out by margin: any block whose own
 * block_check_box() test could possibly fire this tick must have the player
 * within its own hbRight/hbBottom reach (at most 176px, this stage's own
 * tallest entry, see the derivation below), well inside this gate's own
 * margin, so both members of any real crush pair are always in or always
 * out together. ghz_invisibleblocks_sh is x-sorted ascending
 * (write_scene_table) -- the row itself is NOT safe to read via a typed
 * cast (see this file's own top comment on why every field is read via
 * rd16be/single-byte dereferences instead), so this window reads x the same
 * way. INVISIBLEBLOCK_GATE_MARGIN: hbRight is 8*width+8, and this stage's
 * own converted data (assets/ghz/invisibleblocks.bin) has width up to 16 ->
 * hbRight=136px (the widest reach any entry in this table has); 136 + ~64
 * (player hitbox + buffer) rounds up to 256. */
#define INVISIBLEBLOCK_GATE_MARGIN 256

static void invisibleblock_window(int32_t playerXpx, uint16_t n, uint16_t *lo, uint16_t *hi)
{
	int32_t xloWant = playerXpx - INVISIBLEBLOCK_GATE_MARGIN;
	int32_t xhiWant = playerXpx + INVISIBLEBLOCK_GATE_MARGIN;
	uint16_t a, b, m;

	a = 0; b = n;
	while (a < b) {
		m = (uint16_t)(a + (b - a) / 2);
		if (rd16be(invisibleblock_rec(m)) < xloWant) a = (uint16_t)(m + 1); else b = m;
	}
	*lo = a;

	a = *lo; b = n;
	while (a < b) {
		m = (uint16_t)(a + (b - a) / 2);
		if (rd16be(invisibleblock_rec(m)) < xhiWant) a = (uint16_t)(m + 1); else b = m;
	}
	*hi = a;
}

/* Player_CheckCollisionBox -> RSDK.CheckObjectCollisionBox -> the engine's
 * CheckObjectCollisionBox (Collision.cpp:276-461), same generic derivation
 * sh_src/spring.c's own spring_check_box already documents in full --
 * written as an INDEPENDENT copy here, not shared/exported from spring.c,
 * per this task's own brief: a concurrent task is building moving-platform
 * solidity in this same area, and a silent shared-code change would be hard
 * to attribute later, so this class owns its own self-contained copy of the
 * same math instead of introducing a new shared collision primitive
 * unilaterally.
 *
 * WIDENED from spring_check_box's own int8_t hitbox parameters to int16_t:
 * RSDK's real Hitbox struct is `{ int16 left, top, right, bottom; }`
 * (dependencies/RSDKv5/RSDKv5/RSDK/Graphics/Animation.hpp:17-22) -- spring.c's
 * own int8_t choice is a class-specific optimization safe only because every
 * spring hitbox this port converts is well within +-16px, not a general
 * property of Hitbox itself. InvisibleBlock's own hitbox is NOT always that
 * small: `hitbox.right = 8*width+8` / `hitbox.bottom = 8*height+8`
 * (InvisibleBlock.c:68-71) reaches 176px for this stage's own tallest entry
 * (x=14800,y=790,width=1,height=21 -- verified against assets/ghz/
 * invisibleblocks.bin), which overflows an int8_t (max 127) but fits int16_t
 * comfortably, matching the real engine's own field width.
 *
 * Every spring/InvisibleBlock hitbox this port ever builds is symmetric
 * (left==-right, top==-bottom, InvisibleBlock.c:68-71 constructs it that way
 * directly) and every Sonic hitbox this port generates is too (sonic_data.c),
 * so -- same reasoning spring_check_box's own comment gives -- the FLIP_X/
 * FLIP_Y hitbox-mirroring half of CheckObjectCollisionBox is a provable no-op
 * here and is not transcribed. */
static uint8_t block_check_box(Player *p, int32_t sx, int32_t sy,
                               int16_t hbLeft, int16_t hbTop, int16_t hbRight, int16_t hbBottom)
{
	int8_t oL = p->e.outer.left, oT = p->e.outer.top;
	int8_t oR = p->e.outer.right, oB = p->e.outer.bottom;
	int32_t thisIX = sx >> 16, thisIY = sy >> 16;
	int32_t otherIX = p->e.x >> 16, otherIY = p->e.y >> 16;
	int32_t collideX = p->e.x, collideY = p->e.y;
	uint8_t sideH = C_NONE, sideV = C_NONE, side;
	int32_t cx, cy;

	/* H-test (Collision.cpp:315-328): otherHitbox top/bottom shrunk by 1. */
	if (otherIX <= (hbRight + hbLeft + 2 * thisIX) >> 1) {
		if (otherIX + oR >= thisIX + hbLeft
		    && thisIY + hbTop < otherIY + oB - 1
		    && thisIY + hbBottom > otherIY + oT + 1) {
			sideH = C_LEFT;
			collideX = sx + TO_FIXED(hbLeft - oR);
		}
	} else {
		if (otherIX + oL < thisIX + hbRight
		    && thisIY + hbTop < otherIY + oB - 1
		    && thisIY + hbBottom > otherIY + oT + 1) {
			sideH = C_RIGHT;
			collideX = sx + TO_FIXED(hbRight - oL);
		}
	}

	/* V-test (Collision.cpp:335-349): otherHitbox left/right shrunk by 1. */
	if (otherIY < (hbTop + hbBottom + 2 * thisIY) >> 1) {
		if (otherIY + oB >= thisIY + hbTop
		    && thisIX + hbLeft < otherIX + oR - 1
		    && thisIX + hbRight > otherIX + oL + 1) {
			sideV = C_TOP;
			collideY = sy + TO_FIXED(hbTop - oB);
		}
	} else {
		if (otherIY + oT < thisIY + hbBottom
		    && thisIX + hbLeft < otherIX + oR - 1) {
			if (otherIX + oL + 1 < thisIX + hbRight) {
				sideV = C_BOTTOM;
				collideY = sy + TO_FIXED(hbBottom - oT);
			}
		}
	}

	/* Side pick (Collision.cpp:376-383): larger true penetration wins, a tie
	 * (or a lone axis) prefers V. */
	cx = (collideX - p->e.x) >> 16;
	cy = (collideY - p->e.y) >> 16;
	if ((cx * cx >= cy * cy && (sideV || !sideH)) || (!sideH && sideV))
		side = sideV;
	else
		side = sideH;

	/* setValues=true's application (Collision.cpp:385-461), RETRO_REV0U's
	 * tileCollisions!=TILECOLLISION_UP branch always taken (this port never
	 * runs upside-down gravity, same reduction path.c/spring.c already use). */
	switch (side) {
	case C_TOP:
		p->e.y = collideY;
		if (p->e.velY > 0) p->e.velY = 0;
		if (!p->e.onGround && p->e.velY >= 0) {
			p->e.groundVel = p->e.velX;
			p->e.angle = 0;
			p->e.onGround = 1;
		}
		p->controlLock = 0;
		p->e.collisionMode = CMODE_FLOOR;
		break;
	case C_LEFT:
		p->e.x = collideX;
		/* Player_CheckCollisionBox's C_LEFT wrapper's spindash groundVel
		 * bump (Player.c:2320-2327, its own documented bug) is not
		 * transcribed -- this port has no spindash, same reasoning spring.c's
		 * own spring_check_box comment gives for dropping the identical
		 * dead branch there. */
		p->controlLock = 0;
		break;
	case C_RIGHT:
		p->e.x = collideX;
		p->controlLock = 0;
		break;
	case C_BOTTOM:
		p->e.y = collideY;
		if (p->e.velY < 0) p->e.velY = 0;
		break;
	default:
		break;
	}
	return side;
}

/* InvisibleBlock_Update (InvisibleBlock.c:12-46), minus everything this
 * port's own table/data proves dead for GHZ1's 19 Mania-mode entities
 * (verified against assets/ghz/invisibleblocks.bin's own decoded bytes,
 * not memory or a screenshot):
 *
 *   - planeFilter is 0 on every one of the 19 rows, so `self->planeFilter <=
 *     0` (InvisibleBlock.c:18) is unconditionally true regardless of
 *     player->collisionPlane -- the plane-filter gate is not transcribed.
 *   - noChibi is 0 on every row, and this port has no chibi/isChibi state at
 *     all (no such field anywhere in player.h) -- `!self->noChibi ||
 *     !player->isChibi` (line 18) is unconditionally true -- not
 *     transcribed.
 *   - activeNormal only ever selects RSDK's ACTIVE_NORMAL vs ACTIVE_BOUNDS
 *     entity-scheduling mode (InvisibleBlock.c:63) -- this port evaluates
 *     every marker every frame regardless of on-screen state, same
 *     simplification sh_src/bounds.c's own table comment documents for
 *     BoundsMarker's identical ACTIVE_BOUNDS default -- so this field has no
 *     observable effect here and is not transcribed.
 *   - timeAttackOnly is 1 on 3 of the 19 rows (x=16084,16520,16648): in the
 *     original, `if (self->timeAttackOnly && globals->gameMode <
 *     MODE_TIMEATTACK) destroyEntity(self);` (InvisibleBlock.c:59-60) means
 *     these three are NEVER created outside Time Attack mode -- this port
 *     has no Time Attack mode at all (single continuous act-play run), so
 *     the condition is unconditionally true and these three rows are
 *     skipped every frame below, matching "never spawns" rather than
 *     "spawns invisible/inert".
 *
 * Crush rule (Player_Update, Global/Player.c ~195-200): "if
 * (collisionFlagH==(1|2) || collisionFlagV==(1|2)) deathType =
 * PLAYER_DEATH_DIE_USESFX", checked against the PREVIOUS frame's
 * accumulated flags (every InvisibleBlock/etc.'s own Update runs AFTER
 * Player_Update in RSDK's entity-slot order, so the check always trails the
 * write by one frame; the flags are reset to 0 immediately after the check,
 * same tick). This port collapses that one-frame relay into a single pass:
 * every InvisibleBlock is checked THIS frame and, if the accumulated flags
 * already show both opposite sides touched, player_kill() fires immediately
 * -- strictly more responsive (no frame of lag) than the original, not less
 * correct, and avoids adding a persistent collisionFlagH/V accumulator field
 * to Player itself (see this file's own top-of-block comment on why: a
 * concurrent task is adding moving-platform solidity in this same area, and
 * Player-struct changes are shared-collision-state, not this class's own).
 *
 * COMPROMISE, surfaced: because collisionFlagH/V are kept purely LOCAL to
 * this function's own loop (not a Player-struct field), a crush spanning
 * TWO DIFFERENT object types -- one InvisibleBlock plus, say, a moving
 * platform, one on each side -- is not detected; only an InvisibleBlock vs.
 * another InvisibleBlock (both within this same function's loop) can crush
 * the player. GHZ1's own 19-entry table has no two blocks positioned to
 * straddle the player like that in practice (most are isolated ceiling/
 * floor blockers), so this is not observed to matter for this act, but it is
 * a real, deliberate scope-narrowing from the original's cross-object
 * accumulator and is flagged rather than silently assumed equivalent. */
void invisible_block_apply(Player *p)
{
	uint16_t count = INVISIBLEBLOCK_COUNT;
	uint8_t flagH = 0, flagV = 0;
	uint16_t i, lo, hi;

	invisibleblock_window(p->e.x >> 16, count, &lo, &hi);

	for (i = lo; i < hi; i++) {
		const uint8_t *rec = invisibleblock_rec(i);
		int16_t x = rd16be(rec + 0);
		int16_t y = rd16be(rec + 2);
		uint8_t width = rec[4];
		uint8_t height = rec[5];
		uint8_t noCrush = rec[7];
		uint8_t timeAttackOnly = rec[9];
		int16_t hbRight, hbBottom;
		uint8_t side;

		if (timeAttackOnly) continue;   /* never created outside Time Attack -- see this function's own top comment */

		hbRight = (int16_t)(8 * (int16_t)width + 8);
		hbBottom = (int16_t)(8 * (int16_t)height + 8);

		side = block_check_box(p, TO_FIXED(x), TO_FIXED(y),
		                       (int16_t)-hbRight, (int16_t)-hbBottom, hbRight, hbBottom);

		if (!noCrush) {
			switch (side) {
			case C_TOP:    flagV |= 1; break;
			case C_LEFT:   flagH |= 1; break;
			case C_RIGHT:  flagH |= 2; break;
			case C_BOTTOM: flagV |= 2; break;
			default: break;
			}
		}
	}

	if (flagH == (1 | 2) || flagV == (1 | 2))
		player_kill(p);
}
