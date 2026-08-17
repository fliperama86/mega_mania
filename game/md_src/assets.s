| Converted Green Hill assets, produced by tools/convert_stage.py

	.section .rodata
	.align 2

	.global ghz_pal
ghz_pal:
	.incbin "../assets/ghz/pal.bin"

	.global ghz_tiles
	.global ghz_tiles_end
ghz_tiles:
	.incbin "../assets/ghz/tiles.bin"
ghz_tiles_end:

	.global ghz_blocks
ghz_blocks:
	.incbin "../assets/ghz/blocks.bin"

	.global ghz_map
ghz_map:
	.incbin "../assets/ghz/map_fg.bin"

	.global ghz_bgmap
ghz_bgmap:
	.incbin "../assets/ghz/map_bg.bin"

| collide_rows holds each distinct 70-byte collision row once (tools/
| convert_stage.py dedups them); collide_index has one u16 per block, the row
| number into collide_rows for that block. collide_index is read as u16 by
| sh_src/path.c, which needs it 2-byte aligned -- not otherwise guaranteed,
| same reasoning as bg_blocks' .balign 4 below. collide_rows itself is read
| a byte at a time (path.c indexes it as const uint8_t *), so it carries no
| alignment requirement of its own.
	.balign 2
	.global ghz_collide_index
ghz_collide_index:
	.incbin "../assets/ghz/collide_index.bin"

	.global ghz_collide_rows
ghz_collide_rows:
	.incbin "../assets/ghz/collide_rows.bin"

	.global sonic_pal
sonic_pal:
	.incbin "../assets/sonic/pal.bin"

	.global sonic_tiles
sonic_tiles:
	.incbin "../assets/sonic/tiles.bin"

| Green Hill's parallax background, produced by tools/convert_bg.py. Read
| only by the master SH2 (sh_src/bg.c), via the descriptor like everything
| else above.
	.global bg_pal
bg_pal:
	.incbin "../assets/ghzbg/bg_pal.bin"

| bg_blocks is read a longword at a time by sh_src/bg.c's draw_strip() (see
| its own comment), which needs this symbol 4-byte aligned -- not otherwise
| guaranteed, since it just follows whatever .incbin happens to precede it
| above at whatever length that file is.
	.balign 4
	.global bg_blocks
bg_blocks:
	.incbin "../assets/ghzbg/bg_blocks.bin"

	.global bg_map
bg_map:
	.incbin "../assets/ghzbg/bg_map.bin"

	.global bg_lines
bg_lines:
	.incbin "../assets/ghzbg/bg_lines.bin"

| Rings: entirely 68000-side (game/md_src/rings.c), plain incbin like
| everything else above -- deliberately NOT added to AssetDescriptor
| (descriptor.h), since the architecture keeps rings invisible to the SH2.
|
| ghz_ring_xy is read as int16_t by rings.c, which needs it 2-byte aligned --
| not otherwise guaranteed, same reasoning as ghz_collide_index's .balign 2
| above. ghz_ring_count is rings.bin's own leading big-endian u16 count:
| rings.c checks it against its own compile-time RING_COUNT at boot and
| disables rings on a mismatch (same "cannot go stale in one file and not
| another" reasoning as GHZ_MAP_W/GHZ_MAP_H travelling through the
| descriptor table, descriptor.h) instead of trusting two independently
| edited numbers to agree.
	.balign 2
	.global ghz_ring_count
	.global ghz_ring_xy
ghz_ring_count:
	.incbin "../assets/ghz/rings.bin"
ghz_ring_xy = ghz_ring_count + 2

| Ring/sparkle hardware-sprite tiles, produced by tools/convert_ring.py.
| Read as uint32_t by vdp_tiles_load the same way ghz_tiles/sonic_tiles
| above are -- 68000 access only needs an even address (unlike the SH2's
| bg_blocks below, which does need .balign 4), and rings.bin immediately
| above is an even number of bytes, so no separate balign is needed here.
	.global ring_tiles
ring_tiles:
	.incbin "../assets/ring/tiles.bin"

| Sonic's per-frame hitbox 0, produced by tools/convert_sonic.py. Read a
| byte at a time (rings.c indexes it as const int8_t *), so it carries no
| alignment requirement of its own.
	.global sonic_hitbox
sonic_hitbox:
	.incbin "../assets/sonic/hitbox.bin"
