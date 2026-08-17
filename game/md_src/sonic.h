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

/* Upload the current frame. Call inside vblank. dispRot is the 3-bit snapped
 * rotation step (0-7) comm_read_frame returns (sh_src/comm.h's COMM6
 * repack); facing is Player.direction (0 right, 1 left), the same value
 * sonic_build's flip takes. Both matter here, not just for sonic_build:
 * for ROTCLASS_FULL frames facing selects which of the 45/135-degree baked
 * sets is the source (see sonic_build's comment), so uploading with the
 * wrong facing would DMA the mirror image of what sonic_build's piece
 * table actually points at. */
void sonic_upload(uint16_t frameIndex, uint8_t dispRot, uint8_t facing);

/* Fill sprite entries for the current frame at a screen position, linking them
 * so the caller can chain the rest of its list. Returns how many were used.
 * drawGroupHigh (0 low, 1 high) sets every piece's sprite priority --
 * Player.drawGroupHigh (sh_src/player.h), PlaneSwitch_CheckCollisions'
 * other write alongside collisionPlane (PlaneSwitch.c:94-109), the same
 * Zone->playerDrawGroup[0]/[1] mechanism the original uses to draw Sonic
 * above or below FG High.
 *
 * dispRot picks one of 8 baked orientations per md_src/sonic_rot_data.h's
 * sonic_rot_class[frameIndex]: ROTCLASS_NONE frames (rolling, skidding)
 * ignore it; ROTCLASS_R180 frames (idle, push, look up, crouch) flip both
 * axes when dispRot==4, upright otherwise; ROTCLASS_FULL frames (walk, jog,
 * run, dash, air walk) fold facing into the step and pick the base frame or
 * one of the three baked 45/90/135-degree sets, with flips composed on top
 * -- see this file's resolve_frame() for the exact table. */
uint16_t sonic_build(uint16_t frameIndex, uint8_t dispRot, int16_t sx, int16_t sy,
                     uint8_t flip, uint8_t drawGroupHigh, VDPSprite *list,
                     uint16_t firstLink);

#endif
