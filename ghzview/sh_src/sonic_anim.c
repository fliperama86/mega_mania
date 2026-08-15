#include "sonic_anim.h"

/* Filled in by assets_init() before s_main.c's game loop starts; see
 * assets.h. These point at the one copy of the data, in the 68000's ROM
 * image, through the cartridge window md_addr_to_sh2() maps in. */
extern const SonicFrame *g_sonic_frames;
extern const SonicAnim *g_sonic_anims;

const SonicFrame *sonic_anim_frame(const Animator *a)
{
	return &g_sonic_frames[g_sonic_anims[a->anim].first + a->frameID];
}

uint16_t sonic_anim_frame_index(const Animator *a)
{
	return g_sonic_anims[a->anim].first + a->frameID;
}

/* SetSpriteAnimation: without force, re-selecting the running animation is a
 * no-op, which is what lets the state code set the same animation every frame
 * without restarting it. */
void sonic_set_anim(Animator *a, uint16_t anim, uint8_t force, uint16_t frameID)
{
	if (a->anim == anim && !force) return;

	a->anim = anim;
	a->frameID = frameID;
	a->timer = 0;
	a->speed = g_sonic_anims[anim].speed;
	a->duration = g_sonic_frames[g_sonic_anims[anim].first + frameID].duration;
}

void sonic_process_anim(Animator *a)
{
	const SonicAnim *an = &g_sonic_anims[a->anim];

	a->timer += a->speed;
	while (a->timer > a->duration) {
		a->frameID++;
		a->timer -= a->duration;
		if (a->frameID >= an->count) a->frameID = an->loop;
		a->duration = g_sonic_frames[an->first + a->frameID].duration;
	}
}
