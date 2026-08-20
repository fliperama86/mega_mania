#include "chopper.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_sprite.h"
#include "obj_pool.h"
#include "chopper_data.h"
#include "badnik_base.h"
#include "assets_gen.h"

#define CHOPPER_COUNT 13
#define CHOPPER_JUMP_TYPE 0   /* Chopper.h's ChopperTypes: CHOPPER_JUMP=0, CHOPPER_SWIM=1 */

/* CHOPPER_SCENE, tools/convert_objects.py: row_fmt ">hhBBBx" (8 bytes on
 * disk -- a trailing pad byte after `charge`, no field of its own), fields
 * (x_px, y_px, type, direction, charge) -- Chopper_Serialize (Chopper.c:
 * 325-330). direction/charge only matter to the Swim variant, out of
 * scope here (see chopper.h's own header comment). The pad keeps every row
 * EVEN-sized, matching sizeof(ChopperEntry) here exactly: SceneRecipe
 * (tools/convert_objects.py) asserts every struct-cast-read table's row_fmt
 * packs to an EVEN size, so this struct and recordSize-strided readers like
 * obj_generic.c's entry_x()/entry_y() below always agree with the table's
 * real on-disk stride, row 0 onward. */
typedef struct { int16_t x, y; uint8_t type, direction, charge; } ChopperEntry;

static const uint16_t *const ghz_chopper_count_p = ASSET_GHZ_CHOPPERS;
static const ChopperEntry *const ghz_chopper_xy =
	(const ChopperEntry *)((const uint8_t *)ASSET_GHZ_CHOPPERS + 2);
static const uint32_t *const chopper_tiles = ASSET_CHOPPER_TILES;

/* Chopper.c:47-50 (Chopper_StageLoad): hitboxJump, asymmetric in X (-10/6)
 * -- transcribed as-is; the Jump variant never sets self->direction
 * meaningfully (Chopper_State_Init only branches on self->type), so no
 * flip-mirroring of this box is needed. */
#define CH_HB_L (-10)
#define CH_HB_T (-20)
#define CH_HB_R    6
#define CH_HB_B   20

/* Chopper_State_Jump (Chopper.c:141-170): velocity.y starts at 0 (never
 * set for the Jump path in Chopper_Create/State_Init), gravity +0x1800/
 * tick, and the instant position.y crosses back past startPos.y it snaps
 * to startPos.y and re-launches at velocity.y=-0x70000. One SHARED
 * simulation serves every Jump chopper (all identical physics, so sharing
 * one phase costs nothing beyond what the single VRAM window already
 * forces -- see motobug.c's own comment on class-wide lockstep), advanced
 * once per tick in chopper_tick() rather than replayed from a modulus
 * (the bounce period is not a round tick count). All values 16.16 fixed,
 * matching the decomp's own scale. */
#define CH_GRAVITY   0x1800
#define CH_LAUNCH_VY (-0x70000)

/* VRAM ART BUDGET (art-budget trim task, 2026-08-18): tools/convert_objects.py's
 * CHOPPER_ART now converts only "jump", subsampled to CH_JUMP_FRAMES=2 of
 * its real 8-frame bounce arc (rising vs falling, half the arc apart) --
 * "swim"/"charge" were already never drawn (chopper_decide()'s own "Swim:
 * out of scope" early-out, chopper.h's own header comment) and cost nothing
 * to drop. The old obj_anim_window STREAM (32 tiles, cycling one frame at a
 * time through the full 8-frame arc as velocity changed) is gone too:
 * chopper_jump[] is now a small, PERMANENTLY VRAM-resident pair (also 32
 * tiles total -- this class's own per-frame cost happens not to change,
 * only the mechanism does). The visible cost: chopper_tick() below now just
 * picks rising (frame 0) vs falling (frame 1) off velocity sign instead of
 * animating smoothly through 8 poses across the arc. */
#define CH_JUMP_FRAMES 2

static uint8_t  destroyed[(CHOPPER_COUNT + 7) / 8];
static int32_t  chopperOffY;   /* 16.16, <= 0: how far above startPos.y right now */
static int32_t  chopperVelY;   /* 16.16 */
static uint16_t curFrame;

/* Rebased working copy of chopper_jump[] -- see motobug.c's own comment on
 * mbFrames for why field-by-field. Plain whole-sheet arena residency, no
 * ObjAnimWindow. */
static ObjFrame  chFrames[CH_JUMP_FRAMES];
static uint16_t  chBase;
static uint8_t   chLive;

/* PRECOMPUTED PIECE TEMPLATES (Job 1, lever 1, this task) -- see
 * obj_sprite.h's own top-of-section comment and crabmeat.c's own identical
 * pattern (this file's twin). Sized CH_JUMP_FRAMES * CHOPPER_MAX_FRAME_
 * PIECES, an upper bound on chopper_pieces[]'s own real length. Rebuilt
 * every time chopper_arena_onBase() below fires. */
static ObjPieceTemplate chTemplatesH0[CH_JUMP_FRAMES * CHOPPER_MAX_FRAME_PIECES];
static ObjPieceTemplate chTemplatesH1[CH_JUMP_FRAMES * CHOPPER_MAX_FRAME_PIECES];

static void chopper_rebuild_templates(void)
{
	uint8_t i;
	for (i = 0; i < CH_JUMP_FRAMES; i++) {
		const ObjFrame *f = &chFrames[i];
		/* drawPriority literal 0 here matches chopperType's own drawPriority
		 * field below. */
		obj_build_piece_templates(&chTemplatesH0[f->pieceOffset], &chopper_pieces[f->pieceOffset], f->pieceCount,
		                          f->tileOffset, CHOPPER_PAL, 0, f->pivotX, f->pivotY, 0, 0);
		obj_build_piece_templates(&chTemplatesH1[f->pieceOffset], &chopper_pieces[f->pieceOffset], f->pieceCount,
		                          f->tileOffset, CHOPPER_PAL, 0, f->pivotX, f->pivotY, 1, 0);
	}
}

static ObjDrawDecision chopper_decide(void *st, uint16_t i, int16_t ex, int16_t ey,
                                      int16_t sonicWorldX, int16_t sonicWorldY,
                                      uint16_t sonicFrameIndex)
{
	ObjDrawDecision d;
	const ChopperEntry *e = &ghz_chopper_xy[i];
	int16_t by;
	(void)st;

	d.flipH = 0; d.flipV = 0; d.offX = 0; d.offY = 0; d.frame = OBJ_SKIP;
	if (e->type != CHOPPER_JUMP_TYPE) return d;   /* Swim: out of scope, never drawn */

	by = (int16_t)(ey + (chopperOffY >> 16));

	/* SPRITE-VS-HITBOX DRIFT fix (Job 2, this task): the SAME chopperOffY
	 * the hitbox test above already uses (Chopper_State_Jump's bounce arc,
	 * Chopper.c:141-170 -- see this file's own top comment), now also
	 * carried on the draw decision so obj_type_draw() moves the sprite with
	 * it -- see obj_data.h's own ObjDrawDecision comment. X-only classes
	 * leave d.offX at 0 above; this class is Y-only the same way. */
	d.offY = (int16_t)(chopperOffY >> 16);

	if (badnik_decide_common(destroyed, i, ex, by, CH_HB_L, CH_HB_T, CH_HB_R, CH_HB_B,
	                         sonicWorldX, sonicWorldY, sonicFrameIndex))
		return d;

	if (!chLive) return d;
	d.frame = curFrame;   /* index into chFrames[CH_JUMP_FRAMES] -- always in
	                       * range, chopper_tick() only ever writes 0 or 1 */
	return d;
}

static ObjTypeDesc chopperType = {
	(const void *)0, sizeof(ChopperEntry), CHOPPER_COUNT, ghz_chopper_count_p,
	(const uint32_t *)0, 0,
	chFrames, chopper_pieces,
	OBJ_PRI_BADNIK, CHOPPER_PAL, 0,
	32,
	chopper_decide, (void *)0,
	chTemplatesH0, chTemplatesH1
};

/* Plain whole-sheet VRAM residency -- see motobug.c's own comment on the
 * identical pattern. */
static void chopper_arena_onBase(uint16_t base);
static void chopper_arena_onLive(uint8_t live) { chLive = live; }

static ArenaClassDesc chopperArenaDesc = {
	(const void *)0, sizeof(ChopperEntry), CHOPPER_COUNT,
	(const uint32_t *)0, CH_JUMP_FRAMES * CHOPPER_MAX_FRAME_TILES,
	(int16_t)(ARENA_LOOKAHEAD_X(CH_JUMP_FRAMES * CHOPPER_MAX_FRAME_TILES) + 32),
	OBJ_PRI_BADNIK,
	chopper_arena_onBase, chopper_arena_onLive
};

static void chopper_arena_onBase(uint16_t base)
{
	uint8_t i;
	chBase = base;
	for (i = 0; i < CH_JUMP_FRAMES; i++)
		chFrames[i].tileOffset = (uint16_t)(base + chopper_jump[i].tileOffset);
	chopper_rebuild_templates();   /* Job 1, lever 1: only tileOffset changed above, and only here */
}

static uint8_t chopperInited;

static void chopper_lazy_init(void)
{
	uint16_t i;

	chopperInited = 1;
	chopperOffY = 0;
	chopperVelY = CH_LAUNCH_VY;
	curFrame = 0;
	for (i = 0; i < sizeof(destroyed); i++) destroyed[i] = 0;

	for (i = 0; i < CH_JUMP_FRAMES; i++) {
		chFrames[i].tileOffset = 0;
		chFrames[i].pieceOffset = chopper_jump[i].pieceOffset;
		chFrames[i].tileCount = chopper_jump[i].tileCount;
		chFrames[i].pieceCount = chopper_jump[i].pieceCount;
		chFrames[i].pivotX = chopper_jump[i].pivotX;
		chFrames[i].pivotY = chopper_jump[i].pivotY;
		chFrames[i].duration = chopper_jump[i].duration;
	}

	if (*ghz_chopper_count_p != CHOPPER_COUNT) return;

	chopperType.entries = (const void *)ghz_chopper_xy;
	chopperArenaDesc.entries = (const void *)ghz_chopper_xy;
	chopperArenaDesc.tilePixels = chopper_tiles;

	if (obj_arena_register(&chopperArenaDesc) == ARENA_INVALID_SLOT) return;
}

void chopper_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	(void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;
	if (!chopperInited) chopper_lazy_init();

	chopperOffY += chopperVelY;
	chopperVelY += CH_GRAVITY;
	if (chopperOffY > 0) { chopperOffY = 0; chopperVelY = CH_LAUNCH_VY; }

	/* VRAM ART BUDGET: only 2 resident poses survive (see this file's own
	 * top comment) -- rising (velocity still negative, frame 0) vs falling
	 * (velocity has turned positive, frame 1), replacing the old smooth
	 * 8-pose velocity-threshold mapping. */
	curFrame = (uint16_t)(chopperVelY < 0 ? 0 : 1);
}

uint16_t chopper_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                      uint16_t camX, uint16_t camY,
                      int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	if (!chopperInited || !chLive) return 0;
	return obj_type_draw(&chopperType, list, firstIndex, firstLink, CHOPPER_SPRITE_CAP,
	                     camX, camY, sonicWorldX, sonicWorldY, sonicFrameIndex);
}
