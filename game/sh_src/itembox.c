#include <stdint.h>
#include "itembox.h"

/* assets/ghz/itemboxes.bin's own row shape (tools/convert_objects.py's
 * ITEMBOX_SCENE), matching md_src/itembox.c's own BoxEntry -- see that
 * file's comment. */
typedef struct {
    int16_t x, y;
    uint8_t type, isFalling, hidden, direction, planeFilter, lrzConvPhys;
} BoxDef;

extern const uint16_t ghz_itemboxes_sh[];
static const BoxDef *const k_boxes = (const BoxDef *)((const uint8_t *)ghz_itemboxes_sh + 2);
#define BOX_COUNT (ghz_itemboxes_sh[0])

#define ITEMBOX_EGGMAN 10   /* ItemBoxTypes, ItemBox.h -- see md_src/itembox.c's own copy */

#define BOX_MAX 48   /* headroom above the converted 38 */
static uint8_t boxBroken[BOX_MAX];
static uint8_t boxInit, boxCount;

#define HB_LEFT   (-15)
#define HB_TOP    (-16)
#define HB_RIGHT    15
#define HB_BOTTOM   16

static void box_setup(void)
{
    uint16_t i, n = BOX_COUNT;
    if (n > BOX_MAX) n = BOX_MAX;
    for (i = 0; i < n; i++) boxBroken[i] = 0;
    boxCount = (uint8_t)n;
    boxInit = 1;
}

/* Player_CheckCollisionTouch-shaped test against ItemBox->hitboxItemBox
 * (ItemBox.c:182-185), same symmetric-AABB derivation as every other touch
 * test in this batch (rings.c/spring.c/spikelog.c's own comments). */
static uint8_t box_touches(Player *p, int32_t bx, int32_t by,
                           int8_t hbLeft, int8_t hbTop, int8_t hbRight, int8_t hbBottom)
{
    int32_t otherIX = p->e.x >> 16, otherIY = p->e.y >> 16;
    return bx + hbLeft   < otherIX + p->e.outer.right
        && bx + hbRight  > otherIX + p->e.outer.left
        && by + hbTop    < otherIY + p->e.outer.bottom
        && by + hbBottom > otherIY + p->e.outer.top;
}

/* Player-proximity gate: boxBroken[] is a monotonic, permanent latch (no
 * respawn/refill in this port -- see itembox_apply's own header comment on
 * the classes this port acts on), only ever changed by an actual player
 * touch, so skipping a far-away box can never miss/duplicate a break.
 * ghz_itemboxes_sh is x-sorted ascending (write_scene_table) and BoxDef's own
 * sizeof (10, matching ITEMBOX_SCENE's row_fmt ">hhBBBBBB" exactly --
 * verified against this project's actual sh-elf-gcc) has no padding hazard,
 * so a direct k_boxes[m].x read is safe. BOX_GATE_MARGIN: HB_LEFT/RIGHT are
 * fixed at +-15px (no motion, no per-type size variation); 15 + player
 * hitbox + buffer rounds up to 128. */
#define BOX_GATE_MARGIN 128

static void box_window(int32_t playerXpx, uint8_t n, uint8_t *lo, uint8_t *hi)
{
    int32_t xloWant = playerXpx - BOX_GATE_MARGIN;
    int32_t xhiWant = playerXpx + BOX_GATE_MARGIN;
    uint8_t a, b, m;

    a = 0; b = n;
    while (a < b) {
        m = (uint8_t)(a + (b - a) / 2);
        if (k_boxes[m].x < xloWant) a = (uint8_t)(m + 1); else b = m;
    }
    *lo = a;

    a = *lo; b = n;
    while (a < b) {
        m = (uint8_t)(a + (b - a) / 2);
        if (k_boxes[m].x < xhiWant) a = (uint8_t)(m + 1); else b = m;
    }
    *hi = a;
}

void itembox_apply(Player *p)
{
    uint8_t i, lo, hi;

    if (!boxInit) box_setup();

    /* ItemBox_CheckHit's attacking test (ItemBox.c:418-427), simplified --
     * see md_src/itembox.c's own comment on the identical MD-side
     * substitution (attacking + touching, dropping the velY>=0/onGround
     * gate neither side can keep in agreement without a comm bit neither
     * has to spare). */
    if (!player_is_attacking(p)) return;

    box_window(p->e.x >> 16, boxCount, &lo, &hi);

    for (i = lo; i < hi; i++) {
        const BoxDef *e = &k_boxes[i];
        if (boxBroken[i] || e->hidden) continue;

        if (box_touches(p, e->x, e->y, HB_LEFT, HB_TOP, HB_RIGHT, HB_BOTTOM)) {
            boxBroken[i] = 1;

            /* ItemBox_GivePowerup's ITEMBOX_EGGMAN case (ItemBox.c:538,
             * `Player_Hurt(player, self)`) -- the one type this CPU can act
             * on for real; every other type used by this stage either has
             * its effect applied 68000-side (Ring/HyperRing, md_src/
             * itembox.c) or has no system on either CPU to act on at all
             * (shields/Invincible/Sneakers/1UP_SONIC -- see itembox.h's own
             * header comment). */
            if (e->type == ITEMBOX_EGGMAN)
                player_hit(p, (int32_t)e->x << 16);
        }
    }
}
