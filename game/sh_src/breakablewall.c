#include <stdint.h>
#include "breakablewall.h"

/* assets/ghz/breakablewalls.bin's own row shape (tools/convert_objects.py's
 * BREAKABLEWALL_SCENE), matching md_src/breakablewall.c's own WallEntry --
 * see that file's comment. x/y/type/onlyKnux/onlyMighty/priority are all
 * <=16-bit fields, so a direct struct-cast is always safe for them (SH2
 * only needs 2-byte alignment for those, and the table's own base is
 * guaranteed 2-byte aligned by tools/gen_assets.py's manifest -- same
 * reasoning sh_src/spikes.c's SpikeDef/sh_src/itembox.c's BoxDef document).
 * size_x/size_y are int32_t, though, and this table is only requested at
 * align=2, not align=4: whether they land 4-aligned depends on this
 * build's own packed offset for ghz_breakablewalls (currently they do, by
 * coincidence of what happens to be packed ahead of it -- not by any
 * guarantee), so they are read byte-by-byte below instead of through the
 * struct, the same hazard and the same fix sh_src/spin_booster.c's
 * boostPower and sh_src/platform.c's amplitude/tileOrigin/angle already
 * apply to their own tables' int32 fields. */
typedef struct {
    int16_t x, y;
    uint8_t type, onlyKnux, onlyMighty, priority;
    int32_t size_x, size_y;
} WallDef;

extern const uint16_t ghz_breakablewalls_sh[];
static const WallDef *const k_walls = (const WallDef *)((const uint8_t *)ghz_breakablewalls_sh + 2);
#define WALL_COUNT (ghz_breakablewalls_sh[0])

static int32_t read_i32_be(const uint8_t *p)
{
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                    | ((uint32_t)p[2] << 8) | (uint32_t)p[3]);
}

#define WALL_MAX 32   /* headroom above the converted 23 */
static int16_t wallBlockX0[WALL_MAX], wallBlockY0[WALL_MAX];
static uint8_t wallBlockW[WALL_MAX], wallBlockH[WALL_MAX];
static uint8_t wallLayer[WALL_MAX];
static uint8_t wallBroken[WALL_MAX];
static uint8_t wallInit, wallCount;

static void wall_setup(void)
{
    uint16_t i, n = WALL_COUNT;
    if (n > WALL_MAX) n = WALL_MAX;
    for (i = 0; i < n; i++) {
        const WallDef *e = &k_walls[i];
        /* offsetof(WallDef, size_x)==8, size_y==12 -- see this file's top
         * comment for why these two fields cannot go through `e->` directly. */
        int16_t sx = (int16_t)(read_i32_be((const uint8_t *)e + 8) >> 16);
        int16_t sy = (int16_t)(read_i32_be((const uint8_t *)e + 12) >> 16);
        if (sx <= 0) sx = 2;
        if (sy <= 0) sy = 4;
        wallBlockX0[i] = (int16_t)((e->x >> 4) - sx / 2);
        wallBlockY0[i] = (int16_t)((e->y >> 4) - sy / 2);
        wallBlockW[i] = (uint8_t)sx;
        wallBlockH[i] = (uint8_t)sy;
        wallLayer[i] = (uint8_t)(e->priority == 0 ? 1 : 0);
        wallBroken[i] = 0;
    }
    wallCount = (uint8_t)n;
    wallInit = 1;
}

/* Sonic's own approximate half-width/half-height for the touch test -- see
 * md_src/breakablewall.c's own comment on SONIC_APPROX_HALF for why this
 * port uses one constant rather than threading Sonic's real per-frame
 * hitbox through (this class already drops several other exactness knobs,
 * see this file's own header comment). */
#define SONIC_APPROX_HALF 8

/* Player-proximity gate: wallBroken[] is a monotonic, permanent latch (no
 * respawn in this stage's data, see this file's own comment above) that
 * only ever changes through an actual player touch -- so skipping a
 * far-away entry can never miss a break or un-break one, only defer testing
 * it until the player is close enough to matter. ghz_breakablewalls_sh is
 * x-sorted ascending (write_scene_table) and WallDef's own sizeof (16,
 * matching BREAKABLEWALL_SCENE's row_fmt ">hhBBBBii" exactly -- verified
 * against this project's actual sh-elf-gcc) has no padding hazard, so a
 * direct k_walls[m].x read is safe. WALL_GATE_MARGIN: every entry's own
 * hbHalfW is 8*wallBlockW[i], and this stage's own data (assets/ghz/
 * breakablewalls.bin) has size_x fixed at 2 blocks -> hbHalfW=16px always;
 * 16 + SONIC_APPROX_HALF(8) + a generous buffer rounds up to 128. */
#define WALL_GATE_MARGIN 128

static void wall_window(int32_t playerXpx, uint8_t n, uint8_t *lo, uint8_t *hi)
{
    int32_t xloWant = playerXpx - WALL_GATE_MARGIN;
    int32_t xhiWant = playerXpx + WALL_GATE_MARGIN;
    uint8_t a, b, m;

    a = 0; b = n;
    while (a < b) {
        m = (uint8_t)(a + (b - a) / 2);
        if (k_walls[m].x < xloWant) a = (uint8_t)(m + 1); else b = m;
    }
    *lo = a;

    a = *lo; b = n;
    while (a < b) {
        m = (uint8_t)(a + (b - a) / 2);
        if (k_walls[m].x < xhiWant) a = (uint8_t)(m + 1); else b = m;
    }
    *hi = a;
}

void breakablewall_apply(Player *p)
{
    uint8_t i, lo, hi;
    int32_t sonicX, sonicY;

    if (!wallInit) wall_setup();
    if (!player_is_attacking(p)) return;

    sonicX = p->e.x >> 16;
    sonicY = p->e.y >> 16;

    wall_window(sonicX, wallCount, &lo, &hi);

    for (i = lo; i < hi; i++) {
        int16_t hbHalfW, hbHalfH, cx, cy;
        int32_t left, right, top, bottom;

        if (wallBroken[i]) continue;

        hbHalfW = (int16_t)(8 * wallBlockW[i]);
        hbHalfH = (int16_t)(8 * wallBlockH[i]);
        cx = (int16_t)((wallBlockX0[i] + wallBlockW[i] / 2) << 4);
        cy = (int16_t)((wallBlockY0[i] + wallBlockH[i] / 2) << 4);
        left = cx - hbHalfW; right = cx + hbHalfW;
        top = cy - hbHalfH;  bottom = cy + hbHalfH;

        /* BreakableWall_CheckBreak_Wall's own Player_CheckCollisionTouch
         * gate (BreakableWall.c:340), simplified: see md_src/breakablewall.h's
         * own header comment for why the onGround/groundVel>=0x48000 extra
         * gate is dropped on both sides rather than kept SH2-only. */
        if (sonicX + SONIC_APPROX_HALF > left && sonicX - SONIC_APPROX_HALF < right
            && sonicY + SONIC_APPROX_HALF > top && sonicY - SONIC_APPROX_HALF < bottom)
            wallBroken[i] = 1;
    }
}

uint8_t breakablewall_solid_override(int32_t blockX, int32_t blockY, uint8_t layer)
{
    uint8_t i;
    if (!wallInit) wall_setup();
    for (i = 0; i < wallCount; i++) {
        if (!wallBroken[i] || wallLayer[i] != layer) continue;
        if (blockX >= wallBlockX0[i] && blockX < wallBlockX0[i] + wallBlockW[i]
            && blockY >= wallBlockY0[i] && blockY < wallBlockY0[i] + wallBlockH[i])
            return 1;
    }
    return 0;
}
