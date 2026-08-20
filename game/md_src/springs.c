#include "springs.h"
#include "vdp.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_pool.h"
#include "spring_data.h"
#include "sonic_data.h"    /* SONIC_FRAME_COUNT */
#include "assets_gen.h"

/* tools/convert_springs.py's kept-spring count for GHZ1 (Mania filter),
 * same "assert the leading count word matches" convention rings.c's
 * RING_COUNT already uses. */
#define SPRING_COUNT 35

/* springs_tick()'s own Sonic-proximity gate (2026-08-18, 68000 per-frame
 * cost task). Widest touch test this loop ever runs: spring_hitbox's own
 * largest half-extent is 16 (V/H orientations' own {-16,-8,16,8}/{-8,-16,8,
 * 16}), plus Sonic's own widest published hitbox half-width -- comfortably
 * under 32px in every generated ASSET_SONIC_HITBOX row -- so 48px of slack
 * on top of that sum is generous, round, safe margin, the same style every
 * other hand-picked margin in this codebase already uses. */
#define SPRING_TICK_MARGIN 48

/* assets/ghz/springs.bin: big-endian u16 count, then SPRING_COUNT entries of
 * (int16 x, int16 y, uint8 type, uint8 flipFlag), ascending by x -- Spring.c's
 * own field values (type 0-5, flipFlag the raw editor value), NOT
 * pre-multiplied or transformed. m68k is big-endian and this struct's fields
 * are already naturally 2-byte aligned with no compiler padding (6 bytes:
 * 2+2+1+1), so it overlays the file's own byte layout directly, and doubles
 * as the generic engine's own entries record (obj_data.h's ObjTypeDesc.
 * entries convention: every record starts with `int16_t x, y;`, exactly
 * this struct's first two fields). */
typedef struct {
	int16_t x, y;
	uint8_t type, flipFlag;
} SpringEntry;

/* This table and the tile pixels below live in bank 1 now (tools/
 * gen_assets.py's manifest), the same as every other converted asset --
 * ghz_spring_count_md/ghz_spring_xy_md come from ONE manifest entry
 * (ASSET_GHZ_SPRINGS, assets/ghz/springs.bin: big-endian u16 count then the
 * xy table immediately after), the count pointer and a +2 byte offset for
 * the array, same derivation md_src/assets.s used to do at assembly level
 * with `ghz_ring_xy = ghz_ring_count + 2` for rings.bin's identical shape
 * (see md_src/rings.c). */
static const uint16_t *const ghz_spring_count_md = ASSET_GHZ_SPRINGS;
static const SpringEntry *const ghz_spring_xy_md =
	(const SpringEntry *)((const uint8_t *)ASSET_GHZ_SPRINGS + 2);
static const uint32_t *const spring_tiles_md = ASSET_SPRING_TILES;
static const uint32_t *const spring_stream_tiles_md = ASSET_SPRING_STREAM_TILES;

/* assets/sonic/hitbox.bin: animator hitbox 0 ("outer") per Sonic frame, same
 * table rings.c's own touch test reads (ring_touches_sonic's doc comment
 * has the full RSDK derivation -- this is the same
 * Player_CheckCollisionTouch box shape, just against a spring's own hitbox
 * instead of a ring's fixed one). */
static const int8_t *const sonic_hitbox = ASSET_SONIC_HITBOX;

/* Player_FallbackHitbox (Player.c:12), same fallback rings.c's
 * ring_touches_sonic uses for a frameIndex this table has no row for --
 * cannot happen once boot has produced a first frame, kept as the same
 * defensive stand-in. */
#define FALLBACK_HITBOX_LEFT   (-10)
#define FALLBACK_HITBOX_TOP    (-20)
#define FALLBACK_HITBOX_RIGHT    10
#define FALLBACK_HITBOX_BOTTOM   20

static uint8_t springsLive;

/* Local ObjFrame/ObjPiece tables, built once in springs_init() from the
 * generated spring_resident[]/spring_resident_pieces[]/spring_stream[][0]/
 * spring_stream_pieces[] tables (spring_data.h, tools/convert_spring.py).
 * Index 2*o is orientation o's resident (rest) pose, 2*o+1 its one sampled
 * streamed bounce pose (tools/convert_spring.py's own SPRING_STREAM_FRAME_
 * IDS=[4] deviation: frame 4 of the original's 8-frame bounce cycle,
 * start/peak/settle coarsened down to start/peak -- see that converter's
 * own docstring). Every piece in both the generated resident and stream
 * tables is already frame-local (tile 0 for every single-piece resident
 * frame; the streamed diagonal's 3 pieces restart at 0 too, since only one
 * frame is ever loaded into the shared window at once) -- copied as-is,
 * no re-basing needed (contrast signpost.c's own post-piece shim, which
 * does need one; see that file's comment for why the two generated tables
 * differ). tileOffset becomes each frame's ABSOLUTE VRAM tile base
 * (obj_data.h's ObjFrame comment): residentBase + the generated resident
 * tileOffset for 2*o (residentBase is a single firstTile, since every
 * orientation's resident tiles are uploaded in ONE contiguous DMA whose
 * layout already matches the generated tileOffset values by construction);
 * the shared streamBase, unchanged, for every 2*o+1. */
static ObjFrame sp_frames[SPRING_ORIENT_COUNT * 2];
static ObjPiece  sp_pieces[SPRING_MAX_RESIDENT_PIECES * SPRING_ORIENT_COUNT
                           + SPRING_MAX_STREAM_PIECES * SPRING_ORIENT_COUNT];

/* Spring_State_*'s own animator speed on trigger (Spring.c:167/195/243/286/
 * 342): self->animator.speed = 0x80, self->animator.frameID = 1. Paired
 * with a converted frame's own raw RSDK duration (ObjFrame.duration,
 * RSDK.ProcessAnimation's per-frame value -- same "timer accumulates by
 * speed, frame advances once timer exceeds duration" arithmetic rings.c's
 * sparkle animator already transcribes) to get how many 60Hz ticks this
 * port holds the ONE sampled bounce frame before reverting to resident: the
 * original would have advanced through frames 2..8 using each of their own
 * durations, not just frame 4's -- this port only converted frame 4 (the
 * bounce's peak), so "hold the peak for as long as the real animator would
 * have held it before moving on" is the closest faithful reading of the
 * decompilation the available converted data supports, and stands in for
 * the fuller multi-frame sequence tools/convert_spring.py's own docstring
 * already reports as a deviation (SPRING_STREAM_FRAME_IDS). */
#define SPRING_ANIM_SPEED 0x80

static int16_t  activeSpring = -1;   /* scene index currently bouncing, -1 none */
static uint16_t bounceTicksLeft;
static uint8_t  streamOrient;        /* which orientation's frame is loaded */
static uint8_t  streamDirty;
static uint16_t streamBase;          /* set by springs_set_stream_base() */

static uint16_t bounce_ticks(uint8_t orient)
{
	uint32_t duration = sp_frames[orient * 2 + 1].duration;
	return (uint16_t)((duration + SPRING_ANIM_SPEED - 1) / SPRING_ANIM_SPEED);
}

/* Same symmetric AABB overlap test as rings.c's ring_touches_sonic (see
 * that function's own RSDK derivation comment) -- observational, not the
 * side-resolving collision box sh_src/spring.c's own physics test runs;
 * springs.h's own doc comment has the full reasoning for why this port's
 * visual trigger is the simpler of the two. */
static uint8_t spring_touches_sonic(int16_t sx, int16_t sy, const int8_t *sHb,
                                    int16_t px, int16_t py,
                                    int8_t hbLeft, int8_t hbTop, int8_t hbRight, int8_t hbBottom)
{
	return sx + sHb[0] < px + hbRight
	    && sx + sHb[2] > px + hbLeft
	    && sy + sHb[1] < py + hbBottom
	    && sy + sHb[3] > py + hbTop;
}

static ObjDrawDecision spring_decide(void *st, uint16_t entryIndex, int16_t ex, int16_t ey,
                                     int16_t sonicWorldX, int16_t sonicWorldY,
                                     uint16_t sonicFrameIndex)
{
	const SpringEntry *e = &ghz_spring_xy_md[entryIndex];
	ObjDrawDecision d;
	uint8_t o = (uint8_t)(e->type >> 1);
	(void)st; (void)ex; (void)ey; (void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;

	d.flipH = (uint8_t)((e->flipFlag & 1) != 0);   /* FLIP_X */
	d.flipV = (uint8_t)((e->flipFlag & 2) != 0);   /* FLIP_Y */
	/* Springs never move after spawn -- see obj_data.h's own ObjDrawDecision
	 * comment (Job 2, this task). */
	d.offX = 0; d.offY = 0;
	d.frame = (uint16_t)(o * 2 + ((activeSpring == (int16_t)entryIndex) ? 1 : 0));
	return d;
}

/* tilePixels/residentTileCount/countPtr are 0/NULL here on purpose --
 * springs' RESIDENT tile upload no longer goes through obj_type_init() (see
 * springs_init()/spring_arena_onBase() below, and the arena this now
 * registers the resident set with, md_src/obj_generic.h); the stream
 * portion never went through it either. obj_type_draw() itself never reads
 * any of the three. */
static ObjTypeDesc springType = {
	(const void *)0, sizeof(SpringEntry), SPRING_COUNT, (const uint16_t *)0,
	(const uint32_t *)0, 0,
	sp_frames, sp_pieces,
	OBJ_PRI_SPRING, SPRING_PAL, 0 /* low priority, matches rings/signpost */,
	16,                        /* marginX, matches the old camX-16/+16 window */
	spring_decide, (void *)0,
	/* Not templated (Job 1, this task): spring_decide()'s own d.flipV comes
	 * from each instance's scene-authored flipFlag and genuinely varies
	 * (FLIP_Y), which this fast path does not support -- see obj_data.h's
	 * own ObjTypeDesc.templatesH0/H1 comment. Legacy obj_emit_pieces() path
	 * unchanged. */
	(const ObjPieceTemplate *)0, (const ObjPieceTemplate *)0
};

/* spring_resident[i].tileOffset, stashed at init so spring_arena_onBase()
 * can rebase sp_frames[i*2].tileOffset every time the arena (re)grants
 * springs a base, not just the first time -- residentBase used to be a
 * permanent address computed once in the old firstTile-chained
 * springs_init(); now it can change across an evict/reload cycle. */
static uint16_t sp_residentRawOffset[SPRING_ORIENT_COUNT];

/* Fires once, the instant the arena (re)grants springs' resident set a VRAM
 * base -- rebases every orientation's resident-pose frame. The STREAM
 * frame's own tileOffset is untouched here: it belongs to the separate,
 * unreclaimed shared window springs_set_stream_base() still owns (see this
 * file's own header comment). */
static void spring_arena_onBase(uint16_t base)
{
	uint16_t i;
	for (i = 0; i < SPRING_ORIENT_COUNT; i++)
		sp_frames[i * 2].tileOffset = (uint16_t)(base + sp_residentRawOffset[i]);
}

static void spring_arena_onLive(uint8_t live) { springsLive = live; }

/* entries/tilePixels/lookaheadX are patched in at runtime in springs_init()
 * -- see rings.c's ringArenaDesc for why a static initializer can't do this
 * (residentTotal, needed for ARENA_LOOKAHEAD_X, is itself only known once
 * springs_init() sums spring_resident[]). */
static ArenaClassDesc springArenaDesc = {
	(const void *)0, sizeof(SpringEntry), SPRING_COUNT,
	(const uint32_t *)0, 0,
	0, OBJ_PRI_SPRING,
	spring_arena_onBase, spring_arena_onLive
};

__attribute__((noinline))
void springs_init(void)
{
	uint16_t i, pn = 0, residentTotal = 0;
	uint8_t slot;
	uint16_t base;

	springsLive = 0;

	for (i = 0; i < SPRING_ORIENT_COUNT; i++) residentTotal += spring_resident[i].tileCount;

	for (i = 0; i < SPRING_ORIENT_COUNT; i++) {
		/* SpringFrame -> ObjFrame: tools/convert_objects.py has since
		 * regenerated spring_data.h onto the shared ObjFrame type
		 * (obj_data.h) that this file's own ObjTypeDesc already assumes for
		 * springType's frames/pieces below -- a per-type SpringFrame typedef
		 * no longer exists to name here. Same fields (tileOffset,
		 * pieceOffset, tileCount, pieceCount, pivotX, pivotY, duration), so
		 * this rename is a no-op on every read below. */
		const ObjFrame *rf = &spring_resident[i];
		const ObjFrame *bf = &spring_stream[i][0];
		const ObjPiece *rp = &spring_resident_pieces[rf->pieceOffset];
		const ObjPiece *bp = &spring_stream_pieces[bf->pieceOffset];
		uint8_t k;

		sp_residentRawOffset[i] = rf->tileOffset;
		sp_frames[i * 2].tileOffset = 0;   /* real value set by spring_arena_onBase */
		sp_frames[i * 2].pieceOffset = pn;
		sp_frames[i * 2].tileCount = rf->tileCount;
		sp_frames[i * 2].pieceCount = rf->pieceCount;
		sp_frames[i * 2].pivotX = rf->pivotX;
		sp_frames[i * 2].pivotY = rf->pivotY;
		sp_frames[i * 2].duration = rf->duration;
		for (k = 0; k < rf->pieceCount; k++, pn++) {
			/* Field-by-field, not a struct assignment: this codebase is
			 * -ffreestanding with no libc, and GCC lowers even a 4-byte
			 * struct copy to a memcpy() call under -Os/LTO here, which
			 * then fails to link (no memcpy anywhere in this project). */
			sp_pieces[pn].dx = rp[k].dx;
			sp_pieces[pn].dy = rp[k].dy;
			sp_pieces[pn].size = rp[k].size;
			sp_pieces[pn].tile = rp[k].tile;
		}

		sp_frames[i * 2 + 1].tileOffset = 0;   /* filled by springs_set_stream_base() */
		sp_frames[i * 2 + 1].pieceOffset = pn;
		sp_frames[i * 2 + 1].tileCount = bf->tileCount;
		sp_frames[i * 2 + 1].pieceCount = bf->pieceCount;
		sp_frames[i * 2 + 1].pivotX = bf->pivotX;
		sp_frames[i * 2 + 1].pivotY = bf->pivotY;
		sp_frames[i * 2 + 1].duration = bf->duration;
		for (k = 0; k < bf->pieceCount; k++, pn++) {
			sp_pieces[pn].dx = bp[k].dx;
			sp_pieces[pn].dy = bp[k].dy;
			sp_pieces[pn].size = bp[k].size;
			sp_pieces[pn].tile = bp[k].tile;
		}
	}

	activeSpring = -1;
	streamDirty = 0;

	/* Same staleness guard obj_type_init() used to run before it ever
	 * touched VRAM: springs.bin's own leading count word has to still match
	 * the compile-time SPRING_COUNT every entries-table read below assumes. */
	if (*ghz_spring_count_md != SPRING_COUNT) return;

	springType.entries = (const void *)ghz_spring_xy_md;

	springArenaDesc.entries = (const void *)ghz_spring_xy_md;
	springArenaDesc.tilePixels = spring_tiles_md;
	springArenaDesc.tileCount = residentTotal;
	springArenaDesc.lookaheadX = ARENA_LOOKAHEAD_X(residentTotal);
	slot = obj_arena_register(&springArenaDesc);
	base = obj_arena_boot_load(slot);
	if (base == 0xFFFF) return;
	vdp_tiles_load(spring_tiles_md, base, residentTotal);
	obj_arena_boot_done(slot);
}

void springs_set_stream_base(uint16_t base)
{
	uint16_t i;
	streamBase = base;
	for (i = 0; i < SPRING_ORIENT_COUNT; i++) sp_frames[i * 2 + 1].tileOffset = base;
}

__attribute__((noinline))
void springs_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	uint16_t i;
	int8_t hbLeft, hbTop, hbRight, hbBottom;

	if (!springsLive) return;

	if (sonicFrameIndex < SONIC_FRAME_COUNT) {
		const int8_t *hb = &sonic_hitbox[sonicFrameIndex * 4];
		hbLeft = hb[0]; hbTop = hb[1]; hbRight = hb[2]; hbBottom = hb[3];
	} else {
		hbLeft = FALLBACK_HITBOX_LEFT;   hbTop    = FALLBACK_HITBOX_TOP;
		hbRight = FALLBACK_HITBOX_RIGHT; hbBottom = FALLBACK_HITBOX_BOTTOM;
	}

	for (i = 0; i < SPRING_COUNT; i++) {
		const SpringEntry *e = &ghz_spring_xy_md[i];
		uint8_t o;
		const int8_t *sHb;

		/* Proximity gate -- see SPRING_TICK_MARGIN's own comment. No
		 * integrated per-instance state exists to protect here (the only
		 * state this function ever writes -- activeSpring/bounceTicksLeft
		 * -- is ONE shared value, not per-i, and is only ever set the
		 * instant a touch is detected, never advanced independently of
		 * this check), so skipping a far entry's touch test is exactly
		 * equivalent to running it and getting "no touch", never a
		 * approximation. */
		if (e->x < sonicWorldX - SPRING_TICK_MARGIN || e->x > sonicWorldX + SPRING_TICK_MARGIN)
			continue;

		o = (uint8_t)(e->type >> 1);
		sHb = spring_hitbox[o];

		if (spring_touches_sonic(e->x, e->y, sHb, sonicWorldX, sonicWorldY,
		                         hbLeft, hbTop, hbRight, hbBottom)) {
			activeSpring = (int16_t)i;
			streamOrient = o;
			bounceTicksLeft = bounce_ticks(o);
			streamDirty = 1;
		}
	}

	if (activeSpring >= 0) {
		if (bounceTicksLeft > 0) bounceTicksLeft--;
		else activeSpring = -1;
	}
}

__attribute__((noinline))
void springs_upload(void)
{
	const ObjFrame *f;   /* SpringFrame -> ObjFrame; see springs_init()'s own comment */

	if (!springsLive || !streamDirty) return;

	streamDirty = 0;
	f = &spring_stream[streamOrient][0];
	vdp_tiles_load(&spring_stream_tiles_md[f->tileOffset * 8], streamBase, f->tileCount);
}

__attribute__((noinline))
uint16_t springs_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                      uint16_t camX, uint16_t camY,
                      int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	if (!springsLive) return 0;
	/* sonicWorldX/Y/sonicFrameIndex reach spring_decide() unused (see this
	 * function's own header comment on why they are accepted at all) -- the
	 * pre-migration code passed obj_type_draw() a hardcoded 0,0,0 here for
	 * the exact same reason (spring_decide() has always ignored them, see
	 * its own (void) casts), so threading the real values through instead
	 * changes nothing observable. */
	return obj_type_draw(&springType, list, firstIndex, firstLink, SPRING_SPRITE_CAP,
	                     camX, camY, sonicWorldX, sonicWorldY, sonicFrameIndex);
}
