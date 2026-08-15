#include "comm.h"

void comm_boot_publish(uint32_t descriptorOffset, uint16_t screenCenterY)
{
	MD_COMM12 = descriptorOffset;
	MD_COMM6  = screenCenterY;
	MD_COMM2  = 1;
}

int comm_read_frame(uint16_t *camX, uint16_t *camY, int16_t *worldX, int16_t *worldY,
                     uint16_t *frameIndex, uint8_t *facing)
{
	static uint16_t cCamX, cCamY;
	static int16_t cWorldX, cWorldY;
	static uint16_t cFrameIndex;
	static uint8_t cFacing;
	static int32_t lastSeq = -1;
	int attempt;

	for (attempt = 0; attempt < 4; attempt++) {
		uint16_t word1 = MD_COMM_ANIM;
		uint8_t seq1 = (uint8_t)(word1 >> 8);

		/* seq==0 means the slave has never published a real frame yet
		 * (it reserves 0 and starts real sequence numbers at 1); never
		 * accept this as a consistent frame, even if two reads agree,
		 * and never cache it as "consumed". */
		if (seq1 == 0) continue;

		/* Nothing new since last time: the data words were never re-read,
		 * so there is nothing to tear, and the cache is already correct. */
		if (lastSeq >= 0 && (uint8_t)lastSeq == seq1) break;

		{
			uint16_t cx = MD_COMM2;
			uint16_t cy = MD_COMM6;
			uint16_t wx = MD_COMM8;
			uint16_t wy = MD_COMM10;
			uint16_t word2 = MD_COMM_ANIM;
			uint8_t seq2 = (uint8_t)(word2 >> 8);

			if (seq2 == seq1) {
				cCamX = cx;
				cCamY = cy;
				cWorldX = (int16_t)wx;
				cWorldY = (int16_t)wy;
				cFrameIndex = (uint16_t)((word2 >> 1) & 0x7Fu);
				cFacing = (uint8_t)(word2 & 1u);
				lastSeq = seq1;
				break;
			}
			/* slave overwrote mid-read: torn, retry within the budget */
		}
	}

	*camX = cCamX;
	*camY = cCamY;
	*worldX = cWorldX;
	*worldY = cWorldY;
	*frameIndex = cFrameIndex;
	*facing = cFacing;

	return lastSeq >= 0;
}

void comm_send_input(uint16_t pad)
{
	static uint8_t tick;
	uint16_t word;

	tick++;
	word = ((uint16_t)tick << 8) | (pad & 0xFFu);
	MD_COMM_TICK = word;
}
