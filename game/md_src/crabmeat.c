#include "crabmeat.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_sprite.h"
#include "obj_pool.h"
#include "crabmeat_data.h"
#include "badnik_base.h"
#include "assets_gen.h"

#define CRABMEAT_COUNT 11   /* Crabmeat_Serialize is empty, Crabmeat.c:215 -- position-only record */

typedef struct { int16_t x, y; } CrabmeatEntry;

static const uint16_t *const ghz_crabmeat_count_p = ASSET_GHZ_CRABMEATS;
static const CrabmeatEntry *const ghz_crabmeat_xy =
	(const CrabmeatEntry *)((const uint8_t *)ASSET_GHZ_CRABMEATS + 2);
static const uint32_t *const crabmeat_tiles = ASSET_CRABMEAT_TILES;

/* Crabmeat.c:62-65 (Crabmeat_StageLoad): hitboxBadnik, symmetric. */
#define CM_HB_L (-14)
#define CM_HB_T (-14)
#define CM_HB_R   14
#define CM_HB_B   14

/* Crabmeat_State_Init's velocity.x (Crabmeat.c:117): 0x8000, 0.5px/tick --
 * modelled as "1px every 2 ticks" (integer division) rather than
 * fractional accumulation. Crabmeat_State_Moving's own timer>=128 cap
 * (Crabmeat.c:129) IS the real patrol distance (bounded duration
 * regardless of real ledge sensing) -- decomp-exact, not invented the way
 * motobug.c's own MB_AMPLITUDE had to be: 128 ticks * 0.5px/tick = 64px.
 * Crabmeat_State_Shoot's own timer>=60 pause (Crabmeat.c:152) is likewise
 * decomp-exact. Projectiles cut -- see crabmeat.h's own header comment.
 *
 * VRAM ART BUDGET (art-budget trim task, 2026-08-18): tools/convert_objects.py's
 * CRABMEAT_ART now converts only "walk", subsampled to CM_WALK_FRAMES=2 of
 * its real 7 patrol frames (half the cycle apart) -- "stand" (never
 * referenced by this port's own state machine) and "shoot"/"projectile"
 * (crabmeat.h's own header comment: cut) are gone entirely. The old
 * obj_anim_window STREAM (48 tiles, cycling one frame at a time through a
 * 15-frame "walk"+"shoot" set) is gone too: crabmeat_walk[] is now a small,
 * PERMANENTLY VRAM-resident set (also 48 tiles total -- this class's own
 * per-frame cost happens not to change, only the mechanism does). The
 * visible cost: crabmeat_pose() below just HOLDS the current walk frame
 * through the whole SHOOT pause instead of animating a dedicated firing
 * pose -- the same trade newtron.c's own SHOOT state makes, and motobug.c's
 * own TURN hold. */
#define CM_MOVE_TICKS  128
#define CM_AMPLITUDE   64
#define CM_SHOOT_HOLD  60
#define CM_WALK_FRAMES  2
#define CM_STEP_TICKS   8   /* ticks each of the 2 walk frames holds --
                             * same arbitrary, reasonable cadence
                             * motobug.c's own MB_STEP_TICKS uses */
#define CM_CYCLE (2 * (CM_MOVE_TICKS + CM_SHOOT_HOLD))

static uint8_t  destroyed[(CRABMEAT_COUNT + 7) / 8];
static uint32_t crabmeatTick;

/* Shared class-wide pose, recomputed once per frame in crabmeat_tick() --
 * see motobug.c's own comment on why this is once-per-class, not
 * once-per-instance. */
static int32_t curOffX;
static uint16_t curFrame;
static uint8_t  curFlipH;

/* Rebased working copy of crabmeat_walk[] -- see motobug.c's own comment on
 * mbFrames for why field-by-field, and why tileOffset alone is patched by
 * crabmeat_arena_onBase() rather than the whole struct. Plain whole-sheet
 * arena residency (md_src/obj_generic.h's ArenaClassDesc) -- no
 * ObjAnimWindow, no per-frame streaming. */
static ObjFrame  cmFrames[CM_WALK_FRAMES];
static uint16_t  cmBase;
static uint8_t   cmLive;

/* PRECOMPUTED PIECE TEMPLATES (Job 1, lever 1, this task) -- see
 * obj_sprite.h's own top-of-section comment. Sized CM_WALK_FRAMES *
 * CRABMEAT_MAX_FRAME_PIECES, an upper bound on crabmeat_pieces[]'s own real
 * length that stays correct even if a future regeneration changes the exact
 * per-frame piece count (every frame here happens to use exactly
 * CRABMEAT_MAX_FRAME_PIECES today, so this is tight, not just safe).
 * Rebuilt -- not just written once at boot -- every time
 * crabmeat_arena_onBase() below fires, since that is the only time
 * f->tileOffset (baked into each template's own .attr) can change. */
static ObjPieceTemplate cmTemplatesH0[CM_WALK_FRAMES * CRABMEAT_MAX_FRAME_PIECES];
static ObjPieceTemplate cmTemplatesH1[CM_WALK_FRAMES * CRABMEAT_MAX_FRAME_PIECES];

static void crabmeat_rebuild_templates(void)
{
	uint8_t i;
	for (i = 0; i < CM_WALK_FRAMES; i++) {
		const ObjFrame *f = &cmFrames[i];
		/* drawPriority literal 0 here matches crabmeatType's own drawPriority
		 * field below -- see obj_build_piece_templates()'s own signature. */
		obj_build_piece_templates(&cmTemplatesH0[f->pieceOffset], &crabmeat_pieces[f->pieceOffset], f->pieceCount,
		                          f->tileOffset, CRABMEAT_PAL, 0, f->pivotX, f->pivotY, 0, 0);
		obj_build_piece_templates(&cmTemplatesH1[f->pieceOffset], &crabmeat_pieces[f->pieceOffset], f->pieceCount,
		                          f->tileOffset, CRABMEAT_PAL, 0, f->pivotX, f->pivotY, 1, 0);
	}
}

static void crabmeat_pose(uint32_t tick, int32_t *offX, uint16_t *frame, uint8_t *flipH)
{
	int32_t phase = (int32_t)(tick % CM_CYCLE);

	if (phase < CM_MOVE_TICKS) {
		*offX = -(phase / 2);
		*frame = (uint16_t)((phase / CM_STEP_TICKS) & 1);
		*flipH = 0;
	} else if (phase < CM_MOVE_TICKS + CM_SHOOT_HOLD) {
		*offX = -CM_AMPLITUDE;
		*frame = curFrame;   /* hold whichever walk frame was showing */
		*flipH = 0;
	} else if (phase < 2 * CM_MOVE_TICKS + CM_SHOOT_HOLD) {
		int32_t p2 = phase - CM_MOVE_TICKS - CM_SHOOT_HOLD;
		*offX = -CM_AMPLITUDE + (p2 / 2);
		*frame = (uint16_t)((p2 / CM_STEP_TICKS) & 1);
		*flipH = 1;
	} else {
		*offX = 0;
		*frame = curFrame;   /* hold whichever walk frame was showing */
		*flipH = 1;
	}
}

static ObjDrawDecision crabmeat_decide(void *st, uint16_t i, int16_t ex, int16_t ey,
                                       int16_t sonicWorldX, int16_t sonicWorldY,
                                       uint16_t sonicFrameIndex)
{
	ObjDrawDecision d;
	int16_t bx = (int16_t)(ex + curOffX);
	(void)st;

	/* SPRITE-VS-HITBOX DRIFT fix (Job 2, this task): the SAME curOffX the
	 * hitbox test above already uses (Crabmeat_State_Moving's patrol,
	 * Crabmeat.c:117-129 -- see this file's own top comment), now also
	 * carried on the draw decision so obj_type_draw() moves the sprite with
	 * it -- see obj_data.h's own ObjDrawDecision comment. */
	d.flipH = 0; d.flipV = 0; d.offX = (int16_t)curOffX; d.offY = 0; d.frame = OBJ_SKIP;

	if (badnik_decide_common(destroyed, i, bx, ey, CM_HB_L, CM_HB_T, CM_HB_R, CM_HB_B,
	                         sonicWorldX, sonicWorldY, sonicFrameIndex))
		return d;

	if (!cmLive) return d;
	d.frame = curFrame;   /* index into cmFrames[CM_WALK_FRAMES] -- always in
	                       * range, crabmeat_pose() only ever writes 0 or 1 */
	d.flipH = curFlipH;
	return d;
}

static ObjTypeDesc crabmeatType = {
	(const void *)0, sizeof(CrabmeatEntry), CRABMEAT_COUNT, ghz_crabmeat_count_p,
	(const uint32_t *)0, 0,
	cmFrames, crabmeat_pieces,
	OBJ_PRI_BADNIK, CRABMEAT_PAL, 0,
	CM_AMPLITUDE + 32,
	crabmeat_decide, (void *)0,
	cmTemplatesH0, cmTemplatesH1
};

/* Plain whole-sheet VRAM residency -- replaces the old obj_anim_window
 * streaming reservation. See motobug.c's own comment on the identical
 * pattern. */
static void crabmeat_arena_onBase(uint16_t base);
static void crabmeat_arena_onLive(uint8_t live) { cmLive = live; }

static ArenaClassDesc crabmeatArenaDesc = {
	(const void *)0, sizeof(CrabmeatEntry), CRABMEAT_COUNT,
	(const uint32_t *)0, CM_WALK_FRAMES * CRABMEAT_MAX_FRAME_TILES,
	(int16_t)(ARENA_LOOKAHEAD_X(CM_WALK_FRAMES * CRABMEAT_MAX_FRAME_TILES) + CM_AMPLITUDE + 32),
	OBJ_PRI_BADNIK,
	crabmeat_arena_onBase, crabmeat_arena_onLive
};

static void crabmeat_arena_onBase(uint16_t base)
{
	uint8_t i;
	cmBase = base;
	for (i = 0; i < CM_WALK_FRAMES; i++)
		cmFrames[i].tileOffset = (uint16_t)(base + crabmeat_walk[i].tileOffset);
	crabmeat_rebuild_templates();   /* Job 1, lever 1: only tileOffset changed above, and only here */
}

static uint8_t crabmeatInited;

static void crabmeat_lazy_init(void)
{
	uint16_t i;

	crabmeatInited = 1;
	crabmeatTick = 0;
	curOffX = 0; curFrame = 0; curFlipH = 0;
	for (i = 0; i < sizeof(destroyed); i++) destroyed[i] = 0;

	for (i = 0; i < CM_WALK_FRAMES; i++) {
		cmFrames[i].tileOffset = 0;
		cmFrames[i].pieceOffset = crabmeat_walk[i].pieceOffset;
		cmFrames[i].tileCount = crabmeat_walk[i].tileCount;
		cmFrames[i].pieceCount = crabmeat_walk[i].pieceCount;
		cmFrames[i].pivotX = crabmeat_walk[i].pivotX;
		cmFrames[i].pivotY = crabmeat_walk[i].pivotY;
		cmFrames[i].duration = crabmeat_walk[i].duration;
	}

	if (*ghz_crabmeat_count_p != CRABMEAT_COUNT) return;

	crabmeatType.entries = (const void *)ghz_crabmeat_xy;
	crabmeatArenaDesc.entries = (const void *)ghz_crabmeat_xy;
	crabmeatArenaDesc.tilePixels = crabmeat_tiles;

	if (obj_arena_register(&crabmeatArenaDesc) == ARENA_INVALID_SLOT) return;
}

void crabmeat_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	(void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;
	if (!crabmeatInited) crabmeat_lazy_init();
	crabmeatTick++;
	crabmeat_pose(crabmeatTick, &curOffX, &curFrame, &curFlipH);
}

uint16_t crabmeat_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                       uint16_t camX, uint16_t camY,
                       int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	if (!crabmeatInited || !cmLive) return 0;
	return obj_type_draw(&crabmeatType, list, firstIndex, firstLink, CRABMEAT_SPRITE_CAP,
	                     camX, camY, sonicWorldX, sonicWorldY, sonicFrameIndex);
}
