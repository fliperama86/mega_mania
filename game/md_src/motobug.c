#include "motobug.h"
#include "obj_data.h"
#include "obj_generic.h"
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
 * around its own spawn X regardless of the real platform it stands on,
 * turning in place for TURN_HOLD ticks (motobug_turn's 6 frames) at each
 * end instead of the decomp's ledge-triggered turn. Motobug_State_Smoke's
 * dust-puff (Motobug.c:199-207, motobug_puff's 9 frames) is cut entirely --
 * purely cosmetic, spawned periodically while walking, and not worth a
 * second obj_anim_window registration out of this batch's own tight VRAM
 * slot budget. */
#define MB_SPEED       1
#define MB_AMPLITUDE   48
#define MB_TURN_HOLD   12
#define MB_TURN_FRAMES 6
#define MB_MOVE_FRAMES 12
#define MB_LEG         (MB_AMPLITUDE / MB_SPEED)
#define MB_CYCLE       (2 * (MB_LEG + MB_TURN_HOLD))

static uint8_t  destroyed[(MOTOBUG_COUNT + 7) / 8];
static uint32_t motobugTick;

/* This whole class shares ONE pose (offset from spawn X, window frame
 * index, flipH), computed once per displayed frame in motobug_tick() --
 * NOT once per candidate in decide() -- and reused by every instance's own
 * decide() call, matching rings.c's own "one shared request per tick"
 * pattern (ringFrame) extended from just the animation phase to the whole
 * movement phase too (see badnik_base.h's top comment: every Motobug on
 * screen walks and turns in perfect lockstep, not merely shares an
 * animation window -- a direct, deliberate consequence of that window
 * only ever holding one class-wide pose at a time). */
static int32_t curOffX;
static uint16_t curWinFrame;
static uint8_t  curFlipH;

static ObjFrame  mbFramesRaw[MB_MOVE_FRAMES + MB_TURN_FRAMES];
static ObjAnimWindow *mbWindow;

static void motobug_pose(uint32_t tick, int32_t *offX, uint16_t *frame, uint8_t *flipH)
{
	int32_t phase = (int32_t)(tick % MB_CYCLE);

	if (phase < MB_LEG) {
		*offX = -phase * MB_SPEED;
		*frame = (uint16_t)(phase % MB_MOVE_FRAMES);
		*flipH = 0;
	} else if (phase < MB_LEG + MB_TURN_HOLD) {
		*offX = -MB_AMPLITUDE;
		*frame = (uint16_t)(MB_MOVE_FRAMES
		                    + (phase - MB_LEG) * MB_TURN_FRAMES / MB_TURN_HOLD);
		*flipH = 0;
	} else if (phase < 2 * MB_LEG + MB_TURN_HOLD) {
		int32_t p2 = phase - MB_LEG - MB_TURN_HOLD;
		*offX = -MB_AMPLITUDE + p2 * MB_SPEED;
		*frame = (uint16_t)(p2 % MB_MOVE_FRAMES);
		*flipH = 1;
	} else {
		int32_t p2 = phase - 2 * MB_LEG - MB_TURN_HOLD;
		*offX = 0;
		*frame = (uint16_t)(MB_MOVE_FRAMES + p2 * MB_TURN_FRAMES / MB_TURN_HOLD);
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

	d.flipH = 0; d.flipV = 0; d.frame = OBJ_SKIP;

	if (badnik_decide_common(destroyed, i, bx, ey, MB_HB_L, MB_HB_T, MB_HB_R, MB_HB_B,
	                         sonicWorldX, sonicWorldY, sonicFrameIndex))
		return d;   /* destroyed this tick or already dead: stays OBJ_SKIP */

	if (!obj_anim_window_live(mbWindow)) return d;
	d.frame = 0;   /* obj_anim_window_frames() is always one row -- see its own recipe */
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
	(const ObjFrame *)0, motobug_pieces,
	OBJ_PRI_BADNIK, MOTOBUG_PAL, 0,
	MB_AMPLITUDE + 32,
	motobug_decide, (void *)0
};

static ObjAnimWindowDesc motobugAnimDesc = {
	(const void *)0, sizeof(MotobugEntry), MOTOBUG_COUNT,
	(int16_t)(ARENA_LOOKAHEAD_X(MOTOBUG_MAX_FRAME_TILES) + MB_AMPLITUDE + 32),
	OBJ_PRI_BADNIK,
	(const uint32_t *)0, mbFramesRaw, MB_MOVE_FRAMES + MB_TURN_FRAMES, MOTOBUG_MAX_FRAME_TILES
};

/* Registration only -- see main.c's own boot sequence for why badnik
 * classes register at runtime (main()) rather than at each class's own
 * *_init(), the pattern rings/springs/signpost established: badniks have
 * no "must be visible frame 1" requirement rings' own RING_SPAN_LO does, so
 * none of them need obj_anim_window_boot_load()'s synchronous path -- the
 * ordinary obj_arena_tick() runtime admission (main.c's per-frame loop) is
 * exactly how every non-boot-critical arena tenant already starts. Wired
 * from motobug_tick()'s own first call instead of a separate motobug_init()
 * main.c would have to remember to call -- one row in main.c's
 * OBJ_TYPE_LIST is this class's only main.c touch, matching this batch's
 * own registration contract. */
static uint8_t motobugInited;

static void motobug_lazy_init(void)
{
	uint16_t i;

	motobugInited = 1;
	motobugTick = 0;
	curOffX = 0; curWinFrame = 0; curFlipH = 0;
	for (i = 0; i < sizeof(destroyed); i++) destroyed[i] = 0;

	for (i = 0; i < MB_MOVE_FRAMES; i++) mbFramesRaw[i] = motobug_move[i];
	for (i = 0; i < MB_TURN_FRAMES; i++) mbFramesRaw[MB_MOVE_FRAMES + i] = motobug_turn[i];

	if (*ghz_motobug_count_p != MOTOBUG_COUNT) return;

	motobugType.entries = (const void *)ghz_motobug_xy;
	motobugAnimDesc.entries = (const void *)ghz_motobug_xy;
	motobugAnimDesc.sheetPixels = motobug_tiles;

	mbWindow = obj_anim_window_register(&motobugAnimDesc);
	if (!mbWindow) return;
	motobugType.frames = obj_anim_window_frames(mbWindow);
}

void motobug_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	(void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;
	if (!motobugInited) motobug_lazy_init();
	motobugTick++;
	motobug_pose(motobugTick, &curOffX, &curWinFrame, &curFlipH);
	/* ONE shared request per tick -- see obj_generic.h's own "call this
	 * unconditionally every tick" recipe (rings.c's own ringFrame follows
	 * the identical pattern). A no-op while mbWindow is not yet registered
	 * or not currently granted; harmless to call regardless. */
	obj_anim_window_select(mbWindow, curWinFrame);
}

uint16_t motobug_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                      uint16_t camX, uint16_t camY,
                      int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	if (!motobugInited || !mbWindow) return 0;
	return obj_type_draw(&motobugType, list, firstIndex, firstLink, MOTOBUG_SPRITE_CAP,
	                     camX, camY, sonicWorldX, sonicWorldY, sonicFrameIndex);
}
