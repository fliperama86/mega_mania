#include "descriptor.h"
#include "sonic_data.h"

extern const uint16_t ghz_map[];
extern const uint16_t ghz_collide_index[];
extern const uint8_t ghz_collide_rows[];
extern const uint16_t bg_pal[];
extern const uint8_t bg_blocks[];
extern const uint16_t bg_map[];
extern const uint8_t bg_lines[];

/* aligned(4): the slave SH2 reads these fields as 32-bit longs through the
 * cartridge window, and the SH2 has no unaligned access -- a 2-mod-4
 * placement is an address-error exception on real hardware and a silent
 * 2-byte-early splice in ares (its SH2 masks the address instead of
 * faulting). Nothing else pins this object's alignment: it rides an
 * LTO-merged rodata blob whose internal layout shifts with every build, so
 * without this attribute the alignment is a per-build coin toss (it came up
 * wrong the day rings landed and cost a full debugging session). */
const AssetDescriptor asset_descriptor __attribute__((aligned(4))) = {
	(uint32_t)ghz_map,
	(uint32_t)ghz_collide_index,
	(uint32_t)ghz_collide_rows,
	(uint32_t)sonic_frames,
	(uint32_t)sonic_anims,
	GHZ_MAP_W,
	GHZ_MAP_H,
	(uint32_t)bg_pal,
	(uint32_t)bg_blocks,
	(uint32_t)bg_map,
	(uint32_t)bg_lines,
};
