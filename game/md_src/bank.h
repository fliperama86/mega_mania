#ifndef BANK_H
#define BANK_H

#include <stdint.h>

/* 32X cartridge bank-set register, 0xA15104 (docs/hardware-budget.md,
 * section 3). Low two bits select which 1 MB slice of the cartridge is
 * mapped into the 68000's BANKED ROM window, 0x900000-0x9FFFFF:
 *
 *   bank 0 -> cart 0x000000-0x0FFFFF (power-on value)
 *   bank 1 -> cart 0x100000-0x1FFFFF
 *   bank 2 -> cart 0x200000-0x2FFFFF
 *   bank 3 -> cart 0x300000-0x3FFFFF
 *
 * The FIXED window at 0x880000-0x8FFFFF (cart 0x000000-0x07FFFF) is a
 * different window and ignores this register completely -- everything that
 * already lives there (the whole 68000 program) needs no bank switch and is
 * never at risk from one.
 *
 * Every asset in this game -- everything tools/gen_assets.py's manifest
 * lists, both CPUs' data -- lives in bank 1 (md_src/assets_gen.h, generated;
 * see that file's own header comment). main() calls bank_select(1) exactly
 * ONCE, as close to the top of boot as the code gets, before touching any
 * cartridge data at all, and NEVER switches away again: every fixed pointer
 * in this codebase from that point on (main.c, sonic.c, springs.c,
 * signpost.c, rings.c, descriptor.c) is a bank-1 address, and bank 0's
 * upper half (0x080000-0x0FFFFF) holds nothing the 68000 ever reads through
 * this window any more. That is the whole point of consolidating every
 * asset into one bank: a single switch at boot, never revisited, structurally
 * cannot hit either hazard below, because there is no second switch for a
 * handler or a later call to race against.
 *
 * The two hazards below are kept for the day a SECOND bank switch is ever
 * needed (a roster of objects too big for one bank, say) -- they do not
 * apply to today's single boot-time switch, but they will apply again the
 * moment anything calls bank_select() a second time after boot:
 *
 *  - Interrupt safety. A bank switch is only as safe as "nothing else reads
 *    0x900000+ while the wrong bank is selected". Today that is
 *    structurally impossible: the 68000 never takes an interrupt in this
 *    codebase. The security blob in md_start.s sets the status register to
 *    0x2700 (interrupt priority mask 7) once at boot and nothing here ever
 *    lowers it -- main() polls vdp_wait_vblank() instead of taking a real
 *    vblank interrupt, and md_start.s's own _vblank/_hblank vectors are bare
 *    `rte` stubs that do nothing even if they somehow fired anyway
 *    (Technical Bulletin 9 forbids taking an interrupt with RV set --
 *    hardware-budget.md, section 3). If that ever changes and a real
 *    handler starts reading through the banked window, every caller of a
 *    SECOND bank_select() must disable interrupts before switching and
 *    restore the resting bank before re-enabling them, so a handler firing
 *    mid-switch can never observe the wrong bank.
 *
 *  - VDP DMA. Measured on real hardware (hardware-budget.md, section 3): the
 *    32X adapter transfers zero bytes, silently, when the VDP's own DMA
 *    engine sources from either cartridge window, fixed or banked, at bank
 *    0. Nothing about the bank register changes which bus the DMA engine
 *    reads over, so a non-zero bank has no reason to behave differently --
 *    but that exact case (DMA sourcing from a non-zero bank) has not itself
 *    been measured. This codebase does not DMA from ROM anywhere (vdp.c's
 *    own comment); reach banked data with the same CPU-write helpers
 *    (vdp_tiles_load and friends) everything else already uses, and
 *    re-measure before ever trying otherwise. */
#define BANK_SET (*(volatile uint16_t *)0xA15104)

static inline void bank_select(uint16_t bank) {
	BANK_SET = bank & 3;
}

#endif
