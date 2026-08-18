#include "spikelog.h"
#include "vdp.h"
#include "obj_data.h"
#include "obj_generic.h"
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

/* Rotation frames: streamed through md_src/obj_generic.h's per-class
 * ANIMATION WINDOW instead of held whole-sheet-resident (this class's own
 * previous approach -- see spikelog.h's own header comment for the VRAM
 * pressure that forced this and the lockstep cost it spends). Every log
 * shares the SAME rotation phase now (sharedTimer below), the same "one
 * shared clock" pattern rings.c's own rotation window already uses.
 * spikelog_rotate (spikelog_data.c, generated) is already the RAW frame
 * table this window wants -- every tileOffset is relative to
 * spikelog_tiles_md's own sheet start, not yet rebased to any VRAM address,
 * exactly obj_anim_window_register()'s own contract -- so unlike rings.c
 * (whose generated data had no ready-made ObjFrame[] table and had to build
 * one by hand into a RAM array) this file can register the generated const
 * table directly, no RAM copy needed. */
static ObjAnimWindow *spikelogWindow;   /* NULL until spikelog_init() succeeds */

static ObjDrawDecision spikelog_decide(void *st, uint16_t entryIndex, int16_t ex, int16_t ey,
                                       int16_t sonicWorldX, int16_t sonicWorldY,
                                       uint16_t sonicFrameIndex)
{
    ObjDrawDecision d;
    (void)st; (void)entryIndex; (void)ex; (void)ey;
    (void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;

    d.flipH = 0; d.flipV = 0;
    /* obj_generic.h's own two-line recipe (see rings.c's own ring_decide()
     * for the identical pattern this is copied from): obj_anim_window_
     * frames() is always exactly one row, so "draw the shared rotation
     * frame" is "index 0 of it, if it is actually resident this instant". */
    d.frame = obj_anim_window_live(spikelogWindow) ? 0 : OBJ_SKIP;
    return d;
}

static ObjTypeDesc spikelogType = {
    (const void *)0, sizeof(SpikeLogEntry), SPIKELOG_COUNT, ghz_spikelogs_count_p,
    (const uint32_t *)0, 0,
    (const ObjFrame *)0, /* patched in spikelog_init() to obj_anim_window_frames(spikelogWindow) */
    spikelog_pieces,
    OBJ_PRI_HAZARD, SPIKELOG_PAL, 0,
    16,
    spikelog_decide, (void *)0
};

/* entries/sheetPixels are patched in at runtime in spikelog_init() (same
 * "reading another static variable's VALUE is not a constant expression"
 * reason springs.c's own comment on springType gives). lookaheadX is built
 * with ARENA_LOOKAHEAD_X(SPIKELOG_MAX_FRAME_TILES) -- SPIKELOG_MAX_FRAME_
 * TILES is 6 (spikelog_data.h, the largest single frame across all 32 -- the
 * shortest ones are 4), so this window reserves 2*6=12 tiles total, against
 * 176 for the old whole-sheet residency this replaces. */
static ObjAnimWindowDesc spikelogAnimDesc = {
    (const void *)0, sizeof(SpikeLogEntry), SPIKELOG_COUNT,
    0, /* lookaheadX, patched below */
    OBJ_PRI_HAZARD,
    (const uint32_t *)0, spikelog_rotate, 32, SPIKELOG_MAX_FRAME_TILES
};

void spikelog_init(void)
{
    spikelogWindow = (ObjAnimWindow *)0;
    hazardTick = 0;
    sharedTimer = 0;

    if (ghz_spikelogs_count != SPIKELOG_COUNT) return;

    spikelogType.entries = (const void *)ghz_spikelogs_xy;

    spikelogAnimDesc.entries = (const void *)ghz_spikelogs_xy;
    spikelogAnimDesc.sheetPixels = spikelog_tiles_md;
    spikelogAnimDesc.lookaheadX = ARENA_LOOKAHEAD_X(SPIKELOG_MAX_FRAME_TILES);
    spikelogWindow = obj_anim_window_register(&spikelogAnimDesc);
    if (!spikelogWindow) return;
    /* Boot-load frame 0 (sharedTimer's own initial value) synchronously --
     * GHZ1's own spikelog scene has instances early enough in the level that
     * this avoids a pop-in on whichever ones sit inside the very first
     * camera view, same reasoning rings_init()'s own obj_anim_window_
     * boot_load() call gives. */
    if (obj_anim_window_boot_load(spikelogWindow, 0)) { spikelogWindow = (ObjAnimWindow *)0; return; }
    spikelogType.frames = obj_anim_window_frames(spikelogWindow);
}

uint16_t spikelog_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                       uint16_t camX, uint16_t camY,
                       int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
    if (!spikelogWindow) return 0;

    hazardTick++;
    sharedTimer = (uint8_t)((hazardTick / 3) & 0x1F);
    /* ONE shared request per tick, driving the ONE shared window every log
     * on screen reads from -- never once per candidate, see obj_generic.h's
     * own top-of-section comment. A no-op whenever the window is not
     * currently granted or already showing/loading this exact frame. */
    obj_anim_window_select(spikelogWindow, sharedTimer);

    return obj_type_draw(&spikelogType, list, firstIndex, firstLink, SPIKELOG_SPRITE_CAP,
                         camX, camY, sonicWorldX, sonicWorldY, sonicFrameIndex);
}
