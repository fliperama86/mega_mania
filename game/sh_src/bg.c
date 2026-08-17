/* Green Hill's "BG Outside" parallax layer, on the 32X framebuffer.
 *
 * Technique: line-table scrolling (docs/hardware-budget.md section 4), the
 * same mechanism blitbench/sh_src/m_main.c proves working with its vf_*
 * functions. The framebuffer's first 256 words are a table of per-scanline
 * word offsets; painting a playfield once and rewriting only that table
 * turns scrolling into 224 word writes a frame. This file follows that,
 * rather than inventing a second way to drive the framebuffer.
 *
 * What differs from blitbench's vf playfield: we cannot store the whole
 * 8192x384 px background (tools/convert_bg.py's bg_map is that big), so
 * each screen line keeps only a 576 px strip -- 256 px of slack beyond the
 * 320 visible -- centred on wherever that line is currently reading from,
 * and the strip is periodically recentred ("rebased") as the camera moves
 * it toward the edge.
 *
 * Row mapping: which background row a screen line shows is NOT fixed --
 * see layer_first_row() -- it is RSDKv5's own layer-level Y scroll, on top
 * of the per-row X parallax from bg_lines.bin (parallaxQ8[]/speedQ8[]
 * below, which only ever moved X). A screen line's row only changes slowly
 * (one row per 32 px of camera Y travel), but WHEN it changes, that line's
 * stored strip is for the wrong row entirely, not just mis-centred, so it
 * is handled as its own kind of "dirty" -- see point 2 below.
 *
 * Three things keep the rebase budget honest, replacing an earlier version
 * that rebased 56 lines every frame unconditionally, round robin, whether
 * or not anything needed it:
 *
 * 1. Uniform rows never rebase, ever. classify_rows() walks the block map
 *    once at init, for the layer's full LAYER_H_PX rows (not just the 224
 *    a screen can show at once, since which rows those are now moves): a
 *    row whose whole block row is one block index, and that block is one
 *    flat colour, is routed to a small shared strip (one per distinct
 *    colour actually seen, see MAX_FLAT_COLOURS) instead of a dedicated
 *    one, and never needs a new offset -- any read from a one-colour strip
 *    shows that colour. See rowFlat[]/rowFlatSlot[] (the static, per-row
 *    classification) and lineFlat[]/lineFlatSlot[] (which lines currently
 *    use it, which changes as rows scroll through). With the currently
 *    converted GHZ background this is background rows 272-383 (block rows
 *    17-23 of the raw 24-block-row map) -- out of reach with a fixed
 *    row-per-line mapping (see git history), but genuinely reachable now
 *    that the mapping moves, whenever the player is deep enough that some
 *    screen line's row falls in that range.
 *
 * 2. Row changes are dirty lines, not a full redraw. Every screen line's
 *    row can change on the same frame (a single layer_first_row() step
 *    shifts EVERY line's target row together), so this cannot be "redraw
 *    whichever lines changed" without reintroducing the unconditional-cost
 *    problem point 3 already solved for horizontal drift. Instead a row
 *    mismatch just becomes another candidate for the SAME demand-driven
 *    selection horizontal drift already used, ranked above every
 *    horizontal-only candidate (a wrong row is a worse defect than a
 *    correct row slightly off-centre) and fairly ordered among themselves
 *    by framesDirty[] (longest-waiting line first, so a sustained fall
 *    doesn't starve whichever lines happen to sort last). See bg_frame().
 *
 * 3. Everything else rebases on demand. bg_frame() scores every non-flat,
 *    on-row line by how much of its slack is spent and rebases the
 *    REBASE_CAP most urgent lines (row-dirty or over TRIGGER), not a fixed
 *    round-robin group -- a stationary camera scores nothing above TRIGGER
 *    except the 64 lines bg_lines.bin gives their own nonzero scroll speed
 *    (the cloud bands, rows 0-63: keeping their drift alive with the
 *    camera parked is a requirement, not a bug). Measured against the
 *    shipped asset this settles to ~2-3 strip draws/frame on average once
 *    the camera stops, against a constant 56 every frame, moving or not,
 *    before this change. Worst case (fast camera on both axes at once) is
 *    a flat 80/frame, capped and stable -- see the constants below for the
 *    worst-case math, and layer_first_row()'s comment for the vertical
 *    case specifically, which looks like it should be able to fall behind
 *    without bound and, simulated, does not. */

#include "mars.h"
#include "assets.h"
#include "bg.h"

/* Data starts after the 256-word line table, the same FB_DATA_WORD
 * convention the placeholder gradient used. Layout from there: one
 * dedicated STRIP_W-byte slot per screen line (always all SCREEN_HEIGHT of
 * them -- unlike a fixed row-per-line mapping, any line can need a real
 * draw on any frame, so none of these can be given away), then one small
 * FLAT_STRIP_W-byte shared slot per distinct flat colour classify_rows()
 * finds (at most MAX_FLAT_COLOURS of those). Worst case, every slot used:
 * 224 * 576 = 129,024 bytes, plus the 512-byte table, plus
 * MAX_FLAT_COLOURS * 320 bytes, is <= 130,176 of the 131,072-byte bank
 * with MAX_FLAT_COLOURS = 2 -- 896 to spare. That is a hard ceiling, not a
 * today's-data estimate: every dedicated slot is reserved regardless of
 * what classify_rows() finds, so this bound holds for any art. */
#define BG_DATA_WORD  0x100
#define STRIP_W       576
#define STRIP_WORDS   (STRIP_W / 2)
#define SLACK         (STRIP_W - SCREEN_WIDTH)   /* 256 px */
#define MARGIN        (SLACK / 2)                /* 128 px kept each side on rebase */

/* A flat row's shared strip needs no slack: its offset never changes, so
 * it only ever has to cover the SCREEN_WIDTH pixels a scanline actually
 * reads. Capped at a couple of distinct colours -- the shipped art only
 * ever has one (see the file header) -- and a soft cap besides: a colour
 * that doesn't fit just falls back to an ordinary dedicated slot
 * (classify_rows()), so this number can't turn into a correctness bug,
 * only a missed optimisation. */
#define FLAT_STRIP_W      SCREEN_WIDTH
#define FLAT_STRIP_WORDS  (FLAT_STRIP_W / 2)
#define MAX_FLAT_COLOURS  2

/* tools/convert_bg.py's GHZ/"BG Outside" output. Fixed here, not published
 * through the descriptor like the foreground map's GHZ_MAP_W/H: nothing on
 * the 68000 side indexes this data at compile time, so there is only one
 * place it can go stale, and this is it. Must match convert_bg.py's own
 * reported "map W x H". LAYER_H_PX is the layer's own pixel height (also
 * convert_bg.py's report): the layer tiles vertically at this, which
 * layer_first_row() below wraps into -- a different number from
 * SCREEN_HEIGHT that this file is otherwise careful never to mix with it. */
#define MAP_BLOCKS_W  512
#define MAP_W_PX      (MAP_BLOCKS_W * 16)   /* 8192, power of two: wraps with a mask */
#define BLOCK_BYTES   256
#define LAYER_H_PX    384

/* RSDKv5's layer-level Y scroll (distinct from the per-row X parallax in
 * bg_lines.bin above, which only ever moves X -- see line_offset()).
 * Scene.cpp's LAYER_HSCROLL case (dependencies/RSDKv5/RSDKv5/RSDK/Scene/
 * Scene.cpp in the decompilation) computes a layer's first visible row as
 *     FROM_FIXED((layer->scrollPos + (layer->parallaxFactor * cameraY << 8))
 *                & 0xFFFF0000) % pixelHeight,
 * wrapped positive if negative (16.16 fixed point throughout), then walks
 * one row per scanline, wrapping at pixelHeight. GHZSetup.c's Act 1 branch
 * (SonicMania/Objects/GHZ/GHZSetup.c in the decompilation, the GHZSetup_
 * StageLoad "!Zone->actID" branch) sets BG Outside's scrollPos to 0x180000
 * (24 px in 16.16) and negates the layer's own +8 parallaxFactor to -8 for
 * this act. parallaxFactor is on the same Q8 convention as this file's own
 * per-row parallaxQ8[] (Scene.cpp scales it by /256, same as line_offset()
 * scales parallaxQ8[]), so the whole expression reduces to
 *     firstRow = 24 - cameraY / 32
 * wrapped into 0..LAYER_H_PX-1: the background rises one pixel for every
 * 32 the camera descends. */
#define BG_LAYER_SCROLL_POS   24   /* px; GHZSetup.c: scrollPos = 0x180000 (16.16) */
#define BG_LAYER_PARALLAX_Q8  8    /* GHZSetup.c negates the layer's own +8 to -8 for Act 1 */

/* Demand-driven rebase. A line becomes a horizontal candidate once it has
 * spent more than TRIGGER of its SLACK; a row-mismatched line is always a
 * candidate (see point 2 in the file header). Each frame the REBASE_CAP
 * most urgent candidates get rebased (see bg_frame()), row mismatches
 * always outranking horizontal ones (ROW_DIRTY_BASE below). TRIGGER/
 * REBASE_CAP come from the same worst case the old round-robin scheme
 * sized itself against: ~15 px/frame (16 px/frame camera motion --
 * CENTER_BOUNDS_X, camera.c -- parallax factors all below 1.0, plus up to
 * 1 px/frame of the top rows' own drift).
 *
 * TRIGGER must sit far enough above MARGIN that a line just rebased this
 * frame cannot cross it again by the very next frame (the one-frame window
 * before its catch-up draw finishes, see the pendingLines comment in
 * bg_frame()): a fresh rebase leaves delta at MARGIN plus at most 3 px of
 * rounding slop from the base's 4-px alignment, and one more frame adds at
 * most 15 px, so MARGIN + 3 + 15 = 146 is the ceiling one frame out.
 * TRIGGER = 160 clears that with room to spare.
 *
 * REBASE_CAP must be able to drain every line before it reaches the hard
 * SLACK ceiling. Worst case, every line becomes a candidate at once (e.g.
 * right after bg_init(), which starts them all at the same relative
 * offset): SLACK - TRIGGER = 96 px of headroom left once a line trips
 * TRIGGER, which is 96 / 15 ~= 6.4 frames of grace. Draining 224 lines
 * inside 6 of those frames needs at least 224 / 6 ~= 37.4 lines/frame;
 * REBASE_CAP = 40 clears that too.
 *
 * This bound is specific to HORIZONTAL drift, which is the only kind the
 * old (X-only) version had. Vertical row changes look worse at first: 24 <
 * 32 means layer_first_row() can only ever step by one row a frame
 * (camera.c caps vertical camera speed at CENTER_BOUNDS_Y = 24 px/frame),
 * but that one-row step re-targets EVERY one of the 224 lines
 * simultaneously (file header point 2), and draining 224 lines at
 * REBASE_CAP takes 5.6 frames -- longer than the ~1.3 frames (32 / 24)
 * between steps at a sustained terminal fall, so a naive per-frame bound
 * suggests the backlog should grow without limit. It does not: framesDirty[]
 * ranking is fair (oldest-waiting-first), and a line only ever needs ONE
 * draw to fully catch up no matter how many rows it fell behind while
 * waiting (bg_frame() always targets the CURRENT wantedRow, not a queue of
 * intermediate ones), so the backlog settles into a stable rotation rather
 * than accumulating. Simulated against this file's actual constants and
 * the shipped bg_lines.bin/bg_map.bin, sustained worst-case camera motion
 * on both axes at once (16 px/frame X, 24 px/frame Y, forever) never
 * pushed any line more than 5 frames behind (worst case observed: every
 * line at most 5 frames stale, i.e. lagging the correct row by at most a
 * handful of rows at any instant, self-correcting continuously) and never
 * came close to the SLACK clamp (worst delta observed 217 of 256). See
 * layer_first_row()'s comment for what that costs and what it looks like
 * on screen. */
#define TRIGGER         160
#define REBASE_CAP      40
#define ROW_DIRTY_BASE  (SLACK + 1)   /* always outranks any horizontal delta (clamped to SLACK) */

/* bg_lines.bin's on-disk layout: struct.pack(">HH", parallax, speed) per
 * row, which lines up byte-for-byte with this (both CPUs in this build are
 * big endian: -mb for the SH2, natural for the 68000, so no swap needed). */
typedef struct { uint16_t parallax, speed; } BgLine;

static const uint16_t *g_bg_map;      /* MAP_BLOCKS_W x (LAYER_H_PX/16) blocks, big endian */
static const uint8_t  *g_bg_blocks;   /* BLOCK_BYTES per block, indexed by g_bg_map */
static const uint16_t *g_bg_pal;      /* 256 CRAM words, big endian */
static const BgLine   *g_bg_lines;    /* one parallax/speed pair per background row */

/* Per BACKGROUND ROW (LAYER_H_PX of them -- the whole layer, not just
 * whatever 224 a screen line can currently reach): bg_lines.bin's own
 * data, and the classification classify_rows() derives from the block
 * map. driftAccum lives here, not per screen line, so a row's cloud phase
 * keeps advancing at its own rate whether or not any line is currently
 * showing it, and is exactly where it should be whenever one starts. */
static uint16_t parallaxQ8[LAYER_H_PX];    /* Q8, 256 = 1.0 */
static uint16_t speedQ8[LAYER_H_PX];       /* Q8 px/frame */
static uint32_t driftAccum[LAYER_H_PX];    /* Q8.8 px, wraps mod map width */
static uint8_t  rowFlat[LAYER_H_PX];       /* 1 if this row is one block, one colour */
static uint8_t  rowFlatSlot[LAYER_H_PX];   /* which flatColourVal[], valid only if rowFlat[row] */

static uint8_t flatColourVal[MAX_FLAT_COLOURS];
static uint8_t flatColourCount;

/* Per SCREEN LINE: which row it currently shows and what it currently
 * costs to keep showing it. lineRow[l] is the row lineWordBase[l]'s slot
 * actually holds pixels for right now -- not necessarily the row l WANTS
 * this frame (layer_first_row() + l), which is wantedRow in bg_frame();
 * the two only disagree between a row becoming dirty and it being
 * serviced. framesDirty[l] counts consecutive frames l has wanted a
 * different row than it has and lost the REBASE_CAP priority contest --
 * see the file header point 2 -- and is what keeps a sustained backlog
 * fair rather than always favouring low line numbers. */
static uint16_t lineRow[SCREEN_HEIGHT];
static uint16_t bgBase[SCREEN_HEIGHT];       /* background-space X of lineRow[l]'s strip's left pixel */
static uint8_t  lineFlat[SCREEN_HEIGHT];
static uint8_t  lineFlatSlot[SCREEN_HEIGHT]; /* valid only if lineFlat[l] */
static uint16_t lineWordBase[SCREEN_HEIGHT]; /* the table word this line writes when not being rebased */
static uint8_t  framesDirty[SCREEN_HEIGHT];  /* saturates at 255; only used to rank priority */

/* Lines rebased THIS real frame still need their base redrawn into the
 * OTHER physical bank next real frame before both banks agree -- see the
 * pendingLines comment in bg_frame(). */
static uint8_t pendingLines[REBASE_CAP];
static uint8_t pendingCount;

/* Current background-space X for background row (not screen line -- see
 * the per-row arrays above) row: the drift accumulated so far, plus the
 * camera's contribution scaled by that row's parallax factor, both in the
 * same px domain, wrapped into the map's width. Shared by the per-frame
 * line-table pass and the rebase pass so the two always agree on what
 * "current" means for a given row. */
static uint16_t line_offset(int row, uint16_t camX)
{
	uint32_t v = (driftAccum[row] >> 8) + (((uint32_t)camX * parallaxQ8[row]) >> 8);
	return (uint16_t)(v & (MAP_W_PX - 1));
}

/* RSDKv5's layer-level Y scroll -- see the BG_LAYER_SCROLL_POS/
 * BG_LAYER_PARALLAX_Q8 comment above for the formula this reduces from.
 * camY * BG_LAYER_PARALLAX_Q8 is never negative (both operands are
 * non-negative), so this division truncates exactly like RSDK's own floor
 * -- truncation and floor only disagree on negative inputs, and there are
 * none until the subtraction below, which is not a division. The
 * following %/correction pair is the same "truncate then add the modulus
 * back if still negative" RSDK's own scrollPos code uses, which is exactly
 * floor-mod as long as the value being modded was already floored going
 * in, which the paragraph above establishes.
 *
 * What a fast fall costs: every call here can move the result by at most
 * one row (CENTER_BOUNDS_Y caps camY at 24 px/frame, less than the 32 px
 * a row costs), but that one row re-targets all 224 lines at once -- see
 * the REBASE_CAP comment above for why that looks, on paper, like it
 * should be able to outrun the cap. Simulated against this file's actual
 * constants under sustained worst-case camera motion, it does not: no
 * line ever fell more than 5 frames behind its wanted row before catching
 * up in a single draw (a line always targets whatever row is CURRENT when
 * it is finally serviced, never a stale one it queued earlier). On screen
 * that is a background whose vertical position can lag the camera by a
 * handful of rows -- a few pixels -- while falling at full speed, never
 * more, self-correcting continuously rather than in one visible jump, and
 * fully settled within about 6 frames of the fall ending. A line still
 * waiting for its turn keeps showing its previous (stale-row, but valid
 * and in-bounds) strip content -- see the SLACK clamp in bg_frame()'s
 * table-write loop -- never garbage or a torn read. */
static uint16_t layer_first_row(uint16_t camY)
{
	int32_t whole = BG_LAYER_SCROLL_POS - (int32_t)((camY * BG_LAYER_PARALLAX_Q8) / 256);
	int32_t row = whole % LAYER_H_PX;

	if (row < 0) row += LAYER_H_PX;
	return (uint16_t)row;
}

/* Fills STRIP_W pixels of screen line l's dedicated slot from background
 * row `row`, starting at background column base (must be a multiple of 4
 * -- see bg_init()'s and bg_frame()'s base calculations), wrapping
 * horizontally at the map's width. Walks blocks rather than pixels: only
 * the map lookup is per-block, the actual copy is a tight run of up to 16
 * bytes. Never called for a flat line (lineFlat[l]); those read a shared
 * slot fill_flat_slot() paints once instead, and never call this again for
 * as long as they stay flat. */
static void draw_strip(int l, int row, uint16_t base)
{
	int blockRow = row >> 4, rowInBlock = row & 15;
	const uint16_t *mapRow = g_bg_map + (uint32_t)blockRow * MAP_BLOCKS_W;
	volatile uint8_t *dst = (volatile uint8_t *)((&MARS_FRAMEBUFFER) + BG_DATA_WORD)
	                       + (uint32_t)l * STRIP_W;
	uint16_t x = base;
	int done = 0;

	while (done < STRIP_W) {
		int blockCol = (x >> 4) & (MAP_BLOCKS_W - 1);
		int colInBlock = x & 15;
		int n = 16 - colInBlock;
		const uint8_t *src = g_bg_blocks + (uint32_t)mapRow[blockCol] * BLOCK_BYTES
		                    + rowInBlock * 16 + colInBlock;
		int k;

		if (n > STRIP_W - done) n = STRIP_W - done;

		/* Longword stores measure 11% faster than word stores for
		 * framebuffer writes (hardware-budget.md sec 2), and this loop
		 * used to write a byte at a time -- one bus cycle per pixel, the
		 * single biggest cost in the old profile. Safe as longwords here
		 * because base (and so every x this loop reaches) is a multiple
		 * of 4, which keeps colInBlock, and so n, a multiple of 4 too;
		 * g_bg_blocks is forced 4-aligned in assets.s so src lines up the
		 * same way. The byte tail below only fires if that invariant is
		 * ever broken, e.g. by a future STRIP_W that isn't a multiple of
		 * 4 -- it should never execute as things stand. */
		k = 0;
		for (; k + 4 <= n; k += 4)
			*(volatile uint32_t *)(dst + k) = *(const uint32_t *)(src + k);
		for (; k < n; k++) dst[k] = src[k];

		dst += n;
		x = (uint16_t)(x + n);
		if (x >= MAP_W_PX) x -= MAP_W_PX;
		done += n;
	}
}

/* Paints one shared flat-colour slot, both consecutive real frames' worth
 * (see bg_init()) -- called only at init, never per frame. Every slot is
 * painted regardless of whether any line uses it yet: a line can start
 * using one on any later frame as rows scroll through (file header point
 * 1), and by then there is no "both banks" frame pair left to spend on it. */
static void fill_flat_slot(int slotIdx, uint8_t colour)
{
	volatile uint8_t *dst = (volatile uint8_t *)((&MARS_FRAMEBUFFER) + BG_DATA_WORD)
	                       + (uint32_t)SCREEN_HEIGHT * STRIP_W
	                       + (uint32_t)slotIdx * FLAT_STRIP_W;
	uint32_t word = ((uint32_t)colour << 24) | ((uint32_t)colour << 16)
	              | ((uint32_t)colour << 8) | colour;
	int k;

	for (k = 0; k < FLAT_STRIP_W; k += 4)
		*(volatile uint32_t *)(dst + k) = word;
}

/* Walks g_bg_map once, for every one of the layer's LAYER_H_PX rows (not
 * just whatever 224 happen to be on screen right now -- see the file
 * header), to decide which rows can be backed by a small shared
 * flat-colour strip instead of a dedicated STRIP_W one. A row qualifies
 * when every one of the MAP_BLOCKS_W blocks in its block row is the SAME
 * block index, and that block's BLOCK_BYTES pixels are all one colour --
 * computed once per 16-row block row and broadcast to all 16, since the
 * block map itself only varies at that granularity. Runs once, from
 * bg_init(), after bg_assets_init() has resolved g_bg_map/g_bg_blocks. */
static void classify_rows(void)
{
	int blockRow, row, c;

	flatColourCount = 0;
	for (blockRow = 0; blockRow < LAYER_H_PX / 16; blockRow++) {
		const uint16_t *mapRow = g_bg_map + (uint32_t)blockRow * MAP_BLOCKS_W;
		uint16_t firstBlock = mapRow[0];
		uint8_t colour = 0;
		int uniform = 1;
		int slot = -1;

		for (c = 1; c < MAP_BLOCKS_W; c++)
			if (mapRow[c] != firstBlock) { uniform = 0; break; }

		if (uniform) {
			const uint8_t *px = g_bg_blocks + (uint32_t)firstBlock * BLOCK_BYTES;

			colour = px[0];
			for (c = 1; c < BLOCK_BYTES; c++)
				if (px[c] != colour) { uniform = 0; break; }
		}

		if (uniform) {
			for (c = 0; c < flatColourCount; c++)
				if (flatColourVal[c] == colour) { slot = c; break; }
			if (slot < 0 && flatColourCount < MAX_FLAT_COLOURS) {
				slot = flatColourCount;
				flatColourVal[flatColourCount++] = colour;
			}
			/* else: colour table full. Row falls through as non-flat
			 * below -- correct, just not collapsed. */
		}

		for (row = blockRow * 16; row < blockRow * 16 + 16; row++) {
			rowFlat[row] = (uint8_t)(slot >= 0);
			if (slot >= 0) rowFlatSlot[row] = (uint8_t)slot;
		}
	}
}

/* Reads the descriptor exactly once. COMM12 carries the descriptor's
 * offset only during the boot handshake; once the slave starts publishing
 * steady-state frames it reuses the same register pair as COMM_ANIM/
 * COMM_TICK (comm.h), so a second read after that point would pick up
 * whatever the slave last wrote there instead. bg_assets_init() is called
 * before Hw32xInit specifically so this happens as early as possible, and
 * everything this file needs from the descriptor is pulled out right here,
 * once, rather than re-reading it later from bg_init(). */
void bg_assets_init(void)
{
	const AssetDescriptor *desc;
	uint32_t descAddr;

	/* Same one-shot boot flag assets_init() (s_main.c) polls; the two
	 * SH2s are unsynchronized with each other so both just race to read
	 * it, but the 68000 commits COMM12 strictly before COMM2, so either
	 * CPU sees valid data the instant it observes COMM2 != 0 (see
	 * assets.c's assets_init() for the full argument). */
	while (!MARS_SYS_COMM2) {}
	descAddr = 0x880000u + MARS_SYS_COMM12;
	desc = (const AssetDescriptor *)md_addr_to_sh2(descAddr);

	g_bg_map    = (const uint16_t *)md_addr_to_sh2(desc->bg_map);
	g_bg_blocks = (const uint8_t  *)md_addr_to_sh2(desc->bg_blocks);
	g_bg_pal    = (const uint16_t *)md_addr_to_sh2(desc->bg_pal);
	g_bg_lines  = (const BgLine   *)md_addr_to_sh2(desc->bg_lines);
}

void bg_init(void)
{
	volatile uint16_t *pal = &MARS_CRAM;
	uint16_t camX, camY, firstRow;
	int i, l, row;

	/* Only 24 of the 256 entries are actually used (see convert_bg.py's
	 * own report), but load all 256 as asked: cheap, and avoids leaving
	 * stray CRAM entries from whatever ran before this. */
	for (i = 0; i < 256; i++) pal[i] = g_bg_pal[i];

	/* All LAYER_H_PX rows now, not just the 224 a fixed row-per-line
	 * mapping used to limit this to: which rows are reachable moves with
	 * the camera (layer_first_row()), so every row needs its parallax/
	 * speed/drift and its flat classification ready up front. */
	for (row = 0; row < LAYER_H_PX; row++) {
		parallaxQ8[row] = g_bg_lines[row].parallax;
		speedQ8[row]    = g_bg_lines[row].speed;
		driftAccum[row] = 0;
	}

	classify_rows();

	/* Initial row/base: whatever camX/camY read as right now. If the
	 * slave hasn't published its first frame yet these are just the boot
	 * ready flag's and screenCenterY's leftover values (COMM2's and
	 * COMM6's other roles, see comm.h) rather than real camera state --
	 * harmless, since the very next bg_frame() corrects both, and nothing
	 * is visibly wrong in the meantime, only mispositioned for a few
	 * frames the way the X-only version already was.
	 *
	 * & 0x0FFFu: COMM6's steady-state word also carries dispRot in bits
	 * [14:12] and Player.drawGroupHigh in bit 15 (comm.h) -- this CPU (the
	 * master SH2) never reads either, it only wants camY, so both have to
	 * come off before use or a rotated or airborne-loop frame would read
	 * camY too high and badly mis-place every background row. */
	camX = MARS_SYS_COMM2;
	camY = MARS_SYS_COMM6 & 0x0FFFu;
	firstRow = layer_first_row(camY);
	for (l = 0; l < SCREEN_HEIGHT; l++) {
		row = (firstRow + l) % LAYER_H_PX;
		lineRow[l] = (uint16_t)row;
		framesDirty[l] = 0;
		if (rowFlat[row]) {
			lineFlat[l] = 1;
			lineFlatSlot[l] = rowFlatSlot[row];
		} else {
			lineFlat[l] = 0;
			bgBase[l] = (uint16_t)((line_offset(row, camX) - MARGIN)
			           & (MAP_W_PX - 1)) & ~3u;
		}
	}

	/* Paint every line's strip (dedicated or shared flat) into both banks
	 * before starting the per-frame rebase logic, the same two-consecutive-
	 * frames trick blitbench's vf_fill_bank(x2) uses for its own static
	 * content. */
	for (l = 0; l < SCREEN_HEIGHT; l++)
		if (!lineFlat[l]) draw_strip(l, lineRow[l], bgBase[l]);
	for (i = 0; i < flatColourCount; i++) fill_flat_slot(i, flatColourVal[i]);
	Hw32xScreenFlip(1);
	for (l = 0; l < SCREEN_HEIGHT; l++)
		if (!lineFlat[l]) draw_strip(l, lineRow[l], bgBase[l]);
	for (i = 0; i < flatColourCount; i++) fill_flat_slot(i, flatColourVal[i]);
	Hw32xScreenFlip(1);

	/* lineWordBase[] mirrors the slot each line just got painted into --
	 * dedicated by line index (draw_strip() above already used that
	 * addressing), or the shared flat slot -- so bg_frame()'s first table
	 * write is correct before any rebase has run. */
	for (l = 0; l < SCREEN_HEIGHT; l++)
		lineWordBase[l] = lineFlat[l]
		    ? (uint16_t)(BG_DATA_WORD + (uint32_t)SCREEN_HEIGHT * STRIP_WORDS
		                + (uint32_t)lineFlatSlot[l] * FLAT_STRIP_WORDS)
		    : (uint16_t)(BG_DATA_WORD + (uint32_t)l * STRIP_WORDS);

	pendingCount = 0;
}

void bg_frame(void)
{
	uint16_t camX = MARS_SYS_COMM2;   /* published by the slave, see comm.h */
	/* & 0x0FFFu strips dispRot (COMM6 bits [14:12]) and Player.drawGroupHigh
	 * (COMM6 bit 15, comm.h) -- this CPU only wants camY, see bg_init's
	 * matching comment. */
	uint16_t camY = MARS_SYS_COMM6 & 0x0FFFu;
	uint16_t firstRow = layer_first_row(camY);
	volatile uint16_t *table = &MARS_FRAMEBUFFER;
	uint16_t delta[SCREEN_HEIGHT];
	uint8_t  isRowDirty[SCREEN_HEIGHT];
	uint16_t bestUrgency[REBASE_CAP];
	uint16_t bestRow[REBASE_CAP];
	uint8_t  bestLine[REBASE_CAP];
	uint8_t n = 0, k, worst;
	int l, row;

	for (row = 0; row < LAYER_H_PX; row++) driftAccum[row] += speedQ8[row];

	/* Finish last frame's rebases first: redraw the already-committed
	 * row/base into THIS frame's write-target bank -- the physical bank
	 * Hw32x just handed the CPU is always the one last frame's write
	 * DIDN'T touch, so this is exactly the other bank from the one those
	 * lines were drawn into a moment ago. After this, both banks agree on
	 * every line's pixels, which TRIGGER's margin (see its comment above)
	 * guarantees happens before a horizontal candidate could possibly be
	 * selected again below; a row-dirty line CAN legitimately be selected
	 * again immediately after (another real row change, fairly queued via
	 * framesDirty[] like any other), which is correct, not a race -- this
	 * loop and the selection below never touch the same line in the same
	 * pass, so there is nothing to tear either way. */
	for (l = 0; l < pendingCount; l++) {
		int line = pendingLines[l];

		draw_strip(line, lineRow[line], bgBase[line]);
	}
	pendingCount = 0;

	/* Score every line. A row that just became reachable and is flat
	 * costs nothing and is applied immediately, uncapped (file header
	 * point 1). Everything else is scored into one urgency-ranked pool:
	 * row-dirty (ROW_DIRTY_BASE + framesDirty[l], always above any
	 * horizontal delta, which tops out at SLACK) or, failing that,
	 * horizontal-drift-over-TRIGGER (delta itself). isRowDirty[] records
	 * which lines were scored row-dirty this frame so the loop after
	 * selection knows which of them to age via framesDirty[]. */
	for (l = 0; l < SCREEN_HEIGHT; l++) {
		uint16_t wantedRow = (uint16_t)((firstRow + l) % LAYER_H_PX);
		int mismatch = (wantedRow != lineRow[l]);
		uint16_t urgency, targetRow;

		isRowDirty[l] = 0;

		if (mismatch && rowFlat[wantedRow]) {
			lineRow[l] = wantedRow;
			lineFlat[l] = 1;
			lineFlatSlot[l] = rowFlatSlot[wantedRow];
			lineWordBase[l] = (uint16_t)(BG_DATA_WORD
			                 + (uint32_t)SCREEN_HEIGHT * STRIP_WORDS
			                 + (uint32_t)lineFlatSlot[l] * FLAT_STRIP_WORDS);
			framesDirty[l] = 0;
			continue;
		}

		if (lineFlat[l]) {
			if (!mismatch) { framesDirty[l] = 0; continue; }
			targetRow = wantedRow;
			urgency = (uint16_t)(ROW_DIRTY_BASE + framesDirty[l]);
			isRowDirty[l] = 1;
		} else {
			uint16_t d = (uint16_t)(line_offset(lineRow[l], camX) - bgBase[l])
			           & (MAP_W_PX - 1);

			delta[l] = d;
			if (mismatch) {
				targetRow = wantedRow;
				urgency = (uint16_t)(ROW_DIRTY_BASE + framesDirty[l]);
				isRowDirty[l] = 1;
			} else if (d > TRIGGER) {
				targetRow = lineRow[l];
				urgency = d;
			} else {
				continue;
			}
		}

		if (n < REBASE_CAP) {
			bestUrgency[n] = urgency;
			bestRow[n] = targetRow;
			bestLine[n] = (uint8_t)l;
			n++;
			continue;
		}
		worst = 0;
		for (k = 1; k < REBASE_CAP; k++)
			if (bestUrgency[k] < bestUrgency[worst]) worst = k;
		if (urgency > bestUrgency[worst]) {
			bestUrgency[worst] = urgency;
			bestRow[worst] = targetRow;
			bestLine[worst] = (uint8_t)l;
		}
	}

	/* Commit the selection: pick each one's new (4-px-aligned, see
	 * draw_strip()) base against its target row, draw it into this frame's
	 * bank, and queue it for the catch-up draw above, one real frame from
	 * now. Standing still (or any frame where nothing crosses TRIGGER or
	 * changes row) makes n zero and this whole block a no-op -- the
	 * demand-driven part of the budget. */
	for (k = 0; k < n; k++) {
		int line = bestLine[k];
		int row2 = bestRow[k];
		uint16_t base = (uint16_t)((line_offset(row2, camX) - MARGIN)
		               & (MAP_W_PX - 1)) & ~3u;

		lineRow[line] = (uint16_t)row2;
		lineFlat[line] = 0;
		lineWordBase[line] = (uint16_t)(BG_DATA_WORD + (uint32_t)line * STRIP_WORDS);
		bgBase[line] = base;
		draw_strip(line, row2, base);
		pendingLines[pendingCount++] = (uint8_t)line;
		delta[line] = (uint16_t)(line_offset(row2, camX) - base) & (MAP_W_PX - 1);
		framesDirty[line] = 0;
	}

	/* Age every row-dirty line the cap didn't get to this frame, so the
	 * next frame's contest favours whoever has been waiting longest --
	 * see the file header point 2 and framesDirty[]'s own comment. */
	for (l = 0; l < SCREEN_HEIGHT; l++) {
		if (!isRowDirty[l]) continue;
		for (k = 0; k < n; k++)
			if (bestLine[k] == (uint8_t)l) break;
		if (k == n && framesDirty[l] != 0xFFu) framesDirty[l]++;
	}

	for (l = 0; l < SCREEN_HEIGHT; l++) {
		uint16_t d;

		if (lineFlat[l]) { table[l] = lineWordBase[l]; continue; }
		d = delta[l];
		/* Should never trip for horizontal drift given TRIGGER/REBASE_CAP
		 * above; a row-dirty line still waiting for cap priority reads
		 * this too, clamped the same way, since its stored strip is still
		 * for its OLD row and its OLD base is still a valid (if now
		 * stale) read into it -- exactly the bounded, graceful lag
		 * layer_first_row()'s comment describes, never a read past the
		 * strip's own edge. */
		if (d > SLACK) d = SLACK;
		table[l] = (uint16_t)(lineWordBase[l] + (d >> 1));
	}

	/* Palette shimmer (four CRAM entries rotated roughly every six
	 * frames in the original) would hook in here: a frame counter plus
	 * a CRAM write every ~6 frames, same shape as the driftAccum update
	 * above. Not implemented this pass -- see the file header. */
}
