#ifndef PATH_H
#define PATH_H

/* Grounded collision, ported from RSDK's ProcessPathGrip and friends in
 * Collision.cpp.
 *
 * The entity carries a collision mode which rotates its whole sensor set as
 * the ground angle passes through the diagonals, so slopes, walls and ceilings
 * are all handled by one path rather than special cases. That is what makes
 * running up a steep slope work instead of reading as a wall. */

#define CMODE_FLOOR 0
#define CMODE_LWALL 1
#define CMODE_ROOF  2
#define CMODE_RWALL 3

typedef struct {
	int32_t x, y;          /* 16.16 */
	uint8_t angle;
	uint8_t collided;
} Sensor;

/* Hitbox as RSDK stores it: offsets from the entity position, in pixels */
typedef struct {
	int8_t left, top, right, bottom;
} Box;

typedef struct {
	int32_t x, y;          /* 16.16 world position, centre of the body */
	int32_t groundVel;
	int32_t velX, velY;
	Box outer, inner;      /* hitbox 0 and 1 of the current animation frame */
	uint8_t angle;
	uint8_t collisionMode;
	uint8_t onGround;
	uint8_t collisionPlane; /* RSDK's collisionEntity->collisionPlane: 0 or 1,
	                          * selects which of the two TileConfig paths (and
	                          * which map solidity bits) every finder in
	                          * path.c tests. Written only by plane_switch.c;
	                          * see sh_src/plane_switch.c. */
} PathEntity;

void path_grip(PathEntity *e);
void path_air(PathEntity *e);

#endif
