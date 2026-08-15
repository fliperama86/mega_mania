| Sub-CPU program blob: ../cd/crt.s, assembled and linked flat at 0x6000 by
| the top-level Makefile, then flattened to cd/cd.bin. The 68000 copies the
| bytes between these two labels to PRG RAM 0x426000 (cd.c step 6). The
| Makefile ties this object to cd/cd.bin explicitly, so a rebuild of the
| sub-CPU program always re-embeds it here -- without that dependency the ROM
| would silently ship a stale sub-CPU program.

	.section .rodata

	.global Sub_Start
	.global Sub_End
Sub_Start:
	.incbin "cd/cd.bin"
Sub_End:
