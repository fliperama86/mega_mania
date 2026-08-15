/* Mega CD Mode 1 music, 68000 side. Ported from cdbench/md_src/cd.c (itself
 * lifted from ~/Projects/references/SegaCDMode1PCM/main.c, MIT, Mikael
 * Kalms), proven on real CD hardware in ares; everything about reaching
 * that hardware through the 32X cart adapter came from cdbench, and is not
 * re-derived here. Every on-screen diagnostic is gone: this ROM has no text
 * console in its normal path, so every step reports through a return value.
 *
 * 68000 interrupts stay off for the whole ROM (see md_start.s), so like
 * cdbench this cannot arm a vblank-driven INT2 generator. The tick below
 * drives INT2 off the VDP's own vblank flag instead, and every wait in here
 * is bounded: nothing may spin forever, including with no CD hardware
 * present at all, which is the case on the actual test hardware. */

#include "cd.h"

extern void Kos_Decomp(uint8_t *src, uint8_t *dst);
extern const uint8_t Sub_Start[];
extern const uint8_t Sub_End[];

#define CD_GA_INT2       (*(volatile uint16_t *)0xA12000)
#define CD_GA_RESET      (*(volatile uint8_t  *)0xA12001)
#define CD_GA_MODE_W     (*(volatile uint16_t *)0xA12002)
#define CD_GA_MODE_B     (*(volatile uint8_t  *)0xA12002)
#define CD_MAINCMD       (*(volatile uint8_t  *)0xA1200E)
#define CD_SUBSTAT       (*(volatile uint8_t  *)0xA1200F)
#define CD_ARG0          (*(volatile uint16_t *)0xA12010)
#define CD_RESULT0       (*(volatile uint16_t *)0xA12020)

#define PRG_RAM_BASE     ((uint8_t *)0x420000)
#define PRG_RAM_SIZE     0x20000u
#define SUB_PROGRAM_ADDR ((uint8_t *)0x426000)

/* Same figures as cdbench/md_src/cd.c; see that file for how they were
 * picked. CD_DRVINIT_TIMEOUT in particular is ten times CD_HANDSHAKE_TIMEOUT
 * because it covers real disc spin-up, not just a comm-register round trip;
 * a drive with no disc that never comes ready times out here, once, at
 * boot, rather than hanging cd_init(). */
#define CD_REG_TIMEOUT       2000000u
#define CD_HANDSHAKE_TIMEOUT 2000000u
#define CD_CMD_TIMEOUT       100000u
#define CD_DRVINIT_TIMEOUT   20000000u

static volatile uint16_t *const vdp_ctrl_port = (uint16_t *)0xC00004;

/* Whether cd_init() got the sub-CPU program running. cd_music_play() and
 * cd_music_stop() both check this before touching the gate array, so a call
 * made without checking cd_init()'s return value is a no-op instead of a
 * hang on hardware that was never brought up. */
static int cd_ready = 0;

/* Last state of the VDP's vblank flag that cd_int2_tick() acted on. Matches
 * cdbench's cd_last_vb: initialised to a value & 8 can never produce, so the
 * very first call cannot mistake "unknown" for a real edge. */
static uint16_t cd_last_vb = 8;

/* Sub-CPU level 2 interrupt, edge-detected off the VDP's own vblank flag
 * rather than fired unconditionally, so the rate the sub-CPU sees stays close
 * to a real vblank ISR's regardless of how often this gets called: cd_init()'s
 * wait loops call it on every spin, since a bounded wait can run millions of
 * iterations between real vblanks, while main.c's game loop calls it once per
 * frame. Firing on every spin instead, with no edge detection, can hand the
 * sub-CPU a fresh INT2 sooner than it can leave the last one, so the BIOS
 * never reaches the state a wait loop is waiting for and the wait times out
 * for a reason that has nothing to do with the hardware being driven. See
 * cdbench/md_src/cd.c and docs/hardware-budget.md section 8.
 *
 * This is the ungated one, and the bring-up's own waits have to use it: they
 * run before cd_ready can be set, and the handshake they are waiting on
 * cannot complete without these interrupts. Nothing reaches it without a CD
 * BIOS having already answered at 0x400000, so the register is known to be
 * there by the time it writes. */
static void cd_int2_tick(void) {
	uint16_t now = *vdp_ctrl_port & 8;

	if (now && !cd_last_vb) CD_GA_INT2 |= 0x0100;
	cd_last_vb = now;
}

void cd_vblank(void) {
	/* Gated, unlike the tick above: this one runs every frame forever, and
	 * with no CD under the machine nothing is known about what 0xA12000
	 * does. It is a read-modify-write, so it would read open bus and write
	 * it back for the life of the game on the one configuration this
	 * project can actually test on. A tst and a branch is not a cost worth
	 * measuring; finding out the other way is. */
	if (!cd_ready) return;

	cd_int2_tick();
}

/* No libc under -ffreestanding here (see the other ROMs in this repo); the
 * three primitives the bring-up needs. */
static int cd_eq(const uint8_t *a, const char *b, int n) {
	while (n--) if (*a++ != (uint8_t)*b++) return 0;
	return 1;
}
static void cd_fill(uint8_t *dst, uint8_t v, uint32_t n) {
	volatile uint16_t *d = (volatile uint16_t *)dst;
	uint16_t vv = (uint16_t)(v << 8 | v);

	for (n >>= 1; n--; ) *d++ = vv;
}
/* Word-wise, and deliberately so: see cdbench/md_src/cd.c's cd_copy for why
 * a window that does not honour narrow writes would matter here. Both ends
 * are even-aligned and the sub-CPU program is an even number of bytes. */
static void cd_copy(uint8_t *dst, const uint8_t *src, uint32_t n) {
	volatile uint16_t *d = (volatile uint16_t *)dst;
	const uint16_t *s = (const uint16_t *)src;

	for (n >>= 1; n--; ) *d++ = *s++;
}

/* One command/ack round trip over the comm-register protocol. Returns the
 * status byte the sub-CPU replied with (always nonzero), or 0 on timeout at
 * either wait. Both waits tick INT2 directly, since this only ever runs after
 * the sub-CPU has started and needs it to make progress. */
static uint8_t cd_cmd(uint8_t cmd, uint32_t bound) {
	uint32_t i;

	for (i = 0; i < bound; i++) {
		if (CD_SUBSTAT == 0) break;
		cd_int2_tick();
	}
	if (i == bound) return 0;

	CD_MAINCMD = cmd;
	for (i = 0; i < bound; i++) {
		if (CD_SUBSTAT != 0) break;
		cd_int2_tick();
	}
	if (i == bound) return 0;

	return CD_SUBSTAT;
}

static void cd_cmd_ack(void) {
	CD_MAINCMD = 0;
}

int cd_init(void) {
	uint8_t *bios;
	uint32_t i, sub_len;
	uint8_t reg;

	cd_ready = 0;

	/* ---- detect the CD BIOS. The only step here that is not a bounded
	 * wait -- a straight run of memory compares costs a fixed number of
	 * cycles whether or not anything is out there -- which is what makes
	 * cd_init() return promptly with no CD hardware at all: there is
	 * nothing below this point to reach without a BIOS signature. ---- */
	bios = (uint8_t *)0x415800;
	if (!cd_eq(bios + 0x6D, "SEGA", 4)) {
		bios = (uint8_t *)0x416000;
		if (!cd_eq(bios + 0x6D, "SEGA", 4)) {
			if (!cd_eq(bios + 0x6D, "WONDER", 6)) {
				bios = (uint8_t *)0x41AD00;
				if (!cd_eq(bios + 0x6D, "SEGA", 4)) return 0;
			}
		}
	}

	/* ---- reset the gate array ---- */
	CD_GA_MODE_W = 0xFF00;
	CD_GA_RESET = 0x03;
	CD_GA_RESET = 0x02;
	CD_GA_RESET = 0x00;

	/* ---- reset the sub-CPU, request its bus ---- */
	CD_GA_RESET = 0x02;
	for (i = 0; i < CD_REG_TIMEOUT; i++) {
		reg = CD_GA_RESET;
		if (reg & 2) break;
		CD_GA_RESET = 0x02;
	}
	if (i == CD_REG_TIMEOUT) return 0;

	CD_GA_MODE_W = 0x0002; /* no write protect, bank 0, 2M mode, Word RAM to Sub-CPU */

	/* ---- decompress the sub-CPU BIOS into PRG RAM ---- */
	cd_fill(PRG_RAM_BASE, 0, PRG_RAM_SIZE); /* needed for the LaserActive */
	Kos_Decomp(bios, PRG_RAM_BASE);

	/* ---- copy the sub-CPU program in ---- */
	sub_len = (uint32_t)(Sub_End - Sub_Start);
	cd_copy(SUB_PROGRAM_ADDR, Sub_Start, sub_len);

	/* ---- start the sub-CPU ---- */
	CD_MAINCMD = 0x00;   /* clear main comm port */
	CD_GA_MODE_B = 0x2A; /* write-protect up to 0x5400 */
	CD_GA_RESET = 0x01;  /* clear bus request, deassert reset */
	for (i = 0; i < CD_REG_TIMEOUT; i++) {
		reg = CD_GA_RESET;
		if (reg & 1) break;
		CD_GA_RESET = 0x01;
	}
	if (i == CD_REG_TIMEOUT) return 0;

	/* ---- handshake: wait for INITIALIZING, then READY ---- */
	for (i = 0; i < CD_HANDSHAKE_TIMEOUT; i++) {
		cd_int2_tick();
		if (CD_SUBSTAT == 'I') break;
	}
	if (i == CD_HANDSHAKE_TIMEOUT) return 0;
	for (i = 0; i < CD_HANDSHAKE_TIMEOUT; i++) {
		cd_int2_tick();
		if (CD_SUBSTAT == 0) break;
	}
	if (i == CD_HANDSHAKE_TIMEOUT) return 0;

	/* The sub-CPU program is running at this point regardless of what the
	 * drive does next, so this is what cd_ready reports. A drive with no
	 * disc, or one that never comes ready, only affects the drive init call
	 * below and later cd_music_play() calls, neither of which this return
	 * value speaks to. */
	cd_ready = 1;

	/* ---- drive init: spins the motor up and reads the TOC, so it gets its
	 * own much longer bound. Its outcome does not gate cd_ready --
	 * cd_music_play() makes its own check of the BIOS status word before it
	 * will play anything, so a CD unit with no disc is handled there. ---- */
	cd_cmd('D', CD_DRVINIT_TIMEOUT);
	cd_cmd_ack();

	return 1;
}

int cd_music_play(uint16_t track) {
	uint8_t st, drive;
	uint16_t got;

	if (!cd_ready) return 0;

	/* BIOS status word: high byte 0x40 is an open tray, 0x10 is no disc
	 * (SegaCDMode1PCM's SPMain checks the same two). Asking either of those
	 * to play a track and reporting it as started would be a lie. */
	st = cd_cmd('S', CD_CMD_TIMEOUT);
	got = CD_RESULT0;
	cd_cmd_ack();
	if (st != 'S') return 0;

	drive = (uint8_t)(got >> 8);
	if (drive == 0x40 || drive == 0x10) return 0;

	CD_ARG0 = track;
	st = cd_cmd('A', CD_CMD_TIMEOUT);
	cd_cmd_ack();
	return st == 'A';
}

void cd_music_stop(void) {
	if (!cd_ready) return;
	cd_cmd('X', CD_CMD_TIMEOUT);
	cd_cmd_ack();
}
