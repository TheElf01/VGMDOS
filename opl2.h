#ifndef OPL2_H
#define OPL2_H

void opl2_init(unsigned port);

void opl2_write(unsigned char reg, unsigned char data);

void opl3_write_bank1(unsigned char reg, unsigned char data);
void opl_hard_silence(int bank1);

void opl_set_mute(unsigned long m);

void opl3_write_bank0_fast(unsigned char reg, unsigned char data);
void opl3_write_bank1_fast(unsigned char reg, unsigned char data);

#endif
