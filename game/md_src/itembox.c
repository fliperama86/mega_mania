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

/* VRAM (REVISED 2026-08-18, VRAM capacity task): the class's tiles used to be
 * ONE contiguous 116-tile whole-sheet-resident arena tenant (box 16 + broken
 * 28 + contents 72 -- itembox_tiles_md's own generated layout, itembox_data.c)
 * admitted/evicted as a single unit. That single 116-tile request was too
 * big to ever be granted against the arena's pre-reclaim 203-tile size (it
 * was REFUSED outright at boot and stayed evicted for the whole run --
 * ItemBox drew NOTHING, ever) and is still a poor fit for the post-reclaim
 * 427-tile arena once every other class's own demand is counted (see this
 * task's own final report for the arithmetic). Split into THREE independent
 * tenants instead, sized and migrated according to what each ACTUALLY needs,
 * not migrated uniformly:
 *
 *   - "box" (16 tiles, exactly ONE frame, byte-identical for every instance
 *     regardless of type): a plain, small whole-sheet-resident arena tenant.
 *     No divergence exists to lose here at all -- there is nothing an anim
 *     window's double-buffering would even do differently -- so it just
 *     stays on the plain arena at its own true (tiny) size instead of
 *     dragging broken/contents along with it.
 *
 *   - "contents" (72 tiles, 18 icon sub-frames): KEPT whole-sheet resident
 *     on the plain arena too, deliberately NOT migrated to the per-class
 *     ANIMATION WINDOW mechanism (md_src/obj_generic.h) that SpikeLog and
 *     ItemBox's own "broken" family below both use. A box's contents icon is
 *     per-instance IDENTITY (WHICH reward it gives -- ring, shield, 1-up,
 *     ...), not an animation phase offset the way SpikeLog's rotation or a
 *     badnik's walk cycle is: this stage's own data shows ~10 different
 *     types simultaneously live across GHZ1 (itembox.h's own header
 *     comment). Collapsing that to the anim window's ONE shared frame would
 *     make every visible box but one show the WRONG reward icon at once --
 *     a correctness bug, not the cosmetic "everyone moves in unison" cost
 *     this task's "lockstep animation" sign-off covers. Exactly the "genuine
 *     state divergence... stays on obj_arena_register() instead" case
 *     obj_generic.h's own top-of-section comment calls out by name (using
 *     rings.c's own sparkle portion as its worked example) -- this is the
 *     same call, made here for the same reason.
 *
 *   - "broken" (3 poses, round-robined by instance index i%3 with NO tie to
 *     a box's own type -- ItemBox_Break, ItemBox.c: the original's own
 *     +1-mod-3 counter is arbitrary too, see itembox.h's own header
 *     comment): migrated to the anim window mechanism. Unlike contents,
 *     lockstepping every currently-broken box to the SAME one of the 3
 *     poses loses nothing semantic -- which of the 3 a given box showed was
 *     never tied to its type or any other observable state to begin with --
 *     so this genuinely is the cheap, cosmetic-only "lockstep animation"
 *     cost, not a correctness one. */
#define ITEMBOX_BOX_TILES           16   /* itembox_box[0].tileCount */
#define ITEMBOX_CONTENTS_TILES      72   /* itembox_contents[]'s 18 frames * 4 tiles each */
/* itembox_contents[0].tileOffset (itembox_data.c, generated) -- verified by
 * inspection, not computed at compile time (a generated array's own element
 * is not usable in a constant expression here). "contents" is now its OWN
 * separately-resident VRAM blob, not a sub-range of the old 116-tile
 * combined one, so every itembox_contents[i].tileOffset (44..112, absolute
 * within the FULL 224-tile ROM sheet) needs this subtracted back out to
 * land at 0..68 relative to that blob's own granted base. */
#define ITEMBOX_CONTENTS_TILE_BASE  44

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

/* "broken" -- see this file's own header comment above for why this family,
 * unlike contents, is a safe fit for the shared lockstep window.
 * itembox_broken (itembox_data.c, generated) is already the RAW frame table
 * (tileOffset absolute in the full sheet, not yet rebased to any VRAM
 * address) obj_anim_window_register() wants, same as spikelog.c's own
 * spikelog_rotate -- registered directly, no RAM copy needed. */
static ObjAnimWindow *itemboxBrokenWindow;   /* NULL until registered */

static ObjAnimWindowDesc itemboxBrokenAnimDesc = {
    (const void *)0, sizeof(BoxEntry), ITEMBOX_COUNT,
    0, /* lookaheadX, patched in itembox_init() */
    OBJ_PRI_RING,
    (const uint32_t *)0, itembox_broken, 3, ITEMBOX_MAX_FRAME_TILES
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
    itemboxBrokenWindow = (ObjAnimWindow *)0;
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
     * no frame-1 pop-in to avoid (obj_generic.h's own guidance on when
     * boot_load is worth calling at all). Its own lookaheadX is built from
     * ITEMBOX_MAX_FRAME_TILES (this window's own maxFrameTiles), not the
     * bigger contents-sized margin above -- matches every other migrated
     * window in this codebase (rings' rotation, spikelog's rotation). */
    itemboxBrokenAnimDesc.entries = (const void *)ghz_itemboxes_xy;
    itemboxBrokenAnimDesc.sheetPixels = itembox_tiles_md;
    itemboxBrokenAnimDesc.lookaheadX = ARENA_LOOKAHEAD_X(ITEMBOX_MAX_FRAME_TILES);
    itemboxBrokenWindow = obj_anim_window_register(&itemboxBrokenAnimDesc);
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
    if (!itemboxBoxLive && !itemboxContentsLive && !obj_anim_window_live(itemboxBrokenWindow))
        return 0;

    /* ONE shared request per tick, same "call unconditionally, let the
     * window decide if there is anything to do" rule every migrated window
     * in this codebase follows (obj_anim_window_select()'s own doc
     * comment). Always frame 0: which of the 3 broken poses a given box
     * shows was already an arbitrary per-instance round-robin, not tied to
     * any other state, so there is no "current" value to track across ticks
     * the way SpikeLog's rotation or a badnik's walk cycle needs -- this
     * only ever has one thing to ask for. */
    obj_anim_window_select(itemboxBrokenWindow, 0);

    obj_type_window(&itemboxWindowDesc, camX, &lo, &hi);

    for (i = lo; i < hi && n < ITEMBOX_SPRITE_CAP; i++) {
        const BoxEntry *e = &ghz_itemboxes_xy[i];
        int16_t sx, sy;
        if (e->hidden) continue;

        sx = (int16_t)(e->x - (int16_t)camX);
        sy = (int16_t)(e->y - (int16_t)camY);

        if (boxBroken[i]) {
            const ObjFrame *f;
            if (!obj_anim_window_live(itemboxBrokenWindow)) continue;
            f = obj_anim_window_frames(itemboxBrokenWindow);   /* one row, always index 0 */
            n = (uint16_t)(n + obj_emit_pieces(list, (uint16_t)(firstIndex + n),
                           (uint16_t)(firstLink + n), (uint16_t)(ITEMBOX_SPRITE_CAP - n),
                           &itembox_pieces[f->pieceOffset], f->pieceCount,
                           f->tileOffset, ITEMBOX_PAL,
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
             * own data (see this file's own header comment). */
            if (itemboxContentsLive) {
                const ObjFrame *fc = &itembox_contents[e->type < 18 ? e->type : 0];
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
