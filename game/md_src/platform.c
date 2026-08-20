#include "platform.h"
#include "platform_data.h"
#include "platform_trig.h"
#include "platform_clock.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_pool.h"
#include "obj_sprite.h"
#include "vdp.h"
#include "sonic_data.h"
#include "assets_gen.h"

/* ===========================================================================
 * THE DRAWN-POSITION AGREEMENT, IN ONE PLACE
 *
 * Every moving Platform's position here is computed by the EXACT SAME closed
 * -form functions as sh_src/platform.c's own (fixed_pos/linear_pos/swing_pos/
 * swing_angle/fall_pos), fed g_platform_tick_md (md_src/platform_clock.c)
 * instead of g_platform_tick -- and platform_clock.h's own top comment proves
 * those two are numerically identical for whatever frame is currently on the
 * comm bus. So this is not "close enough" or "usually matches": for Fixed/
 * Linear/Swing (pure functions of the scene row and the tick alone), the
 * 68000's drawn position and the SH2's collision position are bit-for-bit
 * the same value, always, by construction, with no comm traffic beyond what
 * already exists for the seqlock.
 *
 * Fall and Push are different: they are STATEFUL (a trigger tick, an
 * accumulated push offset) that a real collision test decides, and only the
 * SH2 runs the real one. This file re-derives that state OBSERVATIONALLY --
 * the exact same pattern springs.c's spring_touches_sonic() already
 * established for springs' visual bounce trigger (springs.h's own doc
 * comment: "observational, not the side-resolving collision box sh_src/
 * spring.c's own physics test runs") -- using Sonic's own published
 * worldX/worldY and current-frame hitbox (ASSET_SONIC_HITBOX, the same table
 * rings.c/springs.c already read) against the platform's own current
 * position, computed with THIS SAME file's fall_pos()/push_pos(). Because
 * g_platform_tick_md is exactly paired with the worldX/worldY it is read
 * alongside (both come from the SAME comm_publish_frame() call on the slave,
 * platform_clock.h's own proof), this file's touch test runs on EXACTLY the
 * tick the SH2's own real test ran on for that same published frame, so the
 * two sides record the SAME trigger tick -- not a frame-behind approximation
 * (see this batch's own report for the worked-through reasoning on why the
 * naive "one comm round-trip of lag" worry does not actually apply here).
 * =========================================================================== */

static const uint16_t *const ghz_platforms_count_p = ASSET_GHZ_PLATFORMS;
static const uint8_t *const k_platform_bytes = (const uint8_t *)ASSET_GHZ_PLATFORMS + 2;
static const uint32_t *const platform_tiles_md = ASSET_PLATFORM_TILES;
static const int8_t *const sonic_hitbox = ASSET_SONIC_HITBOX;

#define PLATFORM_COUNT 60
#define PLATFORM_RECORD_SIZE 30

/* platform_tick()'s own Sonic-proximity gate (2026-08-18, 68000 per-frame
 * cost task) -- see that function's own comment for the full reasoning.
 * Generous round margin, not the exact worst-case math: the widest touch
 * test this loop ever runs is FALL's touches_top() against k_frameHitbox's
 * largest halfW (32, the swing-seat frame) plus Sonic's own widest hitbox
 * half-width (character hitboxes in this codebase top out well under 32),
 * so 64px of slack on top of that is comfortably safe, the same "round
 * number well past the exact requirement" style every other hand-picked
 * margin in this codebase already uses (SONIC_APPROX_HALF and friends). */
#define PLATFORM_TICK_MARGIN 96

#define PLATFORM_FIXED  0
#define PLATFORM_FALL   1
#define PLATFORM_LINEAR 2
#define PLATFORM_SWING  4
#define PLATFORM_PUSH   6

#define PLATFORM_C_PLATFORM 0
#define PLATFORM_C_SOLID    1
#define PLATFORM_C_NONE     4

typedef struct {
	int16_t x, y;
	uint8_t type;
	int32_t amplitudeX, amplitudeY;
	int8_t  speed;
	int8_t  frameID;
	uint8_t collision;
	int32_t angle;
} PlatformDef;

static int16_t rd_i16(const uint8_t *p) { return (int16_t)(((uint16_t)p[0] << 8) | p[1]); }
static int32_t rd_i32(const uint8_t *p)
{
	return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]);
}

/* Same byte-at-a-time read as sh_src/platform.c's own platform_read() (see
 * that function's own comment for why -- Platform's 30-byte record is not
 * naturally aligned, and while m68k tolerates SOME misalignment better than
 * SH2, word/long access to an ODD address still faults on 68000 too, and
 * this table's own base address is not something either side controls). */
static void platform_read(uint16_t i, PlatformDef *d)
{
	const uint8_t *rec = k_platform_bytes + (uint32_t)i * PLATFORM_RECORD_SIZE;
	d->x = rd_i16(rec + 0);
	d->y = rd_i16(rec + 2);
	d->type = rec[4];
	d->amplitudeX = rd_i32(rec + 5);
	d->amplitudeY = rd_i32(rec + 9);
	d->speed = (int8_t)rec[13];
	d->frameID = (int8_t)rec[15];
	d->collision = rec[16];
	d->angle = rd_i32(rec + 26);
}

/* ---- Local, VRAM-rebased frame/piece tables -----------------------------
 * VRAM ART BUDGET (art-budget trim task, 2026-08-18): "normal" -- already
 * trimmed once, at THIS file's own level, from the raw sheet's 4 frames down
 * to the 3 GHZ1's own scene data actually requests via frameID (0/1/2,
 * 200 tiles: 32+144+24) -- is cut MUCH harder here, at the
 * tools/convert_objects.py PLATFORM_ART level instead: only ONE of those 3
 * survives (frame 2, the cheapest at 24 tiles) -- see that recipe's own
 * comment for the full arithmetic (this single class was, on its own,
 * roughly half of the entire 427-tile arena budget; no other class's own
 * guidance-level trim could close the gap without this one going further
 * than its own guidance ever called for). Every FIXED/LINEAR/PUSH instance
 * now draws this ONE graphic regardless of its own authored frameID --
 * frame_hitbox(d.frameID) below (platform_tick()'s own FALL/PUSH touch
 * tests) still reads the REAL frameID unchanged, so collision/gameplay is
 * untouched, only the drawn ART collapsed. "swing" is unchanged (12/4/4
 * tiles, a 3-part KIT -- seat+link+hub, not alternate poses -- not
 * something frame-count trimming applies to at all, see PLATFORM_ART's own
 * comment). 24+20 = 44 tiles total now, not 220. */
static ObjFrame pfFrames[4];      /* 0 normal, 1 swing seat, 2 swing link, 3 swing hub */
static ObjPiece pfPieces[6];      /* 2 normal, 2+1+1 swing */
static uint8_t normalLive, swingLive;

#define PF_SWING_SEAT 1
#define PF_SWING_LINK 2
#define PF_SWING_HUB  3

static void platform_copy_frame(uint8_t dstIdx, const ObjFrame *src, const ObjPiece *srcPieces, uint8_t *pn)
{
	uint8_t k;
	pfFrames[dstIdx].tileOffset = 0;   /* real value set by the onBase callbacks below */
	pfFrames[dstIdx].pieceOffset = *pn;
	pfFrames[dstIdx].tileCount = src->tileCount;
	pfFrames[dstIdx].pieceCount = src->pieceCount;
	pfFrames[dstIdx].pivotX = src->pivotX;
	pfFrames[dstIdx].pivotY = src->pivotY;
	pfFrames[dstIdx].duration = 0;
	for (k = 0; k < src->pieceCount; k++, (*pn)++) {
		/* Field-by-field, not a struct assignment -- see rings.c/springs.c's
		 * own identical comment: -Os/LTO lowers a struct copy to memcpy()
		 * here, and this -ffreestanding build links against no libc. */
		pfPieces[*pn].dx = srcPieces[k].dx;
		pfPieces[*pn].dy = srcPieces[k].dy;
		pfPieces[*pn].size = srcPieces[k].size;
		pfPieces[*pn].tile = srcPieces[k].tile;
	}
}

static uint16_t pfNormalRawOffset;
static uint16_t pfSwingRawOffset[3];

static void platform_normal_onBase(uint16_t base)
{
	pfFrames[0].tileOffset = (uint16_t)(base + pfNormalRawOffset);
}
static void platform_normal_onLive(uint8_t live) { normalLive = live; }

static void platform_swing_onBase(uint16_t base)
{
	uint8_t i;
	for (i = 0; i < 3; i++) pfFrames[PF_SWING_SEAT + i].tileOffset = (uint16_t)(base + pfSwingRawOffset[i]);
}
static void platform_swing_onLive(uint8_t live) { swingLive = live; }

static ArenaClassDesc platformNormalArenaDesc = {
	(const void *)0, PLATFORM_RECORD_SIZE, PLATFORM_COUNT,
	(const uint32_t *)0, PLATFORM_MAX_FRAME_TILES,   /* 24 -- the one kept frame */
	0, OBJ_PRI_PLATFORM,
	platform_normal_onBase, platform_normal_onLive
};
static ArenaClassDesc platformSwingArenaDesc = {
	(const void *)0, PLATFORM_RECORD_SIZE, PLATFORM_COUNT,
	(const uint32_t *)0, 20,
	0, OBJ_PRI_PLATFORM,
	platform_swing_onBase, platform_swing_onLive
};

/* Window-only descriptor: obj_type_window() (obj_generic.h) is called
 * directly, NOT obj_type_draw() -- Platform's own decide() cannot express
 * "draw this entry at a position other than its own table row" (obj_data.h's
 * ObjDrawDecision carries only a frame index and flip bits), which every
 * moving Platform type genuinely needs. Pointing entries at the STATIC ROM
 * scene table (never a RAM shadow) keeps the binary search's own x-sorted
 * assumption exactly true always -- the alternative (a RAM table updated to
 * each entry's CURRENT position every tick) would need to stay x-sorted too,
 * which a moving entry can violate against its neighbours; not attempted
 * here. marginX is wider than rings/springs' own 16px specifically to
 * compensate: a Linear/Swing/Push entry can be showing up to ~256px away
 * from its OWN scene-authored x (Push's own PUSH_MAX_OFFSET_PX, sh_src/
 * platform.c), so the window has to still include it by its STATIC x even
 * though it may currently be drawn well outside a plain 16px margin. See
 * platform.h's own comment for the measured worst-case sprite COUNT this
 * produces. */
static ObjTypeDesc platformWinDesc = {
	(const void *)0, PLATFORM_RECORD_SIZE, PLATFORM_COUNT, (const uint16_t *)0,
	(const uint32_t *)0, 0,
	(const ObjFrame *)0, (const ObjPiece *)0,
	OBJ_PRI_PLATFORM, PLATFORM_PAL, 0,
	280,
	(ObjDecideFn)0, (void *)0,
	(const ObjPieceTemplate *)0, (const ObjPieceTemplate *)0   /* window-only desc, never drawn through obj_type_draw() */
};

/* ---- Per-instance observational state (Fall/Push only) -- see this file's
 * own top comment for why only these two types need any at all. */
static uint8_t  fallTriggered[PLATFORM_COUNT];
static uint32_t fallTriggerTick[PLATFORM_COUNT];
static int32_t  pushOffsetPx[PLATFORM_COUNT];
static uint8_t  pushInit[PLATFORM_COUNT];

#define PLATFORM_FALL_WAIT_TICKS   30
#define PLATFORM_FALL_GRAVITY      0x3800
#define PLATFORM_FALL_VANISH_TICKS 200
#define PUSH_MAX_OFFSET_PX 256
#define PUSH_SPEED_PX 6   /* matches this stage's own single Push row (speed field = 6) */

static int32_t fall_displacement_px(uint32_t fallTicks)
{
	int32_t f = (int32_t)fallTicks;
	return (PLATFORM_FALL_GRAVITY * (f * (f - 1) / 2)) >> 16;
}

/* ---- Per-type position (pixels) -- bit-for-bit the same formulas as
 * sh_src/platform.c's own; see this file's top comment. */
static void fixed_pos(const PlatformDef *d, int32_t *px, int32_t *py) { *px = d->x; *py = d->y; }

static void linear_pos(const PlatformDef *d, uint32_t tick, int32_t *px, int32_t *py)
{
	int32_t amp10X = d->amplitudeX >> 10;
	int32_t amp10Y = d->amplitudeY >> 10;
	int32_t angle = d->speed * (d->angle + (int32_t)tick);
	int32_t s = platform_sin1024(angle);
	*px = d->x + ((amp10X * s) >> 16);
	*py = d->y + ((amp10Y * s) >> 16);
}

static int32_t swing_angle(const PlatformDef *d, uint32_t tick)
{
	int32_t groundVel = 4 * d->angle;
	int32_t amp10X = d->amplitudeX >> 10;
	return groundVel + 0x100 + (((amp10X * platform_sin1024(d->speed * (int32_t)tick)) + 0x200) >> 14);
}

static void swing_pos(const PlatformDef *d, uint32_t tick, int32_t *px, int32_t *py)
{
	int32_t ampSwingY = d->amplitudeY >> 6;
	int32_t angle = swing_angle(d, tick);
	*px = d->x + ((ampSwingY * platform_cos1024(angle)) >> 16);
	*py = d->y + ((ampSwingY * platform_sin1024(angle)) >> 16);
}

/* fall_pos: returns 0 once vanished (see sh_src/platform.c's own comment on
 * PLATFORM_FALL_VANISH_TICKS for the deliberate simplification this is). */
static uint8_t fall_pos(const PlatformDef *d, uint16_t idx, int32_t *px, int32_t *py)
{
	*px = d->x;
	if (!fallTriggered[idx]) { *py = d->y; return 1; }
	{
		uint32_t elapsed = g_platform_tick_md - fallTriggerTick[idx];
		if (elapsed < PLATFORM_FALL_WAIT_TICKS) { *py = d->y; return 1; }
		{
			uint32_t fallTicks = elapsed - PLATFORM_FALL_WAIT_TICKS;
			if (fallTicks >= PLATFORM_FALL_VANISH_TICKS) return 0;
			*py = d->y + fall_displacement_px(fallTicks);
			return 1;
		}
	}
}

/* Player_GetHitbox stand-in (Player.c:2244-2248) -- same fallback/derivation
 * rings.c's sonic_hitbox_at()/springs.c's own inline copy already use. */
static void sonic_hitbox_at(uint16_t frameIndex, int8_t *l, int8_t *t, int8_t *r, int8_t *b)
{
	if (frameIndex < SONIC_FRAME_COUNT) {
		const int8_t *hb = &sonic_hitbox[frameIndex * 4];
		*l = hb[0]; *t = hb[1]; *r = hb[2]; *b = hb[3];
	} else {
		*l = -10; *t = -20; *r = 10; *b = 20;
	}
}

/* One-way top touch test, observational (mirrors sh_src/platform.c's own
 * platform_land_top(), minus the parts only real collision can do -- see
 * this file's own top comment). */
static uint8_t touches_top(int16_t sonicX, int16_t sonicY, const int8_t *hb,
                           int32_t platPx, int32_t platPy, int8_t halfW, int8_t top)
{
	int32_t feet = sonicY + hb[3];
	int32_t surfaceY = platPy + top;
	if (sonicX + hb[2] <= platPx - halfW || sonicX + hb[0] >= platPx + halfW) return 0;
	if (feet < surfaceY || feet > surfaceY + 8) return 0;
	return 1;
}

/* Frame hitboxes -- identical derivation and values as sh_src/platform.c's
 * own k_frameHitbox/k_swingSeatHitbox (see that file's own comment for the
 * exact piece-bounding-box arithmetic); duplicated, not shared, same
 * two-CPUs-no-shared-memory reason every other duplicated table in this
 * codebase gives. */
typedef struct { int8_t halfW, top, bottom; } PlatHitbox;
static const PlatHitbox k_frameHitbox[3] = { { 32, -22, 10 }, { 32, -19, 125 }, { 24, -16, 16 } };

static const PlatHitbox *frame_hitbox(int8_t frameID)
{
	if (frameID < 0 || frameID > 2) return &k_frameHitbox[0];
	return &k_frameHitbox[frameID];
}

__attribute__((noinline))
void platform_init(void)
{
	uint8_t pn, slot;
	uint16_t base;

	normalLive = 0; swingLive = 0;
	{
		uint16_t i;
		for (i = 0; i < PLATFORM_COUNT; i++) {
			fallTriggered[i] = 0; fallTriggerTick[i] = 0;
			pushOffsetPx[i] = 0; pushInit[i] = 0;
		}
	}

	pn = 0;
	pfNormalRawOffset = 0;
	platform_copy_frame(0, &platform_normal[0], &platform_pieces[platform_normal[0].pieceOffset], &pn);

	/* VRAM ART BUDGET: platform_tiles_md's own layout is now [normal frame
	 * (PLATFORM_MAX_FRAME_TILES=24 tiles), swing seat/link/hub (20 tiles)]
	 * back to back -- tools/convert_objects.py's PLATFORM_ART converts
	 * "normal" (1 frame) then "swing" (3 parts), in that order, into one
	 * shared sheet. */
	pfSwingRawOffset[0] = 0; pfSwingRawOffset[1] = 12; pfSwingRawOffset[2] = 16;
	platform_copy_frame(PF_SWING_SEAT, &platform_swing[0], &platform_pieces[platform_swing[0].pieceOffset], &pn);
	platform_copy_frame(PF_SWING_LINK, &platform_swing[1], &platform_pieces[platform_swing[1].pieceOffset], &pn);
	platform_copy_frame(PF_SWING_HUB, &platform_swing[2], &platform_pieces[platform_swing[2].pieceOffset], &pn);

	if (*ghz_platforms_count_p != PLATFORM_COUNT) return;

	platformWinDesc.entries = (const void *)k_platform_bytes;
	platformNormalArenaDesc.entries = (const void *)k_platform_bytes;
	platformNormalArenaDesc.tilePixels = platform_tiles_md;   /* offset 0, PLATFORM_MAX_FRAME_TILES tiles */
	slot = obj_arena_register(&platformNormalArenaDesc);
	base = obj_arena_boot_load(slot);
	if (base != 0xFFFF) {
		vdp_tiles_load(platform_tiles_md, base, PLATFORM_MAX_FRAME_TILES);
		obj_arena_boot_done(slot);
	}

	platformSwingArenaDesc.entries = (const void *)k_platform_bytes;
	platformSwingArenaDesc.tilePixels = platform_tiles_md + (uint32_t)PLATFORM_MAX_FRAME_TILES * 8;   /* 20 tiles */
	slot = obj_arena_register(&platformSwingArenaDesc);
	base = obj_arena_boot_load(slot);
	if (base != 0xFFFF) {
		vdp_tiles_load(platformSwingArenaDesc.tilePixels, base, 20);
		obj_arena_boot_done(slot);
	}
}

__attribute__((noinline))
void platform_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	uint16_t n, i;
	int8_t hbL, hbT, hbR, hbB;

	platform_clock_sync();
	sonic_hitbox_at(sonicFrameIndex, &hbL, &hbT, &hbR, &hbB);
	{ int8_t hb[4]; hb[0] = hbL; hb[1] = hbT; hb[2] = hbR; hb[3] = hbB;

	n = *ghz_platforms_count_p;
	if (n > PLATFORM_COUNT) n = PLATFORM_COUNT;

	for (i = 0; i < n; i++) {
		PlatformDef d;
		const uint8_t *rec = k_platform_bytes + (uint32_t)i * PLATFORM_RECORD_SIZE;
		uint8_t rawType = rec[4];

		/* Camera/Sonic-proximity gate (2026-08-18 per-frame cost task) --
		 * see PLATFORM_TICK_MARGIN's own comment. Every type OTHER than
		 * FALL/PUSH does nothing at all in this loop (Fixed/Linear/Swing's
		 * own motion is a pure function of the scene row and the shared
		 * g_platform_tick_md, computed entirely at DRAW time -- this file's
		 * own top-of-file comment), so skip the full platform_read() decode
		 * for them unconditionally, not just far ones. An already-triggered
		 * FALL entry likewise has nothing left for this loop to ever do
		 * again (fallTriggered[i] latches permanently, exactly like
		 * itembox's/breakablewall's own broken flags) -- skip the decode
		 * for it too. PUSH is the one type this gate NEVER applies to: it
		 * carries real per-tick INTEGRATED state (pushOffsetPx, an
		 * accumulating offset that must keep evolving once started, not a
		 * pure function of the shared tick) -- the same reason signpost's
		 * fall state and bridge's sag-decay stay fully ungated in their own
		 * files. GHZ1 has exactly one Push row, so never gating it costs
		 * nothing. An UNTRIGGERED FALL entry's own trigger check has no
		 * state to protect (nothing happens until it fires), so it is safe
		 * to skip when Sonic is nowhere near it -- gated by raw x, read
		 * directly off the record before paying for a full decode. */
		if (rawType != PLATFORM_FALL && rawType != PLATFORM_PUSH) continue;
		if (rawType == PLATFORM_FALL) {
			if (fallTriggered[i]) continue;
			{
				int16_t rawX = rd_i16(rec + 0);
				if (rawX < sonicWorldX - PLATFORM_TICK_MARGIN
				    || rawX > sonicWorldX + PLATFORM_TICK_MARGIN)
					continue;
			}
		}

		platform_read(i, &d);

		if (d.type == PLATFORM_FALL) {
			int32_t px, py;
			const PlatHitbox *hbx = frame_hitbox(d.frameID);
			fall_pos(&d, i, &px, &py);
			if (touches_top(sonicWorldX, sonicWorldY, hb, px, py, hbx->halfW, hbx->top)) {
				fallTriggered[i] = 1;
				fallTriggerTick[i] = g_platform_tick_md;
			}
		} else if (d.type == PLATFORM_PUSH) {
			/* Observational replica of sh_src/platform.c's own push_pos():
			 * this side has no onGround/velX to test (never published), so
			 * it drives purely off which side of the platform Sonic's
			 * published X currently sits against -- a further reduction
			 * beyond sh_src/platform.c's own already-documented Push
			 * simplification (see that file's header comment), acceptable
			 * given GHZ1's single Push instance and this batch's own stated
			 * priority (Push is the lowest-priority platform type). */
			const PlatHitbox *hbx = frame_hitbox(d.frameID);
			int32_t curPx = d.x + pushOffsetPx[i];
			if (!pushInit[i]) { pushOffsetPx[i] = 0; pushInit[i] = 1; curPx = d.x; }
			if (sonicWorldX + hbR >= curPx - hbx->halfW && sonicWorldX + hbR <= curPx - hbx->halfW + 6)
				pushOffsetPx[i] -= PUSH_SPEED_PX;
			else if (sonicWorldX + hbL <= curPx + hbx->halfW && sonicWorldX + hbL >= curPx + hbx->halfW - 6)
				pushOffsetPx[i] += PUSH_SPEED_PX;
			if (pushOffsetPx[i] > PUSH_MAX_OFFSET_PX) pushOffsetPx[i] = PUSH_MAX_OFFSET_PX;
			if (pushOffsetPx[i] < -PUSH_MAX_OFFSET_PX) pushOffsetPx[i] = -PUSH_MAX_OFFSET_PX;
		}
	}
	}
}

static uint16_t platform_emit_swing(const PlatformDef *d, VDPSprite *list,
                                    uint16_t firstIndex, uint16_t firstLink, uint16_t maxCount,
                                    uint16_t camX, uint16_t camY)
{
	int32_t angle, ampSwingY, seatPx, seatPy, cnt, i;
	uint16_t n = 0;
	uint8_t flip = 0;
	const ObjFrame *linkF = &pfFrames[PF_SWING_LINK];
	const ObjFrame *hubF = &pfFrames[PF_SWING_HUB];
	const ObjFrame *seatF = &pfFrames[PF_SWING_SEAT];

	if (!swingLive) return 0;

	angle = swing_angle(d, g_platform_tick_md);
	ampSwingY = d->amplitudeY >> 6;
	swing_pos(d, g_platform_tick_md, &seatPx, &seatPy);   /* seat's own real position */
	cnt = (ampSwingY >> 10) - 1;
	if (cnt < 0) cnt = 0;

	/* Chain links: radius 0x400*(i+1) [16.16] from centre, direction `angle`
	 * -- Platform_Draw's own chain loop (Platform.c:119-137). Drawn
	 * axis-aligned: real MD/32X hardware sprites cannot rotate to follow the
	 * arc the way the original's FX_ROTATE does, so only alternating FLIP_X
	 * (self->direction ^= FLIP_X, Platform.c:132) survives here -- a
	 * hardware-forced compromise, not a choice, see platform.h's own
	 * comment/this batch's report. */
	for (i = 0; i < cnt && n < maxCount; i++) {
		int32_t radius = (i + 1) << 10;
		int32_t lx = d->x + ((radius * platform_cos1024(angle)) >> 16);
		int32_t ly = d->y + ((radius * platform_sin1024(angle)) >> 16);
		n = (uint16_t)(n + obj_emit_pieces(list, (uint16_t)(firstIndex + n), (uint16_t)(firstLink + n),
		               (uint16_t)(maxCount - n), &pfPieces[linkF->pieceOffset], linkF->pieceCount,
		               linkF->tileOffset, PLATFORM_PAL,
		               (int16_t)(lx - (int16_t)camX), (int16_t)(ly - (int16_t)camY),
		               linkF->pivotX, linkF->pivotY, flip, 0, 0));
		flip ^= 1;
	}
	if (n < maxCount) {
		n = (uint16_t)(n + obj_emit_pieces(list, (uint16_t)(firstIndex + n), (uint16_t)(firstLink + n),
		               (uint16_t)(maxCount - n), &pfPieces[hubF->pieceOffset], hubF->pieceCount,
		               hubF->tileOffset, PLATFORM_PAL,
		               (int16_t)(d->x - (int16_t)camX), (int16_t)(d->y - (int16_t)camY),
		               hubF->pivotX, hubF->pivotY, 0, 0, 0));
	}
	if (n < maxCount) {
		n = (uint16_t)(n + obj_emit_pieces(list, (uint16_t)(firstIndex + n), (uint16_t)(firstLink + n),
		               (uint16_t)(maxCount - n), &pfPieces[seatF->pieceOffset], seatF->pieceCount,
		               seatF->tileOffset, PLATFORM_PAL,
		               (int16_t)(seatPx - (int16_t)camX), (int16_t)(seatPy - (int16_t)camY),
		               seatF->pivotX, seatF->pivotY, 0, 0, 0));
	}
	return n;
}

__attribute__((noinline))
uint16_t platform_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                       uint16_t camX, uint16_t camY,
                       int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	uint16_t lo, hi, i, n = 0;
	(void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;

	if (!normalLive && !swingLive) return 0;

	obj_type_window(&platformWinDesc, camX, &lo, &hi);

	for (i = lo; i < hi && n < PLATFORM_SPRITE_CAP; i++) {
		PlatformDef d;
		int32_t px, py;
		const ObjFrame *f;
		const uint8_t *rec = k_platform_bytes + (uint32_t)i * PLATFORM_RECORD_SIZE;

		/* PEEK-BEFORE-DECODE (Job 2, this task): same two skip conditions as
		 * before (d.frameID<0, d.collision==PLATFORM_C_NONE), tested directly
		 * on the two raw bytes that platform_read() would decode them from,
		 * BEFORE paying for the rest of that decode -- amplitudeX/amplitudeY/
		 * angle are each their own 4-byte big-endian reconstruction
		 * (rd_i32()), work this entry may never need. Exactly the same
		 * "peek the raw byte, skip before the full decode" shape
		 * platform_tick() above already uses for its own rawType gate.
		 * platform.c's own widened 280px window margin (this file's top
		 * comment) means this loop's candidate range often includes rows
		 * that end up skipped here -- every one of them used to pay for a
		 * full 30-byte decode first. Provably equivalent: rec[15]/rec[16]
		 * are the exact same bytes platform_read() reads into
		 * d.frameID/d.collision (`d->frameID = (int8_t)rec[15];
		 * d->collision = rec[16];`), from the same immutable ROM record, so
		 * testing them here first can never see a different value -- see
		 * this task's own host-harness equivalence proof
		 * (job2_platform_peek_harness.c). */
		if ((int8_t)rec[15] < 0) continue;
		if (rec[16] == PLATFORM_C_NONE) continue;   /* invisible driver row */

		platform_read(i, &d);

		if (d.type == PLATFORM_SWING) {
			n = (uint16_t)(n + platform_emit_swing(&d, list, (uint16_t)(firstIndex + n),
			               (uint16_t)(firstLink + n), (uint16_t)(PLATFORM_SPRITE_CAP - n), camX, camY));
			continue;
		}
		if (!normalLive) continue;

		/* VRAM ART BUDGET: only ONE "normal" art frame is resident now (this
		 * file's own top comment) -- every FIXED/FALL/LINEAR/PUSH instance
		 * draws pfFrames[0] regardless of its own real d.frameID. Position
		 * math is untouched (still reads the real per-type formulas above),
		 * and frame_hitbox(d.frameID) elsewhere (platform_tick()'s own FALL/
		 * PUSH touch tests) still reads the REAL frameID unchanged -- only
		 * the drawn ART collapsed, not collision/gameplay. */
		switch (d.type) {
		case PLATFORM_FIXED:
			fixed_pos(&d, &px, &py);
			f = &pfFrames[0];
			break;
		case PLATFORM_FALL:
			if (!fall_pos(&d, i, &px, &py)) continue;
			f = &pfFrames[0];
			break;
		case PLATFORM_LINEAR:
			linear_pos(&d, g_platform_tick_md, &px, &py);
			f = &pfFrames[0];
			break;
		case PLATFORM_PUSH:
			px = d.x + pushOffsetPx[i];
			py = d.y;
			f = &pfFrames[0];
			break;
		default:
			continue;
		}

		n = (uint16_t)(n + obj_emit_pieces(list, (uint16_t)(firstIndex + n), (uint16_t)(firstLink + n),
		               (uint16_t)(PLATFORM_SPRITE_CAP - n), &pfPieces[f->pieceOffset], f->pieceCount,
		               f->tileOffset, PLATFORM_PAL,
		               (int16_t)(px - (int16_t)camX), (int16_t)(py - (int16_t)camY),
		               f->pivotX, f->pivotY, 0, 0, 0));
	}
	return n;
}
