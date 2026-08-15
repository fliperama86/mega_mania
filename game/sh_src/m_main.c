/* 32X master SH-2.
 *
 * Brings up the VDP in packed pixel mode and draws Green Hill's parallax
 * background (bg.c) on the 32X framebuffer: line-table scrolling, following
 * blitbench/sh_src/m_main.c's proven vf_* technique rather than inventing a
 * second way to drive the framebuffer. This replaces the drifting colour
 * gradient placeholder that used to stand in for the parallax layer main.c
 * drew into Plane B on the Mega Drive side (see draw_background in
 * md_src/main.c and docs/hardware-budget.md, section 3, "Layer order: the
 * MD draws in front").
 *
 * Hw32xInit never sets MARS_VDP_PRIO_32X, so the priority bit stays at its
 * power-on default and the Mega Drive VDP composites in front of this
 * layer, same as the game always intended Sonic and the foreground to sit.
 * The slave SH-2 runs the game (s_main.c); this CPU only ever reads its
 * published camera X (comm.h's COMM2) back out. */

#include "mars.h"
#include "bg.h"

int m_main(void)
{
	/* Read before Hw32xInit's own multi-millisecond framebuffer clear,
	 * to leave as little of a window as possible for the race with the
	 * slave's own descriptor read -- see bg.h. */
	bg_assets_init();

	Hw32xInit(MARS_VDP_MODE_256, 0);
	bg_init();

	for (;;) {
		bg_frame();
		Hw32xScreenFlip(1);
	}

	return 0;
}
