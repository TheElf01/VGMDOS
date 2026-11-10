#ifndef YM2203_OPL_H
#define YM2203_OPL_H

void ym2203opl_init(unsigned port, unsigned long clock, int num_channels);

void ym2203opl_write(int port, unsigned char addr, unsigned char data);

void ym2203opl_silence(void);

#endif
