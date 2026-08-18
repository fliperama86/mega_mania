#ifndef OBJ_SPRITE_H
#define OBJ_SPRITE_H

/* Shared by springs.c/signpost.c: springs and the signpost's own DrawSprite
 * calls (Spring_Draw, Spring.c:31-36; SignPost_Draw, SignPost.c:22-61) both
 * reduce, on this port's side, to the exact same "walk this frame's pieces,
 * mirror each one's box position for the given flip, write a VDPSprite"
 * loop sonic_build() (md_src/sonic.c) also has its own copy of. Written
 * once here as a `static inline` rather than an ordinary extern function:
 * an ordinary function, called from three very different sites (springs.c's
 * one flip-aware call and signpost.c's two always-upright calls) with 11
 * parameters, cost more in per-call-site argument marshalling on the 68000
 * than three independently-optimized inlined copies do -- measured, not
 * assumed, while closing this feature's own 512 KB ROM overage (see
 * docs/green-hill.md/this task's report). `static inline` lets -Os specialize
 * away the always-0 flipH/flipV arguments at signpost.c's two call sites
 * instead of carrying them at runtime the way a real shared function would
 * have to. */

#include "md.h"
#include "obj_data.h"

static inline uint16_t obj_emit_pieces(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                                       uint16_t maxCount, const ObjPiece *p, uint8_t pieceCount,
                                       uint16_t tileBase, uint8_t pal, int16_t sx, int16_t sy,
                                       int8_t pivotX, int8_t pivotY, uint8_t flipH, uint8_t flipV,
                                       uint8_t prio)
{
	uint16_t n = 0;

	for (; pieceCount > 0 && n < maxCount; pieceCount--, p++) {
		int16_t w = (int16_t)(((p->size >> 2) & 3) + 1) << 3;
		int16_t h = (int16_t)((p->size & 3) + 1) << 3;
		int16_t dx = flipH ? (int16_t)(-pivotX - p->dx - w)
		                   : (int16_t)(pivotX + p->dx);
		int16_t dy = flipV ? (int16_t)(-pivotY - p->dy - h)
		                   : (int16_t)(pivotY + p->dy);
		VDPSprite *s = &list[firstIndex + n];

		s->y = (int16_t)(128 + sy + dy);
		s->size = p->size;
		s->link = firstLink + n + 1;
		s->attr = TILE_ATTR(pal, prio, flipV, flipH, tileBase + p->tile);
		s->x = (int16_t)(128 + sx + dx);
		n++;
	}
	return n;
}

#endif
