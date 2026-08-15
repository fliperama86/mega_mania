#include "comm.h"

static uint8_t seq;

void comm_publish_frame(uint16_t camX, uint16_t camY, int16_t worldX, int16_t worldY,
                         uint16_t frameIndex, uint8_t facing)
{
	uint16_t word;

	MARS_SYS_COMM2  = camX;
	MARS_SYS_COMM6  = camY;
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
