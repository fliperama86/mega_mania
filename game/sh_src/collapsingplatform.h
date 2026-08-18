#ifndef COLLAPSINGPLATFORM_H
#define COLLAPSINGPLATFORM_H

#include "player.h"

/* CollapsingPlatform (Common/CollapsingPlatform.c), GHZ Act 1's 15 instances:
 * a solid-from-above platform that starts a `delay`-tick countdown the first
 * tick a grounded, non-sidekick player stands on it (CollapsingPlatform_
 * Update:39-65, `self->stoodPos.x` claimed once, `collapseDelay=self->delay`
 * armed, `eventOnly` gates it off, `delay<0xFFFF` -- always true for this
 * stage's own data, see md_src/collapsingplatform.c) and, once that timer
 * reaches 0, becomes non-solid PERMANENTLY (`self->respawn` is 0 for every
 * one of GHZ1's 15 instances -- verified against the converted data, not
 * assumed -- so the original's own respawn branch, CollapsingPlatform_
 * Update:71-74, never fires here either; every instance instead takes the
 * `destroyEntity(self)` branch, Update:76, which is exactly "gone for good"
 * this port's own permanent-non-solid state already represents).
 *
 * NO ART OF ITS OWN: CollapsingPlatform_Draw's real content (the crumbling
 * tile-clone debris, CollapsingPlatform_State_Left/Right/Center et al.,
 * CollapsingPlatform.c:186-318) spawns BreakableWall_TILE_DYNAMIC clones of
 * the STAGE's own tiles under it, drawn with a per-tile staggered falling
 * timer -- everything else CollapsingPlatform_Draw itself emits (the four
 * corner debug rectangles/TicMark sprites, CollapsingPlatform.c:85-115) is
 * gated on DebugMode->debugActive, dead in retail play. This batch cuts the
 * falling-debris visual entirely (see this batch's own report for why: the
 * per-tile staggered fall needs either a live lookup into the STATIC stage
 * tilemap already resident on the 68000 -- feasible in principle -- or a
 * bespoke multi-sprite emitter matching Bridge's own "one entry expands to
 * many hardware sprites" shape, and this batch's remaining time went to
 * Bridge and Platform's own five types first, per this batch's own stated
 * priority) -- the CORE mechanic (stand on it, it becomes non-solid after a
 * delay, you fall through) is intact and is what this file implements; only
 * the crumbling-tiles VISUAL flourish is missing. md_src/collapsingplatform.c
 * accordingly registers no tile sheet and its draw function always returns
 * 0 sprites, which is the CORRECT rendering of an object whose own retail
 * art is nothing at all, not a cut corner disguised as one. */
void collapsingplatform_apply(Player *p);

#endif
