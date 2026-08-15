/* Slave SH2: pad input, physics, collision, camera and animation. The
 * master SH2 (m_main.c) keeps drawing its gradient, untouched; the 68000
 * keeps 100% of the VDP work and now only draws from what this file
 * publishes over the comm protocol (see comm.h). */

#include <stdint.h>
#include "player.h"
#include "camera.h"
#include "assets.h"
#include "comm.h"

/* Must match md_src/main.c's stage dimensions exactly: the same kind of
 * local duplication path.c already does for MAP_W/MAP_H, now needed here
 * too since update_scroll's map-clamping math moved with the camera. */
#define MAP_W 256
#define MAP_H 128
#define VIEW_BLOCKS_X 32
#define SCREEN_HALF_W 160

void s_main(void)
{
	Player sonic;
	Camera cam;
	uint32_t screenCenterY = assets_init();

	player_init(&sonic, 80, 848);   /* ground at the act start sits near row 53 */
	camera_init(&cam, sonic.e.x, sonic.e.y);

	for (;;) {
		uint16_t pad = comm_wait_tick();
		int32_t x, y;
		uint16_t limitX, limitY;
		uint16_t camX, camY;
		int16_t worldX, worldY;

		player_update(&sonic, pad);
		if (sonic.justJumped) camera_open_y_offset(&cam);
		camera_update(&cam, sonic.e.x, sonic.e.y);

		/* update_scroll, ported from the single-CPU md_src/main.c this
		 * replaced: turns the camera's 16.16 world position into the
		 * screen's top-left corner and clamps it to the map. camera.c only
		 * knows about the target it is following, not the map's size, so
		 * this clamping still belongs at this level, just on this CPU now. */
		x = (cam.x >> 16) - SCREEN_HALF_W;
		y = (cam.y >> 16) - (int32_t)screenCenterY;
		limitX = (MAP_W - VIEW_BLOCKS_X) * 16u;
		limitY = MAP_H * 16u - 224u;

		if (x < 0) x = 0;
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
