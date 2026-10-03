#include <conio.h>
#include "opl2.h"

static unsigned g_opl_port = 0x388;

unsigned long g_opl_mute = 0;
static unsigned char sh_b0[2][9], sh_bd, sh_used1;

static unsigned char bd_mask(void)
{
    unsigned char m = 0;
    if (!(sh_bd & 0x20)) return 0;
    if (g_opl_mute & 0x40) m |= 0x10;
    if (g_opl_mute & 0x80) m |= 0x09;
    if (g_opl_mute & 0x100) m |= 0x06;
    return m;
}

#define OPL_FILT(bank, reg, data) \
    if ((reg & 0xF0) == 0xB0) { \
        if (reg <= 0xB8) { \
            sh_b0[bank][reg - 0xB0] = data; \
            if (bank) sh_used1 = 1; \
            if (g_opl_mute & (1UL << ((bank) * 9 + reg - 0xB0))) data &= 0xDF; \
        } else if (reg == 0xBD && !(bank)) { sh_bd = data; data &= (unsigned char)~bd_mask(); } \
    }

void opl2_init(unsigned port)
{
    g_opl_port = port;
}

static void opl2_delay(int reads)
{
    int i;
    for (i = 0; i < reads; i++)
        (void)inp(g_opl_port);
}

static void opl2_raw(unsigned char reg, unsigned char data);
static void opl3_raw1(unsigned char reg, unsigned char data);

void opl_set_mute(unsigned long m)
{
    unsigned long old = g_opl_mute;
    int i;
    g_opl_mute = m;
    for (i = 0; i < 18; i++) {
        unsigned long b = 1UL << i;
        if ((old ^ m) & b) {
            unsigned char v = sh_b0[i / 9][i % 9];
            if (m & b) v &= 0xDF;
            if (i < 9) opl2_raw((unsigned char)(0xB0 + i), v);
            else if (sh_used1) opl3_raw1((unsigned char)(0xB0 + i - 9), v);
        }
    }
    if ((old ^ m) & 0x1C0) opl2_raw(0xBD, (unsigned char)(sh_bd & ~bd_mask()));
}

void opl2_write(unsigned char reg, unsigned char data)
{
    OPL_FILT(0, reg, data)
    opl2_raw(reg, data);
}

static void opl2_raw(unsigned char reg, unsigned char data)
{
    outp(g_opl_port, reg);
    opl2_delay(6);
    outp((unsigned)(g_opl_port + 1), data);
    opl2_delay(35);
}

void opl3_write_bank1(unsigned char reg, unsigned char data)
{
    OPL_FILT(1, reg, data)
    opl3_raw1(reg, data);
}

static void opl3_raw1(unsigned char reg, unsigned char data)
{
    unsigned port2 = (unsigned)(g_opl_port + 2);
    outp(port2, reg);
    opl2_delay(6);
    outp((unsigned)(port2 + 1), data);
    opl2_delay(35);
}

void opl3_write_bank0_fast(unsigned char reg, unsigned char data)
{
    OPL_FILT(0, reg, data)
    outp(g_opl_port, reg);
    opl2_delay(1);
    outp((unsigned)(g_opl_port + 1), data);
    opl2_delay(1);
}

void opl3_write_bank1_fast(unsigned char reg, unsigned char data)
{
    unsigned port2 = (unsigned)(g_opl_port + 2);
    OPL_FILT(1, reg, data)
    outp(port2, reg);
    opl2_delay(1);
    outp((unsigned)(port2 + 1), data);
    opl2_delay(1);
}

void opl_hard_silence(int bank1)
{
    static const unsigned char ops[18] = { 0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21 };
    int i;
    for (i = 0; i < 18; i++) {
        opl2_write((unsigned char)(0x80 + ops[i]), 0xFF);
        opl2_write((unsigned char)(0x40 + ops[i]), 0x3F);
        if (bank1) {
            opl3_write_bank1((unsigned char)(0x80 + ops[i]), 0xFF);
            opl3_write_bank1((unsigned char)(0x40 + ops[i]), 0x3F);
        }
    }
    for (i = 0; i < 9; i++) {
        opl2_write((unsigned char)(0xB0 + i), 0x00);
        if (bank1) opl3_write_bank1((unsigned char)(0xB0 + i), 0x00);
    }
    opl2_write(0xBD, 0x00);
}
