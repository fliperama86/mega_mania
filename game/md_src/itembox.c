#include "itembox.h"
#include "vdp.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_sprite.h"
#include "obj_pool.h"
#include "itembox_data.h"
#include "sonic_data.h"   /* ANI_JUMP, sonic_anims[] */
#include "rings.h"        /* rings_add() */
#include "assets_gen.h"

/* assets/ghz/itemboxes.bin row shape (tools/convert_objects.py's
 * ITEMBOX_SCENE, ItemBox_Serialize order, ItemBox.c:1254-1262). */
typedef struct {
    int16_t x, y;
    uint8_t type, isFalling, hidden, direction, planeFilter, lrzConvPhys;
} BoxEntry;

static const uint16_t *const ghz_itemboxes_count_p = ASSET_GHZ_ITEMBOXES;
#define ghz_itemboxes_count (*ghz_itemboxes_count_p)
static const BoxEntry *const ghz_itemboxes_xy =
    (const BoxEntry *)((const uint8_t *)ASSET_GHZ_ITEMBOXES + 2);
static const uint32_t *const itembox_tiles_md = ASSET_ITEMBOX_TILES;

/* ItemBoxTypes (ItemBox.h), the values this stage's own converted data
 * actually uses -- see itembox.h's own header comment for the full count
 * breakdown. */
#define ITEMBOX_RING           0
#define ITEMBOX_EGGMAN         10
#define ITEMBOX_HYPERRING      11

/* VRAM ART BUDGET (art-budget trim task, 2026-08-18, on top of the earlier
 * 2026-08-18 VRAM-capacity split into 3 independent tenants -- box, broken,
 * contents -- see the git history of this comment for that task's own
 * reasoning, unchanged below): tools/convert_objects.py's ITEMBOX_ART now
 * converts far less art than before:
 *
 *   - "box" (16 tiles, exactly ONE frame, byte-identical for every instance
 *     regardless of type): unchanged, already minimal.
 *
 *   - "contents": cut from all 18 ItemBoxTypes icons to EXACTLY the 10
 *     types GHZ1's own Scene1.bin actually places (ITEMBOX_ART's own
 *     comment lists them) -- 40 tiles, not 72. Still plain whole-sheet
 *     resident, deliberately NOT the anim-window mechanism, for the exact
 *     "genuine per-instance state divergence" reason this comment's earlier
 *     revision already gave (a box's reward ICON is which reward it is, not
 *     an animation phase -- collapsing 10 simultaneously-live types to one
 *     shared frame would show the WRONG reward on every box but one, a
 *     correctness bug). Because the kept types are no longer contiguous
 *     from 0 (type 8 ITEMBOX_1UP_TAILS and 9 ITEMBOX_1UP_KNUX are skipped --
 *     never placed in this stage), itembox_contents[] is no longer indexable
 *     BY TYPE VALUE directly the way ItemBox_GivePowerup's own
 *     contentsAnimator.frameID = self->type does in the original (ItemBox.c:
 *     1208) -- itemboxContentsRemap[] below maps a real type value to this
 *     array's own compact 0..9 index instead, built by hand from
 *     ITEMBOX_ART's own explicit frame_ids list (tools/convert_objects.py),
 *     which the two must be kept in sync with if that list ever changes.
 *
 *   - "broken": cut from 3 round-robined poses to the single CHEAPEST of the
 *     3 (ItemBox_Break's own +1-mod-3 counter was already arbitrary, tied to
 *     no other state -- see this comment's earlier revision), and MIGRATED
 *     OFF the anim-window mechanism entirely, onto the same plain
 *     whole-sheet residency as box/contents -- there is only ever one pose
 *     to show now, so a 2x-double-buffered streaming window bought nothing
 *     a single resident frame does not already give for less (8 tiles vs
 *     the old window's 32-tile reservation, 2x ITEMBOX_MAX_FRAME_TILES=16). */
#define ITEMBOX_BOX_TILES           16   /* itembox_box[0].tileCount */
#define ITEMBOX_BROKEN_TILES         8   /* itembox_broken[0].tileCount */
#define ITEMBOX_CONTENTS_TILES      40   /* itembox_contents[]'s 10 kept frames * 4 tiles each */
/* itembox_broken[0]/itembox_contents[0].tileOffset (itembox_data.c,
 * generated) -- verified by inspection, not computed at compile time (a
 * generated array's own element is not usable in a constant expression
 * here). Each tenant is its OWN separately-resident VRAM blob, so every
 * itembox_broken[i]/itembox_contents[i].tileOffset (absolute within the
 * FULL generated sheet) needs its own tenant's base subtracted back out to
 * land at 0-based relative to that blob's own granted base. */
#define ITEMBOX_BROKEN_TILE_BASE    16
#define ITEMBOX_CONTENTS_TILE_BASE  24

/* itembox_contents[]'s compact index for each real ItemBoxTypes value,
 * built BY HAND from ITEMBOX_ART's own explicit frame_ids list (tools/
 * convert_objects.py: [0,1,2,3,4,5,6,7,10,11], GHZ1's own 10 used reward
 * types, sorted ascending) -- index i of that list is compact index i here.
 * Sized to 12 (covers every real type value this stage's own data ever
 * emits, 0..11); indices 8/9 (ITEMBOX_1UP_TAILS/1UP_KNUX) are never actually
 * read (never placed in GHZ1's own Scene1.bin -- ITEMBOX_ART's own comment)
 * but default to 0 (RING) rather than left undefined, purely defensive. */
static const uint8_t itemboxContentsRemap[12] = {
    0, 1, 2, 3, 4, 5, 6, 7,   /* types 0..7  -> compact index 0..7 (identity) */
    0, 0,                     /* types 8, 9  -> never emitted; defensive 0 */
    8, 9                      /* type 10 (EGGMAN) -> 8, type 11 (HYPERRING) -> 9 */
};

static uint8_t boxBroken[ITEMBOX_COUNT];

static uint16_t itemboxBoxBase;
static uint8_t  itemboxBoxLive;
static void itembox_box_onBase(uint16_t base) { itemboxBoxBase = base; }
static void itembox_box_onLive(uint8_t live) { itemboxBoxLive = live; }

static ArenaClassDesc itemboxBoxArenaDesc = {
    (const void *)0, sizeof(BoxEntry), ITEMBOX_COUNT,
    (const uint32_t *)0, ITEMBOX_BOX_TILES,
    0, OBJ_PRI_RING,   /* see itembox.h's header comment / this task's report
                         * for why ItemBox uses the ring priority. */
    itembox_box_onBase, itembox_box_onLive
};

static uint16_t itemboxContentsBase;
static uint8_t  itemboxContentsLive;
static void itembox_contents_onBase(uint16_t base) { itemboxContentsBase = base; }
static void itembox_contents_onLive(uint8_t live) { itemboxContentsLive = live; }

static ArenaClassDesc itemboxContentsArenaDesc = {
    (const void *)0, sizeof(BoxEntry), ITEMBOX_COUNT,
    (const uint32_t *)0, ITEMBOX_CONTENTS_TILES,
    0, OBJ_PRI_RING,
    itembox_contents_onBase, itembox_contents_onLive
};

/* "broken" -- plain whole-sheet residency now (this file's own top comment),
 * a single resident frame, no ObjAnimWindow. */
static uint16_t itemboxBrokenBase;
static uint8_t  itemboxBrokenLive;
static void itembox_broken_onBase(uint16_t base) { itemboxBrokenBase = base; }
static void itembox_broken_onLive(uint8_t live) { itemboxBrokenLive = live; }

static ArenaClassDesc itemboxBrokenArenaDesc = {
    (const void *)0, sizeof(BoxEntry), ITEMBOX_COUNT,
    (const uint32_t *)0, ITEMBOX_BROKEN_TILES,
    0, /* lookaheadX, patched in itembox_init() */
    OBJ_PRI_RING,
    itembox_broken_onBase, itembox_broken_onLive
};

static ObjTypeDesc itemboxWindowDesc;   /* obj_type_window() only */

/* attacking(): mirrors sh_src/player.c's player_is_attacking() the same way
 * breakablewall.c's own attacking() does -- see that file's comment. */
static uint8_t attacking(uint16_t frameIndex)
{
    return frameIndex >= sonic_anims[ANI_JUMP].first
        && frameIndex < (uint16_t)(sonic_anims[ANI_JUMP].first + sonic_anims[ANI_JUMP].count);
}

#define SONIC_APPROX_HALF 8
/* ItemBox->hitboxItemBox, ItemBox_StageLoad (ItemBox.c:182-185). */
#define HB_LEFT   (-15)
#define HB_TOP    (-16)
#define HB_RIGHT    15
#define HB_BOTTOM   16

void itembox_init(void)
{
    uint16_t i, n = ghz_itemboxes_count;
    uint8_t slot;
    uint16_t base;

    itemboxBoxLive = 0;
    itemboxContentsLive = 0;
    itemboxBrokenLive = 0;
    for (i = 0; i < ITEMBOX_COUNT; i++) boxBroken[i] = 0;
    if (n != ITEMBOX_COUNT) return;

    itemboxWindowDesc.entries = (const void *)ghz_itemboxes_xy;
    itemboxWindowDesc.recordSize = sizeof(BoxEntry);
    itemboxWindowDesc.recordCount = ITEMBOX_COUNT;
    itemboxWindowDesc.marginX = 24;

    /* Box: boot-loaded synchronously so nothing pops in if an unbroken box
     * sits inside GHZ1's very first camera view. Deliberately given the
     * SAME (larger) lookaheadX as contents below, not its own true smaller
     * one -- box and contents are drawn together for every unbroken
     * instance, so admitting their windows at the same camera distance
     * means box (a fast 1-frame load) finishes and sits ready while
     * contents (a slower ~5-frame load) catches up, rather than the other
     * way around: a box shown briefly without its icon reads better than an
     * icon floating with no box around it. */
    itemboxBoxArenaDesc.entries = (const void *)ghz_itemboxes_xy;
    itemboxBoxArenaDesc.tilePixels = itembox_tiles_md;
    itemboxBoxArenaDesc.lookaheadX = ARENA_LOOKAHEAD_X(ITEMBOX_CONTENTS_TILES);
    slot = obj_arena_register(&itemboxBoxArenaDesc);
    base = obj_arena_boot_load(slot);
    if (base != 0xFFFF) {
        vdp_tiles_load(itembox_tiles_md, base, ITEMBOX_BOX_TILES);
        obj_arena_boot_done(slot);
    }

    /* Contents: registered (and thus, on a tie, admitted/loaded) AFTER box
     * -- see the comment above. Boot-loaded for the same pop-in reason. */
    itemboxContentsArenaDesc.entries = (const void *)ghz_itemboxes_xy;
    itemboxContentsArenaDesc.tilePixels = itembox_tiles_md + (uint32_t)ITEMBOX_CONTENTS_TILE_BASE * 8;
    itemboxContentsArenaDesc.lookaheadX = ARENA_LOOKAHEAD_X(ITEMBOX_CONTENTS_TILES);
    slot = obj_arena_register(&itemboxContentsArenaDesc);
    base = obj_arena_boot_load(slot);
    if (base != 0xFFFF) {
        vdp_tiles_load(itemboxContentsArenaDesc.tilePixels, base, ITEMBOX_CONTENTS_TILES);
        obj_arena_boot_done(slot);
    }

    /* Broken: NOT boot-loaded -- nothing starts already broken, so there is
     * no frame-1 pop-in to avoid (obj_generic.h's own guidance on when a
     * boot-time load is worth its synchronous cost at all). Its own
     * lookaheadX is built from its own real (small) resident size now, not a
     * single frame's -- same as every other migrated tenant in this batch. */
    itemboxBrokenArenaDesc.entries = (const void *)ghz_itemboxes_xy;
    itemboxBrokenArenaDesc.tilePixels = itembox_tiles_md + (uint32_t)ITEMBOX_BROKEN_TILE_BASE * 8;
    itemboxBrokenArenaDesc.lookaheadX = ARENA_LOOKAHEAD_X(ITEMBOX_BROKEN_TILES);
    (void)obj_arena_register(&itemboxBrokenArenaDesc);
}

/* ItemBox_CheckHit's attacking test (ItemBox.c:418-427), the Sonic-only
 * reduction: `anim==ANI_JUMP && (velY>=0 || onGround || direction)` -- this
 * stage's own data has direction==0 on every row (verified), so the
 * `|| self->direction` arm is dead weight, not transcribed. velY/onGround
 * are not on the comm wire (see rings.h's own top comment on what IS
 * published); this port approximates with "attacking + touching", the same
 * simplification md_src/breakablewall.c's own header comment documents and
 * justifies for the identical reason.
 *
 * FOUND WHILE MIGRATING, NOT INTRODUCED BY THIS TASK: this function used to
 * open with `if (!itemboxLive) return;`, tying hit detection (and the ring
 * award it triggers) to whether the class's WHOLE 116-tile sheet had ever
 * been granted VRAM -- under the pre-reclaim 203-tile arena that request was
 * always refused, so this early-out made ItemBox's hit detection dead code
 * for the entire run, not just its rendering. Hit detection has no actual
 * VRAM dependency (rings.c's own touch test is the same way -- see its own
 * comment: "the collect touch-test does not depend on tile residency at all
 * -- it never did"), so this gate is simply removed rather than rebuilt
 * against one of the three flags this file now has: a box's reward should
 * still register even on the rare tick its icon or broken pose is not yet
 * resident to draw. */
void itembox_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
    uint16_t i;
    uint8_t isAttacking;

    isAttacking = attacking(sonicFrameIndex);
    if (!isAttacking) return;

    for (i = 0; i < ITEMBOX_COUNT; i++) {
        const BoxEntry *e = &ghz_itemboxes_xy[i];
        if (boxBroken[i] || e->hidden) continue;

        if (sonicWorldX + SONIC_APPROX_HALF > e->x + HB_LEFT
            && sonicWorldX - SONIC_APPROX_HALF < e->x + HB_RIGHT
            && sonicWorldY + SONIC_APPROX_HALF > e->y + HB_TOP
            && sonicWorldY - SONIC_APPROX_HALF < e->y + HB_BOTTOM) {
            boxBroken[i] = 1;
            /* ItemBox_GivePowerup's ITEMBOX_RING/HYPERRING cases -- see
             * itembox.h's own header comment for why this fires here,
             * 68000-side, rather than crossing back from the SH2's own
             * independent break detection (sh_src/itembox.c): rings.c's
             * counter already lives on THIS cpu, so there is nothing to
             * cross at all. */
            if (e->type == ITEMBOX_RING || e->type == ITEMBOX_HYPERRING)
                rings_add(10);
        }
    }
}

uint16_t itembox_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                      uint16_t camX, uint16_t camY,
                      int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
    uint16_t lo, hi, i, n = 0;
    (void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;

    /* Fast bail-out: nothing any of the three tenants could draw is
     * resident yet (typical for the first few frames after a fresh
     * admission, or before this class's window has ever overlapped the
     * camera at all) -- skip the binary search below entirely. */
    if (!itemboxBoxLive && !itemboxContentsLive && !itemboxBrokenLive)
        return 0;

    obj_type_window(&itemboxWindowDesc, camX, &lo, &hi);

    for (i = lo; i < hi && n < ITEMBOX_SPRITE_CAP; i++) {
        const BoxEntry *e = &ghz_itemboxes_xy[i];
        int16_t sx, sy;
        if (e->hidden) continue;

        sx = (int16_t)(e->x - (int16_t)camX);
        sy = (int16_t)(e->y - (int16_t)camY);

        if (boxBroken[i]) {
            const ObjFrame *f;
            if (!itemboxBrokenLive) continue;
            f = &itembox_broken[0];   /* only one resident pose now -- see this file's own top comment */
            n = (uint16_t)(n + obj_emit_pieces(list, (uint16_t)(firstIndex + n),
                           (uint16_t)(firstLink + n), (uint16_t)(ITEMBOX_SPRITE_CAP - n),
                           &itembox_pieces[f->pieceOffset], f->pieceCount,
                           (uint16_t)(itemboxBrokenBase + (f->tileOffset - ITEMBOX_BROKEN_TILE_BASE)),
                           ITEMBOX_PAL,
                           sx, sy, f->pivotX, f->pivotY, 0, 0, 0));
        } else {
            if (itemboxBoxLive) {
                const ObjFrame *fb = &itembox_box[0];
                n = (uint16_t)(n + obj_emit_pieces(list, (uint16_t)(firstIndex + n),
                               (uint16_t)(firstLink + n), (uint16_t)(ITEMBOX_SPRITE_CAP - n),
                               &itembox_pieces[fb->pieceOffset], fb->pieceCount,
                               (uint16_t)(itemboxBoxBase + fb->tileOffset), ITEMBOX_PAL,
                               sx, sy, fb->pivotX, fb->pivotY, 0, 0, 0));
            }
            /* ItemBox_State_Idle, ItemBox.c:293-296: contentsPos.y =
             * position.y -/+ 3, direction always FLIP_NONE in this stage's
             * own data (see this file's own header comment). e->type is
             * remapped through itemboxContentsRemap[] to this class's own
             * compact (10-entry) resident index -- see that table's own
             * comment for why a direct `e->type` index no longer works. */
            if (itemboxContentsLive) {
                uint8_t idx = e->type < 12 ? itemboxContentsRemap[e->type] : 0;
                const ObjFrame *fc = &itembox_contents[idx];
                n = (uint16_t)(n + obj_emit_pieces(list, (uint16_t)(firstIndex + n),
                               (uint16_t)(firstLink + n), (uint16_t)(ITEMBOX_SPRITE_CAP - n),
                               &itembox_pieces[fc->pieceOffset], fc->pieceCount,
                               (uint16_t)(itemboxContentsBase + (fc->tileOffset - ITEMBOX_CONTENTS_TILE_BASE)),
                               ITEMBOX_PAL,
                               sx, (int16_t)(sy - 3), fc->pivotX, fc->pivotY, 0, 0, 0));
            }
        }
    }
    return n;
}
