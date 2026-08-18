#include "collapsingplatform.h"

/* See this file's own header comment: CollapsingPlatform has no retail art
 * at all in this batch's port (sh_src/collapsingplatform.h has the full
 * reasoning), so this is the entire 68000-side implementation. */
uint16_t collapsingplatform_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                                 uint16_t camX, uint16_t camY,
                                 int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	(void)list; (void)firstIndex; (void)firstLink; (void)camX; (void)camY;
	(void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;
	return 0;
}
