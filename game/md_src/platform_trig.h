#ifndef MD_PLATFORM_TRIG_H
#define MD_PLATFORM_TRIG_H

#include <stdint.h>

/* 68000-side copy of sh_src/platform_trig.h's own tables -- same duplication
 * convention md_src/rings.c's k_sin256/k_cos256 already uses for the SH2's
 * trig.c (two CPUs, no shared memory, see rings.c's own comment on that
 * pair), just at RSDK's finer Sin1024/Cos1024/Sin512 resolution instead of
 * Sin256's. Values MUST be bit-for-bit identical to the SH2 side's own copy
 * -- Platform/Bridge's whole "drawn position derived identically on both
 * CPUs" guarantee (this batch's report) depends on both sides' Sin1024(x)
 * returning the exact same int16 for the exact same x, not just a
 * numerically-close one. See sh_src/platform_trig.h for the generation
 * formula and RSDK line references; both files were generated from the same
 * script run, not independently transcribed twice. */
extern const int16_t k_sin1024[1024];
extern const int16_t k_cos1024[1024];
extern const int16_t k_sin512[512];

#define platform_sin1024(a) (k_sin1024[(uint16_t)(a) & 0x3FFu])
#define platform_cos1024(a) (k_cos1024[(uint16_t)(a) & 0x3FFu])
#define platform_sin512(a)  (k_sin512[(uint16_t)(a) & 0x1FFu])

#endif
