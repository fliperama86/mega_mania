/* 32X master SH-2.
 *
 * Brings up the VDP in packed pixel mode and fills the framebuffer with a
 * drifting vertical colour gradient: unmistakable and moving, so a static
 * screen is distinguishable from a working one. This stands in for the
 * parallax layer main.c used to draw into Plane B on the Mega Drive side
 * (see draw_background in md_src/main.c and docs/hardware-budget.md,
 * section 3, "Layer order: the MD draws in front").
 *
 * Hw32xInit never sets MARS_VDP_PRIO_32X, so the priority bit stays at its
 * power-on default and the Mega Drive VDP composites in front of this
 * layer, same as ghzview always intended Sonic and the foreground to sit.
 * The slave SH-2 stays idle; there is nothing to share with it yet. */

#include "mars.h"

#define FB_LINE_WORDS 160   /* 320 px, 8bpp packed pixel, 2 px per word */
#define FB_DATA_WORD  0x100 /* pixel data starts after the line offset table */

int m_main(void)
{
	uint16_t offset = 0;

	Hw32xInit(MARS_VDP_MODE_256, 0);

	/* A ramp across the palette for the gradient to draw from. Index 0 is
	 * left alone (black, set by Hw32xInit): framebuffer pixel value 0 is
	 * transparent and would show through to whatever the MD is drawing, so
	 * the per-line colour below is never allowed to land on it. */
	{
		volatile uint16_t *pal = &MARS_CRAM;
		uint16_t i;
		for (i = 1; i < 256; i++)
			pal[i] = COLOR(i & 0x1F, (i >> 3) & 0x1F, (~i) & 0x1F);
	}

	for (;;) {
		volatile uint16_t *dst = (&MARS_FRAMEBUFFER) + FB_DATA_WORD;
		uint16_t y;

		for (y = 0; y < 224; y++) {
			uint8_t c = (uint8_t)(y + offset);
			uint16_t v, x;
			if (c == 0) c = 1;   /* never transparent, see the palette note above */
			v = ((uint16_t)c << 8) | c;
			for (x = 0; x < FB_LINE_WORDS; x++) dst[x] = v;
			dst += FB_LINE_WORDS;
		}

		Hw32xScreenFlip(1);
		offset++;
	}

	return 0;
}
