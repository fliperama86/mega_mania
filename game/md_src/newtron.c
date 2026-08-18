#include "newtron.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_pool.h"
#include "newtron_data.h"
#include "badnik_base.h"
#include "assets_gen.h"

#define NEWTRON_COUNT 21
#define NEWTRON_SHOOT_TYPE 0   /* Newtron.h's NewtronTypes: SHOOT=0, FLY=1, PROJECTILE=2 (unused here) */
#define NEWTRON_FLY_TYPE   1

/* NEWTRON_SCENE, tools/convert_objects.py: row_fmt ">hhBB", fields
 * (x_px, y_px, type, direction) -- Newtron_Serialize (Newtron.c:339-343). */
typedef struct { int16_t x, y; uint8_t type, direction; } NewtronEntry;

static const uint16_t *const ghz_newtron_count_p = ASSET_GHZ_NEWTRONS;
static const NewtronEntry *const ghz_newtron_xy =
	(const NewtronEntry *)((const uint8_t *)ASSET_GHZ_NEWTRONS + 2);
static const uint32_t *const newtron_tiles = ASSET_NEWTRON_TILES;

/* Newtron.c:97-100 (Newtron_StageLoad): hitboxRange, the per-instance
 * proximity trigger both variants share (Newtron_State_CheckPlayerInRange,
 * Newtron.c:172-183). Newtron.c:79-82: hitboxShoot, the badnik hitbox BOTH
 * variants use too (Newtron_CheckPlayerCollisions, Newtron.c:118-127 --
 * "hitboxFly... goes unused in this object", that file's own comment). */
/* +-128 (Newtron.c:97-100) does not fit int8_t (max 127) -- badnik_touches_
 * sonic()'s hitbox parameters are int8_t throughout (every other class's
 * hitbox in this batch fits comfortably), so the +128 edge is narrowed by
 * 1px rather than widening one function's signature for a single class's
 * one field. Immaterial: 1px off a 256px-wide trigger box. */
#define NT_TRIG_L (-127)
#define NT_TRIG_T  (-64)
#define NT_TRIG_R   127
#define NT_TRIG_B    64
#define NT_HB_L  (-12)
#define NT_HB_T  (-14)
#define NT_HB_R    12
#define NT_HB_B    14

/* Newtron_State_Shoot's own tick thresholds (Newtron.c:250-274): fire at
 * 30 (projectile cut, see newtron.h), revert to idle pose at 45, deactivate
 * at 90 -- all decomp-exact. NT_FLY_TICKS/NT_FLY_SPEED: velocity 0x20000
 * (2px/tick) is decomp-exact (Newtron.c:222-224); the 128-tick duration is
 * this port's own choice (the real Fly state has no timer at all, it flies
 * until CheckOffScreen respawns it off-camera -- see newtron.h's own
 * comment on why floor-following flight could not be ported as-is). */
#define NT_SHOOT_FIRE  30
#define NT_SHOOT_IDLE  45
#define NT_SHOOT_END   90
#define NT_FLY_SPEED    2
#define NT_FLY_TICKS  128

#define NT_STATE_DORMANT 0
#define NT_STATE_ACTIVE  1

#define NT_FRAME_SHOOTIDLE 0
#define NT_FRAME_SHOOT0    1
#define NT_FRAME_SHOOT_N   5
#define NT_FRAME_FLY       6
#define NT_FRAME_COUNT     7

static uint8_t  destroyed[(NEWTRON_COUNT + 7) / 8];
static uint8_t  ntState[NEWTRON_COUNT];
static uint16_t ntTimer[NEWTRON_COUNT];
static uint8_t  ntDir[NEWTRON_COUNT];    /* 0 = +X (right), 1 = -X (left) */
static int16_t  ntOffX[NEWTRON_COUNT];

static ObjFrame  ntFramesRaw[NT_FRAME_COUNT];
static ObjAnimWindow *ntWindow;

static ObjDrawDecision newtron_decide(void *st, uint16_t i, int16_t ex, int16_t ey,
                                      int16_t sonicWorldX, int16_t sonicWorldY,
                                      uint16_t sonicFrameIndex)
{
	ObjDrawDecision d;
	int16_t bx;
	(void)st;

	d.flipH = 0; d.flipV = 0; d.frame = OBJ_SKIP;
	if (ntState[i] != NT_STATE_ACTIVE) return d;

	bx = (int16_t)(ex + ntOffX[i]);

	if (badnik_decide_common(destroyed, i, bx, ey, NT_HB_L, NT_HB_T, NT_HB_R, NT_HB_B,
	                         sonicWorldX, sonicWorldY, sonicFrameIndex))
		return d;

	if (!obj_anim_window_live(ntWindow)) return d;
	d.frame = 0;
	d.flipH = ntDir[i];
	return d;
}

static ObjTypeDesc newtronType = {
	(const void *)0, sizeof(NewtronEntry), NEWTRON_COUNT, ghz_newtron_count_p,
	(const uint32_t *)0, 0,
	(const ObjFrame *)0, newtron_pieces,
	OBJ_PRI_BADNIK, NEWTRON_PAL, 0,
	NT_FLY_SPEED * NT_FLY_TICKS + 32,
	newtron_decide, (void *)0
};

static ObjAnimWindowDesc newtronAnimDesc = {
	(const void *)0, sizeof(NewtronEntry), NEWTRON_COUNT,
	(int16_t)(ARENA_LOOKAHEAD_X(NEWTRON_MAX_FRAME_TILES) + NT_FLY_SPEED * NT_FLY_TICKS + 32),
	OBJ_PRI_BADNIK,
	(const uint32_t *)0, ntFramesRaw, NT_FRAME_COUNT, NEWTRON_MAX_FRAME_TILES
};

static uint8_t newtronInited;

static void newtron_lazy_init(void)
{
	uint16_t i;

	newtronInited = 1;
	for (i = 0; i < sizeof(destroyed); i++) destroyed[i] = 0;
	for (i = 0; i < NEWTRON_COUNT; i++) { ntState[i] = NT_STATE_DORMANT; ntTimer[i] = 0; ntDir[i] = 0; ntOffX[i] = 0; }

	ntFramesRaw[NT_FRAME_SHOOTIDLE] = newtron_shootidle[0];
	for (i = 0; i < NT_FRAME_SHOOT_N; i++) ntFramesRaw[NT_FRAME_SHOOT0 + i] = newtron_shoot[i];
	ntFramesRaw[NT_FRAME_FLY] = newtron_fly[0];

	if (*ghz_newtron_count_p != NEWTRON_COUNT) return;

	newtronType.entries = (const void *)ghz_newtron_xy;
	newtronAnimDesc.entries = (const void *)ghz_newtron_xy;
	newtronAnimDesc.sheetPixels = newtron_tiles;

	ntWindow = obj_anim_window_register(&newtronAnimDesc);
	if (!ntWindow) return;
	newtronType.frames = obj_anim_window_frames(ntWindow);
}

void newtron_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	int8_t sl, st, sr, sb;
	uint16_t i;

	if (!newtronInited) newtron_lazy_init();
	badnik_sonic_hitbox(sonicFrameIndex, &sl, &st, &sr, &sb);

	for (i = 0; i < NEWTRON_COUNT; i++) {
		const NewtronEntry *e = &ghz_newtron_xy[i];

		if (badnik_is_destroyed(destroyed, i)) continue;

		if (ntState[i] == NT_STATE_DORMANT) {
			if (badnik_touches_sonic(e->x, e->y, NT_TRIG_L, NT_TRIG_T, NT_TRIG_R, NT_TRIG_B,
			                         sonicWorldX, sonicWorldY, sl, st, sr, sb)) {
				ntState[i] = NT_STATE_ACTIVE;
				ntTimer[i] = 0;
				ntOffX[i] = 0;
				ntDir[i] = (e->type == NEWTRON_FLY_TYPE)
				           ? (uint8_t)(sonicWorldX < e->x)
				           : (uint8_t)(e->direction & 1);
			}
			continue;
		}

		{
			uint16_t t = ntTimer[i];
			uint16_t limit = (e->type == NEWTRON_FLY_TYPE) ? NT_FLY_TICKS : NT_SHOOT_END;
			uint16_t frame;

			if (t >= limit) { ntState[i] = NT_STATE_DORMANT; continue; }
			ntTimer[i] = (uint16_t)(t + 1);

			if (e->type == NEWTRON_FLY_TYPE) {
				int32_t d = (int32_t)t * NT_FLY_SPEED;
				ntOffX[i] = (int16_t)(ntDir[i] ? -d : d);
				frame = NT_FRAME_FLY;
			} else if (t < NT_SHOOT_FIRE) {
				frame = NT_FRAME_SHOOTIDLE;
			} else if (t < NT_SHOOT_IDLE) {
				frame = (uint16_t)(NT_FRAME_SHOOT0
				                   + (t - NT_SHOOT_FIRE) * NT_FRAME_SHOOT_N / (NT_SHOOT_IDLE - NT_SHOOT_FIRE));
			} else {
				frame = NT_FRAME_SHOOTIDLE;
			}
			obj_anim_window_select(ntWindow, frame);
		}
	}
}

uint16_t newtron_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                      uint16_t camX, uint16_t camY,
                      int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	if (!newtronInited || !ntWindow) return 0;
	return obj_type_draw(&newtronType, list, firstIndex, firstLink, NEWTRON_SPRITE_CAP,
	                     camX, camY, sonicWorldX, sonicWorldY, sonicFrameIndex);
}
