#include "crabmeat.h"
#include "obj_data.h"
#include "obj_generic.h"
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
 * decomp-exact. Projectiles cut -- see crabmeat.h's own header comment. */
#define CM_MOVE_TICKS  128
#define CM_AMPLITUDE   64
#define CM_SHOOT_HOLD  60
#define CM_WALK_FRAMES  7
#define CM_SHOOT_FRAMES 8
#define CM_CYCLE (2 * (CM_MOVE_TICKS + CM_SHOOT_HOLD))

static uint8_t  destroyed[(CRABMEAT_COUNT + 7) / 8];
static uint32_t crabmeatTick;

/* Shared class-wide pose, recomputed once per frame in crabmeat_tick() --
 * see motobug.c's own comment on why this is once-per-class, not
 * once-per-instance. */
static int32_t curOffX;
static uint16_t curWinFrame;
static uint8_t  curFlipH;

static ObjFrame  cmFramesRaw[CM_WALK_FRAMES + CM_SHOOT_FRAMES];
static ObjAnimWindow *cmWindow;

static void crabmeat_pose(uint32_t tick, int32_t *offX, uint16_t *frame, uint8_t *flipH)
{
	int32_t phase = (int32_t)(tick % CM_CYCLE);

	if (phase < CM_MOVE_TICKS) {
		*offX = -(phase / 2);
		*frame = (uint16_t)(phase % CM_WALK_FRAMES);
		*flipH = 0;
	} else if (phase < CM_MOVE_TICKS + CM_SHOOT_HOLD) {
		int32_t p2 = phase - CM_MOVE_TICKS;
		*offX = -CM_AMPLITUDE;
		*frame = (uint16_t)(CM_WALK_FRAMES + (p2 * CM_SHOOT_FRAMES) / CM_SHOOT_HOLD);
		*flipH = 0;
	} else if (phase < 2 * CM_MOVE_TICKS + CM_SHOOT_HOLD) {
		int32_t p2 = phase - CM_MOVE_TICKS - CM_SHOOT_HOLD;
		*offX = -CM_AMPLITUDE + (p2 / 2);
		*frame = (uint16_t)(p2 % CM_WALK_FRAMES);
		*flipH = 1;
	} else {
		int32_t p2 = phase - 2 * CM_MOVE_TICKS - CM_SHOOT_HOLD;
		*offX = 0;
		*frame = (uint16_t)(CM_WALK_FRAMES + (p2 * CM_SHOOT_FRAMES) / CM_SHOOT_HOLD);
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

	d.flipH = 0; d.flipV = 0; d.frame = OBJ_SKIP;

	if (badnik_decide_common(destroyed, i, bx, ey, CM_HB_L, CM_HB_T, CM_HB_R, CM_HB_B,
	                         sonicWorldX, sonicWorldY, sonicFrameIndex))
		return d;

	if (!obj_anim_window_live(cmWindow)) return d;
	d.frame = 0;
	d.flipH = curFlipH;
	return d;
}

static ObjTypeDesc crabmeatType = {
	(const void *)0, sizeof(CrabmeatEntry), CRABMEAT_COUNT, ghz_crabmeat_count_p,
	(const uint32_t *)0, 0,
	(const ObjFrame *)0, crabmeat_pieces,
	OBJ_PRI_BADNIK, CRABMEAT_PAL, 0,
	CM_AMPLITUDE + 32,
	crabmeat_decide, (void *)0
};

static ObjAnimWindowDesc crabmeatAnimDesc = {
	(const void *)0, sizeof(CrabmeatEntry), CRABMEAT_COUNT,
	(int16_t)(ARENA_LOOKAHEAD_X(CRABMEAT_MAX_FRAME_TILES) + CM_AMPLITUDE + 32),
	OBJ_PRI_BADNIK,
	(const uint32_t *)0, cmFramesRaw, CM_WALK_FRAMES + CM_SHOOT_FRAMES, CRABMEAT_MAX_FRAME_TILES
};

static uint8_t crabmeatInited;

static void crabmeat_lazy_init(void)
{
	uint16_t i;

	crabmeatInited = 1;
	crabmeatTick = 0;
	curOffX = 0; curWinFrame = 0; curFlipH = 0;
	for (i = 0; i < sizeof(destroyed); i++) destroyed[i] = 0;

	for (i = 0; i < CM_WALK_FRAMES; i++) cmFramesRaw[i] = crabmeat_walk[i];
	for (i = 0; i < CM_SHOOT_FRAMES; i++) cmFramesRaw[CM_WALK_FRAMES + i] = crabmeat_shoot[i];

	if (*ghz_crabmeat_count_p != CRABMEAT_COUNT) return;

	crabmeatType.entries = (const void *)ghz_crabmeat_xy;
	crabmeatAnimDesc.entries = (const void *)ghz_crabmeat_xy;
	crabmeatAnimDesc.sheetPixels = crabmeat_tiles;

	cmWindow = obj_anim_window_register(&crabmeatAnimDesc);
	if (!cmWindow) return;
	crabmeatType.frames = obj_anim_window_frames(cmWindow);
}

void crabmeat_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	(void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;
	if (!crabmeatInited) crabmeat_lazy_init();
	crabmeatTick++;
	crabmeat_pose(crabmeatTick, &curOffX, &curWinFrame, &curFlipH);
	obj_anim_window_select(cmWindow, curWinFrame);
}

uint16_t crabmeat_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                       uint16_t camX, uint16_t camY,
                       int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	if (!crabmeatInited || !cmWindow) return 0;
	return obj_type_draw(&crabmeatType, list, firstIndex, firstLink, CRABMEAT_SPRITE_CAP,
	                     camX, camY, sonicWorldX, sonicWorldY, sonicFrameIndex);
}
