#ifndef PLATFORM_TRIG_H
#define PLATFORM_TRIG_H

#include <stdint.h>

/* RSDK::Sin1024/Cos1024/Sin512 (dependencies/RSDKv5/RSDKv5/RSDK/Core/
 * Math.hpp:52-53, Math.cpp:54-70,72-88): angle is 0-1023 (Sin1024/Cos1024) or
 * 0-511 (Sin512) over a full turn, result scaled to +-1024/+-512. A SEPARATE,
 * higher-resolution table from sh_src/trig.c's own Sin256/Cos256 (0-255,
 * scaled to 256) -- RSDK genuinely ships three independent-resolution trig
 * tables, and Platform_State_Linear/Swing/Circular (Platform.c) and
 * Bridge_Draw/Bridge_HandleCollisions (Bridge.c) are this port's first
 * consumers of the two finer ones, so neither existed here before this
 * batch. Generated, not hand-written (same convention trig.c's own header
 * comment states for Sin256/Cos256): Math.cpp:55-56's own formula,
 * `(int32)(sinf/cosf((i/(N/2)) * PI) * N)` for N=1024 (i=0..1023) or N=512
 * (i=0..511) -- a C `(int32)` cast TRUNCATES toward zero, not rounds, and
 * Math.cpp:62-70/80-88 overrides the four quarter-turn indices (0, N/4, N/2,
 * 3N/4) to their exact values rather than trust the float sample there --
 * both behaviors reproduced exactly by the generator script (not
 * hand-typed) that produced platform_trig.c/md_src/platform_trig.c's
 * identical tables. */
extern const int16_t k_sin1024[1024];
extern const int16_t k_cos1024[1024];
extern const int16_t k_sin512[512];

#define platform_sin1024(a) (k_sin1024[(uint16_t)(a) & 0x3FFu])
#define platform_cos1024(a) (k_cos1024[(uint16_t)(a) & 0x3FFu])
#define platform_sin512(a)  (k_sin512[(uint16_t)(a) & 0x1FFu])

#endif
