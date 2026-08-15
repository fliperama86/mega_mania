/* Ported from blitbench/md_src/md_main.c. Nothing in this ROM made sound
 * before CD music, so nothing ever put the audio hardware in a known-quiet
 * state. Left alone the PSG powers up audible, the YM2612 comes up with
 * undefined register contents, and the Z80 runs loose out of uninitialised
 * RAM writing garbage into both -- see docs/hardware-budget.md, section 6. */

#include <stdint.h>
#include "audio.h"

#define Z80_BUS_REQ ((volatile uint16_t *)0xA11100)
#define Z80_RESET   ((volatile uint16_t *)0xA11200)

static void ym_write(uint8_t part, uint8_t reg, uint8_t val) {
	volatile uint8_t *ym = (volatile uint8_t *)0xA04000;
	while (ym[0] & 0x80) ;            /* wait for not-busy */
	ym[part ? 2 : 0] = reg;
	while (ym[0] & 0x80) ;
	ym[part ? 3 : 1] = val;
}

void audio_silence(void) {
	volatile uint8_t *psg = (volatile uint8_t *)0xC00011;
	uint8_t part, op, ch;

	/* Attenuation 15 (silent) on all four PSG channels */
	psg[0] = 0x9F;
	psg[0] = 0xBF;
	psg[0] = 0xDF;
	psg[0] = 0xFF;

	/* Take the Z80 bus so the 68000 can reach the YM2612. Bit 0 reads back
	 * set while the Z80 still holds it; bit 8 is the request we just wrote
	 * and never clears, so waiting on that hangs the 68000. */
	*Z80_BUS_REQ = 0x100;
	while (*Z80_BUS_REQ & 0x0001) ;

	ym_write(0, 0x22, 0x00);         /* LFO off */
	ym_write(0, 0x27, 0x00);         /* normal timer mode */
	ym_write(0, 0x2B, 0x00);         /* DAC off */

	for (part = 0; part < 2; part++) {
		for (ch = 0; ch < 3; ch++) {
			for (op = 0; op < 4; op++)
				ym_write(part, 0x40 + (op << 2) + ch, 0x7F);  /* max attenuation */
		}
	}

	/* Key off every operator on all six channels */
	for (ch = 0; ch < 3; ch++) {
		ym_write(0, 0x28, ch);
		ym_write(0, 0x28, 4 + ch);
	}

	/* Then hold the Z80 in reset so it cannot undo any of it */
	*Z80_RESET = 0x000;
	*Z80_BUS_REQ = 0x000;
}
