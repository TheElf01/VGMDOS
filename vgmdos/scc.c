#include <string.h>
#include "scc.h"

#define FIXED_SHIFT 8
#define FIXED_ONE (1UL << FIXED_SHIFT)

static signed char scc_mix_table[16][256];
static int scc_table_built = 0;

static void build_scc_mix_table(void);
static const Scc *s_owner;
static int scc_div = 120;

void scc_set_gain(int solo)
{
    int d = solo ? 92 : 120;
    if (d != scc_div) { scc_div = d; scc_table_built = 0; }
    build_scc_mix_table();
    s_owner = 0;
}

static void build_scc_mix_table(void)
{
    int vol, idx;
    if (scc_table_built) return;
    for (vol = 0; vol < 16; vol++) {
        for (idx = 0; idx < 256; idx++) {
            signed char wave = (signed char)idx;
            { int p_ = (int)wave * vol, h_ = scc_div >> 1; scc_mix_table[vol][idx] = (signed char)((p_ >= 0 ? p_ + h_ : p_ - h_) / scc_div); }
        }
    }
    scc_table_built = 1;
}

static signed char s_ct[5][SCC_CT_LEN];
static const Scc *s_owner = 0;
static unsigned int c_per[5], c_pinc[5];
static unsigned long c_base;
static const Scc *c_base_owner = 0;

static void scc_rebuild_ct(Scc *s, unsigned char mask)
{
    int ch, k;
    if (s_owner != s) { mask = 0x1F; s_owner = s; }
    for (ch = 0; ch < 5; ch++) {
        signed char *dst = s_ct[ch];
        const signed char *w = s->waveform[ch];
        const signed char *mt = scc_mix_table[s->volume[ch]];
        if (!(mask & (1 << ch))) continue;
        if (!s->enable[ch]) { memset(dst, 0, SCC_CT_LEN); continue; }
        for (k = 0; k < 32; k++) {
            int a = w[k], d = w[(k + 1) & 31] - a, v = a << 3, j;
            signed char *t = dst + (k << 3);
            for (j = 0; j < 8; j++, v += d) t[j] = mt[(unsigned char)(v >> 3)];
        }
    }
}

void scc_init(Scc *s, unsigned long clock, unsigned long sample_rate)
{
    int i;
    unsigned long ticks_fp;
    build_scc_mix_table();
    memset(s, 0, sizeof(*s));
    s->ct_dirty = 0x1F;
    s->clock = clock;
    s->sample_rate = sample_rate;
    for (i = 0; i < 5; i++)
        s->phase_counter[i] = 0;
    s_owner = 0;
    c_base_owner = 0;
    ticks_fp = (unsigned long)((clock << FIXED_SHIFT) / sample_rate);
    s->ticks_int = (unsigned int)(ticks_fp >> FIXED_SHIFT);
    s->ticks_frac = (unsigned char)(ticks_fp & (FIXED_ONE - 1));
}

void scc_write_reg(Scc *s, unsigned addr, unsigned char data)
{
    if (addr >= 0x100 && addr <= 0x19F) {
        if (s->waveform[(addr - 0x100) >> 5][addr & 31] != (signed char)data)
            s->ct_dirty |= (unsigned char)(1 << ((addr - 0x100) >> 5));
    } else if (addr <= 0x5F) {
        if (s->waveform[addr >> 5][addr & 31] != (signed char)data)
            s->ct_dirty |= (unsigned char)(1 << (addr >> 5));
    } else if (addr <= 0x7F) {
        if (s->waveform[3][addr & 31] != (signed char)data ||
            s->waveform[4][addr & 31] != (signed char)data) s->ct_dirty |= 0x18;
    }
    else if (addr >= 0x8A && addr <= 0x8E) {
        if (s->volume[addr - 0x8A] != (data & 0x0F)) s->ct_dirty |= (unsigned char)(1 << (addr - 0x8A));
    } else if (addr == 0x8F) {
        unsigned char old = (unsigned char)(s->enable[0] | (s->enable[1] << 1) | (s->enable[2] << 2) |
                                            (s->enable[3] << 3) | (s->enable[4] << 4));
        s->ct_dirty |= (unsigned char)((old ^ data) & 0x1F);
    }
    if (addr >= 0x100 && addr <= 0x19F) {
        unsigned ch = (addr - 0x100) / 32;
        unsigned pos = (addr - 0x100) % 32;
        if (ch < 5) s->waveform[ch][pos] = (signed char)data;
        return;
    }
    if (addr <= 0x5F) {
        unsigned ch = addr / 32;
        unsigned pos = addr % 32;
        s->waveform[ch][pos] = (signed char)data;
    } else if (addr <= 0x7F) {
        unsigned pos = addr - 0x60;
        s->waveform[3][pos] = (signed char)data;
        s->waveform[4][pos] = (signed char)data;
    } else if (addr <= 0x89) {
        unsigned ch = (addr - 0x80) / 2;
        if ((addr - 0x80) % 2 == 0)
            s->freq[ch] = (unsigned short)((s->freq[ch] & 0x0F00) | data);
        else
            s->freq[ch] = (unsigned short)((s->freq[ch] & 0x00FF) | ((data & 0x0F) << 8));
    } else if (addr <= 0x8E) {
        unsigned ch = addr - 0x8A;
        s->volume[ch] = data & 0x0F;
    } else if (addr == 0x8F) {
        int ch;
        for (ch = 0; ch < 5; ch++)
            s->enable[ch] = (data & (1 << ch)) ? 1 : 0;
    }
}

static unsigned int c_ph0, c_ph1, c_ph2, c_ph3, c_ph4;
static unsigned int c_inc0, c_inc1, c_inc2, c_inc3, c_inc4, c_n;
static unsigned char far *c_out;

static void scc_prep(Scc *s)
{
    int ch;
    unsigned long base;
    if (c_base_owner != s) {
        c_base = (((s->clock >> 1) << 8) / s->sample_rate) << 4;
        c_per[0] = c_per[1] = c_per[2] = c_per[3] = c_per[4] = 0xFFFF;
        c_base_owner = s;
    }
    base = c_base;
    if (s->ct_dirty || s_owner != s) { scc_rebuild_ct(s, s->ct_dirty); s->ct_dirty = 0; }
    for (ch = 0; ch < 5; ch++) {
        unsigned int inc = 0, p = s->freq[ch];
        if (s->enable[ch] && p > 8) {
            if (p != c_per[ch]) {
                unsigned long v = base / (p + 1U);
                c_per[ch] = p;
                c_pinc[ch] = v > 0xFFFFUL ? 0 : (unsigned int)v;
            }
            inc = c_pinc[ch];
        }
        switch (ch) {
            case 0: c_inc0 = inc; break;
            case 1: c_inc1 = inc; break;
            case 2: c_inc2 = inc; break;
            case 3: c_inc3 = inc; break;
            default: c_inc4 = inc; break;
        }
    }
    c_ph0 = (unsigned int)s->phase_counter[0]; c_ph1 = (unsigned int)s->phase_counter[1];
    c_ph2 = (unsigned int)s->phase_counter[2]; c_ph3 = (unsigned int)s->phase_counter[3];
    c_ph4 = (unsigned int)s->phase_counter[4];
}

static void scc_done(Scc *s)
{
    s->phase_counter[0] = c_ph0; s->phase_counter[1] = c_ph1; s->phase_counter[2] = c_ph2;
    s->phase_counter[3] = c_ph3; s->phase_counter[4] = c_ph4;
}

static void scc_mix_set(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov cx, c_n
        les di, c_out
        mov si, c_ph0
        mov dx, c_ph1
        mov ax, c_ph2
        push bp
        mov bp, ax
    slp:
        mov al, 128
        add si, c_inc0
        mov bx, si
        mov bl, bh
        xor bh, bh
        add al, s_ct[bx]
        add dx, c_inc1
        mov bx, dx
        mov bl, bh
        xor bh, bh
        add al, s_ct+256[bx]
        add bp, c_inc2
        mov bx, bp
        mov bl, bh
        xor bh, bh
        add al, s_ct+512[bx]
        mov bx, c_ph3
        add bx, c_inc3
        mov c_ph3, bx
        mov bl, bh
        xor bh, bh
        add al, s_ct+768[bx]
        mov bx, c_ph4
        add bx, c_inc4
        mov c_ph4, bx
        mov bl, bh
        xor bh, bh
        add al, s_ct+1024[bx]
        stosb
        dec cx
        jz  sdone
        jmp slp
    sdone:
        mov ax, bp
        pop bp
        mov c_ph2, ax
        mov c_ph0, si
        mov c_ph1, dx
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

static void scc_mix_add(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov cx, c_n
        les di, c_out
        mov si, c_ph0
        mov dx, c_ph1
        mov ax, c_ph2
        push bp
        mov bp, ax
    alp2:
        xor al, al
        add si, c_inc0
        mov bx, si
        mov bl, bh
        xor bh, bh
        add al, s_ct[bx]
        add dx, c_inc1
        mov bx, dx
        mov bl, bh
        xor bh, bh
        add al, s_ct+256[bx]
        add bp, c_inc2
        mov bx, bp
        mov bl, bh
        xor bh, bh
        add al, s_ct+512[bx]
        mov bx, c_ph3
        add bx, c_inc3
        mov c_ph3, bx
        mov bl, bh
        xor bh, bh
        add al, s_ct+768[bx]
        mov bx, c_ph4
        add bx, c_inc4
        mov c_ph4, bx
        mov bl, bh
        xor bh, bh
        add al, s_ct+1024[bx]
        cbw
        mov bl, es:[di]
        xor bh, bh
        add ax, bx
        test ah, ah
        jz  aok2
        sar ah, 1
        not ah
        mov al, ah
    aok2:
        stosb
        dec cx
        jz  adone2
        jmp alp2
    adone2:
        mov ax, bp
        pop bp
        mov c_ph2, ax
        mov c_ph0, si
        mov c_ph1, dx
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

void scc_run(Scc *s, unsigned char far *out, unsigned long count)
{
    scc_prep(s);
    while (count) {
        unsigned int n = count > 60000UL ? 60000U : (unsigned int)count;
        count -= n;
        c_out = out; c_n = n;
        scc_mix_set();
        out += n;
    }
    scc_done(s);
}

void scc_run_additive(Scc *s, unsigned char far *out, unsigned long count)
{
    scc_prep(s);
    while (count) {
        unsigned int n = count > 60000UL ? 60000U : (unsigned int)count;
        count -= n;
        c_out = out; c_n = n;
        scc_mix_add();
        out += n;
    }
    scc_done(s);
}
