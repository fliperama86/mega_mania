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
#include "pad.h"
#include "collide.h"
#include "player.h"

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

/* Placeholder for Sonic: a 32x40 box built from two hardware sprites, using
 * tiles generated at boot. Enough to see where the character is and to hang
 * the ground sensors off later. */
#define BOX_TILE   (TILE_BASE + 0)   /* filled in at boot, see build_box */
#define BOX_W      32
#define BOX_H      40

/* How far the box will climb or drop in one frame before it is treated as a
 * wall or as a fall. Sonic's own step tolerance is about this. */
#define STEP_UP    14
#define STEP_DOWN  14
#define FALL_SPEED 4

static uint16_t camX;           /* pixels */
static int16_t  boxX = 64, boxY = 0;   /* world pixels, top left of the box */
static uint16_t boxTile;
static uint8_t  groundAngle;
static int16_t  prevX;
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

/* A hollow rectangle, so the edges of the collision box are visible.
 * MD sprites read their tiles column major, and a sprite is at most 4x4 tiles,
 * so the 32x40 box is emitted as a 4x4 block followed by a 4x1 strip. */
static void build_box(uint16_t at)
{
	static uint32_t tile[21 * 8];
	uint16_t i = 0, tx, ty, y, px;

	for (tx = 0; tx < 4; tx++) {          /* top 32x32, column major */
		for (ty = 0; ty < 4; ty++) {
			for (y = 0; y < 8; y++) {
				uint32_t row = 0;
				for (px = 0; px < 8; px++) {
					uint16_t wx = tx * 8 + px, wy = ty * 8 + y;
					uint8_t v = (wx == 0 || wx == BOX_W - 1 || wy == 0) ? 2 : 1;
					row = (row << 4) | v;
				}
				tile[i++] = row;
			}
		}
	}
	for (tx = 0; tx < 4; tx++) {          /* bottom 32x8 strip */
		for (y = 0; y < 8; y++) {
			uint32_t row = 0;
			for (px = 0; px < 8; px++) {
				uint16_t wx = tx * 8 + px;
				uint8_t v = (wx == 0 || wx == BOX_W - 1 || y == 7) ? 2 : 1;
				row = (row << 4) | v;
			}
			tile[i++] = row;
		}
	}
	/* CPU writes, not vdp_tiles_load: this skeleton's DMA path only works
	 * from ROM, and handing it a RAM array corrupts VRAM elsewhere. */
	for (y = 0; y < 8; y++) tile[i++] = 0x33333333;   /* sensor marker */

	vram_addr(at * 32);
	for (i = 0; i < 21 * 8; i++)
		*((volatile uint32_t *)0xC00000) = tile[i];
}

int main(void)
{
	uint16_t tileCount = (uint16_t)(ghz_tiles_end - ghz_tiles) / 8;
	uint16_t lastCol, firstCol;
	int16_t dir = 0;
	uint16_t padPrev = 0;
	Player sonic;

	vdp_init();
	enable_ints;

	vdp_colors(0, ghz_pal, 64);
	vdp_tiles_load(ghz_tiles, TILE_BASE, tileCount);
	boxTile = TILE_BASE + tileCount;
	build_box(boxTile);
	player_init(&sonic, 80, camBlockY * 16 + 80);
	vdp_map_clear(VDP_PLAN_A);
	vdp_map_clear(VDP_PLAN_B);

	draw_background();
	draw_screen();
	firstCol = 0;
	lastCol = VIEW_BLOCKS_X - 1;

	/* Drive the camera with the pad so the whole conversion can be looked at */
	for (;;) {
		uint16_t limit = (MAP_W - VIEW_BLOCKS_X) * 16u;
		uint16_t pad = pad_read();

		(void)dir;
		player_update(&sonic, pad);
		boxX = (int16_t)(sonic.x >> 16) - 16;
		boxY = (int16_t)(sonic.y >> 16) - 20;
		groundAngle = sonic.angle;

		/* camera follows, keeping the player near the middle */
		{
			int16_t want = boxX - 144;
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
			lastCol = firstCol + VIEW_BLOCKS_X - 1;
		}

		/* the box lives in world space; sprites are screen space */
		{
			static VDPSprite list[4];
			int16_t sx = boxX - (int16_t)camX;
			int16_t sy = boxY - (int16_t)(camBlockY * 16);

			list[0].y = 128 + sy;
			list[0].size = SPRITE_SIZE(4, 4);
			list[0].link = 1;
			list[0].attr = TILE_ATTR(3, 1, 0, 0, boxTile);
			list[0].x = 128 + sx;

			list[1].y = 128 + sy + 32;
			list[1].size = SPRITE_SIZE(4, 1);
			list[1].link = 0;
			list[1].attr = TILE_ATTR(3, 1, 0, 0, boxTile + 16);
			list[1].x = 128 + sx;

			/* the two foot sensors, so their position is never a guess */
			list[2].y = 128 + sy + BOX_H;
			list[2].size = SPRITE_SIZE(1, 1);
			list[2].link = 3;
			list[2].attr = TILE_ATTR(1, 1, 0, 0, boxTile + 20);
			list[2].x = 128 + sx + 7;

			list[3].y = 128 + sy + BOX_H;
			list[3].size = SPRITE_SIZE(1, 1);
			list[3].link = 0;
			list[3].attr = TILE_ATTR(1, 1, 0, 0, boxTile + 20);
			list[3].x = 128 + sx + 24;

			sprites_write(list, 4);
		}

		vdp_vsync();
		vdp_hscroll(VDP_PLAN_A, -(int16_t)camX);
		vdp_hscroll(VDP_PLAN_B, -(int16_t)(camX >> 1));
	}

	return 0;
}
