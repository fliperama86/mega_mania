/* cdbench master SH-2. Only the 68000 can reach the Mega CD hardware, so
 * this side has nothing to do yet; it just idles so the ROM boots cleanly
 * while md_src/md_main.c gets built out with the real Mode 1 work. */

#include "mars.h"

int m_main(void)
{
	for (;;) ;
	return 0;
}
