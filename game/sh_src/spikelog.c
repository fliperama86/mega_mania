#include <stdint.h>
#include "spikelog.h"

/* assets/ghz/spikelogs.bin's own row shape (tools/convert_objects.py's
 * SPIKELOG_SCENE): x,y then the raw editor `frame` byte, matching
 * md_src/spikelog.c's own SpikeLogEntry -- see that file's comment. Linked
 * straight into this SH2 program's own image, same convention
 * sh_src/spring.c's own k_springs already uses.
 *
 * ROW STRIDE (fixed 2026-08-18, data-layout bug fix): SPIKELOG_SCENE's own
 * row_fmt is ">hhBx" (6 bytes, a trailing pad byte after `frame`), not the
 * logical ">hhB" (5 bytes) the three decoded fields alone would suggest --
 * SceneRecipe now asserts every struct-cast-read table packs to an EVEN
 * size for exactly this reason (tools/convert_objects.py). sizeof(
 * SpikeLogDef) is 6 (its largest member needs 2-byte alignment), so every
 * row here already lines up with what k_spikelogs[i] reads, on both CPUs
 * (md_src/spikelog.c's own SpikeLogEntry is the identical shape). This loop
 * is still NOT gated by a player-x window, unlike most of this batch's other
 * _apply() scans (2026-08-18 camera-X gating task) -- that is an unrelated,
 * pre-existing perf note, left as the full linear scan it already was. */
typedef struct {
    int16_t x, y;
    uint8_t frame;
} SpikeLogDef;

extern const uint16_t ghz_spikelogs_sh[];
static const SpikeLogDef *const k_spikelogs = (const SpikeLogDef *)((const uint8_t *)ghz_spikelogs_sh + 2);
#define SPIKELOG_COUNT (ghz_spikelogs_sh[0])

/* SpikeLog->hitboxSpikeLog, SpikeLog_StageLoad (SpikeLog.c:48-51). */
#define HB_LEFT   (-8)
#define HB_TOP    (-16)
#define HB_RIGHT    8
#define HB_BOTTOM   0

/* This CPU's own local stand-in for Zone->timer -- see md_src/spikelog.c's
 * own header comment for why each side keeps an independent copy. */
static uint32_t hazardTick;

/* Player_CheckCollisionTouch (Collision.cpp:209-241), same symmetric-AABB
 * shape rings.c's ring_touches_sonic()/spring.c's spring_touches() already
 * transcribe -- see either's own derivation comment for why the flip
 * transform is a provable no-op here too (SpikeLog's own direction is never
 * set away from FLIP_NONE, see spikelog.h's own header comment). */
static uint8_t spikelog_touches(Player *p, int32_t lx, int32_t ly,
                                int8_t hbLeft, int8_t hbTop, int8_t hbRight, int8_t hbBottom)
{
    int32_t thisIX = lx, thisIY = ly;
    int32_t otherIX = p->e.x >> 16, otherIY = p->e.y >> 16;

    return thisIX + hbLeft < otherIX + p->e.outer.right
        && thisIX + hbRight > otherIX + p->e.outer.left
        && thisIY + hbTop < otherIY + p->e.outer.bottom
        && thisIY + hbBottom > otherIY + p->e.outer.top;
}

void spikelog_apply(Player *p)
{
    uint32_t i, n = SPIKELOG_COUNT;
    uint8_t sharedTimer;

    hazardTick++;
    sharedTimer = (uint8_t)((hazardTick / 3) & 0x1F);

    for (i = 0; i < n; i++) {
        const SpikeLogDef *e = &k_spikelogs[i];
        uint8_t frameID = (uint8_t)(((uint8_t)(e->frame * 4) + sharedTimer) & 0x1F);

        /* SpikeLog_State_Main, SpikeLog.c:65-66: only frames 8-11 (the
         * "spikes facing out" quarter of the 32-frame rotation) are ever
         * dangerous -- `(frameID & 0xFFFFFFFC) != 8` skips every other
         * frame. player->shield != SHIELD_FIRE is unconditionally true in
         * this port (no shield system) -- see spikelog.h's own header
         * comment for the burn/chain branch this permanently skips. */
        if ((frameID & ~3) != 8) continue;

        if (spikelog_touches(p, e->x, e->y, HB_LEFT, HB_TOP, HB_RIGHT, HB_BOTTOM))
            player_hit(p, (int32_t)e->x << 16);   /* hazardWorldX is 16.16, matching p->e.x's own scale */
    }
}
