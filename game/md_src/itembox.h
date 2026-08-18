#ifndef ITEMBOX_H
#define ITEMBOX_H

/* Global/ItemBox.c: GHZ1's 38 Mania-mode ItemBox entities (tools/
 * gen_assets.py's ghz_itemboxes/itembox_tiles manifest entries). Act 1 uses
 * 10 of ItemBoxTypes' ~17 values (ItemBox.h's own enum): RING(16 instances),
 * BLUESHIELD(2), BUBBLESHIELD(2), FIRESHIELD(3), LIGHTNINGSHIELD(3),
 * INVINCIBLE(3), SNEAKERS(1), 1UP_SONIC(5), EGGMAN(1), HYPERRING(2) --
 * counted directly off this stage's own converted itemboxes.bin, not
 * estimated.
 *
 * WHICH POWERUPS ACTUALLY DO SOMETHING, stated up front (see
 * md_src/itembox.c's own itembox_give_powerup() and sh_src/itembox.c's
 * matching comment for the full per-type derivation):
 *   WORKS:  RING (+10 rings, rings_add()), HYPERRING (+10 rings, see below),
 *           EGGMAN (Player_Hurt equivalent, player_hit()).
 *   INERT:  BLUESHIELD/BUBBLESHIELD/FIRESHIELD/LIGHTNINGSHIELD (no shield
 *           system exists in this port -- player.h's own top comment),
 *           INVINCIBLE (no invincibility/invincibleTimer), SNEAKERS (no
 *           speed-shoe system), 1UP_SONIC (no lives counter -- this port's
 *           own documented deviation, sh_src/s_main.c's own comment on
 *           player_kill()'s respawn path). Every inert type still breaks
 *           correctly (icon reveals, box vanishes) -- only the gameplay
 *           EFFECT is missing, never silently swallowed.
 *   SUBSTITUTED: HYPERRING (ItemBox.c:540-543, `player->hyperRing = true` --
 *           a ring-magnetism/auto-collect flag this port has no system to
 *           back) is NOT left inert: the task brief explicitly calls for it
 *           to "genuinely award rings, since rings exist" -- it is wired to
 *           the SAME rings_add(10) the Ring type itself uses, the one
 *           verified "an item box gives rings" constant this file has,
 *           rather than inventing a new number for the substitution.
 *
 * PERSISTENT STATE: each side (this file, 68000/draw+ring-award; sh_src/
 * itembox.c, SH2/hit-response) keeps its OWN independent 38-bit broken
 * bitmap, inferred locally off the SAME simplified attacking+touch rule
 * (this file's own itembox_tick()), the same "no comm bit to spare"
 * observational pattern md_src/breakablewall.h's own header comment already
 * documents for that class. Awarding rings happens ENTIRELY on this
 * (68000) side, since ringPlayerCount (md_src/rings.c) already lives here --
 * the slave SH2 never needs to be told a ring-type box broke at all, only
 * its OWN side's break bit, for whichever types it can act on itself
 * (Eggman's player_hit()).
 *
 * COMPROMISES beyond the powerup table above:
 *   - No solid-platform/bump-from-below physics (ItemBox_CheckHit's
 *     Player_CheckCollisionBox branch, ItemBox.c:435-470 -- an unbroken box
 *     can be stood on, or bumped upward from below to start it falling, in
 *     the original). This port's item boxes have no collision at all except
 *     the attacking+touch break trigger -- Sonic passes through an unbroken
 *     box's own footprint from any non-breaking angle. Verified against
 *     this stage's own data that this never strands the player: every one
 *     of the 38 rows has isFalling=lrzConvPhys=0 (no box starts already
 *     airborne or conveyor-driven), so nothing here depended on that
 *     physics to reach its resting place in the first place.
 *   - The 2 hidden=1 rows (ItemBox_Create:161-163,
 *     `self->state = StateMachine_None`) are permanently skipped on both
 *     sides -- matches the original's own default (nothing in this port's
 *     object roster has an "unhide this box" trigger to fire either).
 *   - No flying-icon-bounce/disappear animation, no debris chips, no
 *     Explosion/score-bonus child entities (ItemBox_Break, ItemBox.c:
 *     798-895): breaking is instant -- box swaps to one of the 3 "Broken"
 *     poses (round-robined by instance index, not RSDK's own +1-mod-3
 *     counter) and the reward applies immediately, no bounce delay. */
#include "md.h"

#define ITEMBOX_COUNT 38
#define ITEMBOX_SPRITE_CAP 24   /* 2 pieces (box+icon) per unbroken visible
                                  * instance, 1 per broken one; generous
                                  * headroom over the measured worst case. */

void itembox_init(void);

/* Per-object pre-step (obj_pool.h's ObjTickFn): independently infers this
 * side's own broken bitmap off worldX/worldY/frameIndex (the same published
 * bus fields breakablewall.c's own tick already reads), and calls
 * rings_add() directly for whichever newly-broken box is Ring or
 * HyperRing typed -- see this file's own header comment. */
void itembox_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

uint16_t itembox_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                      uint16_t camX, uint16_t camY,
                      int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

#endif
