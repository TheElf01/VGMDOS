#ifndef NESAPU_H
#define NESAPU_H

#include <dos.h>

typedef struct {
    unsigned long clock;
    unsigned long sample_rate;
    unsigned long clock_accum;
    unsigned long base_phase_pulse;
    unsigned long base_phase_tri;
    unsigned long ticks_apu_precalc;
    unsigned char duty[2];
    unsigned char len_halt[2];
    unsigned char const_vol[2];
    unsigned char vol_param[2];
    unsigned short period[2];
    unsigned char length[2];
    unsigned short phase[2];
    unsigned char sweep_enable[2];
    unsigned char sweep_period[2];
    unsigned char sweep_negate[2];
    unsigned char sweep_shift[2];
    unsigned char sweep_reload[2];
    unsigned char sweep_divider[2];
    unsigned char env_start[2];
    unsigned char env_divider[2];
    unsigned char env_decay[2];
    unsigned char tri_halt;
    unsigned char tri_reload_val;
    unsigned char tri_linear_counter;
    unsigned char tri_reload_flag;
    unsigned short tri_period;
    unsigned char tri_length;
    unsigned short tri_phase;
    unsigned char noise_halt;
    unsigned char noise_const_vol;
    unsigned char noise_vol_param;
    unsigned char noise_mode;
    unsigned char noise_period_idx;
    unsigned char noise_length;
    unsigned int noise_idx;
    unsigned int noise_cycle_accum;
    unsigned char noise_env_start;
    unsigned char noise_env_divider;
    unsigned char noise_env_decay;
    unsigned char enable_mask;
    unsigned char output_mask;
    unsigned char frame_mode;
    long frame_counter;
    long frame_reload_precalc;
    int hw_noise;
    int frame_step;
    unsigned int frame_q8;
    unsigned int frame_acc;
    unsigned int frame_left;
    unsigned long base_q8;
    const unsigned char far *dmc_ram;
    unsigned char dmc_loop, dmc_rate_idx, dmc_level, dmc_out;
    unsigned char dmc_addr_reg, dmc_len_reg, dmc_sr, dmc_bits;
    unsigned int dmc_cur, dmc_left;
    unsigned int dmc_inc, dmc_acc;
    int dmc_playing;
    const unsigned char far *pcm;
    int st_on;
    unsigned int st_idx, st_frac, st_si, st_sf, st_left;
    unsigned char boff;
    unsigned char prev_out;
    int click_delta;
} NesApu;

void nesapu_init(NesApu *a, unsigned long clock, unsigned long sample_rate);

void nesapu_write_reg(NesApu *a, unsigned reg, unsigned char data);

void nesapu_run(NesApu *a, unsigned char far *out, unsigned long count);

void nesapu_set_ram(NesApu *a, const unsigned char far *ram);

#define NES_PCM_RING 32768U
void nesapu_set_pcm(NesApu *a, const unsigned char far *ring);
void nesapu_stream_freq(NesApu *a, unsigned long hz);
void nesapu_stream_start(NesApu *a, unsigned long off, unsigned long len);
void nesapu_stream_stop(NesApu *a);

const unsigned char far *nesapu_get_noise_table(int mode, unsigned int *period);

const unsigned short *nesapu_get_period_table(void);

void nesapu_free_noise_tables(void);

#endif
