#include <stdint.h>
#include "collapsingplatform.h"
#include "path.h"
#include "platform.h"   /* g_platform_tick */

#define TO_FIXED(x) ((int32_t)(x) << 16)

/* GHZ Scene1.bin's CollapsingPlatform entities (CollapsingPlatform_Serialize,
 * CollapsingPlatform.c:365-374). Not naturally aligned (size_x/size_y sit at
 * offsets 4/8, fine, but targetLayer's uint16 sits at offset 13, delay's at
 * 16 -- odd/misaligned for a direct struct cast on SH2), same reason sh_src/
 * platform.c reads its own 30-byte record byte-by-byte rather than casting a
 * struct pointer over it. */
extern const uint16_t ghz_collapsingplatforms_sh[];
static const uint8_t *const k_cp_bytes = (const uint8_t *)ghz_collapsingplatforms_sh + 2;

#define CP_COUNT 15   /* tools/convert_objects.py's kept count for GHZ1 */
#define CP_RECORD_SIZE 20

typedef struct {
	int16_t x, y;
	int32_t sizeX, sizeY;   /* 16.16 */
	uint8_t respawn;
	uint8_t type;
	uint16_t delay;
	uint8_t eventOnly;
} CPDef;

static int16_t rd_i16(const uint8_t *p) { return (int16_t)(((uint16_t)p[0] << 8) | p[1]); }
static int32_t rd_i32(const uint8_t *p)
{
	return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]);
}
static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

/* x-only read for cp_window() below, same reasoning platform.c's own
 * platform_x() comment gives. */
static int16_t cp_x(uint16_t i) { return rd_i16(k_cp_bytes + (uint32_t)i * CP_RECORD_SIZE); }

/* Player-proximity gate, same shape/rationale as platform.c's own
 * platform_window() (ghz_collapsingplatforms_sh is x-sorted ascending, same
 * write_scene_table convention). CP_GATE_MARGIN: this table's own hitboxTrigger
 * halfW is size.x>>17, and the actual converted data (assets/ghz/
 * collapsingplatforms.bin) tops out at size_x=11534336 -> halfW=88px --
 * every instance is otherwise static (no amplitude/push-style drift, unlike
 * Platform), so 88 (own halfW) + ~64 (player hitbox + buffer) rounds up to
 * 256, comfortably generous. */
#define CP_GATE_MARGIN 256

static void cp_window(int32_t playerXpx, uint16_t n, uint16_t *lo, uint16_t *hi)
{
	int32_t xloWant = playerXpx - CP_GATE_MARGIN;
	int32_t xhiWant = playerXpx + CP_GATE_MARGIN;
	uint16_t a, b, m;

	a = 0; b = n;
	while (a < b) {
		m = (uint16_t)(a + (b - a) / 2);
		if (cp_x(m) < xloWant) a = (uint16_t)(m + 1); else b = m;
	}
	*lo = a;

	a = *lo; b = n;
	while (a < b) {
		m = (uint16_t)(a + (b - a) / 2);
		if (cp_x(m) < xhiWant) a = (uint16_t)(m + 1); else b = m;
	}
	*hi = a;
}

static void cp_read(uint16_t i, CPDef *d)
{
	const uint8_t *rec = k_cp_bytes + (uint32_t)i * CP_RECORD_SIZE;
	d->x = rd_i16(rec + 0);
	d->y = rd_i16(rec + 2);
	d->sizeX = rd_i32(rec + 4);
	d->sizeY = rd_i32(rec + 8);
	d->respawn = rec[12];
	/* targetLayer (rec+13, 2 bytes) not read: purely a drawing-plane choice
	 * (Zone->fgLayer[0]/[1]), irrelevant to this port's own synthetic,
	 * non-tile-based collision box. */
	d->type = rec[15];
	d->delay = rd_u16(rec + 16);
	d->eventOnly = rec[18];
	/* mightyOnly (rec+19) not read: MANIA_USE_PLUS-only (Mighty's hammer-drop
	 * character), out of scope -- this port has no character switch. */
}

/* Per-instance runtime state: 0 = intact, 1 = countdown armed (a grounded
 * player has touched it at least once), 2 = collapsed (permanently
 * non-solid -- every GHZ1 instance has respawn==0, see collapsingplatform.h's
 * own comment, so this state never reverts). */
#define CP_INTACT    0
#define CP_ARMED     1
#define CP_COLLAPSED 2

static uint8_t  cpState[CP_COUNT];
static uint32_t cpArmTick[CP_COUNT];

void collapsingplatform_apply(Player *p)
{
	uint16_t n, i, lo, hi;

	n = ghz_collapsingplatforms_sh[0];
	if (n > CP_COUNT) n = CP_COUNT;

	/* cpState[]/cpArmTick[] latch through INTACT->ARMED->COLLAPSED only via
	 * an actual player touch (requires the player within this margin) and
	 * the ARMED->COLLAPSED delay is a plain (g_platform_tick - cpArmTick[i])
	 * difference against a shared clock that keeps advancing every tick
	 * regardless of this loop's own windowing (platform_apply() increments
	 * it unconditionally, and runs before this function in s_main.c) -- so
	 * an out-of-range instance's delay is never under- or over-counted, only
	 * evaluated a tick later than it would have been, which cannot matter
	 * while nothing could be standing on it. */
	cp_window(p->e.x >> 16, n, &lo, &hi);

	for (i = lo; i < hi; i++) {
		CPDef d;
		int32_t halfW, top;
		int32_t playerX, playerY;
		int8_t oL, oR, oB;
		int32_t surfaceY;

		cp_read(i, &d);
		if (cpState[i] == CP_COLLAPSED) continue;

		/* CollapsingPlatform_Create's own snap-to-8px-grid (Update:122-123,
		 * `position &= 0xFFF80000`) shifts the anchor by at most 8px --
		 * skipped here (this port keeps the scene's own authored pixel
		 * position exactly), a sub-8px, purely cosmetic hitbox-alignment
		 * difference against the original's own editor-grid snap. */
		halfW = (d.sizeX >> 17);          /* size.x>>16 (px) then /2 -- matches Create's own hitboxTrigger.right=size.x>>17 */
		top = -16 - (d.sizeY >> 17);      /* Create:154: hitboxTrigger.top = -16 - size.y>>17 */
		surfaceY = TO_FIXED(d.y) + TO_FIXED(top);

		playerX = p->e.x; playerY = p->e.y;
		oL = p->e.outer.left; oR = p->e.outer.right; oB = p->e.outer.bottom;

		if (cpState[i] == CP_INTACT) {
			/* CollapsingPlatform_Update:40-58: grounded, non-sidekick,
			 * !eventOnly, floor collision mode, touching the trigger box.
			 * player->collisionMode==0 is CMODE_FLOOR (path.h). */
			if (p->e.onGround && p->e.collisionMode == CMODE_FLOOR && !d.eventOnly
			    && playerX + TO_FIXED(oR) > TO_FIXED(d.x) - TO_FIXED(halfW)
			    && playerX + TO_FIXED(oL) < TO_FIXED(d.x) + TO_FIXED(halfW)
			    && playerY + TO_FIXED(oB) >= surfaceY - TO_FIXED(4)
			    && playerY + TO_FIXED(oB) <= surfaceY + TO_FIXED(4)) {
				cpState[i] = CP_ARMED;
				cpArmTick[i] = g_platform_tick;
				if (d.delay == 0) cpState[i] = CP_COLLAPSED;
			}
		}

		if (cpState[i] == CP_ARMED) {
			if (g_platform_tick - cpArmTick[i] >= d.delay) { cpState[i] = CP_COLLAPSED; continue; }
		}

		if (cpState[i] == CP_COLLAPSED) continue;

		/* Solid-from-above only, same 8px landing band sh_src/platform.c's
		 * own platform_land_top() uses (Bridge_HandleCollisions' own
		 * one-way band width, see that function's header comment) --
		 * CollapsingPlatform is a static top-only surface for as long as it
		 * is intact/armed, so a full copy of that helper is not worth
		 * factoring out for one extra call site. */
		if (p->e.velY >= 0
		    && playerX + TO_FIXED(oR) > TO_FIXED(d.x) - TO_FIXED(halfW)
		    && playerX + TO_FIXED(oL) < TO_FIXED(d.x) + TO_FIXED(halfW)
		    && playerY + TO_FIXED(oB) >= surfaceY
		    && playerY + TO_FIXED(oB) <= surfaceY + TO_FIXED(8)) {
			p->e.y = surfaceY - TO_FIXED(oB);
			if (p->e.velY > 0) p->e.velY = 0;
			p->e.onGround = 1;
			p->e.collisionMode = CMODE_FLOOR;
			p->e.groundVel = p->e.velX;
			p->controlLock = 0;
		}
	}
}
