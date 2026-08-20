#include "bridge.h"
#include "bridge_data.h"
#include "platform_trig.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_pool.h"
#include "obj_sprite.h"
#include "vdp.h"
#include "sonic_data.h"
#include "assets_gen.h"

/* GHZ Scene1.bin's Bridge entities, same naturally-aligned {int16 x, int16
 * y, uint8 length, uint8 burnable} pointer-cast convention as md_src/
 * springs.c's own SpringEntry (both 6-byte, no compiler padding). */
typedef struct { int16_t x, y; uint8_t length, burnable; } BridgeDef;
static const uint16_t *const ghz_bridges_count_p = ASSET_GHZ_BRIDGES;
static const BridgeDef *const k_bridges_md =
	(const BridgeDef *)((const uint8_t *)ASSET_GHZ_BRIDGES + 2);
static const uint32_t *const bridge_tiles_md = ASSET_BRIDGE_TILES;
static const int8_t *const sonic_hitbox = ASSET_SONIC_HITBOX;

#define BRIDGE_COUNT 13
#define TO_FIXED(x) ((int32_t)(x) << 16)

#define BSTOOD_NONE   0
#define BSTOOD_PLAYER 1
#define BSTOOD_LEFT   2

/* Replicated Bridge_Update+HandleCollisions sag state (Bridge.c:12-44,
 * 143-279) -- see this file's own top comment for why this side needs its
 * own full copy of sh_src/bridge.c's state machine, not just its own draw
 * math: bridgeDepth is a genuinely STATEFUL integration (timer ramps
 * +-8/tick, depression only updates while touched), not a pure function of
 * any shared clock the way Platform's Linear/Swing are, so there is no
 * shortcut around re-deriving it here.
 *
 * REDUCTION FROM sh_src/bridge.c's OWN (already-reduced, single-player)
 * version: this side has no p->e.velY/onGround/outer.bottom (never
 * published -- sh_src/comm.h's own register map carries world position and
 * animation frame only) so it approximates velY's SIGN from this-tick-minus
 * -last-tick worldY (velYApprox below) and always uses Sonic's current-frame
 * hitbox bottom (ASSET_SONIC_HITBOX, same table rings.c/springs.c already
 * read) in place of Player_GetHitbox(). This is a second, DELIBERATE
 * reduction on top of sh_src/bridge.c's own single-player specialisation --
 * flagged in this batch's own report, not silently absorbed -- but the
 * drawn plank positions still come from the EXACT SAME bridgeDepth/stoodPos
 * formulas (Bridge_Draw, Bridge.c:50-81) applied to this side's own
 * replicated depression/timer, so a bridge sags visibly in step with the
 * real one even when the exact tick a landing is recognised drifts by one
 * or two frames from the SH2's own real collision test. */
typedef struct {
	int32_t timer;
	int32_t depression;
	int32_t stoodPos;
	uint8_t stoodState;
} BridgeState;

static BridgeState bstate[BRIDGE_COUNT];
static uint16_t logTileBase;
static uint8_t  bridgeLive;
static int16_t  lastWorldY;
static uint8_t  haveLastWorldY;

static void bridge_onBase(uint16_t base) { logTileBase = base; }
static void bridge_onLive(uint8_t live) { bridgeLive = live; }

static ArenaClassDesc bridgeArenaDesc = {
	(const void *)0, sizeof(BridgeDef), BRIDGE_COUNT,
	(const uint32_t *)0, BRIDGE_MAX_FRAME_TILES,
	0, OBJ_PRI_PLATFORM,
	bridge_onBase, bridge_onLive
};

/* Window-only descriptor, same reason md_src/platform.c's own
 * platformWinDesc exists: obj_type_draw()'s decide()-returns-a-frame-index
 * shape cannot express "N sprites at N independently computed positions"
 * (Bridge_Draw expands one entity into length+1 planks), so this file walks
 * obj_type_window()'s own x-sorted binary search directly and emits every
 * plank itself. marginX is a plain 32px (not Platform's own wide 280px):
 * a bridge's own X FOOTPRINT never moves (only its sag, vertically), so its
 * scene-authored span is always exactly where it will ever be drawn. */
static ObjTypeDesc bridgeWinDesc = {
	(const void *)0, sizeof(BridgeDef), BRIDGE_COUNT, (const uint16_t *)0,
	(const uint32_t *)0, 0,
	(const ObjFrame *)0, (const ObjPiece *)0,
	OBJ_PRI_PLATFORM, BRIDGE_PAL, 0,
	32,
	(ObjDecideFn)0, (void *)0,
	(const ObjPieceTemplate *)0, (const ObjPieceTemplate *)0   /* window-only desc, never drawn through obj_type_draw() */
};

__attribute__((noinline))
void bridge_init(void)
{
	uint16_t i;
	uint8_t slot;
	uint16_t base;

	bridgeLive = 0;
	haveLastWorldY = 0;
	for (i = 0; i < BRIDGE_COUNT; i++) {
		bstate[i].timer = 0; bstate[i].depression = 0;
		bstate[i].stoodPos = 0; bstate[i].stoodState = BSTOOD_NONE;
	}

	if (*ghz_bridges_count_p != BRIDGE_COUNT) return;

	bridgeWinDesc.entries = (const void *)k_bridges_md;
	bridgeArenaDesc.entries = (const void *)k_bridges_md;
	bridgeArenaDesc.tilePixels = bridge_tiles_md;
	slot = obj_arena_register(&bridgeArenaDesc);
	base = obj_arena_boot_load(slot);
	if (base == 0xFFFF) return;
	vdp_tiles_load(bridge_tiles_md, base, BRIDGE_MAX_FRAME_TILES);
	obj_arena_boot_done(slot);
}

__attribute__((noinline))
void bridge_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	uint16_t n, i;
	int8_t hbBottom;
	int32_t velYApprox;
	int32_t sx, sy;

	if (!bridgeLive) return;

	/* sh_src/bridge.c's own matching guard (see that file's own top comment
	 * on bridge_apply() for the full derivation) -- this side has no direct
	 * PSTATE_DEATH to read (never published, see this file's own top
	 * comment on why this whole side only ever approximates the real
	 * SH2-side player), but state_death() (sh_src/player.c) plays ANI_DIE
	 * and nothing else for its entire death arc (sonic_set_anim(&p->
	 * animator, ANI_DIE, 0, 0), every tick, until player_init() resets
	 * everything on respawn), so sonicFrameIndex landing inside
	 * sonic_anims[ANI_DIE]'s own [first, first+count) range is exactly as
	 * reliable a "the real player is currently dying" signal as this file's
	 * hbBottom lookup already trusts sonicFrameIndex for -- the same
	 * membership test md_src/rings.c (ANI_HURT), md_src/breakablewall.c and
	 * md_src/itembox.c (both ANI_JUMP) already use against this identical
	 * parameter. Skipping here keeps this shadow copy's own bstate[] in
	 * step with the real bridge_apply() now skipping too: without this, a
	 * player who dies while "already stood" on a bridge would keep this
	 * side's own copy locked at BSTOOD_PLAYER with a frozen sag forever
	 * (this file's own "already stood" branch re-triggers off sy > posY -
	 * ..., true whenever the death arc's own worldY dips back toward the
	 * bridge, which real bridge_apply() no longer lets happen either now
	 * that it also skips), rather than decaying back to flat the way an
	 * ordinary walk-off/fall-off does. */
	if (sonicFrameIndex >= sonic_anims[ANI_DIE].first
	    && sonicFrameIndex < (uint16_t)(sonic_anims[ANI_DIE].first + sonic_anims[ANI_DIE].count))
		return;

	hbBottom = (sonicFrameIndex < SONIC_FRAME_COUNT) ? sonic_hitbox[sonicFrameIndex * 4 + 3] : 20;
	velYApprox = haveLastWorldY ? (int32_t)sonicWorldY - (int32_t)lastWorldY : 0;
	lastWorldY = sonicWorldY;
	haveLastWorldY = 1;

	sx = TO_FIXED(sonicWorldX);
	sy = TO_FIXED(sonicWorldY);

	n = *ghz_bridges_count_p;
	if (n > BRIDGE_COUNT) n = BRIDGE_COUNT;

	for (i = 0; i < n; i++) {
		const BridgeDef *def = &k_bridges_md[i];
		BridgeState *s = &bstate[i];
		int32_t posX = TO_FIXED(def->x), posY = TO_FIXED(def->y);
		int32_t planks = (int32_t)def->length + 1;
		int32_t half = planks << 19;
		int32_t startPos = posX - half, endPos = posX + half;
		int32_t bridgeDepth;

		if (s->stoodState == BSTOOD_PLAYER) {
			if (s->timer < 0x80) s->timer += 8;
		} else if (s->timer) {
			s->stoodState = BSTOOD_LEFT;
			s->timer -= 8;
		} else {
			s->depression = 0;
		}
		bridgeDepth = (s->depression * s->timer) >> 7;

		if (sx <= startPos || sx >= endPos) {
			if (s->stoodState == BSTOOD_PLAYER) { s->timer = 32; s->stoodState = BSTOOD_LEFT; }
			continue;
		}

		if (s->stoodState != BSTOOD_PLAYER) {
			if (velYApprox >= 0) {
				int32_t offsetFromStart = sx - startPos;
				int32_t sinArg = (offsetFromStart << 7) / offsetFromStart;   /* == 1<<7 -- mirrors sh_src/bridge.c's own always-true "just claimed" branch */
				int32_t hitY = (bridgeDepth * platform_sin512(sinArg) >> 9) - 0x80000;
				/* BUG FIX (2026-08-18, this task): same missing "+posY" this
				 * side's own sh_src/bridge.c twin had -- see that file's own
				 * matching comment for the full derivation. hitY is a delta
				 * from the bridge's own position (posY), not an absolute
				 * world Y; without this the landing band sat at world Y
				 * roughly [-8,0] regardless of where any GHZ1 bridge actually
				 * sits (y=200-1960), so this shadow copy's own stoodState
				 * never left BSTOOD_NONE either -- bridgeDepth stayed 0
				 * forever and the sag animation never played, independently
				 * of the real SH2-side fall-through this mirrors. */
				int32_t hitYAbs = hitY + posY;
				int32_t bandTop = hitYAbs >> 16, bandBottom = bandTop + 8;
				int32_t playerBottom = sonicWorldY + hbBottom;

				if (playerBottom >= bandTop && playerBottom <= bandBottom) {
					s->stoodPos = offsetFromStart;
					s->stoodState = BSTOOD_PLAYER;
					s->timer = 0x80;
				}
			}
		} else {
			int32_t distance = endPos - startPos;
			s->stoodPos = sx - startPos;
			s->depression = platform_sin512((s->stoodPos >> 8) / (distance >> 16)) * (distance >> 13);
			if (sy > posY - 0x300000) {
				if (velYApprox < 0) s->stoodState = BSTOOD_LEFT;
			}
		}
	}
}

__attribute__((noinline))
uint16_t bridge_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                     uint16_t camX, uint16_t camY,
                     int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
	uint16_t lo, hi, i, n = 0;
	(void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;

	if (!bridgeLive) return 0;

	obj_type_window(&bridgeWinDesc, camX, &lo, &hi);

	for (i = lo; i < hi && n < BRIDGE_SPRITE_CAP; i++) {
		const BridgeDef *def = &k_bridges_md[i];
		BridgeState *s = &bstate[i];
		int32_t posX = TO_FIXED(def->x), posY = TO_FIXED(def->y);
		int32_t planks = (int32_t)def->length + 1;
		int32_t half = planks << 19;
		int32_t startPos = posX - half, endPos = posX + half;
		int32_t bridgeDepth = (s->depression * s->timer) >> 7;
		int32_t stoodPosPlanks = (s->stoodState == BSTOOD_NONE) ? 0 : (s->stoodPos >> 20);
		int32_t id, ang, drawX, drawY, divisor;

		/* Bridge_Draw, Bridge.c:50-81, transcribed exactly against this
		 * file's own replicated bridgeDepth/stoodPos. */
		ang = 0x80000;
		drawX = startPos + 0x80000;
		for (id = 0; id < stoodPosPlanks && n < BRIDGE_SPRITE_CAP; id++) {
			int32_t sine = platform_sin512((ang << 7) / s->stoodPos);
			drawY = ((bridgeDepth * sine) >> 9) + posY;
			n = (uint16_t)(n + obj_emit_pieces(list, (uint16_t)(firstIndex + n), (uint16_t)(firstLink + n),
			               (uint16_t)(BRIDGE_SPRITE_CAP - n), &bridge_pieces[bridge_log[0].pieceOffset],
			               bridge_log[0].pieceCount, logTileBase, BRIDGE_PAL,
			               (int16_t)((drawX >> 16) - (int16_t)camX), (int16_t)((drawY >> 16) - (int16_t)camY),
			               bridge_log[0].pivotX, bridge_log[0].pivotY, 0, 0, 0));
			drawX += 0x100000;
			ang += 0x100000;
		}
		if (n < BRIDGE_SPRITE_CAP) {
			drawY = bridgeDepth + posY;
			n = (uint16_t)(n + obj_emit_pieces(list, (uint16_t)(firstIndex + n), (uint16_t)(firstLink + n),
			               (uint16_t)(BRIDGE_SPRITE_CAP - n), &bridge_pieces[bridge_log[0].pieceOffset],
			               bridge_log[0].pieceCount, logTileBase, BRIDGE_PAL,
			               (int16_t)((drawX >> 16) - (int16_t)camX), (int16_t)((drawY >> 16) - (int16_t)camY),
			               bridge_log[0].pivotX, bridge_log[0].pivotY, 0, 0, 0));
			drawX += 0x100000;
		}
		id++;

		ang = 0x80000;
		divisor = endPos - startPos - s->stoodPos;
		drawX = endPos - 0x80000;
		for (; id < planks && n < BRIDGE_SPRITE_CAP; id++) {
			int32_t sine = platform_sin512((ang << 7) / divisor);
			drawY = ((bridgeDepth * sine) >> 9) + posY;
			n = (uint16_t)(n + obj_emit_pieces(list, (uint16_t)(firstIndex + n), (uint16_t)(firstLink + n),
			               (uint16_t)(BRIDGE_SPRITE_CAP - n), &bridge_pieces[bridge_log[0].pieceOffset],
			               bridge_log[0].pieceCount, logTileBase, BRIDGE_PAL,
			               (int16_t)((drawX >> 16) - (int16_t)camX), (int16_t)((drawY >> 16) - (int16_t)camY),
			               bridge_log[0].pivotX, bridge_log[0].pivotY, 0, 0, 0));
			drawX -= 0x100000;
			ang += 0x100000;
		}
	}
	return n;
}
