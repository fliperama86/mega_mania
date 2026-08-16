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
                     uint8_t drawGroupHigh, VDPSprite *list, uint16_t firstLink)
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
		/* Priority follows drawGroupHigh instead of being fixed low: FG Low
		 * (Plane A, always low) belongs behind Sonic and FG High (Plane B,
		 * always high -- see main.c's draw_block_column/row) belongs in
		 * front of him only while drawGroupHigh is set, matching Mania's
		 * PlaneSwitch_CheckCollisions (PlaneSwitch.c:94-109: other->drawGroup
		 * = low/high, chosen off the marker's flags bits the player crossed
		 * on the correct side of -- the same Zone->playerDrawGroup[0]/[1]
		 * mechanism the original names) rather than the fixed above-player
		 * stacking this port drew before PlaneSwitch existed. That needs
		 * Sonic able to sit between the two planes, which the Genesis VDP's
		 * fixed layer order only gives a low-priority sprite: back to front,
		 * Plane B low, Plane A low, sprite low, Plane B high, Plane A high,
		 * sprite high -- high priority here draws Sonic above FG High, low
		 * draws him below it. drawGroupHigh arrives from the slave SH2 over
		 * COMM6's bit 15 (sh_src/comm.h; sh_src/plane_switch.c ports
		 * PlaneSwitch_CheckCollisions itself). */
		list[i].attr = TILE_ATTR(SONIC_PAL, drawGroupHigh, 0, flip, vramTile + p->tile);
		list[i].x = 128 + sx + dx;
	}
	return f->pieceCount;
}
