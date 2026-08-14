#include "common.h"

// 32X COMM
static volatile uint16_t* const mars_comm0  = (uint16_t*) MARS_COMM0;
static volatile uint16_t* const mars_comm2  = (uint16_t*) MARS_COMM2;
static volatile uint16_t* const mars_comm8  = (uint16_t*) MARS_COMM8;
static volatile uint32_t* const mars_comm12 = (uint32_t*) MARS_COMM12;

// VDP
static volatile uint16_t* const vdp_data_port = (uint16_t*) VDP_DATA_PORT;
static volatile uint16_t* const vdp_ctrl_port = (uint16_t*) VDP_CTRL_PORT;
static volatile uint32_t* const vdp_ctrl_wide = (uint32_t*) VDP_CTRL_PORT;

// External functions
extern uint16_t read_joypad(uint8_t player);

uint32_t timer = 0;
uint16_t vramOffset = 0;

/* Genesis VDP scrolling background.
 *
 * The whole point of this mode: the MD VDP scrolls a tilemap in hardware at
 * zero SH-2 cost, so the 32X layer only has to draw sprites over it. Plane B
 * holds the stats text, Plane A is free. Font tiles occupy 0..37, so the
 * background tiles go well clear of them at VRAM 0x0800. */
#define PLANE_A_ADDR   0xC000
#define HSCROLL_ADDR   0xFC00
#define BG_TILE_ADDR   0x0800
#define BG_TILE_INDEX  (BG_TILE_ADDR / 32)
#define BG_PALETTE     0x4000       /* name table bits 13-14 select palette 2 */

#define SPR_TILE_ADDR  0x0880
#define SPR_TILE_INDEX (SPR_TILE_ADDR / 32)
/* Per-line HScroll occupies 0xFC00 through 0xFF7F (224 lines x 4 bytes), which
 * runs straight over the default sprite table at 0xFE00. Move the sprites down
 * into the unused space instead. */
#define SPR_ATTR_ADDR  0xF000
#define SPR_PALETTE    0x6000        /* palette 3 */

static uint8_t bgEnabled = 0;
static uint8_t bgReady = 0;
static uint16_t bgScroll = 0;
static uint8_t mdSprites = 0;        /* hardware sprites drawn by the VDP */
static uint8_t sprReady = 0;
static uint8_t parallaxOn = 0;

void vdp_color(uint16_t index, uint16_t color);

/* Tile rows are computed rather than read from a const table. Anything const
 * lands in ROM, and the whole file is deliberately kept off the ROM so the
 * 68000 does not contend with the SH-2s for the cartridge bus. 4bpp, one
 * nibble per pixel. */
__attribute__((section(".data")))
static uint32_t bg_tile_row(uint16_t tile, uint16_t row) {
	if(row == 0) return 0x22222222;         /* top edge line on every tile */
	switch(tile) {
	case 1:  return (row & 1) ? 0x13111311 : 0x31113111;
	case 2:  return (row & 2) ? 0x12111121 : 0x11111111;
	case 3:  return (row & 1) ? 0x13131313 : 0x11111111;
	default: return (row == 7) ? 0x11211121 : 0x11111111;
	}
}

__attribute__((section(".data")))
static void vdp_vram_addr(uint16_t addr) {
	*vdp_ctrl_wide = (((uint32_t)(0x4000 | (addr & 0x3FFF))) << 16)
	               | ((addr >> 14) & 3);
}

__attribute__((section(".data")))
static void vdp_vsram_addr(uint16_t addr) {
	*vdp_ctrl_wide = (((uint32_t)(0x4000 | (addr & 0x3FFF))) << 16)
	               | (((addr >> 14) & 3) | 0x10);
}

__attribute__((section(".data")))
static void bg_init(void) {
	uint16_t t, row;

	/* Palette 2, entries 34 onwards. Entry 33 is already red from startup. */
	vdp_color(33, 0x0240);      /* BGR, overrides the red set at startup */
	vdp_color(34, 0x0620);
	vdp_color(35, 0x0940);
	vdp_color(36, 0x0230);

	vdp_vram_addr(BG_TILE_ADDR);
	for(t = 0; t < 4; t++) {
		for(row = 0; row < 8; row++)
			*((volatile uint32_t*)vdp_data_port) = bg_tile_row(t, row);
	}

	bgReady = 1;
}

/* Fill Plane A with the pattern, or blank it out. Font tile 0 is a space, so
 * writing zero entries clears the plane. */
/* Fill Plane A with the pattern, or blank it out. Font tile 0 is a space, so
 * writing zero entries clears the plane. */
__attribute__((section(".data")))
static void bg_fill(uint8_t on) {
	uint16_t row, cell;

	vdp_vram_addr(PLANE_A_ADDR);
	for(row = 0; row < 32; row++) {
		for(cell = 0; cell < 64; cell++) {
			// Leave the stats rows clear so the Plane B text stays readable
			if(on && row >= 4) {
				uint16_t t2 = ((cell >> 1) + (row >> 1)) & 3;
				*vdp_data_port = BG_PALETTE | (BG_TILE_INDEX + t2);
			} else {
				*vdp_data_port = 0;
			}
		}
	}
}

/* Hardware sprites. The VDP draws up to 80 of these itself, so they cost the
 * SH-2s nothing at all, which is the whole point given the MD layer sits in
 * front of the 32X framebuffer. 16x16 each, two tiles by two. */
__attribute__((section(".data")))
static void spr_init(void) {
	uint16_t t, row;

	*vdp_ctrl_port = 0x8500 | (SPR_ATTR_ADDR / 0x200);   /* reg 5 */
	vdp_color(49, 0x00EE);           /* BGR: yellow */
	vdp_color(50, 0x004E);           /* orange */

	vdp_vram_addr(SPR_TILE_ADDR);
	for(t = 0; t < 4; t++) {
		for(row = 0; row < 8; row++) {
			uint32_t v = 0x22222222;
			if((t < 2 && row == 0) || (t >= 2 && row == 7)) v = 0x11111111;
			*((volatile uint32_t*)vdp_data_port) = v;
		}
	}
	sprReady = 1;
}

/* Rewrite the sprite table. 80 sprites is 320 word writes, comfortably
 * affordable on the 68000 and invisible to the SH-2s. */
__attribute__((section(".data")))
static void spr_update(void) {
	uint16_t i, n = mdSprites;

	vdp_vram_addr(SPR_ATTR_ADDR);
	for(i = 0; i < n; i++) {
		uint16_t x = 24 + ((i * 37 + (bgScroll >> 1)) & 255);
		uint16_t y = 48 + ((i * 61 + (bgScroll >> 2)) & 127);
		*vdp_data_port = y + 128;                       /* Y */
		*vdp_data_port = 0x0500 | ((i + 1) < n ? i + 1 : 0);  /* 2x2, link */
		*vdp_data_port = SPR_PALETTE | SPR_TILE_INDEX;  /* attributes */
		*vdp_data_port = x + 128;                       /* X */
	}
	if(n == 0) {                     /* one offscreen sprite terminates it */
		*vdp_data_port = 0;
		*vdp_data_port = 0;
		*vdp_data_port = 0;
		*vdp_data_port = 0;
	}
}

/* Per-line horizontal scroll: real parallax, one word per line, no CPU cost
 * on the 32X side whatsoever. */
__attribute__((section(".data")))
static void parallax_update(void) {
	uint16_t l;
	uint16_t acc = 0;
	uint16_t delta = bgScroll >> 6;

	vdp_vram_addr(HSCROLL_ADDR);
	for(l = 0; l < 224; l++) {
		*vdp_data_port = -(uint16_t)((bgScroll >> 2) + (acc >> 4));  /* A */
		*vdp_data_port = 0;                                          /* B */
		acc += delta;
	}
}

/* Called once per frame. Hardware scroll, so this is the entire per-frame
 * cost of the background: two word writes. */
__attribute__((section(".data")))
static void bg_scroll_update(void) {
	bgScroll++;
	vdp_vram_addr(HSCROLL_ADDR);
	*vdp_data_port = -(bgScroll >> 1);      /* plane A */
	vdp_vsram_addr(0);
	*vdp_data_port = (bgScroll >> 3) & 0xFF; /* plane A vertical drift */
}

// It is recommended to put functions that run 1+ times every frame into RAM
// by specifying this attribute before the signature. This keeps the M68K off
// the ROM so the SH-2s can access it without slowdown.
// It should be safe to add or remove it from any function and experiment with
// the speed vs space differences

__attribute__((section(".data")))
void vdp_color(uint16_t index, uint16_t color) {
	index <<= 1;
	*vdp_ctrl_wide = ((0xC000 + (((uint32_t)index) & 0x3FFF)) << 16) + (((uint32_t)index) >> 14);
	*vdp_data_port = color;
}

__attribute__((section(".data")))
void do_commands(void) {
	uint16_t cmd = *mars_comm0;
	switch(cmd >> 8) {
	default: break; // Unknown command
	case 0: return; // No command
	case 3:
		*mars_comm8 = read_joypad(cmd);
		break;
	case 4:
		break;
	case 5: // Set VRAM or Plane offset
		vramOffset = *mars_comm2;
		break;
	case 6: // Write tile to Plane B
		*vdp_ctrl_wide = (((uint32_t)0x6000 + ((vramOffset) & 0x3FFF)) << 16) + (((vramOffset) >> 14) | 0x03);
		*vdp_data_port = *mars_comm2;
		vramOffset += 2;
		break;
	case 7: // Write word to VRAM address
		*vdp_ctrl_wide = (((uint32_t)0x4000 + ((vramOffset) & 0x3FFF)) << 16) + (((vramOffset) >> 14) | 0x00);
		*vdp_data_port = *mars_comm2;
		vramOffset += 2;
		break;
	case 9: // Number of hardware sprites for the VDP to draw
		if(!sprReady) spr_init();
		mdSprites = *mars_comm2 > 80 ? 80 : (uint8_t)*mars_comm2;
		spr_update();
		break;
	case 10: // Per-line horizontal scroll on Plane A
		parallaxOn = *mars_comm2 ? 1 : 0;
		// Mode register 3: 0x03 = per-line HSCROLL, 0x00 = whole screen
		*vdp_ctrl_port = 0x8B00 | (parallaxOn ? 0x03 : 0x00);
		break;
	case 8: // Enable or disable the MD scrolling background on Plane A
		if(*mars_comm2) {
			if(!bgReady) bg_init();
			bg_fill(1);
			bgEnabled = 1;
		} else {
			bgEnabled = 0;
			bg_fill(0);
			vdp_vram_addr(HSCROLL_ADDR);
			*vdp_data_port = 0;
			vdp_vsram_addr(0);
			*vdp_data_port = 0;
		}
		break;
	}
	*mars_comm0 = 0;
}

const uint16_t color_cycle[10] = { 0xEEE, 0xCCC, 0xAAA, 0x888, 0x666, 0x444, 0x666, 0x888, 0xAAA, 0xCCC };

/* Nothing here makes sound, so put the audio hardware in a known quiet state.
 * Commercial games do this as a matter of course; the marsdev skeleton never
 * did, which is why it crackles. Left alone the PSG powers up audible, the
 * YM2612 comes up with undefined register contents, and the Z80 runs loose
 * out of uninitialized RAM writing garbage into both. */

__attribute__((section(".data")))
static void ym_write(uint8_t part, uint8_t reg, uint8_t val) {
	volatile uint8_t *ym = (volatile uint8_t *)0xA04000;
	while(ym[0] & 0x80) ;            /* wait for not-busy */
	ym[part ? 2 : 0] = reg;
	while(ym[0] & 0x80) ;
	ym[part ? 3 : 1] = val;
}

__attribute__((section(".data")))
static void audio_silence(void) {
	volatile uint8_t *psg = (volatile uint8_t *)0xC00011;
	volatile uint16_t *z80_reset = (volatile uint16_t *)Z80_RESET;
	volatile uint16_t *z80_busreq = (volatile uint16_t *)Z80_BUS_REQ;
	uint8_t part, op, ch;

	/* Attenuation 15 (silent) on all four PSG channels */
	psg[0] = 0x9F;
	psg[0] = 0xBF;
	psg[0] = 0xDF;
	psg[0] = 0xFF;

	/* Take the Z80 bus so the 68000 can reach the YM2612. Bit 0 reads back
	 * set while the Z80 still holds it; bit 8 is the request we just wrote
	 * and never clears, so waiting on that hangs the 68000. */
	*z80_busreq = 0x100;
	while(*z80_busreq & 0x0001) ;

	ym_write(0, 0x22, 0x00);         /* LFO off */
	ym_write(0, 0x27, 0x00);         /* normal timer mode */
	ym_write(0, 0x2B, 0x00);         /* DAC off */

	for(part = 0; part < 2; part++) {
		for(ch = 0; ch < 3; ch++) {
			for(op = 0; op < 4; op++)
				ym_write(part, 0x40 + (op << 2) + ch, 0x7F);  /* max attenuation */
		}
	}

	/* Key off every operator on all six channels */
	for(ch = 0; ch < 3; ch++) {
		ym_write(0, 0x28, ch);
		ym_write(0, 0x28, 4 + ch);
	}

	/* Then hold the Z80 in reset so it cannot undo any of it */
	*z80_reset = 0x000;
	*z80_busreq = 0x000;
}

__attribute__((section(".data")))
void main(void) {
	uint16_t ticks = 0, col = 0;

	audio_silence();
	while(1) {
		// Cycle the backdrop while idle. With the background plane on, hold a
		// steady dark colour instead: it doubles as confirmation that the
		// enable command actually reached the 68000.
		if(++ticks >= 8) {
			ticks = 0;
			if(++col >= 10) col = 0;
		}
		vdp_color(0, bgEnabled ? 0x0400 : color_cycle[col]);
		// TODO: Remove this after fixing _vblank
		while(*vdp_ctrl_port & 8) do_commands();
		while(!(*vdp_ctrl_port & 8)) do_commands();
		if(bgEnabled) {
			if(parallaxOn) parallax_update();
			else bg_scroll_update();
		}
		if(mdSprites) { bgScroll++; spr_update(); }
		*mars_comm12 = ++timer;
	}
}
