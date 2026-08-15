#ifndef PAD_H
#define PAD_H

#define PAD_UP     0x01
#define PAD_DOWN   0x02
#define PAD_LEFT   0x04
#define PAD_RIGHT  0x08
#define PAD_B      0x10
#define PAD_C      0x20
#define PAD_A      0x40
#define PAD_START  0x80

void pad_init(void);
uint16_t pad_read(void);

#endif
