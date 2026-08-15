#include "descriptor.h"
#include "sonic_data.h"

extern const uint16_t ghz_map[];
extern const uint8_t ghz_collide[];
extern const uint16_t bg_pal[];
extern const uint8_t bg_blocks[];
extern const uint16_t bg_map[];
extern const uint8_t bg_lines[];

const AssetDescriptor asset_descriptor = {
	(uint32_t)ghz_map,
	(uint32_t)ghz_collide,
	(uint32_t)sonic_frames,
	(uint32_t)sonic_anims,
	GHZ_MAP_W,
	GHZ_MAP_H,
	(uint32_t)bg_pal,
	(uint32_t)bg_blocks,
	(uint32_t)bg_map,
	(uint32_t)bg_lines,
};
