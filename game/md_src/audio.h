#ifndef AUDIO_H
#define AUDIO_H

/* Ported from blitbench/md_src/md_main.c. Puts the PSG, YM2612 and Z80 into
 * a known-quiet state; see docs/hardware-budget.md, section 6, "Silence the
 * audio hardware at boot". Call once, early in boot, before anything that
 * might rely on the audio hardware already being quiet -- CD music above
 * all, which is what this keeps from starting out under boot-time noise. */
void audio_silence(void);

#endif
