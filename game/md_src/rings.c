#include "rings.h"
#include "vdp.h"
#include "obj_data.h"
#include "obj_generic.h"
#include "obj_pool.h"
#include "sonic_data.h"   /* SONIC_FRAME_COUNT */
#include "ring_data.h"    /* RING_* frame/tile constants, ring_tiles[],
                           * ring_sparkle{1,3}_durations[] -- tools/convert_ring.py */
#include "assets_gen.h"

/* tools/convert_rings.py's kept-ring count for GHZ1 (Mania filter, "keep
 * entity iff (filter & 3) != 0", applied to Scene1.bin's Ring entities).
 * Sized here at compile time -- same convention as descriptor.h's
 * GHZ_MAP_W/GHZ_MAP_H -- rather than read from rings.bin's own leading
 * count field, since the collected bitfield below has to be a fixed size.
 * rings_init() verifies the two agree and disables rings on a mismatch. */
#define RING_COUNT 445

/* assets/ghz/rings.bin: big-endian u16 count, then RING_COUNT * (int16 x,
 * int16 y), pixel centre, ascending by x. m68k is big-endian, so these
 * fields read directly with no byte-swap, same as every other .incbin'd
 * table in this codebase. One manifest entry now (ASSET_GHZ_RINGS, tools/
 * gen_assets.py) instead of two separately-linked symbols: ghz_ring_count
 * is that entry's own pointer, ghz_ring_xy is the same bytes plus the
 * leading count word's fixed 2-byte width, same derivation md_src/assets.s
 * used to spell as `ghz_ring_xy = ghz_ring_count + 2`. */
static const uint16_t *const ghz_ring_count_p = ASSET_GHZ_RINGS;
#define ghz_ring_count (*ghz_ring_count_p)
static const int16_t *const ghz_ring_xy =
	(const int16_t *)((const uint8_t *)ASSET_GHZ_RINGS + 2);
#define RING_X(i) ghz_ring_xy[(i) * 2]
#define RING_Y(i) ghz_ring_xy[(i) * 2 + 1]

/* assets/sonic/hitbox.bin: animator hitbox 0 ("outer") per Sonic frame, same
 * order as sonic_frames[] -- tools/convert_sonic.py. Bank-1 asset now (tools/
 * gen_assets.py's manifest), same as ghz_ring_count/ghz_ring_xy above. */
static const int8_t *const sonic_hitbox = ASSET_SONIC_HITBOX;

/* ring_tiles used to be `extern const uint32_t ring_tiles[];` (ring_data.h,
 * resolved against assets.s's own .incbin); that generator no longer
 * declares it -- see tools/convert_ring.py's own comment -- since the data
 * moved to bank 1 (tools/gen_assets.py's manifest) too. */
static const uint32_t *const ring_tiles = ASSET_RING_TILES;

/* Ring.c:102-105 (Ring_StageLoad): the ring's own, fixed hitbox. */
#define RING_HITBOX_LEFT   (-8)
#define RING_HITBOX_TOP    (-8)
#define RING_HITBOX_RIGHT    8
#define RING_HITBOX_BOTTOM   8

/* Player.c:12: Player_FallbackHitbox, used when Player_GetHitbox has no real
 * per-frame hitbox to return (Player.c:2244-2248). The real engine only
 * takes this path on a null animator, which cannot happen once this port's
 * boot handshake has produced a first frame (main.c blocks on it), so the
 * bounds check below is this port's stand-in for that same defensive
 * intent against a corrupt/out-of-range frameIndex. */
#define FALLBACK_HITBOX_LEFT   (-10)
#define FALLBACK_HITBOX_TOP    (-20)
#define FALLBACK_HITBOX_RIGHT    10
#define FALLBACK_HITBOX_BOTTOM   20

/* Ring.c:177, the surviving iterations of Ring_Collect's sparkle loop (see
 * spawn comments below): i=0 gives timer=2*0=0, i=2 gives timer=2*2=4. */
#define SPARKLE0_TIMER 0
#define SPARKLE1_TIMER 4

/* RING_TYPE_SPARKLE1/3, Ring.h:9,11 -- kept as the same numbers Ring_Collect
 * uses (RING_TYPE_SPARKLE1 + i%3, Ring.c:169) so a diff against the source
 * reads directly, even though only these two values ever appear here. */
#define SPARKLE_ANIM_1 2
#define SPARKLE_ANIM_3 4

typedef struct {
    uint8_t  active;
    uint8_t  animID;         /* SPARKLE_ANIM_1 or SPARKLE_ANIM_3 */
    uint8_t  maxFrameCount;  /* RING_SPARKLE{1,3}_MAXFRAME, ring_data.h */
    uint8_t  frameID;
    uint8_t  speed;          /* RSDK.Rand(6,8), Ring.c:176 */
    uint8_t  timer;          /* Ring_State_Sparkle, Ring.c:757-769 */
    uint16_t animTimer;      /* Animator.timer, Animation.cpp:150-176 */
    int16_t  worldX, worldY; /* fixed at spawn; sparkles never move here */
    uint16_t age;            /* spawn order, for the pool-full eviction below */
} Sparkle;

static uint8_t  collected[(RING_COUNT + 7) / 8];
static uint16_t ringPlayerCount;      /* player->rings, Player.c:926's clamp */
static uint8_t  ringFrame;            /* Zone->ringFrame, Zone.c:92-95 */
static uint8_t  ringFrameParity;      /* "every 2nd tick" -- Zone->timer&1 stand-in */
static uint32_t ringRandSeed;
static Sparkle  sparkles[SPARKLE_POOL_SIZE];
static uint16_t sparkleClock;         /* increases every spawn */
static uint16_t sparkleDropCount;     /* pool-full evictions, cosmetic deviation */

/* Stashed once per rings_update() call, read by ring_decide() below --
 * ObjDecideFn (obj_data.h) does not carry camY or Sonic's hitbox directly
 * (only entryIndex/ex/ey/sonicWorldX/Y/sonicFrameIndex), so a decide() that
 * needs either recomputes it once here rather than per-candidate (rings.c's
 * own x window can hold dozens of entries a frame). */
static uint16_t curCamY;
static int8_t   curHbLeft, curHbTop, curHbRight, curHbBottom;

/* Rotation frames: VRAM ART BUDGET (art-budget trim task, 2026-08-18) --
 * used to STREAM through md_src/obj_generic.h's per-class ANIMATION WINDOW
 * (an 8-tile reservation, cycling one of the 16 real rotation frames in at a
 * time -- the exact churn this whole task exists to kill). tools/
 * convert_objects.py's convert_ring_art now converts only RING_ANIM0_FRAMES
 * =4 of those 16 (Sonic 1's own count, this task's brief), subsampled evenly
 * around the spin (quarter-turns) -- ringRotFrames[] holds all 4,
 * PERMANENTLY VRAM-resident (16 tiles total, ring_rot_onBase() below), no
 * streaming, no churn. Every ring on screen still shares the SAME rotation
 * phase (ringFrame below, Zone_StaticUpdate's own behaviour, Zone.c:92-95)
 * -- rings were never independently animated in the original either -- but
 * ringFrame keeps counting its original 0..15 range at its original pace
 * (unchanged timing feel); ring_decide() below just maps it onto this
 * smaller resident set with ringFrame>>2. */
static ObjFrame ringRotFrames[RING_ANIM0_FRAMES];
static uint16_t ringRotBase;
static uint8_t  ringRotLive;
static uint8_t  ringRotRegistered;   /* boot-load success, see rings_update()'s own comment */
static ObjPiece ringPiece;

static ObjDrawDecision ring_decide(void *st, uint16_t entryIndex, int16_t ex, int16_t ey,
                                   int16_t sonicWorldX, int16_t sonicWorldY,
                                   uint16_t sonicFrameIndex);

/* entries is patched in at runtime, in rings_init() -- the usual "reading
 * another static variable's VALUE is not a constant expression" reason
 * (springs.c's own comment on springType). frames points at ringRotFrames
 * (a stable pointer for the whole program's life -- only ITS CONTENTS'
 * tileOffset changes, once, in ring_rot_onBase() below, the same "rebase
 * once at grant time" pattern every plain-resident tenant in this batch
 * uses, e.g. motobug.c's own mbFrames).
 *
 * tilePixels/residentTileCount are 0/NULL here on purpose -- rings' tile
 * upload never goes through obj_type_init() at all: both the rotation
 * portion (ringRotArenaDesc below) and the sparkle portion
 * (ringSparkleArenaDesc further down) upload through the arena's own
 * boot-load path directly, neither of which obj_type_draw() itself ever
 * reads either field for. */
static ObjTypeDesc ringType = {
    (const void *)ghz_ring_xy, sizeof(int16_t) * 2, RING_COUNT, ghz_ring_count_p,
    (const uint32_t *)0, 0,
    ringRotFrames,
    &ringPiece,
    OBJ_PRI_RING, RING_PAL, 0 /* low priority, matches springs/signpost */,
    16,                        /* marginX, matches the old camX-16/+16 window */
    ring_decide, 0,
    /* Not templated (Job 1, this task): ringPiece is a single SHARED piece
     * (dx=dy=0, pieceOffset always 0) reused across all RING_ANIM0_FRAMES
     * rotation frames -- the parallel-array-indexed-by-pieceOffset scheme
     * every other templated type uses cannot represent "4 different frames'
     * worth of attr at the same index 0" this way. Rings' own per-piece
     * legacy cost is already minimal (1 piece, constant dx/dy, no flip) --
     * flagged as a candidate for a bespoke per-frame (not per-pieceOffset)
     * template scheme in a follow-up if profiling still shows this hot.
     * Legacy obj_emit_pieces() path unchanged. */
    0, 0
};

/* Every one of the 4 kept rotation frames is exactly 4 tiles (RING_ANIM0_
 * FRAMES*4=16 checks out against RING_SPARKLE1_TILE_BASE=16, ring_data.h),
 * so this resident tenant is 16 tiles total. */
#define RING_ROT_TILES ((uint16_t)(RING_ANIM0_FRAMES * 4))
#define RING_SPARKLE_TILES ((uint16_t)(RING_TILE_COUNT - RING_SPARKLE1_TILE_BASE))   /* 8 */

/* entries/sheetPixels are patched in at runtime in rings_init() (same reason
 * springType.entries/tilePixels above are: a static initializer here would
 * be reading another static const variable's VALUE, not a constant
 * expression GCC is willing to fold, only its ADDRESS -- see springs.c's own
 * comment on springType).
 *
 * lookaheadX is a plain ARENA_LOOKAHEAD_X(RING_ROT_TILES) now -- the old
 * "avoid contending with the sparkle grant's own amortized upload budget"
 * concern this comment used to carry no longer applies: both rotation and
 * sparkle are boot-loaded SYNCHRONOUSLY (obj_arena_boot_load(), one plain
 * vdp_tiles_load() each, no per-vblank amortization at all), so there is no
 * shared runtime upload budget left for the two to contend over any more --
 * see obj_arena_boot_load()'s own doc comment. */
static void ring_rot_onBase(uint16_t base);
static void ring_rot_onLive(uint8_t live) { ringRotLive = live; }

static ArenaClassDesc ringRotArenaDesc = {
    (const void *)0, sizeof(int16_t) * 2, RING_COUNT,
    (const uint32_t *)0, RING_ROT_TILES,
    (int16_t)ARENA_LOOKAHEAD_X(RING_ROT_TILES),
    OBJ_PRI_RING,
    ring_rot_onBase, ring_rot_onLive
};

static void ring_rot_onBase(uint16_t base)
{
    uint8_t i;
    ringRotBase = base;
    for (i = 0; i < RING_ANIM0_FRAMES; i++)
        ringRotFrames[i].tileOffset = (uint16_t)(base + i * 4);
}

/* The sparkle portion of ring_tiles (ring_data.h: RING_SPARKLE1_TILE_BASE=64
 * through RING_TILE_COUNT=156, 92 tiles) stays on the WHOLE-SHEET-RESIDENT
 * arena above, registered separately from the rotation window above it --
 * NOT migrated onto obj_anim_window_register(), on purpose. Every active
 * collect sparkle (rings_emit_sparkles' own Sparkle pool) carries its OWN
 * independent frameID, timer and speed (RSDK.Rand(6,8) per spawn) -- real
 * simultaneous per-instance divergence, exactly the workload obj_generic.h's
 * own top-of-section comment names as the case a single lockstep window
 * cannot correctly serve (two sparkles born a few ticks apart would be
 * forced to show the identical frame, visibly wrong on an effect whose
 * whole visual IS its frame-by-frame decay). 92 tiles resident is cheap
 * enough that there is no reason to solve that harder problem here; this is
 * the "stay on the arena instead" half of this file's own migration, kept
 * deliberately unmigrated and documented as such rather than silently left
 * looking like an oversight. */
static uint16_t ringSparkleBase;
static uint8_t  ringSparkleLive;
static void ring_sparkle_onBase(uint16_t base) { ringSparkleBase = base; }
static void ring_sparkle_onLive(uint8_t live) { ringSparkleLive = live; }

static ArenaClassDesc ringSparkleArenaDesc = {
    (const void *)0, sizeof(int16_t) * 2, RING_COUNT,
    (const uint32_t *)0, RING_SPARKLE_TILES,
    ARENA_LOOKAHEAD_X(RING_SPARKLE_TILES),
    OBJ_PRI_RING,
    ring_sparkle_onBase, ring_sparkle_onLive
};

/* RSDK::Rand, dependencies/RSDKv5/RSDKv5/RSDK/Core/Math.hpp:115-128 (the
 * unseeded, global-randSeed overload -- Ring_Collect never uses
 * RandSeeded). Transcribed exactly, including the LCG's own wraparound
 * (randSeed is uint32 in the source too, so this is well-defined modular
 * arithmetic there as much as here) and the min>max-safe reduction.
 *
 * Deviation: the original seeds randSeed from the title's own boot sequence
 * (SetRandSeed, driven by RNG state this port has no equivalent of);
 * rings_init() seeds it with a fixed constant instead, and calls it with
 * plain pixel ranges rather than the original's 16.16 fixed-point ones
 * (Ring_Collect's own values, scaled: RSDK.Rand(-TO_FIXED(8), TO_FIXED(8))
 * for the sparkle spawn offset, RSDK.Rand(6,8) for its speed -- only the
 * speed call's range survives unscaled, since this port never carries
 * sub-pixel position anywhere else either). Net effect: sparkle scatter
 * position and the 6-vs-7 speed choice follow a different, but equally
 * pseudo-random, stream than the original's -- cosmetic only. */
static int32_t rings_rand(int32_t min, int32_t max)
{
    uint32_t seed1 = ringRandSeed * 0x41c64e6dUL + 0x3039UL;
    uint32_t seed2 = seed1 * 0x41c64e6dUL + 0x3039UL;
    int32_t res;

    ringRandSeed = seed2 * 0x41c64e6dUL + 0x3039UL;
    res = (int32_t)((((seed1 >> 0x10) & 0x7ffUL) << 10 ^ ((seed2 >> 0x10) & 0x7ffUL)) << 10
                    ^ ((ringRandSeed >> 0x10) & 0x7ffUL));

    if (min < max)
        return min + res % (max - min);
    return max + res % (min - max);
}

static uint8_t ring_is_collected(uint16_t i)
{
    return (collected[i >> 3] >> (i & 7)) & 1;
}

static void ring_set_collected(uint16_t i)
{
    collected[i >> 3] |= (uint8_t)(1 << (i & 7));
}

/* ---- Lost rings (Ring_LoseRings, Ring.c:199-246) -------------------------
 *
 * A hit's scattered rings: their own runtime pool, same shape as the
 * sparkle pool above (fixed-size, oldest-dropped-on-full) but with real
 * per-tick physics instead of a fire-and-forget animation -- gravity, a
 * bounce, a re-collection window, an expiry timer, all transcribed from
 * Ring_State_Lost (Ring.c:598-631).
 *
 * Deviation 1, the trigger: the original tells every lost ring what to do
 * because Player_Hit calls Ring_LoseRings directly, in the same C++ call
 * stack, on the same CPU. This port's ring state (ringPlayerCount, this
 * pool) is 68000-only and sh_src/player.c's player_hit() runs on the slave
 * SH2 -- the wire protocol between them has no spare bit in the 68000<-SH2
 * direction to carry a "the player was just hit" event (sh_src/comm.h's own
 * "fully allocated, no spare bits" note; the one register that direction
 * owns, COMM_TICK, is tick+pad with every pad bit already real controller
 * state). So this file infers a hit instead: player_hit() unconditionally
 * plays ANI_HURT (sh_src/player.c), and that animation's frame index is
 * ALREADY published every tick, for drawing -- watching it enter ANI_HURT's
 * range is a hit event with zero new wire cost. rings_lost_tick() below is
 * the one place that inference happens (see its own comment for the exact
 * edge-detection this needs, given ANI_HURT's frame 0 can be held for
 * several consecutive ticks at speed 1). Once the edge fires, this file
 * already owns everything Ring_LoseRings itself needs -- ringPlayerCount
 * (the count) and Sonic's just-published world position (the origin) -- so
 * no cross-CPU data beyond the existing published frame index is required
 * at all, only the inference of WHEN to look.
 *
 * Deviation 2, the tier cap: Ring_LoseRings scatters up to 3 tiers (inner
 * 0-16, outer 16-32, "big ring" 32-48, Ring.c:203-205); only the first two
 * are ported here (LOST_RING_CAP=32, rings.h). The third tier uses a
 * distinct, larger, growing-ring visual (RING_TYPE_BIG, Ring_State_LostFX)
 * this port's ring converter (tools/convert_ring.py) has never emitted --
 * adding it is a real new asset, not a code gap, so it is dropped rather
 * than approximated. A hit above 32 rings still zeroes ringPlayerCount
 * (matching the original exactly -- Player_Hit always sets player->rings=0
 * regardless of how many actually got a flying entity), it just does not
 * visually represent rings 33-48 as flying entities the way stock Mania
 * does.
 *
 * Deviation 3, the bounce: Ring_State_Lost bounces off REAL tile collision
 * (RSDK.ObjectTileCollision, Ring.c:608). That data (ghz_collide_index/rows,
 * ghz_map_fgh) is linked only into the slave SH2's own program image --
 * see sh_src/path.c's own comment on why plane 1's tables take a different
 * route than plane 0's precisely because the 68000 cannot reach either one
 * of them at all -- the signpost's own fall (md_src/signpost.c) hit this
 * same wall and used a fixed, scene-authored landing Y instead of a real
 * scan for exactly this reason. Lost rings have no such fixed Y (they
 * scatter from wherever Sonic was hit, not a scene-authored position), so
 * this file approximates: each ring bounces once off the flat Y it was
 * launched from (spawn_lost_ring's floorY), not the real terrain under it.
 * Correct on flat ground (the common case: most hits land Sonic on a
 * floor), visibly wrong on a slope/staircase, and wrong in the specific way
 * of "grounding" partway through their arc if Sonic was hit mid-air (the
 * ring bounces at its OWN spawn height, which is above the real floor in
 * that case, rather than continuing to fall). Flagged, not silently
 * smoothed over; a real fix needs either exposing plane-0 collision to the
 * 68000 or moving this pool's physics to the SH2, both larger changes than
 * this task's own scope. */

typedef struct {
	uint8_t  active;
	uint16_t timer;      /* Ring_State_Lost's self->timer */
	int32_t  x, y;        /* 16.16, matches Ring_LoseRings' position scale */
	int32_t  velX, velY;  /* 16.16 */
	int16_t  floorY;       /* the approximate ground reference above --
	                        * this ring's own launch Y, not real terrain */
} LostRing;

static LostRing lostRings[LOST_RING_CAP];
/* [hurtFirst, hurtFirst+hurtCount) -- sonic_anims[ANI_HURT], read once in
 * rings_init() since the table never changes at runtime. */
static uint16_t hurtFirst, hurtCount;
/* Edge-detection state for the hit inference above: the frame index observed
 * LAST tick. Starts equal to SONIC_FRAME_COUNT (a value ANI_HURT's range
 * never contains, same "not a real frame" sentinel comm.h/sonic.c already
 * use), so a hit landing on frame 0 of a fresh boot -- impossible in
 * practice, since blinkTimer/onGround start clean, but not something to
 * leave to chance -- still reads as a real edge rather than a false "was
 * already hurt last tick" match against an uninitialized 0. */
static uint16_t lastSonicFrameIndex = SONIC_FRAME_COUNT;

/* RSDK::Sin256/Cos256, angle 0-255 over a full turn, result scaled to 256 --
 * exact copy of the slave SH2's own generated table (sh_src/trig.c), needed
 * here only for Ring_LoseRings' angle math (Ring.c:207-246): nothing else on
 * the 68000 side has ever needed trig, so this is the one place carrying a
 * second copy rather than a shared module neither side otherwise wants. */
static const int16_t k_sin256[256] = {
	    0,     6,    13,    19,    25,    31,    38,    44,
	   50,    56,    62,    68,    74,    80,    86,    92,
	   98,   104,   109,   115,   121,   126,   132,   137,
	  142,   147,   152,   157,   162,   167,   172,   177,
	  181,   185,   190,   194,   198,   202,   206,   209,
	  213,   216,   220,   223,   226,   229,   231,   234,
	  237,   239,   241,   243,   245,   247,   248,   250,
	  251,   252,   253,   254,   255,   255,   256,   256,
	  256,   256,   256,   255,   255,   254,   253,   252,
	  251,   250,   248,   247,   245,   243,   241,   239,
	  237,   234,   231,   229,   226,   223,   220,   216,
	  213,   209,   206,   202,   198,   194,   190,   185,
	  181,   177,   172,   167,   162,   157,   152,   147,
	  142,   137,   132,   126,   121,   115,   109,   104,
	   98,    92,    86,    80,    74,    68,    62,    56,
	   50,    44,    38,    31,    25,    19,    13,     6,
	    0,    -6,   -13,   -19,   -25,   -31,   -38,   -44,
	  -50,   -56,   -62,   -68,   -74,   -80,   -86,   -92,
	  -98,  -104,  -109,  -115,  -121,  -126,  -132,  -137,
	 -142,  -147,  -152,  -157,  -162,  -167,  -172,  -177,
	 -181,  -185,  -190,  -194,  -198,  -202,  -206,  -209,
	 -213,  -216,  -220,  -223,  -226,  -229,  -231,  -234,
	 -237,  -239,  -241,  -243,  -245,  -247,  -248,  -250,
	 -251,  -252,  -253,  -254,  -255,  -255,  -256,  -256,
	 -256,  -256,  -256,  -255,  -255,  -254,  -253,  -252,
	 -251,  -250,  -248,  -247,  -245,  -243,  -241,  -239,
	 -237,  -234,  -231,  -229,  -226,  -223,  -220,  -216,
	 -213,  -209,  -206,  -202,  -198,  -194,  -190,  -185,
	 -181,  -177,  -172,  -167,  -162,  -157,  -152,  -147,
	 -142,  -137,  -132,  -126,  -121,  -115,  -109,  -104,
	  -98,   -92,   -86,   -80,   -74,   -68,   -62,   -56,
	  -50,   -44,   -38,   -31,   -25,   -19,   -13,    -6,
};

static const int16_t k_cos256[256] = {
	  256,   256,   256,   255,   255,   254,   253,   252,
	  251,   250,   248,   247,   245,   243,   241,   239,
	  237,   234,   231,   229,   226,   223,   220,   216,
	  213,   209,   206,   202,   198,   194,   190,   185,
	  181,   177,   172,   167,   162,   157,   152,   147,
	  142,   137,   132,   126,   121,   115,   109,   104,
	   98,    92,    86,    80,    74,    68,    62,    56,
	   50,    44,    38,    31,    25,    19,    13,     6,
	    0,    -6,   -13,   -19,   -25,   -31,   -38,   -44,
	  -50,   -56,   -62,   -68,   -74,   -80,   -86,   -92,
	  -98,  -104,  -109,  -115,  -121,  -126,  -132,  -137,
	 -142,  -147,  -152,  -157,  -162,  -167,  -172,  -177,
	 -181,  -185,  -190,  -194,  -198,  -202,  -206,  -209,
	 -213,  -216,  -220,  -223,  -226,  -229,  -231,  -234,
	 -237,  -239,  -241,  -243,  -245,  -247,  -248,  -250,
	 -251,  -252,  -253,  -254,  -255,  -255,  -256,  -256,
	 -256,  -256,  -256,  -255,  -255,  -254,  -253,  -252,
	 -251,  -250,  -248,  -247,  -245,  -243,  -241,  -239,
	 -237,  -234,  -231,  -229,  -226,  -223,  -220,  -216,
	 -213,  -209,  -206,  -202,  -198,  -194,  -190,  -185,
	 -181,  -177,  -172,  -167,  -162,  -157,  -152,  -147,
	 -142,  -137,  -132,  -126,  -121,  -115,  -109,  -104,
	  -98,   -92,   -86,   -80,   -74,   -68,   -62,   -56,
	  -50,   -44,   -38,   -31,   -25,   -19,   -13,    -6,
	    0,     6,    13,    19,    25,    31,    38,    44,
	   50,    56,    62,    68,    74,    80,    86,    92,
	   98,   104,   109,   115,   121,   126,   132,   137,
	  142,   147,   152,   157,   162,   167,   172,   177,
	  181,   185,   190,   194,   198,   202,   206,   209,
	  213,   216,   220,   223,   226,   229,   231,   234,
	  237,   239,   241,   243,   245,   247,   248,   250,
	  251,   252,   253,   254,   255,   255,   256,   256,
};

/* Ring_Collect's sparkle-spawn loop, Ring.c:155-178. The nominal spawn count
 * is `4 * (type==RING_TYPE_BIG) + 4` == 4 for our always-normal rings
 * (Ring.c:153), but `sparkle->timer = 2 * i++` at the end of the loop body
 * double-increments i: the for-loop's own `++i` runs again right after, so
 * the sequence is i=0 (body), i jumps 0->1->2 (post-increment then the
 * for-loop step), i=2 (body), i jumps 2->3->4, loop test 4<4 fails. Only
 * i=0 and i=2 ever run their body, i.e. exactly 2 sparkles, not 4:
 *   i=0: RSDK.SetSpriteAnimation(..., RING_TYPE_SPARKLE1 + 0%3, ...) = anim 2
 *   i=2: RSDK.SetSpriteAnimation(..., RING_TYPE_SPARKLE1 + 2%3, ...) = anim 4
 * matching rings_update()'s two spawn_sparkle() calls exactly. */
static void spawn_sparkle(int16_t ringX, int16_t ringY, uint8_t animID,
                          uint8_t maxFrameCount, uint8_t timer)
{
    uint16_t slot, oldest, i;

    slot = SPARKLE_POOL_SIZE;
    for (i = 0; i < SPARKLE_POOL_SIZE; i++) {
        if (!sparkles[i].active) { slot = i; break; }
    }
    if (slot == SPARKLE_POOL_SIZE) {
        /* Pool full: drop the oldest rather than the original's unlimited
         * entity list. Cosmetic (a missed sparkle flash), and rare -- the
         * pool is sized for 8 simultaneous ring collections (rings.h). */
        oldest = 0;
        for (i = 1; i < SPARKLE_POOL_SIZE; i++) {
            if (sparkles[i].age < sparkles[oldest].age) oldest = i;
        }
        slot = oldest;
        sparkleDropCount++;
    }

    /* Ring.c:156-157: x/y offset drawn before the entity's other fields are
     * set, in that order -- matched here so the shared RNG stream advances
     * identically in sequence (see rings_rand's own deviation note). */
    sparkles[slot].worldX = ringX + (int16_t)rings_rand(-8, 8);
    sparkles[slot].worldY = ringY + (int16_t)rings_rand(-8, 8);
    sparkles[slot].active = 1;
    sparkles[slot].animID = animID;
    sparkles[slot].maxFrameCount = maxFrameCount;
    sparkles[slot].frameID = 0;
    sparkles[slot].animTimer = 0;
    sparkles[slot].timer = timer;
    sparkles[slot].speed = (uint8_t)rings_rand(6, 8);   /* Ring.c:176 */
    sparkles[slot].age = ++sparkleClock;
}

/* Player_CheckCollisionTouch (Player.c:2255-2266) -> RSDK.
 * CheckObjectCollisionTouchBox -> the engine's CheckObjectCollisionTouch
 * (dependencies/RSDKv5/RSDKv5/RSDK/Scene/Collision.cpp:209-241).
 *
 * The source flips both hitboxes first when thisEntity->direction (the
 * RING's own direction, not Sonic's) has FLIP_X or FLIP_Y set --
 * Ring_Draw_Normal sets it from `frameID > 8` (Ring.c:781), a purely visual
 * choice made the tick AFTER collision runs, using the PREVIOUS tick's
 * value. Not transcribed: Ring.hitbox {-8,-8,8,8} is symmetric under both
 * flips (a flip of a box centred on 0 is itself), and every generated Sonic
 * hitbox is left/right symmetric too (verified against game/md_src/
 * sonic_data.c: outerLeft == -outerRight in every row), and ring->direction
 * only ever carries FLIP_X, never FLIP_Y -- so the flip transform is
 * provably a no-op on both operands for this game's actual data, not an
 * assumption. Player's own facing is never consulted by this function at
 * all (only thisEntity, the ring, is checked), so it is correctly absent
 * here too.
 *
 * thisEntity = the ring, thisHitbox = Ring.hitbox; otherEntity = Sonic,
 * otherHitbox = Sonic's current-frame hitbox 0. FROM_FIXED (Collision.cpp:
 * 234-237) is a no-op here: this port's positions are already whole pixels
 * (sh_src/comm.h), which is itself a documented sub-pixel (<1px) deviation
 * from the original's 16.16 fixed point, not a new one introduced by this
 * comparison. */
static uint8_t ring_touches_sonic(int16_t rx, int16_t ry,
                                  int16_t sonicX, int16_t sonicY,
                                  int8_t hbLeft, int8_t hbTop,
                                  int8_t hbRight, int8_t hbBottom)
{
    return rx + RING_HITBOX_LEFT   < sonicX + hbRight
        && rx + RING_HITBOX_RIGHT  > sonicX + hbLeft
        && ry + RING_HITBOX_TOP    < sonicY + hbBottom
        && ry + RING_HITBOX_BOTTOM > sonicY + hbTop;
}

/* The one per-object hook (obj_data.h's ObjDecideFn): touch-test this
 * uncollected ring, collect it and spawn its two sparkles on a hit
 * (Ring_Collect, Ring.c:137-178, transcribed exactly as rings_update() used
 * to do inline), else fall through to the shared rotation frame if it is
 * also inside the Y window. entryIndex indexes straight into ghz_ring_xy
 * (RING_X/RING_Y), the same table the generic engine already read ex/ey
 * out of -- kept here too rather than trusting ex/ey alone only because
 * RING_X/RING_Y read the SAME underlying int16 pair the generic engine's
 * own entry_x()/entry_y() do, so this is not a second, possibly-divergent
 * read, just the existing macros. curCamY/curHbLeft.. are this file's own
 * once-per-call stash (see their own comment above); every candidate in the
 * x window gets this call even past RING_SPRITE_CAP visible sprites --
 * obj_generic.c's own comment on obj_type_draw's loop explains why that
 * matters here specifically (Ring_Collect touch-tests every uncollected
 * ring in the x window regardless of whether it is currently drawn). */
static ObjDrawDecision ring_decide(void *st, uint16_t entryIndex, int16_t ex, int16_t ey,
                                   int16_t sonicWorldX, int16_t sonicWorldY,
                                   uint16_t sonicFrameIndex)
{
    ObjDrawDecision d;
    int32_t ylo, yhi;
    (void)st; (void)sonicFrameIndex;

    /* Rings never move after spawn -- see obj_data.h's own ObjDrawDecision
     * comment (Job 2, this task): 0 here makes obj_type_draw()'s new
     * offX/offY application an exact no-op. */
    d.flipH = 0; d.flipV = 0; d.offX = 0; d.offY = 0; d.frame = OBJ_SKIP;

    if (ring_is_collected(entryIndex)) return d;

    if (ring_touches_sonic(ex, ey, sonicWorldX, sonicWorldY,
                           curHbLeft, curHbTop, curHbRight, curHbBottom)) {
        ring_set_collected(entryIndex);
        /* Player_GiveRings, Player.c:926: CLAMP(rings+amount, 0, 999);
         * amount is always 1 for a normal ring (Ring.c:142-147). No score
         * (Ring_Collect never calls Player_GiveScore). */
        if (ringPlayerCount < 999) ringPlayerCount++;
        spawn_sparkle(ex, ey, SPARKLE_ANIM_1, RING_SPARKLE1_MAXFRAME, SPARKLE0_TIMER);
        spawn_sparkle(ex, ey, SPARKLE_ANIM_3, RING_SPARKLE3_MAXFRAME, SPARKLE1_TIMER);
        return d;
    }

    ylo = (int32_t)curCamY - 16;
    yhi = (int32_t)curCamY + SCREEN_HEIGHT + 16;
    if (ey < ylo || ey >= yhi) return d;

    /* ringFrame (0..15, Zone_StaticUpdate's own pace) maps onto this
     * class's own 4 resident frames with >>2 -- always in range. Both
     * poses being simultaneously resident now (no streaming window to lag
     * behind a request) means every ring reads the exact current value,
     * not a "may still be showing the previous step" approximation. */
    d.frame = ringRotLive ? (uint16_t)(ringFrame >> 2) : OBJ_SKIP;
    return d;
}

/* No firstTile parameter any more (contrast the pre-arena rings_init(),
 * which took one and chained its return into springs_init()): rings no
 * longer occupy a permanent slice of a fixed boot-time layout. Two
 * independent arena tenants now, registered in this order -- ringRotArenaDesc's
 * own RING_ROT_TILES=16-tile resident set and ringSparkleArenaDesc's own
 * 8-tile resident set, both via plain obj_arena_boot_load()/obj_arena_boot_done()
 * (same pattern springs.c/signpost.c's own resident sets already use, and
 * what ringSparkleArenaDesc already did before this task) -- both need a
 * synchronous boot-time admission+upload, since GHZ1's very first ring
 * (RING_SPAN_LO) sits inside the first camera view and the normal runtime
 * admission path only ever runs from inside main()'s own per-frame loop,
 * well after this. Lands both at the SAME addresses the old fixed boot-time
 * chain always gave the combined 156-tile block, first-fit against an empty
 * arena, just split into two smaller grants instead of one. */
void rings_init(void)
{
    uint16_t i;
    uint8_t slot;
    uint16_t base;

    for (i = 0; i < RING_ANIM0_FRAMES; i++) {
        ringRotFrames[i].tileOffset = 0;   /* real value set by ring_rot_onBase() */
        ringRotFrames[i].pieceOffset = 0;
        ringRotFrames[i].tileCount = 4;
        ringRotFrames[i].pieceCount = 1;
        ringRotFrames[i].pivotX = -8;
        ringRotFrames[i].pivotY = -8;
        ringRotFrames[i].duration = 0;
    }
    ringPiece.dx = 0;
    ringPiece.dy = 0;
    ringPiece.size = SPRITE_SIZE(2, 2);
    ringPiece.tile = 0;

    for (i = 0; i < sizeof(collected); i++) collected[i] = 0;
    ringPlayerCount = 0;
    ringFrame = 0;
    ringFrameParity = 0;
    ringRandSeed = 1;   /* deviation: fixed seed, see rings_rand()'s comment */
    for (i = 0; i < SPARKLE_POOL_SIZE; i++) sparkles[i].active = 0;
    sparkleClock = 0;
    sparkleDropCount = 0;
    ringRotRegistered = 0;
    ringRotLive = 0;
    ringSparkleLive = 0;

    /* sonic_anims[] is a link-time constant (tools/convert_sonic.py's
     * generated table), so this only ever needs reading once. See this
     * file's own comment on rings_lost_tick for why watching ANI_HURT's
     * range is this port's hit trigger. */
    hurtFirst = sonic_anims[ANI_HURT].first;
    hurtCount = sonic_anims[ANI_HURT].count;
    lastSonicFrameIndex = SONIC_FRAME_COUNT;
    for (i = 0; i < LOST_RING_CAP; i++) lostRings[i].active = 0;

    /* Same "cannot go stale in one file and not another" guard obj_type_init
     * used to run before it ever touched VRAM (obj_generic.c) -- rings.bin's
     * own leading count word has to still match RING_COUNT, or the x-sorted
     * table below is not what every compile-time RING_COUNT reference in
     * this file assumes it is. On a mismatch, rings stay permanently
     * disabled for this run (ringRotRegistered stays 0, rings_enabled()
     * reports it) exactly like before. */
    if (ghz_ring_count != RING_COUNT) return;

    ringRotArenaDesc.entries = (const void *)ghz_ring_xy;
    ringRotArenaDesc.tilePixels = ring_tiles;
    slot = obj_arena_register(&ringRotArenaDesc);
    base = obj_arena_boot_load(slot);
    if (base == 0xFFFF) return;
    vdp_tiles_load(ring_tiles, base, RING_ROT_TILES);
    obj_arena_boot_done(slot);
    ringRotRegistered = 1;

    ringSparkleArenaDesc.entries = (const void *)ghz_ring_xy;
    ringSparkleArenaDesc.tilePixels = ring_tiles + (uint32_t)RING_SPARKLE1_TILE_BASE * 8;
    slot = obj_arena_register(&ringSparkleArenaDesc);
    base = obj_arena_boot_load(slot);
    if (base == 0xFFFF) return;   /* rotation stays live; only the sparkle effect is lost -- see rings_emit_sparkles()'s own gate */
    vdp_tiles_load(ringSparkleArenaDesc.tilePixels, base, ringSparkleArenaDesc.tileCount);
    obj_arena_boot_done(slot);
}

uint16_t rings_collected_count(void)     { return ringPlayerCount; }

void rings_add(uint16_t amount)
{
    uint32_t v = (uint32_t)ringPlayerCount + amount;
    ringPlayerCount = (v > 999) ? 999 : (uint16_t)v;
}

uint8_t  rings_enabled(void)             { return ringRotLive; }
uint16_t rings_sparkle_drop_count(void)  { return sparkleDropCount; }

uint16_t rings_emit_sparkles(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                             uint16_t camX, uint16_t camY)
{
    uint16_t slot, n = 0;

    /* Gated on the SPARKLE arena's own live flag, independent of the
     * rotation window above -- see ringSparkleArenaDesc's own comment for
     * why the two are separate registrations now. In practice they almost
     * always move together (same entries table, near-identical camera
     * range), so this is a defensive distinction more than a commonly-taken
     * branch. */
    if (!ringSparkleLive) return 0;

    for (slot = 0; slot < SPARKLE_POOL_SIZE; slot++) {
        Sparkle *s = &sparkles[slot];
        const uint16_t *durations;
        uint16_t tileBase;
        VDPSprite *out;

        if (!s->active) continue;

        /* Ring_State_Sparkle, Ring.c:757-769: while timer is positive it
         * just decrements, invisible; the tick it reaches (or starts at) 0
         * it animates and is drawn. */
        if (s->timer > 0) {
            s->timer--;
            continue;
        }

        durations = (s->animID == SPARKLE_ANIM_1) ? ring_sparkle1_durations
                                                   : ring_sparkle3_durations;
        tileBase  = (s->animID == SPARKLE_ANIM_1) ? RING_SPARKLE1_TILE_BASE
                                                   : RING_SPARKLE3_TILE_BASE;

        /* RSDK::ProcessAnimation, Animation.cpp:150-176 (the sprite-anim
         * branch: frames != (SpriteFrame*)1). The source's wraparound
         * (`if (frameID >= frameCount) frameID = loopIndex`) is not
         * reachable here and is not transcribed: every converted duration
         * is 8 or 16 (ring_data.c) and speed is always 6 or 7
         * (RSDK.Rand(6,8)), so timer can cross at most one duration per
         * call, meaning frameID advances by at most 1 per call -- it always
         * lands exactly on maxFrameCount, one call after maxFrameCount-1,
         * and gets destroyed below before a second increment could ever be
         * in the same reach as the true (unconverted) frameCount. The
         * `s->frameID < s->maxFrameCount` guard below is this port's
         * stand-in: it bounds the array read without needing the original's
         * separate frameCount constant, and is a no-op given the same
         * invariant. */
        s->animTimer += s->speed;
        while (s->frameID < s->maxFrameCount && s->animTimer > durations[s->frameID]) {
            s->animTimer -= durations[s->frameID];
            s->frameID++;
        }

        if (s->frameID >= s->maxFrameCount) {
            /* Ring_State_Sparkle, Ring.c:768-769: destroyEntity(self). */
            s->active = 0;
            continue;
        }

        if (n >= SPARKLE_POOL_SIZE) continue;   /* list[] always has room; defensive only */
        out = &list[firstIndex + n];
        out->y = (int16_t)(128 + (s->worldY - 8 - (int16_t)camY));
        out->size = SPRITE_SIZE(2, 2);
        out->link = firstLink + n + 1;
        /* Priority 1: the original draws collect sparkles in draw group 8,
         * above the player's 4 and FG High's 6 (Ring_Collect, Ring.c:167;
         * Zone.c:185-187; the FG High layer's drawGroup byte in GHZ
         * Scene1.bin), so the sparkle clears Plane B too. Table position
         * ahead of Sonic's pieces (main.c's build order) handles the
         * sprite-vs-sprite half. Deviation: the original puts a
         * plane-switched player (group 12) back above sparkles; this port's
         * one-bit sprite priority cannot express that, so sparkles draw
         * above Sonic unconditionally. */
        /* ringSparkleBase is the grant for JUST the sparkle portion of
         * ring_tiles (ringSparkleArenaDesc.tilePixels already starts at
         * RING_SPARKLE1_TILE_BASE -- see rings_init()) -- tileBase's own
         * RING_SPARKLE1/3_TILE_BASE value is relative to the WHOLE sheet, so
         * subtract the same base rings_init() offset tilePixels by, to land
         * on this grant's own local numbering instead. */
        out->attr = TILE_ATTR(RING_PAL, 1, 0, 0,
                              (uint16_t)(ringSparkleBase + (tileBase - RING_SPARKLE1_TILE_BASE) + s->frameID * 4));
        out->x = (int16_t)(128 + (s->worldX - 8 - (int16_t)camX));
        n++;
    }
    return n;
}

/* Player_GetHitbox, Player.c:2244-2248 -- see FALLBACK_HITBOX_* above for
 * why the bounds check, rather than a null check, is this port's equivalent
 * trigger. Shared by rings_update() (ring_decide()'s touch test) and
 * rings_lost_tick() (a lost ring's own re-collection touch test) so both
 * read Sonic's current-frame hitbox the same way, rather than one of them
 * trusting the other's once-per-call cache from a different point in
 * main.c's per-frame order (rings_lost_tick runs in the TICK phase, before
 * rings_update's own DRAW-phase cache below is refreshed for this frame --
 * see main.c's OBJ_TYPE_LIST comment on tick-then-draw order). */
static void sonic_hitbox_at(uint16_t sonicFrameIndex,
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

uint16_t rings_update(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                      uint16_t camX, uint16_t camY,
                      int16_t sonicWorldX, int16_t sonicWorldY,
                      uint16_t sonicFrameIndex)
{
    /* Permanently-disabled-for-this-run check only (boot failure) -- NOT
     * "currently resident": ring_decide() already reads ringRotLive to skip
     * drawing (and the collect touch-test does not depend on tile residency
     * at all -- it never did, Ring_Collect's own collision check has
     * nothing to do with what is in VRAM), so no separate liveness gate
     * belongs here. */
    if (!ringRotRegistered) return 0;

    /* Zone.c:92-95 (Zone_StaticUpdate): ringFrame advances every 2nd tick,
     * wraps &0xF. rings_update() is called once per displayed 68000 frame
     * (main.c), the same nominal 60 Hz cadence Zone_StaticUpdate runs at, so
     * "every 2nd tick" becomes "every 2nd call" here. Both resident poses
     * this maps onto (ring_decide()'s own ringFrame>>2) are simultaneously
     * VRAM-resident now -- no per-tick "select which one to stream in"
     * request left to make, unlike the old anim-window mechanism. */
    if (ringFrameParity) ringFrame = (ringFrame + 1) & 0xF;
    ringFrameParity ^= 1;

    /* Stashed once per call for ring_decide() -- see that variable's own
     * comment. */
    sonic_hitbox_at(sonicFrameIndex, &curHbLeft, &curHbTop, &curHbRight, &curHbBottom);
    curCamY = camY;

    return obj_type_draw(&ringType, list, firstIndex, firstLink, RING_SPRITE_CAP,
                         camX, camY, sonicWorldX, sonicWorldY, sonicFrameIndex);
}

/* ---- Lost rings: tick (physics/scatter-trigger) and draw ----------------- */

static uint8_t frame_in_hurt(uint16_t idx)
{
    return idx >= hurtFirst && idx < (uint16_t)(hurtFirst + hurtCount);
}

static void spawn_lost_ring(int16_t x, int16_t y, int32_t velX, int32_t velY)
{
    uint16_t slot = LOST_RING_CAP, i, oldestTimer = 0, oldest = 0;

    for (i = 0; i < LOST_RING_CAP; i++) {
        if (!lostRings[i].active) { slot = i; break; }
    }
    if (slot == LOST_RING_CAP) {
        /* Pool full: drop the one closest to expiry rather than the
         * original's unlimited entity list -- cosmetic, same convention as
         * spawn_sparkle()'s own oldest-eviction above. */
        for (i = 0; i < LOST_RING_CAP; i++) {
            if (lostRings[i].timer >= oldestTimer) { oldestTimer = lostRings[i].timer; oldest = i; }
        }
        slot = oldest;
    }

    lostRings[slot].active = 1;
    lostRings[slot].timer = 0;
    lostRings[slot].x = (int32_t)x << 16;
    lostRings[slot].y = (int32_t)y << 16;
    lostRings[slot].velX = velX;
    lostRings[slot].velY = velY;
    lostRings[slot].floorY = y;
}

/* Ring_LoseRings' inner (Ring.c:203-226) and outer (228-246) tiers -- see
 * this file's own top-of-section comment for why the third "big ring" tier
 * is not ported. hitX/hitY is Sonic's own world position the tick the hurt
 * edge fired (rings_lost_tick's caller), matching Player_Hit calling
 * Ring_LoseRings with player->position still at the hazard touch. */
static void scatter_lost_rings(int16_t hitX, int16_t hitY)
{
    int32_t inner = ringPlayerCount, outer;
    int32_t angleStart, angle;
    int32_t i;

    if (inner > 16) inner = 16;
    outer = (int32_t)ringPlayerCount - 16;
    if (outer < 0) outer = 0;
    if (outer > 16) outer = 16;

    /* Ring.c:207-226 */
    angleStart = 0xC0 - 8 * (inner & ~1);
    angle = (inner & 1) ? angleStart + 8 : angleStart - 8;
    for (i = 0; i < inner; i++) {
        spawn_lost_ring(hitX, hitY,
                        (int32_t)k_cos256[angle & 0xFF] << 9,
                        (int32_t)k_sin256[angle & 0xFF] << 9);
        angle += 0x10;
    }

    /* Ring.c:228-246 */
    angleStart = 0xC0 - 8 * (outer & ~1);
    angle = (outer & 1) ? angleStart + 8 : angleStart - 8;
    for (i = 0; i < outer; i++) {
        spawn_lost_ring(hitX, hitY,
                        (int32_t)k_cos256[angle & 0xFF] << 10,
                        (int32_t)k_sin256[angle & 0xFF] << 10);
        angle += 0x10;
    }

    /* Player_Hit, Player.c:3619: player->rings = 0, regardless of how many
     * of them got a flying entity above. */
    ringPlayerCount = 0;
}

void rings_lost_tick(int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
    uint16_t i;

    /* Boot-failure gate only -- see rings_update()'s own comment on why
     * this is ringRotRegistered, not a transient residency check: a lost
     * ring's own physics/scatter-trigger has nothing to do with which tiles
     * happen to be resident right now. */
    if (!ringRotRegistered) return;

    /* The hit inference: see this file's top-of-section comment. Edge
     * (not level) detection is required -- ANI_HURT's frame 0 (speed 1,
     * see tools/convert_sonic.py's summary) can be the published frame for
     * several consecutive ticks before it advances, so a level check would
     * scatter rings repeatedly for one hit. */
    if (frame_in_hurt(sonicFrameIndex) && !frame_in_hurt(lastSonicFrameIndex))
        scatter_lost_rings(sonicWorldX, sonicWorldY);
    lastSonicFrameIndex = sonicFrameIndex;

    for (i = 0; i < LOST_RING_CAP; i++) {
        LostRing *ring = &lostRings[i];
        int32_t px, py;

        if (!ring->active) continue;

        /* Ring_State_Lost, Ring.c:602-605 */
        ring->velY += 0x1800;
        ring->x += ring->velX;
        ring->y += ring->velY;

        py = ring->y >> 16;
        if (ring->velY > 0 && py + 8 >= ring->floorY) {
            /* Ring.c:608-612, against the approximate flat plane instead of
             * real tile collision -- see this file's top-of-section
             * comment for the exact compromise this is. */
            ring->velY = (ring->velY >> 2) - ring->velY;
            if (ring->velY > -0x10000) ring->velY = -0x10000;
            py = (int32_t)ring->floorY - 8;
            ring->y = py << 16;
        }

        /* Ring.c:620-630: re-collectible past tick 0x3F, expires past 0xFF.
         * No alpha fade (Ring.c:630-631) -- real MD/32X hardware sprites
         * have no per-sprite alpha, so this port's rings (and sparkles, see
         * rings_emit_sparkles) never fade anywhere else either; a lost ring
         * simply pops out of existence at expiry instead. */
        ring->timer++;
        if (ring->timer > 0x3F) {
            px = ring->x >> 16;
            if (ring_touches_sonic((int16_t)px, (int16_t)py, sonicWorldX, sonicWorldY,
                                   curHbLeft, curHbTop, curHbRight, curHbBottom)) {
                if (ringPlayerCount < 999) ringPlayerCount++;
                spawn_sparkle((int16_t)px, (int16_t)py, SPARKLE_ANIM_1, RING_SPARKLE1_MAXFRAME, SPARKLE0_TIMER);
                spawn_sparkle((int16_t)px, (int16_t)py, SPARKLE_ANIM_3, RING_SPARKLE3_MAXFRAME, SPARKLE1_TIMER);
                ring->active = 0;
                continue;
            }
        }

        if (ring->timer > 0xFF) ring->active = 0;
    }
}

uint16_t rings_lost_draw(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                         uint16_t camX, uint16_t camY,
                         int16_t sonicWorldX, int16_t sonicWorldY, uint16_t sonicFrameIndex)
{
    uint16_t i, n = 0;
    int32_t ylo, yhi;
    (void)sonicWorldX; (void)sonicWorldY; (void)sonicFrameIndex;

    /* Unlike rings_lost_tick() above, this DOES read VRAM (the tile index
     * below), so this needs the real, transient residency check -- never
     * draw a rotation frame's tile index while this tenant is not live. */
    if (!ringRotLive) return 0;

    ylo = (int32_t)camY - 16;
    yhi = (int32_t)camY + SCREEN_HEIGHT + 16;

    for (i = 0; i < LOST_RING_CAP; i++) {
        LostRing *ring = &lostRings[i];
        VDPSprite *out;
        int16_t sx, sy;

        if (!ring->active) continue;

        sy = (int16_t)(ring->y >> 16);
        if (sy < ylo || sy >= yhi) continue;
        sx = (int16_t)(ring->x >> 16);

        out = &list[firstIndex + n];
        out->y = (int16_t)(128 + (sy - 8 - (int16_t)camY));
        out->size = SPRITE_SIZE(2, 2);
        out->link = (uint16_t)(firstLink + n + 1);
        /* Priority 0: a lost ring is still a ring -- same low priority as
         * ringType's own descriptor above, not the sparkles' high one.
         * Reads the SAME shared rotation frame every resident ring reads
         * (ringRotFrames[ringFrame>>2] -- guaranteed live, just checked
         * above), the exact current value, same lockstep agreement every
         * other ring already follows. */
        out->attr = TILE_ATTR(RING_PAL, 0, 0, 0, ringRotFrames[ringFrame >> 2].tileOffset);
        out->x = (int16_t)(128 + (sx - 8 - (int16_t)camX));
        n++;
    }
    return n;
}
