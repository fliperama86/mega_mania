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

/* ROM/DMA window diagnostic -------------------------------------------
 *
 * Question 1: does VDP DMA sourced from the 32X ROM windows (0x880000
 * fixed, 0x900000 banked) actually work, with a DMA-from-MD-work-RAM case
 * alongside it as a control (work RAM is not on the cartridge bus, so it
 * isolates the DMA engine itself from anything specific to the ROM
 * windows). Question 2: what does the 68000 reading cartridge ROM every
 * frame cost the SH-2s. sh_src/m_main.c drives this from the pad and
 * reports on screen; this side just moves data and reports counts back
 * through the comm registers.
 */

#define TEST_LONGS    128        /* one block, 16 tiles' worth at 4bpp */
#define TEST_WORDS    (TEST_LONGS * 2)
#define TEST_DMA_VRAM 0x1000     /* scratch, clear of every other VRAM user */
#define TEST_CPU_VRAM 0x1800

/* Known, self-checking pattern, deliberately left in ROM: no .data
 * attribute, unlike everything else in this file, because this is exactly
 * what the window tests are supposed to read through the cartridge. Each
 * longword is 0xA5A5 in the top half and its own index in the bottom half,
 * so a wrong source address, a wrapped transfer, or byte-swapped data all
 * show up as an obviously wrong low half.
 *
 * md.ld links this file's ROM region at origin 0x00880000 with no LMA
 * offset, so &romTestPattern is already a valid 0x880000-window address
 * with no cross-CPU address math needed. It lands within the first few KB
 * of md_start.bin, comfortably inside both the 512 KB fixed window and
 * bank 0 of the 900000 window (bank 0 is what the hardware powers up with,
 * and nothing here ever touches the bank register at 0xA15104). */
#define P4(i)  (0xA5A50000u|(uint32_t)(i)),     (0xA5A50000u|(uint32_t)((i)+1)), \
               (0xA5A50000u|(uint32_t)((i)+2)), (0xA5A50000u|(uint32_t)((i)+3))
#define P16(i) P4(i), P4((i)+4), P4((i)+8), P4((i)+12)
#define P64(i) P16(i), P16((i)+16), P16((i)+32), P16((i)+48)
static const uint32_t romTestPattern[TEST_LONGS] = { P64(0), P64(64) };
#undef P4
#undef P16
#undef P64

static uint16_t testMatchCount  = 0;      /* longwords that matched, 0..TEST_LONGS */
static uint16_t testMismatchIdx = 0xFFFF; /* 0xFFFF = no mismatch found */
static uint32_t testMismatchVal = 0;      /* what actually came back, at the first mismatch */

static uint16_t romReadWords = 0;         /* per-frame ROM read volume, in words; 0 = off */

#define ROM_READ_STRIDE  64               /* bytes between successive reads */
#define ROM_READ_WINDOW  0x40000          /* stays well inside the fixed window */

/* Control case: the same pattern, but in MD work RAM rather than on the
 * cartridge. Work RAM is not on the cartridge bus at all, so if DMA from
 * here succeeds while both ROM windows fail, the finding is airtight: the
 * 32X adapter is not serving VDP DMA cycles from the cartridge windows. If
 * this also fails, the DMA setup itself is still wrong somewhere, and the
 * ROM window is exonerated. Ordinary writable .data, unlike romTestPattern:
 * this one is meant to live in RAM, not ROM. */
static uint32_t romTestPatternRAM[TEST_LONGS];

__attribute__((section(".data")))
static void ram_pattern_init(void) {
	uint16_t i;
	for (i = 0; i < TEST_LONGS; i++)
		romTestPatternRAM[i] = 0xA5A50000u | i;
}

/* Set the VRAM address for a VDP read (CD1CD0 = 00, vs 01 for a write) */
__attribute__((section(".data")))
static void vdp_vram_addr_read(uint16_t addr) {
	*vdp_ctrl_wide = (((uint32_t)(addr & 0x3FFF)) << 16) | ((addr >> 14) & 3);
}

/* Kick a Genesis VDP 68k-to-VRAM DMA and wait for it to finish. words is
 * the transfer length in 16-bit words (0 would mean 65536; not used here). */
__attribute__((section(".data")))
static void vdp_dma_from(uint32_t src, uint16_t dest, uint16_t words) {
	*vdp_ctrl_port = 0x9300 | (words & 0xFF);        /* reg 0x13: length low */
	*vdp_ctrl_port = 0x9400 | ((words >> 8) & 0xFF);  /* reg 0x14: length high */
	*vdp_ctrl_port = 0x9500 | ((src >> 1) & 0xFF);    /* reg 0x15: src addr low */
	*vdp_ctrl_port = 0x9600 | ((src >> 9) & 0xFF);    /* reg 0x16: src addr mid */
	*vdp_ctrl_port = 0x9700 | ((src >> 17) & 0x7F);   /* reg 0x17: src addr high, bit7=0 selects 68k source */
	/* CD5 is what actually starts the transfer and it lives in bit 7 of the
	 * SECOND control word, not the first. Putting it in the first word only
	 * sets address bit 7 and no DMA ever runs. */
	*vdp_ctrl_wide = (((uint32_t)(0x4000 | (dest & 0x3FFF))) << 16)
	               | (((dest >> 14) & 3) | 0x80);
	while (*vdp_ctrl_port & 2) ;                      /* bit1 = DMA in progress */
}

/* CPU-write control: read the pattern with ordinary 68000 memory reads and
 * write it to VRAM with ordinary CPU writes, no DMA hardware involved. If
 * this passes and the DMA path does not, the fault is in the DMA engine,
 * not the pattern, the ROM window, the VRAM address, or the readback path. */
__attribute__((section(".data")))
static void cpu_upload_pattern(const uint32_t *src, uint16_t vramAddr) {
	uint16_t i;
	vdp_vram_addr(vramAddr);
	for (i = 0; i < TEST_LONGS; i++)
		*((volatile uint32_t*)vdp_data_port) = src[i];
}

/* Read TEST_LONGS longwords back from VRAM and compare against the known
 * pattern. Scans the whole block regardless of where a mismatch starts, so
 * the match count is a true total rather than just a matching prefix. */
__attribute__((section(".data")))
static void vdp_check_pattern(uint16_t vramAddr) {
	uint16_t i;

	testMatchCount = 0;
	testMismatchIdx = 0xFFFF;
	testMismatchVal = 0;

	vdp_vram_addr_read(vramAddr);
	for (i = 0; i < TEST_LONGS; i++) {
		uint32_t v = *((volatile uint32_t*)vdp_data_port);
		uint32_t expect = 0xA5A50000u | i;
		if (v == expect) {
			testMatchCount++;
		} else if (testMismatchIdx == 0xFFFF) {
			testMismatchIdx = i;
			testMismatchVal = v;
		}
	}
}

/* Question 2: what a tilemap streamer's per-frame ROM reads cost the SH-2s.
 * Samples one word every ROM_READ_STRIDE bytes through the fixed window,
 * wrapping inside ROM_READ_WINDOW, so it touches the requested byte volume
 * spread across a wide span rather than hammering one cache line. The read
 * is volatile, so the compiler cannot discard it regardless of the result. */
__attribute__((section(".data")))
static void rom_stream_read(uint16_t words) {
	static uint32_t cursor = 0;
	volatile uint16_t *base = (volatile uint16_t*)0x00880000;
	uint16_t i;

	for (i = 0; i < words; i++) {
		(void)base[(cursor & (ROM_READ_WINDOW - 1)) >> 1];
		cursor += ROM_READ_STRIDE;
	}
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
	case 11: // Run DMA-path test, fixed 0x880000 window
		vdp_dma_from((uint32_t)romTestPattern, TEST_DMA_VRAM, TEST_WORDS);
		vdp_check_pattern(TEST_DMA_VRAM);
		break;
	case 12: // Run DMA-path test, banked 0x900000 window, bank 0
		vdp_dma_from(0x00900000u + ((uint32_t)romTestPattern - 0x00880000u),
		             TEST_DMA_VRAM, TEST_WORDS);
		vdp_check_pattern(TEST_DMA_VRAM);
		break;
	case 13: // Run CPU-write control, fixed window
		cpu_upload_pattern(romTestPattern, TEST_CPU_VRAM);
		vdp_check_pattern(TEST_CPU_VRAM);
		break;
	case 14: // Run CPU-write control, banked window
		cpu_upload_pattern((const uint32_t*)(0x00900000u
		                   + ((uint32_t)romTestPattern - 0x00880000u)), TEST_CPU_VRAM);
		vdp_check_pattern(TEST_CPU_VRAM);
		break;
	case 15: // Report match count
		*mars_comm8 = testMatchCount;
		break;
	case 16: // Report first mismatch index
		*mars_comm8 = testMismatchIdx;
		break;
	case 17: // Report first mismatch value, high 16 bits
		*mars_comm8 = (uint16_t)(testMismatchVal >> 16);
		break;
	case 18: // Report first mismatch value, low 16 bits
		*mars_comm8 = (uint16_t)(testMismatchVal & 0xFFFF);
		break;
	case 19: // Set per-frame ROM read volume, in words; 0 disables it
		romReadWords = *mars_comm2;
		break;
	case 20: // Run DMA-path test, MD work RAM source (control, off the cartridge bus)
		vdp_dma_from((uint32_t)romTestPatternRAM, TEST_DMA_VRAM, TEST_WORDS);
		vdp_check_pattern(TEST_DMA_VRAM);
		break;
	case 21: // Run CPU-write control, MD work RAM source
		cpu_upload_pattern(romTestPatternRAM, TEST_CPU_VRAM);
		vdp_check_pattern(TEST_CPU_VRAM);
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
	ram_pattern_init();
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
		if(romReadWords) rom_stream_read(romReadWords);
		*mars_comm12 = ++timer;
	}
}
