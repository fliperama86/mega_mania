OUTPUT_ARCH(sh)
SEARCH_DIR(.)

/* The memory map look like this:
 * +--------------------+ <- 0x02000000
 * | .text              |
 * |        _etext      |
 * |        ctor list   | the ctor and dtor lists are for
 * |        dtor list   | C++ support
 * +--------------------+
 * .                    .
 * .                    .
 * .                    .
 * +--------------------+ <- 0x06000000
 * | .data              | initialized data goes here
 * |        _edata      |
 * +--------------------+
 * | .bss               |
 * |        __bss_start | start of bss, cleared by crt0
 * |        _end        | start of heap, used by sbrk()
 * +--------------------+
 * .                    .
 * .                    .
 * .                    .
 * |        __stack     | top of stack (for Master SH2)
 * +--------------------+ <- 0x0603FC00
 */

/* rom is capped just ahead of sonicrot below, so the SH2 program's
 * .text/.data load image overflowing becomes a hard link error instead of
 * silently landing on top of the rotated-sprite data. The 68000's stricter
 * limit, its program staying inside the fixed 512 KB cartridge window, is
 * enforced separately by the M68K_CODE_END ASSERT at the bottom of this
 * script, since rom's LENGTH now runs past 512 KB. */
MEMORY {
	rom (rx) : ORIGIN = 0x02000000, LENGTH = 0x00094000

	/* Sonic's baked rotated frames (assets/sonic/rot_tiles.bin, sh_src/
	 * sonic_rot.s), placed right after rom's shrunken end so both CPUs reach
	 * them with no bank switch -- the SH2 sees them directly at this ORIGIN,
	 * and the 68000 sees the same bytes at the fixed pointer 0x994000
	 * (md_src/sonic.c's sonic_rot_tiles_md). This ORIGIN, that pointer and
	 * sonic_rot.s's AT() load address are a hand-synced trio: change one,
	 * change all three -- same convention as maphigh's own trio just below. */
	sonicrot (r) : ORIGIN = 0x02094000, LENGTH = 0x0002C000

	ram (wx) : ORIGIN = 0x06000000, LENGTH = 0x0003FC00

	/* FG High's map (assets/ghz/map_fgh.bin, sh_src/map_fgh.s), too big for
	 * the 68000's own ROM window so it is linked here instead: cartridge
	 * offset 0xC0000, right after sonicrot above, ending exactly at the 1 MB
	 * boundary the 68000's bank 0 covers. Both CPUs reach this
	 * address with no bank switch -- the SH2s see it directly at this
	 * ORIGIN, and the 68000 sees the same bytes at the fixed pointer
	 * 0x9C0000 (md_src/main.c's ghz_map_fgh_md). This ORIGIN, that pointer
	 * and map_fgh.s's AT() load address are a hand-synced trio: change one,
	 * change all three. */
	maphigh (r) : ORIGIN = 0x020C0000, LENGTH = 0x00040000
}

PROVIDE (__stack = 0x0603FC00);

SECTIONS {
	.text 0x02000000 : AT(0x00000000) {
		*(.text .text.*)
		. = ALIGN(0x4);

		__CTOR_LIST__ = .;
		___CTOR_LIST__ = .;
		LONG((__CTOR_END__ - __CTOR_LIST__) / 4 - 2)
		*(.ctors)
		LONG(0)
		__CTOR_END__ = .;
		__DTOR_LIST__ = .;
		___DTOR_LIST__ = .;
		LONG((__DTOR_END__ - __DTOR_LIST__) / 4 - 2)
		*(.dtors)
		LONG(0)
		__DTOR_END__ = .;
		*(.rodata .rodata.*)
		*(.gcc_except_table .gcc_except_table.*)

		. = ALIGN(0x10);
		__INIT_SECTION__ = . ;
		*(.init)
		SHORT (0x000B)	/* rts */
		SHORT (0x0009)	/* nop */

		. = ALIGN(0x10);
		__FINI_SECTION__ = . ;
		*(.fini)
		SHORT (0x000B)	/* rts */
		SHORT (0x0009)	/* nop */

		_etext = .;
		*(.lit)
		. = ALIGN(0x4);
	} > rom
	_stext = SIZEOF (.text);

	/* rot_tiles.bin, whole and unparsed -- see MEMORY's sonicrot comment
	 * above. AT(0x00094000) keeps its load address contiguous with .text's
	 * LMA range (AT(0x00000000) above) in the flat .32x image, right after
	 * rom's shrunken 0x94000, matching maphigh's own AT() convention just
	 * below. KEEP: nothing else references this section by name, so without
	 * it a garbage-collecting link could drop it. */
	.sonicrot 0x02094000 : AT(0x00094000) {
		KEEP(*(.sonicrot))
	} > sonicrot

	/* map_fgh.bin, whole and unparsed -- see MEMORY's maphigh comment above.
	 * AT(0x00080000) keeps its load address contiguous with .text's LMA
	 * range (AT(0x00000000) above) in the flat .32x image, right after
	 * rom's 512 KB, rather than defaulting to its VMA (which would blow the
	 * image out to gigabytes of padding). KEEP: nothing else references
	 * this section by name, so without it a garbage-collecting link could
	 * drop it. */
	.maphigh 0x020C0000 : AT(0x000C0000) {
		KEEP(*(.maphigh))
	} > maphigh

	.data 0x06000000 : AT(LOADADDR(.text) + SIZEOF(.text)) {
		*(.got.plt) *(.got)
		*(.shdata)
		*(.data .data.*)
		_edata = .;
		. = ALIGN(0x4);
	} > ram
	_sdata = SIZEOF (.data);

	.bss 0x06000000 + SIZEOF(.data) :
	{
		__bss_start = . ;
		*(.shbss)
		*(.bss .bss.*)
		*(COMMON)
		_end =  ALIGN (0x8);
		__end = _end;
		end = _end;
	} > ram

	.eh_frame 0 (NOLOAD) :
	{
		*(.eh_frame)
	}
}

/* The 68000-visible program (mars_start.s's M68K_CODE incbin) must stay
 * inside the 68000's fixed 512 KB cartridge window; rom's LENGTH stopped
 * guaranteeing that when it grew past 512 KB for maphigh's sake. */
ASSERT(M68K_CODE_END <= 0x02080000, "68000 program spills past its fixed 512 KB cartridge window")
