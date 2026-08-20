#include "obj_generic.h"
#include "obj_sprite.h"
#include "vdp.h"

/* entries' own convention (obj_data.h's ObjTypeDesc.entries comment): every
 * record is at least 4 bytes, beginning with `int16_t x, y;`. Reading
 * through an (const int16_t *) here is exactly what every existing
 * generated table (RingEntry, SpringEntry) already lays out at that offset
 * with no compiler padding (both start with two naturally-aligned int16_t
 * fields), so this is not a new alignment assumption, just the one this
 * codebase's own generated tables already satisfy. Raw (entries,recordSize)
 * rather than an (const ObjTypeDesc *) here -- obj_x_window below (the
 * shared binary search) reads through this too, and the arena's
 * ArenaClassDesc (obj_generic.h) carries the same entries/recordSize pair
 * without being an ObjTypeDesc at all. */
static int16_t entry_x(const void *entries, uint8_t recordSize, uint16_t i)
{
	const uint8_t *rec = (const uint8_t *)entries + (uint32_t)i * recordSize;
	return *(const int16_t *)rec;
}

static int16_t entry_y(const void *entries, uint8_t recordSize, uint16_t i)
{
	const uint8_t *rec = (const uint8_t *)entries + (uint32_t)i * recordSize;
	return *(const int16_t *)(rec + 2);
}

uint16_t obj_type_init(const ObjTypeDesc *desc, uint8_t *live, uint16_t firstTile)
{
	uint8_t ok = 1;

	if (desc->countPtr) ok = (*desc->countPtr == desc->recordCount);

	if (!desc->tilePixels) {
		*live = ok;
		return firstTile;
	}

	ok = ok && ((uint32_t)firstTile + desc->residentTileCount <= TILE_FONTINDEX);
	if (ok) vdp_tiles_load(desc->tilePixels, firstTile, desc->residentTileCount);
	*live = ok;
	return ok ? (uint16_t)(firstTile + desc->residentTileCount) : firstTile;
}

/* The binary search obj_type_window (per-frame sprite visibility) and the
 * arena's own window check (per-frame tile residency, further down this
 * file) both need: same x-sorted-table shape, different margin. Factored
 * out once rather than duplicated. */
static void x_window(const void *entries, uint8_t recordSize, uint16_t recordCount,
                     uint16_t camX, int16_t marginX, uint16_t *lo, uint16_t *hi)
{
	int32_t xlo = (int32_t)camX - marginX;
	int32_t xhi = (int32_t)camX + SCREEN_WIDTH + marginX;
	uint16_t a, b, m;

	if (recordCount == 0) { *lo = *hi = 0; return; }

	/* Fast reject (2026-08-18, 68000 per-frame cost task): every table this
	 * is ever called on is x-sorted ascending (the precondition the binary
	 * searches below already depend on), so reading just the FIRST and LAST
	 * entry is enough to prove the whole table is empty of this frame's
	 * window without paying for either search. Most of the 18 registered
	 * draw types (obj_type_draw) plus every arena tenant (arena_window_
	 * overlaps) are spread across the level and so are empty at any given
	 * camX far more often than not -- this measured hot in both
	 * obj_type_draw and obj_arena_tick (this task's own profiling). Exactly
	 * equivalent to running both searches down to lo==hi, not an
	 * approximation: if the last entry's x is still below xlo, EVERY entry
	 * is below xlo, so the real search would land lo==hi==recordCount; if
	 * the first entry's x is already >= xhi, EVERY entry is >= xhi (xhi is
	 * always > xlo, SCREEN_WIDTH+marginX*2 > 0), so the real search would
	 * land lo==hi==0. */
	if (entry_x(entries, recordSize, (uint16_t)(recordCount - 1)) < xlo) {
		*lo = *hi = recordCount;
		return;
	}
	if (entry_x(entries, recordSize, 0) >= xhi) {
		*lo = *hi = 0;
		return;
	}

	/* Lower bound: first index with x >= xlo. */
	a = 0; b = recordCount;
	while (a < b) {
		m = (uint16_t)(a + (b - a) / 2);
		if (entry_x(entries, recordSize, m) < xlo) a = (uint16_t)(m + 1); else b = m;
	}
	*lo = a;

	/* Upper bound: first index with x >= xhi (search starts at *lo, never
	 * below it, since the table is sorted ascending). */
	a = *lo; b = recordCount;
	while (a < b) {
		m = (uint16_t)(a + (b - a) / 2);
		if (entry_x(entries, recordSize, m) < xhi) a = (uint16_t)(m + 1); else b = m;
	}
	*hi = a;
}

void obj_type_window(const ObjTypeDesc *desc, uint16_t camX, uint16_t *lo, uint16_t *hi)
{
	x_window(desc->entries, desc->recordSize, desc->recordCount, camX, desc->marginX, lo, hi);
}

uint16_t obj_type_draw(const ObjTypeDesc *desc, VDPSprite *list,
                       uint16_t firstIndex, uint16_t firstLink, uint16_t maxCount,
                       uint16_t camX, uint16_t camY,
                       int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	uint16_t lo, hi, i, n = 0;

	obj_type_window(desc, camX, &lo, &hi);

	/* No `n < maxCount` in this loop's own condition: decide() can carry
	 * side effects (rings.c's own touch test/collect/sparkle-spawn is the
	 * one that matters here) that the original engine runs for every
	 * uncollected candidate in the x window regardless of how many are
	 * actually drawn -- see rings.c's ring_decide() comment. maxCount only
	 * bounds how many list[] entries get WRITTEN (obj_emit_pieces' own
	 * maxCount-n argument, which the piece loop's own `n < maxCount` guard
	 * turns into a no-op write once the cap is reached): a type whose
	 * decide() is a pure function (springs, signpost) pays a few wasted
	 * calls in the astronomically rare case its own window ever exceeds its
	 * cap; rings.c depends on every candidate's decide() actually running. */
	for (i = lo; i < hi; i++) {
		int16_t ex = entry_x(desc->entries, desc->recordSize, i);
		int16_t ey = entry_y(desc->entries, desc->recordSize, i);
		ObjDrawDecision d = desc->decide(desc->state, i, ex, ey,
		                                 sonicWorldX, sonicWorldY, sonicFrameIndex);
		const ObjFrame *f;
		const ObjPiece *p;

		if (d.frame == OBJ_SKIP) continue;
		if (n >= maxCount) continue;

		f = &desc->frames[d.frame];
		p = &desc->pieces[f->pieceOffset];
		/* (ex+d.offX, ey+d.offY), not (ex,ey): SPRITE-VS-HITBOX DRIFT fix
		 * (this task, Job 2, obj_data.h's own ObjDrawDecision comment) --
		 * decide()'s returned offset is the exact same one it already added
		 * to ex/ey for its own hitbox touch test, so the drawn sprite tracks
		 * wherever the hitbox actually is instead of staying pinned at the
		 * entry's raw scene position. 0 for every class that never moves an
		 * instance after spawn (rings/springs/signpost/spikelog/decoration),
		 * making this an exact no-op there. */
		if (!d.flipV && desc->templatesH0) {
			/* PRECOMPUTED PIECE TEMPLATES fast path (Job 1, lever 1, this
			 * task) -- see obj_data.h's own ObjTypeDesc.templatesH0/H1
			 * comment and obj_sprite.h's own obj_emit_pieces_templated().
			 * templatesH0/H1 are parallel arrays to desc->pieces (same
			 * length, same f->pieceOffset indexing), so selecting by
			 * d.flipH here is exactly the same lookup obj_emit_pieces()'s
			 * own p = &desc->pieces[f->pieceOffset] above already does,
			 * just into whichever flip variant this decision asked for. */
			const ObjPieceTemplate *t = (d.flipH ? desc->templatesH1 : desc->templatesH0) + f->pieceOffset;
			n = (uint16_t)(n + obj_emit_pieces_templated(list, (uint16_t)(firstIndex + n),
			               (uint16_t)(firstLink + n), (uint16_t)(maxCount - n),
			               t, f->pieceCount,
			               (int16_t)((int16_t)(ex + d.offX) - (int16_t)camX),
			               (int16_t)((int16_t)(ey + d.offY) - (int16_t)camY)));
		} else {
			n = (uint16_t)(n + obj_emit_pieces(list, (uint16_t)(firstIndex + n),
			               (uint16_t)(firstLink + n), (uint16_t)(maxCount - n),
			               p, f->pieceCount, f->tileOffset, desc->palette,
			               (int16_t)((int16_t)(ex + d.offX) - (int16_t)camX),
			               (int16_t)((int16_t)(ey + d.offY) - (int16_t)camY),
			               f->pivotX, f->pivotY, d.flipH, d.flipV, desc->drawPriority));
		}
	}
	return n;
}

/* ---- Shared VRAM tile residency arena ---------------------------------
 * See obj_generic.h's own comment for the shape of the problem and why a
 * class's whole tile sheet, not one frame of it, is the unit of residency,
 * and its own comment on ARENA_TILES_PER_FRAME for where that number comes
 * from. */

/* Compile-time cap on concurrently REGISTERED classes (resident, loading,
 * or merely evicted-but-known-about) -- not a limit on how many exist in
 * the game, only on how many can be tracked by this arena at once. 3 real
 * consumers today (rings, springs' resident set, signpost's resident set);
 * headroom left for the roster's first classes once their own decide()/
 * entries tables exist (out of this task's own scope -- see obj_generic.h).
 *
 * Raised from 8 to 16 (BADNIKS batch, 2026-08-17): rings/springs/signpost
 * already use 4 of the original 8 slots (rings' rotation window + its
 * separate sparkle resident set, springs' resident set, signpost's resident
 * set), leaving only 4 free -- not enough for six badnik classes' own
 * anim-window registrations even though the real VRAM tile budget (~60
 * free tiles after boot) was always going to be the binding constraint, not
 * this array's size. See obj_generic.h's OBJ_ANIM_WINDOW_MAX comment for
 * the full reasoning; both constants are raised together since every anim
 * window is also one of this array's own slots. */
/* BUG FOUND WHILE INVESTIGATING "no badniks visible anywhere" (this task's
 * own report): 16 was still one slot short of this codebase's REAL
 * obj_arena_register()/obj_anim_window_register() call-site count. Counted
 * directly (grep every md_src .c file for a real obj_arena_register( or
 * obj_anim_window_register( call, excluding obj_generic.c's own
 * definitions): 17 call sites --
 * rings (2: rotation window + sparkle), springs (1), signpost (1),
 * decoration (1), spikes (1), spikelog (1), itembox (1), platform (2:
 * normal + swing), bridge (1) = 11 at boot, plus motobug/crabmeat/
 * buzzbomber/chopper/newtron/batbrain (1 each = 6) registered lazily on
 * each class's own first tick() call, in that OBJ_TYPE_LIST row order. At
 * 16, the 17th registration to actually RUN -- batbrain's, since it is last
 * in OBJ_TYPE_LIST's badnik rows and every earlier one has already claimed
 * a slot by the time its own first tick() fires -- hit
 * `arenaSlotCount >= ARENA_MAX_CLASSES` and got ARENA_INVALID_SLOT back,
 * permanently: obj_anim_window_register() returned NULL, brWindow (batbrain.c)
 * stayed NULL for the rest of the run, and batbrain_draw()'s own
 * `if (!batbrainInited || !brWindow) return 0;` guard made Batbrain never
 * draw a single sprite -- deterministic, not camera- or timing-dependent,
 * confirmed by counting the real call sites rather than by observation.
 * Raised to 24 for headroom (a few bytes of static RAM per slot -- see this
 * constant's own pre-existing comment above -- well worth not hitting this
 * exact bug again the next time a class is added). */
#define ARENA_MAX_CLASSES 24
#define ARENA_MAX_FREE_BLOCKS (ARENA_MAX_CLASSES + 1)

enum { ARENA_EVICTED = 0, ARENA_LOADING = 1, ARENA_RESIDENT = 2 };

typedef struct {
	const ArenaClassDesc *desc;
	uint16_t vramBase;    /* valid only while state != ARENA_EVICTED */
	uint16_t uploaded;    /* tiles of tilePixels already written, this residency */
	uint8_t  state;
} ArenaSlot;

typedef struct { uint16_t start, len; } FreeBlock;

static ArenaSlot  arenaSlots[ARENA_MAX_CLASSES];
static uint8_t    arenaSlotCount;
static FreeBlock  arenaFree[ARENA_MAX_FREE_BLOCKS];
static uint8_t    arenaFreeCount;
static uint8_t    arenaLoadingSlot = ARENA_INVALID_SLOT;
static uint16_t   arenaRefusedCount;

/* obj_arena_tick()'s own "nothing changed" short-circuit (2026-08-18, 68000
 * per-frame cost task): every class's admit/evict eligibility (arena_
 * window_overlaps()) is a pure function of camX and that class's own STATIC
 * desc (entries/lookaheadX) -- nothing else obj_arena_tick() touches depends
 * on elapsed time or any other input. So while camX is unchanged from the
 * last call (Sonic standing still, or simply not having moved enough to
 * cross a whole pixel this tick), EVERY overlap test below would return the
 * exact same answer it did last tick, meaning neither pass could possibly
 * evict or admit anything -- both full ARENA_MAX_CLASSES-sized scans are
 * pure repeated work. Measured cause for chasing this: obj_arena_tick was
 * 9/75 PC samples with Sonic standing still at spawn (this task's own
 * profiling), i.e. this exact camera-stationary case. arenaTickPrimed
 * guards the very first call (and any call right after obj_arena_init(),
 * e.g. a hypothetical future re-init): camX==0 is not a safe "nothing
 * changed" sentinel on its own, since 0 is also a legitimate real camX.
 *
 * WHILE MOVING (2026-08-18, same task, follow-up): camX changes almost
 * every tick once Sonic is actually running, so the exact-equality check
 * above almost never fires and every class pays the full scan every single
 * frame -- measured 18/150 PC samples in this function with DEBUG_AUTORUN
 * running. Widened to a 16px-BLOCK comparison (camX>>4) instead of exact
 * equality, and this is still exact, not approximate: every registered
 * class's own lookaheadX is built from ARENA_LOOKAHEAD_X() (obj_generic.h),
 * which is (loadFrames+1)*ARENA_MAX_SCROLL_PX -- i.e. every class already
 * carries at least one whole ARENA_MAX_SCROLL_PX=16px unit of margin
 * specifically to cover "admission is decided once per tick" (that macro's
 * own comment). Comparing camX>>4 instead of camX itself can defer the
 * rescan by at most 15px of further camX travel before the block changes
 * and forces one (camX can only stay in the same 16px block for so long
 * before crossing out of it, regardless of how many frames that takes) --
 * that is within the SAME 16px unit every class's margin already reserves
 * for exactly this kind of once-per-tick scheduling slack, so no class's
 * real admission/eviction point can ever arrive before this rescans. */
static uint16_t   arenaLastTickCamX;
static uint8_t    arenaTickPrimed;

void obj_arena_init(uint16_t base, uint16_t size)
{
	arenaSlotCount = 0;
	arenaLoadingSlot = ARENA_INVALID_SLOT;
	arenaRefusedCount = 0;
	arenaFreeCount = 1;
	arenaFree[0].start = base;
	arenaFree[0].len = size;
	arenaTickPrimed = 0;
}

/* First-fit: the arena only ever holds a handful of classes at once, so the
 * simplest correct allocator (linear scan, no size-class buckets) is the
 * right one here -- easy to verify by inspection, which matters more than
 * raw speed for something that runs at most once or twice a frame. */
static uint8_t arena_alloc(uint16_t size, uint16_t *outBase)
{
	uint8_t i;
	for (i = 0; i < arenaFreeCount; i++) {
		if (arenaFree[i].len >= size) {
			*outBase = arenaFree[i].start;
			arenaFree[i].start = (uint16_t)(arenaFree[i].start + size);
			arenaFree[i].len = (uint16_t)(arenaFree[i].len - size);
			if (arenaFree[i].len == 0 && arenaFreeCount > 0) {
				/* Drop the now-empty block: shift the tail down one. */
				uint8_t j;
				for (j = i; j + 1 < arenaFreeCount; j++) arenaFree[j] = arenaFree[j + 1];
				arenaFreeCount--;
			}
			return 1;
		}
	}
	return 0;
}

/* Insert [start,start+len) back into the free list, sorted by start,
 * coalescing with whichever neighbour(s) it now touches -- keeps
 * arenaFreeCount bounded by "number of resident classes + 1" regardless of
 * how many alloc/free cycles run, the standard free-list invariant. */
static void arena_free(uint16_t start, uint16_t len)
{
	uint8_t i, at;

	if (len == 0) return;

	for (at = 0; at < arenaFreeCount && arenaFree[at].start < start; at++) {}

	/* Merge with the following block if adjacent. */
	if (at < arenaFreeCount && start + len == arenaFree[at].start) {
		arenaFree[at].start = start;
		arenaFree[at].len = (uint16_t)(arenaFree[at].len + len);
	} else {
		/* Insert a fresh block at `at`, shifting the tail up one. */
		for (i = arenaFreeCount; i > at; i--) arenaFree[i] = arenaFree[i - 1];
		arenaFree[at].start = start;
		arenaFree[at].len = len;
		arenaFreeCount++;
	}

	/* Merge with the preceding block if adjacent (either branch above may
	 * have left one to merge with). */
	if (at > 0 && (uint16_t)(arenaFree[at - 1].start + arenaFree[at - 1].len) == arenaFree[at].start) {
		arenaFree[at - 1].len = (uint16_t)(arenaFree[at - 1].len + arenaFree[at].len);
		for (i = at; i + 1 < arenaFreeCount; i++) arenaFree[i] = arenaFree[i + 1];
		arenaFreeCount--;
	}
}

static uint8_t arena_window_overlaps(const ArenaClassDesc *desc, uint16_t camX)
{
	uint16_t lo, hi;
	x_window(desc->entries, desc->recordSize, desc->recordCount, camX, desc->lookaheadX, &lo, &hi);
	return lo < hi;
}

/* Fully evicts slot i: fires onLive(0) if it was ever visible, returns its
 * VRAM to the free pool, clears arenaLoadingSlot if it was the one loading.
 * Called both from obj_arena_tick()'s own "window left" pass and from its
 * admission pass when a higher-priority newcomer needs the room. */
static void arena_evict(uint8_t i)
{
	if (arenaSlots[i].state == ARENA_RESIDENT || arenaSlots[i].state == ARENA_LOADING) {
		if (arenaSlots[i].desc->onLive) arenaSlots[i].desc->onLive(0);
	}
	arena_free(arenaSlots[i].vramBase, arenaSlots[i].desc->tileCount);
	arenaSlots[i].state = ARENA_EVICTED;
	if (arenaLoadingSlot == i) arenaLoadingSlot = ARENA_INVALID_SLOT;
}

uint8_t obj_arena_register(const ArenaClassDesc *desc)
{
	uint8_t slot;
	if (arenaSlotCount >= ARENA_MAX_CLASSES) return ARENA_INVALID_SLOT;
	slot = arenaSlotCount++;
	arenaSlots[slot].desc = desc;
	arenaSlots[slot].vramBase = 0;
	arenaSlots[slot].uploaded = 0;
	arenaSlots[slot].state = ARENA_EVICTED;
	return slot;
}

uint16_t obj_arena_boot_load(uint8_t slot)
{
	uint16_t base;
	if (slot >= arenaSlotCount) return 0xFFFF;
	if (!arena_alloc(arenaSlots[slot].desc->tileCount, &base)) return 0xFFFF;
	arenaSlots[slot].vramBase = base;
	arenaSlots[slot].uploaded = 0;
	arenaSlots[slot].state = ARENA_LOADING;
	if (arenaSlots[slot].desc->onBase) arenaSlots[slot].desc->onBase(base);
	return base;
}

void obj_arena_boot_done(uint8_t slot)
{
	if (slot >= arenaSlotCount) return;
	arenaSlots[slot].uploaded = arenaSlots[slot].desc->tileCount;
	arenaSlots[slot].state = ARENA_RESIDENT;
	if (arenaSlots[slot].desc->onLive) arenaSlots[slot].desc->onLive(1);
}

void obj_arena_tick(uint16_t camX)
{
	uint8_t i, best;

	/* Nothing-changed short-circuit -- see arenaLastTickCamX's own comment
	 * above. Safe to skip BOTH passes below whole: every overlap test either
	 * pass could run is a pure function of camX alone (plus this class's own
	 * unchanging desc), so an unchanged 16px BLOCK guarantees an unchanged
	 * result, for every class, every time (see the block-quantization
	 * comment above for why 16px is exact here, not approximate). */
	if (arenaTickPrimed && (camX >> 4) == (arenaLastTickCamX >> 4)) return;
	arenaTickPrimed = 1;
	arenaLastTickCamX = camX;

	/* Pass 1: evict anyone whose (lookahead-widened) window has left. This
	 * runs before admission so space freed this same tick is immediately
	 * available to a newcomer below -- no eviction is ever deferred a frame
	 * behind the window check that caused it. */
	for (i = 0; i < arenaSlotCount; i++) {
		if (arenaSlots[i].state == ARENA_EVICTED) continue;
		if (!arena_window_overlaps(arenaSlots[i].desc, camX)) arena_evict(i);
	}

	/* Pass 2: admit at most one newcomer -- the highest-priority evicted
	 * class whose window now overlaps -- and only if nothing else is
	 * currently mid-load (ARENA_TILES_PER_FRAME is a per-ARENA budget, not
	 * per-class; see obj_generic.h's own comment on why concurrent loads
	 * are not supported). Ties break toward the lower slot index
	 * (registration order), same documented tie-break obj_pool_arbitrate
	 * already uses. */
	if (arenaLoadingSlot != ARENA_INVALID_SLOT) return;

	best = ARENA_MAX_CLASSES;
	for (i = 0; i < arenaSlotCount; i++) {
		if (arenaSlots[i].state != ARENA_EVICTED) continue;
		if (!arena_window_overlaps(arenaSlots[i].desc, camX)) continue;
		if (best == ARENA_MAX_CLASSES || arenaSlots[i].desc->priority > arenaSlots[best].desc->priority)
			best = i;
	}
	if (best == ARENA_MAX_CLASSES) return;

	{
		uint16_t base;
		uint16_t needed = arenaSlots[best].desc->tileCount;
		while (!arena_alloc(needed, &base)) {
			/* No single free block is big enough -- try to make one by
			 * evicting the lowest-priority resident/loading class that
			 * outranks nobody here (never evicts anything >= best's own
			 * priority: a newcomer can never bump an equally- or
			 * more-important class, only a strictly less important one). */
			uint8_t victim = ARENA_MAX_CLASSES;
			for (i = 0; i < arenaSlotCount; i++) {
				if (i == best) continue;
				if (arenaSlots[i].state == ARENA_EVICTED) continue;
				if (arenaSlots[i].desc->priority >= arenaSlots[best].desc->priority) continue;
				if (victim == ARENA_MAX_CLASSES || arenaSlots[i].desc->priority < arenaSlots[victim].desc->priority)
					victim = i;
			}
			if (victim == ARENA_MAX_CLASSES) {
				/* Nothing left evictable -- deterministic refusal, not a
				 * partial/corrupt allocation. Retried next tick: either the
				 * camera moves on and this class's own window closes again,
				 * or something else's window closes first and frees room. */
				arenaRefusedCount++;
				return;
			}
			arena_evict(victim);
		}
		arenaSlots[best].vramBase = base;
		arenaSlots[best].uploaded = 0;
		if (arenaSlots[best].desc->tilePixels) {
			arenaSlots[best].state = ARENA_LOADING;
			arenaLoadingSlot = best;
		} else {
			/* tilePixels==NULL: obj_anim_window_register()'s own reservation
			 * mode (obj_generic.h's own comment on ArenaClassDesc.tilePixels)
			 * -- no whole-blob copy to wait for, so this address range is
			 * resident (onLive(1) below) the instant it is granted. Never
			 * occupies arenaLoadingSlot: obj_arena_upload() must never see
			 * this slot, since it would try to read through a NULL
			 * tilePixels the moment it did. */
			arenaSlots[best].uploaded = arenaSlots[best].desc->tileCount;
			arenaSlots[best].state = ARENA_RESIDENT;
		}
		if (arenaSlots[best].desc->onBase) arenaSlots[best].desc->onBase(base);
		if (!arenaSlots[best].desc->tilePixels && arenaSlots[best].desc->onLive)
			arenaSlots[best].desc->onLive(1);
	}
}

uint8_t obj_arena_is_loading(void) { return arenaLoadingSlot != ARENA_INVALID_SLOT; }

void obj_arena_upload(void)
{
	ArenaSlot *s;
	uint16_t remaining, chunk;

	if (arenaLoadingSlot == ARENA_INVALID_SLOT) return;
	s = &arenaSlots[arenaLoadingSlot];

	remaining = (uint16_t)(s->desc->tileCount - s->uploaded);
	chunk = remaining < ARENA_TILES_PER_FRAME ? remaining : ARENA_TILES_PER_FRAME;
	vdp_tiles_load(s->desc->tilePixels + (uint32_t)s->uploaded * 8,
	              (uint16_t)(s->vramBase + s->uploaded), chunk);
	s->uploaded = (uint16_t)(s->uploaded + chunk);

	if (s->uploaded >= s->desc->tileCount) {
		s->state = ARENA_RESIDENT;
		arenaLoadingSlot = ARENA_INVALID_SLOT;
		if (s->desc->onLive) s->desc->onLive(1);
	}
}

uint16_t obj_arena_refused_count(void) { return arenaRefusedCount; }

/* ---- Per-class animation window ----------------------------------------
 * See obj_generic.h's own (long) comment on this section for the design
 * this implements and why. Short version: one window = a double-buffered
 * (never torn) reservation of 2*maxFrameTiles tiles, admitted/evicted as an
 * ordinary arena tenant (tilePixels==NULL, see obj_arena_tick() above), with
 * its own tiny state machine layered on top for "which frame is active,
 * which is loading, how far". */

#define ANIM_WINDOW_NONE 0xFFFF   /* activeIndex/loadIndex: no such frame yet */

struct ObjAnimWindow {
	const ObjAnimWindowDesc *desc;
	uint16_t vramBase;     /* valid only while granted */
	uint8_t  arenaSlot;    /* this window's own obj_arena_register() slot, for obj_anim_window_boot_load() */
	uint8_t  granted;      /* arena-level grant active (tilePixels==NULL onLive) */
	uint8_t  activeHalf;   /* which half frames[0].tileOffset currently points at */
	uint16_t activeIndex;  /* desc->frames[] index currently resident, or NONE */
	uint16_t loadIndex;    /* desc->frames[] index currently loading, or NONE */
	uint16_t loadProgress; /* tiles of loadIndex's own frame already copied */
	ObjFrame frames[1];    /* obj_anim_window_frames()'s own return value */
};

static ObjAnimWindow  animWindows[OBJ_ANIM_WINDOW_MAX];
static ArenaClassDesc animWindowArenaDescs[OBJ_ANIM_WINDOW_MAX];
static uint8_t         animWindowCount;
static uint8_t         animLoadingWindow = ARENA_INVALID_SLOT;

/* One onBase/onLive trampoline pair per slot: ArenaClassDesc callbacks are
 * plain function pointers with no userdata (every real registrant in this
 * codebase already accepts that -- rings.c/springs.c's own static callbacks
 * follow the identical one-callback-per-static-instance shape), so a window
 * needs its own compile-time-distinct pair to know which of animWindows[]
 * it is. Same pattern this task's own host harness (scratchpad's
 * arena_verify/driver.c, SYN_CB) already used for its 4 synthetic classes. */
#define ANIM_WINDOW_CB(N) \
	static void anim_window_onBase_##N(uint16_t base) { animWindows[N].vramBase = base; } \
	static void anim_window_onLive_##N(uint8_t live) { \
		animWindows[N].granted = live; \
		if (!live) { \
			animWindows[N].activeIndex = ANIM_WINDOW_NONE; \
			animWindows[N].loadIndex = ANIM_WINDOW_NONE; \
			animWindows[N].activeHalf = 1; \
			if (animLoadingWindow == (N)) animLoadingWindow = ARENA_INVALID_SLOT; \
		} \
	}
ANIM_WINDOW_CB(0) ANIM_WINDOW_CB(1) ANIM_WINDOW_CB(2) ANIM_WINDOW_CB(3)
ANIM_WINDOW_CB(4) ANIM_WINDOW_CB(5) ANIM_WINDOW_CB(6) ANIM_WINDOW_CB(7)
ANIM_WINDOW_CB(8) ANIM_WINDOW_CB(9) ANIM_WINDOW_CB(10) ANIM_WINDOW_CB(11)
ANIM_WINDOW_CB(12) ANIM_WINDOW_CB(13) ANIM_WINDOW_CB(14) ANIM_WINDOW_CB(15)

static void (*const animWindowOnBase[OBJ_ANIM_WINDOW_MAX])(uint16_t) = {
	anim_window_onBase_0, anim_window_onBase_1, anim_window_onBase_2, anim_window_onBase_3,
	anim_window_onBase_4, anim_window_onBase_5, anim_window_onBase_6, anim_window_onBase_7,
	anim_window_onBase_8, anim_window_onBase_9, anim_window_onBase_10, anim_window_onBase_11,
	anim_window_onBase_12, anim_window_onBase_13, anim_window_onBase_14, anim_window_onBase_15
};
static void (*const animWindowOnLive[OBJ_ANIM_WINDOW_MAX])(uint8_t) = {
	anim_window_onLive_0, anim_window_onLive_1, anim_window_onLive_2, anim_window_onLive_3,
	anim_window_onLive_4, anim_window_onLive_5, anim_window_onLive_6, anim_window_onLive_7,
	anim_window_onLive_8, anim_window_onLive_9, anim_window_onLive_10, anim_window_onLive_11,
	anim_window_onLive_12, anim_window_onLive_13, anim_window_onLive_14, anim_window_onLive_15
};

ObjAnimWindow *obj_anim_window_register(const ObjAnimWindowDesc *desc)
{
	ObjAnimWindow *w;
	ArenaClassDesc *ad;
	uint8_t slot, arenaSlot;

	if (animWindowCount >= OBJ_ANIM_WINDOW_MAX) return (ObjAnimWindow *)0;
	slot = animWindowCount++;
	w = &animWindows[slot];

	w->desc = desc;
	w->vramBase = 0;
	w->arenaSlot = ARENA_INVALID_SLOT;
	w->granted = 0;
	w->activeHalf = 1;   /* first-ever load lands in half 0 -- see 1-activeHalf below */
	w->activeIndex = ANIM_WINDOW_NONE;
	w->loadIndex = ANIM_WINDOW_NONE;
	w->loadProgress = 0;
	w->frames[0].tileOffset = 0;
	w->frames[0].pieceOffset = 0;
	w->frames[0].tileCount = 0;
	w->frames[0].pieceCount = 0;
	w->frames[0].pivotX = 0;
	w->frames[0].pivotY = 0;
	w->frames[0].duration = 0;

	ad = &animWindowArenaDescs[slot];
	ad->entries = desc->entries;
	ad->recordSize = desc->recordSize;
	ad->recordCount = desc->recordCount;
	ad->tilePixels = (const uint32_t *)0;   /* reservation-only, see obj_arena_tick() */
	ad->tileCount = (uint16_t)(2u * desc->maxFrameTiles);
	ad->lookaheadX = desc->lookaheadX;
	ad->priority = desc->priority;
	ad->onBase = animWindowOnBase[slot];
	ad->onLive = animWindowOnLive[slot];

	arenaSlot = obj_arena_register(ad);
	if (arenaSlot == ARENA_INVALID_SLOT) return (ObjAnimWindow *)0;
	w->arenaSlot = arenaSlot;
	return w;
}

uint8_t obj_anim_window_boot_load(ObjAnimWindow *w, uint16_t frame)
{
	uint16_t base;
	const ObjFrame *src;

	if (!w || frame >= w->desc->frameCount) return 1;
	base = obj_arena_boot_load(w->arenaSlot);
	if (base == 0xFFFF) return 1;

	src = &w->desc->frames[frame];
	vdp_tiles_load(w->desc->sheetPixels + (uint32_t)src->tileOffset * 8, base, src->tileCount);
	obj_arena_boot_done(w->arenaSlot);   /* fires onLive(1) -> our trampoline sets w->granted */

	/* obj_arena_boot_done() only knows about whole-blob residency; this
	 * window's OWN "which frame, which half" state is ours alone to set,
	 * exactly what obj_anim_window_upload()'s flip step would have done had
	 * this gone through the amortized path instead. */
	w->activeHalf = 0;
	w->activeIndex = frame;
	w->frames[0].tileOffset = base;
	w->frames[0].pieceOffset = src->pieceOffset;
	w->frames[0].tileCount = src->tileCount;
	w->frames[0].pieceCount = src->pieceCount;
	w->frames[0].pivotX = src->pivotX;
	w->frames[0].pivotY = src->pivotY;
	w->frames[0].duration = src->duration;
	return 0;
}

void obj_anim_window_select(ObjAnimWindow *w, uint16_t frame)
{
	if (!w || !w->granted) return;
	if (frame >= w->desc->frameCount) return;   /* defensive: out-of-range request, ignored */
	if (frame == w->activeIndex || frame == w->loadIndex) return;   /* already showing/loading */
	w->loadIndex = frame;
	w->loadProgress = 0;
}

uint8_t obj_anim_window_live(const ObjAnimWindow *w)
{
	return w != (const ObjAnimWindow *)0 && w->granted && w->activeIndex != ANIM_WINDOW_NONE;
}

const ObjFrame *obj_anim_window_frames(const ObjAnimWindow *w)
{
	return w->frames;
}

void obj_anim_window_upload(void)
{
	ObjAnimWindow *w;
	const ObjFrame *src;
	uint16_t remaining, chunk, dstHalf, dstBase, i;

	/* Yield the shared per-vblank budget to a whole-class fill -- see this
	 * function's own header comment for why this check has to run before
	 * obj_arena_upload() does, every vblank, not after. */
	if (obj_arena_is_loading()) return;

	if (animLoadingWindow == ARENA_INVALID_SLOT) {
		for (i = 0; i < animWindowCount; i++) {
			if (animWindows[i].granted && animWindows[i].loadIndex != ANIM_WINDOW_NONE) {
				animLoadingWindow = (uint8_t)i;
				break;
			}
		}
		if (animLoadingWindow == ARENA_INVALID_SLOT) return;
	}

	w = &animWindows[animLoadingWindow];
	/* Defensive only: the owning arena slot's own onLive(0) trampoline
	 * (ANIM_WINDOW_CB above) already clears loadIndex/animLoadingWindow the
	 * instant eviction happens, and obj_arena_tick() always runs before
	 * obj_anim_window_upload() in the documented per-frame order, so this
	 * should never actually see a stale/evicted window -- kept as the same
	 * belt-and-suspenders check obj_arena_upload() itself does not need
	 * (its own arenaLoadingSlot is reset by the identical eviction path). */
	if (!w->granted || w->loadIndex == ANIM_WINDOW_NONE) { animLoadingWindow = ARENA_INVALID_SLOT; return; }

	src = &w->desc->frames[w->loadIndex];
	remaining = (uint16_t)(src->tileCount - w->loadProgress);
	chunk = remaining < ARENA_TILES_PER_FRAME ? remaining : ARENA_TILES_PER_FRAME;
	dstHalf = (uint16_t)(1 - w->activeHalf);
	dstBase = (uint16_t)(w->vramBase + dstHalf * w->desc->maxFrameTiles + w->loadProgress);
	vdp_tiles_load(w->desc->sheetPixels + ((uint32_t)src->tileOffset + w->loadProgress) * 8,
	              dstBase, chunk);
	w->loadProgress = (uint16_t)(w->loadProgress + chunk);

	if (w->loadProgress >= src->tileCount) {
		/* The one atomic flip: everything every instance reads
		 * (obj_anim_window_frames()[0]) changes in one step, only once the
		 * new frame's tiles are 100% present in the half that was NOT being
		 * read from -- see obj_generic.h's own "why the window lags instead
		 * of ever tearing" comment. */
		w->activeHalf = (uint8_t)dstHalf;
		w->activeIndex = w->loadIndex;
		w->frames[0].tileOffset = (uint16_t)(w->vramBase + dstHalf * w->desc->maxFrameTiles);
		w->frames[0].pieceOffset = src->pieceOffset;
		w->frames[0].tileCount = src->tileCount;
		w->frames[0].pieceCount = src->pieceCount;
		w->frames[0].pivotX = src->pivotX;
		w->frames[0].pivotY = src->pivotY;
		w->frames[0].duration = src->duration;
		w->loadIndex = ANIM_WINDOW_NONE;
		animLoadingWindow = ARENA_INVALID_SLOT;
	}
}
