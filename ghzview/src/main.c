/* Green Hill Zone, converted to Mega Drive assets, with Sonic on it.
 *
 * The point of this ROM is to prove the pipeline end to end: tile reduction,
 * palette fitting, layout conversion, hardware scrolling with columns streamed
 * in as the camera moves, and the ported physics and collision driving the
 * real character sprite.
 *
 * Blocks are 16x16, so two cells by two. Plane A is 64x32 cells, which is 32
 * blocks by 16, and it wraps, so scrolling only ever needs the one column that
 * just came into view.
 */

#include "md.h"
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

static uint16_t camX;            /* pixels */
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
		/* bits 12-13 are collision flags, not part of the index */
		uint16_t b  = (by < MAP_H)
		            ? (ghz_map[by * MAP_W + blockX] & MAP_BLOCK_MASK) : 0;
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

int main(void)
{
	uint16_t tileCount = (uint16_t)(ghz_tiles_end - ghz_tiles) / 8;
	uint16_t firstCol;
	Player sonic;

	vdp_init();
	enable_ints;

	/* three palettes for the stage, the fourth is Sonic's */
	vdp_colors(0, ghz_pal, 48);
	vdp_colors(48, sonic_pal, 16);
	vdp_tiles_load(ghz_tiles, TILE_BASE, tileCount);
	sonic_gfx_init(TILE_BASE + tileCount);
	player_init(&sonic, 80, camBlockY * 16 + 80);
	vdp_map_clear(VDP_PLAN_A);
	vdp_map_clear(VDP_PLAN_B);

	draw_background();
	draw_screen();
	firstCol = 0;

	for (;;) {
		uint16_t limit = (MAP_W - VIEW_BLOCKS_X) * 16u;
		uint16_t pad = pad_read();
		int16_t worldX, worldY;
		uint16_t used;
		static VDPSprite list[SONIC_MAX_PIECES];

		player_update(&sonic, pad);
		worldX = (int16_t)(sonic.e.x >> 16);
		worldY = (int16_t)(sonic.e.y >> 16);

		/* camera follows, keeping the player near the middle */
		{
			int16_t want = worldX - 160;
			if (want < 0) want = 0;
			if (want > (int16_t)limit) want = (int16_t)limit;
			camX = (uint16_t)want;
		}

		/* Stream the column that just entered view. The plane is only
		 * VIEW_BLOCKS_X wide and wraps, so the window has to move as a fixed
		 * width range: expanding it on both sides leaves stale columns behind
		 * once the camera has travelled more than one plane width. */
		{
			uint16_t want = camX >> 4;
			while (firstCol < want) {
				firstCol++;
				draw_block_column(firstCol + VIEW_BLOCKS_X - 1);
			}
			while (firstCol > want) {
				firstCol--;
				draw_block_column(firstCol);
			}
		}

		/* Sonic lives in world space; sprites are screen space */
		used = sonic_build(&sonic.animator,
		                   worldX - (int16_t)camX,
		                   worldY - (int16_t)(camBlockY * 16),
		                   sonic.direction, list, 0);
		list[used - 1].link = 0;

		vdp_vsync();
		/* the frame's tiles go in during vblank, the sprite table right after */
		sonic_upload(&sonic.animator);
		sprites_write(list, used);
		vdp_hscroll(VDP_PLAN_A, -(int16_t)camX);
		vdp_hscroll(VDP_PLAN_B, -(int16_t)(camX >> 1));
	}

	return 0;
}
