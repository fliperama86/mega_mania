#ifndef __TEXT_H__
#define __TEXT_H__

#include "common.h"

/* Plane B text output. Font tiles and the palette 0 colours (black bg,
 * light gray fg) are already set up by md_start.s before main() runs. */
void cd_print(int line, const char *s);

/* Number/string formatting, ported from blitbench's sh_src/m_main.c so a
 * later status line (CD register values, sector counts, ...) reuses this
 * instead of new formatting code. */
int cd_put_uint(char *out, uint32_t v, int width);
int cd_put_hex(char *out, uint32_t v, int digits);
int cd_put_str(char *out, const char *s);

#endif
