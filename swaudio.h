#ifndef SWAUDIO_H
#define SWAUDIO_H

typedef enum {
    SWAUDIO_LPT1 = 1,
    SWAUDIO_LPT2 = 2,
    SWAUDIO_LPT3 = 3,
    SWAUDIO_LPT4 = 4,
    SWAUDIO_SPEAKER = 5
} SwaudioMode;

unsigned swaudio_detect_lpt(int n);

int swaudio_start(SwaudioMode mode, unsigned sample_rate,
                   const unsigned char far *buffer, unsigned buffer_len,
                   unsigned lpt_port);

void swaudio_set_write_pos(unsigned write_pos);

void swaudio_stop(void);

#endif
