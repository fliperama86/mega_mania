#ifndef ASSETS_H
#define ASSETS_H

/* How the slave SH2 reaches the stage/character assets that are linked into
 * the 68000 program only (ghz_map/ghz_collide via md_src/assets.s's
 * .incbin, sonic_frames/sonic_anims via md_src/sonic_data.c's generated
 * array literals). The assets themselves never move and are never
 * duplicated: this file only resolves runtime pointers to the one copy that
 * lives in the 68000's ROM image, reached through the SH2's own cartridge
 * window. */

#include <stdint.h>

/* Hand-kept mirror of md_src/descriptor.h's AssetDescriptor: same field
 * order, same uint32_t types (so this side cannot accidentally dereference a
 * field without going through md_addr_to_sh2() below). Keep in sync by hand
 * if descriptor.h's field list ever changes.
 *
 * screenCenterY is not a field here either, for the same reason it is not
 * one in descriptor.h: it is a runtime-only value and this struct's ROM
 * storage cannot be written to after the cartridge is flashed. It travels
 * over the spare COMM6 register instead; see assets_init()'s implementation
 * and descriptor.h's comment for the full reasoning. */
typedef struct {
	uint32_t ghz_map;       /* address of ghz_map[], as a raw 68000 address */
	uint32_t ghz_collide;   /* address of ghz_collide[] */
	uint32_t sonic_frames;  /* address of sonic_frames[] */
	uint32_t sonic_anims;   /* address of sonic_anims[] */
} AssetDescriptor;

/* The 68000 and the SH2 see the same cartridge flash through two different
 * windows. The 68000's program links at 0x880000 and up, so its own
 * addresses (and every address published to us through the descriptor
 * table) are offsets from that base. The SH2 reaches the identical flash
 * through its own cache-through cartridge window at 0x22000000. Converting
 * one 68000 address into the matching SH2 address is always this one
 * subtraction and addition; every pointer that crosses from the 68000 side
 * must go through this function, never open-coded elsewhere. */
static inline const void *md_addr_to_sh2(uint32_t md_addr)
{
	return (const void *)(md_addr - 0x880000u + 0x22000000u);
}

/* Spin-waits on the descriptor-ready flag (COMM2), reads the descriptor
 * table's own offset from COMM12 (still its one-shot 32-bit boot role at
 * this point, before the post-boot per-frame protocol reinterprets that
 * address as two 16-bit halves), resolves every address field through
 * md_addr_to_sh2(), and stores the results for path.c and sonic_anim.c to
 * use (g_ghz_map, g_ghz_collide, g_sonic_frames, g_sonic_anims). Returns
 * screenCenterY unconverted. Call once, before the game loop starts. */
uint32_t assets_init(void);

#endif
