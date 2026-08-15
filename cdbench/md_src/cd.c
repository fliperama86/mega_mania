/* Mega CD Mode 1 bring-up, 68000 side. Register sequence lifted from
 * ~/Projects/references/SegaCDMode1PCM/main.c (MIT, Mikael Kalms), proven on
 * real CD hardware; everything about reaching that hardware through the 32X
 * cart adapter is new and is exactly what this ROM exists to find out.
 *
 * 68000 interrupts stay off for the whole ROM (see md_start.s), so unlike
 * the reference this cannot arm a vblank-driven INT2 generator. Every wait
 * that runs after the sub-CPU starts drives INT2 itself instead, off the
 * VDP's vblank flag so the rate stays what the BIOS expects, and every such
 * wait is bounded: nothing here may spin forever except the two deliberate
 * terminal states (no CD found, and the steady-state loop). */

#include "common.h"
#include "text.h"
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

/* Bring-up waits get the reference's own figure: 2,000,000 iterations, ten
 * times what the slowest unit (the LaserActive) needs. The two gate-array
 * bus handshakes (reset/busreq, start) are not timed in the reference -- it
 * assumes working hardware -- so they get the same bound here rather than a
 * separate guess. Steady-state pings, once the sub-CPU is already up and
 * running, get a far smaller bound so an unresponsive sub-CPU cannot stall
 * a frame. */
#define CD_REG_TIMEOUT       2000000u
#define CD_HANDSHAKE_TIMEOUT 2000000u
#define CD_CMD_TIMEOUT       100000u

#define CD_PING_WORD 0x1234u

static volatile uint16_t *const vdp_ctrl_port = (uint16_t *)0xC00004;

/* Last state of the VDP's vblank flag that cd_int2_tick() below acted on. */
static uint16_t cd_last_vb = 8;

/* Modelled on ghzview/md_src/vdp.c's vdp_wait_vblank(): status bit 3 is the
 * VDP's vblank flag. Returns at the start of vblank. */
static void vdp_wait_vblank(void) {
	while (*vdp_ctrl_port & 8) ;
	while (!(*vdp_ctrl_port & 8)) ;
	cd_last_vb = 0; /* this edge is real but was consumed here, not there */
}

/* Sub-CPU level 2 interrupt. The CD BIOS needs these to run; with 68000
 * interrupts off (Technical Bulletin 9: no interrupts while RV is set) this
 * has to be an explicit synchronous poke instead of a vblank ISR. */
static void cd_int2(void) {
	*(volatile uint16_t *)0xA12000 |= 0x0100;
}

/* What a wait loop calls, and the only thing that should: INT2 on the vblank
 * flag's rising edge, so the sub-CPU sees roughly the 60 Hz a real vblank ISR
 * would have given it. Poking on every spin instead can hand the sub-CPU a
 * fresh INT2 sooner than it can leave the last one, so the BIOS never reaches
 * the state being waited for and the wait times out for a reason that has
 * nothing to do with the hardware being tested. */
static void cd_int2_tick(void) {
	uint16_t now = *vdp_ctrl_port & 8;

	if (now && !cd_last_vb) cd_int2();
	cd_last_vb = now;
}

/* No libc under -nostdlib here (see the other ROMs in this repo); these are
 * the three primitives the bring-up needs. */
static int cd_eq(const uint8_t *a, const char *b, int n) {
	while (n--) if (*a++ != (uint8_t)*b++) return 0;
	return 1;
}
static void cd_fill(uint8_t *dst, uint8_t v, uint32_t n) {
	volatile uint16_t *d = (volatile uint16_t *)dst;
	uint16_t vv = (uint16_t)(v << 8 | v);

	for (n >>= 1; n--; ) *d++ = vv;
}
/* Word-wise, and deliberately so: the reference reached PRG RAM through
 * newlib's memcpy, which moves longs on aligned data, so a window that does
 * not honour narrow writes would never have shown up there. Both ends are
 * even-aligned and the sub-CPU program is an even number of bytes. */
static void cd_copy(uint8_t *dst, const uint8_t *src, uint32_t n) {
	volatile uint16_t *d = (volatile uint16_t *)dst;
	const uint16_t *s = (const uint16_t *)src;

	for (n >>= 1; n--; ) *d++ = *s++;
}
/* Returns the first differing offset, or n if the two runs match. */
static uint32_t cd_diff(const uint8_t *a, const uint8_t *b, uint32_t n) {
	uint32_t i;
	for (i = 0; i < n; i++) if (a[i] != b[i]) return i;
	return n;
}

/* Writes str into buf[n..] and returns the new length; small helper so the
 * per-step lines below read as one expression each. */
static int cd_app(char *buf, int n, const char *str) {
	return n + cd_put_str(buf + n, str);
}

/* Word-pattern scan across the whole 128 KB PRG RAM window: write every
 * word, then read every word back, so a stuck bit or address-line fault
 * shows up as well as a window that simply does not decode. Nothing here
 * waits on hardware state, so it needs no bound -- it is a fixed number of
 * CPU cycles either way. */
static int cd_prg_probe(uint32_t *fail_off) {
	volatile uint16_t *p = (volatile uint16_t *)PRG_RAM_BASE;
	uint32_t off;

	for (off = 0; off < PRG_RAM_SIZE; off += 2)
		p[off >> 1] = (uint16_t)(off ^ 0xA5A5u);
	for (off = 0; off < PRG_RAM_SIZE; off += 2) {
		if (p[off >> 1] != (uint16_t)(off ^ 0xA5A5u)) {
			*fail_off = off;
			return 0;
		}
	}
	return 1;
}

/* One command/ack round trip over the comm-register protocol. Returns the
 * status byte the sub-CPU replied with (always nonzero), or 0 on timeout at
 * either wait. Both waits tick INT2, since this only ever runs after the
 * sub-CPU has started and needs it to make progress. */
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

/* 'P' ping: sub reads CD_ARG0, replies with its ones complement in
 * CD_RESULT0 and status 'P'. */
static int cd_ping(uint16_t send, uint16_t *got) {
	uint8_t st;

	CD_ARG0 = send;
	st = cd_cmd('P', CD_CMD_TIMEOUT);
	*got = CD_RESULT0;
	cd_cmd_ack();
	return st == 'P' && *got == (uint16_t)~send;
}

void cd_run(void) {
	char line[48];
	uint8_t *bios;
	uint32_t i, fail_off, sub_len;
	uint8_t reg;
	int n, ok;

	/* ---- 1: detect the CD BIOS ---- */
	bios = (uint8_t *)0x415800;
	if (!cd_eq(bios + 0x6D, "SEGA", 4)) {
		bios = (uint8_t *)0x416000;
		if (!cd_eq(bios + 0x6D, "SEGA", 4)) {
			if (!cd_eq(bios + 0x6D, "WONDER", 6)) {
				bios = (uint8_t *)0x41AD00;
				if (!cd_eq(bios + 0x6D, "SEGA", 4)) {
					cd_print(1, "NO CD DETECTED");
					while (1) ;
				}
			}
		}
	}
	n = cd_app(line, 0, "BIOS FOUND AT ");
	n += cd_put_hex(line + n, (uint32_t)bios, 6);
	line[n] = 0;
	cd_print(1, line);

	/* ---- 2: reset the gate array ---- */
	CD_GA_MODE_W = 0xFF00;
	CD_GA_RESET = 0x03;
	CD_GA_RESET = 0x02;
	CD_GA_RESET = 0x00;
	cd_print(2, "GATE ARRAY RESET");

	/* ---- 3: reset the sub-CPU, request its bus ---- */
	CD_GA_RESET = 0x02;
	for (i = 0; i < CD_REG_TIMEOUT; i++) {
		reg = CD_GA_RESET;
		if (reg & 2) break;
		CD_GA_RESET = 0x02;
	}
	n = cd_app(line, 0, "SUB CPU BUS REG ");
	n += cd_put_hex(line + n, reg, 2);
	if (i == CD_REG_TIMEOUT) n = cd_app(line, n, " TIMEOUT");
	line[n] = 0;
	cd_print(3, line);

	/* ---- 4: probe PRG RAM -- the 32X-specific unknown ---- */
	CD_GA_MODE_W = 0x0002; /* no write protect, bank 0, 2M mode, Word RAM to Sub-CPU */
	ok = cd_prg_probe(&fail_off);
	if (ok) {
		cd_print(4, "PRG RAM PROBE PASS");
	} else {
		n = cd_app(line, 0, "PRG RAM PROBE FAIL AT ");
		n += cd_put_hex(line + n, (uint32_t)PRG_RAM_BASE + fail_off, 6);
		line[n] = 0;
		cd_print(4, line);
	}

	/* Byte against word into the same PRG RAM address, reported separately:
	 * which widths the window honours is its own fact, and a copy that only
	 * works one way would otherwise look like a copy that does not work. */
	{
		volatile uint16_t *w = (volatile uint16_t *)SUB_PROGRAM_ADDR;
		volatile uint8_t *b = (volatile uint8_t *)(SUB_PROGRAM_ADDR + 4);
		uint16_t gotw;
		uint8_t gotb;

		*w = 0xA55A;
		gotw = *w;
		*b = 0x5A;
		gotb = *b;

		n = cd_app(line, 0, "PRG WORD A55A GOT ");
		n += cd_put_hex(line + n, gotw, 4);
		n = cd_app(line, n, " BYTE 5A GOT ");
		n += cd_put_hex(line + n, gotb, 2);
		line[n] = 0;
		cd_print(5, line);
	}

	/* ---- 5: decompress the sub-CPU BIOS into PRG RAM ---- */
	cd_fill(PRG_RAM_BASE, 0, PRG_RAM_SIZE); /* needed for the LaserActive */
	Kos_Decomp(bios, PRG_RAM_BASE);
	cd_print(6, "SUB BIOS DECOMPRESSED");

	/* ---- 6: copy the sub-CPU program, verify it landed ---- */
	sub_len = (uint32_t)(Sub_End - Sub_Start);
	cd_copy(SUB_PROGRAM_ADDR, Sub_Start, sub_len);
	{
		uint32_t at = cd_diff(SUB_PROGRAM_ADDR, Sub_Start, sub_len);

		ok = at == sub_len;
		n = cd_app(line, 0, ok ? "SUB PROGRAM COPY PASS LEN " : "SUB PROGRAM COPY FAIL LEN ");
		n += cd_put_uint(line + n, sub_len, 1);
		line[n] = 0;
		cd_print(7, line);

		/* Which byte, and what each side holds: a window that does not
		 * decode, a stale value and a write-protected range all fail the
		 * same way without this line. */
		if (!ok) {
			n = cd_app(line, 0, "AT ");
			n += cd_put_hex(line + n, at, 4);
			n = cd_app(line, n, " ROM ");
			n += cd_put_hex(line + n, Sub_Start[at], 2);
			n = cd_app(line, n, " PRG ");
			n += cd_put_hex(line + n, SUB_PROGRAM_ADDR[at], 2);
			line[n] = 0;
			cd_print(8, line);
		}
	}

	/* The same bytes again, one at a time through a volatile pointer, into a
	 * scratch offset the sub-CPU program does not use: whether this window
	 * honours narrow writes is its own fact, worth knowing separately from
	 * whether the upload above worked. See the traps section of
	 * docs/hardware-budget.md for what this line was originally chasing. */
	{
		volatile uint8_t *scratch = (volatile uint8_t *)(PRG_RAM_BASE + 0x7000);
		uint32_t k, bad = 0;

		for (k = 0; k < sub_len; k++) scratch[k] = Sub_Start[k];
		for (k = 0; k < sub_len; k++) if (scratch[k] != Sub_Start[k]) { bad = k + 1; break; }

		n = cd_app(line, 0, bad ? "BYTE WRITE FAIL AT " : "BYTE WRITE PASS AT ");
		n += cd_put_hex(line + n, bad ? bad - 1 : 0, 4);
		line[n] = 0;
		cd_print(9, line);
	}

	/* ---- 7: start the sub-CPU ---- */
	CD_MAINCMD = 0x00;   /* clear main comm port */
	CD_GA_MODE_B = 0x2A; /* write-protect up to 0x5400 */
	CD_GA_RESET = 0x01;  /* clear bus request, deassert reset */
	for (i = 0; i < CD_REG_TIMEOUT; i++) {
		reg = CD_GA_RESET;
		if (reg & 1) break;
		CD_GA_RESET = 0x01;
	}
	n = cd_app(line, 0, "SUB CPU START REG ");
	n += cd_put_hex(line + n, reg, 2);
	if (i == CD_REG_TIMEOUT) n = cd_app(line, n, " TIMEOUT");
	line[n] = 0;
	cd_print(10, line);

	/* ---- 8: handshake -- wait for INITIALIZING, then READY ---- */
	{
		uint32_t iter1 = 0, iter2 = 0;
		int stage = 0;

		for (; iter1 < CD_HANDSHAKE_TIMEOUT; iter1++) {
			cd_int2_tick();
			if (CD_SUBSTAT == 'I') { stage = 1; break; }
		}
		if (stage == 1) {
			for (; iter2 < CD_HANDSHAKE_TIMEOUT; iter2++) {
				cd_int2_tick();
				if (CD_SUBSTAT == 0) { stage = 2; break; }
			}
		}
		n = cd_app(line, 0, "HANDSHAKE STAGE ");
		n += cd_put_uint(line + n, (uint32_t)stage, 1);
		n = cd_app(line, n, " ITER ");
		n += cd_put_uint(line + n, iter1 + iter2, 1);
		line[n] = 0;
		cd_print(11, line);
	}

	/* ---- 9: send a command, check the ack ---- */
	{
		uint16_t got;
		ok = cd_ping(CD_PING_WORD, &got);
		n = cd_app(line, 0, ok ? "PING PASS SENT " : "PING FAIL SENT ");
		n += cd_put_hex(line + n, CD_PING_WORD, 4);
		n = cd_app(line, n, " GOT ");
		n += cd_put_hex(line + n, got, 4);
		line[n] = 0;
		cd_print(12, line);
	}

	/* ---- 10: steady state -- re-ping every frame ---- */
	{
		uint32_t frame = 0, fails = 0;
		for (;;) {
			uint16_t got;
			vdp_wait_vblank();
			cd_int2_tick();
			if (!cd_ping(CD_PING_WORD, &got)) fails++;
			frame++;

			n = cd_app(line, 0, "FRAME ");
			n += cd_put_uint(line + n, frame, 6);
			n = cd_app(line, n, " FAIL ");
			n += cd_put_uint(line + n, fails, 6);
			line[n] = 0;
			cd_print(14, line);
		}
	}
}
