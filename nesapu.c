#include <string.h>
#include "nesapu.h"
#include "dosmem.h"
#include "fastmath.h"

#define NES_NOISE_LONG_PERIOD 32767UL
#define NES_NOISE_SHORT_PERIOD 93UL

static DosBuffer noise_long_buf, noise_short_buf;
static unsigned char far *noise_long_table = 0;
static unsigned char far *noise_short_table = 0;
static int nes_noise_tables_built = 0;

static void build_nes_noise_tables(void)
{
    unsigned short shift;
    unsigned long i;
    if (nes_noise_tables_built) return;
{
    DosBuffer *bufs[2];
    unsigned sizes[2];
    int i;
    bufs[0] = &noise_long_buf; bufs[1] = &noise_short_buf;
    sizes[0] = (unsigned)NES_NOISE_LONG_PERIOD;
    sizes[1] = (unsigned)NES_NOISE_SHORT_PERIOD;
    for (i = 0; i < 2; i++) {
        if (!dosmem_alloc_simple(bufs[i], sizes[i])) {
            int j;
            for (j = 0; j < i; j++) dosmem_free(bufs[j]);
            return;
        }
    }
}
    noise_long_table = noise_long_buf.ptr;
    noise_short_table = noise_short_buf.ptr;
    shift = 1;
    for (i = 0; i < NES_NOISE_LONG_PERIOD; i++) {
        noise_long_table[i] = (unsigned char)(shift & 1);
        {
            unsigned short fb = (unsigned short)((shift ^ (shift >> 1)) & 1);
            shift = (unsigned short)((shift >> 1) | (fb << 14));
        }
    }
    shift = 1;
    for (i = 0; i < NES_NOISE_SHORT_PERIOD; i++) {
        noise_short_table[i] = (unsigned char)(shift & 1);
        {
            unsigned short fb = (unsigned short)((shift ^ (shift >> 6)) & 1);
            shift = (unsigned short)((shift >> 1) | (fb << 14));
        }
    }
    nes_noise_tables_built = 1;
}

void nesapu_free_noise_tables(void)
{
    if (!nes_noise_tables_built) return;
    dosmem_free(&noise_long_buf);
    dosmem_free(&noise_short_buf);
    noise_long_table = 0;
    noise_short_table = 0;
    nes_noise_tables_built = 0;
}

#define FIXED_SHIFT 8
#define FIXED_ONE (1UL << FIXED_SHIFT)
#define MAX_CLICK_DELTA 10

static const unsigned char length_table[32] = {
    10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,
    12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30
};

static const unsigned char duty_table[4][8] = {
    {0,1,0,0,0,0,0,0},
    {0,1,1,0,0,0,0,0},
    {0,1,1,1,1,0,0,0},
    {1,0,0,1,1,1,1,1}
};

static const unsigned short noise_period_table[16] = {
    4,8,16,32,64,96,128,160,202,254,380,508,762,1016,2034,4068
};

static const signed char triangle_seq[32] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0
};

void nesapu_init(NesApu *a, unsigned long clock, unsigned long sample_rate)
{
    build_nes_noise_tables();
    memset(a, 0, sizeof(*a));
    a->output_mask = 0xFF;
    a->enable_mask = 0x0F;
    a->clock = clock;
    a->sample_rate = sample_rate;
    a->prev_out = 128;
    a->click_delta = (int)((64UL * 44100UL) / sample_rate);
    if (a->click_delta < 64) a->click_delta = 64;
    a->base_phase_pulse = (clock * 512UL) / sample_rate;
    a->base_phase_tri = (clock * 1024UL) / sample_rate;
    a->ticks_apu_precalc = (unsigned long)((clock << FIXED_SHIFT) / (2UL * sample_rate));
    a->frame_reload_precalc = (long)((clock / 2UL) / 240UL);
    if (a->frame_reload_precalc < 1) a->frame_reload_precalc = 1;
    a->frame_counter = a->frame_reload_precalc;
    a->frame_q8 = (unsigned int)((sample_rate * 256UL) / 240UL);
    a->frame_left = a->frame_q8 >> 8;
    a->frame_acc = a->frame_q8 & 0xFF;
    a->base_q8 = (clock * 256UL) / sample_rate;
    a->dmc_inc = (unsigned int)(a->base_q8 / 428U);
    a->boff = 128;
}

static const unsigned short dmc_rate_table[16] = {
    428,380,340,320,286,254,226,214,190,160,142,128,106,84,72,54
};

static void dmc_restart(NesApu *a)
{
    a->dmc_cur = (unsigned int)a->dmc_addr_reg << 6;
    a->dmc_left = ((unsigned int)a->dmc_len_reg << 4) + 1;
    a->dmc_playing = (a->dmc_ram != 0);
}

void nesapu_set_ram(NesApu *a, const unsigned char far *ram)
{
    a->dmc_ram = ram;
}

void nesapu_set_pcm(NesApu *a, const unsigned char far *ring)
{
    a->pcm = ring;
}

void nesapu_stream_freq(NesApu *a, unsigned long hz)
{
    unsigned long st;
    if (hz == 0 || hz > 65535UL) return;
    st = (hz << 16) / a->sample_rate;
    a->st_si = (unsigned int)(st >> 16);
    a->st_sf = (unsigned int)(st & 0xFFFF);
}

void nesapu_stream_start(NesApu *a, unsigned long off, unsigned long len)
{
    if (!a->pcm || len == 0) return;
    a->st_idx = (unsigned int)off & (NES_PCM_RING - 1);
    a->st_frac = 0;
    a->st_left = len > 65535UL ? 65535U : (unsigned int)len;
    a->st_on = 1;
    a->dmc_level = a->pcm[a->st_idx] & 0x7F;
    a->dmc_out = (unsigned char)(a->dmc_level >> 2);
}

void nesapu_stream_stop(NesApu *a)
{
    a->st_on = 0;
}

static void dmc_clock(NesApu *a, unsigned int bits)
{
    unsigned char lv = a->dmc_level, sr = a->dmc_sr, nb = a->dmc_bits;
    while (bits--) {
        if (nb == 0) {
            if (a->dmc_left == 0) { a->dmc_playing = 0; break; }
            sr = a->dmc_ram[a->dmc_cur & 0x3FFF];
            a->dmc_cur++;
            nb = 8;
            if (--a->dmc_left == 0 && a->dmc_loop) dmc_restart(a);
        }
        if (sr & 1) { if (lv <= 125) lv += 2; }
        else        { if (lv >= 2) lv -= 2; }
        sr >>= 1;
        nb--;
    }
    a->dmc_level = lv; a->dmc_sr = sr; a->dmc_bits = nb;
    a->dmc_out = (unsigned char)(lv >> 2);
}

void nesapu_write_reg(NesApu *a, unsigned reg, unsigned char data)
{
    switch (reg) {
        case 0x00: case 0x04: {
            int ch = (reg >= 0x04) ? 1 : 0;
            a->duty[ch] = (data >> 6) & 0x03;
            a->len_halt[ch] = (data >> 5) & 0x01;
            a->const_vol[ch] = (data >> 4) & 0x01;
            a->vol_param[ch] = data & 0x0F;
            break;
        }
        case 0x01: case 0x05: {
            int ch = (reg >= 0x05) ? 1 : 0;
            a->sweep_enable[ch] = (data >> 7) & 0x01;
            a->sweep_period[ch] = (data >> 4) & 0x07;
            a->sweep_negate[ch] = (data >> 3) & 0x01;
            a->sweep_shift[ch] = data & 0x07;
            a->sweep_reload[ch] = 1;
            break;
        }
        case 0x02: case 0x06: {
            int ch = (reg >= 0x06) ? 1 : 0;
            a->period[ch] = (unsigned short)((a->period[ch] & 0x0700) | data);
            break;
        }
        case 0x03: case 0x07: {
            int ch = (reg >= 0x07) ? 1 : 0;
            a->period[ch] = (unsigned short)((a->period[ch] & 0x00FF) | ((data & 0x07) << 8));
            if (a->enable_mask & (1 << ch))
                a->length[ch] = length_table[(data >> 3) & 0x1F];
            a->phase[ch] = 0;
            a->env_start[ch] = 1;
            break;
        }
        case 0x08:
            a->tri_halt = (data >> 7) & 0x01;
            a->tri_reload_val = data & 0x7F;
            break;
        case 0x0A:
            a->tri_period = (unsigned short)((a->tri_period & 0x0700) | data);
            break;
        case 0x0B:
            a->tri_period = (unsigned short)((a->tri_period & 0x00FF) | ((data & 0x07) << 8));
            if (a->enable_mask & 0x04)
                a->tri_length = length_table[(data >> 3) & 0x1F];
            a->tri_reload_flag = 1;
            break;
        case 0x0C:
            a->noise_halt = (data >> 5) & 0x01;
            a->noise_const_vol = (data >> 4) & 0x01;
            a->noise_vol_param = data & 0x0F;
            break;
        case 0x0E:
            a->noise_mode = (data >> 7) & 0x01;
            a->noise_period_idx = data & 0x0F;
            break;
        case 0x0F:
            if (a->enable_mask & 0x08)
                a->noise_length = length_table[(data >> 3) & 0x1F];
            a->noise_env_decay = 15;
            a->noise_env_divider = a->noise_vol_param;
            a->noise_env_start = 0;
            break;
        case 0x10:
            a->dmc_loop = (data >> 6) & 1;
            a->dmc_rate_idx = data & 0x0F;
            a->dmc_inc = (unsigned int)(a->base_q8 / dmc_rate_table[data & 0x0F]);
            break;
        case 0x11:
            a->dmc_level = data & 0x7F;
            a->dmc_out = (unsigned char)(a->dmc_level >> 2);
            break;
        case 0x12: a->dmc_addr_reg = data; break;
        case 0x13: a->dmc_len_reg = data; break;
        case 0x15:
            if (data & 0x10) { if (a->dmc_left == 0) dmc_restart(a); }
            else a->dmc_left = 0;
            a->enable_mask = data & 0x1F;
            if (!(data & 0x01)) a->length[0] = 0;
            if (!(data & 0x02)) a->length[1] = 0;
            if (!(data & 0x04)) a->tri_length = 0;
            if (!(data & 0x08)) a->noise_length = 0;
            break;
        case 0x17:
            a->frame_mode = (data >> 7) & 0x01;
            a->frame_step = 0;
            break;
        default:
            break;
    }
}

static void frame_tick(NesApu *a, int do_half)
{
    if (a->env_start[0]) {
        a->env_start[0] = 0; a->env_decay[0] = 15; a->env_divider[0] = a->vol_param[0];
    } else if (a->env_divider[0] == 0) {
        a->env_divider[0] = a->vol_param[0];
        if (a->env_decay[0] > 0) a->env_decay[0]--;
        else if (a->len_halt[0]) a->env_decay[0] = 15;
    } else {
        a->env_divider[0]--;
    }
    if (a->env_start[1]) {
        a->env_start[1] = 0; a->env_decay[1] = 15; a->env_divider[1] = a->vol_param[1];
    } else if (a->env_divider[1] == 0) {
        a->env_divider[1] = a->vol_param[1];
        if (a->env_decay[1] > 0) a->env_decay[1]--;
        else if (a->len_halt[1]) a->env_decay[1] = 15;
    } else {
        a->env_divider[1]--;
    }
    if (a->noise_env_divider == 0) {
        a->noise_env_divider = a->noise_vol_param;
        if (a->noise_env_decay > 0) a->noise_env_decay--;
        else if (a->noise_halt) a->noise_env_decay = 15;
    } else {
        a->noise_env_divider--;
    }
    if (a->tri_reload_flag) {
        a->tri_linear_counter = a->tri_reload_val;
    } else if (a->tri_linear_counter > 0) {
        a->tri_linear_counter--;
    }
    if (!a->tri_halt) a->tri_reload_flag = 0;
    if (!do_half) return;
    if (a->length[0] > 0 && !a->len_halt[0]) a->length[0]--;
    if (a->sweep_divider[0] == 0 && a->sweep_enable[0] && a->sweep_shift[0]) {
        int delta = a->period[0] >> a->sweep_shift[0];
        int newperiod = a->sweep_negate[0] ? (a->period[0] - delta - 1) : (a->period[0] + delta);
        if (newperiod >= 0 && newperiod <= 0x7FF && a->period[0] >= 8)
            a->period[0] = (unsigned short)newperiod;
    }
    if (a->sweep_divider[0] == 0 || a->sweep_reload[0]) {
        a->sweep_divider[0] = a->sweep_period[0]; a->sweep_reload[0] = 0;
    } else {
        a->sweep_divider[0]--;
    }
    if (a->length[1] > 0 && !a->len_halt[1]) a->length[1]--;
    if (a->sweep_divider[1] == 0 && a->sweep_enable[1] && a->sweep_shift[1]) {
        int delta = a->period[1] >> a->sweep_shift[1];
        int newperiod = a->sweep_negate[1] ? (a->period[1] - delta) : (a->period[1] + delta);
        if (newperiod >= 0 && newperiod <= 0x7FF && a->period[1] >= 8)
            a->period[1] = (unsigned short)newperiod;
    }
    if (a->sweep_divider[1] == 0 || a->sweep_reload[1]) {
        a->sweep_divider[1] = a->sweep_period[1]; a->sweep_reload[1] = 0;
    } else {
        a->sweep_divider[1]--;
    }
    if (a->tri_length > 0 && !a->tri_halt) a->tri_length--;
    if (a->noise_length > 0 && !a->noise_halt) a->noise_length--;
}

static const unsigned char duty_mask_tab[4] = { 0x02, 0x06, 0x1E, 0x7D };

static unsigned char pt0t[256], pt1t[256];
static unsigned char pt_key[2] = { 0xFF, 0xFF };
static unsigned int pt_kinc[2];
static unsigned char tri256[256];
static unsigned char tri_cur[256];
static unsigned char tri_cur_on = 0xFF;
static unsigned char out_tab[256];
static const unsigned char duty_hi[4] = { 1, 2, 4, 6 };
static unsigned int s_dc;
static unsigned char dmc_buf[256];
static int tabs_built = 0;

static unsigned int s_ph0, s_ph1, s_th, s_inc0, s_inc1, s_tinc;
static unsigned int s_nacc, s_ninc, s_nidx, s_nper, s_nseg;
static unsigned char s_namp;
static unsigned char far *s_out;
static unsigned int s_n;

static void build_tabs(void)
{
    int k;
    for (k = 0; k < 256; k++) tri256[k] = (unsigned char)(triangle_seq[k >> 3] << 1);
    for (k = 0; k < 256; k++) {
        int o = 128 + ((k - 128) * 7) / 4;
        out_tab[k] = (unsigned char)(o < 0 ? 0 : (o > 255 ? 255 : o));
    }
    tabs_built = 1;
}

static unsigned int phase_inc(unsigned long base, unsigned int div)
{
    if ((unsigned int)(base >> 16) >= div) return 0;
    return fast_div16(base, div);
}

static void pulse_tab(unsigned char *t, unsigned char duty, unsigned char v, unsigned int inc)
{
    unsigned char m = duty_mask_tab[duty];
    int s;
    for (s = 0; s < 8; s++) memset(t + (s << 5), ((m >> s) & 1) ? v : 0, 32);
    if (v == 0 || inc < 256 || inc > 8192) return;
    {
        unsigned int rinc = (unsigned int)(((unsigned long)v << 16) / inc);
        for (s = 0; s < 8; s++) {
            unsigned char prev = (unsigned char)((m >> ((s + 7) & 7)) & 1);
            unsigned char cur = (unsigned char)((m >> s) & 1);
            unsigned int d, j;
            if (prev == cur) continue;
            for (d = 128, j = (unsigned int)(s << 5); d < inc; d += 256, j = (j + 1) & 255) {
                unsigned char f = (unsigned char)(((unsigned long)d * rinc) >> 16);
                t[j] = cur ? f : (unsigned char)(v - f);
            }
        }
    }
}

static void nes_prep(NesApu *a)
{
    unsigned char en = (unsigned char)(a->enable_mask & a->output_mask);
    unsigned short period0 = a->period[0], period1 = a->period[1];
    unsigned char v0 = 0, v1 = 0, k0, k1, ton;
    if ((en & 0x01) && a->length[0] > 0 && period0 >= 8)
        v0 = a->const_vol[0] ? a->vol_param[0] : a->env_decay[0];
    if ((en & 0x02) && a->length[1] > 0 && period1 >= 8)
        v1 = a->const_vol[1] ? a->vol_param[1] : a->env_decay[1];
    k0 = (unsigned char)((a->duty[0] << 4) | v0);
    k1 = (unsigned char)((a->duty[1] << 4) | v1);
    s_inc0 = period0 >= 8 ? phase_inc(a->base_phase_pulse << 3, (unsigned int)period0 + 1) : 0;
    s_inc1 = period1 >= 8 ? phase_inc(a->base_phase_pulse << 3, (unsigned int)period1 + 1) : 0;
    if (pt_key[0] != k0 || pt_kinc[0] != s_inc0) {
        pulse_tab(pt0t, a->duty[0], v0, s_inc0); pt_key[0] = k0; pt_kinc[0] = s_inc0;
    }
    if (pt_key[1] != k1 || pt_kinc[1] != s_inc1) {
        pulse_tab(pt1t, a->duty[1], v1, s_inc1); pt_key[1] = k1; pt_kinc[1] = s_inc1;
    }
    ton = (unsigned char)((en & 0x04) ? 1 : 0);
    if (tri_cur_on != ton) {
        if (ton) memcpy(tri_cur, tri256, 256); else memset(tri_cur, 0, 256);
        tri_cur_on = ton;
    }
    s_tinc = 0;
    if (a->tri_linear_counter > 0 && a->tri_length > 0 && a->tri_period >= 2)
        s_tinc = phase_inc(a->base_phase_tri << 1, (unsigned int)a->tri_period + 1);
    s_namp = 0;
    s_nper = a->noise_mode ? (unsigned int)NES_NOISE_SHORT_PERIOD : (unsigned int)NES_NOISE_LONG_PERIOD;
    s_ninc = (unsigned int)(a->base_q8 / noise_period_table[a->noise_period_idx]);
    if (s_nidx >= s_nper) s_nidx = 0;
    if (nes_noise_tables_built && (en & 0x08) && a->noise_length > 0) {
        const unsigned char far *t = a->noise_mode ? noise_short_table : noise_long_table;
        int amp = a->noise_const_vol ? a->noise_vol_param : a->noise_env_decay;
        s_nseg = FP_SEG(t) + (FP_OFF(t) >> 4);
        if ((FP_OFF(t) & 15) == 0)
            s_namp = (unsigned char)(amp - (amp >> 3));
    }
    s_dc = ((unsigned int)v0 * duty_hi[a->duty[0]] + (unsigned int)v1 * duty_hi[a->duty[1]]) >> 3;
    s_dc += s_tinc ? 15 : tri_cur[s_th >> 8];
    s_dc += s_namp >> 1;
}

static void nes_frame(NesApu *a)
{
    if (!a->frame_mode) {
        frame_tick(a, (a->frame_step == 1 || a->frame_step == 3));
        a->frame_step = (a->frame_step + 1) & 0x03;
    } else {
        frame_tick(a, (a->frame_step == 1 || a->frame_step == 4));
        a->frame_step++;
        if (a->frame_step >= 5) a->frame_step = 0;
    }
    a->frame_acc += a->frame_q8;
    a->frame_left = a->frame_acc >> 8;
    a->frame_acc &= 0xFF;
}

static void dmc_render(NesApu *a, unsigned int n)
{
    if (a->st_on) {
        const unsigned char far *pc = a->pcm;
        unsigned int idx = a->st_idx, f = a->st_frac, left = a->st_left;
        unsigned int si = a->st_si, sf = a->st_sf;
        unsigned char lv = a->dmc_level, dv = (unsigned char)((lv >> 2) + a->boff);
        for (; n; n--) {
            unsigned int adv = si;
            f += sf;
            if (f < sf) adv++;
            if (adv) {
                if (adv >= left) {
                    a->st_on = 0;
                    for (; n; n--) dmc_buf[n] = dv;
                    break;
                }
                left -= adv;
                idx = (idx + adv) & (NES_PCM_RING - 1);
                lv = (unsigned char)(pc[idx] & 0x7F);
                dv = (unsigned char)((lv >> 2) + a->boff);
            }
            dmc_buf[n] = dv;
        }
        a->st_idx = idx; a->st_frac = f; a->st_left = left;
        a->dmc_level = lv;
        a->dmc_out = (unsigned char)(lv >> 2);
    } else if (a->dmc_playing) {
        unsigned int dacc = a->dmc_acc, dinc = a->dmc_inc;
        unsigned char dout = a->dmc_out;
        int on = 1;
        for (; n; n--) {
            if (on) {
                dacc += dinc;
                if (dacc >= 256) {
                    dmc_clock(a, dacc >> 8);
                    dacc &= 0xFF;
                    dout = a->dmc_out;
                    on = a->dmc_playing;
                }
            }
            dmc_buf[n] = (unsigned char)(dout + a->boff);
        }
        a->dmc_acc = dacc;
    } else {
        memset(dmc_buf + 1, a->dmc_out + a->boff, n);
    }
}

static void nes_mix_asm(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov cx, s_n
        les di, s_out
        mov si, s_ph0
        mov dx, s_ph1
        push bp
        mov bp, s_th
    nlp:
        add si, s_inc0
        mov bx, si
        mov bl, bh
        xor bh, bh
        mov al, pt0t[bx]
        add dx, s_inc1
        mov bx, dx
        mov bl, bh
        xor bh, bh
        add al, pt1t[bx]
        add bp, s_tinc
        mov bx, bp
        mov bl, bh
        xor bh, bh
        add al, tri_cur[bx]
        mov bx, cx
        add al, dmc_buf[bx]
        cmp s_namp, 0
        je  nnoz
        mov bx, s_nacc
        add bx, s_ninc
        mov byte ptr s_nacc, bl
        mov bl, bh
        xor bh, bh
        add bx, s_nidx
        cmp bx, s_nper
        jb  nnok
        sub bx, s_nper
    nnok:
        mov s_nidx, bx
        mov ah, s_namp
        push ds
        mov ds, s_nseg
        cmp byte ptr [bx], 0
        pop ds
        jne nnoz
        add al, ah
    nnoz:
        mov bl, al
        xor bh, bh
        mov al, out_tab[bx]
        stosb
        loop nlp
        mov s_th, bp
        pop bp
        mov s_ph0, si
        mov s_ph1, dx
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

void nesapu_run(NesApu *a, unsigned char far *out, unsigned long count)
{
    int prev_out = a->prev_out;
    int click_delta = a->click_delta;
    int limit = click_delta < 120;
    if (!tabs_built) build_tabs();
    s_ph0 = a->phase[0]; s_ph1 = a->phase[1]; s_th = a->tri_phase;
    s_nidx = a->noise_idx; s_nacc = a->noise_cycle_accum & 0xFF;
    nes_prep(a);
    while (count) {
        unsigned int n = a->frame_left;
        if (n > 255) n = 255;
        if ((unsigned long)n > count) n = (unsigned int)count;
        count -= n;
        a->frame_left -= n;
        {
            unsigned int dc = s_dc + a->dmc_out;
            int target = dc >= 128 ? 0 : 128 - (int)dc;
            int d = target - (int)a->boff;
            if (d > 0) a->boff += (unsigned char)(d > 16 ? d >> 3 : 1);
            else if (d < 0) a->boff -= (unsigned char)(d < -16 ? (-d) >> 3 : 1);
        }
        dmc_render(a, n);
        s_out = out; s_n = n;
        nes_mix_asm();
        if (limit) {
            unsigned int k;
            for (k = 0; k < n; k++) {
                int o_ = out[k], d_ = o_ - prev_out;
                if (d_ > click_delta) o_ = prev_out + click_delta;
                else if (-d_ > click_delta) o_ = prev_out - click_delta;
                prev_out = o_;
                out[k] = (unsigned char)o_;
            }
        } else {
            prev_out = out[n - 1];
        }
        out += n;
        if (a->frame_left == 0) {
            nes_frame(a);
            nes_prep(a);
        }
    }
    a->phase[0] = s_ph0; a->phase[1] = s_ph1;
    a->tri_phase = s_th;
    a->noise_idx = s_nidx;
    a->noise_cycle_accum = s_nacc;
    a->prev_out = (unsigned char)prev_out;
}

const unsigned char far *nesapu_get_noise_table(int mode, unsigned int *period)
{
    if (mode == 0) {
        *period = (unsigned int)NES_NOISE_LONG_PERIOD;
        return noise_long_table;
    }
    *period = (unsigned int)NES_NOISE_SHORT_PERIOD;
    return noise_short_table;
}

const unsigned short *nesapu_get_period_table(void)
{
    return noise_period_table;
}
