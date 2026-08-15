#ifndef MD_H
#define MD_H

#include <stddef.h>
#include <stdint.h>

#include "string.h"
#include "vdp.h"

/* enable_ints is gone: on 32X the vector table belongs to
 * sh_src/mars_start.s, not this program, and Technical Bulletin 9 forbids
 * taking an interrupt while RV is set (see docs/hardware-budget.md,
 * section 3). main() polls the VDP status instead, same as it already does
 * for vsync. hard_reset() is gone too: it jumped to _hard_reset in the old
 * MD boot file, which 32X does not have. */
#define disable_ints __asm__("move #0x2700,%sr")
#endif
