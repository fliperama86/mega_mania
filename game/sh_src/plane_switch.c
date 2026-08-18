#include <stdint.h>
#include "plane_switch.h"
#include "force_spin.h"

/* PlaneSwitch's marker fields, cut down to what this port's table needs.
 * x,y: px, PlaneSwitch->position pre-TO_FIXED. angle: byte-scale,
 * PlaneSwitch->angle (this port recomputes negAngle per call, same as
 * force_spin.c does for ForceSpin, rather than precomputing and storing it
 * the way PlaneSwitch_Create does at self->negAngle). halfLen: px,
 * PlaneSwitch->size*8 -- the "size << 19" term in PlaneSwitch_CheckCollisions'
 * containment check is exactly TO_FIXED(size*8), same convention as
 * force_spin.c's ForceSpinDef.halfLen, so the table stores the
 * already-multiplied pixel value rather than a second "size" column. flags:
 * PlaneSwitch->flags, the raw 4-bit editor value -- bit0/bit2 are the
 * "<" side's plane/priority bits, bit1/bit3 the ">=" side's, tested directly
 * in plane_switch_apply rather than decoded here (see PlaneSwitch.c:94-109).
 * onPath: PlaneSwitch->onPath, gates the whole check off ACTIVE_BOUNDS-style
 * proximity and onto "on a path" -- see plane_switch_apply's gate. */
typedef struct {
	int16_t x, y;
	uint8_t angle;
	uint8_t halfLen;
	uint8_t flags;
	uint8_t onPath;
} PlaneSwitchDef;

/* GHZ Scene1.bin's PlaneSwitch entities, Mania-mode filter (filter=1; the 11
 * filter=4 Encore-only rows are dropped), ascending slot order -- both
 * because PlaneSwitch_Update's foreach_active walks the active list in slot
 * order and because a later slot's write simply overwrites an earlier one
 * for any marker the player is inside both of, same "later slot wins"
 * rationale bounds.c's own table comment gives. 106 rows: two of them (the
 * pair at px (5860,1704), slots 630/631 in the dump) are exact duplicates in
 * the source scene data -- kept as transcribed rather than collapsed, since
 * they are harmless (an identical marker checked twice has no observable
 * effect beyond the redundant check) and dropping one would be editing the
 * scene's own data rather than porting it. */
static const PlaneSwitchDef k_markers[] = {
	/*      x,     y, angle, halfLen, flags, onPath */
	{   5504,   564,   0,  32,  2, 1 }, /* slot 10 */
	{   5652,   616,   0,  96,  2, 0 }, /* slot 11 */
	{   5356,   616,   0,  96,  0, 0 }, /* slot 12 */
	{  11136,  1076,   0,  32,  2, 1 }, /* slot 63 */
	{  11284,  1128,   0,  96,  2, 0 }, /* slot 64 */
	{  10988,  1128,   0,  96,  0, 0 }, /* slot 65 */
	{   6912,  1876,   0,  32,  2, 1 }, /* slot 308 */
	{   7060,  1928,   0,  96,  2, 0 }, /* slot 309 */
	{   6764,  1928,   0,  96,  0, 0 }, /* slot 310 */
	{   8380,   112,   0, 104,  0, 0 }, /* slot 339 */
	{   8528,   104,   0,  32,  2, 1 }, /* slot 340 */
	{   8672,   184,   0, 120,  2, 0 }, /* slot 341 */
	{   5720,  1568,   0,  32,  8, 1 }, /* slot 378 */
	{   6544,  1912,   0, 112,  2, 1 }, /* slot 402 */
	{   9052,  1540,   0,  40,  0, 0 }, /* slot 423 */
	{   8900,  1536,   0,  48,  8, 0 }, /* slot 424 */
	{  15112,   320,   0, 104,  8, 0 }, /* slot 449 */
	{  15240,   320,   0, 104,  2, 0 }, /* slot 450 */
	{  15176,   477,  64,  32,  0, 0 }, /* slot 451 */
	{  14540,   396,   0,  96,  0, 0 }, /* slot 456 */
	{  14820,   396,   0,  96,  2, 0 }, /* slot 457 */
	{   6136,  1868,  64,  48,  0, 0 }, /* slot 562 */
	{   8756,  1856,  64,  48,  0, 0 }, /* slot 563 */
	{  13068,   296,   0,  48,  8, 0 }, /* slot 574 */
	{  13156,   544,  64,  48,  0, 0 }, /* slot 575 */
	{  14672,   344,   0,  32,  2, 1 }, /* slot 581 */
	{   5860,  1704,   0,  32,  0, 0 }, /* slot 630 */
	{   5860,  1704,   0,  32,  0, 0 }, /* slot 631 */
	{  11056,   952, 235, 104,  4, 0 }, /* slot 643 */
	{  11192,   940,  25,  96,  1, 0 }, /* slot 644 */
	{   5816,  1888,   0,  40, 10, 0 }, /* slot 808 */
	{   3352,  1490, 128,  64,  1, 0 }, /* slot 835 */
	{   3440,  1418,  64,  80,  1, 0 }, /* slot 836 */
	{   3512,  1506,   0,  64,  1, 0 }, /* slot 837 */
	{   3424,  1562, 192,  80,  1, 0 }, /* slot 838 */
	{  10460,   304, 128,  80,  1, 0 }, /* slot 839 */
	{  10548,   232,  64,  80,  1, 0 }, /* slot 840 */
	{  10620,   320,   0,  80,  1, 0 }, /* slot 841 */
	{  10532,   392, 192,  80,  1, 0 }, /* slot 842 */
	{  11000,   528,   0, 120,  1, 0 }, /* slot 843 */
	{  10904,   400,  64, 104,  1, 0 }, /* slot 844 */
	{  10792,   512, 128, 120,  1, 0 }, /* slot 845 */
	{  10888,   640, 192, 104,  1, 0 }, /* slot 846 */
	{  15328,   416,  64,  40,  2, 0 }, /* slot 891 */
	{  15320,   780,  64,  48,  0, 0 }, /* slot 893 */
	{  15184,   224,  64,  64,  2, 0 }, /* slot 905 */
	{  13212,   536,   0,  32,  0, 0 }, /* slot 909 */
	{   6812,  1768,   0,  64,  4, 0 }, /* slot 912 */
	{   6952,  1712,   0, 120,  1, 0 }, /* slot 913 */
	{   6952,  1584,   0,   8,  1, 0 }, /* slot 914 */
	{   6952,  1568,   0,   8,  0, 0 }, /* slot 915 */
	{   7088,  1336,   0,  32,  1, 0 }, /* slot 916 */
	{   7008,  1320, 128,  32,  1, 0 }, /* slot 917 */
	{   7040,  1360, 192,  40,  1, 0 }, /* slot 918 */
	{  14768,   996,   0,  40,  2, 0 }, /* slot 921 */
	{  14800,  1284,   0,  40,  0, 0 }, /* slot 922 */
	{  14824,  1220,  64,  32,  0, 0 }, /* slot 923 */
	{  14840,  1332,  64,  48,  0, 0 }, /* slot 924 */
	{   8452,  1844,   0,  80,  0, 0 }, /* slot 926 */
	{   8612,  1984,  64,  80, 10, 0 }, /* slot 927 */
	{   5644,   496,  64,  32,  0, 0 }, /* slot 930 */
	{   7052,  1808,  64,  32,  0, 0 }, /* slot 931 */
	{  11276,  1008,  64,  32,  0, 0 }, /* slot 932 */
	{  14812,   276,  64,  32,  0, 0 }, /* slot 933 */
	{   8664,    40,  64,  32,  0, 0 }, /* slot 934 */
	{   6168,  1900,  64,  16,  0, 0 }, /* slot 942 */
	{   3712,   554, 128,  80,  1, 0 }, /* slot 973 */
	{   3768,   482,  64,  48,  1, 0 }, /* slot 974 */
	{   4240,   594,  64,  64,  1, 0 }, /* slot 975 */
	{   4112,   738, 192,  80,  1, 0 }, /* slot 976 */
	{   4328,   682,   0,  80,  1, 0 }, /* slot 977 */
	{   4312,   594,  64,  24,  1, 0 }, /* slot 978 */
	{   4256,   738, 192,  80,  1, 0 }, /* slot 979 */
	{   3952,   738, 192,  80,  1, 0 }, /* slot 980 */
	{   3792,   684, 171, 104,  1, 0 }, /* slot 981 */
	{   3920,   482,  64, 104,  1, 0 }, /* slot 982 */
	{   4104,   536,  43, 104,  1, 0 }, /* slot 983 */
	{   7936,  1868,   0, 120,  4, 0 }, /* slot 986 */
	{   7936,  1630,   0, 120,  4, 0 }, /* slot 987 */
	{   8240,  1844,   0, 120,  1, 0 }, /* slot 988 */
	{   8244,  1602,   0, 120,  1, 0 }, /* slot 989 */
	{   9152,  1886,   0,  72,  4, 0 }, /* slot 999 */
	{   9296,  1886,   0,  72,  1, 0 }, /* slot 1000 */
	{   9232,  1950,  64,  72,  4, 0 }, /* slot 1001 */
	{   9232,  1806,  64,  72,  1, 0 }, /* slot 1002 */
	{   8624,   320,  64,  72,  0, 0 }, /* slot 1008 */
	{   4328,   594,   0,   8,  0, 0 }, /* slot 1017 */
	{   3712,   482,   0,   8,  0, 0 }, /* slot 1018 */
	{   3352,  1418,  64,   8,  0, 0 }, /* slot 1019 */
	{   3352,  1578,   0,   8,  0, 0 }, /* slot 1020 */
	{   3512,  1578,   0,   8,  0, 0 }, /* slot 1021 */
	{   3528,  1418,  64,   8,  0, 0 }, /* slot 1022 */
	{   7008,  1376,   0,   8,  0, 0 }, /* slot 1023 */
	{   7088,  1376,   0,   8,  0, 0 }, /* slot 1024 */
	{  10792,   656,   0,   8,  0, 0 }, /* slot 1025 */
	{  10792,   400,   0,   8,  0, 0 }, /* slot 1026 */
	{  11000,   656,   0,   8,  0, 0 }, /* slot 1027 */
	{  10620,   232,   0,   8,  0, 0 }, /* slot 1028 */
	{  10620,   408,   0,   8,  0, 0 }, /* slot 1029 */
	{  10460,   408,   0,   8,  0, 0 }, /* slot 1030 */
	{  10460,   232,   0,   8,  0, 0 }, /* slot 1031 */
	{  11122,   858,   0,   8,  0, 0 }, /* slot 1032 */
	{   9296,  1806,   0,   8,  0, 0 }, /* slot 1033 */
	{   9296,  1966,   0,   8,  0, 0 }, /* slot 1034 */
	{   9152,  1966,   0,   8,  0, 0 }, /* slot 1035 */
	{   9152,  1806,   0,   8,  0, 0 }, /* slot 1036 */
};
#define MARKER_COUNT (sizeof(k_markers) / sizeof(k_markers[0]))

#define TO_FIXED(x) ((int32_t)(x) << 16)

static int32_t iabs(int32_t v)
{
	return v < 0 ? -v : v;
}

/* PlaneSwitch_CheckCollisions (PlaneSwitch.c:81-113), player-only
 * (foreach_active(Player, player) only ever visits the one entity this port
 * has, same reasoning force_spin.c's ForceSpin_Update comment gives) and
 * with switchdrawGroup hardwired true: the only call site in the original,
 * PlaneSwitch_Update (PlaneSwitch.c:16-19), always passes true, so the
 * switchdrawGroup==false dead branch is not ported.
 *
 * pivotVel's rotation pivot is (0,0), not "self->velocity" like the
 * original's `Zone_RotateOnPivot(&pivotVel, &self->velocity, self->negAngle)`
 * literally reads: PlaneSwitch is a static scene marker with no motion code
 * anywhere in this object (Create/Update never touch a velocity field), so
 * self->velocity is always (0,0) and rotating around it is exactly rotating
 * around the origin -- same simplification force_spin.c documents for why
 * ForceSpin_Update's own pivotVel is dropped entirely there (there it is
 * unused after computing; here PlaneSwitch_CheckCollisions does use it, in
 * the side test below, so it is computed, just against a fixed zero pivot). */
void plane_switch_apply(Player *p)
{
	uint32_t i;

	for (i = 0; i < MARKER_COUNT; i++) {
		const PlaneSwitchDef *m = &k_markers[i];
		int32_t mx = TO_FIXED(m->x);
		int32_t my = TO_FIXED(m->y);
		int32_t rx, ry, rvx, rvy;
		uint8_t negAngle;

		/* gate: "!self->onPath || other->onGround" (PlaneSwitch.c:91) --
		 * skip only when onPath demands grounded contact and the player
		 * is airborne, same ACTIVE_BOUNDS-replacement rationale bounds.c
		 * documents for why this port evaluates every marker every frame
		 * regardless of on-screen state rather than gating on it. */
		if (m->onPath && !p->e.onGround) continue;

		/* Cheap, EXACT pre-filter, same derivation as force_spin_apply's
		 * own (identical) comment: rotation around (mx,my) preserves the
		 * player's own Euclidean distance from the pivot, so if the
		 * containment test just below (position only, not velocity --
		 * PlaneSwitch.c:92) could ever pass, the RAW (unrotated) axis
		 * distances |p->e.x-mx| and |p->e.y-my| are already individually
		 * below TO_FIXED(24+halfLen) -- this table's own k_markers is
		 * hand-transcribed in scene slot order, same as bounds.c's/
		 * force_spin.c's, so this skips the expensive rotation (BOTH
		 * calls below -- position AND velocity, four trig-table lookups
		 * total, paid unconditionally for all MARKER_COUNT=106 entries
		 * every tick before this change) rather than reordering/windowing
		 * the table itself. */
		if (iabs(p->e.x - mx) >= TO_FIXED(24 + m->halfLen)) continue;
		if (iabs(p->e.y - my) >= TO_FIXED(24 + m->halfLen)) continue;

		rx = p->e.x;
		ry = p->e.y;
		rvx = p->e.velX;
		rvy = p->e.velY;
		negAngle = (uint8_t)(0 - m->angle);   /* PlaneSwitch_Create:43 */

		rotate_on_pivot(&rx, &ry, mx, my, negAngle);
		rotate_on_pivot(&rvx, &rvy, 0, 0, negAngle);

		/* containment (PlaneSwitch.c:92): position only, not velocity */
		if (iabs(rx - mx) >= TO_FIXED(24)) continue;
		if (iabs(ry - my) >= TO_FIXED(m->halfLen)) continue;

		/* side test (PlaneSwitch.c:93-110): rotated position PLUS rotated
		 * velocity, compared against the marker's own (unrotated)
		 * position -- this is what lets a fast-enough player crossing the
		 * line this frame switch a step early, same as the original. */
		if (rx + rvx >= mx) {
			p->e.collisionPlane = (m->flags >> 3) & 1; /* collision plane bit */
			p->drawGroupHigh    = (m->flags >> 2) & 1; /* priority bit */
		} else {
			p->e.collisionPlane = (m->flags >> 1) & 1; /* collision plane bit */
			p->drawGroupHigh    =  m->flags       & 1; /* priority bit */
		}
	}
}
