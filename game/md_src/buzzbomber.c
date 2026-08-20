#include "buzzbomber.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_sprite.h"
#include "obj_pool.h"
#include "buzzbomber_data.h"
#include "badnik_base.h"
#include "assets_gen.h"

#define BUZZBOMBER_COUNT 18

/* BUZZBOMBER_SCENE, tools/convert_objects.py: row_fmt ">hhBB", fields
 * (x_px, y_px, direction, shotRange) -- BuzzBomber_Serialize (BuzzBomber.c:
 * 316-320). shotRange is read but unused (only feeds the cut proximity
 * detector, see buzzbomber.h's own header comment). */
typedef struct { int16_t x, y; uint8_t direction, shotRange; } BuzzbomberEntry;

static const uint16_t *const ghz_buzzbomber_count_p = ASSET_GHZ_BUZZBOMBERS;
static const BuzzbomberEntry *const ghz_buzzbomber_xy =
	(const BuzzbomberEntry *)((const uint8_t *)ASSET_GHZ_BUZZBOMBERS + 2);
static const uint32_t *const buzzbomber_tiles = ASSET_BUZZBOMBER_TILES;

/* BuzzBomber.c:91-94 (BuzzBomber_StageLoad): hitboxBadnik, symmetric. */
#define BB_HB_L (-24)
#define BB_HB_T (-12)
#define BB_HB_R   24
#define BB_HB_B   12

/* BuzzBomber_State_Init's velocity.x magnitude (BuzzBomber.c:161-163):
 * 0x40000, 4px/tick. self->timer=128 (BuzzBomber.c:49) flying ticks each
 * leg, self->timer=60 (BuzzBomber.c:178) idle ticks at each end
 * (BuzzBomber_State_Flying/Idle, BuzzBomber.c:169-207) -- all decomp-exact,
 * no invented amplitude needed (contrast motobug.c's own MB_AMPLITUDE): a
 * BuzzBomber never touches terrain in the original either. */
#define BB_SPEED      4
#define BB_FLY_TICKS  128
#define BB_IDLE_TICKS 60
#define BB_AMPLITUDE  (BB_SPEED * BB_FLY_TICKS)
#define BB_CYCLE      (2 * (BB_FLY_TICKS + BB_IDLE_TICKS))

static uint8_t  destroyed[(BUZZBOMBER_COUNT + 7) / 8];
static uint32_t buzzbomberTick;

/* VRAM ART BUDGET (art-budget trim task, 2026-08-18): buzzbomber.c has only
 * ever drawn "fly" (a single static cruising pose -- even under the old
 * streaming window, buzzbomber_lazy_init() only ever copied
 * buzzbomber_fly[0]); tools/convert_objects.py's BUZZBOMBER_ART now converts
 * only that one frame, so this is not a new cut, just the pre-existing
 * choice made permanent. Plain whole-sheet arena residency (18 tiles)
 * replaces the old 38-tile obj_anim_window streaming reservation -- no
 * ObjAnimWindow, no per-frame streaming, same pattern motobug.c/crabmeat.c
 * use. */
static ObjFrame  bbFrames[1];
static uint16_t  bbBase;
static uint8_t   bbLive;

/* PRECOMPUTED PIECE TEMPLATES (Job 1, lever 1, this task) -- see
 * obj_sprite.h's own top-of-section comment and crabmeat.c's own identical
 * pattern (this file's twin). Sized 1 * BUZZBOMBER_MAX_FRAME_PIECES (one
 * frame only), an upper bound on buzzbomber_pieces[]'s own real length.
 * Rebuilt every time buzzbomber_arena_onBase() below fires. */
static ObjPieceTemplate bbTemplatesH0[BUZZBOMBER_MAX_FRAME_PIECES];
static ObjPieceTemplate bbTemplatesH1[BUZZBOMBER_MAX_FRAME_PIECES];

static void buzzbomber_rebuild_templates(void)
{
	const ObjFrame *f = &bbFrames[0];
	/* drawPriority literal 0 here matches buzzbomberType's own drawPriority
	 * field below. */
	obj_build_piece_templates(&bbTemplatesH0[f->pieceOffset], &buzzbomber_pieces[f->pieceOffset], f->pieceCount,
	                          f->tileOffset, BUZZBOMBER_PAL, 0, f->pivotX, f->pivotY, 0, 0);
	obj_build_piece_templates(&bbTemplatesH1[f->pieceOffset], &buzzbomber_pieces[f->pieceOffset], f->pieceCount,
	                          f->tileOffset, BUZZBOMBER_PAL, 0, f->pivotX, f->pivotY, 1, 0);
}

/* Magnitude of the shared class-wide phase, sign applied per-instance by
 * decide() off that instance's own scene-authored `direction` byte
 * (BuzzBomber.c:160-163: direction==0 flies left first, direction!=0 flies
 * right first) -- see motobug.c's own comment for why the phase itself is
 * one shared class-wide value, and buzzbomber.h's header comment for why
 * per-instance direction breaks that sharing for offset (not for pose:
 * BuzzBomber's own body is a single static frame, so pose never diverges). */
static int32_t curMag;   /* 0..BB_AMPLITUDE, unsigned distance from spawn */

static void buzzbomber_pose(uint32_t tick, int32_t *mag)
{
	int32_t phase = (int32_t)(tick % BB_CYCLE);

	if (phase < BB_FLY_TICKS)
		*mag = phase * BB_SPEED;
	else if (phase < BB_FLY_TICKS + BB_IDLE_TICKS)
		*mag = BB_AMPLITUDE;
	else if (phase < 2 * BB_FLY_TICKS + BB_IDLE_TICKS)
		*mag = BB_AMPLITUDE - (phase - BB_FLY_TICKS - BB_IDLE_TICKS) * BB_SPEED;
	else
		*mag = 0;
}

static ObjDrawDecision buzzbomber_decide(void *st, uint16_t i, int16_t ex, int16_t ey,
                                         int16_t sonicWorldX, int16_t sonicWorldY,
                                         uint16_t sonicFrameIndex)
{
	ObjDrawDecision d;
	const BuzzbomberEntry *e = &ghz_buzzbomber_xy[i];
	int8_t sign = (e->direction & 1) ? 1 : -1;
	int16_t bx = (int16_t)(ex + sign * curMag);
	(void)st;

	/* SPRITE-VS-HITBOX DRIFT fix (Job 2, this task): the SAME sign*curMag
	 * the hitbox test above already uses (BuzzBomber_State_Flying/Idle,
	 * BuzzBomber.c:169-207 -- see this file's own top comment), now also
	 * carried on the draw decision so obj_type_draw() moves the sprite with
	 * it -- see obj_data.h's own ObjDrawDecision comment. */
	d.flipH = 0; d.flipV = 0; d.offX = (int16_t)(sign * curMag); d.offY = 0; d.frame = OBJ_SKIP;

	if (badnik_decide_common(destroyed, i, bx, ey, BB_HB_L, BB_HB_T, BB_HB_R, BB_HB_B,
	                         sonicWorldX, sonicWorldY, sonicFrameIndex))
		return d;

	if (!bbLive) return d;
	d.frame = 0;   /* bbFrames[1] -- only ever one row */
	d.flipH = (uint8_t)(sign > 0);
	return d;
}

static ObjTypeDesc buzzbomberType = {
	(const void *)0, sizeof(BuzzbomberEntry), BUZZBOMBER_COUNT, ghz_buzzbomber_count_p,
	(const uint32_t *)0, 0,
	bbFrames, buzzbomber_pieces,
	OBJ_PRI_BADNIK, BUZZBOMBER_PAL, 0,
	BB_AMPLITUDE + 32,
	buzzbomber_decide, (void *)0,
	bbTemplatesH0, bbTemplatesH1
};

/* Plain whole-sheet VRAM residency -- replaces the old obj_anim_window
 * streaming reservation. See motobug.c's own comment on the identical
 * pattern. */
static void buzzbomber_arena_onBase(uint16_t base);
static void buzzbomber_arena_onLive(uint8_t live) { bbLive = live; }

static ArenaClassDesc buzzbomberArenaDesc = {
	(const void *)0, sizeof(BuzzbomberEntry), BUZZBOMBER_COUNT,
	(const uint32_t *)0, BUZZBOMBER_MAX_FRAME_TILES,
	(int16_t)(ARENA_LOOKAHEAD_X(BUZZBOMBER_MAX_FRAME_TILES) + BB_AMPLITUDE + 32),
	OBJ_PRI_BADNIK,
	buzzbomber_arena_onBase, buzzbomber_arena_onLive
};

static void buzzbomber_arena_onBase(uint16_t base)
{
	bbBase = base;
	bbFrames[0].tileOffset = (uint16_t)(base + buzzbomber_fly[0].tileOffset);
	buzzbomber_rebuild_templates();   /* Job 1, lever 1: only tileOffset changed above, and only here */
}

static uint8_t buzzbomberInited;

static void buzzbomber_lazy_init(void)
{
	uint16_t i;

	buzzbomberInited = 1;
	buzzbomberTick = 0;
	curMag = 0;
	for (i = 0; i < sizeof(destroyed); i++) destroyed[i] = 0;

	bbFrames[0].tileOffset = 0;
	bbFrames[0].pieceOffset = buzzbomber_fly[0].pieceOffset;
	bbFrames[0].tileCount = buzzbomber_fly[0].tileCount;
	bbFrames[0].pieceCount = buzzbomber_fly[0].pieceCount;
	bbFrames[0].pivotX = buzzbomber_fly[0].pivotX;
	bbFrames[0].pivotY = buzzbomber_fly[0].pivotY;
	bbFrames[0].duration = buzzbomber_fly[0].duration;

	if (*ghz_buzzbomber_count_p != BUZZBOMBER_COUNT) return;

	buzzbomberType.entries = (const void *)ghz_buzzbomber_xy;
	buzzbomberArenaDesc.entries = (const void *)ghz_buzzbomber_xy;
	buzzbomberArenaDesc.tilePixels = buzzbomber_tiles;

	if (obj_arena_register(&buzzbomberArenaDesc) == ARENA_INVALID_SLOT) return;
}

void buzzbomber_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	(void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;
	if (!buzzbomberInited) buzzbomber_lazy_init();
	buzzbomberTick++;
	buzzbomber_pose(buzzbomberTick, &curMag);
}

uint16_t buzzbomber_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                         uint16_t camX, uint16_t camY,
                         int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	if (!buzzbomberInited || !bbLive) return 0;
	return obj_type_draw(&buzzbomberType, list, firstIndex, firstLink, BUZZBOMBER_SPRITE_CAP,
	                     camX, camY, sonicWorldX, sonicWorldY, sonicFrameIndex);
}
