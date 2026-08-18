#ifndef SPRINGS_H
#define SPRINGS_H

/* GHZ1's 35 springs: drawing is entirely 68000-side (this port's
 * architecture; the slave SH2 owns spring PHYSICS from its own table,
 * sh_src/spring.c -- the two never communicate). See tools/
 * convert_spring.py (Global/Springs.bin -> assets/spring/ + this file's
 * data tables) and tools/convert_springs.py (the scene's spring table ->
 * assets/ghz/springs.bin, same x-sorted-table shape as tools/
 * convert_rings.py's rings.bin).
 *
 * Draw path: table-driven, on md_src/obj_generic.h's shared skeleton (see
 * obj_data.h's own comment) -- springs.c's own decide() hook now also owns
 * the bounce trigger this file used to document as cut.
 *
 * BOUNCE ANIMATION RESTORED (this task, part 4): the observational
 * AABB-triggered bounce the original pre-approved design called for was cut
 * only because the 68000's fixed 512 KB cartridge window had no room left
 * for the trigger/animate/stream code once every art/data economy still
 * left the compiled result too big (see this file's git history / the
 * task report that made the cut, and docs/green-hill.md). That window has
 * since gained ~484 KB of free space (moving every asset into cartridge
 * bank 1, tools/gen_assets.py) with room to spare, so the cut's own reason
 * is gone: every spring now detects an AABB overlap with Sonic's own
 * current-frame hitbox (springs.c's own spring_touches_sonic(), the same
 * symmetric-box test rings.c's ring_touches_sonic() already uses -- purely
 * observational, independent of sh_src/spring.c's own physics-side
 * collision test, exactly as this file always documented the design would
 * be) and switches to its streamed bounce pose (assets/spring/
 * stream_tiles.bin, already converted, previously unused on the cart) for
 * a duration derived from the converted frame's own RSDK duration value and
 * Spring_State_*'s own animator speed (0x80, Spring.c:167/195/243/286/342)
 * -- see springs.c's own comment for the exact tick arithmetic. Only ONE
 * spring's bounce pose is ever resident at a time (the shared streamed VRAM
 * window this file goes back to sharing with signpost.c -- see this file's
 * own comment on SPRING_BOUNCE_TICKS/the shared window below, and
 * signpost.h's own doc comment): newest trigger wins the slot, matching
 * the shared window's original documented eviction rule. sh_src/spring.c's
 * own physics (collision, launch velocity, player state/animation) is
 * completely untouched by any of this -- the two mechanisms still never
 * talk to each other. */

#include "md.h"

/* Measured worst case: at most 3 of GHZ1's 35 springs ever sit inside any
 * single 320x224 camera window (tools/convert_springs.py's own x-sorted
 * table, scanned the same way rings.c's own 32-ring measurement was); 6
 * leaves headroom without spending sprite budget rings.c/sonic.c/signpost.h
 * need. This is now the generic path's per-frame maxCount for this type
 * (obj_generic.h), not a permanent slice of the hardware sprite table --
 * see md_src/obj_pool.h. */
#define SPRING_SPRITE_CAP 6

/* Upload the resident (rest pose) tiles for all 3 orientations through the
 * shared VRAM arena (md_src/obj_generic.h: registers springs' resident set
 * as an arena class, OBJ_PRI_SPRING, first-fit allocates its address out of
 * the arena's own free space -- obj_arena_init() has to have already run,
 * main.c's boot sequence -- and uploads it with one plain vdp_tiles_load()
 * exactly like every pre-arena springs_init() always did), and reserve this
 * type's stream state (the shared STREAM window is a separate, unreclaimed
 * fixed allocation still owned by signpost_init(); call
 * springs_set_stream_base() with its signpost_stream_tile_base() right
 * after). Runtime-checks assets/ghz/springs.bin's own leading count against
 * the compile-time SPRING_COUNT (springs.c), same "cannot go stale in one
 * file and not another" convention rings_init() already uses; on that or
 * an arena allocation failure, springs are permanently disabled for this
 * run (every entry point becomes a no-op) rather than uploading over
 * whatever else the arena already holds. */
void springs_init(void);

/* The shared streamed window's absolute VRAM tile base -- pass
 * signpost_stream_tile_base()'s return value here once, right after both
 * springs_init() and signpost_init() have run (main.c's boot sequence). */
void springs_set_stream_base(uint16_t base);

/* Run the AABB bounce trigger against Sonic's just-published world position
 * and current-frame hitbox, advance whichever spring is currently bouncing
 * one tick closer to reverting to its resident pose, and mark the shared
 * stream window dirty if the frame it should hold changed. Call once per
 * displayed frame, before building this frame's sprite list (same "tick
 * decides, draw reads, upload DMAs in vblank" split signpost.c already
 * uses). */
void springs_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

/* DMA the shared stream window's tiles if springs_tick() changed them. Call
 * from inside vblank, alongside signpost_upload() -- see this file's own
 * doc comment for why the two safely share one window (x-exclusive scene
 * geometry: no GHZ1 spring sits anywhere near where the signpost lives). */
void springs_upload(void);

/* Append hardware-sprite entries for every spring visible in the camera's x
 * window into list[], starting at list[firstIndex], continuing the link-
 * chain convention rings.c/sonic_build() already use (main.c fixes up the
 * true last entry's link to 0 afterward). Returns how many list[] entries
 * were written.
 *
 * sonicWorldX/sonicWorldY/sonicFrameIndex are accepted, and ignored, purely
 * so this function's signature matches md_src/obj_pool.h's ObjDrawFn exactly
 * -- the same one function-pointer type every registered object type's own
 * draw call is stored and invoked through (main.c's OBJ_TYPE_LIST). spring_
 * decide() never reads them (springs.c's own spring_touches_sonic() already
 * ran, inside springs_tick(), before this is ever called); widened here
 * instead of giving springs.c a second, differently-shaped wrapper function
 * whose only job would be dropping three arguments before calling this one. */
uint16_t springs_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                      uint16_t camX, uint16_t camY,
                      int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

#endif
