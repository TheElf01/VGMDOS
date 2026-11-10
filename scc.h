#ifndef SCC_H
#define SCC_H

#define SCC_CT_LEN 256

#include <dos.h>

typedef struct {
    unsigned long clock;
    unsigned long sample_rate;
    signed char waveform[5][32];
    unsigned short freq[5];
    long  phase_counter[5];
    unsigned char wave_pos[5];
    unsigned char volume[5];
    unsigned char enable[5];
    unsigned long clock_accum;
    unsigned int ticks_int;
    unsigned char ticks_frac;
    unsigned char toggle;
    int last_mix;
    unsigned char ct_dirty;
} Scc;

void scc_init(Scc *s, unsigned long clock, unsigned long sample_rate);
void scc_write_reg(Scc *s, unsigned addr, unsigned char data);
void scc_run(Scc *s, unsigned char far *out, unsigned long count);

void scc_set_gain(int solo);

void scc_run_additive(Scc *s, unsigned char far *out, unsigned long count);

#endif
