#include <string.h>
#include "pcengine.h"

static const unsigned char amp_tab[31] = {
    255, 215, 181, 152, 128, 108,  91,  76,  64,  54,  45,  38,  32,  27,  23,  19,
     16,  13,  11,  10,   8,   7,   6,   5,   4,   3,   3,   2,   2,   2,   1
};

static unsigned int fast_div16(unsigned long dividend, unsigned int divisor)
{
    if (divisor == 0) return 0;
    if ((dividend >> 16) >= divisor) return 0xFFFFU;
#ifdef __WATCOMC__
    {
    unsigned int hi = (unsigned int)(dividend >> 16);
    unsigned int lo = (unsigned int)(dividend & 0xFFFFUL);
    unsigned int quot;
    _asm {
        mov dx, hi
        mov ax, lo
        div divisor
        mov quot, ax
    }
    return quot;
    }
#else
    return (unsigned int)(dividend / divisor);
#endif
}

static unsigned int noise_prng(unsigned int state)
{
    state ^= (unsigned int)(state << 7);
    state ^= (unsigned int)(state >> 9);
    state ^= (unsigned int)(state << 8);
    return state;
}

static int side_amp(unsigned g, unsigned c, unsigned v)
{
    unsigned lg = g ? g * 2 + 1 : 0, lc = c ? c * 2 + 1 : 0;
    unsigned att = (31 - lg) + (31 - lc) + (31 - v) + 1;
    return att >= 31 ? 0 : amp_tab[att];
}

static int chan_amp5(PceApu *a, int ch)
{
    unsigned g = a->global_bal, b = a->bal[ch], v = a->vol[ch];
    int l = side_amp(g >> 4, b >> 4, v), r = side_amp(g & 15, b & 15, v);
    return (l + r) * 3;
}

void pceapu_init(PceApu *a, unsigned long clock, unsigned long sample_rate)
{
    int i;
    memset(a, 0, sizeof(*a));
    a->clock = clock;
    a->sample_rate = sample_rate;
    a->base_phase_wave = ((clock << 8) / sample_rate) << 3;
    a->base_phase_noise = (clock * 8UL) / sample_rate;
    a->click_delta = (int)((16UL * 44100UL) / sample_rate);
    if (a->click_delta < 16) a->click_delta = 16;
    a->prev_out = 128;
    a->global_bal = 0xFF;
    for (i = 0; i < 6; i++) {
        a->noise_lfsr[i] = (unsigned int)(0xACE1U + i * 0x1111U);
        a->bal[i] = 0xFF;
        a->freq_dirty[i] = 1;
        a->tab_dirty[i] = 1;
    }
    a->active_dirty = 1;
}

void pceapu_write_reg(PceApu *a, unsigned int addr, unsigned char data)
{
    unsigned char ch = a->sel_channel;
    int i;
    switch (addr) {
        case 0x00:
            a->sel_channel = (unsigned char)(data & 0x07);
            break;
        case 0x01:
            if (a->global_bal != data) {
                a->global_bal = data;
                for (i = 0; i < 6; i++) a->tab_dirty[i] = 1;
                a->active_dirty = 1;
            }
            break;
        case 0x02:
            if (ch < 6) { a->freq_lo[ch] = data; a->freq_dirty[ch] = 1; }
            break;
        case 0x03:
            if (ch < 6) { a->freq_hi[ch] = (unsigned char)(data & 0x0F); a->freq_dirty[ch] = 1; }
            break;
        case 0x04:
            if (ch < 6) {
                unsigned char new_on  = (unsigned char)((data >> 7) & 1);
                unsigned char new_dda = (unsigned char)((data >> 6) & 1);
                unsigned char new_vol = (unsigned char)(data & 0x1F);
                if (!new_on && new_dda) a->wave_widx[ch] = 0;
                if (new_vol != a->vol[ch]) { a->vol[ch] = new_vol; a->tab_dirty[ch] = 1; }
                if (new_on != a->on[ch] || new_dda != a->dda[ch]) {
                    if (a->dda[ch] && !new_dda) a->phase[ch] = 0;
                    a->on[ch] = new_on;
                    a->dda[ch] = new_dda;
                }
                a->active_dirty = 1;
            }
            break;
        case 0x05:
            if (ch < 6 && a->bal[ch] != data) {
                a->bal[ch] = data;
                a->tab_dirty[ch] = 1;
                a->active_dirty = 1;
            }
            break;
        case 0x06:
            if (ch < 6) {
                if (a->dda[ch]) {
                    if (a->dda_val[ch] != (data & 0x1F)) {
                        a->dda_val[ch] = (unsigned char)(data & 0x1F);
                        if (!a->active_dirty && !a->tab_dirty[ch] && a->on[ch]) {
                            int np_ = (((int)a->dda_val[ch] - 16) * a->c_amp5[ch]) >> 9;
                            a->c_dda_sum += np_ - a->c_dda_part[ch];
                            a->c_dda_part[ch] = np_;
                        } else a->active_dirty = 1;
                    }
                } else if (!a->on[ch]) {
                    a->wave[ch][a->wave_widx[ch]] = (unsigned char)(data & 0x1F);
                    a->wave_widx[ch] = (unsigned char)((a->wave_widx[ch] + 1) & 0x1F);
                    a->tab_dirty[ch] = 1;
                }
            }
            break;
        case 0x07:
            if (ch < 6) {
                a->noise_on[ch] = (unsigned char)((data >> 7) & 1);
                a->noise_freq[ch] = (unsigned char)(data & 0x1F);
                a->freq_dirty[ch] = 1;
                a->active_dirty = 1;
            }
            break;
        default:
            break;
    }
}

static unsigned int isqrt16(unsigned int v)
{
    unsigned int r = 1;
    while ((unsigned long)(r + 1) * (r + 1) <= v) r++;
    return r;
}

static void rebuild_channel(PceApu *a, int ch)
{
    int k, p[33];
    int amp5 = chan_amp5(a, ch);
    signed char *dst = a->c_ct[ch];
    a->c_amp5[ch] = amp5;
    for (k = 0; k < 32; k++) a->c_dlut[ch][k] = (signed char)(((k - 16) * amp5) >> 9);
    if (!amp5) { memset(dst, 0, PCE_CT_LEN); return; }
    for (k = 0; k < 32; k++) p[k] = ((int)a->wave[ch][k] - 16) * amp5;
    p[32] = p[0];
    for (k = 0; k < 32; k++) {
        int b = p[k], q = (p[k + 1] >> 2) - (b >> 2);
        dst[4 * k]     = (signed char)(b >> 9);
        dst[4 * k + 1] = (signed char)((b + q) >> 9);
        dst[4 * k + 2] = (signed char)((b + 2 * q) >> 9);
        dst[4 * k + 3] = (signed char)((b + 3 * q) >> 9);
    }
}

void pceapu_run(PceApu *a, unsigned char far *out, unsigned long count)
{
    int ch, k;
    unsigned int i, n = (unsigned int)count;
    int prev_out = a->prev_out;
    int click_delta = a->click_delta;
    int base;
    const signed char *tp[6];
    unsigned int ph[6], inc[6];
    unsigned int nph[6], ninc[6], nlfsr[6];
    int namp[6], nout[6];
    int nw, nn, ns;
    unsigned long sp[6], se[6], ss[6];
    const signed char *sl[6];
    unsigned char sr[6];
    int sv[6];
    const unsigned char far *bank = a->bank;
    for (ch = 0; ch < 6; ch++) {
        if (a->freq_dirty[ch]) {
            unsigned int freq12 = (unsigned int)a->freq_lo[ch] | ((unsigned int)a->freq_hi[ch] << 8);
            unsigned int nfreq = (unsigned int)(a->noise_freq[ch] ^ 0x1F);
            unsigned int kk;
            a->c_phase_inc[ch] = fast_div16(a->base_phase_wave, freq12);
            if (nfreq == 0) nfreq = 1;
            a->c_noise_phase_inc[ch] = fast_div16(a->base_phase_noise, nfreq);
            kk = (a->c_noise_phase_inc[ch] <= 256U) ? 256U : 4096U / isqrt16(a->c_noise_phase_inc[ch]);
            a->c_noise_amp[ch] = (int)kk;
            a->freq_dirty[ch] = 0;
            a->active_dirty = 1;
        }
        if (a->tab_dirty[ch]) {
            rebuild_channel(a, ch);
            a->tab_dirty[ch] = 0;
            a->active_dirty = 1;
        }
    }
    if (a->active_dirty) {
        a->c_n_wave_active = 0;
        a->c_n_noise_active = 0;
        a->c_dda_sum = 0;
        a->c_n_st = 0;
        for (ch = 0; ch < 6; ch++) {
            a->c_dda_part[ch] = 0;
            if (!a->on[ch] || !a->c_amp5[ch]) continue;
            if (a->dda[ch] && a->st_on[ch] && a->bank) {
                a->c_st_active[a->c_n_st++] = ch;
            } else if (a->dda[ch]) {
                a->c_dda_part[ch] = (((int)a->dda_val[ch] - 16) * a->c_amp5[ch]) >> 9;
                a->c_dda_sum += a->c_dda_part[ch];
            }
            else if (ch >= 4 && a->noise_on[ch])
                a->c_noise_active[a->c_n_noise_active++] = ch;
            else if (a->c_phase_inc[ch])
                a->c_wave_active[a->c_n_wave_active++] = ch;
        }
        a->active_dirty = 0;
    }
    nw = a->c_n_wave_active;
    nn = a->c_n_noise_active;
    ns = a->c_n_st;
    for (k = 0; k < ns; k++) {
        ch = a->c_st_active[k];
        sp[k] = a->st_pos[ch];
        se[k] = a->st_end[ch];
        ss[k] = a->st_step[ch];
        sl[k] = a->c_dlut[ch];
        sr[k] = a->dda_val[ch];
        sv[k] = sl[k][sr[k]];
    }
    base = 128 + a->c_dda_sum;
    for (k = 0; k < nw; k++) {
        ch = a->c_wave_active[k];
        tp[k] = a->c_ct[ch];
        ph[k] = a->phase[ch];
        inc[k] = a->c_phase_inc[ch];
    }
    for (k = 0; k < nn; k++) {
        long t;
        ch = a->c_noise_active[k];
        nph[k] = a->noise_phase[ch];
        ninc[k] = a->c_noise_phase_inc[ch];
        nlfsr[k] = a->noise_lfsr[ch];
        t = ((long)a->c_amp5[ch] * 31L * (long)a->c_noise_amp[ch]) >> 18;
        namp[k] = (int)t;
        nout[k] = a->noise_bit[ch] ? namp[k] : -namp[k];
    }
    {
    const signed char *t0 = tp[0], *t1 = tp[1], *t2 = tp[2], *t3 = tp[3], *t4 = tp[4], *t5 = tp[5];
    unsigned int p0 = ph[0], p1 = ph[1], p2 = ph[2], p3 = ph[3], p4 = ph[4], p5 = ph[5];
    unsigned int i0 = inc[0], i1 = inc[1], i2 = inc[2], i3 = inc[3], i4 = inc[4], i5 = inc[5];
    for (i = 0; i < n; i++) {
        int mix = base;
        switch (nw) {
            case 6: p5 += i5; mix += t5[p5 >> 9];
            case 5: p4 += i4; mix += t4[p4 >> 9];
            case 4: p3 += i3; mix += t3[p3 >> 9];
            case 3: p2 += i2; mix += t2[p2 >> 9];
            case 2: p1 += i1; mix += t1[p1 >> 9];
            case 1: p0 += i0; mix += t0[p0 >> 9];
            default: break;
        }
        for (k = 0; k < nn; k++) {
            unsigned int np = nph[k] + ninc[k];
            if ((np >> 8) != (nph[k] >> 8)) {
                nlfsr[k] = noise_prng(nlfsr[k]);
                nout[k] = (nlfsr[k] & 1) ? namp[k] : -namp[k];
            }
            nph[k] = np & 0x1FFU;
            mix += nout[k];
        }
        for (k = 0; k < ns; k++) {
            if (sp[k] < se[k]) {
                sr[k] = bank[(unsigned int)(sp[k] >> 16)];
                sv[k] = sl[k][sr[k]];
                sp[k] += ss[k];
            }
            mix += sv[k];
        }
        if (mix > 255) mix = 255;
        else if (mix < 0) mix = 0;
        if (mix - prev_out > click_delta) mix = prev_out + click_delta;
        else if (prev_out - mix > click_delta) mix = prev_out - click_delta;
        prev_out = mix;
        out[i] = (unsigned char)mix;
    }
    ph[0] = p0; ph[1] = p1; ph[2] = p2; ph[3] = p3; ph[4] = p4; ph[5] = p5;
    }
    for (k = 0; k < nw; k++) a->phase[a->c_wave_active[k]] = (unsigned short)ph[k];
    for (k = 0; k < nn; k++) {
        ch = a->c_noise_active[k];
        a->noise_phase[ch] = nph[k];
        a->noise_lfsr[ch] = nlfsr[k];
        a->noise_bit[ch] = (unsigned char)(nout[k] > 0);
    }
    for (k = 0; k < ns; k++) {
        ch = a->c_st_active[k];
        a->st_pos[ch] = sp[k];
        a->dda_val[ch] = sr[k];
        if (sp[k] >= se[k]) { a->st_on[ch] = 0; a->active_dirty = 1; }
    }
    a->prev_out = prev_out;
}

void pceapu_set_bank(PceApu *a, const unsigned char far *bank, unsigned long size)
{
    a->bank = bank;
    a->bank_size = size;
}

void pceapu_stream_freq(PceApu *a, int ch, unsigned long hz)
{
    if (ch < 0 || ch > 5 || !a->sample_rate) return;
    if (hz > 65535UL) hz = 65535UL;
    a->st_step[ch] = (hz << 16) / a->sample_rate;
}

void pceapu_stream_start(PceApu *a, int ch, unsigned long off, unsigned long len)
{
    if (ch < 0 || ch > 5 || !a->bank || off >= a->bank_size) return;
    if (off + len > a->bank_size) len = a->bank_size - off;
    a->st_pos[ch] = off << 16;
    a->st_end[ch] = (off + len) << 16;
    a->st_on[ch] = 1;
    a->active_dirty = 1;
}

void pceapu_stream_stop(PceApu *a, int ch)
{
    if (ch < 0 || ch > 5) return;
    a->st_on[ch] = 0;
    a->active_dirty = 1;
}
