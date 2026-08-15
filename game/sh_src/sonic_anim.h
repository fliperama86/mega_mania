#ifndef SONIC_ANIM_H
#define SONIC_ANIM_H

/* Sonic's animator, split off md_src/sonic.c: game logic (which anim is
 * playing, which frame, hitbox lookup) moves to the SH2 with the physics
 * that drives it; sprite-building stays on the 68000 in md_src/sonic.c,
 * which has direct linked-in access to sonic_frames/sonic_pieces/sonic_tiles
 * since those symbols live in its own link. This side never links that data,
 * it only ever reads through the runtime pointers assets.c resolves. */

#include <stdint.h>
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

void sonic_set_anim(Animator *a, uint16_t anim, uint8_t force, uint16_t frameID);
void sonic_process_anim(Animator *a);

/* Used internally and by player.c for hitbox extraction (replaces the old,
 * MD-side sonic_frame()). */
const SonicFrame *sonic_anim_frame(const Animator *a);

/* The absolute index into sonic_frames[], published once per tick in the
 * comm protocol's packed anim word (see sh_src/comm.h) so the 68000 can look
 * the frame up in its own linked-in sonic_frames[] for sprite building. */
uint16_t sonic_anim_frame_index(const Animator *a);

#endif
