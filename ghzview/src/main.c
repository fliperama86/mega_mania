/* Green Hill Zone, converted to Mega Drive assets and scrolled with the VDP.
 *
 * The point of this ROM is to prove the art pipeline end to end: tile
 * reduction, palette fitting, layout conversion, and hardware scrolling with
 * columns streamed in as the camera moves. No physics, no objects.
 *
 * Blocks are 16x16, so two cells by two. Plane A is 64x32 cells, which is 32
 * blocks by 16, and it wraps, so scrolling only ever needs the one column that
 * just came into view.
 */

#include "md.h"

extern const uint16_t ghz_pal[];
extern const uint32_t ghz_tiles[];
extern const uint32_t ghz_tiles_end[];
extern const uint16_t ghz_blocks[];
extern const uint16_t ghz_map[];
extern const uint16_t ghz_bgmap[];

#define MAP_W      256          /* blocks, matches convert_stage.py */
#define MAP_H      128
#define TILE_BASE  TILE_USERINDEX

#define VIEW_BLOCKS_X 32        /* 64 cells */
#define VIEW_BLOCKS_Y 16        /* 32 cells */

#define BGMAP_W    512          /* BG Outside, from convert_stage.py */
#define BGMAP_H    24
/* Which BG row lands at the top of the screen. Lower puts the horizon further
 * down, which is how Mania frames it. */
#define BG_TOP_ROW 3

static uint16_t camX;           /* pixels */
static uint16_t camBlockY = 48;  /* ground at the act start sits near row 55 */

/* One column of blocks, two cells wide, straight into plane A */
static void draw_block_column(uint16_t blockX)
{
	uint16_t cellX = (blockX * 2) & (PLAN_WIDTH - 1);
	uint16_t colL[VIEW_BLOCKS_Y * 2];
	uint16_t colR[VIEW_BLOCKS_Y * 2];
	uint16_t i;

	if (blockX >= MAP_W) return;

	for (i = 0; i < VIEW_BLOCKS_Y; i++) {
		uint16_t by = camBlockY + i;
		uint16_t b  = (by < MAP_H) ? ghz_map[by * MAP_W + blockX] : 0;
		const uint16_t *e = &ghz_blocks[b * 4];

		colL[i * 2 + 0] = e[0] + TILE_BASE;
		colR[i * 2 + 0] = e[1] + TILE_BASE;
		colL[i * 2 + 1] = e[2] + TILE_BASE;
		colR[i * 2 + 1] = e[3] + TILE_BASE;
	}

	for (i = 0; i < VIEW_BLOCKS_Y * 2; i++) {
		vdp_map_xy(VDP_PLAN_A, colL[i], cellX,     i);
		vdp_map_xy(VDP_PLAN_A, colR[i], cellX + 1, i);
	}
}

static void draw_screen(void)
{
	uint16_t x;
	for (x = 0; x < VIEW_BLOCKS_X; x++) draw_block_column(x);
}

/* The background is small enough to sit in plane B whole, and it scrolls at
 * half speed for parallax. Bottom aligned so the horizon meets the ground. */
static void draw_background(void)
{
	uint16_t x, y;

	for (x = 0; x < VIEW_BLOCKS_X; x++) {
		for (y = 0; y < VIEW_BLOCKS_Y; y++) {
			uint16_t by = BG_TOP_ROW + y;
			uint16_t b  = (by < BGMAP_H) ? ghz_bgmap[by * BGMAP_W + x] : 0;
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
	uint16_t lastCol;
	int16_t dir = 0;   /* start still, so the opening screen can be compared */

	vdp_init();
	enable_ints;

	vdp_colors(0, ghz_pal, 64);
	vdp_tiles_load(ghz_tiles, TILE_BASE, tileCount);
	vdp_map_clear(VDP_PLAN_A);
	vdp_map_clear(VDP_PLAN_B);

	draw_background();
	draw_screen();
	lastCol = VIEW_BLOCKS_X - 1;

	/* Auto scroll, turning round at either end of the converted window */
	for (;;) {
		uint16_t limit = (MAP_W - VIEW_BLOCKS_X) * 16u;

		if (dir > 0 && camX >= limit) dir = -2;
		if (dir < 0 && camX == 0)     dir = 2;
		camX += dir;

		/* stream in whatever column just came into view */
		{
			uint16_t need = (camX >> 4) + VIEW_BLOCKS_X - 1;
			while (lastCol < need) {
				lastCol++;
				draw_block_column(lastCol);
			}
		}

		vdp_vsync();
		vdp_hscroll(VDP_PLAN_A, -(int16_t)camX);
		vdp_hscroll(VDP_PLAN_B, -(int16_t)(camX >> 1));
	}

	return 0;
}
