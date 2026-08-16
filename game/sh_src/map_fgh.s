! FG High layer (loop fronts, overhangs), produced by tools/convert_stage.py.
!
! Unlike every other converted asset (md_src/assets.s's .incbin lines), this
! one is not linked into the 68000 program: at 262,144 B it cannot fit that
! program's own 512 KB ROM window (see md_src/main.c's ghz_map_fgh_md
! comment). It is linked directly into this SH2 program instead, placed by
! sh_src/mars.ld's maphigh region at cartridge offset 0xC0000 -- a fixed
! address both CPUs reach with no bank switch. mars.ld's maphigh ORIGIN, this
! file's AT() load address and ghz_map_fgh_md's 0x9C0000 are a hand-synced
! trio; see mars.ld's comment for the other two.
!
! Same cell format as ghz_map (md_src/assets.s): u16 per cell, bits 0-11 a
! block index into the shared ghz_blocks[], bits 12-15 solidity. FG High
! shares FG Low's GHZ_MAP_W/GHZ_MAP_H (convert_stage.py asserts the two
! layers' scene dimensions match), so there is no separate width/height
! constant to publish for it.
!
! No .balign needed: maphigh's own ORIGIN (mars.ld) is 0x02080000, already
! word aligned, and this is the only thing in the section.

	.section .maphigh,"a",@progbits

! sh-elf-gcc prepends an underscore to every C-level symbol (see any C
! function's own name in a disassembly, or mars_start.s's _fast_memcpy and
! friends), so the label C's "extern const uint16_t ghz_map_fgh[]"
! (sh_src/path.c) resolves against has to carry one too, hand-written here
! the same way mars_start.s hand-writes its own C-visible symbols.
	.global _ghz_map_fgh
_ghz_map_fgh:
	.incbin "../assets/ghz/map_fgh.bin"
