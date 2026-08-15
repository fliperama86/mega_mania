#include "md.h"
#include "pad.h"

/* The port control registers decide which pins are outputs. Until TH is one,
 * writing 0x40 to the data register does nothing to the line and the reads
 * below are whatever the pin happens to float to, which is why this has to run
 * before the first pad_read. Emulators tend to hide it by driving TH anyway. */
void pad_init(void)
{
	*((volatile uint8_t *)0xA10009) = 0x40;   /* port 1 control: TH output */
	*((volatile uint8_t *)0xA1000B) = 0x40;   /* port 2 */
	*((volatile uint8_t *)0xA1000D) = 0x40;   /* ext */
	*((volatile uint8_t *)0xA10003) = 0x40;
}

/* Standard 3-button read. TH high gives C, B and the d-pad; TH low gives
 * Start and A. Everything is active low, so it is inverted on the way out. */
uint16_t pad_read(void)
{
	volatile uint8_t *port = (volatile uint8_t *)0xA10003;
	uint8_t hi, lo;

	*port = 0x40;
	__asm__ volatile("nop"); __asm__ volatile("nop");
	hi = *port;

	*port = 0x00;
	__asm__ volatile("nop"); __asm__ volatile("nop");
	lo = *port;

	*port = 0x40;

	return (uint16_t)(~(((lo & 0x30) << 2) | (hi & 0x3F)) & 0xFF);
}
