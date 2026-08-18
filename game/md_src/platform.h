#ifndef MD_PLATFORM_H
#define MD_PLATFORM_H

#include "md.h"

/* Platform (Common/Platform.c): GHZ Act 1's 60 instances, drawing only --
 * physics/collision is sh_src/platform.c's own, from the SAME generated
 * scene table read at a different linked address (tools/gen_assets.py's
 * manifest: ghz_platforms/_ghz_platforms_sh, one converted table, two
 * link names, same convention every other class in this codebase already
 * uses). See this file's own .c for the drawn-position agreement proof
 * (md_src/platform_clock.h) and per-type decomp line references.
 *
 * Measured worst case (this batch's own scan of assets/ghz/platforms.bin,
 * piece-counting every frame a Fixed/Fall/Linear/Push entry can show,
 * windowed at PLATFORM's own marginX): 48 hardware-sprite PIECES from
 * non-Swing Platform types alone in one 320px camera view (four consecutive
 * PLATFORM_LINEAR entries near x=7500-7900, all using the 144-tile/10-piece
 * platform_normal[1] frame, plus neighbouring Fixed/Fall entries) -- by far
 * the single largest per-class demand this whole TRAVERSAL batch produces,
 * bigger than Bridge's own measured 19-21 planks. See this batch's own
 * final report for the full arithmetic and why OBJ_PRI_PLATFORM (this
 * type's own drop rank, obj_pool.h) makes this a deliberate, surfaced
 * tradeoff rather than an accident: a platform silently missing its own
 * sprite costs the player a fall-through, so this type wins the shared pool
 * over rings/badniks/scenery at the one screen location where it matters. */
#define PLATFORM_SPRITE_CAP 56

void platform_init(void);

/* ObjTickFn (obj_pool.h): advances the shared platform_clock (md_src/
 * platform_clock.c) and this file's own observational replicas of Fall's
 * trigger tick / Push's accumulated offset -- see platform.c's own top
 * comment for why these have to be independently re-derived here rather
 * than read off the SH2, and the precedent (springs.c's spring_touches_sonic)
 * this follows. */
void platform_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

uint16_t platform_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                       uint16_t camX, uint16_t camY,
                       int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

#endif
