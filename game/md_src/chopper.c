#include "chopper.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_pool.h"
#include "chopper_data.h"
#include "badnik_base.h"
#include "assets_gen.h"

#define CHOPPER_COUNT 13
#define CHOPPER_JUMP_TYPE 0   /* Chopper.h's ChopperTypes: CHOPPER_JUMP=0, CHOPPER_SWIM=1 */

/* CHOPPER_SCENE, tools/convert_objects.py: row_fmt ">hhBBBx" (8 bytes on
 * disk -- a trailing pad byte after `charge`, no field of its own), fields
 * (x_px, y_px, type, direction, charge) -- Chopper_Serialize (Chopper.c:
 * 325-330). direction/charge only matter to the Swim variant, out of
 * scope here (see chopper.h's own header comment). The pad keeps every row
 * EVEN-sized, matching sizeof(ChopperEntry) here exactly: SceneRecipe
 * (tools/convert_objects.py) asserts every struct-cast-read table's row_fmt
 * packs to an EVEN size, so this struct and recordSize-strided readers like
 * obj_generic.c's entry_x()/entry_y() below always agree with the table's
 * real on-disk stride, row 0 onward. */
typedef struct { int16_t x, y; uint8_t type, direction, charge; } ChopperEntry;

static const uint16_t *const ghz_chopper_count_p = ASSET_GHZ_CHOPPERS;
static const ChopperEntry *const ghz_chopper_xy =
	(const ChopperEntry *)((const uint8_t *)ASSET_GHZ_CHOPPERS + 2);
static const uint32_t *const chopper_tiles = ASSET_CHOPPER_TILES;

/* Chopper.c:47-50 (Chopper_StageLoad): hitboxJump, asymmetric in X (-10/6)
 * -- transcribed as-is; the Jump variant never sets self->direction
 * meaningfully (Chopper_State_Init only branches on self->type), so no
 * flip-mirroring of this box is needed. */
#define CH_HB_L (-10)
#define CH_HB_T (-20)
#define CH_HB_R    6
#define CH_HB_B   20

/* Chopper_State_Jump (Chopper.c:141-170): velocity.y starts at 0 (never
 * set for the Jump path in Chopper_Create/State_Init), gravity +0x1800/
 * tick, and the instant position.y crosses back past startPos.y it snaps
 * to startPos.y and re-launches at velocity.y=-0x70000. One SHARED
 * simulation serves every Jump chopper (all identical physics, so sharing
 * one phase costs nothing beyond what the single VRAM window already
 * forces -- see motobug.c's own comment on class-wide lockstep), advanced
 * once per tick in chopper_tick() rather than replayed from a modulus
 * (the bounce period is not a round tick count). All values 16.16 fixed,
 * matching the decomp's own scale. */
#define CH_GRAVITY   0x1800
#define CH_LAUNCH_VY (-0x70000)

static uint8_t  destroyed[(CHOPPER_COUNT + 7) / 8];
static int32_t  chopperOffY;   /* 16.16, <= 0: how far above startPos.y right now */
static int32_t  chopperVelY;   /* 16.16 */
static uint16_t curWinFrame;

static ObjFrame  chFramesRaw[8];
static ObjAnimWindow *chWindow;

static ObjDrawDecision chopper_decide(void *st, uint16_t i, int16_t ex, int16_t ey,
                                      int16_t sonicWorldX, int16_t sonicWorldY,
                                      uint16_t sonicFrameIndex)
{
	ObjDrawDecision d;
	const ChopperEntry *e = &ghz_chopper_xy[i];
	int16_t by;
	(void)st;

	d.flipH = 0; d.flipV = 0; d.frame = OBJ_SKIP;
	if (e->type != CHOPPER_JUMP_TYPE) return d;   /* Swim: out of scope, never drawn */

	by = (int16_t)(ey + (chopperOffY >> 16));

	if (badnik_decide_common(destroyed, i, ex, by, CH_HB_L, CH_HB_T, CH_HB_R, CH_HB_B,
	                         sonicWorldX, sonicWorldY, sonicFrameIndex))
		return d;

	if (!obj_anim_window_live(chWindow)) return d;
	d.frame = 0;
	return d;
}

static ObjTypeDesc chopperType = {
	(const void *)0, sizeof(ChopperEntry), CHOPPER_COUNT, ghz_chopper_count_p,
	(const uint32_t *)0, 0,
	(const ObjFrame *)0, chopper_pieces,
	OBJ_PRI_BADNIK, CHOPPER_PAL, 0,
	32,
	chopper_decide, (void *)0
};

static ObjAnimWindowDesc chopperAnimDesc = {
	(const void *)0, sizeof(ChopperEntry), CHOPPER_COUNT,
	(int16_t)(ARENA_LOOKAHEAD_X(CHOPPER_MAX_FRAME_TILES) + 32),
	OBJ_PRI_BADNIK,
	(const uint32_t *)0, chFramesRaw, 8, CHOPPER_MAX_FRAME_TILES
};

static uint8_t chopperInited;

static void chopper_lazy_init(void)
{
	uint16_t i;

	chopperInited = 1;
	chopperOffY = 0;
	chopperVelY = CH_LAUNCH_VY;
	curWinFrame = 0;
	for (i = 0; i < sizeof(destroyed); i++) destroyed[i] = 0;

	for (i = 0; i < 8; i++) chFramesRaw[i] = chopper_jump[i];

	if (*ghz_chopper_count_p != CHOPPER_COUNT) return;

	chopperType.entries = (const void *)ghz_chopper_xy;
	chopperAnimDesc.entries = (const void *)ghz_chopper_xy;
	chopperAnimDesc.sheetPixels = chopper_tiles;

	chWindow = obj_anim_window_register(&chopperAnimDesc);
	if (!chWindow) return;
	chopperType.frames = obj_anim_window_frames(chWindow);
}

void chopper_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	int16_t v16;
	uint16_t frame;
	(void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;
	if (!chopperInited) chopper_lazy_init();

	chopperOffY += chopperVelY;
	chopperVelY += CH_GRAVITY;
	if (chopperOffY > 0) { chopperOffY = 0; chopperVelY = CH_LAUNCH_VY; }

	/* Not RSDK.ProcessAnimation's real speed-threshold logic
	 * (Chopper.c:148-159) -- an approximate, monotonic velocity-to-frame
	 * mapping across the 8 jump frames, close enough to read as "rises
	 * through the arc" without replaying the animator timer exactly. */
	v16 = (int16_t)(chopperVelY >> 16);
	frame = (uint16_t)(v16 + 7);
	if (frame > 7) frame = 7;
	curWinFrame = frame;
	obj_anim_window_select(chWindow, curWinFrame);
}

uint16_t chopper_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                      uint16_t camX, uint16_t camY,
                      int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	if (!chopperInited || !chWindow) return 0;
	return obj_type_draw(&chopperType, list, firstIndex, firstLink, CHOPPER_SPRITE_CAP,
	                     camX, camY, sonicWorldX, sonicWorldY, sonicFrameIndex);
}
