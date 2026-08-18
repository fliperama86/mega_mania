OUTPUT_ARCH(m68k)
SEARCH_DIR(.)

MEMORY {
	rom (rx) : ORIGIN = 0x00880000, LENGTH = 4M
	ram (wx) : ORIGIN = 0x00FF0000, LENGTH = 64K
}

PROVIDE (__stack = 0x01000000);

SECTIONS {
	.text 0x008803F0 : {
		__text_start = .;
		*(.text.keepboot)
		*(.text .text.*)
		*(.rodata .rodata.*)
		/* Pad the SECTION, not just the symbol: .data's AT() below places
		 * its load image at LOADADDR(.text) + SIZEOF(.text), and _start's
		 * copy loop reads from __text_end. A bare ALIGN here only rounds
		 * the symbol, so an odd-sized .text put the two one byte apart and
		 * every initialized variable arrived byte-shifted (the 2026-08-17
		 * full-roster boot death). Advancing the location counter pads the
		 * section itself, keeping SIZEOF(.text) and __text_end in step. */
		. = ALIGN(2);
		__text_end = .;
	} > rom
	__text_size = SIZEOF(.text);

	.data 0x00FF0000 : AT(LOADADDR(.text) + SIZEOF(.text)) {
		__data_start = .;
		*(.data .data.*)
		/* Same section-vs-symbol trap as .text above: _start copies
		 * __data_size in words, so an odd SIZEOF(.data) would silently
		 * drop the last byte. */
		. = ALIGN(2);
		__data_end = .;
	} > ram
	__data_size = SIZEOF(.data);

	.bss 0x00FF0000 + SIZEOF(.data) : {
		__bss_start = . ;
		*(.bss .bss.*)
		__bss_end = ALIGN(2);
	} > ram
	__bss_size = SIZEOF(.bss);
}
