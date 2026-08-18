#include "decoration.h"
#include "vdp.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_pool.h"
#include "decoration_data.h"
#include "assets_gen.h"

/* tools/convert_objects.py's kept-decoration count for GHZ1 (Mania filter),
 * same "assert the leading count word matches" convention rings.c's
 * RING_COUNT/springs.c's SPRING_COUNT already use. */
#define DECORATION_COUNT 21

/* This port's own compiler-laid-out copy of assets/ghz/decorations.bin's
 * x/y/direction fields -- see decoration.h's own top comment for why this
 * cannot be a direct struct-cast over the generated blob the way rings.c/
 * springs.c cast theirs (this class's own 23-byte row is odd, so every
 * odd-indexed row's fields would land at a misaligned address). Compiler-
 * padded to 6 bytes (two naturally-aligned int16 fields force 2-byte
 * struct alignment, which rounds the logical 5-byte size up to 6) -- an
 * ordinary static array of this type is safe to index and safe to cast the
 * FIRST two fields of through obj_generic.c's own entry_x()/entry_y()
 * (ObjTypeDesc.entries' documented "every record begins with int16 x,y"
 * convention, obj_data.h). */
typedef struct {
	int16_t x, y;
	uint8_t direction;
} DecorationEntry;

static DecorationEntry decorationEntries[DECORATION_COUNT];

static const uint16_t *const ghz_decoration_count = ASSET_GHZ_DECORATIONS;
static const uint32_t *const decoration_tiles = ASSET_DECORATION_TILES;

/* Stashed once per decoration_draw() call, read by decoration_decide() --
 * ObjDecideFn (obj_data.h) does not carry camY directly, same reason
 * rings.c's own curCamY exists (see that file's comment on the same
 * pattern). */
static uint16_t curCamY;

static ObjFrame decorationFrame;      /* rebased copy of decoration_bridgepost[0] */
static uint16_t decorationRawOffset;  /* decoration_bridgepost[0].tileOffset, stashed
                                        * so decoration_arena_onBase() can rebase on
                                        * every (re)grant, not just the first one --
                                        * same convention springs.c's own
                                        * sp_residentRawOffset[] follows. */
static uint8_t  decorationLive;       /* this frame's VRAM residency (arena onLive) */
static uint8_t  decorationEnabled;    /* boot-time table check + arena registration
                                        * both succeeded -- decorationType.entries is
                                        * only ever valid once this is true. Distinct
                                        * from decorationLive (residency can toggle
                                        * every frame as the camera moves; this does
                                        * not, it is a permanent-for-this-run flag,
                                        * same split rings.c's ringRotWindow!=NULL
                                        * check keeps from rings_enabled()). */

static ObjDrawDecision decoration_decide(void *st, uint16_t entryIndex, int16_t ex, int16_t ey,
                                         int16_t sonicWorldX, int16_t sonicWorldY,
                                         uint16_t sonicFrameIndex)
{
	const DecorationEntry *e = &decorationEntries[entryIndex];
	ObjDrawDecision d;
	int32_t ylo, yhi;
	(void)st; (void)ex; (void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;

	/* direction is 0 (FLIP_NONE) or 1 (FLIP_X) for every one of GHZ1's 21
	 * entries -- see decoration.h's own top comment -- so this port's single
	 * flipH bit already covers every value this table ever holds. */
	d.flipH = e->direction;
	d.flipV = 0;
	d.frame = OBJ_SKIP;

	ylo = (int32_t)curCamY - 16;
	yhi = (int32_t)curCamY + SCREEN_HEIGHT + 16;
	if (ey < ylo || ey >= yhi) return d;

	if (!decorationLive) return d;
	d.frame = 0;   /* decorationType.frames is always this one rebased row */
	return d;
}

/* tilePixels/residentTileCount/countPtr are 0/NULL here on purpose, same
 * reason springType carries the same zeros (springs.c's own comment):
 * Decoration's tile upload goes through the shared VRAM arena below, not
 * obj_type_init(), and its own staleness check already ran once in
 * decoration_init(). entries is patched in there too (a static initializer
 * here would only be reading decorationEntries' ADDRESS, not a constant
 * expression problem, but it is patched alongside frames/tilePixels below
 * for the same one-place-to-look reason springType/ringType already keep
 * all three together). */
static ObjTypeDesc decorationType = {
	(const void *)0, sizeof(DecorationEntry), DECORATION_COUNT, (const uint16_t *)0,
	(const uint32_t *)0, 0,
	&decorationFrame, decoration_pieces,
	OBJ_PRI_SCENERY, DECORATION_PAL, 0 /* low priority -- see decoration.h's own
	                                    * drawGroup comment: GetFrameID is always 0
	                                    * right after SetSpriteAnimation(...,0), so
	                                    * every GHZ1 instance takes the original's
	                                    * own low objectDrawGroup[0] branch */,
	16,                        /* marginX, matches rings/springs/signpost */
	decoration_decide, (void *)0
};

static void decoration_arena_onBase(uint16_t base)
{
	decorationFrame.tileOffset = (uint16_t)(base + decorationRawOffset);
}

static void decoration_arena_onLive(uint8_t live) { decorationLive = live; }

static ArenaClassDesc decorationArenaDesc = {
	(const void *)0, sizeof(DecorationEntry), DECORATION_COUNT,
	(const uint32_t *)0, DECORATION_MAX_FRAME_TILES,
	(int16_t)ARENA_LOOKAHEAD_X(DECORATION_MAX_FRAME_TILES), OBJ_PRI_SCENERY,
	decoration_arena_onBase, decoration_arena_onLive
};

void decoration_init(void)
{
	const uint8_t *base;
	uint16_t i;
	uint8_t slot;

	decorationLive = 0;
	decorationEnabled = 0;

	/* Byte-at-a-time extraction of x,y,direction out of the raw 23-byte-
	 * stride generated table -- see decoration.h's own top comment and this
	 * file's DecorationEntry comment for why a typed cast/array-index over
	 * the raw blob is unsafe here specifically. base points at the first
	 * row (past the leading big-endian u16 count); every read below is a
	 * single uint8_t dereference, never misaligned regardless of the
	 * resulting byte offset's parity. row layout (DECORATION_SCENE's
	 * row_fmt ">hhBBbiiii"): x(2) y(2) type(1, unused -- always 0) direction
	 * (1) rotSpeed(1, unused) repeatTimes_x/y(4/4, unused)
	 * repeatSpacing_x/y(4/4, unused). */
	base = (const uint8_t *)ASSET_GHZ_DECORATIONS + 2;
	for (i = 0; i < DECORATION_COUNT; i++) {
		const uint8_t *rec = base + (uint32_t)i * 23;
		decorationEntries[i].x = (int16_t)(((uint16_t)rec[0] << 8) | rec[1]);
		decorationEntries[i].y = (int16_t)(((uint16_t)rec[2] << 8) | rec[3]);
		decorationEntries[i].direction = rec[5];
	}

	decorationRawOffset = decoration_bridgepost[0].tileOffset;   /* 0 */
	decorationFrame.tileOffset = 0;   /* real value set by decoration_arena_onBase */
	decorationFrame.pieceOffset = decoration_bridgepost[0].pieceOffset;
	decorationFrame.tileCount = decoration_bridgepost[0].tileCount;
	decorationFrame.pieceCount = decoration_bridgepost[0].pieceCount;
	decorationFrame.pivotX = decoration_bridgepost[0].pivotX;
	decorationFrame.pivotY = decoration_bridgepost[0].pivotY;
	decorationFrame.duration = decoration_bridgepost[0].duration;

	/* Same "cannot go stale in one file and not another" guard rings_init()/
	 * springs_init() each run before touching VRAM: decorations.bin's own
	 * leading count word has to still match the compile-time DECORATION_COUNT
	 * every loop above assumes. */
	if (*ghz_decoration_count != DECORATION_COUNT) return;

	decorationType.entries = (const void *)decorationEntries;

	decorationArenaDesc.entries = (const void *)decorationEntries;
	decorationArenaDesc.tilePixels = decoration_tiles;
	slot = obj_arena_register(&decorationArenaDesc);
	if (slot == ARENA_INVALID_SLOT) return;
	/* No boot_load: GHZ1's first Decoration (x=528) is outside the very
	 * first camera view around spawn (x=108, +-160px half-screen) -- see
	 * decoration.h's own comment -- so the ordinary runtime admission path
	 * (obj_arena_tick(), called every frame in main.c's loop) has margin to
	 * load this class in well before the camera arrives. */

	decorationEnabled = 1;
}

uint16_t decoration_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                         uint16_t camX, uint16_t camY,
                         int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	/* Permanently-disabled-for-this-run check (boot failure) -- entries is
	 * only ever a valid pointer once decoration_init() reaches
	 * `decorationEnabled = 1`; without this guard obj_type_draw() would
	 * binary-search through a NULL entries pointer on a failed boot. */
	if (!decorationEnabled) return 0;

	curCamY = camY;
	return obj_type_draw(&decorationType, list, firstIndex, firstLink, DECORATION_SPRITE_CAP,
	                     camX, camY, sonicWorldX, sonicWorldY, sonicFrameIndex);
}
