#ifndef BUZZBOMBER_H
#define BUZZBOMBER_H

#include "md.h"

/* BuzzBomber (BuzzBomber.c): flies a fixed-length horizontal patrol with an
 * idle hover at each end, GHZ1 x18. Fully terrain-free in the decomp itself
 * (State_Flying/Idle never touch RSDK.ObjectTileGrip/TileCollision -- this
 * is the one class in this batch that needed no terrain-sensing
 * approximation at all), so BUZZBOMBER_AMPLITUDE/IDLE_TICKS below ARE
 * decomp-exact (BuzzBomber.c:49/161-163/176-183/196-200), not invented.
 *
 * CUT, both flagged in this batch's own final report:
 *   - Player-proximity detection and its fired projectile (BuzzBomber_
 *     State_DetectedPlayer/ProjectileCharge/ProjectileShot, BuzzBomber.c:
 *     209-280) -- projectiles are cut across every class in this batch
 *     uniformly, see crabmeat.h's own comment.
 *   - The wing/thrust overlay sprites (self->wingAnimator/thrustAnimator,
 *     BuzzBomber_Draw, BuzzBomber.c:22-38) -- each is its own independently-
 *     timed animation needing its own VRAM window (obj_generic.h's own
 *     "genuine state divergence" rule), and this batch's arena slot/tile
 *     budget has no room for two more windows per BuzzBomber instance on
 *     top of its body's own. BuzzBomber draws its body only. */

#define BUZZBOMBER_SPRITE_CAP 36   /* 18 instances * 2 pieces/frame (buzzbomber_data.h) */

void buzzbomber_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);
uint16_t buzzbomber_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                         uint16_t camX, uint16_t camY,
                         int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

#endif
