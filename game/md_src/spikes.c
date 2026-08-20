#include "spikes.h"
#include "vdp.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_sprite.h"
#include "obj_pool.h"
#include "spikes_data.h"
#include "assets_gen.h"

/* assets/ghz/spikes.bin row shape, tools/convert_objects.py's SPIKES_SCENE
 * (Spikes_Serialize order, Spikes.c:749-757): x,y then type/moving/count/
 * stagger/timer/planeFilter, all uint8. Byte-identical to SpikeEntry, same
 * "cast the incbin'd bytes in place" convention as springs.c's SpringEntry. */
typedef struct {
    int16_t x, y;
    uint8_t type, moving, count, stagger, timer, planeFilter;
} SpikeEntry;

static const uint16_t *const ghz_spikes_count_p = ASSET_GHZ_SPIKES;
#define ghz_spikes_count (*ghz_spikes_count_p)
static const SpikeEntry *const ghz_spikes_xy =
    (const SpikeEntry *)((const uint8_t *)ASSET_GHZ_SPIKES + 2);
static const uint32_t *const spikes_tiles_md = ASSET_SPIKES_TILES;

/* Collision.cpp's C_* result codes -- same numbering spring.c's own
 * spring_check_box() already uses. */
#define C_NONE   0
#define C_TOP    1
#define C_LEFT   2
#define C_RIGHT  3
#define C_BOTTOM 4

/* Spikes_Create, Spikes.c:342-378: raw scene `type` packs dir=type&1,
 * orient=(type>>1)&1. orient0(vertical): dir0->C_TOP, dir1->C_BOTTOM.
 * orient1(horizontal): dir0->C_RIGHT, dir1->C_LEFT. Precomputed once at
 * init into spikeSide[]/spikeVert[] rather than re-derived every frame. */
static uint8_t spikeSide[SPIKES_COUNT];   /* C_TOP/LEFT/RIGHT/BOTTOM */
static uint8_t spikeVert[SPIKES_COUNT];   /* 1 vertical (spans X), 0 horizontal (spans Y) */
static uint8_t spikeCount[SPIKES_COUNT];  /* clamped >=2, Spikes_Create:325-326 */

/* SHOWN/HIDDEN toggle state -- see spikes.h's own comment on the collapsed
 * 4-tick-slide compromise. Static (moving=0) spikes never leave SHOWN. */
#define SPIKE_HIDDEN 0
#define SPIKE_SHOWN  1

/* Stand-in for Zone->timer (Spikes.c:21-22,42), a monotonic tick this
 * object type owns entirely itself -- see spikes.h's own header comment for
 * why a locally-owned counter, incremented once per displayed frame from
 * spikes_draw() below (OBJ_TYPE_LIST gives this row no separate tick, same
 * "fold the once-per-call bookkeeping into draw()" shape rings_update()
 * already uses), is this port's substitute. sh_src/spikes.c keeps its own,
 * independent copy, incremented once per SH2 tick -- the two run at the
 * same real 60 Hz rate by construction (comm.h's tick protocol) so they
 * agree on TIMING even though they start counting from different, unrelated
 * epochs (whichever CPU's own init ran first) -- a constant few-frame phase
 * offset between the two, invisible on a ~128-tick period. */
static uint32_t hazardTick;

static uint8_t spikesLive;

/* CAMERA-WINDOWED HAZARD TIMER (Job 1.2, this task). The old
 * spikes_tick_advance() (removed) looped all SPIKES_COUNT=41 entries EVERY
 * frame regardless of the camera window -- flagged by the previous batch's
 * own report as a risk left for time reasons (a stateful XOR toggle looked
 * dangerous to touch under pressure). It is safe to window after all,
 * because the toggle is a PURE function of hazardTick, not a random walk:
 * Spikes.c:20-27/41-47's own gate, `(hazardTick&0x3F)==e->timer &&
 * (hazardTick&0x40!=0)==(stagger!=0)`, matches at exactly ONE tick out of
 * every 64 (the low-6-bits equality) AND only on every OTHER such occasion
 * (the bit-6/stagger equality, since adding 64 always flips bit 6 while
 * leaving the low 6 bits alone) -- i.e. matches recur with an exact period
 * of 128 ticks, starting at t0 = timer + (stagger?64:0) (derivable in
 * [0,127) directly from the entry's own scene-authored fields, no replay
 * needed). A moving spike starts HIDDEN (spikes_init(), unchanged) and XORs
 * once per match, so at any hazardTick T the SHOWN/HIDDEN bit is just the
 * PARITY of how many matches have occurred by T -- a closed-form count, not
 * an accumulated history: 0 matches before t0 (HIDDEN, matching init), then
 * exactly one every 128 ticks after. spike_is_shown() below computes this
 * directly from (T, moving, timer, stagger) with no stored per-entry state
 * and no loop over ticks the entry was never drawn on -- an off-window
 * entry now costs EXACTLY ZERO per-frame work (not even a loop iteration
 * touches it: spikes_draw()'s own obj_type_window() call already excludes
 * it from the candidate range below), and an entry that re-enters the
 * window gets the exact right answer on its very first frame back, no
 * "catch up" pass required. Proven byte-identical to the old incremental
 * spikeShown[]/spikes_tick_advance() implementation by a host harness
 * sweeping all 41 real entries across the full hazardTick period and a
 * full camera sweep -- see this task's own report. */
static uint8_t spike_is_shown(uint32_t tick, uint8_t moving, uint8_t timer, uint8_t stagger)
{
    uint32_t t0, matches;

    if (!moving) return SPIKE_SHOWN;   /* Spikes_Create:380-384's static case: never toggles */

    t0 = (uint32_t)timer + (stagger ? 64u : 0u);
    /* hazardTick is never 0 when this is queried -- spikes_draw() increments
     * it BEFORE the first draw, so the sequence of ticks the old
     * spikes_tick_advance() ever actually ran the match test against was
     * 1,2,3,... never 0. A nominal t0 of exactly 0 (timer==0 && stagger==0
     * -- true for 2 of GHZ1's real 3 moving spikes, indices 15 and 24 in
     * assets/ghz/spikes.bin) names a match at tick 0 that therefore never
     * really fired; its first REACHABLE occurrence is one full period
     * later, at tick 128, same as any other entry's t0 -- caught by this
     * task's own host harness (job1_2_spikes_harness.c) diffing against
     * the real per-tick incremental toggle for the actual GHZ1 entries: an
     * un-adjusted t0=0 flipped entries 15/24 SHOWN 128 ticks too early on
     * every single query. */
    if (t0 == 0) t0 = 128u;
    if (tick < t0) return SPIKE_HIDDEN;   /* first match hasn't happened yet */

    matches = (tick - t0) / 128u + 1u;    /* how many XOR toggles have fired by `tick` */
    return (uint8_t)(matches & 1u);       /* SPIKE_HIDDEN=0/SPIKE_SHOWN=1 by construction */
}

/* Reused shape of spring.c's spring_check_box() (Player_CheckCollisionBox /
 * Collision.cpp:276-461, setValues=true) -- kept here too, not shared,
 * because it is genuinely a different translation unit with its own static
 * state, same convention every other physics file in sh_src already
 * follows (no shared collision-box helper exists across files). This MD-side
 * copy is NEVER used for collision (that is sh_src/spikes.c's own job) --
 * it does not exist here at all; md_src only ever needs the SHOWN/HIDDEN
 * bit and the draw geometry, both position/orientation, not side-resolved
 * collision. */

static ObjTypeDesc spikesWindowDesc;   /* only .entries/.recordSize/.recordCount/.marginX are read (obj_type_window) */

static uint16_t spikesBase;
static void spikes_onBase(uint16_t base) { spikesBase = base; }
static void spikes_onLive(uint8_t live) { spikesLive = live; }

static ArenaClassDesc spikesArenaDesc = {
    (const void *)0, sizeof(SpikeEntry), SPIKES_COUNT,
    (const uint32_t *)0, SPIKES_MAX_FRAME_TILES * 2,   /* spikes_v (16) + spikes_h (16) = 32 */
    0, OBJ_PRI_HAZARD,
    spikes_onBase, spikes_onLive
};

void spikes_init(void)
{
    uint16_t i;
    uint8_t slot;
    uint16_t base;

    spikesLive = 0;
    hazardTick = 0;

    if (ghz_spikes_count != SPIKES_COUNT) return;

    for (i = 0; i < SPIKES_COUNT; i++) {
        const SpikeEntry *e = &ghz_spikes_xy[i];
        uint8_t dir = (uint8_t)(e->type & 1);
        uint8_t orient = (uint8_t)((e->type >> 1) & 1);
        uint8_t count = e->count;
        if (count < 2) count = 2;
        spikeCount[i] = count;
        spikeVert[i] = (uint8_t)(orient == 0);
        if (orient == 0) spikeSide[i] = dir ? C_BOTTOM : C_TOP;
        else              spikeSide[i] = dir ? C_LEFT : C_RIGHT;
        /* No spikeShown[] init any more -- spike_is_shown() (above) derives
         * "a moving spike starts fully retracted" (Spikes_Create:380-384)
         * directly from hazardTick==0 < t0 for every entry, since t0 is
         * always >= 0. */
    }

    spikesWindowDesc.entries = (const void *)ghz_spikes_xy;
    spikesWindowDesc.recordSize = sizeof(SpikeEntry);
    spikesWindowDesc.recordCount = SPIKES_COUNT;
    spikesWindowDesc.marginX = 64;   /* widest hitbox is 20*8=160px half-width */

    spikesArenaDesc.entries = (const void *)ghz_spikes_xy;
    spikesArenaDesc.tilePixels = spikes_tiles_md;
    spikesArenaDesc.lookaheadX = ARENA_LOOKAHEAD_X((uint16_t)(SPIKES_MAX_FRAME_TILES * 2));
    slot = obj_arena_register(&spikesArenaDesc);
    base = obj_arena_boot_load(slot);
    if (base == 0xFFFF) return;
    vdp_tiles_load(spikes_tiles_md, base, (uint16_t)(SPIKES_MAX_FRAME_TILES * 2));
    obj_arena_boot_done(slot);
}

uint16_t spikes_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                     uint16_t camX, uint16_t camY,
                     int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
    uint16_t lo, hi, i, n = 0;
    (void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;

    if (!spikesLive) return 0;

    /* Only the O(1) counter advance is unconditional now -- see this file's
     * own comment above spike_is_shown() (Job 1.2, this task) for why the
     * per-entry work that used to run here for all 41 entries every frame
     * is gone, not just windowed. */
    hazardTick++;

    obj_type_window(&spikesWindowDesc, camX, &lo, &hi);

    for (i = lo; i < hi && n < SPIKES_SPRITE_CAP; i++) {
        const SpikeEntry *e = &ghz_spikes_xy[i];
        const ObjFrame *f;
        uint8_t count, segs, k;
        int16_t startX, startY;

        if (spike_is_shown(hazardTick, e->moving, e->timer, e->stagger) != SPIKE_SHOWN) continue;

        count = spikeCount[i];
        segs = (uint8_t)((count >> 1) + (count & 1));
        f = spikeVert[i] ? spikes_v : spikes_h;

        /* Spikes_Draw_Global, Spikes.c:457-495: drawPos starts at
         * position +/- (16 - 8*count) along the spanning axis, steps 32px
         * per full segment, and the odd leftover segment (if any) is pulled
         * back 16px rather than drawn a further 32px out. */
        if (spikeVert[i]) {
            startX = (int16_t)(e->x + 16 - 8 * (int16_t)count);
            startY = e->y;
        } else {
            startX = e->x;
            startY = (int16_t)(e->y + 16 - 8 * (int16_t)count);
        }

        for (k = 0; k < segs && n < SPIKES_SPRITE_CAP; k++) {
            int16_t sx = startX, sy = startY;
            if (spikeVert[i]) sx = (int16_t)(startX + k * 32);
            else              sy = (int16_t)(startY + k * 32);
            if (k == segs - 1 && (count & 1)) {
                if (spikeVert[i]) sx = (int16_t)(sx - 16);
                else              sy = (int16_t)(sy - 16);
            }

            n = (uint16_t)(n + obj_emit_pieces(list, (uint16_t)(firstIndex + n),
                           (uint16_t)(firstLink + n), (uint16_t)(SPIKES_SPRITE_CAP - n),
                           &spikes_pieces[f->pieceOffset], f->pieceCount,
                           (uint16_t)(spikesBase + f->tileOffset), SPIKES_PAL,
                           (int16_t)(sx - (int16_t)camX), (int16_t)(sy - (int16_t)camY),
                           f->pivotX, f->pivotY, 0, 0, 0));
        }
    }
    return n;
}
