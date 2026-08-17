#include "sonic.h"
#include "sonic_rot_data.h"
#include "vdp.h"

/* Sonic gets the fourth hardware palette; the stage is fitted into the other
 * three, see tools/convert_stage.py. */
#define SONIC_PAL 3

/* Where the current frame is uploaded. Only the largest single frame has to
 * fit, which is why a character costs about a kilobyte of VRAM instead of the
 * forty its sheet would take. */
static uint16_t vramTile;

/* Sonic's baked rotated frames' tile pixels (assets/sonic/rot_tiles.bin,
 * tools/convert_sonic.py), too big for this program's own 512 KB ROM window
 * alongside everything else, so they are linked directly into the SH2
 * program instead (sh_src/sonic_rot.s), at cartridge offset 0x94000. That
 * offset falls in the 68000's banked window (0x900000-0x9FFFFF), which shows
 * bank 0 -- cartridge 0x000000-0x0FFFFF -- at 0xA15104's power-on value, and
 * nothing in this codebase ever writes that register, so the fixed pointer
 * below needs no bank switch to stay valid. This literal, sh_src/mars.ld's
 * sonicrot ORIGIN and sh_src/sonic_rot.s's AT() are a hand-synced trio:
 * change one, change all three -- same convention main.c's ghz_map_fgh_md
 * uses for FG High's map. */
static const uint32_t *const sonic_rot_tiles_md = (const uint32_t *)0x994000;

uint16_t sonic_gfx_init(uint16_t firstTile)
{
	vramTile = firstTile;
	return firstTile + SONIC_MAX_FRAME_TILES;
}

/* Resolved source for one display frame: which tile/piece tables to read,
 * and the flip bits to compose on top (TILE_ATTR's flipH/flipV plus piece
 * position mirroring). Shared by sonic_upload (needs only the tile half)
 * and sonic_build (needs all of it), so the fold logic below has exactly
 * one copy. */
typedef struct {
	const uint32_t *tiles;
	const SonicPiece *pieces;
	uint16_t tileOffset, pieceOffset;
	uint8_t tileCount, pieceCount;
	int8_t pivotX, pivotY;
	uint8_t flipH, flipV;
} FrameSource;

/* dispRot/flip(facing) -> which baked set and which flips, per frame's
 * sonic_rot_class[] (md_src/sonic_rot_data.h):
 *
 *   ROTCLASS_NONE  (ANI_JUMP/ANI_SKID/ANI_SKID_TURN): dispRot ignored, base
 *                  frame, only facing's flipH -- these are baked
 *                  ROTSTYLE_NONE in the original sheet (rotation computed
 *                  on the SH2, sh_src/player.c, but never displayed).
 *
 *   ROTCLASS_R180  (ANI_IDLE/ANI_PUSH/ANI_LOOK_UP/ANI_CROUCH): base frame
 *                  always (no baked art); flipV added on top of facing's
 *                  flipH when dispRot==4 (0x100 of 0x1FF, ROTSTYLE_180DEG's
 *                  own 2-state snap, Drawing.cpp:2715-2719) -- exact, since
 *                  rotating a raster 180 degrees is flipH+flipV.
 *
 *   ROTCLASS_FULL  (ANI_WALK/ANI_JOG/ANI_RUN/ANI_DASH/ANI_AIR_WALK): fold
 *                  facing into the step first -- facing left mirrors the
 *                  whole rotation, which for a horizontal-flip-then-rotate
 *                  composition is the same picture as negating the step
 *                  and rotating first, then flipping (flip and rotation
 *                  anticommute in sign for a horizontal mirror):
 *                    step = facing ? (8 - dispRot) & 7 : dispRot;
 *                  then, since only 45/90/135 degrees are baked (steps
 *                  1-3) and 180/225/270/315 (steps 4-7) are exact flips of
 *                  0/45/90/135:
 *                    if (step >= 4) { step -= 4; flipH ^= 1; flipV ^= 1; }
 *                  step 0 is the base frame (upright, or facing's flipH
 *                  alone if step reached 0 only via the >=4 branch, which
 *                  also toggled both flips -- a walk cycle rotated exactly
 *                  180 degrees, e.g. upside-down at a loop's top); steps
 *                  1/2/3 select sonic_rot_frames[rotIndex + step-1] (the
 *                  45/90/135-degree baked set) with whatever flips the
 *                  folds above left set. Piece POSITIONS mirror around the
 *                  pivot for flipV the same way they already do for flipH
 *                  (TILE_ATTR's flip bits mirror each piece's own tile
 *                  content automatically; only where a piece's box sits
 *                  needs the sign flip here). */
static void resolve_frame(uint16_t frameIndex, uint8_t dispRot, uint8_t facing,
                          FrameSource *out)
{
	const SonicFrame *f = &sonic_frames[frameIndex];
	uint8_t cls = sonic_rot_class[frameIndex];
	uint8_t flipH = (uint8_t)(facing & 1u);
	uint8_t flipV = 0;

	out->tiles = sonic_tiles;
	out->pieces = sonic_pieces;
	out->tileOffset = f->tileOffset;
	out->pieceOffset = f->pieceOffset;
	out->tileCount = f->tileCount;
	out->pieceCount = f->pieceCount;
	out->pivotX = f->pivotX;
	out->pivotY = f->pivotY;

	if (cls == SONIC_ROTCLASS_R180) {
		if (dispRot == 4) flipV = 1;
	} else if (cls == SONIC_ROTCLASS_FULL) {
		uint8_t step = dispRot;
		int16_t rotIdx;

		if (facing) step = (uint8_t)((8u - step) & 7u);
		if (step >= 4) {
			step = (uint8_t)(step - 4u);
			flipH ^= 1u;
			flipV ^= 1u;
		}
		if (step != 0) {
			rotIdx = sonic_rot_index[frameIndex];
			if (rotIdx >= 0) {
				const SonicRotFrame *rf = &sonic_rot_frames[rotIdx + (step - 1)];
				out->tiles = sonic_rot_tiles_md;
				out->pieces = sonic_rot_pieces;
				out->tileOffset = rf->tileOffset;
				out->pieceOffset = rf->pieceOffset;
				out->tileCount = rf->tileCount;
				out->pieceCount = rf->pieceCount;
				out->pivotX = rf->pivotX;
				out->pivotY = rf->pivotY;
			}
		}
	}
	/* else SONIC_ROTCLASS_NONE: base frame, upright, facing's flipH only */

	out->flipH = flipH;
	out->flipV = flipV;
}

void sonic_upload(uint16_t frameIndex, uint8_t dispRot, uint8_t facing)
{
	FrameSource fs;

	/* facing has to match what sonic_build() was/will be called with for
	 * this same display frame: for ROTCLASS_FULL frames it can select a
	 * different baked set (45 vs 135 degrees), not just a flip bit -- see
	 * resolve_frame's comment. */
	resolve_frame(frameIndex, dispRot, facing, &fs);
	vdp_tiles_load(&fs.tiles[fs.tileOffset * 8], vramTile, fs.tileCount);
}

uint16_t sonic_build(uint16_t frameIndex, uint8_t dispRot, int16_t sx, int16_t sy,
                     uint8_t flip, uint8_t drawGroupHigh, VDPSprite *list,
                     uint16_t firstLink)
{
	FrameSource fs;
	const SonicPiece *p;
	uint16_t i;

	resolve_frame(frameIndex, dispRot, flip, &fs);
	p = &fs.pieces[fs.pieceOffset];

	for (i = 0; i < fs.pieceCount; i++, p++) {
		/* Facing/rotation flips mirror the whole assembly about the entity,
		 * so a piece sits its own width/height in from the far edge and is
		 * flipped itself (TILE_ATTR's flipH/flipV bits handle a piece's own
		 * tile content automatically, hardware side -- this only has to
		 * mirror where the piece's box sits). */
		int16_t w = (int16_t)(((p->size >> 2) & 3) + 1) << 3;
		int16_t h = (int16_t)((p->size & 3) + 1) << 3;
		int16_t dx = fs.flipH ? (int16_t)(-fs.pivotX - p->dx - w)
		                      : (int16_t)(fs.pivotX + p->dx);
		int16_t dy = fs.flipV ? (int16_t)(-fs.pivotY - p->dy - h)
		                      : (int16_t)(fs.pivotY + p->dy);

		list[i].y = 128 + sy + dy;
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
		list[i].attr = TILE_ATTR(SONIC_PAL, drawGroupHigh, fs.flipV, fs.flipH,
		                         vramTile + p->tile);
		list[i].x = 128 + sx + dx;
	}
	return fs.pieceCount;
}
