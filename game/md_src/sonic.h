#ifndef SONIC_H
#define SONIC_H

/* Sonic's sprite, on Mega Drive hardware sprites. Rendering only: the
 * animator (which anim is playing, which frame) now lives on the slave SH2
 * (sh_src/sonic_anim.c/.h), which publishes the absolute frame index once
 * per tick over the comm protocol (see sh_src/comm.h). This file looks that
 * index up directly in the linked-in sonic_frames[]/sonic_pieces[], which
 * live in this program's own ROM image, no descriptor lookup needed.
 *
 * A frame is a handful of hardware sprites over a contiguous block of tiles,
 * so showing a frame is one DMA into a fixed VRAM window plus a few sprite
 * table entries. That is how the classic games did it and it is why the whole
 * sheet does not have to fit in VRAM: only the largest single frame does.
 *
 * The tables come from tools/convert_sonic.py, see sonic_data.h. */

#include "md.h"
#include "sonic_data.h"

/* Reserve the VRAM window the current frame is uploaded into. Takes the first
 * free tile and returns the next one after the window. */
uint16_t sonic_gfx_init(uint16_t firstTile);

/* Upload the current frame. Call inside vblank. */
void sonic_upload(uint16_t frameIndex);

/* Fill sprite entries for the current frame at a screen position, linking them
 * so the caller can chain the rest of its list. Returns how many were used. */
uint16_t sonic_build(uint16_t frameIndex, int16_t sx, int16_t sy, uint8_t flip,
                     VDPSprite *list, uint16_t firstLink);

#endif
