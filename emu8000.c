#include <conio.h>
#include <stdio.h>
#include "emu8000.h"
#include "pctimer.h"

#define DATA0(b)   (b)
#define DATA1(b)   ((unsigned)((b) + 0x400))
#define DATA2(b)   ((unsigned)((b) + 0x402))
#define DATA3(b)   ((unsigned)((b) + 0x800))
#define POINTERP(b) ((unsigned)((b) + 0x802))

static unsigned g_base = 0;
static unsigned g_atkhldv = 0x7F7F, g_dcysusv = 0x7F00;
static unsigned char g_pan = 0x80;
static unsigned g_filt = 0xFF00U;
void emu8000_set_filter(unsigned char f) { g_filt = (unsigned)f << 8; }
void emu8000_set_pan(unsigned char p) { g_pan = p; }

static void set_ptr(unsigned char reg, unsigned char channel)
{
    outpw(POINTERP(g_base), (unsigned)(((unsigned)reg << 5) | (channel & 0x1F)));
}

static void write_dw(unsigned port, unsigned char reg, unsigned char channel, unsigned long value)
{
    set_ptr(reg, channel);
    outpw(port, (unsigned)(value & 0xFFFFUL));
    outpw((unsigned)(port + 2), (unsigned)((value >> 16) & 0xFFFFUL));
}

static void write_w(unsigned port, unsigned char reg, unsigned char channel, unsigned value)
{
    set_ptr(reg, channel);
    outpw(port, value);
}

static unsigned read_w(unsigned port, unsigned char reg, unsigned char channel)
{
    set_ptr(reg, channel);
    return inpw(port);
}

static void tiny_wait_ms(unsigned long ms)
{
    unsigned long target = pctimer_now() + (PCTIMER_TICKS_PER_SEC * ms) / 1000UL;
    while (pctimer_now() < target) ;
}

static const unsigned short set1_i1[32] = {
    0x03ff,0x0030,0x07ff,0x0130,0x0bff,0x0230,0x0fff,0x0330,
    0x13ff,0x0430,0x17ff,0x0530,0x1bff,0x0630,0x1fff,0x0730,
    0x23ff,0x0830,0x27ff,0x0930,0x2bff,0x0a30,0x2fff,0x0b30,
    0x33ff,0x0c30,0x37ff,0x0d30,0x3bff,0x0e30,0x3fff,0x0f30};
static const unsigned short set1_i2[32] = {
    0x43ff,0x0030,0x47ff,0x0130,0x4bff,0x0230,0x4fff,0x0330,
    0x53ff,0x0430,0x57ff,0x0530,0x5bff,0x0630,0x5fff,0x0730,
    0x63ff,0x0830,0x67ff,0x0930,0x6bff,0x0a30,0x6fff,0x0b30,
    0x73ff,0x0c30,0x77ff,0x0d30,0x7bff,0x0e30,0x7fff,0x0f30};
static const unsigned short set1_i3[32] = {
    0x83ff,0x0030,0x87ff,0x0130,0x8bff,0x0230,0x8fff,0x0330,
    0x93ff,0x0430,0x97ff,0x0530,0x9bff,0x0630,0x9fff,0x0730,
    0xa3ff,0x0830,0xa7ff,0x0930,0xabff,0x0a30,0xafff,0x0b30,
    0xb3ff,0x0c30,0xb7ff,0x0d30,0xbbff,0x0e30,0xbfff,0x0f30};
static const unsigned short set1_i4[32] = {
    0xc3ff,0x0030,0xc7ff,0x0130,0xcbff,0x0230,0xcfff,0x0330,
    0xd3ff,0x0430,0xd7ff,0x0530,0xdbff,0x0630,0xdfff,0x0730,
    0xe3ff,0x0830,0xe7ff,0x0930,0xebff,0x0a30,0xefff,0x0b30,
    0xf3ff,0x0c30,0xf7ff,0x0d30,0xfbff,0x0e30,0xffff,0x0f30};

static const unsigned short set2_i1[32] = {
    0x03ff,0x8030,0x07ff,0x8130,0x0bff,0x8230,0x0fff,0x8330,
    0x13ff,0x8430,0x17ff,0x8530,0x1bff,0x8630,0x1fff,0x8730,
    0x23ff,0x8830,0x27ff,0x8930,0x2bff,0x8a30,0x2fff,0x8b30,
    0x33ff,0x8c30,0x37ff,0x8d30,0x3bff,0x8e30,0x3fff,0x8f30};
static const unsigned short set2_i2[32] = {
    0x43ff,0x8030,0x47ff,0x8130,0x4bff,0x8230,0x4fff,0x8330,
    0x53ff,0x8430,0x57ff,0x8530,0x5bff,0x8630,0x5fff,0x8730,
    0x63ff,0x8830,0x67ff,0x8930,0x6bff,0x8a30,0x6fff,0x8b30,
    0x73ff,0x8c30,0x77ff,0x8d30,0x7bff,0x8e30,0x7fff,0x8f30};
static const unsigned short set2_i3[32] = {
    0x83ff,0x8030,0x87ff,0x8130,0x8bff,0x8230,0x8fff,0x8330,
    0x93ff,0x8430,0x97ff,0x8530,0x9bff,0x8630,0x9fff,0x8730,
    0xa3ff,0x8830,0xa7ff,0x8930,0xabff,0x8a30,0xafff,0x8b30,
    0xb3ff,0x8c30,0xb7ff,0x8d30,0xbbff,0x8e30,0xbfff,0x8f30};
static const unsigned short set2_i4[32] = {
    0xc3ff,0x8030,0xc7ff,0x8130,0xcbff,0x8230,0xcfff,0x8330,
    0xd3ff,0x8430,0xd7ff,0x8530,0xdbff,0x8630,0xdfff,0x8730,
    0xe3ff,0x8830,0xe7ff,0x8930,0xebff,0x8a30,0xefff,0x8b30,
    0xf3ff,0x8c30,0xf7ff,0x8d30,0xfbff,0x8e30,0xffff,0x8f30};

static const unsigned short set3_i1[32] = {
    0x0C10,0x8470,0x14FE,0xB488,0x167F,0xA470,0x18E7,0x84B5,
    0x1B6E,0x842A,0x1F1D,0x852A,0x0DA3,0x9F7C,0x167E,0xF254,
    0x0000,0x842A,0x0001,0x852A,0x18E6,0x9BAA,0x1B6D,0xF234,
    0x229F,0x8429,0x2746,0x8529,0x1F1C,0x96E7,0x229E,0xF224};
static const unsigned short set3_i2[32] = {
    0x0DA4,0x8429,0x2C29,0x8529,0x2745,0x97F6,0x2C28,0xF254,
    0x383B,0x8428,0x320F,0x8528,0x320E,0x9F02,0x1341,0xF264,
    0x3EB6,0x8428,0x3EB9,0x8528,0x383A,0x9FA9,0x3EB5,0xF294,
    0x3EB7,0x8474,0x3EBA,0x8575,0x3EB8,0xC4C3,0x3EBB,0xC5C3};
static const unsigned short set3_i3[32] = {
    0x0000,0xA404,0x0001,0xA504,0x141F,0x8671,0x14FD,0x8287,
    0x3EBC,0xE610,0x3EC8,0x8C7B,0x031A,0x87E6,0x3EC8,0x86F7,
    0x3EC0,0x821E,0x3EBE,0xD208,0x3EBD,0x821F,0x3ECA,0x8386,
    0x3EC1,0x8C03,0x3EC9,0x831E,0x3ECA,0x8C4C,0x3EBF,0x8C55};
static const unsigned short set3_i4[32] = {
    0x3EC9,0xC208,0x3EC4,0xBC84,0x3EC8,0x8EAD,0x3EC8,0xD308,
    0x3EC2,0x8F7E,0x3ECB,0x821E,0x3ECB,0xD208,0x3EC5,0x831F,
    0x3EC6,0xC308,0x3EC3,0xB2FF,0x3EC9,0x8265,0x3EC9,0x831E,
    0x1342,0xD308,0x3EC7,0xB3FF,0x0000,0x8365,0x1420,0x9570};

static const unsigned short set4_i1[32] = {
    0x0C10,0x8470,0x14FE,0xB488,0x167F,0xA470,0x18E7,0x84B5,
    0x1B6E,0x842A,0x1F1D,0x852A,0x0DA3,0x0F7C,0x167E,0x7254,
    0x0000,0x842A,0x0001,0x852A,0x18E6,0x0BAA,0x1B6D,0x7234,
    0x229F,0x8429,0x2746,0x8529,0x1F1C,0x06E7,0x229E,0x7224};
static const unsigned short set4_i2[32] = {
    0x0DA4,0x8429,0x2C29,0x8529,0x2745,0x07F6,0x2C28,0x7254,
    0x383B,0x8428,0x320F,0x8528,0x320E,0x0F02,0x1341,0x7264,
    0x3EB6,0x8428,0x3EB9,0x8528,0x383A,0x0FA9,0x3EB5,0x7294,
    0x3EB7,0x8474,0x3EBA,0x8575,0x3EB8,0x44C3,0x3EBB,0x45C3};
static const unsigned short set4_i3[32] = {
    0x0000,0xA404,0x0001,0xA504,0x141F,0x0671,0x14FD,0x0287,
    0x3EBC,0xE610,0x3EC8,0x0C7B,0x031A,0x07E6,0x3EC8,0x86F7,
    0x3EC0,0x821E,0x3EBE,0xD208,0x3EBD,0x021F,0x3ECA,0x0386,
    0x3EC1,0x0C03,0x3EC9,0x031E,0x3ECA,0x8C4C,0x3EBF,0x0C55};
static const unsigned short set4_i4[32] = {
    0x3EC9,0xC208,0x3EC4,0xBC84,0x3EC8,0x0EAD,0x3EC8,0xD308,
    0x3EC2,0x8F7E,0x3ECB,0x021E,0x3ECB,0xD208,0x3EC5,0x031F,
    0x3EC6,0xC308,0x3EC3,0x32FF,0x3EC9,0x0265,0x3EC9,0x831E,
    0x1342,0xD308,0x3EC7,0x33FF,0x0000,0x8365,0x1420,0x9570};

static void write_init_set(const unsigned short *i1, const unsigned short *i2,
                            const unsigned short *i3, const unsigned short *i4)
{
    int ch;
    for (ch = 0; ch < 32; ch++) {
        write_w(DATA1(g_base), 2, (unsigned char)ch, i1[ch]);
        write_w(DATA2(g_base), 2, (unsigned char)ch, i2[ch]);
        write_w(DATA1(g_base), 3, (unsigned char)ch, i3[ch]);
        write_w(DATA2(g_base), 3, (unsigned char)ch, i4[ch]);
    }
}

int emu8000_init(unsigned base_port)
{
    int ch;
    g_base = base_port;
    if (base_port == 0) return 0;
    write_w(DATA1(g_base), 1, 29, 0x0059);
    write_w(DATA1(g_base), 1, 30, 0x0020);
    for (ch = 0; ch < 32; ch++)
        write_w(DATA1(g_base), 5, (unsigned char)ch, 0x0080);
    for (ch = 0; ch < 32; ch++) {
        unsigned char c = (unsigned char)ch;
        write_w(DATA1(g_base), 4, c, 0);
        write_w(DATA1(g_base), 6, c, 0);
        write_w(DATA1(g_base), 7, c, 0);
        write_w(DATA2(g_base), 4, c, 0);
        write_w(DATA2(g_base), 5, c, 0);
        write_w(DATA2(g_base), 6, c, 0);
        write_w(DATA2(g_base), 7, c, 0);
        write_w(DATA3(g_base), 0, c, 0);
        write_w(DATA3(g_base), 1, c, 0);
        write_w(DATA3(g_base), 2, c, 0);
        write_w(DATA3(g_base), 3, c, 0);
        write_w(DATA3(g_base), 4, c, 0);
        write_w(DATA3(g_base), 5, c, 0);
        write_dw(DATA0(g_base), 1, c, 0);
        write_dw(DATA0(g_base), 3, c, 0);
        write_dw(DATA0(g_base), 6, c, 0);
        write_dw(DATA0(g_base), 7, c, 0);
        write_dw(DATA1(g_base), 0, c, 0);
    }
    for (ch = 0; ch < 32; ch++) {
        unsigned char c = (unsigned char)ch;
        write_dw(DATA0(g_base), 0, c, 0);
        write_dw(DATA0(g_base), 2, c, 0);
    }
    write_dw(DATA1(g_base), 1, 20, 0);
    write_dw(DATA1(g_base), 1, 21, 0);
    write_dw(DATA1(g_base), 1, 22, 0);
    write_dw(DATA1(g_base), 1, 23, 0);
    write_init_set(set1_i1, set1_i2, set1_i3, set1_i4);
    tiny_wait_ms(24);
    write_init_set(set2_i1, set2_i2, set2_i3, set2_i4);
    write_init_set(set3_i1, set3_i2, set3_i3, set3_i4);
    write_dw(DATA1(g_base), 1, 9, 0UL);
    write_dw(DATA1(g_base), 1, 10, 0x00000083UL);
    write_dw(DATA1(g_base), 1, 13, 0x00008000UL);
    write_init_set(set4_i1, set4_i2, set4_i3, set4_i4);
    write_w(DATA1(g_base), 3, 31, 0x0004);
    return 1;
}

int emu8000_load_sample(unsigned long addr, const short *data, unsigned long count)
{
    unsigned long i;
    unsigned dummy;
    write_w(DATA1(g_base), 5, 31, 0x0080);
    write_dw(DATA0(g_base), 3, 31, 0);
    write_dw(DATA0(g_base), 2, 31, 0);
    write_dw(DATA0(g_base), 1, 31, 0x40000000UL);
    write_dw(DATA0(g_base), 0, 31, 0x40000000UL);
    write_dw(DATA0(g_base), 6, 31, 0);
    write_dw(DATA0(g_base), 7, 31, 0);
    write_dw(DATA1(g_base), 0, 31, 0x06000000UL);
    dummy = read_w(DATA1(g_base), 1, 22);
    (void)dummy;
    write_dw(DATA1(g_base), 1, 22, addr);
    for (i = 0; i < count; i++) {
        unsigned long timeout = 100000UL;
        while (timeout--) {
            unsigned hi = read_w(DATA2(g_base), 1, 22);
            if (!(hi & 0x8000U)) break;
        }
        write_w(DATA1(g_base), 1, 26, (unsigned)data[i]);
    }
    {
        unsigned long timeout = 100000UL;
        while (timeout--) {
            unsigned hi = read_w(DATA2(g_base), 1, 22);
            if (!(hi & 0x8000U)) break;
        }
    }
    write_dw(DATA1(g_base), 0, 31, 0);
    return 1;
}

void emu8000_note_on(int channel, unsigned long start, unsigned long end, unsigned int pitch, int volume)
{
    unsigned char c = (unsigned char)channel;
    unsigned char atten = (unsigned char)(255 - (volume > 255 ? 255 : volume));
    write_w(DATA1(g_base), 4, c, 0x8000);
    write_w(DATA1(g_base), 6, c, 0x8000);
    write_w(DATA1(g_base), 7, c, 0x7F7F);
    write_w(DATA2(g_base), 4, c, g_atkhldv);
    write_w(DATA2(g_base), 5, c, 0x8000);
    write_w(DATA2(g_base), 6, c, 0x7F7F);
    write_w(DATA2(g_base), 7, c, 0x8000);
    write_w(DATA3(g_base), 0, c, pitch);
    write_w(DATA3(g_base), 1, c, (unsigned)(0xFF00U | atten));
    write_w(DATA3(g_base), 2, c, 0);
    write_w(DATA3(g_base), 3, c, 0);
    write_w(DATA3(g_base), 4, c, 0x0010);
    write_w(DATA3(g_base), 5, c, 0x0010);
    write_dw(DATA0(g_base), 6, c, start);
    write_dw(DATA0(g_base), 7, c, end);
    write_dw(DATA1(g_base), 0, c, 0x00000000UL | start);
    write_dw(DATA0(g_base), 3, c, 0x0000FFFFUL);
    write_dw(DATA0(g_base), 2, c, 0x0000FFFFUL);
    write_w(DATA1(g_base), 5, c, g_dcysusv);
    write_dw(DATA0(g_base), 1, c, 0x40000000UL);
    write_dw(DATA0(g_base), 0, c, 0x40000000UL);
}

void emu8000_note_off(int channel)
{
    unsigned char c = (unsigned char)channel;
    write_w(DATA1(g_base), 5, c, 0x0080);
    write_dw(DATA0(g_base), 3, c, 0x0000FFFFUL);
    write_dw(DATA0(g_base), 2, c, 0x0000FFFFUL);
    write_w(DATA3(g_base), 1, c, (g_filt | 0xFFU));
}

void emu8000_set_pitch(int channel, unsigned int pitch)
{
    write_w(DATA3(g_base), 0, (unsigned char)channel, pitch);
}

void emu8000_set_volume(int channel, int volume)
{
    unsigned char atten = (unsigned char)(255 - (volume > 255 ? 255 : volume));
    unsigned cur = read_w(DATA3(g_base), 1, (unsigned char)channel);
    write_w(DATA3(g_base), 1, (unsigned char)channel, (unsigned)((cur & 0xFF00U) | atten));
}

void emu8000_set_loop(int channel, unsigned long start, unsigned long end)
{
    unsigned char c = (unsigned char)channel;
    write_dw(DATA0(g_base), 7, c, end);
    write_dw(DATA0(g_base), 6, c, start);
}

void emu8000_play(int channel, unsigned long start, unsigned long lstart, unsigned long lend,
                  unsigned int pitch, unsigned char atten)
{
    unsigned char c = (unsigned char)channel;
    write_w(DATA1(g_base), 5, c, 0x0080);
    write_w(DATA1(g_base), 4, c, 0x8000);
    write_w(DATA1(g_base), 6, c, 0x8000);
    write_w(DATA1(g_base), 7, c, 0x7F7F);
    write_w(DATA2(g_base), 4, c, g_atkhldv);
    write_w(DATA2(g_base), 5, c, 0x8000);
    write_w(DATA2(g_base), 6, c, 0x7F7F);
    write_w(DATA2(g_base), 7, c, 0x8000);
    write_w(DATA3(g_base), 0, c, pitch);
    write_w(DATA3(g_base), 1, c, (g_filt | atten));
    write_w(DATA3(g_base), 2, c, 0);
    write_w(DATA3(g_base), 3, c, 0);
    write_w(DATA3(g_base), 4, c, 0x0010);
    write_w(DATA3(g_base), 5, c, 0x0010);
    write_dw(DATA0(g_base), 6, c, ((unsigned long)g_pan << 24) | lstart);
    write_dw(DATA0(g_base), 7, c, lend);
    write_dw(DATA1(g_base), 0, c, start);
    write_dw(DATA0(g_base), 3, c, 0x0000FFFFUL);
    write_dw(DATA0(g_base), 2, c, 0x0000FFFFUL);
    write_w(DATA1(g_base), 5, c, g_dcysusv);
    write_dw(DATA0(g_base), 1, c, 0x40000000UL);
    write_dw(DATA0(g_base), 0, c, 0x40000000UL);
}

void emu8000_set_atten(int channel, unsigned char atten)
{
    write_w(DATA3(g_base), 1, (unsigned char)channel, (g_filt | atten));
}
