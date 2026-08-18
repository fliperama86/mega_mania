#ifndef OBJ_GENERIC_H
#define OBJ_GENERIC_H

/* The generic table-driven object skeleton every ObjTypeDesc (obj_data.h)
 * runs on: init-time tile upload + staleness guard, a binary-search camera
 * window (replaces the persistent incremental sliding window rings.c used
 * to hand-roll and springs.c never had at all -- springs.c linear-scanned
 * all 35 entries every frame instead, an asymmetry that existed only
 * because springs.c was written after rings.c; this makes every migrated
 * type's window O(log n) with no per-type extra state to carry frame to
 * frame), and the decide()-driven piece-emission loop. See obj_data.h's own
 * comments for the exact contract each field/hook carries. */

#include "md.h"
#include "obj_data.h"

/* Upload desc->tilePixels (if non-NULL) at firstTile, after checking the
 * tile budget and, if desc->countPtr is non-NULL, that the generated
 * table's own leading count word still matches desc->recordCount -- same
 * "cannot go stale in one file and not another" guard every hand-written
 * *_init() used to duplicate. On any failure *live is left false and
 * nothing is uploaded (the type is permanently disabled for this run,
 * exactly like today's rings_init()/springs_init() on their own failure
 * path); on success *live is set true. Returns the next free VRAM tile
 * (firstTile unchanged on failure, or when tilePixels is NULL -- a type
 * managing its own upload, e.g. the signpost, calls this only for the
 * staleness check, or not at all).
 *
 * UNUSED as of the VRAM tile residency arena (this file's own comment
 * above obj_arena_init): rings.c/springs.c/signpost.c all moved their
 * upload off this and onto the arena (obj_arena_boot_load/boot_done),
 * since a fixed-offset "upload once at firstTile forever" is exactly what
 * the arena replaces. Kept, not deleted: still a correct, simpler primitive
 * for some future type that genuinely wants permanent, non-evictable
 * residency with no arena bookkeeping at all -- flagged here rather than
 * silently left orphaned, since nothing in this codebase calls it today. */
uint16_t obj_type_init(const ObjTypeDesc *desc, uint8_t *live, uint16_t firstTile);

/* Binary-search desc->entries (x ascending, recordCount rows) for the
 * half-open index range whose x falls in
 * [camX - desc->marginX, camX + SCREEN_WIDTH + desc->marginX). */
void obj_type_window(const ObjTypeDesc *desc, uint16_t camX, uint16_t *lo, uint16_t *hi);

/* Walk every entry in the camera's x window (obj_type_window above), call
 * desc->decide() for each, and emit hardware-sprite pieces for every
 * decision that isn't OBJ_SKIP into list[] starting at list[firstIndex],
 * continuing the link-chain convention every emitter in this codebase uses
 * (the caller fixes up the true last entry's link afterward). Stops once
 * maxCount entries have been written -- the type's own natural per-frame
 * cap (e.g. RING_SPRITE_CAP), not a hardware limit; the shared 80-sprite
 * budget is arbitrated separately, over this call's *output*, by
 * md_src/obj_pool.h. Returns how many list[] entries were written. Caller
 * is responsible for checking its own liveness flag first (obj_type_init's
 * *live) -- this function does not know about it, so every entry point in
 * this codebase still reads "every entry point becomes a no-op" off one
 * flag the same way it always has. */
uint16_t obj_type_draw(const ObjTypeDesc *desc, VDPSprite *list,
                       uint16_t firstIndex, uint16_t firstLink, uint16_t maxCount,
                       uint16_t camX, uint16_t camY,
                       int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex);

/* ---- Shared VRAM tile residency arena ---------------------------------
 *
 * The tile budget problem this solves: TILE_FONTINDEX - TILE_USERINDEX is
 * 1296 tiles total, and today's three "always resident" consumers of it
 * (stage 1036, Sonic's own per-frame window 33, rings 156, spring+signpost
 * 43 resident + a 24-tile shared stream window they already manage
 * themselves -- see springs.h/signpost.h) leave 4 tiles free. The upcoming
 * roster's converted art is 2638 tiles across 12 classes, several of which
 * (motobug 387, itembox 224, newtron 284, buzzbomber 313, chopper 384,
 * crabmeat 408, platform 252 -- tools/gen_assets.py's own manifest) alone
 * exceed any budget this reclaim can produce, so this arena's job is NOT
 * "make everything fit" -- nothing can. It is: hold as many camera-nearby
 * classes resident as the reclaimed space allows, load/evict them
 * automatically as the camera approaches/leaves, upload amortized so no
 * single frame pays for a whole class at once, and when a class genuinely
 * cannot fit, refuse it deterministically (obj_arena_refused_count below)
 * rather than silently corrupting whatever else is resident.
 *
 * Reclaimed budget: rings' 156 resident tiles and spring+signpost's 43
 * resident tiles (their existing 24-tile shared STREAM window is untouched
 * -- it is already minimal, already amortized-by-nature at 24 tiles per
 * burst, and event-triggered rather than camera-distance-triggered, a
 * different, already-solved problem) move from "uploaded once at boot,
 * never freed" to arena-managed slots, freeing 199 tiles that combine with
 * the 4 already-free tiles into a 203-tile arena. See main.c's boot
 * sequence for the exact address arithmetic.
 *
 * Per-class residency, not per-frame: a class's ENTIRE tile sheet (every
 * frame/pose it can ever show) is loaded as one unit and stays resident for
 * as long as the class is camera-relevant, the same granularity rings'
 * always-resident upload already used -- NOT Sonic's own "one frame's worth
 * at a time" streaming (sonic.c), because unlike Sonic (one instance, one
 * frame shown at a time, this program always knows which) a class can have
 * many simultaneous instances at independent animation phases (SpikeLog:
 * 61 instances sharing one 176-tile sheet), so anything less than the whole
 * sheet resident could not draw an arbitrary instance's current frame.
 *
 * Loading is amortized at ARENA_TILES_PER_FRAME tiles per vblank (see
 * obj_generic.c's own comment on that constant for where the number comes
 * from), at most ONE class loading at a time -- a second class whose window
 * opens while another is still loading simply waits its turn, same
 * "one shared window, newest/next in line wins the slot" rule the existing
 * spring/signpost stream window already uses.
 *
 * Eviction is immediate, not amortized: the instant a resident class's
 * window (entries +/- lookaheadX) stops overlapping the camera, onLive(0)
 * fires and its VRAM range returns to the free pool THIS tick -- there is
 * nothing to stream out, only tiles to stop reading. */

typedef struct {
	/* Same x-sorted "every record starts with int16 x,y" table convention
	 * ObjTypeDesc.entries already uses (obj_data.h) -- often literally the
	 * same table (rings/springs reuse their own ObjTypeDesc's entries here). */
	const void      *entries;
	uint8_t          recordSize;
	uint16_t         recordCount;

	/* This class's whole tile sheet and its length -- what gets uploaded,
	 * ARENA_TILES_PER_FRAME tiles at a time, once the arena grants it space.
	 *
	 * tilePixels == NULL is a second, deliberate meaning (distinct from
	 * ObjTypeDesc.tilePixels==NULL in obj_data.h, which means "this type's
	 * DRAW path never touches the arena at all" -- this one still allocates
	 * arena space, it just does not want the arena's own one-shot copy):
	 * admission grants [base, base+tileCount) and calls onLive(1)
	 * IMMEDIATELY, with no amortized wait, because there is no fixed blob to
	 * copy -- the caller manages every byte of that range's content itself,
	 * from its own separate upload path, forever, not just once at grant
	 * time. This is what obj_anim_window_register() (further down this file)
	 * registers its own reservation as: the arena still owns admit/evict-by-
	 * camera-proximity, priority arbitration and address bookkeeping for
	 * that range exactly as for any other class, it just never runs
	 * obj_arena_upload()'s chunked fill against it (see that function's own
	 * comment). A tileCount tiles' worth of address space is reserved and
	 * handed over via onBase the same as always; only the "wait for content"
	 * step is skipped, because there is no single fixed content to wait for. */
	const uint32_t  *tilePixels;
	uint16_t         tileCount;

	/* World pixels of margin added on BOTH sides of entries' own x window
	 * (same role as ObjTypeDesc.marginX, just sized for "loading has to
	 * finish before the camera arrives" instead of "visible on screen") --
	 * see main.c's own per-class arithmetic for how this is derived from
	 * tileCount, ARENA_TILES_PER_FRAME and the camera's own top scroll
	 * speed (sh_src/camera.c's CENTER_BOUNDS_X). */
	int16_t          lookaheadX;

	/* Eviction rank when the arena has to make room for a class that would
	 * otherwise refuse: LOWER priority is evicted first. Reuses obj_pool.h's
	 * OBJ_PRI_* constants on purpose -- the same "how expendable is this"
	 * judgment call, just arbitrating VRAM tiles instead of hardware sprite
	 * slots, so one ranking serves both without inventing a parallel scale. */
	uint8_t          priority;

	/* Fires once, the instant this class is (re)granted a VRAM base --
	 * before any tile of it is actually correct -- so the class can rebase
	 * its own frame tables the way springs_set_stream_base() already does
	 * for the shared stream window. Never NULL in practice: a class with no
	 * frame table to rebase still needs SOMEWHERE to learn its own base. */
	void           (*onBase)(uint16_t vramBase);

	/* Fires with live=1 the instant the class finishes uploading (safe to
	 * draw from the very next call to this class's own draw function), and
	 * live=0 the instant it is evicted (MUST stop drawing before the next
	 * frame -- its VRAM range may already belong to someone else). */
	void           (*onLive)(uint8_t live);
} ArenaClassDesc;

#define ARENA_INVALID_SLOT 0xFF

/* Tiles uploaded per frame for the class currently mid-load. NOT measured on
 * this project's own hardware (no cycle-counter/logic-analyzer run against
 * the Neptune or the Mark I cart for this task) -- inferred from two
 * independent, cross-checked sources and then given a real safety margin on
 * top of both:
 *
 *   1. Community-published Genesis VDP timing (rasterscroll.com's "Inner
 *      Workings of the VDP"): "just 18 bytes can be written to VRAM each
 *      scanline" in active display -- this EXACT figure is independently
 *      already in this project's own docs/hardware-budget.md ("A tile DMA
 *      placed [outside vblank] gets about eighteen bytes a scanline"),
 *      which is what makes the same source's vblank figure trustworthy
 *      enough to build on: "~7 KB per vblank via DMA, about half that via
 *      plain 68000 writes" -- roughly 3584 bytes, 112 32-byte tiles, if the
 *      WHOLE vblank were spent on nothing else at all.
 *
 *   2. This codebase's own existing, currently-shipping worst case: Sonic's
 *      own SONIC_MAX_FRAME_TILES=33-tile window (sonic.c) re-uploads EVERY
 *      frame, unconditionally, through the same vdp_tiles_load() CPU-write
 *      path this arena uses, inside the same vblank that also writes the
 *      80-entry hardware sprite table and (occasionally) the spring/
 *      signpost 24-tile shared stream burst -- and the ROM boots and runs
 *      reliably today with exactly that combined cost. That is this
 *      project's own empirical anchor, not a datasheet number.
 *
 * 16 tiles (512 bytes) is HALF of the already-proven-safe 33-tile anchor,
 * chosen deliberately conservative rather than pushed toward the ~112-tile
 * inferred ceiling: the worst realistic frame (Sonic's 33 + a full 80-entry
 * sprite table write, ~640 bytes + this arena's 16 + a coincident
 * spring/signpost 24-tile burst) totals ~2984 of the inferred ~3584-byte
 * budget, leaving real margin against a figure this project has not itself
 * measured. See this task's own final report for the full arithmetic and
 * the explicit flag that this number wants a real hardware timing pass
 * before it is trusted anywhere near the ceiling. */
#define ARENA_TILES_PER_FRAME 16

/* sh_src/camera.c's CENTER_BOUNDS_X (0x100000, 16.0 px in 16.16 fixed) --
 * the camera's own hard per-frame cap, the fastest camX can ever move in one
 * tick regardless of Sonic's own velocity (a spring launch or a roll down a
 * slope can both exceed the player's own top speed; the camera's follow
 * code clamps to this cap regardless -- Camera_State_FollowXY's own
 * CENTER_BOUNDS_X clamp, sh_src/camera.c). This is the "game's maximum
 * scroll speed" the lookahead arithmetic below is built on. */
#define ARENA_MAX_SCROLL_PX 16

/* Frames needed to amortize-load `tiles` tiles at ARENA_TILES_PER_FRAME per
 * frame, plus one frame of scheduling slack (admission is decided once per
 * tick, so a class whose window opens right at a tick boundary should not
 * come up one tile short against a camera scrolling at the hard per-frame
 * cap the entire time it takes to load). */
#define ARENA_LOAD_FRAMES(tiles) \
	(((tiles) + ARENA_TILES_PER_FRAME - 1) / ARENA_TILES_PER_FRAME + 1)

/* The lookahead margin, in world pixels, a class's own ArenaClassDesc.
 * lookaheadX needs so loading always finishes before the camera can
 * possibly arrive: frames-to-load * the camera's own hard per-frame cap.
 * Every migrated class computes its lookaheadX with this macro rather than
 * a hand-picked constant, so the number can never silently drift out of
 * sync with its own tile count. */
#define ARENA_LOOKAHEAD_X(tiles) (ARENA_LOAD_FRAMES(tiles) * ARENA_MAX_SCROLL_PX)

/* Set the arena's address range. Call once, before any obj_arena_register(). */
void obj_arena_init(uint16_t base, uint16_t size);

/* Register one class. Returns its slot handle for obj_arena_boot_load/
 * obj_arena_boot_done below, or ARENA_INVALID_SLOT if the (compile-time
 * fixed) slot table itself is full -- a configuration mistake, not a
 * runtime VRAM condition. Touches no VRAM and allocates no arena space:
 * the class starts EVICTED, exactly as if its window were nowhere near the
 * camera, until either obj_arena_boot_load (below) or a later
 * obj_arena_tick() admits it. */
uint8_t obj_arena_register(const ArenaClassDesc *desc);

/* Boot-only synchronous admission: first-fit slot->tileCount tiles out of
 * the arena's currently free space (called in registration order against
 * an empty arena, so it hands back the same addresses every boot), fire
 * onBase with the granted base, and return that base so the caller can run
 * its OWN plain vdp_tiles_load() exactly as every *_init() already does --
 * see rings_init()'s own comment for why boot keeps doing the upload
 * itself rather than going through the amortized path. Returns 0xFFFF if
 * even an empty arena has no room (a TILE_ARENA_SIZE misconfiguration --
 * there is no camera yet at this point in main() to wait on). Caller must
 * follow with obj_arena_boot_done() once its own upload has actually
 * finished. */
uint16_t obj_arena_boot_load(uint8_t slot);

/* Marks a boot-loaded slot fully resident and fires onLive(1). Call right
 * after the vdp_tiles_load() that obj_arena_boot_load()'s returned base was
 * for. */
void obj_arena_boot_done(uint8_t slot);

/* Per-frame residency service: evict any resident/loading class whose
 * window (entries +/- lookaheadX) no longer overlaps the camera's screen
 * (onLive(0) fires immediately, its VRAM returns to the free pool), then
 * -- if no class is currently mid-load -- admit the highest-priority
 * evicted class whose window now DOES overlap, evicting lower-priority
 * residents first if that is the only way to fit it. Call once per frame,
 * before building this frame's sprite lists (the same phase obj_pool.h's
 * ObjTickFn hooks run in). Never touches VRAM itself -- see
 * obj_arena_upload() for the actual upload, which must run from vblank. */
void obj_arena_tick(uint16_t camX);

/* Uploads this frame's ARENA_TILES_PER_FRAME-tile amortized chunk for
 * whichever class obj_arena_tick() left mid-load, if any, firing onLive(1)
 * once its last chunk lands. Call from inside vblank, alongside
 * sonic_upload()/springs_upload()/signpost_upload(). */
void obj_arena_upload(void);

/* How many times obj_arena_tick() found no room for a class that wanted to
 * load, even after evicting every lower-priority resident class -- for a
 * debug overlay or a boot-time sanity check, never silently swallowed.
 * Purely a function of the registration table and the camX sequence, so
 * this is deterministic and reproducible, not a timing-dependent count. */
uint16_t obj_arena_refused_count(void);

/* True while obj_arena_upload() has a whole-class blob fill in progress
 * (arenaLoadingSlot valid). obj_anim_window_upload() (below) reads this to
 * yield the shared per-vblank tile budget to a whole-class admission fill
 * whenever both want it the same vblank -- see that function's own comment
 * for why checking this BEFORE obj_arena_upload() runs, every vblank, is
 * what keeps the two mechanisms' combined write size bounded by
 * ARENA_TILES_PER_FRAME instead of silently doubling on the one vblank a
 * class's fill happens to finish. Not useful outside this file/this pairing
 * -- exposed only because obj_anim_window_upload() lives in the same
 * translation unit but cannot see arenaLoadingSlot itself (file-static). */
uint8_t obj_arena_is_loading(void);

/* ---- Per-class ANIMATION window: streaming ONE frame at a time ---------
 *
 * The problem the arena above cannot solve: it holds a class's ENTIRE tile
 * sheet resident, which is the right call for a small sheet (rings' 64
 * rotation tiles, springs' 6 resident poses) but is exactly backwards for
 * the roster now being built on top of this file -- motobug is 387 tiles
 * across 29 frames (~13 tiles/frame), crabmeat 408, chopper 384, seven of
 * the twelve classes individually exceed this whole 203-tile arena on their
 * own. Nothing can hold those whole. What CAN always fit is what is
 * actually being drawn this instant: one frame. This section generalizes
 * the mechanism this codebase already ships twice -- Sonic's own
 * SONIC_MAX_FRAME_TILES=33-tile "only the current frame is ever resident"
 * window (sonic.c) and springs/signpost's shared 24-tile streamed bounce/
 * face-plate window (springs.h/signpost.h) -- into one reusable primitive
 * any class can register instead of hand-rolling its own third copy.
 *
 * THE HARD PROBLEM: many instances, one window. Sonic's window works
 * because there is exactly one Sonic. A class is not one instance -- five
 * motobugs can be on five different frames at the same instant (one
 * mid-turn, three walking out of step with each other, one in a totally
 * different animation because it just died). One VRAM window can only ever
 * hold ONE frame's pixels. Two structurally-viable answers exist:
 *
 *   (a) SYNCHRONIZE every instance of a class to one shared animation
 *       phase, so the whole class draws whatever the ONE window currently
 *       holds. Real precedent already in this codebase, not a new idea:
 *       every one of GHZ1's 445 rings already shares ONE rotation phase
 *       (rings.c's own ringFrame, a single counter, Zone_StaticUpdate's own
 *       behaviour) and it reads as correct in motion -- rings were never
 *       meant to animate independently in the first place.
 *   (b) Hold the K most recently used frames per class in a small ring
 *       buffer, so divergent instances mostly hit a resident frame.
 *
 * THIS FILE IMPLEMENTS (a), chosen deliberately over (b), for three
 * reasons. First, cost: (a) is O(1) VRAM regardless of instance count --
 * one window serves 1 motobug or 500 identically; (b) needs K resident
 * frames just for ONE class with divergent instances, and K has to grow
 * with how much divergence that class can show at once, eating back
 * exactly the budget this mechanism exists to free. Second, and more
 * important for a roster about to be built by several people in parallel:
 * (b) has no clean, small API. "Is frame 7 one of my K resident slots right
 * now" is a question every decide() would have to ask and handle a NO for,
 * with the ANSWER changing frame to frame as the LRU churns under other
 * instances' unrelated demand -- easy to get subtly wrong seventeen times.
 * (a) reduces to one question with a constant answer shape: "is my class's
 * shared window live" -- see the recipe below. Third, safety: (a) makes
 * "which frame is resident" a single deterministic value the whole class
 * agrees on; (b) makes it a moving target that depends on every OTHER
 * instance's recent history too.
 *
 * THE COST, STATED PLAINLY, NOT BURIED: choosing (a) means instances of one
 * class doing the SAME thing animate IN LOCKSTEP -- five walking motobugs
 * take their steps in unison instead of each running its own free-running
 * timer, a real, visible departure from the original game. This is the
 * price of (a)'s O(1) footprint and is the single largest compromise this
 * task's own report flags for the user's sign-off. It is NOT a cost for
 * rings (never independently animated to begin with -- see the precedent
 * above) and it does not apply at all to instances in a genuinely different
 * DISCRETE STATE (see "genuine state divergence" below) -- only to several
 * live instances of the class simultaneously running the SAME animation.
 *
 * GENUINE STATE DIVERGENCE is a different problem from animation phase and
 * this mechanism does not paper over the difference: a walking motobug and
 * a destroyed one are not "the same animation, different frame", they are
 * different ANIMATIONS entirely (motobug's own Motobug.bin has a distinct
 * aniFrames slot per state -- move/idle/turn/smoke -- Motobug_State_* each
 * calls RSDK.SetSpriteAnimation with a different index, never just a
 * different frame of one shared cycle). This mechanism's answer: register
 * ONE ObjAnimWindow PER DISCRETE STATE FAMILY a class can be in (a walking
 * window and a separate destroy-effect window, say), each with its own
 * small VRAM reservation and its own independent live/select/upload
 * lifecycle. Instances within a family still share that family's own
 * lockstep phase (the cost above, paid once per family, not per class) --
 * a walking motobug and a mid-turn motobug both read the SAME "alive"
 * window (their difference IS animation phase, so this file's (a) applies);
 * a destroyed motobug reads a DIFFERENT, second window entirely, never the
 * first one's memory. Multiple ObjAnimWindow registrations are cheap
 * (address-space bookkeeping only, admitted/evicted independently) and
 * nothing here caps how many a class uses beyond OBJ_ANIM_WINDOW_MAX.
 *
 * A THIRD case exists and this mechanism deliberately does NOT try to serve
 * it: independent PER-INSTANCE timers with real simultaneous divergent
 * state, already live in this codebase today -- rings.c's own collect-
 * sparkle pool. Multiple sparkles can be active at once, each with its OWN
 * age-driven frameID (RSDK.Rand(6,8) speed, started at whatever tick it was
 * spawned), genuinely independent, not lockstep-able without visibly wrong
 * results (two sparkles born a few ticks apart would be forced to show the
 * identical frame). That workload keeps using the ARENA above instead (its
 * own small resident sheet, ring_data.h's RING_SPARKLE1/3 portion, 92
 * tiles) -- correctly, on purpose. This mechanism is for the COMMON case
 * (one shared phase serves a whole family cheaply), not a universal
 * replacement for whole-sheet residency; a class/portion with real
 * independent per-instance state stays on obj_arena_register() instead. See
 * rings.c's own migration for exactly this split, both mechanisms in one
 * file, each used where it is actually the right tool.
 *
 * WHAT HAPPENS WHEN THE FRAME AN INSTANCE WANTS IS NOT RESIDENT: this is
 * the one safety rule every implementer needs, and it collapses to nothing
 * subtle by construction, not by convention an implementer has to remember.
 * obj_anim_window_frames() always returns a ONE-ROW table. There is no
 * "wrong index" to accidentally read -- the only two states are "this
 * window currently holds a real, fully-uploaded frame, safe to draw as
 * frames[0]" (obj_anim_window_live() true) or "it does not" (false, because
 * the class is not camera-relevant right now, or it was just admitted and
 * its very first frame has not finished uploading yet, or a frame swap is
 * still in flight). The uniform, deterministic rule, never violated: FALSE
 * MEANS SKIP THE INSTANCE. Never draw frames[0] when live() is false --
 * doing so would read a half-written buffer or a stale one still holding
 * some OTHER frame's tiles, exactly the "another object's pixels on a
 * badnik" bug this whole arena exists to prevent. The recipe every decide()
 * needs is two lines:
 *
 *     if (!obj_anim_window_live(w)) { d.frame = OBJ_SKIP; return d; }
 *     d.frame = 0;   // -- always 0: obj_anim_window_frames(w) has one row
 *
 * with desc->frames set ONCE, at registration time, to
 * obj_anim_window_frames(w) (a stable pointer for the window's whole
 * lifetime -- only the ONE row's contents change, never which pointer
 * desc->frames holds). desc->pieces is untouched by any of this -- piece
 * tables are ROM/RAM data, never VRAM-resident, so they need no window at
 * all; only tileOffset (a VRAM address) ever needs rebasing, which this
 * mechanism does internally, invisibly, on every frame swap.
 *
 * WHY THE WINDOW LAGS INSTEAD OF EVER TEARING: each window reserves
 * 2 * maxFrameTiles tiles from the arena -- a front half and a back half,
 * never one. A frame swap chunks the NEW frame's tiles into the CURRENTLY
 * INACTIVE half, amortized at ARENA_TILES_PER_FRAME per vblank exactly like
 * a whole-class fill (see obj_anim_window_upload()'s own comment on how the
 * two share that one budget); the active half -- what every instance is
 * reading THIS instant -- is never touched until the new frame is 100%
 * present in the other half, at which point ONE pointer flip makes it the
 * new active half, atomically, between one obj_anim_window_upload() call
 * and the next. Nothing ever reads a half mid-write. A frame whose own
 * tileCount exceeds ARENA_TILES_PER_FRAME (motobug's largest is 22) takes
 * more than one vblank to land -- during that time obj_anim_window_live()
 * keeps reporting the OLD frame live (not false: the active half is still
 * fully correct, just not the newest one), so the visible result is the
 * animation holding its previous pose one or two ticks longer than it
 * "should", never a torn or wrong sprite. This is the concrete shape of
 * "an animation should lag by a frame, never tear".
 *
 * COST: 2 * maxFrameTiles tiles per registered window, reserved for as long
 * as that window is resident (evicted the same camera-proximity way as any
 * arena class -- obj_anim_window_register()'s own lookaheadX argument
 * should be built with ARENA_LOOKAHEAD_X(maxFrameTiles), the same macro
 * every whole-class registration already uses, just against the window's
 * own much smaller number instead of a whole sheet's). For motobug
 * (maxFrameTiles=22) that is 44 tiles for its "alive" family -- against a
 * 387-tile whole sheet that could never have fit at all. Upload happens
 * ONLY when obj_anim_window_select() is called with a frame different from
 * what is already active or already loading -- "upload only when the
 * displayed frame changes" is not a convention an implementer has to
 * remember, it falls out of obj_anim_window_select() being a no-op on a
 * repeated or already-in-flight request.
 *
 * WHEN THIS IS THE WRONG TOOL, discovered while sizing the above rather than
 * theorized: this mechanism assumes maxFrameTiles is small relative to the
 * arena -- true for every class with many modest frames (motobug's 22,
 * crabmeat's 24, chopper's 16) but false for platform (tools/gen_assets.py's
 * manifest: PLATFORM_MAX_FRAME_TILES=144 across only 7 total frames).
 * Doubling 144 is 288 tiles -- more than this whole 203-tile arena, and more
 * than platform's own 252-tile whole SHEET (already one of the seven
 * classes too big to hold whole-resident in the first place, this file's
 * own top comment). A class shaped like that -- few frames, each one huge,
 * versus many classes' many-small-frames shape -- makes double-buffering
 * strictly worse than the plain whole-class arena it was supposed to beat.
 * Registering it here anyway is not a runtime failure this mechanism can
 * catch (obj_anim_window_register() has no way to know a size is
 * "unreasonable"), just a silently bad VRAM budget. Not fixed here --
 * flagged for whoever migrates platform to choose deliberately: single-
 * buffered streaming (accept a lag/skip on a swap instead of reserving two
 * copies), splitting into smaller per-sub-family windows (platform_normal's
 * 4 frames vs platform_swing's 3 may not share one maxFrameTiles as badly
 * as the combined table suggests), or simply staying on obj_arena_register()
 * if platform's own real per-frame demand turns out low enough in practice. */

typedef struct ObjAnimWindow ObjAnimWindow;

/* Compile-time cap on concurrently registered windows -- see obj_arena.c's
 * own ARENA_MAX_CLASSES for the same tradeoff, just for this second table.
 * Each window is also its own arena tenant (uses one of ARENA_MAX_CLASSES'
 * own slots), so this cannot usefully exceed that either.
 *
 * Raised from 8 to 16 (BADNIKS batch, 2026-08-17), alongside
 * ARENA_MAX_CLASSES in obj_generic.c: the pre-existing 8-slot cap left only
 * 4 free registrations after rings(2)/springs(1)/signpost(1), and this
 * batch alone needs one window per badnik class (6) -- see obj_generic.c's
 * own comment on ARENA_MAX_CLASSES for why registering more classes than
 * can ever be simultaneously RESIDENT is still safe (registration only
 * reserves a bookkeeping slot, not VRAM tiles; the arena's own admit/evict-
 * by-camera-proximity logic, unchanged, is what actually throttles tile
 * occupancy at runtime, refusing gracefully via obj_arena_refused_count()
 * rather than failing to register at all). Both arrays only grow in static
 * RAM size (a few bytes per slot) -- no existing registrant's behaviour
 * changes. Flagged in this task's own report as a shared-infrastructure
 * edit, since three other batches build on this same file concurrently. */
#define OBJ_ANIM_WINDOW_MAX 16

typedef struct {
	/* Same x-sorted table convention as ArenaClassDesc.entries -- often
	 * literally the class's own ObjTypeDesc.entries, same table, different
	 * purpose (this is the VRAM admit/evict window, not the per-frame draw
	 * window). */
	const void      *entries;
	uint8_t          recordSize;
	uint16_t         recordCount;

	/* Build with ARENA_LOOKAHEAD_X(maxFrameTiles) below -- NOT against the
	 * whole sheet's tile count, against this window's own much smaller
	 * reservation, since that is what actually has to finish loading before
	 * the camera arrives now. */
	int16_t          lookaheadX;
	uint8_t          priority;   /* same OBJ_PRI_* eviction rank as ArenaClassDesc.priority */

	/* The class's whole tile sheet (RAW pixel source -- tools/
	 * convert_objects.py's own generated *_tiles table) and its RAW frame
	 * table (every frames[i].tileOffset an offset into sheetPixels, exactly
	 * the shape *_data.c already generates and obj_type_init/ArenaClassDesc
	 * already read elsewhere in this codebase -- no new data format). This
	 * window reads from these two tables; it never modifies or uploads them
	 * wholesale, only the ONE selected frame's own slice at a time. */
	const uint32_t  *sheetPixels;
	const ObjFrame  *frames;
	uint16_t         frameCount;

	/* The largest .tileCount across every row of frames[] above -- this
	 * class's own *_MAX_FRAME_TILES generated constant. Reserves
	 * 2*maxFrameTiles tiles from the arena; every frame this window is ever
	 * asked to show must fit within it, or obj_anim_window_upload() will
	 * write past the reserved range (a config-time bug, not a runtime
	 * condition -- there is nothing to gracefully refuse mid-upload). */
	uint16_t         maxFrameTiles;
} ObjAnimWindowDesc;

/* Register one animation window (see this section's own top comment for
 * when a class needs more than one). Stores the POINTER given, not a copy
 * -- same contract as obj_arena_register(), so desc needs `static` storage
 * for the life of the program, same convention every migrated type's own
 * ArenaClassDesc already follows. Touches no VRAM and reserves no address
 * space yet: like a freshly-registered arena class, this starts fully
 * evicted until obj_arena_tick() first finds its window overlapping the
 * camera. Returns NULL if OBJ_ANIM_WINDOW_MAX or the underlying arena's own
 * ARENA_MAX_CLASSES table is full -- a configuration mistake, check at
 * boot, not a runtime VRAM condition. */
ObjAnimWindow *obj_anim_window_register(const ObjAnimWindowDesc *desc);

/* Boot-only synchronous admission, mirroring obj_arena_boot_load()/
 * obj_arena_boot_done() (this same file) for a whole class: grants this
 * window its full 2*maxFrameTiles reservation out of an arena assumed still
 * empty (or at least with room), uploads `frame`'s own tiles SYNCHRONOUSLY
 * (one plain vdp_tiles_load(), not amortized -- there is no camera position
 * yet at boot to decide "is this near enough to bother", same reasoning
 * obj_arena_boot_load()'s own doc comment gives), and marks the window live
 * with `frame` already active. Returns 0 on success, nonzero if the arena
 * had no room (permanently-disabled-for-this-run territory, same convention
 * every other boot-time registration failure in this codebase uses -- check
 * this return, do not assume success).
 *
 * Only worth calling for a window whose class must be visible starting
 * frame 1 (rings: RING_SPAN_LO sits inside GHZ1's very first camera view,
 * so rings.c calls this so there is no boot pop-in). Most future classes
 * should skip this entirely and just call obj_anim_window_register(): the
 * normal runtime path (obj_arena_tick() admitting it once its camera window
 * first overlaps, obj_anim_window_upload() streaming its first frame in
 * over the next vblank or two) is simpler and is exactly how every
 * whole-class arena tenant that is NOT rings/springs/signpost already
 * starts too -- a class that only becomes camera-relevant partway through
 * the level has no "frame 1" to be ready for in the first place. */
uint8_t obj_anim_window_boot_load(ObjAnimWindow *w, uint16_t frame);

/* Tell this window which frame (an index into the desc->frames[] table
 * passed to obj_anim_window_register -- NOT into obj_anim_window_frames()'s
 * own one-row output) it should be showing from now on. Call this ONCE PER
 * TICK, from the class's own tick()/update() function, with ONE shared
 * value the whole class agrees on (rings.c's own ringFrame is the pattern
 * to copy) -- NOT once per instance with each instance's own idea of what
 * it would like to show, which would just thrash the load target between
 * whichever instance's decide() happened to run last and starve every
 * frame swap before it could finish. A no-op if `frame` is already the
 * active frame or already the one currently loading -- this is the entire
 * mechanism behind "upload only when the displayed frame changes", nothing
 * else has to implement that rule. A no-op, too, while this window is not
 * currently granted any VRAM (evicted / camera far away) -- there is
 * nowhere to load into yet; call this unconditionally every tick regardless
 * of camera distance, exactly like rings.c already calls its own frame
 * advance unconditionally, and let this function decide whether there is
 * anything to do. */
void obj_anim_window_select(ObjAnimWindow *w, uint16_t frame);

/* True iff obj_anim_window_frames(w)[0] holds a real, fully-uploaded frame
 * safe to draw THIS instant. See this section's own top comment for the
 * exact, uniform rule this exists to make impossible to get wrong: false
 * means skip the instance, full stop, never draw frames[0] anyway. */
uint8_t obj_anim_window_live(const ObjAnimWindow *w);

/* The one-row ObjFrame table to install as ObjTypeDesc.frames -- a stable
 * pointer for this window's whole lifetime; only the row's CONTENTS change,
 * on every frame swap. decide() always returns frame index 0 into this
 * (after checking obj_anim_window_live() first) -- see this section's own
 * top comment for the two-line recipe. */
const ObjFrame *obj_anim_window_frames(const ObjAnimWindow *w);

/* Services AT MOST ONE pending frame swap (across every registered window,
 * whichever was found wanting one first, oldest-registered-first -- same
 * "one shared loader, newest/next in line waits its turn" rule
 * obj_arena_upload() already uses for whole-class fills) with one
 * ARENA_TILES_PER_FRAME-tile chunk. Call from vblank, alongside
 * sonic_upload()/springs_upload()/signpost_upload()/obj_arena_upload() --
 * and call it BEFORE obj_arena_upload(), every time, not after: this
 * function yields the shared budget whenever obj_arena_is_loading() is true
 * as of the START of this vblank, so that on the one vblank a whole-class
 * fill happens to finish, this function has already decided to skip BEFORE
 * that fill's own completion clears the "is loading" flag -- calling the
 * two in the other order would let both write a chunk that same vblank,
 * silently doubling the per-vblank tile budget on that one frame. */
void obj_anim_window_upload(void);

#endif
