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

| A VRAM DMA that crosses a 128 KB boundary in ROM wraps back inside that
| block on real hardware. Frames are uploaded one DMA each, so the whole set
| starts on a boundary and stays under 128 KB, and no single frame can span
| one.
	.balign 131072
	.global sonic_tiles
sonic_tiles:
	.incbin "../assets/sonic/tiles.bin"
