#include "signpost.h"
#include "vdp.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_pool.h"
#include "signpost_data.h"
#include "assets_gen.h"

/* Signpost tile pixels live in bank 1 (tools/gen_assets.py's manifest), same
 * as springs.c's own spring_tiles_md -- see that file's comment. */
static const uint32_t *const signpost_tiles_md = ASSET_SIGNPOST_TILES;
static const uint32_t *const signpost_stream_tiles_md = ASSET_SIGNPOST_STREAM_TILES;

/* Scene1.bin slot 324 (Mania filter kept, type=1 SIGNPOST_DROP), verified
 * against the pack -- see this file's former header comment (now
 * signpost.h) for the landing-Y discussion: a real per-pixel floor scan at
 * x=15792 was attempted and found ambiguous ground (a background/arena-wall
 * structure from y=0 to ~y=1120, the next genuine floor 340px lower at
 * y=1552), well past where the scene places the entity -- since the
 * original never moves the signpost's Y at all (only X, and only when a
 * boss exists), the scene's own authored Y is already the intended resting
 * position by construction. TRIGGER_X is this port's own SIGNPOST_X - 64
 * deviation. */
#define SIGNPOST_X        15792
#define SIGNPOST_LANDING_Y 1208
#define SIGNPOST_TRIGGER_X (SIGNPOST_X - 64)

/* Not decomp values -- this port's own fall, see signpost.h's header
 * comment. One screen height above the landing spot is comfortably off the
 * top edge at the moment the trigger fires (SCREEN_HEIGHT is <=224 in this
 * port). 1px/tick roughly matches SignPost_HandleSpin's own ~237-tick
 * settle time over that distance; the two are otherwise independent (Y
 * clamps at SIGNPOST_LANDING_Y whenever it gets there first, and the spin
 * keeps running its own decay regardless -- cosmetically harmless, since
 * the original settling later than landing is normal there too, just on a
 * much shorter spinCount=4 decay this port does not reproduce). */
#define SIGNPOST_FALL_START_OFFSET 256
#define SIGNPOST_FALL_SPEED 1

#define SP_IDLE   0
#define SP_ACTIVE 1   /* falling + spinning, SignPost_State_Falling+Spin merged */
#define SP_DONE   2   /* SignPost_State_Done: static, face fully revealed */

static uint16_t streamBase;
static uint8_t signpostLive;

/* Renamed from the old bare `state` to avoid shadowing signpost_decide()'s
 * own `state` parameter (obj_data.h's ObjDecideFn) -- this object has only
 * one instance, so decide() reads this and the face/step globals below
 * directly rather than through that generic per-type state pointer (NULL
 * in this type's own ObjTypeDesc; springs.c's per-entry bounce timers are
 * the case that pointer exists for). */
static uint8_t spState = SP_IDLE;
static int32_t posY;
static uint32_t angle, maxAngle;
static uint16_t spinSpeed;
static uint8_t spinCount;
static uint16_t rotation;         /* published each tick, 0-511 */

static uint8_t curFace;           /* 0 Sonic, 1 Eggman -- SignPost_Draw:31 */
static uint8_t curStep;           /* index into signpost_plate[face][] */
static uint8_t streamDirty;

/* Every record obj_generic.h's window/scan code reads starts with
 * `int16_t x, y;` (obj_data.h's ObjTypeDesc.entries convention) -- this
 * object has no real scene table, so these 4 rows (post top/sidebar/stand,
 * face plate) are synthetic, all sharing the signpost's one live anchor
 * point, kept current by signpost_tick() below rather than baked at
 * conversion time. */
typedef struct { int16_t x, y; } SignpostEntry;
static SignpostEntry sp_entries[4];

/* Local ObjFrame/ObjPiece tables, built once in signpost_init() from the
 * generated signpost_post[]/signpost_post_pieces[]/signpost_plate[][]/
 * signpost_plate_pieces[] tables (signpost_data.h, tools/convert_spring.py):
 * a small adapter, not a duplicate of the ~170-line skeleton this task
 * removes elsewhere. Two things this shim does that the generated tables
 * don't:
 *   1. duration=0 (ObjFrame's field the generated SignPostFrame predates --
 *      every signpost pose is a static frame, no per-frame animator here).
 *   2. tileOffset becomes each frame's ABSOLUTE VRAM tile base (the arena's
 *      own granted base, added to the post frame's own generated tileOffset
 *      by signpost_arena_onBase() below every time the arena (re)grants
 *      one -- see that function's own comment; streamBase, unchanged, for
 *      every plate frame, since the shared window holds exactly one frame
 *      at a time) rather than an offset into the source pixel array -- see
 *      obj_data.h's ObjFrame comment for why the generic engine wants
 *      that, and note this file's own post-piece tile values get re-based
 *      to match (see the loop below): the generated
 *      signpost_post_pieces[] already bakes each piece's frame.tileOffset
 *      into its own `tile` field (today's signpost_draw() only ever added
 *      the resident base to it, never `+ tileOffset`, and reads the right
 *      tiles precisely because of that pre-baking) while
 *      signpost_plate_pieces[] does not (every plate frame's pieces are
 *      already local/0-based, since only one frame is ever loaded into the
 *      shared window at once) -- two different, both already-correct
 *      conventions in the generated data that this shim normalizes into
 *      one uniform rule (piece.tile always frame-local) for the generic
 *      engine's single formula (tileBase = frame.tileOffset) to work on
 *      either without needing to know which convention a given frame came
 *      from. */
static ObjFrame sp_frames[3 + 2 * SIGNPOST_PLATE_STEPS];
static ObjPiece  sp_pieces[3 + 2 * SIGNPOST_PLATE_STEPS * 3];   /* generous; see init */

#define SP_PLATE_FRAME(face, step) (3 + (face) * SIGNPOST_PLATE_STEPS + (step))

static ObjDrawDecision signpost_decide(void *st, uint16_t entryIndex, int16_t ex, int16_t ey,
                                       int16_t sonicWorldX, int16_t sonicWorldY,
                                       uint16_t sonicFrameIndex)
{
	ObjDrawDecision d;
	(void)st; (void)ex; (void)ey; (void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;

	d.flipH = 0;
	d.flipV = 0;
	if (spState == SP_IDLE) { d.frame = OBJ_SKIP; return d; }

	if (entryIndex < 3) {
		/* Post Bits: 0 post top, 1 sidebar, 2 stand -- SignPost_Draw:55-59
		 * ("if (self->state)", drawn every frame the signpost is active),
		 * never flipped, same as today. */
		d.frame = entryIndex;
	} else {
		d.frame = SP_PLATE_FRAME(curFace, curStep);
	}
	return d;
}

static ObjTypeDesc signpostType = {
	sp_entries, sizeof(SignpostEntry), 4, 0,
	0, 0,                       /* signpost manages its own tile upload */
	sp_frames, sp_pieces,
	OBJ_PRI_SIGNPOST, SIGNPOST_PAL_POST, 0 /* low priority, matches rings/springs */,
	32,                          /* marginX, matches the old camX-32/+32 check */
	signpost_decide, 0
};

/* One fixed row, {SIGNPOST_X, SIGNPOST_LANDING_Y}, for the ARENA's own
 * window check (obj_generic.h's ArenaClassDesc.entries) -- deliberately NOT
 * sp_entries above: sp_entries is a live, signpost_tick()-updated RAM table
 * signpostType's own sprite-visibility window reads every frame (its four
 * rows track the signpost's actual falling Y), while residency only ever
 * needs "is the signpost's one real world X, which never moves, within
 * range" -- a static table sidesteps any ordering question between when
 * signpost_tick() refreshes sp_entries and when obj_arena_tick() runs. */
static const int16_t signpost_arena_xy[2] = { SIGNPOST_X, SIGNPOST_LANDING_Y };

/* signpost_post[i].tileOffset, stashed at init so signpost_arena_onBase()
 * can rebase sp_frames[i].tileOffset every time the arena (re)grants
 * signpost's post-bits a base, not just the first time. */
static uint16_t sp_postRawOffset[3];

static void signpost_arena_onBase(uint16_t base)
{
	uint16_t i;
	for (i = 0; i < 3; i++)
		sp_frames[i].tileOffset = (uint16_t)(base + sp_postRawOffset[i]);
}

static void signpost_arena_onLive(uint8_t live) { signpostLive = live; }

static ArenaClassDesc signpostArenaDesc = {
	(const void *)signpost_arena_xy, sizeof(int16_t) * 2, 1,
	(const uint32_t *)0, SIGNPOST_MAX_RESIDENT_TILES,
	ARENA_LOOKAHEAD_X(SIGNPOST_MAX_RESIDENT_TILES), OBJ_PRI_SIGNPOST,
	signpost_arena_onBase, signpost_arena_onLive
};

/* The shared spring/signpost STREAM window is a separate, unreclaimed fixed
 * allocation (obj_generic.h's own comment on why it stays out of the
 * arena): sized to SIGNPOST_MAX_STREAM_TILES (signpost_data.h, 24, the
 * larger of what springs.c's own 22-tile need and signpost's 24-tile need
 * ever ask for) and pinned at the very top of the ENTIRE usable tile range,
 * right below VDP_PLAN_A's own tilemap (VDP_PLAN_A >> 5 = 1536) -- not right
 * below TILE_FONTINDEX any more (2026-08-18 VRAM reclaim): TILE_FONTINDEX
 * (1312) .. VDP_PLAN_A>>5 (1536) is 224 tiles that were dead in this build
 * before this change -- the font's own 96-tile pattern range is loaded
 * (vdp_font_load, vdp.c) but never DISPLAYED (DEBUG_OVERLAY is 0, so
 * vdp_puts() never runs and no plane cell ever names a font tile index) and
 * Plane W's own 128-tile nametable range right after it is never read either
 * (Plane W's window registers are left at 0 in vdp_init(), covering zero
 * rows/columns) -- confirmed from these exact call sites, not assumed from
 * the prior "~224" estimate. Moving this window from right-below-the-font to
 * right-below-Plane-A frees that whole 224-tile range for the arena to reach
 * contiguously, instead of leaving it stranded on the far side of this
 * window. main.c's own arena-size arithmetic computes the arena's own upper
 * bound the same way, from the same two constants, so the two can never
 * disagree about where one ends and the other begins. */
#define SIGNPOST_STREAM_BASE ((uint16_t)((VDP_PLAN_A >> 5) - SIGNPOST_MAX_STREAM_TILES))

__attribute__((noinline))
void signpost_init(void)
{
	uint16_t i, pn = 0;
	uint8_t slot;
	uint16_t base;

	signpostLive = 0;
	streamBase = SIGNPOST_STREAM_BASE;

	/* Post Bits: copy pieces as-is (every one of signpost_post_pieces[]'s
	 * three rows already has tile=0 -- verified, not assumed: each of the
	 * 3 post pieces is its own single ObjPiece, sized to its own frame's
	 * tileCount [1/4/6 tiles] with no within-frame offset of its own to
	 * carry), frame.tileOffset becomes each frame's absolute VRAM base.
	 *
	 * FOUND WHILE MIGRATING, NOT INTRODUCED BY THIS TASK: the pre-migration
	 * signpost_draw() called obj_emit_pieces(..., residentBase, ...) for
	 * all 3 post pieces -- the SAME tileBase every time, never adding this
	 * frame's own tileOffset (0/1/5) on top. Since every piece.tile is 0,
	 * that made every post piece read from residentBase+0 (the post-top
	 * tile) regardless of which piece it was: the sidebar and stand were
	 * drawing the WRONG tiles (post-top's own graphic, resized to their
	 * own bounding box) on every frame the signpost was ever visible, a
	 * pre-existing, silent, until-now-undetected rendering bug, not
	 * something this task's own changes created. Caught by this task's own
	 * native old-vs-new comparison harness (see the report), which is
	 * exactly the kind of thing that verification method catches and a
	 * quick glance at the emulator would not (three greyish/washed-looking
	 * signpost pieces at the very end of the act is easy to miss). Fixed
	 * here rather than preserved: the generic engine's one draw formula
	 * (tileBase = frame's own absolute tileOffset) is what every OTHER
	 * migrated frame already correctly relies on, and there is no reading
	 * of the decompilation or the converted data that makes "always read
	 * the post-top tile" the intended behavior. Reported as a real,
	 * understood compromise this task's report flags, not a silent fix. */
	for (i = 0; i < 3; i++) {
		/* SignPostFrame -> ObjFrame: tools/convert_objects.py has since
		 * regenerated signpost_data.h onto the shared ObjFrame type
		 * (obj_data.h) this file's own ObjTypeDesc already assumes for
		 * signpostType's frames/pieces below -- a per-type SignPostFrame
		 * typedef no longer exists to name here. Same fields this file ever
		 * reads off f (tileOffset, pieceOffset, tileCount, pieceCount,
		 * pivotX, pivotY); ObjFrame's own extra `duration` field is never
		 * read from f anywhere in this file (every sp_frames[...].duration
		 * write below is a literal 0, not f->duration -- see this file's
		 * own header comment on why), so this rename is a no-op. */
		const ObjFrame *f = &signpost_post[i];
		const ObjPiece *src = &signpost_post_pieces[f->pieceOffset];
		uint8_t k;

		sp_postRawOffset[i] = f->tileOffset;
		sp_frames[i].tileOffset = 0;   /* real value set by signpost_arena_onBase */
		sp_frames[i].pieceOffset = pn;
		sp_frames[i].tileCount = f->tileCount;
		sp_frames[i].pieceCount = f->pieceCount;
		sp_frames[i].pivotX = f->pivotX;
		sp_frames[i].pivotY = f->pivotY;
		sp_frames[i].duration = 0;

		for (k = 0; k < f->pieceCount; k++, pn++) {
			/* Field-by-field, not a struct assignment: this codebase is
			 * -ffreestanding with no libc, and GCC lowers even a 4-byte
			 * struct copy to a memcpy() call under -Os/LTO here, which
			 * then fails to link (no memcpy anywhere in this project). */
			sp_pieces[pn].dx = src[k].dx;
			sp_pieces[pn].dy = src[k].dy;
			sp_pieces[pn].size = src[k].size;
			sp_pieces[pn].tile = src[k].tile;
		}
	}

	/* Face plate: pieces are already frame-local (only one frame ever
	 * resident in the shared window at a time), copied as-is; every plate
	 * frame's tileOffset becomes the shared window's one absolute base. */
	{
		uint8_t face, step;
		for (face = 0; face < 2; face++) {
			for (step = 0; step < SIGNPOST_PLATE_STEPS; step++) {
				const ObjFrame *f = &signpost_plate[face][step];   /* SignPostFrame -> ObjFrame; see the post-bits loop's own comment above */
				const ObjPiece *src = &signpost_plate_pieces[f->pieceOffset];
				uint8_t k, idx = SP_PLATE_FRAME(face, step);

				sp_frames[idx].tileOffset = streamBase;
				sp_frames[idx].pieceOffset = pn;
				sp_frames[idx].tileCount = f->tileCount;
				sp_frames[idx].pieceCount = f->pieceCount;
				sp_frames[idx].pivotX = f->pivotX;
				sp_frames[idx].pivotY = f->pivotY;
				sp_frames[idx].duration = 0;

				for (k = 0; k < f->pieceCount; k++, pn++) {
					sp_pieces[pn].dx = src[k].dx;
					sp_pieces[pn].dy = src[k].dy;
					sp_pieces[pn].size = src[k].size;
					sp_pieces[pn].tile = src[k].tile;
				}
			}
		}
	}

	spState = SP_IDLE;
	streamDirty = 0;

	slot = obj_arena_register(&signpostArenaDesc);
	base = obj_arena_boot_load(slot);
	if (base == 0xFFFF) return;   /* arena misconfigured -- see obj_arena_boot_load's own comment */
	vdp_tiles_load(signpost_tiles_md, base, SIGNPOST_MAX_RESIDENT_TILES);
	obj_arena_boot_done(slot);
}

uint16_t signpost_stream_tile_base(void) { return streamBase; }

/* SignPost_HandleSpin (SignPost.c:220-235), transcribed directly. */
static void handle_spin(void)
{
	angle += spinSpeed;
	if (angle >= maxAngle) {
		int32_t speed;

		maxAngle += 0x20000;
		speed = 0x600L * spinCount;
		spinSpeed = (uint16_t)(speed < 0x3000 ? speed : 0x3000);
		if (!--spinCount) {
			spinSpeed = 0;
			angle = 0x10000;
			spState = SP_DONE;
		}
	}
	rotation = (uint16_t)((angle >> 8) & 0x1FF);
}

/* Fold a 0-511 rotation into signpost_plate[]'s 0..SIGNPOST_PLATE_STEPS-1
 * baked widths: |cos(rotation)| is symmetric about 128/256/384 (each
 * quarter-turn), so every rotation maps onto the same one quarter-turn's
 * worth of steps tools/convert_spring.py baked. SIGNPOST_PLATE_STEPS is 2
 * (that converter's own reported deviation), so the nearest-step rounding
 * collapses to one threshold compare against the quarter-turn's midpoint
 * (64 of 0-128). */
static uint8_t rotation_to_step(uint16_t rot)
{
	uint16_t r = rot & 0x1FF;
	uint16_t folded;

	if (r <= 128) folded = r;
	else if (r <= 256) folded = 256 - r;
	else if (r <= 384) folded = r - 256;
	else folded = 512 - r;

	return (folded >= 64) ? (SIGNPOST_PLATE_STEPS - 1) : 0;
}

__attribute__((noinline))
void signpost_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	uint8_t face, step, i;
	(void)sonicWorldY; (void)sonicFrameIndex;   /* see this function's own doc comment (signpost.h) */

	if (!signpostLive) return;

	if (spState == SP_IDLE) {
		if (sonicWorldX < SIGNPOST_TRIGGER_X) return;
		spState = SP_ACTIVE;
		posY = SIGNPOST_LANDING_Y - SIGNPOST_FALL_START_OFFSET;
		angle = 0;
		maxAngle = 0x10000;
		spinSpeed = 0x3000;
		spinCount = 16;
	}

	if (spState == SP_ACTIVE) {
		posY += SIGNPOST_FALL_SPEED;
		if (posY > SIGNPOST_LANDING_Y) posY = SIGNPOST_LANDING_Y;
		handle_spin();
	}

	/* SignPost_Draw:31: rotation<=128 or >=384 picks the egg plate. */
	face = (rotation <= 128 || rotation >= 384) ? 1 : 0;
	step = rotation_to_step(rotation);
	if (face != curFace || step != curStep) {
		curFace = face;
		curStep = step;
		streamDirty = 1;
	}

	for (i = 0; i < 4; i++) {
		sp_entries[i].x = SIGNPOST_X;
		sp_entries[i].y = (int16_t)posY;
	}
}

__attribute__((noinline))
void signpost_upload(void)
{
	const ObjFrame *f;   /* SignPostFrame -> ObjFrame; see signpost_init()'s own comment */

	if (!signpostLive || !streamDirty || spState == SP_IDLE) return;

	streamDirty = 0;
	f = &signpost_plate[curFace][curStep];
	vdp_tiles_load(&signpost_stream_tiles_md[f->tileOffset * 8],
	               streamBase, f->tileCount);
}

__attribute__((noinline))
uint16_t signpost_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                       uint16_t camX, uint16_t camY,
                       int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	if (!signpostLive) return 0;
	/* sonicWorldX/Y/sonicFrameIndex reach signpost_decide() unused (see this
	 * function's own header comment, signpost.h) -- the pre-migration code
	 * passed obj_type_draw() a hardcoded 0,0,0 here for the exact same
	 * reason (signpost_decide() has always ignored them), so threading the
	 * real values through instead changes nothing observable. */
	return obj_type_draw(&signpostType, list, firstIndex, firstLink, SIGNPOST_SPRITE_CAP,
	                     camX, camY, sonicWorldX, sonicWorldY, sonicFrameIndex);
}
