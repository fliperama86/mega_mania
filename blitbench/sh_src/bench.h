#ifndef __BENCH_H__
#define __BENCH_H__

#include "mars.h"

/* Play area sits below the text rows so the MD layer stays readable.
 * Framebuffer pixels with value 0 are transparent and show the MD plane. */
#define PLAY_TOP    32
#define PLAY_HEIGHT (SCREEN_HEIGHT - PLAY_TOP)

#define SPRITE_W     32
#define SPRITE_H     32
#define SPRITE_WORDS (SPRITE_W / 2)

#define MAX_SPRITES 512

/* Framebuffer layout in packed pixel mode: 224 line table entries, then
 * pixel data. Line n starts at word (n * 160 + 0x100), 2 pixels per word. */
#define FB_LINE_WORDS 160
#define FB_DATA_WORD  0x100

/* Virtual playfield for line-table scrolling.
 *
 * In packed pixel mode the first entries of the framebuffer are a table of
 * word offsets, one per scanline. Draw a playfield larger than the screen
 * once, then scrolling is nothing but rewriting that table: 224 word writes
 * a frame instead of repainting 61,440 pixels.
 *
 * 384x256 at 8bpp is 98,304 bytes, plus the table and one blank line, which
 * fits inside a 128 KB bank with room to spare. The slack is how far the view
 * can travel before the buffer has to be redrawn or rebased. */
#define VF_W       384
#define VF_WORDS   (VF_W / 2)
#define VF_H       256
#define VF_SLACK_X (VF_W - SCREEN_WIDTH)      /* 64 px */
#define VF_SLACK_Y (VF_H - PLAY_HEIGHT)       /* 64 lines */
#define VF_BLANK_W (FB_DATA_WORD + VF_WORDS * VF_H)

/* One line of playfield pattern, and the variant used every 32nd line so
 * vertical movement is visible. Restores copy from these, so the cost is a
 * straight memory copy, same as lifting tiles out of a tileset. */
extern uint16_t vfRow[VF_WORDS];
extern uint16_t vfGridRow[VF_WORDS];

/* Background source image, play area sized, 2 pixels per word */
extern uint16_t bgImage[PLAY_HEIGHT * FB_LINE_WORDS];
/* Sprite bitmap, palette index 0 is transparent */
extern uint16_t sprImage[SPRITE_H * SPRITE_WORDS];

/* Slave command word in MARS_SYS_COMM4: busy flag plus total sprite count.
 * MARS_SYS_COMM6 carries the frame counter so both CPUs derive the same
 * positions without sharing memory (which would need cache flushing). */
#define SLAVE_BUSY  0x8000
#define SLAVE_READY 0x4000
#define SLAVE_SDRAM 0x2000   /* work in SDRAM instead of the framebuffer */
#define SLAVE_VF    0x1000   /* draw into the scrolling virtual playfield */
#define SLAVE_OPAQ  0x0800   /* plain framebuffer writes, no transparency */
#define SLAVE_LONG  0x0400   /* ... and longword wide */
#define SLAVE_COUNT 0x03FF   /* sprite count occupies the low ten bits */

/* Scroll position derived from the frame counter, so both CPUs agree without
 * anything being shared. Ping-pongs inside the slack, which exercises the
 * line table without needing the buffer rebased. */
static inline void scroll_pos(uint32_t frame, int *sx, int *sy)
{
	uint32_t t = frame & 255;
	uint32_t u = (frame >> 1) & 255;

	*sx = (int)((t < 128 ? t : 255 - t) >> 1);   /* 0..63 */
	*sy = (int)((u < 128 ? u : 255 - u) >> 1);   /* 0..63 */
}

/* Scratch destination for the SDRAM workload, larger than the 4K cache so
 * the writes genuinely reach memory rather than sitting in a hot line */
#define SCRATCH_SLOTS 8
extern uint16_t scratchBuf[SCRATCH_SLOTS * SPRITE_H * SPRITE_WORDS];

/* Deterministic sprite position for a given index and frame. Kept free of
 * division so it costs nothing next to the blit itself. */
static inline void sprite_pos(int i, uint32_t frame, int *x, int *y)
{
	uint32_t h = (uint32_t)i * 2654435761u;
	int bx = (int)((h >> 8) & 255) + (int)((h >> 7) & 31);
	int by = (int)((h >> 18) & 127) + (int)((h >> 17) & 31);
	int ox = (int)((frame + (uint32_t)i * 7) & 31) - 16;
	int oy = (int)((frame + (uint32_t)i * 13) & 15) - 8;

	bx += ox;
	by += oy;

	if (bx < 0) bx = 0;
	if (bx > SCREEN_WIDTH - SPRITE_W) bx = SCREEN_WIDTH - SPRITE_W;
	if (by < 0) by = 0;
	if (by > PLAY_HEIGHT - SPRITE_H) by = PLAY_HEIGHT - SPRITE_H;

	*x = bx & ~1;             /* even X so writes stay word aligned */
	*y = by + PLAY_TOP;
}

/* Blit one sprite through the overwrite image, where byte writes of 0 are
 * discarded by the hardware. That gives transparency for free. */
static inline void blit_sprite(int x, int y)
{
	volatile uint16_t *dst = (&MARS_OVERWRITE_IMG)
	                       + FB_DATA_WORD + y * FB_LINE_WORDS + (x >> 1);
	const uint16_t *src = sprImage;
	int row;

	for (row = 0; row < SPRITE_H; row++) {
		dst[0]  = src[0];  dst[1]  = src[1];  dst[2]  = src[2];  dst[3]  = src[3];
		dst[4]  = src[4];  dst[5]  = src[5];  dst[6]  = src[6];  dst[7]  = src[7];
		dst[8]  = src[8];  dst[9]  = src[9];  dst[10] = src[10]; dst[11] = src[11];
		dst[12] = src[12]; dst[13] = src[13]; dst[14] = src[14]; dst[15] = src[15];
		src += SPRITE_WORDS;
		dst += FB_LINE_WORDS;
	}
}

/* The same shape of work as blit_sprite, but writing to SDRAM instead of
 * the framebuffer. Identical instruction mix and write count, so comparing
 * the two isolates the destination as the only variable. */
static inline void blit_scratch(int slot)
{
	uint16_t *dst = scratchBuf + (slot & (SCRATCH_SLOTS - 1))
	              * (SPRITE_H * SPRITE_WORDS);
	const uint16_t *src = sprImage;
	int row;

	for (row = 0; row < SPRITE_H; row++) {
		dst[0]  = src[0];  dst[1]  = src[1];  dst[2]  = src[2];  dst[3]  = src[3];
		dst[4]  = src[4];  dst[5]  = src[5];  dst[6]  = src[6];  dst[7]  = src[7];
		dst[8]  = src[8];  dst[9]  = src[9];  dst[10] = src[10]; dst[11] = src[11];
		dst[12] = src[12]; dst[13] = src[13]; dst[14] = src[14]; dst[15] = src[15];
		src += SPRITE_WORDS;
		dst += SPRITE_WORDS;
	}
}

/* Same sprite, same source, same word count, but written to the plain
 * framebuffer address instead of the overwrite image. No transparency, so it
 * is not usable as-is; it exists to price the two destinations against each
 * other. Any gap is the cost of hardware transparency. */
static inline void blit_sprite_opaque(int x, int y)
{
	volatile uint16_t *dst = (&MARS_FRAMEBUFFER)
	                       + FB_DATA_WORD + y * FB_LINE_WORDS + (x >> 1);
	const uint16_t *src = sprImage;
	int row;

	for (row = 0; row < SPRITE_H; row++) {
		dst[0]  = src[0];  dst[1]  = src[1];  dst[2]  = src[2];  dst[3]  = src[3];
		dst[4]  = src[4];  dst[5]  = src[5];  dst[6]  = src[6];  dst[7]  = src[7];
		dst[8]  = src[8];  dst[9]  = src[9];  dst[10] = src[10]; dst[11] = src[11];
		dst[12] = src[12]; dst[13] = src[13]; dst[14] = src[14]; dst[15] = src[15];
		src += SPRITE_WORDS;
		dst += FB_LINE_WORDS;
	}
}

/* Longword variant of the same thing. If a long write costs about what a word
 * write costs, this halves the price outright. Needs x aligned to 4 px. */
static inline void blit_sprite_opaque_long(int x, int y)
{
	volatile uint32_t *dst = (volatile uint32_t *)((&MARS_FRAMEBUFFER)
	                       + FB_DATA_WORD + y * FB_LINE_WORDS + (x >> 1));
	const uint32_t *src = (const uint32_t *)sprImage;
	int row;

	for (row = 0; row < SPRITE_H; row++) {
		dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
		dst[4] = src[4]; dst[5] = src[5]; dst[6] = src[6]; dst[7] = src[7];
		src += SPRITE_WORDS / 2;
		dst += FB_LINE_WORDS / 2;
	}
}

/* Same blit, but into the virtual playfield, which has its own stride */
static inline void vf_blit_sprite(int vx, int vy)
{
	volatile uint16_t *dst = (&MARS_OVERWRITE_IMG)
	                       + FB_DATA_WORD + vy * VF_WORDS + (vx >> 1);
	const uint16_t *src = sprImage;
	int row;

	for (row = 0; row < SPRITE_H; row++) {
		dst[0]  = src[0];  dst[1]  = src[1];  dst[2]  = src[2];  dst[3]  = src[3];
		dst[4]  = src[4];  dst[5]  = src[5];  dst[6]  = src[6];  dst[7]  = src[7];
		dst[8]  = src[8];  dst[9]  = src[9];  dst[10] = src[10]; dst[11] = src[11];
		dst[12] = src[12]; dst[13] = src[13]; dst[14] = src[14]; dst[15] = src[15];
		src += SPRITE_WORDS;
		dst += VF_WORDS;
	}
}

/* Restore playfield under a sprite sized rect, copying from the row pattern */
static inline void vf_restore_rect(int vx, int vy)
{
	volatile uint16_t *dst = (&MARS_FRAMEBUFFER)
	                       + FB_DATA_WORD + vy * VF_WORDS + (vx >> 1);
	int row;

	for (row = 0; row < SPRITE_H; row++) {
		const uint16_t *src = (((vy + row) & 31) == 0 ? vfGridRow : vfRow)
		                    + (vx >> 1);
		dst[0]  = src[0];  dst[1]  = src[1];  dst[2]  = src[2];  dst[3]  = src[3];
		dst[4]  = src[4];  dst[5]  = src[5];  dst[6]  = src[6];  dst[7]  = src[7];
		dst[8]  = src[8];  dst[9]  = src[9];  dst[10] = src[10]; dst[11] = src[11];
		dst[12] = src[12]; dst[13] = src[13]; dst[14] = src[14]; dst[15] = src[15];
		dst += VF_WORDS;
	}
}

/* Clear one sprite sized rect to index 0, which is transparent and shows
 * whatever the Genesis VDP is drawing underneath. Written through the normal
 * framebuffer address, not the overwrite image, so the zeros land. */
static inline void clear_rect(int x, int y)
{
	volatile uint16_t *dst = (&MARS_FRAMEBUFFER)
	                       + FB_DATA_WORD + y * FB_LINE_WORDS + (x >> 1);
	int row;

	for (row = 0; row < SPRITE_H; row++) {
		dst[0]  = 0; dst[1]  = 0; dst[2]  = 0; dst[3]  = 0;
		dst[4]  = 0; dst[5]  = 0; dst[6]  = 0; dst[7]  = 0;
		dst[8]  = 0; dst[9]  = 0; dst[10] = 0; dst[11] = 0;
		dst[12] = 0; dst[13] = 0; dst[14] = 0; dst[15] = 0;
		dst += FB_LINE_WORDS;
	}
}

/* Restore the background under one sprite sized rect. This is the dirty
 * rect technique: cheaper than a full redraw once sprites are sparse. */
static inline void restore_rect(int x, int y)
{
	volatile uint16_t *dst = (&MARS_FRAMEBUFFER)
	                       + FB_DATA_WORD + y * FB_LINE_WORDS + (x >> 1);
	const uint16_t *src = bgImage + (y - PLAY_TOP) * FB_LINE_WORDS + (x >> 1);
	int row;

	for (row = 0; row < SPRITE_H; row++) {
		dst[0]  = src[0];  dst[1]  = src[1];  dst[2]  = src[2];  dst[3]  = src[3];
		dst[4]  = src[4];  dst[5]  = src[5];  dst[6]  = src[6];  dst[7]  = src[7];
		dst[8]  = src[8];  dst[9]  = src[9];  dst[10] = src[10]; dst[11] = src[11];
		dst[12] = src[12]; dst[13] = src[13]; dst[14] = src[14]; dst[15] = src[15];
		src += FB_LINE_WORDS;
		dst += FB_LINE_WORDS;
	}
}

#endif
