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

/* ANI_SPRING_TWIRL/ANI_SPRING_DIAGONAL appended at the end (tools/
 * convert_sonic.py's ANIMATIONS list order): Sonic's "Spring Twirl" (10
 * frames) and "Spring Diagonal" (2 frames), Player.h's rotationFlag 0 for
 * both (verified against the pack, not assumed -- same ROTSTYLE_NONE class
 * as ANI_JUMP/ANI_SKID/ANI_SKID_TURN, so no baked rotated art). Used by
 * sh_src/spring.c's vertical/diagonal trigger effects (Spring.c:158,332) and
 * restored from by sh_src/player.c's air_state() (Player_State_Air,
 * Player.c:3890-3897).
 *
 * ANI_HURT/ANI_DIE appended after those two, same convention (tools/
 * convert_sonic.py's ANIMATIONS list order again): Sonic's "Hurt" (5 frames,
 * loop index 4 -- Ring.c-adjacent Player.c data, not Ring.c itself) and
 * "Die" (1 frame), both rotationFlag 0 (ROTSTYLE_NONE), verified against the
 * pack the same way the two spring poses were. Brought the exported total
 * to 124 of COMM_ANIM's 127-frame budget (sh_src/comm.h). Used by
 * sh_src/player.c's player_hit()/state_hurt()/player_kill()/state_death(). */
enum { ANI_IDLE, ANI_WALK, ANI_JOG, ANI_RUN, ANI_DASH, ANI_SKID, ANI_SKID_TURN, ANI_AIR_WALK, ANI_JUMP, ANI_PUSH, ANI_LOOK_UP, ANI_CROUCH, ANI_SPRING_TWIRL, ANI_SPRING_DIAGONAL, ANI_HURT, ANI_DIE, SONIC_ANIM_COUNT };

/* Hand-kept mirror of md_src/sonic_data.h's generated SONIC_FRAME_COUNT --
 * same "must be updated by hand to match" rule as this file's own top
 * comment. Used by sh_src/s_main.c to publish COMM_ANIM's out-of-range
 * "do not draw Sonic this tick" sentinel (sh_src/comm.h's COMM_ANIM entry,
 * sh_src/player.h's Player.hidden). */
#define SONIC_FRAME_COUNT 124

#endif
