#ifndef YM3812_OPL_H
#define YM3812_OPL_H

void ym3812opl_init(unsigned port);

void ym3812opl_write(unsigned char addr, unsigned char data);

void ym3526opl_write(unsigned char addr, unsigned char data);

void y8950opl_write(unsigned char addr, unsigned char data);

void ymf262opl_write(int bank, unsigned char addr, unsigned char data);

void ym3812opl_silence(void);
void ymf262opl_silence(void);

#endif
