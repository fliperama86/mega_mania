#ifndef BREAKABLEWALL_H
#define BREAKABLEWALL_H

/* Common/BreakableWall.c: GHZ1's 23 Mania-mode BreakableWall entities
 * (tools/gen_assets.py's ghz_breakablewalls manifest entry -- no tiles.bin:
 * this class carries almost no art of its own).
 *
 * THE MECHANISM (BreakableWall_Draw_Tile, BreakableWall.c:286-291,
 * BreakableWall_Break, :628-699): the object never draws its own sprite in
 * retail play (BreakableWall_Draw_Wall/_Floor's TicMark corner markers are
 * `self->visible = DebugMode->debugActive`-gated, i.e. debug-only -- this
 * port's own SceneRecipe comment on BREAKABLEWALL_SCENE already establishes
 * this). What the player actually sees IS the level's own real FG Low/FG
 * High tile art at that position: the object is solid (or not) purely
 * because the STAGE's own tile collision data says so at that cell, and
 * "breaking" it is RSDK.SetTile(...,-1) -- overwriting the level's own
 * tilemap cell to empty, both its collision AND its graphic, at the exact
 * spot the wall occupies. Nothing about this needs new tile art or a new
 * VRAM budget; it needs the existing, already-resident stage tileset's
 * cells to become mutable at 23 known locations.
 *
 * THIS PORT'S VERSION: the stage's own map data (ghz_map/ghz_map_fgh) is
 * ROM, immutable at runtime, and is streamed into Plane A/B fresh every time
 * the camera revisits a column (md_src/main.c's draw_block_column/row,
 * reading straight from ROM every call) -- so a broken cell cannot be
 * "erased" by writing the VDP plane once; the very next time the camera
 * scrolls that column back into view, draw_block_column/row would redraw
 * the ROM's original (still-solid) tile over it. This file's own
 * breakablewall_tile_override() is main.c's hook for exactly this: a tiny
 * (<=23-entry) table of currently-broken block cells that draw_block_column/
 * row consult and force to the map's own "empty" sentinel (block index 0,
 * already the same fallback every out-of-bounds cell uses) instead of the
 * ROM value, on every single redraw, not just the one at break time.
 *
 * PERSISTENT STATE: each side (this file, 68000/draw; sh_src/breakablewall.c,
 * SH2/collision) keeps its OWN independent 23-bit broken bitmap -- there is
 * no spare comm-protocol bit to carry one shared flag across (sh_src/comm.h's
 * own "fully allocated" note), so both sides INFER "was this wall just
 * broken" locally, off already-published bus data (world X/Y, animation
 * frame index) the same observational way rings.c's lost-ring hit-detection
 * and springs.c's own bounce trigger already do. Both use the IDENTICAL
 * simplified rule (see this file's own comment on breakablewall_tick()) so
 * the two bitmaps can only ever disagree by the ~1-frame comm latency
 * between "the SH2 saw the touch" and "the 68000 saw it published" -- never
 * permanently, since breaking is monotonic (never un-breaks) on both sides.
 *
 * COMPROMISES, stated up front (see breakablewall_tick()'s own comment for
 * the full derivation):
 *   - Every instance is treated as BREAKWALL_TYPE_WALL (single-hit, whole-
 *     block break). 22 of Act 1's 23 instances already ARE that type; the
 *     1 BREAKWALL_TYPE_BURROWFLOORUP instance (a multi-hit, shrinking-
 *     hitbox mechanic, BreakableWall_CheckBreak_BurrowFloorUp) is folded
 *     into the same single-hit rule rather than its own real state machine.
 *   - The break condition drops BreakableWall_CheckBreak_Wall's own extra
 *     onGround/groundVel>=0x48000 gate (Wall's own canBreak test,
 *     BreakableWall.c:321): worldVelocity/onGround are not on the comm wire
 *     at all, so the 68000 side could never verify them even if the SH2
 *     side kept enforcing them, and a strict-on-one-side-only gate risks
 *     the two bitmaps disagreeing about whether a touch actually broke the
 *     wall. Both sides instead use the SAME simplified rule (attacking +
 *     touching), which both CAN observe identically -- see that function's
 *     own comment for the exact substitution and its cost (a slow airborne
 *     roll can now break a wall the original would not have).
 *   - onlyKnux/onlyMighty are not read at all: verified against this
 *     stage's own converted data (23/23 rows have both = 0), so this is not
 *     an approximation, just dead data for this act.
 *   - No flying debris (BreakableWall_Break's per-tile trig-computed
 *     scatter, :628-699): a broken wall's cells simply go empty, with no
 *     spawned entity animating outward. A real, visible cut -- flagged, not
 *     silently smoothed over. */
#include "md.h"

#define BREAKABLEWALL_COUNT 23

void breakablewall_init(void);

/* Per-object pre-step (obj_pool.h's ObjTickFn): independently infers this
 * side's own broken bitmap off worldX/worldY/frameIndex, the same published
 * bus fields every other observational hazard already reads. */
void breakablewall_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

/* Always returns 0 -- BreakableWall draws no sprite of its own (see this
 * file's own header comment). Registered anyway, matching obj_pool.h's
 * ObjDrawFn shape, so OBJ_TYPE_LIST's uniform row macro needs no special
 * case for a class with no sprite output. */
uint16_t breakablewall_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                            uint16_t camX, uint16_t camY,
                            int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

/* main.c's own hook, called from draw_block_column()/draw_block_row() right
 * where each cell's raw ghz_map/ghz_map_fgh value is read -- see this file's
 * own header comment for why the override has to be re-applied on every
 * redraw, not just once at break time. layer 0 = FG Low (ghz_map/Plane A),
 * 1 = FG High (ghz_map_fgh/Plane B) -- matches BreakableWall_Create's own
 * `self->priority == BREAKWALL_PRIO_HIGH ? fgLayer[1] : fgLayer[0]` split
 * (BreakableWall.c:67). Returns 1 if (blockX,blockY) on that layer falls
 * inside a currently-broken instance's own footprint (caller should use
 * block index 0, the map's own existing "out of range" empty sentinel,
 * instead of the ROM value), 0 otherwise. A short linear scan over
 * <=BREAKABLEWALL_COUNT entries -- cheap, called from the same streaming
 * loop that already does one array read per cell. */
uint8_t breakablewall_block_override(uint16_t blockX, uint16_t blockY, uint8_t layer);

#endif
