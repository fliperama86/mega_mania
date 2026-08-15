/* Slave SH2: pad input, physics, collision, camera and animation. The
 * master SH2 (m_main.c) keeps drawing its gradient, untouched; the 68000
 * keeps 100% of the VDP work and now only draws from what this file
 * publishes over the comm protocol (see comm.h). */

#include <stdint.h>
#include "player.h"
#include "camera.h"
#include "assets.h"
#include "comm.h"

/* Map size (g_map_w/g_map_h, from assets.h) comes from the descriptor the
 * 68000 publishes at boot, not a local #define: update_scroll's map-
 * clamping math moved to this CPU with the camera, and a second copy of
 * md_src/main.c's dimensions here is exactly what used to let this go stale
 * against the converted data (see md_src/descriptor.h's GHZ_MAP_W comment). */
#define VIEW_BLOCKS_X 32
#define SCREEN_HALF_W 160

/* Act 1's own numbers, not the layer's. The scene places the player here, and
 * GHZSetup overrides the camera's left and bottom bounds for this act rather
 * than letting them default to the layer size (GHZSetup.c in the RSDKv5
 * decompilation, and Zone.c where the defaults it replaces are set). Clamping
 * to the layer instead, which is what this did, lets the view sink far below
 * where the act ever meant it to go. 1412 is also the act's death boundary,
 * for whenever falling stops being survivable. */
#define PLAYER_SPAWN_X 108
#define PLAYER_SPAWN_Y 947
#define CAM_BOUND_B    1412u
#define CAM_BOUND_L    (256u - SCREEN_HALF_W)

void s_main(void)
{
	Player sonic;
	Camera cam;
	uint32_t screenCenterY = assets_init();

	/* Where the scene actually puts the player, read out of Scene1.bin's
	 * object list rather than eyeballed off the tilemap. */
	player_init(&sonic, PLAYER_SPAWN_X, PLAYER_SPAWN_Y);
	camera_init(&cam, sonic.e.x, sonic.e.y);

	for (;;) {
		uint16_t pad = comm_wait_tick();
		int32_t x, y;
		uint16_t limitX, limitY;
		uint16_t camX, camY;
		int16_t worldX, worldY;

		player_update(&sonic, pad);

		/* Zone_HandlePlayerBounds's Left/Right/Bottom (Zone.c ~558-640): the
		 * bound values are this act's, already CAM_BOUND_L/CAM_BOUND_B above
		 * (the original sets its player bounds from the same camera bounds
		 * at stage load, so this is not a second copy of those numbers). The
		 * right bound is the layer width in pixels: g_map_w is in 16-pixel
		 * collision blocks, same conversion update_scroll's limitX uses
		 * below. */
		player_apply_world_bounds(&sonic,
		                           (int32_t)CAM_BOUND_L << 16,
		                           (int32_t)(g_map_w * 16u) << 16,
		                           (int32_t)CAM_BOUND_B << 16);

		if (sonic.justJumped) camera_open_y_offset(&cam);
		camera_update(&cam, sonic.e.x, sonic.e.y, sonic.camAdjustY);

		/* update_scroll, ported from the single-CPU md_src/main.c this
		 * replaced: turns the camera's 16.16 world position into the
		 * screen's top-left corner and clamps it to the map. camera.c only
		 * knows about the target it is following, not the map's size, so
		 * this clamping still belongs at this level, just on this CPU now. */
		x = (cam.x >> 16) - SCREEN_HALF_W;
		y = (cam.y >> 16) - (int32_t)screenCenterY;
		limitX = (g_map_w - VIEW_BLOCKS_X) * 16u;
		limitY = CAM_BOUND_B - 224u;

		if (x < (int32_t)CAM_BOUND_L) x = (int32_t)CAM_BOUND_L;
		if (x > (int32_t)limitX) x = (int32_t)limitX;
		if (y < 0) y = 0;
		if (y > (int32_t)limitY) y = (int32_t)limitY;

		camX = (uint16_t)x;
		camY = (uint16_t)y;

		worldX = (int16_t)(sonic.e.x >> 16);
		worldY = (int16_t)(sonic.e.y >> 16);

		comm_publish_frame(camX, camY, worldX, worldY,
		                   sonic_anim_frame_index(&sonic.animator), sonic.direction);
	}
}
