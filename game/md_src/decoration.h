#ifndef DECORATION_H
#define DECORATION_H

/* GHZ1's 21 Decoration entities (SonicMania/Objects/Common/Decoration.c):
 * purely decorative, one fixed sprite ("Bridge Post", GHZ/Decoration.bin's
 * only named animation) repeated/rotated per instance. No player
 * interaction anywhere in the source (Decoration_Update only advances its
 * own rotation angle and processes its animator; nothing in this class ever
 * reads or touches EntityPlayer) -- an entirely 68000-side, draw-only
 * feature, same architectural split as rings.c/springs.c's own top comments
 * describe for their own classes.
 *
 * CUT, verified against every one of GHZ1's 21 Mania-mode entries in
 * assets/ghz/decorations.bin (tools/convert_objects.py's DECORATION_SCENE,
 * row_fmt ">hhBBbiiii": x,y,type,direction,rotSpeed,repeatTimes_x/y,
 * repeatSpacing_x/y):
 *   - type is 0 in every row -- GHZ/Decoration.bin has exactly one named
 *     animation ("Bridge Post"), so Decoration_Create's `RSDK.SetSpriteAnimation
 *     (Decoration->aniFrames, self->type, ...)` always selects the same one;
 *     tools/convert_objects.py's own DECORATION_SCENE comment already notes
 *     this. decoration_data.c accordingly generated exactly ONE ObjFrame
 *     (decoration_bridgepost[1]).
 *   - rotSpeed is 0 in every row, so Decoration_Update's
 *     `self->rotation = (self->rotation + self->rotSpeed) & 0x1FF` never
 *     advances and Decoration_Create's `if (self->rotSpeed) self->drawFX |=
 *     FX_ROTATE;` (Decoration.c:49-50) never fires -- no instance ever draws
 *     rotated. Rotation is accordingly NOT ported: nothing in this port's
 *     table has a nonzero rotSpeed to rotate by.
 *   - repeatTimes and repeatSpacing are (0,0) in every row, so
 *     Decoration_DrawSprite's double loop (Decoration.c:112-119) always runs
 *     its y=0..0/x=0..0 bounds exactly once, at drawPos == self->position
 *     with no offset -- the repeat-tiling loop collapses to "draw one
 *     sprite at the entity's own position" for every GHZ1 instance. NOT
 *     ported: nothing in this port's table ever repeats.
 *   - additive/INK_ADD (Decoration_Create:52-58) is TMZ-only
 *     (RSDK.CheckSceneFolder("TMZ1")/("TMZ2")) -- GHZ never sets it. NOT
 *     ported.
 *   - direction is always 0 (FLIP_NONE) or 1 (FLIP_X) across all 21 rows,
 *     never FLIP_Y(2) or FLIP_XY(3) -- Decoration_Create's `self->drawFX |=
 *     FX_FLIP` (Decoration.c:44) requests only horizontal-flip capability
 *     for this stage's own data, matching what this port's ObjDrawDecision.
 *     flipH alone (no flipV) already expresses.
 *   - drawGroup: `RSDK.GetFrameID(&self->animator)` (Decoration.c:64) is
 *     read immediately after `RSDK.SetSpriteAnimation(..., frameID=0)`
 *     (Decoration.c:62), so it is always 0 (false) -- every instance takes
 *     the `Zone->objectDrawGroup[0]` (low) branch, never [1]. This port's
 *     drawPriority is fixed at 0 (low) for exactly this reason, not by
 *     convention alone -- see decoration.c's own ObjTypeDesc initializer.
 *
 * Net effect: this port's Decoration is Decoration_Create/_Draw stripped to
 * exactly what GHZ1 ever exercises -- one static sprite per instance, at a
 * fixed world position, optionally horizontally flipped, drawn at low
 * priority. Registered as ONE row in md_src/main.c's OBJ_TYPE_LIST (see the
 * BATCH ANCHOR: SCENERY row there) -- the only one of this task's four
 * classes that draws anything at all in retail (see sh_src/invisible_block.h/
 * spin_booster.h/corkscrew_path.h for the other three, which draw nothing
 * and live entirely on the slave SH2 instead). */

#include "md.h"

/* Measured worst case over assets/ghz/decorations.bin's own 21 x-sorted
 * entries: at most 5 ever sit inside any single 320-wide camera window (the
 * 7840/7984/7984/8144/8208/8208 cluster, +-16px margin on both sides, same
 * measurement method springs.h/signpost.h's own doc comments describe for
 * their own caps) -- 6 leaves one slot of headroom, same margin those two
 * files chose for their own measured worst case. This is the generic path's
 * own per-frame maxCount for this type (obj_generic.h), not a permanent
 * slice of the hardware sprite table -- see md_src/obj_pool.h. */
#define DECORATION_SPRITE_CAP 6

/* Copy assets/ghz/decorations.bin's own x/y/direction fields (the only
 * fields this port ever reads -- see this file's own top comment) into a
 * small aligned RAM table, one time, via safe byte-at-a-time reads.
 *
 * NOT a direct struct-cast over the generated blob, unlike rings.c's
 * ghz_ring_xy / springs.c's ghz_spring_xy_md: this class's own generated row
 * is 23 bytes (2+2+1+1+1+4*4, DECORATION_SCENE's row_fmt ">hhBBbiiii") --
 * ODD -- so row 1 sits at byte offset 23 from the table base, row 2 at 46,
 * etc.: every ODD-indexed row's x/y fields would land at an ODD address,
 * and the 68000 (like every m68k core) raises an Address Error exception on
 * any misaligned word/long access. rings.c/springs.c's own direct casts are
 * safe only because THEIR rows (4 and 6 bytes respectively) are even, so
 * every row's own base offset stays 2-aligned; this class does not have
 * that property, so it copies through single-byte reads (never misaligned,
 * regardless of row parity) into its own compiler-laid-out array once here,
 * and reads that array, never the raw blob, everywhere else. Registers
 * DECORATION_PAL's resident tile set with the shared VRAM arena
 * (md_src/obj_generic.h) exactly like every other migrated type -- OBJ_PRI_
 * SCENERY, the lowest eviction priority any tenant carries, matching this
 * type's own drop-order rank in main.c's OBJ_TYPE_LIST. NOT boot-loaded
 * synchronously (contrast rings.c's rotation window): GHZ1's first
 * Decoration sits at x=528, outside the very first camera view around the
 * player's own spawn (x=108, half-screen 160px), so the ordinary runtime
 * admission path (obj_arena_tick(), ARENA_LOOKAHEAD_X margin) has time to
 * load it well before the camera ever arrives. On either the generated
 * table's own count mismatch or an arena allocation failure, Decoration is
 * permanently disabled for this run (decoration_draw() becomes a no-op)
 * rather than drawing from a mis-sized table or corrupted VRAM -- same
 * convention every other migrated type in this codebase already follows. */
void decoration_init(void);

/* Per-frame draw call: window assets/ghz/decorations.bin's own x-sorted
 * table against the camera, apply each visible instance's own direction as
 * a horizontal flip, and append its one hardware-sprite piece into list[]
 * starting at list[firstIndex], continuing the same link-chain convention
 * every emitter in this codebase already uses (the caller fixes up the true
 * last entry's link afterward). sonicWorldX/Y/sonicFrameIndex are accepted,
 * unused (ObjDrawFn's own uniform shape, obj_pool.h) -- Decoration has no
 * player interaction at all, see this file's own top comment. Returns how
 * many list[] entries were written. */
uint16_t decoration_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                         uint16_t camX, uint16_t camY,
                         int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

#endif
