#include <stdint.h>
#include "platform.h"
#include "platform_trig.h"
#include "path.h"

#define TO_FIXED(x) ((int32_t)(x) << 16)

/* GHZ Scene1.bin's Platform entities, Mania filter, x-sorted -- same
 * generated-table convention every other sh_src consumer of a converted
 * scene table uses (sh_src/spring.c's own k_springs is the model this
 * follows). ghz_platforms_sh comes from tools/gen_assets.py's manifest
 * (_ghz_platforms_sh, sh_src/assets_gen.s), the SH2-side link name for the
 * exact same bytes md_src/platform.c reads through ASSET_GHZ_PLATFORMS.
 *
 * Platform's own 30-byte record (tools/convert_objects.py's PLATFORM_SCENE,
 * row_fmt ">hhBiibBbBiiBi") is NOT naturally aligned the way SpringEntry's
 * 6-byte {int16,int16,uint8,uint8} is: amplitude/tileOrigin/angle's int32
 * fields sit at odd byte offsets (5, 9, 17, 21, 26). SH2 hardware faults on
 * a misaligned multi-byte load, so this file reads every multi-byte field
 * BYTE BY BYTE (platform_read() below) rather than casting a struct pointer
 * over the raw bytes the way SpringEntry does -- the same reason tools/
 * convert_objects.py's own SceneRecipe comment gives for why this shape
 * needed a byte-level Python reader too, just mirrored in C. */
extern const uint16_t ghz_platforms_sh[];
static const uint8_t *const k_platform_bytes = (const uint8_t *)ghz_platforms_sh + 2;

/* tools/convert_objects.py's kept Platform count for GHZ1 (Mania filter) --
 * compile-time cap for this file's own per-instance state arrays (Fall's
 * trigger tick, Push's offset), same "cannot go stale in one file and not
 * another" concern rings.c's RING_COUNT documents, checked in platform_apply()
 * below against ghz_platforms_sh[0] every call (cheap, and avoids a separate
 * init() entry point this file has never needed one of). */
#define PLATFORM_COUNT 60

uint32_t g_platform_tick = 0;

/* PlatformTypes or PlatformCollisionTypes, Platform.h:6-43 -- only the values
 * Act 1's own data actually uses (verified by tools/convert_objects.py's own
 * _validate_platform() against every kept entity). */
#define PLATFORM_FIXED  0
#define PLATFORM_FALL   1
#define PLATFORM_LINEAR 2
#define PLATFORM_SWING  4
#define PLATFORM_PUSH   6

#define PLATFORM_C_PLATFORM 0
#define PLATFORM_C_SOLID    1
#define PLATFORM_C_NONE     4

typedef struct {
	int16_t x, y;
	uint8_t type;
	int32_t amplitudeX, amplitudeY;
	int8_t  speed;
	uint8_t hasTension;
	int8_t  frameID;
	uint8_t collision;
	uint8_t childCount;
	int32_t angle;
} PlatformDef;

static int16_t rd_i16(const uint8_t *p) { return (int16_t)(((uint16_t)p[0] << 8) | p[1]); }
static int32_t rd_i32(const uint8_t *p)
{
	return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]);
}

/* x-only read for platform_window() below -- avoids platform_read()'s full
 * 13-field byte-by-byte decode just to binary-search on x. */
static int16_t platform_x(uint16_t i) { return rd_i16(k_platform_bytes + (uint32_t)i * 30u); }

/* PLATFORM_SCENE's own field order, Platform_Serialize (Platform.c:2759-2770)
 * via tools/convert_objects.py's PLATFORM_SCENE. tileOrigin (bytes 17-24) is
 * read past but never stored: it is PLATFORM_REACT/HOVER_REACT-only state
 * (Platform_State_React/Hover_React/ReactWait/ReactSlow, none of which Act 1
 * ever uses -- _validate_platform()'s own known={0,1,2,4,6} set), dead data
 * for every row this file ever reads. */
static void platform_read(uint16_t i, PlatformDef *d)
{
	const uint8_t *rec = k_platform_bytes + (uint32_t)i * 30u;
	d->x = rd_i16(rec + 0);
	d->y = rd_i16(rec + 2);
	d->type = rec[4];
	d->amplitudeX = rd_i32(rec + 5);
	d->amplitudeY = rd_i32(rec + 9);
	d->speed = (int8_t)rec[13];
	d->hasTension = rec[14];
	d->frameID = (int8_t)rec[15];
	d->collision = rec[16];
	d->childCount = rec[25];
	d->angle = rd_i32(rec + 26);
}

/* Frame hitboxes, hand-derived from md_src/platform_data.c's own generated
 * platform_pieces[]/platform_normal[]/platform_swing[] piece tables (tools/
 * convert_objects.py) -- Platform's converted data carries no separate
 * hitbox table (unlike Sonic's/rings' own conversion), and Platform_Create's
 * own no-hitbox fallback (Platform.c:347-352: left=-32,top=-16,right=-8,
 * bottom=32) is a visibly malformed 24px-wide box sitting entirely left of
 * center, not something to reproduce on purpose. Derivation (bounding box of
 * every piece's own pivot+dx,dy+size rectangle, SPRITE_SIZE's own w=((size>>2
 * &3)+1)*8, h=((size&3)+1)*8 decode, md_src/obj_sprite.h):
 *   normal[0] (32t,  pieces 0-1, pivot -32,-22): two 32x32 pieces at dx=0,32
 *     -> x -32..32 (halfW 32), y -22..10.
 *   normal[1] (144t, pieces 2-11, pivot -32,-19): two columns (dx 0,32) of
 *     five 32-tall rows (dy 0,32,64,96,128, last row 16 tall)
 *     -> x -32..32 (halfW 32), y -19..125.
 *   normal[2] (24t,  pieces 12-13, pivot -24,-16): 32x32 at dx=0 + 16x32 at
 *     dx=32 -> x -24..24 (halfW 24), y -16..16.
 *   swing[0] (seat, pieces 16-17, pivot -24,-8): 32x16 at dx=0 + 16x16 at
 *     dx=32 -> x -24..24 (halfW 24), y -8..8.
 * normal[3] (32t) is never referenced by any Act 1 row (frameIDs used are
 * only 0,1,2 and -1/"do not draw") so it has no entry here. */
typedef struct { int8_t halfW, top, bottom; } PlatHitbox;
static const PlatHitbox k_frameHitbox[3] = {
	{ 32, -22,  10 },
	{ 32, -19, 125 },
	{ 24, -16,  16 },
};
static const PlatHitbox k_swingSeatHitbox = { 24, -8, 8 };

static const PlatHitbox *frame_hitbox(int8_t frameID)
{
	if (frameID < 0 || frameID > 2) return &k_frameHitbox[0];
	return &k_frameHitbox[frameID];
}

/* Per-instance runtime state -- ONLY Fall (trigger tick) and Push
 * (accumulated offset) need any at all: Fixed/Linear/Swing's drawn position
 * is a PURE function of g_platform_tick and the scene row alone (Platform_
 * State_Fixed/Linear/Swing, Platform.c:484-494,496-509,511-524, none of
 * which read anything but self->centerPos/amplitude/speed/angle/Zone->timer
 * -- verified against the read decomp, not assumed), which is exactly what
 * lets the 68000 side (md_src/platform.c) recompute the same three types'
 * positions with zero per-instance state of its own, only g_platform_tick.
 * Fall and Push are the two genuinely STATEFUL types in Act 1's set (a
 * trigger that only fires once, an accumulated push offset) -- see each
 * one's own function below for why the state recorded here is still enough
 * for the 68000 to independently reconstruct via its own observational
 * touch test (md_src/platform.c's own top comment has the full proof). */
static uint8_t  fallTriggered[PLATFORM_COUNT];
static uint32_t fallTriggerTick[PLATFORM_COUNT];
static int32_t  pushOffsetPx[PLATFORM_COUNT];   /* pixels, +right */
static uint8_t  pushInit[PLATFORM_COUNT];

/* Platform_State_Falling's own closed form (Platform.c:577-594,827-856):
 * velocity.y starts at 0 the instant falling begins and gains 0x3800 (16.16)
 * every tick, applied BEFORE the gain each tick (drawPos.y += velocity.y;
 * velocity.y += 0x3800;) -- so after f completed falling ticks (f=0,1,2,...),
 * displacement = sum_{k=0}^{f-1} k*0x3800 = 0x3800 * f*(f-1)/2. Falling2
 * (Platform.c:827-856) continues the IDENTICAL per-tick update with no
 * discontinuity (verified: both states run the exact same two lines), so
 * this one closed form covers both RSDK states for as long as this port
 * keeps the platform "falling" at all -- see PLATFORM_FALL_VANISH_TICKS
 * below for where this port stops, a deliberate simplification of Falling2's
 * own off-screen-reset-to-Hold/possible-respawn tail (Platform.c:837-856),
 * which depends on RSDK's own CheckOnScreen against this camera's exact
 * bounds and (Platform_State_Hold, Platform.c:788-797) an off-screen-forever
 * re-Create() loop this port does not reproduce -- see this batch's own
 * report for why: GHZ1's Fall platforms all sit over pits/gaps, so "falls
 * once, then is permanently gone" reads the same to a player as "falls
 * forever off-screen, technically still existing," and implementing RSDK's
 * own ambiguous reset tail exactly was not worth this batch's remaining time
 * budget against a difference that is never visible in play. */
#define PLATFORM_FALL_WAIT_TICKS   30   /* Platform_State_Fall's own self->timer, Platform.c:862-868 */
#define PLATFORM_FALL_GRAVITY      0x3800
#define PLATFORM_FALL_VANISH_TICKS 200  /* ~440px fallen (0x3800*200*199/2>>16) -- comfortably past any camera */

static int32_t fall_displacement_px(uint32_t fallTicks)
{
	/* 0x3800 * f*(f-1)/2, right-shifted to pixels -- f*(f-1) fits well
	 * inside int32 for every f this port ever reaches (capped at
	 * PLATFORM_FALL_VANISH_TICKS=200 by the caller before this is ever
	 * invoked with a larger f, 200*199=39800, times 0x3800 is still well
	 * inside int32 range). */
	int32_t f = (int32_t)fallTicks;
	int32_t disp16_16 = (PLATFORM_FALL_GRAVITY * (f * (f - 1) / 2));
	return disp16_16 >> 16;
}

/* Player_CheckCollisionBox (Collision.cpp:276-461) via sh_src/spring.c's own
 * already-verified spring_check_box() -- see that function's own header
 * comment for the full derivation this reuses verbatim, generalized here to
 * (a) an arbitrary hitbox (spring_check_box's own hbLeft/Top/Right/Bottom
 * parameters, unchanged) and (b) a platform's own this-tick velocity, so a
 * standing player is carried along exactly like Platform_HandleStood's own
 * collisionOffset application (Platform.c:1994-1996) -- Spring never needed
 * that second part (a spring never moves), Platform always does for a moving
 * solid type (Push, the only Act 1 SOLID entries that ever move). Returns
 * the C_* side, 0 (C_NONE) if no collision. */
#define C_NONE   0
#define C_TOP    1
#define C_LEFT   2
#define C_RIGHT  3
#define C_BOTTOM 4

static uint8_t platform_check_box(Player *p, int32_t sx, int32_t sy,
                                  int8_t hbLeft, int8_t hbTop, int8_t hbRight, int8_t hbBottom,
                                  int32_t velX, int32_t velY)
{
	int8_t oL = p->e.outer.left, oT = p->e.outer.top;
	int8_t oR = p->e.outer.right, oB = p->e.outer.bottom;
	int32_t thisIX = sx >> 16, thisIY = sy >> 16;
	int32_t otherIX = p->e.x >> 16, otherIY = p->e.y >> 16;
	int32_t collideX = p->e.x, collideY = p->e.y;
	uint8_t sideH = C_NONE, sideV = C_NONE, side;
	(void)velY;   /* Y is always exactly snapped from sy (already current-tick); see C_TOP's own comment below */
	int32_t cx, cy;

	if (otherIX <= (hbRight + hbLeft + 2 * thisIX) >> 1) {
		if (otherIX + oR >= thisIX + hbLeft
		    && thisIY + hbTop < otherIY + oB - 1
		    && thisIY + hbBottom > otherIY + oT + 1) {
			sideH = C_LEFT;
			collideX = sx + TO_FIXED(hbLeft - oR);
		}
	} else {
		if (otherIX + oL < thisIX + hbRight
		    && thisIY + hbTop < otherIY + oB - 1
		    && thisIY + hbBottom > otherIY + oT + 1) {
			sideH = C_RIGHT;
			collideX = sx + TO_FIXED(hbRight - oL);
		}
	}

	if (otherIY < (hbTop + hbBottom + 2 * thisIY) >> 1) {
		if (otherIY + oB >= thisIY + hbTop
		    && thisIX + hbLeft < otherIX + oR - 1
		    && thisIX + hbRight > otherIX + oL + 1) {
			sideV = C_TOP;
			collideY = sy + TO_FIXED(hbTop - oB);
		}
	} else {
		if (otherIY + oT < thisIY + hbBottom
		    && thisIX + hbLeft < otherIX + oR - 1) {
			if (otherIX + oL + 1 < thisIX + hbRight) {
				sideV = C_BOTTOM;
				collideY = sy + TO_FIXED(hbBottom - oT);
			}
		}
	}

	cx = (collideX - p->e.x) >> 16;
	cy = (collideY - p->e.y) >> 16;
	if ((cx * cx >= cy * cy && (sideV || !sideH)) || (!sideH && sideV))
		side = sideV;
	else
		side = sideH;

	switch (side) {
	case C_TOP:
		p->e.y = collideY;
		if (p->e.velY > 0) p->e.velY = 0;
		if (!p->e.onGround && p->e.velY >= 0) {
			p->e.groundVel = p->e.velX;
			p->e.angle = 0;
			p->e.onGround = 1;
		}
		p->controlLock = 0;
		p->e.collisionMode = CMODE_FLOOR;
		/* Platform_HandleStood's own carry (Platform.c:1994-1996): ride the
		 * platform's own this-tick HORIZONTAL delta only -- collideY above
		 * already snapped the player to the platform's CURRENT (this-tick)
		 * surface directly (sy is the platform's own already-advanced
		 * position, not last tick's), so adding a Y delta on top of an
		 * already-exact snap would double-count vertical motion. X is never
		 * snapped by this function (only tested for overlap), so it is the
		 * only axis that genuinely needs an explicit carry. */
		p->e.x += velX;
		break;
	case C_LEFT:
		p->e.x = collideX;
		p->controlLock = 0;
		break;
	case C_RIGHT:
		p->e.x = collideX;
		p->controlLock = 0;
		break;
	case C_BOTTOM:
		p->e.y = collideY;
		if (p->e.velY < 0) p->e.velY = 0;
		break;
	default:
		break;
	}
	return side;
}

/* One-way top-only landing (PLATFORM_C_PLATFORM -- Platform_Collision_
 * Platform's own Player_CheckCollisionPlatform call, Platform.c:1737-1760,
 * whose own RSDK source was not among this batch's read decomp excerpts).
 * 8px vertical landing band -- not a guess: matches Bridge_HandleCollisions'
 * own one-way test width (Bridge.c:176-182, hitboxBridge.bottom =
 * hitboxBridge.top + 8), the one place this batch's read decomp DOES show
 * RSDK's actual one-way-platform band width, reused here rather than
 * inventing an unrelated tolerance. Carries the player with the platform's
 * own this-tick HORIZONTAL velocity only, same as platform_check_box's
 * C_TOP case and for the identical reason (surfaceY below is always derived
 * from platPy, the CALLER's already-current-tick position, so the Y axis is
 * exactly snapped with no separate delta needed -- only X, never otherwise
 * written here, needs an explicit carry). velY is accepted, not applied, so
 * every call site can pass a symmetric (dx,dy) pair without each needing to
 * know this function only wants one half of it. */
static uint8_t platform_land_top(Player *p, int32_t platPx, int32_t platPy,
                                 int8_t halfW, int8_t top, int32_t velX, int32_t velY)
{
	int32_t playerX = p->e.x >> 16, playerY = p->e.y >> 16;
	int8_t oL = p->e.outer.left, oR = p->e.outer.right, oB = p->e.outer.bottom;
	int32_t surfaceY = platPy + top;
	int32_t feet;
	(void)velY;

	if (p->e.velY < 0) return 0;
	if (playerX + oR <= platPx - halfW || playerX + oL >= platPx + halfW) return 0;

	feet = playerY + oB;
	if (feet < surfaceY || feet > surfaceY + 8) return 0;

	p->e.y = TO_FIXED(surfaceY - oB);
	if (p->e.velY > 0) p->e.velY = 0;
	p->e.onGround = 1;
	p->e.collisionMode = CMODE_FLOOR;
	p->e.groundVel = p->e.velX;
	p->controlLock = 0;
	p->e.x += velX;
	return 1;
}

/* ---- Per-type position (all pixels, matching this file's own PlatformDef.x/y) --- */

/* Platform_State_Fixed (Platform.c:484-494): drawPos==centerPos always.
 * Deviation: self->stoodAngle's small Sin256-driven bob (only active while
 * self->hasTension, +-4/tick toward a 64-step cap, Platform_Update:26-31) is
 * not reproduced -- verified against this stage's own data that this never
 * matters for the SOLID collision surface's own position (the bob is
 * Sin256(stoodAngle)<<10 in 16.16, at most Sin256(64)<<10 = 256<<10 =
 * 262144, i.e. 4px -- a real but small, purely cosmetic wobble; every Fixed
 * entry in this stage has hasTension==0 anyway, so this deviation has zero
 * effect on Act 1's actual 22 Fixed instances, confirmed by this batch's own
 * data dump, not assumed). */
static void fixed_pos(const PlatformDef *d, int32_t *px, int32_t *py)
{
	*px = d->x;
	*py = d->y;
}

/* Platform_State_Linear (Platform.c:496-509): drawPos = amplitude*Sin1024(
 * speed*(rotation+Zone->timer)) + centerPos, rotation==self->angle (scene
 * field, always 0 in this stage's own 16 Linear rows) since Platform_Create
 * zeroes self->angle after copying it to self->rotation (Platform.c:185-186).
 * amplitude here is the GENERIC self->amplitude>>=10 every type gets
 * (Platform.c:154-155), used directly with no further shift -- see this
 * file's own header comment on the exact 16.16 arithmetic this collapses to
 * (amp10 * Sin1024(angle) is already a 16.16 pixel delta, no >>16 missing,
 * only added on the FINAL pixel conversion this integer-pixel table needs). */
static void linear_pos(const PlatformDef *d, uint32_t tick, int32_t *px, int32_t *py)
{
	int32_t amp10X = d->amplitudeX >> 10;
	int32_t amp10Y = d->amplitudeY >> 10;
	int32_t angle = d->speed * (d->angle + (int32_t)tick);
	int32_t s = platform_sin1024(angle);

	*px = d->x + ((amp10X * s) >> 16);
	*py = d->y + ((amp10Y * s) >> 16);
}

/* Platform_State_Swing (Platform.c:511-524), plain PLATFORM_SWING only (Act 1
 * never places PLATFORM_SWING_REACT -- _validate_platform()'s own known set).
 * groundVel = 4*self->angle (scene field, always 0 in this stage's own 5
 * Swing rows -- verified) is folded in for completeness rather than assumed
 * zero. self->amplitude.y here is the Swing-specific <<4 rescale
 * (Platform_Create:254, applied ON TOP of the generic >>10 every type gets),
 * i.e. ((raw>>10)<<4) == raw>>6 -- see this file's header comment for the
 * pixel-radius derivation that confirms this (8px raw amplitude -> 128px
 * swing radius, matching this stage's own scene data). Returns the seat's
 * own angle too (needed by md_src/platform.c's chain-link emitter, which
 * this file has no equivalent of -- SH2-side collision only ever needs the
 * seat's own final position, never the chain's, since the chain is pure
 * decoration with no hitbox of its own in the original either). */
static int32_t swing_angle(const PlatformDef *d, uint32_t tick)
{
	int32_t groundVel = 4 * d->angle;
	int32_t amp10X = d->amplitudeX >> 10;
	return groundVel + 0x100 + (((amp10X * platform_sin1024(d->speed * (int32_t)tick)) + 0x200) >> 14);
}

static void swing_pos(const PlatformDef *d, uint32_t tick, int32_t *px, int32_t *py)
{
	int32_t ampSwingY = d->amplitudeY >> 6;   /* (raw>>10)<<4 == raw>>6 */
	int32_t angle = swing_angle(d, tick);

	*px = d->x + ((ampSwingY * platform_cos1024(angle)) >> 16);
	*py = d->y + ((ampSwingY * platform_sin1024(angle)) >> 16);
}

/* Platform_State_Fall/Falling/Falling2 (Platform.c:858-875,577-594,827-856) --
 * see fall_displacement_px()'s own comment for the closed form. Returns 0 if
 * this instance has fully "vanished" (this port's own simplification of the
 * original's off-screen tail, see PLATFORM_FALL_VANISH_TICKS's own comment). */
static uint8_t fall_pos(const PlatformDef *d, uint16_t idx, int32_t *px, int32_t *py)
{
	*px = d->x;
	if (!fallTriggered[idx]) { *py = d->y; return 1; }

	{
		uint32_t elapsed = g_platform_tick - fallTriggerTick[idx];
		if (elapsed < PLATFORM_FALL_WAIT_TICKS) { *py = d->y; return 1; }
		{
			uint32_t fallTicks = elapsed - PLATFORM_FALL_WAIT_TICKS;
			if (fallTicks >= PLATFORM_FALL_VANISH_TICKS) return 0;
			*py = d->y + fall_displacement_px(fallTicks);
			return 1;
		}
	}
}

/* Platform_State_Push (Platform.c:686-786), reduced: this port carries the
 * "player pushes it, it slides while pushed, drifts back to rest speed when
 * released" behaviour (Platform.c:693-723's own left/right velocity pick and
 * timer-gated ramp) but drops the tile-grip floor-follow (RSDK.ObjectTileGrip,
 * Platform.c:738-750) and the fall-when-unsupported/slide-off states
 * (Platform_State_Push_Fall/SlideOffL/R, Platform.c:981-1056) -- both need
 * real tile collision under an ARBITRARY, MOVING x position, which only the
 * SH2's own path.c collision tables could answer and this batch's remaining
 * time did not extend to wiring a Platform-specific tile-follow through them.
 * GHZ1's own single Push instance (x=15168) sits on visibly flat ground, so
 * this simplification never triggers a floor mismatch there; a Push
 * instance placed over a slope or ledge elsewhere would show it. Horizontal
 * only, matching the original (Push never gets a Y offset of its own). */
#define PUSH_MAX_OFFSET_PX 256   /* Platform_State_Push_Init's own updateRange.x, TO_FIXED(512)/2 -- headroom, not decomp-exact */

static void push_pos(const PlatformDef *d, uint16_t idx, Player *p, int32_t *px, int32_t *py, int32_t *velXOut)
{
	const PlatHitbox *hb = frame_hitbox(d->frameID);
	int32_t curPx = d->x + pushOffsetPx[idx];
	int32_t speedPx = d->speed;   /* self->speed<<=11 in Create is a fixed-point detail this port skips: driving directly in px/tick */
	int32_t playerX = p->e.x >> 16, playerY = p->e.y >> 16;
	int8_t oL = p->e.outer.left, oR = p->e.outer.right, oT = p->e.outer.top, oB = p->e.outer.bottom;
	int32_t moveX = 0;

	if (!pushInit[idx]) { pushOffsetPx[idx] = 0; pushInit[idx] = 1; curPx = d->x; }

	/* Pushed while grounded, holding into a side, and vertically overlapping
	 * the platform's own box -- Platform_State_Push's own player-side test
	 * (Platform.c:728-736) reduced to a plain overlap check (no per-player
	 * pushPlayersL/R bitmask needed with a single player). */
	if (p->e.onGround && playerY + oB > d->y + hb->top && playerY + oT < d->y + hb->bottom) {
		if (p->e.velX > 0 && playerX + oR >= curPx - hb->halfW && playerX + oR <= curPx - hb->halfW + 4)
			moveX = -speedPx;
		else if (p->e.velX < 0 && playerX + oL <= curPx + hb->halfW && playerX + oL >= curPx + hb->halfW - 4)
			moveX = speedPx;
	}

	pushOffsetPx[idx] += moveX;
	if (pushOffsetPx[idx] > PUSH_MAX_OFFSET_PX) pushOffsetPx[idx] = PUSH_MAX_OFFSET_PX;
	if (pushOffsetPx[idx] < -PUSH_MAX_OFFSET_PX) pushOffsetPx[idx] = -PUSH_MAX_OFFSET_PX;

	*px = d->x + pushOffsetPx[idx];
	*py = d->y;
	*velXOut = TO_FIXED(moveX);
}

/* CollapsingPlatform-style trigger test, reused for Fall's own top-only
 * touch detection: Player_CheckCollisionPlatform's own effect (Platform_
 * HandleStood is called the tick a player's feet land in the platform's own
 * top band) IS platform_land_top()'s own return value -- Fall just also
 * needs to remember the FIRST tick that happens, which platform_land_top()
 * itself has no reason to know about. */
/* Player-proximity gate for the per-tick scan below (perf: GHZ1's own 60
 * Platform rows, most of a ~15000px-wide act away from the player on any
 * given tick, would otherwise all run their own trig/collision math every
 * tick regardless). ghz_platforms_sh is x-sorted ascending (tools/
 * convert_objects.py's write_scene_table sorts every generated scene table
 * this way -- see that function's own comment; this file's own top comment
 * already documents this table specifically as "x-sorted"), so the same
 * binary-search shape md_src/obj_generic.c's own x_window()/obj_type_window()
 * already uses for the identical "camera/player only ever touches a small
 * slice of a big x-sorted table" situation on the 68000 side applies here
 * too. Gated on the PLAYER's own x, not the camera's: this loop's job is
 * collision (does this entity touch the player this tick), not drawing, and
 * s_main.c has not yet recomputed the camera's screen position for this
 * tick when platform_apply() runs (that happens at the very end of the
 * loop, after every _apply() call) -- using the player's own
 * just-updated (player_update() already ran) position needs no extra
 * parameter threaded through this or any other _apply() call in s_main.c's
 * batch, and is exactly what actually determines whether a touch is
 * possible this tick.
 *
 * PLATFORM_GATE_MARGIN (px) must exceed the largest possible distance
 * between an entity's AUTHORED x (what this table is sorted by) and any
 * player x that could still touch it THIS tick -- verified against this
 * stage's own converted data (assets/ghz/platforms.bin), not guessed:
 *   Push (the only type whose live x drifts from its authored x): up to
 *     PUSH_MAX_OFFSET_PX=256 (push_pos's own comment) plus its own frame
 *     hitbox halfW (<=32, k_frameHitbox).
 *   Swing: amplitude.y<=524288 (16.16) -> ampSwingY=amplitude.y>>6<=8192 ->
 *     radius (ampSwingY*1024)>>16 <= 128px (matches swing_pos's own "8px raw
 *     amplitude -> 128px swing radius" comment), well under Push's figure.
 *   Linear: worst observed amplitude.x=-6291456 -> amp10X=-6144 ->
 *     (amp10X*1024)>>16 = 96px, also well under Push's figure.
 *   Fixed/Fall: no x drift at all (fixed_pos/fall_pos both leave *px==d->x).
 * 256 (Push's own drift) + 32 (widest frame halfW) + ~64 (player's own
 * hitbox plus a speed/rounding buffer) = 352, rounded well up to 512 for
 * headroom -- correctness matters far more here than a tight window, and
 * 512px out of this act's ~15000px width still removes the overwhelming
 * majority of the table on almost every tick. */
#define PLATFORM_GATE_MARGIN 512

static void platform_window(int32_t playerXpx, uint16_t n, uint16_t *lo, uint16_t *hi)
{
	int32_t xloWant = playerXpx - PLATFORM_GATE_MARGIN;
	int32_t xhiWant = playerXpx + PLATFORM_GATE_MARGIN;
	uint16_t a, b, m;

	a = 0; b = n;
	while (a < b) {
		m = (uint16_t)(a + (b - a) / 2);
		if (platform_x(m) < xloWant) a = (uint16_t)(m + 1); else b = m;
	}
	*lo = a;

	a = *lo; b = n;
	while (a < b) {
		m = (uint16_t)(a + (b - a) / 2);
		if (platform_x(m) < xhiWant) a = (uint16_t)(m + 1); else b = m;
	}
	*hi = a;
}

void platform_apply(Player *p)
{
	uint16_t n, i, lo, hi;

	g_platform_tick++;

	n = ghz_platforms_sh[0];
	if (n > PLATFORM_COUNT) n = PLATFORM_COUNT;

	/* Every type's own drawn/collision position this tick is a pure
	 * function of g_platform_tick (just advanced above, unconditionally,
	 * exactly once regardless of how the loop below is windowed) and the
	 * scene row alone -- see this file's own PlatformDef comment. Fall's
	 * fallTriggered[]/fallTriggerTick[] and Push's pushOffsetPx[] are the
	 * only per-instance state, and both are safe to skip while out of
	 * range: Fall's own elapsed-tick check (fall_pos) re-derives correctly
	 * from g_platform_tick (a shared clock that keeps advancing above
	 * regardless of windowing) the instant this index is processed again,
	 * with no drift; Push's own offset only ever changes through a player
	 * actively touching it (push_pos's own onGround+overlap gate), which
	 * cannot happen while the player is outside this margin. */
	platform_window(p->e.x >> 16, n, &lo, &hi);

	for (i = lo; i < hi; i++) {
		PlatformDef d;
		int32_t px = 0, py = 0;
		const PlatHitbox *hb;

		platform_read(i, &d);

		switch (d.type) {
		case PLATFORM_FIXED:
			fixed_pos(&d, &px, &py);
			hb = frame_hitbox(d.frameID);
			if (d.collision == PLATFORM_C_SOLID) {
				platform_check_box(p, TO_FIXED(px), TO_FIXED(py),
				                   (int8_t)(-hb->halfW), hb->top, hb->halfW, hb->bottom, 0, 0);
			} else if (d.collision == PLATFORM_C_PLATFORM) {
				platform_land_top(p, px, py, hb->halfW, hb->top, 0, 0);
			}
			break;

		case PLATFORM_FALL:
			hb = frame_hitbox(d.frameID);
			if (!fall_pos(&d, i, &px, &py)) break;   /* vanished: no collision left */
			if (!fallTriggered[i]) {
				if (platform_land_top(p, px, py, hb->halfW, hb->top, 0, 0))
					{ fallTriggered[i] = 1; fallTriggerTick[i] = g_platform_tick; }
			} else {
				platform_land_top(p, px, py, hb->halfW, hb->top, 0, 0);
			}
			break;

		case PLATFORM_LINEAR:
			if (d.collision == PLATFORM_C_NONE) break;   /* invisible driver row -- see md_src/platform.c's own comment */
			{
				int32_t prevPx, prevPy;
				linear_pos(&d, g_platform_tick - 1, &prevPx, &prevPy);
				linear_pos(&d, g_platform_tick, &px, &py);
				hb = frame_hitbox(d.frameID);
				platform_land_top(p, px, py, hb->halfW, hb->top,
				                  TO_FIXED(px - prevPx), TO_FIXED(py - prevPy));
			}
			break;

		case PLATFORM_SWING:
			{
				int32_t prevPx, prevPy;
				swing_pos(&d, g_platform_tick - 1, &prevPx, &prevPy);
				swing_pos(&d, g_platform_tick, &px, &py);
				platform_land_top(p, px, py, k_swingSeatHitbox.halfW, k_swingSeatHitbox.top,
				                  TO_FIXED(px - prevPx), TO_FIXED(py - prevPy));
			}
			break;

		case PLATFORM_PUSH:
			{
				int32_t velX;
				push_pos(&d, i, p, &px, &py, &velX);
				hb = frame_hitbox(d.frameID);
				platform_check_box(p, TO_FIXED(px), TO_FIXED(py),
				                   (int8_t)(-hb->halfW), hb->top, hb->halfW, hb->bottom, velX, 0);
			}
			break;

		default:
			break;
		}
	}
}
