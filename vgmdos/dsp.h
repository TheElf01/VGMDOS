#ifndef DSP_H
#define DSP_H

typedef struct {
    unsigned base;
    unsigned char ver_major, ver_minor;
    unsigned char highspeed;
    unsigned char bits16;
} Dsp;

typedef enum {
    SOUND_NONE  = 0,
    SOUND_SB16  = 1,
    SOUND_COVOX = 2
} SoundHardware;

extern int g_sound_hardware;

void dsp_open(Dsp *d, unsigned base);

int dsp_reset(Dsp *d);

void dsp_play_once(Dsp *d, unsigned long linear_addr, unsigned length,
                    unsigned sample_rate, int dma_channel);

void dsp_play_loop(Dsp *d, unsigned long linear_addr, unsigned dma_length,
                    unsigned block_size, unsigned sample_rate, int dma_channel);

void dsp_stop(Dsp *d);

unsigned long dsp_real_rate(Dsp *d, unsigned long rate);

void dsp_play_loop16(Dsp *d, unsigned long linear_addr, unsigned length,
                      unsigned sample_rate, int dma16);
#endif
