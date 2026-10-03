#ifndef FDS_H
#define FDS_H

#include <dos.h>

typedef struct {
    unsigned long clock;
    unsigned long sample_rate;
    unsigned long clock_accum;
    unsigned long clock_step;
    unsigned int tick_last;
    unsigned char wave[64];
    unsigned char mod_wave[64];
    unsigned int  freq_wav;
    unsigned int  freq_mod;
    unsigned long phase_wav;
    unsigned long phase_mod;
    unsigned char wav_write;
    unsigned char wav_halt;
    unsigned char env_halt;
    unsigned char mod_halt;
    int mod_pos;
    unsigned int mod_write_pos;
    unsigned char env_mode[2];
    unsigned char env_disable[2];
    unsigned long env_timer[2];
    unsigned char env_speed[2];
    int env_out[2];
    unsigned char master_env_speed;
    unsigned char master_io;
    unsigned char master_vol;
    long rc_accum;
    long rc_k, rc_l;
    long dc_avg;
    int prev_out;
    int click_delta;
    int last_mix;
} FdsApu;

void fds_init(FdsApu *f, unsigned long clock, unsigned long sample_rate);

void fds_write_reg(FdsApu *f, unsigned int addr, unsigned char data);

void fds_run(FdsApu *f, unsigned char far *out, unsigned long n, int mix_mode);

#endif
