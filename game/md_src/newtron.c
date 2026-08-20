#include "newtron.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_sprite.h"
#include "obj_pool.h"
#include "newtron_data.h"
#include "badnik_base.h"
#include "assets_gen.h"

#define NEWTRON_COUNT 21
#define NEWTRON_SHOOT_TYPE 0   /* Newtron.h's NewtronTypes: SHOOT=0, FLY=1, PROJECTILE=2 (unused here) */
#define NEWTRON_FLY_TYPE   1

/* NEWTRON_SCENE, tools/convert_objects.py: row_fmt ">hhBB", fields
 * (x_px, y_px, type, direction) -- Newtron_Serialize (Newtron.c:339-343). */
typedef struct { int16_t x, y; uint8_t type, direction; } NewtronEntry;

static const uint16_t *const ghz_newtron_count_p = ASSET_GHZ_NEWTRONS;
static const NewtronEntry *const ghz_newtron_xy =
	(const NewtronEntry *)((const uint8_t *)ASSET_GHZ_NEWTRONS + 2);
static const uint32_t *const newtron_tiles = ASSET_NEWTRON_TILES;

/* Newtron.c:97-100 (Newtron_StageLoad): hitboxRange, the per-instance
 * proximity trigger both variants share (Newtron_State_CheckPlayerInRange,
 * Newtron.c:172-183). Newtron.c:79-82: hitboxShoot, the badnik hitbox BOTH
 * variants use too (Newtron_CheckPlayerCollisions, Newtron.c:118-127 --
 * "hitboxFly... goes unused in this object", that file's own comment). */
/* +-128 (Newtron.c:97-100) does not fit int8_t (max 127) -- badnik_touches_
 * sonic()'s hitbox parameters are int8_t throughout (every other class's
 * hitbox in this batch fits comfortably), so the +128 edge is narrowed by
 * 1px rather than widening one function's signature for a single class's
 * one field. Immaterial: 1px off a 256px-wide trigger box. */
#define NT_TRIG_L (-127)
#define NT_TRIG_T  (-64)
#define NT_TRIG_R   127
#define NT_TRIG_B    64
#define NT_HB_L  (-12)
#define NT_HB_T  (-14)
#define NT_HB_R    12
#define NT_HB_B    14

/* Newtron_State_Shoot's own tick thresholds (Newtron.c:250-274): fire at
 * 30 (projectile cut, see newtron.h), revert to idle pose at 45, deactivate
 * at 90 -- all decomp-exact. NT_FLY_TICKS/NT_FLY_SPEED: velocity 0x20000
 * (2px/tick) is decomp-exact (Newtron.c:222-224); the 128-tick duration is
 * this port's own choice (the real Fly state has no timer at all, it flies
 * until CheckOffScreen respawns it off-camera -- see newtron.h's own
 * comment on why floor-following flight could not be ported as-is). */
#define NT_SHOOT_FIRE  30
#define NT_SHOOT_IDLE  45
#define NT_SHOOT_END   90
#define NT_FLY_SPEED    2
#define NT_FLY_TICKS  128

#define NT_STATE_DORMANT 0
#define NT_STATE_ACTIVE  1

/* VRAM ART BUDGET (art-budget trim task, 2026-08-18): tools/convert_objects.py's
 * NEWTRON_ART now converts only "shootidle" and "fly" -- see that recipe's
 * own comment for why "shoot"/"flyidle"/"flyfall"/"flame"/"projectile" are
 * gone. Both kept poses are now PERMANENTLY, SIMULTANEOUSLY VRAM-resident
 * (31 tiles total, one arena tenant spanning both, replacing the old
 * 42-tile obj_anim_window streaming reservation), which removes a real,
 * pre-existing bug along the way: the old single shared streaming window
 * could only ever show ONE pose at a time class-wide, so an active SHOOT-
 * type Newtron and an active FLY-type Newtron on screen together would
 * fight over the one window, and whichever obj_anim_window_select() call
 * ran LAST in newtron_tick()'s per-instance loop that tick would leave
 * EVERY active instance -- including the other type -- showing its pose.
 * With both poses resident at once, newtron_decide() below now picks the
 * frame that matches each instance's own real, static `type` field
 * directly, per instance, every tick -- genuine state divergence, not a
 * shared animation phase, exactly the distinction md_src/obj_generic.h's own
 * top-of-file comment draws between the two. NT_FRAME_SHOOTIDLE/NT_FRAME_FLY
 * index ntFrames[NT_FRAME_COUNT] below, never anything computed from a
 * timer. */
#define NT_FRAME_SHOOTIDLE 0
#define NT_FRAME_FLY       1
#define NT_FRAME_COUNT     2

static uint8_t  destroyed[(NEWTRON_COUNT + 7) / 8];
static uint8_t  ntState[NEWTRON_COUNT];
static uint16_t ntTimer[NEWTRON_COUNT];
static uint8_t  ntDir[NEWTRON_COUNT];    /* 0 = +X (right), 1 = -X (left) */
static int16_t  ntOffX[NEWTRON_COUNT];

/* Rebased working copy of {newtron_shootidle[0], newtron_fly[0]} -- see
 * motobug.c's own comment on mbFrames for why field-by-field. Plain
 * whole-sheet arena residency, no ObjAnimWindow. */
static ObjFrame  ntFrames[NT_FRAME_COUNT];
static uint16_t  ntBase;
static uint8_t   ntLive;

/* PRECOMPUTED PIECE TEMPLATES (Job 1, lever 1, this task) -- see
 * obj_sprite.h's own top-of-section comment and crabmeat.c's own identical
 * pattern (this file's twin). Sized NT_FRAME_COUNT * NEWTRON_MAX_FRAME_
 * PIECES, an upper bound on newtron_pieces[]'s own real length (the two
 * source frames -- shootidle/fly -- don't each use exactly
 * NEWTRON_MAX_FRAME_PIECES, so this over-allocates slightly; harmless,
 * see this file's own top comment on how pieceOffset indexing still works
 * correctly regardless). Rebuilt every time newtron_arena_onBase() below
 * fires. */
static ObjPieceTemplate ntTemplatesH0[NT_FRAME_COUNT * NEWTRON_MAX_FRAME_PIECES];
static ObjPieceTemplate ntTemplatesH1[NT_FRAME_COUNT * NEWTRON_MAX_FRAME_PIECES];

static void newtron_rebuild_templates(void)
{
	uint8_t i;
	for (i = 0; i < NT_FRAME_COUNT; i++) {
		const ObjFrame *f = &ntFrames[i];
		/* drawPriority literal 0 here matches newtronType's own drawPriority
		 * field below. */
		obj_build_piece_templates(&ntTemplatesH0[f->pieceOffset], &newtron_pieces[f->pieceOffset], f->pieceCount,
		                          f->tileOffset, NEWTRON_PAL, 0, f->pivotX, f->pivotY, 0, 0);
		obj_build_piece_templates(&ntTemplatesH1[f->pieceOffset], &newtron_pieces[f->pieceOffset], f->pieceCount,
		                          f->tileOffset, NEWTRON_PAL, 0, f->pivotX, f->pivotY, 1, 0);
	}
}

static ObjDrawDecision newtron_decide(void *st, uint16_t i, int16_t ex, int16_t ey,
                                      int16_t sonicWorldX, int16_t sonicWorldY,
                                      uint16_t sonicFrameIndex)
{
	ObjDrawDecision d;
	int16_t bx;
	const NewtronEntry *e = &ghz_newtron_xy[i];
	(void)st;

	d.flipH = 0; d.flipV = 0; d.offX = 0; d.offY = 0; d.frame = OBJ_SKIP;
	if (ntState[i] != NT_STATE_ACTIVE) return d;

	bx = (int16_t)(ex + ntOffX[i]);

	/* SPRITE-VS-HITBOX DRIFT fix (Job 2, this task): the SAME ntOffX[i] the
	 * hitbox test above already uses (Newtron_State_Fly's own per-instance
	 * flight offset, Newtron.c:222-224 -- see this file's own top comment),
	 * now also carried on the draw decision so obj_type_draw() moves the
	 * sprite with it -- see obj_data.h's own ObjDrawDecision comment.
	 * SHOOT-type instances never advance ntOffX[i] past 0 (this file's own
	 * newtron_tick() comment), so this is a no-op for them, matching that
	 * they never move in the original either. */
	d.offX = ntOffX[i];

	if (badnik_decide_common(destroyed, i, bx, ey, NT_HB_L, NT_HB_T, NT_HB_R, NT_HB_B,
	                         sonicWorldX, sonicWorldY, sonicFrameIndex))
		return d;

	if (!ntLive) return d;
	d.frame = (e->type == NEWTRON_FLY_TYPE) ? NT_FRAME_FLY : NT_FRAME_SHOOTIDLE;
	d.flipH = ntDir[i];
	return d;
}

static ObjTypeDesc newtronType = {
	(const void *)0, sizeof(NewtronEntry), NEWTRON_COUNT, ghz_newtron_count_p,
	(const uint32_t *)0, 0,
	ntFrames, newtron_pieces,
	OBJ_PRI_BADNIK, NEWTRON_PAL, 0,
	NT_FLY_SPEED * NT_FLY_TICKS + 32,
	newtron_decide, (void *)0,
	ntTemplatesH0, ntTemplatesH1
};

/* Plain whole-sheet VRAM residency -- see motobug.c's own comment on the
 * identical pattern. */
static void newtron_arena_onBase(uint16_t base);
static void newtron_arena_onLive(uint8_t live) { ntLive = live; }

static ArenaClassDesc newtronArenaDesc = {
	(const void *)0, sizeof(NewtronEntry), NEWTRON_COUNT,
	(const uint32_t *)0, NEWTRON_MAX_FRAME_TILES + 10,   /* shootidle 21 + fly 10 = 31 */
	(int16_t)(ARENA_LOOKAHEAD_X((uint16_t)(NEWTRON_MAX_FRAME_TILES + 10)) + NT_FLY_SPEED * NT_FLY_TICKS + 32),
	OBJ_PRI_BADNIK,
	newtron_arena_onBase, newtron_arena_onLive
};

static void newtron_arena_onBase(uint16_t base)
{
	ntBase = base;
	ntFrames[NT_FRAME_SHOOTIDLE].tileOffset = (uint16_t)(base + newtron_shootidle[0].tileOffset);
	ntFrames[NT_FRAME_FLY].tileOffset = (uint16_t)(base + newtron_fly[0].tileOffset);
	newtron_rebuild_templates();   /* Job 1, lever 1: only tileOffset changed above, and only here */
}

static uint8_t newtronInited;

static void newtron_lazy_init(void)
{
	uint16_t i;

	newtronInited = 1;
	for (i = 0; i < sizeof(destroyed); i++) destroyed[i] = 0;
	for (i = 0; i < NEWTRON_COUNT; i++) { ntState[i] = NT_STATE_DORMANT; ntTimer[i] = 0; ntDir[i] = 0; ntOffX[i] = 0; }

	ntFrames[NT_FRAME_SHOOTIDLE].tileOffset = 0;
	ntFrames[NT_FRAME_SHOOTIDLE].pieceOffset = newtron_shootidle[0].pieceOffset;
	ntFrames[NT_FRAME_SHOOTIDLE].tileCount = newtron_shootidle[0].tileCount;
	ntFrames[NT_FRAME_SHOOTIDLE].pieceCount = newtron_shootidle[0].pieceCount;
	ntFrames[NT_FRAME_SHOOTIDLE].pivotX = newtron_shootidle[0].pivotX;
	ntFrames[NT_FRAME_SHOOTIDLE].pivotY = newtron_shootidle[0].pivotY;
	ntFrames[NT_FRAME_SHOOTIDLE].duration = newtron_shootidle[0].duration;

	ntFrames[NT_FRAME_FLY].tileOffset = 0;
	ntFrames[NT_FRAME_FLY].pieceOffset = newtron_fly[0].pieceOffset;
	ntFrames[NT_FRAME_FLY].tileCount = newtron_fly[0].tileCount;
	ntFrames[NT_FRAME_FLY].pieceCount = newtron_fly[0].pieceCount;
	ntFrames[NT_FRAME_FLY].pivotX = newtron_fly[0].pivotX;
	ntFrames[NT_FRAME_FLY].pivotY = newtron_fly[0].pivotY;
	ntFrames[NT_FRAME_FLY].duration = newtron_fly[0].duration;

	if (*ghz_newtron_count_p != NEWTRON_COUNT) return;

	newtronType.entries = (const void *)ghz_newtron_xy;
	newtronArenaDesc.entries = (const void *)ghz_newtron_xy;
	newtronArenaDesc.tilePixels = newtron_tiles;

	if (obj_arena_register(&newtronArenaDesc) == ARENA_INVALID_SLOT) return;
}

void newtron_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	int8_t sl, st, sr, sb;
	uint16_t i;

	if (!newtronInited) newtron_lazy_init();
	badnik_sonic_hitbox(sonicFrameIndex, &sl, &st, &sr, &sb);

	for (i = 0; i < NEWTRON_COUNT; i++) {
		const NewtronEntry *e = &ghz_newtron_xy[i];

		if (badnik_is_destroyed(destroyed, i)) continue;

		if (ntState[i] == NT_STATE_DORMANT) {
			if (badnik_touches_sonic(e->x, e->y, NT_TRIG_L, NT_TRIG_T, NT_TRIG_R, NT_TRIG_B,
			                         sonicWorldX, sonicWorldY, sl, st, sr, sb)) {
				ntState[i] = NT_STATE_ACTIVE;
				ntTimer[i] = 0;
				ntOffX[i] = 0;
				ntDir[i] = (e->type == NEWTRON_FLY_TYPE)
				           ? (uint8_t)(sonicWorldX < e->x)
				           : (uint8_t)(e->direction & 1);
			}
			continue;
		}

		{
			uint16_t t = ntTimer[i];
			uint16_t limit = (e->type == NEWTRON_FLY_TYPE) ? NT_FLY_TICKS : NT_SHOOT_END;

			if (t >= limit) { ntState[i] = NT_STATE_DORMANT; continue; }
			ntTimer[i] = (uint16_t)(t + 1);

			/* VRAM ART BUDGET: "shoot"'s 5-frame firing animation is gone
			 * (this file's own top comment) -- a SHOOT-type instance just
			 * holds NT_FRAME_SHOOTIDLE (newtron_decide()'s own type check)
			 * for its whole active duration now, no frame timing left to
			 * compute here at all. FLY-type still advances its own real
			 * per-instance offset. */
			if (e->type == NEWTRON_FLY_TYPE) {
				int32_t d = (int32_t)t * NT_FLY_SPEED;
				ntOffX[i] = (int16_t)(ntDir[i] ? -d : d);
			}
		}
	}
}

uint16_t newtron_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                      uint16_t camX, uint16_t camY,
                      int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	if (!newtronInited || !ntLive) return 0;
	return obj_type_draw(&newtronType, list, firstIndex, firstLink, NEWTRON_SPRITE_CAP,
	                     camX, camY, sonicWorldX, sonicWorldY, sonicFrameIndex);
}
