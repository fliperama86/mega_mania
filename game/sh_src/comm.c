#include "comm.h"

static uint8_t seq;

void comm_publish_frame(uint16_t camX, uint16_t camY, int16_t worldX, int16_t worldY,
                         uint16_t frameIndex, uint8_t facing, uint8_t drawGroupHigh)
{
	uint16_t word;

	MARS_SYS_COMM2  = camX;
	/* camY & 0x7FFFu: defensive, not load-bearing -- comm.h's invariant
	 * already guarantees camY never reaches bit 15 on its own, but masking
	 * before OR-ing means a future violation of that invariant would only
	 * truncate camY instead of also corrupting drawGroupHigh's read. */
	MARS_SYS_COMM6  = (camY & 0x7FFFu) | ((uint16_t)(drawGroupHigh & 1u) << 15);
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
		tick = (uint8_t)(word >> 8);
		if ((int16_t)tick != lastTick) break;
	}
	lastTick = (int16_t)tick;

	return (uint16_t)(word & 0xFFu);
}
