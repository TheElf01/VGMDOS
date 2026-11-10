#ifndef RF5C164_H
#define RF5C164_H

#include <stdio.h>

int  rf5c_init(unsigned long clock, unsigned long sample_rate);
void rf5c_free(void);
void rf5c_write(unsigned reg, unsigned data);
void rf5c_mem(unsigned addr, unsigned data);
void rf5c_ram_begin(unsigned addr);
void rf5c_ram_data(const unsigned char *src, unsigned n);
int  rf5c_bank_load(FILE *f, long offset, unsigned long size);
void rf5c_copy(unsigned long src, unsigned dst, unsigned long n);
void rf5c_set_half(int on);
void rf5c_set_boost(int on);
void rf5c_set_rate(unsigned long sample_rate);

void rf5c_run(unsigned char far *out, unsigned n);

void rf5c_run_add(unsigned char far *out, unsigned n);

unsigned rf5c_active(void);

void rf5c_set_mute(unsigned m);
void rf5c_run16(const unsigned char far *in8, int far *out16, unsigned n);

extern int g_awe_fwd;
int  rf5c_awe_enable(unsigned port, unsigned kb);
int  rf5c_awe_on(void);
void rf5c_awe_stats(unsigned *nres, unsigned long *kb);
void rf5c_awe_table_begin(void);
void rf5c_awe_table_data(const unsigned char *p, unsigned n);
void rf5c_awe_table_done(void);
void rf5c_awe_hw_ring(unsigned v);
void rf5c_awe_hw_reg(unsigned reg, unsigned data);

#endif

extern int g_awe_ptrim;
extern int g_awe_gain;
