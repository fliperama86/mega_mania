#include "badnik_base.h"
#include "sonic_data.h"
#include "assets_gen.h"

/* assets/sonic/hitbox.bin: same table rings.c/springs.c already read
 * (ASSET_SONIC_HITBOX, tools/convert_sonic.py). */
static const int8_t *const sonic_hitbox = ASSET_SONIC_HITBOX;

/* Player_FallbackHitbox (Player.c:12) -- same fallback rings.c's own
 * FALLBACK_HITBOX_* uses for a frameIndex this table has no row for. */
#define FALLBACK_HITBOX_LEFT   (-10)
#define FALLBACK_HITBOX_TOP    (-20)
#define FALLBACK_HITBOX_RIGHT    10
#define FALLBACK_HITBOX_BOTTOM   20

void badnik_sonic_hitbox(uint16_t sonicFrameIndex,
                          int8_t *l, int8_t *t, int8_t *r, int8_t *b)
{
	if (sonicFrameIndex < SONIC_FRAME_COUNT) {
		const int8_t *hb = &sonic_hitbox[sonicFrameIndex * 4];
		*l = hb[0]; *t = hb[1]; *r = hb[2]; *b = hb[3];
	} else {
		*l = FALLBACK_HITBOX_LEFT;   *t = FALLBACK_HITBOX_TOP;
		*r = FALLBACK_HITBOX_RIGHT;  *b = FALLBACK_HITBOX_BOTTOM;
	}
}

uint8_t badnik_touches_sonic(int16_t bx, int16_t by,
                              int8_t hbL, int8_t hbT, int8_t hbR, int8_t hbB,
                              int16_t sx, int16_t sy,
                              int8_t sHbL, int8_t sHbT, int8_t sHbR, int8_t sHbB)
{
	return bx + hbL < sx + sHbR
	    && bx + hbR > sx + sHbL
	    && by + hbT < sy + sHbB
	    && by + hbB > sy + sHbT;
}

uint8_t badnik_sonic_attacking(uint16_t sonicFrameIndex)
{
	uint16_t first = sonic_anims[ANI_JUMP].first;
	uint16_t count = sonic_anims[ANI_JUMP].count;
	return sonicFrameIndex >= first && sonicFrameIndex < (uint16_t)(first + count);
}

uint8_t badnik_is_destroyed(const uint8_t *bitmap, uint16_t i)
{
	return (uint8_t)((bitmap[i >> 3] >> (i & 7)) & 1);
}

void badnik_set_destroyed(uint8_t *bitmap, uint16_t i)
{
	bitmap[i >> 3] |= (uint8_t)(1 << (i & 7));
}

uint8_t badnik_decide_common(uint8_t *bitmap, uint16_t i,
                              int16_t bx, int16_t by,
                              int8_t hbL, int8_t hbT, int8_t hbR, int8_t hbB,
                              int16_t sonicWorldX, int16_t sonicWorldY,
                              uint16_t sonicFrameIndex)
{
	int8_t sl, st, sr, sb;

	if (badnik_is_destroyed(bitmap, i)) return 1;

	badnik_sonic_hitbox(sonicFrameIndex, &sl, &st, &sr, &sb);
	if (!badnik_touches_sonic(bx, by, hbL, hbT, hbR, hbB,
	                          sonicWorldX, sonicWorldY, sl, st, sr, sb))
		return 0;

	if (!badnik_sonic_attacking(sonicFrameIndex)) return 0;   /* survivable hurt: SH2-only consequence */

	badnik_set_destroyed(bitmap, i);
	return 1;
}
