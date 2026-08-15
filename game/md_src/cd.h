#ifndef CD_H
#define CD_H

#include <stdint.h>

/* Mega CD Mode 1 music, ported from cdbench/md_src/cd.c with every
 * on-screen diagnostic removed: this ROM has no text console in its normal
 * path, so every step reports through a return value instead of cd_print().
 * Only the 68000 can reach the CD hardware; the SH-2s never touch it. */

/* Full bring-up: detect the CD BIOS, reset the sub-CPU, upload and start its
 * program, complete the handshake, and initialise the drive. Returns
 * nonzero if a CD unit is present and its sub-CPU program is running, zero
 * otherwise -- including "no CD hardware at all", the case on the actual
 * test hardware. Every wait is bounded, matching cdbench, so this returns
 * cleanly and promptly with no CD hardware to find. Call once, at boot. */
int cd_init(void);

/* Starts a track looping. Refuses (returns 0) if cd_init() never succeeded,
 * or if the BIOS status word says the tray is open or no disc is loaded --
 * the same check cdbench makes before it plays. */
int cd_music_play(uint16_t track);

/* Stops whatever track is playing. No-op if cd_init() never succeeded. */
void cd_music_stop(void);

/* The BIOS status word, or 0 if the sub-CPU did not answer. Bit 15 is its
 * not-ready flag and the high byte is the drive state, 0x01 being playing.
 * Diagnostic only: nothing in the game's normal path needs it. */
uint16_t cd_status_word(uint16_t *songs);
/* How many level 2 interrupts the sub-CPU has actually taken. Diagnostic, but
 * kept: the game drives INT2 by hand, and this counter standing still while
 * the game thinks it is poking is exactly the failure that silenced the music
 * once already. */
uint16_t cd_int2_count(void);

/* Sub-CPU level 2 interrupt, once per frame. The CD BIOS needs this at
 * roughly vblank rate to keep running; call it from the main loop at the
 * same point the frame already synchronises to vblank (see main.c). Safe to
 * call whether or not a CD was found -- see the comment on its definition. */
void cd_vblank(void);

#endif
