| Sub-CPU program, minimal port of
| ~/Projects/references/SegaCDMode1PCM/cd/crt.s
| MIT License, Copyright (c) 2023 Mikael Kalms
|
| Stripped to just the header, the SPInit/SPMain/SPInt2 entry points the CD
| BIOS dispatches, and a comm loop: ping, BIOS status, drive init and CD
| audio playback (play/stop a track). No PCM, no ADPCM, no sound sources --
| that is still the next piece of work, not this one.

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
	beq	CmdPing
	cmpi.b	#'S,0x800E.w
	beq	CmdStatus
	cmpi.b	#'D,0x800E.w
	beq	CmdDriveInit
	cmpi.b	#'A,0x800E.w
	beq	CmdPlayAudio
	cmpi.b	#'X,0x800E.w
	beq	CmdStopAudio

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
	move.w	16(a0),0x8022.w		/* first song, last song: the TOC as the
					 * BIOS reads it, which is the only way to
					 * tell "will not play" from "cannot see a
					 * track to play". SegaCDMode1PCM's
					 * GetDiscInfo returns the same word. */
	move.b	#'S,0x800F.w		/* sub comm port = done, status */
	bra.b	WaitAck

CmdDriveInit:
	lea	drive_init_parms(pc),a0
	move.w	#0x0010,d0		/* DRVINIT, see SegaCDMode1PCM/cd/crt.s:44 */
	jsr	0x5F22.w		/* call CDBIOS function */

	/* The fader has to be opened by hand or the drive plays into silence.
	 * It is the CD-DA attenuator (gate array 0xFF8034), it is not left at
	 * full by whatever ran before this program, and nothing else here
	 * touches it. Bit 15 selects master volume, 0x400 is the maximum of
	 * the 0..1024 range. Same call and same value as the reference's own
	 * init, SegaCDMode1PCM/cd/crt.s:47. */
	move.w	#0x0085,d0		/* FDRSET - set audio volume */
	move.w	#0x8400,d1		/* master volume, full */
	jsr	0x5F22.w		/* call CDBIOS function */

	move.w	#0x0089,d0		/* CDCSTOP - stop reading data, crt.s:51 */
	jsr	0x5F22.w		/* call CDBIOS function */

	/* Interrupt mask, sub side: bit 2 is the level 2 the 68000 sends us and
	 * bit 4 is the CDD interrupt the BIOS services the drive from. The mask
	 * belongs to the program, not the BIOS, and this program never set it,
	 * so both were left at whatever survived boot. Measured with an INT2
	 * counter published to the 68000: it reached 19 and stopped, while the
	 * game went on poking once a frame. */
	ori.b	#0x14,0x8033.w

	move.b	#'D,0x800F.w		/* sub comm port = done, drive init */
	bra	WaitAck

CmdPlayAudio:
	move.w	#0x0002,d0		/* MSCSTOP - stop playing, see SegaCDMode1PCM/cd/crt.s:130 */
	jsr	0x5F22.w		/* call CDBIOS function */

	move.w	0x8010.w,d1		/* argument word: track number */
	lea	track_number(pc),a0
	move.w	d1,(a0)
	move.w	#0x0013,d0		/* MSCPLAYR - play with repeat, see SegaCDMode1PCM/cd/crt.s:138 */
	jsr	0x5F22.w		/* call CDBIOS function */

	move.b	#'A,0x800F.w		/* sub comm port = done, play audio */
	bra	WaitAck

CmdStopAudio:
	move.w	#0x0002,d0		/* MSCSTOP - stop playing, see SegaCDMode1PCM/cd/crt.s:151 */
	jsr	0x5F22.w		/* call CDBIOS function */
	move.b	#'X,0x800F.w		/* sub comm port = done, stop audio */
	bra	WaitAck

| Sub-CPU Program VBlank (INT02) Service Handler

SPInt2:
	/* Counts every level 2 the BIOS passes on, published where the 68000
	 * can read it. This ROM drives INT2 by hand, so "is it arriving at
	 * all" is a question worth being able to answer from the screen. */
	addq.w	#1,0x8026.w
	rts

| Sub-CPU program Reserved Function

SPNull:
	rts

| Sub-CPU variables

	.align	2
drive_init_parms:
	.byte	0x01, 0xFF		/* first track (1), last track (all) */

track_number:
	.word	0

	.global	_start
_start:
