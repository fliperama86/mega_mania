#include <stdint.h>
#include "path.h"
#include "trig.h"

/* Ported from RSDK Collision.cpp: FindFloorPosition, FindLWallPosition,
 * FindRoofPosition, FindRWallPosition, SetPathGripSensors, ProcessPathGrip
 * and ProcessAirCollision_Down, built as RETRO_REV0U=1, RETRO_USE_ORIGINAL_CODE=0.
 *
 * The stage data is a block map rather than RSDK's tile layer, but the shape
 * is the same: a 16x16 cell with a mask per column giving the surface, and
 * solidity carried on the map entry. collisionEntity->collisionPlane (here,
 * PathEntity.collisionPlane, written only by plane_switch.c) selects between
 * RSDK's two TileConfig paths -- see the SOLID_FLOOR/SOLID_SIDES macros and
 * collide_row() below, transcribed one finder at a time from that finder's
 * own `solid =` line in Collision.cpp, not a blanket rule: the air
 * *Collision family reads a different bit pairing than the position finders
 * do (see the macro comment). collisionMasks[cPlane]/tileInfo[cPlane] being
 * orthogonal to the per-layer loop in the original means both layers -- FG
 * Low and FG High -- always read the same plane; nothing here scans one
 * layer on plane 0 and the other on plane 1. TILECOLLISION_UP and its
 * ProcessAirCollision_Up counterpart are dropped since nothing here runs
 * upside-down gravity. The per-*layer* loop does NOT collapse anywhere
 * below: the original registers both FG Low and FG High as collision layers
 * (SonicMania/Objects/Global/Zone.c:212) and every one of RSDK's eight
 * finders (FindFloorPosition/FindLWallPosition/FindRoofPosition/
 * FindRWallPosition and FloorCollision/RoofCollision/LWallCollision/
 * RWallCollision) scans both, so every finder here does too -- see each
 * group's own comment for what state it carries across the two layers,
 * since it is not the same shape in both groups.
 *
 * ghz_map and ghz_collide_index/ghz_collide_rows (plane 0) are linked into
 * the 68000 program only; this SH2 side reaches them through runtime
 * pointers assets.c fills in from the descriptor table (see assets.h), not
 * through a linked-in extern array. ghz_map_fgh and ghz_collide_b_index/
 * ghz_collide_b_rows (plane 1) are different again: see their own
 * comments. */

extern const uint16_t *g_ghz_map;
/* FG High (loop fronts, overhangs): unlike g_ghz_map above, this is not
 * reached through the descriptor table and md_addr_to_sh2() -- it is not
 * linked into the 68000 program at all. sh_src/map_fgh.s links it straight
 * into this SH2 program's own image, at the fixed address sh_src/mars.ld's
 * maphigh region gives it, so it is an ordinary linked array here, resolved
 * by this link the same way any other extern is. */
extern const uint16_t ghz_map_fgh[];
/* collide_index[block] is a row number into collide_rows; convert_stage.py
 * dedups identical 70-byte rows (many blocks -- different tiles, or flip
 * variants whose masks happen to be symmetric -- share one), so this is one
 * extra indirection rather than a single b * STRIDE lookup into one flat
 * per-block array. */
extern const uint16_t *g_ghz_collide_index;
extern const uint8_t *g_ghz_collide_rows;
/* Plane 1's block->row index and rows: sh_src/collide_b.s links these
 * straight into this SH2 program's own image (like ghz_map_fgh above), not
 * reached through the descriptor table -- see that file's comment for why
 * path B takes this route instead of path A's. Same index-then-row shape as
 * g_ghz_collide_index/g_ghz_collide_rows above, just a linked array instead
 * of a runtime pointer, so collide_row() below can treat the two
 * symmetrically. */
extern const uint16_t ghz_collide_b_index[];
extern const uint8_t ghz_collide_b_rows[];
/* FG Low's size in blocks, published by the 68000 through the descriptor
 * table and filled in by assets_init() (sh_src/assets.c). Not a local
 * #define: see md_src/descriptor.h's GHZ_MAP_W comment for why keeping a
 * second copy of this number here is exactly the bug this is avoiding. */
extern uint16_t g_map_w, g_map_h;

#define CELL_SIZE 16   /* the collision grid, not the VDP tile */

#define BLOCK_MASK  0x0FFF

/* Per-plane solid bits, transcribed per finder from that finder's own
 * `solid =` line in Collision.cpp (RETRO_REV0U=1; tileCollisions is always
 * TILECOLLISION_DOWN in this port -- see this file's top comment -- so
 * every finder's REV0U branch always takes its TILECOLLISION_DOWN arm,
 * which is also byte-for-byte what the same finder's non-REV0U #else arm
 * computes, so there is exactly one formula per finder regardless):
 *   FindFloorPosition (2174), FloorCollision (2394): SOLID_FLOOR
 *   FindRoofPosition (2287), RoofCollision (2520): SOLID_SIDES
 *   FindLWallPosition (2236), FindRWallPosition (2341):
 *     SOLID_FLOOR|SOLID_SIDES -- either bit blocks a ground wall probe,
 *     unlike every other finder
 *   LWallCollision (2477), RWallCollision (2603): SOLID_SIDES alone, NOT
 *     the pair -- a push-out air probe only needs to know something solid
 *     is there, not whether it is a plausible slope continuation */
#define SOLID_FLOOR(plane) ((plane) ? (1u << 14) : (1u << 12))
#define SOLID_SIDES(plane) ((plane) ? (1u << 15) : (1u << 13))

#define STRIDE     70
#define OFF_FLOOR  0
#define OFF_LWALL  16
#define OFF_RWALL  32
#define OFF_ROOF   48
#define OFF_ANGLE  64        /* floor, lwall, rwall, roof */

#define TO_FIXED(x)   ((int32_t)(x) << 16)
#define FROM_FIXED(x) ((int32_t)(x) >> 16)

/* highCollisionTolerance/lowCollisionTolerance and the three angle
 * tolerances, as fixed by this build's RSDK config. */
#define HIGH_COLLISION_TOLERANCE 14
#define LOW_COLLISION_TOLERANCE  8
#define FLOOR_ANGLE_TOLERANCE 0x20
#define WALL_ANGLE_TOLERANCE  0x20
#define ROOF_ANGLE_TOLERANCE  0x20

/* COLLISION_OFFSET, RETRO_REV0U=1 RETRO_USE_ORIGINAL_CODE=0 (Collision.cpp:
 * 9-18): the TO_FIXED(8) branch is REV0U-original-code only, this build's
 * config takes TO_FIXED(4). Gated per call by useCollisionOffset
 * (Collision.cpp:949-961), computed once in path_grip and threaded into
 * set_sensors below -- see its own comment for why path_air never needs it. */
#define COLLISION_OFFSET TO_FIXED(4)

/* Set on entry by path_grip/path_air exactly as ProcessObjectMovement sets
 * RSDK::collisionTolerance; the four position finders below read it. */
static int32_t collisionTolerance;

static int32_t iabs(int32_t v)
{
	return v < 0 ? -v : v;
}

static uint16_t cell_at(const uint16_t *map, int32_t cx, int32_t cy)
{
	/* g_map_w/g_map_h are uint16_t, but every operand here promotes to
	 * this build's 32-bit int (confirmed: no -mshort on either CPU)
	 * before the multiply, same as cx/cy already were, so the row*width
	 * term (up to 127*1024) never truncates through a 16-bit intermediate.
	 * map_w/map_h are shared by both layouts (converter-asserted equal),
	 * so the bounds check needs no map-specific variant. */
	if (cx < 0 || cy < 0 || cx >= g_map_w * CELL_SIZE || cy >= g_map_h * CELL_SIZE)
		return 0;
	return map[(cy / CELL_SIZE) * g_map_w + (cx / CELL_SIZE)];
}

/* collide_index[block] is a row number into collide_rows, one indirection
 * (see the extern declarations above); plane 0 reaches both through the
 * runtime pointers assets.c fills in, plane 1 through sh_src/collide_b.s's
 * linked arrays. Centralized here rather than open-coded in each of the
 * eight finders below: that many call sites duplicating one branch is
 * exactly the kind of small fixed-shape logic this SH2 target's compiler
 * tends to duplicate rather than share, and sh_src/mars.ld's rom region has
 * no room to spare for that (same reasoning find_floor_layer's own comment
 * gives for staying a real function instead of a 2-iteration layer loop). */
static const uint8_t *collide_row(int32_t plane, uint16_t block)
{
	uint16_t b = block & BLOCK_MASK;
	if (plane)
		return &ghz_collide_b_rows[(uint32_t)ghz_collide_b_index[b] * STRIDE];
	return &g_ghz_collide_rows[(uint32_t)g_ghz_collide_index[b] * STRIDE];
}

/* ---- position finders, one per collision mode ---------------------------- */

/* One layer's worth of find_floor's probe: the original 3-cell scan,
 * parameterized on which layout to read (map) and threaded through startY by
 * pointer, so find_floor below can call this once per layer and have
 * acceptance state (s->collided, *startY) carry across the two calls exactly
 * like RSDK's FindFloorPosition carries it across its layer loop
 * (Collision.cpp:2177-2229: outer layer loop at 2179, startY updated on
 * accept at 2213, i=3 early-out only breaks the inner cell loop -- the early
 * "return" below is this port's equivalent, since the inner loop is now this
 * whole function). A real function, called twice, rather than a 2-iteration
 * loop over {g_ghz_map, ghz_map_fgh}: on this SH2 target a small fixed-trip-
 * count loop wrapping real work is exactly what the compiler tends to
 * duplicate in the name of speed, and sh_src/mars.ld's shrunk rom region has
 * no room to spare for that. */
static void find_floor_layer(Sensor *s, const uint16_t *map, int32_t plane,
                              int32_t posX, int32_t posY, int32_t *startY)
{
	int32_t cy = (posY & -CELL_SIZE) - CELL_SIZE;
	int32_t i;

	for (i = 0; i < 3; i++, cy += CELL_SIZE) {
		uint16_t cell = cell_at(map, posX, cy);
		const uint8_t *row;
		uint8_t mask;
		int32_t ty, tileAngle, adiff;

		if (!(cell & SOLID_FLOOR(plane))) continue;
		row = collide_row(plane, cell);
		mask = row[OFF_FLOOR + (posX & 0xF)];
		if (mask == 0xFF) continue;

		ty = cy + mask;
		if (!s->collided || *startY >= ty) {
			if (iabs(posY - ty) > collisionTolerance) continue;

			tileAngle = row[OFF_ANGLE + 0];
			adiff = (int32_t)s->angle - tileAngle;
			/* wraparound terms catch a near-0 tile angle read against a
			 * sensor angle just past the 0/255 seam, and vice versa */
			if (iabs(adiff) <= FLOOR_ANGLE_TOLERANCE
			    || iabs(adiff + 0x100) <= FLOOR_ANGLE_TOLERANCE
			    || iabs(adiff - 0x100) <= FLOOR_ANGLE_TOLERANCE) {
				s->y = TO_FIXED(ty);
				s->angle = (uint8_t)tileAngle;
				s->collided = 1;
				*startY = ty;
				return;
			}
		}
	}
}

static void find_floor(Sensor *s, int32_t plane)
{
	int32_t posX = FROM_FIXED(s->x), posY = FROM_FIXED(s->y);
	int32_t startY = posY;  /* only gates find_floor_layer's !collided ||
	                          * *startY>=ty preference test, but carried
	                          * across BOTH layers below -- so FG High only
	                          * wins over an already-accepted FG Low floor
	                          * when it is at least as preferred, not merely
	                          * because it is probed second. */

	/* FG Low then FG High: both are registered collision layers in the
	 * original (SonicMania/Objects/Global/Zone.c:212's collisionLayers), in
	 * this order (Zone->fgLayer[0] is FG Low, fgLayer[1] is FG High). Both
	 * calls read the same plane -- collisionMasks[cPlane] is orthogonal to
	 * the layer loop in the original, see this file's top comment. */
	find_floor_layer(s, g_ghz_map, plane, posX, posY, &startY);
	find_floor_layer(s, ghz_map_fgh, plane, posX, posY, &startY);
}

/* One layer's worth of find_roof's probe; see find_floor_layer's comment for
 * why this is a real function called twice rather than a loop. */
static void find_roof_layer(Sensor *s, const uint16_t *map, int32_t plane,
                             int32_t posX, int32_t posY, int32_t *startY)
{
	int32_t cy = (posY & -CELL_SIZE) + CELL_SIZE;
	int32_t i;

	for (i = 0; i < 3; i++, cy -= CELL_SIZE) {
		uint16_t cell = cell_at(map, posX, cy);
		const uint8_t *row;
		uint8_t mask;
		int32_t ty, tileAngle;

		if (!(cell & SOLID_SIDES(plane))) continue;
		row = collide_row(plane, cell);
		mask = row[OFF_ROOF + (posX & 0xF)];
		if (mask == 0xFF) continue;

		ty = cy + mask;
		tileAngle = row[OFF_ANGLE + 3];
		if ((!s->collided || *startY <= ty)
		    && iabs(posY - ty) <= collisionTolerance
		    && iabs((int32_t)s->angle - tileAngle) <= ROOF_ANGLE_TOLERANCE) {
			s->y = TO_FIXED(ty);
			s->angle = (uint8_t)tileAngle;
			s->collided = 1;
			*startY = ty;
			return;
		}
	}
}

static void find_roof(Sensor *s, int32_t plane)
{
	int32_t posX = FROM_FIXED(s->x), posY = FROM_FIXED(s->y);
	int32_t startY = posY;  /* carried across both layers, see find_floor */

	find_roof_layer(s, g_ghz_map, plane, posX, posY, &startY);
	find_roof_layer(s, ghz_map_fgh, plane, posX, posY, &startY);
}

/* One layer's worth of find_lwall's probe; see find_floor_layer's comment
 * for why this is a real function called twice rather than a loop. */
static void find_lwall_layer(Sensor *s, const uint16_t *map, int32_t plane,
                              int32_t posX, int32_t posY, int32_t *startX)
{
	int32_t cx = (posX & -CELL_SIZE) - CELL_SIZE;
	int32_t i;

	for (i = 0; i < 3; i++, cx += CELL_SIZE) {
		uint16_t cell = cell_at(map, cx, posY);
		const uint8_t *row;
		uint8_t mask;
		int32_t tx, tileAngle;

		/* unlike the floor/roof finders, either solid bit blocks a wall,
		 * matching Collision.cpp:2236 */
		if (!(cell & (SOLID_FLOOR(plane) | SOLID_SIDES(plane)))) continue;
		row = collide_row(plane, cell);
		mask = row[OFF_LWALL + (posY & 0xF)];
		if (mask == 0xFF) continue;

		tx = cx + mask;
		tileAngle = row[OFF_ANGLE + 1];
		if ((!s->collided || *startX >= tx)
		    && iabs(posX - tx) <= collisionTolerance
		    && iabs((int32_t)s->angle - tileAngle) <= WALL_ANGLE_TOLERANCE) {
			s->x = TO_FIXED(tx);
			s->angle = (uint8_t)tileAngle;
			s->collided = 1;
			*startX = tx;
			return;
		}
	}
}

static void find_lwall(Sensor *s, int32_t plane)
{
	int32_t posX = FROM_FIXED(s->x), posY = FROM_FIXED(s->y);
	int32_t startX = posX;  /* carried across both layers, see find_floor */

	find_lwall_layer(s, g_ghz_map, plane, posX, posY, &startX);
	find_lwall_layer(s, ghz_map_fgh, plane, posX, posY, &startX);
}

/* One layer's worth of find_rwall's probe; see find_floor_layer's comment
 * for why this is a real function called twice rather than a loop. */
static void find_rwall_layer(Sensor *s, const uint16_t *map, int32_t plane,
                              int32_t posX, int32_t posY, int32_t *startX)
{
	int32_t cx = (posX & -CELL_SIZE) + CELL_SIZE;
	int32_t i;

	for (i = 0; i < 3; i++, cx -= CELL_SIZE) {
		uint16_t cell = cell_at(map, cx, posY);
		const uint8_t *row;
		uint8_t mask;
		int32_t tx, tileAngle;

		if (!(cell & (SOLID_FLOOR(plane) | SOLID_SIDES(plane)))) continue;
		row = collide_row(plane, cell);
		mask = row[OFF_RWALL + (posY & 0xF)];
		if (mask == 0xFF) continue;

		tx = cx + mask;
		tileAngle = row[OFF_ANGLE + 2];
		if ((!s->collided || *startX <= tx)
		    && iabs(posX - tx) <= collisionTolerance
		    && iabs((int32_t)s->angle - tileAngle) <= WALL_ANGLE_TOLERANCE) {
			s->x = TO_FIXED(tx);
			s->angle = (uint8_t)tileAngle;
			s->collided = 1;
			*startX = tx;
			return;
		}
	}
}

static void find_rwall(Sensor *s, int32_t plane)
{
	int32_t posX = FROM_FIXED(s->x), posY = FROM_FIXED(s->y);
	int32_t startX = posX;  /* carried across both layers, see find_floor */

	find_rwall_layer(s, g_ghz_map, plane, posX, posY, &startX);
	find_rwall_layer(s, ghz_map_fgh, plane, posX, posY, &startX);
}

/* ---- air-collision finders -------------------------------------------
 *
 * ProcessAirCollision_Down doesn't reuse the four finders above; it calls
 * RSDK's FloorCollision/RoofCollision/LWallCollision/RWallCollision, a
 * separate, simpler scan:
 *   - the wall checks test SOLID_SIDES only, not SOLID_FLOOR|SOLID_SIDES,
 *     and drop the angle-tolerance check entirely - a push-out sensor only
 *     needs to know something solid is there, not whether it's a plausible
 *     continuation of the current slope;
 *   - the floor/roof checks use the fixed collisionMinimumDistance (14px,
 *     not collisionTolerance) and only look at the two cells nearest the
 *     sensor, taking the first solid one rather than comparing all three.
 *
 * These four scan both foreground layers too, same layer loop as the
 * position finders above (Collision.cpp:2406, 2479, 2532, 2605, all reading
 * "for (int32 l = 0, layerID = 1; l < LAYER_COUNT; ++l, layerID <<= 1) {"
 * then "if (collisionEntity->collisionLayers & layerID) {"), but what
 * carries across layers is not startY/startX:
 *   - FloorCollision/RoofCollision carry a running best -- collidePos/
 *     collideAngle, declared before the layer loop (2402-2403, 2528-2529)
 *     and applied to the sensor once, after both layers, in the block below
 *     the loop (2461-2469, 2590-2595). The accept test inside the loop is
 *     not "is this candidate better than the last", it is "is the sensor
 *     still on the near side of the last-recorded one": `colY < collidePos`
 *     at 2434 (floor), `colY > collidePos` at 2563 (roof). colY is just
 *     posY here (layer->position is always 0 in this port, so RSDK's
 *     colY = posY - layer->position.y never differs from posY, same as
 *     every finder above). Ported below as floor_collision_layer/
 *     roof_collision_layer taking collidePos/collideAngle by pointer.
 *   - LWallCollision/RWallCollision (2472-2511, 2598-2637) carry nothing at
 *     all: they write straight to sensor->collided/angle/position.x with no
 *     comparison, so a later layer's hit unconditionally overwrites an
 *     earlier one's. lwall_collision_layer/rwall_collision_layer below
 *     reproduce that directly -- no shared state passed between the two
 *     calls, whichever finds a hit last wins, exactly like the original. */

/* One layer's worth of floor_collision's probe. collidePos/collideAngle are
 * the running best, carried across both calls by pointer exactly like
 * RSDK's collidePos/collideAngle are carried across its layer loop
 * (Collision.cpp:2402-2403); accepting here means "the sensor is still above
 * the last-recorded floor" (posY < *collidePos, transcribed from colY <
 * collidePos at 2434), not "this candidate beats the last one". No Sensor
 * parameter: unlike the position finders and the wall air-finders below,
 * RSDK's FloorCollision never touches sensor-> inside the layer loop either
 * -- only the running best, applied to the sensor once after both layers
 * (see floor_collision). */
static void floor_collision_layer(const uint16_t *map, int32_t plane,
                                   int32_t posX, int32_t posY,
                                   int32_t *collidePos, int32_t *collideAngle)
{
	int32_t cy = (posY & -CELL_SIZE) - CELL_SIZE;
	int32_t i;

	for (i = 0; i < 2; i++, cy += CELL_SIZE) {
		uint16_t cell = cell_at(map, posX, cy);
		const uint8_t *row;
		uint8_t mask;

		if (!(cell & SOLID_FLOOR(plane))) continue;
		row = collide_row(plane, cell);
		mask = row[OFF_FLOOR + (posX & 0xF)];
		if (mask == 0xFF) continue;

		if (posY < *collidePos) {
			*collideAngle = row[OFF_ANGLE + 0];
			*collidePos = cy + mask;
			return;
		}
	}
}

static void floor_collision(Sensor *s, int32_t plane)
{
	int32_t posX = FROM_FIXED(s->x), posY = FROM_FIXED(s->y);
	int32_t collidePos = 0x7FFFFFFF;
	int32_t collideAngle = 0;

	floor_collision_layer(g_ghz_map, plane, posX, posY, &collidePos, &collideAngle);
	floor_collision_layer(ghz_map_fgh, plane, posX, posY, &collidePos, &collideAngle);

	if (collidePos != 0x7FFFFFFF) {
		int32_t collideDist = s->y - TO_FIXED(collidePos);
		if (s->y >= TO_FIXED(collidePos) && collideDist <= TO_FIXED(14)) {
			s->angle = (uint8_t)collideAngle;
			s->y = TO_FIXED(collidePos);
			s->collided = 1;
		}
	}
}

/* One layer's worth of roof_collision's probe; mirror of
 * floor_collision_layer (see its comment for why there is no Sensor
 * parameter) with the comparison flipped (posY > *collidePos, transcribed
 * from colY > collidePos at Collision.cpp:2563) and the running best
 * starting at -1 instead of 0x7FFFFFFF, matching RSDK's own collidePos
 * inits (2403 vs 2529). */
static void roof_collision_layer(const uint16_t *map, int32_t plane,
                                  int32_t posX, int32_t posY,
                                  int32_t *collidePos, int32_t *collideAngle)
{
	int32_t cy = (posY & -CELL_SIZE) + CELL_SIZE;
	int32_t i;

	for (i = 0; i < 2; i++, cy -= CELL_SIZE) {
		uint16_t cell = cell_at(map, posX, cy);
		const uint8_t *row;
		uint8_t mask;

		if (!(cell & SOLID_SIDES(plane))) continue;
		row = collide_row(plane, cell);
		mask = row[OFF_ROOF + (posX & 0xF)];
		if (mask == 0xFF) continue;

		if (posY > *collidePos) {
			*collideAngle = row[OFF_ANGLE + 3];
			*collidePos = cy + mask;
			return;
		}
	}
}

static void roof_collision(Sensor *s, int32_t plane)
{
	int32_t posX = FROM_FIXED(s->x), posY = FROM_FIXED(s->y);
	int32_t collidePos = -1;
	int32_t collideAngle = 0;

	roof_collision_layer(g_ghz_map, plane, posX, posY, &collidePos, &collideAngle);
	roof_collision_layer(ghz_map_fgh, plane, posX, posY, &collidePos, &collideAngle);

	if (collidePos >= 0 && s->y <= TO_FIXED(collidePos)
	    && s->y - TO_FIXED(collidePos) >= -TO_FIXED(14)) {
		s->angle = (uint8_t)collideAngle;
		s->y = TO_FIXED(collidePos);
		s->collided = 1;
	}
}

/* One layer's worth of lwall_collision's probe. No shared state with the
 * other call (see the block comment above): whichever call finds a hit last
 * simply overwrites s, same as RSDK's LWallCollision. Tests SOLID_SIDES
 * alone (LWallCollision, Collision.cpp:2477), not the FLOOR|SIDES pair the
 * ground find_lwall above tests -- see the SOLID_FLOOR/SOLID_SIDES macro
 * comment. */
static void lwall_collision_layer(Sensor *s, const uint16_t *map, int32_t plane,
                                   int32_t posX, int32_t posY)
{
	int32_t cx = (posX & -CELL_SIZE) - CELL_SIZE;
	int32_t i;

	for (i = 0; i < 3; i++, cx += CELL_SIZE) {
		uint16_t cell = cell_at(map, cx, posY);
		const uint8_t *row;
		uint8_t mask;
		int32_t tx;

		if (!(cell & SOLID_SIDES(plane))) continue;
		row = collide_row(plane, cell);
		mask = row[OFF_LWALL + (posY & 0xF)];
		if (mask == 0xFF) continue;

		tx = cx + mask;
		if (posX >= tx && iabs(posX - tx) <= 14) {
			s->x = TO_FIXED(tx);
			s->angle = row[OFF_ANGLE + 1];
			s->collided = 1;
			return;
		}
	}
}

static void lwall_collision(Sensor *s, int32_t plane)
{
	int32_t posX = FROM_FIXED(s->x), posY = FROM_FIXED(s->y);

	lwall_collision_layer(s, g_ghz_map, plane, posX, posY);
	lwall_collision_layer(s, ghz_map_fgh, plane, posX, posY);
}

/* One layer's worth of rwall_collision's probe; see lwall_collision_layer's
 * comment (SOLID_SIDES alone, RWallCollision Collision.cpp:2603). */
static void rwall_collision_layer(Sensor *s, const uint16_t *map, int32_t plane,
                                   int32_t posX, int32_t posY)
{
	int32_t cx = (posX & -CELL_SIZE) + CELL_SIZE;
	int32_t i;

	for (i = 0; i < 3; i++, cx -= CELL_SIZE) {
		uint16_t cell = cell_at(map, cx, posY);
		const uint8_t *row;
		uint8_t mask;
		int32_t tx;

		if (!(cell & SOLID_SIDES(plane))) continue;
		row = collide_row(plane, cell);
		mask = row[OFF_RWALL + (posY & 0xF)];
		if (mask == 0xFF) continue;

		tx = cx + mask;
		if (posX <= tx && iabs(posX - tx) <= 14) {
			s->x = TO_FIXED(tx);
			s->angle = row[OFF_ANGLE + 2];
			s->collided = 1;
			return;
		}
	}
}

static void rwall_collision(Sensor *s, int32_t plane)
{
	int32_t posX = FROM_FIXED(s->x), posY = FROM_FIXED(s->y);

	rwall_collision_layer(s, g_ghz_map, plane, posX, posY);
	rwall_collision_layer(s, ghz_map_fgh, plane, posX, posY);
}

/* ---- SetPathGripSensors -------------------------------------------------- */

/* useOffset: SetPathGripSensors's useCollisionOffset (Collision.cpp:949-961),
 * computed once by path_grip's caller and passed down rather than read from
 * a file-scope flag -- see path_grip's own comment for the exact formula. */
static void set_sensors(PathEntity *e, Sensor *s, int32_t useOffset)
{
	int32_t offset = useOffset ? COLLISION_OFFSET : 0;

	switch (e->collisionMode) {
	default:
	case CMODE_FLOOR:
		s[0].y = s[1].y = s[2].y = s[4].y + TO_FIXED(e->outer.bottom);
		s[3].y = s[4].y + offset;
		s[0].x = s[4].x + TO_FIXED(e->inner.left) - TO_FIXED(1);
		s[1].x = s[4].x;
		s[2].x = s[4].x + TO_FIXED(e->inner.right);
		s[3].x = (e->groundVel <= 0)
		       ? s[4].x + TO_FIXED(e->outer.left) - TO_FIXED(1)
		       : s[4].x + TO_FIXED(e->outer.right);
		break;

	case CMODE_LWALL:
		s[0].x = s[1].x = s[2].x = s[4].x + TO_FIXED(e->outer.bottom);
		s[3].x = s[4].x;
		s[0].y = s[4].y + TO_FIXED(e->inner.left) - TO_FIXED(1);
		s[1].y = s[4].y;
		s[2].y = s[4].y + TO_FIXED(e->inner.right);
		s[3].y = (e->groundVel <= 0)
		       ? s[4].y - TO_FIXED(e->outer.left)
		       : s[4].y - TO_FIXED(e->outer.right) - TO_FIXED(1);
		break;

	case CMODE_ROOF:
		s[0].y = s[1].y = s[2].y =
			s[4].y - TO_FIXED(e->outer.bottom) - TO_FIXED(1);
		s[3].y = s[4].y - offset;
		s[0].x = s[4].x + TO_FIXED(e->inner.left) - TO_FIXED(1);
		s[1].x = s[4].x;
		s[2].x = s[4].x + TO_FIXED(e->inner.right);
		s[3].x = (e->groundVel <= 0)
		       ? s[4].x - TO_FIXED(e->outer.left)
		       : s[4].x - TO_FIXED(e->outer.right) - TO_FIXED(1);
		break;

	case CMODE_RWALL:
		s[0].x = s[1].x = s[2].x =
			s[4].x - TO_FIXED(e->outer.bottom) - TO_FIXED(1);
		s[3].x = s[4].x;
		s[0].y = s[4].y + TO_FIXED(e->inner.left) - TO_FIXED(1);
		s[1].y = s[4].y;
		s[2].y = s[4].y + TO_FIXED(e->inner.right);
		s[3].y = (e->groundVel <= 0)
		       ? s[4].y - TO_FIXED(e->outer.left)
		       : s[4].y - TO_FIXED(e->outer.right) - TO_FIXED(1);
		break;
	}
}

/* ---- move epilogue --------------------------------------------------------
 *
 * ProcessObjectMovement runs this unconditionally after ProcessPathGrip or
 * ProcessAirCollision_Down, keyed on the *final* onGround state, regardless
 * of what the move itself already did internally - grounded entities always
 * get their world velocity re-derived from groundVel+angle, airborne ones
 * always get groundVel re-synced from velocity.x. Both path_grip and path_air
 * need it, so it lives here once rather than duplicated in each. */
static void sync_velocity(PathEntity *e)
{
	if (e->onGround) {
		e->velX = (e->groundVel * cos256(e->angle)) >> 8;
		e->velY = (e->groundVel * sin256(e->angle)) >> 8;
	} else {
		e->groundVel = e->velX;
	}
}

/* ---- ProcessPathGrip's post-loop application ------------------------------
 *
 * Collision.cpp:1901-2086: runs exactly once, after the scan loop below
 * exits, over whichever sensors last ran -- a second, separate switch on
 * collisionMode from the one the loop itself uses. newCollisionMode/newAngle
 * (RETRO_REV0U's "tileCollisions == TILECOLLISION_DOWN ? CMODE_FLOOR :
 * CMODE_ROOF" and that shifted to a starting angle) collapse to plain
 * CMODE_FLOOR/0 here: this port never runs upside-down gravity, same as
 * find_floor_layer's group comment already notes for TILECOLLISION_UP.
 *
 * Each case's "leaving the ground" branch (the else of "sensors[0..2] didn't
 * collide") independently recomputes velocity from the *pre-transition*
 * angle/groundVel and re-derives groundVel from it -- sync_velocity() below
 * then harmlessly repeats that same groundVel=velX assignment once more
 * (its onGround-false half), and for the branches that stay grounded,
 * sync_velocity's onGround-true half is what actually derives velX/velY from
 * the angle this function just settled on (LWALL/RWALL's grounded branch
 * touches only e->angle, same as the original touching only
 * collisionEntity->angle -- velocity is entirely sync_velocity's job there). */
static void apply_grip_result(PathEntity *e, Sensor *s)
{
	switch (e->collisionMode) {
	default:
	case CMODE_FLOOR:
		if (s[0].collided || s[1].collided || s[2].collided) {
			e->angle = s[0].angle;
			if (!s[3].collided) {
				e->x = s[4].x;
			} else {
				if (e->groundVel > 0) e->x = s[3].x - TO_FIXED(e->outer.right);
				if (e->groundVel < 0) e->x = s[3].x - TO_FIXED(e->outer.left) + TO_FIXED(1);
				e->groundVel = 0;
				e->velX = 0;
			}
			e->y = s[4].y;
		} else {
			e->onGround = 0;
			e->collisionMode = CMODE_FLOOR;
			e->velX = (cos256(e->angle) * e->groundVel) >> 8;
			e->velY = (sin256(e->angle) * e->groundVel) >> 8;
			if (e->velY < -TO_FIXED(16)) e->velY = -TO_FIXED(16);
			if (e->velY > TO_FIXED(16)) e->velY = TO_FIXED(16);
			e->groundVel = e->velX;
			e->angle = 0;
			if (!s[3].collided) {
				e->x += e->velX;
			} else {
				if (e->groundVel > 0) e->x = s[3].x - TO_FIXED(e->outer.right);
				if (e->groundVel < 0) e->x = s[3].x - TO_FIXED(e->outer.left) + TO_FIXED(1);
				e->groundVel = 0;
				e->velX = 0;
			}
			e->y += e->velY;
		}
		break;

	case CMODE_LWALL:
		if (s[0].collided || s[1].collided || s[2].collided) {
			e->angle = s[0].angle;
		} else {
			e->onGround = 0;
			e->collisionMode = CMODE_FLOOR;
			e->velX = (cos256(e->angle) * e->groundVel) >> 8;
			e->velY = (sin256(e->angle) * e->groundVel) >> 8;
			if (e->velY < -TO_FIXED(16)) e->velY = -TO_FIXED(16);
			if (e->velY > TO_FIXED(16)) e->velY = TO_FIXED(16);
			e->groundVel = e->velX;
			e->angle = 0;
		}
		if (!s[3].collided) {
			e->x = s[4].x;
			e->y = s[4].y;
		} else {
			if (e->groundVel > 0) e->y = s[3].y + TO_FIXED(e->outer.right) + TO_FIXED(1);
			if (e->groundVel < 0) e->y = s[3].y - TO_FIXED(e->outer.left);
			e->groundVel = 0;
			e->x = s[4].x;
		}
		break;

	case CMODE_ROOF:
		if (s[0].collided || s[1].collided || s[2].collided) {
			e->angle = s[0].angle;
			if (!s[3].collided) {
				e->x = s[4].x;
			} else {
				if (e->groundVel > 0) e->x = s[3].x + TO_FIXED(e->outer.right);
				if (e->groundVel < 0) e->x = s[3].x + TO_FIXED(e->outer.left) - TO_FIXED(1);
				e->groundVel = 0;
			}
		} else {
			e->onGround = 0;
			e->collisionMode = CMODE_FLOOR;
			e->velX = (cos256(e->angle) * e->groundVel) >> 8;
			e->velY = (sin256(e->angle) * e->groundVel) >> 8;
			if (e->velY < -TO_FIXED(16)) e->velY = -TO_FIXED(16);
			if (e->velY > TO_FIXED(16)) e->velY = TO_FIXED(16);
			e->angle = 0;
			e->groundVel = e->velX;
			if (!s[3].collided) {
				e->x += e->velX;
			} else {
				if (e->groundVel > 0) e->x = s[3].x - TO_FIXED(e->outer.right);
				if (e->groundVel < 0) e->x = s[3].x - TO_FIXED(e->outer.left) + TO_FIXED(1);
				e->groundVel = 0;
			}
		}
		e->y = s[4].y;
		break;

	case CMODE_RWALL:
		if (s[0].collided || s[1].collided || s[2].collided) {
			e->angle = s[0].angle;
		} else {
			e->onGround = 0;
			e->collisionMode = CMODE_FLOOR;
			e->velX = (cos256(e->angle) * e->groundVel) >> 8;
			e->velY = (sin256(e->angle) * e->groundVel) >> 8;
			if (e->velY < -TO_FIXED(16)) e->velY = -TO_FIXED(16);
			if (e->velY > TO_FIXED(16)) e->velY = TO_FIXED(16);
			e->groundVel = e->velX;
			e->angle = 0;
		}
		if (!s[3].collided) {
			e->x = s[4].x;
			e->y = s[4].y;
		} else {
			if (e->groundVel > 0) e->y = s[3].y - TO_FIXED(e->outer.right);
			if (e->groundVel < 0) e->y = s[3].y - TO_FIXED(e->outer.left) + TO_FIXED(1);
			e->groundVel = 0;
			e->x = s[4].x;
		}
		break;
	}

	sync_velocity(e);
}

/* ---- ProcessPathGrip ----------------------------------------------------- */

void path_grip(PathEntity *e)
{
	Sensor s[6];
	int32_t xVel = 0, yVel = 0;
	int32_t absSpeed = (e->groundVel < 0) ? -e->groundVel : e->groundVel;
	int32_t checkDist = absSpeed >> 18;
	int32_t i, best;
	int32_t useOffset;
	uint8_t wallHit;

	collisionTolerance = HIGH_COLLISION_TOLERANCE;
	if (iabs(e->groundVel) < TO_FIXED(6) && e->angle == 0)
		collisionTolerance = LOW_COLLISION_TOLERANCE;

	/* useCollisionOffset (Collision.cpp:949-961): tileCollisions is always
	 * TILECOLLISION_DOWN here, so its ternary is always the angle==0 arm;
	 * ANDed with the !RETRO_USE_ORIGINAL_CODE chibi-hitbox fix (959-961)
	 * folded in directly since it is unconditional in this build. Computed
	 * once from the angle/box this call started with, not re-evaluated per
	 * sensor probe -- set_sensors is called again below as the loop steps,
	 * but always with this same useOffset. */
	useOffset = (e->angle == 0 && e->outer.bottom >= 14);

	absSpeed &= 0x3FFFF;

	s[4].x = e->x;
	s[4].y = e->y;
	for (i = 0; i < 6; i++) {
		s[i].angle = e->angle;
		s[i].collided = 0;
	}
	set_sensors(e, s, useOffset);

	while (checkDist > -1) {
		if (checkDist >= 1) {
			xVel = (int32_t)cos256(e->angle) << 10;
			yVel = (int32_t)sin256(e->angle) << 10;
			checkDist--;
		} else {
			xVel = (absSpeed * cos256(e->angle)) >> 8;
			yVel = (absSpeed * sin256(e->angle)) >> 8;
			checkDist = -1;
		}
		if (e->groundVel < 0) {
			xVel = -xVel;
			yVel = -yVel;
		}

		s[0].collided = s[1].collided = s[2].collided = 0;
		s[4].x += xVel;
		s[4].y += yVel;

		s[3].x += xVel;
		s[3].y += yVel;

		/* sensor-3 wall probe: the angle-free family (Collision.cpp:1644/
		 * 1652 FLOOR, 1714/1717 LWALL, 1772/1780 ROOF, 1839/1842 RWALL),
		 * already ported as path_air's own wall sensors -- a wall-stop probe
		 * only needs to know something solid is there, not whether it is a
		 * plausible continuation of the current slope. REV0U nudges
		 * (1645-1648, 1653-1656 FLOOR; 1773-1776, 1781-1784 ROOF) reposition
		 * the sensor that would otherwise re-probe past the wall next
		 * iteration; LWALL/RWALL get no such nudge in the original (their
		 * case blocks, 1709-1765 and 1834-1889, have no #if RETRO_REV0U at
		 * all), so none is added here either. */
		switch (e->collisionMode) {
		default:
		case CMODE_FLOOR:
			if (e->groundVel > 0) {
				lwall_collision(&s[3], e->collisionPlane);
				if (s[3].collided) s[2].x = s[3].x - TO_FIXED(2);
			}
			if (e->groundVel < 0) {
				rwall_collision(&s[3], e->collisionPlane);
				if (s[3].collided) s[0].x = s[3].x + TO_FIXED(2);
			}
			break;
		case CMODE_LWALL:
			if (e->groundVel > 0) roof_collision(&s[3], e->collisionPlane);
			if (e->groundVel < 0) floor_collision(&s[3], e->collisionPlane);
			break;
		case CMODE_ROOF:
			if (e->groundVel > 0) {
				rwall_collision(&s[3], e->collisionPlane);
				if (s[3].collided) s[2].x = s[3].x + TO_FIXED(2);
			}
			if (e->groundVel < 0) {
				lwall_collision(&s[3], e->collisionPlane);
				if (s[3].collided) s[0].x = s[3].x - TO_FIXED(2);
			}
			break;
		case CMODE_RWALL:
			if (e->groundVel > 0) floor_collision(&s[3], e->collisionPlane);
			if (e->groundVel < 0) roof_collision(&s[3], e->collisionPlane);
			break;
		}

		/* Per-mode wall-hit zero (Collision.cpp:1659-1662/1787-1790 zero
		 * xVel for FLOOR/ROOF; 1719-1722/1844-1847 zero yVel for LWALL/
		 * RWALL): neither groundVel nor the other axis is touched here --
		 * that only happens once, after the loop, from sensor 3
		 * (apply_grip_result above). */
		wallHit = s[3].collided;
		if (wallHit) {
			checkDist = -1;
			if (e->collisionMode == CMODE_FLOOR || e->collisionMode == CMODE_ROOF)
				xVel = 0;
			else
				yVel = 0;
		}

		for (i = 0; i < 3; i++) {
			s[i].x += xVel;
			s[i].y += yVel;
			switch (e->collisionMode) {
			default:
			case CMODE_FLOOR: find_floor(&s[i], e->collisionPlane); break;
			case CMODE_LWALL: find_lwall(&s[i], e->collisionPlane); break;
			case CMODE_ROOF:  find_roof(&s[i], e->collisionPlane);  break;
			case CMODE_RWALL: find_rwall(&s[i], e->collisionPlane); break;
			}
		}

		/* pick the sensor that found the highest ground for this mode.
		 * LWALL/RWALL (Collision.cpp:1733/1858) compare the opposite way
		 * from what their names suggest: LWALL (walking up a left-hand
		 * wall) prefers the SMALLER x, RWALL the LARGER. FLOOR alone also
		 * carries an equal-height tie-break toward the flatter angle
		 * (1677, wraps through the 0/255 seam). */
		best = -1;
		for (i = 0; i < 3; i++) {
			if (!s[i].collided) continue;
			if (best < 0) { best = i; continue; }
			switch (e->collisionMode) {
			default:
			case CMODE_FLOOR:
				if (s[i].y < s[best].y) best = i;
				if (s[i].y == s[best].y && (s[i].angle < 0x08 || s[i].angle > 0xF8))
					best = i;
				break;
			case CMODE_LWALL: if (s[i].x < s[best].x) best = i; break;
			case CMODE_ROOF:  if (s[i].y > s[best].y) best = i; break;
			case CMODE_RWALL: if (s[i].x > s[best].x) best = i; break;
			}
		}

		/* The body position comes back from the sensors, not from its own
		 * advance. When the leading sensor hit a wall the step was zeroed, so
		 * the floor sensors still hold the pre-collision position and this is
		 * what rolls the body back out of the wall. Collision.cpp keeps the
		 * mode-transition thresholds inside this same per-mode case, after
		 * the tileDistance<=-1 check but not gated by it (1701-1705 etc): a
		 * pass that finds nothing still re-tests whatever angle sensor 0 held
		 * from the last pass that did, which is harmless since every mode's
		 * own "nothing collided" epilogue (apply_grip_result) only cares
		 * about the collided flags, never about collisionMode itself. */
		switch (e->collisionMode) {
		default:
		case CMODE_FLOOR:
			if (best < 0) {
				checkDist = -1;
			} else {
				s[0].y = s[1].y = s[2].y = s[best].y;
				s[0].angle = s[1].angle = s[2].angle = s[best].angle;
				s[4].x = s[1].x;
				s[4].y = s[0].y - TO_FIXED(e->outer.bottom);
			}
			if (s[0].angle < 0xDE && s[0].angle > 0x80) e->collisionMode = CMODE_LWALL;
			if (s[0].angle > 0x22 && s[0].angle < 0x80) e->collisionMode = CMODE_RWALL;
			break;
		case CMODE_LWALL:
			if (best < 0) {
				checkDist = -1;
			} else {
				s[0].x = s[1].x = s[2].x = s[best].x;
				s[0].angle = s[1].angle = s[2].angle = s[best].angle;
				s[4].y = s[1].y;
				s[4].x = s[0].x - TO_FIXED(e->outer.bottom);
			}
			if (s[0].angle > 0xE2) e->collisionMode = CMODE_FLOOR;
			if (s[0].angle < 0x9E) e->collisionMode = CMODE_ROOF;
			break;
		case CMODE_ROOF:
			if (best < 0) {
				checkDist = -1;
			} else {
				s[0].y = s[1].y = s[2].y = s[best].y;
				s[0].angle = s[1].angle = s[2].angle = s[best].angle;
				s[4].x = s[1].x;
				s[4].y = s[0].y + TO_FIXED(e->outer.bottom) + TO_FIXED(1);
			}
			if (s[0].angle > 0xA2) e->collisionMode = CMODE_LWALL;
			if (s[0].angle < 0x5E) e->collisionMode = CMODE_RWALL;
			break;
		case CMODE_RWALL:
			if (best < 0) {
				checkDist = -1;
			} else {
				s[0].x = s[1].x = s[2].x = s[best].x;
				s[0].angle = s[1].angle = s[2].angle = s[best].angle;
				s[4].y = s[1].y;
				s[4].x = s[0].x + TO_FIXED(e->outer.bottom) + TO_FIXED(1);
			}
			if (s[0].angle < 0x1E) e->collisionMode = CMODE_FLOOR;
			if (s[0].angle > 0x62) e->collisionMode = CMODE_ROOF;
			break;
		}

		if (best >= 0) e->angle = s[0].angle;

		if (!wallHit)
			set_sensors(e, s, useOffset);
		else
			checkDist = -2;
	}

	apply_grip_result(e, s);
}

/* ---- ProcessAirCollision_Down ---------------------------------------------
 *
 * useCollisionOffset is only ever true for a grounded, angle==0 entity, and
 * ProcessObjectMovement forces it false before dispatching here since this
 * path only runs while airborne - so the COLLISION_OFFSET nudge the reference
 * applies to sensors 0/1's Y is always zero here and has been dropped rather
 * than carried as dead code. */
void path_air(PathEntity *e)
{
	Sensor s[6];
	uint8_t movingDown = 0, movingUp = 0, movingLeft = 0, movingRight = 0;
	int32_t collisionMaskAir = e->outer.bottom >= 14 ? 19 : 17;
	int32_t absVelX = iabs(e->velX), absVelY = iabs(e->velY);
	int32_t cnt, velX, velY, velX2, velY2;
	int32_t i;

	collisionTolerance = HIGH_COLLISION_TOLERANCE;
	if (iabs(e->groundVel) < TO_FIXED(6) && e->angle == 0)
		collisionTolerance = LOW_COLLISION_TOLERANCE;

	if (e->velX >= 0) {
		movingRight = 1;
		s[0].x = e->x + TO_FIXED(e->outer.right);
		s[0].y = e->y;
	}
	if (e->velX <= 0) {
		movingLeft = 1;
		s[1].x = e->x + TO_FIXED(e->outer.left) - TO_FIXED(1);
		s[1].y = e->y;
	}

	s[2].x = e->x + TO_FIXED(e->outer.left) + TO_FIXED(1);
	s[3].x = e->x + TO_FIXED(e->outer.right) - TO_FIXED(2);
	s[4].x = s[2].x;
	s[5].x = s[3].x;

	for (i = 0; i < 6; i++) { s[i].collided = 0; s[i].angle = 0; }

	if (e->velY >= 0) {
		movingDown = 1;
		s[2].y = e->y + TO_FIXED(e->outer.bottom);
		s[3].y = e->y + TO_FIXED(e->outer.bottom);
	}
	if (absVelX > TO_FIXED(1) || e->velY < 0) {
		movingUp = 1;
		s[4].y = e->y + TO_FIXED(e->outer.top) - TO_FIXED(1);
		s[5].y = e->y + TO_FIXED(e->outer.top) - TO_FIXED(1);
	}

	cnt = (absVelX <= absVelY) ? ((absVelY >> collisionMaskAir) + 1)
	                           : ((absVelX >> collisionMaskAir) + 1);
	velX = e->velX / cnt;
	velY = e->velY / cnt;
	velX2 = e->velX - velX * (cnt - 1);
	velY2 = e->velY - velY * (cnt - 1);

	while (cnt > 0) {
		if (cnt < 2) {
			velX = velX2;
			velY = velY2;
		}
		cnt--;

		if (movingRight == 1) {
			s[0].x += velX;
			s[0].y += velY;
			lwall_collision(&s[0], e->collisionPlane);
			if (s[0].collided) movingRight = 2;
		}

		if (movingLeft == 1) {
			s[1].x += velX;
			s[1].y += velY;
			rwall_collision(&s[1], e->collisionPlane);
			if (s[1].collided) movingLeft = 2;
		}

		if (movingRight == 2) {
			e->velX = 0;
			e->groundVel = 0;
			e->x = s[0].x - TO_FIXED(e->outer.right);

			s[2].x = e->x + TO_FIXED(e->outer.left) + TO_FIXED(1);
			s[3].x = e->x + TO_FIXED(e->outer.right) - TO_FIXED(2);
			s[4].x = s[2].x;
			s[5].x = s[3].x;

			velX = 0;
			velX2 = 0;
			movingRight = 3;
		}

		if (movingLeft == 2) {
			e->velX = 0;
			e->groundVel = 0;
			e->x = s[1].x - TO_FIXED(e->outer.left) + TO_FIXED(1);

			s[2].x = e->x + TO_FIXED(e->outer.left) + TO_FIXED(1);
			s[3].x = e->x + TO_FIXED(e->outer.right) - TO_FIXED(2);
			s[4].x = s[2].x;
			s[5].x = s[3].x;

			velX = 0;
			velX2 = 0;
			movingLeft = 3;
		}

		if (movingDown == 1) {
			for (i = 2; i < 4; i++) {
				if (!s[i].collided) {
					s[i].x += velX;
					s[i].y += velY;
					floor_collision(&s[i], e->collisionPlane);
				}
			}
			if (s[2].collided || s[3].collided) {
				movingDown = 2;
				cnt = 0;
			}
		}

		if (movingUp == 1) {
			for (i = 4; i < 6; i++) {
				if (!s[i].collided) {
					s[i].x += velX;
					s[i].y += velY;
					roof_collision(&s[i], e->collisionPlane);
				}
			}
			if (s[4].collided || s[5].collided) {
				movingUp = 2;
				cnt = 0;
			}
		}
	}

	if (movingRight < 2 && movingLeft < 2)
		e->x += e->velX;

	if (movingUp < 2 && movingDown < 2) {
		e->y += e->velY;
		sync_velocity(e);
		return;
	}

	if (movingDown == 2) {
		e->onGround = 1;

		if (s[2].collided && s[3].collided) {
			if (s[2].y >= s[3].y) {
				e->y = s[3].y - TO_FIXED(e->outer.bottom);
				e->angle = s[3].angle;
			} else {
				e->y = s[2].y - TO_FIXED(e->outer.bottom);
				e->angle = s[2].angle;
			}
		} else if (s[2].collided) {
			e->y = s[2].y - TO_FIXED(e->outer.bottom);
			e->angle = s[2].angle;
		} else if (s[3].collided) {
			e->y = s[3].y - TO_FIXED(e->outer.bottom);
			e->angle = s[3].angle;
		}

		if (e->angle > 0xA0 && e->angle < 0xDE && e->collisionMode != CMODE_LWALL) {
			e->collisionMode = CMODE_LWALL;
			e->x -= TO_FIXED(4);
		}
		if (e->angle > 0x22 && e->angle < 0x60 && e->collisionMode != CMODE_RWALL) {
			e->collisionMode = CMODE_RWALL;
			e->x += TO_FIXED(4);
		}

		{
			int32_t speed = 0;

			if (e->angle < 0x80) {
				if (e->angle < 0x10)
					speed = e->velX;
				else if (e->angle >= 0x20)
					speed = (iabs(e->velX) <= iabs(e->velY)) ? e->velY : e->velX;
				else
					speed = (iabs(e->velX) <= iabs(e->velY >> 1)) ? (e->velY >> 1) : e->velX;
			} else if (e->angle > 0xF0) {
				speed = e->velX;
			} else if (e->angle <= 0xE0) {
				speed = (iabs(e->velX) <= iabs(e->velY)) ? -e->velY : e->velX;
			} else {
				speed = (iabs(e->velX) <= iabs(e->velY >> 1)) ? -(e->velY >> 1) : e->velX;
			}

			if (speed < -TO_FIXED(24)) speed = -TO_FIXED(24);
			if (speed > TO_FIXED(24)) speed = TO_FIXED(24);

			e->groundVel = speed;
			e->velX = speed;
			e->velY = 0;
		}
	}

	if (movingUp == 2) {
		int32_t sensorAngle = 0;

		if (s[4].collided && s[5].collided) {
			if (s[4].y <= s[5].y) {
				e->y = s[5].y - TO_FIXED(e->outer.top) + TO_FIXED(1);
				sensorAngle = s[5].angle;
			} else {
				e->y = s[4].y - TO_FIXED(e->outer.top) + TO_FIXED(1);
				sensorAngle = s[4].angle;
			}
		} else if (s[4].collided) {
			e->y = s[4].y - TO_FIXED(e->outer.top) + TO_FIXED(1);
			sensorAngle = s[4].angle;
		} else if (s[5].collided) {
			e->y = s[5].y - TO_FIXED(e->outer.top) + TO_FIXED(1);
			sensorAngle = s[5].angle;
		}
		sensorAngle &= 0xFF;

		/* a steep enough ceiling grounds you on its wall face instead of
		 * just stopping the upward motion */
		if (sensorAngle < 0x62) {
			if (e->velY < -iabs(e->velX)) {
				e->onGround = 1;
				e->angle = (uint8_t)sensorAngle;
				e->collisionMode = CMODE_RWALL;
				e->x += TO_FIXED(4);
				e->y -= TO_FIXED(2);
				e->groundVel = (e->angle <= 0x60) ? e->velY : (e->velY >> 1);
			}
		}

		if (sensorAngle > 0x9E && sensorAngle < 0xC1) {
			if (e->velY < -iabs(e->velX)) {
				e->onGround = 1;
				e->angle = (uint8_t)sensorAngle;
				e->collisionMode = CMODE_LWALL;
				e->x -= TO_FIXED(4);
				e->y -= TO_FIXED(2);
				e->groundVel = (e->angle >= 0xA0) ? -e->velY : -(e->velY >> 1);
			}
		}

		if (e->velY < 0) e->velY = 0;
	}

	sync_velocity(e);
}
