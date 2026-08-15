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

	.global ghz_collide
ghz_collide:
	.incbin "../assets/ghz/collide.bin"

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
