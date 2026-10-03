#ifndef WSAPU_H
#define WSAPU_H

#include <dos.h>

typedef struct {
    unsigned char wave_ram[64];
    unsigned int wave_base;
    unsigned short freq[4];
    unsigned short phase[4];
    unsigned char vol_l[4], vol_r[4];
    unsigned char ch_enable;
    signed char sweep_value;
    unsigned char sweep_ticks;
    long sweep_counter;
    unsigned char noise_ctrl;
    unsigned int noise_idx;
    unsigned long noise_cycle_accum;
    unsigned long clock;
    unsigned long sample_rate;
    unsigned long ticks_cpu_precalc;
    unsigned long clock_accum;
    int prev_out;
    int click_delta;
    unsigned char vt[4][32];
    unsigned char vt_dirty;
    unsigned int blk_addr[8];
    unsigned char blk_data[8][64];
    unsigned char blk_next;
} WsApu;

void wsapu_init(WsApu *a, unsigned long clock, unsigned long sample_rate);

void wsapu_write_port(WsApu *a, unsigned char port, unsigned char data);

void wsapu_write_mem(WsApu *a, unsigned int addr, unsigned char data);

void wsapu_run(WsApu *a, unsigned char far *out, unsigned long count);

void wsapu_free_noise_tables(void);

#endif
