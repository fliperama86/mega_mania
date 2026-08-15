#include "assets.h"
#include "sonic_data.h"
#include "mars.h"

/* Filled in once by assets_init(). path.c and sonic_anim.c declare their own
 * matching externs rather than including this file, keeping them free of
 * any descriptor/comm knowledge. */
const uint16_t *g_ghz_map;
const uint8_t *g_ghz_collide;
const SonicFrame *g_sonic_frames;
const SonicAnim *g_sonic_anims;

uint32_t assets_init(void)
{
	const AssetDescriptor *desc;
	uint32_t offset, descAddr, screenCenterY;

	/* The 68000 may not have reached its publish point yet even though this
	 * SH2 is already running (mars_start.s starts both SH2s and the 68000
	 * concurrently, with no ordering guarantee between them). */
	while (!MARS_SYS_COMM2) {}

	/* By the time the ready flag is visible, the 68000's earlier writes to
	 * COMM12 and COMM6 are already committed: it writes them strictly
	 * before COMM2 (comm_boot_publish, md_src/comm.c), and the 68000 does
	 * not reorder its own bus writes. */
	offset = MARS_SYS_COMM12;
	screenCenterY = MARS_SYS_COMM6;

	descAddr = 0x880000u + offset;
	/* The descriptor table's own address is resolved through the same
	 * conversion function as every field inside it, never open-coded. */
	desc = (const AssetDescriptor *)md_addr_to_sh2(descAddr);

	g_ghz_map      = (const uint16_t *)md_addr_to_sh2(desc->ghz_map);
	g_ghz_collide  = (const uint8_t *)md_addr_to_sh2(desc->ghz_collide);
	g_sonic_frames = (const SonicFrame *)md_addr_to_sh2(desc->sonic_frames);
	g_sonic_anims  = (const SonicAnim *)md_addr_to_sh2(desc->sonic_anims);

	return screenCenterY;
}
