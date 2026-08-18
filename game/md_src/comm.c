#include "comm.h"
#include "rings.h"

void comm_boot_publish(uint32_t descriptorOffset, uint16_t screenCenterY)
{
	MD_COMM12 = descriptorOffset;
	MD_COMM6  = screenCenterY;
	MD_COMM2  = 1;
}

/* File-scope (was a comm_read_frame()-local static): TRAVERSAL batch's
 * comm_last_seq() below needs to read the same cache comm_read_frame()
 * already maintains, not a second one -- see comm.h's own comment on why. */
static int32_t lastSeq = -1;

int comm_read_frame(uint16_t *camX, uint16_t *camY, int16_t *worldX, int16_t *worldY,
                     uint16_t *frameIndex, uint8_t *facing, uint8_t *drawGroupHigh,
                     uint8_t *dispRot)
{
	static uint16_t cCamX, cCamY;
	static int16_t cWorldX, cWorldY;
	static uint16_t cFrameIndex;
	static uint8_t cFacing;
	static uint8_t cDrawGroupHigh;
	static uint8_t cDispRot;
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
			uint16_t cyWord = MD_COMM6;
			uint16_t wx = MD_COMM8;
			uint16_t wy = MD_COMM10;
			uint16_t word2 = MD_COMM_ANIM;
			uint8_t seq2 = (uint8_t)(word2 >> 8);

			if (seq2 == seq1) {
				cCamX = cx;
				/* bits [11:0] camera Y, bits [14:12] dispRot, bit 15
				 * drawGroupHigh -- split here, once, so *camY is always the
				 * clean coordinate (sh_src/comm.h's COMM6 entry). */
				cCamY = cyWord & 0x0FFFu;
				cDispRot = (uint8_t)((cyWord >> 12) & 7u);
				cDrawGroupHigh = (uint8_t)((cyWord >> 15) & 1u);
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
	*drawGroupHigh = cDrawGroupHigh;
	*dispRot = cDispRot;

	return lastSeq >= 0;
}

void comm_send_input(uint16_t pad)
{
	static uint8_t tick;
	uint16_t word;
	/* rings.c's own counter, not a second one -- see sh_src/comm.h's
	 * COMM_TICK entry for why only this one bit crosses, not the count. */
	uint8_t hasRings = rings_collected_count() != 0;

	/* Wraps mod 128, matching COMM_TICK's field now being 7 bits wide
	 * instead of 8 (sh_src/comm.h) -- freeing bit [8] for hasRings. */
	tick = (uint8_t)((tick + 1) & 0x7Fu);
	word = ((uint16_t)tick << 9) | ((uint16_t)hasRings << 8) | (pad & 0xFFu);
	MD_COMM_TICK = word;
}

uint8_t comm_last_seq(void)
{
	return lastSeq >= 0 ? (uint8_t)lastSeq : 0;
}
