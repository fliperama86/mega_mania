#ifndef DESCRIPTOR_H
#define DESCRIPTOR_H

#include <stdint.h>

/* GHZ Act 1's FG Low layer, in 16x16 blocks: the one spelling of this number
 * on the 68000 side. main.c's streaming code needs it as a compile-time
 * constant (array indexing, the plane-window arithmetic), and the SH2 side
 * needs it too (collision, camera clamp) but has no compile-time visibility
 * into this program's build at all -- it can only learn it at runtime, via
 * the ghz_map_w/ghz_map_h fields below. main.c's MAP_W/MAP_H and
 * descriptor.c's struct literal both expand from these two macros, so
 * there is exactly one place to change the map size; nothing can be
 * changed here and left stale somewhere else because nothing else holds a
 * second copy of the number. Must match tools/convert_stage.py's mapW/mapH
 * arguments exactly. */
#define GHZ_MAP_W 1024
#define GHZ_MAP_H 128

/* GHZ's "BG Outside" parallax layer, in 16x16 blocks -- see the
 * bg_pal/bg_blocks/bg_map/bg_lines comment below. Read only by the master
 * SH2 (sh_src/bg.c), never by this program. */
#define BG_MAP_W 512
#define BG_MAP_H 24

/* Lets the slave SH2 reach the stage/character assets it cannot see any
 * other way: ghz_map/ghz_collide_index/ghz_collide_rows, cartridge bank 1
 * assets (tools/gen_assets.py's manifest) the 68000 reaches through the
 * generated pointers in md_src/assets_gen.h, and sonic_frames/sonic_anims,
 * small compiled tables that stayed linked into this 68000 program (md_src/
 * sonic_data.c's generated array literals). The SH2 has no visibility into
 * this program's symbol table or bank 1's generated pointers either, so it
 * needs the addresses handed to it explicitly; see sh_src/assets.h for the
 * other half of this mechanism, including how it tells the two kinds of
 * address in this struct apart.
 *
 * ghz_collide_index/ghz_collide_rows split what used to be one ghz_collide[]
 * array: tools/convert_stage.py now dedups identical 70-byte collision rows
 * (many blocks -- different tiles, or flip variants whose masks happen to be
 * symmetric -- share one), storing each unique row once in ghz_collide_rows
 * and one row-number-per-block in ghz_collide_index. Cut collide.bin from
 * 51,100 to 12,870 bytes on GHZ (163 unique rows for 730 blocks), which is
 * what brought the 68000 program back under its 512 KB ROM window.
 *
 * The five pointer fields (ghz_map, ghz_collide_index, ghz_collide_rows,
 * sonic_frames, sonic_anims) are raw 68000 addresses, deliberately uint32_t
 * rather than a typed pointer, so SH2 code cannot dereference one directly
 * without going through md_addr_to_sh2() (sh_src/assets.h). ghz_map_w and
 * ghz_map_h are not addresses at all, just GHZ_MAP_W/GHZ_MAP_H carried
 * across to the SH2; they stay uint32_t anyway rather than the narrower
 * uint16_t the values would fit in, so this struct's layout cannot come out
 * different between m68k-elf-gcc and sh-elf-gcc, which read the exact same
 * bytes through two unrelated compilers with no shared ABI to guarantee
 * that agreement -- a uniform field size sidesteps the question entirely.
 * asset_descriptor itself lives in ROM like every other constant table in
 * this codebase (ghz_pal and friends): a plain top-level const global
 * already lands in .rodata with this build's flags, which is exactly the
 * property that makes its own address meaningful to publish as a
 * cartridge-relative offset.
 *
 * bg_pal/bg_blocks/bg_map/bg_lines (tools/convert_bg.py's output, bank-1
 * assets the same way as ghz_map/ghz_collide_index/ghz_collide_rows above)
 * follow the identical convention: raw addresses,
 * converted through md_addr_to_sh2() the same way. The only difference is
 * who reads them -- the master SH2
 * (sh_src/m_main.c via sh_src/bg.c), not the slave, since the master owns
 * the framebuffer and the slave owns nothing background-related. bg_map is
 * BG_MAP_W x BG_MAP_H (below) blocks, fixed here rather than published at
 * runtime like ghz_map_w/ghz_map_h: nothing on the 68000 side indexes this
 * data at compile time, so unlike GHZ_MAP_W/GHZ_MAP_H there is only one
 * place this number can live, and this is it. Must match convert_bg.py's
 * own reported "map W x H".
 *
 * screenCenterY (SCREEN_HALF_H - 16, PAL/NTSC-aware) deliberately is NOT a
 * field here. Unlike the nine data pointers above, all of which are link-time
 * constants, screenCenterY is only known once vdp_init() has read the
 * PAL/NTSC hardware bit at runtime, and this struct's storage is genuinely
 * read-only ROM once the cartridge is flashed: an ordinary bus write to a
 * ROM/flash address does not update its contents on real hardware (and can
 * be actively unsafe against a flash chip's command sequences), so main()
 * could never legally write a late value into any field here no matter how
 * the struct were declared. screenCenterY is instead published once, at
 * boot, through the spare COMM6 register, which otherwise sits idle until
 * steady state begins, exactly the way COMM12 below already carries the
 * descriptor offset at boot before being reinterpreted for steady-state use.
 * See comm_boot_publish() in comm.c and assets_init() in sh_src/assets.c. */
typedef struct {
	uint32_t ghz_map;           /* address of ghz_map[] */
	uint32_t ghz_collide_index; /* address of ghz_collide_index[]: one u16
	                             * per block, row number into ghz_collide_rows */
	uint32_t ghz_collide_rows;  /* address of ghz_collide_rows[]: the
	                             * deduplicated 70-byte collision rows */
	uint32_t sonic_frames;      /* address of sonic_frames[] */
	uint32_t sonic_anims;       /* address of sonic_anims[] */
	uint32_t ghz_map_w;         /* GHZ_MAP_W, FG Low width in blocks */
	uint32_t ghz_map_h;         /* GHZ_MAP_H, FG Low height in blocks */
	uint32_t bg_pal;            /* address of bg_pal[]: 256 CRAM words, big endian */
	uint32_t bg_blocks;         /* address of bg_blocks[]: 16x16 8bpp blocks */
	uint32_t bg_map;            /* address of bg_map[]: BG_MAP_W x BG_MAP_H block indices */
	uint32_t bg_lines;          /* address of bg_lines[]: parallax/speed pair per bg row */
} AssetDescriptor;

extern const AssetDescriptor asset_descriptor;

#endif
