#ifndef AY8910_H
#define AY8910_H

#include <dos.h>

typedef struct {
    unsigned long clock;
    unsigned long sample_rate;
    unsigned short tone_period[3];
    long  tone_counter[3];
    unsigned char tone_output[3];
    unsigned char noise_period;
    unsigned long noise_shift;
    long  noise_counter;
    unsigned char noise_output;
    unsigned char mixer;
    unsigned char vol_reg[3];
    unsigned short env_period;
    unsigned char  env_shape;
    long  env_counter;
    unsigned char env_level;
    unsigned char env_dir;
    unsigned char env_holding;
    unsigned long clock_accum;
    unsigned long tq8;
    unsigned short c_per[3], c_inc[3];
    unsigned char c_np; unsigned int c_nstep;
    unsigned short c_ep; unsigned long c_einc;
    unsigned char vt_s[16], et_s[32];
    const unsigned char *vol_table;
    const unsigned char *env_table;
    unsigned char mute;
} Ay8910;

#define AY8910_CHIP_AY     0
#define AY8910_CHIP_YM2149 1

void ay8910_init(Ay8910 *a, unsigned long clock, unsigned long sample_rate, int chip_type);

void ay8910_write_reg(Ay8910 *a, unsigned reg, unsigned char data);

void ay8910_set_gain(Ay8910 *a, unsigned g256);

void ay8910_run(Ay8910 *a, unsigned char far *out, unsigned long count);

#endif
