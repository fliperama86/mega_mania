#ifndef SPIKES_H
#define SPIKES_H

#include "player.h"

/* Spikes, ported from SonicMania/Objects/Global/Spikes.c: GHZ1's 41
 * Mania-mode Spikes entities, applied every frame in slot order (same shape
 * sh_src/spring.c already uses) after player_update() has settled this
 * frame's position.
 *
 * This is the MANIA_USE_PLUS build path (Spikes_Update's #if MANIA_USE_PLUS
 * arm, Spikes.c:109-191/507-649 -- GAME_VERSION defaults to VER_106 in this
 * decompilation, so MANIA_USE_PLUS is the code that actually ships, not the
 * #else fallback): every Mighty-character/Ice-shield branch is dropped (this
 * port has neither), which reduces Spikes_CheckHit (Spikes.c:507-649) to its
 * final, unconditional tail -- knockback direction off which side of the
 * spike Sonic is on, then Player_Hit -- exactly player_hit()'s own contract.
 *
 * The slave SH2 owns hazard PHYSICS only (collision push-out, the hit
 * trigger): the appear/disappear timer visual is a separate, purely
 * observational 68000-side concern (md_src/spikes.c), which never talks to
 * this file or vice versa -- see that file's own doc comment for why both
 * sides can independently derive the same SHOWN/HIDDEN timing with no comm
 * bit spent. */
void spikes_apply(Player *p);

#endif
