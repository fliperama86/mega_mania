! Sonic's baked rotated sprite frames (45/90/135 degrees, WALK/JOG/RUN/DASH/
! AIR_WALK only), produced by tools/convert_sonic.py. See md_src/
! sonic_rot_data.h for the per-baked-frame table this data is indexed by.
!
! Unlike every other converted asset (md_src/assets.s's .incbin lines), this
! one is not linked into the 68000 program: sh_src/mars.ld shrinks rom's
! LENGTH specifically to make room for it right after the SH2 program's own
! .text/.data, at cartridge offset 0x94000. Both CPUs reach that address with
! no bank switch -- the SH2 sees it directly at this ORIGIN, and the 68000
! sees the same bytes at the fixed pointer 0x994000 (md_src/sonic.c's
! sonic_rot_tiles_md). mars.ld's sonicrot ORIGIN, this file's AT() load
! address and that pointer are a hand-synced trio; see mars.ld's comment for
! the other two -- same convention sh_src/map_fgh.s already uses for maphigh.
!
! Same 4bpp tile packing as sonic_tiles (assets/sonic/tiles.bin, md_src/
! assets.s): 8 big-endian u32s per 8x8 tile, left pixel high nibble.
!
! No .balign needed: sonicrot's own ORIGIN (mars.ld) is 0x02094000, already
! word aligned, and this is the only thing in the section.

	.section .sonicrot,"a",@progbits

! sh-elf-gcc prepends an underscore to every C-level symbol (see map_fgh.s's
! own comment on the same convention), so the label C's "extern const
! uint32_t sonic_rot_tiles_sh[]" would resolve against has to carry one too --
! though nothing on this CPU currently reads it (rotation display is
! 68000-side only, per this port's architecture), the label is kept for
! symmetry with map_fgh.s/collide_b.s and as a debugging anchor in the .lst.
	.global _sonic_rot_tiles_sh
_sonic_rot_tiles_sh:
	.incbin "../assets/sonic/rot_tiles.bin"
