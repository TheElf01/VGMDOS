#ifndef YM2413_OPL_H
#define YM2413_OPL_H

void ym2413opl_init(unsigned port);

void ym2413opl_write(unsigned char addr, unsigned char data);

void ym2413opl_silence(void);

#endif
