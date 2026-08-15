#ifndef BG_H
#define BG_H

/* Green Hill's parallax background, drawn by the master SH2 on the 32X
 * framebuffer with line-table scrolling. See bg.c for the layout and the
 * per-frame technique. */

/* Reads the descriptor's bg_* addresses (md_addr_to_sh2()'d, same mechanism
 * as sh_src/assets.c). Call as the very first thing in m_main(), before
 * Hw32xInit: this spins on the same one-shot COMM2 flag assets_init() does
 * on the slave, and the two SH2s are otherwise unsynchronized, so reading it
 * before Hw32xInit's own multi-millisecond framebuffer clear leaves as
 * little window as possible for the race (see bg.c's resolve_assets()). */
void bg_assets_init(void);

/* Loads the palette and paints the initial framebuffer content into both
 * banks. Call once, after Hw32xInit has set 8bpp packed pixel mode and
 * after bg_assets_init(). */
void bg_init(void);

/* One frame's worth of parallax: rewrites the 224-entry line table for
 * whichever bank is the current write target, and rebases whichever lines
 * actually need it this frame (demand driven, capped, see bg.c). Call once
 * per frame, before Hw32xScreenFlip. */
void bg_frame(void);

#endif
