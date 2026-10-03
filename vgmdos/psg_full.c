#include <string.h>
#include "psg_full.h"

static const unsigned char tone_vol_table[16] = {
    40, 32, 25, 20, 16, 13, 10, 8, 6, 5, 4, 3, 3, 2, 2, 0
};
#define noise_vol_table tone_vol_table

void psg_full_set_vol(unsigned vol256);

#define FIXED_SHIFT 8
#define FIXED_ONE (1UL << FIXED_SHIFT)

static int g_t6 = 0;
static unsigned short g_t6_f2 = 0;

static int noise_interval(unsigned char n_ctrl, unsigned short f2, unsigned int w_max)
{
    int np;
    switch (n_ctrl & 0x03) {
        case 0: np = 32; break;
        case 1: np = 64; break;
        case 2: np = 128; break;
        default: np = (f2 <= 1) ? 2 : 2 * (int)f2; break;
    }
    if (np < (int)w_max) np = (int)w_max;
    return np;
}

#define K_SCALE(amp, w) ((unsigned int)(((unsigned int)(amp) << 8) / (w)))

#define TONE_STEP(f, c, o, amp, k)                                        \
    if ((f) > 1) {                                                        \
        (c) -= (int)W;                                                    \
        if ((c) > 0) {                                                    \
            mix += (o) ? (amp) : -(amp);                                  \
        } else {                                                          \
            int t_ = (c) + (int)W;                                        \
            unsigned int hi_ = (o) ? (unsigned int)t_ : 0;                \
            int rem_ = (int)W - t_;                                       \
            (o) ^= 1;                                                     \
            while (rem_ >= (int)(f)) {                                    \
                if (o) hi_ += (unsigned int)(f);                          \
                rem_ -= (int)(f);                                         \
                (o) ^= 1;                                                 \
            }                                                             \
            if (o) hi_ += (unsigned int)rem_;                             \
            (c) = (int)(f) - rem_;                                        \
            mix += (int)((hi_ * (k)) >> 7) - (amp);                       \
        }                                                                 \
    } else {                                                              \
        mix += (amp);                                                     \
    }

unsigned char g_psg_mute = 0;

void psg_full_init(PsgFull *p, unsigned long clock, unsigned long sample_rate)
{
    int i;
    unsigned long ticks_fp;
    memset(p, 0, sizeof(*p));
    p->clock = clock;
    p->sample_rate = sample_rate;
    p->noise_shift = 0x8000;
    for (i = 0; i < 3; i++) {
        p->tone_volume[i] = 0x0F;
        p->tone_counter[i] = 1;
    }
    p->noise_volume = 0x0F;
    p->noise_counter = 1;
    psg_full_set_vol(256);
    ticks_fp = (unsigned long)((clock << FIXED_SHIFT) / (16UL * sample_rate));
    p->ticks_int = (unsigned int)(ticks_fp >> FIXED_SHIFT);
    p->ticks_frac = (unsigned char)(ticks_fp & (FIXED_ONE - 1));
}

void psg_full_write(PsgFull *p, unsigned char data)
{
    if (data & 0x80) {
        unsigned channel = (data >> 5) & 0x03;
        unsigned is_vol = (data >> 4) & 0x01;
        p->latched_channel = (unsigned char)channel;
        p->latched_is_volume = (unsigned char)is_vol;
        if (channel == 3) {
            if (is_vol)
                p->noise_volume = data & 0x0F;
            else {
                p->noise_ctrl = data & 0x07;
                p->noise_shift = 0x8000;
            }
        } else if (is_vol) {
            p->tone_volume[channel] = data & 0x0F;
        } else {
            p->tone_freq[channel] = (unsigned short)((p->tone_freq[channel] & 0x3F0) | (data & 0x0F));
        }
    } else {
        unsigned ch = p->latched_channel;
        if (p->latched_is_volume) {
            if (ch == 3) p->noise_volume = data & 0x0F;
            else p->tone_volume[ch] = data & 0x0F;
        } else if (ch == 3) {
            p->noise_ctrl = data & 0x07;
            p->noise_shift = 0x8000;
        } else {
            p->tone_freq[ch] = (unsigned short)((p->tone_freq[ch] & 0x0F) | ((data & 0x3F) << 4));
        }
    }
}

#define PSG_CLUT_OFS 32
static unsigned char psg_clut[256 + 2 * PSG_CLUT_OFS + 1];

void psg_full_set_vol(unsigned vol256)
{
    int k;
    for (k = 0; k <= 256 + 2 * PSG_CLUT_OFS; k++) {
        int m = k - PSG_CLUT_OFS;
        if (m < 0) m = 0;
        else if (m > 255) m = 255;
        if (vol256 != 256) {
            m = 128 + (int)(((long)(m - 128) * (long)vol256) / 256L);
            if (m < 0) m = 0;
            if (m > 255) m = 255;
        }
        psg_clut[k] = (unsigned char)m;
    }
}

#define TONE_FAST(f, c, o, amp, s, k)                                     \
    (c) -= (int)W;                                                        \
    if ((c) > 0) {                                                        \
        mix += (s);                                                       \
    } else {                                                              \
        int t_ = (c) + (int)W;                                            \
        unsigned int hi_ = (o) ? (unsigned int)t_ : 0;                    \
        int rem_ = (int)W - t_;                                           \
        (o) ^= 1;                                                         \
        while (rem_ >= (int)(f)) {                                        \
            if (o) hi_ += (unsigned int)(f);                              \
            rem_ -= (int)(f);                                             \
            (o) ^= 1;                                                     \
        }                                                                 \
        if (o) hi_ += (unsigned int)rem_;                                 \
        (c) = (int)(f) - rem_;                                            \
        mix += (int)((hi_ * (k)) >> 7) - (amp);                           \
        (s) = (o) ? (amp) : -(amp);                                       \
    }

#define PSG_IDLE_CTR 30000
#define PSG_BLOCK 256u

void psg_full_run(PsgFull *p, unsigned char far *out, unsigned long count)
{
    unsigned int ticks_int = p->ticks_int;
    unsigned char ticks_frac = p->ticks_frac;
    unsigned char clock_frac = (unsigned char)(p->clock_accum & (FIXED_ONE - 1));
    unsigned short f0 = p->tone_freq[0], f1 = p->tone_freq[1], f2 = p->tone_freq[2];
    int c0 = (int)p->tone_counter[0], c1 = (int)p->tone_counter[1], c2 = (int)p->tone_counter[2];
    unsigned char o0 = p->tone_output[0], o1 = p->tone_output[1], o2 = p->tone_output[2];
    int n_ctr = (int)p->noise_counter;
    unsigned short n_shift = p->noise_shift;
    unsigned char n_out = p->noise_output;
    unsigned char n_ctrl = p->noise_ctrl;
    int np = noise_interval(n_ctrl, g_t6 ? g_t6_f2 : f2, ticks_int + 1);
    int wm = (int)ticks_int + 1, wm2 = 2 * ((int)ticks_int + 1);
    int amp0 = (g_psg_mute & 1) ? 0 : tone_vol_table[p->tone_volume[0]];
    int amp1 = (g_psg_mute & 2) ? 0 : tone_vol_table[p->tone_volume[1]];
    int amp2 = (g_psg_mute & 4) ? 0 : tone_vol_table[p->tone_volume[2]];
    int ampn = (g_psg_mute & 8) ? 0 : noise_vol_table[p->noise_volume];
    unsigned int k0a = K_SCALE(amp0, ticks_int), k0b = K_SCALE(amp0, ticks_int + 1);
    unsigned int k1a = K_SCALE(amp1, ticks_int), k1b = K_SCALE(amp1, ticks_int + 1);
    unsigned int k2a = K_SCALE(amp2, ticks_int), k2b = K_SCALE(amp2, ticks_int + 1);
    int s0 = o0 ? amp0 : -amp0, s1 = o1 ? amp1 : -amp1, s2 = o2 ? amp2 : -amp2;
    int sn = n_out ? ampn : -ampn;
    int base = 128 + PSG_CLUT_OFS;
    int sc0 = c0, sc1 = c1, sc2 = c2;
    const unsigned char *clut = psg_clut;
    if (f0 <= 1 || amp0 == 0) { base += amp0; s0 = 0; }
    if (f1 <= 1 || amp1 == 0) { base += amp1; s1 = 0; }
    if (f2 <= 1 || amp2 == 0) { base += amp2; s2 = 0; }
    while (count > 0) {
        unsigned int chunk = (count > PSG_BLOCK) ? PSG_BLOCK : (unsigned int)count;
        unsigned int i;
        if (f0 <= 1 || amp0 == 0) c0 = PSG_IDLE_CTR;
        if (f1 <= 1 || amp1 == 0) c1 = PSG_IDLE_CTR;
        if (f2 <= 1 || amp2 == 0) c2 = PSG_IDLE_CTR;
        for (i = 0; i < chunk; ) {
            unsigned int W;
            int mix, wide;
            unsigned int new_frac;
            {
                int mc = c0;
                if (c1 < mc) mc = c1;
                if (c2 < mc) mc = c2;
                if (ampn && n_ctr < mc) mc = n_ctr;
                if (mc > wm2) {
                    unsigned int k = (unsigned int)(mc - 1) / wm, fr, q;
                    int T;
                    unsigned char v;
                    if (k > chunk - i) k = chunk - i;
                    fr = (unsigned int)clock_frac + k * (unsigned int)ticks_frac;
                    T = (int)(k * ticks_int + (fr >> 8));
                    v = clut[base + s0 + s1 + s2 + sn];
                    clock_frac = (unsigned char)(fr & 255);
                    c0 -= T; c1 -= T; c2 -= T; n_ctr -= T;
                    if (n_ctr <= 0) n_ctr = np - ((-n_ctr) % np);
                    for (q = 0; q < k; q++) *out++ = v;
                    i += k;
                    continue;
                }
            }
            new_frac = (unsigned int)clock_frac + ticks_frac;
            if (new_frac >= 256) {
                clock_frac = (unsigned char)(new_frac - 256);
                W = ticks_int + 1;
                wide = 1;
            } else {
                clock_frac = (unsigned char)new_frac;
                W = ticks_int;
                wide = 0;
            }
            mix = base;
            TONE_FAST(f0, c0, o0, amp0, s0, wide ? k0b : k0a)
            TONE_FAST(f1, c1, o1, amp1, s1, wide ? k1b : k1a)
            TONE_FAST(f2, c2, o2, amp2, s2, wide ? k2b : k2a)
            n_ctr -= (int)W;
            if (n_ctr <= 0) {
                unsigned short fb;
                n_ctr += np;
                n_out = (unsigned char)(n_shift & 1);
                if (n_ctrl & 0x04)
                    fb = (unsigned short)((n_shift & 1) ^ ((n_shift >> 3) & 1));
                else
                    fb = (unsigned short)(n_shift & 1);
                n_shift = (unsigned short)((n_shift >> 1) | (fb << 15));
                sn = n_out ? ampn : -ampn;
            }
            *out++ = clut[mix + sn];
            i++;
        }
        count -= chunk;
    }
    if (f0 <= 1 || amp0 == 0) c0 = sc0;
    if (f1 <= 1 || amp1 == 0) c1 = sc1;
    if (f2 <= 1 || amp2 == 0) c2 = sc2;
    p->clock_accum = clock_frac;
    p->tone_counter[0] = c0; p->tone_counter[1] = c1; p->tone_counter[2] = c2;
    p->tone_output[0] = o0;  p->tone_output[1] = o1;  p->tone_output[2] = o2;
    p->noise_counter = n_ctr;
    p->noise_shift = n_shift;
    p->noise_output = n_out;
}

void psg_full_run_batched(PsgFull *p, unsigned char far *out, unsigned long total_samples,
                          const PsgWrite *writes, int n_writes)
{
    unsigned long pos = 0;
    int wi = 0;
    while (pos < total_samples) {
        unsigned long next_stop = total_samples;
        if (wi < n_writes && (unsigned long)writes[wi].sample_offset < next_stop)
            next_stop = writes[wi].sample_offset;
        if (next_stop > pos)
            psg_full_run(p, out + pos, next_stop - pos);
        pos = next_stop;
        while (wi < n_writes && (unsigned long)writes[wi].sample_offset == pos) {
            psg_full_write(p, writes[wi].data);
            wi++;
        }
    }
}

static unsigned char vol_index_for(int amp)
{
    int k, best = 15, bd = 1000;
    for (k = 0; k < 16; k++) {
        int d = (int)tone_vol_table[k] - amp;
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = k; }
    }
    return (unsigned char)best;
}

void psg_full_run_t6w28(PsgFull *chip_tone, PsgFull *chip_noise,
                         unsigned char far *out, unsigned long count)
{
    unsigned char sv[3], svn = chip_tone->noise_volume, sctrl = chip_tone->noise_ctrl;
    unsigned short sshift = chip_tone->noise_shift;
    long scnt = chip_tone->noise_counter;
    unsigned char sout = chip_tone->noise_output;
    int k;
    for (k = 0; k < 3; k++) {
        sv[k] = chip_tone->tone_volume[k];
        chip_tone->tone_volume[k] = vol_index_for(((int)tone_vol_table[sv[k]] +
                                                   (int)tone_vol_table[chip_noise->tone_volume[k]]) >> 1);
    }
    chip_tone->noise_volume = vol_index_for(((int)noise_vol_table[svn] +
                                             (int)noise_vol_table[chip_noise->noise_volume]) >> 1);
    chip_tone->noise_ctrl = chip_noise->noise_ctrl;
    chip_tone->noise_shift = chip_noise->noise_shift;
    chip_tone->noise_counter = chip_noise->noise_counter;
    chip_tone->noise_output = chip_noise->noise_output;
    g_t6 = 1; g_t6_f2 = chip_noise->tone_freq[2];
    psg_full_run(chip_tone, out, count);
    g_t6 = 0;
    chip_noise->noise_shift = chip_tone->noise_shift;
    chip_noise->noise_counter = chip_tone->noise_counter;
    chip_noise->noise_output = chip_tone->noise_output;
    for (k = 0; k < 3; k++) chip_tone->tone_volume[k] = sv[k];
    chip_tone->noise_volume = svn; chip_tone->noise_ctrl = sctrl;
    chip_tone->noise_shift = sshift; chip_tone->noise_counter = scnt; chip_tone->noise_output = sout;
}

int psg_full_silent(const PsgFull *p)
{
    return (p->tone_volume[0] & p->tone_volume[1] & p->tone_volume[2] & p->noise_volume & 15) == 15;
}
