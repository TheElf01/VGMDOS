#include "opl2.h"
#include "ym3812_opl.h"

#define Y8950_TL_BOOST  12
#define YM3526_TL_BOOST 12

unsigned char far *g_pcm_voice_buffer = 0;
unsigned long g_pcm_voice_size = 0;
unsigned long g_pcm_voice_pos = 0;
int g_pcm_voice_active = 0;

static unsigned char g_feedback_connection[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};

void ym3812opl_init(unsigned port)
{
    int i;
    opl2_init(port);
    opl3_write_bank1(0x05, 0x00);
    opl3_write_bank1(0x04, 0x00);
    for (i = 0; i < 9; i++) {
        g_feedback_connection[i] = 0;
    }
    g_pcm_voice_pos = 0;
    g_pcm_voice_active = 0;
}

void ym3812opl_write(unsigned char addr, unsigned char data)
{
    if (addr >= 0xC0 && addr <= 0xC8) {
        g_feedback_connection[addr - 0xC0] = data & 0x01;
    }
    opl2_write(addr, data);
}

static unsigned char apply_tl_boost(unsigned char addr, unsigned char data, unsigned char boost)
{
    static const struct {
        unsigned char canal;
        unsigned char tipo;
    } ops[0x16] = {
        {0, 0}, {1, 0}, {2, 0}, {0, 1}, {1, 1}, {2, 1}, {0, 0}, {0, 0},
        {3, 0}, {4, 0}, {5, 0}, {3, 1}, {4, 1}, {5, 1}, {0, 0}, {0, 0},
        {6, 0}, {7, 0}, {8, 0}, {6, 1}, {7, 1}, {8, 1}
    };
    unsigned char idx = (unsigned char)(addr - 0x40);
    int es_carrier = 0;
    if (idx >= 0x16 || (addr & 0x0F) == 0x06 || (addr & 0x0F) == 0x07 ||
        (addr & 0x0F) == 0x0E || (addr & 0x0F) == 0x0F) {
        return data;
    }
    if (ops[idx].tipo == 1) {
        es_carrier = 1;
    } else {
        unsigned char ch = ops[idx].canal;
        if (g_feedback_connection[ch] == 1) {
            es_carrier = 1;
        }
    }
    if (!es_carrier) {
        return data;
    }
    {
        unsigned char ksl = data & 0xC0;
        unsigned char tl = data & 0x3F;
        if (tl > boost) {
            tl = (unsigned char)(tl - boost);
        } else {
            tl = 0;
        }
        return (unsigned char)(ksl | tl);
    }
}

void ym3526opl_write(unsigned char addr, unsigned char data)
{
    if (addr >= 0xC0 && addr <= 0xC8) {
        g_feedback_connection[addr - 0xC0] = data & 0x01;
    }
    if (addr >= 0x40 && addr <= 0x55)
        data = apply_tl_boost(addr, data, YM3526_TL_BOOST);
    opl2_write(addr, data);
}

void y8950opl_write(unsigned char addr, unsigned char data)
{
    if (addr == 0x07) {
        if (data & 0x80) {
            g_pcm_voice_pos = 0;
            g_pcm_voice_active = 1;
        } else {
            g_pcm_voice_active = 0;
        }
        return;
    }
    if (addr < 0x20) return;
    if (addr >= 0xC0 && addr <= 0xC8) {
        g_feedback_connection[addr - 0xC0] = data & 0x01;
    }
    if (addr >= 0x40 && addr <= 0x55)
        data = apply_tl_boost(addr, data, Y8950_TL_BOOST);
    opl2_write(addr, data);
}

void ymf262opl_write(int bank, unsigned char addr, unsigned char data)
{
    if (bank == 0) {
        if (addr >= 0xC0 && addr <= 0xC8) g_feedback_connection[addr - 0xC0] = data & 0x01;
        opl3_write_bank0_fast(addr, data);
    } else {
        opl3_write_bank1_fast(addr, data);
    }
}

void ym3812opl_silence(void)
{
    int ch;
    for (ch = 0; ch < 9; ch++) {
        opl2_write((unsigned char)(0xB0 + ch), 0x00);
    }
    g_pcm_voice_active = 0;
}

void ymf262opl_silence(void)
{
    int ch;
    ym3812opl_silence();
    for (ch = 0; ch < 9; ch++) {
        opl3_write_bank1((unsigned char)(0xB0 + ch), 0x00);
    }
}
