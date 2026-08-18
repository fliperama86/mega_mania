/* Green Hill Zone, converted to Mega Drive assets, with Sonic on it.
 *
 * The point of this ROM is to prove the pipeline end to end: tile reduction,
 * palette fitting, layout conversion, hardware scrolling with rows and
 * columns streamed in as the camera moves, and the ported physics and
 * collision driving the real character sprite. The 68000 handles all VDP
 * work; pad input, physics, collision, camera and animation run on the
 * slave SH2 and cross back over the comm-register protocol (comm.h).
 *
 * Blocks are 16x16, so two cells by two. Plane A is 64x32 cells, which is 32
 * blocks by 16, and it wraps in both axes, so scrolling only ever needs the
 * one row or column that just came into view.
 */

#include "md.h"
#include "pad.h"
#include "sonic.h"
#include "descriptor.h"
#include "comm.h"
#include "audio.h"
#include "cd.h"
#include "rings.h"
#include "springs.h"
#include "signpost.h"
#include "decoration.h"
#include "signpost_data.h"   /* SIGNPOST_MAX_STREAM_TILES, for the arena/stream-window boundary below */
#include "obj_generic.h"     /* the shared VRAM tile residency arena */
#include "obj_pool.h"
#include "motobug.h"
#include "crabmeat.h"
#include "buzzbomber.h"
#include "chopper.h"
#include "newtron.h"
#include "batbrain.h"
#include "spikes.h"
#include "spikelog.h"
#include "breakablewall.h"
#include "itembox.h"
#include "platform.h"
#include "bridge.h"
#include "collapsingplatform.h"
#include "bank.h"
#include "assets_gen.h"

/* 1: draw the top-row debug text (frame counter, pad, CD state, rings). */
#define DEBUG_OVERLAY 0

/* Track to loop once the disc is spinning; the disc image built alongside
 * this ROM by tools/make_disc.py is audio-only and starts at track 1. */
#define CD_MUSIC_TRACK 2

/* Every asset below lives in cartridge bank 1, reached through the banked
 * window (md_src/bank.h) -- assets_gen.h's pointers already carry that
 * window's base address, tools/gen_assets.py's manifest is the one place
 * their addresses are computed, and main() selects bank 1 exactly once, at
 * the top of main() below, before any of these is first read. */
static const uint16_t *const ghz_pal    = ASSET_GHZ_PAL;
static const uint16_t *const ghz_blocks = ASSET_GHZ_BLOCKS;
static const uint16_t *const ghz_map    = ASSET_GHZ_MAP;
/* ghz_bgmap (assets/ghz/map_bg.bin) has no reader anywhere in this codebase
 * -- discovered while writing tools/gen_assets.py's manifest, not introduced
 * by it. Its bytes are still relocated into bank 1 (see the manifest's own
 * comment on this entry) so this task does not unilaterally drop 24,576
 * bytes of asset data; nothing in main.c defines a pointer for it any more
 * since nothing here ever read one. Flagged in this task's final report. */
/* sonic_pal used to be `extern const uint16_t sonic_pal[16];`, resolved
 * against md_src/sonic_data.c's own link (tools/convert_sonic.py's
 * generated sonic_data.h declared it); that generator no longer declares
 * it -- see its own comment -- since the data moved to bank 1 too. */
static const uint16_t *const sonic_pal = ASSET_SONIC_PAL;

/* blocks; GHZ_MAP_W/GHZ_MAP_H (descriptor.h) are the one definition, also
 * used to fill the descriptor published to the SH2 below, so this and that
 * copy can never disagree. */
#define MAP_W      GHZ_MAP_W
#define MAP_H      GHZ_MAP_H
#define MAP_BLOCK_MASK 0x0FFF   /* bits 12-13 carry collision solidity */
#define MAP_SOLID_FLOOR 0x1000
#define MAP_SOLID_SIDES 0x2000
#define TILE_BASE  TILE_USERINDEX

/* FG High (loop fronts, overhangs): tools/convert_stage.py's map_fgh.bin,
 * same cell format as ghz_map above, sharing its MAP_W/MAP_H (converter-
 * asserted equal). Linked into the slave SH2's image too (generated
 * sh_src/assets_gen.s, ghz_map_fgh's sh_link_name in tools/gen_assets.py's
 * manifest, since sh_src/path.c links against it by name directly -- see
 * that manifest entry's own comment); this pointer is the same bytes'
 * 68000-side address, in bank 1. */
static const uint16_t *const ghz_map_fgh_md = ASSET_GHZ_MAP_FGH;

/* GHZ's main tileset (assets/ghz/tiles.bin, tools/convert_stage.py): the
 * asset that originally proved out md_src/bank.h's bank register end to
 * end, now just one more entry in tools/gen_assets.py's manifest alongside
 * everything else bank 1 holds.
 *
 * GHZ_TILE_COUNT replaces what used to be a runtime `(ghz_tiles_end -
 * ghz_tiles) / 8` computed from this program's own linker symbols -- not
 * possible once the data left this program's link entirely, and no longer a
 * hand-typed `33152 / 32` either: ASSET_GHZ_TILES_SIZE is generated from the
 * same manifest entry that placed the data itself, so the two can never
 * disagree. 32 bytes/tile is 8 big-endian u32s, same packing as
 * sonic_tiles/sonicrot. */
static const uint32_t *const ghz_tiles_banked = ASSET_GHZ_TILES;
#define GHZ_TILE_COUNT (ASSET_GHZ_TILES_SIZE / 32)

#define VIEW_BLOCKS_X 32        /* 64 cells */
#define VIEW_BLOCKS_Y 16        /* 32 cells */

#define BGMAP_W    512          /* BG Outside, from convert_stage.py */
#define BGMAP_H    24
/* Which BG row lands at the top of the screen. Lower puts the horizon further
 * down, which is how Mania frames it. */
#define BG_TOP_ROW 3

/* Screen's top-left corner in the world, published by the slave SH2 over the
 * comm protocol, already clamped to the map (see sh_src/s_main.c). */
static uint16_t camX;
static uint16_t camY;

/* First block of the currently-populated plane window: draw_block_column
 * uses firstRow to know which world rows the column it is drawing covers,
 * draw_block_row uses firstCol the same way for columns. Both are kept in
 * lockstep with camX/camY by the streaming code in the main loop. */
static uint16_t firstCol;
static uint16_t firstRow;

/* One column of blocks, two cells wide, straight into plane A. World block
 * (blockX, firstRow + i) always lives at plane cell
 * ((blockX*2)&(PLAN_WIDTH-1), ((firstRow+i)*2)&(PLAN_HEIGHT-1)); that is a
 * pure function of world coordinates, so it does not matter where in the
 * window firstRow currently sits. */
static void draw_block_column(uint16_t blockX)
{
	uint16_t cellX = (blockX * 2) & (PLAN_WIDTH - 1);
	uint16_t colL[VIEW_BLOCKS_Y * 2];
	uint16_t colR[VIEW_BLOCKS_Y * 2];
	uint16_t i;

	if (blockX >= MAP_W) return;

	for (i = 0; i < VIEW_BLOCKS_Y; i++) {
		uint16_t by = firstRow + i;
		/* bits 12-13 are collision flags, not part of the index */
		uint16_t b = 0, bh = 0;
		const uint16_t *e, *eh;

		if (by < MAP_H) {
			b  = ghz_map[by * MAP_W + blockX] & MAP_BLOCK_MASK;
			bh = ghz_map_fgh_md[by * MAP_W + blockX] & MAP_BLOCK_MASK;
			/* breakablewall.h's own hook: a broken instance's footprint
			 * forces block index 0 (the map's own existing "out of range"
			 * empty sentinel, same value `by>=MAP_H` above already uses)
			 * on whichever layer it lives on, EVERY time this cell streams
			 * back into view -- see that header's own comment for why a
			 * one-shot VDP write at break time is not enough (this loop
			 * re-reads straight from ROM every single call). */
			if (breakablewall_block_override(blockX, by, 0)) b = 0;
			if (breakablewall_block_override(blockX, by, 1)) bh = 0;
		}
		e  = &ghz_blocks[b * 4];
		eh = &ghz_blocks[bh * 4];

		colL[i * 2 + 0] = e[0] + TILE_BASE;
		colR[i * 2 + 0] = e[1] + TILE_BASE;
		colL[i * 2 + 1] = e[2] + TILE_BASE;
		colR[i * 2 + 1] = e[3] + TILE_BASE;

		/* FG High goes straight to Plane B here rather than through a second
		 * buffer-then-write pass like Plane A above: high priority, so it
		 * draws above Plane A and above low-priority sprites, which is FG
		 * High's above-the-player stacking (see sonic.c's sonic_build(),
		 * where Sonic's own sprite priority is set to match). */
		{
			uint16_t cellY = (by * 2) & (PLAN_HEIGHT - 1);
			vdp_map_xy(VDP_PLAN_B, (eh[0] + TILE_BASE) | 0x8000, cellX,     cellY);
			vdp_map_xy(VDP_PLAN_B, (eh[1] + TILE_BASE) | 0x8000, cellX + 1, cellY);
			vdp_map_xy(VDP_PLAN_B, (eh[2] + TILE_BASE) | 0x8000, cellX,     cellY + 1);
			vdp_map_xy(VDP_PLAN_B, (eh[3] + TILE_BASE) | 0x8000, cellX + 1, cellY + 1);
		}
	}

	for (i = 0; i < VIEW_BLOCKS_Y; i++) {
		uint16_t cellY = ((firstRow + i) * 2) & (PLAN_HEIGHT - 1);
		vdp_map_xy(VDP_PLAN_A, colL[i * 2 + 0], cellX,     cellY);
		vdp_map_xy(VDP_PLAN_A, colR[i * 2 + 0], cellX + 1, cellY);
		vdp_map_xy(VDP_PLAN_A, colL[i * 2 + 1], cellX,     cellY + 1);
		vdp_map_xy(VDP_PLAN_A, colR[i * 2 + 1], cellX + 1, cellY + 1);
	}
}

/* Mirror of draw_block_column: one row of blocks, two cells tall, across the
 * currently visible column window (firstCol .. firstCol + VIEW_BLOCKS_X - 1). */
static void draw_block_row(uint16_t blockY)
{
	uint16_t cellY = (blockY * 2) & (PLAN_HEIGHT - 1);
	uint16_t rowT[VIEW_BLOCKS_X * 2];
	uint16_t rowB[VIEW_BLOCKS_X * 2];
	uint16_t i;

	if (blockY >= MAP_H) return;

	for (i = 0; i < VIEW_BLOCKS_X; i++) {
		uint16_t bx = firstCol + i;
		uint16_t b = 0, bh = 0;
		const uint16_t *e, *eh;

		if (bx < MAP_W) {
			b  = ghz_map[blockY * MAP_W + bx] & MAP_BLOCK_MASK;
			bh = ghz_map_fgh_md[blockY * MAP_W + bx] & MAP_BLOCK_MASK;
			/* See draw_block_column's own comment on this same hook. */
			if (breakablewall_block_override(bx, blockY, 0)) b = 0;
			if (breakablewall_block_override(bx, blockY, 1)) bh = 0;
		}
		e  = &ghz_blocks[b * 4];
		eh = &ghz_blocks[bh * 4];

		rowT[i * 2 + 0] = e[0] + TILE_BASE;
		rowT[i * 2 + 1] = e[1] + TILE_BASE;
		rowB[i * 2 + 0] = e[2] + TILE_BASE;
		rowB[i * 2 + 1] = e[3] + TILE_BASE;

		/* FG High straight to Plane B; see draw_block_column's comment. */
		{
			uint16_t cx = (bx * 2) & (PLAN_WIDTH - 1);
			vdp_map_xy(VDP_PLAN_B, (eh[0] + TILE_BASE) | 0x8000, cx,     cellY);
			vdp_map_xy(VDP_PLAN_B, (eh[1] + TILE_BASE) | 0x8000, cx + 1, cellY);
			vdp_map_xy(VDP_PLAN_B, (eh[2] + TILE_BASE) | 0x8000, cx,     cellY + 1);
			vdp_map_xy(VDP_PLAN_B, (eh[3] + TILE_BASE) | 0x8000, cx + 1, cellY + 1);
		}
	}

	for (i = 0; i < VIEW_BLOCKS_X; i++) {
		uint16_t cellX = ((firstCol + i) * 2) & (PLAN_WIDTH - 1);
		vdp_map_xy(VDP_PLAN_A, rowT[i * 2 + 0], cellX,     cellY);
		vdp_map_xy(VDP_PLAN_A, rowT[i * 2 + 1], cellX + 1, cellY);
		vdp_map_xy(VDP_PLAN_A, rowB[i * 2 + 0], cellX,     cellY + 1);
		vdp_map_xy(VDP_PLAN_A, rowB[i * 2 + 1], cellX + 1, cellY + 1);
	}
}

static void draw_screen(void)
{
	uint16_t base = camX >> 4;
	uint16_t x;
	for (x = 0; x < VIEW_BLOCKS_X; x++) draw_block_column(base + x);
}

/* The shared object registration table. This list is the ONE place a new
 * object type gets wired into the per-frame pipeline below it (candidate
 * collection, drop-priority arbitration, and the final link-chain assembly,
 * all inside the main loop) -- add one row here and nothing else in this
 * file needs to change: not the scratch[] region size, not a PoolBlock, not
 * a copy-into-list[] line. Sonic and the ring sparkles are NOT rows here,
 * on purpose: Sonic is reserved out of the shared 80-sprite budget
 * unconditionally rather than arbitrated at all (md_src/obj_pool.h's own
 * comment on obj_pool_arbitrate has the reasoning), and the sparkles are a
 * runtime particle pool with no ObjTypeDesc/scene table behind them at all
 * (rings.h's own comment on rings_emit_sparkles) -- neither fits this
 * table's one uniform shape, and forcing either in would not have reduced
 * real per-type wiring, only hidden it. Both stay hand-written, at their
 * own fixed call sites in the main loop below, exactly as before.
 *
 * Row order is DRAW order, and DRAW order is NOT the same thing as the drop
 * PRIORITY each row also carries (its 4th field, obj_pool.h's OBJ_PRI_*):
 * this list is expanded, in this exact written order, into the final
 * hardware sprite list's link chain, directly after Sonic's own pieces --
 * real MD/32X hardware resolves sprite-vs-sprite overlap by table order
 * alone, not by the priority bit (sparkles are emitted ahead of Sonic
 * specifically so they land above him, matching the decomp's drawGroup 8 >
 * FG High 6 > player 4 -- see rings_emit_sparkles' own doc comment), so this
 * row order has to stay this list's own explicit, hand-picked choice,
 * never implicit or alphabetical. Drop priority is a completely separate
 * ranking -- which candidates get truncated first when a frame would
 * exceed the hardware's 80-sprite table -- and has no bearing on where a
 * surviving type's sprites land in the list.
 *
 * Each row: X(draw, tick, capacity, priority).
 *   draw     ObjDrawFn (obj_pool.h) -- this type's per-frame draw call.
 *   tick     ObjTickFn (obj_pool.h), or 0 for a type with no per-frame
 *            pre-step of its own (see that typedef's own comment for why
 *            rings has none).
 *   capacity this type's own natural per-frame cap (what used to size a
 *            fixed scratch[] region and one PoolBlock by hand, e.g.
 *            RING_SPRITE_CAP).
 *   priority this type's drop-order rank (obj_pool.h's OBJ_PRI_*). */
#define OBJ_TYPE_LIST(X) \
	X(rings_update,  0,             RING_SPRITE_CAP,     OBJ_PRI_RING) \
	X(springs_draw,  springs_tick,  SPRING_SPRITE_CAP,   OBJ_PRI_SPRING) \
	X(signpost_draw, signpost_tick, SIGNPOST_SPRITE_CAP, OBJ_PRI_SIGNPOST) \
	X(rings_lost_draw, rings_lost_tick, LOST_RING_CAP,   OBJ_PRI_RING) \
	/* == BATCH ANCHOR: BADNIKS == append badnik rows directly below == */ \
	X(motobug_draw,    motobug_tick,    MOTOBUG_SPRITE_CAP,    OBJ_PRI_BADNIK) \
	X(crabmeat_draw,   crabmeat_tick,   CRABMEAT_SPRITE_CAP,   OBJ_PRI_BADNIK) \
	X(buzzbomber_draw, buzzbomber_tick, BUZZBOMBER_SPRITE_CAP, OBJ_PRI_BADNIK) \
	X(chopper_draw,    chopper_tick,    CHOPPER_SPRITE_CAP,    OBJ_PRI_BADNIK) \
	X(newtron_draw,    newtron_tick,    NEWTRON_SPRITE_CAP,    OBJ_PRI_BADNIK) \
	X(batbrain_draw,   batbrain_tick,   BATBRAIN_SPRITE_CAP,   OBJ_PRI_BADNIK) \
	/* == BATCH ANCHOR: HAZARDS == append hazard/item rows directly below == */ \
	X(spikes_draw,       0,                   SPIKES_SPRITE_CAP,       OBJ_PRI_HAZARD) \
	X(spikelog_draw,     0,                   SPIKELOG_SPRITE_CAP,     OBJ_PRI_HAZARD) \
	X(breakablewall_draw, breakablewall_tick, 0,                       OBJ_PRI_HAZARD) \
	X(itembox_draw,      itembox_tick,        ITEMBOX_SPRITE_CAP,      OBJ_PRI_RING) \
	/* == BATCH ANCHOR: TRAVERSAL == append platform/bridge rows below == */ \
	X(platform_draw,          platform_tick,          PLATFORM_SPRITE_CAP,          OBJ_PRI_PLATFORM) \
	X(bridge_draw,            bridge_tick,            BRIDGE_SPRITE_CAP,            OBJ_PRI_PLATFORM) \
	X(collapsingplatform_draw, 0,                      1,                            OBJ_PRI_PLATFORM) \
	/* == BATCH ANCHOR: SCENERY == append decorative/logic rows below == */ \
	X(decoration_draw,  0,             DECORATION_SPRITE_CAP, OBJ_PRI_SCENERY)

#define OBJ_ROW(DRAW, TICK, CAP, PRI) { DRAW, TICK, (CAP), (PRI) },
static const ObjRegistration objTable[] = { OBJ_TYPE_LIST(OBJ_ROW) };
#undef OBJ_ROW
#define OBJ_TABLE_COUNT ((uint16_t)(sizeof(objTable) / sizeof(objTable[0])))

/* Compile-time sum of every row's own capacity -- the same figure this file
 * used to hand-sum into scratch[]'s array size (SPARKLE_POOL_SIZE +
 * SONIC_MAX_PIECES + RING_SPRITE_CAP + SPRING_SPRITE_CAP +
 * SIGNPOST_SPRITE_CAP below); expands to that identical sum, computed from
 * OBJ_TYPE_LIST so a new row grows it automatically instead of needing its
 * own edit here. */
#define OBJ_ADD_CAP(DRAW, TICK, CAP, PRI) + (CAP)
#define OBJ_TABLE_TOTAL_CAP (0 OBJ_TYPE_LIST(OBJ_ADD_CAP))

/* ===========================================================================
 * TEMPORARY MEASUREMENT KNOB -- tile-upload-per-frame budget probe.
 * NOT part of the game. OFF by default. Do not ship a build with this on.
 *
 * STALE SINCE THE 2026-08-18 VRAM RECLAIM: the "dead" range this knob writes
 * into below (TILE_FONTINDEX .. VDP_PLAN_A>>5, 224 tiles) is dead no longer
 * -- it is now live object-arena space plus the relocated spring/signpost
 * stream window (see obj_arena_init()'s own call site above main() for the
 * new arithmetic). Turning this knob on in a build that also has the
 * enlarged arena would scribble over whichever badnik/spikelog/itembox tiles
 * happen to be resident there, not into dead space -- never enable both at
 * once.
 *
 * Answers one question: how many 8x8 tiles can the 68000 push into VRAM per
 * frame, through the exact same CPU-write vdp_tiles_load() path every
 * object type already uses (Sonic's 33-tile window, rings, springs,
 * signpost), before the frame budget breaks -- and whether the upload has
 * to happen in vblank. This is what decides whether the remaining badniks
 * can each hold only their current animation frame resident and
 * re-upload it as they animate (docs task brief: ~13 tiles/badnik frame).
 *
 * Destination: TILE_FONTINDEX upward, in the font's own VRAM tile-pattern
 * range and then into Plane W's tilemap range right after it. Both are
 * dead in this build: the font IS uploaded at boot (vdp_font_load) but
 * DEBUG_OVERLAY is 0 above, so vdp_puts() never runs and no plane cell
 * ever names a font tile index; Plane W itself is never enabled (its
 * window registers are left at 0, so it covers zero rows/columns). That
 * gives (VDP_PLAN_A>>5) - TILE_FONTINDEX = 1536 - 1312 = 224 tiles of
 * headroom before the first byte that IS on screen (Plane A's own
 * tilemap). DTB_MAX_N stays under that with margin. The tile PIXEL DATA
 * written does not matter -- borrowed from ghz_tiles_banked (1036 tiles,
 * comfortably longer than DTB_MAX_N) -- only the write COST does, and that
 * cost is real: same vdp_tiles_load(), same VDP data port, same FIFO.
 *
 * What it does: once enabled, ramps N (tiles uploaded that frame) up in
 * DTB_STEP-tile steps, holding each step for DTB_FRAMES_STEP frames, and
 * cycles through three phases so one boot run answers every question in
 * the task:
 *   phase 0 (t=0s..54s):    N tiles, ONE vdp_tiles_load() call, in vblank,
 *                            right after sonic_upload() -- exactly where a
 *                            real per-class window would sit.
 *   phase 1 (t=54s..108s):  same N, same vblank slot, but split into
 *                            ceil(N/DTB_SPLIT_CHUNK) separate calls of
 *                            DTB_SPLIT_CHUNK tiles each (13, this task's
 *                            own badnik-frame estimate) -- simulates
 *                            several small per-class windows instead of
 *                            one big one, landing at the same total N.
 *                            Compare its breaking point against phase 0's:
 *                            if they match, the cost is linear in tiles and
 *                            per-call overhead does not matter; if phase 1
 *                            breaks at a lower N, many small windows cost
 *                            more than one big one for the same tile total.
 *   phase 2 (t=108s..162s): same N, ONE call, moved to BEFORE
 *                            vdp_wait_vblank() -- i.e. issued during ACTIVE
 *                            DISPLAY instead of vblank. Compare its
 *                            breaking point against phase 0's to see
 *                            whether vblank-only upload is a hard
 *                            requirement or just the safer default.
 * Then it wraps back to phase 0 and repeats, so it can run unattended.
 *
 * N(t) within any phase: N = min(DTB_MAX_N, DTB_STEP * floor((t mod 54s) /
 * 2s)), t = seconds since this ROM's gameplay started (the loop below,
 * not power-on). Time it with a stopwatch against the real console and
 * this schedule tells you N with no extra tooling.
 *
 * How to read it in ares: attach lldb to the running ares process and read
 * `program.vblanksPerSecond` (desktop-ui/program/program.hpp) once a
 * second -- see docs/hardware-budget.md section 7 for ares's own timing
 * caveats before trusting that number.
 *
 * ALREADY TRIED, FOR THE RECORD (2026-08-17, this build, this Mac): VPS held
 * rock steady at 59-61 through every phase, every N up to DTB_MAX_N, AND
 * with DTB_REPEAT temporarily raised to 8 and then 64 (that many extra
 * passes over the same N-tile range, so up to ~13,300 tile-uploads'
 * worth of VDP writes in a single frame) -- no dip, ever. Conclusion: on
 * this host, ares runs this ROM with so much real-time headroom that even
 * a wildly unrealistic write volume never costs a frame; VPS is not a
 * usable instrument for this question on this instrument. Do not expect
 * ares to show a cliff at any N worth shipping -- go straight to hardware.
 *
 * On real hardware there is no VPS counter and none is added here: watch
 * the running game for stutter (a hard dropped frame) versus tearing/
 * glitching in the stage, Sonic, or the HUD sprites (a soft one-frame
 * corruption) as N ramps up -- this knob never touches on-screen VRAM
 * itself (see "Destination" above), so any visible glitch comes from it
 * competing for the SAME frame's budget with the writes that ARE on
 * screen: sonic_upload/signpost_upload/springs_upload/obj_arena_upload/
 * vdp_sprites_write/hscroll/vscroll in phases 0-1, plus draw_block_row/
 * column's own active-display map writes and everything upstream of
 * vdp_wait_vblank() in phase 2.
 * ========================================================================= */
#define DEBUG_TILE_BENCH 0   /* 1 = enable the sweep above. Leave 0 to ship. */

#if DEBUG_TILE_BENCH
#define DTB_BASE         TILE_FONTINDEX  /* scratch only -- never displayed, see above */
#define DTB_MAX_N        208             /* < 224 tiles of headroom before Plane A's tilemap */
#define DTB_STEP         8               /* tiles added per ramp step */
#define DTB_FRAMES_STEP  120             /* ~2s/step at a nominal, un-degraded 60fps */
#define DTB_SPLIT_CHUNK  13              /* ~ a badnik frame's tile count (task brief) */
#define DTB_STEPS        (DTB_MAX_N / DTB_STEP + 1)
#define DTB_PHASE_FRAMES ((uint32_t)DTB_FRAMES_STEP * DTB_STEPS)
/* Repeats the whole DTB_MAX_N-tile upload this many times per frame, same
 * destination overwritten each pass -- a way to push the total per-frame
 * write volume past what the 224-tile safe scratch window can hold at N=1,
 * without touching a single byte of on-screen VRAM. Real per-class badnik
 * windows never need this (DTB_MAX_N alone already covers the task's whole
 * plausible range); it exists to find out whether ares shows a cost cliff
 * ANYWHERE before giving up on ares entirely for this measurement. 1 =
 * off/realistic. Leave at 1 for a normal run of this knob. */
#define DTB_REPEAT       1

static uint32_t dtbFrame = 0;
static uint16_t dtbPhase = 0;
static uint16_t dtbN = 0;

static void debug_tile_bench_upload(uint16_t base, uint16_t n, uint8_t split) {
	uint8_t rep;
	for (rep = 0; rep < DTB_REPEAT; rep++) {
		uint16_t b = base, remaining = n;
		if (!split) { vdp_tiles_load(ghz_tiles_banked, b, remaining); continue; }
		while (remaining) {
			uint16_t c = remaining < DTB_SPLIT_CHUNK ? remaining : DTB_SPLIT_CHUNK;
			vdp_tiles_load(ghz_tiles_banked, b, c);
			b = (uint16_t)(b + c);
			remaining = (uint16_t)(remaining - c);
		}
	}
}

/* Pre-vblank call site (active display, phase 2's only). Advances the
 * ramp/phase state, so this one runs FIRST each loop iteration -- the
 * in-vblank call site below just reads what this one just set. */
static void debug_tile_bench_active(void) {
	dtbFrame++;
	dtbPhase = (uint16_t)((dtbFrame / DTB_PHASE_FRAMES) % 3);
	{
		uint16_t step = (uint16_t)((dtbFrame % DTB_PHASE_FRAMES) / DTB_FRAMES_STEP);
		dtbN = (uint16_t)(step * DTB_STEP);
		if (dtbN > DTB_MAX_N) dtbN = DTB_MAX_N;
	}
	if (dtbPhase != 2) return;
	debug_tile_bench_upload(DTB_BASE, dtbN, 0);
}

/* In-vblank call site (phases 0 and 1). */
static void debug_tile_bench_vblank(void) {
	if (dtbPhase == 0) debug_tile_bench_upload(DTB_BASE, dtbN, 0);
	else if (dtbPhase == 1) debug_tile_bench_upload(DTB_BASE, dtbN, 1);
}
#endif /* DEBUG_TILE_BENCH */

int main(void)
{
	uint16_t frame = 0;
	int16_t worldX = 0, worldY = 0;
	uint16_t frameIndex = 0;
	uint8_t facing = 0;
	uint8_t drawGroupHigh = 0;
	uint8_t dispRot = 0;
	int cdPresent, cdState = 0;

	/* Bank 1 holds every asset in the game now (tools/gen_assets.py's
	 * manifest), so this is the ONE bank_select() call in the whole
	 * codebase: select it before touching anything else, and never switch
	 * away again -- see md_src/bank.h's own comment for why that structurally
	 * rules out every bank-switch hazard it used to have to warn callers
	 * about. Ahead of even audio_silence(): nothing this early needs a
	 * cartridge read, but there is no reason to leave the resting state as
	 * bank 0 for even one instruction once bank 0 has nothing left to offer
	 * this CPU. */
	bank_select(1);

	/* First thing after that, before anything else might rely on the audio
	 * hardware already being quiet (see docs/hardware-budget.md, section
	 * 6, "Silence the audio hardware at boot"). */
	audio_silence();

	vdp_init();
	pad_init();
	/* 68000 interrupts stay off: on 32X the vector table belongs to
	 * sh_src/mars_start.s, not this program, and Technical Bulletin 9
	 * forbids taking an interrupt while RV is set (see
	 * docs/hardware-budget.md, section 3). vdp_wait_vblank below already
	 * polls the VDP status the same way an interrupt handler would have. */

	/* three palettes for the stage, the fourth is Sonic's */
	vdp_colors(0, ghz_pal, 48);
	vdp_colors(48, sonic_pal, 16);
	/* Not DMA -- see bank.h's VDP DMA hazard comment; every tile upload in
	 * this codebase is a CPU-write through vdp_tiles_load. */
	vdp_tiles_load(ghz_tiles_banked, TILE_BASE, GHZ_TILE_COUNT);

	/* The shared VRAM tile residency arena (md_src/obj_generic.h): spans
	 * every tile between Sonic's own per-frame window and the spring/
	 * signpost shared STREAM window (signpost.c's own SIGNPOST_STREAM_BASE,
	 * computed from the same VDP_PLAN_A/SIGNPOST_MAX_STREAM_TILES constants
	 * used here, so the two can never disagree about where the boundary
	 * sits). rings_init()/springs_init()/signpost_init() below each register
	 * with it and synchronously upload their own resident set, in that
	 * order -- first-fit against an initially-empty arena, so every tenant
	 * lands immediately after the previous one with nothing wasted in
	 * between.
	 *
	 * 2026-08-18 VRAM reclaim: the arena used to stop at
	 * TILE_FONTINDEX - SIGNPOST_MAX_STREAM_TILES (1288), leaving 224 tiles
	 * (TILE_FONTINDEX=1312 .. VDP_PLAN_A>>5=1536) permanently dead on the far
	 * side of the spring/signpost stream window -- the font's own 96-tile
	 * pattern range is loaded but never displayed (DEBUG_OVERLAY is 0) and
	 * Plane W's own 128-tile nametable range right after it is never read
	 * either (Plane W's window registers are left at 0, covering zero rows/
	 * columns) -- verified directly from vdp.c/vdp_init() and this file's
	 * own DEBUG_OVERLAY gate, not assumed. signpost.c's SIGNPOST_STREAM_BASE
	 * now sits at the very top of the WHOLE usable range instead (right
	 * below VDP_PLAN_A's own tilemap, VDP_PLAN_A>>5), so the arena below
	 * reaches all the way up to it in one contiguous block: 1296 total tiles
	 * - 1036 stage - 33 Sonic - 24 shared stream window = 427 tiles of
	 * arena, not 203.
	 *
	 * Addresses shifted once already before this reclaim: rings_init() now
	 * spends 100 of the arena's own tiles (8 for its rotation window + 92
	 * resident for its sparkle portion -- rings.h's own doc comment), not
	 * 156, so springs/signpost each land 56 tiles earlier than a
	 * from-scratch reader of an old build log would expect. See this task's
	 * own final report for the post-reclaim per-class residency table. */
	{
		uint16_t arenaBase = sonic_gfx_init(TILE_BASE + GHZ_TILE_COUNT);
		uint16_t arenaEnd = (uint16_t)((VDP_PLAN_A >> 5) - SIGNPOST_MAX_STREAM_TILES);
		obj_arena_init(arenaBase, (uint16_t)(arenaEnd - arenaBase));
	}
	rings_init();
	springs_init();
	signpost_init();
	decoration_init();
	spikes_init();
	spikelog_init();
	breakablewall_init();
	itembox_init();
	platform_init();
	bridge_init();
	/* Springs' bounce pose and the signpost's face plate share ONE streamed
	 * VRAM window (springs.h/signpost.h's own doc comments) at the fixed
	 * address signpost.c computes independently of the arena above --
	 * springs.c still needs to be told it explicitly, the two never DMA
	 * into it at the same moment (no GHZ1 spring sits anywhere near where
	 * the signpost lives), so sharing one physical region is safe. */
	springs_set_stream_base(signpost_stream_tile_base());

	/* CD bring-up is entirely 68000-local (only this CPU can reach the CD
	 * hardware) and every wait inside it is bounded, so it can run here
	 * without disturbing the SH2 handshake below. On the actual test
	 * hardware, a bare 32X with no CD unit, this returns promptly. */
	cdPresent = cd_init();
	if (cdPresent) cdState = cd_music_play(CD_MUSIC_TRACK) ? 2 : 1;
#if !DEBUG_OVERLAY
	(void)frame; (void)cdState;   /* only the debug overlay reads these */
#endif

	/* Player/Camera now live only on the slave SH2, so the descriptor table
	 * and screenCenterY (SCREEN_HALF_H is only valid after vdp_init()) are
	 * published before anything else that needs them. */
	comm_boot_publish((uint32_t)&asset_descriptor - 0x880000u,
	                   (uint16_t)(SCREEN_HALF_H - 16));

	/* One-time startup block: this 68000 can no longer compute its own
	 * initial camera position now that Player/Camera live only on the
	 * slave, so it waits here for the slave's first published frame before
	 * it has anything to draw. Every later call to comm_read_frame is
	 * non-blocking; this loop is not part of that steady-state protocol. */
	while (!comm_read_frame(&camX, &camY, &worldX, &worldY, &frameIndex, &facing,
	                        &drawGroupHigh, &dispRot)) {}

	firstCol = camX >> 4;
	firstRow = camY >> 4;

	vdp_map_clear(VDP_PLAN_A);
	vdp_map_clear(VDP_PLAN_B);

	draw_screen();

	for (;;) {
		uint16_t pad = pad_read();
		uint16_t wantCol, wantRow;
		uint16_t sparkleCount, sonicCount;
		uint16_t total, i;
		/* blocks[0] is the hand-written sparkle candidate (see below);
		 * blocks[1..] are OBJ_TYPE_LIST's own rows, table order, one per
		 * registered type -- Sonic is never one of these, see obj_pool.h's
		 * own comment on obj_pool_arbitrate for why his slice of the budget
		 * is reserved unconditionally instead of arbitrated at all. */
		PoolBlock blocks[1 + OBJ_TABLE_COUNT];
		/* Where each objTable[] row's candidates start in scratch[] below
		 * -- filled in during the candidate phase, read back during final
		 * assembly (a truncated row's own surviving prefix still starts
		 * here regardless of how much of its tail arbitration dropped). */
		uint16_t objOffset[OBJ_TABLE_COUNT];
		uint16_t budget;

		/* The shared hardware-sprite pool (md_src/obj_pool.h): every
		 * emitter first writes its OWN candidates -- everything it would
		 * draw this frame with no 80-sprite ceiling at all -- into this
		 * scratch buffer, at its own fixed region (sized off each type's
		 * natural per-frame cap, the same constants that used to be a
		 * PERMANENT slice of the hardware table and now just bound how
		 * much scratch RAM one type's candidates can ever need). Static,
		 * not stack, like the list it replaces; RAM headroom here is not
		 * the scarce resource the 80-entry hardware table is.
		 *
		 * OBJ_TABLE_TOTAL_CAP (defined above main(), from OBJ_TYPE_LIST)
		 * replaces the old hand-summed "+ RING_SPRITE_CAP + SPRING_
		 * SPRITE_CAP + SIGNPOST_SPRITE_CAP": adding a table row grows this
		 * automatically. */
		static VDPSprite scratch[SPARKLE_POOL_SIZE + SONIC_MAX_PIECES + OBJ_TABLE_TOTAL_CAP];
		/* The real hardware sprite table this frame actually uploads --
		 * exactly HW_SPRITE_CAP (80) entries, never more, regardless of how
		 * many object types this game grows to. */
		static VDPSprite list[HW_SPRITE_CAP];
		/* Sparkles' and Sonic's own fixed regions within scratch[] above --
		 * an enum, not #define, so it stays scoped to this loop body. Every
		 * OTHER type's region is computed at runtime into objOffset[]
		 * below, right after OFF_SONIC's own -- a runtime prefix sum over
		 * OBJ_TABLE_COUNT rows costs nothing worth avoiding here, and is
		 * what lets a new table row need no enum entry of its own. */
		enum {
			OFF_SPARKLE = 0,
			OFF_SONIC   = OFF_SPARKLE + SPARKLE_POOL_SIZE
		};

		/* Right after pad_read(), before anything else, matching where the
		 * single-CPU original called player_update: this is what keeps the
		 * phase relationship between "a vblank happened" and "input for
		 * that vblank is available" unchanged now that the two live on
		 * different CPUs (see sh_src/comm.h). */
		comm_send_input(pad);

		/* Non-blocking: on a torn or absent update this just re-delivers
		 * the previous frame's cached camera/Sonic values. */
		comm_read_frame(&camX, &camY, &worldX, &worldY, &frameIndex, &facing,
		                &drawGroupHigh, &dispRot);

		/* Shared VRAM tile residency arena (md_src/obj_generic.h): evicts
		 * whichever resident/loading class's window just left camX's
		 * range and admits whichever evicted class's window just entered
		 * it, flipping rings_enabled()-style live flags as it goes. Must
		 * run before every draw() call below (rings_update()/springs_draw()/
		 * signpost_draw() all read the live flag this can just have
		 * flipped) -- no VRAM write happens here, only bookkeeping; the
		 * actual amortized upload is obj_arena_upload() below, in vblank. */
		obj_arena_tick(camX);

		/* Per-object pre-step (obj_pool.h's ObjTickFn): every registered
		 * type that has one of its own -- springs' observational AABB
		 * bounce trigger, the signpost's fall/spin state machine -- runs
		 * here, in OBJ_TYPE_LIST's own row order, off Sonic's just-
		 * published position, before any type's draw() call below
		 * (signpost.h's own "tick decides, draw reads, upload DMAs in
		 * vblank" split -- springs.h follows the same three-phase shape
		 * sonic_upload()/sonic_build() already use for Sonic's own frame).
		 * A row with tick == 0 (rings -- see OBJ_TYPE_LIST's own comment
		 * for why it has none of its own) is simply skipped. */
		for (i = 0; i < OBJ_TABLE_COUNT; i++)
			if (objTable[i].tick) objTable[i].tick(worldX, worldY, frameIndex);

		/* Stream every row and column that just entered view. The plane is
		 * only VIEW_BLOCKS_X by VIEW_BLOCKS_Y and wraps in both axes, so each
		 * window has to move as a fixed-size range: expanding it on both
		 * sides leaves stale cells behind once the camera has travelled more
		 * than a plane width or height.
		 *
		 * Rows are streamed first, across the OLD column window, then
		 * columns are streamed across the NEW row window. A cell that is a
		 * new row in an already-visible column is caught by the row pass. A
		 * cell that is both a new row and a new column is missed by the row
		 * pass (its column isn't in the window yet) but caught by the column
		 * pass right after, since firstRow is fully updated by then. Doing
		 * the two passes in the other order leaves that corner cell stale. */
		wantCol = camX >> 4;
		wantRow = camY >> 4;

		while (firstRow < wantRow) {
			firstRow++;
			draw_block_row(firstRow + VIEW_BLOCKS_Y - 1);
		}
		while (firstRow > wantRow) {
			firstRow--;
			draw_block_row(firstRow);
		}

		while (firstCol < wantCol) {
			firstCol++;
			draw_block_column(firstCol + VIEW_BLOCKS_X - 1);
		}
		while (firstCol > wantCol) {
			firstCol--;
			draw_block_column(firstCol);
		}

		/* Candidate phase: every emitter writes into its own fixed scratch
		 * region, at its own natural per-frame cap, with no notion of the
		 * shared 80-sprite budget yet -- firstLink=0 in every call below is
		 * never read again (the final assembly pass rebuilds every link
		 * field from the entries that actually survive arbitration), so
		 * each emitter's own internal link chain is scratch, not final.
		 * Sonic is written here too (fixed at OFF_SONIC) but is NOT one of
		 * the arbitrated blocks below -- see obj_pool.h's own comment for
		 * why his slice of the budget is reserved unconditionally instead.
		 * Every OTHER type's own candidates are collected generically, off
		 * OBJ_TYPE_LIST's own row order (this file's own comment above),
		 * each into its own region of scratch[] starting right after
		 * Sonic's own -- objOffset[] remembers where, for the final
		 * assembly pass below. */
		blocks[0].count = sparkleCount = rings_emit_sparkles(scratch, OFF_SPARKLE, 0, camX, camY);
		blocks[0].priority = OBJ_PRI_SPARKLE;
		sonicCount = sonic_build(frameIndex, dispRot,
		                         worldX - (int16_t)camX,
		                         worldY - (int16_t)camY,
		                         facing, drawGroupHigh, &scratch[OFF_SONIC], 0);
		{
			uint16_t off = (uint16_t)(OFF_SONIC + SONIC_MAX_PIECES);
			for (i = 0; i < OBJ_TABLE_COUNT; i++) {
				objOffset[i] = off;
				blocks[1 + i].count = objTable[i].draw(scratch, off, 0, camX, camY,
				                                       worldX, worldY, frameIndex);
				blocks[1 + i].priority = objTable[i].priority;
				off = (uint16_t)(off + objTable[i].capacity);
			}
		}

		/* Arbitration phase: drop the least important candidates (lowest
		 * priority first, obj_pool.h's own OBJ_PRI_* constants) until
		 * everything else plus Sonic's reserved, unconditional slice fits
		 * in HW_SPRITE_CAP. Every compromise this represents -- which
		 * priority order, what "least important" even means here -- is
		 * this task's own report, not a silent pick baked into this loop. */
		budget = (uint16_t)(HW_SPRITE_CAP - (sonicCount < HW_SPRITE_CAP ? sonicCount : HW_SPRITE_CAP));
		obj_pool_arbitrate(blocks, (uint8_t)(1 + OBJ_TABLE_COUNT), budget);
		sparkleCount = blocks[0].count;

		/* Final assembly: copy each block's SURVIVING prefix (arbitration
		 * only ever truncates a block's tail, never reorders it) into the
		 * real hardware list, in OBJ_TYPE_LIST's own explicit draw order --
		 * sparkles above Sonic, Sonic above every registered type in table
		 * order (this file's own comment above on why that order is load-
		 * bearing for MD/32X's table-order sprite-vs-sprite overlap, not
		 * cosmetic) -- rebuilding every link field along the way, since a
		 * truncated block shifts every following entry's true position. */
		total = 0;
		/* Field-by-field, not a struct assignment: this codebase is
		 * -ffreestanding with no libc, and GCC lowers even an 8-byte
		 * VDPSprite copy to a memcpy() call here, which then fails to link
		 * (no memcpy anywhere in this project -- md_src/springs.c/
		 * signpost.c hit the same thing building their own local ObjPiece
		 * tables and are copied field-by-field for the same reason).
		 *
		 * BUG FOUND WHILE BUILDING THIS TASK'S OWN pipeline-level host
		 * verification, NOT introduced by this task -- present verbatim in
		 * the pre-refactor main.c too (every call site here already passed
		 * `total++` as dstIdx before this task touched this loop). Every
		 * call site below passes a side-effecting `total++` as dstIdx, and
		 * dstIdx used to appear four times in this macro's own body -- one
		 * per field assignment -- so plain C parameter substitution
		 * expanded that into FOUR separate `total++` evaluations per
		 * logical copy, not one: `total` advanced by 4 per sprite instead
		 * of 1, and a single sprite's four fields landed in four different,
		 * mostly-never-otherwise-written list[] slots instead of together
		 * in one. Confirmed with a minimal standalone repro of this exact
		 * macro+call-site pattern outside this codebase, not assumed.
		 * Fixed by capturing dstIdx into a local ONCE, so a side-effecting
		 * argument is safe regardless of how many fields the macro body
		 * touches. */
#define OBJ_POOL_COPY(dstIdx, src) \
		do { \
			uint16_t d_ = (dstIdx); \
			list[d_].y = (src).y; \
			list[d_].size = (src).size; \
			list[d_].attr = (src).attr; \
			list[d_].x = (src).x; \
		} while (0)
		for (i = 0; i < sparkleCount; i++) OBJ_POOL_COPY(total++, scratch[OFF_SPARKLE + i]);
		for (i = 0; i < sonicCount; i++)   OBJ_POOL_COPY(total++, scratch[OFF_SONIC + i]);
		{
			uint16_t t, j;
			for (t = 0; t < OBJ_TABLE_COUNT; t++) {
				uint16_t n = blocks[1 + t].count;
				for (j = 0; j < n; j++) OBJ_POOL_COPY(total++, scratch[objOffset[t] + j]);
			}
		}
#undef OBJ_POOL_COPY
		for (i = 0; i < total; i++) list[i].link = (uint16_t)(i + 1);
		list[total - 1].link = 0;

		/* Debug overlay, compile-time gated (user asked for a clean screen once
	 * rings proved out). Re-enable for bring-up work -- on real hardware
	 * this row is what separates a hang from an input that never arrives,
	 * and the ring counter is the only collect feedback until a real HUD
	 * exists. */
#if DEBUG_OVERLAY
	/* A frame counter next to the raw pad bits: on real hardware this is
		 * what separates a hang from an input that never arrives. Plane A
		 * scrolls under it, so it goes on whichever cell row the top of the
		 * screen currently lands on rather than on plane row 0. */
		{
			char buf[32];
			uint16_t songs;
			/* CD digit: 0 none found, 1 brought up, 2 music playing. The
			 * music has no other visible sign, so without it a silent
			 * failure and a working disc look identical. */
			{
				uint16_t stat = cd_status_word(&songs);
				/* CD digit: 0 none, 1 brought up, 2 music started.
				 * The status word next to it is the drive's own:
				 * 0100 is playing, 1000 no disc, 4000 tray open,
				 * and bit 15 is the BIOS still busy. Ring count last:
				 * "R OFF" if rings_init() refused to come up (tile
				 * budget or table-count check, rings.h) instead of a
				 * corrupted font or a mis-walked table being the first
				 * sign of that. */
				if (rings_enabled()) {
					sprintf(buf, "%04X %02X CD%d %04X R%03u", frame++, pad,
					        cdState, stat, rings_collected_count());
				} else {
					sprintf(buf, "%04X %02X CD%d %04X R OFF", frame++, pad,
					        cdState, stat);
				}
			}
			vdp_puts(VDP_PLAN_A, buf, 1, (camY >> 3) & (PLAN_HEIGHT - 1));
		}

#endif

#if DEBUG_TILE_BENCH
		/* Phase 2's call site -- see the knob's own comment above main(). */
		debug_tile_bench_active();
#endif

		/* vdp_wait_vblank, not vdp_vsync: the latter returns once vblank has
		 * ended, which would put the tile DMA in active display where the VDP
		 * accepts a trickle and stalls the 68000 for most of the frame. */
		vdp_wait_vblank();
		/* The CD BIOS needs a level 2 interrupt at about this rate to keep
		 * running, and a 32X ROM cannot give it one from an interrupt
		 * handler. Safe to call with no CD present: it returns immediately
		 * unless cd_init() brought one up. */
		cd_vblank();
		sonic_upload(frameIndex, dispRot, facing);
#if DEBUG_TILE_BENCH
		/* Phases 0 and 1's call site -- see the knob's own comment above
		 * main(). Right after Sonic's own window, matching where a real
		 * per-class badnik window would be uploaded. */
		debug_tile_bench_vblank();
#endif
		signpost_upload();
		springs_upload();
		/* obj_anim_window_upload() BEFORE obj_arena_upload(), always -- see
		 * obj_anim_window_upload()'s own doc comment for why this exact
		 * order is what keeps the two mechanisms' combined per-vblank tile
		 * budget from silently doubling on the one vblank a whole-class fill
		 * happens to finish (rings.c's own rotation window is this
		 * project's one live user of this call today). */
		obj_anim_window_upload();
		/* This frame's amortized ARENA_TILES_PER_FRAME-tile chunk for
		 * whichever class obj_arena_tick() above left mid-load -- returns
		 * immediately, touching no VRAM at all, whenever nothing is loading
		 * (the common case: rings/springs/signpost are already resident from
		 * boot). See obj_generic.h's own comment for the budget this is
		 * sized against. */
		obj_arena_upload();
		vdp_sprites_write(list, total);
		vdp_hscroll(VDP_PLAN_A, -(int16_t)camX);
		/* Plane B now carries FG High, streamed from the same firstCol/
		 * firstRow window as Plane A's FG Low (draw_block_column/row above),
		 * so it scrolls in lockstep with Plane A rather than the half-rate
		 * parallax this used to be before the 32X framebuffer took over the
		 * background layer (docs/hardware-budget.md, section 3, "Layer
		 * order: the MD draws in front"). */
		vdp_hscroll(VDP_PLAN_B, -(int16_t)camX);
		vdp_vscroll(VDP_PLAN_A, (int16_t)camY);
		vdp_vscroll(VDP_PLAN_B, (int16_t)camY);
	}

	return 0;
}
