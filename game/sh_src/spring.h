#ifndef SPRING_H
#define SPRING_H

#include "player.h"

/* Spring, ported from SonicMania/Objects/Global/Spring.c: a per-scene table
 * of GHZ1's 35 Mania-mode Spring entities, applied every frame in slot order
 * (same shape as sh_src/force_spin.c/plane_switch.c) after player_update has
 * settled this frame's position -- see s_main.c's call site comment for why
 * that ordering matches the original's own entity-slot update order
 * (RSDK::ProcessObjects, Object.cpp:380-383).
 *
 * The slave SH2 owns spring PHYSICS only (collision, velocity, state,
 * animation): the spring's own bounce animation is a separate, purely
 * observational 68000-side concern (md_src/springs.c), which never talks to
 * this file or vice versa -- see that file's own doc comment. */
void spring_apply(Player *p);

#endif
