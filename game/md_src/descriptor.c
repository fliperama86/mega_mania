#include "descriptor.h"
#include "sonic_data.h"

extern const uint16_t ghz_map[];
extern const uint8_t ghz_collide[];

const AssetDescriptor asset_descriptor = {
	(uint32_t)ghz_map,
	(uint32_t)ghz_collide,
	(uint32_t)sonic_frames,
	(uint32_t)sonic_anims,
};
