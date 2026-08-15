#include "md.h"
#include "pad.h"

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
