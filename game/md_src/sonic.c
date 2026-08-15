#include "sonic.h"
#include "vdp.h"

/* Sonic gets the fourth hardware palette; the stage is fitted into the other
 * three, see tools/convert_stage.py. */
#define SONIC_PAL 3

/* Where the current frame is uploaded. Only the largest single frame has to
 * fit, which is why a character costs about a kilobyte of VRAM instead of the
 * forty its sheet would take. */
static uint16_t vramTile;

uint16_t sonic_gfx_init(uint16_t firstTile)
{
	vramTile = firstTile;
	return firstTile + SONIC_MAX_FRAME_TILES;
}

static const SonicFrame *frame_at(uint16_t frameIndex)
{
	return &sonic_frames[frameIndex];
}

void sonic_upload(uint16_t frameIndex)
{
	const SonicFrame *f = frame_at(frameIndex);

	vdp_tiles_load(&sonic_tiles[f->tileOffset * 8], vramTile, f->tileCount);
}

uint16_t sonic_build(uint16_t frameIndex, int16_t sx, int16_t sy, uint8_t flip,
                     VDPSprite *list, uint16_t firstLink)
{
	const SonicFrame *f = frame_at(frameIndex);
	const SonicPiece *p = &sonic_pieces[f->pieceOffset];
	uint16_t i;

	for (i = 0; i < f->pieceCount; i++, p++) {
		/* Facing left mirrors the whole assembly about the entity, so a piece
		 * sits its own width in from the far edge and is flipped itself. */
		int16_t w = (int16_t)(((p->size >> 2) & 3) + 1) << 3;
		int16_t dx = flip ? (int16_t)(-f->pivotX - p->dx - w)
		                  : (int16_t)(f->pivotX + p->dx);

		list[i].y = 128 + sy + f->pivotY + p->dy;
		list[i].size = p->size;
		list[i].link = firstLink + i + 1;
		list[i].attr = TILE_ATTR(SONIC_PAL, 1, 0, flip, vramTile + p->tile);
		list[i].x = 128 + sx + dx;
	}
	return f->pieceCount;
}
