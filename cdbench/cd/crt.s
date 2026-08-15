| Sub-CPU program, minimal port of
| ~/Projects/references/SegaCDMode1PCM/cd/crt.s
| MIT License, Copyright (c) 2023 Mikael Kalms
|
| Stripped to just the header, the SPInit/SPMain/SPInt2 entry points the CD
| BIOS dispatches, and a two-command comm loop (ping and BIOS status). No
| PCM, no ADPCM, no sound sources, no drive access -- that is the next piece
| of work, not this one.

	.text

| Standard MegaCD Sub-CPU Program Header (copied to 0x6000)

SPHeader:
	.asciz	"MAIN-SUBCPU"
	.word	0x0001,0x0000
	.long	0x00000000
	.long	0x00000000
	.long	SPHeaderOffsets-SPHeader
	.long	0x00000000

SPHeaderOffsets:
	.word	SPInit-SPHeaderOffsets
	.word	SPMain-SPHeaderOffsets
	.word	SPInt2-SPHeaderOffsets
	.word	SPNull-SPHeaderOffsets
	.word	0x0000

| Sub-CPU Program Initialization (VBlank not enabled yet)

SPInit:
	move.b	#'I,0x800F.w		/* sub comm port = INITIALIZING */
	andi.b	#0xE2,0x8003.w		/* Priority Mode = off, 2M mode, Sub-CPU has access */
	rts

| Sub-CPU Program Main Entry Point (VBlank now enabled)

SPMain:
	move.b	#0,0x800F.w		/* sub comm port = READY */

| wait for command in main comm port
WaitCmd:
	tst.b	0x800E.w
	beq.b	WaitCmd
	cmpi.b	#'P,0x800E.w
	beq.b	CmdPing
	cmpi.b	#'S,0x800E.w
	beq.b	CmdStatus

	move.b	#'E,0x800F.w		/* sub comm port = ERROR */
WaitAck:
	tst.b	0x800E.w
	bne.b	WaitAck			/* wait for result acknowledged */
	move.b	#0,0x800F.w		/* sub comm port = READY */
	bra.b	WaitCmd

CmdPing:
	move.w	0x8010.w,d0		/* argument word */
	not.w	d0			/* ones complement */
	move.w	d0,0x8020.w		/* result word */
	move.b	#'P,0x800F.w		/* sub comm port = done, ping */
	bra.b	WaitAck

CmdStatus:
	move.w	#0x0081,d0		/* CDBSTAT */
	jsr	0x5F22.w		/* call CDBIOS function */
	move.w	0(a0),0x8020.w		/* BIOS status word */
	move.b	#'S,0x800F.w		/* sub comm port = done, status */
	bra.b	WaitAck

| Sub-CPU Program VBlank (INT02) Service Handler

SPInt2:
	rts

| Sub-CPU program Reserved Function

SPNull:
	rts

	.global	_start
_start:
