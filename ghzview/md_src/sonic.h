#ifndef SONIC_H
#define SONIC_H

/* Sonic's sprite, on Mega Drive hardware sprites.
 *
 * A frame is a handful of hardware sprites over a contiguous block of tiles,
 * so showing a frame is one DMA into a fixed VRAM window plus a few sprite
 * table entries. That is how the classic games did it and it is why the whole
 * sheet does not have to fit in VRAM: only the largest single frame does.
 *
 * The tables come from tools/convert_sonic.py, see sonic_data.h. */

#include "md.h"
#include "sonic_data.h"

/* RSDK's Animator, cut down to one character. Timing is RSDK's:
 * ProcessAnimation adds speed each frame and steps while it exceeds the
 * current frame's duration. */
typedef struct {
	uint16_t anim;
	uint16_t frameID;      /* index within the animation */
	int16_t  timer;
	int16_t  speed;
	int16_t  duration;
} Animator;

/* Reserve the VRAM window the current frame is uploaded into. Takes the first
 * free tile and returns the next one after the window. */
uint16_t sonic_gfx_init(uint16_t firstTile);

void sonic_set_anim(Animator *a, uint16_t anim, uint8_t force, uint16_t frameID);
void sonic_process_anim(Animator *a);

const SonicFrame *sonic_frame(const Animator *a);

/* Upload the current frame. Call inside vblank. */
void sonic_upload(const Animator *a);

/* Fill sprite entries for the current frame at a screen position, linking them
 * so the caller can chain the rest of its list. Returns how many were used. */
uint16_t sonic_build(const Animator *a, int16_t sx, int16_t sy, uint8_t flip,
                     VDPSprite *list, uint16_t firstLink);

#endif
