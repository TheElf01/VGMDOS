#ifndef GBAPU_H
#define GBAPU_H

#include <dos.h>

typedef struct {
    unsigned long clock;
    unsigned long sample_rate;
    unsigned long clock_accum;
    unsigned long base_phase_pulse;
    unsigned long base_phase_wave;
    unsigned long ticks_cpu_precalc;
unsigned short phase_inc0;
    unsigned short phase_inc1;
    unsigned short phase_inc_wave;
    unsigned char duty[2];
    unsigned char len_halt[2];
    unsigned char env_vol[2];
    unsigned char env_dir[2];
    unsigned char env_period[2];
    unsigned short freq[2];
    unsigned char length[2];
    unsigned short phase[2];
    unsigned char sweep_period;
    unsigned char sweep_dir;
    unsigned char sweep_shift;
    unsigned char sweep_enabled;
    unsigned char sweep_neg_used;
    unsigned char sweep_timer;
    unsigned short sweep_freq;
    unsigned char env_start[2];
    unsigned char env_timer[2];
    unsigned char env_level[2];
    unsigned char env_enabled[2];
    unsigned char chan_on[2];
    unsigned char wave_dac_on;
    unsigned char wave_level;
    unsigned short wave_freq;
    unsigned short wave_length;
    unsigned short wave_phase;
    unsigned char wave_on;
    unsigned char wave_len_halt;
    unsigned char wave_ram[16];
unsigned char wave_ram_unpacked[32];
    unsigned char noise_env_vol, noise_env_dir, noise_env_period;
    unsigned char noise_shift_amt, noise_width7, noise_divisor_code;
    unsigned int noise_step;
    unsigned int noise_k;
    unsigned char noise_len_halt;
    unsigned char noise_length;
    unsigned int noise_idx;
    unsigned long noise_cycle_accum;
    unsigned char noise_env_start, noise_env_timer, noise_env_level;
    unsigned char noise_env_enabled;
    unsigned char noise_on;
    unsigned char noise_dac_on;
    unsigned char nr51;
    unsigned char power;
    long frame_counter;
    long frame_reload_precalc;
    unsigned char noise_toggle;
    unsigned char noise_last;
    int frame_step;
    unsigned int frame_q8;
    unsigned int frame_acc;
    unsigned int frame_left;
    unsigned char prev_out;
    int click_delta;
    int noise_lpf;
} GbApu;

void gbapu_init(GbApu *a, unsigned long clock, unsigned long sample_rate);

void gbapu_write_reg(GbApu *a, unsigned reg, unsigned char data);

void gbapu_run(GbApu *a, unsigned char far *out, unsigned long count);

void gbapu_free_noise_tables(void);

#endif
