#ifndef ITEMBOX_H
#define ITEMBOX_H

#include "player.h"

/* ItemBox, ported from SonicMania/Objects/Global/ItemBox.c: GHZ1's 38
 * Mania-mode ItemBox entities. See md_src/itembox.h's own header comment for
 * the full powerup-effect table (which types work, which are inert, which
 * are substituted) and every compromise this port makes.
 *
 * This file owns the SH2-side half of "which types can this CPU act on":
 * only ITEMBOX_EGGMAN (Player_Hurt -> player_hit()) needs anything Player-
 * side; Ring/HyperRing's ring award happens entirely on the 68000
 * (md_src/itembox.c, since rings.c's counter already lives there) and every
 * other type used by this stage (shields, Invincible, Sneakers, 1UP_SONIC)
 * has no system on EITHER cpu to act on -- see itembox.h's own header
 * comment for why those are reported inert, not silently no-op'd. */
void itembox_apply(Player *p);

#endif
