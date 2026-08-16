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

#endif
