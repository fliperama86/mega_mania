#include <stdint.h>
#include "bounds.h"
#include "assets.h"

/* BoundsMarker->type (BoundsMarker.c:58,64,72). BOUNDSMARKER_BELOW_Y_ANY
 * (the fourth case, BoundsMarker.c:79-82) is not ported: no GHZ1 marker
 * uses it (see the table below), so it would be dead code here. */
#define BOUNDSMARKER_ANY_Y   0
#define BOUNDSMARKER_ABOVE_Y 1
#define BOUNDSMARKER_BELOW_Y 2

typedef struct {
	int16_t x, y;    /* px, marker->position before RSDK's TO_FIXED */
	int16_t halfWidth; /* px; marker->width post-Create is this << 16
	                    * (BoundsMarker.c:38: self->width << 15 applied to
	                    * the raw *full* width, i.e. this halved value << 16) */
	uint8_t type;
} BoundsMarkerDef;

/* Scene1.bin's BoundsMarker entities (Mania-mode filter; the three
 * filter=4 rows are Encore-only and dropped; vsDisable only matters in
 * competition mode, BoundsMarker.c:33-35). Slot order is application
 * order: both BoundsMarker_Update (BoundsMarker.c:12-20) and the
 * BoundsMarker_Create setPos pass (BoundsMarker.c:28-47) apply markers by
 * walking entity slots ascending, so a later slot's write overwrites an
 * earlier one where both are in range of the player. offset is 0 for every
 * one of these markers, so the offset term in BOUNDSMARKER_ABOVE_Y/BELOW_Y
 * (BoundsMarker.c:65,73) always drops out; apply_marker below omits it. */
static const BoundsMarkerDef k_markers[] = {
	/*    x,    y, halfWidth, type */  /* slot */
	{  6704, 1280,  24, BOUNDSMARKER_ABOVE_Y }, /* 326 */
	{  5276, 2048,  24, BOUNDSMARKER_ANY_Y   }, /* 327 */
	{  5180, 1004,  24, BOUNDSMARKER_ABOVE_Y }, /* 328 */
	{  7888, 1280,  24, BOUNDSMARKER_ABOVE_Y }, /* 329 */
	{  8348, 2048,  24, BOUNDSMARKER_ANY_Y   }, /* 330 */
	{ 11752, 1248,  24, BOUNDSMARKER_ANY_Y   }, /* 331 */
	{ 11288, 1536,  24, BOUNDSMARKER_ANY_Y   }, /* 332 */
	{  9744, 2048,  24, BOUNDSMARKER_ANY_Y   }, /* 333 */
	{ 11560, 1536,  24, BOUNDSMARKER_ANY_Y   }, /* 334 */
	{ 10176, 1540,  24, BOUNDSMARKER_ANY_Y   }, /* 335 */
	{ 13272, 1248,  24, BOUNDSMARKER_ANY_Y   }, /* 398 */
	{ 14212, 1888,  24, BOUNDSMARKER_ANY_Y   }, /* 404 */
	{ 14904, 1888,  24, BOUNDSMARKER_ANY_Y   }, /* 602 */
	{ 15424, 1588,  24, BOUNDSMARKER_ABOVE_Y }, /* 815 */
	{ 15524, 1346,  24, BOUNDSMARKER_BELOW_Y }, /* 990 */
	{   276, 1818,  24, BOUNDSMARKER_ABOVE_Y }, /* 995 */
	{   104, 1724,  64, BOUNDSMARKER_ABOVE_Y }, /* 996 */
	{  3344, 2048,  24, BOUNDSMARKER_ABOVE_Y }, /* 997 */
	{  3312, 1024,  24, BOUNDSMARKER_ABOVE_Y }, /* 998 */
	{   256, 1004, 256, BOUNDSMARKER_ABOVE_Y }, /* 1009 */
	{   112, 1004, 256, BOUNDSMARKER_ABOVE_Y }, /* 1010 */
	{  2832, 1024,  24, BOUNDSMARKER_ABOVE_Y }, /* 1011 */
	{ 14816, 1888,  24, BOUNDSMARKER_ANY_Y   }, /* 1287 */
	{ 14656, 1888,  24, BOUNDSMARKER_ANY_Y   }, /* 1288 */
};
#define MARKER_COUNT (sizeof(k_markers) / sizeof(k_markers[0]))

static int32_t abs32(int32_t v) { return v < 0 ? -v : v; }

/* BoundsMarker_ApplyBounds (BoundsMarker.c:51-98), minus two things this
 * port does not need: the Player_CheckValidState/DebugMode guard (line 55,
 * always true here, one player, no debug mode) and the setPos branch (lines
 * 88-96, copying Zone's camera bounds into the camera entity), which only
 * ever runs once at scene load in the original and is folded into
 * camera_init instead (see camera.c), since bounds_init already calls this
 * function once before the camera exists. */
static void apply_marker(ZoneBounds *z, const BoundsMarkerDef *m,
                         int32_t playerX, int32_t playerY)
{
	int32_t mx = (int32_t)m->x << 16;
	int32_t my = (int32_t)m->y << 16;
	int32_t halfWidth = (int32_t)m->halfWidth << 16;

	if (abs32(mx - playerX) >= halfWidth) return;

	switch (m->type) {
	case BOUNDSMARKER_ANY_Y:
		/* BoundsMarker.c:58-62 */
		z->playerBoundsB = my;
		z->cameraBoundsB = (int32_t)m->y;
		break;

	case BOUNDSMARKER_ABOVE_Y:
		/* BoundsMarker.c:64-69 */
		if (playerY < my) {
			z->playerBoundsB = my;
			z->cameraBoundsB = (int32_t)m->y;
		}
		break;

	case BOUNDSMARKER_BELOW_Y:
		/* BoundsMarker.c:72-76 */
		if (playerY > my) {
			z->playerBoundsT = my;
			z->cameraBoundsT = (int32_t)m->y;
		}
		break;

	default:
		break;
	}
}

void bounds_init(ZoneBounds *z, int32_t playerX, int32_t playerY)
{
	/* Zone_StageLoad (Zone.c:221-235): defaults from the FG Low layer size.
	 * g_map_w/g_map_h are in 16px blocks (assets.h), so *16 converts to the
	 * px scale cameraBounds* uses. */
	z->cameraBoundsL = 0;
	z->cameraBoundsT = 0;
	z->cameraBoundsR = (int32_t)g_map_w * 16;
	z->cameraBoundsB = (int32_t)g_map_h * 16;

	z->playerBoundsL = z->cameraBoundsL << 16;
	z->playerBoundsR = z->cameraBoundsR << 16;
	z->playerBoundsT = z->cameraBoundsT << 16;
	z->playerBoundsB = z->cameraBoundsB << 16;

	/* Zone.c:232, Zone_StageLoad's own deathBoundary assignment: the act's
	 * cameraBoundsB BEFORE any marker ever narrows it, so this must be
	 * captured here, not derived from playerBoundsB/cameraBoundsB later --
	 * both of those keep changing for the rest of the act. */
	z->deathBoundsB = z->cameraBoundsB << 16;

	/* BoundsMarker_Create's non-setPos half (BoundsMarker.c:41-44): every
	 * marker already in the scene applies once against the spawn position
	 * before the first frame, so the act starts pinned by whichever markers
	 * cover the spawn rather than opening on the full-layer default above. */
	bounds_apply_markers(z, playerX, playerY);
}

/* This port skips the original's ACTIVE_XBOUNDS on-screen gating (only
 * entities within range of a screen even run their per-frame Update in
 * RSDK): a marker only ever acts within halfWidth of the player regardless
 * of whether it is on-screen, and the camera never strays from the player
 * by more than its own follow lag, well inside the original's update
 * margin, so evaluating all of k_markers every frame instead of only the
 * on-screen ones cannot change which markers end up active. */
/* NOT windowed by a binary search (2026-08-18 camera-X gating task), unlike
 * most of sh_src's other per-tick scene-table scans -- k_markers above is
 * transcribed by hand in SCENE SLOT order (see this file's own top comment:
 * "later slot wins where two active markers disagree"), not run through
 * tools/convert_objects.py's write_scene_table (the x-sorted generic
 * table writer every OTHER per-tick scan in this batch reads its own table
 * through). Reordering it by x to binary-search would silently change which
 * marker wins when two are simultaneously in range -- a real correctness
 * risk for a table this task's own brief says to leave ungated rather than
 * risk (sort it into work RAM only if the reordering can be proven safe,
 * which "later slot wins" here says it cannot, at least not without also
 * restoring slot order among the surviving candidates, which buys nothing
 * back: apply_marker() below already rejects a non-candidate in one
 * subtract+compare, as cheap as any window check could make it, so there is
 * no meaningful per-entry cost left to cut). 24 entries, all this cheap, is
 * not worth the risk. */
void bounds_apply_markers(ZoneBounds *z, int32_t playerX, int32_t playerY)
{
	uint32_t i;
	for (i = 0; i < MARKER_COUNT; i++)
		apply_marker(z, &k_markers[i], playerX, playerY);
}
