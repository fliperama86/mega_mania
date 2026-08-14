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
