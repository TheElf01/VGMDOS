#ifndef PSG_FULL_H
#define PSG_FULL_H

#include <dos.h>

typedef struct {
    unsigned long clock;
    unsigned long sample_rate;
    unsigned int  ticks_int;
    unsigned char ticks_frac;
    unsigned short tone_freq[3];
    long  tone_counter[3];
    unsigned char tone_output[3];
    unsigned char tone_volume[3];
    unsigned short noise_shift;
    unsigned char  noise_ctrl;
    unsigned char  noise_volume;
    long  noise_counter;
    unsigned char noise_output;
    unsigned char latched_channel;
    unsigned char latched_is_volume;
    unsigned long clock_accum;
} PsgFull;

void psg_full_init(PsgFull *p, unsigned long clock, unsigned long sample_rate);
void psg_full_write(PsgFull *p, unsigned char data);
void psg_full_run(PsgFull *p, unsigned char far *out, unsigned long count);
int psg_full_silent(const PsgFull *p);

void psg_full_set_vol(unsigned vol256);

typedef struct {
    unsigned short sample_offset;
    unsigned char  data;
} PsgWrite;

void psg_full_run_batched(PsgFull *p, unsigned char far *out, unsigned long total_samples,
                          const PsgWrite *writes, int n_writes);

void psg_full_run_t6w28(PsgFull *chip_tone, PsgFull *chip_noise,
                         unsigned char far *out, unsigned long count);

extern unsigned char g_psg_mute;
#endif
