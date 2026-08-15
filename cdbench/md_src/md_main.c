/* cdbench 68000 side. Only the 68000 can reach the Mega CD hardware, so
 * every later Mode 1 bring-up step lands here; for now this is just the
 * minimum needed to prove text lands on screen. The SH-2s (sh_src) stay
 * idle in this skeleton. */

#include "common.h"
#include "text.h"
#include "cd.h"

static volatile uint16_t* const vdp_data_port = (uint16_t*) VDP_DATA_PORT;
static volatile uint32_t* const vdp_ctrl_wide = (uint32_t*) VDP_CTRL_PORT;

/* Name Table B, matching InitVDPRegs in md_start.s (reg 0x84 = 0xE000 /
 * 0x2000). Its palette 0 is already black bg / light gray fg from that
 * file's CRAM setup, so plain tile indices are enough for readable text. */
#define PLANE_B_ADDR  0xE000
#define PLANE_B_COLS  64
#define TEXT_ROWS     28

static void vdp_vram_addr(uint16_t addr) {
	*vdp_ctrl_wide = (((uint32_t)(0x4000 | (addr & 0x3FFF))) << 16)
	               | ((addr >> 14) & 3);
}

/* Font tile layout from font.s: 0 = space, 1 = fallback glyph, 2-11 =
 * '0'-'9', 12-37 = 'A'-'Z' (lowercase folds onto the same glyphs, the font
 * has no separate lowercase set). */
static uint16_t char_tile(char c) {
	if (c >= '0' && c <= '9') return (uint16_t)(c - '0' + 2);
	if (c >= 'A' && c <= 'Z') return (uint16_t)(c - 'A' + 12);
	if (c >= 'a' && c <= 'z') return (uint16_t)(c - 'a' + 12);
	if (c == ' ') return 0;
	return 1;
}

void cd_print(int line, const char *s) {
	vdp_vram_addr((uint16_t)(PLANE_B_ADDR + (line & 31) * PLANE_B_COLS * 2));
	while (*s) *vdp_data_port = char_tile(*s++);
}

/* ---- formatting helpers, ported verbatim from blitbench's sh_src/m_main.c ---- */

int cd_put_uint(char *out, uint32_t v, int width) {
	char tmp[12];
	int n = 0, i;

	do {
		tmp[n++] = '0' + (v % 10);
		v /= 10;
	} while (v);

	for (i = n; i < width; i++) *out++ = ' ';
	for (i = n - 1; i >= 0; i--) *out++ = tmp[i];
	return (width > n ? width : n);
}

int cd_put_hex(char *out, uint32_t v, int digits) {
	static const char hx[] = "0123456789ABCDEF";
	int i;
	for (i = 0; i < digits; i++)
		out[i] = hx[(v >> ((digits - 1 - i) * 4)) & 0xF];
	return digits;
}

int cd_put_str(char *out, const char *s) {
	int n = 0;
	while (*s) out[n++] = *s++;
	return n;
}

/* VRAM is not guaranteed clear at power-on; wipe the visible rows of Plane B
 * once so nothing stray shows up around the text. */
static void cd_clear_screen(void) {
	uint16_t row, col;
	vdp_vram_addr(PLANE_B_ADDR);
	for (row = 0; row < TEXT_ROWS; row++)
		for (col = 0; col < PLANE_B_COLS; col++)
			*vdp_data_port = 0;
}

void main(void) {
	cd_clear_screen();
	cd_print(0, "CDBENCH MODE 1 BRINGUP");
	cd_run();
}
