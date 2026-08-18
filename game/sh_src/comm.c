#include "comm.h"

static uint8_t seq;
/* Cached by comm_wait_tick() from COMM_TICK's bit [8]; see comm_has_rings()
 * below and that register's entry in this file's header. */
static uint8_t s_hasRings;

void comm_publish_frame(uint16_t camX, uint16_t camY, int16_t worldX, int16_t worldY,
                         uint16_t frameIndex, uint8_t facing, uint8_t drawGroupHigh,
                         uint16_t rotation)
{
	uint16_t word;
	/* ROTSTYLE_45DEG's own snap (Drawing.cpp:2703-2704): 8 steps, 0-7. */
	uint16_t dispRot = (uint16_t)(((rotation + 0x20u) & 0x1C0u) >> 6);

	MARS_SYS_COMM2  = camX;
	/* camY & 0x0FFFu: defensive, not load-bearing -- comm.h's invariant
	 * already guarantees camY never reaches bit 12 on its own, but masking
	 * before OR-ing means a future violation of that invariant would only
	 * truncate camY instead of also corrupting dispRot/drawGroupHigh's read. */
	MARS_SYS_COMM6  = (camY & 0x0FFFu) | (dispRot << 12)
	                | ((uint16_t)(drawGroupHigh & 1u) << 15);
	MARS_SYS_COMM8  = (uint16_t)worldX;
	MARS_SYS_COMM10 = (uint16_t)worldY;

	seq++;
	/* 0 is reserved to mean "never published"; skip over it on wrap so
	 * every genuinely published seq value is in 1..255 (see comm.h). */
	if (seq == 0) seq++;
	word = ((uint16_t)seq << 8) | ((frameIndex & 0x7Fu) << 1) | (facing & 1u);
	COMM_ANIM = word;
}

uint16_t comm_wait_tick(void)
{
	static int16_t lastTick = -1;
	uint16_t word;
	uint8_t tick;

	for (;;) {
		word = COMM_TICK;
		/* >> 9 leaves exactly the 7 tick bits (word is 16-bit, so the
		 * result already fits a uint8_t with no mask needed), same as the
		 * old >> 8 left exactly the 8 tick bits before this field was
		 * narrowed (comm.h). */
		tick = (uint8_t)(word >> 9);
		if ((int16_t)tick != lastTick) break;
	}
	lastTick = (int16_t)tick;
	/* Cached from the very word that just proved fresh above -- atomic
	 * with tick and pad, never a second register read (comm.h). */
	s_hasRings = (uint8_t)((word >> 8) & 1u);

	return (uint16_t)(word & 0xFFu);
}

uint8_t comm_has_rings(void)
{
	return s_hasRings;
}
