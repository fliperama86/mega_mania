#ifndef OBJ_DATA_H
#define OBJ_DATA_H

#include <stdint.h>

/* Shared piece layout for springs (tools/convert_spring.py's
 * SpringPiece) and the signpost (SignPostPiece): identical fields to
 * Sonic's own SonicPiece (md_src/sonic_data.h) and every other piece
 * table in this codebase -- one hardware-sprite piece's offset, VDP size
 * field and first tile, relative to whichever frame it belongs to. Named
 * once here, rather than once per generated header, so md_src/
 * obj_sprite.c's shared piece-emission loop (springs.c/signpost.c's own
 * doc comments) can take one pointer type instead of two structurally-
 * identical ones -- this pairing was worth making explicit once the 68000
 * program's fixed 512 KB window left no room for the duplicated code that
 * two copies of that loop cost (see obj_sprite.c). */
typedef struct {
	int8_t  dx, dy;
	uint8_t size;
	uint8_t tile;
} ObjPiece;

/* PRECOMPUTED PIECE TEMPLATE (Job 1, lever 1, this task): one piece's
 * "ready to copy" draw record for ONE flip state -- see obj_sprite.h's own
 * obj_build_piece_templates()/obj_emit_pieces_templated() for how these are
 * built and consumed, and ObjTypeDesc.templatesH0/H1 below for how a type
 * opts in. Declared here (not obj_sprite.h, where the functions that use it
 * live) purely so ObjTypeDesc below can name it -- obj_sprite.h includes
 * this header, not the other way around. */
typedef struct {
	int16_t  dx, dy;
	uint8_t  size;
	uint16_t attr;
} ObjPieceTemplate;

/* Generic per-object-type infrastructure (game/md_src/obj_generic.c): the
 * table-driven replacement for rings.c/springs.c/signpost.c's own copies of
 * "x-sorted scene table, sliding camera window, tile-budget check at init,
 * table-count staleness guard, link-chain bookkeeping". A new object type
 * is added by writing its own scene-table entries (or a small synthetic
 * runtime table for a non-scene-file object, the way signpost.c's own
 * always-falling-back-to-4-synthetic-rows does), one ObjTypeDesc, and one
 * decide() function -- nothing else in this skeleton is per-object any
 * more.
 *
 * One frame: which tiles (already an ABSOLUTE VRAM tile index, not an
 * offset into some pixel source array -- see obj_generic.c's own comment on
 * why every migrated type's frame table bakes this in at init time once its
 * VRAM base is known, rather than needing a second "which region" field
 * here) and which pieces (via pieceOffset/pieceCount into the type's own
 * ObjPiece array, itself piece-local i.e. piece->tile is relative to this
 * frame's own tileOffset) make up one drawable pose. duration is the raw
 * RSDK per-frame animator value for object types that animate through more
 * than one frame (0 for a frame that is never auto-timed, e.g. every static
 * pose in this codebase today) -- a per-type decide()/tick() reads it to
 * drive its own timing, the generic engine itself never touches it (decide()
 * is the only per-object hook, see ObjDecideFn below; a type that wants
 * frame-driven timing does that arithmetic itself, the same way springs.c's
 * bounce timer does). */
typedef struct {
	uint16_t tileOffset, pieceOffset;
	uint8_t  tileCount, pieceCount;
	int8_t   pivotX, pivotY;
	uint16_t duration;
} ObjFrame;

/* One entry's per-frame draw decision: which of the type's ObjFrame[] table
 * to show this tick, its flip bits (obj_emit_pieces' own flipH/flipV), and
 * (offX,offY) -- the SPRITE-VS-HITBOX DRIFT fix (this task, Job 2). Several
 * badnik classes move an instance after spawn by adding a live offset to
 * its scene X/Y before running the hitbox touch test (crabmeat/motobug's
 * shared curOffX patrol, chopper's chopperOffY jump arc, buzzbomber's
 * per-instance sign*curMag, newtron's per-instance ntOffX[], batbrain's
 * per-instance brOffX[]/brOffY[] -- see each class's own decide() for the
 * exact formula, already derived from the decomp for the hitbox test long
 * before this fix existed). Before this field pair existed, obj_type_draw()
 * (obj_generic.c) had no way to learn that offset, so it drew every piece
 * at the entry's raw, unmoved scene position while the SH2's own identical
 * offset formula (sh_src/badnik.c, computed independently for collision --
 * see badnik_base.h's own top comment on why the two sides never talk to
 * each other directly) moved the HITBOX -- a real, visible desync: sprite
 * pinned at spawn, hitbox correctly walking/dropping/flying away from it.
 * decide() now returns the SAME offset it already adds to ex/ey for its own
 * touch test (never a second, independently-derived one -- see each class's
 * own decide() comment for "identical formula, both uses"), so sprite and
 * hitbox agree by construction, not by coincidence of two people getting
 * the same number twice. Zero for every class that has no such offset
 * (rings, springs, signpost, spikelog, decoration) -- those decide()
 * functions set both to 0 unconditionally, same as flipH/flipV, so this
 * change is a no-op for them (verified: see this task's own host-harness
 * equivalence proof, comparing every draw decision before/after for every
 * unaffected class across a full camera sweep). frame == OBJ_SKIP means "do
 * not draw this entry at all this frame" (off-screen in Y, already
 * collected, the type not yet triggered, ...) -- obj_type_draw() never
 * reads offX/offY on that path (its own `if (d.frame == OBJ_SKIP) continue`
 * short-circuits first), so a skip decision does not need a meaningful
 * offset, only frame does. */
typedef struct {
	uint16_t frame;
	uint8_t  flipH, flipV;
	int16_t  offX, offY;
} ObjDrawDecision;

#define OBJ_SKIP 0xFFFF

/* The one per-object hook: given this entry's own world position (ex,ey,
 * pulled by the generic engine straight out of the type's entries table --
 * see ObjTypeDesc.entries' own comment for the layout convention that makes
 * that possible with no per-type accessor) and Sonic's just-published
 * position/frame, decide what to draw. entryIndex is the same index the
 * generic engine used to read ex/ey, so a decide() function can re-index
 * its own type-specific table (hitbox, orientation, flip flag, whatever the
 * generic engine has no need to know about) directly, with no need for the
 * engine to pass those fields through generically. state is
 * ObjTypeDesc.state verbatim, for whatever per-type runtime bookkeeping
 * (an active bounce timer, a collected bitmap, ...) decide() needs across
 * calls; the generic engine never reads or writes it itself. */
typedef ObjDrawDecision (*ObjDecideFn)(void *state, uint16_t entryIndex,
                                       int16_t ex, int16_t ey,
                                       int16_t sonicWorldX, int16_t sonicWorldY,
                                       uint16_t sonicFrameIndex);

/* One object type's whole static configuration.
 *
 * entries: an x-sorted table of fixed-size records, EVERY record beginning
 * with `int16_t x, y;` (the rest is opaque to the generic engine, and never
 * read by it) -- the same layout rings.c's ghz_ring_xy/springs.c's
 * SpringEntry already use. Static scene-file-backed types (rings, springs)
 * point this at their generated .bin table directly; a synthetic type with
 * no scene file at all (signpost: one instance, position changes at
 * runtime) points this at a small RAM array it keeps current itself (see
 * signpost.c) -- `const` here only promises the generic engine will not
 * write through it, not that the memory behind it is ROM.
 *
 * recordSize/recordCount: stride through entries, and the compile-time
 * expected row count. countPtr, if non-NULL, is a runtime count word (a
 * generated table's own leading count field) obj_type_init() checks against
 * recordCount at boot -- the exact "cannot go stale in one file and not
 * another" guard rings_init()/springs_init() used to each hand-roll. NULL
 * for a synthetic type with no such generated field to check (signpost).
 *
 * tilePixels/residentTileCount: the type's own resident tile source and
 * count, for obj_type_init()'s upload -- NULL/0 for a type that manages its
 * own tile upload entirely itself (signpost: resident post-bits tiles plus
 * the shared streamed window, see signpost.c).
 *
 * frames/pieces: this type's whole ObjFrame/ObjPiece tables; decide()'s
 * returned frame index selects into frames[], whose pieceOffset/pieceCount
 * select into pieces[].
 *
 * spritePriority: this type's rank in the shared hardware-sprite pool's
 * drop order (md_src/obj_pool.h) -- higher survives a full frame longer,
 * lower gets truncated first. Replaces the old private per-type compile-
 * time cap (RING_SPRITE_CAP and friends stay, but now mean "this type's own
 * natural per-frame maximum", not "this type's permanently reserved slice
 * of the 80-sprite table").
 *
 * palette/drawPriority: TILE_ATTR's pal/prio bits, uniform for every piece
 * this type ever draws (every migrated type today draws at one fixed
 * palette line and one fixed low/high VDP priority; a future type that
 * needs to vary either per-entry would do so through its own decide()
 * result today's ObjDrawDecision does not carry that, so such a type would
 * need this struct extended -- flagged, not encountered yet).
 *
 * marginX: how far outside the screen's left/right edge (in world pixels)
 * an entry can still be worth considering, same role camX-16/camX+16 played
 * in rings.c/springs.c's own hand-written window math.
 *
 * decide/state: see ObjDecideFn's own comment.
 *
 * templatesH0/templatesH1: PRECOMPUTED PIECE TEMPLATES (Job 1, lever 1,
 * this task) -- optional fast-path arrays, parallel to `pieces` (same
 * length, same indexing via ObjFrame.pieceOffset/pieceCount), holding every
 * piece's ready-to-copy draw record for flipH=0 (templatesH0) and flipH=1
 * (templatesH1), flipV always baked as 0 -- see obj_sprite.h's own
 * obj_build_piece_templates()/obj_emit_pieces_templated() for how a type
 * builds and uses them. NULL (the default: appended at the END of this
 * struct, so every existing positional initializer that predates this pair
 * gets NULL for both, C99 6.7.8p21 -- no other file needed to change to
 * keep compiling) tells obj_type_draw() (obj_generic.c) this type has not
 * opted in, or that its d.flipV is not always 0 (springs/signpost's own
 * per-instance flipFlag -- not eligible for this fast path), and to keep
 * using the legacy per-piece-decode obj_emit_pieces() path unchanged. */
typedef struct {
	const void      *entries;
	uint8_t          recordSize;
	uint16_t         recordCount;
	const uint16_t  *countPtr;
	const uint32_t  *tilePixels;
	uint16_t         residentTileCount;
	const ObjFrame  *frames;
	const ObjPiece  *pieces;
	uint8_t          spritePriority;
	uint8_t          palette;
	uint8_t          drawPriority;
	int16_t          marginX;
	ObjDecideFn      decide;
	void            *state;
	const ObjPieceTemplate *templatesH0;
	const ObjPieceTemplate *templatesH1;
} ObjTypeDesc;

#endif
