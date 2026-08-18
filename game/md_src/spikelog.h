#ifndef SPIKELOG_H
#define SPIKELOG_H

/* GHZ/SpikeLog.c: GHZ1's 61 rotating log hazards (tools/gen_assets.py's
 * ghz_spikelogs/spikelog_tiles manifest entries), the single most numerous
 * object in this batch. Drawing is 68000-only; hazard physics (the touch
 * test and hit trigger) is slave-SH2-only (sh_src/spikelog.c) -- neither
 * side talks to the other, both independently derive the same rotation
 * frame from their own local tick counter, same pattern spikes.c already
 * uses (see that file's own header comment on why this is safe).
 *
 * VRAM (REVISED 2026-08-18, VRAM capacity task): now streams through
 * md_src/obj_generic.h's per-class ANIMATION WINDOW (obj_anim_window_
 * register()), same mechanism rings.c's own rotation uses, NOT the
 * whole-176-tile-sheet-resident plain arena this class used before.
 *
 * THE COST, STATED PLAINLY (this task's own report flags it for the user's
 * sign-off, same as every other lockstep migration this task made):
 * SpikeLog_State_Main (SpikeLog.c:62) sets `self->animator.frameID =
 * (self->frame + SpikeLog->timer) & 0x1F` -- self->frame is a PER-INSTANCE
 * constant (the scene's own editable byte, x4) added to the ONE shared
 * clock, so in the ORIGINAL game different logs sit at genuinely different
 * points of the rotation simultaneously (this stage's own data uses all 8
 * possible offsets, GHZ1's own scene dump, ~equally distributed across 61
 * logs). A single shared animation window can only ever hold ONE frame, so
 * this migration drops the per-instance offset entirely: every log on
 * screen now shows the SAME rotation frame at the SAME instant
 * (spikelog.c's own sharedTimer, no `+ self->frame*4` any more). This is a
 * real, visible departure from the original (all 61 logs "in unison"
 * instead of individually staggered) -- previously flagged here as the
 * reason NOT to do this (an 8-window-per-offset-family split was considered
 * and rejected as too expensive in OBJ_ANIM_WINDOW_MAX slots). It is done
 * anyway now because the alternative measured worse: at the arena's
 * pre-reclaim 203-tile size, SpikeLog's 176-tile whole-sheet request was
 * REFUSED at boot outright (obj_arena_boot_load() returning no room) and
 * never became resident for the rest of the run -- SpikeLog drew NOTHING,
 * ever, which is strictly worse than staggered-vs-lockstep rotation. The
 * window costs 2*SPIKELOG_MAX_FRAME_TILES=12 tiles instead of 176, comfortably
 * fits even the un-reclaimed arena, and this task's own user sign-off
 * explicitly covers "lockstep animation per class" as an approved cost for
 * this migration. */
#include "md.h"

#define SPIKELOG_COUNT 61
#define SPIKELOG_SPRITE_CAP 24   /* generous headroom; one piece per visible
                                  * log, no repeated tiling unlike Spikes. */

void spikelog_init(void);
uint16_t spikelog_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                       uint16_t camX, uint16_t camY,
                       int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

#endif
