/* 32X blit budget benchmark, master SH-2.
 *
 * Ramps the sprite count until the frame's drawing work stops fitting in the
 * target frame time, and reports the number. That number is the ceiling every
 * other design decision hangs off: art size, entity counts, frame rate.
 *
 * Controls:
 *   A      background mode: none / full redraw / dirty rect / MD plane /
 *          line-table scroll
 *   B      CPU mode: 1, 2, 2S (slave busy in SDRAM only)
 *   C      target frame rate, 60 or 30
 *   LEFT   MD hardware sprites: 0, 32, 80
 *   RIGHT  per-line parallax on the MD plane
 *   START  toggle auto ramp, then UP/DOWN set the count by hand
 *   MODE   per-frame ROM read load on the 68000: off / 2 KB / 32 KB
 *   Y      switch to the ROM/DMA window diagnostic screen; Y again to
 *          return here, A on that screen to (re)run it
 */

#include "mars.h"
#include "string.h"
#include "bench.h"

uint16_t bgImage[PLAY_HEIGHT * FB_LINE_WORDS] __attribute__((aligned(4)));
uint16_t sprImage[SPRITE_H * SPRITE_WORDS] __attribute__((aligned(4)));
uint16_t scratchBuf[SCRATCH_SLOTS * SPRITE_H * SPRITE_WORDS] __attribute__((aligned(4)));
uint16_t vfRow[VF_WORDS] __attribute__((aligned(4)));
uint16_t vfGridRow[VF_WORDS] __attribute__((aligned(4)));

static uint32_t lastTick = 0;
static uint16_t currentFB = 0;

static uint16_t joypad = 0, joypadPrev = 0;

/* Timer -------------------------------------------------------------- */

/* Free running timer at phi/32: 719 kHz, 1.39 us per tick, wraps at 91 ms */
static void frt_init(void)
{
	SH2_FRT_TCR = 1;
	SH2_FRT_FRCH = 0;
	SH2_FRT_FRCL = 0;
}

static inline uint16_t frt_read(void)
{
	uint16_t hi = SH2_FRT_FRCH;
	uint16_t lo = SH2_FRT_FRCL;
	return (hi << 8) | lo;
}

/* 32000000 / 23011360 = 1.39064 us per tick, as a fixed point multiply */
static inline uint32_t ticks_to_us(uint32_t ticks)
{
	return (ticks * 5698u) >> 12;
}

/* Text ---------------------------------------------------------------- */

static char textBuf[40];
static uint32_t usClearCpu = 0, usClearFill = 0;
static uint32_t usFillAlone = 0, usWorkAlone = 0, usBoth = 0, usInterleaved = 0;
static uint32_t usCopyCpu = 0, usCopyDma = 0;
static uint32_t sdramSink = 0;

static int put_uint(char *out, uint32_t v, int width)
{
	char tmp[12];
	int n = 0, i;

	do {
		tmp[n++] = '0' + (v % 10);
		v /= 10;
	} while (v);

	for (i = n; i < width; i++) *out++ = ' ';
	for (i = n - 1; i >= 0; i--) *out++ = tmp[i];
	return (width > n ? width : n);
}

static int put_str(char *out, const char *s)
{
	int n = 0;
	while (*s) out[n++] = *s++;
	return n;
}

/* Content ------------------------------------------------------------- */

/* A checkerboard of colour ramps, so a wrong blit is obvious on screen */
static void build_background(void)
{
	int y, x;

	for (y = 0; y < PLAY_HEIGHT; y++) {
		for (x = 0; x < SCREEN_WIDTH; x += 2) {
			int tile = ((x >> 5) + (y >> 5)) & 1;
			uint8_t a = tile ? (uint8_t)(64 + ((x >> 1) & 31))
			                 : (uint8_t)(96 + ((y >> 1) & 31));
			uint8_t b = tile ? (uint8_t)(64 + (((x + 1) >> 1) & 31))
			                 : (uint8_t)(96 + ((y >> 1) & 31));
			bgImage[y * FB_LINE_WORDS + (x >> 1)] = ((uint16_t)a << 8) | b;
		}
	}
}

/* A filled disc, index 0 outside so the overwrite image skips those bytes */
static void build_sprite(void)
{
	int y, x;
	const int r = SPRITE_W / 2;

	for (y = 0; y < SPRITE_H; y++) {
		for (x = 0; x < SPRITE_W; x += 2) {
			uint8_t px[2];
			int i;
			for (i = 0; i < 2; i++) {
				int dx = (x + i) - r;
				int dy = y - r;
				int d2 = dx * dx + dy * dy;
				if (d2 < r * r) {
					int shade = 8 + ((r * r - d2) >> 3);
					if (shade > 31) shade = 31;
					px[i] = (uint8_t)shade;
				} else {
					px[i] = 0;
				}
			}
			sprImage[y * SPRITE_WORDS + (x >> 1)] =
				((uint16_t)px[0] << 8) | px[1];
		}
	}
}

static void build_palette(void)
{
	volatile uint16_t *pal = &MARS_CRAM;
	int i;

	pal[0] = COLOR(0, 0, 0);
	/* 1..31 sprite shading, blue to white */
	for (i = 1; i < 32; i++) pal[i] = COLOR(i >> 1, i >> 1, 31);
	/* 64..95 and 96..127 the two background tile ramps */
	for (i = 0; i < 32; i++) pal[64 + i] = COLOR(0, 6 + (i >> 2), 2);
	for (i = 0; i < 32; i++) pal[96 + i] = COLOR(3, 3, 8 + (i >> 2));
}

/* One line of playfield: vertical colour bands, with a distinct line every
 * 32nd row so vertical movement is obvious on screen */
static void build_vf_rows(void)
{
	int i;

	for (i = 0; i < VF_WORDS; i++) {
		int x = i << 1;
		uint8_t a = (uint8_t)(64 + ((x >> 3) & 31));
		uint8_t b = (uint8_t)(64 + (((x + 1) >> 3) & 31));
		uint8_t c = (uint8_t)(96 + ((x >> 4) & 31));
		vfRow[i]     = ((uint16_t)a << 8) | b;
		vfGridRow[i] = ((uint16_t)c << 8) | c;
	}
}

/* Paint the whole playfield into the current bank, once. After this the view
 * moves by rewriting the line table and nothing else. */
static void vf_fill_bank(void)
{
	volatile uint16_t *fb = &MARS_FRAMEBUFFER;
	int y, i;

	for (y = 0; y < VF_H; y++) {
		volatile uint16_t *dst = fb + FB_DATA_WORD + y * VF_WORDS;
		const uint16_t *src = ((y & 31) == 0) ? vfGridRow : vfRow;
		for (i = 0; i < VF_WORDS; i++) dst[i] = src[i];
	}
	for (i = 0; i < VF_WORDS; i++) fb[VF_BLANK_W + i] = 0;
}

/* The entire per-frame cost of scrolling: 224 word writes */
static void vf_set_line_table(int sx, int sy)
{
	volatile uint16_t *fb = &MARS_FRAMEBUFFER;
	int l;

	for (l = 0; l < PLAY_TOP; l++) fb[l] = VF_BLANK_W;
	for (l = PLAY_TOP; l < SCREEN_HEIGHT; l++)
		fb[l] = (uint16_t)(FB_DATA_WORD + (sy + l - PLAY_TOP) * VF_WORDS
		                 + (sx >> 1));
}

/* Put the line table back to the plain one-line-per-row layout */
static void std_set_line_table(void)
{
	volatile uint16_t *fb = &MARS_FRAMEBUFFER;
	int l;

	for (l = 0; l < SCREEN_HEIGHT; l++)
		fb[l] = (uint16_t)(FB_DATA_WORD + l * FB_LINE_WORDS);
}

/* Copy the whole play area in, the cost a naive engine pays every frame */
static void draw_background_full(void)
{
	volatile uint32_t *dst = (volatile uint32_t *)((&MARS_FRAMEBUFFER)
	                       + FB_DATA_WORD + PLAY_TOP * FB_LINE_WORDS);
	const uint32_t *src = (const uint32_t *)bgImage;
	int n = (PLAY_HEIGHT * FB_LINE_WORDS) / 2;

	while (n >= 8) {
		dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
		dst[4] = src[4]; dst[5] = src[5]; dst[6] = src[6]; dst[7] = src[7];
		dst += 8; src += 8; n -= 8;
	}
	while (n--) *dst++ = *src++;
}

/* The 32X VDP can fill framebuffer words by itself. The length register is
 * 8 bits and the address wraps inside a 256 word block, so a large area is
 * a run of 256 word fills from an aligned start. */
static void vdp_fill(uint16_t addr, uint16_t words, uint16_t data)
{
	while (MARS_VDP_FBCTL & MARS_VDP_FEN) ;
	MARS_VDP_FILLEN = words - 1;
	MARS_VDP_FILADR = addr;
	MARS_VDP_FILDAT = data;
}

/* Clear the play area with the fill hardware instead of CPU writes.
 * 30,720 words from an aligned start is exactly 120 full blocks. */
static void fill_play_area(uint16_t data)
{
	uint16_t addr = FB_DATA_WORD + PLAY_TOP * FB_LINE_WORDS;
	int i;

	for (i = 0; i < (PLAY_HEIGHT * FB_LINE_WORDS) / 256; i++) {
		vdp_fill(addr, 256, data);
		addr += 256;
	}
	while (MARS_VDP_FBCTL & MARS_VDP_FEN) ;
}

/* Does the fill actually run in the background? Time a fill on its own, then
 * time the same fill with a fixed lump of SDRAM work alongside it. If the
 * second is much less than the sum, the fill is free while the CPU works. */
#define WORK_PASSES 8
#define WORK_WORDS  ((SCRATCH_SLOTS * SPRITE_H * SPRITE_WORDS) / 2)

static uint32_t sdram_work_pass(int pass)
{
	volatile uint32_t *p = (volatile uint32_t *)scratchBuf;
	uint32_t sum = 0;
	int i;

	for (i = 0; i < WORK_WORDS; i++) sum += p[i] ^ (uint32_t)pass;
	return sum;
}

static uint32_t sdram_work_lump(void)
{
	uint32_t sum = 0;
	int pass;

	for (pass = 0; pass < WORK_PASSES; pass++) sum += sdram_work_pass(pass);
	return sum;
}

/* The fill re-arms every 256 words, so overlapping means issuing a block and
 * doing a slice of work while that block runs, rather than draining the whole
 * fill first. */
static uint32_t fill_interleaved(void)
{
	uint16_t addr = FB_DATA_WORD + PLAY_TOP * FB_LINE_WORDS;
	const int blocks = (PLAY_HEIGHT * FB_LINE_WORDS) / 256;
	uint32_t sum = 0;
	int k;

	/* One slice of work per fill block. A block takes about 35 us, so the
	 * slices have to be that size or the fills just queue up behind them. */
	{
		volatile uint32_t *p = (volatile uint32_t *)scratchBuf;
		const int total = WORK_PASSES * WORK_WORDS;
		const int slice = total / blocks;
		int idx = 0;

		for (k = 0; k < blocks; k++) {
			int j;
			while (MARS_VDP_FBCTL & MARS_VDP_FEN) ;
			MARS_VDP_FILLEN = 255;
			MARS_VDP_FILADR = addr;
			MARS_VDP_FILDAT = 0;
			addr += 256;
			for (j = 0; j < slice; j++, idx++)
				sum += p[idx % WORK_WORDS] ^ (uint32_t)(idx / WORK_WORDS);
		}
	}
	while (MARS_VDP_FBCTL & MARS_VDP_FEN) ;
	return sum;
}

/* SH-2 DMA channel 0, memory to memory, longwords, auto request, burst.
 * Source is read through the cache-through mirror so it cannot see stale
 * cache lines. */
static void dma_copy(uint32_t dst, uint32_t src, uint32_t bytes)
{
	SH2_DMA_DMAOR = 0;
	SH2_DMA_CHCR0 = 0;
	SH2_DMA_SAR0  = src | 0x20000000;
	SH2_DMA_DAR0  = dst;
	SH2_DMA_TCR0  = bytes >> 2;
	SH2_DMA_DMAOR = 1;                  /* DME */
	SH2_DMA_CHCR0 = 0x5A21;             /* inc/inc, long, auto, burst, enable */
	while (!(SH2_DMA_CHCR0 & 2)) ;      /* TE */
	SH2_DMA_CHCR0 = 0;
	SH2_DMA_DMAOR = 0;
}

/* Main ---------------------------------------------------------------- */

static void joypad_update(void)
{
	joypadPrev = joypad;
	HwMdReadPad(0);
	joypad = MARS_SYS_COMM8;
}

static inline int pressed(uint16_t btn)
{
	return (joypad & btn) && !(joypadPrev & btn);
}

/* Ask the 68000 to turn its scrolling background plane on or off */
static void md_set_bg(int on)
{
	while (MARS_SYS_COMM0) ;
	MARS_SYS_COMM2 = on ? 1 : 0;
	MARS_SYS_COMM0 = 0x0800;
	while (MARS_SYS_COMM0) ;
}

/* Ask the 68000 for N hardware sprites, and for per-line parallax */
static void md_set_sprites(int n)
{
	while (MARS_SYS_COMM0) ;
	MARS_SYS_COMM2 = (uint16_t)n;
	MARS_SYS_COMM0 = 0x0900;
	while (MARS_SYS_COMM0) ;
}

static void md_set_parallax(int on)
{
	while (MARS_SYS_COMM0) ;
	MARS_SYS_COMM2 = on ? 1 : 0;
	MARS_SYS_COMM0 = 0x0A00;
	while (MARS_SYS_COMM0) ;
}

/* Wipe the play area to transparent, used when switching modes so the
 * previous mode's leftovers do not linger in the other buffer */
static void clear_play_area(void)
{
	volatile uint32_t *dst = (volatile uint32_t *)((&MARS_FRAMEBUFFER)
	                       + FB_DATA_WORD + PLAY_TOP * FB_LINE_WORDS);
	int n = (PLAY_HEIGHT * FB_LINE_WORDS) / 2;

	while (n--) *dst++ = 0;
}

static void swap_buffers(void)
{
	while (lastTick == MARS_SYS_COMM12) ;
	MARS_VDP_FBCTL = currentFB ^ 1;
	while ((MARS_VDP_FBCTL & MARS_VDP_FS) == currentFB) ;
	currentFB ^= 1;
	lastTick = MARS_SYS_COMM12;
}

/* ROM/DMA window diagnostic -------------------------------------------
 *
 * Question 1: does VDP DMA sourced from the 32X ROM windows work, with both
 * SH-2s under their normal load and with them idle. Question 2: what does
 * the 68000 reading cartridge ROM every frame cost the SH-2s. The 68000
 * side (md_src/md_main.c) owns the pattern and the comparison; this side
 * triggers it and reads back counts.
 */

#define MD_CMD_DMA_FIXED    0x0B00
#define MD_CMD_DMA_BANKED   0x0C00
#define MD_CMD_CPU_FIXED    0x0D00
#define MD_CMD_CPU_BANKED   0x0E00
#define MD_CMD_GET_MATCH    0x0F00
#define MD_CMD_GET_MISSIDX  0x1000
#define MD_CMD_GET_MISSHI   0x1100
#define MD_CMD_GET_MISSLO   0x1200
#define MD_CMD_SET_ROMLOAD  0x1300

#define ROMTEST_LONGS   128    /* must match TEST_LONGS in md_src/md_main.c */
#define ROMTEST_REPS    32     /* repeats per case, to catch timing-dependent faults */
#define ROMTEST_SPRITES 64     /* fixed SDRAM-only load size for the "busy" case */

/* Per-frame ROM read volumes for Question 2. SMALL is roughly what a strip
 * of freshly scrolled-in tiles costs (a couple dozen 32x32 4bpp tiles a
 * frame); LARGE is a full-screen tile redraw, the worst case a streamer
 * would ever hit in one frame. Word counts, matching rom_stream_read() on
 * the 68000 side. */
#define ROMLOAD_SMALL_WORDS  1024   /* 2 KB/frame */
#define ROMLOAD_LARGE_WORDS  16384  /* 32 KB/frame */

typedef struct {
	uint16_t matchCount;
	uint16_t mismatchIdx;
	uint32_t mismatchVal;
} RomTestResult;

typedef struct {
	uint16_t dmaMatch, cpuMatch;
	uint16_t dmaMissIdx, cpuMissIdx;
	uint32_t dmaMissVal, cpuMissVal;
} RomTestRow;

static RomTestRow romtestRows[4];   /* 0=FIX/BUSY 1=FIX/IDLE 2=BANK/BUSY 3=BANK/IDLE */
static int romtestHasResult = 0;
static char row0Text[40];           /* the one-off storage-timing line, saved so it survives a visit to this screen */

static void md_cmd(uint16_t cmd, uint16_t param)
{
	while (MARS_SYS_COMM0) ;
	MARS_SYS_COMM2 = param;
	MARS_SYS_COMM0 = cmd;
	while (MARS_SYS_COMM0) ;
}

static uint16_t md_cmd_result(uint16_t cmd)
{
	md_cmd(cmd, 0);
	return MARS_SYS_COMM8;
}

/* Cycles and applies the Question 2 per-frame ROM read load: off, small,
 * large. Shared by the MODE button (six-button pads) and the UP button
 * (three-button-safe, only while autoRamp is on, see m_main). */
static int romload_cycle(int romLoadMode)
{
	romLoadMode = (romLoadMode + 1) % 3;
	md_cmd(MD_CMD_SET_ROMLOAD, romLoadMode == 0 ? 0
	       : (romLoadMode == 1 ? ROMLOAD_SMALL_WORDS : ROMLOAD_LARGE_WORDS));
	return romLoadMode;
}

static void md_fetch_result(RomTestResult *r)
{
	uint16_t hi, lo;
	r->matchCount  = md_cmd_result(MD_CMD_GET_MATCH);
	r->mismatchIdx = md_cmd_result(MD_CMD_GET_MISSIDX);
	hi = md_cmd_result(MD_CMD_GET_MISSHI);
	lo = md_cmd_result(MD_CMD_GET_MISSLO);
	r->mismatchVal = ((uint32_t)hi << 16) | lo;
}

/* Real concurrent load: kick the slave onto the same SDRAM-only workload the
 * normal benchmark already uses (SLAVE_SDRAM), then give the master its own
 * share, so the MD command sandwiched in between genuinely overlaps both
 * SH-2s working, the same as it would during real gameplay. */
static void romtest_kick_slave(uint32_t frame)
{
	MARS_SYS_COMM6 = (uint16_t)frame;
	MARS_SYS_COMM4 = SLAVE_BUSY | SLAVE_SDRAM | ROMTEST_SPRITES;
}

static void romtest_settle_slave(void)
{
	(void)sdram_work_lump();
	while (MARS_SYS_COMM4 & SLAVE_BUSY) ;
}

static void romtest_run_one(uint16_t dmaCmd, uint16_t cpuCmd, int busy, RomTestRow *row)
{
	uint32_t frame = 0;
	int rep;
	RomTestResult r;

	row->dmaMatch = ROMTEST_LONGS; row->dmaMissIdx = 0xFFFF; row->dmaMissVal = 0;
	row->cpuMatch = ROMTEST_LONGS; row->cpuMissIdx = 0xFFFF; row->cpuMissVal = 0;

	for (rep = 0; rep < ROMTEST_REPS; rep++) {
		if (busy) romtest_kick_slave(frame);
		md_cmd(dmaCmd, 0);
		md_fetch_result(&r);
		if (busy) romtest_settle_slave();
		if (r.matchCount < row->dmaMatch) row->dmaMatch = r.matchCount;
		if (r.mismatchIdx != 0xFFFF && row->dmaMissIdx == 0xFFFF) {
			row->dmaMissIdx = r.mismatchIdx;
			row->dmaMissVal = r.mismatchVal;
		}

		if (busy) romtest_kick_slave(frame);
		md_cmd(cpuCmd, 0);
		md_fetch_result(&r);
		if (busy) romtest_settle_slave();
		if (r.matchCount < row->cpuMatch) row->cpuMatch = r.matchCount;
		if (r.mismatchIdx != 0xFFFF && row->cpuMissIdx == 0xFFFF) {
			row->cpuMissIdx = r.mismatchIdx;
			row->cpuMissVal = r.mismatchVal;
		}

		frame++;
	}
}

/* slaveAlive gates the "busy" reps: if the slave never came up, dispatching
 * SLAVE_BUSY to it would spin forever waiting for a flag nobody clears. The
 * "idle" reps never touch the slave and are unaffected either way. */
static void romtest_run_all(int slaveAlive)
{
	romtest_run_one(MD_CMD_DMA_FIXED,  MD_CMD_CPU_FIXED,  slaveAlive, &romtestRows[0]);
	romtest_run_one(MD_CMD_DMA_FIXED,  MD_CMD_CPU_FIXED,  0, &romtestRows[1]);
	romtest_run_one(MD_CMD_DMA_BANKED, MD_CMD_CPU_BANKED, slaveAlive, &romtestRows[2]);
	romtest_run_one(MD_CMD_DMA_BANKED, MD_CMD_CPU_BANKED, 0, &romtestRows[3]);
	romtestHasResult = 1;
}

static int put_hex(char *out, uint32_t v, int digits)
{
	static const char hx[] = "0123456789ABCDEF";
	int i;
	for (i = 0; i < digits; i++)
		out[i] = hx[(v >> ((digits - 1 - i) * 4)) & 0xF];
	return digits;
}

static void clear_text_row(int y)
{
	char buf[39];
	int i;
	for (i = 0; i < 38; i++) buf[i] = ' ';
	buf[38] = 0;
	HwMdPuts(buf, 0x2000, 1, y);
}

static void romtest_format_row(char *buf, const char *label, const RomTestRow *row)
{
	int n = 0;
	n += put_str(buf, label);
	n += put_str(buf + n, " D");
	n += put_uint(buf + n, row->dmaMatch, 3);
	n += put_str(buf + n, "/");
	n += put_uint(buf + n, ROMTEST_LONGS, 1);
	n += put_str(buf + n, " C");
	n += put_uint(buf + n, row->cpuMatch, 3);
	n += put_str(buf + n, "/");
	n += put_uint(buf + n, ROMTEST_LONGS, 1);
	buf[n] = 0;
}

/* Shows the first mismatch found, in FIX-BUSY, FIX-IDLE, BANK-BUSY,
 * BANK-IDLE scan order, DMA before CPU within each. Which row it belongs to
 * is already visible from the match counts above it. Expected is always
 * 0xA5A50000 | index, so it is shown from the index rather than sent back
 * from the 68000 separately. */
static void romtest_format_miss(char *buf)
{
	static const char *labels[4] = {"FIX BUSY", "FIX IDLE", "BNK BUSY", "BNK IDLE"};
	int i, n;

	for (i = 0; i < 4; i++) {
		const RomTestRow *row = &romtestRows[i];
		uint16_t idx = 0xFFFF;
		uint32_t val = 0;
		const char *method = 0;

		if (row->dmaMissIdx != 0xFFFF) { idx = row->dmaMissIdx; val = row->dmaMissVal; method = "DMA"; }
		else if (row->cpuMissIdx != 0xFFFF) { idx = row->cpuMissIdx; val = row->cpuMissVal; method = "CPU"; }

		if (method) {
			/* Kept under 40 columns: the MD screen is 40 wide and
			 * anything past that is simply not on the picture. */
			n = put_str(buf, labels[i]);
			n += put_str(buf + n, " ");
			n += put_str(buf + n, method);
			n += put_str(buf + n, " i");
			n += put_uint(buf + n, idx, 3);
			n += put_str(buf + n, " E");
			n += put_hex(buf + n, 0xA5A50000u | idx, 8);
			n += put_str(buf + n, " G");
			n += put_hex(buf + n, val, 8);
			buf[n] = 0;
			return;
		}
	}
	n = put_str(buf, "ALL MATCHED, every case ");
	n += put_uint(buf + n, ROMTEST_LONGS, 1);
	n += put_str(buf + n, "/");
	n += put_uint(buf + n, ROMTEST_LONGS, 1);
	buf[n] = 0;
}

static void romtest_display(const char *title)
{
	char buf[40];
	int n;

	n = put_str(buf, title);
	buf[n] = 0;
	HwMdPuts(buf, 0x2000, 1, 0);

	if (!romtestHasResult) {
		n = put_str(buf, "press A to run the battery");
		buf[n] = 0;
		HwMdPuts(buf, 0x2000, 1, 1);
		clear_text_row(2);
		clear_text_row(3);
		clear_text_row(4);
		clear_text_row(5);
		return;
	}

	romtest_format_row(buf, "FIX BUSY", &romtestRows[0]);
	HwMdPuts(buf, 0x2000, 1, 1);
	romtest_format_row(buf, "FIX IDLE", &romtestRows[1]);
	HwMdPuts(buf, 0x2000, 1, 2);
	romtest_format_row(buf, "BNK BUSY", &romtestRows[2]);
	HwMdPuts(buf, 0x2000, 1, 3);
	romtest_format_row(buf, "BNK IDLE", &romtestRows[3]);
	HwMdPuts(buf, 0x2000, 1, 4);
	romtest_format_miss(buf);
	HwMdPuts(buf, 0x2000, 1, 5);
}

int m_main(void)
{
	uint32_t frame = 0;
	int nSprites = 16;
	int peak = 0;
	int bgMode = 1;
	/* 0 = master only, 1 = both blitting, 2 = master blits everything while
	 * the slave does the same amount of work in SDRAM only */
	int cpuMode = 0;
	int slaveAlive = 0;
	int target60 = 1;
	int autoRamp = 1;
	uint32_t usFrame = 0;
	int prevCount[2] = { 0, 0 };
	uint32_t prevFrame[2] = { 0, 0 };
	/* A single frame's timing is too noisy to steer on, so hold the count
	 * fixed for a window and judge it by its worst frame. Worst case is
	 * what a game actually has to live inside anyway. */
	uint32_t winMin = 0xFFFFFFFFu, winMax = 0;
	uint32_t repMin = 0, repMax = 0;
	int winFrames = 0;
	int clearPending = 0;
	int vfPending = (bgMode == 4) ? 2 : 0;   /* paint both banks first */
	int stdTablePending = 0;
	int prevScrollX[2] = { 0, 0 }, prevScrollY[2] = { 0, 0 };
	int scrollX = 0, scrollY = 0;
	int mdSpr = 0, parallax = 0;
	int uiMode = 0;       /* 0 = benchmark, 1 = ROM/DMA window diagnostic */
	int romLoadMode = 0;  /* 0 = off, 1 = small per-frame ROM read, 2 = large */

	Hw32xInit(MARS_VDP_MODE_256, 0);
	/* 32X PWM off. Cycle 0 is a prohibited value, so give it a valid period
	 * and mute the channels rather than zeroing it. */
	MARS_PWM_CYCLE = 0x1E4;
	MARS_PWM_CTRL = 0;
	MARS_PWM_MONO = 0;
	frt_init();
	build_palette();
	build_background();
	build_sprite();
	build_vf_rows();

	Hw32xDelay(1);

	/* One-off: what does clearing the play area cost each way? */
	{
		uint16_t a, b;
		int n;
		a = frt_read();
		clear_play_area();
		b = frt_read();
		usClearCpu = ticks_to_us((uint16_t)(b - a));

		a = frt_read();
		fill_play_area(0);
		b = frt_read();
		usClearFill = ticks_to_us((uint16_t)(b - a));

		/* fill alone, work alone, then both together */
		a = frt_read();
		fill_play_area(0);
		b = frt_read();
		usFillAlone = ticks_to_us((uint16_t)(b - a));

		a = frt_read();
		sdramSink = sdram_work_lump();
		b = frt_read();
		usWorkAlone = ticks_to_us((uint16_t)(b - a));

		a = frt_read();
		{
			uint16_t fa = FB_DATA_WORD + PLAY_TOP * FB_LINE_WORDS;
			int k;
			for (k = 0; k < (PLAY_HEIGHT * FB_LINE_WORDS) / 256; k++) {
				vdp_fill(fa, 256, 0);
				fa += 256;
			}
			sdramSink += sdram_work_lump();     /* while the fill runs */
			while (MARS_VDP_FBCTL & MARS_VDP_FEN) ;
		}
		b = frt_read();
		usBoth = ticks_to_us((uint16_t)(b - a));

		a = frt_read();
		draw_background_full();
		b = frt_read();
		usCopyCpu = ticks_to_us((uint16_t)(b - a));

		a = frt_read();
		dma_copy((uint32_t)((&MARS_FRAMEBUFFER) + FB_DATA_WORD
		                    + PLAY_TOP * FB_LINE_WORDS),
		         (uint32_t)bgImage,
		         PLAY_HEIGHT * FB_LINE_WORDS * 2);
		b = frt_read();
		usCopyDma = ticks_to_us((uint16_t)(b - a));

		n = put_str(textBuf, "CLR ");
		n += put_uint(textBuf + n, usClearCpu, 4);
		n += put_str(textBuf + n, " FIL ");
		n += put_uint(textBuf + n, usFillAlone, 4);
		n += put_str(textBuf + n, " CPY ");
		n += put_uint(textBuf + n, usCopyCpu, 4);
		n += put_str(textBuf + n, " DMA ");
		n += put_uint(textBuf + n, usCopyDma, 4);
		textBuf[n] = 0;
		HwMdPuts(textBuf, 0x2000, 1, 0);

		/* Saved so row 0 can be restored after a visit to the ROM/DMA
		 * diagnostic screen, which reuses this row for its own title. */
		{
			int k;
			for (k = 0; k <= n && k < 39; k++) row0Text[k] = textBuf[k];
			row0Text[k] = 0;
		}
	}

	/* Let the slave purge its cache and come up before handing it work.
	 * Bounded, so a slave that never answers costs us the second CPU
	 * rather than the whole program. */
	{
		uint32_t spins = 0;
		while (MARS_SYS_COMM4 != SLAVE_READY && ++spins < 4000000u) ;
		slaveAlive = (MARS_SYS_COMM4 == SLAVE_READY);
		MARS_SYS_COMM4 = 0;
	}
	cpuMode = slaveAlive ? 1 : 0;
	md_set_bg(bgMode == 3);
	md_set_sprites(mdSpr);
	md_set_parallax(parallax);

	/* Question 1 needs no button to reach: run the battery once
	 * automatically at boot and hold the result on screen until any button
	 * is pressed. This is required, not cosmetic: read_joypad's three-button
	 * fallback in md_src/md_start.s (the 0x010F substitution) makes MODE, X,
	 * Y and Z read as permanently unpressed, so on a three-button pad the Y
	 * toggle below can never be reached. Any of the buttons that do survive
	 * the fallback (D-pad, A, B, C, START) dismisses this screen. Y and A
	 * still work afterward, for repeat runs on a six-button pad. */
	{
		int r;
		romtest_run_all(slaveAlive);
		for (r = 0; r <= 5; r++) clear_text_row(r);
		romtest_display("ROMDMA TEST  press any key");
		/* The result reporting hands values back through COMM8, which is
		 * also where the joypad arrives, so the first read after a battery
		 * is a leftover result rather than buttons. Two flush reads put a
		 * real pad value in both joypad and joypadPrev before the edge test,
		 * otherwise this screen dismisses itself instantly. */
		joypad_update();
		joypad_update();
		do {
			joypad_update();
		} while (!(joypad & (uint16_t)~joypadPrev));
		for (r = 1; r <= 5; r++) clear_text_row(r);
		HwMdPuts(row0Text, 0x2000, 1, 0);
	}

	for (;;) {
		uint16_t t0, t1;
		int i, step;
		uint32_t targetUs = target60 ? 16667 : 33333;

		joypad_update();

		/* Y: switch between the sprite benchmark and the ROM/DMA window
		 * diagnostic. Checked first and unconditionally so none of the
		 * benchmark's own buttons (A included) leak through while the
		 * diagnostic is on screen. Entering the diagnostic suspends the
		 * per-frame ROM read load (Question 2) so it cannot confound
		 * Question 1's result; leaving restores whatever load was picked. */
		if (pressed(SEGA_CTRL_Y)) {
			int r;
			uiMode ^= 1;
			peak = 0;
			md_set_bg(uiMode ? 0 : (bgMode == 3));
			md_set_sprites(uiMode ? 0 : mdSpr);
			md_cmd(MD_CMD_SET_ROMLOAD, uiMode ? 0
			       : (romLoadMode == 0 ? 0
			          : (romLoadMode == 1 ? ROMLOAD_SMALL_WORDS : ROMLOAD_LARGE_WORDS)));
			if (uiMode) {
				for (r = 0; r <= 5; r++) clear_text_row(r);
				romtest_display("ROMDMA TEST  A=RUN  Y=BACK");
			} else {
				for (r = 1; r <= 5; r++) clear_text_row(r);
				HwMdPuts(row0Text, 0x2000, 1, 0);
			}
		}
		if (uiMode) {
			/* The diagnostic screen owns A for itself (re-run) and takes
			 * over the whole loop body: none of the benchmark's ramp,
			 * drawing or timing logic below this point runs while it is
			 * up, and the 32X framebuffer is left exactly as it was. */
			if (pressed(SEGA_CTRL_A)) {
				romtest_run_all(slaveAlive);
				romtest_display("ROMDMA TEST  A=RUN  Y=BACK");
			}
			continue;
		}

		/* Any config change invalidates the peak for the old config */
		if (pressed(SEGA_CTRL_A)) {
			int wasVF = (bgMode == 4);
			bgMode = (bgMode + 1) % 7;
			peak = 0;
			md_set_bg(bgMode == 3);
			if (bgMode == 4) {
				vfPending = 2;        /* paint the playfield in both banks */
			} else {
				clearPending = 2;     /* wipe both buffers of the old mode */
				if (wasVF) stdTablePending = 2;
			}
		}
		if (pressed(SEGA_CTRL_B) && slaveAlive) { cpuMode = (cpuMode + 1) % 3; peak = 0; }
		if (pressed(SEGA_CTRL_C)) { target60 ^= 1; peak = 0; }
		if (pressed(SEGA_CTRL_START)) { autoRamp ^= 1; peak = 0; }
		if (!autoRamp) {
			if (pressed(SEGA_CTRL_UP))   nSprites += 8;
			if (pressed(SEGA_CTRL_DOWN)) nSprites -= 8;
		} else if (pressed(SEGA_CTRL_UP)) {
			/* UP only steers nSprites by hand when autoRamp is off, so it is
			 * idle whenever autoRamp is on. That makes it the three-button-
			 * safe way to cycle the Question 2 ROM read load: MODE reads as
			 * permanently unpressed on a three-button pad (read_joypad's
			 * 0x010F fallback in md_src/md_start.s), but UP survives it. */
			romLoadMode = romload_cycle(romLoadMode);
			peak = 0;
		}
		/* The MD VDP draws these itself. If the peak does not move when they
		 * are switched on, they really are free as far as the SH-2s care. */
		if (pressed(SEGA_CTRL_LEFT)) {
			mdSpr = (mdSpr == 0) ? 32 : (mdSpr == 32 ? 80 : 0);
			md_set_sprites(mdSpr);
			peak = 0;
		}
		if (pressed(SEGA_CTRL_RIGHT)) {
			parallax ^= 1;
			md_set_parallax(parallax);
			peak = 0;
		}
		/* MODE: cycle the Question 2 per-frame ROM read load. Off, then a
		 * small edge-streaming volume, then a full-screen redraw volume.
		 * Six-button pads only; UP above does the same job on any pad. */
		if (pressed(SEGA_CTRL_MODE)) {
			romLoadMode = romload_cycle(romLoadMode);
			peak = 0;
		}
		if (nSprites < 0) nSprites = 0;
		if (nSprites > MAX_SPRITES) nSprites = MAX_SPRITES;

		/* Everything between the two timer reads is the frame's drawing
		 * work. The vsync wait and the text output are deliberately
		 * outside it. */
		if (vfPending)       { vf_fill_bank();      vfPending--; }
		if (stdTablePending) { std_set_line_table(); stdTablePending--; }
		if (clearPending)    { clear_play_area();    clearPending--; }

		scroll_pos(frame, &scrollX, &scrollY);

		t0 = frt_read();

		/* In SDRAM mode the master still draws every sprite, so the only
		 * difference from the single CPU case is that the slave is busy
		 * elsewhere. That makes it a clean A/B against CPU 1. */
		step = (cpuMode == 1) ? 2 : 1;
		if (cpuMode != 0) {
			MARS_SYS_COMM6 = (uint16_t)frame;
			MARS_SYS_COMM10 = (uint16_t)prevCount[1];   /* restores to undo */
			MARS_SYS_COMM4 = SLAVE_BUSY | (uint16_t)nSprites
			               | ((cpuMode == 2) ? SLAVE_SDRAM : 0)
			               | ((bgMode == 4) ? SLAVE_VF : 0)
			               | ((bgMode == 5 || bgMode == 6) ? SLAVE_OPAQ : 0)
			               | ((bgMode == 6) ? SLAVE_LONG : 0);
		}

		if (bgMode == 4) {
			/* Scrolling is just the line table. Restores are split with the
			 * slave the same way the blits are, or the master ends up doing
			 * one and a half times the work. */
			vf_set_line_table(scrollX, scrollY);
			for (i = 0; i < prevCount[1]; i += step) {
				int x, y;
				sprite_pos(i, prevFrame[1], &x, &y);
				vf_restore_rect(prevScrollX[1] + x,
				                prevScrollY[1] + (y - PLAY_TOP));
			}
		} else if (bgMode == 5 || bgMode == 6) {
			fill_play_area(0);
		} else if (bgMode == 3) {
			/* Genesis VDP owns the background, so the 32X layer only has to
			 * go back to transparent. One hardware fill beats clearing per
			 * sprite once there are more than about 30 of them. */
			fill_play_area(0);
		} else if (bgMode == 1) {
			draw_background_full();
		} else if (bgMode == 2) {
			/* Restore what the sprites covered two frames ago, since
			 * that is when this buffer was last drawn into */
			for (i = 0; i < prevCount[1]; i++) {
				int x, y;
				sprite_pos(i, prevFrame[1], &x, &y);
				restore_rect(x, y);
			}
		}

		for (i = 0; i < nSprites; i += step) {
			int x, y;
			sprite_pos(i, frame, &x, &y);
			if (bgMode == 4)
				vf_blit_sprite(scrollX + x, scrollY + (y - PLAY_TOP));
			else if (bgMode == 5)
				blit_sprite_opaque(x, y);
			else if (bgMode == 6)
				blit_sprite_opaque_long(x & ~3, y);
			else
				blit_sprite(x, y);
		}

		if (cpuMode != 0)
			while (MARS_SYS_COMM4 & SLAVE_BUSY) ;

		t1 = frt_read();
		usFrame = ticks_to_us((uint16_t)(t1 - t0));

		/* Skip the first frames of a window: the count just changed and
		 * with BG mode 2 the restore still refers to the old count */
		if (winFrames >= 2) {
			if (usFrame > winMax) winMax = usFrame;
			if (usFrame < winMin) winMin = usFrame;
		}
		winFrames++;

		prevCount[1] = prevCount[0];
		prevFrame[1] = prevFrame[0];
		prevScrollX[1] = prevScrollX[0];
		prevScrollY[1] = prevScrollY[0];
		prevCount[0] = nSprites;
		prevFrame[0] = frame;
		prevScrollX[0] = scrollX;
		prevScrollY[0] = scrollY;

		if (winFrames >= 32) {
			int n = 0;
			int measured = nSprites;   /* what the window actually ran */

			/* Judge the window by its worst frame, then take one step */
			if (winMax <= targetUs) {
				if (nSprites > peak) peak = nSprites;
				if (autoRamp) nSprites += 8;
			} else if (autoRamp) {
				nSprites -= 8;
			}

			repMin = winMin;
			repMax = winMax;
			winMin = 0xFFFFFFFFu;
			winMax = 0;
			winFrames = 0;

			n = 0;
			n += put_str(textBuf, "SPR ");
			n += put_uint(textBuf + n, (uint32_t)measured, 3);
			n += put_str(textBuf + n, "  PEAK ");
			n += put_uint(textBuf + n, (uint32_t)peak, 3);
			textBuf[n] = 0;
			HwMdPuts(textBuf, 0x2000, 1, 1);

			n = 0;
			n += put_str(textBuf, "MIN ");
			n += put_uint(textBuf + n, repMin, 5);
			n += put_str(textBuf + n, "  MAX ");
			n += put_uint(textBuf + n, repMax, 5);
			n += put_str(textBuf + n, autoRamp ? "  AUTO  " : "  MANUAL");
			/* Which button currently cycles the Question 2 ROM read load:
			 * UP only works while auto-ramping (it is busy steering the
			 * count by hand otherwise), MODE always works but needs a
			 * six-button pad. Shown here, next to AUTO/MANUAL, so the
			 * active mapping is unambiguous from a single photograph. */
			n += put_str(textBuf + n, autoRamp ? " UP/MD=RD" : " MD=RD");
			textBuf[n] = 0;
			HwMdPuts(textBuf, 0x2000, 1, 3);

			n = 0;
			n += put_str(textBuf, "CPU ");
			n += put_str(textBuf + n, cpuMode == 0 ? "1 "
			                        : cpuMode == 1 ? "2 " : "2S");
			n += put_str(textBuf + n, "  BG ");
			n += put_uint(textBuf + n, (uint32_t)bgMode, 1);
			n += put_str(textBuf + n, "  HZ ");
			n += put_uint(textBuf + n, target60 ? 60 : 30, 2);
			n += put_str(textBuf + n, "  MDS ");
			n += put_uint(textBuf + n, (uint32_t)mdSpr, 2);
			n += put_str(textBuf + n, parallax ? " PX" : "   ");
			n += put_str(textBuf + n, romLoadMode == 0 ? "  RD OFF"
			                        : romLoadMode == 1 ? "  RD 2K" : "  RD 32K");
			textBuf[n] = 0;
			HwMdPuts(textBuf, 0x2000, 1, 2);

		}

		frame++;
		swap_buffers();
	}

	return 0;
}
