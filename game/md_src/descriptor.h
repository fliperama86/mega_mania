#ifndef DESCRIPTOR_H
#define DESCRIPTOR_H

#include <stdint.h>

/* Lets the slave SH2 reach the stage/character assets that are linked into
 * this 68000 program only: ghz_map/ghz_collide (md_src/assets.s's .incbin)
 * and sonic_frames/sonic_anims (md_src/sonic_data.c's generated array
 * literals). The SH2 has no visibility into this program's symbol table, so
 * it needs the addresses handed to it explicitly; see sh_src/assets.h for
 * the other half of this mechanism.
 *
 * Every field is a raw 68000 address, deliberately uint32_t rather than a
 * typed pointer, so SH2 code cannot dereference one directly without going
 * through md_addr_to_sh2() (sh_src/assets.h). asset_descriptor itself lives
 * in ROM like every other constant table in this codebase (ghz_pal and
 * friends): a plain top-level const global already lands in .rodata with
 * this build's flags, which is exactly the property that makes its own
 * address meaningful to publish as a cartridge-relative offset.
 *
 * screenCenterY (SCREEN_HALF_H - 16, PAL/NTSC-aware) deliberately is NOT a
 * field here. Unlike the four data pointers, all of which are link-time
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
	uint32_t ghz_map;       /* address of ghz_map[] */
	uint32_t ghz_collide;   /* address of ghz_collide[] */
	uint32_t sonic_frames;  /* address of sonic_frames[] */
	uint32_t sonic_anims;   /* address of sonic_anims[] */
} AssetDescriptor;

extern const AssetDescriptor asset_descriptor;

#endif
