#ifndef OBJ_POOL_H
#define OBJ_POOL_H

#include "md.h"

/* The Mega Drive hardware sprite table: 80 entries, hard, no more ever
 * possible regardless of how many object types this game grows. Used to be
 * enforced by every object type reserving its own private compile-time cap
 * and main.c sizing list[] as their sum (79 of 80, one slot of headroom,
 * with roughly 21 more object types still to come and at least one
 * measured future spot -- a double rope bridge, each plank its own sprite
 * -- needing +27 at once): a static reservation that has to sum to <=80
 * forever, which cannot survive a roster this size. This pool replaces that
 * with one shared 80-slot budget, allocated per frame by priority instead
 * of reserved per type forever. */
#define HW_SPRITE_CAP 80

/* Default drop-priority ranking for today's five emitters (Sonic excluded,
 * see below). LOWER drops FIRST when a frame is over budget.
 *
 * This ranking is a real gameplay tradeoff, not a mechanical default, and
 * it is flagged in this task's own final report for the user's sign-off
 * rather than picked silently, per that report's own compromises section:
 * a still-visible RING is a missed collectible, a still-visible SPRING is a
 * platforming hazard/aid the player needs to see coming, a SPARKLE is pure
 * already-collected juice with zero gameplay cost if it never appears, and
 * the SIGNPOST is purely decorative and exists for at most the last few
 * seconds of the act. Proposed order, most to least expendable:
 * signpost < sparkle < spring < ring. Every future object type not listed
 * here defaults to OBJ_PRI_SIGNPOST (the lowest, i.e. dropped before any of
 * today's five) until its own author picks a deliberate rank -- a new,
 * untested object type should never silently outrank the ones already
 * proven to matter. */
/* Rank 0 is reserved for purely decorative scenery: it carries no gameplay
 * meaning at all, so it is the first thing that should ever vanish.
 *
 * Ranks 1-4 are the user's own confirmed ordering for the objects that
 * shipped first (2026-08-17).
 *
 * Ranks 5-7 sit ABOVE those deliberately, on one rule: anything that can
 * hurt the player, kill the player, or carry the player must never be
 * invisible while still being solid. An unseen spike that still wounds, or
 * an unseen platform the player falls through, reads as a broken game rather
 * than as a dropped sprite -- whereas a missing ring or sparkle merely reads
 * as a missing decoration. Platforms rank highest because their absence
 * costs the player a life through no fault of their own. */
#define OBJ_PRI_SCENERY  0
#define OBJ_PRI_SIGNPOST 1
#define OBJ_PRI_SPARKLE  2
#define OBJ_PRI_SPRING   3
#define OBJ_PRI_RING     4
#define OBJ_PRI_BADNIK   5
#define OBJ_PRI_HAZARD   6
#define OBJ_PRI_PLATFORM 7
/* Sonic is never one of these numbers at all -- see obj_pool_arbitrate's
 * own comment for why it is reserved outside the arbitration entirely
 * rather than assigned a priority high enough to always win one. */

/* One candidate block: the entries a single emitter (Sonic excluded --
 * see this file's own note below -- sparkles, or one ObjTypeDesc's own
 * obj_type_draw() output) WOULD draw this frame if the hardware table had
 * no limit at all, already written into a scratch VDPSprite range (RAM,
 * not the hardware-limited kind -- see main.c's own scratch buffer, sized
 * to the SUM of every type's natural per-frame cap, same as list[] used to
 * be sized, except this is ordinary working memory now, not the 80-sprite
 * hardware table itself) at candidate-collection time, before this frame's
 * true total is known.
 *
 * priority: this block's rank in the drop order -- LOWER is dropped
 * FIRST. Assigning these numbers across sparkles/rings/springs/signpost/
 * every future object type is a real gameplay tradeoff (which becomes
 * invisible first when a frame is over budget), not a mechanical one; see
 * main.c's OBJ_PRI_* constants for the proposed default and this task's
 * own final report for why it needs the user's sign-off, not a silent
 * pick. */
typedef struct {
	uint16_t count;
	uint8_t  priority;
} PoolBlock;

/* Drops entries from the lowest-priority block(s) first (fully draining one
 * before touching the next-lowest), truncating each affected block's own
 * COUNT (never reordering which of that block's own candidates survive --
 * the earliest-scanned ones do, the same convention every sliding window in
 * this codebase already keeps for its own natural cap) until the sum of
 * every block's count is <= budget. A tie in priority drains the earlier
 * array entry's block first (an arbitrary, documented tie-break -- with
 * today's five blocks all at distinct priorities it never fires). Does
 * nothing if the blocks already fit.
 *
 * Sonic is not a block here at all: main.c reserves his exact piece count
 * out of HW_SPRITE_CAP UNCONDITIONALLY before this ever runs, and calls
 * this with budget = HW_SPRITE_CAP - sonicCount over every other block --
 * the one guarantee this whole mechanism exists to keep ("Sonic must never
 * be dropped") holds structurally, not by giving Sonic a priority number
 * high enough to always win an arbitration it is never entered into. */
void obj_pool_arbitrate(PoolBlock *blocks, uint8_t numBlocks, uint16_t budget);

/* One object type's per-frame draw call, stored in the shared registration
 * table (main.c's OBJ_TYPE_LIST) and invoked through this ONE pointer type
 * instead of by name -- this is what lets main.c's candidate-collection loop
 * be a loop instead of one hand-written call site per type. Every migrated
 * type's own _draw()/_update() entry point already has exactly this shape:
 * rings_update() natively (it already threads sonicWorldX/Y/sonicFrameIndex
 * through to its decide() hook, which uses them for the touch test), and
 * springs_draw()/signpost_draw() widened to match, ignoring the three Sonic
 * params their own decide() functions never read (spring_decide()/
 * signpost_decide() already (void)-cast every one of them) -- so no per-type
 * shim function was needed to store either in this one table. */
typedef uint16_t (*ObjDrawFn)(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                               uint16_t camX, uint16_t camY,
                               int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

/* One object type's OPTIONAL per-frame pre-step -- springs' AABB bounce
 * trigger, signpost's fall/spin state machine -- run once per displayed
 * frame, in table order, before any type's draw() call (same "tick decides,
 * draw reads" split every migrated type already documents). NULL for a type
 * with no such pre-step of its own: rings.c folds its equivalent bookkeeping
 * (advancing the shared ring-rotation frame, stashing Sonic's current-frame
 * hitbox for ring_decide() to read) directly into rings_update() itself,
 * since that only needs recomputing once per call, not once per candidate --
 * that stays a rings.c implementation detail, invisible here and to main.c's
 * generic loop. */
typedef void (*ObjTickFn)(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

/* One row of the shared object registration table (main.c's OBJ_TYPE_LIST
 * macro): everything main.c's per-frame loop needs to know about one object
 * type, without knowing its name. capacity is this type's own natural
 * per-frame cap -- what used to size main.c's scratch[] region and one
 * PoolBlock by hand (RING_SPRITE_CAP and friends); priority is this type's
 * rank in the shared pool's drop order (OBJ_PRI_* above). This is the DROP
 * order, not the DRAW order: draw order (which entry's sprites land where in
 * the final link chain, deciding what overlaps what on real MD/32X hardware)
 * is OBJ_TYPE_LIST's own row order, kept separate and explicit for exactly
 * that reason -- see main.c's own comment on OBJ_TYPE_LIST for why the two
 * orderings must never be conflated. Sonic and the ring sparkles are NOT
 * rows here -- see main.c's own comment on why both stay hand-written
 * outside this table. */
typedef struct {
	ObjDrawFn draw;
	ObjTickFn tick;
	uint16_t  capacity;
	uint8_t   priority;
} ObjRegistration;

#endif
