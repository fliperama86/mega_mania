#ifndef ASSETS_H
#define ASSETS_H

/* How the slave SH2 reaches the stage/character assets it cannot see any
 * other way: ghz_map/ghz_collide_index/ghz_collide_rows, cartridge bank 1
 * assets (tools/gen_assets.py's manifest) the 68000 reaches through
 * generated fixed pointers, and sonic_frames/sonic_anims, small compiled
 * tables still linked into the 68000 program (md_src/sonic_data.c's
 * generated array literals). The assets themselves never move and are never
 * duplicated: this file only resolves runtime pointers to the one copy that
 * lives in the cartridge image, reached through the SH2's own cartridge
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
	uint32_t ghz_map;           /* address of ghz_map[], as a raw 68000 address */
	uint32_t ghz_collide_index; /* address of ghz_collide_index[]: one u16
	                             * per block, row number into ghz_collide_rows */
	uint32_t ghz_collide_rows;  /* address of ghz_collide_rows[]: the
	                             * deduplicated 70-byte collision rows */
	uint32_t sonic_frames;      /* address of sonic_frames[] */
	uint32_t sonic_anims;       /* address of sonic_anims[] */
	uint32_t ghz_map_w;         /* FG Low width, in 16x16 blocks -- not an
	                             * address, read directly, no md_addr_to_sh2() */
	uint32_t ghz_map_h;         /* FG Low height, in 16x16 blocks */
	uint32_t bg_pal;            /* address of bg_pal[], read by bg.c instead */
	uint32_t bg_blocks;         /* address of bg_blocks[] */
	uint32_t bg_map;            /* address of bg_map[] */
	uint32_t bg_lines;          /* address of bg_lines[] */
} AssetDescriptor;

/* The 68000 and the SH2 see the same cartridge flash through different
 * windows, and which subtraction/addition converts one into the other now
 * depends on which of the 68000's TWO windows a given address falls in
 * (md_src/bank.h):
 *
 *  - Its FIXED window, 0x880000-0x8FFFFF, always cartridge 0x000000-
 *    0x07FFFF: still where this program's own small compiled tables live
 *    (sonic_frames/sonic_anims, md_src/sonic_data.c -- everything else that
 *    used to live here moved to bank 1, tools/gen_assets.py's manifest).
 *  - Its BANKED window, 0x900000-0x9FFFFF, cartridge (bank * 0x100000) and
 *    up depending which bank is currently selected. Every asset this
 *    function is ever asked to convert that is NOT one of the two fixed-
 *    window fields above lives in bank 1, cartridge 0x100000-0x1FFFFF, and
 *    main.c's boot sequence selects bank 1 exactly once and never switches
 *    away (md_src/bank.h's own comment) -- so a banked-window address is
 *    always, structurally, a bank-1 address for as long as this program
 *    runs, and needs no runtime "which bank is live" check to convert
 *    correctly.
 *
 * The SH2 reaches the identical flash through its own cache-through
 * cartridge window, base 0x22000000 for cartridge offset 0 -- so a fixed-
 * window address converts by subtracting 0x880000 (back to cartridge
 * offset 0) then adding that base, and a banked-window address converts by
 * subtracting 0x900000 (back to cartridge offset 0 -- of BANK 1, not the
 * cartridge) then adding 0x22100000 (the SH2 address of cartridge offset
 * 0x100000, i.e. that base plus bank 1's own cartridge offset). Every
 * pointer that crosses from the 68000 side must go through this function,
 * never open-coded elsewhere. */
static inline const void *md_addr_to_sh2(uint32_t md_addr)
{
	if (md_addr >= 0x900000u && md_addr <= 0x9FFFFFu)
		return (const void *)(md_addr - 0x900000u + 0x22100000u);
	return (const void *)(md_addr - 0x880000u + 0x22000000u);
}

/* Map dimensions, in 16x16 blocks, narrowed from the descriptor's
 * ghz_map_w/ghz_map_h by assets_init(). path.c's collision and s_main.c's
 * camera clamp read these instead of keeping their own #define, which is
 * what used to let the two go out of sync with the converted data (see
 * descriptor.h's GHZ_MAP_W/GHZ_MAP_H comment). path.c declares its own
 * matching extern rather than including this file, same as it does for
 * g_ghz_map/g_ghz_collide_index/g_ghz_collide_rows below. */
extern uint16_t g_map_w, g_map_h;

/* Spin-waits on the descriptor-ready flag (COMM2), reads the descriptor
 * table's own offset from COMM12 (still its one-shot 32-bit boot role at
 * this point, before the post-boot per-frame protocol reinterprets that
 * address as two 16-bit halves), resolves every address field through
 * md_addr_to_sh2(), and stores the results for path.c and sonic_anim.c to
 * use (g_ghz_map, g_ghz_collide_index, g_ghz_collide_rows, g_sonic_frames,
 * g_sonic_anims, g_map_w, g_map_h). Returns screenCenterY unconverted. Call
 * once, before the game loop starts. */
uint32_t assets_init(void);

#endif
