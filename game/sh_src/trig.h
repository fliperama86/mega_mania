#ifndef TRIG_H
#define TRIG_H

extern const int16_t sin256_table[256];
extern const int16_t cos256_table[256];

#define sin256(a) (sin256_table[(uint8_t)(a)])
#define cos256(a) (cos256_table[(uint8_t)(a)])

/* RSDK's Cos1024: angle is 0-1023 over a full turn, result scaled to 1024
 * (RSDK::cos1024LookupTable, dependencies/RSDKv5/RSDKv5/RSDK/Core/Math.cpp:
 * 7,55,62-65 -- `(int32)(cosf((i/512.f)*PI)*1024.f)` for every i, with the
 * four quadrant boundaries [0x000]=1024,[0x100]=0,[0x200]=-1024,[0x300]=0
 * hardcoded exact rather than left to sinf/cosf's own rounding, exactly as
 * Math.cpp itself does). This port's only consumer is sh_src/
 * corkscrew_path.c (CorkscrewPath_Update's `RSDK.Cos1024(...)`, GHZ/
 * CorkscrewPath.c) -- sin256_table/cos256_table above are a DIFFERENT,
 * coarser (256-step) table used everywhere else in this port and cannot
 * stand in for this one: reusing them at 1/4 the angular resolution would
 * visibly chunk the corkscrew's sine path, a real position (not just
 * cosmetic-rotation) computation. Generated bit-for-bit from the same
 * formula (a small one-off C program using actual `cosf`, not a Python
 * float64 approximation -- see this task's own report for how), not hand
 * typed, same "Generated, not hand written" provenance trig.c's own
 * sin256_table/cos256_table already claim for themselves. */
extern const int16_t cos1024_table[1024];
#define cos1024(a) (cos1024_table[(uint16_t)(a) & 0x3FF])

#endif
