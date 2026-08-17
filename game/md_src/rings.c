#include "rings.h"
#include "vdp.h"
#include "sonic_data.h"   /* SONIC_FRAME_COUNT */
#include "ring_data.h"    /* RING_* frame/tile constants, ring_tiles[],
                           * ring_sparkle{1,3}_durations[] -- tools/convert_ring.py */

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
 * table in this codebase. ghz_ring_count is the leading count word;
 * ghz_ring_xy the entries (md_src/assets.s). */
extern const uint16_t ghz_ring_count;
extern const int16_t ghz_ring_xy[];
#define RING_X(i) ghz_ring_xy[(i) * 2]
#define RING_Y(i) ghz_ring_xy[(i) * 2 + 1]

/* assets/sonic/hitbox.bin: animator hitbox 0 ("outer") per Sonic frame, same
 * order as sonic_frames[] -- tools/convert_sonic.py. */
extern const int8_t sonic_hitbox[];

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
static uint16_t ringTileBase;
static uint8_t  ringsLive;
static uint8_t  ringFrame;            /* Zone->ringFrame, Zone.c:92-95 */
static uint8_t  ringFrameParity;      /* "every 2nd tick" -- Zone->timer&1 stand-in */
static uint16_t windowLo, windowHi;   /* sliding window into ghz_ring_xy, [lo,hi) */
static uint32_t ringRandSeed;
static Sparkle  sparkles[SPARKLE_POOL_SIZE];
static uint16_t sparkleClock;         /* increases every spawn */
static uint16_t sparkleDropCount;     /* pool-full evictions, cosmetic deviation */

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

uint16_t rings_init(uint16_t firstTile)
{
    uint16_t i;

    ringTileBase = firstTile;
    ringsLive = ((uint32_t)firstTile + RING_TILE_COUNT <= TILE_FONTINDEX)
             && (ghz_ring_count == RING_COUNT);
    if (ringsLive) {
        vdp_tiles_load(ring_tiles, firstTile, RING_TILE_COUNT);
    }

    for (i = 0; i < sizeof(collected); i++) collected[i] = 0;
    ringPlayerCount = 0;
    ringFrame = 0;
    ringFrameParity = 0;
    windowLo = 0;
    windowHi = 0;
    ringRandSeed = 1;   /* deviation: fixed seed, see rings_rand()'s comment */
    for (i = 0; i < SPARKLE_POOL_SIZE; i++) sparkles[i].active = 0;
    sparkleClock = 0;
    sparkleDropCount = 0;

    return ringsLive ? (uint16_t)(firstTile + RING_TILE_COUNT) : firstTile;
}

uint16_t rings_collected_count(void)     { return ringPlayerCount; }
uint8_t  rings_enabled(void)             { return ringsLive; }
uint16_t rings_sparkle_drop_count(void)  { return sparkleDropCount; }

uint16_t rings_emit_sparkles(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                             uint16_t camX, uint16_t camY)
{
    uint16_t slot, n = 0;

    if (!ringsLive) return 0;

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
        out->attr = TILE_ATTR(RING_PAL, 1, 0, 0, ringTileBase + tileBase + s->frameID * 4);
        out->x = (int16_t)(128 + (s->worldX - 8 - (int16_t)camX));
        n++;
    }
    return n;
}

uint16_t rings_update(VDPSprite *list, uint16_t firstIndex, uint16_t firstLink,
                      uint16_t camX, uint16_t camY,
                      int16_t sonicWorldX, int16_t sonicWorldY,
                      uint16_t sonicFrameIndex)
{
    int32_t xlo, xhi, ylo, yhi;
    uint16_t i, n;
    int8_t hbLeft, hbTop, hbRight, hbBottom;

    if (!ringsLive) return 0;

    /* Zone.c:92-95 (Zone_StaticUpdate): ringFrame advances every 2nd tick,
     * wraps &0xF. rings_update() is called once per displayed 68000 frame
     * (main.c), the same nominal 60 Hz cadence Zone_StaticUpdate runs at, so
     * "every 2nd tick" becomes "every 2nd call" here. */
    if (ringFrameParity) ringFrame = (ringFrame + 1) & 0xF;
    ringFrameParity ^= 1;

    /* Player_GetHitbox, Player.c:2244-2248 -- see FALLBACK_HITBOX_* above
     * for why the bounds check, rather than a null check, is this port's
     * equivalent trigger. */
    if (sonicFrameIndex < SONIC_FRAME_COUNT) {
        const int8_t *hb = &sonic_hitbox[sonicFrameIndex * 4];
        hbLeft = hb[0]; hbTop = hb[1]; hbRight = hb[2]; hbBottom = hb[3];
    } else {
        hbLeft = FALLBACK_HITBOX_LEFT;   hbTop    = FALLBACK_HITBOX_TOP;
        hbRight = FALLBACK_HITBOX_RIGHT; hbBottom = FALLBACK_HITBOX_BOTTOM;
    }

    /* Persistent sliding window [windowLo,windowHi) over ghz_ring_xy, x
     * ascending, correct under backward scrolling (camX can decrease as
     * well as increase). int32_t throughout so camX-16 never wraps the way
     * it would as uint16_t arithmetic when camX < 16. */
    xlo = (int32_t)camX - 16;
    xhi = (int32_t)camX + SCREEN_WIDTH + 16;
    while (windowHi < RING_COUNT && RING_X(windowHi) < xhi) windowHi++;
    while (windowHi > 0 && RING_X(windowHi - 1) >= xhi) windowHi--;
    while (windowLo < RING_COUNT && RING_X(windowLo) < xlo) windowLo++;
    while (windowLo > 0 && RING_X(windowLo - 1) >= xlo) windowLo--;

    ylo = (int32_t)camY - 16;
    yhi = (int32_t)camY + SCREEN_HEIGHT + 16;

    n = 0;
    for (i = windowLo; i < windowHi; i++) {
        int16_t rx, ry;

        if (ring_is_collected(i)) continue;

        rx = RING_X(i);
        ry = RING_Y(i);

        /* Ring_Collect, Ring.c:137: touch-tested every tick regardless of
         * whether the ring is currently drawn -- every uncollected ring in
         * the x window, not only the ones inside the y window below. */
        if (ring_touches_sonic(rx, ry, sonicWorldX, sonicWorldY,
                               hbLeft, hbTop, hbRight, hbBottom)) {
            ring_set_collected(i);
            /* Player_GiveRings, Player.c:926: CLAMP(rings+amount, 0, 999);
             * amount is always 1 for a normal ring (Ring.c:142-147). No
             * score (Ring_Collect never calls Player_GiveScore). */
            if (ringPlayerCount < 999) ringPlayerCount++;
            spawn_sparkle(rx, ry, SPARKLE_ANIM_1, RING_SPARKLE1_MAXFRAME, SPARKLE0_TIMER);
            spawn_sparkle(rx, ry, SPARKLE_ANIM_3, RING_SPARKLE3_MAXFRAME, SPARKLE1_TIMER);
            continue;
        }

        if (ry >= ylo && ry < yhi && n < RING_SPRITE_CAP) {
            VDPSprite *s = &list[firstIndex + n];
            s->y = (int16_t)(128 + (ry - 8 - (int16_t)camY));
            s->size = SPRITE_SIZE(2, 2);
            s->link = firstLink + n + 1;
            /* Priority 0 (low): rings sit below FG High exactly like the
             * original's low object draw group (Ring_Create, Ring.c:35-37 --
             * Zone->objectDrawGroup[0]+1, GHZ's default). Sonic's pieces are
             * earlier in this same link chain, so he draws in front. */
            s->attr = TILE_ATTR(RING_PAL, 0, 0, 0,
                                ringTileBase + RING_ANIM0_TILE_BASE + ringFrame * 4);
            s->x = (int16_t)(128 + (rx - 8 - (int16_t)camX));
            n++;
        }
    }

    return n;
}
