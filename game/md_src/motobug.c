#include "motobug.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_sprite.h"
#include "obj_pool.h"
#include "motobug_data.h"
#include "badnik_base.h"
#include "assets_gen.h"

/* GHZ1's 9 Mania-mode Motobug entities (tools/convert_objects.py's
 * MOTOBUG_SCENE: Motobug_Serialize is empty, Motobug.c:254, so the record
 * is position-only). Same big-endian-count-then-table shape rings.c/
 * springs.c already read; #define wrapper matches rings.c's RING_COUNT. */
#define MOTOBUG_COUNT 9

typedef struct { int16_t x, y; } MotobugEntry;

static const uint16_t *const ghz_motobug_count_p = ASSET_GHZ_MOTOBUGS;
static const MotobugEntry *const ghz_motobug_xy =
	(const MotobugEntry *)((const uint8_t *)ASSET_GHZ_MOTOBUGS + 2);
static const uint32_t *const motobug_tiles = ASSET_MOTOBUG_TILES;

/* Motobug.c:59-62 (Motobug_StageLoad): hitboxBadnik, symmetric, no
 * direction-flip transcription needed (same reasoning rings.c's own
 * ring_touches_sonic comment gives for a symmetric box). */
#define MB_HB_L (-14)
#define MB_HB_T (-14)
#define MB_HB_R   14
#define MB_HB_B   14

/* Motobug_State_Init's own velocity.x magnitude (Motobug.c:193): 0x10000,
 * i.e. 1px/tick. AMPLITUDE (48px) and TURN_HOLD (12 ticks) are this port's
 * OWN chosen constants, not decomp values -- the real patrol distance comes
 * from real ledge/wall sensing (Motobug_State_Move/Idle/Turn's own
 * RSDK.ObjectTileGrip calls, Motobug.c:110-236), which needs the terrain
 * collision data only the slave SH2 can reach (see badnik_base.h's own top
 * comment). Compromise: every Motobug patrols the SAME fixed 48px radius
 * around its own spawn X regardless of the real platform it stands on.
 *
 * VRAM ART BUDGET (art-budget trim task, 2026-08-18): tools/convert_objects.py's
 * MOTOBUG_ART now converts only "move", subsampled to MB_MOVE_FRAMES=2 of
 * its real 12 walk-cycle frames -- see that recipe's own comment for why
 * "idle"/"turn"/"puff" are gone entirely. The old obj_anim_window STREAM (44
 * tiles, cycling one frame at a time through an 18-frame "move"+"turn" set)
 * is gone too: motobug_move[] is now a small, PERMANENTLY VRAM-resident set
 * (36 tiles total, motobug_arena_onBase() below), no streaming, no churn.
 * The visible cost: motobug_pose() below just alternates the 2 kept walk
 * frames during a leg and HOLDS the current one through the turn (flipping
 * flipH instantly at the transition) instead of animating a dedicated
 * about-face -- a real, visibly simpler turn than before, the trade this
 * task's own brief calls out by name. */
#define MB_SPEED       1
#define MB_AMPLITUDE   48
#define MB_TURN_HOLD   12
#define MB_MOVE_FRAMES 2
#define MB_STEP_TICKS  8   /* ticks each of the 2 walk frames holds -- an
                            * arbitrary, reasonable cadence (no 12-frame
                            * cycle left to time this off of) */
#define MB_LEG         (MB_AMPLITUDE / MB_SPEED)
#define MB_CYCLE       (2 * (MB_LEG + MB_TURN_HOLD))

static uint8_t  destroyed[(MOTOBUG_COUNT + 7) / 8];
static uint32_t motobugTick;

/* This whole class shares ONE pose (offset from spawn X, resident frame
 * index, flipH), computed once per displayed frame in motobug_tick() --
 * NOT once per candidate in decide() -- and reused by every instance's own
 * decide() call, matching rings.c's own "one shared request per tick"
 * pattern (ringFrame) extended from just the animation phase to the whole
 * movement phase too (see badnik_base.h's top comment: every Motobug on
 * screen walks and turns in perfect lockstep). */
static int32_t curOffX;
static uint16_t curFrame;
static uint8_t  curFlipH;

/* Rebased working copy of motobug_move[] -- tileOffset patched to the
 * arena's granted base by motobug_arena_onBase() below, every field else
 * copied once at init (field-by-field, not a struct assignment: -Os/LTO
 * lowers a struct copy to memcpy() here, and this -ffreestanding build
 * links against no libc -- same reasoning platform.c's own
 * platform_copy_frame() gives). Plain whole-sheet arena residency now
 * (md_src/obj_generic.h's ArenaClassDesc), the exact pattern spikes.c/
 * springs.c already use -- no ObjAnimWindow, no per-frame streaming. */
static ObjFrame  mbFrames[MB_MOVE_FRAMES];
static uint16_t  mbBase;
static uint8_t   mbLive;

/* PRECOMPUTED PIECE TEMPLATES (Job 1, lever 1, this task) -- see
 * obj_sprite.h's own top-of-section comment and crabmeat.c's own identical
 * pattern (this file's twin). Sized MB_MOVE_FRAMES * MOTOBUG_MAX_FRAME_
 * PIECES, an upper bound on motobug_pieces[]'s own real length. Rebuilt
 * every time motobug_arena_onBase() below fires. */
static ObjPieceTemplate mbTemplatesH0[MB_MOVE_FRAMES * MOTOBUG_MAX_FRAME_PIECES];
static ObjPieceTemplate mbTemplatesH1[MB_MOVE_FRAMES * MOTOBUG_MAX_FRAME_PIECES];

static void motobug_rebuild_templates(void)
{
	uint8_t i;
	for (i = 0; i < MB_MOVE_FRAMES; i++) {
		const ObjFrame *f = &mbFrames[i];
		/* drawPriority literal 0 here matches motobugType's own drawPriority
		 * field below. */
		obj_build_piece_templates(&mbTemplatesH0[f->pieceOffset], &motobug_pieces[f->pieceOffset], f->pieceCount,
		                          f->tileOffset, MOTOBUG_PAL, 0, f->pivotX, f->pivotY, 0, 0);
		obj_build_piece_templates(&mbTemplatesH1[f->pieceOffset], &motobug_pieces[f->pieceOffset], f->pieceCount,
		                          f->tileOffset, MOTOBUG_PAL, 0, f->pivotX, f->pivotY, 1, 0);
	}
}

static void motobug_pose(uint32_t tick, int32_t *offX, uint16_t *frame, uint8_t *flipH)
{
	int32_t phase = (int32_t)(tick % MB_CYCLE);

	if (phase < MB_LEG) {
		*offX = -phase * MB_SPEED;
		*frame = (uint16_t)((phase / MB_STEP_TICKS) & 1);
		*flipH = 0;
	} else if (phase < MB_LEG + MB_TURN_HOLD) {
		*offX = -MB_AMPLITUDE;
		*frame = curFrame;   /* hold whichever walk frame was showing */
		*flipH = 0;
	} else if (phase < 2 * MB_LEG + MB_TURN_HOLD) {
		int32_t p2 = phase - MB_LEG - MB_TURN_HOLD;
		*offX = -MB_AMPLITUDE + p2 * MB_SPEED;
		*frame = (uint16_t)((p2 / MB_STEP_TICKS) & 1);
		*flipH = 1;
	} else {
		*offX = 0;
		*frame = curFrame;   /* hold whichever walk frame was showing */
		*flipH = 1;
	}
}

static ObjDrawDecision motobug_decide(void *st, uint16_t i, int16_t ex, int16_t ey,
                                      int16_t sonicWorldX, int16_t sonicWorldY,
                                      uint16_t sonicFrameIndex)
{
	ObjDrawDecision d;
	int16_t bx = (int16_t)(ex + curOffX);
	(void)st;

	/* SPRITE-VS-HITBOX DRIFT fix (Job 2, this task): the SAME curOffX the
	 * hitbox test above already uses (this port's own MB_AMPLITUDE patrol,
	 * see this file's own top comment on why the real distance is invented
	 * rather than decomp-derived -- the offset FORMULA driving both hitbox
	 * and sprite is what this fix keeps in agreement, independent of that),
	 * now also carried on the draw decision so obj_type_draw() moves the
	 * sprite with it -- see obj_data.h's own ObjDrawDecision comment. */
	d.flipH = 0; d.flipV = 0; d.offX = (int16_t)curOffX; d.offY = 0; d.frame = OBJ_SKIP;

	if (badnik_decide_common(destroyed, i, bx, ey, MB_HB_L, MB_HB_T, MB_HB_R, MB_HB_B,
	                         sonicWorldX, sonicWorldY, sonicFrameIndex))
		return d;   /* destroyed this tick or already dead: stays OBJ_SKIP */

	if (!mbLive) return d;
	d.frame = curFrame;   /* index into mbFrames[MB_MOVE_FRAMES] -- always in range,
	                       * motobug_pose() only ever writes 0 or 1 */
	d.flipH = curFlipH;
	return d;
}

/* entries here is the RAW scene table (spawn X unmodified) -- decide()
 * itself adds the live patrol offset, so the x-sorted window binary search
 * (obj_type_window) still runs against each instance's true spawn position,
 * comfortably inside MB_AMPLITUDE+marginX of wherever it actually is. */
static ObjTypeDesc motobugType = {
	(const void *)0, sizeof(MotobugEntry), MOTOBUG_COUNT, ghz_motobug_count_p,
	(const uint32_t *)0, 0,
	mbFrames, motobug_pieces,
	OBJ_PRI_BADNIK, MOTOBUG_PAL, 0,
	MB_AMPLITUDE + 32,
	motobug_decide, (void *)0,
	mbTemplatesH0, mbTemplatesH1
};

/* Plain whole-sheet VRAM residency (md_src/obj_generic.h's ArenaClassDesc) --
 * replaces the old obj_anim_window streaming reservation. tileCount is the
 * class's own generated MOTOBUG total (motobug_tiles' whole trimmed sheet,
 * 2 frames); lookaheadX built the same way every arena tenant's is, off
 * this class's own real tile budget instead of a single frame's. */
static void motobug_arena_onBase(uint16_t base);
static void motobug_arena_onLive(uint8_t live) { mbLive = live; }

static ArenaClassDesc motobugArenaDesc = {
	(const void *)0, sizeof(MotobugEntry), MOTOBUG_COUNT,
	(const uint32_t *)0, MB_MOVE_FRAMES * MOTOBUG_MAX_FRAME_TILES,
	(int16_t)(ARENA_LOOKAHEAD_X(MB_MOVE_FRAMES * MOTOBUG_MAX_FRAME_TILES) + MB_AMPLITUDE + 32),
	OBJ_PRI_BADNIK,
	motobug_arena_onBase, motobug_arena_onLive
};

static void motobug_arena_onBase(uint16_t base)
{
	uint8_t i;
	mbBase = base;
	for (i = 0; i < MB_MOVE_FRAMES; i++)
		mbFrames[i].tileOffset = (uint16_t)(base + motobug_move[i].tileOffset);
	motobug_rebuild_templates();   /* Job 1, lever 1: only tileOffset changed above, and only here */
}

/* Registration only -- see main.c's own boot sequence for why badnik
 * classes register at runtime (main()) rather than at each class's own
 * *_init(), the pattern rings/springs/signpost established: badniks have
 * no "must be visible frame 1" requirement rings' own RING_SPAN_LO does, so
 * none of them need a synchronous boot-time load -- the ordinary
 * obj_arena_tick() runtime admission (main.c's per-frame loop) is exactly
 * how every non-boot-critical arena tenant already starts. Wired from
 * motobug_tick()'s own first call instead of a separate motobug_init()
 * main.c would have to remember to call -- one row in main.c's
 * OBJ_TYPE_LIST is this class's only main.c touch, matching this batch's
 * own registration contract. */
static uint8_t motobugInited;

static void motobug_lazy_init(void)
{
	uint16_t i;

	motobugInited = 1;
	motobugTick = 0;
	curOffX = 0; curFrame = 0; curFlipH = 0;
	for (i = 0; i < sizeof(destroyed); i++) destroyed[i] = 0;

	/* Field-by-field, not a struct assignment -- see this file's own top
	 * comment on mbFrames. tileOffset is patched in by motobug_arena_onBase()
	 * once this class is actually granted VRAM; every other field is fixed
	 * for the class's whole lifetime. */
	for (i = 0; i < MB_MOVE_FRAMES; i++) {
		mbFrames[i].tileOffset = 0;
		mbFrames[i].pieceOffset = motobug_move[i].pieceOffset;
		mbFrames[i].tileCount = motobug_move[i].tileCount;
		mbFrames[i].pieceCount = motobug_move[i].pieceCount;
		mbFrames[i].pivotX = motobug_move[i].pivotX;
		mbFrames[i].pivotY = motobug_move[i].pivotY;
		mbFrames[i].duration = motobug_move[i].duration;
	}

	if (*ghz_motobug_count_p != MOTOBUG_COUNT) return;

	motobugType.entries = (const void *)ghz_motobug_xy;
	motobugArenaDesc.entries = (const void *)ghz_motobug_xy;
	motobugArenaDesc.tilePixels = motobug_tiles;

	if (obj_arena_register(&motobugArenaDesc) == ARENA_INVALID_SLOT) return;
}

void motobug_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	(void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;
	if (!motobugInited) motobug_lazy_init();
	motobugTick++;
	motobug_pose(motobugTick, &curOffX, &curFrame, &curFlipH);
}

uint16_t motobug_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                      uint16_t camX, uint16_t camY,
                      int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	if (!motobugInited || !mbLive) return 0;
	return obj_type_draw(&motobugType, list, firstIndex, firstLink, MOTOBUG_SPRITE_CAP,
	                     camX, camY, sonicWorldX, sonicWorldY, sonicFrameIndex);
}
