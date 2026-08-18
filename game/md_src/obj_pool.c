#include "obj_pool.h"

void obj_pool_arbitrate(PoolBlock *blocks, uint8_t numBlocks, uint16_t budget)
{
	uint16_t total = 0;
	uint8_t i;

	for (i = 0; i < numBlocks; i++) total = (uint16_t)(total + blocks[i].count);
	if (total <= budget) return;

	{
		uint16_t overflow = (uint16_t)(total - budget);

		while (overflow > 0) {
			uint8_t victim = numBlocks;
			uint16_t victimPriority = 0xFFFFu;
			uint16_t drop;

			for (i = 0; i < numBlocks; i++) {
				if (blocks[i].count == 0) continue;
				if (blocks[i].priority < victimPriority) {
					victimPriority = blocks[i].priority;
					victim = i;
				}
			}
			if (victim == numBlocks) break;   /* every block empty; defensive only */

			drop = overflow < blocks[victim].count ? overflow : blocks[victim].count;
			blocks[victim].count = (uint16_t)(blocks[victim].count - drop);
			overflow = (uint16_t)(overflow - drop);
		}
	}
}
