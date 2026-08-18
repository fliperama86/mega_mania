#include "batbrain.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_pool.h"
#include "batbrain_data.h"
#include "badnik_base.h"
#include "assets_gen.h"

#define BATBRAIN_COUNT 7   /* Batbrain_Serialize is empty, Batbrain.c:221 -- position-only record */

typedef struct { int16_t x, y; } BatbrainEntry;

static const uint16_t *const ghz_batbrain_count_p = ASSET_GHZ_BATBRAINS;
static const BatbrainEntry *const ghz_batbrain_xy =
	(const BatbrainEntry *)((const uint8_t *)ASSET_GHZ_BATBRAINS + 2);
static const uint32_t *const batbrain_tiles = ASSET_BATBRAIN_TILES;

/* Batbrain.c:48-51 (Batbrain_StageLoad): hitboxBadnik, symmetric. */
#define BR_HB_L (-12)
#define BR_HB_T (-18)
#define BR_HB_R   12
#define BR_HB_B   18

/* Batbrain_State_CheckPlayerInRange's own distance gate (Batbrain.c:123,
 * 0x800000 = 128px) and DropToPlayer's own arrival gate (Batbrain.c:155,
 * 0x100000 = 16px) are decomp-exact; BR_HANG_DELAY replaces the RNG roll
 * gated on the first (see batbrain.h's own header comment). DropToPlayer's
 * gravity (Batbrain.c:152, 0x1800/tick) and Fly's own launch speed
 * (Batbrain.c:157-160, 0x10000 = 1px/tick) are decomp-exact; FlyToCeiling's
 * own gravity (Batbrain.c:196, 0x1800/tick, sign reversed) is too.
 * BR_FLY_RANGE (0x800000, Batbrain.c:178) reused as a fixed round-trip
 * distance in place of the RNG-gated Fly->FlyToCeiling roll. */
#define BR_TRIGGER_RANGE 128
#define BR_HANG_DELAY     40
#define BR_DROP_GRAVITY   0x1800
#define BR_ARRIVE_DIST    16
#define BR_FLY_SPEED      1
#define BR_FLY_RANGE     128
#define BR_RETURN_GRAVITY 0x1800

#define BR_STATE_HANG   0
#define BR_STATE_DROP   1
#define BR_STATE_FLY    2
#define BR_STATE_RETURN 3

#define BR_FRAME_HANG  0
#define BR_FRAME_FALL0 1
#define BR_FRAME_FALL_N 2
#define BR_FRAME_FLY0  3
#define BR_FRAME_FLY_N 8
#define BR_FRAME_COUNT (BR_FRAME_FALL_N + BR_FRAME_FLY_N + 1)

static uint8_t  destroyed[(BATBRAIN_COUNT + 7) / 8];
static uint8_t  brState[BATBRAIN_COUNT];
static uint16_t brHangTimer[BATBRAIN_COUNT];
static uint16_t brFlyTimer[BATBRAIN_COUNT];
static uint8_t  brDir[BATBRAIN_COUNT];     /* 0 = +X (right), 1 = -X (left) */
static int16_t  brTargetY[BATBRAIN_COUNT];
static int16_t  brOffX[BATBRAIN_COUNT];
static int32_t  brOffY[BATBRAIN_COUNT];    /* 16.16 */
static int32_t  brVelY[BATBRAIN_COUNT];    /* 16.16 */

static ObjFrame  brFramesRaw[BR_FRAME_COUNT];
static ObjAnimWindow *brWindow;

static ObjDrawDecision batbrain_decide(void *st, uint16_t i, int16_t ex, int16_t ey,
                                       int16_t sonicWorldX, int16_t sonicWorldY,
                                       uint16_t sonicFrameIndex)
{
	ObjDrawDecision d;
	int16_t bx, by;
	(void)st;

	d.flipH = 0; d.flipV = 0; d.frame = OBJ_SKIP;

	bx = (int16_t)(ex + brOffX[i]);
	by = (int16_t)(ey + (int16_t)(brOffY[i] >> 16));

	if (badnik_decide_common(destroyed, i, bx, by, BR_HB_L, BR_HB_T, BR_HB_R, BR_HB_B,
	                         sonicWorldX, sonicWorldY, sonicFrameIndex))
		return d;

	if (!obj_anim_window_live(brWindow)) return d;
	d.frame = 0;
	d.flipH = brDir[i];
	return d;
}

static ObjTypeDesc batbrainType = {
	(const void *)0, sizeof(BatbrainEntry), BATBRAIN_COUNT, ghz_batbrain_count_p,
	(const uint32_t *)0, 0,
	(const ObjFrame *)0, batbrain_pieces,
	OBJ_PRI_BADNIK, BATBRAIN_PAL, 0,
	BR_FLY_RANGE + 32,
	batbrain_decide, (void *)0
};

static ObjAnimWindowDesc batbrainAnimDesc = {
	(const void *)0, sizeof(BatbrainEntry), BATBRAIN_COUNT,
	(int16_t)(ARENA_LOOKAHEAD_X(BATBRAIN_MAX_FRAME_TILES) + BR_FLY_RANGE + 32),
	OBJ_PRI_BADNIK,
	(const uint32_t *)0, brFramesRaw, BR_FRAME_COUNT, BATBRAIN_MAX_FRAME_TILES
};

static uint8_t batbrainInited;

static void batbrain_lazy_init(void)
{
	uint16_t i;

	batbrainInited = 1;
	for (i = 0; i < sizeof(destroyed); i++) destroyed[i] = 0;
	for (i = 0; i < BATBRAIN_COUNT; i++) {
		brState[i] = BR_STATE_HANG;
		brHangTimer[i] = 0; brFlyTimer[i] = 0;
		brDir[i] = 0; brTargetY[i] = 0;
		brOffX[i] = 0; brOffY[i] = 0; brVelY[i] = 0;
	}

	brFramesRaw[BR_FRAME_HANG] = batbrain_hang[0];
	for (i = 0; i < BR_FRAME_FALL_N; i++) brFramesRaw[BR_FRAME_FALL0 + i] = batbrain_fall[i];
	for (i = 0; i < BR_FRAME_FLY_N; i++) brFramesRaw[BR_FRAME_FLY0 + i] = batbrain_fly[i];

	if (*ghz_batbrain_count_p != BATBRAIN_COUNT) return;

	batbrainType.entries = (const void *)ghz_batbrain_xy;
	batbrainAnimDesc.entries = (const void *)ghz_batbrain_xy;
	batbrainAnimDesc.sheetPixels = batbrain_tiles;

	brWindow = obj_anim_window_register(&batbrainAnimDesc);
	if (!brWindow) return;
	batbrainType.frames = obj_anim_window_frames(brWindow);
}

void batbrain_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	uint16_t i;
	uint16_t lastFrame = BR_FRAME_HANG;
	(void)sonicFrameIndex;

	if (!batbrainInited) batbrain_lazy_init();

	for (i = 0; i < BATBRAIN_COUNT; i++) {
		const BatbrainEntry *e = &ghz_batbrain_xy[i];

		if (badnik_is_destroyed(destroyed, i)) continue;

		switch (brState[i]) {
		case BR_STATE_HANG: {
			/* Batbrain.c:123-124: within range AND player at/below self. */
			int32_t dx = sonicWorldX - e->x;
			if (dx < 0) dx = -dx;
			if (dx < BR_TRIGGER_RANGE && sonicWorldY >= e->y) {
				if (++brHangTimer[i] >= BR_HANG_DELAY) {
					brState[i] = BR_STATE_DROP;
					brHangTimer[i] = 0;
					brVelY[i] = 0;
					brOffY[i] = 0;
					brTargetY[i] = sonicWorldY;
					brDir[i] = (uint8_t)(sonicWorldX >= e->x);
				}
			} else {
				brHangTimer[i] = 0;
			}
			lastFrame = BR_FRAME_HANG;
			break;
		}
		case BR_STATE_DROP: {
			int16_t curY;
			brOffY[i] += brVelY[i];
			brVelY[i] += BR_DROP_GRAVITY;
			curY = (int16_t)(e->y + (brOffY[i] >> 16));
			if (brTargetY[i] - curY < BR_ARRIVE_DIST) {
				brVelY[i] = 0;
				brOffX[i] = 0;
				brState[i] = BR_STATE_FLY;
				brFlyTimer[i] = 0;
			}
			lastFrame = (uint16_t)(BR_FRAME_FALL0 + ((brOffY[i] >> 16) & 1));
			break;
		}
		case BR_STATE_FLY: {
			brFlyTimer[i]++;
			brOffX[i] = (int16_t)(brDir[i]
			           ? -(brFlyTimer[i] * BR_FLY_SPEED)
			           :  (brFlyTimer[i] * BR_FLY_SPEED));
			if (brFlyTimer[i] * BR_FLY_SPEED >= BR_FLY_RANGE) {
				brState[i] = BR_STATE_RETURN;
			}
			lastFrame = (uint16_t)(BR_FRAME_FLY0 + (brFlyTimer[i] / 4) % BR_FRAME_FLY_N);
			break;
		}
		default: /* BR_STATE_RETURN */
			brVelY[i] -= BR_RETURN_GRAVITY;
			brOffY[i] += brVelY[i];
			if (brOffY[i] <= 0) {
				brState[i] = BR_STATE_HANG;
				brOffX[i] = 0; brOffY[i] = 0; brVelY[i] = 0;
			}
			lastFrame = (uint16_t)(BR_FRAME_FLY0 + (brFlyTimer[i] / 4) % BR_FRAME_FLY_N);
			break;
		}
	}

	/* One shared request per tick (see motobug.c's own comment on why) --
	 * the LAST instance processed this tick wins the shared window if
	 * several disagree, same "last request wins, self-correcting" rule
	 * every other class in this batch follows for the same reason. */
	obj_anim_window_select(brWindow, lastFrame);
}

uint16_t batbrain_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                       uint16_t camX, uint16_t camY,
                       int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	if (!batbrainInited || !brWindow) return 0;
	return obj_type_draw(&batbrainType, list, firstIndex, firstLink, BATBRAIN_SPRITE_CAP,
	                     camX, camY, sonicWorldX, sonicWorldY, sonicFrameIndex);
}
