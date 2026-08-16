#ifndef FORCE_SPIN_H
#define FORCE_SPIN_H

#include "player.h"

/* ForceSpin, ported from SonicMania/Objects/Common/ForceSpin.c: a per-scene
 * table of line-segment markers (GHZ Scene1.bin's ForceSpin entities) that
 * force the player into the tube-roll states while near the line and
 * crossing from its "closed" side, and release them back to normal
 * rolling/falling when crossing from the other side. See force_spin.c for
 * the per-marker math (ForceSpin_Update) and the table itself. */
void force_spin_apply(Player *p);

/* Zone_RotateOnPivot (SonicMania/Objects/Global/Zone.c:506-512): rotate
 * (*px,*py) around (ox,oy) by angle. Defined in force_spin.c (ForceSpin_Update
 * was the first caller) and exported here rather than duplicated: plane_
 * switch.c needs the identical rotation for PlaneSwitch_CheckCollisions'
 * position+velocity rotation -- see force_spin.c's own comment on the
 * function for why the >>8-before-multiply order matters. */
void rotate_on_pivot(int32_t *px, int32_t *py, int32_t ox, int32_t oy, uint8_t angle);

#endif
