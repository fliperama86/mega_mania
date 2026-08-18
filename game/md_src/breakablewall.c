#include "breakablewall.h"
#include "obj_pool.h"
#include "sonic_data.h"   /* ANI_JUMP, sonic_anims[] -- see breakablewall_tick()'s own comment */
#include "assets_gen.h"

/* assets/ghz/breakablewalls.bin row shape (tools/convert_objects.py's
 * BREAKABLEWALL_SCENE, BreakableWall_Serialize order, BreakableWall.c:
 * 828-834): x,y,type,onlyKnux,onlyMighty,priority, then size.x/size.y RAW
 * 32-bit (the converter's own derive() splits the decoded VAR_VECTOR2 into
 * two "i" fields, unscaled -- BreakableWall_Create:69-70 does its own
 * `size.x >>= 0x10` FROM_FIXED at runtime, transcribed below). */
typedef struct {
    int16_t x, y;
    uint8_t type, onlyKnux, onlyMighty, priority;
    int32_t size_x, size_y;
} WallEntry;

static const uint16_t *const ghz_breakablewalls_count_p = ASSET_GHZ_BREAKABLEWALLS;
#define ghz_breakablewalls_count (*ghz_breakablewalls_count_p)
static const WallEntry *const ghz_breakablewalls_xy =
    (const WallEntry *)((const uint8_t *)ASSET_GHZ_BREAKABLEWALLS + 2);

/* Precomputed once: block-space footprint and which plane (BreakableWall_
 * Create:67, priority==BREAKWALL_PRIO_HIGH(0) -> FG High/layer 1, else FG
 * Low/layer 0) each instance overrides. */
static int16_t wallBlockX0[BREAKABLEWALL_COUNT], wallBlockY0[BREAKABLEWALL_COUNT];
static uint8_t wallBlockW[BREAKABLEWALL_COUNT], wallBlockH[BREAKABLEWALL_COUNT];
static uint8_t wallLayer[BREAKABLEWALL_COUNT];
static uint8_t wallBroken[BREAKABLEWALL_COUNT];
static uint8_t wallCount;

static uint8_t breakablewallLive;

void breakablewall_init(void)
{
    uint16_t i, n = ghz_breakablewalls_count;
    breakablewallLive = 0;
    if (n != BREAKABLEWALL_COUNT) return;

    for (i = 0; i < BREAKABLEWALL_COUNT; i++) {
        const WallEntry *e = &ghz_breakablewalls_xy[i];
        int16_t sx = (int16_t)(e->size_x >> 16);
        int16_t sy = (int16_t)(e->size_y >> 16);
        if (sx <= 0) sx = 2;   /* BreakableWall_Create's own per-type default when size.x==0 */
        if (sy <= 0) sy = 4;
        wallBlockX0[i] = (int16_t)((e->x >> 4) - sx / 2);
        wallBlockY0[i] = (int16_t)((e->y >> 4) - sy / 2);
        wallBlockW[i] = (uint8_t)sx;
        wallBlockH[i] = (uint8_t)sy;
        wallLayer[i] = (uint8_t)(e->priority == 0 ? 1 : 0);
        wallBroken[i] = 0;
    }
    wallCount = BREAKABLEWALL_COUNT;
    breakablewallLive = 1;
}

/* attacking(), the MD-side mirror of sh_src/player.c's player_is_attacking()
 * (`p->animator.anim == ANI_JUMP`): sonic_anims[ANI_JUMP] gives the same
 * frame-index range this port's SH2 side checks by anim ID directly -- the
 * 68000 never learns the anim ID itself (only the absolute frame index is on
 * the wire, comm.h's COMM_ANIM), so it asks "does frameIndex fall inside
 * ANI_JUMP's own range" instead, same technique rings.c's frame_in_hurt()
 * already uses for ANI_HURT. */
static uint8_t attacking(uint16_t frameIndex)
{
    return frameIndex >= sonic_anims[ANI_JUMP].first
        && frameIndex < (uint16_t)(sonic_anims[ANI_JUMP].first + sonic_anims[ANI_JUMP].count);
}

/* Sonic's own current-frame outer hitbox is not needed here (unlike rings.c/
 * springs.c's touch tests): BreakableWall.c's own break test is a plain
 * Player_CheckCollisionTouch against a FIXED, generous hitbox
 * (BreakableWall_Create:123-126, +/-8*size.x/+/-8*size.y) -- this port
 * approximates Sonic's own half-width/half-height as a constant 8px (close
 * to every rolling/curled hitbox width this port's own sonic_data.c
 * generates) rather than threading a second per-frame hitbox table through
 * for a class that already drops several other exactness knobs (see this
 * file's own header comment). */
#define SONIC_APPROX_HALF 8

/* breakablewall_tick()'s own Sonic-proximity gate (2026-08-18, 68000
 * per-frame cost task). GHZ1's own breakablewalls.bin data (verified
 * directly, not assumed) tops out at 2 blocks wide (16px half-width) and 9
 * blocks tall (72px half-height); 96px of slack past the widest of those,
 * plus SONIC_APPROX_HALF, comfortably covers every instance this stage
 * actually has with real room to spare. */
#define BREAKABLEWALL_TICK_MARGIN 96

void breakablewall_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
    uint16_t i;
    uint8_t isAttacking;

    if (!breakablewallLive) return;
    isAttacking = attacking(sonicFrameIndex);
    if (!isAttacking) return;

    for (i = 0; i < wallCount; i++) {
        int16_t hbHalfW, hbHalfH, cx, cy, left, right, top, bottom;
        if (wallBroken[i]) continue;

        /* wallBlockX0/Y0 already carry the -size/2 block offset (init), so
         * the instance's own world CENTER is simply (blockX0+w/2)*16,
         * (blockY0+h/2)*16 -- recovered here rather than stored twice.
         * Computed (and gated on) BEFORE the rest of this iteration's own
         * arithmetic -- see BREAKABLEWALL_TICK_MARGIN's own comment -- since
         * wallBroken[i] is this loop's only state and the touch test itself
         * has none, skipping a far instance is exactly equivalent to
         * running its full test and getting "no touch". */
        cx = (int16_t)((wallBlockX0[i] + wallBlockW[i] / 2) << 4);
        if (cx < sonicWorldX - BREAKABLEWALL_TICK_MARGIN || cx > sonicWorldX + BREAKABLEWALL_TICK_MARGIN)
            continue;

        hbHalfW = (int16_t)(8 * wallBlockW[i]);
        hbHalfH = (int16_t)(8 * wallBlockH[i]);
        cy = (int16_t)((wallBlockY0[i] + wallBlockH[i] / 2) << 4);
        left = (int16_t)(cx - hbHalfW);
        right = (int16_t)(cx + hbHalfW);
        top = (int16_t)(cy - hbHalfH);
        bottom = (int16_t)(cy + hbHalfH);

        if (sonicWorldX + SONIC_APPROX_HALF > left && sonicWorldX - SONIC_APPROX_HALF < right
            && sonicWorldY + SONIC_APPROX_HALF > top && sonicWorldY - SONIC_APPROX_HALF < bottom)
            wallBroken[i] = 1;
    }
}

uint16_t breakablewall_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                            uint16_t camX, uint16_t camY,
                            int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
    (void)list; (void)firstIndex; (void)firstLink; (void)camX; (void)camY;
    (void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;
    return 0;
}

uint8_t breakablewall_block_override(uint16_t blockX, uint16_t blockY, uint8_t layer)
{
    uint8_t i;
    if (!breakablewallLive) return 0;
    for (i = 0; i < wallCount; i++) {
        if (!wallBroken[i] || wallLayer[i] != layer) continue;
        if ((int16_t)blockX >= wallBlockX0[i] && (int16_t)blockX < wallBlockX0[i] + wallBlockW[i]
            && (int16_t)blockY >= wallBlockY0[i] && (int16_t)blockY < wallBlockY0[i] + wallBlockH[i])
            return 1;
    }
    return 0;
}
