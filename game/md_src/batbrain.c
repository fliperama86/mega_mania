#include "batbrain.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_sprite.h"
#include "obj_pool.h"
#include "batbrain_data.h"
#include "badnik_base.h"
#include "assets_gen.h"

#define BATBRAIN_COUNT 7   /* Batbrain_Serialize is empty, Batbrain.c:221 -- position-only record */

typedef struct { int16_t x, y; } BatbrainEntry;

static const uint16_t *const ghz_batbrain_count_p = ASSET_GHZ_BATBRAINS;
static const BatbrainEntry *const ghz_batbrain_xy =
	(const BatbrainEntry *)((const uint8_t *)ASSET_GHZ_BATBRAINS + 2);
static const uint32_t *const batbrain_tiles = ASSET_BATBRAIN_TILES;

/* Batbrain.c:48-51 (Batbrain_StageLoad): hitboxBadnik, symmetric. */
#define BR_HB_L (-12)
#define BR_HB_T (-18)
#define BR_HB_R   12
#define BR_HB_B   18

/* Batbrain_State_CheckPlayerInRange's own distance gate (Batbrain.c:123,
 * 0x800000 = 128px) and DropToPlayer's own arrival gate (Batbrain.c:155,
 * 0x100000 = 16px) are decomp-exact; BR_HANG_DELAY replaces the RNG roll
 * gated on the first (see batbrain.h's own header comment). DropToPlayer's
 * gravity (Batbrain.c:152, 0x1800/tick) and Fly's own launch speed
 * (Batbrain.c:157-160, 0x10000 = 1px/tick) are decomp-exact; FlyToCeiling's
 * own gravity (Batbrain.c:196, 0x1800/tick, sign reversed) is too.
 * BR_FLY_RANGE (0x800000, Batbrain.c:178) reused as a fixed round-trip
 * distance in place of the RNG-gated Fly->FlyToCeiling roll. */
#define BR_TRIGGER_RANGE 128
#define BR_HANG_DELAY     40
#define BR_DROP_GRAVITY   0x1800
#define BR_ARRIVE_DIST    16
#define BR_FLY_SPEED      1
#define BR_FLY_RANGE     128
#define BR_RETURN_GRAVITY 0x1800

#define BR_STATE_HANG   0
#define BR_STATE_DROP   1
#define BR_STATE_FLY    2
#define BR_STATE_RETURN 3

/* VRAM ART BUDGET (art-budget trim task, 2026-08-18): tools/convert_objects.py's
 * BATBRAIN_ART now converts only "hang" (1 frame) and one representative
 * "fly" frame (the middle of its real 8-frame swoop) -- "fall" (the 2-frame
 * drop transition) is gone entirely, see that recipe's own comment. Both
 * kept poses are now PERMANENTLY VRAM-resident (23 tiles total, one arena
 * tenant, replacing the old 36-tile obj_anim_window streaming reservation).
 * BR_STATE_DROP now just shows BR_FRAME_HANG (held through the fall, no
 * distinct falling pose); BR_STATE_RETURN keeps showing BR_FRAME_FLY, same
 * as before. Still ONE shared class-wide value (curFrame below, "last
 * instance processed this tick wins" -- see batbrain_tick()'s own comment,
 * unchanged reasoning), just a binary choice now instead of an 11-way
 * index. */
#define BR_FRAME_HANG  0
#define BR_FRAME_FLY   1
#define BR_FRAME_COUNT 2

static uint8_t  destroyed[(BATBRAIN_COUNT + 7) / 8];
static uint8_t  brState[BATBRAIN_COUNT];
static uint16_t brHangTimer[BATBRAIN_COUNT];
static uint16_t brFlyTimer[BATBRAIN_COUNT];
static uint8_t  brDir[BATBRAIN_COUNT];     /* 0 = +X (right), 1 = -X (left) */
static int16_t  brTargetY[BATBRAIN_COUNT];
static int16_t  brOffX[BATBRAIN_COUNT];
static int32_t  brOffY[BATBRAIN_COUNT];    /* 16.16 */
static int32_t  brVelY[BATBRAIN_COUNT];    /* 16.16 */

/* Rebased working copy of {batbrain_hang[0], batbrain_fly[0]} -- see
 * motobug.c's own comment on mbFrames for why field-by-field. Plain
 * whole-sheet arena residency, no ObjAnimWindow. */
static ObjFrame  brFrames[BR_FRAME_COUNT];
static uint16_t  brBase;
static uint8_t   brLive;
static uint16_t  curFrame;   /* shared class-wide pose request, see this
                              * file's own top comment */

/* PRECOMPUTED PIECE TEMPLATES (Job 1, lever 1, this task) -- see
 * obj_sprite.h's own top-of-section comment and crabmeat.c's own identical
 * pattern (this file's twin). Sized BR_FRAME_COUNT * BATBRAIN_MAX_FRAME_
 * PIECES, an upper bound on batbrain_pieces[]'s own real length. Rebuilt
 * every time batbrain_arena_onBase() below fires. */
static ObjPieceTemplate brTemplatesH0[BR_FRAME_COUNT * BATBRAIN_MAX_FRAME_PIECES];
static ObjPieceTemplate brTemplatesH1[BR_FRAME_COUNT * BATBRAIN_MAX_FRAME_PIECES];

static void batbrain_rebuild_templates(void)
{
	uint8_t i;
	for (i = 0; i < BR_FRAME_COUNT; i++) {
		const ObjFrame *f = &brFrames[i];
		/* drawPriority literal 0 here matches batbrainType's own drawPriority
		 * field below. */
		obj_build_piece_templates(&brTemplatesH0[f->pieceOffset], &batbrain_pieces[f->pieceOffset], f->pieceCount,
		                          f->tileOffset, BATBRAIN_PAL, 0, f->pivotX, f->pivotY, 0, 0);
		obj_build_piece_templates(&brTemplatesH1[f->pieceOffset], &batbrain_pieces[f->pieceOffset], f->pieceCount,
		                          f->tileOffset, BATBRAIN_PAL, 0, f->pivotX, f->pivotY, 1, 0);
	}
}

static ObjDrawDecision batbrain_decide(void *st, uint16_t i, int16_t ex, int16_t ey,
                                       int16_t sonicWorldX, int16_t sonicWorldY,
                                       uint16_t sonicFrameIndex)
{
	ObjDrawDecision d;
	int16_t bx, by;
	(void)st;

	d.flipH = 0; d.flipV = 0; d.frame = OBJ_SKIP;

	bx = (int16_t)(ex + brOffX[i]);
	by = (int16_t)(ey + (int16_t)(brOffY[i] >> 16));

	/* SPRITE-VS-HITBOX DRIFT fix (Job 2, this task): the SAME brOffX[i]/
	 * brOffY[i] the hitbox test above already uses (the drop/fly/return
	 * state machine below, Batbrain.c-derived -- see this file's own top
	 * comment), now also carried on the draw decision so obj_type_draw()
	 * moves the sprite with it -- see obj_data.h's own ObjDrawDecision
	 * comment. */
	d.offX = brOffX[i];
	d.offY = (int16_t)(brOffY[i] >> 16);

	if (badnik_decide_common(destroyed, i, bx, by, BR_HB_L, BR_HB_T, BR_HB_R, BR_HB_B,
	                         sonicWorldX, sonicWorldY, sonicFrameIndex))
		return d;

	if (!brLive) return d;
	d.frame = curFrame;   /* index into brFrames[BR_FRAME_COUNT] -- always in
	                       * range, batbrain_tick() only ever writes 0 or 1 */
	d.flipH = brDir[i];
	return d;
}

static ObjTypeDesc batbrainType = {
	(const void *)0, sizeof(BatbrainEntry), BATBRAIN_COUNT, ghz_batbrain_count_p,
	(const uint32_t *)0, 0,
	brFrames, batbrain_pieces,
	OBJ_PRI_BADNIK, BATBRAIN_PAL, 0,
	BR_FLY_RANGE + 32,
	batbrain_decide, (void *)0,
	brTemplatesH0, brTemplatesH1
};

/* Plain whole-sheet VRAM residency -- see motobug.c's own comment on the
 * identical pattern. */
static void batbrain_arena_onBase(uint16_t base);
static void batbrain_arena_onLive(uint8_t live) { brLive = live; }

static ArenaClassDesc batbrainArenaDesc = {
	(const void *)0, sizeof(BatbrainEntry), BATBRAIN_COUNT,
	(const uint32_t *)0, BATBRAIN_MAX_FRAME_TILES + 6,   /* fly 17 + hang 6 = 23 */
	(int16_t)(ARENA_LOOKAHEAD_X((uint16_t)(BATBRAIN_MAX_FRAME_TILES + 6)) + BR_FLY_RANGE + 32),
	OBJ_PRI_BADNIK,
	batbrain_arena_onBase, batbrain_arena_onLive
};

static void batbrain_arena_onBase(uint16_t base)
{
	brBase = base;
	brFrames[BR_FRAME_HANG].tileOffset = (uint16_t)(base + batbrain_hang[0].tileOffset);
	brFrames[BR_FRAME_FLY].tileOffset = (uint16_t)(base + batbrain_fly[0].tileOffset);
	batbrain_rebuild_templates();   /* Job 1, lever 1: only tileOffset changed above, and only here */
}

static uint8_t batbrainInited;

static void batbrain_lazy_init(void)
{
	uint16_t i;

	batbrainInited = 1;
	for (i = 0; i < sizeof(destroyed); i++) destroyed[i] = 0;
	for (i = 0; i < BATBRAIN_COUNT; i++) {
		brState[i] = BR_STATE_HANG;
		brHangTimer[i] = 0; brFlyTimer[i] = 0;
		brDir[i] = 0; brTargetY[i] = 0;
		brOffX[i] = 0; brOffY[i] = 0; brVelY[i] = 0;
	}

	brFrames[BR_FRAME_HANG].tileOffset = 0;
	brFrames[BR_FRAME_HANG].pieceOffset = batbrain_hang[0].pieceOffset;
	brFrames[BR_FRAME_HANG].tileCount = batbrain_hang[0].tileCount;
	brFrames[BR_FRAME_HANG].pieceCount = batbrain_hang[0].pieceCount;
	brFrames[BR_FRAME_HANG].pivotX = batbrain_hang[0].pivotX;
	brFrames[BR_FRAME_HANG].pivotY = batbrain_hang[0].pivotY;
	brFrames[BR_FRAME_HANG].duration = batbrain_hang[0].duration;

	brFrames[BR_FRAME_FLY].tileOffset = 0;
	brFrames[BR_FRAME_FLY].pieceOffset = batbrain_fly[0].pieceOffset;
	brFrames[BR_FRAME_FLY].tileCount = batbrain_fly[0].tileCount;
	brFrames[BR_FRAME_FLY].pieceCount = batbrain_fly[0].pieceCount;
	brFrames[BR_FRAME_FLY].pivotX = batbrain_fly[0].pivotX;
	brFrames[BR_FRAME_FLY].pivotY = batbrain_fly[0].pivotY;
	brFrames[BR_FRAME_FLY].duration = batbrain_fly[0].duration;

	curFrame = BR_FRAME_HANG;

	if (*ghz_batbrain_count_p != BATBRAIN_COUNT) return;

	batbrainType.entries = (const void *)ghz_batbrain_xy;
	batbrainArenaDesc.entries = (const void *)ghz_batbrain_xy;
	batbrainArenaDesc.tilePixels = batbrain_tiles;

	if (obj_arena_register(&batbrainArenaDesc) == ARENA_INVALID_SLOT) return;
}

void batbrain_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	uint16_t i;
	uint16_t lastFrame = BR_FRAME_HANG;
	(void)sonicFrameIndex;

	if (!batbrainInited) batbrain_lazy_init();

	for (i = 0; i < BATBRAIN_COUNT; i++) {
		const BatbrainEntry *e = &ghz_batbrain_xy[i];

		if (badnik_is_destroyed(destroyed, i)) continue;

		switch (brState[i]) {
		case BR_STATE_HANG: {
			/* Batbrain.c:123-124: within range AND player at/below self. */
			int32_t dx = sonicWorldX - e->x;
			if (dx < 0) dx = -dx;
			if (dx < BR_TRIGGER_RANGE && sonicWorldY >= e->y) {
				if (++brHangTimer[i] >= BR_HANG_DELAY) {
					brState[i] = BR_STATE_DROP;
					brHangTimer[i] = 0;
					brVelY[i] = 0;
					brOffY[i] = 0;
					brTargetY[i] = sonicWorldY;
					brDir[i] = (uint8_t)(sonicWorldX >= e->x);
				}
			} else {
				brHangTimer[i] = 0;
			}
			lastFrame = BR_FRAME_HANG;
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
			/* VRAM ART BUDGET: "fall"'s 2-frame drop transition is gone
			 * (this file's own top comment) -- held on BR_FRAME_HANG through
			 * the whole drop instead of alternating a distinct falling
			 * pose. */
			lastFrame = BR_FRAME_HANG;
			break;
		}
		case BR_STATE_FLY: {
			brFlyTimer[i]++;
			brOffX[i] = (int16_t)(brDir[i]
			           ? -(brFlyTimer[i] * BR_FLY_SPEED)
			           :  (brFlyTimer[i] * BR_FLY_SPEED));
			if (brFlyTimer[i] * BR_FLY_SPEED >= BR_FLY_RANGE) {
				brState[i] = BR_STATE_RETURN;
			}
			lastFrame = BR_FRAME_FLY;
			break;
		}
		default: /* BR_STATE_RETURN */
			brVelY[i] -= BR_RETURN_GRAVITY;
			brOffY[i] += brVelY[i];
			if (brOffY[i] <= 0) {
				brState[i] = BR_STATE_HANG;
				brOffX[i] = 0; brOffY[i] = 0; brVelY[i] = 0;
			}
			lastFrame = BR_FRAME_FLY;
			break;
		}
	}

	/* One shared request per tick (see motobug.c's own comment on why) --
	 * the LAST instance processed this tick wins if several disagree, same
	 * "last request wins, self-correcting" rule every other class in this
	 * batch follows for the same reason -- now just a plain variable write
	 * (both poses are simultaneously resident, nothing to stream/select). */
	curFrame = lastFrame;
}

uint16_t batbrain_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                       uint16_t camX, uint16_t camY,
                       int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	if (!batbrainInited || !brLive) return 0;
	return obj_type_draw(&batbrainType, list, firstIndex, firstLink, BATBRAIN_SPRITE_CAP,
	                     camX, camY, sonicWorldX, sonicWorldY, sonicFrameIndex);
}
