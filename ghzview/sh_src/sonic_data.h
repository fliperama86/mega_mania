#ifndef SH_SONIC_DATA_H
#define SH_SONIC_DATA_H

/* Hand-kept mirror of md_src/sonic_data.h's SonicFrame/SonicAnim layout and
 * ANI_* enum, generated there by tools/convert_sonic.py. This is a
 * type-schema duplication only, never a data duplication: the SH2 never
 * links sonic_frames[]/sonic_anims[], it only ever reads them through
 * runtime pointers assets.c resolves from the 68000's descriptor table (see
 * assets.h), pointing at the one copy of the data that lives in the 68000's
 * ROM image. If md_src/sonic_data.h's generated schema ever changes, this
 * file has to be updated by hand to match.
 *
 * SonicPiece is deliberately not mirrored here: it is rendering-only
 * (sprite piece assembly), which stays entirely on the MD side. */

#include <stdint.h>

typedef struct {
    uint16_t tileOffset;  /* into sonic_tiles, in tiles */
    uint16_t pieceOffset; /* into sonic_pieces */
    uint8_t  tileCount;
    uint8_t  pieceCount;
    int8_t   pivotX, pivotY;
    uint16_t duration;    /* RSDK frame duration, paired with the animator speed */
    int8_t   outerLeft, outerTop, outerRight, outerBottom;
    int8_t   innerLeft, innerTop, innerRight, innerBottom;
} SonicFrame;

typedef struct {
    uint16_t first;       /* first frame index */
    uint8_t  count;
    uint8_t  loop;
    int16_t  speed;
} SonicAnim;

enum { ANI_IDLE, ANI_WALK, ANI_JOG, ANI_RUN, ANI_DASH, ANI_SKID, ANI_SKID_TURN, ANI_AIR_WALK, ANI_JUMP, ANI_PUSH, ANI_LOOK_UP, ANI_CROUCH, SONIC_ANIM_COUNT };

#endif
