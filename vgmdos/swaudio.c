#include <conio.h>
#include <dos.h>
#include "swaudio.h"

#define PIT_CLOCK 1193182UL
#define OLD_DIVISOR 65536UL

static const unsigned char far *g_buffer = 0;
static unsigned g_buffer_len = 0;
static volatile unsigned g_read_pos = 0;
static volatile unsigned g_write_pos = 0;
static unsigned long g_new_divisor = 0;
static unsigned long g_accum = 0;
static long g_error = 0;
static unsigned g_lpt_port = 0;
static SwaudioMode g_mode = 0;
static void (__interrupt __far *g_old_vector)(void) = 0;
static int g_installed = 0;

#define SPEAKER_OVERSAMPLE 2
static unsigned g_oversample_ctr = 0;

static void __interrupt __far swaudio_isr(void)
{
    static unsigned char held_sample = 128;
    if (g_mode == SWAUDIO_SPEAKER) {
        if (g_oversample_ctr == 0 && g_buffer && g_read_pos != g_write_pos) {
            held_sample = g_buffer[g_read_pos];
            g_read_pos++;
            if (g_read_pos >= g_buffer_len) g_read_pos = 0;
        }
        g_oversample_ctr++;
        if (g_oversample_ctr >= SPEAKER_OVERSAMPLE) g_oversample_ctr = 0;
        {
            int centered = (int)held_sample - 128;
            unsigned char port61 = inp(0x61) & 0xFC;
            g_error += centered;
            if (g_error > 0) {
                port61 |= 0x02;
                g_error -= 128;
            } else {
                g_error += 128;
            }
            outp(0x61, port61);
        }
    } else if (g_buffer && g_read_pos != g_write_pos) {
        outp(g_lpt_port, g_buffer[g_read_pos]);
        g_read_pos++;
        if (g_read_pos >= g_buffer_len) g_read_pos = 0;
    }
    g_accum += g_new_divisor;
    if (g_accum >= OLD_DIVISOR) {
        g_accum -= OLD_DIVISOR;
        _chain_intr(g_old_vector);
    }
    outp(0x20, 0x20);
}

unsigned swaudio_detect_lpt(int n)
{
    unsigned far *bda;
    if (n < 1 || n > 4) return 0;
    bda = (unsigned far *)MK_FP(0x0040, 0x0008 + (n - 1) * 2);
    return *bda;
}

int swaudio_start(SwaudioMode mode, unsigned sample_rate,
                   const unsigned char far *buffer, unsigned buffer_len,
                   unsigned lpt_port)
{
    if (!sample_rate || !buffer || !buffer_len) return 0;
    if (mode != SWAUDIO_SPEAKER && !lpt_port) return 0;
    g_buffer = buffer;
    g_buffer_len = buffer_len;
    g_read_pos = 0;
    g_write_pos = 0;
    g_accum = 0;
    g_error = 0;
    g_oversample_ctr = 0;
    g_mode = mode;
    g_lpt_port = lpt_port;
    g_new_divisor = PIT_CLOCK / (sample_rate * (mode == SWAUDIO_SPEAKER ? SPEAKER_OVERSAMPLE : 1));
    if (g_new_divisor < 1) g_new_divisor = 1;
    if (g_new_divisor > 65535) g_new_divisor = 65535;
    g_old_vector = _dos_getvect(0x08);
    _dos_setvect(0x08, swaudio_isr);
    outp(0x43, 0x36);
    outp(0x40, (unsigned char)(g_new_divisor & 0xFF));
    outp(0x40, (unsigned char)((g_new_divisor >> 8) & 0xFF));
    g_installed = 1;
    return 1;
}

void swaudio_set_write_pos(unsigned write_pos)
{
    g_write_pos = write_pos;
}

void swaudio_stop(void)
{
    if (!g_installed) return;
    outp(0x43, 0x36);
    outp(0x40, 0x00);
    outp(0x40, 0x00);
    _dos_setvect(0x08, g_old_vector);
    if (g_mode == SWAUDIO_SPEAKER)
        outp(0x61, inp(0x61) & 0xFC);
    g_installed = 0;
    g_buffer = 0;
}
