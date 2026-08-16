! GHZ collision path B (RSDK TileConfig path 1): the block->row index and its
! deduplicated 70-byte rows, produced by tools/convert_stage.py alongside
! path A's collide_index.bin/collide_rows.bin -- see that script's docstring
! for the row layout (unchanged from path A, just a second, independently
! deduplicated table) and sh_src/path.c for how PathEntity.collisionPlane
! picks between the two tables at runtime.
!
! Path A rides the 68000 program and reaches this CPU through the descriptor
! table (md_src/assets.s's .incbin, sh_src/assets.c's md_addr_to_sh2()) for
! historical reasons: collide_index/collide_rows started out 68000-only
! data, back before this CPU had any collision code of its own. But
! collision is read only by the slave SH2 (sh_src/path.c) -- the 68000 never
! touches ghz_collide_index/ghz_collide_rows, or will ever touch this file's
! symbols either. Path B has no such history and no reason to start one: it
! links straight into this SH2 program's own image instead, the same
! linked-array shape as sh_src/map_fgh.s, but into the ordinary `rom` region
! (sh_src/mars.ld), not the fixed-address `maphigh` region map_fgh.s needs.
! map_fgh.s's fixed address exists only because the 68000 *also* reaches
! that data, at a hand-synced pointer (main.c's ghz_map_fgh_md comment);
! nothing on the 68000 side ever reaches this file's data, so there is no
! pointer to keep in sync and no reason to spend any of the 512 KB cartridge
! window path A's descriptor route still costs the 68000 program. mars.ld's
! rom region has room to spare for it (see that file's own headroom
! comment), so this just lands whereever the linker naturally places it
! alongside the rest of this program's .text/.rodata -- no MEMORY/SECTIONS
! change needed.
!
! sh-elf-gcc prepends an underscore to every C-level symbol (map_fgh.s's own
! comment), so the labels sh_src/path.c's "extern const uint16_t
! ghz_collide_b_index[]"/"extern const uint8_t ghz_collide_b_rows[]" resolve
! against carry one too.
!
! Index-before-rows, same order as md_src/assets.s's path A pair: the index
! is read as u16 (sh_src/path.c), so it needs 2-byte alignment -- not
! otherwise guaranteed, since this fragment's start offset depends on
! whatever other .rodata precedes it at final link time, same reasoning as
! assets.s's own .balign 2 before ghz_collide_index. Rows is read a byte at
! a time, so it carries no alignment requirement of its own and needs no
! balign between the two.

	.section .rodata,"a",@progbits

	.balign 2
	.global _ghz_collide_b_index
_ghz_collide_b_index:
	.incbin "../assets/ghz/collide_b_index.bin"

	.global _ghz_collide_b_rows
_ghz_collide_b_rows:
	.incbin "../assets/ghz/collide_b_rows.bin"
