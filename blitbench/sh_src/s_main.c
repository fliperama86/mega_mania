/* 32X blit budget benchmark, slave SH-2.
 *
 * Takes the sprite count and frame number through the comm registers and
 * derives positions with the same function the master uses, so nothing is
 * shared through SDRAM and no cache flushing is needed on the hot path.
 *
 * Two workloads. Normally the slave draws the odd sprite indices while the
 * master draws the even ones. With SLAVE_SDRAM set it does the same amount
 * of work into an SDRAM scratch buffer instead, never touching the
 * framebuffer, which isolates how much of the contention is specific to
 * framebuffer writes.
 */

#include "mars.h"
#include "bench.h"

void s_main(void)
{
	/* Purge, two way mode, enable. The master built the sprite and
	 * background before releasing us, so cached reads of them are safe. */
	CacheControl(SH2_CCTL_CP | SH2_CCTL_TW | SH2_CCTL_CE);

	MARS_SYS_COMM4 = SLAVE_READY;
	while (MARS_SYS_COMM4 == SLAVE_READY) ;

	for (;;) {
		uint16_t cmd = MARS_SYS_COMM4;

		if (cmd & SLAVE_BUSY) {
			uint32_t frame = MARS_SYS_COMM6;
			int n = cmd & SLAVE_COUNT;
			int i;

			if (cmd & SLAVE_SDRAM) {
				/* Same work, SDRAM destination, no pixels */
				for (i = 1; i < n; i += 2)
					blit_scratch(i);
			} else if (cmd & SLAVE_VF) {
				int sx, sy, px, py;
				int prevN = MARS_SYS_COMM10 & SLAVE_COUNT;

				/* Frames advance by one, so two frames ago is frame - 2 and
				 * its scroll follows from the same function the master uses */
				scroll_pos(frame - 2, &px, &py);
				for (i = 1; i < prevN; i += 2) {
					int x, y;
					sprite_pos(i, frame - 2, &x, &y);
					vf_restore_rect(px + x, py + (y - PLAY_TOP));
				}

				scroll_pos(frame, &sx, &sy);
				for (i = 1; i < n; i += 2) {
					int x, y;
					sprite_pos(i, frame, &x, &y);
					vf_blit_sprite(sx + x, sy + (y - PLAY_TOP));
				}
			} else if (cmd & SLAVE_OPAQ) {
				for (i = 1; i < n; i += 2) {
					int x, y;
					sprite_pos(i, frame, &x, &y);
					if (cmd & SLAVE_LONG) blit_sprite_opaque_long(x & ~3, y);
					else                  blit_sprite_opaque(x, y);
				}
			} else {
				for (i = 1; i < n; i += 2) {
					int x, y;
					sprite_pos(i, frame, &x, &y);
					blit_sprite(x, y);
				}
			}

			MARS_SYS_COMM4 = 0;
		}
	}
}
