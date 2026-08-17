#ifndef MD_COMM_H
#define MD_COMM_H

#include <stdint.h>

/* 68000 side of the comm-register protocol with the slave SH2. The full
 * writeup (register map, bit-packing formulas, seqlock and tick/pad
 * reasoning) lives in sh_src/comm.h; this file only carries the matching
 * 68000-side addresses and this side's two entry points. COMM0 and COMM4
 * are the boot handshake (md_start.s) and are never touched here. */

#define MD_COMM2     (*(volatile uint16_t *)0xA15122) /* boot: descriptor-ready flag; steady state: camera X */
#define MD_COMM6     (*(volatile uint16_t *)0xA15126) /* boot: screenCenterY; steady state: camera Y in bits [11:0], dispRot in bits [14:12], Player.drawGroupHigh in bit 15 (sh_src/comm.h) */
#define MD_COMM8     (*(volatile uint16_t *)0xA15128) /* steady state: Sonic world X */
#define MD_COMM10    (*(volatile uint16_t *)0xA1512A) /* steady state: Sonic world Y */
#define MD_COMM12    (*(volatile uint32_t *)0xA1512C) /* boot only: descriptor offset, 32-bit */
#define MD_COMM_ANIM (*(volatile uint16_t *)0xA1512C) /* steady state: packed anim word (upper half) */
#define MD_COMM_TICK (*(volatile uint16_t *)0xA1512E) /* steady state: packed tick+pad word (lower half) */

/* Boot only: publishes the descriptor table's cartridge-relative offset and
 * screenCenterY, then raises the ready flag last, in that order. Call once,
 * after vdp_init() (so screenCenterY is valid) and before the one-time
 * startup spin on comm_read_frame(). */
void comm_boot_publish(uint32_t descriptorOffset, uint16_t screenCenterY);

/* Steady state: the seqlock reader. Always fills every out-param, with
 * fresh values if a new frame landed or the previous frame's cached values
 * otherwise, so the caller never has to special-case a "no new data" return.
 * Returns 0 only until the very first frame has been consumed (for the
 * one-time startup block main() does before it has any camera/Sonic state
 * to draw with); nonzero on every call after that. *camY comes back already
 * masked to bits [11:0] -- dispRot and drawGroupHigh are extracted from
 * COMM6's bits [14:12] and 15 here, once, so no caller needs to know that
 * camera Y, dispRot and drawGroupHigh ever shared a register (sh_src/
 * comm.h's COMM6 entry has the packing and the invariant that makes it
 * safe). *dispRot is the 3-bit snapped sprite rotation (0-7), consumed by
 * md_src/sonic.c's sonic_build/sonic_upload. */
int comm_read_frame(uint16_t *camX, uint16_t *camY, int16_t *worldX, int16_t *worldY,
                     uint16_t *frameIndex, uint8_t *facing, uint8_t *drawGroupHigh,
                     uint8_t *dispRot);

/* Steady state: bumps the tick counter and publishes it with the pad byte,
 * atomically, in one 16-bit store. Call immediately after pad_read(), before
 * anything else that frame. */
void comm_send_input(uint16_t pad);

#endif
