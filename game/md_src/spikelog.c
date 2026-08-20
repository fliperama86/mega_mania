#include "spikelog.h"
#include "vdp.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_sprite.h"
#include "obj_pool.h"
#include "spikelog_data.h"
#include "assets_gen.h"

/* assets/ghz/spikelogs.bin row shape (tools/convert_objects.py's
 * SPIKELOG_SCENE): x,y then the raw editor `frame` byte (SpikeLog_
 * Serialize, SpikeLog.c:145), pre-multiply -- SpikeLog_Create:37 does
 * `self->frame *= 4` at runtime. On disk each row is 6 bytes (row_fmt
 * ">hhBx" -- a trailing pad byte after `frame`, no field of its own),
 * matching sizeof(SpikeLogEntry) here exactly: SceneRecipe (tools/
 * convert_objects.py) asserts every struct-cast-read table's row_fmt packs
 * to an EVEN size, so this struct and recordSize-strided readers like
 * obj_generic.c's entry_x()/entry_y() below always agree with the table's
 * real on-disk stride, row 0 onward. NOTE (2026-08-18 anim-window
 * migration): the `frame` byte itself is no longer read anywhere in this
 * file -- see spikelog.h's own header comment for why the per-instance
 * phase offset it used to drive is exactly the cost this migration spends.
 * Left in the struct only because it is the ROM row's real layout
 * (SpikeLogEntry has to match tools/convert_objects.py's own SPIKELOG_SCENE
 * byte-for-byte, whether or not this file still reads every field). */
typedef struct {
    int16_t x, y;
    uint8_t frame;
} SpikeLogEntry;

static const uint16_t *const ghz_spikelogs_count_p = ASSET_GHZ_SPIKELOGS;
#define ghz_spikelogs_count (*ghz_spikelogs_count_p)
static const SpikeLogEntry *const ghz_spikelogs_xy =
    (const SpikeLogEntry *)((const uint8_t *)ASSET_GHZ_SPIKELOGS + 2);
static const uint32_t *const spikelog_tiles_md = ASSET_SPIKELOG_TILES;

/* SpikeLog_StaticUpdate, SpikeLog.c:20: `SpikeLog->timer = Zone->timer / 3 &
 * 0x1F` -- Zone->timer stands in for hazardTick, this class's own local
 * counter (see spikelog.h's own header comment for why a locally-owned
 * counter, not a shared one, is this port's substitute). Incremented once
 * per displayed frame from spikelog_draw() below, same "no separate
 * OBJ_TYPE_LIST tick" shape rings_update() already uses. */
static uint32_t hazardTick;
static uint8_t sharedTimer;   /* (hazardTick/3) & 0x1F, recomputed each call */

/* VRAM ART BUDGET (art-budget trim task, 2026-08-18): rotation frames used
 * to STREAM through md_src/obj_generic.h's per-class ANIMATION WINDOW (a
 * 12-tile reservation, 2x SPIKELOG_MAX_FRAME_TILES, cycling one frame at a
 * time through the full 32-frame rotation -- the exact churn this whole
 * task exists to kill). tools/convert_objects.py's SPIKELOG_ART now converts
 * only SPIKELOG_KEPT_FRAMES=2 of those 32, half the rotation apart
 * (even_subsample) -- spikelog_rotate[] is now a small, PERMANENTLY
 * VRAM-resident pair (8 tiles total, spikelog_arena_onBase() below), no
 * streaming, no churn. sharedTimer (0..31, SpikeLog_StaticUpdate's own
 * "Zone->timer/3 & 0x1F") maps onto this 2-frame resident set with
 * sharedTimer>>4 (32/2=16 raw steps per kept frame) rather than indexing the
 * full 32-row table directly -- every log now visibly holds one of 2 poses
 * instead of rotating smoothly, the trade this task's own brief calls out by
 * name. */
#define SPIKELOG_KEPT_FRAMES 2

static ObjFrame  slFrames[SPIKELOG_KEPT_FRAMES];
static uint16_t  slBase;
static uint8_t   slLive;

/* PRECOMPUTED PIECE TEMPLATES (Job 1, lever 1, this task) -- see
 * obj_sprite.h's own top-of-section comment and crabmeat.c's own identical
 * pattern (this file's twin). Sized SPIKELOG_KEPT_FRAMES *
 * SPIKELOG_MAX_FRAME_PIECES, an upper bound on spikelog_pieces[]'s own real
 * length. spikelog_decide() never sets d.flipH nonzero, so slTemplatesH1 is
 * never actually selected at runtime -- built anyway for uniformity/safety
 * with every other templated type. Rebuilt every time
 * spikelog_arena_onBase() below fires. */
static ObjPieceTemplate slTemplatesH0[SPIKELOG_KEPT_FRAMES * SPIKELOG_MAX_FRAME_PIECES];
static ObjPieceTemplate slTemplatesH1[SPIKELOG_KEPT_FRAMES * SPIKELOG_MAX_FRAME_PIECES];

static void spikelog_rebuild_templates(void)
{
    uint8_t i;
    for (i = 0; i < SPIKELOG_KEPT_FRAMES; i++) {
        const ObjFrame *f = &slFrames[i];
        /* drawPriority literal 0 here matches spikelogType's own
         * drawPriority field below. */
        obj_build_piece_templates(&slTemplatesH0[f->pieceOffset], &spikelog_pieces[f->pieceOffset], f->pieceCount,
                                  f->tileOffset, SPIKELOG_PAL, 0, f->pivotX, f->pivotY, 0, 0);
        obj_build_piece_templates(&slTemplatesH1[f->pieceOffset], &spikelog_pieces[f->pieceOffset], f->pieceCount,
                                  f->tileOffset, SPIKELOG_PAL, 0, f->pivotX, f->pivotY, 1, 0);
    }
}

static ObjDrawDecision spikelog_decide(void *st, uint16_t entryIndex, int16_t ex, int16_t ey,
                                       int16_t sonicWorldX, int16_t sonicWorldY,
                                       uint16_t sonicFrameIndex)
{
    ObjDrawDecision d;
    (void)st; (void)entryIndex; (void)ex; (void)ey;
    (void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;

    /* SpikeLog animates in place, never moves -- see obj_data.h's own
     * ObjDrawDecision comment (Job 2, this task). */
    d.flipH = 0; d.flipV = 0; d.offX = 0; d.offY = 0;
    /* sharedTimer is 0..31; >>4 maps it onto this class's own 2 resident
     * frames (0 for raw 0..15, 1 for raw 16..31) -- always in range. */
    d.frame = slLive ? (uint16_t)(sharedTimer >> 4) : OBJ_SKIP;
    return d;
}

static ObjTypeDesc spikelogType = {
    (const void *)0, sizeof(SpikeLogEntry), SPIKELOG_COUNT, ghz_spikelogs_count_p,
    (const uint32_t *)0, 0,
    slFrames, spikelog_pieces,
    OBJ_PRI_HAZARD, SPIKELOG_PAL, 0,
    16,
    spikelog_decide, (void *)0,
    slTemplatesH0, slTemplatesH1
};

/* Plain whole-sheet VRAM residency (md_src/obj_generic.h's ArenaClassDesc) --
 * replaces the old obj_anim_window streaming reservation. See motobug.c's
 * own comment on the identical pattern; lookaheadX is built off this
 * class's own real (small) resident set now, not a single frame's. */
static void spikelog_arena_onBase(uint16_t base);
static void spikelog_arena_onLive(uint8_t live) { slLive = live; }

static ArenaClassDesc spikelogArenaDesc = {
    (const void *)0, sizeof(SpikeLogEntry), SPIKELOG_COUNT,
    (const uint32_t *)0, SPIKELOG_KEPT_FRAMES * SPIKELOG_MAX_FRAME_TILES,
    (int16_t)ARENA_LOOKAHEAD_X(SPIKELOG_KEPT_FRAMES * SPIKELOG_MAX_FRAME_TILES),
    OBJ_PRI_HAZARD,
    spikelog_arena_onBase, spikelog_arena_onLive
};

static void spikelog_arena_onBase(uint16_t base)
{
    uint8_t i;
    slBase = base;
    for (i = 0; i < SPIKELOG_KEPT_FRAMES; i++)
        slFrames[i].tileOffset = (uint16_t)(base + spikelog_rotate[i].tileOffset);
    spikelog_rebuild_templates();   /* Job 1, lever 1: only tileOffset changed above, and only here */
}

void spikelog_init(void)
{
    uint8_t i;

    slLive = 0;
    hazardTick = 0;
    sharedTimer = 0;

    for (i = 0; i < SPIKELOG_KEPT_FRAMES; i++) {
        slFrames[i].tileOffset = 0;
        slFrames[i].pieceOffset = spikelog_rotate[i].pieceOffset;
        slFrames[i].tileCount = spikelog_rotate[i].tileCount;
        slFrames[i].pieceCount = spikelog_rotate[i].pieceCount;
        slFrames[i].pivotX = spikelog_rotate[i].pivotX;
        slFrames[i].pivotY = spikelog_rotate[i].pivotY;
        slFrames[i].duration = spikelog_rotate[i].duration;
    }

    if (ghz_spikelogs_count != SPIKELOG_COUNT) return;

    spikelogType.entries = (const void *)ghz_spikelogs_xy;

    spikelogArenaDesc.entries = (const void *)ghz_spikelogs_xy;
    spikelogArenaDesc.tilePixels = spikelog_tiles_md;
    {
        uint8_t slot = obj_arena_register(&spikelogArenaDesc);
        uint16_t base;
        if (slot == ARENA_INVALID_SLOT) return;
        /* Boot-loaded synchronously -- GHZ1's own spikelog scene has
         * instances early enough in the level that this avoids a pop-in on
         * whichever ones sit inside the very first camera view, same
         * reasoning rings_init()'s own boot-time load gives. */
        base = obj_arena_boot_load(slot);
        if (base == 0xFFFF) return;
        vdp_tiles_load(spikelog_tiles_md, base, SPIKELOG_KEPT_FRAMES * SPIKELOG_MAX_FRAME_TILES);
        obj_arena_boot_done(slot);
    }
}

uint16_t spikelog_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                       uint16_t camX, uint16_t camY,
                       int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
    if (!slLive) return 0;

    hazardTick++;
    sharedTimer = (uint8_t)((hazardTick / 3) & 0x1F);

    return obj_type_draw(&spikelogType, list, firstIndex, firstLink, SPIKELOG_SPRITE_CAP,
                         camX, camY, sonicWorldX, sonicWorldY, sonicFrameIndex);
}
