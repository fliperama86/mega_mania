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

/* GHOST SPRITE FIX (Job A, this task). Root-caused by dumping the LIVE VDP
 * sprite table from a running emulator (ares) via lldb, not from code
 * reading: real MD/32X VDP hardware only keeps the low 9 bits of a sprite's
 * Y coordinate for its per-scanline visibility test (`object.y & 511` in
 * non-interlaced mode -- ares/md/vdp/sprite.cpp's own VDP::Sprite::scan(),
 * which is modelling real silicon here, not an emulator bug) after the
 * write path itself already truncates Y to 10 bits
 * (VDP::Sprite::write()'s `object.y = data.bit(0,9)`). Every emitter in this
 * codebase (main.c's whole candidate-collection phase, upstream of this
 * function) is X-window-culled but NEVER Y-culled -- an object computed to
 * be hundreds of pixels above or below the visible screen was assumed
 * harmless ("VDP just won't draw a line for it"), which is true up to the
 * 512-line wrap and false past it: a piece meant to sit far below/above the
 * screen can wrap back into the visible 0-511 window and appear at a WRONG
 * on-screen row -- looking exactly like a floating duplicate/ghost. Caught
 * red-handed 2026-08-20: a live dump showed a second, unsagged copy of a
 * rope-bridge plank set (GHZ1's own stacked bridge pair, bridges.bin indices
 * 1 and 2, both x=1184 -- a real, deliberately-authored double-bridge
 * crossing, not a scene-data bug) and a badnik piece at raw Y=1248, which
 * `& 511` wraps to 224 -- squarely mid-screen -- while a sibling piece at
 * raw Y=452 (no wrap; comfortably under 512) correctly stayed hidden. Same
 * mechanism either direction: a large negative sy+dy wraps through 16-bit
 * underflow into the same 0-511 window from the other side.
 *
 * Fix: cull each piece here, at the one place every migrated type's per-
 * piece screen position is actually computed, before it is ever written
 * into scratch[] (main.c's pool never gets a chance to arbitrate a piece
 * that was never a candidate to begin with -- cheaper AND correct, and
 * exactly the "structurally impossible" framing Job B's own SAT rewrite
 * wants to coordinate with). The margin is generous on purpose: any piece
 * whose true, unwrapped position is already this far outside the visible
 * area was ALWAYS meant to be invisible -- on real hardware, absent this
 * wraparound quirk, it would simply never intersect a scanline -- so
 * culling it changes nothing about what a correctly-behaving frame draws,
 * it only stops an off-screen piece from accidentally aliasing onto one.
 * 256px past every screen edge is comfortably clear of the 512-line wrap
 * (max on-screen SCREEN_HEIGHT is 240, so the culled band starts at least
 * 256-32=224 raw lines short of the 512 boundary even at max sprite height)
 * while still covering every legitimate margin already in use in this
 * codebase (platform.c's own widest, 256px, for Linear/Swing/Push). */
#define OBJ_EMIT_OFFSCREEN_MARGIN 256

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
		int16_t py = (int16_t)(sy + dy);
		int16_t px = (int16_t)(sx + dx);
		VDPSprite *s;

		/* See this function's own header comment: skip, don't clamp -- a
		 * piece this far outside the screen was never meant to be seen, so
		 * dropping the candidate entirely (rather than writing it with a
		 * "safe" sentinel position) is both the correctness fix and a free
		 * sprite-budget saving. */
		if (py < -OBJ_EMIT_OFFSCREEN_MARGIN || py >= SCREEN_HEIGHT + OBJ_EMIT_OFFSCREEN_MARGIN)
			continue;
		if (px < -OBJ_EMIT_OFFSCREEN_MARGIN || px >= SCREEN_WIDTH + OBJ_EMIT_OFFSCREEN_MARGIN)
			continue;

		s = &list[firstIndex + n];
		s->y = (int16_t)(128 + py);
		s->size = p->size;
		s->link = firstLink + n + 1;
		s->attr = TILE_ATTR(pal, prio, flipV, flipH, tileBase + p->tile);
		s->x = (int16_t)(128 + px);
		n++;
	}
	return n;
}

/* PRECOMPUTED PIECE TEMPLATES (Job 1, lever 1, this task). obj_emit_pieces()
 * above re-decodes, from ROM, EVERY piece of EVERY drawn frame of EVERY
 * instance, EVERY frame: p->size into (w,h), a flip-conditional select of
 * dx/dy, and TILE_ATTR's shift/OR assembly of pal/prio/flipV/flipH/tile.
 * None of that actually depends on the INSTANCE being drawn -- only on
 * (piece, frame's pivot, flip state, tileBase, pal, prio), which for every
 * migrated type is fixed by the type's OWN class-wide state (piece/pivot
 * are ROM-constant; tileBase only moves when the arena evicts/regrants this
 * type's VRAM residency, a rare event, not a per-frame one; pal/prio are
 * fixed for the type's whole lifetime). obj_build_piece_templates() below
 * computes exactly the same dx/dy/attr arithmetic as obj_emit_pieces()
 * above -- byte for byte, so the two can never silently drift apart -- but
 * OFF the per-frame path: a type calls it once at boot and again only
 * inside its own onBase() callback (i.e. only when its VRAM base actually
 * changes), caching the result in a small RAM array indexed the SAME way
 * ObjFrame.pieceOffset already indexes desc->pieces (see each migrated
 * type's own rebuild_templates() for the exact call pattern). Two arrays
 * per type (flipH=0 and flipH=1), since flipH is the one flip axis every
 * high-instance-count type in this codebase actually varies at runtime
 * (walk-direction turnarounds); flipV is baked as 0 into both, so a type
 * that ever needs flipV!=0 (springs/signpost's own per-instance flipFlag)
 * is not eligible for this fast path and stays on legacy obj_emit_pieces()
 * -- ObjTypeDesc.templatesH0/H1 being NULL (the default -- appended at the
 * END of the struct, so every existing positional initializer that omits
 * them gets NULL for free, C99 6.7.8p21) is exactly the signal
 * obj_type_draw() (obj_generic.c) reads to fall back to the legacy,
 * per-piece-decode path unchanged, so this is purely additive: no existing
 * type's behavior or code changes unless it opts in. Once built, the
 * per-frame, per-piece cost obj_emit_pieces_templated() below pays is
 * exactly what this task's brief asks for: add screen X, add screen Y,
 * cull-check (SAME OBJ_EMIT_OFFSCREEN_MARGIN check as obj_emit_pieces(),
 * same formula, same margin -- the Y-cull fix's semantics are therefore
 * unchanged BY CONSTRUCTION, not by re-derivation), copy, next.
 * ObjPieceTemplate itself is declared in obj_data.h (this file's own
 * include), not here -- so ObjTypeDesc can name it without a forward
 * reference the other way. */

/* Fills out[0..pieceCount) for pieces p[0..pieceCount), ONE flip state and
 * ONE tileBase -- called from a type's onBase()/boot-load path only, NEVER
 * from the per-frame draw path (that is the entire point). flipV is always
 * passed 0 by every real call site today (see this function's own top
 * comment on why); kept as a real parameter rather than hardcoded so a
 * future type with a genuinely fixed (never-per-frame-varying) flipV could
 * still use this to precompute ITS one flip state without duplicating the
 * arithmetic a third time. */
static inline void obj_build_piece_templates(ObjPieceTemplate *out, const ObjPiece *p, uint8_t pieceCount,
                                              uint16_t tileBase, uint8_t pal, uint8_t prio,
                                              int8_t pivotX, int8_t pivotY, uint8_t flipH, uint8_t flipV)
{
	uint8_t i;
	for (i = 0; i < pieceCount; i++, p++) {
		int16_t w = (int16_t)(((p->size >> 2) & 3) + 1) << 3;
		int16_t h = (int16_t)((p->size & 3) + 1) << 3;
		out[i].dx = flipH ? (int16_t)(-pivotX - p->dx - w) : (int16_t)(pivotX + p->dx);
		out[i].dy = flipV ? (int16_t)(-pivotY - p->dy - h) : (int16_t)(pivotY + p->dy);
		out[i].size = p->size;
		out[i].attr = TILE_ATTR(pal, prio, flipV, flipH, (uint16_t)(tileBase + p->tile));
	}
}

/* The fast per-frame path: t[] already holds this flip state's dx/dy/size/
 * attr, precomputed -- the only new arithmetic below is the two adds every
 * INSTANCE genuinely needs (screen position, which changes every entry
 * every frame and cannot be precomputed), the SAME Y/X-cull check
 * obj_emit_pieces() runs (this file's own Job A/OBJ_EMIT_OFFSCREEN_MARGIN
 * comment -- identical formula, identical margin, applied to the identical
 * py/px this function computes, so cull behavior is unchanged for every
 * templated type), and the copy itself. Mirrors obj_emit_pieces()'s own
 * signature/loop shape closely on purpose, so the two are easy to read side
 * by side and verify they can only differ in the ways documented above. */
static inline uint16_t obj_emit_pieces_templated(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                                                  uint16_t maxCount, const ObjPieceTemplate *t, uint8_t pieceCount,
                                                  int16_t sx, int16_t sy)
{
	uint16_t n = 0;

	for (; pieceCount > 0 && n < maxCount; pieceCount--, t++) {
		int16_t py = (int16_t)(sy + t->dy);
		int16_t px = (int16_t)(sx + t->dx);
		VDPSprite *s;

		if (py < -OBJ_EMIT_OFFSCREEN_MARGIN || py >= SCREEN_HEIGHT + OBJ_EMIT_OFFSCREEN_MARGIN)
			continue;
		if (px < -OBJ_EMIT_OFFSCREEN_MARGIN || px >= SCREEN_WIDTH + OBJ_EMIT_OFFSCREEN_MARGIN)
			continue;

		s = &list[firstIndex + n];
		s->y = (int16_t)(128 + py);
		s->size = t->size;
		s->link = firstLink + n + 1;
		s->attr = t->attr;
		s->x = (int16_t)(128 + px);
		n++;
	}
	return n;
}

#endif
