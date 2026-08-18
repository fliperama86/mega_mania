#ifndef SIGNPOST_H
#define SIGNPOST_H

/* GHZ1's end-of-act signpost, ported from SonicMania/Objects/Global/
 * SignPost.c -- purely visual per this port's architecture (no gameplay
 * effect, no act-clear/score/HUD system exists yet -- docs/green-hill.md's
 * "Known gaps"). Entirely 68000-side, like rings and springs: the slave SH2
 * never learns the signpost exists.
 *
 * GHZ1's signpost is SIGNPOST_DROP (Scene1.bin slot 324, verified against
 * the pack: type=1, x=15792, y=1208, Mania filter kept), boss-dropped in the
 * original (DDWrecker_State_SpawnSignpost, DDWrecker.c:911-923: the boss's
 * own death position overwrites signPost->position.x, Y stays whatever the
 * scene authored). This port has no boss (GHZ1's boss is out of scope, see
 * docs/green-hill.md's "Known gaps"), so triggering is a pre-approved
 * deviation: the signpost drops the moment Sonic's published world X crosses
 * SIGNPOST_X - 64, using the scene's own authored (x,y) directly rather than
 * a boss's death position.
 *
 * Landing Y is SIGNPOST_LANDING_Y below, the scene's own authored y (1208),
 * not a tile-collision scan -- see this file's own former comment (now in
 * signpost.c, next to SIGNPOST_LANDING_Y) for the column-scan attempt and
 * why the scene's authored Y won out.
 *
 * The spin (SignPost_HandleSpin, SignPost.c:220-235) is transcribed
 * directly. This port's own fall is a plain linear Y interpolation run in
 * parallel with that same spin -- not a transcribed value, since the
 * original's real fall is real per-tile physics this port does not
 * reproduce -- landing exactly when the spin settles.
 *
 * Draw path: this is the FIRST type migrated onto md_src/obj_generic.h's
 * table-driven skeleton (obj_data.h's own comment calls out why signpost
 * went first -- one instance, a degenerate window scan). It has no real
 * scene-file table at all (unlike rings/springs): its "entries" are 4
 * synthetic same-position rows (post-top/sidebar/stand plus the animated
 * face plate) that signpost_tick() keeps current in a small RAM array, one
 * ObjDrawDecision per row picking which of a small local ObjFrame table
 * (built once at init from the generated signpost_post[]/signpost_plate[][]
 * tables, see signpost.c) to show. */

#include "md.h"

/* At most 3 post-bit pieces (post top/sidebar/stand, each 1 piece,
 * SIGNPOST_MAX_RESIDENT_PIECES) plus up to SIGNPOST_MAX_STREAM_PIECES face-
 * plate pieces, all drawn every frame the signpost is visible (there is only
 * ever one signpost) -- an exact upper bound, not a measured-plus-headroom
 * figure the way rings.h/springs.h's own caps are, since a single fixed
 * object's piece count is fully known at conversion time. This is now the
 * generic path's per-frame maxCount for this type (obj_generic.h), not a
 * permanent slice of the hardware sprite table -- see md_src/obj_pool.h. */
#define SIGNPOST_SPRITE_CAP 5

/* Upload the resident Post Bits tiles through the shared VRAM arena
 * (md_src/obj_generic.h: registers signpost's post-bits as an arena class,
 * OBJ_PRI_SIGNPOST -- the lowest priority any arena class carries today,
 * matching this file's own doc comment on why the signpost is the most
 * expendable thing in the game -- first-fit allocates its address out of
 * the arena's own free space, obj_arena_init() has to have already run,
 * main.c's boot sequence, and uploads it with one plain vdp_tiles_load()
 * exactly like every pre-arena signpost_init() always did), and reserve the
 * streamed face-plate window at a FIXED address independent of the arena
 * (SIGNPOST_STREAM_BASE, signpost.c: TILE_FONTINDEX - SIGNPOST_MAX_STREAM_
 * TILES) -- that window is not reclaimed, see obj_generic.h's own comment
 * on why. Call after obj_arena_init(). */
void signpost_init(void);

/* The streamed window's absolute VRAM tile base, valid immediately (a fixed
 * address, not an arena grant) -- springs.c's own bounce animation shares
 * this exact window (main.c wires the two together at boot; see springs.h's
 * own doc comment for why one shared slot, not two independent ones, is
 * what the VRAM budget closes with). */
uint16_t signpost_stream_tile_base(void);

/* Advance the fall/spin state machine against Sonic's just-published world
 * X, and mark the streamed window dirty if the resulting face/step changed
 * (signpost_upload() does the actual DMA). Call once per displayed frame,
 * before vdp_wait_vblank.
 *
 * sonicWorldY/sonicFrameIndex are accepted, and ignored (the trigger has
 * only ever been an X-crossing check, SIGNPOST_TRIGGER_X in signpost.c) --
 * this signature match md_src/obj_pool.h's ObjTickFn exactly, the one
 * function-pointer type every registered object type's own OPTIONAL tick
 * hook is stored and invoked through (main.c's OBJ_TYPE_LIST), same
 * reasoning as signpost_draw()'s own widened signature below. */
void signpost_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

/* DMA the streamed window's tiles if signpost_tick() changed them. Call
 * from inside vblank. */
void signpost_upload(void);

/* Append hardware-sprite entries for the signpost (post bits + face plate)
 * if it is currently visible (state != hidden and on screen), same link-
 * chain convention as rings.c/springs.c/sonic_build(). Returns how many
 * list[] entries were written (0 while hidden or off screen).
 *
 * sonicWorldX/sonicWorldY/sonicFrameIndex are accepted, and ignored, purely
 * so this function's signature matches md_src/obj_pool.h's ObjDrawFn exactly
 * -- see springs_draw()'s own doc comment (springs.h) for the identical
 * reasoning; signpost_decide() never reads them. */
uint16_t signpost_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                       uint16_t camX, uint16_t camY,
                       int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

#endif
