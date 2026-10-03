#ifndef PCENGINE_H
#define PCENGINE_H

#include <dos.h>

#define PCE_CT_LEN 128

typedef struct {
    unsigned long clock;
    unsigned long sample_rate;
    unsigned long base_phase_wave;
    unsigned long base_phase_noise;
    unsigned char sel_channel;
    unsigned char global_bal;
    unsigned char freq_lo[6], freq_hi[6];
    unsigned char on[6], dda[6], vol[6], bal[6];
    unsigned char wave[6][32];
    unsigned char wave_widx[6];
    unsigned char dda_val[6];
    unsigned short phase[6];
    unsigned char noise_on[6], noise_freq[6];
    unsigned int noise_phase[6];
    unsigned int noise_lfsr[6];
    unsigned char noise_bit[6];
    int prev_out;
    int click_delta;
    unsigned char freq_dirty[6];
    unsigned char tab_dirty[6];
    unsigned char active_dirty;
    unsigned int c_phase_inc[6];
    unsigned int c_noise_phase_inc[6];
    int c_amp5[6];
    int c_noise_amp[6];
    signed char c_ct[6][PCE_CT_LEN];
    int c_dda_sum;
    int c_dda_part[6];
    const unsigned char far *bank;
    unsigned long bank_size;
    unsigned char st_on[6];
    unsigned long st_pos[6], st_end[6], st_step[6];
    signed char c_dlut[6][32];
    int c_st_active[6];
    int c_n_st;
    int c_wave_active[6];
    int c_n_wave_active;
    int c_noise_active[6];
    int c_n_noise_active;
} PceApu;

void pceapu_init(PceApu *a, unsigned long clock, unsigned long sample_rate);
void pceapu_write_reg(PceApu *a, unsigned int addr, unsigned char data);
void pceapu_run(PceApu *a, unsigned char far *out, unsigned long count);

void pceapu_set_bank(PceApu *a, const unsigned char far *bank, unsigned long size);
void pceapu_stream_freq(PceApu *a, int ch, unsigned long hz);
void pceapu_stream_start(PceApu *a, int ch, unsigned long off, unsigned long len);
void pceapu_stream_stop(PceApu *a, int ch);

#endif
