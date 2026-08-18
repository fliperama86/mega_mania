#ifndef MD_COLLAPSINGPLATFORM_H
#define MD_COLLAPSINGPLATFORM_H

#include "md.h"

/* CollapsingPlatform (Common/CollapsingPlatform.c): GHZ Act 1's 15 instances.
 * ALL physics/collision lives on sh_src/collapsingplatform.c -- see that
 * file's own header comment for the full mechanism and why this side draws
 * NOTHING: CollapsingPlatform_Draw's only retail-visible content is the
 * falling stage-tile debris (cut from this batch for time, see sh_src/
 * collapsingplatform.h's own comment), and every other sprite it emits
 * (four corner TicMark boxes) is DebugMode-only in the original too. This
 * row exists in md_src/main.c's OBJ_TYPE_LIST purely so the class has a
 * registration (per this batch's own "one line per class" instruction), not
 * because it has anything to draw -- collapsingplatform_draw() below always
 * returns 0. */
uint16_t collapsingplatform_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                                 uint16_t camX, uint16_t camY,
                                 int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

#endif
