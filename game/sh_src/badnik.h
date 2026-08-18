#ifndef BADNIK_H
#define BADNIK_H

#include "player.h"

/* Shared badnik collision core (BADNIKS batch): the SH2-side half of the
 * "player collision test, attacking-vs-hurting decision, destruction" this
 * batch's md_src/badnik_base.h documents on its own side. See that file's
 * top comment for the full split -- this is the ONLY place any badnik class
 * touches Player, and it exists because md_src cannot: player_hit/
 * player_bounce_badnik live entirely in this CPU's own address space.
 *
 * All six classes' own per-class files (sh_src/badniks.c) call this once
 * per instance per tick, in the same shape spring_apply()'s own per-marker
 * loop already uses. */

/* Player_CheckBadnikTouch + Player_CheckBadnikBreak's own bounce-or-hurt
 * split: touch-test the player's own live hitbox (p->e.outer) against
 * (bx,by,hbL..hbB) -- bx/by/the hitbox already 16.16 fixed, matching
 * Player.e.x/y's own scale -- and on a touch, either destroy (attacking)
 * or hurt (not attacking). Returns 1 the tick this call destroys the
 * badnik (the caller's own job to latch that into its own per-class
 * destroyed bitmap, so this is never called again for an already-dead
 * instance -- the same one-way latch md_src/badnik_base.h's
 * badnik_decide_common independently keeps on its own side), 0 otherwise
 * (no touch, or a survivable hurt that leaves the badnik itself
 * unchanged). */
uint8_t badnik_resolve(Player *p, int32_t bx, int32_t by,
                        int8_t hbL, int8_t hbT, int8_t hbR, int8_t hbB);

/* Called once per tick from s_main.c, right after spring_apply() -- same
 * "every marker every frame, player already updated this tick" shape and
 * ordering reasoning as force_spin_apply/plane_switch_apply/spring_apply
 * (s_main.c's own comment on spring_apply covers why player_update must
 * run first). Runs every one of the six classes' own simplified collision
 * models in turn (sh_src/badniks.c). */
void badnik_apply(Player *p);

#endif
