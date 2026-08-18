#include "descriptor.h"
#include "sonic_data.h"
#include "assets_gen.h"

/* Every pointer field below used to be `(uint32_t)linked_extern_array`,
 * resolved against a symbol assets.s (or this program's own link, for
 * sonic_frames/sonic_anims) provided. ghz_map/ghz_collide_index/
 * ghz_collide_rows/bg_pal/bg_blocks/bg_map/bg_lines moved to bank 1
 * (tools/gen_assets.py's manifest) and are ASSET_* constants now instead;
 * sonic_frames/sonic_anims are small compiled C tables (md_src/
 * sonic_data.c) that stayed linked into this program, so they are unchanged.
 * Either way, the field published here is a raw 68000 address -- the SH2
 * side resolves it through md_addr_to_sh2() (sh_src/assets.h), which now
 * has to recognise BOTH address ranges (this program's own fixed window,
 * still used by sonic_frames/sonic_anims, and bank 1's banked window, used
 * by everything else here) -- see that function's own comment for why one
 * mapping correctly covers both, given bank 1 is always the selected bank
 * from boot onward (md_src/bank.h). */

/* aligned(4): the slave SH2 reads these fields as 32-bit longs through the
 * cartridge window, and the SH2 has no unaligned access -- a 2-mod-4
 * placement is an address-error exception on real hardware and a silent
 * 2-byte-early splice in ares (its SH2 masks the address instead of
 * faulting). Nothing else pins this object's alignment: it rides an
 * LTO-merged rodata blob whose internal layout shifts with every build, so
 * without this attribute the alignment is a per-build coin toss (it came up
 * wrong the day rings landed and cost a full debugging session). */
const AssetDescriptor asset_descriptor __attribute__((aligned(4))) = {
	(uint32_t)ASSET_GHZ_MAP,
	(uint32_t)ASSET_GHZ_COLLIDE_INDEX,
	(uint32_t)ASSET_GHZ_COLLIDE_ROWS,
	(uint32_t)sonic_frames,
	(uint32_t)sonic_anims,
	GHZ_MAP_W,
	GHZ_MAP_H,
	(uint32_t)ASSET_BG_PAL,
	(uint32_t)ASSET_BG_BLOCKS,
	(uint32_t)ASSET_BG_MAP,
	(uint32_t)ASSET_BG_LINES,
};
