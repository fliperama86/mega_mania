/* Slave SH2: pad input, physics, collision, camera and animation. The
 * master SH2 (m_main.c) keeps drawing its gradient, untouched; the 68000
 * keeps 100% of the VDP work and now only draws from what this file
 * publishes over the comm protocol (see comm.h). */

#include <stdint.h>
#include "player.h"
#include "camera.h"
#include "bounds.h"
#include "assets.h"
#include "comm.h"

/* Map size (g_map_w/g_map_h, from assets.h) comes from the descriptor the
 * 68000 publishes at boot, not a local #define: update_scroll's map-
 * clamping math moved to this CPU with the camera, and a second copy of
 * md_src/main.c's dimensions here is exactly what used to let this go stale
 * against the converted data (see md_src/descriptor.h's GHZ_MAP_W comment). */
#define VIEW_BLOCKS_X 32
#define SCREEN_HALF_W 160

/* This port's only screen height (see camera.c's VBOUNDS_SCREEN_H, which
 * must agree with this by hand: nothing here shares a single #define with
 * that file, the same manual-sync convention assets.h already uses for
 * descriptor.h's field list). */
#define SCREEN_H 224

/* Act 1's own numbers, not the layer's: read out of Scene1.bin's object
 * list rather than eyeballed off the tilemap. 1412 (bottom) and 256 (left
 * edge, minus half-screen) are NOT these, despite looking like plausible
 * act bounds: those come from GHZSetup_HandleActTransition (GHZSetup.c:
 * 146-151), which only ever runs for Act 2, gated by "if (Zone->actID)" at
 * GHZSetup.c:57. Act 1's real bounds are bounds.c's job: the FG Low layer
 * size by default (Zone_StageLoad, Zone.c:221-235), narrowed by the scene's
 * BoundsMarker entities long before the player ever reaches this act's true
 * edges. */
#define PLAYER_SPAWN_X 108
#define PLAYER_SPAWN_Y 947

void s_main(void)
{
	Player sonic;
	Camera cam;
	ZoneBounds zb;
	uint32_t screenCenterY = assets_init();
	int32_t prevScreenTop;

	/* Where the scene actually puts the player, read out of Scene1.bin's
	 * object list rather than eyeballed off the tilemap. */
	player_init(&sonic, PLAYER_SPAWN_X, PLAYER_SPAWN_Y);

	/* Zone_StageLoad's defaults plus the scene's BoundsMarker entities'
	 * create-time pass (bounds.c), both ahead of camera_init so the camera
	 * can seed its own bounds from the result - matching stage load order
	 * in the original, where Zone_StageLoad and BoundsMarker_Create both
	 * run before Camera_Create reads Zone->cameraBounds* (Camera.c:73-77). */
	bounds_init(&zb, sonic.e.x, sonic.e.y);
	camera_init(&cam, sonic.e.x, sonic.e.y, &zb);

	/* BoundsMarker_Create's setPos pass (BoundsMarker.c:88-96) leaves the
	 * camera already pinned before the first frame, not centred and easing
	 * in from the full-layer default. camera_apply_vbounds needs a "last
	 * frame" screen top to ease from on its very first call, so seed one
	 * here by doing, once, the same conversion+clamp the loop below does
	 * every frame, against the freshly seeded camera. */
	prevScreenTop = (cam.y >> 16) - (int32_t)screenCenterY;
	if (prevScreenTop < cam.boundsT) prevScreenTop = cam.boundsT;
	if (prevScreenTop > cam.boundsB - SCREEN_H) prevScreenTop = cam.boundsB - SCREEN_H;

	for (;;) {
		uint16_t pad = comm_wait_tick();
		int32_t x, y, limitY;
		uint16_t limitX;
		uint16_t camX, camY;
		int16_t worldX, worldY;

		player_update(&sonic, pad);

		/* BoundsMarker_Update (BoundsMarker.c:12-20): every marker in the
		 * scene re-checked against the player's current position, in scene
		 * slot order (bounds.c's table). This port skips the original's
		 * ACTIVE_XBOUNDS on-screen gating (RSDK only runs Update on entities
		 * within range of a screen): a marker only ever acts within
		 * halfWidth of the player regardless of whether it is on-screen, and
		 * the camera never strays from the player by more than its own
		 * follow lag, well inside the original's update margin, so running
		 * every marker every frame instead of only the on-screen ones cannot
		 * change which end up active. */
		bounds_apply_markers(&zb, sonic.e.x, sonic.e.y);

		/* Zone_HandlePlayerBounds's Left/Right/Bottom (Zone.c ~558-640): left
		 * is 0 now (Zone_StageLoad's default, Zone.c:222 - not GHZSetup's
		 * Act-2-only 96), right is the layer width in pixels (unchanged),
		 * bottom is whatever bounds_apply_markers just set. Top is not
		 * consumed: Zone_StageLoad leaves playerBoundActiveT off and nothing
		 * in GHZ1 ever turns it on, so zb.playerBoundsT is maintained
		 * (BOUNDSMARKER_BELOW_Y does write it) but never read past bounds.h. */
		player_apply_world_bounds(&sonic, zb.playerBoundsL, zb.playerBoundsR,
		                           zb.playerBoundsB);

		/* Player_Gravity_True/False (Player.c ~6094-6110): the air path
		 * holds the camera's vertical dead zone open on every airborne
		 * frame and the ground states let it close, so onGround is the
		 * whole signal. This covers the jump frame too: the camera write
		 * in Player_Action_Jump (~3306) is the same pair Gravity_True
		 * asserts, which is why no separate jump pulse survives here.
		 * camera_apply_vbounds runs first, matching Camera_State_FollowXY
		 * calling HandleVBounds before the follow math (Camera.c:322-323). */
		camera_apply_vbounds(&cam, &zb, prevScreenTop);
		camera_update(&cam, sonic.e.x, sonic.e.y, sonic.camAdjustY,
		              !sonic.e.onGround);

		/* update_scroll, ported from the single-CPU md_src/main.c this
		 * replaced: turns the camera's 16.16 world position into the
		 * screen's top-left corner and clamps it (Camera_SetCameraBounds,
		 * Camera.c:103-119). The vertical bounds are camera.c's eased
		 * boundsT/boundsB; the horizontal ones are ZoneBounds' plain,
		 * unchanging L/R (camera.h explains why camera.c does not also
		 * track those). */
		x = (cam.x >> 16) - SCREEN_HALF_W;
		y = (cam.y >> 16) - (int32_t)screenCenterY;

		/* Camera.c:109-110. Read from zb rather than a literal 0 so this
		 * line does not quietly stop matching zb.cameraBoundsL if that ever
		 * changes (it is 0 for the whole act today, Zone.c:222). */
		if (x < zb.cameraBoundsL) x = zb.cameraBoundsL;

		/* Deliberate divergence from Camera.c:112-113 (screen->size.x +
		 * screen->position.x > boundsR): the 68000 draws a fixed 32-block
		 * window that must stay inside the map, a hardware constraint the
		 * original never had, and for this layer's width it is stricter
		 * than boundsR - 320 would be. Revisit if that renderer constraint
		 * ever loosens. g_map_w is in 16-pixel collision blocks. */
		limitX = (g_map_w - VIEW_BLOCKS_X) * 16u;
		if (x > (int32_t)limitX) x = (int32_t)limitX;

		/* Camera.c:115-119, the vertical half of Camera_SetCameraBounds,
		 * against the eased bounds camera_apply_vbounds just updated. */
		limitY = cam.boundsB - SCREEN_H;
		if (y < cam.boundsT) y = cam.boundsT;
		if (y > limitY) y = limitY;

		/* Next frame's camera_apply_vbounds call needs this frame's clamped
		 * top, same as the original reading last frame's screen->position.y
		 * (Camera.c:212 and friends). */
		prevScreenTop = y;

		camX = (uint16_t)x;
		camY = (uint16_t)y;

		worldX = (int16_t)(sonic.e.x >> 16);
		worldY = (int16_t)(sonic.e.y >> 16);

		comm_publish_frame(camX, camY, worldX, worldY,
		                   sonic_anim_frame_index(&sonic.animator), sonic.direction);
	}
}
