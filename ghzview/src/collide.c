#include "md.h"
#include "collide.h"

/* Floor collision, ported from RSDK::ObjectTileCollision (CMODE_FLOOR) in
 * Collision.cpp.
 *
 * The mask value is the surface height measured DOWN from the top of the
 * block, so the world y of the surface is blockTop + mask. 0xFF means the
 * column is empty. The routine looks at three blocks, starting one above the
 * sensor point, and accepts a surface at or below the sensor within RSDK's own
 * 14 pixel tolerance. */

extern const uint16_t ghz_map[];
extern const uint8_t ghz_collide[];

#define MAP_W  256
#define MAP_H  128
#define BLOCK_MASK  0x0FFF
#define SOLID_FLOOR 0x1000
#define COLL_STRIDE 18
#define TILE_SIZE   16
#define GRIP        14

int16_t collide_floor(int16_t x, int16_t y, int16_t maxdist)
{
	int16_t colX = x, colY = y;
	int16_t cy = (colY & -TILE_SIZE) - TILE_SIZE;
	int16_t i;

	(void)maxdist;
	if (colX < 0 || colX >= MAP_W * TILE_SIZE) return COLL_NONE;

	for (i = 0; i < 3; i++, cy += TILE_SIZE) {
		uint16_t cell, b;
		uint8_t mask;
		int16_t ty;

		if (cy < 0 || cy >= MAP_H * TILE_SIZE) continue;

		cell = ghz_map[(cy / TILE_SIZE) * MAP_W + (colX / TILE_SIZE)];
		if (!(cell & SOLID_FLOOR)) continue;

		b = cell & BLOCK_MASK;
		mask = ghz_collide[b * COLL_STRIDE + (colX & 0xF)];
		if (mask == 0xFF) continue;

		ty = cy + mask;
		if (colY >= ty && (colY - ty) <= GRIP) return ty;
	}
	return COLL_NONE;
}

uint8_t collide_angle(int16_t x, int16_t y)
{
	uint16_t cell;
	uint16_t bx = (uint16_t)x >> 4, by = (uint16_t)y >> 4;

	if (bx >= MAP_W || by >= MAP_H) return 0;
	cell = ghz_map[by * MAP_W + bx];
	if (!(cell & SOLID_FLOOR)) return 0;
	return ghz_collide[(cell & BLOCK_MASK) * COLL_STRIDE + 16];
}
