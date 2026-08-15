/* Green Hill Zone, converted to Mega Drive assets, with Sonic on it.
 *
 * The point of this ROM is to prove the pipeline end to end: tile reduction,
 * palette fitting, layout conversion, hardware scrolling with rows and
 * columns streamed in as the camera moves, and the ported physics and
 * collision driving the real character sprite.
 *
 * Blocks are 16x16, so two cells by two. Plane A is 64x32 cells, which is 32
 * blocks by 16, and it wraps in both axes, so scrolling only ever needs the
 * one row or column that just came into view.
 */

#include "md.h"
#include "camera.h"
#include "pad.h"
#include "player.h"
#include "sonic.h"

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

static uint16_t camX;      /* pixels, screen's top-left corner in the world */
static uint16_t camY;
static uint16_t centerY;   /* Camera_Create: screenCenterY - 16 */

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

/* The background is small enough to sit in plane B whole, and it scrolls at
 * half speed for parallax. Bottom aligned so the horizon meets the ground. */
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

/* This skeleton's DMA path only works from ROM: handing it a RAM buffer
 * silently does nothing or corrupts VRAM. Sprites live in RAM, so the table is
 * written with the CPU. Same reason the tile upload and map writes below avoid
 * vdp_tiles_load and vdp_map_vline. */
static void vram_addr(uint16_t addr)
{
	*((volatile uint32_t *)0xC00004) =
		(((uint32_t)(0x4000 | (addr & 0x3FFF))) << 16) | ((addr >> 14) & 3);
}

static void sprites_write(const VDPSprite *list, uint16_t count)
{
	const uint16_t *w = (const uint16_t *)list;
	uint16_t i;

	vram_addr(VDP_SPRITE_TABLE);
	for (i = 0; i < count * 4; i++)
		*((volatile uint16_t *)0xC00000) = w[i];
}

/* Turn the camera's 16.16 world position into the screen's top-left corner
 * and clamp it to the map. camera.c only knows about the target it is
 * following, not the map's size, so that clamping belongs here. */
static void update_scroll(const Camera *cam)
{
	int32_t x = (cam->x >> 16) - SCREEN_HALF_W;
	int32_t y = (cam->y >> 16) - (int32_t)centerY;
	uint16_t limitX = (MAP_W - VIEW_BLOCKS_X) * 16u;
	uint16_t limitY = MAP_H * 16u - 224u;

	if (x < 0) x = 0;
	if (x > (int32_t)limitX) x = (int32_t)limitX;
	if (y < 0) y = 0;
	if (y > (int32_t)limitY) y = (int32_t)limitY;

	camX = (uint16_t)x;
	camY = (uint16_t)y;
}

int main(void)
{
	uint16_t tileCount = (uint16_t)(ghz_tiles_end - ghz_tiles) / 8;
	uint16_t frame = 0;
	Player sonic;
	Camera cam;

	vdp_init();
	pad_init();
	enable_ints;

	/* three palettes for the stage, the fourth is Sonic's */
	vdp_colors(0, ghz_pal, 48);
	vdp_colors(48, sonic_pal, 16);
	vdp_tiles_load(ghz_tiles, TILE_BASE, tileCount);
	sonic_gfx_init(TILE_BASE + tileCount);
	player_init(&sonic, 80, 848);   /* ground at the act start sits near row 53 */
	camera_init(&cam, sonic.e.x, sonic.e.y);
	centerY = (uint16_t)(SCREEN_HALF_H - 16);   /* Camera_Create: screenCenterY - 16 */

	update_scroll(&cam);
	firstCol = camX >> 4;
	firstRow = camY >> 4;

	vdp_map_clear(VDP_PLAN_A);
	vdp_map_clear(VDP_PLAN_B);

	draw_background();
	draw_screen();

	for (;;) {
		uint16_t pad = pad_read();
		int16_t worldX, worldY;
		uint16_t wantCol, wantRow;
		uint16_t used;
		static VDPSprite list[SONIC_MAX_PIECES];

		player_update(&sonic, pad);
		if (sonic.justJumped) camera_open_y_offset(&cam);
		camera_update(&cam, sonic.e.x, sonic.e.y);
		update_scroll(&cam);

		worldX = (int16_t)(sonic.e.x >> 16);
		worldY = (int16_t)(sonic.e.y >> 16);

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
		used = sonic_build(&sonic.animator,
		                   worldX - (int16_t)camX,
		                   worldY - (int16_t)camY,
		                   sonic.direction, list, 0);
		list[used - 1].link = 0;

		/* A frame counter next to the raw pad bits: on real hardware this is
		 * what separates a hang from an input that never arrives. Plane A
		 * scrolls under it, so it goes on whichever cell row the top of the
		 * screen currently lands on rather than on plane row 0. */
		{
			char buf[16];
			sprintf(buf, "%04X %02X", frame++, pad);
			vdp_puts(VDP_PLAN_A, buf, 1, (camY >> 3) & (PLAN_HEIGHT - 1));
		}

		/* vdp_wait_vblank, not vdp_vsync: the latter returns once vblank has
		 * ended, which would put the tile DMA in active display where the VDP
		 * accepts a trickle and stalls the 68000 for most of the frame. */
		vdp_wait_vblank();
		sonic_upload(&sonic.animator);
		sprites_write(list, used);
		vdp_hscroll(VDP_PLAN_A, -(int16_t)camX);
		vdp_hscroll(VDP_PLAN_B, -(int16_t)(camX >> 1));
		vdp_vscroll(VDP_PLAN_A, (int16_t)camY);
		/* Plane B stays put vertically (vscroll left at the 0 vdp_init() set
		 * it to): the background has no vertical parallax yet, and it is
		 * bottom-aligned for the horizon, so a fixed horizon is fine for now. */
	}

	return 0;
}
