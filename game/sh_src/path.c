#include <stdint.h>
#include "path.h"
#include "trig.h"

/* Ported from RSDK Collision.cpp: FindFloorPosition, FindLWallPosition,
 * FindRoofPosition, FindRWallPosition, SetPathGripSensors, ProcessPathGrip
 * and ProcessAirCollision_Down, built as RETRO_REV0U=1, RETRO_USE_ORIGINAL_CODE=0.
 *
 * The stage data is a block map rather than RSDK's tile layer, but the shape
 * is the same: a 16x16 cell with a mask per column giving the surface, and
 * solidity carried on the map entry. Collision plane is always 0 and there is
 * only one layer, so the per-plane/per-layer loops in the reference collapse
 * to a single cell_at() lookup; TILECOLLISION_UP and its ProcessAirCollision_Up
 * counterpart are dropped since nothing here runs upside-down gravity.
 *
 * ghz_map and ghz_collide_index/ghz_collide_rows are linked into the 68000
 * program only; this SH2 side reaches them through runtime pointers assets.c
 * fills in from the descriptor table (see assets.h), not through a
 * linked-in extern array. */

extern const uint16_t *g_ghz_map;
/* collide_index[block] is a row number into collide_rows; convert_stage.py
 * dedups identical 70-byte rows (many blocks -- different tiles, or flip
 * variants whose masks happen to be symmetric -- share one), so this is one
 * extra indirection rather than a single b * STRIDE lookup into one flat
 * per-block array. */
extern const uint16_t *g_ghz_collide_index;
extern const uint8_t *g_ghz_collide_rows;
/* FG Low's size in blocks, published by the 68000 through the descriptor
 * table and filled in by assets_init() (sh_src/assets.c). Not a local
 * #define: see md_src/descriptor.h's GHZ_MAP_W comment for why keeping a
 * second copy of this number here is exactly the bug this is avoiding. */
extern uint16_t g_map_w, g_map_h;

#define CELL_SIZE 16   /* the collision grid, not the VDP tile */

#define BLOCK_MASK  0x0FFF
#define SOLID_FLOOR 0x1000
#define SOLID_SIDES 0x2000

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

/* Set on entry by path_grip/path_air exactly as ProcessObjectMovement sets
 * RSDK::collisionTolerance; the four position finders below read it. */
static int32_t collisionTolerance;

static int32_t iabs(int32_t v)
{
	return v < 0 ? -v : v;
}

static uint16_t cell_at(int32_t cx, int32_t cy)
{
	/* g_map_w/g_map_h are uint16_t, but every operand here promotes to
	 * this build's 32-bit int (confirmed: no -mshort on either CPU)
	 * before the multiply, same as cx/cy already were, so the row*width
	 * term (up to 127*1024) never truncates through a 16-bit intermediate. */
	if (cx < 0 || cy < 0 || cx >= g_map_w * CELL_SIZE || cy >= g_map_h * CELL_SIZE)
		return 0;
	return g_ghz_map[(cy / CELL_SIZE) * g_map_w + (cx / CELL_SIZE)];
}

/* ---- position finders, one per collision mode ---------------------------- */

static void find_floor(Sensor *s)
{
	int32_t posX = FROM_FIXED(s->x), posY = FROM_FIXED(s->y);
	int32_t cy = (posY & -CELL_SIZE) - CELL_SIZE;
	int32_t colY = posY;    /* captured once; unlike startY, never updated */
	int32_t startY = posY;  /* only gates the !collided || startY>=ty preference test */
	int32_t i;

	for (i = 0; i < 3; i++, cy += CELL_SIZE) {
		uint16_t cell = cell_at(posX, cy);
		uint16_t b;
		const uint8_t *row;
		uint8_t mask;
		int32_t ty, tileAngle, adiff;

		if (!(cell & SOLID_FLOOR)) continue;
		b = cell & BLOCK_MASK;
		row = &g_ghz_collide_rows[(uint32_t)g_ghz_collide_index[b] * STRIDE];
		mask = row[OFF_FLOOR + (posX & 0xF)];
		if (mask == 0xFF) continue;

		ty = cy + mask;
		if (!s->collided || startY >= ty) {
			if (iabs(colY - ty) > collisionTolerance) continue;

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
				startY = ty;
				return;
			}
		}
	}
}

static void find_roof(Sensor *s)
{
	int32_t posX = FROM_FIXED(s->x), posY = FROM_FIXED(s->y);
	int32_t cy = (posY & -CELL_SIZE) + CELL_SIZE;
	int32_t colY = posY;
	int32_t startY = posY;
	int32_t i;

	for (i = 0; i < 3; i++, cy -= CELL_SIZE) {
		uint16_t cell = cell_at(posX, cy);
		uint16_t b;
		const uint8_t *row;
		uint8_t mask;
		int32_t ty, tileAngle;

		if (!(cell & SOLID_SIDES)) continue;
		b = cell & BLOCK_MASK;
		row = &g_ghz_collide_rows[(uint32_t)g_ghz_collide_index[b] * STRIDE];
		mask = row[OFF_ROOF + (posX & 0xF)];
		if (mask == 0xFF) continue;

		ty = cy + mask;
		tileAngle = row[OFF_ANGLE + 3];
		if ((!s->collided || startY <= ty)
		    && iabs(colY - ty) <= collisionTolerance
		    && iabs((int32_t)s->angle - tileAngle) <= ROOF_ANGLE_TOLERANCE) {
			s->y = TO_FIXED(ty);
			s->angle = (uint8_t)tileAngle;
			s->collided = 1;
			startY = ty;
			return;
		}
	}
}

static void find_lwall(Sensor *s)
{
	int32_t posX = FROM_FIXED(s->x), posY = FROM_FIXED(s->y);
	int32_t cx = (posX & -CELL_SIZE) - CELL_SIZE;
	int32_t colX = posX;
	int32_t startX = posX;
	int32_t i;

	for (i = 0; i < 3; i++, cx += CELL_SIZE) {
		uint16_t cell = cell_at(cx, posY);
		uint16_t b;
		const uint8_t *row;
		uint8_t mask;
		int32_t tx, tileAngle;

		/* unlike the floor/roof finders, either solid bit blocks a wall */
		if (!(cell & (SOLID_FLOOR | SOLID_SIDES))) continue;
		b = cell & BLOCK_MASK;
		row = &g_ghz_collide_rows[(uint32_t)g_ghz_collide_index[b] * STRIDE];
		mask = row[OFF_LWALL + (posY & 0xF)];
		if (mask == 0xFF) continue;

		tx = cx + mask;
		tileAngle = row[OFF_ANGLE + 1];
		if ((!s->collided || startX >= tx)
		    && iabs(colX - tx) <= collisionTolerance
		    && iabs((int32_t)s->angle - tileAngle) <= WALL_ANGLE_TOLERANCE) {
			s->x = TO_FIXED(tx);
			s->angle = (uint8_t)tileAngle;
			s->collided = 1;
			startX = tx;
			return;
		}
	}
}

static void find_rwall(Sensor *s)
{
	int32_t posX = FROM_FIXED(s->x), posY = FROM_FIXED(s->y);
	int32_t cx = (posX & -CELL_SIZE) + CELL_SIZE;
	int32_t colX = posX;
	int32_t startX = posX;
	int32_t i;

	for (i = 0; i < 3; i++, cx -= CELL_SIZE) {
		uint16_t cell = cell_at(cx, posY);
		uint16_t b;
		const uint8_t *row;
		uint8_t mask;
		int32_t tx, tileAngle;

		if (!(cell & (SOLID_FLOOR | SOLID_SIDES))) continue;
		b = cell & BLOCK_MASK;
		row = &g_ghz_collide_rows[(uint32_t)g_ghz_collide_index[b] * STRIDE];
		mask = row[OFF_RWALL + (posY & 0xF)];
		if (mask == 0xFF) continue;

		tx = cx + mask;
		tileAngle = row[OFF_ANGLE + 2];
		if ((!s->collided || startX <= tx)
		    && iabs(colX - tx) <= collisionTolerance
		    && iabs((int32_t)s->angle - tileAngle) <= WALL_ANGLE_TOLERANCE) {
			s->x = TO_FIXED(tx);
			s->angle = (uint8_t)tileAngle;
			s->collided = 1;
			startX = tx;
			return;
		}
	}
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
 *     sensor, taking the first solid one rather than comparing all three. */

static void floor_collision(Sensor *s)
{
	int32_t posX = FROM_FIXED(s->x), posY = FROM_FIXED(s->y);
	int32_t cy = (posY & -CELL_SIZE) - CELL_SIZE;
	int32_t collidePos = 0x7FFFFFFF;
	int32_t collideAngle = 0;
	int32_t i;

	for (i = 0; i < 2; i++, cy += CELL_SIZE) {
		uint16_t cell = cell_at(posX, cy);
		uint16_t b;
		const uint8_t *row;
		uint8_t mask;

		if (!(cell & SOLID_FLOOR)) continue;
		b = cell & BLOCK_MASK;
		row = &g_ghz_collide_rows[(uint32_t)g_ghz_collide_index[b] * STRIDE];
		mask = row[OFF_FLOOR + (posX & 0xF)];
		if (mask == 0xFF) continue;

		collideAngle = row[OFF_ANGLE + 0];
		collidePos = cy + mask;
		break;
	}

	if (collidePos != 0x7FFFFFFF) {
		int32_t collideDist = s->y - TO_FIXED(collidePos);
		if (s->y >= TO_FIXED(collidePos) && collideDist <= TO_FIXED(14)) {
			s->angle = (uint8_t)collideAngle;
			s->y = TO_FIXED(collidePos);
			s->collided = 1;
		}
	}
}

static void roof_collision(Sensor *s)
{
	int32_t posX = FROM_FIXED(s->x), posY = FROM_FIXED(s->y);
	int32_t cy = (posY & -CELL_SIZE) + CELL_SIZE;
	int32_t collidePos = -1;
	int32_t collideAngle = 0;
	int32_t i;

	for (i = 0; i < 2; i++, cy -= CELL_SIZE) {
		uint16_t cell = cell_at(posX, cy);
		uint16_t b;
		const uint8_t *row;
		uint8_t mask;

		if (!(cell & SOLID_SIDES)) continue;
		b = cell & BLOCK_MASK;
		row = &g_ghz_collide_rows[(uint32_t)g_ghz_collide_index[b] * STRIDE];
		mask = row[OFF_ROOF + (posX & 0xF)];
		if (mask == 0xFF) continue;

		collideAngle = row[OFF_ANGLE + 3];
		collidePos = cy + mask;
		break;
	}

	if (collidePos >= 0 && s->y <= TO_FIXED(collidePos)
	    && s->y - TO_FIXED(collidePos) >= -TO_FIXED(14)) {
		s->angle = (uint8_t)collideAngle;
		s->y = TO_FIXED(collidePos);
		s->collided = 1;
	}
}

static void lwall_collision(Sensor *s)
{
	int32_t posX = FROM_FIXED(s->x), posY = FROM_FIXED(s->y);
	int32_t cx = (posX & -CELL_SIZE) - CELL_SIZE;
	int32_t i;

	for (i = 0; i < 3; i++, cx += CELL_SIZE) {
		uint16_t cell = cell_at(cx, posY);
		uint16_t b;
		const uint8_t *row;
		uint8_t mask;
		int32_t tx;

		if (!(cell & SOLID_SIDES)) continue;
		b = cell & BLOCK_MASK;
		row = &g_ghz_collide_rows[(uint32_t)g_ghz_collide_index[b] * STRIDE];
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

static void rwall_collision(Sensor *s)
{
	int32_t posX = FROM_FIXED(s->x), posY = FROM_FIXED(s->y);
	int32_t cx = (posX & -CELL_SIZE) + CELL_SIZE;
	int32_t i;

	for (i = 0; i < 3; i++, cx -= CELL_SIZE) {
		uint16_t cell = cell_at(cx, posY);
		uint16_t b;
		const uint8_t *row;
		uint8_t mask;
		int32_t tx;

		if (!(cell & SOLID_SIDES)) continue;
		b = cell & BLOCK_MASK;
		row = &g_ghz_collide_rows[(uint32_t)g_ghz_collide_index[b] * STRIDE];
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

/* ---- SetPathGripSensors -------------------------------------------------- */

static void set_sensors(PathEntity *e, Sensor *s)
{
	switch (e->collisionMode) {
	default:
	case CMODE_FLOOR:
		s[0].y = s[1].y = s[2].y = s[4].y + TO_FIXED(e->outer.bottom);
		s[3].y = s[4].y;
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
		s[3].y = s[4].y;
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

/* ---- ProcessPathGrip ----------------------------------------------------- */

void path_grip(PathEntity *e)
{
	Sensor s[6];
	int32_t xVel = 0, yVel = 0;
	int32_t absSpeed = (e->groundVel < 0) ? -e->groundVel : e->groundVel;
	int32_t checkDist = absSpeed >> 18;
	int32_t i, best;

	collisionTolerance = HIGH_COLLISION_TOLERANCE;
	if (iabs(e->groundVel) < TO_FIXED(6) && e->angle == 0)
		collisionTolerance = LOW_COLLISION_TOLERANCE;

	absSpeed &= 0x3FFFF;

	s[4].x = e->x;
	s[4].y = e->y;
	for (i = 0; i < 6; i++) {
		s[i].angle = e->angle;
		s[i].collided = 0;
	}
	set_sensors(e, s);

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

		/* Only the floor mode is stepped for now; the other three reuse the
		 * same shape with their own finders. */
		s[3].x += xVel;
		s[3].y += yVel;
		if (e->collisionMode == CMODE_FLOOR) {
			if (e->groundVel > 0) find_lwall(&s[3]);
			if (e->groundVel < 0) find_rwall(&s[3]);
		} else if (e->collisionMode == CMODE_LWALL) {
			if (e->groundVel > 0) find_roof(&s[3]);
			if (e->groundVel < 0) find_floor(&s[3]);
		} else if (e->collisionMode == CMODE_ROOF) {
			if (e->groundVel > 0) find_rwall(&s[3]);
			if (e->groundVel < 0) find_lwall(&s[3]);
		} else {
			if (e->groundVel > 0) find_floor(&s[3]);
			if (e->groundVel < 0) find_roof(&s[3]);
		}

		if (s[3].collided) {
			xVel = 0;
			yVel = 0;
			checkDist = -1;
			e->groundVel = 0;
		}

		for (i = 0; i < 3; i++) {
			s[i].x += xVel;
			s[i].y += yVel;
			switch (e->collisionMode) {
			default:
			case CMODE_FLOOR: find_floor(&s[i]); break;
			case CMODE_LWALL: find_lwall(&s[i]); break;
			case CMODE_ROOF:  find_roof(&s[i]);  break;
			case CMODE_RWALL: find_rwall(&s[i]); break;
			}
		}

		/* pick the sensor that found the highest ground for this mode */
		best = -1;
		for (i = 0; i < 3; i++) {
			if (!s[i].collided) continue;
			if (best < 0) { best = i; continue; }
			switch (e->collisionMode) {
			default:
			case CMODE_FLOOR: if (s[i].y < s[best].y) best = i; break;
			case CMODE_LWALL: if (s[i].x > s[best].x) best = i; break;
			case CMODE_ROOF:  if (s[i].y > s[best].y) best = i; break;
			case CMODE_RWALL: if (s[i].x < s[best].x) best = i; break;
			}
		}

		if (best < 0) {              /* nothing underfoot: leave the ground */
			e->x = s[4].x;
			e->y = s[4].y;
			e->velX = (e->groundVel * cos256(e->angle)) >> 8;
			e->velY = (e->groundVel * sin256(e->angle)) >> 8;
			e->collisionMode = CMODE_FLOOR;
			e->angle = 0;
			e->onGround = 0;
			sync_velocity(e);
			return;
		}

		/* The body position comes back from the sensors, not from its own
		 * advance. When the leading sensor hit a wall the step was zeroed, so
		 * the floor sensors still hold the pre-collision position and this is
		 * what rolls the body back out of the wall. */
		e->angle = s[best].angle;
		switch (e->collisionMode) {
		default:
		case CMODE_FLOOR:
			s[0].y = s[1].y = s[2].y = s[best].y;
			s[4].x = s[1].x;
			s[4].y = s[best].y - TO_FIXED(e->outer.bottom);
			if (s[best].angle < 0xDE && s[best].angle > 0x80)
				e->collisionMode = CMODE_LWALL;
			if (s[best].angle > 0x22 && s[best].angle < 0x80)
				e->collisionMode = CMODE_RWALL;
			break;
		case CMODE_LWALL:
			s[0].x = s[1].x = s[2].x = s[best].x;
			s[4].y = s[1].y;
			s[4].x = s[best].x - TO_FIXED(e->outer.bottom);
			if (s[best].angle > 0x5E && s[best].angle < 0xC0)
				e->collisionMode = CMODE_ROOF;
			if (s[best].angle < 0x22 || s[best].angle > 0xE2)
				e->collisionMode = CMODE_FLOOR;
			break;
		case CMODE_ROOF:
			s[0].y = s[1].y = s[2].y = s[best].y;
			s[4].x = s[1].x;
			s[4].y = s[best].y + TO_FIXED(e->outer.bottom) + TO_FIXED(1);
			if (s[best].angle > 0xA2 && s[best].angle < 0xE0)
				e->collisionMode = CMODE_LWALL;
			if (s[best].angle > 0x20 && s[best].angle < 0x5E)
				e->collisionMode = CMODE_RWALL;
			break;
		case CMODE_RWALL:
			s[0].x = s[1].x = s[2].x = s[best].x;
			s[4].y = s[1].y;
			s[4].x = s[best].x + TO_FIXED(e->outer.bottom) + TO_FIXED(1);
			if (s[best].angle > 0xA2 && s[best].angle < 0xE0)
				e->collisionMode = CMODE_ROOF;
			if (s[best].angle < 0x9E && s[best].angle > 0x60)
				e->collisionMode = CMODE_FLOOR;
			break;
		}
		set_sensors(e, s);
	}

	e->x = s[4].x;
	e->y = s[4].y;
	sync_velocity(e);
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
			lwall_collision(&s[0]);
			if (s[0].collided) movingRight = 2;
		}

		if (movingLeft == 1) {
			s[1].x += velX;
			s[1].y += velY;
			rwall_collision(&s[1]);
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
					floor_collision(&s[i]);
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
					roof_collision(&s[i]);
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
