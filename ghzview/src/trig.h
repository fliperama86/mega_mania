#ifndef TRIG_H
#define TRIG_H

extern const int16_t sin256_table[256];
extern const int16_t cos256_table[256];

#define sin256(a) (sin256_table[(uint8_t)(a)])
#define cos256(a) (cos256_table[(uint8_t)(a)])

#endif
