#include <string.h>
#include "gbapu.h"
#include "dosmem.h"
#include "fastmath.h"

#define FIXED_SHIFT 8
#define FIXED_ONE (1UL << FIXED_SHIFT)
#define MAX_CLICK_DELTA 10

static const unsigned char duty_table[4] = { 0x80, 0xC0, 0xF0, 0x3F };

static const unsigned char noise_divisor_table[8] = {
    8,16,32,48,64,80,96,112
};

#define GB_NOISE15_PERIOD 32767UL
#define GB_NOISE7_PERIOD 127UL

static DosBuffer noise15_buf, noise7_buf;
static unsigned char far *noise15_table = 0;
static unsigned char far *noise7_table = 0;

static int noise_tables_built = 0;
static void gb_reset_cache(void);
static unsigned char g_k0 = 0xFF, g_k1 = 0xFF;
static int g_wkey = -1;
static int g_wave_dirty = 1;

static const unsigned char noise_table_fallback[1] = { 0 };

static void build_noise_tables(void)
{
    unsigned short lfsr;
    unsigned long i;
    if (noise_tables_built) return;
    {
        DosBuffer *bufs[2];
        unsigned sizes[2];
        int j;
        bufs[0] = &noise15_buf; bufs[1] = &noise7_buf;
        sizes[0] = (unsigned)GB_NOISE15_PERIOD;
        sizes[1] = (unsigned)GB_NOISE7_PERIOD;
        for (j = 0; j < 2; j++) {
            if (!dosmem_alloc_simple(bufs[j], sizes[j])) {
                int k;
                for (k = 0; k < j; k++) dosmem_free(bufs[k]);
                return;
            }
        }
    }
    noise15_table = noise15_buf.ptr;
    noise7_table = noise7_buf.ptr;
    lfsr = 0x7FFF;
    for (i = 0; i < GB_NOISE15_PERIOD; i++) {
        noise15_table[i] = (unsigned char)(lfsr & 1);
        {
            unsigned short fb = (unsigned short)((lfsr ^ (lfsr >> 1)) & 1);
            lfsr = (unsigned short)((lfsr >> 1) | (fb << 14));
        }
    }
    lfsr = 0x7F;
    for (i = 0; i < GB_NOISE7_PERIOD; i++) {
        noise7_table[i] = (unsigned char)(lfsr & 1);
        {
            unsigned short fb = (unsigned short)((lfsr ^ (lfsr >> 1)) & 1);
            lfsr = (unsigned short)(((lfsr >> 1) | (fb << 6)) & 0x7F);
        }
    }
    noise_tables_built = 1;
}

void gbapu_free_noise_tables(void)
{
    if (!noise_tables_built) return;
    dosmem_free(&noise15_buf);
    dosmem_free(&noise7_buf);
    noise15_table = 0;
    noise7_table = 0;
    noise_tables_built = 0;
}

void gbapu_init(GbApu *a, unsigned long clock, unsigned long sample_rate)
{
    build_noise_tables();
    memset(a, 0, sizeof(*a));
    a->clock = clock;
    a->sample_rate = sample_rate;
    a->power = 1;
    a->prev_out = 128;
    a->click_delta = (int)((16UL * 44100UL) / sample_rate);
    if (a->click_delta < 16) a->click_delta = 16;
    a->base_phase_pulse = (clock * 256UL) / sample_rate;
    a->base_phase_wave = (clock * 256UL) / sample_rate;
    a->ticks_cpu_precalc = (unsigned long)((clock << FIXED_SHIFT) / sample_rate);
    a->frame_reload_precalc = (long)(clock / 512UL);
    if (a->frame_reload_precalc < 1) a->frame_reload_precalc = 1;
    a->frame_counter = a->frame_reload_precalc;
    a->frame_q8 = (unsigned int)((sample_rate * 256UL) / 512UL);
    gb_reset_cache();
    a->frame_left = a->frame_q8 >> 8;
    a->frame_acc = a->frame_q8 & 0xFF;
    a->phase_inc0 = 0;
    a->phase_inc1 = 0;
    a->phase_inc_wave = 0;
    memset(a->wave_ram_unpacked, 0, sizeof(a->wave_ram_unpacked));
    gbapu_write_reg(a, 0x12, 0x00);
}

void gbapu_write_reg(GbApu *a, unsigned reg, unsigned char data)
{
    switch (reg) {
        case 0x00:
            if (a->sweep_dir && !((data >> 3) & 0x01) && a->sweep_neg_used)
                a->chan_on[0] = 0;
            a->sweep_period = (data >> 4) & 0x07;
            a->sweep_dir = (data >> 3) & 0x01;
            a->sweep_shift = data & 0x07;
            break;
        case 0x01:
            a->duty[0] = (data >> 6) & 0x03;
            a->length[0] = 64 - (data & 0x3F);
            break;
        case 0x02:
            a->env_vol[0] = (data >> 4) & 0x0F;
            a->env_dir[0] = (data >> 3) & 0x01;
            a->env_period[0] = data & 0x07;
            if (!(data & 0xF8)) a->chan_on[0] = 0;
            break;
        case 0x03:
            a->freq[0] = (unsigned short)((a->freq[0] & 0x0700) | data);
            a->phase_inc0 = a->chan_on[0] ? fast_div16(a->base_phase_pulse, 2048U - a->freq[0]) : 0;
            break;
        case 0x04:
            a->freq[0] = (unsigned short)((a->freq[0] & 0x00FF) | ((data & 0x07) << 8));
            a->len_halt[0] = !((data >> 6) & 0x01);
            if (data & 0x80) {
                a->chan_on[0] = 1;
                if (a->length[0] == 0) a->length[0] = 64;
                a->phase[0] = 0;
                a->env_level[0] = a->env_vol[0];
                a->env_timer[0] = a->env_period[0];
                a->env_start[0] = 0;
                a->env_enabled[0] = 1;
                a->sweep_freq = a->freq[0];
                a->sweep_timer = a->sweep_period ? a->sweep_period : 8;
                a->sweep_enabled = (a->sweep_period || a->sweep_shift);
                a->sweep_neg_used = 0;
                if (a->env_vol[0] == 0 && a->env_dir[0] == 0)
                    a->chan_on[0] = 0;
            }
            a->phase_inc0 = a->chan_on[0] ? fast_div16(a->base_phase_pulse, 2048U - a->freq[0]) : 0;
            break;
        case 0x06:
            a->duty[1] = (data >> 6) & 0x03;
            a->length[1] = 64 - (data & 0x3F);
            break;
        case 0x07:
            a->env_vol[1] = (data >> 4) & 0x0F;
            a->env_dir[1] = (data >> 3) & 0x01;
            a->env_period[1] = data & 0x07;
            if (!(data & 0xF8)) a->chan_on[1] = 0;
            break;
        case 0x08:
            a->freq[1] = (unsigned short)((a->freq[1] & 0x0700) | data);
            a->phase_inc1 = a->chan_on[1] ? fast_div16(a->base_phase_pulse, 2048U - a->freq[1]) : 0;
            break;
        case 0x09:
            a->freq[1] = (unsigned short)((a->freq[1] & 0x00FF) | ((data & 0x07) << 8));
            a->len_halt[1] = !((data >> 6) & 0x01);
            if (data & 0x80) {
                a->chan_on[1] = 1;
                if (a->length[1] == 0) a->length[1] = 64;
                a->phase[1] = 0;
                a->env_level[1] = a->env_vol[1];
                a->env_timer[1] = a->env_period[1];
                a->env_start[1] = 0;
                a->env_enabled[1] = 1;
                if (a->env_vol[1] == 0 && a->env_dir[1] == 0)
                    a->chan_on[1] = 0;
            }
            a->phase_inc1 = a->chan_on[1] ? fast_div16(a->base_phase_pulse, 2048U - a->freq[1]) : 0;
            break;
        case 0x0A:
            a->wave_dac_on = (data >> 7) & 0x01;
            if (!a->wave_dac_on) a->wave_on = 0;
            break;
        case 0x0B:
            a->wave_length = (unsigned short)(256 - data);
            break;
        case 0x0C:
            a->wave_level = (data >> 5) & 0x03;
            break;
        case 0x0D:
            a->wave_freq = (unsigned short)((a->wave_freq & 0x0700) | data);
            a->phase_inc_wave = a->wave_on ? fast_div16(a->base_phase_wave, 2048U - a->wave_freq) : 0;
            break;
        case 0x0E:
            a->wave_freq = (unsigned short)((a->wave_freq & 0x00FF) | ((data & 0x07) << 8));
            a->wave_len_halt = !((data >> 6) & 0x01);
            if (data & 0x80) {
                a->wave_on = a->wave_dac_on;
                if (a->wave_length == 0) a->wave_length = 256;
                a->wave_phase = 0;
            }
            a->phase_inc_wave = a->wave_on ? fast_div16(a->base_phase_wave, 2048U - a->wave_freq) : 0;
            break;
        case 0x10:
            a->noise_length = (unsigned char)(64 - (data & 0x3F));
            break;
        case 0x11:
            a->noise_env_vol = (data >> 4) & 0x0F;
            a->noise_env_dir = (data >> 3) & 0x01;
            a->noise_env_period = data & 0x07;
            a->noise_dac_on = (data & 0xF8) ? 1 : 0;
            if (!a->noise_dac_on) a->noise_on = 0;
            break;
        case 0x12:
            a->noise_shift_amt = (data >> 4) & 0x0F;
            a->noise_width7 = (data >> 3) & 0x01;
            a->noise_divisor_code = data & 0x07;
            if (a->noise_shift_amt >= 14) a->noise_step = 0;
            else {
                unsigned long st = a->ticks_cpu_precalc /
                    ((unsigned long)noise_divisor_table[a->noise_divisor_code] << a->noise_shift_amt);
                a->noise_step = (st > 0xFF00UL) ? 0xFF00U : (unsigned int)st;
            }
            if (a->noise_step <= 256U) a->noise_k = 256U;
            else {
                unsigned int r = 1, st2 = a->noise_step;
                while ((unsigned long)(r + 1) * (r + 1) <= st2) r++;
                a->noise_k = 4096U / r;
            }
            break;
        case 0x13:
            a->noise_len_halt = !((data >> 6) & 0x01);
            if (data & 0x80) {
                a->noise_on = 1;
                if (a->noise_length == 0) a->noise_length = 64;
                a->noise_idx = 0;
                a->noise_cycle_accum = 0;
                a->noise_env_level = a->noise_env_vol;
                a->noise_env_timer = a->noise_env_period;
                a->noise_env_start = 0;
                a->noise_env_enabled = 1;
                if (!a->noise_dac_on) a->noise_on = 0;
            }
            break;
        case 0x15:
            a->nr51 = data;
            break;
        case 0x16:
            a->power = (data >> 7) & 0x01;
            break;
        default:
            if (reg >= 0x20 && reg <= 0x2F) {
                unsigned int w = reg - 0x20;
                a->wave_ram[w] = data;
                a->wave_ram_unpacked[w << 1]       = data >> 4;
                a->wave_ram_unpacked[(w << 1) + 1] = data & 0x0F;
                g_wave_dirty = 1;
            }
            break;
    }
}

static void frame_tick(GbApu *a)
{
    int step = a->frame_step;
    if (!(step & 1)) {
        if (!a->len_halt[0] && a->length[0] > 0) { a->length[0]--; if (a->length[0]==0) a->chan_on[0]=0; }
        if (!a->len_halt[1] && a->length[1] > 0) { a->length[1]--; if (a->length[1]==0) a->chan_on[1]=0; }
        if (!a->noise_len_halt && a->noise_length > 0) { a->noise_length--; if (a->noise_length==0) a->noise_on=0; }
        if (!a->wave_len_halt && a->wave_length > 0) {
            a->wave_length--;
            if (a->wave_length == 0) a->wave_on = 0;
        }
    }
    if (step == 2 || step == 6) {
        if (a->sweep_enabled && a->sweep_timer > 0) {
            a->sweep_timer--;
            if (a->sweep_timer == 0) {
                a->sweep_timer = a->sweep_period ? a->sweep_period : 8;
                if (a->sweep_period > 0) {
                    int delta = a->sweep_freq >> a->sweep_shift;
                    int newfreq;
                    if (a->sweep_dir) { a->sweep_neg_used = 1; newfreq = a->sweep_freq - delta; }
                    else newfreq = a->sweep_freq + delta;
                    if (newfreq > 2047) {
                        a->chan_on[0] = 0;
                    } else if (a->sweep_shift > 0) {
                        a->sweep_freq = (unsigned short)newfreq;
                        a->freq[0] = a->sweep_freq;
                        a->phase_inc0 = a->chan_on[0] ? fast_div16(a->base_phase_pulse, 2048U - a->freq[0]) : 0;
                    }
                }
            }
        }
    }
    if (step == 7) {
        int ch;
        for (ch = 0; ch < 2; ch++) {
            if (!a->env_enabled[ch] || a->env_period[ch] == 0) continue;
            if (a->env_timer[ch] > 0) {
                a->env_timer[ch]--;
                if (a->env_timer[ch] == 0) {
                    a->env_timer[ch] = a->env_period[ch];
                    if (a->env_dir[ch]) {
                        if (a->env_level[ch] < 15) a->env_level[ch]++;
                        else a->env_enabled[ch] = 0;
                    } else {
                        if (a->env_level[ch] > 0) a->env_level[ch]--;
                        else a->env_enabled[ch] = 0;
                    }
                }
            }
        }
        if (a->noise_env_enabled && a->noise_env_period > 0 && a->noise_env_timer > 0) {
            a->noise_env_timer--;
            if (a->noise_env_timer == 0) {
                a->noise_env_timer = a->noise_env_period;
                if (a->noise_env_dir) {
                    if (a->noise_env_level < 15) a->noise_env_level++;
                    else a->noise_env_enabled = 0;
                } else {
                    if (a->noise_env_level > 0) a->noise_env_level--;
                    else a->noise_env_enabled = 0;
                }
            }
        }
    }
    a->frame_step = (step + 1) & 0x07;
}

static unsigned char gpt0[256], gpt1[256];
static unsigned char gwt[256];
static unsigned char gout[128];
static int gtabs_built = 0;

static unsigned int g_ph0, g_ph1, g_phw, g_inc0, g_inc1, g_incw;
static unsigned int g_nacc, g_nstep, g_nidx, g_nper, g_nseg, g_n;
static unsigned char g_nact, g_nhi, g_nlo;
static int g_prev, g_cd;
static unsigned int g_f0 = 0xFFFF, g_f1 = 0xFFFF, g_fw = 0xFFFF;
static unsigned char far *g_out;

static void gb_reset_cache(void)
{
    g_f0 = g_f1 = g_fw = 0xFFFF;
}

static unsigned int gb_inc(unsigned long base, unsigned int div)
{
    if (div == 0 || (unsigned int)(base >> 16) >= div) return 0;
    return fast_div16(base, div);
}

static void gb_prep(GbApu *a)
{
    int k;
    unsigned char m0 = 0, m1 = 0, v0 = 0, v1 = 0, wsh = 0, won = 0;
    const unsigned char far *w = a->wave_ram_unpacked;
    if (a->power) {
        if (a->chan_on[0] && a->length[0] > 0 && (a->nr51 & 0x11)) { m0 = duty_table[a->duty[0]]; v0 = a->env_level[0]; }
        if (a->chan_on[1] && a->length[1] > 0 && (a->nr51 & 0x22)) { m1 = duty_table[a->duty[1]]; v1 = a->env_level[1]; }
        if (a->wave_on && a->wave_length > 0 && a->wave_level && (a->nr51 & 0x44)) { won = 1; wsh = (unsigned char)(a->wave_level - 1); }
    }
    {
        unsigned char key0 = (unsigned char)(m0 ? ((a->duty[0] << 4) | v0) : 0);
        unsigned char key1 = (unsigned char)(m1 ? ((a->duty[1] << 4) | v1) : 0);
        if (g_k0 != key0) {
            for (k = 0; k < 8; k++) memset(gpt0 + (k << 5), ((m0 >> k) & 1) ? v0 : 0, 32);
            g_k0 = key0;
        }
        if (g_k1 != key1) {
            for (k = 0; k < 8; k++) memset(gpt1 + (k << 5), ((m1 >> k) & 1) ? v1 : 0, 32);
            g_k1 = key1;
        }
    }
    k = won ? wsh : -2;
    if (k == g_wkey && !g_wave_dirty) ;
    else if (!won) { memset(gwt, 0, 256); g_wkey = -2; }
    else for (g_wkey = wsh, g_wave_dirty = 0, k = 0; k < 32; k++) {
        int a_ = w[k] >> wsh;
        int d_ = (w[(k + 1) & 31] >> wsh) - a_;
        unsigned char *t = gwt + (k << 3);
        t[0] = t[1] = (unsigned char)a_;
        t[2] = t[3] = (unsigned char)(a_ + ((d_ + 2) >> 2));
        t[4] = t[5] = (unsigned char)(a_ + ((2 * d_ + 2) >> 2));
        t[6] = t[7] = (unsigned char)(a_ + ((3 * d_ + 2) >> 2));
    }
    if (a->freq[0] != g_f0) { g_f0 = a->freq[0]; g_inc0 = g_f0 < 2048 ? gb_inc(a->base_phase_pulse << 3, 2048U - g_f0) : 0; }
    if (a->freq[1] != g_f1) { g_f1 = a->freq[1]; g_inc1 = g_f1 < 2048 ? gb_inc(a->base_phase_pulse << 3, 2048U - g_f1) : 0; }
    if (a->wave_freq != g_fw) { g_fw = a->wave_freq; g_incw = g_fw < 2048 ? gb_inc(a->base_phase_wave << 2, 2048U - g_fw) : 0; }
    g_nact = 0;
    if (noise_tables_built && a->power && a->noise_on && a->noise_length > 0 && (a->nr51 & 0x88)) {
        const unsigned char far *t = a->noise_width7 ? noise7_table : noise15_table;
        unsigned int e = a->noise_env_level;
        g_nper = a->noise_width7 ? (unsigned int)GB_NOISE7_PERIOD : (unsigned int)GB_NOISE15_PERIOD;
        g_nstep = a->noise_step;
        if ((g_nstep >> 8) >= g_nper) g_nstep = (g_nper - 1) << 8;
        if (g_nidx >= g_nper) g_nidx = 0;
        g_nhi = (unsigned char)((e * (256U + a->noise_k) + 256U) >> 9);
        g_nlo = (unsigned char)((e * (256U - a->noise_k) + 256U) >> 9);
        g_nseg = FP_SEG(t) + (FP_OFF(t) >> 4);
        if ((FP_OFF(t) & 15) == 0) g_nact = 1;
    }
}

static void gb_mix_asm(void)
{
    _asm {
        push bx
        push cx
        push dx
        push es
        push si
        push di
        mov cx, g_n
        les di, g_out
        mov si, g_ph0
        mov dx, g_ph1
        push bp
        mov bp, g_phw
    glp:
        add si, g_inc0
        mov bx, si
        mov bl, bh
        xor bh, bh
        mov al, gpt0[bx]
        add dx, g_inc1
        mov bx, dx
        mov bl, bh
        xor bh, bh
        add al, gpt1[bx]
        add bp, g_incw
        mov bx, bp
        mov bl, bh
        xor bh, bh
        add al, gwt[bx]
        cmp g_nact, 0
        je  gnoz
        mov bx, g_nacc
        add bx, g_nstep
        mov byte ptr g_nacc, bl
        mov bl, bh
        xor bh, bh
        add bx, g_nidx
        cmp bx, g_nper
        jb  gnok
        sub bx, g_nper
    gnok:
        mov g_nidx, bx
        push ds
        mov ds, g_nseg
        cmp byte ptr [bx], 0
        pop ds
        mov ah, g_nlo
        jne gnb
        mov ah, g_nhi
    gnb:
        add al, ah
    gnoz:
        mov bl, al
        xor bh, bh
        mov al, gout[bx]
        xor ah, ah
        mov bx, ax
        sub bx, g_prev
        cmp bx, g_cd
        jle gc1
        mov ax, g_prev
        add ax, g_cd
        xor bx, bx
    gc1:
        neg bx
        cmp bx, g_cd
        jle gc2
        mov ax, g_prev
        sub ax, g_cd
    gc2:
        mov g_prev, ax
        stosb
        dec cx
        jz  gdone
        jmp glp
    gdone:
        mov g_phw, bp
        pop bp
        mov g_ph0, si
        mov g_ph1, dx
        pop di
        pop si
        pop es
        pop dx
        pop cx
        pop bx
    }
}

static void gb_frame(GbApu *a)
{
    frame_tick(a);
    a->frame_acc += a->frame_q8;
    a->frame_left = a->frame_acc >> 8;
    a->frame_acc &= 0xFF;
}

void gbapu_run(GbApu *a, unsigned char far *out, unsigned long count)
{
    if (!gtabs_built) {
        int k;
        for (k = 0; k < 128; k++) gout[k] = (unsigned char)(k * 2 + 83 > 255 ? 255 : k * 2 + 83);
        gtabs_built = 1;
    }
    {
        int p1_silent = !a->chan_on[0] || a->length[0] == 0;
        int p2_silent = !a->chan_on[1] || a->length[1] == 0;
        int wave_silent = !a->wave_on || a->wave_length == 0;
        int noise_silent = !a->noise_on || a->noise_length == 0;
        if (!a->power || (p1_silent && p2_silent && wave_silent && noise_silent)) {
            const int target = 83;
            int prev_out = a->prev_out, cd = a->click_delta;
            unsigned int j = 0, c16 = (unsigned int)count;
            unsigned long c = count;
            while (j < c16 && prev_out != target) {
                if (prev_out < target) prev_out += (prev_out + cd < target) ? cd : (target - prev_out);
                else prev_out -= (prev_out - cd > target) ? cd : (prev_out - target);
                out[j++] = (unsigned char)prev_out;
            }
            for (; j < c16; j++) out[j] = (unsigned char)target;
            while (c >= a->frame_left) {
                c -= a->frame_left;
                a->frame_acc += a->frame_q8;
                a->frame_left = a->frame_acc >> 8;
                a->frame_acc &= 0xFF;
            }
            a->frame_left -= (unsigned int)c;
            a->prev_out = (unsigned char)prev_out;
            return;
        }
    }
    g_ph0 = a->phase[0]; g_ph1 = a->phase[1]; g_phw = a->wave_phase;
    g_nidx = a->noise_idx; g_nacc = (unsigned int)(a->noise_cycle_accum & 0xFF);
    g_prev = a->prev_out; g_cd = a->click_delta;
    gb_prep(a);
    while (count) {
        unsigned int n = a->frame_left;
        if ((unsigned long)n > count) n = (unsigned int)count;
        count -= n;
        a->frame_left -= n;
        if (n) {
            g_out = out; g_n = n;
            gb_mix_asm();
            out += n;
        }
        if (a->frame_left == 0) {
            gb_frame(a);
            gb_prep(a);
        }
    }
    a->phase[0] = g_ph0; a->phase[1] = g_ph1; a->wave_phase = g_phw;
    a->noise_idx = g_nidx; a->noise_cycle_accum = g_nacc;
    a->prev_out = (unsigned char)g_prev;
}
