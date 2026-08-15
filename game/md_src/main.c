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

/* Track to loop once the disc is spinning; the disc image built alongside
 * this ROM by tools/make_disc.py is audio-only and starts at track 1. */
#define CD_MUSIC_TRACK 2

extern const uint16_t ghz_pal[];
extern const uint32_t ghz_tiles[];
extern const uint32_t ghz_tiles_end[];
extern const uint16_t ghz_blocks[];
extern const uint16_t ghz_map[];
extern const uint16_t ghz_bgmap[];

#define MAP_W      256          /* blocks, matches convert_stage.py */
#define MAP_BLOCK_MASK 0x0FFF   /* bits 12-13 carry collision solidity */
#define MAP_SOLID_FLOOR 0x1000
#define MAP_SOLID_SIDES 0x2000
#define MAP_H      128
#define TILE_BASE  TILE_USERINDEX

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
		uint16_t b  = (by < MAP_H)
		            ? (ghz_map[by * MAP_W + blockX] & MAP_BLOCK_MASK) : 0;
		const uint16_t *e = &ghz_blocks[b * 4];

		colL[i * 2 + 0] = e[0] + TILE_BASE;
		colR[i * 2 + 0] = e[1] + TILE_BASE;
		colL[i * 2 + 1] = e[2] + TILE_BASE;
		colR[i * 2 + 1] = e[3] + TILE_BASE;
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
		uint16_t b  = (bx < MAP_W)
		            ? (ghz_map[blockY * MAP_W + bx] & MAP_BLOCK_MASK) : 0;
		const uint16_t *e = &ghz_blocks[b * 4];

		rowT[i * 2 + 0] = e[0] + TILE_BASE;
		rowT[i * 2 + 1] = e[1] + TILE_BASE;
		rowB[i * 2 + 0] = e[2] + TILE_BASE;
		rowB[i * 2 + 1] = e[3] + TILE_BASE;
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

/* The background now comes from the 32X framebuffer layer behind the Mega
 * Drive (see sh_src/m_main.c and docs/hardware-budget.md, section 3, "Layer
 * order: the MD draws in front"). Kept here, unused, as a record of what
 * Plane B used to hold. */
__attribute__((unused))
static void draw_background(void)
{
	uint16_t x, y;

	for (x = 0; x < VIEW_BLOCKS_X; x++) {
		for (y = 0; y < VIEW_BLOCKS_Y; y++) {
			uint16_t by = BG_TOP_ROW + y;
			uint16_t b  = (by < BGMAP_H)
			            ? (ghz_bgmap[by * BGMAP_W + x] & MAP_BLOCK_MASK) : 0;
			const uint16_t *e = &ghz_blocks[b * 4];

			vdp_map_xy(VDP_PLAN_B, e[0] + TILE_BASE, x * 2,     y * 2);
			vdp_map_xy(VDP_PLAN_B, e[1] + TILE_BASE, x * 2 + 1, y * 2);
			vdp_map_xy(VDP_PLAN_B, e[2] + TILE_BASE, x * 2,     y * 2 + 1);
			vdp_map_xy(VDP_PLAN_B, e[3] + TILE_BASE, x * 2 + 1, y * 2 + 1);
		}
	}
}

int main(void)
{
	uint16_t tileCount = (uint16_t)(ghz_tiles_end - ghz_tiles) / 8;
	uint16_t frame = 0;
	int16_t worldX = 0, worldY = 0;
	uint16_t frameIndex = 0;
	uint8_t facing = 0;
	int cdPresent, cdState = 0;

	/* First thing in boot, before anything else might rely on the audio
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
	vdp_tiles_load(ghz_tiles, TILE_BASE, tileCount);
	sonic_gfx_init(TILE_BASE + tileCount);

	/* CD bring-up is entirely 68000-local (only this CPU can reach the CD
	 * hardware) and every wait inside it is bounded, so it can run here
	 * without disturbing the SH2 handshake below. On the actual test
	 * hardware, a bare 32X with no CD unit, this returns promptly. */
	cdPresent = cd_init();
	if (cdPresent) cdState = cd_music_play(CD_MUSIC_TRACK) ? 2 : 1;

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
	while (!comm_read_frame(&camX, &camY, &worldX, &worldY, &frameIndex, &facing)) {}

	firstCol = camX >> 4;
	firstRow = camY >> 4;

	vdp_map_clear(VDP_PLAN_A);
	vdp_map_clear(VDP_PLAN_B);

	draw_screen();

	for (;;) {
		uint16_t pad = pad_read();
		uint16_t wantCol, wantRow;
		uint16_t used;
		static VDPSprite list[SONIC_MAX_PIECES];

		/* Right after pad_read(), before anything else, matching where the
		 * single-CPU original called player_update: this is what keeps the
		 * phase relationship between "a vblank happened" and "input for
		 * that vblank is available" unchanged now that the two live on
		 * different CPUs (see sh_src/comm.h). */
		comm_send_input(pad);

		/* Non-blocking: on a torn or absent update this just re-delivers
		 * the previous frame's cached camera/Sonic values. */
		comm_read_frame(&camX, &camY, &worldX, &worldY, &frameIndex, &facing);

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

		/* Sonic lives in world space; sprites are screen space */
		used = sonic_build(frameIndex,
		                   worldX - (int16_t)camX,
		                   worldY - (int16_t)camY,
		                   facing, list, 0);
		list[used - 1].link = 0;

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
				 * and bit 15 is the BIOS still busy. */
				sprintf(buf, "%04X %02X CD%d %04X", frame++, pad,
				        cdState, stat);
			}
			vdp_puts(VDP_PLAN_A, buf, 1, (camY >> 3) & (PLAN_HEIGHT - 1));
		}

		/* vdp_wait_vblank, not vdp_vsync: the latter returns once vblank has
		 * ended, which would put the tile DMA in active display where the VDP
		 * accepts a trickle and stalls the 68000 for most of the frame. */
		vdp_wait_vblank();
		/* The CD BIOS needs a level 2 interrupt at about this rate to keep
		 * running, and a 32X ROM cannot give it one from an interrupt
		 * handler. Safe to call with no CD present: it returns immediately
		 * unless cd_init() brought one up. */
		cd_vblank();
		sonic_upload(frameIndex);
		vdp_sprites_write(list, used);
		vdp_hscroll(VDP_PLAN_A, -(int16_t)camX);
		vdp_hscroll(VDP_PLAN_B, -(int16_t)(camX >> 1));
		vdp_vscroll(VDP_PLAN_A, (int16_t)camY);
		/* Plane B stays put vertically (vscroll left at the 0 vdp_init() set
		 * it to): the background has no vertical parallax yet, and it is
		 * bottom-aligned for the horizon, so a fixed horizon is fine for now. */
	}

	return 0;
}
