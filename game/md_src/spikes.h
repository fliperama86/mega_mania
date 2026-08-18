#ifndef SPIKES_H
#define SPIKES_H

/* Global/Spikes.c, GHZ1's 41 Mania-mode Spikes entities (tools/gen_assets.py's
 * ghz_spikes/spikes_tiles manifest entries). Drawing is 68000-only, hazard
 * physics/collision is slave-SH2-only (sh_src/spikes.c) -- neither side talks
 * to the other; both independently derive the same appear/disappear timer
 * state from their own local frame counter (see hazardTick below), the same
 * "no comm bit to spare, so infer it locally" pattern rings.c's lost-ring
 * hit-detection already established.
 *
 * COMPROMISE, stated up front: Spikes_Update's SPIKES_MOVE_APPEAR/DISAPPEAR
 * states (Spikes.c:30-58) slide the spike in/out over 4 ticks (moveOffset
 * 0->32px). This port collapses that slide to an instant SHOWN<->HIDDEN
 * toggle at the same transition tick the original's *state machine enters*
 * that slide -- the toggle TIMING (which real tick the spike becomes
 * dangerous/safe) is transcribed exactly (Spikes.c:20-27,41-47's stagger/
 * timer test), only the ~67ms cosmetic slide itself is cut. 3 of Act 1's 41
 * spikes use this (spikes.bin's moving=1 rows); the other 38 are permanently
 * SHOWN. */
#include "md.h"

#define SPIKES_COUNT 41
#define SPIKES_SPRITE_CAP 40  /* sum of ceil(count/2) across a plausible on-
                                * screen set; count is mostly 2-3 (spikes.bin),
                                * one outlier at 20 -- truncated gracefully
                                * (obj_type_draw's own convention) if ever
                                * exceeded, never a hard failure. */

void spikes_init(void);

/* Per-object pre-step (obj_pool.h's ObjTickFn shape, but Spikes has no
 * per-frame player-position dependency at all -- see this file's own
 * comment on why the appear/disappear state machine is purely time-driven,
 * not position-driven): advances the shared local tick counter and every
 * moving spike's SHOWN/HIDDEN toggle. Registered with a NULL tick in
 * OBJ_TYPE_LIST instead -- see spikes.c's own comment on why the counter
 * advances from spikes_draw() instead, once per displayed frame, matching
 * every other tick-less row's own reasoning (rings_update()). */
uint16_t spikes_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                     uint16_t camX, uint16_t camY,
                     int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

#endif
