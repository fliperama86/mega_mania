#ifndef COLLIDE_H
#define COLLIDE_H

/* Ground sensing against the converted stage collision.
 *
 * Each 16x16 block has 16 column heights, measured up from the block's bottom
 * edge, with 0xFF meaning that column is empty. A placement is only solid if
 * the map entry says so, which is what stops decorative tiles acting as floor.
 */

#define COLL_NONE 0x7FFF

/* Height of the ground at world x, searching down from world y.
 * Returns the world y of the surface, or COLL_NONE if nothing was found
 * within the search distance. */
int16_t collide_floor(int16_t x, int16_t y, int16_t maxdist);

/* Floor angle byte at world x,y, RSDK's 0-255 scale where 0 is flat. */
uint8_t collide_angle(int16_t x, int16_t y);

#endif
