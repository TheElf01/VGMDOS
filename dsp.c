#include <conio.h>
#include "dsp.h"
#include "dma.h"
#include "pctimer.h"

#define DSP_RESET(base)       ((base) + 0x06)
#define DSP_READ_DATA(base)   ((base) + 0x0A)
#define DSP_WRITE_CMD(base)   ((base) + 0x0C)
#define DSP_WRITE_STATUS(base)((base) + 0x0C)
#define DSP_READ_STATUS(base) ((base) + 0x0E)

int g_sound_hardware = SOUND_SB16;

void dsp_open(Dsp *d, unsigned base)
{
    d->base = base;
    d->bits16 = 0;
}

static int dsp_write_byte(Dsp *d, unsigned char val)
{
    unsigned tries;
    for (tries = 0; tries < 0xFFFFU; tries++) {
        if ((inp(DSP_WRITE_STATUS(d->base)) & 0x80) == 0) {
            outp(DSP_WRITE_CMD(d->base), val);
            return 1;
        }
    }
    return 0;
}

static int dsp_read_byte(Dsp *d, unsigned char *out)
{
    unsigned tries;
    for (tries = 0; tries < 0xFFFFU; tries++) {
        if (inp(DSP_READ_STATUS(d->base)) & 0x80) {
            *out = (unsigned char)inp(DSP_READ_DATA(d->base));
            return 1;
        }
    }
    return 0;
}

int dsp_reset(Dsp *d)
{
    unsigned char resp;
    unsigned long t;
    outp(DSP_RESET(d->base), 1);
    t = pctimer_now();
    while (pctimer_now() - t < 10)
        ;
    outp(DSP_RESET(d->base), 0);
    if (!dsp_read_byte(d, &resp))
        return 0;
    if (resp != 0xAA) return 0;
    d->ver_major = 4; d->ver_minor = 0;
    d->highspeed = 0;
    d->bits16 = 0;
    if (dsp_write_byte(d, 0xE1)) {
        unsigned char ma, mi;
        if (dsp_read_byte(d, &ma) && dsp_read_byte(d, &mi)) {
            d->ver_major = ma; d->ver_minor = mi;
        }
    }
    return 1;
}

unsigned long dsp_real_rate(Dsp *d, unsigned long rate)
{
    unsigned long div;
    if (d->ver_major >= 4 || rate == 0) return rate;
    div = 1000000UL / rate;
    if (div == 0) div = 1;
    return 1000000UL / div;
}

void dsp_play_once(Dsp *d, unsigned long linear_addr, unsigned length,
                    unsigned sample_rate, int dma_channel)
{
    unsigned time_constant;
    unsigned count;
    if (g_sound_hardware == SOUND_COVOX) {
        return;
    }
    time_constant = (unsigned)(256 - (1000000UL / sample_rate));
    dma_start_playback(dma_channel, linear_addr, length, 0);
    dsp_write_byte(d, 0x40);
    dsp_write_byte(d, (unsigned char)time_constant);
    count = length - 1;
    dsp_write_byte(d, 0x14);
    dsp_write_byte(d, (unsigned char)(count & 0xFF));
    dsp_write_byte(d, (unsigned char)((count >> 8) & 0xFF));
}

void dsp_play_loop(Dsp *d, unsigned long linear_addr, unsigned dma_length,
                    unsigned block_size, unsigned sample_rate, int dma_channel)
{
    unsigned block_count;
    if (g_sound_hardware == SOUND_COVOX) {
        return;
    }
    dma_start_playback(dma_channel, linear_addr, dma_length, 1);
    block_count = block_size - 1;
    if (d->ver_major >= 4) {
        dsp_write_byte(d, 0x41);
        dsp_write_byte(d, (unsigned char)((sample_rate >> 8) & 0xFF));
        dsp_write_byte(d, (unsigned char)(sample_rate & 0xFF));
        dsp_write_byte(d, 0xC6);
        dsp_write_byte(d, 0x00);
        dsp_write_byte(d, (unsigned char)(block_count & 0xFF));
        dsp_write_byte(d, (unsigned char)((block_count >> 8) & 0xFF));
    } else {
        unsigned div = (unsigned)(1000000UL / sample_rate);
        dsp_write_byte(d, 0xD1);
        dsp_write_byte(d, 0x40);
        dsp_write_byte(d, (unsigned char)(256 - div));
        dsp_write_byte(d, 0x48);
        dsp_write_byte(d, (unsigned char)(block_count & 0xFF));
        dsp_write_byte(d, (unsigned char)((block_count >> 8) & 0xFF));
        if (sample_rate > 22222U && (d->ver_major > 2 || d->ver_minor >= 1)) {
            dsp_write_byte(d, 0x90);
            d->highspeed = 1;
        } else {
            dsp_write_byte(d, 0x1C);
        }
    }
}

void dsp_play_loop16(Dsp *d, unsigned long linear_addr, unsigned length,
                      unsigned sample_rate, int dma16)
{
    unsigned block_count = length - 1;
    dma_start_playback(dma16, linear_addr, length, 1);
    dsp_write_byte(d, 0x41);
    dsp_write_byte(d, (unsigned char)((sample_rate >> 8) & 0xFF));
    dsp_write_byte(d, (unsigned char)(sample_rate & 0xFF));
    dsp_write_byte(d, 0xB6);
    dsp_write_byte(d, 0x10);
    dsp_write_byte(d, (unsigned char)(block_count & 0xFF));
    dsp_write_byte(d, (unsigned char)((block_count >> 8) & 0xFF));
    d->bits16 = 1;
}

void dsp_stop(Dsp *d)
{
    if (d->bits16) {
        dsp_write_byte(d, 0xD9);
        dsp_write_byte(d, 0xD5);
        d->bits16 = 0;
    }
    dsp_write_byte(d, 0xD0);
    if (d->ver_major < 4) {
        if (d->highspeed) {
            unsigned char resp;
            unsigned long t;
            outp(DSP_RESET(d->base), 1);
            t = pctimer_now();
            while (pctimer_now() - t < 10) ;
            outp(DSP_RESET(d->base), 0);
            dsp_read_byte(d, &resp);
            d->highspeed = 0;
        }
        dsp_write_byte(d, 0xD3);
    }
}
